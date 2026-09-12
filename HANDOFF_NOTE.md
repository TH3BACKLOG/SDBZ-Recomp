# Handoff note -- 2026-09-11 session end

## What happened this session
Resumed the SofDec/loadscreen `st=0xA` stall investigation. Corrected a prior session's
"case 10 is a dead end" conclusion, then the user ran the higher-cap trace that was queued
up (`0x420260:0x4000,0x2c1bb0:0x4000,0x2c1830:0x200`) and it gave full coverage of the stall
for the first time. Root cause is now traced end-to-end, not inferred:

- `camera_fade_is_active` (0x2C1BB0) reads global byte `[0x500E50]`; returns "active"
  unconditionally when it's `2`.
- That byte is pinned at `2` for 60/61 samples at the exact blocking call site, for the
  whole stall.
- The writer (`camera_fade_set`, 0x2C1830) was called only 15 times in the entire run. The
  last call (from `st4b`) sets the byte to "animating" (2) and nothing ever calls it again
  for that channel to finish the animation -- so it never clears.
- `st4b` is a second victim of the exact same stuck global, not a separate bug.

Full detail: `memory/project_sofdec_init_never_runs_5618b4.md` (section "cont. 5"), also
folded into `PS2_PROJECT_STATE.md` as **Part 112**.

**Note**: this is a *separate* diagnostic thread from Part 111's `[0x500728]`/`RenderDispatch`
worker-resume finding -- both are tracked, not yet reconciled into one story.

## Next step (no build required)
Four candidate per-frame "re-tick" wrapper functions were never reached with channel=2
during the stall: `wrap_state_byte_transition_j` (0x3F9D68) and clones `_b` (0x3FF5DC),
`_c` (0x41AC78), `_d` (0x421408). Decompile each and `get_xrefs_to` each to find their
callers -- one of them should be the missing per-frame driver, either never reached at all
or reached but gated on something false during the stall.

## Standing rules unchanged
User runs all builds/launches. No runner-file edits. Fixes go in `game_overrides.cpp` only.
See `PS2_PROJECT_STATE.md` "STOP" section for the full list.
