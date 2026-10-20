// SPDX-License-Identifier: Apache-2.0

#include "gt911_input.h"

#include <string.h>

#define LOG_TAG "gt911_input"

static void record(gt911_input_t *in, const input_event_t *ev, uint16_t raw_x, uint16_t raw_y)
{
    gt911_input_record_t *r = &in->history[in->history_next];
    r->kind = ev->kind;
    r->id = ev->id;
    r->raw_x = raw_x;
    r->raw_y = raw_y;
    r->x = ev->x;
    r->y = ev->y;
    r->ts_ms = ev->ts_ms;
    in->history_next = (in->history_next + 1u) % GT911_INPUT_HISTORY;
    if (in->history_count < GT911_INPUT_HISTORY) {
        in->history_count++;
    }
}

static void enqueue(gt911_input_t *in, const input_event_t *ev)
{
    if (in->count >= GT911_INPUT_QUEUE_LEN) {
        in->stats.dropped++;
        return;
    }
    in->queue[(in->head + in->count) % GT911_INPUT_QUEUE_LEN] = *ev;
    in->count++;
    in->stats.events++;
}

// One snapshot -- a report, or the empty one a timed-out lift stands for --
// through gt911_touch, its events queued and recorded.
static void apply_snapshot(gt911_input_t *in, const gt911_frame_t *frame, uint64_t now_ms)
{
    const gt911_info_t *info = gt911_info(&in->dev);
    gt911_touch_point_t points[GT911_MAX_POINTS];
    size_t n = 0u;
    for (size_t i = 0u; i < frame->count && i < GT911_MAX_POINTS; ++i) {
        points[n].track_id = frame->points[i].track_id;
        gt911_touch_map_point(&in->map, info->x_max, info->y_max, frame->points[i].x,
                              frame->points[i].y, &points[n].x, &points[n].y);
        ++n;
    }

    input_event_t events[GT911_TOUCH_MAX_EVENTS];
    const size_t count = gt911_touch_update(&in->touch, points, n, in->move_threshold_px, now_ms,
                                            events, GT911_TOUCH_MAX_EVENTS);

    for (size_t e = 0u; e < count; ++e) {
        const uint8_t place = events[e].id;
        // The raw position of the event's finger: this report's, or for a
        // lift the last one it had.
        if (events[e].kind != INPUT_TOUCH_UP) {
            const uint8_t track = in->touch.places[place].track_id;
            for (size_t i = 0u; i < frame->count && i < GT911_MAX_POINTS; ++i) {
                if (frame->points[i].track_id == track) {
                    in->raw_x[place] = frame->points[i].x;
                    in->raw_y[place] = frame->points[i].y;
                    break;
                }
            }
        }
        enqueue(in, &events[e]);
        record(in, &events[e], in->raw_x[place], in->raw_y[place]);
    }
}

static bool input_poll(port_input_t *port, input_event_t *out)
{
    gt911_input_t *in = (gt911_input_t *)port->impl;
    if (in->count == 0u) {
        return false;
    }
    *out = in->queue[in->head];
    in->head = (in->head + 1u) % GT911_INPUT_QUEUE_LEN;
    in->count--;
    return true;
}

hal_status_t gt911_input_setup(gt911_input_t *in, const gt911_input_config_t *cfg)
{
    memset(in, 0, sizeof(*in));
    if (cfg == NULL || cfg->map.panel_width == 0u || cfg->map.panel_height == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    const hal_status_t st = gt911_setup(&in->dev, &cfg->device);
    if (st != HAL_OK) {
        return st;
    }
    in->map = cfg->map;
    in->move_threshold_px = cfg->move_threshold_px;
    in->clock = cfg->device.clock;
    in->log = cfg->log;
    gt911_touch_init(&in->touch);
    in->port.poll = input_poll;
    in->port.impl = in;
    return HAL_OK;
}

// Lifts whatever is down, now: nothing will report it again.
static void lift_all(gt911_input_t *in, uint64_t now_ms)
{
    if (gt911_touch_any_down(&in->touch)) {
        const gt911_frame_t lifted = {.ready = true, .count = 0u};
        apply_snapshot(in, &lifted, now_ms);
    }
}

static hal_status_t try_init(gt911_input_t *in)
{
    in->tried = true;
    in->signalled = false;
    in->stats.init_attempts++;
    const hal_status_t st = gt911_init(&in->dev);
    in->ready = (st == HAL_OK);
    if (st != HAL_OK) {
        in->stats.last_error = st;
        in->next_init_ms = in->clock->now_ms(in->clock) + GT911_INPUT_RETRY_INIT_MS;
    }
    return st;
}

hal_status_t gt911_input_init(gt911_input_t *in)
{
    lift_all(in, in->clock->now_ms(in->clock));
    gt911_touch_init(&in->touch);
    return try_init(in);
}

bool gt911_input_ready(const gt911_input_t *in)
{
    return in->ready;
}

// A read failed: the controller is not where it was. Lift what was down,
// and look for it -- at both addresses, by probes alone -- at once.
static void lost(gt911_input_t *in, hal_status_t st, uint64_t now_ms)
{
    in->stats.losses++;
    in->stats.last_error = st;
    lift_all(in, now_ms);
    const log_field_t fields[] = {
        log_u64("address", gt911_info(&in->dev)->address),
        log_i64("status", (int64_t)st),
    };
    port_log_write(in->log, LOG_LEVEL_WARN, LOG_TAG,
                   "read failed: looking for the controller again", fields,
                   sizeof(fields) / sizeof(fields[0]));
    if (try_init(in) == HAL_OK) {
        in->stats.refound++;
    }
}

void gt911_input_signal(gt911_input_t *in)
{
    in->signalled = true;
    in->stats.signals++;
}

uint64_t gt911_input_service(gt911_input_t *in)
{
    const port_clock_t *clock = in->clock;
    if (!in->ready) {
        in->signalled = false;
        if (!in->tried) {
            return GT911_INPUT_NO_DEADLINE;  // never initialised: nothing to look for
        }
        if (clock->now_ms(clock) < in->next_init_ms || try_init(in) != HAL_OK) {
            return in->next_init_ms;
        }
        return GT911_INPUT_NO_DEADLINE;  // found again; nothing down yet
    }
    if (in->dev.asleep) {
        in->signalled = false;
        return GT911_INPUT_NO_DEADLINE;
    }
    const bool down = gt911_touch_any_down(&in->touch);
    const uint64_t before = clock->now_ms(clock);

    if (in->signalled || (down && before >= in->next_read_ms)) {
        in->signalled = false;
        in->stats.reads++;
        gt911_frame_t frame;
        const hal_status_t st = gt911_read_frame(&in->dev, &frame);
        // The events' time: when the report was read, after the transfers.
        const uint64_t now = clock->now_ms(clock);
        if (st != HAL_OK) {
            in->stats.read_errors++;
            lost(in, st, now);
            return in->ready ? GT911_INPUT_NO_DEADLINE : in->next_init_ms;
        }
        if (frame.ready) {
            in->stats.reports++;
            in->last_report_ms = now;
            apply_snapshot(in, &frame, now);
        } else {
            in->stats.empty_reads++;
        }
        in->next_read_ms = now + GT911_INPUT_POLL_WHILE_DOWN_MS;
    }

    const uint64_t now = clock->now_ms(clock);
    if (gt911_touch_any_down(&in->touch) &&
        now >= in->last_report_ms + GT911_INPUT_LIFT_TIMEOUT_MS) {
        const gt911_frame_t lifted = {.ready = true, .count = 0u};
        apply_snapshot(in, &lifted, now);
        in->stats.timed_out_lifts++;
        port_log_write(in->log, LOG_LEVEL_WARN, LOG_TAG,
                       "no report for the lift timeout with a finger down: lifted", NULL, 0u);
    }

    if (!gt911_touch_any_down(&in->touch)) {
        return GT911_INPUT_NO_DEADLINE;
    }
    if (in->next_read_ms < now) {
        in->next_read_ms = now;  // a read is due at once
    }
    const uint64_t lift_at = in->last_report_ms + GT911_INPUT_LIFT_TIMEOUT_MS;
    return (in->next_read_ms < lift_at) ? in->next_read_ms : lift_at;
}

port_input_t *gt911_input_port(gt911_input_t *in)
{
    return &in->port;
}

hal_status_t gt911_input_sleep(gt911_input_t *in)
{
    const hal_status_t st = gt911_sleep(&in->dev);
    if (st == HAL_OK) {
        lift_all(in, in->clock->now_ms(in->clock));
    }
    return st;
}

hal_status_t gt911_input_wake(gt911_input_t *in)
{
    const hal_status_t st = gt911_wake(&in->dev);
    if (st != HAL_OK) {
        in->stats.last_error = st;
    }
    return st;
}

const gt911_t *gt911_input_device(const gt911_input_t *in)
{
    return &in->dev;
}

const gt911_input_stats_t *gt911_input_stats(const gt911_input_t *in)
{
    return &in->stats;
}

size_t gt911_input_history(const gt911_input_t *in, gt911_input_record_t *out, size_t cap)
{
    const size_t n = (in->history_count < cap) ? in->history_count : cap;
    const size_t oldest =
        (in->history_next + GT911_INPUT_HISTORY - in->history_count) % GT911_INPUT_HISTORY;
    const size_t skip = in->history_count - n;
    for (size_t i = 0u; i < n; ++i) {
        out[i] = in->history[(oldest + skip + i) % GT911_INPUT_HISTORY];
    }
    return n;
}
