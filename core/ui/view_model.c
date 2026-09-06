// SPDX-License-Identifier: Apache-2.0

#include "ui/view_model.h"

#include <math.h>
#include <string.h>

// --- string building -------------------------------------------------------
//
// core/ has no stdio, so there is no snprintf here. These four helpers are the
// whole of it: they never write past `cap` and always leave `dst` terminated.

static size_t str_append(char *dst, size_t cap, size_t len, const char *src)
{
    if (cap == 0u) {
        return 0u;
    }
    while (*src != '\0' && len + 1u < cap) {
        dst[len++] = *src++;
    }
    dst[len] = '\0';
    return len;
}

static size_t str_append_u32(char *dst, size_t cap, size_t len, uint32_t value)
{
    char digits[10];
    size_t n = 0u;

    do {
        digits[n++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof(digits));

    while (n > 0u && len + 1u < cap) {
        dst[len++] = digits[--n];
    }
    if (cap != 0u) {
        dst[len] = '\0';
    }
    return len;
}

// Beyond this the value is not a plausible reading, and the conversion to a
// fixed-point integer below would be undefined rather than merely wrong.
#define VM_FORMAT_MAX_SCALED 9999999.0f

// Writes `value` with `decimals` digits after the point, half-away-from-zero.
// Out-of-range and non-finite values become a visible marker rather than
// garbage, so a bad float can never be mistaken for a reading.
static void format_fixed(char *dst, size_t cap, float value, unsigned decimals)
{
    if (cap == 0u) {
        return;
    }
    dst[0] = '\0';

    if (!isfinite(value)) {
        (void)str_append(dst, cap, 0u, "?");
        return;
    }

    float scale = 1.0f;
    for (unsigned i = 0u; i < decimals; ++i) {
        scale *= 10.0f;
    }
    const float scaled = value * scale;

    if (scaled > VM_FORMAT_MAX_SCALED) {
        (void)str_append(dst, cap, 0u, "+++");
        return;
    }
    if (scaled < -VM_FORMAT_MAX_SCALED) {
        (void)str_append(dst, cap, 0u, "---");
        return;
    }

    const long rounded = lroundf(scaled);
    const bool negative = rounded < 0L;
    uint32_t magnitude = (uint32_t)(negative ? -rounded : rounded);

    char digits[12];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + (int)(magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0u && n < sizeof(digits));

    // "0.4", not ".4": pad until there is at least one digit before the point.
    while (n <= (size_t)decimals && n < sizeof(digits)) {
        digits[n++] = '0';
    }

    size_t len = 0u;
    if (negative) {
        len = str_append(dst, cap, len, "-");
    }
    for (size_t i = n; i > 0u; --i) {
        if (decimals > 0u && i == (size_t)decimals) {
            len = str_append(dst, cap, len, ".");
        }
        if (len + 1u >= cap) {
            break;
        }
        dst[len++] = digits[i - 1u];
        dst[len] = '\0';
    }
}

// --- per-status treatment --------------------------------------------------

vm_mark_t view_model_mark_for(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return VM_MARK_NONE;
        case VAL_STALE:
            return VM_MARK_STALE;
        case VAL_ERROR:
            return VM_MARK_ERROR;
        case VAL_WARMUP:
            return VM_MARK_WARMUP;
        case VAL_UNAVAILABLE:
            break;
    }
    return VM_MARK_UNAVAILABLE;
}

uint8_t view_model_ink_for(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return VM_INK_PRIMARY;
        case VAL_STALE:
            return VM_INK_STALE;
        case VAL_ERROR:
            return VM_INK_PRIMARY;  // an error shouts; the mark carries the alarm
        case VAL_WARMUP:
            return VM_INK_WARMUP;
        case VAL_UNAVAILABLE:
            break;
    }
    return VM_INK_UNAVAILABLE;
}

// The text shown in place of a number. Never empty: a blank field is
// indistinguishable from a rendering bug.
static const char *placeholder_for(value_status_t status)
{
    switch (status) {
        case VAL_ERROR:
            return "ERR";
        case VAL_WARMUP:
            return "warm";
        case VAL_OK:
        case VAL_STALE:
        case VAL_UNAVAILABLE:
            break;
    }
    return "n/a";
}

// --- fields ----------------------------------------------------------------

static void build_field(vm_field_view_t *out, const measurement_f32_t *m, const char *label,
                        const char *unit, unsigned decimals)
{
    memset(out, 0, sizeof(*out));

    out->label = label;
    out->unit = unit;
    out->status = m->status;
    out->mark = view_model_mark_for(m->status);
    out->ink = view_model_ink_for(m->status);
    out->has_value = measurement_has_value(m);

    if (out->has_value) {
        format_fixed(out->text, sizeof(out->text), m->value, decimals);
    } else {
        (void)str_append(out->text, sizeof(out->text), 0u, placeholder_for(m->status));
    }
}

// --- CO2 band --------------------------------------------------------------

vm_level_t view_model_co2_level(const measurement_f32_t *co2)
{
    if (!measurement_has_value(co2)) {
        return VM_LEVEL_UNKNOWN;
    }
    if (co2->value < VM_CO2_GOOD_MAX_PPM) {
        return VM_LEVEL_GOOD;
    }
    if (co2->value < VM_CO2_MODERATE_MAX_PPM) {
        return VM_LEVEL_MODERATE;
    }
    if (co2->value < VM_CO2_POOR_MAX_PPM) {
        return VM_LEVEL_POOR;
    }
    return VM_LEVEL_VERY_POOR;
}

const char *view_model_level_text(vm_level_t level)
{
    switch (level) {
        case VM_LEVEL_GOOD:
            return "Good";
        case VM_LEVEL_MODERATE:
            return "Moderate";
        case VM_LEVEL_POOR:
            return "Poor";
        case VM_LEVEL_VERY_POOR:
            return "Very poor";
        case VM_LEVEL_UNKNOWN:
            break;
    }
    return "--";
}

// --- age -------------------------------------------------------------------

#define VM_SECOND_MS 1000u
#define VM_MINUTE_MS 60000u
#define VM_HOUR_MS 3600000u

static void format_age(char *dst, size_t cap, uint64_t age_ms)
{
    if (cap == 0u) {
        return;
    }
    dst[0] = '\0';

    size_t len;
    if (age_ms < VM_MINUTE_MS) {
        len = str_append_u32(dst, cap, 0u, (uint32_t)(age_ms / VM_SECOND_MS));
        (void)str_append(dst, cap, len, " s");
    } else if (age_ms < VM_HOUR_MS) {
        len = str_append_u32(dst, cap, 0u, (uint32_t)(age_ms / VM_MINUTE_MS));
        (void)str_append(dst, cap, len, " min");
    } else {
        len = str_append_u32(dst, cap, 0u, (uint32_t)(age_ms / VM_HOUR_MS));
        (void)str_append(dst, cap, len, " h");
    }
}

// --- assembly --------------------------------------------------------------

static void count_status(const measurement_f32_t *m, uint8_t *errors, uint8_t *stales,
                         uint8_t *warmups)
{
    switch (m->status) {
        case VAL_ERROR:
            (*errors)++;
            break;
        case VAL_STALE:
            (*stales)++;
            break;
        case VAL_WARMUP:
            (*warmups)++;
            break;
        case VAL_OK:
        case VAL_UNAVAILABLE:
            break;
    }
}

static void build_status_text(view_model_t *vm)
{
    char *dst = vm->status_text;
    const size_t cap = sizeof(vm->status_text);

    dst[0] = '\0';

    if (vm->error_count > 0u) {
        size_t len = str_append_u32(dst, cap, 0u, vm->error_count);
        (void)str_append(dst, cap, len,
                         (vm->error_count == 1u) ? " sensor error" : " sensor errors");
        return;
    }
    if (vm->warmup_count > 0u) {
        (void)str_append(dst, cap, 0u, "sensors warming up");
        return;
    }
    if (vm->stale_count > 0u) {
        (void)str_append(dst, cap, 0u, "data is stale");
        return;
    }
    (void)str_append(dst, cap, 0u, "all sensors OK");
}

void view_model_build(view_model_t *vm, const air_reading_t *reading, uint64_t now_ms)
{
    memset(vm, 0, sizeof(*vm));

    build_field(&vm->fields[VM_FIELD_CO2], &reading->co2_ppm, "CO2", "ppm", 0u);
    // No degree sign: the generated fonts cover printable ASCII only.
    build_field(&vm->fields[VM_FIELD_TEMPERATURE], &reading->temperature_c, "Temperature", "C", 1u);
    build_field(&vm->fields[VM_FIELD_HUMIDITY], &reading->humidity_rh, "Humidity", "%", 0u);
    build_field(&vm->fields[VM_FIELD_PRESSURE], &reading->pressure_hpa, "Pressure", "hPa", 0u);
    build_field(&vm->fields[VM_FIELD_IAQ], &reading->iaq, "IAQ", "", 0u);
    build_field(&vm->battery, &reading->battery_percent, "Battery", "%", 0u);

    vm->co2_level = view_model_co2_level(&reading->co2_ppm);
    vm->co2_level_text = view_model_level_text(vm->co2_level);
    vm->co2_warning = (vm->co2_level == VM_LEVEL_POOR || vm->co2_level == VM_LEVEL_VERY_POOR);

    vm->net_connected = reading->net_connected;
    vm->net_text = reading->net_connected ? "Wi-Fi" : "offline";

    // The "last update" line reports the freshest thing on the screen: if any
    // one quantity is current, the picture as a whole is current.
    uint64_t youngest_ms = MEASUREMENT_AGE_UNKNOWN;
    for (size_t i = 0u; i < (size_t)VM_FIELD_COUNT; ++i) {
        if (!vm->fields[i].has_value) {
            continue;
        }
        const measurement_f32_t *m = NULL;
        switch ((vm_field_t)i) {
            case VM_FIELD_CO2:
                m = &reading->co2_ppm;
                break;
            case VM_FIELD_TEMPERATURE:
                m = &reading->temperature_c;
                break;
            case VM_FIELD_HUMIDITY:
                m = &reading->humidity_rh;
                break;
            case VM_FIELD_PRESSURE:
                m = &reading->pressure_hpa;
                break;
            case VM_FIELD_IAQ:
                m = &reading->iaq;
                break;
            case VM_FIELD_COUNT:
                break;
        }
        if (m == NULL) {
            continue;
        }
        const uint64_t age = measurement_age_ms(m, now_ms);
        if (age < youngest_ms) {
            youngest_ms = age;
        }
    }

    if (youngest_ms == MEASUREMENT_AGE_UNKNOWN) {
        vm->has_updated = false;
        (void)str_append(vm->updated_text, sizeof(vm->updated_text), 0u, "--");
    } else {
        vm->has_updated = true;
        format_age(vm->updated_text, sizeof(vm->updated_text), youngest_ms);
    }

    count_status(&reading->co2_ppm, &vm->error_count, &vm->stale_count, &vm->warmup_count);
    count_status(&reading->temperature_c, &vm->error_count, &vm->stale_count, &vm->warmup_count);
    count_status(&reading->humidity_rh, &vm->error_count, &vm->stale_count, &vm->warmup_count);
    count_status(&reading->pressure_hpa, &vm->error_count, &vm->stale_count, &vm->warmup_count);
    count_status(&reading->iaq, &vm->error_count, &vm->stale_count, &vm->warmup_count);

    build_status_text(vm);
}
