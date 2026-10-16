// SPDX-License-Identifier: Apache-2.0

#include "app/sensor_manager.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "sensor_manager"

// The most calls one slot gets in one poll: init(), start() and read() when
// each is due the moment the one before returns -- a replay source, ready as
// soon as it starts. A bound, so no source can keep a poll from returning.
#define STEPS_PER_POLL 3u

// The merge's order of preference among statuses (sensor_manager.h).
#define RANK_NONE (-1)
#define RANK_UNAVAILABLE 0
#define RANK_ERROR 1
#define RANK_WARMUP 2
#define RANK_STALE 3
#define RANK_OK 4

static const char *status_name(hal_status_t status)
{
    switch (status) {
        case HAL_OK:
            return "ok";
        case HAL_ERR_TIMEOUT:
            return "timeout";
        case HAL_ERR_IO:
            return "io";
        case HAL_ERR_NOT_FOUND:
            return "not_found";
        case HAL_ERR_INVALID_ARG:
            return "invalid_arg";
        case HAL_ERR_NO_MEM:
            return "no_mem";
        case HAL_ERR_UNSUPPORTED:
            return "unsupported";
        case HAL_ERR_BUSY:
            return "busy";
        case HAL_ERR_NOT_READY:
            return "not_ready";
        default:
            return "?";
    }
}

const char *sensor_manager_slot_state_name(sensor_slot_state_t state)
{
    switch (state) {
        case SENSOR_SLOT_UNINIT:
            return "uninit";
        case SENSOR_SLOT_IDLE:
            return "idle";
        case SENSOR_SLOT_MEASURING:
            return "measuring";
        case SENSOR_SLOT_BACKOFF:
            return "backoff";
        default:
            return "?";
    }
}

sensor_manager_config_t sensor_manager_config_default(void)
{
    const sensor_manager_config_t cfg = {
        .max_age_ms = SENSOR_MANAGER_DEFAULT_MAX_AGE_MS,
        .stall_timeout_ms = SENSOR_MANAGER_DEFAULT_STALL_TIMEOUT_MS,
        .failures_before_reinit = SENSOR_MANAGER_DEFAULT_FAILURES_BEFORE_REINIT,
        .backoff_first_ms = SENSOR_MANAGER_DEFAULT_BACKOFF_FIRST_MS,
        .backoff_max_ms = SENSOR_MANAGER_DEFAULT_BACKOFF_MAX_MS,
        .retry_ms = SENSOR_MANAGER_DEFAULT_RETRY_MS,
        .pressure_delta_hpa = SENSOR_MANAGER_DEFAULT_PRESSURE_DELTA_HPA,
    };
    return cfg;
}

void sensor_manager_init(sensor_manager_t *m, const port_clock_t *clock, const port_log_t *log,
                         const sensor_manager_config_t *cfg)
{
    memset(m, 0, sizeof(*m));
    m->clock = clock;
    m->log = log;
    m->cfg = *cfg;
    if (m->cfg.failures_before_reinit == 0u) {
        m->cfg.failures_before_reinit = 1u;  // 0 would re-init before the first failure
    }
    if (m->cfg.retry_ms == 0u) {
        m->cfg.retry_ms = 1u;  // the floor that keeps a "not yet" from spinning
    }
    m->next_deadline_ms = SENSOR_MANAGER_NO_DEADLINE;
}

static sensor_manager_slot_t *slot_by_id(sensor_manager_t *m, uint8_t id)
{
    for (size_t i = 0u; i < m->count; ++i) {
        if (m->slots[i].source->id == id) {
            return &m->slots[i];
        }
    }
    return NULL;
}

hal_status_t sensor_manager_add(sensor_manager_t *m, sensor_source_t *source, uint32_t interval_ms)
{
    if (source == NULL || source->id == 0u || interval_ms == 0u ||
        slot_by_id(m, source->id) != NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    if (m->count >= SENSOR_MANAGER_MAX_SOURCES) {
        return HAL_ERR_NO_MEM;
    }

    sensor_manager_slot_t *slot = &m->slots[m->count++];
    memset(slot, 0, sizeof(*slot));  // every field VAL_UNAVAILABLE, due at once
    slot->source = source;
    slot->interval_ms = interval_ms;
    slot->state = SENSOR_SLOT_UNINIT;
    m->next_deadline_ms = 0u;  // the new source wants its init() now
    return HAL_OK;
}

hal_status_t sensor_manager_set_priority(sensor_manager_t *m, sensor_field_t field,
                                         const uint8_t *ids, size_t count)
{
    if ((unsigned)field >= (unsigned)SENSOR_FIELD_COUNT || count > SENSOR_MANAGER_MAX_SOURCES ||
        (count > 0u && ids == NULL)) {
        return HAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (ids[i] == 0u) {
            return HAL_ERR_INVALID_ARG;
        }
    }
    memset(m->priority[field], 0, sizeof(m->priority[field]));
    for (size_t i = 0u; i < count; ++i) {
        m->priority[field][i] = ids[i];
    }
    return HAL_OK;
}

// --- fields ------------------------------------------------------------------

static sensor_caps_t cap_of(sensor_field_t field)
{
    switch (field) {
        case SENSOR_FIELD_TEMPERATURE:
            return SENS_CAP_TEMPERATURE;
        case SENSOR_FIELD_HUMIDITY:
            return SENS_CAP_HUMIDITY;
        case SENSOR_FIELD_PRESSURE:
            return SENS_CAP_PRESSURE;
        case SENSOR_FIELD_CO2:
            return SENS_CAP_CO2;
        case SENSOR_FIELD_IAQ:
            return SENS_CAP_IAQ;
        case SENSOR_FIELD_VOC_INDEX:
            return SENS_CAP_VOC;
        case SENSOR_FIELD_GAS_RESISTANCE:
        default:
            return SENS_CAP_GAS_RES;
    }
}

static measurement_f32_t *sample_field(sensor_sample_t *s, sensor_field_t field)
{
    switch (field) {
        case SENSOR_FIELD_TEMPERATURE:
            return &s->temperature_c;
        case SENSOR_FIELD_HUMIDITY:
            return &s->humidity_rh;
        case SENSOR_FIELD_PRESSURE:
            return &s->pressure_hpa;
        case SENSOR_FIELD_CO2:
            return &s->co2_ppm;
        case SENSOR_FIELD_IAQ:
            return &s->iaq;
        case SENSOR_FIELD_VOC_INDEX:
            return &s->voc_index;
        case SENSOR_FIELD_GAS_RESISTANCE:
        default:
            return &s->gas_resistance_ohm;
    }
}

static measurement_f32_t *reading_field(air_reading_t *r, sensor_field_t field)
{
    switch (field) {
        case SENSOR_FIELD_TEMPERATURE:
            return &r->temperature_c;
        case SENSOR_FIELD_HUMIDITY:
            return &r->humidity_rh;
        case SENSOR_FIELD_PRESSURE:
            return &r->pressure_hpa;
        case SENSOR_FIELD_CO2:
            return &r->co2_ppm;
        case SENSOR_FIELD_IAQ:
            return &r->iaq;
        case SENSOR_FIELD_VOC_INDEX:
            return &r->voc_index;
        case SENSOR_FIELD_GAS_RESISTANCE:
        default:
            return &r->gas_resistance_ohm;
    }
}

static bool has_cap(const sensor_manager_slot_t *slot, sensor_field_t field)
{
    return (slot->source->caps & cap_of(field)) != 0u;
}

// --- logging -----------------------------------------------------------------

static void log_slot(const sensor_manager_t *m, log_level_t level, const char *msg,
                     const sensor_manager_slot_t *slot, const log_field_t *extra,
                     size_t extra_count)
{
    log_field_t fields[LOG_FIELDS_MAX];
    size_t n = 0u;
    fields[n++] = log_str("source", slot->source->name != NULL ? slot->source->name : "?");
    fields[n++] = log_u64("id", slot->source->id);
    for (size_t i = 0u; i < extra_count && n < LOG_FIELDS_MAX; ++i) {
        fields[n++] = extra[i];
    }
    port_log_write(m->log, level, LOG_TAG, msg, fields, n);
}

// --- the life cycle ----------------------------------------------------------

static uint64_t now_of(const sensor_manager_t *m)
{
    return m->clock->now_ms(m->clock);
}

static uint64_t backoff_ms(const sensor_manager_t *m, uint32_t attempts)
{
    uint64_t ms = m->cfg.backoff_first_ms;
    for (uint32_t i = 0u; i < attempts && ms < m->cfg.backoff_max_ms; ++i) {
        ms *= 2u;
    }
    return (ms < m->cfg.backoff_max_ms) ? ms : m->cfg.backoff_max_ms;
}

static bool stalled(const sensor_manager_t *m, const sensor_manager_slot_t *slot, uint64_t now)
{
    return slot->in_cycle && now >= slot->cycle_started_ms &&
           now - slot->cycle_started_ms >= m->cfg.stall_timeout_ms;
}

// Every field the source claims, VAL_ERROR as of `now` -- whatever read()
// left in its output, which a failing source need not have filled in.
static void mark_failed(sensor_manager_slot_t *slot, uint64_t now)
{
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (has_cap(slot, (sensor_field_t)f)) {
            *sample_field(&slot->sample, (sensor_field_t)f) =
                measurement_status(VAL_ERROR, now, slot->source->id);
        }
    }
}

static void note_failure(sensor_manager_t *m, sensor_manager_slot_t *slot, const char *step,
                         hal_status_t status, uint64_t now)
{
    slot->failures++;
    slot->in_cycle = false;
    mark_failed(slot, now);
    if (!slot->failing) {
        const log_field_t extra[] = {log_str("step", step), log_str("status", status_name(status))};
        log_slot(m, LOG_LEVEL_WARN, "source failed", slot, extra, sizeof(extra) / sizeof(extra[0]));
        slot->failing = true;
    }
}

static void schedule_reinit(sensor_manager_t *m, sensor_manager_slot_t *slot, uint64_t now)
{
    const uint64_t wait_ms = backoff_ms(m, slot->reinit_attempts);
    if (slot->reinit_attempts < UINT32_MAX) {
        slot->reinit_attempts++;
    }
    slot->consecutive_failures = 0u;
    slot->state = SENSOR_SLOT_BACKOFF;
    slot->due_ms = now + wait_ms;

    const log_field_t extra[] = {
        log_u64("in_ms", wait_ms),
        log_u64("attempt", slot->reinit_attempts),
    };
    log_slot(m, LOG_LEVEL_INFO, "source will be initialised again", slot, extra,
             sizeof(extra) / sizeof(extra[0]));
}

// A start() or read() that failed, or a measurement that never came: retry at
// the next interval, or start the source over once that has happened
// failures_before_reinit times in a row.
static void fail(sensor_manager_t *m, sensor_manager_slot_t *slot, const char *step,
                 hal_status_t status, uint64_t now)
{
    note_failure(m, slot, step, status, now);
    slot->consecutive_failures++;
    if (slot->consecutive_failures >= m->cfg.failures_before_reinit) {
        schedule_reinit(m, slot, now);
        return;
    }
    // A failing source is not asked more often than a working one.
    slot->state = SENSOR_SLOT_IDLE;
    slot->next_start_ms = now + slot->interval_ms;
    slot->due_ms = slot->next_start_ms;
}

static void do_init(sensor_manager_t *m, sensor_manager_slot_t *slot)
{
    sensor_source_t *src = slot->source;
    slot->inits++;
    const hal_status_t status = src->init(src);
    const uint64_t now = now_of(m);

    if (status != HAL_OK) {
        note_failure(m, slot, "init", status, now);
        schedule_reinit(m, slot, now);
        return;
    }

    const log_field_t extra[] = {
        log_u64("inits", slot->inits),
        log_bool("after_failure", slot->failing),
    };
    log_slot(m, LOG_LEVEL_INFO, "source initialised", slot, extra,
             sizeof(extra) / sizeof(extra[0]));

    slot->ever_ready = true;
    slot->state = SENSOR_SLOT_IDLE;
    slot->due_ms = now;
    slot->next_start_ms = now;
    slot->in_cycle = false;
    slot->consecutive_failures = 0u;
    slot->ambient_sent = false;  // init() restored the sensor's own defaults
}

static void do_start(sensor_manager_t *m, sensor_manager_slot_t *slot)
{
    sensor_source_t *src = slot->source;
    if (!slot->in_cycle) {
        const uint64_t begin = now_of(m);
        slot->in_cycle = true;
        slot->cycle_started_ms = begin;
        slot->next_start_ms = begin + slot->interval_ms;
    }

    const hal_status_t status = src->start(src);
    const uint64_t now = now_of(m);

    if (status == HAL_OK) {
        slot->state = SENSOR_SLOT_MEASURING;
        slot->due_ms = now + src->ready_in_ms(src);
    } else if (status == HAL_ERR_BUSY || status == HAL_ERR_NOT_READY) {
        if (stalled(m, slot, now)) {
            fail(m, slot, "start_stalled", HAL_ERR_TIMEOUT, now);
        } else {
            slot->due_ms = now + m->cfg.retry_ms;
        }
    } else {
        fail(m, slot, "start", status, now);
    }
}

// The fields the source claims, as it reported them; anything else it filled
// in is dropped. A source that does not advertise CO2 cannot put a number in
// the CO2 field, however plausible.
static void accept(sensor_manager_slot_t *slot, const sensor_sample_t *sample)
{
    slot->sample = *sample;
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        if (!has_cap(slot, (sensor_field_t)f)) {
            *sample_field(&slot->sample, (sensor_field_t)f) = measurement_unavailable();
        }
    }
}

static void do_read(sensor_manager_t *m, sensor_manager_slot_t *slot)
{
    sensor_source_t *src = slot->source;
    sensor_sample_t sample;
    memset(&sample, 0, sizeof(sample));  // every field VAL_UNAVAILABLE

    const hal_status_t status = src->read(src, &sample);
    const uint64_t now = now_of(m);

    if (status == HAL_OK) {
        accept(slot, &sample);
        slot->reads++;
        slot->in_cycle = false;
        slot->consecutive_failures = 0u;
        slot->reinit_attempts = 0u;
        if (slot->failing) {
            log_slot(m, LOG_LEVEL_INFO, "source reading again", slot, NULL, 0u);
            slot->failing = false;
        }
        slot->state = SENSOR_SLOT_IDLE;
        slot->due_ms = (slot->next_start_ms > now) ? slot->next_start_ms : now;
        return;
    }
    if (status == HAL_ERR_NOT_READY) {
        if (stalled(m, slot, now)) {
            fail(m, slot, "read_stalled", HAL_ERR_TIMEOUT, now);
            return;
        }
        const uint32_t ready_in = src->ready_in_ms(src);
        slot->due_ms = now + ((ready_in > m->cfg.retry_ms) ? ready_in : m->cfg.retry_ms);
        return;
    }
    fail(m, slot, "read", status, now);
}

static void run_slot(sensor_manager_t *m, sensor_manager_slot_t *slot)
{
    for (unsigned step = 0u; step < STEPS_PER_POLL; ++step) {
        if (slot->due_ms > now_of(m)) {
            return;
        }
        switch (slot->state) {
            case SENSOR_SLOT_UNINIT:
            case SENSOR_SLOT_BACKOFF:
                do_init(m, slot);
                break;
            case SENSOR_SLOT_IDLE:
                do_start(m, slot);
                break;
            case SENSOR_SLOT_MEASURING:
            default:
                do_read(m, slot);
                break;
        }
    }
}

// --- the merge ---------------------------------------------------------------

static int rank_of(value_status_t status)
{
    switch (status) {
        case VAL_OK:
            return RANK_OK;
        case VAL_STALE:
            return RANK_STALE;
        case VAL_WARMUP:
            return RANK_WARMUP;
        case VAL_ERROR:
            return RANK_ERROR;
        case VAL_UNAVAILABLE:
        default:
            return RANK_UNAVAILABLE;
    }
}

// Keeps `candidate` if it beats `*best`; ties go to whoever came first.
static void consider(const sensor_manager_t *m, const sensor_manager_slot_t *slot,
                     sensor_field_t field, uint64_t now, measurement_f32_t *best, int *best_rank)
{
    if (slot == NULL || !has_cap(slot, field)) {
        return;
    }
    sensor_sample_t copy = slot->sample;
    measurement_f32_t candidate = *sample_field(&copy, field);
    measurement_apply_age(&candidate, now, m->cfg.max_age_ms);
    const int rank = rank_of(candidate.status);
    if (rank > *best_rank) {
        *best = candidate;
        *best_rank = rank;
    }
}

static measurement_f32_t merge_field(sensor_manager_t *m, sensor_field_t field, uint64_t now)
{
    measurement_f32_t best = measurement_unavailable();
    int best_rank = RANK_NONE;

    if (m->priority[field][0] != 0u) {
        for (size_t i = 0u; i < SENSOR_MANAGER_MAX_SOURCES && m->priority[field][i] != 0u; ++i) {
            consider(m, slot_by_id(m, m->priority[field][i]), field, now, &best, &best_rank);
        }
    } else {
        for (size_t i = 0u; i < m->count; ++i) {
            consider(m, &m->slots[i], field, now, &best, &best_rank);
        }
    }
    return best;
}

// --- ambient compensation (docs/adr/0018) ------------------------------------

static void hand_ambient(sensor_manager_t *m, const air_reading_t *merged)
{
    if (merged->pressure_hpa.status != VAL_OK) {
        return;  // nothing trustworthy to hand over; the sources keep what they have
    }
    const float pressure = merged->pressure_hpa.value;
    const sensor_ambient_t ambient = {.pressure_hpa = merged->pressure_hpa};

    for (size_t i = 0u; i < m->count; ++i) {
        sensor_manager_slot_t *slot = &m->slots[i];
        sensor_source_t *src = slot->source;
        if (src->set_ambient == NULL ||
            (slot->state != SENSOR_SLOT_IDLE && slot->state != SENSOR_SLOT_MEASURING)) {
            continue;
        }
        if (slot->ambient_sent &&
            fabsf(pressure - slot->ambient_pressure_hpa) < m->cfg.pressure_delta_hpa) {
            continue;
        }

        const hal_status_t status = src->set_ambient(src, &ambient);
        if (status != HAL_OK && status != HAL_ERR_INVALID_ARG) {
            continue;  // busy, or the bus: asked again at the next poll
        }
        const bool first = !slot->ambient_sent;
        slot->ambient_sent = true;  // an INVALID_ARG value is not offered again either
        slot->ambient_pressure_hpa = pressure;

        const log_field_t extra[] = {
            log_f32("pressure_hpa", pressure),
            log_str("status", status_name(status)),
        };
        log_slot(m, (first || status != HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_DEBUG,
                 "ambient handed over", slot, extra, sizeof(extra) / sizeof(extra[0]));
    }
}

// --- polling -----------------------------------------------------------------

uint64_t sensor_manager_poll(sensor_manager_t *m, air_reading_t *reading)
{
    for (size_t i = 0u; i < m->count; ++i) {
        run_slot(m, &m->slots[i]);
    }

    const uint64_t now = now_of(m);
    for (unsigned f = 0u; f < (unsigned)SENSOR_FIELD_COUNT; ++f) {
        *reading_field(reading, (sensor_field_t)f) = merge_field(m, (sensor_field_t)f, now);
    }
    hand_ambient(m, reading);

    uint64_t next = SENSOR_MANAGER_NO_DEADLINE;
    for (size_t i = 0u; i < m->count; ++i) {
        if (m->slots[i].due_ms < next) {
            next = m->slots[i].due_ms;
        }
    }
    if (next != SENSOR_MANAGER_NO_DEADLINE && next < now) {
        next = now;  // something is still due: a slot used up its steps for this poll
    }
    m->next_deadline_ms = next;
    return next;
}

uint64_t sensor_manager_next_deadline(const sensor_manager_t *m)
{
    return m->next_deadline_ms;
}

const sensor_manager_slot_t *sensor_manager_slot(const sensor_manager_t *m, size_t index)
{
    return (index < m->count) ? &m->slots[index] : NULL;
}
