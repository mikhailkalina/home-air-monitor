// SPDX-License-Identifier: Apache-2.0
//
// core/recording/record_files.c: the names of the recordings and the log,
// the order of their keys, and the bound that removes the oldest day first --
// over adp_files_dir in a scratch directory, so a removed file is really
// gone.

#ifndef HAC_TEST_TMP_DIR
#error "HAC_TEST_TMP_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

#define _POSIX_C_SOURCE 200809L

#include "recording/record_files.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "adp_files_dir.h"
#include "hac_test.h"

// --- names -----------------------------------------------------------------------

static void keys_are_days_and_numbered_undated_boots(void)
{
    char key[RECORD_KEY_MAX];
    const civil_time_t day = {.year = 2026, .month = 3, .day = 7};
    record_key_for_day(key, sizeof(key), &day);
    HAC_CHECK(strcmp(key, "2026-03-07") == 0);
    record_key_undated(key, sizeof(key), 3u);
    HAC_CHECK(strcmp(key, "undated-0003") == 0);
    record_key_undated(key, sizeof(key), 12345u);
    HAC_CHECK(strcmp(key, "undated-12345") == 0);
    HAC_CHECK_EQ_U64(record_key_undated_seq("undated-0003"), 3u);
    HAC_CHECK_EQ_U64(record_key_undated_seq("2026-03-07"), 0u);
}

static void names_are_built_from_their_parts(void)
{
    char name[PORT_FILES_PATH_MAX];
    HAC_CHECK(record_name_build(name, sizeof(name), "data", "2026-10-16", "scd4x", 1u, ".csv"));
    HAC_CHECK(strcmp(name, "data/2026-10-16_scd4x.csv") == 0);
    HAC_CHECK(record_name_build(name, sizeof(name), "data", "2026-10-16", "scd4x", 2u, ".csv"));
    HAC_CHECK(strcmp(name, "data/2026-10-16_scd4x_2.csv") == 0);
    HAC_CHECK(record_name_build(name, sizeof(name), "log", "undated-0003", NULL, 1u, ".log"));
    HAC_CHECK(strcmp(name, "log/undated-0003.log") == 0);
    HAC_CHECK(record_name_build(name, sizeof(name), "log", "2026-10-16", "", 12u, ".log"));
    HAC_CHECK(strcmp(name, "log/2026-10-16_12.log") == 0);

    // Parts a name may not have, and a name too long for its buffer.
    HAC_CHECK(!record_name_build(name, sizeof(name), "data", "2026-10-16", "SCD4x", 1u, ".csv"));
    HAC_CHECK(!record_name_build(name, sizeof(name), "data", "2026-10-16", "scd_4x", 1u, ".csv"));
    HAC_CHECK(!record_name_build(name, sizeof(name), "data", "2026-10-16", "42", 1u, ".csv"));
    HAC_CHECK(!record_name_build(name, sizeof(name), "data", "2026-10-16", "scd4x", 0u, ".csv"));
    HAC_CHECK(!record_name_build(name, sizeof(name), "data", "yesterday", "scd4x", 1u, ".csv"));
    HAC_CHECK(!record_name_build(name, 20u, "data", "2026-10-16", "scd4x", 1u, ".csv"));
    HAC_CHECK(strcmp(name, "") == 0);
}

static void names_parse_back_into_their_parts(void)
{
    record_name_t n;
    HAC_CHECK(record_name_parse("2026-10-16_scd4x.csv", &n));
    HAC_CHECK(strcmp(n.key, "2026-10-16") == 0 && strcmp(n.tag, "scd4x") == 0);
    HAC_CHECK_EQ_U64(n.index, 1u);
    HAC_CHECK(strcmp(n.ext, ".csv") == 0);

    HAC_CHECK(record_name_parse("2026-10-16_bme68x_3.csv", &n));
    HAC_CHECK(strcmp(n.tag, "bme68x") == 0);
    HAC_CHECK_EQ_U64(n.index, 3u);

    HAC_CHECK(record_name_parse("undated-0007.log", &n));
    HAC_CHECK(strcmp(n.key, "undated-0007") == 0 && n.tag[0] == '\0');

    HAC_CHECK(record_name_parse("2026-10-16_2.log", &n));
    HAC_CHECK(n.tag[0] == '\0');
    HAC_CHECK_EQ_U64(n.index, 2u);
}

static void names_not_of_the_form_are_not_ours(void)
{
    static const char *const NOT_OURS[] = {
        "notes.txt",                 // no key
        "2026-1-16_scd4x.csv",       // not a day
        "undated-12_scd4x.csv",      // fewer than four digits
        "undated-0000_scd4x.csv",    // no boot 0
        "2026-10-16_scd4x_1.csv",    // the first file has no suffix
        "2026-10-16_scd4x_02.csv",   // a leading zero
        "2026-10-16__scd4x.csv",     // an empty part
        "2026-10-16_scd4x_.csv",     // a trailing '_'
        "2026-10-16_scd4x_2_3.csv",  // two indices
        "2026-10-16_2_scd4x.csv",    // the tag after the index
        "2026-10-16_SCD4x.csv",      // upper case
        "2026-10-16_scd4x",          // no extension
        "2026-10-16_scd4x.CSV",      // an upper-case extension
        ".csv",                      // nothing before the extension
    };
    for (size_t i = 0u; i < sizeof(NOT_OURS) / sizeof(NOT_OURS[0]); ++i) {
        record_name_t n;
        const bool parsed = record_name_parse(NOT_OURS[i], &n);
        if (parsed) {
            printf("  parsed, but should not have: %s\n", NOT_OURS[i]);
        }
        HAC_CHECK(!parsed);
    }
}

static void undated_keys_are_older_than_every_day(void)
{
    HAC_CHECK(record_key_compare("undated-0002", "undated-0010") < 0);
    HAC_CHECK(record_key_compare("undated-9999", "2000-01-01") < 0);
    HAC_CHECK(record_key_compare("2026-10-16", "undated-0001") > 0);
    HAC_CHECK(record_key_compare("2026-09-30", "2026-10-01") < 0);
    HAC_CHECK(record_key_compare("2026-10-01", "2026-10-01") == 0);
}

// --- over a directory --------------------------------------------------------------

#define ROOT HAC_TEST_TMP_DIR "/record_files"

static void make(port_files_t *f, const char *path, size_t bytes)
{
    port_file_t *file = NULL;
    HAC_CHECK_EQ_INT(f->open_append(f, path, &file), HAL_OK);
    char buf[256];
    memset(buf, 'x', sizeof(buf));
    while (bytes > 0u) {
        const size_t n = (bytes < sizeof(buf)) ? bytes : sizeof(buf);
        HAC_CHECK_EQ_INT(f->append(f, file, buf, n), HAL_OK);
        bytes -= n;
    }
    HAC_CHECK_EQ_INT(f->close(f, file), HAL_OK);
}

static bool exists(const char *rel)
{
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", ROOT, rel);
    return access(full, F_OK) == 0;
}

// Removes every file `dir` holds, through the port: what an earlier run left.
typedef struct {
    char names[32][PORT_FILES_COMPONENT_MAX + 1u];
    size_t count;
} names_t;

static void collect(void *ctx, const char *name, uint64_t size_bytes)
{
    (void)size_bytes;
    names_t *n = (names_t *)ctx;
    if (n->count < 32u) {
        snprintf(n->names[n->count++], sizeof(n->names[0]), "%s", name);
    }
}

static void empty_dir(port_files_t *f, const char *dir)
{
    static names_t names;
    names.count = 0u;
    (void)f->list(f, dir, collect, &names);
    for (size_t i = 0u; i < names.count; ++i) {
        char path[PORT_FILES_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, names.names[i]);
        (void)f->remove(f, path);
    }
}

static void the_next_index_follows_the_highest(void)
{
    static adp_files_dir_t d;
    HAC_CHECK_EQ_INT(adp_files_dir_init(&d, ROOT), HAL_OK);
    port_files_t *f = adp_files_dir_port(&d);
    empty_dir(f, "idx");

    uint32_t index = 0u;
    HAC_CHECK_EQ_INT(record_next_index(f, "idx", "2026-10-16", "scd4x", ".csv", &index), HAL_OK);
    HAC_CHECK_EQ_U64(index, 1u);  // a directory that does not exist yet

    make(f, "idx/2026-10-16_scd4x.csv", 1u);
    make(f, "idx/2026-10-16_bme68x_7.csv", 1u);  // another tag
    HAC_CHECK_EQ_INT(record_next_index(f, "idx", "2026-10-16", "scd4x", ".csv", &index), HAL_OK);
    HAC_CHECK_EQ_U64(index, 2u);
    make(f, "idx/2026-10-16_scd4x_2.csv", 1u);
    HAC_CHECK_EQ_INT(record_next_index(f, "idx", "2026-10-16", "scd4x", ".csv", &index), HAL_OK);
    HAC_CHECK_EQ_U64(index, 3u);
    HAC_CHECK_EQ_INT(record_next_index(f, "idx", "2026-10-15", "scd4x", ".csv", &index), HAL_OK);
    HAC_CHECK_EQ_U64(index, 1u);  // another day

    uint32_t seq = 0u;
    HAC_CHECK_EQ_INT(record_next_undated_seq(f, "idx", &seq), HAL_OK);
    HAC_CHECK_EQ_U64(seq, 1u);
    make(f, "idx/undated-0003_scd4x.csv", 1u);
    make(f, "idx/undated-0001_bme68x.csv", 1u);
    HAC_CHECK_EQ_INT(record_next_undated_seq(f, "idx", &seq), HAL_OK);
    HAC_CHECK_EQ_U64(seq, 4u);

    adp_files_dir_set_present(&d, false);
    HAC_CHECK_EQ_INT(record_next_undated_seq(f, "idx", &seq), HAL_ERR_NOT_READY);
    adp_files_dir_set_present(&d, true);
    empty_dir(f, "idx");
    adp_files_dir_deinit(&d);
}

static void the_bound_removes_the_oldest_day_first(void)
{
    static adp_files_dir_t d;
    HAC_CHECK_EQ_INT(adp_files_dir_init(&d, ROOT), HAL_OK);
    port_files_t *f = adp_files_dir_port(&d);
    empty_dir(f, "cap");

    make(f, "cap/undated-0003_scd4x.csv", 100u);
    make(f, "cap/2026-10-14_scd4x.csv", 100u);
    make(f, "cap/2026-10-14_bme68x.csv", 100u);
    make(f, "cap/2026-10-15_scd4x.csv", 100u);
    make(f, "cap/2026-10-15_scd4x_2.csv", 100u);
    make(f, "cap/2026-10-16_scd4x.csv", 100u);
    make(f, "cap/notes.txt", 10000u);  // not ours: neither counted nor removed

    record_retention_t r;
    const char *protected_keys[] = {"2026-10-16"};

    // Within the bound: nothing.
    HAC_CHECK_EQ_INT(record_enforce_cap(f, "cap", 600u, protected_keys, 1u, &r), HAL_OK);
    HAC_CHECK_EQ_U64(r.files_removed, 0u);
    HAC_CHECK_EQ_U64(r.total_bytes, 600u);

    // 500: the undated boot goes first, whole.
    HAC_CHECK_EQ_INT(record_enforce_cap(f, "cap", 500u, protected_keys, 1u, &r), HAL_OK);
    HAC_CHECK_EQ_U64(r.files_removed, 1u);
    HAC_CHECK(!exists("cap/undated-0003_scd4x.csv"));
    HAC_CHECK(exists("cap/2026-10-14_scd4x.csv"));

    // 250: then whole days, oldest first -- both of the 14th, both of the 15th.
    HAC_CHECK_EQ_INT(record_enforce_cap(f, "cap", 250u, protected_keys, 1u, &r), HAL_OK);
    HAC_CHECK_EQ_U64(r.files_removed, 4u);
    HAC_CHECK_EQ_U64(r.bytes_removed, 400u);
    HAC_CHECK_EQ_U64(r.total_bytes, 100u);
    HAC_CHECK(!r.over_cap);
    HAC_CHECK(!exists("cap/2026-10-14_bme68x.csv"));
    HAC_CHECK(!exists("cap/2026-10-15_scd4x_2.csv"));

    // 50: only the protected day is left, and it stays.
    HAC_CHECK_EQ_INT(record_enforce_cap(f, "cap", 50u, protected_keys, 1u, &r), HAL_OK);
    HAC_CHECK_EQ_U64(r.files_removed, 0u);
    HAC_CHECK(r.over_cap);
    HAC_CHECK(exists("cap/2026-10-16_scd4x.csv"));
    HAC_CHECK(exists("cap/notes.txt"));

    // Without a medium, it says so.
    adp_files_dir_set_present(&d, false);
    HAC_CHECK_EQ_INT(record_enforce_cap(f, "cap", 50u, protected_keys, 1u, &r), HAL_ERR_NOT_READY);
    adp_files_dir_set_present(&d, true);
    empty_dir(f, "cap");
    adp_files_dir_deinit(&d);
}

int main(void)
{
    HAC_RUN(keys_are_days_and_numbered_undated_boots);
    HAC_RUN(names_are_built_from_their_parts);
    HAC_RUN(names_parse_back_into_their_parts);
    HAC_RUN(names_not_of_the_form_are_not_ours);
    HAC_RUN(undated_keys_are_older_than_every_day);
    HAC_RUN(the_next_index_follows_the_highest);
    HAC_RUN(the_bound_removes_the_oldest_day_first);
    return hac_test_summary();
}
