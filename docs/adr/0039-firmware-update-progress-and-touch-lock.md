# 39. Show firmware update progress and lock touch until it ends

Date: 2026-10-23

## Status

Accepted, at the owner's request. Amends ADR 0037's background update with
visible progress. Verified by host tests and a BSEC firmware build. The owner
confirmed the update screen and touch lock work on the board on 2026-10-23
after installing build `b358b13`. No detailed log or timing accompanied the
confirmation; the new duration remains to be measured.

## Context

An update from the card takes about twenty seconds on the board, but leaves
the ordinary screen up without indicating what is happening. Touch can still
switch screens. The updater already knows which stage it is executing and
how many bytes it has written; it runs on the card's task, while only the
pump owns the framebuffer and panel.

## Decision

- A dedicated **Firmware update** screen replaces the ordinary screen as soon
  as an image file is found. It shows the actual stages: checking the image,
  writing firmware, verifying the image, preparing the restart, then
  restarting. Failures and rejected images have their own result messages.
- The progress bar and percentage measure **bytes successfully written out
  of the file's size**. `100% written` is not a claim that verification or
  boot selection has succeeded. Bytes are also shown in KiB. Unknown or empty
  files show zero; the percentage is clamped to 100.
- Write milestones are reported at ten percentage-point thresholds. Each
  stage transition is also reported. Screen entry is full; content changes
  are partial without waiting for the sensor cycle or the partial budget.
  A panel without partial mode uses full refreshes at these milestones.
- The updater sends a copied progress value to a dedicated one-entry queue
  and waits for the pump's acknowledgement of the display attempt. It does
  not erase/write flash while the panel is displaying that milestone. The
  pump keeps polling sensors and the reporter. The card starts only after
  the pump has started, including at boot, so its first mount can show an
  update without waiting for a task that does not yet exist.
- Touch events are discarded during an active update, including restart.
  Each progress report resets the tap recognizer. Queued events from before
  a failure are discarded too, so an old gesture cannot dismiss the result.
  The diagnostics timeout cannot replace the update screen.
- A failure or refusal unlocks touch: a tap returns home, or the result goes
  home after ten seconds. Both returns use a full refresh. If the display
  cannot show a milestone, the update stops before proceeding; the source
  file stays for another mount. Once the boot slot has been selected, a
  failed final display attempt does not prevent restarting into that slot.

No new port or dependency: the updater reports application events; rendering
and touch policy are pure core code shared with the host tests.

## Consequences and validation

Updates take longer by the cost of the initial full refresh and the partial
milestones. The new duration is to be measured on the board; the old 19.4 s
measurement does not include this UI. Cache stalls from flash operations can
still delay sensors, but a progress flush itself is acknowledged before the
next flash operation.

`test_screen_controller` covers every active stage, ignored taps, a gesture
started before the update, queued gestures after an error, a failure's tap
and timeout, boot directly into the update screen, immediate progress and
panels without partial mode. `test_firmware_update_progress` covers empty,
partial, complete and extreme-size byte counts without overflow. Seven
golden frames document the stages and both error outcomes at 960x540.

The first installation of this feature still runs the old updater, so it
cannot show this screen. After installing the new firmware, copy an image
again (the same version is allowed) to see the progress and touch lock. Check
both a successful update and a rejected image, and confirm that tapping
during writing cannot switch screens or replay after the result.
