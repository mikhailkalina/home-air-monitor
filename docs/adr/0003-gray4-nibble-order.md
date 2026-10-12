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

## Amendment (phase 2b): the FastEPD backend does not get the memcpy

The warning in the last bullet above fired. `docs/adr/0007-epaper-backend.md`
chose FastEPD (`bitbank2/FastEPD`) as the display backend, not EPDiy, and
FastEPD's own GRAY4 packing was checked against this decision before any
driver code was written, per that bullet's own instruction.

**It does not match.** `bb_ep_gfx.inl`'s `bbSetPixel16Clr()` — the function
that actually backs FastEPD's `BB_MODE_4BPP` — packs **even x into the high
nibble, odd x into the low**, which is the exact opposite of the order this
ADR chose. The stride still matches (`native_width >> 1`, the same 480 bytes
at width 960 this ADR relies on), and the polarity still matches (0 = black,
15 = white in both — confirmed against `core/ui/view_model.h`'s
`VM_INK_PRIMARY = 0u` / `VM_INK_BACKGROUND = 15u` and FastEPD.inl's own
"0-black to 15-white" comment), but the nibble position does not, and no
comment in a header can fix that the way this ADR's own reasoning intended.

**This ADR's decision is not reversed.** The core's GRAY4 layout still
matches the ED047TC1 hardware reference, which is what the "Decision" section
above actually argued for — the core's byte layout has exactly one hardware
reality behind it, and FastEPD's own internal convention (which its
multi-pass grayscale LUT construction is free to choose independently, since
nothing outside FastEPD reads `pCurrent` directly) is not that reality. What
changes is the "Consequences" section's premise that a display adapter can
always hand this buffer to the panel driver with a bare memcpy: for this one
backend, it cannot, and paying a conversion cost is no longer optional the
way "Option A" was in 2026-09-05's Decision above.

**The cost this pays is much smaller than "Option A" would have been,
because it no longer needs to be a full-frame walk.** Phase 2a's dirty-rect
tracking did not exist when this ADR was first written; it does now, and
`flush()` already receives the rectangle that actually changed —
`platform/esp32_t5s3/adp_display.c`'s `swap_nibbles_into_fastepd()` swaps
nibbles only inside that rectangle. A routine partial redraw is a small
fraction of the panel (824×228 observed on hardware during phase 2a's
bring-up run; smaller still for a single changed field in the simulator), so
this is not "walk 253 125 bytes on every flush, forever" — it is bounded by
the same rectangle the ghosting budget already cares about, and only a full
or clear refresh pays the whole-buffer cost.

Consequences for a future reader:

- `core/ui/framebuffer.c` is unchanged. The nibble order this ADR chose is
  still correct for the core and for any future EPDiy-family backend; it is
  specifically FastEPD that needs a conversion, and that conversion lives
  entirely in `platform/esp32_t5s3/adp_display.c`, on the adapter side of the
  `port_display_t` boundary this ADR always intended conversions to live on.
- The swap-site comment in `adp_display.c` cites this amendment and
  `bb_ep_gfx.inl`'s exact function name and line, so a reader who finds the
  swap and wonders why it exists does not have to re-derive the mismatch from
  scratch.
- If a future backend change moves off FastEPD, re-check this amendment the
  same way this amendment re-checked the original decision: read the new
  backend's own pixel-packing function, do not assume either convention.


## Second amendment (phase 2b): EPDiy agrees, and the swap is gone

The amendment above ends by saying that a future backend change should
re-check it by reading the new backend's own pixel-packing function rather
than assuming either convention. That is what happened, and the answer put
the original decision back.

`epdiy.c`'s `epd_draw_pixel()` packs:

```c
uint8_t* buf_ptr = &framebuffer[y * epd_width() / 2 + x / 2];
if (x % 2) {
    *buf_ptr = (*buf_ptr & 0x0F) | (color & 0xF0);   /* odd x  -> high nibble */
} else {
    *buf_ptr = (*buf_ptr & 0xF0) | (color >> 4);     /* even x -> low nibble  */
}
```

Odd x in the high nibble, even x in the low, indexed at `width / 2` bytes per
row. That is `core/ui/framebuffer.c`'s order and stride exactly — which is no
coincidence, since this ADR chose it to match the ED047TC1 reference layout
and EPDiy matches the same hardware.

**Consequences.** `swap_nibbles_into_fastepd()` is deleted along with the
rest of the FastEPD integration. `platform/esp32_t5s3/adp_display.c` now
hands `framebuffer_t::pixels` to `epd_draw_base()` by pointer: no copy, no
conversion, nothing walked per flush. The memcpy-free path this ADR's
original decision was made for is not merely restored but improved on — there
is no copy at all.

Two things this retires:

- the amendment above is now history rather than active guidance. It is kept
  because the reasoning is still the right reasoning, and because a reader
  who finds FastEPD in the git log should find out what it required;
- `docs/hardware/board_notes.md`'s "Buffer layout" section, which has said
  since phase 1a that the matching nibble order lets the adapter hand the
  buffer straight to EPDiy, turns out to have been right all along. It needs
  no correction — only the FastEPD detour did.
