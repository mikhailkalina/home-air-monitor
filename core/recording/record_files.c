// SPDX-License-Identifier: Apache-2.0

#include "recording/record_files.h"

#include <string.h>

#include "recording/record_text.h"

#define DAY_KEY_LEN 10u  // "YYYY-MM-DD"
#define YEAR_DIGITS 4u
#define TWO_DIGITS 2u
#define DECIMAL_BASE 10u

// The most files of one key removed per listing; a key with more takes
// another listing. A day has one file per source plus the odd extra.
#define RETENTION_BATCH 8u

// A bound on the passes record_enforce_cap() makes, so a medium that keeps
// refusing to shrink cannot keep it looping: a year of days and then some.
#define RETENTION_MAX_PASSES 400u

// The largest index or undated number a name may carry; far past anything a
// card sees, and small enough for the arithmetic below.
#define NUMBER_MAX 99999999u

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

void record_key_for_day(char *dst, size_t cap, const civil_time_t *local_day)
{
    size_t len = record_text_u64(dst, cap, 0u, (uint64_t)local_day->year, YEAR_DIGITS);
    len = record_text_char(dst, cap, len, '-');
    len = record_text_u64(dst, cap, len, local_day->month, TWO_DIGITS);
    len = record_text_char(dst, cap, len, '-');
    len = record_text_u64(dst, cap, len, local_day->day, TWO_DIGITS);
    if (record_text_overflowed(len, cap) && cap > 0u) {
        dst[0] = '\0';
    }
}

void record_key_undated(char *dst, size_t cap, uint32_t seq)
{
    size_t len = record_text_str(dst, cap, 0u, RECORD_UNDATED_PREFIX);
    len = record_text_u64(dst, cap, len, seq, RECORD_UNDATED_DIGITS);
    if (record_text_overflowed(len, cap) && cap > 0u) {
        dst[0] = '\0';
    }
}

bool record_tag_ok(const char *tag)
{
    if (tag == NULL) {
        return false;
    }
    size_t n = 0u;
    bool letter = false;
    for (; tag[n] != '\0'; ++n) {
        if (!is_lower(tag[n]) && !is_digit(tag[n])) {
            return false;
        }
        letter = letter || is_lower(tag[n]);
    }
    // At least one letter: an all-digit part of a name is its index.
    return n > 0u && n < RECORD_TAG_MAX && letter;
}

// "YYYY-MM-DD", digits where digits belong; not checked as a calendar date.
static bool is_day_key(const char *s, size_t n)
{
    if (n != DAY_KEY_LEN) {
        return false;
    }
    for (size_t i = 0u; i < n; ++i) {
        const bool dash = (i == 4u || i == 7u);
        if (dash ? s[i] != '-' : !is_digit(s[i])) {
            return false;
        }
    }
    return true;
}

// Parses `n` decimal digits, nothing else, into *out; false past NUMBER_MAX.
static bool parse_number(const char *s, size_t n, uint32_t *out)
{
    if (n == 0u) {
        return false;
    }
    uint32_t value = 0u;
    for (size_t i = 0u; i < n; ++i) {
        if (!is_digit(s[i])) {
            return false;
        }
        value = value * DECIMAL_BASE + (uint32_t)(s[i] - '0');
        if (value > NUMBER_MAX) {
            return false;
        }
    }
    *out = value;
    return true;
}

static bool is_undated_key(const char *s, size_t n, uint32_t *seq)
{
    const size_t prefix = sizeof(RECORD_UNDATED_PREFIX) - 1u;
    if (n < prefix + RECORD_UNDATED_DIGITS || memcmp(s, RECORD_UNDATED_PREFIX, prefix) != 0) {
        return false;
    }
    uint32_t value = 0u;
    if (!parse_number(s + prefix, n - prefix, &value) || value == 0u) {
        return false;
    }
    if (seq != NULL) {
        *seq = value;
    }
    return true;
}

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0u; i < n; ++i) {
        if (!is_digit(s[i])) {
            return false;
        }
    }
    return n > 0u;
}

static bool copy_part(char *dst, size_t cap, const char *src, size_t n)
{
    if (n + 1u > cap) {
        return false;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

bool record_name_parse(const char *name, record_name_t *out)
{
    memset(out, 0, sizeof(*out));
    out->index = 1u;
    if (name == NULL) {
        return false;
    }

    // The extension: from the last '.', a dot and lower-case letters.
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return false;
    }
    for (const char *p = dot + 1; *p != '\0'; ++p) {
        if (!is_lower(*p)) {
            return false;
        }
    }
    if (!copy_part(out->ext, sizeof(out->ext), dot, strlen(dot))) {
        return false;
    }

    // The base: key, then up to two '_'-separated parts.
    const char *part = name;
    const char *end = dot;
    unsigned part_no = 0u;
    while (part < end) {
        const char *sep = memchr(part, '_', (size_t)(end - part));
        const char *part_end = (sep != NULL) ? sep : end;
        const size_t n = (size_t)(part_end - part);

        if (part_no == 0u) {
            if (!(is_day_key(part, n) || is_undated_key(part, n, NULL)) ||
                !copy_part(out->key, sizeof(out->key), part, n)) {
                return false;
            }
        } else if (all_digits(part, n)) {
            // The index: the last part, from 2 on, written without leading zero.
            uint32_t index = 0u;
            if (sep != NULL || part[0] == '0' || !parse_number(part, n, &index) || index < 2u) {
                return false;
            }
            out->index = index;
        } else {
            // The tag: only right after the key.
            if (part_no != 1u || !copy_part(out->tag, sizeof(out->tag), part, n) ||
                !record_tag_ok(out->tag)) {
                return false;
            }
        }

        ++part_no;
        if (sep == NULL) {
            break;
        }
        part = sep + 1;
        if (part == end) {
            return false;  // a name ending in '_'
        }
    }
    return part_no > 0u;
}

bool record_name_build(char *dst, size_t cap, const char *dir, const char *key, const char *tag,
                       uint32_t index, const char *ext)
{
    if (cap > 0u) {
        dst[0] = '\0';
    }
    const size_t key_len = strlen(key);
    if (!(is_day_key(key, key_len) || is_undated_key(key, key_len, NULL)) ||
        (tag != NULL && tag[0] != '\0' && !record_tag_ok(tag)) || index == 0u ||
        index > NUMBER_MAX) {
        return false;
    }

    size_t len = 0u;
    if (dir != NULL && dir[0] != '\0') {
        len = record_text_str(dst, cap, len, dir);
        len = record_text_char(dst, cap, len, '/');
    }
    len = record_text_str(dst, cap, len, key);
    if (tag != NULL && tag[0] != '\0') {
        len = record_text_char(dst, cap, len, '_');
        len = record_text_str(dst, cap, len, tag);
    }
    if (index > 1u) {
        len = record_text_char(dst, cap, len, '_');
        len = record_text_u64(dst, cap, len, index, 1u);
    }
    len = record_text_str(dst, cap, len, ext);
    if (record_text_overflowed(len, cap)) {
        if (cap > 0u) {
            dst[0] = '\0';
        }
        return false;
    }
    return true;
}

uint32_t record_key_undated_seq(const char *key)
{
    uint32_t seq = 0u;
    return is_undated_key(key, strlen(key), &seq) ? seq : 0u;
}

int record_key_compare(const char *a, const char *b)
{
    const uint32_t seq_a = record_key_undated_seq(a);
    const uint32_t seq_b = record_key_undated_seq(b);
    if (seq_a != 0u || seq_b != 0u) {
        if (seq_a == 0u) {
            return 1;  // a is a day: newer than any undated key
        }
        if (seq_b == 0u) {
            return -1;
        }
        return (seq_a < seq_b) ? -1 : (seq_a > seq_b) ? 1 : 0;
    }
    const int c = strcmp(a, b);
    return (c < 0) ? -1 : (c > 0) ? 1 : 0;
}

// --- scans over a directory ----------------------------------------------------

typedef struct {
    const char *key;
    const char *tag;
    const char *ext;
    uint32_t max_index;
    uint32_t max_undated;
} index_scan_t;

static void visit_index(void *ctx, const char *name, uint64_t size_bytes)
{
    (void)size_bytes;
    index_scan_t *scan = (index_scan_t *)ctx;
    record_name_t parsed;
    if (!record_name_parse(name, &parsed)) {
        return;
    }
    const uint32_t seq = record_key_undated_seq(parsed.key);
    if (seq > scan->max_undated) {
        scan->max_undated = seq;
    }
    if (scan->key != NULL && strcmp(parsed.key, scan->key) == 0 &&
        strcmp(parsed.tag, (scan->tag != NULL) ? scan->tag : "") == 0 &&
        strcmp(parsed.ext, scan->ext) == 0 && parsed.index > scan->max_index) {
        scan->max_index = parsed.index;
    }
}

hal_status_t record_next_index(port_files_t *files, const char *dir, const char *key,
                               const char *tag, const char *ext, uint32_t *out)
{
    index_scan_t scan = {.key = key, .tag = tag, .ext = ext};
    const hal_status_t status = files->list(files, dir, visit_index, &scan);
    if (status != HAL_OK) {
        return status;
    }
    *out = scan.max_index + 1u;
    return HAL_OK;
}

hal_status_t record_next_undated_seq(port_files_t *files, const char *dir, uint32_t *out)
{
    index_scan_t scan = {.key = NULL};
    const hal_status_t status = files->list(files, dir, visit_index, &scan);
    if (status != HAL_OK) {
        return status;
    }
    *out = scan.max_undated + 1u;
    return HAL_OK;
}

typedef struct {
    const char *const *protected_keys;
    size_t n_protected;
    uint64_t total;
    bool have_oldest;
    char oldest[RECORD_KEY_MAX];
} retention_scan_t;

static bool is_protected(const retention_scan_t *scan, const char *key)
{
    for (size_t i = 0u; i < scan->n_protected; ++i) {
        if (scan->protected_keys[i] != NULL && strcmp(scan->protected_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void visit_retention(void *ctx, const char *name, uint64_t size_bytes)
{
    retention_scan_t *scan = (retention_scan_t *)ctx;
    record_name_t parsed;
    if (!record_name_parse(name, &parsed)) {
        return;
    }
    scan->total += size_bytes;
    if (is_protected(scan, parsed.key)) {
        return;
    }
    if (!scan->have_oldest || record_key_compare(parsed.key, scan->oldest) < 0) {
        memcpy(scan->oldest, parsed.key, sizeof(scan->oldest));
        scan->have_oldest = true;
    }
}

typedef struct {
    const char *key;
    char names[RETENTION_BATCH][PORT_FILES_COMPONENT_MAX + 1u];
    uint64_t sizes[RETENTION_BATCH];
    size_t count;
} victims_t;

static void visit_victims(void *ctx, const char *name, uint64_t size_bytes)
{
    victims_t *v = (victims_t *)ctx;
    record_name_t parsed;
    if (v->count >= RETENTION_BATCH || !record_name_parse(name, &parsed) ||
        strcmp(parsed.key, v->key) != 0 || strlen(name) >= sizeof(v->names[0])) {
        return;
    }
    memcpy(v->names[v->count], name, strlen(name) + 1u);
    v->sizes[v->count] = size_bytes;
    v->count++;
}

hal_status_t record_enforce_cap(port_files_t *files, const char *dir, uint64_t cap_bytes,
                                const char *const *protected_keys, size_t n_protected,
                                record_retention_t *out)
{
    memset(out, 0, sizeof(*out));
    if (n_protected > RECORD_PROTECTED_MAX) {
        return HAL_ERR_INVALID_ARG;
    }

    for (unsigned pass = 0u; pass < RETENTION_MAX_PASSES; ++pass) {
        retention_scan_t scan = {.protected_keys = protected_keys, .n_protected = n_protected};
        hal_status_t status = files->list(files, dir, visit_retention, &scan);
        if (status != HAL_OK) {
            return status;
        }
        out->total_bytes = scan.total;
        if (scan.total <= cap_bytes) {
            return HAL_OK;
        }
        if (!scan.have_oldest) {
            out->over_cap = true;  // only what is open now is left
            return HAL_OK;
        }

        victims_t victims;
        memset(&victims, 0, sizeof(victims));
        victims.key = scan.oldest;
        status = files->list(files, dir, visit_victims, &victims);
        if (status != HAL_OK) {
            return status;
        }
        for (size_t i = 0u; i < victims.count; ++i) {
            char path[PORT_FILES_PATH_MAX];
            size_t len = record_text_str(path, sizeof(path), 0u, dir);
            len = record_text_char(path, sizeof(path), len, '/');
            len = record_text_str(path, sizeof(path), len, victims.names[i]);
            if (record_text_overflowed(len, sizeof(path))) {
                continue;
            }
            status = files->remove(files, path);
            if (status != HAL_OK && status != HAL_ERR_NOT_FOUND) {
                return status;
            }
            out->files_removed++;
            out->bytes_removed += victims.sizes[i];
        }
    }
    out->over_cap = true;
    return HAL_OK;
}
