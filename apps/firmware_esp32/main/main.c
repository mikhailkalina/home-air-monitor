// SPDX-License-Identifier: Apache-2.0
//
// Composition root for the ESP32-S3 firmware.
//
// Phase 0 only proves that the platform-independent core links and runs on the
// target. Phase 2 replaces the body of app_main() with the real wiring from
// docs/architecture.md 6.2: adapters are constructed here, handed to
// app_core_create(), and the event loop takes over.

#include "esp_log.h"

#include "domain/measurement.h"

static const char *TAG = "hac";

#define DEMO_MAX_AGE_MS 60000u
#define DEMO_TAKEN_AT_MS 1000u
#define DEMO_NOW_MS 120000u

void app_main(void)
{
    air_reading_t reading;
    air_reading_init(&reading);

    reading.co2_ppm = measurement_ok(738.0f, DEMO_TAKEN_AT_MS, 1u);
    air_reading_apply_age(&reading, DEMO_NOW_MS, DEMO_MAX_AGE_MS);

    ESP_LOGI(TAG, "core linked: co2=%.0f ppm status=%d pressure status=%d",
             (double)reading.co2_ppm.value, (int)reading.co2_ppm.status,
             (int)reading.pressure_hpa.status);
    ESP_LOGI(TAG, "phase 0: no adapters wired yet, see docs/architecture.md section 10");
}
