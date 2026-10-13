// SPDX-License-Identifier: Apache-2.0

#include "adp_i2c.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/i2c_periph.h"

#include "adp_i2c_pure.h"
#include "board_config.h"

// The S3 has two I2C controllers and the GPIO matrix can route either to any
// pin. Controller 0 is free: EPDiy's board definition for this board
// (epd_board_lilygo_t5_47_s3.c) uses none, and nothing else in the firmware
// claims one.
#define ADP_I2C_CONTROLLER I2C_NUM_0

// Pulses shorter than this many controller clock cycles are filtered out;
// 7 is the value the driver's own documentation gives as typical
// (i2c_master_bus_config_t::glitch_ignore_cnt).
#define ADP_I2C_GLITCH_IGNORE_CYCLES 7u

// The bus-clear procedure's clock limit (UM10204 3.1.16): a slave stuck in
// the middle of transmitting a byte has at most eight bits plus an
// acknowledge left to clock out.
#define ADP_I2C_RECOVERY_MAX_PULSES 9u

// Half an SCL period at the bus rate, for the bit-banged sequences below.
// I2C has no minimum clock rate, so a task switch that stretches one of
// these phases slows the sequence down and does nothing worse.
#define ADP_I2C_HALF_PERIOD_US (500000u / BOARD_I2C_SCL_HZ)

// How long a released line may take to read high before it counts as held
// low by something else. Rise time on a 100 kHz bus is around a
// microsecond; a millisecond is a line that is not going to rise.
#define ADP_I2C_LINE_RISE_TIMEOUT_US 1000u

// recover() has no deadline of its own. Every other holder of the lock
// releases it within its own `to_ms`, so waiting for it is bounded by them.
#define ADP_I2C_RECOVER_LOCK_WAIT portMAX_DELAY

#define US_PER_MS 1000

static const gpio_num_t SDA = (gpio_num_t)BOARD_I2C_SDA_GPIO;
static const gpio_num_t SCL = (gpio_num_t)BOARD_I2C_SCL_GPIO;

// --- error mapping ------------------------------------------------------------

// Every esp_err_t the driver can return, onto the port's vocabulary. A failed
// transfer's ESP_ERR_INVALID_STATE is classified further before this is
// reached (classify_failed_transfer()); here it is the generic case.
static hal_status_t status_from_esp_err(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return HAL_OK;
        case ESP_ERR_TIMEOUT:
            return HAL_ERR_TIMEOUT;
        case ESP_ERR_NOT_FOUND:
            return HAL_ERR_NOT_FOUND;
        case ESP_ERR_INVALID_ARG:
            return HAL_ERR_INVALID_ARG;
        case ESP_ERR_NO_MEM:
            return HAL_ERR_NO_MEM;
        case ESP_ERR_NOT_SUPPORTED:
            return HAL_ERR_UNSUPPORTED;
        case ESP_ERR_INVALID_STATE:
        default:
            return HAL_ERR_IO;
    }
}

static adp_i2c_probe_t probe_from_esp_err(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return ADP_I2C_PROBE_ACK;
        case ESP_ERR_NOT_FOUND:
            return ADP_I2C_PROBE_NACK;
        case ESP_ERR_TIMEOUT:
            return ADP_I2C_PROBE_TIMEOUT;
        default:
            return ADP_I2C_PROBE_OTHER;
    }
}

// --- deadlines ------------------------------------------------------------------

static int64_t deadline_after(uint32_t to_ms)
{
    return esp_timer_get_time() + (int64_t)to_ms * US_PER_MS;
}

// What to hand the driver (or xSemaphoreTake, in ms) so that it waits at
// least until `deadline_us`. 0 once the deadline has passed.
static uint32_t wait_ms_until(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0u;
    }
    return adp_i2c_driver_wait_ms((uint64_t)remaining_us, (uint32_t)configTICK_RATE_HZ);
}

static bool lock_until(adp_i2c_t *a, int64_t deadline_us)
{
    const uint32_t wait_ms = wait_ms_until(deadline_us);
    if (wait_ms == 0u) {
        return false;
    }
    return xSemaphoreTake((SemaphoreHandle_t)a->lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

static void unlock(adp_i2c_t *a)
{
    xSemaphoreGive((SemaphoreHandle_t)a->lock);
}

// --- the controller -------------------------------------------------------------

static esp_err_t bus_create(adp_i2c_t *a)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = ADP_I2C_CONTROLLER,
        .sda_io_num = SDA,
        .scl_io_num = SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = ADP_I2C_GLITCH_IGNORE_CYCLES,
        .intr_priority = 0,      // the driver's default
        .trans_queue_depth = 0,  // synchronous transfers only
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus = NULL;
    const esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    a->bus = (err == ESP_OK) ? bus : NULL;
    return err;
}

// Removes every device handle and deletes the bus, releasing the pins. The
// driver refuses to delete a bus with devices still attached, and refuses to
// remove a device while its status says a command is half-written (READ,
// WRITE or START -- i2c_master_bus_rm_device()). After any completed
// transfer the status is past those, so removal normally just works; if it
// is refused, i2c_master_bus_reset() puts the status back to IDLE and the
// removal is tried once more, so a refusal cannot strand the controller in
// an acquired state nobody can rebuild.
//
// Only as that fallback, never up front: the reset includes the controller's
// own clear-bus, and recover() destroys the bus before clocking it -- an
// unconditional reset here could free a stuck slave before recover()'s own
// pulses ran, and its log would credit the wrong mechanism.
static void bus_destroy(adp_i2c_t *a)
{
    bool reset_done = false;
    for (size_t i = 0; i < a->device_count; i++) {
        i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)a->devices[i].handle;
        if (i2c_master_bus_rm_device(dev) != ESP_OK && !reset_done && a->bus != NULL) {
            (void)i2c_master_bus_reset((i2c_master_bus_handle_t)a->bus);
            reset_done = true;
            (void)i2c_master_bus_rm_device(dev);
        }
    }
    a->device_count = 0u;
    a->next_eviction = 0u;
    if (a->bus != NULL) {
        (void)i2c_del_master_bus((i2c_master_bus_handle_t)a->bus);
        a->bus = NULL;
    }
}

static i2c_master_dev_handle_t device_for(adp_i2c_t *a, uint8_t addr, esp_err_t *err)
{
    *err = ESP_OK;
    for (size_t i = 0; i < a->device_count; i++) {
        if (a->devices[i].addr == addr) {
            return (i2c_master_dev_handle_t)a->devices[i].handle;
        }
    }

    adp_i2c_device_t *slot;
    if (a->device_count < ADP_I2C_MAX_DEVICES) {
        slot = &a->devices[a->device_count++];
    } else {
        slot = &a->devices[a->next_eviction];
        a->next_eviction = (a->next_eviction + 1u) % ADP_I2C_MAX_DEVICES;
        (void)i2c_master_bus_rm_device((i2c_master_dev_handle_t)slot->handle);
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_SCL_HZ,
        // 0 keeps the driver's default SCL stretch limit (2 ms, which the S3
        // rounds up to 2^17 XTAL cycles, ~3.3 ms). Nothing on this bus is
        // known to stretch the clock; a device that does will need more. It
        // is not what bounds a call: an SCL held low before START never trips
        // it (observed, ADR 0011) -- the caller's deadline does.
        .scl_wait_us = 0,
    };
    i2c_master_dev_handle_t dev = NULL;
    *err = i2c_master_bus_add_device((i2c_master_bus_handle_t)a->bus, &cfg, &dev);
    if (*err != ESP_OK) {
        // The slot is unusable; drop it rather than keep a dangling handle.
        *slot = a->devices[a->device_count - 1u];
        a->device_count--;
        a->next_eviction = 0u;
        return NULL;
    }
    slot->addr = addr;
    slot->handle = dev;
    return dev;
}

// --- the transfer -------------------------------------------------------------

// A failed transfer came back as ESP_ERR_INVALID_STATE, which the driver uses
// for every way a transfer can fail. Probe the address with whatever is left
// of the deadline: whether it answers now says which failure it was.
static hal_status_t classify_failed_transfer(adp_i2c_t *a, uint8_t addr, int64_t deadline_us)
{
    const uint32_t wait_ms = wait_ms_until(deadline_us);
    if (wait_ms == 0u) {
        return adp_i2c_classify_failure(ADP_I2C_PROBE_NOT_RUN);
    }
    const esp_err_t err = i2c_master_probe((i2c_master_bus_handle_t)a->bus, addr, (int)wait_ms);
    return adp_i2c_classify_failure(probe_from_esp_err(err));
}

static bool args_valid(uint8_t addr, uint32_t to_ms)
{
    return addr <= PORT_I2C_ADDR_MAX && to_ms > 0u;
}

// The transaction itself, with the lock held. `*err` is what the driver last
// returned, for the log.
static hal_status_t transfer_locked(adp_i2c_t *a, uint8_t addr, const uint8_t *w, size_t wn,
                                    uint8_t *r, size_t rn, int64_t deadline_us, esp_err_t *err)
{
    if (a->bus == NULL) {
        return HAL_ERR_IO;  // the last rebuild failed; recover() is the way back
    }

    // The previous transfer failed. The driver resets its controller before
    // the next transfer by itself only while its own status still says
    // "timeout" -- and the probe classify_failed_transfer() ran sets that
    // status back to "done" (i2c_master_probe()'s last step). So the reset
    // the driver would have done is done here instead.
    if (a->reset_pending) {
        (void)i2c_master_bus_reset((i2c_master_bus_handle_t)a->bus);
        a->reset_pending = false;
    }

    const uint32_t wait_ms = wait_ms_until(deadline_us);
    if (wait_ms == 0u) {
        return HAL_ERR_TIMEOUT;  // waiting for the lock took the whole deadline
    }

    if (wn == 0u && rn == 0u) {
        // The driver rejects a zero-length transmit as an invalid argument;
        // its probe is the same START, address+W, STOP on the wire.
        *err = i2c_master_probe((i2c_master_bus_handle_t)a->bus, addr, (int)wait_ms);
        return status_from_esp_err(*err);
    }

    i2c_master_dev_handle_t dev = device_for(a, addr, err);
    if (dev == NULL) {
        return status_from_esp_err(*err);
    }

    if (rn == 0u) {
        *err = i2c_master_transmit(dev, w, wn, (int)wait_ms);
    } else if (wn == 0u) {
        *err = i2c_master_receive(dev, r, rn, (int)wait_ms);
    } else {
        *err = i2c_master_transmit_receive(dev, w, wn, r, rn, (int)wait_ms);
    }

    if (*err == ESP_ERR_INVALID_STATE) {
        return classify_failed_transfer(a, addr, deadline_us);
    }
    return status_from_esp_err(*err);
}

// One transaction: an address-only probe when wn == rn == 0, a write when
// rn == 0, a read when wn == 0, a write-then-read otherwise. The whole call,
// waiting for the lock included, fits inside one deadline.
static hal_status_t transfer(adp_i2c_t *a, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r,
                             size_t rn, uint32_t to_ms)
{
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;  // adp_i2c_init() failed before the bus existed
    }
    const int64_t deadline_us = deadline_after(to_ms);
    if (!lock_until(a, deadline_us)) {
        return HAL_ERR_BUSY;
    }

    esp_err_t err = ESP_OK;
    const hal_status_t status = transfer_locked(a, addr, w, wn, r, rn, deadline_us, &err);

    if (status == HAL_ERR_TIMEOUT || status == HAL_ERR_IO) {
        a->reset_pending = true;
    }
    if (status != HAL_OK) {
        const log_field_t fields[] = {
            log_u64("addr", addr),
            log_i64("esp_err", (int64_t)err),
            log_i64("status", (int64_t)status),
            log_u64("to_ms", to_ms),
        };
        port_log_write(a->log, LOG_LEVEL_DEBUG, "adp_i2c", "transfer failed", fields,
                       sizeof(fields) / sizeof(fields[0]));
    }
    unlock(a);
    return status;
}

static hal_status_t i2c_write(port_i2c_t *port, uint8_t addr, const uint8_t *d, size_t n,
                              uint32_t to_ms)
{
    if (!args_valid(addr, to_ms) || (d == NULL && n > 0u)) {
        return HAL_ERR_INVALID_ARG;
    }
    return transfer((adp_i2c_t *)port->impl, addr, d, n, NULL, 0u, to_ms);
}

static hal_status_t i2c_read(port_i2c_t *port, uint8_t addr, uint8_t *d, size_t n, uint32_t to_ms)
{
    if (!args_valid(addr, to_ms) || d == NULL || n == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    return transfer((adp_i2c_t *)port->impl, addr, NULL, 0u, d, n, to_ms);
}

static hal_status_t i2c_write_read(port_i2c_t *port, uint8_t addr, const uint8_t *w, size_t wn,
                                   uint8_t *r, size_t rn, uint32_t to_ms)
{
    if (!args_valid(addr, to_ms) || w == NULL || wn == 0u || r == NULL || rn == 0u) {
        return HAL_ERR_INVALID_ARG;
    }
    return transfer((adp_i2c_t *)port->impl, addr, w, wn, r, rn, to_ms);
}

// --- the pins as plain GPIOs ------------------------------------------------------
//
// Open-drain throughout: "high" is released, and a line reads high only if
// nothing on the bus is pulling it low. Only ever called with the controller
// torn down (bus_destroy()), so the controller and these writes never fight.

static esp_err_t pins_to_gpio(void)
{
    // Level first, then direction: the output register already says
    // "released" when the pin starts driving.
    gpio_set_level(SDA, 1u);
    gpio_set_level(SCL, 1u);
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_I2C_SDA_GPIO) | (1ULL << BOARD_I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void half_period(void)
{
    esp_rom_delay_us(ADP_I2C_HALF_PERIOD_US);
}

static bool line_high(gpio_num_t pin)
{
    return gpio_get_level(pin) != 0;
}

// Releases `pin` and waits for it to read high. False if something keeps it
// low -- a slave stretching SCL, or a short.
static bool release(gpio_num_t pin)
{
    gpio_set_level(pin, 1u);
    for (uint32_t waited = 0u; waited < ADP_I2C_LINE_RISE_TIMEOUT_US; waited++) {
        if (line_high(pin)) {
            return true;
        }
        esp_rom_delay_us(1u);
    }
    return line_high(pin);
}

static void pull_low(gpio_num_t pin)
{
    gpio_set_level(pin, 0u);
}

// UM10204 3.1.16, in the form ESP-IDF's own software fallback and Linux's
// i2c_generic_scl_recovery() both use. SCL goes low first: if a slave is
// holding an acknowledge with SCL high, this falling edge is what releases
// it. Then, while SDA is still held, one more clock at a time, up to nine.
// SDA is sampled with SCL low, and the STOP follows with only a rising edge
// of SCL, so the stuck slave never sees one falling edge too many: if it was
// transmitting, the STOP lands while it is presenting a 1 -- a STOP it
// recognises -- rather than on its next bit.
//
// It stops as soon as SDA is free instead of always clocking nine times: a
// slave that was RECEIVING when the bus stuck lets go after one clock, and
// eight more would clock 0xFF into whatever register it was writing.
//
// Returns the number of full SCL pulses after the first falling edge.
static unsigned bus_clear(bool *scl_stuck)
{
    unsigned pulses = 0u;
    *scl_stuck = false;

    pull_low(SCL);
    half_period();
    while (!line_high(SDA) && pulses < ADP_I2C_RECOVERY_MAX_PULSES) {
        if (!release(SCL)) {
            *scl_stuck = true;
            return pulses;
        }
        half_period();
        pull_low(SCL);
        half_period();
        pulses++;
    }

    // STOP: SDA low while SCL is low, SCL high, then SDA high.
    pull_low(SDA);
    half_period();
    if (!release(SCL)) {
        *scl_stuck = true;
    }
    half_period();
    (void)release(SDA);
    half_period();
    return pulses;
}

static hal_status_t i2c_recover(port_i2c_t *port)
{
    adp_i2c_t *a = (adp_i2c_t *)port->impl;
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, ADP_I2C_RECOVER_LOCK_WAIT);

    bool sda_was_high = false;
    bool scl_was_high = false;
    adp_i2c_lines(a, &sda_was_high, &scl_was_high);

    bus_destroy(a);
    const esp_err_t gpio_err = pins_to_gpio();
    bool scl_stuck = false;
    const unsigned pulses = (gpio_err == ESP_OK) ? bus_clear(&scl_stuck) : 0u;
    const bool sda_high = line_high(SDA);
    const bool scl_high = line_high(SCL);
    const esp_err_t bus_err = bus_create(a);

    a->reset_pending = false;
    a->last_recover_pulses = pulses;

    hal_status_t status = HAL_OK;
    if (gpio_err != ESP_OK || bus_err != ESP_OK) {
        status = status_from_esp_err(gpio_err != ESP_OK ? gpio_err : bus_err);
    } else if (!sda_high || !scl_high || scl_stuck) {
        status = HAL_ERR_IO;
    }

    const log_field_t fields[] = {
        log_bool("sda_was_high", sda_was_high),
        log_bool("scl_was_high", scl_was_high),
        log_u64("pulses", pulses),
        log_bool("sda_high", sda_high),
        log_bool("scl_high", scl_high),
        log_i64("status", (int64_t)status),
    };
    port_log_write(a->log, status == HAL_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "adp_i2c",
                   "recover", fields, sizeof(fields) / sizeof(fields[0]));

    unlock(a);
    return status;
}

// --- public -----------------------------------------------------------------------

hal_status_t adp_i2c_init(adp_i2c_t *a, const port_log_t *log)
{
    memset(a, 0, sizeof *a);
    a->log = log;
    a->port.write = i2c_write;
    a->port.read = i2c_read;
    a->port.write_read = i2c_write_read;
    a->port.recover = i2c_recover;
    a->port.impl = a;

    a->lock = xSemaphoreCreateMutex();
    if (a->lock == NULL) {
        return HAL_ERR_NO_MEM;
    }

    const esp_err_t err = bus_create(a);
    bool sda_high = false;
    bool scl_high = false;
    adp_i2c_lines(a, &sda_high, &scl_high);

    const log_field_t fields[] = {
        log_u64("sda_gpio", BOARD_I2C_SDA_GPIO), log_u64("scl_gpio", BOARD_I2C_SCL_GPIO),
        log_u64("scl_hz", BOARD_I2C_SCL_HZ),     log_bool("sda_high", sda_high),
        log_bool("scl_high", scl_high),          log_i64("esp_err", (int64_t)err),
    };
    port_log_write(log, err == ESP_OK ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR, "adp_i2c", "bus up",
                   fields, sizeof(fields) / sizeof(fields[0]));

    return status_from_esp_err(err);
}

port_i2c_t *adp_i2c_port(adp_i2c_t *a)
{
    return &a->port;
}

void adp_i2c_deinit(adp_i2c_t *a)
{
    if (a->lock == NULL) {
        return;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, ADP_I2C_RECOVER_LOCK_WAIT);
    bus_destroy(a);
    unlock(a);
    vSemaphoreDelete((SemaphoreHandle_t)a->lock);
    a->lock = NULL;
}

void adp_i2c_lines(adp_i2c_t *a, bool *sda_high, bool *scl_high)
{
    (void)a;  // the pins are the board's, whoever owns them
    *sda_high = line_high(SDA);
    *scl_high = line_high(SCL);
}

// --- fault injection ----------------------------------------------------------------

hal_status_t adp_i2c_inject_stuck_slave(adp_i2c_t *a, uint8_t addr)
{
    if (addr > PORT_I2C_ADDR_MAX) {
        return HAL_ERR_INVALID_ARG;
    }
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, ADP_I2C_RECOVER_LOCK_WAIT);

    bus_destroy(a);
    hal_status_t status = HAL_OK;
    bool acked = false;
    if (pins_to_gpio() != ESP_OK || !line_high(SDA) || !line_high(SCL)) {
        status = HAL_ERR_BUSY;  // not an idle bus to start from
    } else {
        // START: SDA falls while SCL is high.
        pull_low(SDA);
        half_period();
        pull_low(SCL);
        half_period();

        const uint8_t byte = (uint8_t)((addr << 1) | 1u);  // read
        for (int bit = 7; bit >= 0; bit--) {
            if ((byte >> bit) & 1u) {
                (void)release(SDA);
            } else {
                pull_low(SDA);
            }
            half_period();
            (void)release(SCL);
            half_period();
            pull_low(SCL);
        }

        // The acknowledge clock: SDA released for the slave, SCL up -- and
        // left up. The slave holds its acknowledge until SCL falls.
        (void)release(SDA);
        half_period();
        (void)release(SCL);
        half_period();
        acked = !line_high(SDA);
    }

    const esp_err_t err = bus_create(a);
    const bool held = !line_high(SDA);
    if (status == HAL_OK) {
        if (err != ESP_OK) {
            status = status_from_esp_err(err);
        } else if (!acked || !held) {
            status = HAL_ERR_IO;
        }
    }

    const log_field_t fields[] = {
        log_u64("addr", addr),
        log_bool("acked", acked),
        log_bool("sda_held_after_handover", held),
        log_i64("status", (int64_t)status),
    };
    port_log_write(a->log, LOG_LEVEL_INFO, "adp_i2c", "inject stuck slave", fields,
                   sizeof(fields) / sizeof(fields[0]));

    unlock(a);
    return status;
}

hal_status_t adp_i2c_hold_scl_low(adp_i2c_t *a)
{
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, ADP_I2C_RECOVER_LOCK_WAIT);
    // The output register first, then the matrix: gpio_set_direction() routes
    // the plain GPIO output to the pin in place of the controller's SCL, and
    // leaves the controller's SCL input reading the pin.
    gpio_set_level(SCL, 0u);
    const esp_err_t err = gpio_set_direction(SCL, GPIO_MODE_INPUT_OUTPUT_OD);
    const bool low = !line_high(SCL);
    unlock(a);
    if (err != ESP_OK) {
        return status_from_esp_err(err);
    }
    return low ? HAL_OK : HAL_ERR_IO;
}

hal_status_t adp_i2c_release_scl(adp_i2c_t *a)
{
    if (a->lock == NULL) {
        return HAL_ERR_NOT_READY;
    }
    (void)xSemaphoreTake((SemaphoreHandle_t)a->lock, ADP_I2C_RECOVER_LOCK_WAIT);
    // Exactly what the driver's own pin setup does for SCL (i2c_common.c,
    // s_hp_i2c_pins_config()): idle level high, controller's SCL output back
    // on the pin.
    gpio_set_level(SCL, 1u);
    esp_rom_gpio_connect_out_signal(
        (uint32_t)SCL, i2c_periph_signal[ADP_I2C_CONTROLLER].scl_out_sig, false, false);
    const bool high = release(SCL);
    unlock(a);
    return high ? HAL_OK : HAL_ERR_IO;
}
