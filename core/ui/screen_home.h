// SPDX-License-Identifier: Apache-2.0
//
// The MVP home screen of docs/requirements.md 14.4: CO2, temperature,
// humidity, pressure, IAQ, battery and link status, the local time of the last
// update, and the sensor error / stale indicators.
//
// It draws a view_model_t and nothing else. Any question of the form "what
// should this say when ..." is answered in core/ui/view_model.c, which is why
// the status logic can be unit tested without rendering a pixel.

#ifndef HAC_CORE_UI_SCREEN_HOME_H
#define HAC_CORE_UI_SCREEN_HOME_H

#include "ui/framebuffer.h"
#include "ui/view_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// The layout's fixed points, for the 960x540 panel. Public so that a test can
// check that nothing on the screen runs into anything else
// (tests/unit/test_screen_home_layout.c); nothing else should need them.
//
// Every text is set as large as the widest thing it can say allows: the CO2
// reading at 160px, because "40000" must still clear the band beside it --
// its unit and mark sit at fixed places on the label's line, "CO2 ppm", so a
// change in the reading's width moves nothing else (screen_home.c);
// the grid's readings at 72px, because "warm" must fit a quarter of the panel;
// the grid's labels at 32px, because "Temperature" must. Rules and
// separators are SCREEN_HOME_RULE_PX thick, enough to be seen at arm's length.
#define SCREEN_HOME_MARGIN 20
#define SCREEN_HOME_RULE_PX 3

#define SCREEN_HOME_BAR_TOP 0
#define SCREEN_HOME_BAR_RULE_Y 58
// The headline's first line, "CO2 ppm" and the CO2 mark: everything in it
// stays where it is whatever the reading below it says.
#define SCREEN_HOME_HEAD_LABEL_BASELINE 104
#define SCREEN_HOME_HEAD_RULE_Y 266
#define SCREEN_HOME_FOOT_RULE_Y 458

// The headline's band: SCREEN_HOME_BANNER_W wide, right-aligned at the margin.
#define SCREEN_HOME_BANNER_W 270
#define SCREEN_HOME_BANNER_Y 132
#define SCREEN_HOME_BANNER_H 112

// The grid: SCREEN_HOME_GRID_COLUMNS equal columns across the width inside the
// margins, a separator at each inner boundary from GRID_SEP_TOP to
// GRID_SEP_BOTTOM. A cell's text starts SCREEN_HOME_CELL_PAD right of its
// separator (the first one at the margin) and keeps SCREEN_HOME_CELL_GAP clear
// of the next.
#define SCREEN_HOME_GRID_COLUMNS 4
#define SCREEN_HOME_GRID_SEP_TOP 282
#define SCREEN_HOME_GRID_SEP_BOTTOM 442
#define SCREEN_HOME_CELL_PAD 14
#define SCREEN_HOME_CELL_GAP 12

// Repaints the whole of `fb`. The layout is tuned for the 960x540 panel but
// derives its columns from fb->width, and every primitive clips, so a smaller
// buffer renders a cropped screen rather than corrupting memory.
void screen_home_render(framebuffer_t *fb, const view_model_t *vm);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_SCREEN_HOME_H
