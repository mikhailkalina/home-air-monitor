// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics screen (docs/adr/0025-touch-input-gt911.md): a tap on the
// home screen brings it, a tap or a minute takes it away
// (core/app/screen_controller.h). It draws a diag_view_t
// (core/ui/diagnostics_view.h) and nothing else.
//
//   Diagnostics                               online    tap to return
//   ----------------------------------------------------------------------
//   scd4x        running      failures 0          restarts 0
//   bme68x       failing      failures 3          restarts 1
//   ----------------------------------------------------------------------
//   CO2          ok     -                from scd4x
//   Temperature  ok     -                from bme68x
//   ...                                                (six readings)
//   ----------------------------------------------------------------------
//   Wi-Fi  up      sim-ap        -61 dBm                   192.0.2.10
//   MQTT   online  sent 412      refused 0                 last 14:07
//   Clock: set (rtc)                                      Recording: on
//
// All text 32px, laid out in fixed columns wide enough for the widest thing
// each can say -- "restarting", "failures 999+", "unreliable",
// "Temperature", "joining", "sent 999k+", "refused 999+", "-127 dBm",
// "255.255.255.255", "last 15.10 23:58" -- measured off the font; a source's
// name and the network's name are shortened to their columns.
// tests/unit/test_screen_diagnostics_layout.c holds the worst cases to that
// by pixels. Trouble -- a failing source, an error, an uncalibrated number,
// the link not up, no session, a refusal, a recorder that cannot write, a
// clock not set -- is black; what is well is grey, so the eye goes to what is
// wrong.
//
// The network's two lines (docs/adr/0031-the-network-on-the-screen.md) took
// the room of the title's larger type: the screen holds thirteen lines of
// 32px text -- four sources at most, six readings, the network's two and the
// clock's -- and the title is 32px as its hint always was. Beside the hint
// goes the status bar's word for the link, held as the bar holds it: the
// network's lines are as of the drawing, and this word is what redraws them.

#ifndef HAC_CORE_UI_SCREEN_DIAGNOSTICS_H
#define HAC_CORE_UI_SCREEN_DIAGNOSTICS_H

#include "ui/diagnostics_view.h"
#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// The layout's fixed points, for the 960x540 panel; public for the layout
// test. Rules are SCREEN_DIAG_RULE_PX thick.
#define SCREEN_DIAG_MARGIN 20
#define SCREEN_DIAG_RULE_PX 3

// A line of 32px text is 24 px above its baseline and 7 below; SCREEN_DIAG_PITCH
// apart they keep 5 px clear. A rule keeps 3 px clear of the text above and
// below it.
#define SCREEN_DIAG_PITCH 36

#define SCREEN_DIAG_TITLE_BASELINE 32  // the title, the link's word and the hint
#define SCREEN_DIAG_TITLE_RULE_Y 43
// What the link's word keeps clear of the hint, right of it.
#define SCREEN_DIAG_TITLE_GAP 28

#define SCREEN_DIAG_SLOTS_BASELINE 73  // the first source's line; up to DIAG_MAX_SLOTS
#define SCREEN_DIAG_SLOTS_RULE_Y 192

#define SCREEN_DIAG_FIELDS_BASELINE 222  // the first reading's line; DIAG_FIELD_COUNT of them
#define SCREEN_DIAG_FIELDS_RULE_Y 413

#define SCREEN_DIAG_NET_BASELINE 443  // the Wi-Fi line; the MQTT line a pitch below
#define SCREEN_DIAG_FOOT_BASELINE 515

// The columns, as x of each text's left end.
#define SCREEN_DIAG_SLOT_NAME_X SCREEN_DIAG_MARGIN
#define SCREEN_DIAG_SLOT_STATE_X 262
#define SCREEN_DIAG_SLOT_FAILURES_X 440
#define SCREEN_DIAG_SLOT_RESTARTS_X 690

#define SCREEN_DIAG_FIELD_LABEL_X SCREEN_DIAG_MARGIN
#define SCREEN_DIAG_FIELD_STATUS_X 250
#define SCREEN_DIAG_FIELD_ACCURACY_X 360
#define SCREEN_DIAG_FIELD_SOURCE_X 620

// The network's lines: the label, the state, the network's name or the
// messages sent, the signal or the refusals; the address or the time of the
// last message right-aligned at the margin.
#define SCREEN_DIAG_NET_LABEL_X SCREEN_DIAG_MARGIN
#define SCREEN_DIAG_NET_STATE_X 130
#define SCREEN_DIAG_NET_DETAIL_X 250
#define SCREEN_DIAG_NET_COUNT_X 452

// What a column's text keeps clear of the next column.
#define SCREEN_DIAG_COLUMN_GAP 14

// Repaints the whole of `fb`. Laid out for 960x540; every primitive clips,
// so a smaller buffer renders a cropped screen rather than corrupting memory.
void screen_diagnostics_render(framebuffer_t *fb, const diag_view_t *v);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_SCREEN_DIAGNOSTICS_H
