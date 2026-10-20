// SPDX-License-Identifier: Apache-2.0
//
// core/reporting/publish_policy.h at its boundaries: a session's first
// message, every delta at exactly its value and just past it, every status
// and IAQ's accuracy, the heartbeat to the millisecond, one message per
// sensor cycle, and a clock that restarted.

#include <string.h>

#include "hac_test.h"
#include "reporting/publish_policy.h"
#include "reporting/report_limits.h"

#define TS 1000u

static air_reading_t reading(void)
{
    air_reading_t r;
    air_reading_init(&r);
    // 0 C: the one value where 0.2 more is exactly the float 0.2, so that a
    // move of exactly the delta can be told from one just past it.
    r.temperature_c = measurement_ok(0.0f, TS, 2u);
    r.humidity_rh = measurement_ok(45.0f, TS, 2u);
    r.pressure_hpa = measurement_ok(1006.0f, TS, 2u);
    r.co2_ppm = measurement_ok(800.0f, TS, 1u);
    r.iaq = measurement_ok(50.0f, TS, 2u);
    r.iaq.accuracy = ACC_HIGH;
    r.gas_resistance_ohm = measurement_ok(150000.0f, TS, 2u);
    return r;
}

static publish_policy_t committed_at(uint64_t t_ms, const air_reading_t *r)
{
    publish_policy_t p;
    const publish_policy_config_t cfg = publish_policy_config_default();
    publish_policy_init(&p, &cfg);
    publish_policy_commit(&p, r, t_ms);
    return p;
}

static publish_decision_t at(const publish_policy_t *p, const air_reading_t *r, uint64_t t_ms)
{
    publish_decision_t d;
    publish_policy_evaluate(p, r, t_ms, &d);
    return d;
}

static void the_defaults_are_report_limits(void)
{
    const publish_policy_config_t cfg = publish_policy_config_default();
    HAC_CHECK_EQ_F32(cfg.co2_delta_ppm, REPORT_CO2_DELTA_PPM, 0.0f);
    HAC_CHECK_EQ_F32(cfg.temperature_delta_c, REPORT_TEMPERATURE_DELTA_C, 0.0f);
    HAC_CHECK_EQ_F32(cfg.humidity_delta_rh, REPORT_HUMIDITY_DELTA_RH, 0.0f);
    HAC_CHECK_EQ_F32(cfg.pressure_delta_hpa, REPORT_PRESSURE_DELTA_HPA, 0.0f);
    HAC_CHECK_EQ_F32(cfg.iaq_delta, REPORT_IAQ_DELTA, 0.0f);
    HAC_CHECK_EQ_U64(cfg.heartbeat_ms, REPORT_HEARTBEAT_MS);
    HAC_CHECK_EQ_U64(cfg.min_interval_ms, SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
}

static void a_session_publishes_at_once(void)
{
    publish_policy_t p;
    const publish_policy_config_t cfg = publish_policy_config_default();
    publish_policy_init(&p, &cfg);
    const air_reading_t r = reading();
    publish_decision_t d = at(&p, &r, 0u);
    HAC_CHECK(d.due);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_SESSION);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, 0u);

    // A new session a second after the last message: at once, not held.
    publish_policy_commit(&p, &r, 100000u);
    publish_policy_reset(&p);
    d = at(&p, &r, 101000u);
    HAC_CHECK(d.due && !d.held_for_interval);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_SESSION);
}

static void nothing_changed_waits_for_the_heartbeat(void)
{
    const air_reading_t r = reading();
    const publish_policy_t p = committed_at(10000u, &r);
    publish_decision_t d = at(&p, &r, 10000u + REPORT_HEARTBEAT_MS - 1u);
    HAC_CHECK(!d.due);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_NONE);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, 10000u + REPORT_HEARTBEAT_MS);
    d = at(&p, &r, 10000u + REPORT_HEARTBEAT_MS);
    HAC_CHECK(d.due);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_HEARTBEAT);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, 10000u + REPORT_HEARTBEAT_MS);
}

typedef struct {
    measurement_f32_t *(*field)(air_reading_t *r);
    float delta;
    publish_reason_t reason;
    const char *name;
} delta_case_t;

static measurement_f32_t *co2(air_reading_t *r)
{
    return &r->co2_ppm;
}
static measurement_f32_t *temperature(air_reading_t *r)
{
    return &r->temperature_c;
}
static measurement_f32_t *humidity(air_reading_t *r)
{
    return &r->humidity_rh;
}
static measurement_f32_t *pressure(air_reading_t *r)
{
    return &r->pressure_hpa;
}
static measurement_f32_t *iaq(air_reading_t *r)
{
    return &r->iaq;
}

static void each_value_publishes_past_its_delta_not_at_it(void)
{
    const delta_case_t cases[] = {
        {co2, REPORT_CO2_DELTA_PPM, PUBLISH_REASON_CO2, "co2"},
        {temperature, REPORT_TEMPERATURE_DELTA_C, PUBLISH_REASON_TEMPERATURE, "temperature"},
        {humidity, REPORT_HUMIDITY_DELTA_RH, PUBLISH_REASON_HUMIDITY, "humidity"},
        {pressure, REPORT_PRESSURE_DELTA_HPA, PUBLISH_REASON_PRESSURE, "pressure"},
        {iaq, REPORT_IAQ_DELTA, PUBLISH_REASON_IAQ, "iaq"},
    };
    const uint64_t t = 10000u + REPORT_MIN_INTERVAL_MS;
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const air_reading_t base = reading();
        const publish_policy_t p = committed_at(10000u, &base);
        for (int sign = -1; sign <= 1; sign += 2) {
            air_reading_t r = base;
            measurement_f32_t *m = cases[i].field(&r);
            const float v0 = m->value;
            m->value = v0 + (float)sign * cases[i].delta;
            publish_decision_t d = at(&p, &r, t);
            hac_test_check(!d.due, cases[i].name, "exactly the delta", __LINE__);
            // A step the float can tell apart from the delta, at this magnitude.
            m->value = v0 + (float)sign * cases[i].delta * 1.001f;
            d = at(&p, &r, t);
            hac_test_check(d.due && d.reason == cases[i].reason, cases[i].name,
                           "just past the delta", __LINE__);
        }
    }
}

static void gas_resistance_and_the_link_ride_along(void)
{
    const air_reading_t base = reading();
    const publish_policy_t p = committed_at(10000u, &base);
    air_reading_t r = base;
    r.gas_resistance_ohm.value *= 2.0f;
    r.wifi_rssi_dbm = -80;
    r.net_connected = !base.net_connected;
    r.uptime_s += 3600u;
    r.temperature_c.accuracy = ACC_LOW;  // not in the message: only IAQ's is
    const publish_decision_t d = at(&p, &r, 10000u + REPORT_MIN_INTERVAL_MS);
    HAC_CHECK(!d.due);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, 10000u + REPORT_HEARTBEAT_MS);
}

static void every_status_and_iaq_accuracy_publish(void)
{
    const uint64_t t = 10000u + REPORT_MIN_INTERVAL_MS;
    const air_reading_t base = reading();
    const publish_policy_t p = committed_at(10000u, &base);
    measurement_f32_t *(*const fields[])(air_reading_t *) = {co2, temperature, humidity, pressure,
                                                             iaq};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        air_reading_t r = base;
        fields[i](&r)->status = VAL_STALE;
        const publish_decision_t d = at(&p, &r, t);
        HAC_CHECK(d.due && d.reason == PUBLISH_REASON_STATUS);
    }
    air_reading_t r = base;
    r.gas_resistance_ohm.status = VAL_ERROR;
    HAC_CHECK_EQ_INT(at(&p, &r, t).reason, PUBLISH_REASON_STATUS);

    // Any step of IAQ's accuracy -- medium and high look alike on the glass,
    // not in the message.
    r = base;
    r.iaq.accuracy = ACC_MEDIUM;
    HAC_CHECK_EQ_INT(at(&p, &r, t).reason, PUBLISH_REASON_STATUS);

    // A value gained is a status change, not a delta.
    air_reading_t none = base;
    none.co2_ppm = measurement_unavailable();
    const publish_policy_t q = committed_at(10000u, &none);
    HAC_CHECK_EQ_INT(at(&q, &base, t).reason, PUBLISH_REASON_STATUS);
}

static void one_message_per_sensor_cycle(void)
{
    const air_reading_t base = reading();
    const publish_policy_t p = committed_at(10000u, &base);
    air_reading_t r = base;
    r.co2_ppm.value += 500.0f;
    publish_decision_t d = at(&p, &r, 10000u + 1000u);
    HAC_CHECK(!d.due && d.held_for_interval);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_CO2);
    HAC_CHECK_EQ_U64(d.next_deadline_ms, 10000u + REPORT_MIN_INTERVAL_MS);
    d = at(&p, &r, 10000u + REPORT_MIN_INTERVAL_MS - 1u);
    HAC_CHECK(!d.due);
    d = at(&p, &r, 10000u + REPORT_MIN_INTERVAL_MS);
    HAC_CHECK(d.due && !d.held_for_interval);
}

static void a_restarted_clock_publishes(void)
{
    const air_reading_t r = reading();
    const publish_policy_t p = committed_at(500000u, &r);
    const publish_decision_t d = at(&p, &r, 1000u);
    HAC_CHECK(d.due);
    HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_HEARTBEAT);
}

static void deltas_count_from_the_last_message_not_the_last_reading(void)
{
    // A slow drift: each step within the delta, the sum past it.
    const air_reading_t base = reading();
    publish_policy_t p = committed_at(0u, &base);
    air_reading_t r = base;
    uint64_t t = 0u;
    unsigned published = 0u;
    for (unsigned i = 0u; i < 6u; ++i) {
        t += REPORT_MIN_INTERVAL_MS;
        r.co2_ppm.value += 20.0f;  // 120 ppm in all
        const publish_decision_t d = at(&p, &r, t);
        if (d.due) {
            HAC_CHECK_EQ_INT(d.reason, PUBLISH_REASON_CO2);
            publish_policy_commit(&p, &r, t);
            published++;
        }
    }
    HAC_CHECK_EQ_INT((int)published, 2);  // past 50 at +60, past 50 again at +120
}

static void reasons_have_names(void)
{
    static const char *const names[] = {"none",     "session",  "status", "co2",      "temperature",
                                        "humidity", "pressure", "iaq",    "heartbeat"};
    for (unsigned i = 0u; i < (unsigned)PUBLISH_REASON_COUNT; ++i) {
        HAC_CHECK(strcmp(publish_policy_reason_name((publish_reason_t)i), names[i]) == 0);
    }
    HAC_CHECK(strcmp(publish_policy_reason_name(PUBLISH_REASON_COUNT), "?") == 0);
}

int main(void)
{
    HAC_RUN(the_defaults_are_report_limits);
    HAC_RUN(a_session_publishes_at_once);
    HAC_RUN(nothing_changed_waits_for_the_heartbeat);
    HAC_RUN(each_value_publishes_past_its_delta_not_at_it);
    HAC_RUN(gas_resistance_and_the_link_ride_along);
    HAC_RUN(every_status_and_iaq_accuracy_publish);
    HAC_RUN(one_message_per_sensor_cycle);
    HAC_RUN(a_restarted_clock_publishes);
    HAC_RUN(deltas_count_from_the_last_message_not_the_last_reading);
    HAC_RUN(reasons_have_names);
    return hac_test_summary();
}
