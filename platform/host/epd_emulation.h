// SPDX-License-Identifier: Apache-2.0
//
// Every number the simulator uses to imitate the ED047TC1, in one place.
//
// READ THIS BEFORE TRUSTING ANY OF IT: not one of these values has been
// measured. They are plausible estimates, taken from the ranges quoted for
// e-paper panels of this class and from the vendor's warning about partial
// refreshing (docs/hardware/board_notes.md). Phase 2c -- board bring-up with a
// scope and a stopwatch on the real panel -- is where each one is replaced by
// a measured figure, and where any that turns out to be meaningless is
// deleted rather than kept for decoration.
//
// They are gathered here, rather than spread through the adapters, for exactly
// that reason: replacing an estimate with a measurement should be an edit to
// this file and nothing else.
//
// What the emulation is FOR is not visual fidelity. The vendor states that
// prolonged partial refreshing leaves residual images and causes irreversible
// damage to this panel. A refresh strategy bad enough to do that must become
// visible on a PC, in the log and in the window, long before it reaches
// glass. Getting the ghosting rate slightly wrong does not undermine that;
// having no ghosting at all would.

#ifndef HAC_PLATFORM_HOST_EPD_EMULATION_H
#define HAC_PLATFORM_HOST_EPD_EMULATION_H

// --- refresh latency -------------------------------------------------------
//
// UNMEASURED. Each mode is quoted as a range; the model interpolates between
// the two by the fraction of the panel the flush covers, so a small partial
// update lands near the minimum and a whole-screen one near the maximum.
// Phase 2c replaces these with timings taken from the panel.

#define EPD_SIM_FULL_REFRESH_MIN_MS 300u
#define EPD_SIM_FULL_REFRESH_MAX_MS 1000u

#define EPD_SIM_PARTIAL_REFRESH_MIN_MS 80u
#define EPD_SIM_PARTIAL_REFRESH_MAX_MS 250u

#define EPD_SIM_FAST_MONO_REFRESH_MIN_MS 40u
#define EPD_SIM_FAST_MONO_REFRESH_MAX_MS 120u

// A clear cycle drives the panel through several inversions; it does not scale
// with area. UNMEASURED, replaced in phase 2c.
#define EPD_SIM_CLEAR_REFRESH_MS 1200u

// How long the inverted image is held during a full refresh. On the real
// panel this is part of the waveform rather than a separate step; here it is
// what makes a full refresh unmistakable in the window. UNMEASURED.
#define EPD_SIM_FLASH_MS 140u

// --- panel constraints reported up to the core -----------------------------
//
// These two reach core/app/update_policy.c through port_display, and are the
// only reason it can protect the panel without knowing which panel it is.
//
// GUESSED, and the more consequential of the two guesses is the budget: the
// vendor warns against prolonged partial refreshing but names no number.
// Twenty is a deliberately conservative stand-in. Phase 2c establishes the
// real figure by ghosting measurement on the panel; until then, treat a run
// that repeatedly hits this limit as a signal about the refresh strategy, not
// as a measurement of the hardware.
#define EPD_SIM_MAX_PARTIAL_REFRESHES_BEFORE_FULL 20u

// GUESSED. The shortest interval between full refreshes the panel is assumed
// to tolerate. Phase 2c replaces it.
#define EPD_SIM_MIN_FULL_REFRESH_INTERVAL_MS 60000u

// --- ghosting --------------------------------------------------------------
//
// Residual charge left behind by a partial refresh, in 8-bit grey units, added
// wherever a pixel changed value. It darkens the displayed image without
// touching the framebuffer, which is what makes accumulated ghosting look the
// way it does on real e-paper: a smudge of what used to be there.
//
// GUESSED, both of them. The shape of the effect is what matters here, not
// the coefficient; phase 2c measures the real decay.
#define EPD_SIM_GHOST_PER_PARTIAL 20u
#define EPD_SIM_GHOST_MAX 150u

// --- presentation ----------------------------------------------------------

// The flushed rectangle is outlined for at least this long even when the
// flush itself is shorter, so a fast partial update is still visible as one.
// A property of the window, not of the panel: no phase-2c measurement will
// change it.
#define EPD_SIM_HIGHLIGHT_MIN_MS 90u

#endif  // HAC_PLATFORM_HOST_EPD_EMULATION_H
