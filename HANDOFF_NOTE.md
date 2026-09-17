# Handoff note -- 2026-09-16 session end (warped 3D)

## Headline
**The smeared 3D is fixed.** Two GS bugs, built and run: the user sees no smearing.
The game reaches the **Ranking** screen. New open item: its cloud sky dome looks upside down.

Full detail: `PS2_PROJECT_STATE.md` **Part 119**; memory `project_capp_scene_identification.md`;
plan `C:\Users\mwlab\.claude\plans\playful-juggling-fog.md`.

## Done this session
- Guest FP rounding made PCSX2-exact (`Ps2ApplyGuestFpMode`, `FPU_SQRT_S` -> nearest).
  Projection matrix now bit-exact at the title. **Not the cause of the warp** (still smeared after).
- Fix A `ps2_gs_rasterizer.cpp` drawTriangle: perspective divide was cancelled -> affine textures.
- Fix B `ps2_gs_gpu.cpp` vertexKick: ADC=1 / XYZ3 kicks skipped the strip rotation -> stale triangles.
- Both `cl /Zs` clean. Built 06:14, run 06:21. Smearing gone.
- Pre-fix copies of both GS files are in the session scratchpad only (not in the repo).

## Uncommitted core-file edits (all approved)
`ps2_runtime_macros.h`, `ps2_runtime.cpp`, `game_overrides.cpp`, `ps2_gs_rasterizer.cpp`, `ps2_gs_gpu.cpp`.
Not committed -- commit only if asked.

## Next -- Ranking screen sky (NOT yet proven to be a bug)
1. User: let PCSX2 run the attract loop to the **Ranking** screen. Take a screenshot there and
   a GS dump (lands in `PCSX2\snaps`). Take a recomp screenshot too.
2. Compare. If PCSX2's sky is right way up:
   - `python build_scripts/gsdump_parse.py <dump>.gs.zst --emit-replay gsdump/ranking.gsr`
   - replay through our rasterizer (`PS2X_GSDUMP` / `PS2X_GSDUMP_OUT`). Needs a ps2xTest build.
     Part 118 says `ps2x_tests` no longer builds (6 stale test files) -- check first.
   - Replay upside down = our rasterizer. Right way up = GS emission upstream.
3. Already on disk: `logs/vucap/demo10` = PCSX2 VU1 capture of the Ranking 3D background (Part 118).
4. Also: does PCSX2 show characters or a platform on Ranking? If yes, that is a separate bug.

## Other open items
- EE `div.s`: PCSX2 rounds nearest, ours chops (788 inlined `/` sites -> generator change).
- Re-measure the old 67.5% vs 12.2% Z-saturation premise (it was scene-mismatched).
- Flagged GS items in Part 119 section 3 (PRMODE, XYOFFSET sub-pixel, V4-5 `<<3`, etc.).

## Do NOT
- Don't chase the warp in the matrix/FPU path again -- falsified.
- `PS2X_GSHISTORY_DUMP` records from boot with no start gate: useless for a late screen
  unless a start-time option is added (core file -- ask first).
- Clear `PS2X_VUROUND` in the shell before runs (it lingered once).
