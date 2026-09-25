# Handoff note -- 2026-09-23 session end (Part 161, preliminary)

## Headline

**Training mode was reached, same bug reported, and video evidence shows something
sharper than the "clean Y-flip" model Parts 158-160 have been chasing.**

The recomp's own training-mode video (`2026-09-23 15-23-09.mp4`, Goku vs Vegeta):
arena flythrough and fight HUD render **correctly**. But the 3D viewport itself
shows **no character geometry at all** at t=228-252s (correct floor grid, then
flat black), then only two tiny (~20-30px) sprite fragments clipped in just under
the HUD by t~366s -- never a full-size figure anywhere sampled, right-side-up or
upside-down. The supplied PCSX2 reference screenshot shows both fighters full-size,
standing on the floor, filling most of the screen.

This looks more like the character-model draws are positioned/scaled almost
entirely **outside the visible viewport** (a sliver clipping in) than a pure
framebuffer-wide axis flip -- the arena/HUD draws are unaffected. **Not confirmed**
(crops too small to prove orientation directly), but it's a materially sharper,
more repeatable symptom than what Parts 158-160 investigated.

Full detail: `PS2_PROJECT_STATE.md` **Part 161** (top of file); memory
`project_upside_down_framebuffer.md` section 13.

## The Part 161 plan is still the active plan

Plan file: `C:\Users\mwlab\.claude\plans\shiny-jumping-abelson.md` --
"Stop reconstructing the transform; read what VU1 actually sent." Read raw
`VuCapRecorder` GIF-packet captures directly instead of the VUMAT-dump +
matrix-reconstruction pipeline Parts 158-160 used (which already produced one
decode bug, section 12b).

**Step 1 (prove the capture/replay round trip on a disposable capture) was handed
to the user this session but completion is NOT yet confirmed.** Command handed
over: `PS2X_VUCAP=<path>`, `PS2X_VUCAP_AT=0`, `PS2X_VUCAP_FRAMES=9000`, FULLMEM off,
targeting the auto-playing intro cutscene (`roundtrip_test2.vucap`). No rebuild
needed -- `VuCapRecorder.obj` is already linked into the current exe (relinked
2026-09-23 01:05:59). Next session: ask whether this ran, then:
```
python build_scripts/vucap.py roundtrip_test2.vucap        # sanity: clean loss check
```
then run `ps2x_vucap_replay.exe` against it (agent-run, allowed) and confirm 0
mismatches. This is the one previously-untested link in the whole plan.

## New payoff from this session -- a real capture-window estimate

Previously Step 3 needed the user to eyeball an in-match timestamp for the bug
moment. The training-mode video gives a concrete wall-clock shape for the same
repro path (character select -> loading -> arena flyover -> HUD-up -> broken
characters): HUD appears ~t=220s, garbled character fragments visible by ~t=360s.
**Next training-mode capture should bracket `PS2X_VUCAP_AT=220,280,340`** (seconds
since launch), a few hundred `PS2X_VUCAP_FRAMES` each, FULLMEM off.

`build_scripts/vucap_decode_kick.py` (Step 2's decoder, untracked file, already
written and reviewed, not yet run against any real capture) is ready to point at
whatever this capture produces.

## Also open, lower priority

- The **title-screen consumption mystery** (`project_pad_input_requires_real_gamepad.md`):
  keyboard input demonstrably reaches the guest pad buffer (`[pad] change` log,
  real PAD_START transitions) but the title screen historically never visually
  advanced on its own. The user evidently got past it to reach training mode this
  session by some means -- not investigated how. Revisit only if it starts
  blocking repro again.
- Part 160's two open VUMAT/VFLIP mismatches (dump 6, dump 10 -- section 12d/12e)
  are still unresolved but now lower priority than the VuCap plan, which sidesteps
  that whole reconstruction pipeline.

## Uncommitted edits (nothing committed this session -- commit only if asked)

Same working tree as last session end (`git status` shows the Part 156-era diffs
still uncommitted): runtime, tests, `build.ps1`, `build_scripts/presets.py`,
`PS2_PROJECT_STATE.md`, `HANDOFF_NOTE.md`, plus untracked
`build_scripts/vucap_decode_kick.py` (Step 2 of this plan) and various `recovered/`
files from earlier decompilation work. Nothing from this session's analysis
touched the repo -- it was read-only (ffmpeg frame extraction, image reads).

## Do NOT

- Don't treat the training-mode video finding as proof of a Y-flip -- the sampled
  frames show absent/near-absent geometry, not inverted geometry. Say what was
  observed (tiny fragments, wrong position) rather than asserting "upside-down"
  as confirmed for this repro.
- Don't re-run this agent's own analysis commands (ffmpeg extraction paths,
  scratchpad frame dumps) -- they were session-scratchpad, already done, not
  reusable across sessions (scratchpad is session-specific).
- Don't build or run the recomp exe from this agent -- user runs every build and
  every launch, per `feedback_user_runs_builds` / `feedback_delegated_x64dbg_recomp_control`.
