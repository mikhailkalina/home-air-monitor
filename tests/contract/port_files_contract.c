// SPDX-License-Identifier: Apache-2.0

#include "port_files_contract.h"

#include <string.h>

#define DIR "pfct"
#define FILE_A DIR "/a.txt"
#define FILE_GONE DIR "/gone.txt"
#define FILE_FULL DIR "/full.txt"
#define FILE_ROOT "pfct_root.txt"

// The PORT_FILES_MIN_OPEN files held open at once.
static const char *const MANY[PORT_FILES_MIN_OPEN] = {
    DIR "/m0.txt",
    DIR "/m1.txt",
    DIR "/m2.txt",
    DIR "/m3.txt",
};

// The longest path the rule allows: a 22-character directory, '/', and a
// 40-character name -- 63 characters, PORT_FILES_PATH_MAX with its
// terminator -- and one a character longer, every component still within
// PORT_FILES_COMPONENT_MAX, so only the total refuses it.
#define LONG_DIR_LEN 22u
#define LONG_NAME_LEN PORT_FILES_COMPONENT_MAX
#define LONG_EXT ".txt"

#define READ_CAP 64u

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

static bool holds(const port_files_contract_env_t *env, const char *path, const char *expected)
{
    uint8_t buf[READ_CAP];
    size_t len = 0u;
    if (env->read_back(env->ctx, path, buf, sizeof(buf), &len) != HAL_OK) {
        return false;
    }
    return len == strlen(expected) && memcmp(buf, expected, len) == 0;
}

static bool absent(const port_files_contract_env_t *env, const char *path)
{
    uint8_t buf[READ_CAP];
    size_t len = 0u;
    return env->read_back(env->ctx, path, buf, sizeof(buf), &len) == HAL_ERR_NOT_FOUND;
}

static hal_status_t append_text(port_files_t *f, port_file_t *file, const char *text)
{
    return f->append(f, file, text, strlen(text));
}

// Opens, appends, closes; HAL_OK only when all three were.
static hal_status_t write_file(port_files_t *f, const char *path, const char *text)
{
    port_file_t *file = NULL;
    hal_status_t status = f->open_append(f, path, &file);
    if (status != HAL_OK) {
        return status;
    }
    status = append_text(f, file, text);
    const hal_status_t closed = f->close(f, file);
    return (status != HAL_OK) ? status : closed;
}

typedef struct {
    const char *name;
    uint64_t size;
    bool found;
    unsigned entries;
} find_t;

static void visit_find(void *ctx, const char *name, uint64_t size_bytes)
{
    find_t *find = (find_t *)ctx;
    find->entries++;
    if (find->name != NULL && strcmp(find->name, name) == 0) {
        find->found = true;
        find->size = size_bytes;
    }
}

static find_t find_in(port_files_t *f, const char *dir, const char *name, hal_status_t *status)
{
    find_t find = {.name = name};
    *status = f->list(f, dir, visit_find, &find);
    return find;
}

static void long_paths(char *accepted, char *refused, char *dir_out)
{
    memset(dir_out, 'd', LONG_DIR_LEN);
    memcpy(dir_out, "pfctlong", 8u);
    dir_out[LONG_DIR_LEN] = '\0';

    char name[LONG_NAME_LEN + 1u];
    memset(name, 'n', LONG_NAME_LEN);
    memcpy(name + LONG_NAME_LEN - strlen(LONG_EXT), LONG_EXT, strlen(LONG_EXT));
    name[LONG_NAME_LEN] = '\0';

    // accepted: <dir>/<name>; refused: <dir>e/<name>, one character more.
    size_t n = 0u;
    memcpy(accepted, dir_out, LONG_DIR_LEN);
    n = LONG_DIR_LEN;
    accepted[n++] = '/';
    memcpy(accepted + n, name, LONG_NAME_LEN + 1u);

    memcpy(refused, dir_out, LONG_DIR_LEN);
    refused[LONG_DIR_LEN] = 'e';
    refused[LONG_DIR_LEN + 1u] = '/';
    memcpy(refused + LONG_DIR_LEN + 2u, name, LONG_NAME_LEN + 1u);
}

static void cleanup(port_files_t *f, const char *long_accepted)
{
    static const char *const PATHS[] = {FILE_A, FILE_GONE, FILE_FULL, FILE_ROOT};
    for (size_t i = 0u; i < sizeof(PATHS) / sizeof(PATHS[0]); ++i) {
        (void)f->remove(f, PATHS[i]);
    }
    for (size_t i = 0u; i < PORT_FILES_MIN_OPEN; ++i) {
        (void)f->remove(f, MANY[i]);
    }
    (void)f->remove(f, long_accepted);
}

static void check_paths(port_files_t *f, contract_report_fn report, void *ctx, unsigned *failures,
                        const port_files_contract_env_t *env)
{
    port_file_t *file = NULL;
    check(report, ctx, failures, "path: an absolute path is refused",
          f->open_append(f, "/abs.txt", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: a '..' component is refused",
          f->open_append(f, DIR "/../x.txt", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: a second directory level is refused",
          f->open_append(f, DIR "/sub/x.txt", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: an empty component is refused",
          f->open_append(f, DIR "//x.txt", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: a name starting with '.' is refused",
          f->open_append(f, DIR "/.x", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: a space is refused",
          f->open_append(f, DIR "/a b.txt", &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: an empty path is refused",
          f->open_append(f, "", &file) == HAL_ERR_INVALID_ARG);

    char accepted[PORT_FILES_PATH_MAX + 8u];
    char refused[PORT_FILES_PATH_MAX + 8u];
    char long_dir[LONG_DIR_LEN + 1u];
    long_paths(accepted, refused, long_dir);
    check(report, ctx, failures, "path: one character past PORT_FILES_PATH_MAX is refused",
          f->open_append(f, refused, &file) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "path: the longest the rule allows is written",
          write_file(f, accepted, "long\n") == HAL_OK && holds(env, accepted, "long\n"));
    (void)f->remove(f, accepted);
}

static void check_append_and_reopen(port_files_t *f, contract_report_fn report, void *ctx,
                                    unsigned *failures, const port_files_contract_env_t *env)
{
    port_file_t *file = NULL;
    check(report, ctx, failures, "open_append creates the file and its directory",
          f->open_append(f, FILE_A, &file) == HAL_OK && file != NULL);
    if (file == NULL) {
        return;
    }
    check(report, ctx, failures, "append is accepted", append_text(f, file, "hello\n") == HAL_OK);
    check(report, ctx, failures, "flush is accepted", f->flush(f, file) == HAL_OK);
    check(report, ctx, failures, "after flush the medium holds the bytes",
          holds(env, FILE_A, "hello\n"));

    hal_status_t status = HAL_ERR_IO;
    const find_t find = find_in(f, DIR, "a.txt", &status);
    check(report, ctx, failures, "list names the file with its flushed size",
          status == HAL_OK && find.found && find.size == 6u);

    check(report, ctx, failures, "a zero-length append changes nothing",
          f->append(f, file, NULL, 0u) == HAL_OK && f->flush(f, file) == HAL_OK &&
              holds(env, FILE_A, "hello\n"));
    check(report, ctx, failures, "append of NULL data with a length is refused",
          f->append(f, file, NULL, 1u) == HAL_ERR_INVALID_ARG);

    port_file_t *second = NULL;
    check(report, ctx, failures, "an open path cannot be opened again",
          f->open_append(f, FILE_A, &second) == HAL_ERR_BUSY);
    check(report, ctx, failures, "an open file cannot be removed",
          f->remove(f, FILE_A) == HAL_ERR_BUSY);

    check(report, ctx, failures, "close is accepted", f->close(f, file) == HAL_OK);
    check(report, ctx, failures, "a closed handle is refused",
          append_text(f, file, "x") == HAL_ERR_INVALID_ARG);

    check(report, ctx, failures, "reopening appends at the end",
          write_file(f, FILE_A, "world\n") == HAL_OK && holds(env, FILE_A, "hello\nworld\n"));
}

static void check_many_open(port_files_t *f, contract_report_fn report, void *ctx,
                            unsigned *failures, const port_files_contract_env_t *env)
{
    static const char *const TEXT[PORT_FILES_MIN_OPEN] = {"zero\n", "one\n", "two\n", "three\n"};
    port_file_t *files[PORT_FILES_MIN_OPEN] = {NULL};
    bool opened = true;
    for (size_t i = 0u; i < PORT_FILES_MIN_OPEN; ++i) {
        opened = opened && f->open_append(f, MANY[i], &files[i]) == HAL_OK;
    }
    check(report, ctx, failures, "PORT_FILES_MIN_OPEN files open at once", opened);

    bool kept_apart = true;
    for (size_t i = 0u; i < PORT_FILES_MIN_OPEN; ++i) {
        if (files[i] != NULL) {
            kept_apart = append_text(f, files[i], TEXT[i]) == HAL_OK && kept_apart;
        }
    }
    for (size_t i = 0u; i < PORT_FILES_MIN_OPEN; ++i) {
        if (files[i] != NULL) {
            kept_apart = f->close(f, files[i]) == HAL_OK && kept_apart;
        }
        kept_apart = holds(env, MANY[i], TEXT[i]) && kept_apart;
    }
    check(report, ctx, failures, "files open at once each keep their own bytes", kept_apart);
}

static void check_list_and_remove(port_files_t *f, contract_report_fn report, void *ctx,
                                  unsigned *failures, const port_files_contract_env_t *env)
{
    hal_status_t status = HAL_ERR_IO;
    const find_t missing = find_in(f, "pfctnone", NULL, &status);
    check(report, ctx, failures, "list of a missing directory is empty",
          status == HAL_OK && missing.entries == 0u);

    find_t unused = {0};
    check(report, ctx, failures, "list refuses a path that is not one directory",
          f->list(f, DIR "/a.txt", visit_find, &unused) == HAL_ERR_INVALID_ARG);

    check(report, ctx, failures, "remove deletes the file",
          f->remove(f, FILE_A) == HAL_OK && absent(env, FILE_A));
    const find_t gone = find_in(f, DIR, "a.txt", &status);
    check(report, ctx, failures, "list no longer names a removed file",
          status == HAL_OK && !gone.found);
    check(report, ctx, failures, "remove of a missing file is HAL_ERR_NOT_FOUND",
          f->remove(f, FILE_A) == HAL_ERR_NOT_FOUND);

    check(report, ctx, failures, "a file at the root is written and removed",
          write_file(f, FILE_ROOT, "root\n") == HAL_OK && holds(env, FILE_ROOT, "root\n") &&
              f->remove(f, FILE_ROOT) == HAL_OK);
}

static void check_medium_gone(port_files_t *f, contract_report_fn report, void *ctx,
                              unsigned *failures, const port_files_contract_env_t *env)
{
    if (env->set_present == NULL) {
        check(report, ctx, failures, "medium gone: not exercised by this runner", true);
        return;
    }
    port_file_t *file = NULL;
    const bool opened = f->open_append(f, FILE_GONE, &file) == HAL_OK &&
                        append_text(f, file, "kept\n") == HAL_OK && f->flush(f, file) == HAL_OK;
    check(report, ctx, failures, "medium gone: a file written and flushed before", opened);
    if (!opened) {
        if (file != NULL) {
            (void)f->close(f, file);
        }
        return;
    }

    env->set_present(env->ctx, false);
    port_file_t *other = NULL;
    find_t unused = {0};
    check(report, ctx, failures, "medium gone: append is HAL_ERR_NOT_READY",
          append_text(f, file, "lost\n") == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "medium gone: flush is HAL_ERR_NOT_READY",
          f->flush(f, file) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "medium gone: open_append is HAL_ERR_NOT_READY",
          f->open_append(f, FILE_A, &other) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "medium gone: list is HAL_ERR_NOT_READY",
          f->list(f, DIR, visit_find, &unused) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "medium gone: remove is HAL_ERR_NOT_READY",
          f->remove(f, FILE_GONE) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "medium gone: close releases the handle, HAL_ERR_NOT_READY",
          f->close(f, file) == HAL_ERR_NOT_READY);

    env->set_present(env->ctx, true);
    check(report, ctx, failures, "medium back: what was flushed before is there",
          holds(env, FILE_GONE, "kept\n"));
    check(report, ctx, failures, "medium back: files open and append again",
          write_file(f, FILE_GONE, "again\n") == HAL_OK && holds(env, FILE_GONE, "kept\nagain\n"));
}

static void check_medium_full(port_files_t *f, contract_report_fn report, void *ctx,
                              unsigned *failures, const port_files_contract_env_t *env)
{
    if (env->set_full == NULL) {
        check(report, ctx, failures, "medium full: not exercised by this runner", true);
        return;
    }
    port_file_t *file = NULL;
    if (f->open_append(f, FILE_FULL, &file) != HAL_OK) {
        check(report, ctx, failures, "medium full: a file to fill opens", false);
        return;
    }
    env->set_full(env->ctx, true);
    // A full medium may say so at the append or only at the flush.
    hal_status_t status = append_text(f, file, "does not fit\n");
    if (status == HAL_OK) {
        status = f->flush(f, file);
    }
    check(report, ctx, failures, "medium full: append or flush is HAL_ERR_NO_MEM",
          status == HAL_ERR_NO_MEM);

    env->set_full(env->ctx, false);
    (void)f->close(f, file);
    check(report, ctx, failures, "medium full: with room again, files are written",
          write_file(f, FILE_FULL, "fits\n") == HAL_OK);
}

unsigned port_files_contract_run(port_files_t *files, const port_files_contract_env_t *env,
                                 contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;
    char accepted[PORT_FILES_PATH_MAX + 8u];
    char refused[PORT_FILES_PATH_MAX + 8u];
    char long_dir[LONG_DIR_LEN + 1u];
    long_paths(accepted, refused, long_dir);

    cleanup(files, accepted);  // whatever an interrupted run left
    check_paths(files, report, report_ctx, &failures, env);
    check_append_and_reopen(files, report, report_ctx, &failures, env);
    check_many_open(files, report, report_ctx, &failures, env);
    check_list_and_remove(files, report, report_ctx, &failures, env);
    check_medium_gone(files, report, report_ctx, &failures, env);
    check_medium_full(files, report, report_ctx, &failures, env);
    cleanup(files, accepted);
    return failures;
}
