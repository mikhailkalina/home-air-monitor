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
#
# A panel-ready PNG is a 4-bit greyscale PNG in which every pixel already is
# one of the panel's levels. It is exactly what the glass will be driven
# with, it opens in any viewer, and GitHub shows it in a review. Everything
# lossy -- scaling, cropping, tone, dithering -- happens in `prepare`, once,
# where the result can be looked at. `embed` only repacks, losslessly, and
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

# On this unit, level 1 cannot be told from level 0 by eye
# (docs/hardware/board_notes.md, the waveform greys item, phase 2c;
# EPD_SIM_DARKEST_MERGED_LEVEL in platform/host/epd_emulation.h). Dithering
# towards it would spend error on a grey the panel does not show, so
# `--levels distinct`, the default, leaves it out.
DARKEST_MERGED_LEVEL = 1

# ESTIMATE, not a measurement: level L is assumed to look like 8-bit grey
# L * 17, evenly spaced from black to white. Phase 2c judged the ramp by eye
# (monotonic, fifteen distinguishable steps) but measured no reflectance. A
# photometric reading of the calibration ramp would replace this table, and
# with it the tone every dithered picture gets; no branch has it planned yet
# (docs/adr/0012-image-pipeline.md, "Consequences"). Until then, --gamma is
# the knob.
LEVEL_VALUE = [level * 17 for level in range(LEVEL_COUNT)]

GENERATOR = "apps/tools/image_gen/image_gen.py"


def parse_size(text):
    match = re.fullmatch(r"(\d+)x(\d+)", text)
    if not match:
        raise argparse.ArgumentTypeError(f"expected WIDTHxHEIGHT, got {text!r}")
    width, height = int(match.group(1)), int(match.group(2))
    if not (0 < width <= 0xFFFF and 0 < height <= 0xFFFF):
        raise argparse.ArgumentTypeError(f"size out of range: {text}")
    return width, height


def palette_levels(mode):
    """The levels a picture may be dithered to, darkest first."""
    if mode == "all":
        return list(range(LEVEL_COUNT))
    return [level for level in range(LEVEL_COUNT) if level != DARKEST_MERGED_LEVEL]


# --- quantizing and dithering -------------------------------------------------
#
# Pure Python over a flat, row-major list of 8-bit grey values (floats are
# fine), returning a bytearray of panel levels. Slow by image-processing
# standards -- a second or two per panel-sized picture -- and dependency-free,
# which is what lets `demo` run without Pillow and keeps every method in one
# place where they can be compared.

def _nearest_table(levels):
    """For every 8-bit value, the index into `levels` of the nearest level."""
    table = []
    for value in range(256):
        best = min(range(len(levels)), key=lambda i: abs(LEVEL_VALUE[levels[i]] - value))
        table.append(best)
    return table


def _clamp_round(value):
    if value <= 0.0:
        return 0
    if value >= 255.0:
        return 255
    return int(value + 0.5)


def quantize_nearest(values, width, height, levels):
    nearest = _nearest_table(levels)
    return bytearray(levels[nearest[_clamp_round(v)]] for v in values)


def quantize_floyd_steinberg(values, width, height, levels):
    # Serpentine: alternate rows run right to left, which breaks up the
    # diagonal "worms" a one-way scan leaves in smooth gradients.
    nearest = _nearest_table(levels)
    targets = [LEVEL_VALUE[level] for level in levels]
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
            clamped = _clamp_round(buf[i])
            index = nearest[clamped]
            out[i] = levels[index]
            err = clamped - targets[index]
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


def quantize_atkinson(values, width, height, levels):
    # Spreads only 6/8 of the error, to six neighbours. Crisper and more
    # contrasty than Floyd-Steinberg; loses detail in the deepest shadows and
    # brightest highlights, where the dropped quarter accumulates.
    nearest = _nearest_table(levels)
    targets = [LEVEL_VALUE[level] for level in levels]
    buf = [float(v) for v in values]
    out = bytearray(width * height)
    neighbours = ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2))
    for y in range(height):
        row = y * width
        for x in range(width):
            i = row + x
            clamped = _clamp_round(buf[i])
            index = nearest[clamped]
            out[i] = levels[index]
            share = (clamped - targets[index]) / 8
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


def quantize_bayer(values, width, height, levels):
    # Ordered 8x8: a fixed threshold pattern instead of carried error. A
    # regular cross-hatch rather than noise, no error creeping across edges,
    # and it compresses far better -- at the cost of a visible texture.
    bayer = _bayer(8)
    cells = len(bayer) * len(bayer)
    targets = [LEVEL_VALUE[level] for level in levels]
    # For every 8-bit value: the levels either side of it and how far between
    # them it lies, so an uneven palette (level 1 left out) dithers correctly.
    bracket = []
    for value in range(256):
        lo = max(i for i in range(len(levels)) if targets[i] <= value)
        hi = min(i for i in range(len(levels)) if targets[i] >= value)
        span = targets[hi] - targets[lo]
        bracket.append((lo, hi, 0.0 if span == 0 else (value - targets[lo]) / span))
    out = bytearray(width * height)
    for y in range(height):
        thresholds = [(bayer[y % 8][x % 8] + 0.5) / cells for x in range(8)]
        row = y * width
        for x in range(width):
            lo, hi, t = bracket[_clamp_round(values[row + x])]
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
    """Writes `levels` (one panel level per pixel, row-major) as a 4-bit
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
    pathlib.Path(path).write_bytes(bytes(png))


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

def cmd_prepare(args):
    try:
        from PIL import Image, ImageFilter, ImageOps
    except ImportError:
        sys.exit("prepare needs Pillow: pip install pillow (or apt install python3-pil)")

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    levels = palette_levels(args.levels)
    target_w, target_h = args.size
    background = LEVEL_VALUE[args.background]
    quantize = DITHERS[args.dither]

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
            image = ImageOps.fit(image, (target_w, target_h), Image.Resampling.LANCZOS)
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
        canvas.paste(image, ((target_w - image.width) // 2, (target_h - image.height) // 2))

        panel = quantize(list(canvas.tobytes()), target_w, target_h, levels)
        note = (f"prepare {source.name} fit={args.fit} dither={args.dither} "
                f"gamma={args.gamma} autocontrast={args.autocontrast} sharpen={args.sharpen}")
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


def demo_dither_methods(width, height, levels):
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
        quantized = DITHERS[method](ramp, gradient_w, band_h, levels)
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


def demo_shaded_sphere(width, height, levels):
    """A Lambert-shaded sphere on black, Floyd-Steinberg dithered: the
    dithering on a curved, photograph-like tone, and a mostly dark frame for
    what a dark picture leaves behind on the next one."""
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
    return quantize_floyd_steinberg(values, width, height, levels)


def demo_checkerboard(width, height):
    """60 px black and white squares: the hardest whole-panel transition,
    and a clean edge for spotting residue the previous picture left."""
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            out[y * width + x] = 0 if ((x // 60) + (y // 60)) % 2 == 0 else LEVEL_COUNT - 1
    return out


def cmd_demo(args):
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    width, height = args.size
    levels = palette_levels("distinct")
    pictures = (
        ("01_grey_ramp", lambda: demo_grey_ramp(width, height)),
        ("02_dither_methods", lambda: demo_dither_methods(width, height, levels)),
        ("03_fine_detail", lambda: demo_fine_detail(width, height)),
        ("04_shaded_sphere", lambda: demo_shaded_sphere(width, height, levels)),
        ("05_checkerboard", lambda: demo_checkerboard(width, height)),
    )
    for name, make in pictures:
        path = out_dir / f"{name}.png"
        write_png_gray4(path, make(), width, height, f"demo {name}")
        print(f"wrote {path} ({path.stat().st_size} bytes)")


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
                         help="clockwise, after EXIF orientation (default: %(default)s)")
    prepare.add_argument("--dither", choices=tuple(DITHERS), default="floyd-steinberg")
    prepare.add_argument("--levels", choices=("distinct", "all"), default="distinct",
                         help="distinct: leave out level 1, which this panel shows as 0 "
                              "(default: %(default)s)")
    prepare.add_argument("--gamma", type=float, default=1.0,
                         help="above 1 lightens the midtones, below 1 darkens them")
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
    demo.set_defaults(run=cmd_demo)

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
