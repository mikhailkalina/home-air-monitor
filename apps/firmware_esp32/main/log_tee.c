// SPDX-License-Identifier: Apache-2.0

#include "log_tee.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#define US_PER_MS 1000

// The slots hold a uint64_t: aligned for it.
#define LOG_TEE_ALIGN 8u

static log_ring_t s_ring;
static bool s_started;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_console;
static esp_log_level_t s_console_level;
static esp_log_level_t s_file_level;
static volatile bool s_hold;
static uint32_t s_held;  // under s_lock

// ESP-IDF's text format (log v1, colours off) starts a line with its level's
// letter: "I (1234) tag: ...". A format that does not is printed as INFO.
static esp_log_level_t level_of(const char *fmt, char *letter)
{
    if (fmt != NULL && fmt[0] != '\0' && fmt[1] == ' ' && fmt[2] == '(') {
        *letter = fmt[0];
        switch (fmt[0]) {
            case 'E':
                return ESP_LOG_ERROR;
            case 'W':
                return ESP_LOG_WARN;
            case 'I':
                return ESP_LOG_INFO;
            case 'D':
                return ESP_LOG_DEBUG;
            case 'V':
                return ESP_LOG_VERBOSE;
            default:
                break;
        }
    }
    *letter = '\0';
    return ESP_LOG_INFO;
}

// Runs in the task that logged. Never waits on anything but the spinlock
// around the reservation.
static int tee_vprintf(const char *fmt, va_list args)
{
    char letter = '\0';
    const esp_log_level_t level = level_of(fmt, &letter);

    if (level <= s_file_level && s_hold) {
        // Held: nothing in PSRAM, only a count in internal RAM.
        portENTER_CRITICAL_SAFE(&s_lock);
        s_held++;
        portEXIT_CRITICAL_SAFE(&s_lock);
    } else if (level <= s_file_level) {
        const uint64_t mono_ms = (uint64_t)(esp_timer_get_time() / US_PER_MS);
        portENTER_CRITICAL_SAFE(&s_lock);
        log_ring_slot_t *slot = log_ring_reserve(&s_ring, mono_ms);
        portEXIT_CRITICAL_SAFE(&s_lock);
        if (slot != NULL) {
            va_list copy;
            va_copy(copy, args);
            const int n = vsnprintf(slot->text, log_ring_text_cap(&s_ring) + 1u, fmt, copy);
            va_end(copy);
            log_ring_commit(&s_ring, slot, letter, (n > 0) ? (size_t)n : 0u);
        }
    }
    return (level <= s_console_level) ? s_console(fmt, args) : 0;
}

hal_status_t log_tee_start(esp_log_level_t file_level)
{
    if (s_started) {
        return HAL_OK;
    }
    const size_t bytes = log_ring_storage_bytes(LOG_TEE_SLOT_BYTES, LOG_TEE_SLOTS);
    void *storage = heap_caps_aligned_alloc(LOG_TEE_ALIGN, bytes, MALLOC_CAP_SPIRAM);
    if (storage == NULL) {
        return HAL_ERR_NO_MEM;
    }
    log_ring_init(&s_ring, storage, LOG_TEE_SLOT_BYTES, LOG_TEE_SLOTS);

    // The console filters nothing ESP-IDF passes, as before -- unless the
    // file wants more than the console's level: then the extra lines must
    // reach the hook to reach the file, and the hook keeps them off the
    // console.
    s_file_level = file_level;
    s_console_level = ESP_LOG_VERBOSE;
    if (file_level > esp_log_get_default_level()) {
        s_console_level = esp_log_get_default_level();
        esp_log_level_set("*", file_level);
    }
    s_console = esp_log_set_vprintf(tee_vprintf);
    s_started = true;
    return HAL_OK;
}

log_ring_t *log_tee_ring(void)
{
    return s_started ? &s_ring : NULL;
}

uint32_t log_tee_take_dropped(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    const uint32_t n = log_ring_take_dropped(&s_ring);
    portEXIT_CRITICAL_SAFE(&s_lock);
    return n;
}

void log_tee_hold(bool hold)
{
    s_hold = hold;
}

uint32_t log_tee_take_held(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    const uint32_t n = s_held;
    s_held = 0u;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return n;
}
