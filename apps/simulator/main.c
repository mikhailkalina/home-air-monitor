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
//   simulator --wall-time 2026-10-25T00:58:00Z ...    (or --no-wall-time)
//   simulator --record out/rec --scenario-co2 ... --scenario-env ...
//   simulator --recording card/data/2026-10-16_scd4x.csv
//             --recording card/data/2026-10-16_bme68x.csv
//   simulator --headless --taps "30:480,270;45:480,270" ...
//
// Two ways to the same sensor_source_t (architecture 5.4): replay sources
// that play the scenario files straight into the core, or the real drivers
// -- the SCD4x and BME68x code the board runs -- over the fake I2C bus,
// whose device models measure a room played from the same files
// (platform/host/scenario_world.h). Either way core/app/sensor_manager.c
// drives them, with the ids and priorities given below, as
// apps/firmware_esp32/main/main.c gives them on the board.
//
// The wall clock: the PC's time of day when the run starts, or the one
// --wall-time pins, moving on with virtual time at --time-scale -- or none at
// all with --no-wall-time, which is the board with its RTC not yet set. The
// footer shows it in the board's zone, Vilnius
// (docs/adr/0023-wall-clock-and-the-time-of-the-last-update.md). Replaying
// recordings, it starts at the earliest of their first rows instead, unless
// one of those two options says otherwise.
//
// Recordings (docs/adr/0024-sd-card-recordings-and-debug-log.md): --record
// writes each source's rows into <dir>/data/ through the host's port_files,
// as the board writes them to its card, with the board's file names; and
// --recording replays such a file -- the board's, or the simulator's own --
// as the source that recorded it: the capabilities its header shows, the id
// and the priority that source has on the board, which the file name's tag
// says (_scd4x, _bme68x). So a day recorded on the board plays through the
// same sensor_manager merge.
//
// Touch (docs/adr/0025-touch-input-gt911.md): the window's clicks, and with
// --taps a script of taps for headless runs, go through port_input to
// core/app/screen_controller.c, as the board's GT911 does -- a tap switches
// between the home screen and the diagnostics screen, and the loop prints
// how long the change took to reach the glass on the panel model.
//
// Not yet wired, and deliberately: the control panel, deep-sleep emulation,
// networking and telemetry. There is no app_core yet either: the loop below
// runs the sensor manager, the screen controller and the update policy
// itself, and sleeps until the earliest of their deadlines.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adp_clock_virtual.h"
#include "adp_display_png.h"
#include "adp_files_dir.h"
#include "adp_i2c_fake.h"
#include "adp_input_script.h"
#include "adp_log_console.h"
#include "app/screen_controller.h"
#include "app/sensor_manager.h"
#include "app/update_policy.h"
#include "bme68x_limits.h"
#include "bme68x_source.h"
#include "board_config.h"  // plain data: where the board's sensors answer
#include "domain/civil_time.h"
#include "domain/measurement.h"
#include "epd_model.h"
#include "host_time.h"
#include "i2c_models/bme680_model.h"
#include "i2c_models/scd41_model.h"
#include "port_display.h"
#include "port_input.h"
#include "port_log.h"
#include "recording/record_files.h"
#include "recording/recorder.h"
#include "replay/replay_source.h"
#include "scd4x_source.h"
#include "scenario_world.h"
#include "ui/framebuffer.h"

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

// The tags the two sources' recordings carry in their file names, the
// firmware's: what --record writes and what --recording reads a file's
// source from.
#define SIM_CO2_RECORDING_TAG "scd4x"
#define SIM_ENV_RECORDING_TAG "bme68x"

// --recording may be given once per source the manager can hold.
#define SIM_MAX_RECORDINGS SENSOR_MANAGER_MAX_SOURCES

// How many recorder events one poll can produce: one per source.
#define SIM_EVENTS_PER_POLL SENSOR_MANAGER_MAX_SOURCES

// The zone the footer's time is shown in: the board's, Vilnius -- EET, UTC+2,
// with the EU's summer time -- as apps/firmware_esp32/main/main.c has it.
#define SIM_LOCAL_STANDARD_OFFSET_MIN 120

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

// Room for what --taps can say is wrong.
#define SIM_TAPS_ERROR_MAX 160u

// port_input implementations at once: the window's and the script's.
#define SIM_MAX_INPUTS 2u

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
    bool no_wall_time;
    bool wall_time_pinned;
    uint64_t wall_time_ms;  // the pinned start, when wall_time_pinned
    const char *record_dir;
    const char *recordings[SIM_MAX_RECORDINGS];
    size_t recording_count;
    const char *taps;  // --taps, unparsed
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
           "  --wall-time <utc>      start the wall clock here, YYYY-MM-DDTHH:MM:SSZ, instead\n"
           "                         of at this PC's time; it moves on with --time-scale\n"
           "  --no-wall-time         no wall clock: the board before its RTC is set\n"
           "  --record <dir>         write each source's rows into <dir>/data/, as the board\n"
           "                         writes them to its card\n"
           "  --recording <file>     replay a recording (the board's or --record's) as the\n"
           "                         source its name says: <day>_scd4x.csv, <day>_bme68x.csv;\n"
           "                         repeatable, one per source. The wall clock starts at\n"
           "                         the earliest first row unless --wall-time says otherwise\n"
           "  --taps <list>          scripted taps, \"<seconds>:<x>,<y>;...\" in panel pixels: a\n"
           "                         tap switches between the home and diagnostics screens\n"
           "                         (headless or windowed; the window's clicks do the same)\n"
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
        } else if (strcmp(arg, "--wall-time") == 0) {
            TAKE_VALUE("--wall-time");
            const char *text = argv[++i];
            civil_time_t t;
            if (!civil_time_parse_iso8601_utc(text, &t) ||
                !civil_time_to_unix_ms(&t, &out->wall_time_ms)) {
                fprintf(stderr, "--wall-time must be UTC as YYYY-MM-DDTHH:MM:SSZ, not %s\n", text);
                return false;
            }
            out->wall_time_pinned = true;
        } else if (strcmp(arg, "--no-wall-time") == 0) {
            out->no_wall_time = true;
        } else if (strcmp(arg, "--record") == 0) {
            TAKE_VALUE("--record");
            out->record_dir = argv[++i];
        } else if (strcmp(arg, "--recording") == 0) {
            TAKE_VALUE("--recording");
            if (out->recording_count >= SIM_MAX_RECORDINGS) {
                fprintf(stderr, "--recording may be given at most %u times\n",
                        (unsigned)SIM_MAX_RECORDINGS);
                return false;
            }
            out->recordings[out->recording_count++] = argv[++i];
        } else if (strcmp(arg, "--taps") == 0) {
            TAKE_VALUE("--taps");
            out->taps = argv[++i];
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
    if (out->no_wall_time && out->wall_time_pinned) {
        fprintf(stderr, "--wall-time and --no-wall-time contradict each other\n");
        return false;
    }
    if (out->recording_count > 0u && out->sensors == SIM_SENSORS_FAKE_I2C) {
        fprintf(stderr, "a recording replays into the core directly: --recording needs "
                        "--sensors replay\n");
        return false;
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
    replay_source_t recordings[SIM_MAX_RECORDINGS];
    size_t recording_count;
    uint64_t earliest_utc_ms;  // the earliest first row among the recordings; 0 for none

    // fake-i2c mode
    adp_i2c_fake_t bus;
    scd41_model_t scd41;
    bme680_model_t bme680;
    scenario_world_t world;
    bool world_loaded;
    scd4x_source_t scd4x;
    bme68x_source_t bme68x;

    sensor_source_t *sources[SENSOR_MANAGER_MAX_SOURCES];
    sensor_caps_t accuracy_caps[SENSOR_MANAGER_MAX_SOURCES];  // what each source's file rates
    size_t count;
} sim_sensors_rig_t;

static const sensor_caps_t CO2_CAPS =
    (sensor_caps_t)(SENS_CAP_CO2 | SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY);
static const sensor_caps_t ENV_CAPS = (sensor_caps_t)(SENS_CAP_TEMPERATURE | SENS_CAP_HUMIDITY |
                                                      SENS_CAP_PRESSURE | SENS_CAP_GAS_RES);

static bool id_taken(const sim_sensors_rig_t *r, uint8_t id)
{
    for (size_t i = 0u; i < r->count; ++i) {
        if (r->sources[i]->id == id) {
            return true;
        }
    }
    return false;
}

static void add_source(sim_sensors_rig_t *r, replay_source_t *rs)
{
    r->accuracy_caps[r->count] = replay_timeline_accuracy_caps(&rs->timeline);
    r->sources[r->count++] = &rs->port;
}

// The source a recording came from, by the tag in its name -- the board's
// names (core/recording/record_files.h): <key>_<tag>[_<n>].csv.
static bool recording_source_id(const char *path, uint8_t *id)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    record_name_t name;
    if (!record_name_parse(base, &name)) {
        return false;
    }
    if (strcmp(name.tag, SIM_CO2_RECORDING_TAG) == 0) {
        *id = SIM_CO2_SOURCE_ID;
        return true;
    }
    if (strcmp(name.tag, SIM_ENV_RECORDING_TAG) == 0) {
        *id = SIM_ENV_SOURCE_ID;
        return true;
    }
    return false;
}

// A recording replays as the source that wrote it, claiming what its header
// shows. Interpolated like a scenario, between rows a few seconds apart.
static bool add_recording(sim_sensors_rig_t *r, const char *path, const port_clock_t *clock)
{
    uint8_t id = 0u;
    if (!recording_source_id(path, &id)) {
        fprintf(stderr,
                "%s: cannot tell which sensor recorded it -- a recording's name ends in _%s.csv "
                "or _%s.csv (optionally _<n> before .csv), as the board writes it\n",
                path, SIM_CO2_RECORDING_TAG, SIM_ENV_RECORDING_TAG);
        return false;
    }
    if (id_taken(r, id)) {
        fprintf(stderr, "%s: another file already plays that sensor\n", path);
        return false;
    }
    replay_source_t *rs = &r->recordings[r->recording_count];
    const hal_status_t st = replay_source_init(rs, path, (sensor_caps_t)0, id, clock, true);
    if (st != HAL_OK) {
        fprintf(stderr, "cannot load %s (status %d)\n", path, (int)st);
        return false;
    }
    rs->port.caps = replay_timeline_caps(&rs->timeline);
    r->recording_count++;
    add_source(r, rs);

    const uint64_t first = rs->timeline.first_utc_ms;
    if (first != 0u && (r->earliest_utc_ms == 0u || first < r->earliest_utc_ms)) {
        r->earliest_utc_ms = first;
    }
    return true;
}

// Replay: the CO2 file stands in for the SCD41, with the SCD41's three
// quantities, so the fallback of architecture 6.3 has something to fall back
// on; a column the file lacks is simply VAL_UNAVAILABLE. Recordings come
// first, so a scenario file cannot take a sensor a recording plays.
static bool build_replay(sim_sensors_rig_t *r, const sim_args_t *args, const port_clock_t *clock)
{
    for (size_t i = 0u; i < args->recording_count; ++i) {
        if (!add_recording(r, args->recordings[i], clock)) {
            return false;
        }
    }
    if (args->scenario_co2 != NULL) {
        if (id_taken(r, SIM_CO2_SOURCE_ID)) {
            fprintf(stderr, "--scenario-co2 and a _%s recording both play the SCD41\n",
                    SIM_CO2_RECORDING_TAG);
            return false;
        }
        const hal_status_t st = replay_source_init(&r->co2_replay, args->scenario_co2, CO2_CAPS,
                                                   SIM_CO2_SOURCE_ID, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args->scenario_co2, (int)st);
            return false;
        }
        add_source(r, &r->co2_replay);
    }
    if (args->scenario_env != NULL) {
        if (id_taken(r, SIM_ENV_SOURCE_ID)) {
            fprintf(stderr, "--scenario-env and a _%s recording both play the BME68x\n",
                    SIM_ENV_RECORDING_TAG);
            return false;
        }
        const hal_status_t st = replay_source_init(&r->env_replay, args->scenario_env, ENV_CAPS,
                                                   SIM_ENV_SOURCE_ID, clock, true);
        if (st != HAL_OK) {
            fprintf(stderr, "cannot load %s (status %d)\n", args->scenario_env, (int)st);
            return false;
        }
        add_source(r, &r->env_replay);
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
// BME68x, the SCD41 as fallback; CO2 from the SCD41 alone; pressure, gas and
// IAQ from the BME68x alone. Same as the firmware's.
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
    (void)sensor_manager_set_priority(m, SENSOR_FIELD_IAQ, only_env, 1u);
}

// --- recording --------------------------------------------------------------

// --record: the board's recorder over a directory, the same tags, so the
// files are named and laid out as on the card.
typedef struct {
    bool on;
    adp_files_dir_t dir;
    recorder_t rec;
    recorder_watch_t watch;
} sim_recording_t;

static bool start_recording(sim_recording_t *rec, const char *dir, const sim_sensors_rig_t *rig,
                            const time_zone_t *zone, const port_log_t *log)
{
    if (adp_files_dir_init(&rec->dir, dir) != HAL_OK) {
        fprintf(stderr, "--record: cannot use %s as the card (is its parent there?)\n", dir);
        return false;
    }
    const recorder_config_t cfg = recorder_config_default();
    recorder_init(&rec->rec, adp_files_dir_port(&rec->dir), log, zone, &cfg);
    for (size_t i = 0u; i < rig->count; ++i) {
        const uint8_t id = rig->sources[i]->id;
        const char *tag = (id == SIM_CO2_SOURCE_ID) ? SIM_CO2_RECORDING_TAG : SIM_ENV_RECORDING_TAG;
        (void)recorder_add_source(&rec->rec, id, tag, rig->accuracy_caps[i]);
    }
    recorder_watch_init(&rec->watch);
    rec->on = true;
    return true;
}

// --- the loop --------------------------------------------------------------

// What one step of the loop needs, and nothing else: the loop keeps its own
// local state, so this does not become a second, shadow composition root.
typedef struct {
    port_display_t *display;
    const port_clock_t *clock;
    time_zone_t zone;
    sensor_manager_t sensors;
    update_policy_t policy;
    screen_controller_t screens;
    air_reading_t reading;
    bool deferral_logged;
    sim_recording_t *recording;  // NULL without --record
    const char *clock_source;    // the diagnostics screen's word for the wall clock
    const epd_model_t *panel;    // how long each flush takes on the glass

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

// What the screens draw from, at `now_ms` -- the time the reading's wall
// time was read at.
static screen_inputs_t screen_inputs(const sim_t *sim, uint64_t now_ms)
{
    screen_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.reading = &sim->reading;
    in.now_ms = now_ms;
    in.zone = &sim->zone;
    in.sensors = &sim->sensors;
    in.recorder.present = sim->recording != NULL;
    if (sim->recording != NULL) {
        in.recorder.state = sim->recording->rec.state;
    }
    in.clock_source = sim->clock_source;
    return in;
}

// The screen that changed under a tap, and how long it took: the time the
// event waited for this loop on the virtual clock, plus the flush on the
// panel model -- the window waits that out in real time, after this.
static void log_screen_change(const screen_redraw_note_t *note, uint64_t flush_ms)
{
    if (!note->screen_changed) {
        return;
    }
    if (!note->by_tap) {
        printf("[screen] %s (the diagnostics screen's timeout)\n",
               screen_controller_screen_name(note->screen));
        return;
    }
    printf("[screen] %s (tap) latency_ms=%llu: %llu waiting for the loop + %llu on the glass\n",
           screen_controller_screen_name(note->screen),
           (unsigned long long)(note->latency_ms + flush_ms), (unsigned long long)note->latency_ms,
           (unsigned long long)flush_ms);
}

// `now_ms` is the time the reading's wall time was read at.
static void redraw(sim_t *sim, const update_decision_t *d, uint64_t now_ms)
{
    framebuffer_t *fb = sim->display->get_framebuffer(sim->display);
    if (fb == NULL) {
        return;
    }

    const screen_inputs_t in = screen_inputs(sim, now_ms);
    screen_controller_render(&sim->screens, fb, &in);

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

    const uint64_t panel_before = (sim->panel != NULL) ? sim->panel->simulated_ms_total : 0u;
    if (sim->display->flush(sim->display, area, mode) == HAL_OK) {
        update_policy_commit(&sim->policy, &sim->reading, d);
        memcpy(sim->previous_fb.pixels, fb->pixels, fb->pixels_size);
        screen_redraw_note_t note;
        screen_controller_committed(&sim->screens, sim->clock->now_ms(sim->clock), &note);
        const uint64_t panel_after = (sim->panel != NULL) ? sim->panel->simulated_ms_total : 0u;
        log_screen_change(&note, panel_after - panel_before);
    }
}

// One pass: whatever the sensors have due, then whatever the panel has due.
// Returns when the loop should next look -- the earliest of the sensor
// manager's deadline, the screen controller's and the policy's.
static uint64_t step(sim_t *sim)
{
    const uint64_t sensors_due = sensor_manager_poll(&sim->sensors, &sim->reading);

    // The time after the poll, not before it: a poll takes time -- the
    // drivers wait out their sensors' command times on this clock -- and a
    // reading stamped later than the "now" it is aged against has no age at
    // all, which measurement_age_ms() rightly calls stale.
    // The wall clock at the same point, so that a value's wall time is the
    // reading's minus its age (view_model_build()).
    const uint64_t now_ms = sim->clock->now_ms(sim->clock);
    sim->reading.wall_time_ms = sim->clock->wall_ms(sim->clock);
    sim->reading.uptime_s = (uint32_t)(now_ms / 1000u);
    air_reading_apply_age(&sim->reading, now_ms, sim->sensors.cfg.max_age_ms);

    // --record: what the board's card writer does with the events its event
    // loop posts, here in the same loop -- the simulator does not mind
    // waiting on a disk.
    if (sim->recording != NULL) {
        recorder_event_t events[SIM_EVENTS_PER_POLL];
        const size_t n = recorder_watch(&sim->recording->watch, &sim->sensors, now_ms,
                                        sim->reading.wall_time_ms, events, SIM_EVENTS_PER_POLL);
        for (size_t i = 0u; i < n; ++i) {
            recorder_record(&sim->recording->rec, &events[i]);
        }
        recorder_tick(&sim->recording->rec, now_ms);
    }

    const uint64_t screens_due = screen_controller_tick(&sim->screens, &sim->policy, now_ms);
    const screen_inputs_t in = screen_inputs(sim, now_ms);

    update_decision_t decision;
    screen_controller_evaluate(&sim->screens, &sim->policy, &in, &decision);

    if (decision.action != UPDATE_NONE) {
        log_decision(&decision, &sim->policy);
        redraw(sim, &decision, now_ms);
        sim->deferral_logged = false;
        // What is due after this redraw: the deadline above was "now".
        screen_controller_evaluate(&sim->screens, &sim->policy, &in, &decision);
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
    if (screens_due < next) {
        next = screens_due;
    }
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

    if (args.scenario_co2 == NULL && args.scenario_env == NULL && args.recording_count == 0u) {
        fprintf(stderr,
                "nothing to replay: pass --scenario-co2, --scenario-env or --recording\n\n");
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

    // The wall clock: pinned, none, a recording's own first row, or this PC's.
    const char *wall_source = "this PC's";
    const char *clock_word = NULL;  // the diagnostics screen's: "PC", "--wall-time", "recording"
    if (!args.no_wall_time) {
        uint64_t wall = host_wall_ms();
        clock_word = "PC";
        if (args.wall_time_pinned) {
            wall = args.wall_time_ms;
            wall_source = "--wall-time";
            clock_word = "--wall-time";
        } else if (rig.earliest_utc_ms != 0u) {
            wall = rig.earliest_utc_ms;
            wall_source = "the recording's first row";
            clock_word = "recording";
        }
        adp_clock_virtual_set_wall(&vclock, wall);
    }

    // --- display and input ---
    adp_display_png_t png_display;
    port_display_t *display = NULL;
    const epd_model_t *panel_model = NULL;
    port_input_t *inputs[SIM_MAX_INPUTS];
    size_t input_count = 0u;

    // --taps: a script of taps, in either mode.
    static adp_input_script_t script;
    adp_input_script_init(&script, clock);
    if (args.taps != NULL) {
        char err[SIM_TAPS_ERROR_MAX];
        if (adp_input_script_add_taps(&script, args.taps, (uint16_t)SIM_PANEL_WIDTH,
                                      (uint16_t)SIM_PANEL_HEIGHT, err, sizeof(err)) != HAL_OK) {
            fprintf(stderr, "--taps: %s\n", err);
            return 2;
        }
        inputs[input_count++] = adp_input_script_port(&script);
    }

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
        inputs[input_count++] = adp_input_sdl_port(&sdl_input);
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
    sim.zone = time_zone_eu(SIM_LOCAL_STANDARD_OFFSET_MIN);
    sim.clock_source = clock_word;
    sim.panel = panel_model;
    const screen_controller_config_t screens_cfg = screen_controller_config_default();
    screen_controller_init(&sim.screens, &screens_cfg);
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
    const uint64_t wall_ms = clock->wall_ms(clock);
    if (wall_ms == 0u) {
        printf("[sim] wall clock: none (--no-wall-time): the footer says \"updated --:--\"\n");
    } else {
        const civil_time_t utc = civil_time_from_unix_ms(wall_ms);
        const civil_time_t local = civil_time_local(wall_ms, &sim.zone);
        printf("[sim] wall clock: %04d-%02u-%02uT%02u:%02u:%02uZ (%s), %02u:%02u in Vilnius\n",
               (int)utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second, wall_source,
               local.hour, local.minute);
    }
    for (size_t i = 0u; i < rig.recording_count; ++i) {
        printf("[sim] recording %s plays source id %u\n", rig.recordings[i].name,
               (unsigned)rig.recordings[i].port.id);
    }

    static sim_recording_t recording;
    if (args.record_dir != NULL) {
        if (!start_recording(&recording, args.record_dir, &rig, &sim.zone, log)) {
            return 1;
        }
        sim.recording = &recording;
        printf("[sim] recording into %s/data/\n", args.record_dir);
    }

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
            const uint64_t tap_at = adp_input_script_next_ms(&script);
            if (tap_at < target) {
                target = tap_at;
            }
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

        // Touch first: a tap that changes the screen is a redraw due now.
        for (size_t i = 0u; i < input_count; ++i) {
            input_event_t ev;
            while (inputs[i]->poll(inputs[i], &ev)) {
                if (ev.kind != INPUT_TOUCH_MOVE) {
                    printf("[input] %s at %u,%u t=%llu ms\n",
                           (ev.kind == INPUT_TOUCH_DOWN) ? "down" : "up", ev.x, ev.y,
                           (unsigned long long)ev.ts_ms);
                }
                if (screen_controller_input(&sim.screens, &sim.policy, &ev)) {
                    printf("[screen] tap at %u,%u: %s next\n", ev.x, ev.y,
                           screen_controller_screen_name(screen_controller_screen(&sim.screens)));
                    next_wake_ms = now_ms;
                }
            }
        }

        if (now_ms >= next_wake_ms) {
            next_wake_ms = step(&sim);
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
    if (sim.recording != NULL) {
        recorder_close(&recording.rec, clock->now_ms(clock));
        printf("[sim] recorded %u rows, %llu bytes, %u dropped, into %s/data/\n",
               recorder_rows_written(&recording.rec),
               (unsigned long long)recorder_bytes_written(&recording.rec),
               recorder_rows_dropped(&recording.rec), args.record_dir);
        adp_files_dir_deinit(&recording.dir);
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
