#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Regenerates core/ui/fonts/font_dejavu_sans_*.generated.c from the vendored
# DejaVu Sans TTF. Not part of the CMake build: run it by hand after changing
# the font, the sizes, or the covered codepoint range, and commit the result.
#
# Requires Pillow: pip install pillow
#
# Usage:
#   python apps/tools/font_gen/generate_fonts.py
#
# The source TTF and its license live in third_party/fonts/dejavu-sans/. Only
# the printable ASCII range (0x20-0x7E) is rasterized; core/ui/gfx.c skips any
# codepoint outside a font's covered range.

import datetime
import pathlib
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[3]
TTF_PATH = ROOT / "third_party" / "fonts" / "dejavu-sans" / "DejaVuSans.ttf"
OUT_DIR = ROOT / "core" / "ui" / "fonts"

FIRST_CODEPOINT = 0x20
LAST_CODEPOINT = 0x7E
SIZES = (16, 32, 64)

# Large enough that no glyph at any generated size gets clipped by the canvas
# itself; only the font's own ink is measured back out via getbbox().
CANVAS = 200
PEN = 100
THRESHOLD = 128


def rasterize_glyph(font, ch):
    img = Image.new("L", (CANVAS, CANVAS), 0)
    draw = ImageDraw.Draw(img)
    draw.text((PEN, PEN), ch, font=font, fill=255, anchor="ls")
    bbox = img.getbbox()
    advance = round(draw.textlength(ch, font=font))

    if bbox is None:
        # No ink (space and similar): a zero-sized bitmap that still advances
        # the cursor.
        return {
            "width": 0,
            "height": 0,
            "x_offset": 0,
            "y_offset": 0,
            "advance": advance,
            "rows": [],
        }

    x0, y0, x1, y1 = bbox
    width = x1 - x0
    height = y1 - y0
    cropped = img.crop(bbox)

    rows = []
    row_bytes = (width + 7) // 8
    for y in range(height):
        row = bytearray(row_bytes)
        for x in range(width):
            if cropped.getpixel((x, y)) >= THRESHOLD:
                row[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(row))

    return {
        "width": width,
        "height": height,
        "x_offset": x0 - PEN,
        "y_offset": y0 - PEN,
        "advance": advance,
        "rows": rows,
    }


def emit_font(size):
    font = ImageFont.truetype(str(TTF_PATH), size)
    ascent, descent = font.getmetrics()

    glyphs = []
    bitmap = bytearray()
    for codepoint in range(FIRST_CODEPOINT, LAST_CODEPOINT + 1):
        g = rasterize_glyph(font, chr(codepoint))

        assert 0 <= g["width"] <= 255 and 0 <= g["height"] <= 255
        assert -128 <= g["x_offset"] <= 127 and -128 <= g["y_offset"] <= 127
        assert 0 <= g["advance"] <= 255
        assert len(bitmap) <= 0xFFFF

        glyphs.append((codepoint, g, len(bitmap)))
        for row in g["rows"]:
            bitmap.extend(row)

    assert len(bitmap) <= 0xFFFF, "bitmap_offset no longer fits uint16_t"

    name = f"font_dejavu_sans_{size}"
    lines = []
    lines.append("// SPDX-License-Identifier: Apache-2.0")
    lines.append("//")
    lines.append(f"// GENERATED FILE - do not edit by hand.")
    lines.append(f"// Produced by apps/tools/font_gen/generate_fonts.py on "
                 f"{datetime.date.today().isoformat()}")
    lines.append("// from third_party/fonts/dejavu-sans/DejaVuSans.ttf (Bitstream Vera")
    lines.append("// license; see third_party/fonts/dejavu-sans/LICENSE), rasterized at "
                 f"{size}px.")
    lines.append("// Re-run the generator and commit the result to change it.")
    lines.append("")
    # Paths are relative to the core/ include root (see cmake/sources_core.cmake),
    # matching the convention every other core/ source file uses.
    lines.append('#include "ui/fonts/font.h"')
    lines.append('#include "ui/fonts/font_dejavu_sans.h"')
    lines.append("")

    if bitmap:
        lines.append(f"static const uint8_t k_bitmap_data[{len(bitmap)}] = {{")
        for i in range(0, len(bitmap), 16):
            chunk = ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
    else:
        lines.append("static const uint8_t k_bitmap_data[1] = {0};  // unused; avoids a zero-length array")
    lines.append("")

    glyph_count = LAST_CODEPOINT - FIRST_CODEPOINT + 1
    lines.append(f"static const font_glyph_t k_glyphs[{glyph_count}] = {{")
    for codepoint, g, offset in glyphs:
        ch = chr(codepoint)
        comment = "' '" if ch == " " else ("'\\''" if ch == "'" else f"'{ch}'")
        lines.append(
            f"    {{.width = {g['width']}, .height = {g['height']}, "
            f".x_offset = {g['x_offset']}, .y_offset = {g['y_offset']}, "
            f".x_advance = {g['advance']}, .bitmap_offset = {offset}}},  // 0x{codepoint:02x} {comment}"
        )
    lines.append("};")
    lines.append("")

    lines.append(f"const font_t {name} = {{")
    lines.append(f'    .name = "DejaVu Sans {size}px",')
    lines.append(f"    .line_height = {ascent + descent},")
    lines.append(f"    .first_codepoint = 0x{FIRST_CODEPOINT:02x},")
    lines.append(f"    .glyph_count = {glyph_count},")
    lines.append("    .glyphs = k_glyphs,")
    lines.append("    .bitmap_data = k_bitmap_data,")
    lines.append("};")
    lines.append("")

    out_path = OUT_DIR / f"{name}.generated.c"
    out_path.write_text("\n".join(lines), encoding="ascii")
    print(f"wrote {out_path} ({len(bitmap)} bytes of bitmap data, "
          f"line_height={ascent + descent})")

    clang_format = shutil.which("clang-format")
    if clang_format:
        subprocess.run([clang_format, "-i", str(out_path)], check=True)
    else:
        print("  warning: clang-format not found on PATH; "
              "run it on the output before committing (CI enforces formatting)")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for size in SIZES:
        emit_font(size)


if __name__ == "__main__":
    main()
