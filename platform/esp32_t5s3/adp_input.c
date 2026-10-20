// SPDX-License-Identifier: Apache-2.0

#include "adp_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#include "board_config.h"

#define TAG "adp_input"

// The shared GPIO ISR service's interrupt flags: none -- any priority level,
// a handler not in IRAM. Named also because scripts/check_sources.sh reads a
// bare literal as the first argument of a gpio_*() call as a pin number.
#define ISR_SERVICE_FLAGS 0

static adp_input_t *self_of(void *ctx)
{
    return (adp_input_t *)ctx;
}

// --- the INT line, for the driver ------------------------------------------------
//
// The interrupt is masked whenever the host drives the line, so its own
// edges are not taken for the controller's, and unmasked when the line is
// the controller's again.

static void int_mask(adp_input_t *a)
{
    if (a->isr_on) {
        (void)gpio_intr_disable(BOARD_TOUCH_IRQ_GPIO);
    }
}

static void int_unmask(adp_input_t *a)
{
    if (a->isr_on) {
        (void)gpio_intr_enable(BOARD_TOUCH_IRQ_GPIO);
    }
}

static void line_drive_low(void *ctx)
{
    adp_input_t *a = self_of(ctx);
    int_mask(a);
    (void)gpio_set_level(BOARD_TOUCH_IRQ_GPIO, 0);
    (void)gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_OUTPUT);
}

static void line_release(void *ctx)
{
    adp_input_t *a = self_of(ctx);
    (void)gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_INPUT);
    int_unmask(a);
}

// The datasheet's 2-5 ms, busy-waited with the scheduler held, so that not
// even EPDiy's render tasks -- the highest priority there is, on both cores
// -- can stretch it past 5 ms. Interrupts still run; they take microseconds.
static void line_pulse_high(void *ctx, uint32_t us)
{
    adp_input_t *a = self_of(ctx);
    int_mask(a);
    (void)gpio_set_drive_capability(BOARD_TOUCH_IRQ_GPIO, BOARD_TOUCH_INT_WAKE_DRIVE);
    (void)gpio_set_level(BOARD_TOUCH_IRQ_GPIO, 1);
    vTaskSuspendAll();
    (void)gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_delay_us(us);
    (void)gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_INPUT);
    (void)xTaskResumeAll();
    int_unmask(a);
}

// --- the interrupt and the task ----------------------------------------------------

static void touch_isr(void *arg)
{
    adp_input_t *a = (adp_input_t *)arg;
    a->interrupts++;
    if (a->level_triggered) {
        // The line stays at its level until the report is read: masked until
        // the task has read it.
        (void)gpio_intr_disable(BOARD_TOUCH_IRQ_GPIO);
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)a->task, &woken);
    portYIELD_FROM_ISR(woken);
}

// The events gt911_input queued, into the queue the event loop polls.
static size_t hand_over(adp_input_t *a)
{
    port_input_t *from = gt911_input_port(&a->gt);
    size_t moved = 0u;
    input_event_t ev;
    while (from->poll(from, &ev)) {
        if (xQueueSend((QueueHandle_t)a->queue, &ev, 0) == pdTRUE) {
            ++moved;
        } else {
            a->queue_dropped++;
        }
    }
    return moved;
}

static void track_stack(adp_input_t *a)
{
    // Bytes on ESP-IDF's Xtensa port (event_loop.c).
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (free_bytes >= a->stack_free_min_bytes) {
        return;
    }
    a->stack_free_min_bytes = free_bytes;
    const log_field_t fields[] = {log_u64("stack_free_min_bytes", free_bytes)};
    port_log_write(a->cfg.log, LOG_LEVEL_INFO, TAG, "stack low-water mark", fields, 1u);
}

static TickType_t ticks_until(const adp_input_t *a, uint64_t deadline)
{
    if (deadline == GT911_INPUT_NO_DEADLINE) {
        return portMAX_DELAY;
    }
    const uint64_t now = a->cfg.clock->now_ms(a->cfg.clock);
    const uint64_t ms = (deadline > now) ? deadline - now : 0u;
    // Whole ticks, rounded up, and at least one: a deadline is never early.
    return (TickType_t)(pdMS_TO_TICKS((uint32_t)ms) + 1u);
}

static gpio_int_type_t intr_type_of(gt911_trigger_t trigger)
{
    switch (trigger) {
        case GT911_TRIGGER_RISING:
            return GPIO_INTR_POSEDGE;
        case GT911_TRIGGER_FALLING:
            return GPIO_INTR_NEGEDGE;
        case GT911_TRIGGER_LOW_LEVEL:
            return GPIO_INTR_LOW_LEVEL;
        case GT911_TRIGGER_HIGH_LEVEL:
            return GPIO_INTR_HIGH_LEVEL;
    }
    return GPIO_INTR_POSEDGE;
}

// The interrupt on the edge the controller's configuration selects, once the
// controller has been found -- the edge is its configuration's. Once.
static hal_status_t start_isr(adp_input_t *a)
{
    if (a->isr_on) {
        return HAL_OK;
    }
    const gt911_trigger_t trigger = gt911_info(&a->gt.dev)->trigger;
    a->level_triggered = trigger == GT911_TRIGGER_LOW_LEVEL || trigger == GT911_TRIGGER_HIGH_LEVEL;

    // Someone else may have installed the shared GPIO ISR service already.
    esp_err_t err = gpio_install_isr_service(ISR_SERVICE_FLAGS);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = gpio_set_intr_type(BOARD_TOUCH_IRQ_GPIO, intr_type_of(trigger));
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(BOARD_TOUCH_IRQ_GPIO, touch_isr, a);
    }
    if (err == ESP_OK) {
        a->isr_on = true;
        err = gpio_intr_enable(BOARD_TOUCH_IRQ_GPIO);
    }
    const log_field_t fields[] = {
        log_u64("gpio", BOARD_TOUCH_IRQ_GPIO),
        log_str("trigger", gt911_trigger_name(trigger)),
        log_i64("esp_err", (int64_t)err),
    };
    port_log_write(a->cfg.log, (err == ESP_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                   (err == ESP_OK) ? "interrupt on"
                                   : "interrupt not installed: reads only by polling",
                   fields, sizeof(fields) / sizeof(fields[0]));
    // Without the interrupt the task still runs: it reads every time a finger
    // is down -- which, without an interrupt, it never learns of. Touch is
    // then off in all but name; the log says so.
    return (err == ESP_OK) ? HAL_OK : HAL_ERR_IO;
}

// The controller's first init, on this task rather than app_main's: a
// controller that does not answer at once costs up to a second of looking,
// which the first frame should not wait for, and app_main's 3.5 KB stack is
// no place for an I2C driver's error lines (adp_input.h).
static void first_init(adp_input_t *a)
{
    const port_clock_t *clock = a->cfg.clock;
    const uint64_t start = clock->now_ms(clock);
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
    hal_status_t st = gt911_input_init(&a->gt);
    if (st == HAL_OK) {
        st = start_isr(a);
    }
    a->init_status = st;
    (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
    const log_field_t fields[] = {
        log_i64("status", (int64_t)st),
        log_u64("took_ms", clock->now_ms(clock) - start),
    };
    port_log_write(a->cfg.log, (st == HAL_OK) ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, TAG,
                   (st == HAL_OK) ? "touch ready"
                                  : "no touch controller: looked for again every minute",
                   fields, sizeof(fields) / sizeof(fields[0]));
    a->init_done = true;
}

static void touch_task(void *arg)
{
    adp_input_t *a = (adp_input_t *)arg;
    a->stack_free_min_bytes = UINT32_MAX;
    first_init(a);
    uint64_t deadline = gt911_input_service(&a->gt);
    for (;;) {
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, ticks_until(a, deadline));
        (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
        if (notified > 0u) {
            gt911_input_signal(&a->gt);
        }
        deadline = gt911_input_service(&a->gt);
        if (!a->isr_on && gt911_input_ready(&a->gt)) {
            a->init_status = start_isr(a);  // found at last, after an init that found nothing
        }
        const size_t moved = hand_over(a);
        if (a->level_triggered && !a->gt.dev.asleep) {
            int_unmask(a);
        }
        (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
        if (moved > 0u && a->waker != NULL) {
            a->waker(a->waker_ctx);
        }
        track_stack(a);
    }
}

// --- port_input ---------------------------------------------------------------------

static bool adp_poll(port_input_t *port, input_event_t *out)
{
    adp_input_t *a = (adp_input_t *)port->impl;
    if (a->queue == NULL) {
        return false;
    }
    return xQueueReceive((QueueHandle_t)a->queue, out, 0) == pdTRUE;
}

hal_status_t adp_input_init(adp_input_t *a, const adp_input_config_t *cfg)
{
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    a->port.poll = adp_poll;
    a->port.impl = a;
    a->init_status = HAL_ERR_NOT_READY;

    a->lock = xSemaphoreCreateMutex();
    a->queue = xQueueCreate(ADP_INPUT_QUEUE_LEN, sizeof(input_event_t));
    if (a->lock == NULL || a->queue == NULL) {
        port_log_write(cfg->log, LOG_LEVEL_ERROR, TAG, "queue or mutex not created: no touch", NULL,
                       0u);
        a->init_status = HAL_ERR_NO_MEM;
        return a->init_status;
    }

    // An input, no pull: the GT911 drives INT, and the board fits no
    // pull-up on it (docs/hardware/board_notes.md, "Connectors": N5 is NC).
    (void)gpio_reset_pin(BOARD_TOUCH_IRQ_GPIO);
    (void)gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_INPUT);
    (void)gpio_set_pull_mode(BOARD_TOUCH_IRQ_GPIO, GPIO_FLOATING);

    a->line.drive_low = line_drive_low;
    a->line.pulse_high = line_pulse_high;
    a->line.release = line_release;
    a->line.ctx = a;

    gt911_input_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.device.bus = cfg->bus;
    gcfg.device.clock = cfg->clock;
    gcfg.device.log = cfg->log;
    gcfg.device.address = BOARD_TOUCH_I2C_ADDRESS;
    gcfg.device.alt_address = BOARD_TOUCH_ALT_I2C_ADDRESS;
    gcfg.device.int_line = &a->line;
    gcfg.map.swap_xy = BOARD_TOUCH_SWAP_XY != 0;
    gcfg.map.mirror_x = BOARD_TOUCH_MIRROR_X != 0;
    gcfg.map.mirror_y = BOARD_TOUCH_MIRROR_Y != 0;
    gcfg.map.panel_width = (uint16_t)BOARD_EPD_WIDTH;
    gcfg.map.panel_height = (uint16_t)BOARD_EPD_HEIGHT;
    gcfg.move_threshold_px = GT911_INPUT_MOVE_THRESHOLD_PX;
    gcfg.log = cfg->log;
    const hal_status_t st = gt911_input_setup(&a->gt, &gcfg);
    if (st != HAL_OK) {
        a->init_status = st;
        return st;
    }

    // The controller is found by the task, not here (first_init()).
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(touch_task, "hac_touch", cfg->task_stack_bytes, a,
                                (UBaseType_t)cfg->task_priority, &task,
                                (BaseType_t)cfg->task_core) != pdPASS) {
        port_log_write(cfg->log, LOG_LEVEL_ERROR, TAG, "task not created: no touch", NULL, 0u);
        a->init_status = HAL_ERR_NO_MEM;
        return a->init_status;
    }
    a->task = task;
    return HAL_OK;
}

hal_status_t adp_input_wait_ready(adp_input_t *a, uint32_t timeout_ms)
{
    const port_clock_t *clock = a->cfg.clock;
    if (a->task == NULL || clock == NULL) {
        return a->init_status;
    }
    const uint64_t give_up_at = clock->now_ms(clock) + timeout_ms;
    while (!a->init_done) {
        if (clock->now_ms(clock) >= give_up_at) {
            return HAL_ERR_TIMEOUT;
        }
        clock->delay_ms(clock, ADP_INPUT_WAIT_POLL_MS);
    }
    return a->init_status;
}

port_input_t *adp_input_port(adp_input_t *a)
{
    return &a->port;
}

void adp_input_set_waker(adp_input_t *a, adp_input_waker_fn fn, void *ctx)
{
    a->waker_ctx = ctx;
    a->waker = fn;
}

hal_status_t adp_input_sleep(adp_input_t *a)
{
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
    const hal_status_t st = gt911_input_sleep(&a->gt);
    const size_t moved = hand_over(a);  // a finger down is lifted now
    (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
    if (moved > 0u && a->waker != NULL) {
        a->waker(a->waker_ctx);
    }
    return st;
}

// Wakes a controller put to sleep -- or, one never found, looks for it
// again from the start, and turns the interrupt on if it is found now.
hal_status_t adp_input_wake(adp_input_t *a)
{
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
    hal_status_t st;
    if (gt911_input_ready(&a->gt)) {
        st = gt911_input_wake(&a->gt);
    } else {
        st = gt911_input_init(&a->gt);
        if (st == HAL_OK) {
            st = start_isr(a);
        }
        a->init_status = st;
    }
    (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
    return st;
}

void adp_input_status(adp_input_t *a, adp_input_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->init_status = a->init_status;
    out->init_done = a->init_done;
    if (a->lock == NULL) {
        return;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
    const gt911_t *dev = gt911_input_device(&a->gt);
    out->found = dev->found;
    out->asleep = dev->asleep;
    out->info = *gt911_info(dev);
    out->stats = *gt911_input_stats(&a->gt);
    out->history_count = gt911_input_history(&a->gt, out->history, GT911_INPUT_HISTORY);
    (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
    out->interrupts = a->interrupts;
    out->queue_dropped = a->queue_dropped;
    out->stack_free_min_bytes = a->stack_free_min_bytes;
}

void adp_input_deinit(adp_input_t *a)
{
    if (a->lock != NULL) {
        (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, portMAX_DELAY);
    }
    if (a->isr_on) {
        (void)gpio_intr_disable(BOARD_TOUCH_IRQ_GPIO);
        (void)gpio_isr_handler_remove(BOARD_TOUCH_IRQ_GPIO);
        a->isr_on = false;
    }
    if (a->task != NULL) {
        vTaskDelete(
            (TaskHandle_t)a->task);  // waiting for the lock or a notification: holds nothing
        a->task = NULL;
    }
    if (a->queue != NULL) {
        vQueueDelete((QueueHandle_t)a->queue);
        a->queue = NULL;
    }
    if (a->lock != NULL) {
        (void)xSemaphoreGive((SemaphoreHandle_t)a->lock);
        vSemaphoreDelete((SemaphoreHandle_t)a->lock);
        a->lock = NULL;
    }
}
