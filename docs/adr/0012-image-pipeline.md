# 12. Pictures: a panel-ready PNG as the source, embedded into C at build time

Date: 2026-10-13

## Status

Proposed, on branch `slideshow-test-app`. Confirmed on the board
2026-10-13: all five demo pictures shown in turn and the cycle wrapped, every
flush `status=0`, timings below ("Seen on the panel").

## Context

The slideshow test app (`apps/slideshow/`) shows pictures on the panel, one
whole-panel refresh a minute, on the simulator and on the board. That needs an
answer to a question the product will ask again for the air-quality icons of
`docs/requirements.md` §9.2: how does a picture get from a file on a desktop
into a buffer the core can draw?

What the answer has to respect:

- **The panel has sixteen greys, fifteen of them distinct.** Level 1 looks like
  level 0 on this unit (`docs/hardware/board_notes.md`, phase 2c). A photo
  needs scaling, tone and dithering to look like anything at 4 bpp, and those
  steps are where every visible decision is made.
- **The GRAY4 layout is fixed** (`docs/adr/0003-gray4-nibble-order.md`): even
  pixel in the low nibble. A picture already in that layout lands in the
  framebuffer one `memcpy` per row.
- **A 960 x 540 picture is 259,200 bytes** however it is stored in flash
  uncompressed: about fourteen fit in the 4 MB app slot beside the app.
- **Two builds, one source of truth.** The host and ESP-IDF builds must embed
  a picture identically, as they compile the core identically
  (`cmake/sources_core.cmake`).
- **ESP-IDF's Python environment has no Pillow, nor does this WSL's system
  Python.** Anything the build runs must be standard-library Python.
- **The repository is public.** Nothing should be committed that is large,
  unreviewable, or not ours to publish.

## Options considered

**A. Generated C arrays, committed** -- what `apps/tools/font_gen/` does for
fonts. No build step and any toolchain. But a full-panel picture is ~1.6 MB of
hex as C: a handful of pictures puts megabytes into the history for good, a
change to one is an unreadable diff, and there is nothing to look at.

**B. A panel-ready PNG as the source, embedded at build time.** *(Chosen.)*
The lossy work happens once, by hand, into a 4-bit greyscale PNG that holds
exactly the levels the glass will get; the build only repacks it. See
"Decision".

**C. A raw GRAY4 `.bin`, embedded with ESP-IDF's `EMBED_FILES`.** Saves
generating C on the device side only: the host build would need its own
bin-to-C step (`file(READ ... HEX)`, or C23 `#embed`), so the two builds would
embed differently -- the drift `sources_core.cmake` exists to prevent. A
`.bin` cannot be looked at, and a converter is needed to make it anyway.

**D. Compressed in flash, decoded row by row into the framebuffer.** RLE over
4-bit runs shrinks synthetic and UI pictures tens of times (the grey ramp is
960 bytes as a PNG), but a dithered photo is noise to RLE; it would take a
real LZ codec (heatshrink, LZ4, or the S3 ROM's miniz inflate), a decoder in
the core with unit tests and a fuzz target, and decode time on every frame.
The right move once there are many pictures or many icons -- not for a test
app that fits uncompressed.

**E. Pictures in the `storage` partition**, flashed separately as a
SPIFFS/LittleFS image and read at runtime. Pictures change without rebuilding
the firmware, and the 7.9 MB partition holds ~30 raw full-panel pictures (more
as PNGs with an on-device decoder). It needs `port_storage` and a file reader,
neither of which exists before phase 6, and a host adapter to match. The
answer for a real picture frame; out of scope here.

**F. ImageMagick for the lossy stage instead of Pillow.** One command does what
`prepare` does, with no Python package:

```bash
magick photo.jpg -auto-orient -colorspace Gray -resize 960x540^ \
    -gravity center -extent 960x540 -dither FloydSteinberg \
    -remap ramp15.png PNG8:out.png
```

(`ramp15.png`: any image holding exactly the fifteen distinct levels.) It is
compatible with B rather than an alternative to it -- `embed` accepts any PNG
whose pixels are all panel levels, palette or grey -- so it remains open to
anyone who prefers it. The same holds for GIMP (Indexed mode with a
sixteen-grey palette).

## Decision

**B.** `apps/tools/image_gen/image_gen.py` has three subcommands:

- **`prepare`** (Pillow): photo -> panel-ready PNG. EXIF orientation, an
  optional rotation, `--fit cover|contain|stretch` to the panel, then tone
  (`--autocontrast`, `--gamma`, `--sharpen`) and dithering to the panel's
  levels: `none`, `bayer` (8 x 8 ordered), `floyd-steinberg` (serpentine;
  the default) or `atkinson`. Level 1 is left out of the palette by default,
  since dithering towards a grey the panel does not show wastes error. Run
  by hand; the output is the thing to look at before flashing anything.
- **`demo`** (standard library): six synthetic test pictures -- the
  sixteen-level ramp, the four dithering methods side by side, fine
  patterns and a Siemens star, a shaded sphere, a checkerboard, and (added
  2026-10-14) a level-match chart for calibrating the greys by eye. Made
  here, so they carry this project's license. Committed in
  `apps/slideshow/images/` (74 KB together); regenerate them if the script
  changes.
- **`embed`** (standard library): panel-ready PNGs -> `<name>.generated.c/.h`,
  `image_t`s in the framebuffer's own nibble order, plus `<name>_all[]` and
  `<name>_names[]` in file-name order. **Strict**: a grey that is not a
  panel level, a colour, or transparency stops the build and names the
  pixel. `--quantize` rounds instead, only when asked.

`cmake/image_gen.cmake` (`hac_embed_images()`) runs `embed` at build time and
compiles the result into a target. The host build and ESP-IDF component
CMakeLists both include it -- the device build with ESP-IDF's own Python --
so a picture is embedded one way. Generated C lives in the build tree only.

In the core, `ui/image.h` (`image_t`) and `gfx_blit_image()`: an opaque copy
of a picture in the framebuffer's own format, clipped, whole bytes per row
where the alignment allows. It is the "blit" `docs/architecture.md` lists among
the gfx primitives and what §9.2's icons will draw with; converting between
formats is left to the caller, since GRAY4 to MONO1 means choosing a threshold
or a dither.

## Consequences

- **Flash.** 259,200 bytes per full-panel picture. The slideshow image with
  the demo set is 1.5 MB of the 4 MB slot; about fourteen full-panel
  pictures fit. More needs D or E.
- **Build.** The slideshow and the pipeline's test need a Python 3
  interpreter (standard library only); without one the host build leaves
  both out with a warning, as it leaves the simulator's window out without
  SDL2. Embedding the demo set takes about two seconds and reruns only when a
  picture or the script changes. Compiling the 8 MB of generated C takes
  well under a second on the host.
- **Pillow is needed for `prepare` only** -- and was already needed for
  `font_gen` -- and is installed nowhere here by default
  (`pip install pillow` in a venv, or `apt install python3-pil`).
  `THIRD_PARTY_NOTICES.md` lists it as a developer tool.
- **The tone curve is an estimate.** `image_gen.py` assumes level L looks like
  8-bit grey L x 17. Phase 2c checked the ramp by eye -- monotonic, fifteen
  distinct steps -- but measured no reflectance. A photometric reading of the
  calibration ramp would replace `LEVEL_VALUE`; no branch has it planned.
  Until then `--gamma` is the knob, and the dithered sphere and gradients of
  the demo set are what to judge it by on the glass. **Superseded
  2026-10-14** by a measured table, `docs/adr/0013-grey-level-lightness.md`.
- **Tested.** `tests/unit/test_image_embed.c` embeds the same 7 x 3 picture
  from five encodings -- 4-, 8- and 16-bit grey, a palette written in reverse
  order, neutral RGB; all but one written by Pillow, so the decoder is
  checked against an encoder other than its own -- and asserts every level
  and the nibble order of the bytes. CTest cases assert the exact refusal for
  an off-level grey, a colour and transparency. `gfx_blit_image()` is checked
  pixel by pixel at every placement from wholly off one edge to wholly off
  the other.
- **Judged by eye on the panel** -- see "Seen on the panel" below:
  Floyd-Steinberg is the smoothest of the four methods and stays the
  default; the sphere shows rings nobody has explained yet; and fine
  patterns leave residue that whole-panel refreshes do not clear.
- **A side use.** The slideshow is one whole-panel refresh a minute for as
  long as it runs, and its order puts the mostly black sphere and the
  checkerboard before the grey ramp, where `board_notes.md`'s open item on
  residue says grey shows what white hides. It is a ready-made soak test for
  that item.
- **User pictures stay out of the repository.** `SLIDESHOW_IMAGES_DIR` points
  the build at any directory; photos of people, or anything not ours to
  publish, belong outside the tree.

## Seen on the panel

2026-10-13, this unit, image ELF `bd7fa6729...`, running on `app_main`'s task
(core 0). From the boot log; `flush_ms` is `port_display->flush()` timed with
`esp_timer`, the GC16 figure is EPDiy's own `total`, and the clear is their
difference (power bracket included).

| Picture | Simulator | Panel `flush_ms` | GC16 | Clear |
|---|---|---|---|---|
| `01_grey_ramp` | 3,662 | 3,660 | 1,204 | 2,456 |
| `02_dither_methods` | 3,598 | 3,594 | 1,137 | 2,457 |
| `03_fine_detail` | 3,608 | 3,604 | 1,148 | 2,456 |
| `04_shaded_sphere` | 3,662 | 3,660 | 1,203 | 2,457 |
| `05_checkerboard` | 3,662 | 3,661 | 1,203 | 2,458 |

- **The emulation holds on pictures it was not fitted to**: within 4 ms
  (0.11 %) on every one, including the two whose white rows let EPDiy skip
  writing part of the panel. ADR 0008's model was fitted to calibration
  patterns and `screen_home`.
- **The clear is the calibrated 2,456 ms again**, five times, from the same
  task context as the calibration and on different content. That supports
  ADR 0008's "Task context" reading of the product's 2,322 ms (core 1) as a
  property of the calling task rather than of the content; it does not
  replace re-measuring from the product's task.
- **Cadence**: refresh starts 63.64-63.70 s apart -- the 60 s interval from
  the end of each refresh, plus the refresh, plus about 40 ms of rendering
  and the wait's tick rounding (`docs/adr/0006`).
- **Memory**: free PSRAM after display init 7,349,100 bytes, the same as the
  product's; the 1.3 MB of pictures stay in flash (the 1,358,064-byte
  mapped rodata segment) and cost no RAM. Free internal RAM 228,219 bytes,
  about 3.8 KB below the product's, which the larger main-task stack
  (8,192 bytes against 3,584) accounts for.

### By eye

Judged by the user, looking at the panel, 2026-10-13:

- **Floyd-Steinberg is the smoothest** of the four bands of
  `02_dither_methods`; the undithered band shows its steps most clearly, as
  it should. Floyd-Steinberg stays `prepare`'s default.
- **One-pixel patterns do not resolve from where a picture is looked at.**
  The 1 px checkerboard of `03_fine_detail` reads as flat grey and can be
  told apart only from about 10 cm. That is what dithering relies on: its
  pattern disappears at viewing distance and only the average grey is seen.
- **The sphere's shading is smooth, but shows concentric white rings**, and
  its shadowed side merges with the black background. The merge is
  expected: the darkest tone on the sphere is about 6 % grey, dithered
  between levels 0 and 2 at the dark end of the panel, where its levels lie
  closest together. The rings are not explained. Two candidates, neither
  tested: the panel's levels are not evenly spaced -- `LEVEL_VALUE` is an
  estimate -- so the step between two particular neighbours shows as a
  contour; or Floyd-Steinberg leaves flat, undithered areas wherever the
  gradient passes exactly through a level, and those read as bands. A
  photometric reading of the ramp would test the first; the same sphere
  with a little noise added before dithering would test the second.
- **Fine patterns leave residue that whole-panel refreshes do not clear.**
  The 1-8 px checkerboards and line patterns of `03_fine_detail` stayed
  visible as faint stripes and small squares on the greys of
  `01_grey_ramp` and `02_dither_methods`, three and four full refreshes
  later -- each an `epd_clear()` and a GC16 drive, with no partial refresh
  anywhere in the cycle. The 60 px squares of `05_checkerboard`, on the
  panel immediately before the ramp, did not show through at all. Residue
  appears to follow edges, not area. It joins `board_notes.md`'s open item
  on residue, which so far had only seen it after partial refreshes.

## The photo frame

2026-10-13. The pipeline's first real pictures: twelve family photographs,
all portrait 3:4 (3072 x 4080, Pixel), shown by a second app,
`apps/photo_frame/`.

- **No code of its own.** `apps/photo_frame/host/` and `esp32/` compile the
  slideshow's compositions (`apps/slideshow/host/main.c`,
  `apps/slideshow/esp32/main/main.c` with its `Kconfig.projbuild`) against
  their own pictures, with the caption off -- `SLIDESHOW_DEFAULT_CAPTION=0`
  on the host, `CONFIG_HAC_SLIDESHOW_CAPTION` unset on the board. A separate
  ESP-IDF project rather than a build variant of the slideshow's, so it has
  its own build directory, `sdkconfig` and image, and `idf.py build` in its
  directory is the whole recipe. `apps/slideshow/slideshow_images.cmake`
  became the shared "which PNGs" helper for both.
- **The pictures stay out of git.** They live in `apps/photo_frame/images/`,
  which `.gitignore` excludes, or wherever `PHOTO_FRAME_IMAGES_DIR` points.
  With none there -- a fresh clone, CI -- the host build skips the frame
  with a status line and the ESP-IDF project refuses to build. Their pixels
  do reach the build directories and the image, which are gitignored too;
  nothing of them is pushed.
- **Portrait, by turning the device.** A 3:4 portrait cropped to the
  landscape 16:9 panel keeps 42 % of its height, which takes heads or feet
  off full-length shots; fitted whole, it leaves 58 % of the panel empty.
  Stood on its side, the panel is 540 x 960, and the photograph fills it
  with 12.5 % trimmed from each side. The rotation is made by `prepare
  --rotate 270`, so the panel-ready PNG is what the glass gets, sideways,
  and the firmware needs no rotated drawing. 270 puts the photograph's top
  at the panel's left edge: the device is turned a quarter turn clockwise.
  Turned the other way, the pictures are upside down -- stand it on its
  other side, or re-prepare with `--rotate 90`.
- **Crops that keep the subject.** `prepare --focus X,Y` centres a `cover`
  crop on a point of the upright photograph as nearly as its edges allow.
  Eleven of the twelve are centred; on one the child is at the right, and
  `--focus 0.7,0.5` keeps all of her.
- **Settings:** `--rotate 270 --fit cover --dither floyd-steinberg
  --sharpen 80`, autocontrast 0.5 %, gamma 1, level 1 left out -- the same
  options before and after the levels were calibrated, though since
  2026-10-14 they mean dithering in light (ADR 0013). Each PNG's `tEXt`
  chunk records the settings it was made with, the lightness table
  included.
- **Budget:** 12 x 259,200 bytes; the image is 3.39 MB, 81 % of the 4 MB app
  slot. About three more full-panel photographs fit before the pictures need
  option D or E.
- **Seen on the panel, 2026-10-14: the photographs look overexposed**, while
  the simulator shows them as intended and their files are mid-dark (mean
  level 6.7 of 15). The tone curve this ADR calls an estimate is the
  likely cause: the panel shows its middle levels much lighter than
  `LEVEL_VALUE` assumes, and the simulator, assuming the same, cannot show
  the difference. `board_notes.md`, "How light each grey level really is";
  measured and fixed in `docs/adr/0013-grey-level-lightness.md`.
  A first comparison on the panel -- three photographs, each prepared at
  gamma 1.0, 0.7 and 0.5 (`prepare --gamma`, below 1 darkens) -- put the
  best look at about 0.75, judged by eye. A second round,
  0.65/0.75/0.85, found 0.85 clearly still overexposed; a third,
  0.7/0.6/0.5, found 0.7 too light and 0.5 too dark; a fourth,
  0.55/0.6/0.65, settled on **0.6**, the same for all three photographs.
  The frame's photographs were prepared at 0.6 until the levels were
  calibrated: a gamma is one number standing in for a sixteen-entry table.
  **Since 2026-10-14 they are prepared in light towards the measured table
  at gamma 1.0**, judged on the panel over the old 0.6 and over the new
  pipeline at 0.6 and 1.15 (`docs/adr/0013-grey-level-lightness.md`).
- **To watch for:** a dithered photograph is fine pattern everywhere, which
  is exactly what the demo set found to leave residue through full
  refreshes ("By eye" above). Each photograph may show faintly in the greys
  of the next ones.
