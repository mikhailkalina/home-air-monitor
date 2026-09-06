// SPDX-License-Identifier: Apache-2.0
//
// A minimal, dependency-free grayscale PNG writer.
//
// Two callers: adp_display_png, which is the headless simulator's whole
// output, and the golden tests, which use it only to dump a mismatch for a
// human to look at -- a golden comparison is over raw bytes and needs no image
// library at all.
//
// It lives under platform/host because writing files is a platform concern;
// core/ has no stdio and never sees it.

#ifndef HAC_PLATFORM_HOST_PNG_WRITE_H
#define HAC_PLATFORM_HOST_PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/framebuffer.h"

// Writes an 8-bit grayscale PNG to `path`. `pixels` holds width * height
// bytes, row-major, one byte per pixel (0 = black, 255 = white). The IDAT
// stream uses uncompressed ("stored") deflate blocks: valid PNG, just larger
// than a compressed encoder would produce, which is fine for occasional
// diagnostic dumps of small test images.
bool png_write_gray8(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height);

// Unpacks `fb` to 8-bit greyscale and writes it as a PNG to `path`: GRAY4
// expands each 4-bit level to 8-bit grey (level * 17, so 0 maps to 0 and 15
// to 255), MONO1 becomes black and white. This is the one place that
// unpacking happens -- every caller that turns a framebuffer_t into a PNG
// (a golden reference, a golden mismatch dump, a headless frame) goes through
// it, so a packing bug shows up in the PNG instead of being hidden by a
// second, independent decoder.
bool png_write_framebuffer_gray8(const framebuffer_t *fb, const char *path);

#endif  // HAC_PLATFORM_HOST_PNG_WRITE_H
