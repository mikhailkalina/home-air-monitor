// SPDX-License-Identifier: Apache-2.0
//
// platform/esp32_t5s3/usb_disk_pure.c, compiled on the host from where the
// firmware compiles it (see tests/CMakeLists.txt): when the card's USB disk
// mode gives the card back, and the button that asks for it.

#include "usb_disk_pure.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hac_test.h"

#define T0 5000u

static usb_disk_session_t g_s;

static void started(void)
{
    const usb_disk_session_config_t cfg = usb_disk_session_config_default();
    usb_disk_session_start(&g_s, &cfg, T0);
}

// Started, and configured by a host at T0 + 1 s.
static void configured(void)
{
    started();
    usb_disk_session_configured(&g_s, T0 + 1000u);
}

static void the_defaults_are_the_limits_header(void)
{
    const usb_disk_session_config_t cfg = usb_disk_session_config_default();
    HAC_CHECK_EQ_U64(cfg.host_wait_ms, USB_DISK_HOST_WAIT_MS);
    HAC_CHECK_EQ_U64(cfg.host_gone_ms, USB_DISK_HOST_GONE_MS);
    HAC_CHECK_EQ_U64(cfg.suspend_ms, USB_DISK_SUSPEND_MS);
    // A re-enumeration must fit in the grace a gone host gets, and the
    // wait for a first host must be longer than that grace.
    HAC_CHECK(USB_DISK_HOST_GONE_MS < USB_DISK_HOST_WAIT_MS);
    HAC_CHECK(USB_DISK_BUTTON_DEBOUNCE_MS < USB_DISK_BUTTON_HOLD_MS);
    HAC_CHECK(USB_DISK_MSC_BUFFER_BYTES % 512u == 0u);
}

static void a_session_keeps_the_card_while_nothing_happens(void)
{
    started();
    HAC_CHECK(usb_disk_session_medium_present(&g_s));
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0), USB_DISK_KEEP);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_WAIT_MS - 1u),
                     USB_DISK_KEEP);
}

static void no_host_gives_it_back_at_the_wait_exactly(void)
{
    started();
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_WAIT_MS),
                     USB_DISK_BACK_NO_HOST);
    // A suspended bus with no host is the same: no host.
    started();
    usb_disk_session_suspended(&g_s, T0 + 10u);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_WAIT_MS - 1u),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_WAIT_MS),
                     USB_DISK_BACK_NO_HOST);
}

static void a_configured_host_keeps_it_past_the_wait(void)
{
    configured();
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 10u * USB_DISK_HOST_WAIT_MS),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_U64(g_s.configurations, 1u);
}

static void an_eject_gives_it_back_and_the_medium_is_gone_at_once(void)
{
    configured();
    usb_disk_session_start_stop(&g_s, false, true);
    HAC_CHECK(!usb_disk_session_medium_present(&g_s));
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 2000u), USB_DISK_BACK_EJECTED);
}

static void a_stop_without_eject_or_a_load_changes_nothing(void)
{
    configured();
    usb_disk_session_start_stop(&g_s, false, false);  // stop, the medium kept
    usb_disk_session_start_stop(&g_s, true, true);    // load
    usb_disk_session_start_stop(&g_s, true, false);   // start
    HAC_CHECK(usb_disk_session_medium_present(&g_s));
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 2000u), USB_DISK_KEEP);
}

static void a_host_gone_is_given_its_grace_for_a_reconfiguration(void)
{
    configured();
    const uint64_t gone = T0 + 60000u;
    usb_disk_session_unconfigured(&g_s, gone);
    HAC_CHECK(usb_disk_session_medium_present(&g_s));
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, gone + USB_DISK_HOST_GONE_MS - 1u),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, gone + USB_DISK_HOST_GONE_MS),
                     USB_DISK_BACK_HOST_GONE);

    // A bus reset and a new configuration within the grace: kept.
    configured();
    usb_disk_session_unconfigured(&g_s, gone);
    usb_disk_session_configured(&g_s, gone + 200u);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, gone + 10u * USB_DISK_HOST_GONE_MS),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_U64(g_s.configurations, 2u);
}

static void an_unconfiguration_before_any_configuration_is_still_no_host(void)
{
    started();
    usb_disk_session_unconfigured(&g_s, T0 + 100u);  // a bus reset in enumeration
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_GONE_MS + 100u),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + USB_DISK_HOST_WAIT_MS),
                     USB_DISK_BACK_NO_HOST);
}

static void a_long_suspend_gives_it_back_and_a_resume_cancels_it(void)
{
    configured();
    const uint64_t asleep = T0 + 30000u;
    usb_disk_session_suspended(&g_s, asleep);
    // A second suspend report does not restart the count.
    usb_disk_session_suspended(&g_s, asleep + 5000u);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, asleep + USB_DISK_SUSPEND_MS - 1u),
                     USB_DISK_KEEP);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, asleep + USB_DISK_SUSPEND_MS),
                     USB_DISK_BACK_SUSPENDED);

    configured();
    usb_disk_session_suspended(&g_s, asleep);
    usb_disk_session_resumed(&g_s);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, asleep + 10u * USB_DISK_SUSPEND_MS),
                     USB_DISK_KEEP);
}

static void a_configuration_ends_a_suspend(void)
{
    configured();
    usb_disk_session_suspended(&g_s, T0 + 2000u);
    usb_disk_session_unconfigured(&g_s, T0 + 2100u);
    usb_disk_session_configured(&g_s, T0 + 2200u);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 2000u + 10u * USB_DISK_SUSPEND_MS),
                     USB_DISK_KEEP);
}

static void the_button_and_a_failed_card_win_over_everything(void)
{
    configured();
    usb_disk_session_start_stop(&g_s, false, true);
    usb_disk_session_button(&g_s);
    HAC_CHECK(!usb_disk_session_medium_present(&g_s));
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 2000u), USB_DISK_BACK_BUTTON);
    usb_disk_session_card_failed(&g_s);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 2000u), USB_DISK_BACK_CARD_FAILED);

    // ... and the button with no host yet, before the wait is out.
    started();
    usb_disk_session_button(&g_s);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 1u), USB_DISK_BACK_BUTTON);
}

static void a_clock_that_went_back_never_gives_it_back(void)
{
    configured();
    usb_disk_session_unconfigured(&g_s, T0 + 50000u);
    HAC_CHECK_EQ_INT(usb_disk_session_verdict(&g_s, T0 + 10u), USB_DISK_KEEP);
}

static void verdicts_have_names(void)
{
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_KEEP), "keep") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_CARD_FAILED), "card_failed") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_BUTTON), "button") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_EJECTED), "ejected") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_NO_HOST), "no_host") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_HOST_GONE), "host_gone") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name(USB_DISK_BACK_SUSPENDED), "suspended") == 0);
    HAC_CHECK(strcmp(usb_disk_verdict_name((usb_disk_verdict_t)99), "?") == 0);
}

// --- the button ----------------------------------------------------------------------

#define DEB USB_DISK_BUTTON_DEBOUNCE_MS
#define HOLD USB_DISK_BUTTON_HOLD_MS
#define TICK 250u  // the card's task samples every CARD_WRITER_TICK_MS

static usb_disk_button_t g_b;

static void button(void)
{
    usb_disk_button_init(&g_b, DEB, HOLD);
}

static usb_disk_button_event_t at(bool pressed, uint64_t now)
{
    return usb_disk_button_sample(&g_b, pressed, now);
}

static void a_press_is_reported_once_it_has_settled(void)
{
    button();
    HAC_CHECK_EQ_INT(at(false, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + TICK), USB_DISK_BUTTON_NONE);  // first seen
    HAC_CHECK_EQ_INT(at(true, T0 + TICK + DEB - 1u), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + TICK + DEB), USB_DISK_BUTTON_PRESS);
    HAC_CHECK_EQ_INT(at(true, T0 + TICK + DEB + 1u), USB_DISK_BUTTON_NONE);  // once
}

static void a_bounce_shorter_than_the_debounce_is_nothing(void)
{
    button();
    HAC_CHECK_EQ_INT(at(true, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(false, T0 + DEB - 1u), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(false, T0 + 10u * DEB), USB_DISK_BUTTON_NONE);
    HAC_CHECK(!g_b.pressed);
}

static void a_hold_is_reported_once_at_its_length_from_the_press(void)
{
    button();
    HAC_CHECK_EQ_INT(at(true, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + TICK), USB_DISK_BUTTON_PRESS);
    uint64_t t = T0 + TICK;
    while (t + TICK < T0 + HOLD) {
        t += TICK;
        HAC_CHECK_EQ_INT(at(true, t), USB_DISK_BUTTON_NONE);
    }
    HAC_CHECK_EQ_INT(at(true, T0 + HOLD), USB_DISK_BUTTON_HOLD);
    HAC_CHECK_EQ_INT(at(true, T0 + 3u * HOLD), USB_DISK_BUTTON_NONE);  // once
    // Released, pressed again: a new press, and a new hold.
    HAC_CHECK_EQ_INT(at(false, T0 + 4u * HOLD), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(false, T0 + 4u * HOLD + DEB), USB_DISK_BUTTON_NONE);
    const uint64_t again = T0 + 5u * HOLD;
    HAC_CHECK_EQ_INT(at(true, again), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, again + DEB), USB_DISK_BUTTON_PRESS);
    HAC_CHECK_EQ_INT(at(true, again + HOLD), USB_DISK_BUTTON_HOLD);
}

static void a_sparse_sample_reports_the_press_before_the_hold(void)
{
    button();
    HAC_CHECK_EQ_INT(at(true, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + 2u * HOLD), USB_DISK_BUTTON_PRESS);
    HAC_CHECK_EQ_INT(at(true, T0 + 2u * HOLD + 1u), USB_DISK_BUTTON_HOLD);
}

static void a_consumed_press_reports_no_hold_and_the_next_press_does(void)
{
    button();
    HAC_CHECK_EQ_INT(at(true, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + DEB), USB_DISK_BUTTON_PRESS);
    usb_disk_button_consume(&g_b);  // the press gave the card back
    HAC_CHECK_EQ_INT(at(true, T0 + 2u * HOLD), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(false, T0 + 3u * HOLD), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(false, T0 + 3u * HOLD + DEB), USB_DISK_BUTTON_NONE);
    const uint64_t again = T0 + 4u * HOLD;
    HAC_CHECK_EQ_INT(at(true, again), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, again + DEB), USB_DISK_BUTTON_PRESS);
    HAC_CHECK_EQ_INT(at(true, again + HOLD), USB_DISK_BUTTON_HOLD);
}

static void consuming_with_nothing_pressed_ignores_nothing(void)
{
    button();
    usb_disk_button_consume(&g_b);
    HAC_CHECK_EQ_INT(at(true, T0), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, T0 + DEB), USB_DISK_BUTTON_PRESS);
    HAC_CHECK_EQ_INT(at(true, T0 + HOLD), USB_DISK_BUTTON_HOLD);
}

static void a_button_held_from_boot_is_a_press(void)
{
    button();
    HAC_CHECK_EQ_INT(at(true, 0u), USB_DISK_BUTTON_NONE);
    HAC_CHECK_EQ_INT(at(true, DEB), USB_DISK_BUTTON_PRESS);
}

int main(void)
{
    HAC_RUN(the_defaults_are_the_limits_header);
    HAC_RUN(a_session_keeps_the_card_while_nothing_happens);
    HAC_RUN(no_host_gives_it_back_at_the_wait_exactly);
    HAC_RUN(a_configured_host_keeps_it_past_the_wait);
    HAC_RUN(an_eject_gives_it_back_and_the_medium_is_gone_at_once);
    HAC_RUN(a_stop_without_eject_or_a_load_changes_nothing);
    HAC_RUN(a_host_gone_is_given_its_grace_for_a_reconfiguration);
    HAC_RUN(an_unconfiguration_before_any_configuration_is_still_no_host);
    HAC_RUN(a_long_suspend_gives_it_back_and_a_resume_cancels_it);
    HAC_RUN(a_configuration_ends_a_suspend);
    HAC_RUN(the_button_and_a_failed_card_win_over_everything);
    HAC_RUN(a_clock_that_went_back_never_gives_it_back);
    HAC_RUN(verdicts_have_names);
    HAC_RUN(a_press_is_reported_once_it_has_settled);
    HAC_RUN(a_bounce_shorter_than_the_debounce_is_nothing);
    HAC_RUN(a_hold_is_reported_once_at_its_length_from_the_press);
    HAC_RUN(a_sparse_sample_reports_the_press_before_the_hold);
    HAC_RUN(a_consumed_press_reports_no_hold_and_the_next_press_does);
    HAC_RUN(consuming_with_nothing_pressed_ignores_nothing);
    HAC_RUN(a_button_held_from_boot_is_a_press);
    return hac_test_summary();
}
