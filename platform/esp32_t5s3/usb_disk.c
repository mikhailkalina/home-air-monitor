// SPDX-License-Identifier: Apache-2.0

#include "usb_disk.h"

#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"

// TinyUSB's headers and its descriptor macros shift and narrow ints into
// bytes, which -Wconversion and -Wsign-conversion flag in every translation
// unit that expands them. Suppressed for this file's TinyUSB parts only, as
// adp_display.c does around EPDiy's header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tusb.h"
#pragma GCC diagnostic pop

// The disk's one interface and its two bulk endpoints, full speed's 64 bytes.
enum { ITF_MSC = 0, ITF_COUNT };
#define EP_MSC_OUT 0x01u
#define EP_MSC_IN 0x81u
#define EP_MSC_SIZE 64u
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

// String descriptors' indices.
enum {
    STR_LANGUAGE = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_INTERFACE,
    STR_COUNT,
};

// The longest string descriptor, in UTF-16 code units.
#define STRING_MAX_CHARS 32u
#define LANGUAGE_EN_US 0x0409u

// SCSI's additional sense codes this disk answers with (SPC-4, Annex D).
#define ASC_INVALID_COMMAND 0x20u
#define ASC_INVALID_FIELD 0x24u
#define ASC_MEDIUM_NOT_PRESENT 0x3Au
#define ASC_UNRECOVERED_READ 0x11u
#define ASC_WRITE_ERROR 0x0Cu
#define SCSI_SYNCHRONIZE_CACHE_10 0x35u

#define MAC_BYTES 6u
#define SERIAL_CHARS (2u * MAC_BYTES)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"

static const tusb_desc_device_t DEVICE = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // The class is the interface's: mass storage.
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_DISK_VID,
    .idProduct = USB_DISK_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t CONFIGURATION[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, USB_DISK_MAX_POWER_MA),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_INTERFACE, EP_MSC_OUT, EP_MSC_IN, EP_MSC_SIZE),
};

#pragma GCC diagnostic pop

static const char *const STRINGS[STR_COUNT] = {
    [STR_LANGUAGE] = NULL,  // the language list, below
    [STR_MANUFACTURER] = "Home Air Monitor",
    [STR_PRODUCT] = "Home Air Monitor card",
    [STR_SERIAL] = NULL,  // the MAC, at start
    [STR_INTERFACE] = "microSD card",
};

typedef struct {
    adp_sd_t *sd;
    bool active;
    usb_phy_handle_t otg_phy;
    // USB-Serial-JTAG's PHY once this has given it back; NULL before the
    // first session, when nothing has taken it.
    usb_phy_handle_t usj_phy;
    usb_disk_session_t session;
    uint64_t now_ms;  // what usb_disk_step() was given: TinyUSB's callbacks run inside it
    uint32_t sector_bytes;
    uint32_t sectors;
    usb_disk_stats_t stats;
    char serial[SERIAL_CHARS + 1u];
    uint16_t string[1u + STRING_MAX_CHARS];
} disk_t;

static disk_t s_d;

static uint32_t elapsed_us(int64_t since_us)
{
    const int64_t us = esp_timer_get_time() - since_us;
    return (us > 0 && us < (int64_t)UINT32_MAX) ? (uint32_t)us : 0u;
}

static void make_serial(void)
{
    static const char HEX[] = "0123456789ABCDEF";
    uint8_t mac[MAC_BYTES] = {0};
    (void)esp_efuse_mac_get_default(mac);
    for (size_t i = 0u; i < MAC_BYTES; ++i) {
        s_d.serial[2u * i] = HEX[mac[i] >> 4];
        s_d.serial[2u * i + 1u] = HEX[mac[i] & 0x0Fu];
    }
    s_d.serial[SERIAL_CHARS] = '\0';
}

// The PHY to USB-Serial-JTAG: its D+ pull-up shows the host the serial port
// again. ESP-IDF's usb_del_phy() leaves the internal PHY on the controller
// that had it -- the selection is in RTC_CNTL_USB_CONF_REG, which a software
// reset keeps, and only the console's USB-Serial-JTAG driver, installed late
// in a boot, selects it again -- so the switch back is made here.
static void phy_to_usb_serial_jtag(void)
{
    if (s_d.usj_phy != NULL) {
        return;
    }
    const usb_phy_config_t usj = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target = USB_PHY_TARGET_INT,
    };
    (void)usb_new_phy(&usj, &s_d.usj_phy);
}

hal_status_t usb_disk_start(adp_sd_t *sd, uint64_t now_ms)
{
    if (s_d.active) {
        return HAL_ERR_BUSY;
    }
    uint32_t sectors = 0u;
    uint32_t sector_bytes = 0u;
    if (adp_sd_lent_geometry(sd, &sectors, &sector_bytes) != HAL_OK) {
        return HAL_ERR_NOT_READY;
    }
    if (sector_bytes == 0u || USB_DISK_MSC_BUFFER_BYTES % sector_bytes != 0u ||
        sector_bytes > UINT16_MAX) {
        return HAL_ERR_UNSUPPORTED;
    }

    s_d.sd = sd;
    s_d.sectors = sectors;
    s_d.sector_bytes = sector_bytes;
    s_d.now_ms = now_ms;
    make_serial();
    const usb_disk_session_config_t cfg = usb_disk_session_config_default();
    usb_disk_session_start(&s_d.session, &cfg, now_ms);

    // USB-Serial-JTAG gives the PHY up first, if a session before this one
    // gave it back through usb_new_phy().
    if (s_d.usj_phy != NULL) {
        (void)usb_del_phy(s_d.usj_phy);
        s_d.usj_phy = NULL;
    }
    const usb_phy_config_t otg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    if (usb_new_phy(&otg, &s_d.otg_phy) != ESP_OK) {
        s_d.otg_phy = NULL;
        phy_to_usb_serial_jtag();
        return HAL_ERR_IO;
    }
    const tusb_rhport_init_t init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    // The controller's interrupt is allocated on this task's core.
    if (!tusb_init(0, &init)) {
        (void)usb_del_phy(s_d.otg_phy);
        s_d.otg_phy = NULL;
        phy_to_usb_serial_jtag();
        return HAL_ERR_IO;
    }
    // A new session's figures; one that never started leaves the last one's.
    const uint32_t sessions = s_d.stats.sessions;
    memset(&s_d.stats, 0, sizeof(s_d.stats));
    s_d.stats.sessions = sessions + 1u;
    s_d.stats.card_sectors = sectors;
    s_d.active = true;
    s_d.stats.active = true;
    return HAL_OK;
}

void usb_disk_step(uint32_t timeout_ms, uint64_t now_ms)
{
    if (!s_d.active) {
        return;
    }
    s_d.now_ms = now_ms;
    tud_task_ext(timeout_ms, false);
}

void usb_disk_press(void)
{
    if (s_d.active) {
        usb_disk_session_button(&s_d.session);
    }
}

usb_disk_verdict_t usb_disk_verdict(uint64_t now_ms)
{
    return s_d.active ? usb_disk_session_verdict(&s_d.session, now_ms) : USB_DISK_KEEP;
}

void usb_disk_stop(uint64_t now_ms)
{
    if (!s_d.active) {
        return;
    }
    s_d.stats.verdict = usb_disk_session_verdict(&s_d.session, now_ms);
    s_d.stats.lasted_ms = (now_ms > s_d.session.started_ms) ? now_ms - s_d.session.started_ms : 0u;
    s_d.stats.configurations = s_d.session.configurations;
    // The controller's pull-up off -- the host sees the disk unplugged --
    // its interrupt freed, the stack's queue deleted.
    (void)tusb_deinit(0);
    (void)usb_del_phy(s_d.otg_phy);
    s_d.otg_phy = NULL;
    phy_to_usb_serial_jtag();
    s_d.active = false;
    s_d.stats.active = false;
    s_d.sd = NULL;
}

bool usb_disk_active(void)
{
    return s_d.active;
}

void usb_disk_stats(usb_disk_stats_t *out)
{
    *out = s_d.stats;
    if (s_d.active) {
        out->verdict = USB_DISK_KEEP;
        out->lasted_ms =
            (s_d.now_ms > s_d.session.started_ms) ? s_d.now_ms - s_d.session.started_ms : 0u;
        out->configurations = s_d.session.configurations;
    }
}

// --- TinyUSB's callbacks: all on the task in usb_disk_step() ------------------------

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&DEVICE;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return CONFIGURATION;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chars = 0u;
    if (index == STR_LANGUAGE) {
        s_d.string[1] = LANGUAGE_EN_US;
        chars = 1u;
    } else {
        const char *text = NULL;
        if (index == STR_SERIAL) {
            text = s_d.serial;
        } else if (index < STR_COUNT) {
            text = STRINGS[index];
        }
        if (text == NULL) {
            return NULL;
        }
        chars = strlen(text);
        if (chars > STRING_MAX_CHARS) {
            chars = STRING_MAX_CHARS;
        }
        for (size_t i = 0u; i < chars; ++i) {
            s_d.string[1u + i] = (uint16_t)(uint8_t)text[i];  // ASCII is UTF-16's first block
        }
    }
    // The header: the descriptor's type, and its length in bytes.
    s_d.string[0] = (uint16_t)(((uint16_t)TUSB_DESC_STRING << 8) | (uint16_t)(2u * chars + 2u));
    return s_d.string;
}

void tud_mount_cb(void)
{
    usb_disk_session_configured(&s_d.session, s_d.now_ms);
}

void tud_umount_cb(void)
{
    usb_disk_session_unconfigured(&s_d.session, s_d.now_ms);
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    usb_disk_session_suspended(&s_d.session, s_d.now_ms);
}

void tud_resume_cb(void)
{
    usb_disk_session_resumed(&s_d.session);
}

static bool present(uint8_t lun)
{
    if (s_d.active && usb_disk_session_medium_present(&s_d.session)) {
        return true;
    }
    (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
    return false;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    // SCSI's fixed-width fields, padded with spaces.
    memcpy(vendor_id, "HAC     ", 8u);
    memcpy(product_id, "microSD card    ", 16u);
    memcpy(product_rev, "1.0 ", 4u);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return present(lun);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_d.sectors;
    *block_size = (uint16_t)s_d.sector_bytes;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    usb_disk_session_start_stop(&s_d.session, start, load_eject);
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

// The sectors a READ(10) or WRITE(10) piece covers, or 0 when it is not
// whole sectors: TinyUSB hands this callback at most
// USB_DISK_MSC_BUFFER_BYTES, a whole number of sectors, from offset 0.
static uint32_t whole_sectors(uint8_t lun, uint32_t offset, uint32_t bufsize)
{
    if (offset != 0u || bufsize == 0u || bufsize % s_d.sector_bytes != 0u) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0x00);
        return 0u;
    }
    return bufsize / s_d.sector_bytes;
}

// A transfer the card failed. One it no longer answers ends the session.
static void card_failed(uint8_t lun, hal_status_t status, uint8_t asc)
{
    s_d.stats.io_errors++;
    if (status == HAL_ERR_NOT_READY) {
        usb_disk_session_card_failed(&s_d.session);
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return;
    }
    const uint8_t key =
        (status == HAL_ERR_INVALID_ARG) ? SCSI_SENSE_ILLEGAL_REQUEST : SCSI_SENSE_MEDIUM_ERROR;
    (void)tud_msc_set_sense(lun, key, asc, 0x00);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize)
{
    if (!present(lun)) {
        s_d.stats.refused++;
        return TUD_MSC_RET_ERROR;
    }
    const uint32_t count = whole_sectors(lun, offset, bufsize);
    if (count == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    const int64_t start = esp_timer_get_time();
    const hal_status_t status = adp_sd_lent_read(s_d.sd, lba, buffer, count);
    const uint32_t took = elapsed_us(start);
    if (took > s_d.stats.slowest_read_us) {
        s_d.stats.slowest_read_us = took;
    }
    if (status != HAL_OK) {
        card_failed(lun, status, ASC_UNRECOVERED_READ);
        return TUD_MSC_RET_ERROR;
    }
    s_d.stats.bytes_read += bufsize;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    if (!present(lun)) {
        s_d.stats.refused++;
        return TUD_MSC_RET_ERROR;
    }
    const uint32_t count = whole_sectors(lun, offset, bufsize);
    if (count == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    const int64_t start = esp_timer_get_time();
    const hal_status_t status = adp_sd_lent_write(s_d.sd, lba, buffer, count);
    const uint32_t took = elapsed_us(start);
    if (took > s_d.stats.slowest_write_us) {
        s_d.stats.slowest_write_us = took;
    }
    if (status != HAL_OK) {
        card_failed(lun, status, ASC_WRITE_ERROR);
        return TUD_MSC_RET_ERROR;
    }
    s_d.stats.bytes_written += bufsize;
    return (int32_t)bufsize;
}

// What TinyUSB does not answer itself. SYNCHRONIZE CACHE, which hosts send
// before an eject, has nothing to do: every write is on the card when its
// callback returns.
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (scsi_cmd[0] == SCSI_SYNCHRONIZE_CACHE_10) {
        return 0;
    }
    (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0x00);
    return TUD_MSC_RET_ERROR;
}
