// SPDX-License-Identifier: Apache-2.0
//
// Every number the board's Wi-Fi station runs on (phase 4e,
// docs/adr/0032-wifi-on-the-board.md): the decisions' in adp_net_pure.h, the
// adapter's in adp_net.c. None is measured yet; each says what it is and
// which board check gives it a measured value (docs/hardware/board_notes.md,
// open item "Wi-Fi on the board").

#ifndef HAC_PLATFORM_ESP32_T5S3_ADP_NET_LIMITS_H
#define HAC_PLATFORM_ESP32_T5S3_ADP_NET_LIMITS_H

// CHOICE: the wait after a failed attempt starts at ADP_NET_BACKOFF_FIRST_MS
// and doubles to ADP_NET_BACKOFF_MAX_MS. An attempt is itself a scan of every
// channel -- about 2.5 s with the radio receiving -- so a ceiling of 30 s
// keeps an access point that is gone for hours down to a scan or two a
// minute, and a link back within 30 s of the access point's return. A link
// that was up and is lost is sought again at once, with no wait.
#define ADP_NET_BACKOFF_FIRST_MS 1000u
#define ADP_NET_BACKOFF_MAX_MS 30000u

// ESTIMATE: an attempt -- the scan, the association, the handshake and
// DHCP's address -- takes a few seconds on a home network; the radio's start
// (esp_wifi_start() and its STA_START event) takes well under one. Either
// given up past this. Generous on purpose: what it catches is an attempt that
// stopped answering, DHCP above all, which lwIP otherwise retries for ever.
// The board's `adp_net: up ... took_ms=` lines replace it: the first two,
// 2026-10-21, 6.7 and 6.9 s from up() -- the scan 2.4 s, association 1 s,
// DHCP 3.3 s (docs/hardware/board_notes.md, "Wi-Fi on the board").
#define ADP_NET_ATTEMPT_TIMEOUT_MS 30000u

// CHOICE: how often the adapter's task reads the signal strength while the
// link is up. rssi_dbm() returns what it read last, never asking the driver
// itself: the state message carries it once a minute at most
// (core/reporting/report_limits.h).
#define ADP_NET_RSSI_REFRESH_MS 10000u

// CHOICE: what the driver's events and the callers' requests wait in for
// the adapter's task. A connection is a handful of events; one that does not
// fit is dropped and counted, and the attempt's timeout covers it.
#define ADP_NET_QUEUE_LEN 16u

#endif  // HAC_PLATFORM_ESP32_T5S3_ADP_NET_LIMITS_H
