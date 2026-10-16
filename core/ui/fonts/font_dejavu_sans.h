// SPDX-License-Identifier: Apache-2.0
//
// DejaVu Sans, rasterized to the font_t format at the sizes the screens use
// (core/ui/screen_home.c, apps/slideshow/). DejaVu is a permissively licensed (Bitstream
// Vera-derived) typeface; the source TTF and its license are vendored under
// third_party/fonts/dejavu-sans/, and the dependency is recorded in
// THIRD_PARTY_NOTICES.md.
//
// All cover the printable ASCII range 0x20-0x7E only. Regenerate with
// apps/tools/font_gen/generate_fonts.py.

#ifndef HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H
#define HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H

#include "ui/fonts/font.h"

#ifdef __cplusplus
extern "C" {
#endif

// Small: the slideshow's caption and the test card.
extern const font_t font_dejavu_sans_16;

// The grid's labels and units.
extern const font_t font_dejavu_sans_32;

// The status bar, the footer and the headline's label.
extern const font_t font_dejavu_sans_40;

// The CO2 unit and the CO2 band.
extern const font_t font_dejavu_sans_48;

// The grid's readings: the largest size at which the widest of them ("warm")
// still fits a quarter of the 960px panel.
extern const font_t font_dejavu_sans_72;

// The headline CO2 reading, to be read from across the room: the largest
// size at which "40000 ppm" still clears the band beside it.
extern const font_t font_dejavu_sans_160;

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H
