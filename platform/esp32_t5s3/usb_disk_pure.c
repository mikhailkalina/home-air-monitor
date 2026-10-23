// SPDX-License-Identifier: Apache-2.0

#include "usb_disk_pure.h"

#include <string.h>

usb_disk_session_config_t usb_disk_session_config_default(void)
{
    const usb_disk_session_config_t cfg = {
        .host_wait_ms = USB_DISK_HOST_WAIT_MS,
        .host_gone_ms = USB_DISK_HOST_GONE_MS,
        .suspend_ms = USB_DISK_SUSPEND_MS,
    };
    return cfg;
}

void usb_disk_session_start(usb_disk_session_t *s, const usb_disk_session_config_t *cfg,
                            uint64_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->started_ms = now_ms;
}

void usb_disk_session_configured(usb_disk_session_t *s, uint64_t now_ms)
{
    (void)now_ms;
    s->configured = true;
    s->ever_configured = true;
    s->suspended = false;  // a configuration needs a bus that runs
    s->configurations++;
}

void usb_disk_session_unconfigured(usb_disk_session_t *s, uint64_t now_ms)
{
    if (!s->configured) {
        return;
    }
    s->configured = false;
    s->unconfigured_ms = now_ms;
}

void usb_disk_session_suspended(usb_disk_session_t *s, uint64_t now_ms)
{
    if (s->suspended) {
        return;
    }
    s->suspended = true;
    s->suspended_ms = now_ms;
}

void usb_disk_session_resumed(usb_disk_session_t *s)
{
    s->suspended = false;
}

void usb_disk_session_start_stop(usb_disk_session_t *s, bool start, bool load_eject)
{
    if (load_eject && !start) {
        s->ejected = true;
    }
}

void usb_disk_session_button(usb_disk_session_t *s)
{
    s->button = true;
}

void usb_disk_session_card_failed(usb_disk_session_t *s)
{
    s->card_failed = true;
}

bool usb_disk_session_medium_present(const usb_disk_session_t *s)
{
    return !s->ejected && !s->button && !s->card_failed;
}

static bool lasted(uint64_t since_ms, uint64_t now_ms, uint32_t span_ms)
{
    return now_ms >= since_ms && now_ms - since_ms >= span_ms;
}

usb_disk_verdict_t usb_disk_session_verdict(const usb_disk_session_t *s, uint64_t now_ms)
{
    if (s->card_failed) {
        return USB_DISK_BACK_CARD_FAILED;
    }
    if (s->button) {
        return USB_DISK_BACK_BUTTON;
    }
    if (s->ejected) {
        return USB_DISK_BACK_EJECTED;
    }
    if (!s->ever_configured) {
        return lasted(s->started_ms, now_ms, s->cfg.host_wait_ms) ? USB_DISK_BACK_NO_HOST
                                                                  : USB_DISK_KEEP;
    }
    if (!s->configured) {
        return lasted(s->unconfigured_ms, now_ms, s->cfg.host_gone_ms) ? USB_DISK_BACK_HOST_GONE
                                                                       : USB_DISK_KEEP;
    }
    if (s->suspended && lasted(s->suspended_ms, now_ms, s->cfg.suspend_ms)) {
        return USB_DISK_BACK_SUSPENDED;
    }
    return USB_DISK_KEEP;
}

const char *usb_disk_verdict_name(usb_disk_verdict_t verdict)
{
    switch (verdict) {
        case USB_DISK_KEEP:
            return "keep";
        case USB_DISK_BACK_CARD_FAILED:
            return "card_failed";
        case USB_DISK_BACK_BUTTON:
            return "button";
        case USB_DISK_BACK_EJECTED:
            return "ejected";
        case USB_DISK_BACK_NO_HOST:
            return "no_host";
        case USB_DISK_BACK_HOST_GONE:
            return "host_gone";
        case USB_DISK_BACK_SUSPENDED:
            return "suspended";
        default:
            return "?";
    }
}

// --- the button ----------------------------------------------------------------------

void usb_disk_button_init(usb_disk_button_t *b, uint32_t debounce_ms, uint32_t hold_ms)
{
    memset(b, 0, sizeof(*b));
    b->debounce_ms = debounce_ms;
    b->hold_ms = hold_ms;
}

usb_disk_button_event_t usb_disk_button_sample(usb_disk_button_t *b, bool raw_pressed,
                                               uint64_t now_ms)
{
    if (raw_pressed != b->raw) {
        b->raw = raw_pressed;
        b->raw_since_ms = now_ms;
    }
    if (!lasted(b->raw_since_ms, now_ms, b->debounce_ms) || b->raw == b->pressed) {
        // A level not yet settled, or the one already taken: only a hold to
        // report, of a press that has lasted.
        if (b->pressed && !b->hold_reported && !b->ignore_until_release &&
            lasted(b->raw_since_ms, now_ms, b->hold_ms) && b->raw) {
            b->hold_reported = true;
            return USB_DISK_BUTTON_HOLD;
        }
        return USB_DISK_BUTTON_NONE;
    }

    b->pressed = b->raw;
    if (!b->pressed) {
        b->hold_reported = false;
        b->ignore_until_release = false;
        return USB_DISK_BUTTON_NONE;
    }
    return USB_DISK_BUTTON_PRESS;
}

void usb_disk_button_consume(usb_disk_button_t *b)
{
    b->ignore_until_release = b->pressed;
}
