// SPDX-License-Identifier: Apache-2.0
//
// A read-only picture: the source side of gfx_blit_image(), as framebuffer_t
// is the destination side. The packing and row layout are framebuffer.h's own
// -- for PIXFMT_GRAY4 the even pixel of a row in the low nibble and the odd
// one in the high (docs/adr/0003-gray4-nibble-order.md) -- so a picture lands
// in a framebuffer one memcpy per row, with no conversion.
//
// The core never allocates or decodes one. Pictures are const data compiled
// into the application, emitted from panel-ready PNGs by
// apps/tools/image_gen/image_gen.py (docs/adr/0012-image-pipeline.md).

#ifndef HAC_CORE_UI_IMAGE_H
#define HAC_CORE_UI_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "ui/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *pixels;  // `stride` * `height` bytes, never written through
    uint16_t width;
    uint16_t height;
    size_t stride;  // bytes per row, each row padded to a whole byte
    pixel_format_t format;
} image_t;

#ifdef __cplusplus
}
#endif

#endif  // HAC_CORE_UI_IMAGE_H
