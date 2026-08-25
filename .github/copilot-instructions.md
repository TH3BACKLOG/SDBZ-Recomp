# Agent instructions — SDBZ Recomp

Read this before touching anything. It is short on purpose; the live state lives elsewhere and is linked below.

## What this project is

A **static recompilation** of *Super Dragon Ball Z* (PS2, US NTSC, `SLUS_214.42`) — **not an emulator**. The game's MIPS R5900 code was translated ahead of time into ~30,000 C++ files. A handwritten runtime stands in for PS2 hardware.

```
ELF (SLUS_214.42)  --[ps2_recomp]-->  ps2xRuntime/src/runner/*.cpp   <- machine output, NEVER edit
                                      ps2xRuntime/src/lib/*.cpp      <- handwritten runtime, edit here
                                      ps2xRuntime/include/**/*.h     <- headers, NEVER edit (see below)
```

Mental model: when something breaks, ask **"is the translation wrong, or is the environment incomplete?"** It is the environment ~95% of the time.

## Hard rules — violating these costs hours or days

1. **NEVER clean the build.** No `--clean-first`, no `Remove-Item build*`, no `--target clean`, no deleting `.obj` files. A full `ps2EntryRunner` rebuild is **3.5+ hours** and right now the `build/` tree holds **both arms of an in-flight A/B experiment** (Debug and RelWithDebInfo). Cleaning destroys the experiment, not just time.
2. **NEVER edit `ps2xRuntime/src/runner/*.cpp`, `fn_*.cpp`, or `start_*.cpp`.** Generated from MIPS; the recompiler overwrites you. All behavioural fixes go in **`ps2xRuntime/src/lib/game_overrides.cpp`** — no exceptions.
3. **NEVER edit `.h` headers.** They are included by all ~30,000 runner TUs — one header edit is a full multi-hour rebuild. Use file-scope `static` in a `.cpp`, or `extern` declarations between `.cpp` files. If a header change is genuinely unavoidable, **stop and tell the user the cost first**.
4. **NEVER hand-edit `fn_forward_decls.h`.** It is regenerated from a regex scan of `game_overrides.cpp` on every build. Add the missing `fn_*` stub to `game_overrides.cpp` instead.
5. **NEVER run builds or the game yourself. Give the user the command to run.** Same for `deepseek_*.ps1`. This is a standing user rule, not a suggestion.
6. **NEVER hand-write IOP output values** in `game_overrides.cpp` to fake a result. The IOP modules run for real in an embedded R3000 interpreter.
7. **NEVER run destructive git commands** — no `checkout`, `clean`, `reset`, `stash`, `pull`.
8. **NEVER list or glob inside `runner/`.** 30,000+ files will blow up your context. `Test-Path` and a single targeted file read are fine.
9. **NEVER claim code compiles without reading real build output.**
10. **NEVER create files in the repo root.** Temp files go to a scratch dir. Only `PS2_PROJECT_STATE.md` and the run scripts belong at root.
11. **NEVER commit generated game code.** `src/runner/`, `fn_*.cpp`, `start_*.cpp` and decompile dumps stay out of git. History was already purged once (340 MB → 16 MB).
12. **NEVER write `dir/` in `.gitignore`.** A bare directory entry kills every `!` negation beneath it and silently untracks files you believe are committed. Use `dir/*`.
13. **NEVER regenerate with `ps2_recomp` without asking.** It overwrites all 30,000 runner files and forces the multi-hour rebuild.
14. **NEVER trust a fix you have not seen in a fresh run log.** "It should work now" is not a result.

## Shared memory — read this too

Long-lived facts live outside the repo, in the agent memory directory:

```
C:\Users\mwlab\.claude\projects\f--SDBZ-Recomp\memory\MEMORY.md
```

That index is one line per fact, each linking a `.md` file in the same folder. **Read `MEMORY.md` at the start of a task** and open the specific entries it points to when they are relevant. It carries things the code cannot tell you — closed investigations, retracted theories, user preferences, why a given approach was abandoned.

If you learn something durable, say so in your answer so the user can have it written to memory. Do not write to that directory yourself.

Two entries worth knowing up front:

- **`command_log.md` is the canonical source for paths and invocations.** (An older revision of this file claimed it did not exist — that was wrong.) Read it rather than reconstructing any path from memory.
- **`PS2_PROJECT_STATE.md` is ~500 KB** — larger than most read limits. Grep it or read it in ranges; never load the whole file.

## Where the state is

**[PS2_PROJECT_STATE.md](../PS2_PROJECT_STATE.md)** is the single source of truth and the first thing to read. It is large and append-structured, **newest session at the top under `## Current Phase`** — read the newest session section and the sub-phase tracker table; do not read the whole file.

- `## Active Runner Command` — the exact launch line. Read it verbatim, never reconstruct it.
- `### Sub-phase tracker` — which numbered stage is 🔵 ACTIVE and what its exit test is.
- Closed threads are archived out; the tracker is authoritative for what is still open.

## Build and run (hand these to the user, do not execute them)

```powershell
cd "F:\SDBZ Recomp"
.\build.ps1                      # Debug, default
.\build.ps1 RelWithDebInfo 6     # <config> <jobs>
```

```powershell
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```

⚠️ **`-Exe` is mandatory.** `launch_recomp.ps1` defaults to the *Debug* exe, and `build.ps1` defaults to the *Debug* config. Three sessions were lost to runs that silently measured the wrong build. Always pass both explicitly, exactly as written above.

**Always run the RelWithDebInfo exe.** It executes **109×** more guest work than the Debug build for the same CPU seconds (measured 2026-07-28). A Debug run covers ~1 % of the guest execution for the same wall-clock wait. The Debug tree must not be deleted — it is a control arm — but there is no reason to run it.

Launcher switches worth knowing:

| switch | effect |
|---|---|
| `-Determinism 0` | **required for diagnostic runs.** `det=1` never reproduces the derail. |
| `-RunSeconds N` | time-boxes the run. **Wall clock is therefore fixed and meaningless** — compare CPU seconds and the watchdog `progress` counter. |
| `-HostProfile` | enables the sampling profiler; emits `[hostprof]`. |
| `-HwWatch` | arms the hardware data-breakpoint sweeper. **Default off, and perf numbers from a `-HwWatch` run are untrustworthy** — it suspends every thread 4×/s. |
| `-Exe <path>` | run a specific build (used for the Debug vs RelWithDebInfo A/B). |

`run_log.txt` is **UTF-16** — `Select-String -Encoding unicode`, or you will match nothing and conclude the run failed.

## Build facts that are not obvious

- **`/m:N` on MSBuild is project-level parallelism.** `ps2EntryRunner` is one `.vcxproj`, so `/m` does nothing for it — its 4520 unity TUs go to a single `cl.exe`. The real knob is `cl /MP<N>`, set in `ps2xRuntime/CMakeLists.txt`.
- **This target has no CL `.tlog` files**, so **no build ever resumes**. A partial `.obj` count is not progress and must never be quoted as an ETA.
- **`src/runner/sub_0022F200_0x22f200.cpp` is 278,316 lines** and is an `/O2` optimizer bomb (2 h 09 m in one TU). It is deliberately quarantined out of the unity batch and compiled `/Od /Ob0` in all configurations. See BUG-026.

## Verification discipline

This project has produced a lot of confident wrong answers. The corrections that stuck:

- **Measure, don't infer.** A bounded log cap hides "still running"; an unsampled counter cannot tell slow from dead. Never call recomp behaviour anomalous without a real PCSX2 number to compare against.
- **For "who wrote value X to address Y", use the hardware data breakpoint**, not log-greps or store-site bisection — the latter produced four retractions in ten days.
- **A rise in a thread's CPU time is not "more work done"** until you know what that thread was actually doing.
- **When a log line sits in a retry path, cap it at the emit site, not at the sink.** An unbounded `cerr` in a dispatch-miss loop once produced 225 MB of log and consumed 42% of a run.
- **Watch for stale object files.** This has bitten the project twice: a source edit silently failed to recompile and the "verified" fix was never in the binary. If a probe's fields do not match the source, force-touch the source mtime and rebuild before trusting anything.

## Working with the user

- The user is **dyslexic** — write short, bullet-heavy, low-density responses. Terse beats thorough.
- The user runs all builds and game launches. You prepare commands.
- Do not work on `ps2xStudio` — it is out of scope. "The debugger" means `RecompDebugger.exe` or PCSX2's built-in debugger.
- Prefer one batched request over drip-feeding (e.g. give every disassembly address at once).

## Current situation — 2026-08-04

**Phase 5, Stage 5.9 is the live blocker.** Everything below this heading was rewritten on 2026-08-04; anything you find elsewhere describing Stage 5.7 or 5.8 as open is stale.

**Symptom:** the game boots, reaches MainMenu, and renders — but the screen presents black with a flashing, empty text box. Glyphs are drawn in `PSMT4` (the PS2's standard 4-bit indexed font format) and come out blank.

**The finding that relocated the whole investigation.** The `[gifsrc]` probe in `PS2Memory::submitGifPacket` shows a 256 KB Path3 upload issued every frame from EE RAM `0x89d400` that is **entirely zero**, and the surrounding 384 KB (`0x88d400`–`0x8fd400`) is zero as well. A blank glyph sheet would still sit in a populated neighbourhood; 384 KB of consecutive zeros means **the asset was never loaded into RAM at all**. Stage 5.9 is therefore an **asset-load problem, not a render problem**.

**Closed — do not reopen any of these:**

| Thread | Verdict |
|---|---|
| Performance / framerate | CLOSED 07-28. It was the Debug build: RelWithDebInfo does **109×** the guest work per CPU second. |
| `pc=0x421f10` self-loop (Stage 5.7) | RETRACTED. A `jal` in straight-line code; `stuckSecs` was a sampling artifact. |
| `gstate@0x5e6b3c` "frozen state machine" | RETRACTED. The watched words are static singleton pointers in `.data`, never reassigned. |
| BUG-009 `rpc_handle_valid` / `SifBindRpc` | CLOSED, BENIGN. It is `sceSifCheckStatRpc`, a BUSY-check; our always-0 answer is correct. Four sessions lost to a misleading function-map name. |
| Stage 5.8 VU1 microcode | PASSED 08-04 (`tme=1`, `tex0.tbp=0x2b60`). VU1 sitting idle is **correct** — MainMenu is all 2D sprites. |
| DISPFB page-flipping / dropped frames | DISPROVEN 08-04. `[gsreg]` shows DISPFB1 and DISPFB2 alternating `0x1070`↔`0x1000` in lockstep with FRAME.FBP, 32× each. The flip works. |
| `tme=0` in `[gs:frame-change]` | SAMPLING ARTIFACT. That probe fires on FRAME/TEX0 *change* and happens to snapshot the untextured `prim=0x6` clear sprites (TME is bit 4). The rasterizer honours per-primitive `prim.tme` correctly. |
| GS write path / swizzle / FBMSK / Z-alias / addressing | All exonerated. Interleaved PSMT8 uploads land non-zero, so DMA and GIF plumbing are fine. |

**Next step (wired, not yet run).** `PS2X_HWWATCH_ADDR` was added to `game_overrides.cpp` on 08-04 so the existing DR0/VEH hardware-watchpoint machinery can be parked on a fixed address instead of a guest-derived one. See [COPILOT_PLAYBOOK.md](COPILOT_PLAYBOOK.md) Plan A for how to run it and how to read the three possible outcomes.

Fast-iteration method: use `ps2xTest`, not a full game run —
```powershell
& "F:\SDBZ Recomp\build.ps1" Debug -Test
& "F:\SDBZ Recomp\build\ps2xTest\Debug\ps2x_tests.exe" Sif
```
Always pass a filter — a bare invocation hangs and silently skips ~40 suites (BUG-031). Baseline is 27/28 passing; the 1 failure (BUG-032, `sceSifSetDma` multi-descriptor validation) is untriaged and probably unrelated.

**Do not re-litigate anything in the closed table above.** Any statement reopening those threads is obsolete, wherever you find it.

**Read the ★★ HANDOFF section (newest, top of file) of [PS2_PROJECT_STATE.md](../PS2_PROJECT_STATE.md)** for the full closed-threads table and ordered next steps.

## Pick up work here

[COPILOT_PLAYBOOK.md](COPILOT_PLAYBOOK.md) holds the ordered, self-contained work plans for this project — what to do, in what order, with the exit test for each. Start there when the user asks you to make progress and has not named a specific task. It also carries the conventions (probe idiom, header-avoidance pattern, how to read a run) that this file only summarises.
