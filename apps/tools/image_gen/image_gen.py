#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Pictures for the ED047TC1's sixteen greys, in two stages that meet at a
# "panel-ready" PNG (docs/adr/0012-image-pipeline.md):
#
#   prepare  photo(s) -> panel-ready PNG(s)   needs Pillow; run by hand
#   demo     synthetic panel-ready PNGs        standard library only
#   embed    panel-ready PNG(s) -> C source    standard library only; the
#                                               build runs this one
#   levels   how light each level is on the glass, as `prepare` assumes
#
# A panel-ready PNG is a 4-bit greyscale PNG in which every pixel already is
# one of the panel's levels. It is exactly what the glass will be driven
# with, it opens in any viewer, and GitHub shows it in a review. Everything
# lossy -- scaling, cropping, tone, dithering -- happens in `prepare`, once,
# where the result can be looked at -- dithering in light, towards how light
# each level measured on the glass (docs/adr/0013-grey-level-lightness.md).
# `embed` only repacks, losslessly, and
# refuses any image that is not already panel-ready, so a build never makes a
# picture decision of its own.
#
# Usage:
#   python3 apps/tools/image_gen/image_gen.py prepare ~/Pictures/*.jpg \
#       --out-dir ~/slideshow --dither floyd-steinberg
#   python3 apps/tools/image_gen/image_gen.py demo --out-dir apps/slideshow/images
#   python3 apps/tools/image_gen/image_gen.py embed apps/slideshow/images/*.png \
#       --name slideshow_images --out-dir build/gen
#
# `prepare` needs Pillow (pip install pillow, or apt install python3-pil);
# nothing else here does, which is why the ESP-IDF build -- whose Python
# environment has no Pillow -- can run `embed` itself.

import argparse
import pathlib
import random
import re
import struct
import sys
import zlib

# --- the panel --------------------------------------------------------------

# The default target size: the ED047TC1 in the orientation the driver
# presents (docs/hardware/board_notes.md, "Display orientation"). Only a
# default -- embed accepts any size, and the slideshow centres or crops.
PANEL_SIZE = (960, 540)

LEVEL_COUNT = 16

# How a panel level is written into a panel-ready PNG: level L as 8-bit grey
# L * 17, so the file opens as an ordinary greyscale image. An encoding only;
# how light a level looks on the glass is panel_lightness()'s business.
LEVEL_CODE = [level * 17 for level in range(LEVEL_COUNT)]

GENERATOR = "apps/tools/image_gen/image_gen.py"

ROOT = pathlib.Path(__file__).resolve().parents[3]

# The simulator's table of how light each level is on the glass, read from
# where it is defined so the two can never disagree
# (docs/adr/0013-grey-level-lightness.md). That header says which levels are
# measured and which are estimates.
LIGHTNESS_HEADER = ROOT / "platform" / "host" / "epd_emulation.h"


def parse_size(text):
    match = re.fullmatch(r"(\d+)x(\d+)", text)
    if not match:
        raise argparse.ArgumentTypeError(f"expected WIDTHxHEIGHT, got {text!r}")
    width, height = int(match.group(1)), int(match.group(2))
    if not (0 < width <= 0xFFFF and 0 < height <= 0xFFFF):
        raise argparse.ArgumentTypeError(f"size out of range: {text}")
    return width, height


def panel_lightness():
    """How light each level is on the glass: 0.0 at the panel's black, 1.0 at
    its white, linear in light -- so a mix of two levels is as light as the
    average of theirs."""
    text = LIGHTNESS_HEADER.read_text()
    match = re.search(r"#define\s+EPD_SIM_LEVEL_LIGHTNESS_PERMILLE\b([^}]*)\}", text)
    if not match:
        sys.exit(f"{LIGHTNESS_HEADER}: EPD_SIM_LEVEL_LIGHTNESS_PERMILLE not found")
    body = match.group(1).replace("\\", " ").replace("{", " ")
    permille = [int(v) for v in body.split(",") if v.strip()]
    if (len(permille) != LEVEL_COUNT or permille[0] != 0 or permille[-1] != 1000
            or permille != sorted(permille)):
        sys.exit(f"{LIGHTNESS_HEADER}: EPD_SIM_LEVEL_LIGHTNESS_PERMILLE must be "
                 f"{LEVEL_COUNT} values rising from 0 to 1000, got {permille}")
    return [value / 1000 for value in permille]


def nominal_lightness():
    """How light each level would be if level L looked like 8-bit sRGB grey
    L * 17 -- except level 1, counted as black as the panel shows it
    (docs/adr/0008-panel-timing.md), so `--levels distinct` leaves it out.
    Since the full refresh darkens from white (docs/adr/0035), photographs
    dithered towards this looked better on the glass than towards the
    measured table, which was measured under the old GC16 full refresh."""
    light = [srgb_to_light(code) for code in LEVEL_CODE]
    light[1] = light[0]
    return light


def palette_levels(mode, lightness):
    """The levels a picture may be dithered to, darkest first. `distinct`
    leaves out any level that looks the same as a darker one -- level 1 on
    this unit -- since dithering towards it would spend error on a grey the
    panel does not show."""
    if mode == "all":
        return list(range(LEVEL_COUNT))
    levels = []
    for level in range(LEVEL_COUNT):
        if not levels or lightness[level] > lightness[levels[-1]]:
            levels.append(level)
    return levels


def srgb_to_light(code):
    """8-bit sRGB grey to light, 0.0-1.0."""
    v = code / 255
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def light_to_srgb(light):
    """Light, 0.0-1.0, to sRGB grey on a 0.0-1.0 scale."""
    return light * 12.92 if light <= 0.0031308 else 1.055 * light ** (1 / 2.4) - 0.055


# The demo's test pictures dither towards the nominal greys, L * 17 with level
# 1 left out, not towards the measured ones: they test the levels and the
# dithering themselves, and docs/adr/0012-image-pipeline.md's observations were
# made on them. Photographs go through `prepare`, which uses the measured table.
DEMO_LEVELS = [0] + list(range(2, LEVEL_COUNT))
DEMO_TARGETS = [LEVEL_CODE[level] for level in DEMO_LEVELS]


# --- quantizing and dithering -------------------------------------------------
#
# Pure Python over a flat, row-major list of grey values on a 0-255 scale
# (floats welcome), returning a bytearray of panel levels. `levels` lists the
# levels allowed, darkest first, and `targets` their greys on the values' own
# scale: 8-bit grey for the demo's test pictures, light for the photographs
# `prepare` makes. The error carried to the neighbours keeps its fraction:
# rounded away, it would lose the faint shadows of a picture dithered in light,
# where the whole step from level 0 to level 2 is 12 of the 255.
#
# Slow by image-processing standards -- a second or two per panel-sized
# picture -- and dependency-free, which is what lets `demo` run without Pillow
# and keeps every method in one place where they can be compared.

def _clamp(value):
    if value <= 0.0:
        return 0.0
    if value >= 255.0:
        return 255.0
    return value


def _nearest_table(targets):
    """For every whole value 0-255, the index of the nearest target."""
    return [min(range(len(targets)), key=lambda i: abs(targets[i] - value))
            for value in range(256)]


def quantize_nearest(values, width, height, levels, targets):
    nearest = _nearest_table(targets)
    return bytearray(levels[nearest[int(_clamp(v) + 0.5)]] for v in values)


def quantize_floyd_steinberg(values, width, height, levels, targets):
    # Serpentine: alternate rows run right to left, which breaks up the
    # diagonal "worms" a one-way scan leaves in smooth gradients.
    nearest = _nearest_table(targets)
    buf = [float(v) for v in values]
    out = bytearray(width * height)
    for y in range(height):
        forward = (y % 2) == 0
        step = 1 if forward else -1
        xs = range(width) if forward else range(width - 1, -1, -1)
        row = y * width
        below = row + width
        has_below = y + 1 < height
        for x in xs:
            i = row + x
            value = _clamp(buf[i])
            index = nearest[int(value + 0.5)]
            out[i] = levels[index]
            err = value - targets[index]
            if err == 0:
                continue
            ahead = x + step
            behind = x - step
            if 0 <= ahead < width:
                buf[row + ahead] += err * 7 / 16
            if has_below:
                if 0 <= behind < width:
                    buf[below + behind] += err * 3 / 16
                buf[below + x] += err * 5 / 16
                if 0 <= ahead < width:
                    buf[below + ahead] += err * 1 / 16
    return out


def quantize_atkinson(values, width, height, levels, targets):
    # Spreads only 6/8 of the error, to six neighbours. Crisper and more
    # contrasty than Floyd-Steinberg; loses detail in the deepest shadows and
    # brightest highlights, where the dropped quarter accumulates.
    nearest = _nearest_table(targets)
    buf = [float(v) for v in values]
    out = bytearray(width * height)
    neighbours = ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2))
    for y in range(height):
        row = y * width
        for x in range(width):
            i = row + x
            value = _clamp(buf[i])
            index = nearest[int(value + 0.5)]
            out[i] = levels[index]
            share = (value - targets[index]) / 8
            if share == 0:
                continue
            for dx, dy in neighbours:
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and ny < height:
                    buf[ny * width + nx] += share
    return out


def _bayer(order):
    matrix = [[0]]
    while len(matrix) < order:
        n = len(matrix)
        matrix = [
            [4 * matrix[y % n][x % n] + (0, 2, 3, 1)[(y // n) * 2 + (x // n)]
             for x in range(2 * n)]
            for y in range(2 * n)
        ]
    return matrix


def quantize_bayer(values, width, height, levels, targets):
    # Ordered 8x8: a fixed threshold pattern instead of carried error. A
    # regular cross-hatch rather than noise, no error creeping across edges,
    # and it compresses far better -- at the cost of a visible texture.
    bayer = _bayer(8)
    cells = len(bayer) * len(bayer)
    last = len(targets) - 1
    # For every whole value, the highest target at or below it; the value's
    # own fraction then says how far towards the next target it lies, so an
    # uneven palette dithers correctly.
    floor_index = [max(i for i in range(len(targets)) if targets[i] <= value)
                   for value in range(256)]
    out = bytearray(width * height)
    for y in range(height):
        thresholds = [(bayer[y % 8][x % 8] + 0.5) / cells for x in range(8)]
        row = y * width
        for x in range(width):
            value = _clamp(values[row + x])
            lo = floor_index[int(value)]
            hi = min(lo + 1, last)
            span = targets[hi] - targets[lo]
            t = 0.0 if span <= 0 else (value - targets[lo]) / span
            out[row + x] = levels[hi if t > thresholds[x % 8] else lo]
    return out


DITHERS = {
    "none": quantize_nearest,
    "bayer": quantize_bayer,
    "floyd-steinberg": quantize_floyd_steinberg,
    "atkinson": quantize_atkinson,
}


# --- PNG, written and read with the standard library -------------------------

def _chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _filter_row(kind, row, prev):
    if kind == 0:
        return row
    out = bytearray(len(row))
    for i, value in enumerate(row):
        left = row[i - 1] if i > 0 else 0
        up = prev[i]
        corner = prev[i - 1] if i > 0 else 0
        predictor = (0, left, up, (left + up) // 2, _paeth(left, up, corner))[kind]
        out[i] = (value - predictor) & 0xFF
    return out


def write_png_gray4(path, levels, width, height, note):
    """Writes encode_png_gray4()'s bytes to `path`."""
    pathlib.Path(path).write_bytes(encode_png_gray4(levels, width, height, note))


def encode_png_gray4(levels, width, height, note):
    """Encodes `levels` (one panel level per pixel, row-major) as a 4-bit
    greyscale PNG. PNG packs the leftmost pixel of each pair into the HIGH
    nibble -- the opposite of the framebuffer's order (docs/adr/0003) --
    which embed() undoes."""
    stride = (width + 1) // 2
    raw = bytearray()
    prev = bytearray(stride)
    for y in range(height):
        row = bytearray(stride)
        base = y * width
        for x in range(width):
            level = levels[base + x]
            if x % 2 == 0:
                row[x // 2] = level << 4
            else:
                row[x // 2] |= level
        # The usual heuristic: the filter whose output has the smallest sum
        # of absolute (signed) bytes compresses best.
        candidates = [(_filter_row(kind, row, prev), kind) for kind in range(5)]
        best, kind = min(candidates,
                         key=lambda c: sum(b if b < 128 else 256 - b for b in c[0]))
        raw.append(kind)
        raw.extend(best)
        prev = row
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 4, 0, 0, 0, 0))
    png += _chunk(b"tEXt", b"Software\x00" + f"{GENERATOR} {note}".encode("latin-1"))
    png += _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _chunk(b"IEND", b"")
    return bytes(png)


class NotPanelReady(Exception):
    pass


def read_png_grey(path):
    """Decodes a non-interlaced PNG to (width, height, values), one 8-bit grey
    value per pixel. Greyscale and palette images of any bit depth are read
    as they are; colour images only if every pixel is neutral grey, and any
    image only if it is fully opaque. Anything else raises NotPanelReady: an
    image that needs converting needs `prepare`, not a silent guess."""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise NotPanelReady("not a PNG file")
    pos = 8
    header = None
    palette = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            raise NotPanelReady("it has transparency (a tRNS chunk); flatten it first")
        elif kind == b"IDAT":
            idat.extend(body)
        elif kind == b"IEND":
            break
    if header is None:
        raise NotPanelReady("no IHDR chunk")
    width, height, depth, colour, _compression, _filter, interlace = header
    if interlace != 0:
        raise NotPanelReady("it is interlaced; re-save it without interlacing")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if channels is None:
        raise NotPanelReady(f"unknown PNG colour type {colour}")
    if colour == 3 and palette is None:
        raise NotPanelReady("palette image without a PLTE chunk")

    bits_per_pixel = channels * depth
    stride = (width * bits_per_pixel + 7) // 8
    bpp = max(1, bits_per_pixel // 8)
    raw = zlib.decompress(bytes(idat))
    if len(raw) != height * (stride + 1):
        raise NotPanelReady("image data has the wrong length")

    max_sample = (1 << depth) - 1
    values = []
    prev = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = row[i - bpp] if i >= bpp else 0
            up = prev[i]
            corner = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                row[i] = (row[i] + left) & 0xFF
            elif kind == 2:
                row[i] = (row[i] + up) & 0xFF
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                row[i] = (row[i] + _paeth(left, up, corner)) & 0xFF
            elif kind != 0:
                raise NotPanelReady(f"unknown filter type {kind} in row {y}")
        prev = row

        if depth == 16:
            samples = [(row[i] << 8) | row[i + 1] for i in range(0, len(row), 2)]
        elif depth == 8:
            samples = list(row)
        else:
            per_byte = 8 // depth
            samples = [(row[i // per_byte] >> (8 - depth * (i % per_byte + 1))) & max_sample
                       for i in range(width * channels)]

        for x in range(width):
            pixel = samples[x * channels:(x + 1) * channels]
            if colour == 3:
                if pixel[0] >= len(palette):
                    raise NotPanelReady(f"palette index out of range at ({x}, {y})")
                r, g, b = palette[pixel[0]]
                if not r == g == b:
                    raise NotPanelReady(f"palette entry {pixel[0]} is a colour, not a grey")
                values.append(r)
                continue
            if colour in (4, 6) and pixel[-1] != max_sample:
                raise NotPanelReady(f"pixel ({x}, {y}) is not fully opaque")
            if colour in (2, 6) and not pixel[0] == pixel[1] == pixel[2]:
                raise NotPanelReady(f"pixel ({x}, {y}) is a colour, not a grey")
            sample = pixel[0]
            # Every depth below 16 scales to 8 bits exactly (x255, x85, x17,
            # x1). A 16-bit sample may land between two 8-bit values, and is
            # then passed on as a fraction for embed to reject or round.
            if depth == 16:
                values.append(sample // 257 if sample % 257 == 0 else sample / 257)
            else:
                values.append(sample * 255 // max_sample)
    return width, height, values


# --- prepare -----------------------------------------------------------------

def parse_focus(text):
    match = re.fullmatch(r"([0-9.]+),([0-9.]+)", text)
    if not match:
        raise argparse.ArgumentTypeError(f"expected X,Y fractions such as 0.5,0.4, got {text!r}")
    x, y = float(match.group(1)), float(match.group(2))
    if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
        raise argparse.ArgumentTypeError(f"focus fractions must lie in 0..1, got {text}")
    return x, y


def rotate_focus(focus, degrees):
    """Where a point given as fractions of the upright picture lands, as
    fractions of the picture turned `degrees` clockwise."""
    x, y = focus
    return {0: (x, y), 90: (1 - y, x), 180: (1 - x, 1 - y), 270: (y, 1 - x)}[degrees]


def cover_box(width, height, target_w, target_h, focus):
    """The crop (left, top, right, bottom) of the largest target-shaped window
    inside a width x height picture, centred on `focus` (fractions of width
    and height) as nearly as the picture's edges allow."""
    ratio = target_w / target_h
    if width / height > ratio:
        crop_w, crop_h = height * ratio, float(height)
    else:
        crop_w, crop_h = float(width), width / ratio
    left = min(max(focus[0] * width - crop_w / 2, 0.0), width - crop_w)
    top = min(max(focus[1] * height - crop_h / 2, 0.0), height - crop_h)
    return (left, top, left + crop_w, top + crop_h)


def cmd_prepare(args):
    try:
        from PIL import Image, ImageFilter, ImageOps
    except ImportError:
        sys.exit("prepare needs Pillow: pip install pillow (or apt install python3-pil)")

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    target_w, target_h = args.size
    quantize = DITHERS[args.dither]

    # The picture is dithered in light, towards how light each level really
    # is on the glass (docs/adr/0013-grey-level-lightness.md): its 8-bit greys
    # are decoded as sRGB, and the panel's black and white stand for the
    # picture's. `--lightness nominal` dithers towards L * 17 instead.
    lightness = panel_lightness() if args.lightness == "measured" else nominal_lightness()
    levels = palette_levels(args.levels, lightness)
    targets = [255 * lightness[level] for level in levels]
    to_light = [255 * srgb_to_light(code) for code in range(256)]
    background_light = 255 * lightness[args.background]
    background = round(255 * light_to_srgb(lightness[args.background]))

    for source in args.inputs:
        source = pathlib.Path(source)
        out_path = out_dir / f"{source.stem}{args.suffix}.png"
        if out_path.resolve() == source.resolve():
            sys.exit(f"{source}: refusing to overwrite the input; choose another --out-dir")

        image = ImageOps.exif_transpose(Image.open(source))
        # Transparency is flattened onto the background level, which is what
        # the bars around a `contain` fit are filled with too.
        if image.mode in ("RGBA", "LA", "PA") or "transparency" in image.info:
            image = image.convert("RGBA")
            flat = Image.new("RGBA", image.size, (background, background, background, 255))
            image = Image.alpha_composite(flat, image)
        image = image.convert("L")
        if args.rotate:
            image = image.rotate(-args.rotate, expand=True)  # clockwise

        if args.fit == "cover":
            box = cover_box(image.width, image.height, target_w, target_h,
                            rotate_focus(args.focus, args.rotate))
            image = image.resize((target_w, target_h), Image.Resampling.LANCZOS, box=box)
        elif args.fit == "contain":
            image = ImageOps.contain(image, (target_w, target_h), Image.Resampling.LANCZOS)
        else:
            image = image.resize((target_w, target_h), Image.Resampling.LANCZOS)

        # Tone before padding, so the bars of a `contain` fit do not count
        # towards the histogram autocontrast stretches.
        if args.autocontrast > 0:
            image = ImageOps.autocontrast(image, cutoff=args.autocontrast)
        if args.gamma != 1.0:
            lut = [round(255 * (v / 255) ** (1 / args.gamma)) for v in range(256)]
            image = image.point(lut)
        if args.sharpen > 0:
            image = image.filter(ImageFilter.UnsharpMask(radius=1.0, percent=args.sharpen,
                                                         threshold=0))

        canvas = Image.new("L", (target_w, target_h), background)
        left, top = (target_w - image.width) // 2, (target_h - image.height) // 2
        canvas.paste(image, (left, top))

        if args.noise > 0:
            # Triangular noise, seeded from the file name so a re-run makes the
            # same picture: error diffusion draws thin contours where a smooth
            # tone crosses a level, and a little noise blurs the crossings
            # (docs/adr/0013-grey-level-lightness.md).
            rng = random.Random(zlib.crc32(source.name.encode()))
            light = [255 * srgb_to_light(min(255.0, max(0.0, code + args.noise
                                                          * (rng.random() - rng.random()))))
                     for code in canvas.tobytes()]
        else:
            light = [to_light[code] for code in canvas.tobytes()]
        # The bars around a `contain` fit are exactly the background level,
        # not whatever its nearest sRGB grey dithers to.
        for y in range(target_h):
            inside_rows = top <= y < top + image.height
            for x in range(target_w):
                if not (inside_rows and left <= x < left + image.width):
                    light[y * target_w + x] = background_light

        panel = quantize(light, target_w, target_h, levels, targets)
        note = (f"prepare {source.name} size={target_w}x{target_h} fit={args.fit} "
                f"focus={args.focus[0]:g},{args.focus[1]:g} rotate={args.rotate} "
                f"dither={args.dither} levels={args.levels} gamma={args.gamma} "
                f"autocontrast={args.autocontrast} sharpen={args.sharpen} noise={args.noise} "
                f"lightness={'/'.join(str(round(1000 * v)) for v in lightness)}")
        write_png_gray4(out_path, panel, target_w, target_h, note)
        print(f"wrote {out_path} ({target_w}x{target_h}, {args.dither})")


# --- demo --------------------------------------------------------------------
#
# Synthetic test pictures, each aimed at one question about the panel. They
# are made here rather than taken from anywhere, so they carry this
# project's license and no one else's.

def _canvas(width, height, value):
    return [value] * (width * height)


def _fill(values, width, x0, y0, x1, y1, value):
    for y in range(y0, y1):
        base = y * width
        for x in range(x0, x1):
            values[base + x] = value


def demo_grey_ramp(width, height):
    """All sixteen levels as bars, dark to light across the top three
    quarters and light to dark along the bottom: which neighbours can be
    told apart, in both directions."""
    levels = bytearray(width * height)
    split = height * 3 // 4
    for x in range(width):
        level = min(LEVEL_COUNT - 1, x * LEVEL_COUNT // width)
        for y in range(height):
            levels[y * width + x] = level if y < split else LEVEL_COUNT - 1 - level
    return levels


def demo_dither_methods(width, height):
    """The same black-to-white gradient four times, top to bottom: none,
    bayer, floyd-steinberg, atkinson, each framed in black so its white end
    shows against the page. The left margin carries one to four black
    squares naming the band."""
    margin, gap = 48, 12
    band_h = (height - gap * 5) // 4
    out = bytearray([LEVEL_COUNT - 1]) * (width * height)
    gradient_w = width - margin - gap
    for band, method in enumerate(("none", "bayer", "floyd-steinberg", "atkinson")):
        top = gap + band * (band_h + gap)
        ramp = [255 * x / (gradient_w - 1) for x in range(gradient_w)] * band_h
        quantized = DITHERS[method](ramp, gradient_w, band_h, DEMO_LEVELS, DEMO_TARGETS)
        for y in range(band_h):
            row = (top + y) * width + margin
            out[row:row + gradient_w] = quantized[y * gradient_w:(y + 1) * gradient_w]
        for x in range(margin - 1, margin + gradient_w + 1):
            out[(top - 1) * width + x] = 0
            out[(top + band_h) * width + x] = 0
        for y in range(top - 1, top + band_h + 1):
            out[y * width + margin - 1] = 0
            out[y * width + margin + gradient_w] = 0
        for mark in range(band + 1):
            y0 = top + 8 + mark * 14
            for y in range(y0, y0 + 10):
                out[y * width + 18:y * width + 28] = bytes(10)
    return out


def demo_fine_detail(width, height):
    """Left: a grid of patterns -- rows checkerboard, vertical lines,
    horizontal lines, diagonal lines; columns periods of 1, 2, 4 and 8 px.
    Right: a 48-spoke Siemens star. Sharpness and crosstalk at every scale
    and angle."""
    import math

    white, black = LEVEL_COUNT - 1, 0
    out = bytearray([white]) * (width * height)
    half = width // 2
    cell_w, cell_h = (half - 40) // 4, (height - 50) // 4
    patterns = (
        lambda x, y, p: ((x // p) + (y // p)) % 2,
        lambda x, y, p: (x // p) % 2,
        lambda x, y, p: (y // p) % 2,
        lambda x, y, p: ((x + y) // p) % 2,
    )
    for row, pattern in enumerate(patterns):
        for col, period in enumerate((1, 2, 4, 8)):
            x0 = 20 + col * (cell_w + 0) + 4
            y0 = 25 + row * cell_h + 4
            for y in range(cell_h - 8):
                base = (y0 + y) * width + x0
                for x in range(cell_w - 8):
                    out[base + x] = black if pattern(x, y, period) else white

    cx, cy = half + half // 2, height // 2
    radius = min(half, height) // 2 - 20
    spokes = 48
    for y in range(cy - radius, cy + radius + 1):
        for x in range(cx - radius, cx + radius + 1):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy > radius * radius:
                continue
            angle = math.atan2(dy, dx) % (2 * math.pi)
            if int(angle / (2 * math.pi) * spokes * 2) % 2 == 0:
                out[y * width + x] = black
    return out


def sphere_shading(width, height):
    """The demo sphere's 8-bit greys before dithering: Lambert shading lit
    from the top left, on black."""
    import math

    values = _canvas(width, height, 0.0)
    cx, cy = width // 2, height // 2
    radius = min(width, height) * 2 // 5
    light = (-0.5, -0.6, 0.62)
    norm = math.sqrt(sum(c * c for c in light))
    light = tuple(c / norm for c in light)
    for y in range(cy - radius, cy + radius + 1):
        for x in range(cx - radius, cx + radius + 1):
            nx, ny = (x - cx) / radius, (y - cy) / radius
            nn = nx * nx + ny * ny
            if nn > 1.0:
                continue
            nz = math.sqrt(1.0 - nn)
            lambert = max(0.0, nx * light[0] + ny * light[1] + nz * light[2])
            values[y * width + x] = 255.0 * (0.06 + 0.94 * lambert)
    return values


def demo_shaded_sphere(width, height):
    """A Lambert-shaded sphere on black, Floyd-Steinberg dithered: the
    dithering on a curved, photograph-like tone, and a mostly dark frame for
    what a dark picture leaves behind on the next one."""
    return quantize_floyd_steinberg(sphere_shading(width, height), width, height,
                                    DEMO_LEVELS, DEMO_TARGETS)


def demo_checkerboard(width, height):
    """60 px black and white squares: the hardest whole-panel transition,
    and a clean edge for spotting residue the previous picture left."""
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            out[y * width + x] = 0 if ((x // 60) + (y // 60)) % 2 == 0 else LEVEL_COUNT - 1
    return out


# Digits for the level-match chart, 5 x 7, drawn without a font file so that
# `demo` stays standard library only.
_DIGITS = {
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
}
_DIGIT_SCALE = 2
_DIGIT_ADVANCE = 6 * _DIGIT_SCALE


def _draw_digits(out, width, x, y, text, level):
    for ch in text:
        for gy, row in enumerate(_DIGITS[ch]):
            for gx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(_DIGIT_SCALE):
                        base = (y + gy * _DIGIT_SCALE + dy) * width + x + gx * _DIGIT_SCALE
                        out[base:base + _DIGIT_SCALE] = bytes([level]) * _DIGIT_SCALE
        x += _DIGIT_ADVANCE


def demo_level_match(width, height):
    """How light each grey level is on the glass, judged by eye
    (docs/hardware/board_notes.md, "How light each grey level really is").

    One row per level, 1 to 14. Across each row, 21 cells dithered from
    levels 0 and 15 -- 0 % white on the left to 100 % on the right, in
    steps of 5 %, the percentages printed above -- each with a solid patch
    of the row's level in its middle. A fine black-and-white mix blends to
    one grey at viewing distance, and light adds linearly, so a mix of
    fraction f is as light as the panel's black plus f of the way to its
    white. Where a patch disappears into its cell, that f is how light the
    level is. Levels 0 and 15 are the mix's own ends and need no row.

    The mixes are ordered 8 x 8 Bayer patterns anchored to the panel's own
    pixels. Fine black-and-white patterns may darken or lighten a little
    on e-paper, where neighbouring pixels pull on each other, so the table
    this yields is checked against the photographs, not trusted blindly."""
    white, black = LEVEL_COUNT - 1, 0
    out = bytearray([white]) * (width * height)
    columns = list(range(0, 101, 5))
    rows = list(range(1, LEVEL_COUNT - 1))
    left, top = 34, 24
    gap = 4
    cell_w = (width - left - 4) // len(columns)
    pitch = (height - top - 4) // len(rows)
    cell_h = pitch - gap
    patch_w, patch_h = cell_w * 9 // 20, cell_h // 2
    bayer = _bayer(8)

    for col, percent in enumerate(columns):
        text = str(percent)
        x = left + col * cell_w + (cell_w - len(text) * _DIGIT_ADVANCE + _DIGIT_SCALE) // 2
        _draw_digits(out, width, x, 4, text, black)

    for row, level in enumerate(rows):
        y0 = top + row * pitch
        _draw_digits(out, width, 4, y0 + (cell_h - 7 * _DIGIT_SCALE) // 2,
                     f"{level:2d}".replace(" ", ""), black)
        for col, percent in enumerate(columns):
            x0 = left + col * cell_w
            threshold = percent * 64 / 100
            for y in range(y0, y0 + cell_h):
                base = y * width
                cells = bayer[y % 8]
                for x in range(x0, x0 + cell_w):
                    out[base + x] = white if cells[x % 8] + 0.5 <= threshold else black
            px, py = x0 + (cell_w - patch_w) // 2, y0 + (cell_h - patch_h) // 2
            for y in range(py, py + patch_h):
                out[y * width + px:y * width + px + patch_w] = bytes([level]) * patch_w
    return out


def cmd_demo(args):
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    width, height = args.size
    pictures = (
        ("01_grey_ramp", lambda: demo_grey_ramp(width, height)),
        ("02_dither_methods", lambda: demo_dither_methods(width, height)),
        ("03_fine_detail", lambda: demo_fine_detail(width, height)),
        ("04_shaded_sphere", lambda: demo_shaded_sphere(width, height)),
        ("05_checkerboard", lambda: demo_checkerboard(width, height)),
        ("06_level_match", lambda: demo_level_match(width, height)),
    )
    stale = []
    for name, make in pictures:
        path = out_dir / f"{name}.png"
        levels = make()
        if args.verify:
            # Pixels, not bytes: the compressed stream depends on the zlib
            # version, so byte equality would fail on a machine whose zlib
            # differs even when every pixel is the same.
            try:
                w, h, values = read_png_grey(path)
                same = (w, h) == (width, height) and levels == bytearray(v // 17 for v in values)
            except (OSError, NotPanelReady, zlib.error, struct.error):
                same = False
            if not same:
                stale.append(path.name)
            continue
        png = encode_png_gray4(levels, width, height, f"demo {name}")
        path.write_bytes(png)
        print(f"wrote {path} ({len(png)} bytes)")
    if args.verify:
        if stale:
            sys.exit(f"demo pictures out of date in {out_dir}: {', '.join(stale)}. "
                     f"Regenerate them: {GENERATOR} demo --out-dir {out_dir}")
        print(f"demo pictures in {out_dir} are current")


# --- levels ------------------------------------------------------------------

def cmd_levels(args):
    lightness = panel_lightness()
    print(f"how light each level is on the glass, from {LIGHTNESS_HEADER.relative_to(ROOT)}:")
    for level, light in enumerate(lightness):
        print(f"level {level:2}: lightness {light:.3f}, looks like 8-bit grey "
              f"{round(255 * light_to_srgb(light)):3} (written to PNGs as {LEVEL_CODE[level]:3})")


# --- embed -------------------------------------------------------------------

C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _symbol(stem):
    return re.sub(r"[^a-z0-9_]", "_", stem.lower())


def _caption(stem):
    # The firmware's fonts cover printable ASCII only (core/ui/fonts/font.h).
    return "".join(c if " " <= c <= "~" and c not in '"\\' else "_" for c in stem)


def cmd_embed(args):
    if not C_IDENTIFIER.fullmatch(args.name):
        sys.exit(f"--name must be a C identifier, got {args.name!r}")
    if not args.inputs:
        sys.exit("embed: no images given")

    pictures = []
    seen = {}
    for source in args.inputs:
        source = pathlib.Path(source)
        symbol = f"{args.name}_{_symbol(source.stem)}"
        if symbol in seen:
            sys.exit(f"{source} and {seen[symbol]} both become {symbol}; rename one")
        seen[symbol] = source
        try:
            width, height, values = read_png_grey(source)
        except NotPanelReady as reason:
            sys.exit(f"{source}: not panel-ready: {reason}. Run `{GENERATOR} prepare` on it.")
        except (OSError, zlib.error, struct.error) as reason:
            sys.exit(f"{source}: cannot read it as a PNG: {reason}")
        if width > 0xFFFF or height > 0xFFFF:
            sys.exit(f"{source}: {width}x{height} does not fit image_t's uint16_t sides")

        levels = bytearray(width * height)
        for i, value in enumerate(values):
            if isinstance(value, int) and value % 17 == 0:
                levels[i] = value // 17
                continue
            if not args.quantize:
                x, y = i % width, i // width
                sys.exit(f"{source}: not panel-ready: pixel ({x}, {y}) is grey {value:g}, "
                         f"not one of the sixteen levels (multiples of 17). Run "
                         f"`{GENERATOR} prepare` on it, or pass --quantize to round "
                         f"every pixel to the nearest level without dithering.")
            levels[i] = int(value / 17 + 0.5)

        # The framebuffer's order, not PNG's: the even pixel of each pair in
        # the low nibble, the odd one in the high (docs/adr/0003). An odd
        # width leaves the last high nibble as padding, written as 0.
        stride = (width + 1) // 2
        packed = bytearray(stride * height)
        for y in range(height):
            base = y * width
            row = y * stride
            for x in range(0, width, 2):
                low = levels[base + x]
                high = levels[base + x + 1] if x + 1 < width else 0
                packed[row + x // 2] = (high << 4) | low
        pictures.append((symbol, _caption(source.stem), source.name, width, height, stride,
                         packed))

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    header_name = f"{args.name}.generated.h"
    count_macro = f"{args.name.upper()}_COUNT"
    guard = f"HAC_GENERATED_{args.name.upper()}_H"
    sources = ", ".join(p[2] for p in pictures)

    h = [
        "// SPDX-License-Identifier: Apache-2.0",
        "//",
        f"// GENERATED by {GENERATOR} embed -- do not edit, do not commit.",
        f"// From: {sources}",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include \"ui/image.h\"",
        "",
        f"#define {count_macro} {len(pictures)}u",
        "",
    ]
    h += [f"extern const image_t {p[0]};" for p in pictures]
    h += [
        "",
        "// In file-name order, as given to the generator.",
        f"extern const image_t *const {args.name}_all[{count_macro}];",
        f"extern const char *const {args.name}_names[{count_macro}];",
        "",
        f"#endif  // {guard}",
        "",
    ]

    c = [
        "// SPDX-License-Identifier: Apache-2.0",
        "//",
        f"// GENERATED by {GENERATOR} embed -- do not edit, do not commit.",
        "",
        f"#include \"{header_name}\"",
        "",
        "#include <stdint.h>",
        "",
    ]
    total = 0
    for symbol, _caption_text, file_name, width, height, stride, packed in pictures:
        total += len(packed)
        c.append(f"// {file_name}: {width}x{height}, GRAY4")
        c.append(f"static const uint8_t k_{symbol}_pixels[{len(packed)}] = {{")
        for i in range(0, len(packed), 24):
            c.append("    " + ", ".join(f"0x{b:02x}" for b in packed[i:i + 24]) + ",")
        c.append("};")
        c.append(f"const image_t {symbol} = {{")
        c.append(f"    .pixels = k_{symbol}_pixels,")
        c.append(f"    .width = {width}u,")
        c.append(f"    .height = {height}u,")
        c.append(f"    .stride = {stride}u,")
        c.append("    .format = PIXFMT_GRAY4,")
        c.append("};")
        c.append("")
    c.append(f"const image_t *const {args.name}_all[{count_macro}] = {{")
    c += [f"    &{p[0]}," for p in pictures]
    c.append("};")
    c.append(f"const char *const {args.name}_names[{count_macro}] = {{")
    c += [f"    \"{p[1]}\"," for p in pictures]
    c.append("};")
    c.append("")

    (out_dir / header_name).write_text("\n".join(h), encoding="ascii")
    (out_dir / f"{args.name}.generated.c").write_text("\n".join(c), encoding="ascii")
    print(f"embedded {len(pictures)} image(s), {total} bytes of pixels, as {args.name}")


# --- command line ------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__ or "Pictures for the ED047TC1.")
    sub = parser.add_subparsers(dest="command", required=True)

    prepare = sub.add_parser("prepare", help="photos -> panel-ready PNGs (needs Pillow)")
    prepare.add_argument("inputs", nargs="+", help="any image Pillow can open")
    prepare.add_argument("--out-dir", required=True)
    prepare.add_argument("--size", type=parse_size, default=PANEL_SIZE,
                         help="target WIDTHxHEIGHT (default: the panel, %(default)s)")
    prepare.add_argument("--fit", choices=("cover", "contain", "stretch"), default="cover",
                         help="cover: fill and crop the overflow; contain: fit whole, "
                              "bars of --background around it; stretch: ignore the aspect "
                              "ratio (default: %(default)s)")
    prepare.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0,
                         help="clockwise, after EXIF orientation. For a panel standing "
                              "portrait: 270 when the device is turned clockwise, 90 when "
                              "anticlockwise -- with --size left at the panel's 960x540 "
                              "(default: %(default)s)")
    prepare.add_argument("--focus", type=parse_focus, default=(0.5, 0.5), metavar="X,Y",
                         help="for --fit cover: the point to keep in view, as fractions of "
                              "the upright picture's width and height, 0,0 top left "
                              "(default: 0.5,0.5, the centre)")
    prepare.add_argument("--dither", choices=tuple(DITHERS), default="floyd-steinberg")
    prepare.add_argument("--levels", choices=("distinct", "all"), default="distinct",
                         help="distinct: leave out any level that looks the same as a darker "
                              "one, level 1 on this panel (default: %(default)s)")
    prepare.add_argument("--lightness", choices=("measured", "nominal"), default="measured",
                         help="measured: how light each level was measured on the glass "
                              "under the old GC16 full refresh (docs/adr/0013); nominal: "
                              "level L as sRGB grey L * 17, level 1 as black -- preferred "
                              "on the glass since the full refresh darkens from white "
                              "(default: %(default)s)")
    prepare.add_argument("--gamma", type=float, default=1.0,
                         help="applied to the picture's greys before they become light: "
                              "above 1 lightens the midtones, below 1 darkens them; 1 keeps "
                              "the photograph's own tones (default: %(default)s)")
    prepare.add_argument("--noise", type=float, default=0.0, metavar="GREYS",
                         help="triangular noise of up to this many 8-bit greys, added before "
                              "dithering: it breaks up the thin contours error diffusion draws "
                              "where a smooth tone crosses a level -- 8 took them off the demo "
                              "sphere on the panel -- at the cost of a fine grain "
                              "(default: off)")
    prepare.add_argument("--autocontrast", type=float, default=0.5,
                         help="percent of the darkest and lightest pixels to clip while "
                              "stretching the histogram; 0 switches it off "
                              "(default: %(default)s)")
    prepare.add_argument("--sharpen", type=int, default=0,
                         help="unsharp-mask strength in percent, applied after scaling; "
                              "50-150 suits a downscaled photo (default: off)")
    prepare.add_argument("--background", type=int, choices=range(LEVEL_COUNT), default=15,
                         metavar="LEVEL", help="level behind a `contain` fit (default: 15)")
    prepare.add_argument("--suffix", default="",
                         help="appended to each output name, e.g. _atkinson, to compare "
                              "settings side by side")
    prepare.set_defaults(run=cmd_prepare)

    demo = sub.add_parser("demo", help="synthetic test pictures (standard library only)")
    demo.add_argument("--out-dir", required=True)
    demo.add_argument("--size", type=parse_size, default=PANEL_SIZE)
    demo.add_argument("--verify", action="store_true",
                      help="compare with the pictures already in --out-dir instead of "
                           "writing them; fail if any differs")
    demo.set_defaults(run=cmd_demo)

    levels = sub.add_parser("levels", help="print how light each level is on the glass")
    levels.set_defaults(run=cmd_levels)

    embed = sub.add_parser("embed", help="panel-ready PNGs -> C (standard library only)")
    embed.add_argument("inputs", nargs="*")
    embed.add_argument("--name", required=True,
                       help="C identifier prefix; also names the output files")
    embed.add_argument("--out-dir", required=True)
    embed.add_argument("--quantize", action="store_true",
                       help="round off-level greys to the nearest level instead of failing")
    embed.set_defaults(run=cmd_embed)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
