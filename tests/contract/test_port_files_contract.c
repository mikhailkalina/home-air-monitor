// SPDX-License-Identifier: Apache-2.0
//
// Host runner for port_files_contract.c (shared, unmodified, with the
// on-target runner -- see port_files_contract.h): adp_files_dir against a
// scratch directory under the build tree, read back with stdio, its two test
// hooks standing in for a card pulled out and a card filled up. Then the
// same suite against deliberately broken wrappers, each of which one of its
// checks must catch.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/contract/CMakeLists.txt)"
#endif

#include <stdio.h>
#include <string.h>

#include "adp_files_dir.h"
#include "contract_print.h"
#include "hac_test.h"
#include "port_files_contract.h"

#define ROOT HAC_TEST_TMP_DIR "/port_files_contract"

static void report_to_hac_test(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
    hac_test_check(ok, check, __FILE__, __LINE__);
}

static hal_status_t read_back(void *ctx, const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    const adp_files_dir_t *d = (const adp_files_dir_t *)ctx;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", d->root, path);
    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    *len = fread(buf, 1u, cap, f);
    fclose(f);
    return HAL_OK;
}

static void set_present(void *ctx, bool present)
{
    adp_files_dir_set_present((adp_files_dir_t *)ctx, present);
}

static void set_full(void *ctx, bool full)
{
    adp_files_dir_set_limit((adp_files_dir_t *)ctx, full ? 1u : 0u);
}

static void adp_files_dir_satisfies_the_port_files_contract(void)
{
    static adp_files_dir_t dir;
    HAC_CHECK_EQ_INT(adp_files_dir_init(&dir, ROOT), HAL_OK);

    const port_files_contract_env_t env = {
        .read_back = read_back,
        .set_present = set_present,
        .set_full = set_full,
        .ctx = &dir,
    };
    contract_print_ctx_t ctx = {.port = "port_files", .implementation = "adp_files_dir"};
    const unsigned failures =
        port_files_contract_run(adp_files_dir_port(&dir), &env, report_to_hac_test, &ctx);
    HAC_CHECK_EQ_INT((int)failures, 0);
    adp_files_dir_deinit(&dir);
}

// --- broken wrappers -------------------------------------------------------------

typedef enum {
    MUTANT_FLUSH_KEEPS_IN_MEMORY = 0,  // bytes reach the medium only at close()
    MUTANT_REOPEN_TRUNCATES,           // open_append() starts the file over
    MUTANT_SPACES_ACCEPTED,            // a space in a path quietly becomes '_'
    MUTANT_FULL_IS_IO,                 // a full medium reported as an I/O error
    MUTANT_GONE_IS_IO,                 // a missing medium reported as an I/O error
} mutant_kind_t;

#define MUTANT_HELD_MAX 256u

typedef struct {
    port_files_t port;
    port_files_t *inner;
    mutant_kind_t kind;
    port_file_t *held_file;  // MUTANT_FLUSH_KEEPS_IN_MEMORY: the one file it holds back
    char held[MUTANT_HELD_MAX];
    size_t held_len;
} mutant_t;

static mutant_t *mutant_of(port_files_t *port)
{
    return (mutant_t *)port->impl;
}

static hal_status_t mutant_status(const mutant_t *m, hal_status_t s)
{
    if (m->kind == MUTANT_FULL_IS_IO && s == HAL_ERR_NO_MEM) {
        return HAL_ERR_IO;
    }
    if (m->kind == MUTANT_GONE_IS_IO && s == HAL_ERR_NOT_READY) {
        return HAL_ERR_IO;
    }
    return s;
}

static void mutant_path(const mutant_t *m, const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    if (m->kind == MUTANT_SPACES_ACCEPTED) {
        for (char *p = out; *p != '\0'; ++p) {
            if (*p == ' ') {
                *p = '_';
            }
        }
    }
}

static hal_status_t mutant_open(port_files_t *port, const char *path, port_file_t **out)
{
    mutant_t *m = mutant_of(port);
    char fixed[PORT_FILES_PATH_MAX + 8u];
    mutant_path(m, path, fixed, sizeof(fixed));
    if (m->kind == MUTANT_REOPEN_TRUNCATES) {
        (void)m->inner->remove(m->inner, fixed);
    }
    return mutant_status(m, m->inner->open_append(m->inner, fixed, out));
}

static hal_status_t mutant_append(port_files_t *port, port_file_t *file, const void *data,
                                  size_t len)
{
    mutant_t *m = mutant_of(port);
    if (m->kind == MUTANT_FLUSH_KEEPS_IN_MEMORY && data != NULL &&
        m->held_len + len <= MUTANT_HELD_MAX) {
        m->held_file = file;
        memcpy(m->held + m->held_len, data, len);
        m->held_len += len;
        return HAL_OK;
    }
    return mutant_status(m, m->inner->append(m->inner, file, data, len));
}

static hal_status_t mutant_flush(port_files_t *port, port_file_t *file)
{
    mutant_t *m = mutant_of(port);
    return mutant_status(m, m->inner->flush(m->inner, file));
}

static hal_status_t mutant_close(port_files_t *port, port_file_t *file)
{
    mutant_t *m = mutant_of(port);
    if (m->kind == MUTANT_FLUSH_KEEPS_IN_MEMORY && file == m->held_file && m->held_len > 0u) {
        (void)m->inner->append(m->inner, file, m->held, m->held_len);
        m->held_len = 0u;
        m->held_file = NULL;
    }
    return mutant_status(m, m->inner->close(m->inner, file));
}

static hal_status_t mutant_list(port_files_t *port, const char *dir, port_files_visit_fn visit,
                                void *ctx)
{
    mutant_t *m = mutant_of(port);
    return mutant_status(m, m->inner->list(m->inner, dir, visit, ctx));
}

static hal_status_t mutant_remove(port_files_t *port, const char *path)
{
    mutant_t *m = mutant_of(port);
    char fixed[PORT_FILES_PATH_MAX + 8u];
    mutant_path(m, path, fixed, sizeof(fixed));
    return mutant_status(m, m->inner->remove(m->inner, fixed));
}

#define MAX_RECORDED_FAILURES 32u

typedef struct {
    const char *failed[MAX_RECORDED_FAILURES];
    unsigned count;
} failure_log_t;

static void record_failures(void *ctx, const char *check, bool ok)
{
    failure_log_t *log = (failure_log_t *)ctx;
    if (!ok && log->count < MAX_RECORDED_FAILURES) {
        log->failed[log->count++] = check;
    }
}

static bool failed(const failure_log_t *log, const char *check)
{
    for (unsigned i = 0u; i < log->count; i++) {
        if (strcmp(log->failed[i], check) == 0) {
            return true;
        }
    }
    return false;
}

static failure_log_t run_against_mutant(mutant_kind_t kind)
{
    static adp_files_dir_t dir;
    (void)adp_files_dir_init(&dir, ROOT);

    static mutant_t m;
    memset(&m, 0, sizeof(m));
    m.port.open_append = mutant_open;
    m.port.append = mutant_append;
    m.port.flush = mutant_flush;
    m.port.close = mutant_close;
    m.port.list = mutant_list;
    m.port.remove = mutant_remove;
    m.port.impl = &m;
    m.inner = adp_files_dir_port(&dir);
    m.kind = kind;

    const port_files_contract_env_t env = {
        .read_back = read_back,
        .set_present = set_present,
        .set_full = set_full,
        .ctx = &dir,
    };
    failure_log_t log = {.count = 0u};
    (void)port_files_contract_run(&m.port, &env, record_failures, &log);
    adp_files_dir_deinit(&dir);
    return log;
}

static void the_contract_catches_a_flush_that_keeps_bytes_in_memory(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_FLUSH_KEEPS_IN_MEMORY);
    HAC_CHECK(failed(&log, "after flush the medium holds the bytes"));
    HAC_CHECK(failed(&log, "list names the file with its flushed size"));
}

static void the_contract_catches_a_reopen_that_truncates(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_REOPEN_TRUNCATES);
    HAC_CHECK(failed(&log, "reopening appends at the end"));
}

static void the_contract_catches_a_path_rule_not_enforced(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_SPACES_ACCEPTED);
    HAC_CHECK(failed(&log, "path: a space is refused"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

static void the_contract_catches_a_full_medium_reported_as_io(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_FULL_IS_IO);
    HAC_CHECK(failed(&log, "medium full: append or flush is HAL_ERR_NO_MEM"));
    HAC_CHECK_EQ_INT((int)log.count, 1);
}

static void the_contract_catches_a_missing_medium_reported_as_io(void)
{
    const failure_log_t log = run_against_mutant(MUTANT_GONE_IS_IO);
    HAC_CHECK(failed(&log, "medium gone: append is HAL_ERR_NOT_READY"));
    HAC_CHECK(failed(&log, "medium gone: open_append is HAL_ERR_NOT_READY"));
    HAC_CHECK(failed(&log, "medium gone: close releases the handle, HAL_ERR_NOT_READY"));
}

int main(void)
{
    HAC_RUN(adp_files_dir_satisfies_the_port_files_contract);
    HAC_RUN(the_contract_catches_a_flush_that_keeps_bytes_in_memory);
    HAC_RUN(the_contract_catches_a_reopen_that_truncates);
    HAC_RUN(the_contract_catches_a_path_rule_not_enforced);
    HAC_RUN(the_contract_catches_a_full_medium_reported_as_io);
    HAC_RUN(the_contract_catches_a_missing_medium_reported_as_io);
    return hac_test_summary();
}
