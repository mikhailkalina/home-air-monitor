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
//
// Not yet wired, and deliberately: the control panel, the remaining screens,
// deep-sleep emulation, networking and telemetry. app_core and sensor_manager
// do not exist yet either, so the loop below reads the sources and merges
// their samples itself; that merge moves into core/app/sensor_manager.c in
// phase 3, together with the source priorities of architecture 6.3.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_display_png.h"
#include "app/update_policy.h"
#include "domain/measurement.h"
#include "epd_model.h"
#include "host_time.h"
#include "port_display.h"
#include "port_input.h"
#include "replay/replay_source.h"
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

// How often the loop reads the sources. Requirements 11.2, USB mode: a sensor
// read every 5 to 30 seconds.
#define SIM_SENSOR_INTERVAL_MS 5000u

// A reading older than this is shown as stale (requirements 9.2). Six sample
// intervals: long enough that one missed read is not an alarm.
#define SIM_MAX_READING_AGE_MS 30000u

// Windowed: how long one loop iteration waits before looking again. Small
// enough that the window stays responsive, large enough not to spin a core.
#define SIM_TICK_MS 20u

// Headless: virtual time per iteration. There is nothing to wait for, so the
// run goes as fast as the machine allows.
#define SIM_HEADLESS_STEP_MS 1000u

// Headless runs must terminate on their own, or CI hangs.
#define SIM_DEFAULT_HEADLESS_DURATION_S 600u

typedef struct {
    const char *scenario_co2;
    const char *scenario_env;
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
           "  --scenario-co2 <file>  CSV replayed as the CO2 source (SCD41 stand-in)\n"
           "  --scenario-env <file>  CSV replayed as the environment source (BME68x stand-in)\n"
           "  --time-scale <n>       virtual seconds per real second (default 1)\n"
           "  --headless             no window: write frames instead (implies a duration)\n"
           "  --frames <dir>         directory for the headless PNG frames\n"
           "  --zoom <f>             window scale, 1 = one panel pixel per screen pixel\n"
           "  --duration <s>         stop after this many virtual seconds (0 = never)\n"
           "  --help\n"
           "\n"
           "Panel: %ux%u, 16 grey levels. Every e-paper timing the simulator shows is an\n"
           "unmeasured estimate; see platform/host/epd_emulation.h.\n",
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

// Copies the quantities a source actually claims. Anything else it happens to
// have filled in is ignored: a source that does not advertise CO2 must not be
// able to put a number in the CO2 field, however plausible.
//
// This is the placeholder for core/app/sensor_manager.c (architecture 6.3),
// which adds the source priorities and the fallback behaviour of
// requirements 4.3. Until then, one source per quantity, last write wins.
static void merge_sample(air_reading_t *reading, const sensor_source_t *src,
                         const sensor_sample_t *sample)
{
    if ((src->caps & SENS_CAP_TEMPERATURE) != 0u) {
        reading->temperature_c = sample->temperature_c;
    }
    if ((src->caps & SENS_CAP_HUMIDITY) != 0u) {
        reading->humidity_rh = sample->humidity_rh;
    }
    if ((src->caps & SENS_CAP_PRESSURE) != 0u) {
        reading->pressure_hpa = sample->pressure_hpa;
    }
    if ((src->caps & SENS_CAP_CO2) != 0u) {
        reading->co2_ppm = sample->co2_ppm;
    }
    if ((src->caps & SENS_CAP_IAQ) != 0u) {
        reading->iaq = sample->iaq;
    }
    if ((src->caps & SENS_CAP_VOC) != 0u) {
        reading->voc_index = sample->voc_index;
    }
    if ((src->caps & SENS_CAP_GAS_RES) != 0u) {
        reading->gas_resistance_ohm = sample->gas_resistance_ohm;
    }
}

// A source that fails to read leaves its quantities marked, not stale-looking:
// requirements 15.1 wants the failure on the screen, not a plausible old
// number pretending to be current.
static void mark_source_failed(air_reading_t *reading, const sensor_source_t *src, uint64_t now_ms)
{
    sensor_sample_t failed;
    memset(&failed, 0, sizeof(failed));

    const measurement_f32_t err = measurement_status(VAL_ERROR, now_ms, src->id);
    failed.temperature_c = err;
    failed.humidity_rh = err;
    failed.pressure_hpa = err;
    failed.co2_ppm = err;
    failed.iaq = err;
    failed.voc_index = err;
    failed.gas_resistance_ohm = err;

    merge_sample(reading, src, &failed);
}

static void sample_sources(air_reading_t *reading, sensor_source_t **sources, size_t count,
                           uint64_t now_ms)
{
    for (size_t i = 0u; i < count; ++i) {
        sensor_source_t *src = sources[i];
        sensor_sample_t sample;

        memset(&sample, 0, sizeof(sample));
        if (src->read(src, &sample) == HAL_OK) {
            merge_sample(reading, src, &sample);
        } else {
            mark_source_failed(reading, src, now_ms);
        }
    }
}

// --- the loop --------------------------------------------------------------

// What drawing a frame needs, and nothing else: the loop keeps its own local
// state, so this does not become a second, shadow composition root.
typedef struct {
    port_display_t *display;
    const port_clock_t *clock;
    update_policy_t policy;
    air_reading_t reading;
} sim_t;

static void log_decision(const update_decision_t *d, const update_policy_t *policy)
{
    printf("[policy] %-8s reason=%-12s partials=%u%s\n", update_policy_action_name(d->action),
           update_policy_reason_name(d->reason), update_policy_partials_since_full(policy),
           d->full_forced_by_budget ? "  (partial budget spent: full refresh required)" : "");
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

    const refresh_mode_t mode = (d->action == UPDATE_FULL) ? REFRESH_FULL : REFRESH_PARTIAL;
    if (sim->display->flush(sim->display, NULL, mode) == HAL_OK) {
        update_policy_commit(&sim->policy, &sim->reading, d);
    }
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

    // --- sensors ---
    replay_source_t co2_source;
    replay_source_t env_source;
    sensor_source_t *sources[2];
    size_t source_count = 0u;

    if (args.scenario_co2 != NULL) {
        const hal_status_t st =
            replay_source_init(&co2_source, args.scenario_co2, SENS_CAP_CO2, 1u, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args.scenario_co2, (int)st);
            return 1;
        }
        sources[source_count++] = &co2_source.port;
    }
    if (args.scenario_env != NULL) {
        const sensor_caps_t env_caps = (sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                                       SENS_CAP_PRESSURE | SENS_CAP_GAS_RES);
        const hal_status_t st =
            replay_source_init(&env_source, args.scenario_env, env_caps, 2u, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args.scenario_env, (int)st);
            return 1;
        }
        sources[source_count++] = &env_source.port;
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

    // --- logic ---
    sim_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.display = display;
    sim.clock = clock;

    const uint64_t stop_at_ms = (args.duration_s == 0u) ? 0u : (uint64_t)args.duration_s * 1000u;

    const update_policy_config_t cfg = update_policy_config_default();
    const update_policy_limits_t limits = update_policy_limits_from_display(display);
    update_policy_init(&sim.policy, clock, &cfg, &limits);

    printf("[sim] panel %ux%u, %u grey levels; policy limits: min full refresh %u ms, "
           "%u partial refreshes before a full one\n",
           display->width, display->height, 16u, limits.min_full_refresh_interval_ms,
           limits.max_partial_refreshes_before_full);
    printf("[sim] time scale x%.4g, %s\n", args.time_scale,
           args.headless ? "headless" : "windowed");

    air_reading_init(&sim.reading);

    for (size_t i = 0u; i < source_count; ++i) {
        (void)sources[i]->init(sources[i]);
        (void)sources[i]->start(sources[i]);
    }

    // --- the wait loop ---
    //
    // Windowed, virtual time follows real time through the configured scale;
    // headless, it simply jumps, because there is nothing to watch.
    uint64_t last_real_ms = host_monotonic_ms();
    uint64_t next_sample_ms = 0u;
    bool deferral_logged = false;
    bool running = true;

    while (running) {
        if (args.headless) {
            adp_clock_virtual_advance(&vclock, SIM_HEADLESS_STEP_MS);
        } else {
            const uint64_t now_real = host_monotonic_ms();
            const uint64_t elapsed_real = (now_real >= last_real_ms) ? now_real - last_real_ms : 0u;
            last_real_ms = now_real;
            adp_clock_virtual_advance(&vclock, adp_clock_virtual_scale_ms(&vclock, elapsed_real));
        }

        const uint64_t now_ms = clock->now_ms(clock);

        if (now_ms >= next_sample_ms) {
            sample_sources(&sim.reading, sources, source_count, now_ms);
            next_sample_ms = now_ms + SIM_SENSOR_INTERVAL_MS;
        }
        sim.reading.uptime_s = (uint32_t)(now_ms / 1000u);
        air_reading_apply_age(&sim.reading, now_ms, SIM_MAX_READING_AGE_MS);

        update_decision_t decision;
        update_policy_evaluate(&sim.policy, &sim.reading, &decision);

        if (decision.action != UPDATE_NONE) {
            log_decision(&decision, &sim.policy);
            redraw(&sim, &decision);
            deferral_logged = false;
        } else if (decision.deferred_by_min_interval && !deferral_logged) {
            // Said once per episode: repeating it every tick would bury the
            // flush log it is meant to explain.
            printf("[policy] deferred: a full refresh is due (%s) but the panel's minimum "
                   "full-refresh interval has not elapsed; waiting until t=%llu ms\n",
                   update_policy_reason_name(decision.reason),
                   (unsigned long long)decision.next_deadline_ms);
            deferral_logged = true;
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
    for (size_t i = 0u; i < source_count; ++i) {
        sources[i]->deinit(sources[i]);
    }

    return 0;
}
