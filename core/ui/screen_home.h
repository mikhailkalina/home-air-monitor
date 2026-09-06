// SPDX-License-Identifier: Apache-2.0
//
// The MVP home screen of docs/requirements.md 14.4: CO2, temperature,
// humidity, pressure, IAQ, battery and link status, the time since the last
// reading, and the sensor error / stale indicators.
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

// Repaints the whole of `fb`. The layout is tuned for the 960x540 panel but
// derives its columns from fb->width, and every primitive clips, so a smaller
// buffer renders a cropped screen rather than corrupting memory.
void screen_home_render(framebuffer_t *fb, const view_model_t *vm);

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_SCREEN_HOME_H
