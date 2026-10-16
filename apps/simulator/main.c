// SPDX-License-Identifier: Apache-2.0
//
// The simulator: composition and a wait loop, and nothing else.
//
// This is the same shape as apps/firmware_esp32/main/main.c and does the same
// job (docs/architecture.md 6.2): build adapters, hand them to the logic, and
// implement waiting. The difference between running on a PC and running on the
// board is meant to live entirely in these two files -- so if something here
// starts looking like a decision about air quality rather than about wiring,
// it belongs in core/.
//
//   simulator --scenario-co2 scenarios/co2_spike_meeting.csv
//             --scenario-env scenarios/normal_day.csv --time-scale 60
//   simulator --headless --frames out/ --duration 600
//   simulator --sensors fake-i2c --scenario-co2 ... --scenario-env ...
//
// Two ways to the same sensor_source_t (architecture 5.4): replay sources
// that play the scenario files straight into the core, or the real drivers
// -- the SCD4x and BME68x code the board runs -- over the fake I2C bus,
// whose device models measure a room played from the same files
// (platform/host/scenario_world.h). Either way core/app/sensor_manager.c
// drives them, with the ids and priorities given below, as
// apps/firmware_esp32/main/main.c gives them on the board.
//
// Not yet wired, and deliberately: the control panel, the remaining screens,
// deep-sleep emulation, networking and telemetry. There is no app_core yet
// either: the loop below runs the sensor manager and the update policy
// itself, and sleeps until the earlier of their deadlines.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_display_png.h"
#include "adp_i2c_fake.h"
#include "adp_log_console.h"
#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "bme68x_limits.h"
#include "bme68x_source.h"
#include "board_config.h"  // plain data: where the board's sensors answer
#include "domain/measurement.h"
#include "epd_model.h"
#include "host_time.h"
#include "i2c_models/bme680_model.h"
#include "i2c_models/scd41_model.h"
#include "port_display.h"
#include "port_input.h"
#include "port_log.h"
#include "replay/replay_source.h"
#include "scd4x_source.h"
#include "scenario_world.h"
#include "ui/screen_home.h"
#include "ui/view_model.h"

#ifdef HAC_SIM_GUI
#include "adp_display_sdl.h"
#include "adp_input_sdl.h"
#endif

// The panel this build targets (docs/hardware/board_notes.md). It reaches the
// core only through port_display, which is why nothing in core/ repeats it.
#define SIM_PANEL_WIDTH 960u
#define SIM_PANEL_HEIGHT 540u

// The sources' ids, which every measurement carries as its source_id, and
// which the priorities below are written in -- the same two the firmware
// gives the same two sensors.
#define SIM_CO2_SOURCE_ID 1u  // the SCD41, or its replay stand-in
#define SIM_ENV_SOURCE_ID 2u  // the BME68x, or its replay stand-in

// Windowed: how long one loop iteration waits before looking again. Small
// enough that the window stays responsive, large enough not to spin a core.
#define SIM_TICK_MS 20u

// Headless with no deadline at all (no sources, no timer pending): how far
// virtual time moves per iteration.
#define SIM_HEADLESS_STEP_MS 1000u

// How soon to try again after a redraw that did not reach the panel.
#define SIM_REDRAW_RETRY_MS 1000u

// Headless runs must terminate on their own, or CI hangs.
#define SIM_DEFAULT_HEADLESS_DURATION_S 600u

typedef enum {
    SIM_SENSORS_REPLAY = 0,
    SIM_SENSORS_FAKE_I2C,
} sim_sensors_t;

typedef struct {
    const char *scenario_co2;
    const char *scenario_env;
    sim_sensors_t sensors;
    const char *frames_dir;
    double time_scale;
    float zoom;
    bool headless;
    uint32_t duration_s;  // 0 = until the window is closed
    bool duration_set;
} sim_args_t;

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "\n"
           "  --scenario-co2 <file>  CSV of what the SCD41 measures: CO2, temperature, humidity\n"
           "  --scenario-env <file>  CSV of what the BME68x measures: temperature, humidity,\n"
           "                         pressure, gas resistance\n"
           "  --sensors <mode>       replay (default): the files feed the core directly;\n"
           "                         fake-i2c: the real SCD4x and BME68x drivers read device\n"
           "                         models on a fake I2C bus, which measure the files\n"
           "  --time-scale <n>       virtual seconds per real second (default 1)\n"
           "  --headless             no window: write frames instead (implies a duration)\n"
           "  --frames <dir>         directory for the headless PNG frames\n"
           "  --zoom <f>             window scale, 1 = one panel pixel per screen pixel\n"
           "  --duration <s>         stop after this many virtual seconds (0 = never)\n"
           "  --help\n"
           "\n"
           "Panel: %ux%u, 16 grey levels. E-paper timings and ghosting are measured on the\n"
           "panel (platform/host/epd_emulation.h, docs/adr/0008-panel-timing.md). The window\n"
           "waits out each refresh divided by --time-scale: use 1 to see the real latency.\n",
           argv0, SIM_PANEL_WIDTH, SIM_PANEL_HEIGHT);
}

// Returns false on a malformed command line, having said what was wrong.
static bool parse_args(int argc, char **argv, sim_args_t *out)
{
    memset(out, 0, sizeof(*out));
    out->time_scale = 1.0;
    out->zoom = 1.0f;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const bool has_value = (i + 1 < argc);

#define TAKE_VALUE(name)                             \
    if (!has_value) {                                \
        fprintf(stderr, "%s needs a value\n", name); \
        return false;                                \
    }

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--scenario-co2") == 0) {
            TAKE_VALUE("--scenario-co2");
            out->scenario_co2 = argv[++i];
        } else if (strcmp(arg, "--scenario-env") == 0) {
            TAKE_VALUE("--scenario-env");
            out->scenario_env = argv[++i];
        } else if (strcmp(arg, "--sensors") == 0) {
            TAKE_VALUE("--sensors");
            const char *mode = argv[++i];
            if (strcmp(mode, "replay") == 0) {
                out->sensors = SIM_SENSORS_REPLAY;
            } else if (strcmp(mode, "fake-i2c") == 0) {
                out->sensors = SIM_SENSORS_FAKE_I2C;
            } else {
                fprintf(stderr, "--sensors must be replay or fake-i2c, not %s\n", mode);
                return false;
            }
        } else if (strcmp(arg, "--frames") == 0) {
            TAKE_VALUE("--frames");
            out->frames_dir = argv[++i];
        } else if (strcmp(arg, "--time-scale") == 0) {
            TAKE_VALUE("--time-scale");
            out->time_scale = atof(argv[++i]);
            if (out->time_scale <= 0.0) {
                fprintf(stderr, "--time-scale must be greater than 0\n");
                return false;
            }
        } else if (strcmp(arg, "--zoom") == 0) {
            TAKE_VALUE("--zoom");
            out->zoom = (float)atof(argv[++i]);
            if (out->zoom <= 0.0f) {
                fprintf(stderr, "--zoom must be greater than 0\n");
                return false;
            }
        } else if (strcmp(arg, "--duration") == 0) {
            TAKE_VALUE("--duration");
            out->duration_s = (uint32_t)strtoul(argv[++i], NULL, 10);
            out->duration_set = true;
        } else if (strcmp(arg, "--headless") == 0) {
            out->headless = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage(argv[0]);
            return false;
        }
#undef TAKE_VALUE
    }

    if (out->frames_dir != NULL && !out->headless) {
        // --frames without --headless would open a window and never write a
        // file, which is not what anyone means by it.
        out->headless = true;
    }
    if (out->headless && !out->duration_set) {
        out->duration_s = SIM_DEFAULT_HEADLESS_DURATION_S;
    }
    return true;
}

// --- sensors ---------------------------------------------------------------

// Everything either mode builds. One of each, static in main(): the
// drivers' state and the device models are too large for comfort on the
// stack, and the SCD4x driver allows one instance per image anyway.
typedef struct {
    // replay mode
    replay_source_t co2_replay;
    replay_source_t env_replay;

    // fake-i2c mode
    adp_i2c_fake_t bus;
    scd41_model_t scd41;
    bme680_model_t bme680;
    scenario_world_t world;
    bool world_loaded;
    scd4x_source_t scd4x;
    bme68x_source_t bme68x;

    sensor_source_t *sources[2];
    size_t count;
} sim_sensors_rig_t;

static const sensor_caps_t CO2_CAPS =
    (sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY);
static const sensor_caps_t ENV_CAPS = (sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                                      SENS_CAP_PRESSURE | SENS_CAP_GAS_RES);

// Replay: the CO2 file stands in for the SCD41, with the SCD41's three
// quantities, so the fallback of architecture 6.3 has something to fall back
// on; a column the file lacks is simply VAL_UNAVAILABLE.
static bool build_replay(sim_sensors_rig_t *r, const sim_args_t *args, const port_clock_t *clock)
{
    if (args->scenario_co2 != NULL) {
        const hal_status_t st = replay_source_init(&r->co2_replay, args->scenario_co2, CO2_CAPS,
                                                   SIM_CO2_SOURCE_ID, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args->scenario_co2, (int)st);
            return false;
        }
        r->sources[r->count++] = &r->co2_replay.port;
    }
    if (args->scenario_env != NULL) {
        const hal_status_t st = replay_source_init(&r->env_replay, args->scenario_env, ENV_CAPS,
                                                   SIM_ENV_SOURCE_ID, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args->scenario_env, (int)st);
            return false;
        }
        r->sources[r->count++] = &r->env_replay.port;
    }
    return true;
}

// Fake I2C: a model on the bus for each scenario given, at the address its
// sensor answers at on the board (board_config.h), measuring the room the
// files describe, and the board's own driver reading it. The BME680's
// die runs warmer than the room by exactly what its driver takes off, so the
// screen shows the room in both modes.
static bool build_fake_i2c(sim_sensors_rig_t *r, const sim_args_t *args,
                           adp_clock_virtual_t *vclock, const port_log_t *log)
{
    const port_clock_t *clock = adp_clock_virtual_port(vclock);
    const hal_status_t st = scenario_world_init(&r->world, args->scenario_co2, args->scenario_env,
                                                true, HAC_BME68X_SELF_HEATING_C);
    if (st != HAL_OK) {
        fprintf(stderr, "cannot load the scenario files (status %d)\n", (int)st);
        return false;
    }
    r->world_loaded = true;
    adp_i2c_fake_init(&r->bus, vclock);

    if (args->scenario_co2 != NULL) {
        scd41_model_init(&r->scd41, vclock->now_ms);
        (void)adp_i2c_fake_attach(&r->bus, BOARD_SCD4X_I2C_ADDRESS, scd41_model_port(&r->scd41));
        const scd4x_source_config_t cfg = {
            .bus = adp_i2c_fake_port(&r->bus),
            .clock = clock,
            .log = log,
            .address = BOARD_SCD4X_I2C_ADDRESS,
            .id = SIM_CO2_SOURCE_ID,
            .mode = SCD4X_MODE_PERIODIC,
        };
        (void)scd4x_source_setup(&r->scd4x, &cfg);
        r->sources[r->count++] = &r->scd4x.port;
    }
    if (args->scenario_env != NULL) {
        bme680_model_init(&r->bme680, vclock->now_ms);
        (void)adp_i2c_fake_attach(&r->bus, BOARD_BME68X_I2C_ADDRESS, bme680_model_port(&r->bme680));
        const bme68x_source_config_t cfg = {
            .bus = adp_i2c_fake_port(&r->bus),
            .clock = clock,
            .log = log,
            .address = BOARD_BME68X_I2C_ADDRESS,
            .id = SIM_ENV_SOURCE_ID,
        };
        (void)bme68x_source_setup(&r->bme68x, &cfg);
        r->sources[r->count++] = &r->bme68x.port;
    }
    scenario_world_attach(&r->world, (args->scenario_co2 != NULL) ? &r->scd41 : NULL,
                          (args->scenario_env != NULL) ? &r->bme680 : NULL);
    return true;
}

// The priorities of architecture 6.3: temperature and humidity from the
// BME68x, the SCD41 as fallback; CO2 from the SCD41 alone; pressure and gas
// from the BME68x alone. Same as the firmware's.
static void set_priorities(sensor_manager_t *m)
{
    static const uint8_t prefer_env[] = {SIM_ENV_SOURCE_ID, SIM_CO2_SOURCE_ID};
    static const uint8_t only_env[] = {SIM_ENV_SOURCE_ID};
    static const uint8_t only_co2[] = {SIM_CO2_SOURCE_ID};
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_TEMPERATURE, prefer_env, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_HUMIDITY, prefer_env, 2u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_PRESSURE, only_env, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_CO2, only_co2, 1u);
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_GAS_RESISTANCE, only_env, 1u);
}

// --- the loop --------------------------------------------------------------

// What one step of the loop needs, and nothing else: the loop keeps its own
// local state, so this does not become a second, shadow composition root.
typedef struct {
    port_display_t *display;
    const port_clock_t *clock;
    sensor_manager_t sensors;
    update_policy_t policy;
    air_reading_t reading;
    bool deferral_logged;

    // A snapshot of what the panel was last successfully flushed with, so a
    // partial redraw can be shrunk to what actually changed instead of the
    // whole panel screen_home_render() unconditionally repaints every time.
    framebuffer_t previous_fb;
} sim_t;

static void log_decision(const update_decision_t *d, const update_policy_t *policy)
{
    const char *note = "";
    if (d->full_forced_by_budget) {
        note = "  (partial budget spent: full refresh required)";
    } else if (d->partial_past_budget) {
        note = "  (past the budget: the full refresh waits for the panel's minimum interval)";
    }
    printf("[policy] %-8s reason=%-12s partials=%u%s\n", update_policy_action_name(d->action),
           update_policy_reason_name(d->reason), update_policy_partials_since_full(policy), note);
}

static void redraw(sim_t *sim, const update_decision_t *d)
{
    framebuffer_t *fb = sim->display->get_framebuffer(sim->display);
    if (fb == NULL) {
        return;
    }

    view_model_t vm;
    view_model_build(&vm, &sim->reading, sim->clock->now_ms(sim->clock));
    screen_home_render(fb, &vm);

    const bool full = (d->action == UPDATE_FULL);
    const refresh_mode_t mode = full ? REFRESH_FULL : REFRESH_PARTIAL;

    // NULL still means "whole panel" (the port contract), and is still the
    // right thing for a full refresh: there is no previous image a partial
    // diff could even be measured against on the first frame, and a forced
    // full repaints everything regardless.
    rect_t dirty_rect;
    const rect_t *area = NULL;
    if (!full) {
        if (!framebuffer_diff_dirty_rect(fb, &sim->previous_fb, &dirty_rect)) {
            // See apps/firmware_esp32/main/event_loop.c's redraw() for why
            // this skips update_policy_commit() too, not just the flush.
            printf("[policy] partial refresh skipped: framebuffer diff was empty (no visible "
                   "change)\n");
            return;
        }
        area = &dirty_rect;
    }

    if (sim->display->flush(sim->display, area, mode) == HAL_OK) {
        update_policy_commit(&sim->policy, &sim->reading, d);
        memcpy(sim->previous_fb.pixels, fb->pixels, fb->pixels_size);
    }
}

// One pass: whatever the sensors have due, then whatever the panel has due.
// Returns when the loop should next look -- the earlier of the sensor
// manager's deadline and the policy's.
static uint64_t step(sim_t *sim)
{
    const uint64_t sensors_due = sensor_manager_poll(&sim->sensors, &sim->reading);

    // The time after the poll, not before it: a poll takes time -- the
    // drivers wait out their sensors' command times on this clock -- and a
    // reading stamped later than the "now" it is aged against has no age at
    // all, which measurement_age_ms() rightly calls stale.
    const uint64_t now_ms = sim->clock->now_ms(sim->clock);
    sim->reading.uptime_s = (uint32_t)(now_ms / 1000u);
    air_reading_apply_age(&sim->reading, now_ms, sim->sensors.cfg.max_age_ms);

    update_decision_t decision;
    update_policy_evaluate(&sim->policy, &sim->reading, &decision);

    if (decision.action != UPDATE_NONE) {
        log_decision(&decision, &sim->policy);
        redraw(sim, &decision);
        sim->deferral_logged = false;
        // What is due after this redraw: the deadline above was "now".
        update_policy_evaluate(&sim->policy, &sim->reading, &decision);
    } else if (decision.deferred_by_min_interval && !sim->deferral_logged) {
        // Only a panel with no partial mode gets here (ADR 0019). Said once
        // per episode: repeating it every tick would bury the flush log it is
        // meant to explain.
        printf("[policy] deferred: a full refresh is due (%s) but the panel's minimum "
               "full-refresh interval has not elapsed; waiting until t=%llu ms\n",
               update_policy_reason_name(decision.reason),
               (unsigned long long)decision.next_deadline_ms);
        sim->deferral_logged = true;
    }

    uint64_t next =
        (sensors_due < decision.next_deadline_ms) ? sensors_due : decision.next_deadline_ms;
    if (next <= now_ms) {
        next = now_ms + SIM_REDRAW_RETRY_MS;  // a redraw that did not reach the panel
    }
    return next;
}

int main(int argc, char **argv)
{
    sim_args_t args;
    if (!parse_args(argc, argv, &args)) {
        return 2;
    }

    if (args.scenario_co2 == NULL && args.scenario_env == NULL) {
        fprintf(stderr, "nothing to replay: pass --scenario-co2 and/or --scenario-env\n\n");
        usage(argv[0]);
        return 2;
    }

    adp_clock_virtual_t vclock;
    adp_clock_virtual_init(&vclock, args.time_scale);
    const port_clock_t *clock = adp_clock_virtual_port(&vclock);

    adp_log_console_t console_log;
    adp_log_console_init(&console_log);
    const port_log_t *log = adp_log_console_port(&console_log);

    // --- sensors ---
    static sim_sensors_rig_t rig;
    const bool built = (args.sensors == SIM_SENSORS_FAKE_I2C)
                           ? build_fake_i2c(&rig, &args, &vclock, log)
                           : build_replay(&rig, &args, clock);
    if (!built) {
        return 1;
    }

    // --- display and input ---
    adp_display_png_t png_display;
    port_display_t *display = NULL;
    const epd_model_t *panel_model = NULL;
    port_input_t *input = NULL;

#ifdef HAC_SIM_GUI
    adp_display_sdl_t *sdl_display = NULL;
    adp_input_sdl_t sdl_input;
#endif

    if (args.headless) {
        const hal_status_t st = adp_display_png_init(&png_display, (uint16_t)SIM_PANEL_WIDTH,
                                                     (uint16_t)SIM_PANEL_HEIGHT, args.frames_dir);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot start the headless display (status %d)\n", (int)st);
            return 1;
        }
        display = adp_display_png_port(&png_display);
        panel_model = &png_display.model;
    } else {
#ifdef HAC_SIM_GUI
        sdl_display = adp_display_sdl_create((uint16_t)SIM_PANEL_WIDTH, (uint16_t)SIM_PANEL_HEIGHT,
                                             args.zoom, args.time_scale);
        if (sdl_display == NULL) {
            return 1;
        }
        display = adp_display_sdl_port(sdl_display);
        panel_model = adp_display_sdl_model(sdl_display);

        adp_input_sdl_init(&sdl_input, (uint16_t)SIM_PANEL_WIDTH, (uint16_t)SIM_PANEL_HEIGHT,
                           args.zoom, clock);
        input = adp_input_sdl_port(&sdl_input);
#else
        fprintf(stderr,
                "this build has no window: it was configured with SIM_GUI=OFF, or SDL2 was not\n"
                "found. Re-run with --headless (and --frames <dir> to save the screens).\n");
        return 1;
#endif
    }

    // --- diff buffer ---
    //
    // A snapshot of what the panel was last successfully flushed with, so
    // redraw() can shrink a partial refresh to what actually changed instead
    // of the whole panel screen_home_render() unconditionally repaints.
    const size_t previous_fb_size =
        framebuffer_required_size(display->width, display->height, display->format);
    uint8_t *previous_fb_pixels = malloc(previous_fb_size);
    if (previous_fb_pixels == NULL) {
        fprintf(stderr, "cannot allocate the %zu-byte diff buffer\n", previous_fb_size);
        return 1;
    }

    // --- logic ---
    static sim_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.display = display;
    sim.clock = clock;
    framebuffer_init(&sim.previous_fb, previous_fb_pixels, previous_fb_size, display->width,
                     display->height, display->format);

    const sensor_manager_config_t sensors_cfg = sensor_manager_config_default();
    sensor_manager_init(&sim.sensors, clock, log, &sensors_cfg);
    for (size_t i = 0u; i < rig.count; ++i) {
        (void)sensor_manager_add(&sim.sensors, rig.sources[i], SENSOR_MANAGER_DEFAULT_INTERVAL_MS);
    }
    set_priorities(&sim.sensors);

    const uint64_t stop_at_ms = (args.duration_s == 0u) ? 0u : (uint64_t)args.duration_s * 1000u;

    const update_policy_config_t cfg = update_policy_config_default();
    const update_policy_limits_t limits = update_policy_limits_from_display(display);
    update_policy_init(&sim.policy, clock, &cfg, &limits);

    printf("[sim] panel %ux%u, %u grey levels; policy limits: min full refresh %u ms, "
           "%u partial refreshes before a full one\n",
           display->width, display->height, 16u, limits.min_full_refresh_interval_ms,
           limits.max_partial_refreshes_before_full);
    printf("[sim] time scale x%.4g, %s, sensors: %s\n", args.time_scale,
           args.headless ? "headless" : "windowed",
           (args.sensors == SIM_SENSORS_FAKE_I2C) ? "fake-i2c (drivers over device models)"
                                                  : "replay");

    air_reading_init(&sim.reading);

    // --- the wait loop ---
    //
    // Windowed, virtual time follows real time through the configured scale
    // and the loop looks every SIM_TICK_MS; headless, there is nothing to
    // watch, so the clock jumps straight to the next deadline.
    uint64_t last_real_ms = host_monotonic_ms();
    uint64_t next_wake_ms = 0u;
    bool running = true;

    while (running) {
        if (args.headless) {
            const uint64_t now = clock->now_ms(clock);
            uint64_t target = next_wake_ms;
            if (target == SENSOR_MANAGER_NO_DEADLINE) {
                target = now + SIM_HEADLESS_STEP_MS;
            }
            if (stop_at_ms != 0u && target > stop_at_ms) {
                target = stop_at_ms;
            }
            if (target > now) {
                adp_clock_virtual_advance(&vclock, target - now);
            }
        } else {
            const uint64_t now_real = host_monotonic_ms();
            const uint64_t elapsed_real = (now_real >= last_real_ms) ? now_real - last_real_ms : 0u;
            last_real_ms = now_real;
            adp_clock_virtual_advance(&vclock, adp_clock_virtual_scale_ms(&vclock, elapsed_real));
        }

        const uint64_t now_ms = clock->now_ms(clock);
        if (now_ms >= next_wake_ms) {
            next_wake_ms = step(&sim);
        }

        if (input != NULL) {
            input_event_t ev;
            while (input->poll(input, &ev)) {
                // Nothing consumes touch yet: the menu is a later phase. The
                // events are logged so that the adapter can be seen to work.
                printf("[input] kind=%d at %u,%u t=%llu ms\n", (int)ev.kind, ev.x, ev.y,
                       (unsigned long long)ev.ts_ms);
            }
        }

#ifdef HAC_SIM_GUI
        if (sdl_display != NULL) {
            adp_display_sdl_present(sdl_display);
            if (adp_input_sdl_quit_requested(&sdl_input)) {
                running = false;
            }
        }
#endif

        if (stop_at_ms != 0u && now_ms >= stop_at_ms) {
            running = false;
        }

        if (!args.headless) {
            host_sleep_ms(SIM_TICK_MS);
        }
    }

    if (panel_model != NULL) {
        epd_model_log_summary(panel_model);
    }

#ifdef HAC_SIM_GUI
    if (sdl_display != NULL) {
        adp_display_sdl_destroy(sdl_display);
    }
#endif
    if (args.headless) {
        adp_display_png_deinit(&png_display);
    }
    for (size_t i = 0u; i < rig.count; ++i) {
        rig.sources[i]->deinit(rig.sources[i]);
    }
    if (rig.world_loaded) {
        scenario_world_dispose(&rig.world);
    }
    free(previous_fb_pixels);

    return 0;
}
