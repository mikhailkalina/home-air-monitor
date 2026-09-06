# 3. GRAY4 nibble order follows the ED047TC1 reference layout

Date: 2026-09-05

## Status

Accepted.

## Context

`core/ui/framebuffer.c` packs `PIXFMT_GRAY4` two pixels to a byte. Which of the
two pixels lands in the high nibble is arbitrary as far as the core is
concerned — every read and write goes through `framebuffer_set_pixel()` /
`framebuffer_get_pixel()`, and `gfx.c` never touches the packing directly — so
either choice renders identically. It stops being arbitrary at the boundary
with the panel.

Phase 1a chose *even pixel in the high nibble*, which reads naturally: at
`x = 0` you write the first nibble you see in a hex dump.

While resolving the display orientation for the board notes, we compared this
against the vendor reference firmware and found it inverted. LilyGo-EPD47
(branch `esp32s3`, `src/epd_driver.c:339-346`) packs the **odd** pixel high:

```c
uint8_t *buf_ptr = &framebuffer[y * EPD_WIDTH / 2 + x / 2];
if (x % 2) { *buf_ptr = (*buf_ptr & 0x0F) | (color & 0xF0); }  /* odd  -> high */
else       { *buf_ptr = (*buf_ptr & 0xF0) | (color >> 4);   }  /* even -> low  */
```

Everything else about the two layouts already agreed: row-major, x as the fast
axis, `stride = width / 2`, which at the panel's 960 px width is 480 bytes with
no padding. The nibble order was the single remaining difference between our
buffer and the one the panel driver expects.

## Decision

Match the reference: **the odd pixel occupies the high nibble, the even pixel
the low nibble.**

The two options considered:

**Option A — keep even-high in the core, convert in `adp_display.c`.**
Preserves the more readable convention and touches nothing today. The cost is
paid on every flush, forever: a full-frame conversion walks 253 125 bytes to
swap every nibble, on an MCU where the flush path is already the expensive
part of the wake cycle. It also needs a scratch buffer of the same size in
PSRAM, or an in-place mutation of a buffer the core still believes it owns.
Worse, it is a conversion that exists only to preserve a cosmetic preference,
which makes it exactly the kind of code that gets "optimised away" later by
someone who cannot see why it is there.

**Option B — match the reference in the core (chosen).**
`port_display.get_framebuffer()` hands back memory the adapter can pass
straight to `epd_draw_grayscale_image()`, or draw into directly, with no
conversion and no second buffer. The concession is that the packing no longer
matches naive intuition, which is mitigated by stating the order and its
reason in the `framebuffer.h` header comment and pointing here.

Option B wins because the core's GRAY4 layout has exactly one real consumer —
the panel — and no reason of its own to prefer either order. Paying a
per-flush cost to hold a preference the core does not actually hold is the
wrong trade. The readability argument is real but is answered by a comment;
the memcpy argument cannot be answered by a comment.

## Consequences

- `framebuffer_set_pixel()` and `framebuffer_get_pixel()` swap their nibble
  selection. `framebuffer_clear()` is unaffected: it fills with
  `nibble << 4 | nibble`, which is symmetric.
- `tests/golden/test_card_gray4.bin` was regenerated. The image is unchanged —
  verified twice: every one of its 11 000 bytes is the previous byte with its
  nibbles swapped, and decoding the old file under the old convention against
  the new file under the new one gives 0 differing pixels out of 22 000.
  `test_card_mono1.bin` is byte-identical, as it must be.
- The one unit test that asserted raw packed bytes
  (`gray4_even_and_odd_pixels_share_a_byte_independently`) now asserts the new
  order. It is deliberately a byte-level assertion rather than a round-trip
  through `get_pixel()`: it is what pins this decision, and it should fail
  loudly if anyone flips the order back.
- The core is now coupled to one panel family's byte layout. This is a
  deliberate, contained concession, and the containment is the point: **a
  future panel with the opposite convention converts inside its own adapter,
  not in the core.** `port_display` already reports panel-specific constraints
  up to the core rather than pushing panel details down into it, and a nibble
  swap belongs on the same side of that line as waveform modes and refresh
  timing. Do not add a `pixel_order` field to `framebuffer_t` to make the core
  configurable for a hypothetical second panel — that is the over-abstraction
  `docs/architecture.md` §12 warns about. If and when a second panel with the
  other order actually exists, it gets a conversion in its adapter, and pays
  the cost this ADR declined to pay for the panel we do have.
- If the display adapter is ever built on something other than LilyGo-EPD47 /
  EPDiy, re-check this assumption before relying on the memcpy: the decision is
  only worth what the byte-for-byte compatibility is worth.
