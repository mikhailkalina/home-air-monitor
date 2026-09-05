// SPDX-License-Identifier: Apache-2.0
//
// DejaVu Sans, rasterized to the font_t format at two sizes. DejaVu is a
// permissively licensed (Bitstream Vera-derived) typeface; the source TTF and
// its license are vendored under third_party/fonts/dejavu-sans/, and the
// dependency is recorded in THIRD_PARTY_NOTICES.md.
//
// Both cover the printable ASCII range 0x20-0x7E only. Regenerate with
// apps/tools/font_gen/generate_fonts.py.

#ifndef HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H
#define HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H

#include "ui/fonts/font.h"

#ifdef __cplusplus
extern "C" {
#endif

// Small: status lines and labels.
extern const font_t font_dejavu_sans_16;

// Large: headline readings (CO2 ppm, temperature, ...).
extern const font_t font_dejavu_sans_32;

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_FONTS_FONT_DEJAVU_SANS_H
