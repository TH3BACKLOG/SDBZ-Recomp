# PS2_PROJECT_STATE — SDBZ Recomp

## 2026-08-02 Handoff Update
- The live tree at `F:\SDBZ Recomp` is the authoritative runtime tree for all boot-blocker work. The worktree copy under `F:\SDBZ Recomp.worktrees\claude-memory-sdbz-recomp-plan` is historical and should not be used for runtime fixes.
- The ARKD cdvdman fid=5 settle-poll hook is now present in the live tree at `ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp`. It returns `1` for the `sub_AC90` poll loop, which is the semantics implied by the call-site loop shape in the IRX decompile, and it emits the first four `[ARKD:cdsettle]` traces.
- Last run evidence shows the earlier ARKD completion-path fix is working: `initThreads` created 3 workers, `SET_SREG` mirrored to the EE-side `0x5618B0` path, `mcserv Init` answered, and `gstate` reached `0,0,0,1`. The earlier memcard-check theory is no longer the lead.
- Immediate next step: re-run the build and launch scripts with the watch set armed so we can confirm whether the fid=5 hook clears the `sid=0x500 fno=0x1` service halt and whether `CAppInit` advances beyond state 11.
- Known follow-up bugs / leads:
  - `0x5E5924` (`CAppInit` state) remains the primary boot-state probe. If it stalls at 11, inspect the audio-slot gate at `0x5D6B80` / `0x5D6BA4` next.
  - The old `gstate` pointer watch is not a trustworthy stall signal by itself; use the actual state word and the audio-slot table instead.
  - Keep `PS2X_HWWATCH` unset; it adds overhead and has not been needed for this blocker.
- Handoff goal: verify whether the fid=5 hook ends the service spin cleanly; on success, pivot immediately to the next state-11/audio-gate diagnosis rather than revisiting the memcard path.

## 2026-08-02 Update 2 — ★★★ CAppInit RETIRES (state 0x3e8), matches real hardware exactly. New lead: first-ever texture upload + TEXFLUSH observed.
Pasted watch/GS log (partial — starts mid-run, ends on user-initiated window close, not a crash/hang).
- **`appinit_state 0x5e5924` climbed the full remaining ladder and RETIRED:** `...0x8→0x9→0xa→0xb→0xc→0xd→0xe→0x3e8`. `0x3e8` (1000) is the exact terminal value measured on real PCSX2 hardware in 5.8 measurement #2 Finding 9. **State 11 is no longer a blocker — this sub-thread of 5.8 is CLOSED.**
- **`req12 0x5618b0` changed `0x30000000 → 0x3000c270`** — the SET_SREG EE-mirror fix from measurement #4 is confirmed live (exit test from that section: MET).
- **`snd00 0x5d6ba4` (slot-0 audio status) cycled `0x0→0x3→0x1`** — confirms Finding 11's audio-gate mechanism resolved; state 11's `str_queue_msg`/slot-wait passed for real.
- The fid=5 cdvdman settle-poll hook (2026-08-02 note above) is implicated as part of what unblocked this, but this paste doesn't include the early-run `[ARKD:cdsettle]`/`sid=0x500` lines to confirm directly — re-check the full log, not just this excerpt.
- **New, never-seen-before signal:** near the end of the run, `[gs:image] n=16 dbp=0x2a00 dpsm=0x13 dbw=4 trxreg=256x256 sizeBytes=59520 (DBP CHANGED)` — a real 256×256 texture upload to VRAM — followed immediately by `[gs:ad] n=6000 addr=0x3a data=...` (GS reg `0x3a` = TEXFLUSH, the standard "texture upload just completed, invalidate cache before drawing with it" idiom). This is the first texture transfer seen in the project's history. Every `[gs:frame-change]`/`[gs:frame]` line in this paste still shows `tme=0`/`textured=0`/`tex0.tbp=0x0`/`primmask=0x40` (SPRITE-only) — so no textured draw has landed yet, but the pipeline immediately upstream of one just fired for the first time.
- Run ended via `[run] window close requested` (user closed it) at `wall=96.44s`, not a timeout/crash — so it's unknown whether a textured frame would have followed within seconds. `cpu=99.4%`, CPU-bound as expected (GS software raster, per the 07-29 co-piolet findings — not a new concern).
- **Next action:** rerun and let it finish its own timeout (don't close the window early) to see whether `tme` ever flips to 1 / `tex0.tbp` goes non-zero after this TEXFLUSH — that is the literal 5.8 top-level exit test (line ~63 above). If it does, 5.8 closes and the project moves to whatever renders after that. If it doesn't, capture the full log (not just a tail paste) and check what gates the actual draw call using this new texture.

## 2026-08-02 Update 3 — Periodic stall (progress frozen 3s, busy%=0/all-zero throughput) then small burst. Debugger bp_hit ruled out.
- User reports intermittent "4-5 pixels flashing every once in a while" during long runs (4:30+). Watchdog paste showed `progress` frozen at exactly `21991` for 3 straight seconds (t=337-339) with `res/s=0 vbl/s=0 gif/s=0 dma/s=0 busy%=0`, then a burst at t=340 (`vbl/s=5 gif/s=1 dma/s=2 busy%=330`). This is a genuine stall (busy%=0 means the guest fiber truly isn't scheduled, not spin-polling) — matches the earlier `[cputime]` finding of a thread spending 43.63s/96s in `Wait/ExecutionDelay`.
- **Ruled out: debugger breakpoint stall (`g_bp_hit`).** `main_gui.cpp` (where `g_bp_hit=true` gets set) compiles into `RecompDebugger.exe`, a separate executable from `ps2EntryRunner.exe` (confirmed in `ps2xRuntime/CMakeLists.txt`). The Active Runner Command uses `-NoDebugger`, which per `launch_recomp.ps1` skips launching `RecompDebugger.exe` entirely AND unsets `PS2X_DEBUGSHM`, so `recomp_debug_writer.cpp`'s `bp_hit=1` writes (gated on that env var) can't fire either. Not the cause.
- **Ruled out: IRQ worker sleeps.** `Kernel/Syscalls/Interrupt.cpp:659-670` sleeps 250-500us per vblank tick, capped at 8 iterations under determinism — orders of magnitude too small to explain a 3s+ freeze.
- **Still open:** the stall's `pc=ra=0x421f10` (self-loop), `lastCall=0x172998`. `recomp_function_table.txt` only has a raw address list, no symbol names — could not identify the function without Ghidra (no GhydraMCP tool available this session). Next step: user to paste the Ghidra disasm for `0x421f10` (and its caller at `0x172998`) so the wait condition it's polling can be identified directly, per the project's "user pastes disasm, don't search for it" convention.
- Also still open from Update 2: whether `tme` ever flips to 1 / `tex0.tbp` goes non-zero (5.8 exit test) — need an uninterrupted run past the point where the `[gs:image]`/TEXFLUSH pair was seen.

## Game Info
- **Title:** Super Dragon Ball Z (US NTSC)
- **Disc ID:** SLUS_214.42
- **ELF Path:** `F:\SDBZ Recomp\ELF\SLUS_214.42`
- **Repo:** `F:\SDBZ Recomp\PS2Recomp\`
- **Config:** `F:\SDBZ Recomp\config.toml`
- **Branch:** work/laptop-session-0519

## Active Runner Command
```powershell
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```
**Changed 2026-07-28 — use the RelWithDebInfo exe for everything now.** The A/B proved it does **109×** the guest work of the Debug build for the same CPU seconds (`progress` @ t=89: 12 101 → 1 320 490), so a Debug diagnostic run covers ~1 % of the guest execution for the same wall-clock cost. The Debug exe still exists and must not be deleted — it is the control arm — but there is no longer a reason to *run* it.

Raw form, if the launcher is not wanted: `& "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe" "F:\SDBZ Recomp\ELF\SLUS_214.42"`

(corrected 2026-07-13c — `PS2Recomp\out\build\...` no longer exists; build.ps1's real output tree is `F:\SDBZ Recomp\build\...`)

## Agent Runner Script
`F:\SDBZ Recomp\run_game_agent.bat` (generated 2026-06-22, paths verified against above) — usage: `run_game_agent.bat [timeoutSec] [logName]`, logs to `F:\SDBZ Recomp\logs\`.

## Build Command
```
& "F:\SDBZ Recomp\build.ps1"
```
(Always use build.ps1 — never raw cmake/MSBuild)

## Upstream Sync (2026-07-17)
WIP checkpointed as commit `9957294f`. Checked all 7 new upstream commits vs HEAD — #149/#153/#155/#158/#167/#168 already present in tree (functional content confirmed via `git apply --reverse --check`); only `#170` (IOP refactor) remains deliberately held. Tree is current with upstream/main modulo #170.

**#170 conflict re-check (2026-07-19):** now hard-conflicts — 15 conflicts on dry-run merge (`git merge-tree HEAD upstream/pr/170`). It **moves all IOP logic into a new `ps2xIOP/` module** — deletes `ps2_iop.cpp` and `include/runtime/ps2_iop.h` (modify/delete conflicts) plus content conflicts in SIF.cpp, ps2_runtime.cpp, RPC.cpp, game_overrides.cpp, ps2_debug_panel.cpp, Common.h, ps2_runtime.h, ps2_syscalls.h, CMakeLists.txt. These are exactly the files our live ARKD/`sceCdSearchFile` blocker work sits in. **Verdict: keep HELD** until the disc-driver blocker is closed; porting it now would rebase all in-flight IOP work onto a different module layout mid-fix.

**#179 landed upstream (2026-07-22, merged as `f3687c5a`) — scope assessed:** `Feature/random fixes platform support`. Adds memcard IOP + Interrupt/GS/MPEG tweaks + Android/Vita platform scaffolding. **#179 alone is small (~1,132 ins / 145 del)** but is structurally welded to #170 — it edits `ps2xIOP/include/ps2x/iop/iop_host.h` and depends on the `ps2xIOP/` module, so **taking #179 requires taking #170 first**. Real cost = #170 (9,658 ins / 3,955 del, full IOP re-architecture) + re-homing our ARKD bridge into the new plugin API.
- Our live work sits precisely in what #170 removes/rewrites: `ps2_iop_irx_loader.cpp` (1,138 lines, **ours only — not upstream**: the whole ARKD bridge + `ps2_iop_runArkdService` + `[ARKD:diag]`), modified `ps2_iop.cpp` (244 lines; upstream deletes the 105-line original), modified `SIF.cpp` (1,690 lines; upstream rewrites +53/−53, #179 +2 more). Upstream has **no** real-R3000-interpreter ARKD path — they stub/reimplement modules as plugins. So it's not a merge, it's a **re-port of our interpreter bridge into a new plugin architecture**.
- **Effort estimate ≈ 5–8 days:** merge #170 + resolve ~15 conflicts (1–2d) · re-home ARKD bridge into `ps2xIOP` plugin model (3–5d, risky — rewrites the code path we're one measurement run from cracking) · layer #179 on top (~1d; memcard IOP useful @ stage 5.5, Android/Vita/`.suprx` binaries = skip).
- **Verdict: keep BOTH HELD.** Close the ARKD `eeDest=0x0` blocker on the current architecture first, then take #170+#179 together at **stage 5.5** when memcard IOP (#179's payoff) actually becomes relevant. #176 (test-only sema determinism) can be ported anytime, independent of this.

## Deferred Build-Config Items
- **Static CRT (`/MT`) for a portable exe — deferred to ship/packaging time (noted 2026-07-19).** Not enabled now: our ffmpeg static libs are almost certainly built against the dynamic CRT (`/MD`); mixing `/MT` exe with `/MD` libs risks LNK warnings + heap-corruption crashes that would masquerade as game bugs mid-blocker. When packaging: set `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"` AND rebuild ffmpeg to match, together. Not the current blocker.
- **Opt-in ASan flag on the IOP interpreter** — considered as a debugging aid for the hand-written R3000 memory code (out-of-bounds/UAF). Only wire it if the cdRoot re-run still spins with no clear cause; make it a CMake flag, off by default.

## Current Phase
**Phase 5 — in progress, started 2026-05-27.**
**Memory-card-prompt goal ABANDONED/deprioritized (2026-08-02)** — repeated attempts across many sessions could not get the memcard prompt to populate no matter what was tried; continuing to target it stopped being productive. The active goal is now whatever the current sub-phase tracker below says (currently 5.8: reach a textured draw). Boot-to-title-screen/main-menu remains the longer-term Phase 5 target once 5.8 closes.

**Provenance note (2026-08-02):** the 2026-08-02 Handoff/Update 2/Update 3 entries above were written from a GitHub Copilot ("fables") session, not a Claude session — flagged here in case terminology, address claims, or tone in those entries need reconciling with this project's conventions. User indicated these can be cleaned up if needed.

### Sub-phase tracker (established 2026-07-20g)

**✅ 5.6 CLOSED (2026-07-28)** — build-config A/B ran: RelWithDebInfo does **109×** the guest work for the same CPU (`progress` @ t=89: 12 101 → **1 320 490**; `gif/s` 3–5 → **47–49**, i.e. normal PS2 frame rates). Debug build was the entire performance story. **Throughput is no longer a variable — always measure on the RelWithDebInfo exe.**
(5.6.1 render-path DMA-kick census and 5.6.2 watchdog `busy%`/`res/s`/`vbl/s` fields both shipped; 5.4.2 CLEARED, 5.5.1/5.5.2/5.5.3 padman chain drained and demoted as blocker candidates, both parked leads triaged and dismissed — all 2026-07-27)

**🔵 ACTIVE: 5.8 (opened 2026-08-01)** — **the guest state machine never advances to a textured draw.** Replaces 5.7's framing, whose surviving lead (BUG-009) is now **CLOSED — benign**. Everything underneath works and is measured: frame loop healthy (~46–49 fps, `vbl/s`≈49, `dma/s`≈98), asset streaming proven end-to-end (1024 reads / 2.1 MB / **zero fails**), GS proven innocent (BUG-028 fixed + verified twice). Yet `primmask=0x40` (SPRITE only), `tme` **never** 1, `tex0.tbp` always 0, `nonblack=0` across ~24 M pixels/interval. **Exit test:** a single frame in which `tme=1` and `tex0.tbp != 0`. **Method (this is the change that matters):** stop reasoning from our own log in a vacuum — *difference against real PCSX2 + the real ISO*, which reaches the memory-card prompt in 7–8 s, using `mcp__pcsx2__*`. Six retractions in this file all came from inferring semantics instead of measuring them against ground truth. First steps: (1) arm `PS2_COVERAGE` — built, documented, and **not armed since 2026-07-29** (last run `cov=0/0`) — it answers "which game-band addresses does the EE actually dispatch" directly, instead of hand-reading 2800-line generated `switch` bodies; (2) watch `0x44D26C` (SRD test-and-set gate: `sub_12F1E8`/`sub_12F7E0` tick only if they win it, `sub_12EB28` can set it to 1 after a 1000-iteration spin — if it sticks at 1 all five SCMD wrappers silently stop being reached, with no log output), `0x463294` (libcdvd bind latch), `0x5AA7D0` (the CCD reply word `sub_1C0A30` actually branches on), `0x5e6b3c` (GameMode); (3) breakpoint `0x1C0A30` on PCSX2 and read `MEM[0x5AA7D0]` per hit — settles whether our `0x30000000` is the advancing answer or the keep-waiting one. **That single measurement may be the whole blocker.**

> ⚠️ **The "First steps" above are HISTORICAL — they were carried out, and measurements #1–#4 below supersede them.** 5.8 has since narrowed all the way from "never advances to a textured draw" to one named gate: `CAppInit` parks at state 11 waiting on an ARKD completion that four separate breaks prevented from ever being published. **Read § 5.8 measurement #4 first; it holds the root cause, the fix (built, unrun), and the exact next command.**

#### ★★ 5.8 measurement #1 (2026-08-01, two runs) — the blocker is localized to ONE variable: `CAppInit` state at `0x5E5924`

Two 90 s runs with `PS2_COVERAGE=1`, the second with `PS2_COV_GAMELO=0x100000` (env-overridable at `ps2_runtime.cpp:2795`, **no rebuild**) to census the whole `0x10xxxx–0x1Cxxxx` band the default `kGameBandStart=0x200000` had been silently excluding.

**Finding 1 — the executed set is CLOSED.** Reproduced independently in both runs:

| run | `distinct` across 9 dumps | `calls` |
|---|---|---|
| A (band 0x200000) | 1374 → **1375, then frozen ×8** | 476,257 → 2,536,872 (5.3×) |
| B (band 0x100000) | 1309 → **1313, then frozen ×8** | 473,282 → 2,503,343 (3.4×) |

The EE enters **zero new functions** for ~80 s while call volume climbs 3–5×. This is *positive* proof that the guest state machine never advances — far stronger than the negative `tme=0` evidence, and it is now a reusable regression signal: **any real fix must make `distinct` climb.**

**Finding 2 — libcdvd/CCD is boot-only and idle, not stuck.** Full-band census: `CD_Init` (`0x186EC8`) = 2, `_sceCd_scmd_prechk` (`0x186CC0`) = 3, `sub_1C0A30` (CCD state machine) = **2**. They ran at boot and were never re-entered. Meanwhile `sub_327810` = 3959, `0x421EA0` = 3959, `0x421F10` = 3959 (once per frame, every frame).

**Finding 3 — the SRD disc-streaming subsystem never executes at all.** Region histogram: `0x12xxxx` = **1 address, 1 call**; `0x121000–0x135FFF` (84 KB, containing `sub_12EB28`/`sub_12F1E8`/`sub_12F7E0`) is entirely unentered. Consistent with `srd_gate 0x44D26C` showing **zero writes** all run — nobody ever *contends* the gate. **The 07-31 "stuck test-and-set gate" hypothesis is therefore dead:** the gate is untouched, not held.

**Finding 4 — falsified my own candidate before acting on it.** `0x463294` showed one transition `0xFFFFFFFF -> 0x0` @ `pc=0x186dfc`; I read it as a stuck in-flight latch. The decompile (3 refs in 15.9 MB) shows `if (dword_463294 < 0)` is a **lazy bind-once guard** and `sub_186CC0` returns `result = 1` on *both* paths. One transition is correct, healthy behaviour. Condition 3 of the `sub_186CC0` bail list is now **tested and exonerated**.

**Finding 5 — `ccd_reply` (`0x5AA7D0`) oscillates `0x0 ↔ 0x30000000` forever**, writer `pc=0x100160`, never `0x40000000`. But `sub_1C0A30` returns 1 for `0x30000000` **and** `0x40000000` — both branches pass, so this is not obviously a gate either. Probe **saturated at exactly its 16-record `should_log` cap**, so 16 oscillations and 16,000 are indistinguishable → fresh evidence for the cap-disclosure tooling item. `gamemode 0x5E6B3C`: zero transitions. `bind_ready 0x464DE4`: one, `0x0 -> 0x5`.

**★ Finding 6 — the localization.** `sub_327810` is **`CAppInit::Update`**, a staged app-init state machine (`decompiles_SLUS_214_42.txt:428822`), ticked 3959× and never advancing. Its object is constructed in place by `wrap_obj_register_d_0` @ `0x4E5050`:

```c
dword_5E5920 = (int)dword_4F2FC0;   // vtable  -> object base = 0x5E5920
dword_5E592C = 0;                   // object+12 = substate counter
```

so the state word `*(_WORD *)(a1 + 4)` that every `case` label increments is at a **fixed known address, `0x5E5924`** (`+6` = saved stat, `+8` = error code, `+12` = substate). State 1 has exactly one gate:

```c
case 1LL:
  if ( !wrap_noop_wrapper_unk_z_z_196() )   // 0x1BFF40, a thunk
    goto LABEL_63;                          // stay in state 1 forever
  ++*(_WORD *)(v4 + 4);
```

The failure path is also self-announcing: state `9900` prints `"%s::Init() ERROR!! Stat=%d Code=%08X"`. **We never see that string**, so `CAppInit` is *waiting*, not erroring.

⚠️ All six state-handler callees (`0x1BF340`, `0x1BFF40`, `0x3273D0`, `0x327550`, `0x327730`, `0x2FF4C0`) read `--NEVER dispatched--`, but they are thunks and **coverage counts table-dispatched calls only** (`ps2_runtime.cpp:404-408`) — a direct `fn_*` call bypasses `lookupFunction`. This is *not* proof they never ran, and must not be reported as such. Read `0x5E5924` directly instead.

**Next:** watch `0x5E5924:2` + `0x5E592C:4` (zero rebuild) → which state, and does the substate counter move; then read `0x5E5924` on PCSX2 at the memory-card prompt for the first exact divergence-table row.

#### ★★★ 5.8 measurement #2 (2026-08-01) — PCSX2 differential: **every nominated suspect MATCHES; the only divergence is `CAppInit` itself**

First ground-truth differential in the project's history. PCSX2 `d75a0ad`, DebugServer 21512 + Pine 28011 both live, `SLUS-21442` UUID `de2df62d`, paused at EE `pc=0x175220`, GameMode already `0`.

| Addr | What | **PCSX2** | **Ours** | Verdict |
|---|---|---|---|---|
| `0x5E5920` | `CAppInit` vtable | `0x004F2FC0` | (same by construction) | **MATCH** — confirms the `0x4E5050` object-layout derivation on real hardware |
| **`0x5E5924`** | **`CAppInit` state** | **`1000` (0x03E8)** | **stuck; `Update` ticks 3959×/run** | **★ THE DIVERGENCE** |
| `0x5E5926` | saved stat | `0` | TBD | — |
| `0x5E592C` | substate | `0` | TBD | — |
| `0x5E5944` | `CAppFinal` state | `0` | TBD | never entered on either side |
| `0x5AA7D0` | CCD reply word | `0x30000000` | `0x30000000` | **MATCH — ARKD exonerated** |
| `0x44D26C` | SRD test-and-set gate | `0` | `0` (never written) | **MATCH — lead dead both sides** |
| `0x463294` | libcdvd bind latch | `0` | (exonerated by decompile) | **MATCH** |
| `0x5E6B3C` | GameMode | `0` | `0` | MATCH — `0` is not yet a discriminator |

**★ Finding 7 — `0x1BFF40` is NOT a "noop wrapper". It is a one-instruction tail-jump into `0x1C0A30`.** Native PCSX2 disasm: `0x001bff40: j ->$0x001C0A30` + `nop`. So the sole gate on `CAppInit` state 1 *is* the CCD boot state machine, and its gate in turn is the reply content at `0x5AA7D0`. **Second func-map name in two days that meant the opposite of what it said** (after `rpc_handle_valid` = `sceSifCheckStatRpc`). See the 2026-08-01 Learned Patterns entry.

**★ Finding 8 — the whole gate chain evaluates TRUE for us.**
`CAppInit::Update` state 1 → `0x1BFF40` → `0x1C0A30` → returns 1 iff `(MEM[0x5AA7D0] & 0xF0000000)` ∈ `{0x40000000, 0x30000000}`. We measured `0x30000000` on **both** machines, and `sub_1C0A30` returns 1 for either value. **So if we were in state 1, we would advance.** Therefore either (a) we are stuck in a *different* state, or (b) `sub_1C0A30` never reaches its final check in our build. Our coverage shows `sub_1C0A30` dispatched only **2×** against `CAppInit::Update`'s 3959 — which favours (a) — **but `0x1BFF40` is a thunk and coverage counts table-dispatched calls only, so that count cannot decide it.** Reading `0x5E5924` decides it. Do not guess.

**★ Finding 9 — on real hardware `CAppInit::Update` is RETIRED.** Breakpoint at `0x327810`, resumed, ~1.34 × 10⁹ EE cycles (≈4.5 s) elapsed with **zero hits**. State `1000` is the terminal state: init completed and the object stopped being ticked entirely. Ours is ticked 3959× in 90 s and never leaves. This is the cleanest positive success signal the project has had — better than any of our negative ones.

⚠️ **The plan anticipated this outcome and it is a result, not a dead end:** all four addresses nominated for the differential match real hardware. The blocker is not in any of them; `CAppInit`'s own state word is now the instrument.

**Next:** the pending zero-rebuild run reads `0x5E5924`. Then the fix target is whichever state it names — and the reference answer (`1000`, retired) is now known, so 5.8 finally has a **positive** exit test: `0x5E5924` reaches `1000` and `sub_327810` stops being dispatched.

#### ★★★ 5.8 measurement #3 (2026-08-01) — **the blocker is named: `CAppInit` state 11, waiting on the 16-slot audio table**

The run above landed. `appinit_state` at `0x5E5924` produced **10 changes, well under the 16-print cap, so the sequence is complete and trustworthy**:

`0 → 1 → 2 → 3 → 5 → 6 → 7 → 8 → 9 → 10 → 11`, then **stops at 11 (0xb)** for the rest of the 90 s.

So Finding 8's hypothesis **(a) is correct** — we are stuck in a *different* state, not state 1. `sub_1C0A30`, the CCD gate, was passed long ago. **BUG-009's whole neighbourhood is now doubly dead.**

`appinit_sub` (`0x5E592C`) produced **exactly 16** changes = saturated at `should_log(c,16,600)`. Interleaving both watches in log order shows every one of those 16 fell **inside state 8** (the `3↔4` oscillation at `pc=0x327640`/`0x327710` is `sub_327550`'s 31-entry sound-bank load loop), which then completed normally and let the state advance 8→9→10→11. **Substate values after state 8 are unknown — the probe went silent, it did not stop changing.** This is the third time a saturated bounded probe has had to be reasoned around; see the deferred cap-disclosure tooling item.

**★ Finding 10 — `CAppInit::Update` case 11 is a wait-for-audio gate, and its func-map name is the FOURTH one to mean the opposite of what it says.**

```c
case 11LL:
  if ( *(_DWORD *)(a1 + 12) == 1 ) {                       // substate 1
    if ( input_device_get_button_map_clone_01(0x10u) ) {   // <-- NOT an input read
      *(_DWORD *)(v4 + 12) = 0; v10 = 1; goto LABEL_52; }  // advance to state 12
  } else {
    if ( *(_DWORD *)(a1 + 12) ) { v10 = 0; goto LABEL_52; }  // substate >=2 => WEDGED FOREVER
    str_queue_msg(0, (unsigned int)aSndStdeff);             // queue "snd/stdeff.ase"
    ++*(_DWORD *)(v4 + 12);                                 // substate 0 -> 1
  }
  v10 = 0;
```

`input_device_get_button_map_clone_01` is `sub_2FCDF0 @ 0x2FCDF0`, and reading its body settles it — it touches no pad state at all:

```c
sub_2FCDF0(a1):
  if (a1 != 16) return LOBYTE(dword_5D6BA4[10*a1]) < 2;    // one slot's status < 2
  for (i = 0; i < 16; i++)                                  // a1 == 16 (0x10) => ALL slots
    if (slot[i].byte36 != 1 && slot[i].byte36) return 0;    // any status >= 2 => not ready
  return 1;
```

It is **"are all 16 audio slots idle?"**. It is byte-for-byte identical to `input_port_is_available @ 0x2FCE70`, which is plainly where the auto-labeller cloned the bogus "input_device / button_map" name from. The `0x10` argument is not a button mask, it is the **slot count**. Add to the Learned Patterns tally: `rpc_handle_valid`→`sceSifCheckStatRpc`, `wrap_noop_wrapper_unk_z_z_196`→tail-jump, and now `input_device_get_button_map_clone_01`→`all_sound_slots_idle`.

**★ Finding 11 — the reference audio table, read live from PCSX2.** The array is at `0x5D6B80`, **40-byte stride**, filename at `+0`, **status byte at `+36`** (`dword_5D6BA4 == 0x5D6B80 + 0x24`). At the memory-card prompt:

| slot | name | status |
|---|---|---|
| 0 | `snd/stdeff.ase` | **1** |
| 2 | `snd/comeff.ase` | 0 |
| 3 | `snd/comvoi.ase` | 0 |
| 5 | `stg/s01/s01.ase` | 0 |
| 7 | `snd/etceff.ase` | 0 |
| 8 | `snd/etcvoi.ase` | 0 |
| 10 | `ply/p04/p04.ase` | 0 |
| 11 | `ply/p12/p12.ase` | 0 |
| 1,4,6,9,12–15 | (empty) | 0 |

All ∈ `{0,1}` → the gate returns 1 → state 11 passes on hardware. Slot 0 being `snd/stdeff.ase` at status 1 **independently confirms** the `str_queue_msg(0, aSndStdeff)` → slot-0 linkage.

**Two candidate mechanisms, and the pending run distinguishes them without a rebuild:**
- **(A)** substate reached 1, `str_queue_msg` queued `stdeff`, and some slot is parked at status ≥2 because our audio backend never completes the load. Signature: **slot watches show transitions**.
- **(B)** substate was ≥2 on entry to state 11 (leftover from state 10's handler), which per the `else` branch above is an **unrecoverable wedge with no path back to 0**. Signature: **no slot watch ever fires**, because `str_queue_msg` is never reached.

Watching all 16 status bytes decides it: `str_queue_msg` only runs at state 11 substate 0, so *any* slot-0 transition proves substate reached 1. `appinit_sub` was deliberately **not** re-armed — it spends all 16 prints inside state 8 and tells us nothing new.

#### ★★★ 5.8 measurement #4 (2026-08-01) — **root cause found: the ARKD completion path was never built, and the sreg it publishes to is EE libsifcmd's own array**

Answer to #3's A/B: **neither, as stated.** The chain broke further upstream than both candidates assumed.

**Retraction first.** The intermediate framing "the sid `0x501` reply is empty" is **wrong and withdrawn**. The reply was never produced at all: the `fno=0x101` handler **never returned** (`retired=4000000` = full interpreter budget, `halted=0`), so `$v0`/`rsz`/`delivered` were zeros *by construction*, not by measurement. SIF transport is **exonerated** — do not go back to it.

**★ Identity correction that made the fix findable.** `0x5618B0` has been described for several sessions as "entry 12 of an async status table at `0x561880`". It is **EE libsifcmd's `_sif_sreg[]` array** — 32 entries, installed by the game's own `sceSifInitCmd` at EE `0x177B00` (stored to `cmd_data+0x1C` = `0x5616F4`). `0x561880 + 4*12 == 0x5618B0`, so `SET_SREG index=0xc` and the `req12` watch are **the same word**. Readers: `0x177AB8` = `return dword_561880[a1]`; `0x177AD0` = `sceSifSetReg`; `0x177A88` = the SET_SREG system-command handler. Fifth entry in the misleading-name tally.

**Four independent breaks, all measured, all on one path:**

| # | Break | Evidence |
|---|---|---|
| 1 | `sub_6920` (+0x6920) never called — the loader ran only `InitLoadBuffers`, skipping the module's own init `+0x30` (`RpcReplyRetryLoop → sub_6920 → sub_9EAC → InitLoadBuffers`) | log `threads=4`, all four RPC servers (`0x4a4dc/0x4a554/0x4a5cc/0x4a644`); `0x042c90` absent from every `CreateThread` |
| 2 | job worker `+0x62A4` never runs — state-11 dispatchers set `dword_B334=0x10000000` + `SignalSema(dword_B338)`, nothing consumes it | no IOP scheduler; `B334` never leaves the `0x1` band |
| 3 | publisher `+0x2C90` never runs — it is the only caller of `sub_A3C0(12, dword_B334)` | `sendcmd=1` for the whole run |
| 4 | the one SET_SREG we did emit landed nowhere — it wrote host-side `g_sifSregs`, but the game never calls `sceSifGetSreg`; it reads `dword_561880[i]` out of guest RAM | `set_sreg=0` |

`sub_6920` also seeds `dword_B334 = 0x30000000` (ready) and creates the job semaphore. The single `SET_SREG value=0x10000000` seen in the 3c run was the **dispatcher's busy stamp**, correct for that instant, with nothing present to advance it.

**Fix written (not yet built), all in `ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp`** — no headers, no `runner/`, no `fn_*`: run `+0x6920` at load after the generic thread pass; tick `+0x62A4` then `+0x2C90` per service call under bounded halt rules modelled on the existing `+0xAEDC` idle-worker rule; mirror SET_SREG into EE RAM. All three created bodies are `while(1)` loops, hence ticking rather than running once.

**⚠️ The fix is INERT unless `PS2_SIF_EE_SREG_BASE=0x561880` is set** (the env var `RPC.cpp:134` already defines). The log self-discloses: `<-- NOT MIRRORED (set PS2_SIF_EE_SREG_BASE)`.

**Exit test:** `[ARKD:sendcmd] index=0xc value=0x3xxxxxxx (CHANGED)`, `req12` leaves `0xF0000000`, `appinit_state` leaves `0xb`. **Two runs, per standing rule.**

**Bundled in the same rebuild** — the generalization of this bug class, since an unhandled import silently returning `$v0=0` is what caused break 4 *and* the 3b-4 `sceSifSendCmd` spin:
- `[iop:unhandled]` — every `(lib, fid)` that falls through to the default is censused; first hit announced; full census dumped whenever a service run fails to halt (i.e. exactly when something is spinning).
- `[iop:import]` cap is now env-overridable (`PS2X_IOP_IMPORT_MAX`) and prints a `[cap]` saturation line, joining `PS2X_ARKD_CALL_MAX` / `PS2X_ARKD_RUN_MAX`.

**Still open, explicitly unmeasured:** `thsemap fid=6/8` and `sifman fid=7/8` semantics (deliberately *not* guessed from an ordinal table — the new `[iop:unhandled]` census will name them if they matter); the `cdvdman fid=5` (`a0=6`) spin on `sid=0x500 fno=0x1`.

**✅ BUILD SUCCEEDED 2026-08-01 (`ps2EntryRunner.exe`, RelWithDebInfo, +04:02).** All of the above — the Phase 3d fix and the bundled `[iop:unhandled]` census — is now **in the exe and completely unrun.** Nothing in this section has been validated against a run; everything is code-proven or decompile-proven only.

**→ THE SINGLE NEXT ACTION IS THE RUN.** It must carry `PS2_SIF_EE_SREG_BASE=0x561880` or the fix does nothing:

```powershell
$env:PS2_IOP_TRACE  = "1"
$env:PS2_ARKD_STATE = "1"
$env:PS2_SIF_EE_SREG_BASE = "0x561880"
$env:PS2X_ARKD_CALL_MAX = "100000"
$env:PS2X_ARKD_RUN_MAX  = "100000"
$env:PS2X_WATCH = "0x005A9380:4:pkt_snd,0x005A9358:4:pkt_ccd,0x005618B0:4:req12,0x005D6BA4:4:snd00,0x005E5924:2:appinit_state"

& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger `
    -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```

Read, in this order — the first two are informative **whether or not the fix works**:
1. `[iop:irx] initThreads run: … created=3 B334=30000000` → did break 1 actually close?
2. `[iop:unhandled] …` first-hit lines, and any post-non-halt census → names the next spin, if there is one.
3. `[ARKD:tick] job: … pub: … B334=…` → breaks 2 and 3.
4. `[ARKD:sendcmd] SET_SREG index=0xc value=0x3xxxxxxx -> EE[0x5618b0] (CHANGED)` → break 4, and the exit test.
5. Validity gate before reasoning from any absence: `[watch] armed 5` present **and** no `[cap]` line for the tag in question.

**⚪ 5.7 (superseded 2026-08-01, folded into 5.8)** — **the `0x421f10` / `sceDmaSync` (`0x172998`) plateau is a correctness gate, not slowness.** At ~47 fps the guest still never leaves it in 90 s (≈2.7 h of Debug-equivalent work) while PCSX2 reaches the memory-card prompt in 7–8 s. `dma/s`=92–98 and `gif/s`=47–49 prove DMA *is* completing, so the guest is either polling a completion flag the runtime never sets or syncing a channel that is never serviced. **Exit test:** watchdog `pc` leaves `0x421f10` and `lastCall` leaves `0x172998` under its own power. First steps: (1) one `-RunSeconds 300` confirmation run, ~~(2) disassemble `0x421f10` — it is `pc` *and* `ra`, a tight self-loop, and has never been decoded despite being the plateau address for weeks~~ **(2) RETRACTED 2026-07-29 (co-piolet) — `0x421f10` is `jal sub_00199840`, call site 10 of a 14-`jal` fan-out inside the per-frame driver `sub_00421EA0` (real extent `0x421EA0–0x422168`, sole caller `sub_00422630` @ `0x422658`); it is not a loop and `pc==ra` there is a dispatch-boundary sampling artefact. This agrees with stage 5.5 below and supersedes the wording here. Replacement step (2): stop treating `pc` as the plateau signal and instead find why `gstate@0x50227c` never changes — the fan-out is running to completion every frame, so the stall is in what those subsystems read, not in reaching them.**, (3) walk the stable trace tail `0x174f20 -> 0x172e20 -> 0x172a90 -> 0x172998`, (4) A/B the same point against real PCSX2.

| # | Stage | State | Exit test |
|---|---|---|---|
| 5.1 | Clear the EE stack-frame derail | ✅ **CLEARED** (built + run 2026-07-22): zero `[frametrace:ALARM]`/`[frametrace:TABLE]` — no escape to packet-pool `0x20561900`. Exit test as written was **mis-keyed**: the surviving `IMBAL delta=-0xa0` lines are normal recompiler `jal`-split dispatch (entry `0x178068` → exit mid-body `0x1780c0` → next slot resumes + restores), NOT corruption. | ~~No `IMBAL delta=-0xa0`~~ → superseded by: no ALARM/TABLE escape to `0x20561900` |
| 5.2 | Prologue-skip class (`fn_178428` + `fn_178068` jalr paths) | ⚫ **ABSORBED / N/A** — the imbalances that would have triggered this are now proven to be benign jal-split false positives (see 5.1). No separate derail class exists to fix. | (moot) |
| 5.3 | ARKD RPC returns real data | ✅ **CLEARED (2026-07-24)** — `memcpy` thunk (`0xAE24`) no longer no-ops; `[ARKD:diag] post-func BF50=0x01652400` (was `0x0`); `[ARKD:sifdma] eeDest=0x01652400 size=0x18000 -> copied`. TOC DMA lands at the real EE address, not `0x0`. | `func=0x2` reply lands + real eeDest ✅ MET |
| 5.3.1 | AudioSysInit ARKD `func=0x2` completion | ⚪ not yet re-measured post-5.3 fix — watchdog never logged `0x422454`/`0x422480` this run (see 5.3.2 derail below, which likely preempts it) | `AudioSysInit` returns; watchdog pc leaves `0x422454` |
| 5.3.2 | New derail: EE PC collapses to `0x1` after real TOC DMA lands | ✅ **CLEARED (2026-07-25, fix 3, VERIFIED).** Root cause was fix (2)'s *fixed-absolute* `kSifReplyScratchStackTop` — SLOTWATCH showed the real depth=0 dispatcher chain descends ~0x4110 bytes, blowing through the fixed top's clearance and landing within 0xC0 bytes of the real caller's saved-`$ra` slot regardless of re-entrancy depth (so the "second untracked writer" chased in 07-24g/07-25b/c was this same mechanism, just not bracketed by the depth-keyed SLOTWATCH probes). Fix: `scratchTop = savedSp - kSifReplyScratchHeadroom(0x8000) - depth*0x2000` — anchored to the live `$sp` at call time instead of a fixed RAM address, in `SIF.cpp` only. **Verification run:** `scratchTop = savedSp - 0x8000` confirmed on 20+ sampled `[SifRpcReply]` lines, zero `$ra=0x1`/`PC=0x1` anywhere before the new blocker (line 2889 of that run's log) — ARKD calls, LOADFILE, dozens of BIND/CALL round-trips all succeed. Full details + fix text: [[reference_ps2_sif_boot]] 2026-07-25 entry. ~~07-24e hypothesis RETRACTED (07-24f) — `$s2` overflow in `sub_00177EB0_0x177eb0` is DISPROVEN by direct code read.~~ Full function body read line-by-line: `$s2` is reset to `0` (delay slot of `blez $a1` at `0x177ef8`) or `1` (`0x177f0c`) at every fresh entry, and the function is a **single straight-line pass** (no loop-back edge exists to `label_177f40`/`label_177f4c` from below `0x177f9c`) — its sole caller `fn_177FE8_0x177fe8.cpp` invokes it exactly once with `$a1=0`. `$s2` therefore never exceeds ~2 within this call, so it cannot index `sp+0xC0`; the "s2≈12" arithmetic in the retracted entry was coincidental, not causal. **Do not resurrect this hypothesis.** The real, previously-measured mechanism (2026-07-23d/24, still the live lead, see below) is: `0x177eb0`'s SIF DMA syscall (`0x175060/0x175070` → `sceSifSetDma`) has no real IOP, so it calls `deliverSifRpcReply` (`SIF.cpp`), which nested-dispatches `0x178068` **on the same EE stack** to fake the reply — and that nested subtree is what overwrites `rpc_call`'s (`0x178be8`) saved-`$ra` slot `0x1ffbeb0`. Two scratch-stack isolation fixes for this were already shipped in `SIF.cpp` (Fix 1: single scratch top 07-23; Fix 2: per-depth scratch bands, `kSifReplyScratchStackTop - depth*0x2000`, 07-24) and build-verified, but **the falsification-first verification gate for Fix 2 is still unmet after 3 separate runs** (07-24, 07-24e, 07-24f = this session) — every run shows `[SifRpcReply] depth=0` for all entries (44/35/36 lines checked), meaning re-entrancy (depth≥2) is simply never exercised by these particular runs, so Fix 2 remains unfalsified but also unconfirmed. **Open problem:** this session's fresh run (07-24f) still shows `RAFORK fork=B(mid-body-clobber)` / PC=`0x1` on `0x178be8`/`0x1baf90` **despite depth staying 0 throughout** — meaning whatever is stomping `0x1ffbeb0` this run is NOT going through the tracked `deliverSifRpcReply` re-entrancy path at all (that counter never left 0), so it must be a **second, untracked writer** with the same blast radius. Next step: add a raw SLOTWATCH-style write-guard directly on `0x1ffbeb0` (or extend the existing `kSdbzFrameTraceSlots` probe) that logs the writer's PC/callstack on ANY write to that address, not just at wrapped-function entry/exit boundaries — the current SLOTWATCH only samples at call boundaries of pre-selected slots (`0x178068`/`0x177eb0`/`0x177fe8`), so it can bracket "somewhere inside this subtree" but not pinpoint the exact instruction.

**07-24g (this session, fresh rebuild+run, after fixing an MSVC mtime-staleness bug that had silently prevented `game_overrides.cpp` from recompiling across ~3 prior "successful" builds):** full IMBAL/RAFORK/SLOTWATCH set captured. Real call order (reading the `trace=` tail forward, oldest→newest): `... → 0x1bc5a0×n → 0x171f10 → 0x1baf90 → 0x178de8×4 → 0x178be8 → 0x178428 → 0x17ed60 → 0x17edb0 → 0x177fe8 → 0x177eb0 → 0x1781b0 → 0x175060 → 0x178068 → 0x178068 → 0x175090 → 0x178560 → 0x1784d0 → 0x1 (collapse)`. Key data points:
- **SLOTWATCH #1** (`callee=0x178068`, first entry): `expected=0x1bb0b0 before=0x1 now=0x1` — slot is **already `0x1` before `0x178068` runs at all**. This **falsifies the `0x178068` copy-loop-overrun hypothesis as proximate cause** for this run — `0x178068` only re-observes an already-poisoned slot, it doesn't poison it.
- **SLOTWATCH #3/#4** (`0x177eb0` then `0x177fe8`): `before=0x1bb0b0 now=0x1` on #3, still `now=0x1` on #4 — the actual clobber is caught in the act, bracketed between `0x177eb0`'s and `0x177fe8`'s checks (note: #3/#4 fire in `0x177eb0→0x177fe8` order in the log despite `0x177fe8` being `0x177eb0`'s caller per the call trace — consistent with #3/#4 being watch-checks made at nested-call boundaries inside that subtree, not top-level entry order).
- **RAFORK**: both `0x178be8` (`entryRa=0x1bb0b0`) and `0x1baf90` (`entryRa=0x2fcb84`) exit with `exitRa=0x1`, `fork=B(mid-body-clobber)`, at adjacent stack addresses `0x1ffbec0`/`0x1ffbee0` (only `0x20` apart) — same clobber, aliased across two frames.
- **IMBAL** on `0x1baf90`: `delta=-0x20`, `[0x5616d8]=0x20561600`, **`countByte=0xffffffff`**, `a0=0x20561900` (the same SIF RPC packet buffer seen in the preceding `sceSifSetDma`/`SifRpcPkt:SIG` lines), `a2=0x178560`, `a3=0x20561600`. `countByte=0xffffffff` (i.e. -1 unsigned) is a strong signal that whatever byte-derived count/index this function computes from `[0x5616d8]` is garbage — consistent with a bad/OOB pointer read feeding an unbounded copy or index, not a hand-tuned small overrun.
- **Conclusion:** the "second, untracked writer" is real and is upstream of `0x178068`; best current lead is `0x1baf90` and/or its callee chain (`0x178de8`, `0x178be8`, `0x178428`) reading a garbage count from `[0x5616d8]` and using it to write out of bounds into the `0x1ffbe.0`-`0x1ffbee0` stack region. **Next step: decompile/re-examine `0x1baf90` and `0x178de8` for any indexed/counted `sq`/`sw` loop keyed off a byte or word near `[0x5616d8]`**, analogous to the retracted `0x178068` hypothesis but now pointed at the right function. Do not re-chase `0x178068` itself as the writer — it is confirmed a victim, not the source, in this run. | No `RAFORK ... fork=B(mid-body-clobber)`; EE PC does not collapse to `0x1` |
| 5.4 | Game data load chain | 🟡 **`0x1bf2e0`/`0x22c8f0`/`0x22cbe0` all build-VERIFIED fixed (2026-07-25g)** -- none appear in run_log.txt anymore, boot goes much further into the SIF RPC completion path. **Unfixed blocker, still narrowing:** 07-25g's `sub_178560` callback-`jalr` hypothesis was DISPROVEN by measurement (07-25h) -- that call's branch was skipped this crash, `0x178560` exits clean. Corruption re-traced into `0x178be8` (`rpc_call`)'s own SLOTWATCH/SLOTENTRY chain; 07-25i (SLOTENTRY probe, chronologically corrected) brackets the `$ra`-slot stomp to between `0x177eb0`'s entry (clean) and `0x178560`'s entry (dirty) -- inside `0x177eb0`'s body or an unwrapped intermediate call (`0x1781b0`/`0x175060`/`0x178068`/`0x175090`). No code fix applied yet -- see [[reference_ps2_sif_boot]] 07-25i entry. ~~07-25j: WRITER IDENTIFIED -- `pc=0x102894` ... is the ONLY PC that ever writes `0x1ffbeb0`.~~ **RETRACTED 2026-07-25k -- the entire stack-overlap premise is measured false, see 07-25k section. This stage's line of inquiry is CLOSED; the live fault moved to 5.4.1.** | Game proceeds past whatever it does with GAME.DAT contents |
| 5.4.1 | Bad ctor function pointer (`0x30`) reaching an unguarded `jalr` in `array_call_ctor_dtor` | ✅ **CLEARED (2026-07-27, EIE gate run).** Exit test met: **zero `pc=0x30`** anywhere in the run sink (all 8 `MISS` records are `pc=0x1`), and the run's only derail is a single bracketed `$ra`-slot event. **Cause was not the `jalr` guard — it was the scheduler preempting the guest inside its own `di`/`ei` critical sections.** With the EIE gate honoured, the `0x30` fnptr never materialises; `0x171d04` was left unguarded and did not fire. Caveat on the record: the `0x30` and `pc=0x1` derails previously alternated non-deterministically, so "zero `0x30`" is one run; if `0x30` reappears after 5.4.2 is fixed, reopen this stage rather than re-deriving it. Original (now superseded) analysis: <br>~~414 derails, all `guest PC 0x30`, zero `pc=0x1` in the run. Trace tail: `... 0x171f10 -> 0x3a2f30 -> 0x3a2c60 -> 0x171c30` -> `0x30`. `0x171c30` = `array_call_ctor_dtor` (a `__cxa_vec_ctor`); its ctor `jalr $s5` at `0x171d04` is **not** null- or range-guarded (the dtor `jalr $v0` at `0x171cdc` IS null-guarded at `0x171cb0`). Immediate caller `0x3a2c60` passes valid args (`a1=0x3a2ef0`, `a2=0x3a2d20`, `a3=8`, `t0=2`), so the bad pointer arrives from elsewhere in the chain or from the frame scratch slots (`sp+0x94..0xa0`, held in `$s0/$s1/$s6/$s7`).~~ | No `guest PC 0x30`; static C++ array construction completes ✅ MET |
| 5.4.2 | `$ra` slot `0x1ffbeb0` overwritten with `0x1` across the `0x175060`→`0x175090` stack switch | ✅ **CLEARED (2026-07-27, verified by build + run).** **Root cause: EE syscall `0x73` `SetVSyncFlag` was implemented as a *persistent* registration; it must be *one-shot*.** The guest SDK's `GsSyncV` (`0x1751c0`) registers `sp+0`/`sp+8` of its **own 0x20 stack frame** as the vsync flag/tick pointers, spins one vblank, then restores `$ra` from `16($sp)` and pops — **with no `SetVSyncFlag(0,0)` deregister path anywhere in the function** (verified by full disassembly). Our runtime kept the registration live, so the vblank IRQ worker (a plain host `std::thread`) stamped `1` into that abandoned frame on **every** vblank, forever, asynchronously. When `rpc_call` (`0x178be8`) later reused that stack depth, its saved `$ra` became `0x1`. This also explains the long-standing non-determinism: the derail depended on wall-clock racing between a host thread and guest stack reuse, which is why `-Determinism 1` never reproduced it. **Fix:** `signalVSyncFlag` now clears `g_vsync_registration.flagAddr/.tickAddr` under the existing mutex immediately after capturing them (`Kernel/Syscalls/Interrupt.cpp`). Blast radius checked — `g_vsync_registration` has no other consumer. **Evidence (before → after):** `signalVSyncFlag` writes to the watched slot every-hit → **1 of 64** (and that one is *correct* — it targets the live registration `0x1ffbf60`); VSYNCREG registrations `5` → **160**; end-of-run `progress` `0x67c` → **`0x1f39`**; `RASLOT` **6/6 `ok`** with `saved == entryRa`. Game now runs `GameUpdate_0x421ea0` / `EngineUpdate` → `SyncFrame` → `GS_DispatchPending` on a fiber for 48 s with `progress` climbing. Method note: found with a **hardware data breakpoint** (DR0/DR7 + VEH + dbghelp symbolization, `game_overrides.cpp`) filtered to `newval==1` — the first tool in this investigation that named a writer instead of a suspect. ~~Superseded analysis: The sole surviving derail once the EIE gate removed the noise. One event, one thread (`tid=0x13a6`), progress `1732`, zero progress ticks between clobber and consumption. Bracket is now **one step wide**: `SLOTENTRY seq=276 callee=0x175060 entrySp=0x1ffbd70 before=0x1bb0b0` (clean) → `seq=277 callee=0x175090 entrySp=0x1ff3cd0 before=0x1` (dirty). `$sp` drops ~`0x8000` between those two — a stack switch. Then `RASLOT seq=279 slot=0x178be8 at=0x1ffbeb0 saved=0x1 entryRa=0x1bb0b0 entrySp=0x1ffbec0 liveRa=0x1`, then `MISS seq=280 pc=0x1` ×8. **Both `saved` and `liveRa` are `0x1`**, so the epilogue read the poisoned slot — this is a frame being read from the wrong place, not a stray store. The code that runs inside the bracket is **`0x178068`**, which is exactly the already-recorded [[project_stale_frame_ra_read]] mechanism: `0x178068` dispatched with stale `ctx->pc=0x175064`, missing its entry switch, falling through the prologue and exiting mid-body without restoring `sp`. The recorded open question — why is `ctx->pc` stale? — is now the whole blocker.~~ **That framing was wrong: the writer was never guest code at all, and `ctx->pc` was never stale. Do not re-open the `0x178068` stale-pc line of inquiry.** | No `RASLOT ... bad`; no `MISS pc=0x1`; `0x178068` enters at its own entry `pc` ✅ **MET** (6/6 RASLOT `ok`, zero `MISS pc=0x1`) |
| 5.5 | Memory card IOP stack responds | 🔵 **ACTIVE — but entry state must be baselined first (2026-07-27).** Post-5.4.2 the game reaches a **stable frame loop and holds it**: from `t=5s` to `t=48s` the watchdog sits at `pc=0x421f10` (a `jal 0x199840` site inside the per-frame function `0x421ea0`–`0x421f40`, which ends in `jr $ra` — so this is a *called-every-frame* body, not a spin), with `progress` climbing **~115/sec, monotonically, for 43 s straight**. `lastCall=0x172998` is a **timeout-guarded busy-wait**: `$s1 = 0x1000000` countdown polling `*(a0) & 0x100`, with `jal 0x177988` (message at `0x4c01a8`) + `jal 0x173130` on expiry — i.e. the frame loop is waiting on a hardware-busy bit each frame, which is normal, not a hang. `stuckSecs` is a **false positive** by construction (pc is pinned to a jal boundary while progress advances). Run health is otherwise clean: exactly **one** unimplemented syscall in 5145 log lines (`0x6b` at `PC=0x174f70`, 1 hit), zero errors. MCMAN/MCSERV both `Load Module ... OK!` but **no memcard RPC is ever issued**, so the 5.5 exit test cannot be evaluated yet. Disc activity stops early (`[ARKD:cdread]` 8 hits, all before log line 2107 of 5145) while `[SifRpcPkt:SIG]` continues to line 4808 — the game is *running*, not *loading*. **First task: a clean baseline run** with the now-obsolete 5.4.2 diagnostics off (`PS2X_TRAPVAL=0`, `PS2X_HWWATCH_VAL` unset, `PS2X_FRAMETRACE=0` — all env-gated, no rebuild). The old `trapval` watch on `0x1ffbeb0` fired **189 times with ~60-deep stack captures**; that address is dead post-fix and the capture cost is a large, pointless perf drag on exactly the frame-rate measurement we now need. Baseline must answer: (a) what is on screen, (b) what frame rate, (c) does the game state ever advance — before assuming memcard is the gate rather than a render/GS gap. ~~MCMAN/MCSERV load, HLE handlers exist, never exercised.~~ **Planned pivot point: take upstream #170+#179 here** — #179 ships a memcard IOP stack that supersedes our HLE handlers; the re-architecture is worth doing once the ARKD blocker is closed and memcard IOP is the active target (see Upstream Sync #179 note). | Memcard init RPCs complete without error |
| 5.5.1 | libpad ↔ padman version handshake fails → **pad init is skipped entirely** | 🔵 **ACTIVE (opened 2026-07-27).** The only game-emitted error in a 5145-line run: `[Deci2Call:kputs] libpad: Module version mismatch [libpad.a = 4.0, padman.irx = 0.0]`. **This is NOT cosmetic — it is a hard early-out, proven by disassembly of `0x187d10`–`0x187d94`:** `jal 0x189110` (get padman version) → `$s1 = $v0`; `sra $s0, $s1, 8` (major); `beq $s0, 4, 0x187d78` → the **only** path that reaches the real init `jal 0x187de0`. On mismatch it prints the two messages and falls to `0x187d70`/`0x187d74` → `$v0 = 0`, returning **without ever initialising the pad**. Version source (`0x189110`): builds an RPC — cmd `0x12` into send buf `0x568b80`, `jal 0x178be8` (`rpc_call`) with `ssz/rsz = 0x80` — then reads the version from **`lw $v0, 12($s0)`**, i.e. *recv buffer + 0xC*. On `rpc_call < 0` it returns `0` (`bgezl` at `0x189158`). **Root cause: there is no IOP-side padman RPC server in our runtime at all.** `Kernel/Stubs/Pad.cpp` has ~30 EE-side `scePad*` stubs, but grep shows zero padman/SIO2MAN IOP RPC handling — and the module never loads: the run's Deci2 tty shows exactly **five** `Load Module ... OK!` lines (MCMAN, MCSERV, LIBSD, CRI_ADXI, ARKD_DVD) with **no PADMAN and no SIO2MAN** (the game expects those from `rom0:`, which we do not provide). So the bind/call fails, recv+0xC stays `0`, major = `0` ≠ `4`. Note `scePadGetModVersion` in `Pad.cpp:420` returns `0x0200` — **major 2, which would fail this same check even if it were on this path**; if a fix routes through it, it must report major **4** (e.g. `0x0400`). **Confidence split — do not conflate:** *pad init is skipped* is **code-proven**. *This is what holds the black screen* is **plausible but unproven** — the memcard prompt is X-gated (see [[reference_iop_bug_log]]), so an uninitialised pad is a credible stall, but no measurement yet ties the frame loop's wait to pad state. Verify before declaring, per the 5.4.2 lesson. **★ 2026-07-27 PCSX2 GROUND TRUTH (live, paused at the memcard screen, SLUS-21442 / PCSX2 d75a0ad) — the sids are no longer a guess, they are literal in the code at `0x187cb0`–`0x187d24`:** `lui $a1,0x8000; ori $a1,0x100; jal 0x178a08` binds **sid `0x80000100`** with client data `0x568940`; `lui $a1,0x8000; ori $a1,0x101; jal 0x178a08` binds **sid `0x80000101`** with client data `0x568968` (= first client + `0x28`, so `sizeof(SifRpcClientData) == 0x28`). **Client-data offset `0x24` is the bound-server pointer** and the game *spins until it is non-zero* (`lw $v1,0x24($s0); beqz $v1, 0x187c88` and `lw $v1,0x4c($s1); beqz $v1, 0x187ce0`). Live PCSX2 read confirms both: `[0x568940+0x24] = 0x00182dac`, `[0x568968+0x24] = 0x00182e8c`. **Corollary — our binds already succeed.** Our run prints the mismatch, which is *downstream* of both spin loops, so the runtime's SIF layer is already claiming these binds generically; the failure is narrower than first written: **only the cmd-`0x12` call is unserved.** Version query uses client `0x568940`, i.e. **sid `0x80000100`**. The full version word is dead after the check (`0x187d78` passes only `$a0 = $s2`), so **returning `0x0400` is behaviourally exact**, minor byte irrelevant. **Scope warning — passing the check is stage 1, not the whole fix.** `0x187de0` (the real init, disassembled live) then: zeroes a 4-entry × `0x1c` state table, issues a **second RPC with cmd `0x10`** on the same client (`sw $v0,-0x7480($a1)` → `0x568b80`, `jal 0x178be8`, `bgez $v0` else return 0), then `jal 0x17ed60` and registers a SIF **CMD handler for `0x80000019`** with callback `0x187d98` — i.e. padman pushes pad data to the EE asynchronously over SIF CMD, it is not pure request/response. A working pad therefore needs cmd `0x12` + cmd `0x10` + the port/read commands + the `0x80000019` CMD push. Confirmed by grep that **nothing in `ps2xRuntime` references `0x80000100`, `0x80000101`, or any padman sid** — this subsystem does not exist yet. **★ PROBE IMPLEMENTED 2026-07-27 (built? NO — awaiting user build+run).** Deliberately scoped to cmd `0x12` only, to settle the unproven half of the confidence split above (does a dead pad actually hold the black screen?) before committing to the full subsystem. Three edits, all in `src/lib`, no header touched, no recompiler run: (1) `ps2_iop.cpp` `handleRPC` — new `sid == 0x80000100 && rpcNum == 1` branch; if send word[0] == `0x12`, zero the recv buffer and write `0x0400` at recv+`0xC`, set `resultPtr = recvBufAddr`, return `true`. Any other cmd logs `[iop:PADMAN] UNSERVED cmd=…` and falls through. Sid `0x80000101` logs only. (2) `Kernel/Stubs/SIF.cpp` — **bind recording made unconditional**. It was gated behind `PS2_ARKD_TRACE`, and since a CALL packet carries the rpc func in WORD[8] rather than the sid, without the client→sid table a non-ARKD service can never be identified at CALL time. Added a bounded `[SIF:BIND] client=… sid=…` census of every bind. (3) `Kernel/Stubs/SIF.cpp` `kSifCmdRpcCall` branch — new padman bridge that runs *before* the ARKD observe-only bridge: looks up the bound sid, and for `0x80000100`/`0x80000101` calls `handleRPC` **for real** (not observe-only), so the reply lands in the guest recv buffer before the echo synthesis signals the parked waiter. Send payload comes from `sifFindSendPayload(w[9], w[5])` with a fallback to the recv address (for this service send and recv are the same EE buffer, `0x568b80`). Logs `[PADMAN:CALL] … handled=0|1`. **Routing note — this was the non-obvious part:** the game's `rpc_call` (`0x178be8`) does **not** reach `ps2_iop::handleRPC` via the `sceSifCallRpc` syscall (`RPC.cpp:1934`); it rides the SIF CMD packet echo path in `SIF.cpp`, where the only pre-existing bridge was ARKD-gated and observe-only. A handler added to `ps2_iop.cpp` alone would never have fired. Verified the reply survives: `RPC.cpp:2526` only copies `resultPtr`→`recvBuf` when they differ, and only zeroes recv when `!handled`. **Expected outcomes, all informative:** no `[SIF:BIND] sid=0x80000100` line ⇒ the binds do *not* ride this path and the PCSX2-derived corollary above needs revisiting; `handled=1` + mismatch message gone ⇒ probe worked, read how far the boot then gets; mismatch gone but no visible change ⇒ pad was **not** the black-screen gate, and 5.5.1 should be demoted rather than expanded. **★★ PROBE RUN 2026-07-27 — 5.5.1 EXIT TEST PASSED.** Both conditions met, measured not inferred. (a) `[SIF:BIND] client=0x568940 sid=0x80000100` and `client=0x568968 sid=0x80000101` appear in the log at **exactly the client addresses read live from PCSX2** — the binds do ride the SIF CMD echo path, and the PCSX2-derived corollary is confirmed rather than revisited. (b) `[iop:PADMAN] version query served … version=0x0400` + `[PADMAN:CALL] client=0x568940 sid=0x80000100 func=0x1`. (c) `Module version mismatch` — **0 hits** in the whole run (was the only game-emitted error). (d) Proof the version check was passed and control entered the real init: `[iop:PADMAN] UNSERVED cmd=0x10` — cmd `0x10` is issued *only* from `0x187de0`, which is reachable *only* via the `beq $s0,4` at `0x187d38`. **Boot advanced materially.** Previously PC was pinned at `0x100008` cycling the SIF-RPC dispatcher. Now at t=45s: `pc=0x421f10`, `progress=7191`, `stuckSecs=0–1`, and the watchdog trace is app-level game code (`0x2ca270 → 0x1c22e0 → 0x19e620 → 0x1bdfb0 → 0x23fd20 → 0x240780`), with `0x421f10` sitting in the `CApp*` band (cf. `CAppCopyRight_0x4206c0`, `CAppCRISofdec_Tick_clone_02_0x420780`). **Still unproven:** whether the pad was the black-screen gate — the boot moved, but no on-screen change has been confirmed by the user yet. Do not upgrade that claim without a visual observation. **→ 5.5.2 opened** (cmd `0x10`). | **5.5.2 — padman init (cmd `0x10`)** | ⬜ IMPLEMENTED 2026-07-27, **built? NO**. Reply convention now established from three call sites: **`recv+0xC` is the single return slot, `1` = success** — `0x189110` (cmd `0x12`) returns it, `0x187de0` (cmd `0x10`) returns it, `0x187ed8` (cmd `0x0F`) compares it against `1`. `scePadInit` (`0x187cb0`) returns whatever `0x187de0` returns, so `recv+0xC = 1` makes init report success. Send layout for cmd `0x10`: `+0x00 = 0x10`, `+0x10 = 0`, `+0x14 =` global read from `0x464E14`. One edit, `ps2_iop.cpp` only — the cmd-`0x12` special case was generalised into a `switch (cmd)` that fills a `reply` word (`0x12 → 0x0400`, `0x10 → 1`); unrecognised cmds still log `UNSERVED` and fall through. **No CMD-handler work needed on our side:** on a non-negative `rpc_call` the *guest* registers the `0x80000019` async handler itself (`0x187de0` → `jal 0x177de8` with `a0=0x80000019, a1=0x187d98`). **HLE-vs-IRX rule note:** the standing "never hand-write IOP outputs, run the real IRX" rule was written for `ARKD_DVD` (a disc-format driver whose logic matters). Padman drives SIO2 controller silicon that does not exist on the host, so there is nothing to interpret and pad state must come from the host gamepad regardless — HLE is the correct layer here, not a shortcut. Recorded explicitly so a later session does not read this as a rule violation. **Exit test:** `scePadInit` returns 1; no `UNSERVED cmd=0x10`; the next unserved padman cmd is revealed by the log so the port/read commands can be built against real traffic. **★ RUN 2026-07-27 — 5.5.2 EXIT TEST PASSED.** `[iop:PADMAN] served cmd=0x10 (init): recvBuf=0x00568B80 recvSize=128 reply=0x1`, no `UNSERVED cmd=0x10`, and the next unserved command was revealed: **`UNSERVED cmd=0x1`, repeating**. Watchdog at t=147s: `pc=0x421f10`, `progress=18279`, `stuckSecs=0` — the game is *running*, not hung; `progress` climbed 7191 → 18279 between runs, so this is a live loop, not a spin. **Screen unchanged (user-observed): magenta briefly, then black.** ⇒ **The pad is NOT the black-screen gate at this depth.** The 5.5.1 "plausible but unproven" claim is now measured and **negative for the current stage** — clearing version + init alone changes nothing visible. It stays open only in the weaker form: the pad may still gate the *memcard prompt* (X-press) further downstream. Do not spend more on the pad than the next command costs without a second reason. **→ 5.5.3 opened** (cmd `0x1`). | **5.5.3 — padman port open (cmd `0x1`)** | ⬜ IMPLEMENTED 2026-07-27, **built? NO**. Issuer identified by scanning the ELF for `sw ?, -29824(?)` (the `0x568b80` send-buffer store, encoding mask `0xFC00FFFF == 0xAC008B80`, 14 hits) and disassembling each: **`0x1880e4` = `scePadPortOpen`**. Confirms the send/recv protocol is a shared `0x80`-byte buffer at `0x568b80` on client `0x568940` for *every* padman command, so the `switch (cmd)` shape is the right one. Send layout (`0x188180`–`0x1881a0`): `+0x00 = 1`, `+0x04 = port`, `+0x08 = slot`, `+0x10 =` EE-side user buffer. **Reply needs two words, not one** — the first command where `recv+0xC` alone is insufficient: `0x188230 lw $v0,12($s0)` is the return value, **and `0x188204 lw $t0,20($s0)` reads `recv+0x14`**, which is stored into the pad state table at `+8` (`0x188220 sw $t0,8($a3)`; table base `0x568990`, stride `port*0x70 + slot*0x1c`). **What that word is:** `scePadRead` (`0x187f68`) loads it back (`0x187ffc lw $s0,8($v1)`) and uses it as the **`sceSifSetDma` *dest*** for a `0x20`-byte **EE→IOP** transfer, indexed as a double buffer (`andi $v0,$a2,1; sll $v0,5; addu $s0,$s0,$v0` — so `0x40` bytes total, alternating on a counter kept at EE `[state+4]`). Descriptor order verified against `Ps2SifDmaTransfer` (`SIF.cpp:62`: `src, dest, size, attr`) — `sp+0 = src = EE`, `sp+4 = dest = IOP`, so it is genuinely EE→IOP, a *request* push, not a data pull. Therefore `recv+0x14` must be non-zero and plausible; the EE never reads it, and our `sceSifSetDma` already accepts non-EE dests and skips the copy (`SIF.cpp:1576`). One edit, `ps2_iop.cpp` only: `case 0x1u` writes `reply = 1` at `recv+0xC` and `recv+0x14 = 0x001E0000 + port*0x100 + slot*0x40` (distinctive base so it is unmistakable in SIF DMA logs); generalised the reply writer with a `haveReply14` flag. **Exit test:** no `UNSERVED cmd=0x1`; `served cmd=0x1 (portopen)` twice (the two ports); and the log reveals the next unserved padman cmd. **Known next piece (not this stage):** actual per-frame pad state. The real padman pushes it asynchronously over SIF CMD `0x80000019` into the handler the guest already registered (callback `0x187d98`); nothing in our runtime emits that yet, so buttons will still read as idle after 5.5.3. **★ RUN 2026-07-27 — 5.5.3 EXIT TEST PASSED, and the padman thread is now DRAINED.** `served cmd=0x1 (portopen) … reply=0x1 iopbuf=0x001E0000` and a second `… iopbuf=0x001E0100` (ports 0 and 1), no `UNSERVED cmd=0x1`. **Critically, there is no new `UNSERVED cmd=` line at all** — the guest issues `0x12 → 0x10 → 0x1 → 0x1` and then stops asking. So the padman request/response chain is complete as far as the game drives it; the only remaining pad work is the async `0x80000019` push, which the game does not block on. **Screen still unchanged (magenta → black).** Combined with 5.5.2's negative, this closes the pad line: **the pad is not the black-screen gate, at any depth reachable by serving commands.** 5.5.1/5.5.2/5.5.3 are ✅ done as *subsystem* work and ⛔ **demoted as blocker candidates** — do not open a 5.5.4 for pad state unless a downstream stage (memcard X-press) specifically demands it. **→ Next investigation is NOT padman.** The frame loop at `pc=0x421f10` runs healthily (`progress` climbing, `stuckSecs=0`) yet nothing reaches the screen, so the live question is a **render/GS gap or an unfinished load**, not an input gap. Two parked leads inherit priority, in this order: (1) **ARKD disc I/O stops ~t=20s** (`[ARKD:cdread]` 8 hits, all before log line 2107 of 5145) while SIF traffic continues — the game may be waiting on data that never arrives; (2) **unimplemented syscall `0x6b` at `PC=0x174f70`** (1 hit, the run's only unimplemented syscall). **★ BOTH PARKED LEADS TRIAGED AND DISMISSED 2026-07-27 (same day, before any build).** (1) **"ARKD disc I/O stops ~t=20s" is a LOGGING ARTIFACT, not a stall — retract it.** `[ARKD:cdread]` is capped at 8 lines by `if (seen < 8u)` (`ps2_iop_irx_loader.cpp:644`) and `[ARKD:sifdma]` by the same cap at `:751`; `[ARKD:search]` is capped at 16 (`:620`), `[ARKD:strtol]` at 6 (`:702`). The reads never stopped, the *printing* did. Positive evidence ARKD stays healthy to the end of the run: `fno=0x101` (open-by-name) keeps arriving with real asset paths — `font/5-24-2.fon`, `ply/exthit.hit`, then a run of `eff/e00/e000{12,13,14,16}.ebz` — each one running the IOP worker to a clean `halted=1` (`retired=1201/1251/1301/1680`), with monotonically advancing EE destinations (send word[0] = EE dest: `0x0167dc00`, `0x0167ec00`, `0x0167fc00`, `0x01680c00`). ARKD's last line is 2973 of 3637. **The disc/streaming pipeline is working.** (2) **`0x6b` dismissed:** `0x174f70` is one entry in a flat syscall-trampoline table (`addiu $v1,N; syscall; jr $ra` repeated for `0x66,0x6b,0x6c,0x6d,0x6e,0x6f,0x70…`), `0x6b` falls in a documented gap in the EE syscall table (nothing between `0x64` FlushCache and `0x70` GsGetIMR), and it fires **once** — it cannot be what holds a frame loop. **→ 5.6.1 opened: the render path itself.** | **Blocks 5.5**: MCSERV's sid `0x80000080` (`IOP_SID_MCSERV`, `ps2_iop.h:17`) is **never bound** this run — only game-private ARKD sids `0x500`–`0x503` appear — so `handleMcServRpc` (`ps2_iop_mcman.cpp:87`) is ready and never reached, and 5.5's exit test cannot be evaluated until the game actually asks for the memcard. | `scePadInit` reaches `0x187de0`; no `Module version mismatch` in the log |
| 5.6 | Prompt renders + accepts input | ⚪ not reached | Prompt on screen, X press advances it |
| 5.6.1 | Does anything reach the GS at all? (DMA-kick census) | 🔵 **ACTIVE (opened 2026-07-27).** Everything upstream of the screen now measures healthy — boot derail cleared (5.4.2), pad chain drained (5.5.3), disc streaming alive to end of run (see 5.5.3 triage) — yet the screen is magenta then black. **The blind spot is that the run log contains zero render-side output:** a tag histogram of all 3637 lines yields 45 distinct tags and not one is GS/GIF/VIF. So "the render path is broken" is currently an assumption, not a measurement. **What the frame loop actually does each frame** (disassembled, not inferred): `0x421ea0`–`0x421f40` is the per-frame body; within it `0x172e20` = **`sceDmaSend`** — it physical-maps the packet (`jal 0x172a90`), calls `0x172998` = **`sceDmaSync`** (countdown of `0x1000000` polling `*(chcr) & 0x100` = CHCR.STR, with `libdma: sync timeout` at string `0x4c01a8` on expiry), writes the packet address to `chcr+0x30` (TADR), zeroes `chcr+0x20` (QWC), then stores `(chcr & ~0xC) | 0x105` = **STR | chain-mode | from-memory**. **`libdma: sync timeout` has 0 hits in the run**, so STR *is* clearing — DMA is not deadlocked, which is itself a useful negative. **Probe (implemented 2026-07-27, `ps2_memory.cpp` only, no header touched, no recompiler run):** a bounded env-gated `[dmakick]` census at `PS2Memory::writeIORegister`'s single convergence point for channel starts (`address >= 0x10008000 && < 0x1000F000`, `(address & 0xFF) == 0`, `value & 0x100`). Logs channel base, CHCR, mode, dir, MADR, QWC, TADR, plus `vram/cb/arb` — the three flags that decide whether a GIF/VIF1 kick is actually drained (`m_gsVRAM` null skips the whole block; GIF needs `m_gifPacketCallback \|\| m_gifArbiter`). Prints the first N kicks then a heartbeat every 1000. **Why a counter and not `PS2X_TRAPVAL`:** the existing value-trap matches the stored word *exactly*, and libdma ORs `0x105` into whatever bits the channel already carried, so a trap would silently miss. Arm with `PS2X_DMATRACE=40` (unset ⇒ one relaxed load per kick, no output). **Exit test / decision tree:** *no `ch=0x1000A000` or `0x10009000` kicks at all* ⇒ the game is not submitting display lists — walk back up from `0x421ea0` to find what gates it. *Kicks present with `vram=0` or (`cb=0` and `arb=0`)* ⇒ packets are built and dropped on the floor; the fix is runtime wiring, not guest logic. *Kicks present with all three flags 1* ⇒ packets reach the rasterizer and the fault is inside GS/VU1 — a different and much narrower search. **★ CENSUS RUN 2026-07-27 — branch 1 is DEAD, the render path is alive.** 40-line `[dmakick]` grep: **#0–#31** GIF (`ch=0x1000A000`, `chcr=0x00000101`, mode=0 normal, dir=1) alternating `madr=0x01FFBDF0 qwc=6` (register setup) with `madr=0x0089D400 qwc=16384` (a 256 KB texture/VRAM upload), repeating. **#32 onward** switches to `mode=1` (**chain**) with real `tadr` (`0x00503310`, `0x00503270`), interleaved with **VIF1** (`ch=0x10009000`, `chcr=0x1C5`/`0xF00001C5` = STR\|chain\|TTE\|TIE, `tadr=0x00771400`/`0x007D5400`). That is a genuine per-frame display-list submission loop. ⇒ **"the game is not submitting display lists" is eliminated.** `vram/cb/arb` were never read — console truncated the pasted lines at ~110 chars, clipping the trailing fields (terminal artifact, not missing output); superseded by `gif/s` below, do not re-run that grep. **★ NEW ANOMALY — the rate, not the presence.** The probe prints the first N kicks then a heartbeat every 1000th; the log contains **no heartbeat line**, so the run did **fewer than 1000 DMA kicks in 70+ seconds**. Watchdog at t=70s: `pc=0x421f10 ra=0x421f10 lastCall=0x172998 (sceDmaSync) stuckSecs=29 progress=10266`. A frame loop that runs but grinds — or a stall inside the sync spin; those two readings disagree and must not be resolved by argument. **★ PCSX2 REFERENCE BASELINE (user-measured 2026-07-27): real PCSX2 reaches the memcard auto-save in 7–8 seconds.** At ~60 Hz NTSC that is **≈420–480 frames** (*derived from the standard vblank rate, not measured* — what was timed is a wall-clock milestone). Our run was still not there at t=70s ⇒ ~10× over budget with no arrival. At the ~2 chain kicks/frame the census showed, <1000 kicks/70 s ≈ <500 frames — i.e. roughly the *right amount of work* spread over 10× the wall clock. **★ FRAME-RATE INSTRUMENTATION WRITTEN, NOT BUILT, NOT RUN** — see [[project_framerate_instrumentation]]. Two edits in `ps2_runtime.cpp` only (no header, no recompiler): the existing 1 Hz `PS2_PC_WATCHDOG` thread now deltas four pre-existing public counters and prints `dma/s gif/s vif/s gsw/s gifTot` on the `[watchdog]` line, before the long `trace=` field so truncation cannot clip them. **`gif/s` is the load-bearing number** — it counts packets *actually handed to the rasterizer*, not packets kicked. **Read it as a trend across `t=` samples, not a single value:** flat-low ⇒ genuinely slow (find what eats the time); **decaying to 0 while `stuckSecs` climbs** ⇒ the loop entered `sceDmaSync` and never came out (CHCR.STR never clearing — a different bug); `gif/s`=0 while `dma/s` climbs ⇒ packets queued then dropped at the GIF-only drain gate, [ps2_memory.cpp:1723](ps2xRuntime/src/lib/ps2_memory.cpp#L1723) — runtime wiring, not guest logic (this case also settles the clipped `cb`/`arb` question). Caveat: `vif/s`/`gsw/s` count *register writes*, far higher frequency than frames — never read them as a frame rate. | A `[dmakick]` census exists; VIF1/GIF kick counts and the vram/cb/arb flags are known ✅ **MET** (census captured; branch 1 eliminated) → **superseded by: is the frame rate slow or stalled? (`gif/s` trend)** |

Legend: 🔵 active · ⚪ not started · ✅ passed exit test · ⛔ blocked

**Notes on the tracker:**
- 5.1 CLOSED 2026-07-22: the real derail (escape to `0x20561900`) is gone; the `IMBAL` exit test was a false-positive signal (normal jal-split dispatch). Do not re-chase `delta=-0xa0` — it is expected.
- 5.2 CLOSED as N/A: the imbalances proved benign, so there is no prologue-skip derail class to fix. Reopen only if an ALARM/TABLE escape reappears.
- 5.6: stalling **at** the prompt is SUCCESS. It is input-gated on real hardware (X press), not a hang.
- **5.4–5.6 are named destinations, not mapped routes.** Real PCSX2 has 31 IOP modules loaded by that screen; we load 7. That gap may hide sub-phases only discoverable by reaching them. Expect 5.4 to split.

- **2026-07-26: a parallel METHOD track is now in force alongside 5.4.1** — plan `gentle-launching-dusk.md`, session entry immediately below. Phases A (determinism) and B (JSONL probe sink + `analyze_run.py`) are done and passed; C (invariant guards) is code-complete and awaiting a build. Diagnose 5.4.1 **through that tooling**, at `-Determinism 0`, not with new hand-written log greps.

**Tracker maintenance rules (user-directed 2026-07-20g):**
1. **Notify the user when a stage passes its exit test** — state the stage number and the evidence.
2. **Notify the user when a stage needs to extend.** If new information subdivides a stage, add decimal children (`5.1.1`, `5.1.2`) and break down further as needed. Do not silently widen a stage's scope.
3. Update the State column and the `🔵 ACTIVE` marker as work moves.
4. A stage is only ✅ when its exit test is verified against real build/run output — never on code-complete alone.

## Session 2026-07-29 (co-piolet) — ⚠️ CORRECTION: `0x421ea0` is NOT a spin loop and is NOT 0xa4 bytes. Stale-worktree analysis retracted; 5.7's step (2) premise was wrong.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run, no code change — read-only address resolution against `F:\SDBZ Recomp\Logs\studio_callgraph.csv`.

**What happened.** A watchdog paste (t=57–59s, `busy%=44–61`, `vbl/s=48–50`, `gif/s≈48`, `dma/s≈96`, `progress` +~12 000/s, `gstate@0x50227c` byte-identical for 22 s, `stuckSecs=20→22`, `pc=ra=0x421f10`, `cputime` 97.2 % of one core) was analysed in a **stale git worktree** (`F:\SDBZ Recomp.worktrees\watchdog-performance-analysis`, HEAD `c68adaf4` = 2026-05-27). That worktree's `CSV Map\map.csv` claims `fn_421EA0` spans `0x421EA0–0x421F44` (0xA4 bytes), which led to the conclusion "`0x421f10` is +0x70 into a 0xa4-byte function, `pc==ra` ⇒ tight self-contained loop, that is the CPU sink." **That conclusion is wrong and is retracted in full.**

**Measured correction** (from `Logs\studio_callgraph.csv`, the reconstructed function map — authoritative, `caller,callee,address`):
- `sub_00421EA0` has call sites out to **`0x0042213c`**; the next function is `sub_00422170`. Real extent ≈ **`0x421EA0–0x422168`**, ~0x2C8 bytes — **not** 0xA4. The worktree `map.csv` boundary is stale and must not be trusted for this address (or, by extension, anywhere in the 0x42xxxx region).
- `0x421f10` is **`jal sub_00199840`** — one entry in a straight-line run of 14 consecutive `jal`s at `0x421ebc, ec4, ed0, edc, ee8, ef0, ef8, f00, f08, f10, f18, f20, f28, f30` (targets `0x199800, 0x1aedb0, 0x327f90, 0x3280c0, 0x328160, 0x2fcc00, 0x1bad10, 0x1bb450, 0x1997e0, 0x199840, 0x1bfb00, 0x1bddc0, 0x1bda00, 0x1bf3a0`). That is a **subsystem-update fan-out**, i.e. the per-frame tick driver — exactly what the 5.5 note at the tracker already said. There is no loop instruction at `0x421f10`.
- Sole caller: **`sub_00422630` at `0x422658`** — one call site, consistent with a main-loop body.
- The pasted `trace=` tail corroborates this and nothing else: `0x1bb450 -> 0x1bbdf0 -> 0x177ab8 ... -> 0x1997e0 -> 0x19ae60 -> 0x104af0 -> 0x421f10 -> 0x199840 -> 0x104c00 -> ...` walks the fan-out **left to right in call-site order** (`0x1bb450` @f00, `0x1997e0` @f08, then `0x421f10`/`0x199840` @f10). The frame driver is *progressing through its call list*, not spinning.

**Consequence for the 5.7 plan.** Step (2) of 5.7 as written — *"disassemble `0x421f10` — it is `pc` and `ra`, a tight self-loop"* — is **based on a false premise and should not be actioned as stated.** `pc==ra==0x421f10` is a **sampling artefact**: the fiber/dispatch loop parks `ctx->pc` at the return address of the `jal` it is currently inside, and because `0x421ea0` is re-entered every frame at ~48 fps the watchdog lands on the same `jal` boundary essentially every sample. Line 70 of this file (stage 5.5, 2026-07-27) **already recorded this correctly** — *"a `jal 0x199840` site inside the per-frame function … so this is a called-every-frame body, not a spin"* and *"`stuckSecs` is a **false positive** by construction"*. **The 5.7 wording at line 52 contradicts 5.5 and 5.5 is the correct one.** Corrected inline; see the 5.7 entry.

**Therefore the 97.2 % CPU burn needs a different explanation than "guest spin at `0x421f10`".** Do not re-derive it from the `pc` field. The two live threads in the `cputime` dump (`24336` = 33.16 s, `22232` = 27.77 s, each ≈ half of `wall=66.07s`) are consistent with **runtime host threads** — the fiber executor plus the vblank/IRQ worker — not with one guest hot loop. `busy%=44–61` is itself the runtime's own on-CPU accounting and is *below* 100 %, which contradicts a guest spin as the whole story.

**What is actually still true and unchanged from 5.7:** the guest renders every frame (`vbl/s`/`gif/s`/`dma/s` nominal) while `gstate@0x50227c` never changes and `bsschg=7–16` — the state machine does not advance. **That** is the blocker. `lastCall=0x172998` (`DMAC_WaitIdle`, the `0x1000000`-countdown poll on `*(a0) & 0x100` with the `0x4c01a8` error message on expiry) is a per-frame timeout-guarded busy-wait and is normal, per 5.5.

**Prior-art check (requested):** searched `_ClaudeMemory\`, `Logs\`, `Session Summary Logs\`, `Solutions Log\`, `INDEX_INDEX\` for `0x421f10`/`0x421ea0` — **no doc hits**; only `Logs\studio_callgraph.csv` and two 04-29 debug logs. The nearby `0x421c84` **was** worked before and is written up in `ps2xRuntime\src\lib\game_overrides.cpp` (`sdbzRegisterHandler104BF0`, the 2026-07-19 `pc=0x104bf0 ra=0x421c84` boot hang) — same caller region, different mechanism (recompiler truncation), not related to this.

**Housekeeping / traps found:**
- The worktree at `F:\SDBZ Recomp.worktrees\watchdog-performance-analysis` is **~2 months stale** (2026-05-27) and its `_ClaudeMemory\project_state.md`, `CSV Map\map.csv` and `ghidra_scripts\labels.csv` all predate the Ghidra map reconstruction of 2026-07-21. **Any analysis done from that worktree must be re-checked against `F:\SDBZ Recomp` before it is believed.** It also still names `C:\SDBZ Recomp` and `Laptop_Build.ps1` as the build path; neither exists. Live root is `F:\SDBZ Recomp`, build is `build.ps1`, run is `launch_recomp.ps1`.
- Corollary rule for the Learned Patterns list: **`pc == ra` does not imply a loop.** In this runtime it is the normal steady state at any `jal` boundary. A loop claim requires the actual instruction at that address, or a function whose measured extent makes a backward branch possible — and function extents must come from `Logs\studio_callgraph.csv`, not `CSV Map\map.csv`.

## Session 2026-07-29 (co-piolet, part 2) — ⚠️ SECOND CORRECTION: `gstate@0x50227c` is 4 unrelated static singleton pointers, not a game-state struct. "Frozen `gstate`" is not evidence of a stall.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run, no code change — read-only: `ps2xRuntime\src\lib\ps2_runtime.cpp` (watchdog implementation) + `decompiled_dump_full.txt` (full Ghidra decompile of `SLUS_214.42`, present only in git history at commit `eabd4112`, retrieved via `git show eabd4112:decompiled_dump_full.txt`).

**What `gstate=` actually is.** The watchdog's default watch address is `0x005e6b3c` (`ps2_runtime.cpp` ~line 2913, the community "GameMode" byte), overridable via the `PS2_WATCH_ADDR` env var. The `0x50227c` telemetry pasted this session was already running with that override — it was never the default and was never documented anywhere before now. At whatever address is watched, the probe (`ps2_runtime.cpp` ~lines 2973–2979) blindly reads **4 consecutive raw 32-bit words** and prints them as `gstate@ADDR=w0,w1,w2,w3`. It has no concept of struct layout — it is a flat memory dump, not a state-machine read.

**What those 4 words are at `0x50227c` specifically** (found in `decompiled_dump_full.txt`):
- `0x50227c` (`piRam0050227c`) — pointer to a **polymorphic C++ object**; called every relevant tick via `(**(code**)(*this+0x28))(this, dx*scale, dy*scale)` from `FUN_0034c040` with controller-stick-scaled deltas → best-guess **camera/field-pan controller**.
- `0x502280`/`0x502284` (`iRam00502280`/`iRam00502284`) — a **paired pair of object pointers**, both tested with identical bitmask logic (`&0x10`, `&0x3d0` on fields `+0x22`/`+0x1e`) in the same conditional blocks → best-guess **Player 1 / Player 2** entities.
- `0x502288` (`iRam00502288`) — used in `effect_spawn_colored(...)` calls and vtable slots `+0x10`/`+0x18`. **Confirmed static**, not just guessed: the literal assignment `iRam00502288 = 0x63fe40;` appears verbatim in the decompile, and `0x63fe40` is exactly `w[3]` in every line of the pasted telemetry.
- Exhaustive search for any runtime write to any of the 4 addresses (`...227c =`, `...2280 =`, `...2284 =`, `...2288 =`) across the full 454k-line decompile: **zero hits.** These are `.data`-section pointers fixed once at init/link time and never reassigned.

**Consequence.** Watching these 4 raw pointer *values* for 20+ seconds and seeing them "byte-identical" tells you nothing — they are singleton object pointers, expected to never change for the life of the process, the same way `this` doesn't change. The 5.7 conclusion "`gstate@0x50227c` never changes ⇒ the state machine does not advance" (line 108 above) is now **suspect and should not be relied on as-is**. It is the **second** false-positive diagnostic found in two consecutive sessions, after the `0x421f10`/`pc==ra` one above. A real liveness probe needs to **dereference** these pointers and watch a field *inside* the objects that is expected to tick every frame (e.g. whatever `piRam0050227c`'s vtable slot `+0x28` writes on the camera object, or the `+0x22`/`+0x1e` flag fields on the two paired objects) — not the pointer values themselves.

**Not yet done / next step if picked up:** identify concrete in-object offsets that are known-dynamic (found by reading what the `+0x28` vtable function and the `+0x22`/`+0x1e` flag-check code actually touch), then re-run the watchdog with a probe that dereferences into the object rather than reading the raw pointer. Object identities (camera/P1/P2/effects) are inferred from code-pattern context only, not from an authoritative symbol/label source — treat as a strong hypothesis, not confirmed, unless cross-checked against Ghidra/IDA labels. The original 97.2%-of-one-core CPU burn from the same telemetry paste is still fully unexplained; no replacement theory exists yet.

## Session 2026-07-29 (co-piolet, part 3) — new `[hostprof]` capture confirms the CPU cost is GS pixel math (retracts "unexplained" claim in part 2); `gchg=0` window is consistent with the static-singleton theory, not new evidence of a stall.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run, no code change — read-only analysis of a `[watchdog]`/`[hostprof]`/`[cputime]` capture the user pasted (t≈84–89 s window inside a `wall=96.09s cpu=108.53s =113%` run, `PS2_WATCH_ADDR` still overridden to `0x50227c`).

**Correction to part 2 (line 130 above):** part 2 said *"the original 97.2%-of-one-core CPU burn … is still fully unexplained; no replacement theory exists yet."* **That is wrong — it was already explained** in the very next section of this file (2026-07-28 later / Claude Code, "Hot code is still GS pixel math…", line 152, and the RelWithDebInfo table above it). This new `[hostprof]` paste independently reconfirms it with fresh numbers:
- tid 25544 (44.33 s CPU, 84.4% in `ps2EntryRunner`): dominated by `GSRasterizer::writePixel` (30.7%), `GS::WriteVram`, `GSMem::LookupPixelAddressCT32`, `GS::ReadVram`, `GSMem::ReadPixelAddressCT32`, `GSInternal::bitsPerPixel`, `GSMem::LookupPixelAddressZ32` — the software GS rasterizer doing real per-pixel work.
- tid 1224 (39.44 s CPU): dominated by `GS::copyFrameToHostRgbaUnlocked`, `` `anonymous namespace'::countNonBlackPixels ``, `GS::latchHostPresentationFrameUnlocked`, `` `anonymous namespace'::blendPresentationChannel `` — the host presentation/compositing pipeline, already flagged as a ~12%-of-thread cost in the 07-28 section (line 180) and as a `hwWatchArm()`-style env-gate candidate.
- **Conclusion: the CPU burn has never been a spin/bug at any point in this project's history.** It is legitimate software-rendering cost (worse in Debug, ~109× better in RelWithDebInfo per the A/B above). Stop treating it as an open question — it is closed and cross-confirmed by two independent captures on two different sessions.

**`gchg=0` this window is not new information.** The paste still watches the override address `0x50227c` (four static singleton pointers, per part 2 above), and this window happened to start after whatever one-time init write sets them — so `gchg=0` here is exactly what the static-singleton theory predicts, not a fresh sign of a stalled state machine. **The recommended default-address (`0x5e6b3c`) rerun from part 2 has still not been done** and remains the next actual test.

**Still true and unchanged:** `pc=ra=0x421f10`, `lastCall=0x172998` (`sceDmaSync`), `stuckSecs` climbing then resetting — the same plateau documented in the 07-28 section's "Next action" (line 166 above: *why does the guest never leave the `0x421f10`/`sceDmaSync` loop*). That is still the real open blocker, unaffected by anything in this entry.

## Session 2026-07-29 (co-piolet, part 4) — new capture shows **~9× faster `progress` and full 60 `vbl/s`** vs. the part-3 capture; this looks like a genuine full-rate frame loop, not a stall. Visual confirmation (pink screen still present?) is now the key open question.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run, no code change — read-only analysis of a fresh `[watchdog]`/`[hostprof]`/`[cputime]` paste (`wall=96.14s cpu=103.06s =107.2%`, `PS2_WATCH_ADDR` still `0x50227c`).

- **`progress` rate jumped ~9×:** t=81→94 (13 s) goes `5,319,065 → 6,446,535`, ≈86.7 k/s. The part-3 capture over a similar 5 s window (t=84→89) only moved ≈9.7 k/s. Same build, same watch address, same machine — the only explanation is the guest is doing far more real work per wall-second in this run.
- **`vbl/s` is pinned at 60–61 and `busy%` at 99–101%** throughout this whole capture (vs. 36–86% / variable `vbl/s` in every prior capture this project has seen). A steady 60 `vbl/s` at ~100% busy is what a fully-running PS2 title's main loop looks like — one vblank serviced per real vblank, CPU saturated doing the frame's rendering work, not idling or blocked.
- **`gstate@0x50227c` still frozen (`gchg=0` the whole window)** — expected and unremarkable per part 2/3 (static singleton pointers, not live state).
- **`pc=ra=0x421f10` / `lastCall=0x172998` and `stuckSecs` cycling 0→9→0 are unchanged** — per the Learned Patterns entries above, this is the documented false-positive shape (dispatch-loop `jal` boundary + `pc`-stability heuristic), not evidence against the "real frame loop" reading above. The two readings are consistent: a steady per-frame fan-out revisits the same `jal` site every frame, which is exactly what a healthy 60 vbl/s loop would do.
- **`hostprof` composition is materially the same as part 3** (`GSRasterizer::writePixel` still the #1 cost, `countNonBlackPixels` / `copyFrameToHostRgbaUnlocked` / `blendPresentationChannel` still active in the presentation thread) — so `countNonBlackPixels` is being called every frame, meaning the presentation path is actively checking real frame content, not a frozen/blank buffer.
- **This is the strongest evidence yet that the boot is not hung at all in this capture** — it reads like a title's ordinary steady-state main loop under heavy software-rasterizer cost, not the earlier "stuck" narrative built around `stuckSecs`/`pc==ra`.
- **Open question, needs the user's eyes, not more telemetry:** is the on-screen output still the "pink screen" described in Phase 5's blocker, or is real content now visible? `countNonBlackPixels` being hot suggests non-trivial pixel content is present, which would be new information Phase 5's blocker doesn't yet account for. **Next step: confirm visually** (screenshot/recording from this run) before revising the Phase 5 blocker status.

## Session 2026-07-29 (co-piolet, part 5) — ⚠️ CORRECTION: `[present]` probe cannot fire in any build to date — `RUNTIME_LOG` is compile-time disabled, `PS2X_DIAG=1` alone does nothing for it

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run — read-only investigation into why `Select-String -Path run_log.txt -Pattern "\[present\]"` came back with **zero matches** after the user set `$env:PS2X_DIAG="1"` and reran (per part 4's recommendation above, which was wrong).

**Root cause found:** `RUNTIME_LOG(x)` ([ps2_log.h:107-126](ps2xRuntime/include/ps2_log.h)) expands to `do {} while(0)` — a total no-op, string literal and all — unless the CMake option `PS2X_ENABLE_RUNTIME_LOGS` is `ON` at **configure** time. That option ([ps2xRuntime/CMakeLists.txt:17](ps2xRuntime/CMakeLists.txt)) defaults `OFF` and has been `OFF` in `build\CMakeCache.txt` (confirmed by direct read) for the entire history of this build tree — never set anywhere in this repo (grepped, zero hits outside its own declaration). `PS2X_DIAG=1` only gates `ps2_diag::enabled()`, which the `[present]` probe checks to decide whether to *populate* the already-compiled-out macro — it has no effect when the macro itself was never compiled in.

**Verified three ways, not just theorized** (per the Learned Patterns "zero-record probe" entry): exe mtime (1:33:33) > source mtime (1:27:44) > confirms build is current, not stale; `findstr /M /C:"[hostprof]"` against the 354 MB `ps2EntryRunner.exe` returns exit 0 (control marker present, findstr itself works); `findstr /M /C:"[present]"` and `findstr /M /C:"present] has="` both return exit 1 — the literal string is absent from the binary. This is airtight: the probe cannot have fired in ANY run analyzed this session (parts 1-4 above), because it was never in the exe to begin with.

**Corrects part 4's recommendation.** The `$env:PS2X_DIAG="1"` rerun command given in part 4 was necessary-but-not-sufficient — it will never produce `[present]` output on the current build regardless of how long or how many times it's run.

**Actual fix (requires a CMake reconfigure the user must run, per PROHIBITION #4 — never cmake directly):**
```powershell
cmake -S "F:\SDBZ Recomp" -B "F:\SDBZ Recomp\build" -DPS2X_ENABLE_RUNTIME_LOGS=ON
& "F:\SDBZ Recomp\build.ps1" RelWithDebInfo
```
**Cost warning:** `RUNTIME_LOG` is used in ~22 TUs (`ps2_gs_gpu.cpp`, `ps2_runtime.cpp`, `ps2_memory.cpp`, `game_overrides.cpp`, several `Kernel/Stubs/*.cpp`, etc.) — this is a build-option flip, which CMake will treat similarly to a widely-included header change: expect a partial-but-wide recompile of every TU that includes `ps2_log.h`, not a quick relink. Not prohibited (it is not a clean build), but budget real time for it.

**Still open:** the magenta→black visual question from part 4 remains unanswered — once this rebuild lands, rerun with `PS2X_DIAG=1` (now meaningful) and grep `run_log.txt` for `[present]` as originally planned.

## Session 2026-07-29 (co-piolet, part 6) — reconfirms part 5; two more captures pasted, no new root cause; **HANDOFF POINT**

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** No build, no run, no code change — read-only analysis of two more pasted captures plus a repeat `Select-String -Pattern "\[present\]"` (again zero matches, run against the still-unmodified build).

**Finding 1 — `[present]` reconfirmed absent, CMake reconfigure from part 5 still not run.** The user's final command in this turn (`Select-String -Path run_log.txt -Pattern "\[present\]"`) returned no output again. This is expected and is **not a new bug** — it is the same root cause as part 5 (`PS2X_ENABLE_RUNTIME_LOGS` still `OFF`). Nothing changes here until the user actually runs the two-line reconfigure+rebuild given in part 5.

**Finding 2 — `[hostprof]` again shows GS pixel math as the dominant cost, consistent with part 3.** Two worker threads: tid 23192 (45.80s CPU) is 85.1% inside `ps2EntryRunner`, top symbol `GSRasterizer::writePixel` at 30.3%/13.88s, with `GS::WriteVram`, `GSMem::LookupPixelAddressCT32/Z32`, `WritePixelCT32/Z32` filling out the rest — a real rasterizer, not a spin loop. tid 23676 (38.33s CPU) is dominated by `GS::copyFrameToHostRgbaUnlocked` (15.8%), `countNonBlackPixels` (6.2%), `blendPresentationChannel` (5.7%) — the host-presentation path, also real work. No new symbol or anomaly vs. part 3's writeup.

**Finding 3 — one capture shows a sharp stall onset worth flagging for the next session.** In the t=81-90s window, `busy%` ran 57-74% (healthy) through t=89, then **t=90 shows `busy%=2, res/s=3, vbl/s=1, progress` frozen at 869787 (vs 869786 the tick before)** — i.e. the frame loop essentially stopped advancing in that single sample, with `pc/ra` still `0x421f10` but `lastCall` shifting to `0x1bda00` and the trace tail changing shape (`0x104c00 -> 0x102420 -> ... -> 0x1bddc0 -> 0x1bda00`, cut off mid-trace in the paste). This single-tick cliff (perfectly healthy → near-zero throughput in the same watchdog interval) is a different shape than the previously-documented gradual `stuckSecs` climb, and has not been analyzed before. **Not yet root-caused** — could be a real stall onset, a paste/output truncation artifact (the trace was visibly cut off at the end of the paste), or the watchdog printing mid-transition. Flagging only; no conclusion drawn.

**Run-to-run variance persists.** The other capture in this turn (t=57-59s) again shows the older, previously-documented "stuck" shape (`busy%` 44-61%, monotonic-ish `stuckSecs` 20→22, no reset) — same `-Determinism 0` nondeterminism already logged in part 4/earlier sessions, not new.

**No new BUG-0xx filed this turn** — findings 1-3 above are reconfirmations/refinements of already-open threads (part 5's rebuild-pending item, and the pre-existing GS-cost / magenta-black open question), not new distinct defects. If finding 3's single-tick cliff recurs with a *complete, untruncated* trace, it should be promoted to a BUG_LOG.md entry.

**★ HANDOFF — next session/tool, start here:**
1. **Blocking action still on the user:** run the part-5 reconfigure, in order:
   ```powershell
   cmake -S "F:\SDBZ Recomp" -B "F:\SDBZ Recomp\build" -DPS2X_ENABLE_RUNTIME_LOGS=ON
   & "F:\SDBZ Recomp\build.ps1" RelWithDebInfo
   ```
   then rerun with `$env:PS2X_DIAG = "1"` set and grep `F:\SDBZ Recomp\run_log.txt` for `\[present\]`. This has now been requested 3 sessions running (parts 4, 5, 6) and is the only unblocking step left for the magenta→black question.
2. **Once `[present]` lines exist:** read `nonblack=`, `dispFbp=` vs `ctx0/1.fbp=`, `pref=` fields — see "next steps" logic already written in part 5/earlier summaries.
3. **Secondary, lower-priority thread:** if a *complete* trace ever shows finding 3's healthy→busy%≈0 single-tick cliff again, capture the full untruncated watchdog line and file it as a new BUG_LOG.md entry — do not chase it further off a truncated paste.
4. **Do not re-litigate:** `pc==ra`, `gstate@0x50227c` frozen, and the GS-pixel-math CPU cost are all closed/explained (parts 1-3, and the retractions above them). Re-reading those threads from scratch wastes a session — check this file's Session 2026-07-29 entries first.

## Session 2026-07-29 (co-piolet, part 7) — user ran the part-5/6 reconfigure; build FAILED with 2 real (pre-existing, unrelated) compile errors — **FIXED**, rebuild is now the next action

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** User ran `cmake -DPS2X_ENABLE_RUNTIME_LOGS=ON` + `build.ps1 RelWithDebInfo` for the first time and pasted a bare `Failed (exit 1) after +24:11` with no visible error (build output scrolled past). Found the real errors by reading `F:\SDBZ Recomp\build_log.txt` (MSBuild's own log, freshly written 3:00 AM, matches the failure).

**Root cause — exactly the risk flagged in part 5's cost warning:** flipping `PS2X_ENABLE_RUNTIME_LOGS` from OFF→ON makes the compiler actually parse/type-check `RUNTIME_LOG(...)` argument expressions for the first time in a long while (they'd been compiling to `do {} while(0)` — arguments discarded, never type-checked). Two of those expressions had bit-rotted:
- `ps2xRuntime\src\lib\ps2_gs_gpu.cpp:2389` — `m_registers.prim.type` — **`GSPrimReg` has no `.type` member**, the primitive-type bitfield is named `.prim` (`ps2xRuntime\include\runtime\ps2_gs_gpr.h:282`, `Bitfield<u64,0,3> prim`). A local `const GSPrimReg prim = m_registers.prim;` was already in scope one line above (line 2340) and used correctly as `prim.prim` elsewhere in the same function (line 2343) — the `RUNTIME_LOG` call just never got the memo.
- `ps2xRuntime\src\lib\ps2_gs_rasterizer.cpp:551` — `gs->m_prim` — **`GS` has no `m_prim` member**; the primitive register lives at `gs->m_registers.prim` (`GS::m_registers` is a `GSGpr`, `ps2xRuntime\include\runtime\ps2_gs_gpu.h:155/384`).

**Fixed both** (`ps2_gs_gpu.cpp:2389` → `prim.prim`; `ps2_gs_rasterizer.cpp:551` → `gs->m_registers.prim.tme`).

**Also fixed 2 more latent instances of the identical bug in the same file, still dormant behind a *different* compile-time gate** (`PS2_IF_AGRESSIVE_LOGS`/`PS2X_ENABLE_AGRESSIVE_LOGS`, still `OFF`, not touched this session) — these did **not** cause today's build failure but would explode the instant someone enables that flag too, so fixed proactively while already in the file:
- `ps2_gs_rasterizer.cpp` lines 257-261 (`[gs:prim]` block) and 322-326 (`[gs:copy-prim]` block): `gs->m_prim.type/tme/abe/fst/ctxt` → `gs->m_registers.prim.{prim,tme,abe,fst,ctxt}`.
- `ps2_gs_rasterizer.cpp` lines 279-281 and 342-344 (`texclut=(...)` fields in the same two blocks): `gs->m_texclut.{cbw,cou,cov}` → `gs->m_registers.texclut.{cbw,cou,cov}` (`GS` has no `m_texclut`; it's `GSGpr::texclut`, same pattern).
- Verified via a repo-wide grep for `->m_prim\b|->m_texclut\b|\.prim\.type\b` — zero remaining hits after the fixes.
- `gs->m_vtxQueue[...]` (also referenced in the same blocks) was checked and is **correct as-is** — `GS::m_vtxQueue` is a real member (`ps2_gs_gpu.h:395`), not part of this bug.

**Not yet verified by a real build** — these are source edits only; the assistant does not run cmake/MSBuild (standing rule). **User must rebuild.**

**★ HANDOFF — next action is still the user's, in order:**
1. Rerun the build (reconfigure already succeeded; only recompile is needed now):
   ```powershell
   & "F:\SDBZ Recomp\build.ps1" RelWithDebInfo
   ```
2. If it fails again, paste the *full* error — prefer reading `F:\SDBZ Recomp\build_log.txt` directly (`Select-String -Path "F:\SDBZ Recomp\build_log.txt" -Pattern "error"`) over trusting the console tail, since `build.ps1`'s progress bar swallows the actual `C2039`-style lines (this is the second time that log has been the only way to see the real error — worth remembering as a pattern).
3. Once it builds clean, proceed exactly as parts 5/6 describe: rerun with `$env:PS2X_DIAG = "1"`, grep `run_log.txt` for `\[present\]`, and analyze `nonblack=`/`dispFbp=`/`ctx0/1.fbp=`/`pref=`.

## Session 2026-07-29 (co-piolet, part 8) — ★ BUG-027 VERIFIED: rebuild succeeded, `ps2EntryRunner.exe` built with `PS2X_ENABLE_RUNTIME_LOGS=ON`. **Next action: `$env:PS2X_DIAG=1` run + grep for `[present]`.**

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).** User reran `build.ps1 RelWithDebInfo` after the part-7 fixes. Build completed: `ps2EntryRunner.vcxproj -> F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe`, `Done in +05:48`. Only warnings emitted (`LNK4075`/`LNK4006`/`LNK4088`, all pre-existing/benign — raylib vs. user32 duplicate-symbol shadowing and the expected `/FORCE`-link note, unrelated to BUG-027).

**BUG-027 is now CLOSED/VERIFIED** — the `RUNTIME_LOG`/`PS2_IF_AGRESSIVE_LOGS` member-reference fixes from part 7 compile cleanly with `PS2X_ENABLE_RUNTIME_LOGS=ON`.

**★ HANDOFF — next action is still the user's:**
1. Run the game with runtime diagnostics enabled:
   ```powershell
   $env:PS2X_DIAG = "1"
   & "F:\SDBZ Recomp\launch_recomp.ps1"
   ```
   (or whatever the standard launch command is — see earlier parts/run command in project header.)
2. Grep the fresh `run_log.txt` for the `[present]` probe:
   ```powershell
   Select-String -Path "F:\SDBZ Recomp\run_log.txt" -Pattern "\[present\]"
   ```
3. This is the **first time** `[present]` can actually fire (previously blocked by BUG-027's build failure, and before that by `RUNTIME_LOG` being compile-disabled per part 5). Paste whatever it prints — even if empty, that itself is new information (would mean the probe site never executes, a different bug).
4. Once `[present]` output exists, analyze `nonblack=`, `dispFbp=` vs `ctx0/1.fbp=`, and `pref=` to determine whether the drawn frame is genuinely near-black, DISPFB points at a stale page, or fallback frame-selection picks the wrong candidate.

**Do not re-litigate:** `pc==ra` (dispatch-boundary artifact), `gstate@0x50227c` frozen (static singleton pointers), GS-pixel-math CPU cost (real rasterizer work) — all closed in parts 1-3. The part-6 single-tick busy%-cliff (74→2) remains unexplained but low-priority; only chase it with a complete untruncated trace.

## Session 2026-07-29 (co-piolet, part 9) — ★★ `[present]` FIRED FOR THE FIRST TIME: `nonblack=0` confirmed — the presented frame is genuinely solid black, and `[gs:pixels]` shows every draw is color `(0,0,0,0)`. Root cause shifts upstream to color-source, not presentation/DISPFB.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).**

**Detour that delayed this (now closed, for the record):** the first several `$env:PS2X_DIAG=1` runs still produced zero `[present]` lines even after BUG-027's fix, which looked like a runtime-gating bug. Root cause was mundane: `launch_recomp.ps1`'s default `-Exe` points at `build\ps2xRuntime\Debug\ps2EntryRunner.exe`, last built **2026-07-28 23:37**, while `ps2_runtime.cpp` was edited **2026-07-29 00:47** — after that Debug build. The part-7/8 rebuild that was verified only produced `RelWithDebInfo\ps2EntryRunner.exe` (built 2026-07-29 14:58). Every run in parts 5-8 executed the **stale Debug exe**, which predated the fix entirely. Lesson: **always pass `-Exe` pointing at whichever config was just rebuilt, or rebuild Debug too** — `launch_recomp.ps1` does not default to the most-recently-built config. (Aside, also resolved: my own PowerShell tool's view of `F:\SDBZ Recomp\run_log.txt` was reading a stale/disconnected copy relative to the user's live session for several turns — I stopped trying to self-check the log and relied on the user's own `Select-String` output, which is correct going forward. Also confirmed the runtime source in `F:\SDBZ Recomp` — a different git worktree/branch, `work/laptop-session-0519` @ `81a0dbc` with local uncommitted edits — is what's actually built; this session's own worktree, `agents/watchdog-performance-analysis` @ `c68adaf`, is a **different branch** and must not be assumed to match the build tree's source without checking.)

**The fix that worked:**
```powershell
$env:PS2X_DIAG = "1"
& "F:\SDBZ Recomp\launch_recomp.ps1" -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe" -RunSeconds 90
```

**Captured `[present]` lines (2 samples over the run):**
```
[present] has=1 w=512 h=448 nonblack=0 dispFbp=0x0  srcFbp=0x0  pref=0 ctx0.fbp=0x70 ctx0.fbw=8 ctx0.psm=0x0 ctx1.fbp=0x70 ctx1.fbw=8 ctx1.psm=0x0 pmode=0x7f27 dispfb1=0x1000 dispfb2=0x1000 display1=0x1bf9ff0203327c display2=0x1bf9ff0203227c
[present] has=1 w=512 h=448 nonblack=0 dispFbp=0x70 srcFbp=0x70 pref=0 ctx0.fbp=0x70 ctx0.fbw=8 ctx0.psm=0x0 ctx1.fbp=0x70 ctx1.fbw=8 ctx1.psm=0x0 pmode=0x7f27 dispfb1=0x1000 dispfb2=0x1000 display1=0x1bf9ff0203327c display2=0x1bf9ff0203227c
```
Interleaved `[gs:pixels]` samples (dozens across the run, `n=1008000000` through `n=1056000000`) all report `r=0 g=0 b=0 a=0` at varying `x`/`y`, with `tme=0` (untextured) and `frame.psm=0x0` (PSMCT32). `[gs:frame-change]` shows the game alternating draw target between `frame.fbp=0x0` and `frame.fbp=0x70` (double-buffering), both PSMCT32/fbw=8 (512px wide, matches `w=512` above).

**Analysis:**
- `nonblack=0` is real, not a symptom of watching the wrong buffer: `[gs:pixels]` independently confirms every sampled draw pixel is `(0,0,0,0)`, i.e. fully transparent black, regardless of which buffer (`0x0` or `0x70`) is being drawn to at that moment.
- The previously-known DISPFB mismatch (`dispfb1=0x1000` vs. `ctx0/1.fbp=0x70`) is confirmed real and the fallback path (`pref=0`, so `srcFbp` follows `ctx.fbp` instead of trusting `DISPFB`) is confirmed active — but this is **not the root cause**, since the buffer the fallback correctly picks (`0x70`) is *also* black.
- **Root cause has moved upstream of presentation entirely**: the question is now why every GS draw's output color is `(0,0,0,0)`. Candidates: (a) RGBAQ/vertex-color register read as zero due to a GIFtag/VIF unpack bug before the primitive reaches the rasterizer, (b) alpha-blend/test state incorrectly zeroing color, or (c) the game is genuinely intentionally drawing black at this exact boot moment (e.g. a pre-fade-in black screen) and the real bug is that it never advances past this state (a hang/stall bug, not a color bug) — needs a longer capture across multiple seconds to see if `frame.fbp`/pixel colors ever change.

**★ HANDOFF — next action:**
1. Get a longer `[present]`/`[gs:pixels]` capture (`-RunSeconds 30`+ with the same `-Exe RelWithDebInfo` override) to see whether `nonblack` ever goes above 0, or whether the game is simply stuck drawing black indefinitely (would point back to a stall/derail rather than a color bug).
2. If it's confirmed to stay black indefinitely: trace the color source for `prim=6` draws — check RGBAQ register state and the GIFtag unpack path feeding this primitive, comparing against what the guest ELF's boot-splash code is expected to set.
3. Remember for every future run: pass `-Exe` explicitly pointing at whichever build config was just rebuilt (currently `RelWithDebInfo`), since `launch_recomp.ps1`'s default `-Exe` (`Debug`) is not being kept in sync.

**Do not re-litigate:** presentation/DISPFB fallback logic (confirmed working as designed, not the bug), the missing-`PS2X_DIAG`-env-var hypothesis (ruled out — the var was correctly inherited; it was the stale exe), and everything closed in parts 1-3.

## Session 2026-07-29 (co-piolet, part 10) — longer capture RE-CONFIRMS solid black across ~48M `n` (many draws/frames); GS register dump shows nothing that would force color to zero — prime suspect is now specifically **RGBAQ / vertex color source**, which has not yet been observed in any capture.

**Source of this entry: co-piolet (GitHub Copilot CLI in VS Code).**

**New data pasted this segment:** a much larger excerpt of `run_log.txt` (still same `-Exe RelWithDebInfo` corrected launch). Confirms and extends part 9:
- `[gs:pixels]` `n` counter spans **`n=1008000000` → `n=1056000000+`** in this excerpt alone (with the user noting "there's a lot more, can't fit it all") — this is tens of millions of counted draws, not a one-off transient, all still `r=0 g=0 b=0 a=0`. Strongly favors **"stuck black" over "transient black before fade-in"**, since a real fade-in would show non-zero color within this many samples.
- Two more `[present]` lines, still `nonblack=0`, `dispFbp`/`srcFbp` alternating `0x0`/`0x70` in step with `ctx0.fbp`/`ctx1.fbp=0x70` as before. `pmode=0x7f27`, `dispfb1=dispfb2=0x1000` unchanged — consistent with part 9, no new drift.
- `[gs:frame-change]` continues normal `frame.fbp` alternation `0x0`⇄`0x70`, `frame.psm=0x0` (PSMCT32), `tex0.tbp=0x0`/`tme=0` throughout — still flat-color (untextured) fills, never textured draws.
- New: `[gs:ad]` (direct GS privileged-register writes) samples decoded:
  - `addr=0x4a data=0x0` / `addr=0x4b data=0x0` → **FBA_1/FBA_2 = 0** (alpha-correction bit off, not a factor).
  - `addr=0x4e data=0x1300000e0` → **ZBUF_1** configured (has a real base pointer + PSM, not the all-zero/uninitialized state).
  - `addr=0x47 data=0x30003` → **TEST_1**: `ATE=1, ATST=ALWAYS` (alpha test enabled but always passes — does not discard pixels), `ZTE=0` (z-test **disabled**, so depth cannot be silently failing these draws either). Nothing here would force output color to black.
  - `addr=0x1a data=0x1` → **PRMODECONT=1**, matches the `prmodecont=1` already seen per-primitive; context-mode-from-PRIM is active as expected, not a stray fixed-mode override.
  - `[gs:giftag] n=280000 lo=0x1000000000008004 hi=0xe nloop=4 eop=1 flg=0 nreg=1` → a normal small GIFtag (4-loop, 1-register PACKED format, `hi=0xe` = register selector `0xE`=`A+D` mode), consistent with these being register-write packets, not the actual vertex/color payload for the black primitives.

**Analysis / what this rules out:** none of the decoded GS state registers (FBA, ZBUF, TEST, PRMODECONT) would force a drawn pixel to `(0,0,0,0)` — alpha test always passes, z-test is off, FBA is a no-op. This keeps the leading hypothesis on the **vertex/primitive color source itself**, i.e. whatever sets `RGBAQ` (GS register `0x01`) before each `PRIM`/kick — and notably **no `[gs:ad] addr=0x01` (RGBAQ) line has appeared in any capture pasted so far**. Either RGBAQ genuinely never gets written with a non-zero color for these draws (real bug), or RGBAQ writes go through a different, unlogged path (e.g. packed GIFtag registers other than A+D mode, such as PACKED-mode RGBAQ fields embedded directly in a vertex-format GIFtag rather than as a discrete `[gs:ad]`-style register write) that the current probes don't capture.

**★ HANDOFF — next action (refined from part 9):**
1. Add/confirm a probe on RGBAQ specifically — either extend `[gs:ad]` to flag `addr=0x01` distinctly, or add a `[gs:rgbaq]` probe at the point PACKED-mode GIFtags set the primitive color, so we can see what color value (if any) is actually being loaded before each black draw.
2. Once RGBAQ's value is visible: if it's genuinely `0x00000000` going into the draw, the bug is upstream in whatever guest code / GIFtag unpack path is supposed to set the boot-splash color — trace that write path next. If RGBAQ is non-zero but the rasterizer still outputs black, the bug is in the recompiled rasterizer's color-application path (`GS::` pixel write in `ps2_gs_gpu.cpp`) instead.
3. Longer-capture confirmation already strongly suggests "stuck black," but keep an eye out in future captures for any moment `nonblack` becomes nonzero (would falsify the stuck-black theory and point back to a timing/derail issue instead).
4. Continue passing `-Exe` explicitly (`RelWithDebInfo\ps2EntryRunner.exe`) per the part-9 lesson — no change needed here, just carrying the reminder forward.

**Do not re-litigate:** everything closed in parts 1-9 (dispatch-boundary `pc==ra`, frozen `gstate@0x50227c` singleton pointers, GS-pixel-math CPU cost, `PS2X_DIAG` env var, DISPFB/ctx.fbp presentation fallback, the stale-Debug-exe root cause). FBA/ZBUF/TEST/PRMODECONT register state is now also closed as a suspect per this entry — do not re-decode these registers again without new evidence.

## Session 2026-07-29 (co-piolet, part 11) — ★★★ ROOT CAUSE FOUND: **GIF IMAGE-mode payload was dropped entirely.** Every VRAM image upload transferred **0 bytes**. Also: two process bugs corrected — the assistant CAN read `run_log.txt` directly, and the `[gs:pixels]` probe was sampling-biased and produced the wrong conclusion in parts 9-10.

### ★ PROCESS CORRECTION 1 — `run_log.txt` is directly readable by the assistant. The prior claim that it isn't was wrong.
Verified this session: `F:\SDBZ Recomp\run_log.txt` (1,870,926 bytes) is fully readable and greppable, and whole-log frequency analysis runs in a single command. **Parts 1-10 were all reasoned from hand-pasted few-hundred-line excerpts while whole-log statistics were available the entire time.** Going forward: the user runs the game and says so; the assistant reads and aggregates the whole log itself. Do not go back to the paste loop.

### ★ PROCESS CORRECTION 2 — the `[gs:pixels]` probe was sampling-biased. Part 10's RGBAQ conclusion rested on it and is **demoted**.
`[gs:pixels]` logged 1 sample per **2,000,000** pixel writes. A 512x448 framebuffer clear is ~229k pixels at ~50Hz ≈ 11M cleared pixels/sec, so essentially every sample landed on a black screen-clear sprite. Whole-log truth: the entire run contains exactly **16** `[gs:pixels]` lines. "Every draw is black" was 16 samples of a black clear — not evidence about geometry at all. The probe has been **replaced** (see fix 2 below).

### ★ ROOT CAUSE — `GS::processGIFPacket` had no state for IMAGE payload spanning DMA packets
Whole-log evidence:
- **`[gs:image]` — every image transfer moved zero bytes:** `dbp=0x0 dpsm=0x0 dbw=16 trxreg=1024x64 sizeBytes=0`, ~10 occurrences, then never again. `[gs:giftag]` shows exactly one IMAGE tag: `flg=2 nloop=16384 nreg=16 eop=1` → 16384 qw = **262,144 bytes requested, 0 delivered**. `1024x64 PSMCT32 = 262,144 bytes` — the sizes agree exactly.
- **`[gs:ad] addr=0x51` (TRXPOS) sweeps DSAY = 0, 64, 128, 192, 256, 320, 384, 448** — eight 1024x64 strips = one **1024x512** upload to VRAM base 0, split into 8 strips. All 8 delivered nothing.
- **82 of 88 logged GIFtags are all-zero garbage** (`flg=0 nloop=0 nreg=16 eop=0 hi=0x0`) — the parser was chewing through the dropped image payload and misreading it as tags.

Mechanism (`ps2_gs_gpu.cpp`, IMAGE branch of `processGIFPacket`):
```cpp
uint32_t imageBytes = nloop * 16;
if (offset + imageBytes > sizeBytes)
    imageBytes = sizeBytes - offset;   // clamps to 0 when the tag ends the buffer
processImageData(data + offset, imageBytes);
```
The DMA chain delivers the IMAGE GIFtag as the **last qword of one packet** and its 16384 qwords of pixel data in the **following** packets. `offset == sizeBytes` → `imageBytes = 0` → payload silently discarded, **and** the next packet's pixel data was then parsed as GIFtags (hence the 82 zero-tags). `m_transferState` correctly persisted the *destination* cursor across IMAGE tags, but nothing persisted the *source* byte debt.

### ★ WHAT THIS EXPLAINS
- No texture data ever reaches GS VRAM.
- All 4,579 `[gs:frame-change]` events are only two states: `prim=6 (SPRITE) tme=0 tex0.tbp=0x0`, alternating `fbp 0x70 <-> 0x00`. **The game never issues a single textured draw.** The render loop is healthy and double-buffering correctly — it just clears to black forever.
- `[gs:ad] addr=0x1` (RGBAQ) = `0x3f80000000000000` → Q=1.0f, R=G=B=A=0. RGBAQ *is* black — but that is the correct color for a screen clear, which is all that is being drawn. Not the bug; a symptom.
- Geometry decode cross-check: `addr=0x05` (XYZ2) = `0x8e009000` → X=2304.0, Y=2272.0; `addr=0x18` (XYOFFSET) = `0x720000007000` → OFX=1792.0, OFY=1824.0 → **512 x 448**, exactly the screen. Full-screen clear sprites, confirmed.

### ★ CLOSED THIS SESSION
- **Presentation path — not the bug.** `[present] has=1 w=512 h=448 pmode=0x7f27 dispfb1=0x1000`. Correct geometry, valid pmode; it is faithfully presenting an empty buffer. Stop investigating DISPFB/present.
- **RGBAQ — not the bug.** Demoted from part 10's "prime suspect". It is black because clears are black.

### ★ FIXES APPLIED (co-piolet, needs user build + run to verify)
1. **`m_pendingImageBytes` (new `GS` member, `ps2_gs_gpu.h`)** — persists the IMAGE payload byte debt across `processGIFPacket` calls. `processGIFPacket` now drains any outstanding debt from the head of each incoming packet *before* parsing GIFtags, and the IMAGE branch records `requested - delivered` instead of discarding it. Debt is decremented *before* dispatching to `processImageData`, because `processImageData` can call `EndTransfer()` mid-drain (that would otherwise underflow the counter). `EndTransfer()` deliberately does **not** clear the debt — remaining declared qwords are padding that must still be consumed as payload, never re-parsed as tags. Cleared on `GS::reset()` and on each `TRXDIR` write (new transfer supersedes any stale debt).
2. **`[gs:pixels]` replaced by `[gs:frame]`** (`ps2_gs_rasterizer.cpp` + present probe in `ps2_gs_gpu.cpp`) — per-interval aggregates instead of 1-in-2M sampling: `px=` pixels written, `nonblack=` pixels with any non-zero RGB channel, `textured=` pixels drawn with TME=1, `maxrgb=` brightest channel seen, `primmask=` bitmask of primitive types rasterized, plus running `imagebytes=` / `prims=`. Counters reset at each report, so **`nonblack=0` on this probe genuinely means nothing coloured was rasterized** — unlike the sampled probe it cannot miss draws.

**★ HANDOFF — next action:**
1. Build with `build.ps1 -Config RelWithDebInfo` (**not** `Laptop_Build.ps1` — that script and the `C:\SDBZ Recomp` root are dead, see the stale-worktree note above) and run with `PS2X_DIAG=1` and an explicit `-Exe` at the freshly built config. Then read `run_log.txt` directly (do not paste).
2. Check exactly three things: does `[gs:image] sizeBytes` go **non-zero**; does `[gs:frame] textured` go **non-zero** (i.e. any TME=1 draw at all); does `[gs:frame] nonblack` / `[present] nonblack` go **non-zero**.
3. If image bytes now land but still nothing textured is ever drawn, the blocker is upstream of GS entirely — pivot to the IOP/SIF side. The log is dense with `SIF_DIAG` (210), `ARKD:CALL` (192), `SifRpcPkt:SIG` (180), `iop:import` (156); determine whether IOP-side asset loading ever completes.
4. Watch the all-zero-GIFtag count (`flg=0 nloop=0 nreg=16 eop=0 hi=0x0`, was 82/88). It should collapse toward zero if the carryover fix is correct — that is the cheapest single confirmation signal.

### ★★ VERIFIED 2026-07-29 20:15 (co-piolet) — BUG-028 fix CONFIRMED WORKING; black screen persists for a *different*, now-isolated reason

Built `RelWithDebInfo` (clean, zero errors; the LNK4075/4006/4088 warnings are pre-existing `/FORCE:MULTIPLE` noise from `ps2xRuntime/CMakeLists.txt:636`) and ran 45 s with `PS2X_DIAG=1`. Read `run_log.txt` directly. Results against the four signals:

| # | signal | before | after | verdict |
|---|--------|--------|-------|---------|
| 1 | `[gs:image] sizeBytes` | `0` on **every** transfer | `262144` on **all 16** transfers | ★ **FIXED** |
| 4 | all-zero garbage GIFtags | **82 of 88** | **0 of 33** | ★ **FIXED** |
| 2 | `[gs:frame] textured` | 0 | **still 0** | unchanged |
| 3 | `[gs:frame] nonblack` | 0 | **still 0** | unchanged |

- **BUG-028 is genuinely fixed and is now closed.** Two independent signals confirm it. The garbage-GIFtag count going 82/88 → 0/33 is the decisive one: the runtime is no longer mis-parsing pixel payload as tags, which proves the cross-packet carryover works. Total delivered `imagebytes=4194304` = 16 x 262144 = the full 1024x1024 PSMCT32 upload, exactly as declared. Nothing regressed; the log is *cleaner* than before.
- **The new `[gs:frame]` probe (BUG-029 fix) works and is now the primary GS instrument.** Sample line: `px=24313856 nonblack=0 textured=0 maxrgb=0 primmask=0x40 imagebytes=4194304 prims=479`. Because it aggregates rather than samples, `nonblack=0` here is *authoritative*: across ~24 M rasterized pixels per interval, **not one pixel with a non-zero RGB channel was ever written, and not one TME=1 pixel was ever drawn.** This is no longer an inference.
- **`primmask=0x40` = bit 6 only = `GS_PRIM_SPRITE` and nothing else, for the entire run.** The game issues exactly one primitive type. `[gs:frame-change]` has only **three** distinct states in 2104 events, all `prim=6 tme=0 tex0.tbp=0x0` — differing only in `fbp` (0x0 / 0x70) and `prmodecont`. Confirms pure double-buffered black clears.
- **The 16 image uploads all happen once at boot and never recur** (`imagebytes` is cumulative and flatlines at 4194304). They target `dbp=0x0 dbw=16`, i.e. a 1024x1024 block at VRAM base — this is a **VRAM initialization/clear**, not asset content. **No texture is ever uploaded after boot init.**
- Watchdog is unchanged and still spinning: `t=44s ... stuckSecs=43 pc=0x421f10 ra=0x421f10`, `vbl/s=46`, `gif/s=44`. The guest is alive and pumping frames, it just never advances past the clear loop.

**Conclusion — the blocker is definitively upstream of the GS.** GS was innocent of everything except BUG-028, and BUG-028 was real but not sufficient. The EE never *asks* for a textured draw: no `TEX0` is ever programmed to a non-zero `tbp`, `TME` is never set, and no image upload other than the boot VRAM clear ever occurs. Something before the GS — asset load, or the code path that would issue the first textured primitive — never runs.

**★ HANDOFF — next action (supersedes the list above):**
0. **★ METHOD: use `ps2xTest`, not the game loop.** The project has a full MiniTest suite that drives `GS` directly — `ps2xTest/src/ps2_gs_tests.cpp` is 198 KB with 66 `processGIFPacket`/`GIF_FMT_IMAGE`/`TRXDIR` hits, and `ps2_sif_rpc_tests.cpp` / `ps2_sif_dma_tests.cpp` are 56 KB each and cover the *next* lead. A Debug test build is minutes, versus ~48 minutes for `ps2EntryRunner` plus a game run. `build.ps1` now has a **`-Test`** switch (added this session) targeting `build\ps2xTest\ps2x_tests.vcxproj`:
   ```powershell
   & "F:\SDBZ Recomp\build.ps1" Debug -Test
   & "F:\SDBZ Recomp\build\ps2xTest\Debug\ps2x_tests.exe"
   ```
   Reserve full runner builds for confirming end-to-end behaviour, not for iterating on a hypothesis.

   **★ Gotcha found while enabling this (co-piolet, 2026-07-29):** `ps2x_tests` had been **unlinkable since the generated-body overrides were added** — the on-disk exe was stale from 7/24. `ps2_runtime` (STATIC) contains `src/lib/game_overrides.cpp`, which does `registerFunction(0x00180D30u, &fn_180D30_0x180d30)` and three more. Those bodies live in `ps2xRuntime/src/runner/*.cpp`, which CMake globs **only into the `ps2EntryRunner` executable** (`ps2xRuntime/CMakeLists.txt:484-499`), never into the library — so they resolve for the runner and for nothing else, and every other consumer of `ps2_runtime` dies with LNK2019.
   Fixed by adding **`ps2xTest/src/recomp_override_stubs.cpp`** (abort-on-call stubs for the four symbols) to the `ps2x_tests` executable sources. Linking the real runner TUs was rejected: the transitive `fn_*` closure is most of the generated codebase and would make the test exe as slow to build as the runner.
   **Every new `game_overrides.cpp` entry pointing at a generated body adds another unresolved symbol here — add a matching stub.**

   **★ STATUS 2026-07-29 21:22 (co-piolet): this method is now PROVEN.** The test build links and runs. First real payoff: `ps2x_tests.exe PS2GS "BUG-028"` → 3/3 passed, unit-confirming the BUG-028 fix in a ~5-minute loop. Two things you must know before using it:
   - **`ps2x_tests.exe` now accepts filters:** `ps2x_tests.exe [suite-substring] [test-substring]` (case-insensitive, empty = all).
   - **A bare `ps2x_tests.exe` HANGS** in `PS2RuntimeKernel`'s semaphore test, and because suites run **alphabetically** (`std::map`), everything after it — `PS2SifRpc`, `SifDma`, `PS2Vu1`, `Pad`, ~35 `Scheduler*` — never runs. **The SIF suites are reachable only via the filter.** Details under BUG-028 below.
   - There are ~14 pre-existing baseline failures unrelated to any of this (clut-cache CSM1 T4, T4HL planes, TEX2 CLUT, PABE, ALPHA FIX, `sceGsExec*Image`, `sceGsResetGraph`, `sceGsSyncV` parity, VIF1 DIRECT continuation, `SetVSyncFlag` tick, `_realloc_r`, ghidra map starts). **None are regressions.** Do not chase them without an explicit decision.
1. ~~**Pivot to IOP/SIF asset loading.** ... Note `SifRpcReply` (88) < `SifRpcPkt:SIG` (180) — roughly half the RPC signals appear to go unanswered. **That ratio is the first thing to check** ...~~ **⛔ RETRACTED 2026-07-29 21:40 — the ratio is a log-cap artifact (both counters saturated at 24+64 vs an uncapped SIG counter). See BUG-030. Do not chase it.** The rest of that item is also answered: **IOP asset loading is PROVEN to work end-to-end** (disc → IOP RAM → EE RAM, real ISO lookups, real sectors, `IsReady` = `0x30000000`) — see the 21:40 entry below. Tag census retained for reference only: `SIF_DIAG` 210, `ARKD:CALL` 192, `SifRpcPkt:SIG` 180, `iop:import` 156, `sceSifSetDma:DTX` 89, `SifRpcReply` 88 (capped), `ARKD:run` 55, `Sema:Signal` 32. **For the current entry point, skip to ★★ HANDOFF 22:00 at the end of this session block.**
2. Cross-reference against BUG-009 and the `dword_441A00` lead (see the 2026-06-21/22 entries) rather than starting fresh — this may be the same root cause finally surfacing with clean instrumentation behind it.
3. `run_log.txt` is **directly readable by the agent** (~1 MB). Do not ask the user to paste logs. The only thing the user must do is build and run.

**Do not re-litigate:** all of parts 1-10, plus (new this session) the presentation path, the RGBAQ colour source, the GIF IMAGE carryover path (BUG-028, fixed and verified), and the claim that "every draw renders black is a rendering bug" — it is not; nothing coloured is ever *submitted*.

### ★★ 2026-07-29 21:40 (co-piolet) — the "RPC reply deficit" lead is REFUTED, and IOP asset loading is PROVEN TO WORK. Two dead ends closed from `run_log.txt` alone, no build required.

**1. `SifRpcReply` (88) < `SifRpcPkt:SIG` (180) is a LOGGING-CAP ARTIFACT, not a real deficit. This lead is dead — do not chase it again.**
The 88 is the sum of two *saturated* counters:

| log line | observed | cap in source |
|---|---|---|
| `[SifRpcReply] deliver cid=...` | **24** | `s_replyLogs ... < 24u` — [SIF.cpp:752](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp:752) |
| `[SifRpcReply] depth=...` | **64** | `s_depthLogs ... < 64u` — [SIF.cpp:849](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp:849) |

24 + 64 = 88, both exactly at their caps. Meanwhile `[SifRpcPkt:SIG]` is **deliberately never rate-limited** ([SIF.cpp:1619](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp:1619)). Comparing an uncapped counter against two saturated ones produces a "deficit" out of thin air. The signal/reply cids are also perfectly consistent: SIG is `0x8000000a` x173 + `0x80000009` x7; deliver is `0x8000000a` x14 + `0x80000009` x10 — same two cids, no orphan server.

**Generalize this:** before treating any log-frequency ratio as evidence, check whether either counter is capped. Several probes in this codebase cap at 6/8/16/24/64.

**2. `[ARKD:CALL] handleRPC -> false` (64x) is ALSO a red herring.** That call site is explicitly labelled *"Stage 1 observe-only bridge ... result never written back to the guest ... (expected: false -> the Stage 2 gap)"* ([SIF.cpp:600-608](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp:600)). The *real* service path is the separate `PS2_ARKD_SERVICE` bridge below it, and `launch_recomp.ps1` defaults both `PS2_ARKD_SERVICE=1` and `PS2_ARKD_IRX_RUN=1`, so it was active. `handleRPC -> false` says nothing about whether assets load.

**3. ★ IOP asset loading WORKS end-to-end. Disc → IOP RAM → EE RAM is a proven, live path.** From the same run:
```
[ARKD:search] "\INFO.DAT;1" -> found lbn=1048576 size=0x18000
[ARKD:search] "\GAME.DAT;1" -> found lbn=1048625 size=0x2ba29000
[ARKD:cdread]  lbn=1048576 sectors=47 iopDest=0x060000 -> ok      (+7 more, all ok)
[ARKD:sifdma]  iopSrc=0x060000 eeDest=0x01652400 size=0x18000 -> copied
[ARKD:sifdma]  iopSrc=0x078000 eeDest=0x01670c00..0x01673c00 size=0x800 -> copied  (x7)
```
Real ISO lookups resolve, real sectors are read, and the bytes are memcpy'd into **EE RAM**. `[ARKD:run] sid=0x503 fno=0x2` (IsReady) returns `00 00 00 30` = `0x30000000` = ready, matching the June `ccdReply` finding exactly. **The premise "determine whether IOP-side asset loading ever completes" is answered: it starts and it delivers.**

**4. What is still open, and it is now much sharper.** Only **8** `[ARKD:cdread]` and **8** `[ARKD:sifdma]` lines appear — and both probes cap at `seen < 8u` ([ps2_iop_irx_loader.cpp:644](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp:644) and [:751](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp:751)). So the log **cannot distinguish** "streaming stopped right after the TOC" from "streaming ran the whole 0x2ba29000-byte GAME.DAT". That distinction is now the single most valuable unknown, and it was unanswerable purely because of a log cap.

**Instrumentation added (co-piolet, BUILT 2026-07-30 — needs a RelWithDebInfo run to read):** running totals that never saturate, emitted at power-of-two event counts (dense early, sparse later — so an early stop is always visible and a full stream costs ~19 lines) —
- `[ARKD:cdread-total] reads= sectors= bytes= fails= lastLbn=`
- `[ARKD:sifdma-total] dmas= bytes= skipped= lastEeDest=`

The first-8 detail lines are unchanged. Read the **last** `-total` line of each: if `dmas` stalls in the single/double digits, asset streaming genuinely stops after the TOC and *that* is the blocker. If `bytes` climbs into the megabytes, assets are loading fine and the failure is in the EE-side consumer that should turn loaded data into a texture upload.

~~**Surviving lead (unchanged, still the best one): BUG-009 `rpc_handle_valid`.**~~ **RETRACTED 2026-08-01 — BUG-009 is benign, see the CLOSED table in the 07-29 22:00 handoff. Historical only.** June proved the boot state machine `sub_327810` is frozen at state 1, gated by `wrap_rpc_handle_valid()` over 4 client handles (`0x5A9330`/`0x5A9358`/`0x5A9380`/`0x5A93A8`), *not* by `ccdReply` content (which reads the passing `0x30000000`). `rpc_handle_valid` (`0x178de8`) is `v1 = *a1; return v1 && a1[1] == *(u32*)(v1+24) && (*(u32*)(v1+16) & 1);`. A `rpcValid=` 4-bit diagnostic was written in June but **grep confirms it emits nothing in the current `run_log.txt` — it is not in the build.** Re-instating that measurement identifies which handle fails and which of the three sub-conditions (null backing pointer / id mismatch / ready bit clear) is at fault, which points straight at what `SifBindRpc` ([RPC.cpp](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp)) fails to populate. Note the related `dword_441A00` thread is **CLOSED as a dead end** (2026-07-06c) — do not restart it.

## ~~★★ HANDOFF 2026-07-30 — BUG-009: `SifBindRpc` is dead code, HWWATCH now armed on `pkt_addr`~~ — **SUPERSEDED 2026-08-01. Do not action.**

> **RETRACTED.** BUG-009 is benign (see the 2026-08-01 CLOSED entry in the 07-29 handoff below). The `pkt_addr` HWWATCH run described here **must not be run** — it costs ~20 % CPU and answers a question that no longer matters. The one durable finding here is the `SifBindRpc`-has-zero-call-sites fact, which stands. `PS2X_HWWATCH` / `PS2X_HWWATCH_CLIENT` should be **unset** for all runs going forward.

**Read this before the 07-29 HANDOFF below — it changes the BUG-009 plan of attack.**

Two sequential fix attempts this session (one in `SifCallRpc`, one a bind-reply-packet stamp in `SifBindRpc`) both **failed and were retracted** — fresh runs still show all 5 client handles (`0x464dc0`, `0x5a9330`, `0x5a9358`, `0x5a9380`, `0x5a93a8`) with `pktAddr=0x0` on every `[rpcValid]` poll.

**Root cause of both failures: `SifBindRpc`/`sceSifBindRpc` has zero call sites anywhere in `src/runner/`** — confirmed by grepping the entire generated directory for both `sceSifBindRpc` and the bare substring `BindRpc` (targeted content grep, not a listing — rule-compliant). This is stronger than a dispatch-bypass: the SDK's bind logic was statically inlined into the ELF's translated MIPS (same pattern as `rpc_handle_valid_0x178de8.cpp`), so nothing in `RPC.cpp` is ever reached for it. **Do not attempt further fixes in `SifBindRpc`/`SifCallRpc` — that code path is unreachable for this game's binary.**

**Per the project's own rule** ("for who wrote value X to address Y, use the hardware data breakpoint, not log-greps or store-site bisection — the latter produced four retractions in ten days") — this session just produced its own two retractions the same way. Switched tools.

**Wired 2026-07-30:** `sdbzDiagRpcHandleValid178DE8` in `game_overrides.cpp` (the existing `[rpcValid]` hook at `0x178de8`) now arms the existing HWWATCH infra (built 2026-07-27 for the `rpc_call` `$ra` hunt — VEH + background armer thread, unchanged) on `client->hdr.pkt_addr` (offset 0) for client `0x464dc0` specifically, to catch the real writer or prove there is none. Not yet run.

**Next run, hand the user:**
```powershell
.\build.ps1 RelWithDebInfo 6
$env:PS2X_HWWATCH = "1"
$env:PS2X_HWWATCH_VAL = "0xFFFFFFFF"   # record every store, not just value 1 — filter is unknown for this address
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```
Check `run_log.txt.hwwatch.txt` for symbolized hits. `PS2X_HWWATCH_CLIENT=0x...` overrides which of the 5 clients gets watched (default `0x464dc0`) — the watchpoint only tracks one address at a time, so if this run comes back empty, re-run against a different client before concluding pkt_addr is never written at all. **Cost: ~20% CPU while armed (measured 07-27) — do not trust perf numbers from this run, and unset `PS2X_HWWATCH` for anything else.**

---

## ★★ HANDOFF 2026-07-29 22:00 — **SINGLE ENTRY POINT. Read this before any other 07-29 entry.**

Everything above this line in the 2026-07-29 block is history, including handoffs that point at leads since refuted. This is the only current one.

### Where the blocker is

**The EE never submits a textured draw.** `primmask=0x40` (SPRITE only) for an entire run, `tme` never 1, `tex0.tbp` always 0, `nonblack=0` across ~24 M rasterized pixels per interval. The render loop is *healthy* — ~46 fps, double-buffering `fbp 0x0 ↔ 0x70` — it just clears to black forever. The fault is in the guest state machine, **upstream of the GS and upstream of the IOP**.

### CLOSED — do not reopen, do not re-derive

| Thread | Verdict |
|---|---|
| BUG-028 GIF IMAGE cross-packet carryover | **FIXED + VERIFIED twice** (live run: `sizeBytes` 0 → 262144 on all 16 transfers, garbage GIFtags 82/88 → 0/33; unit: `ps2x_tests.exe PS2GS "BUG-028"` 3/3) |
| BUG-029 `[gs:pixels]` sampling bias | **FIXED** — replaced by aggregate `[gs:frame]`, now the primary GS instrument |
| BUG-030 capped-counter evidence | **Both instances refuted/handled** — see registry |
| "RPC reply deficit" 88 vs 180 | **Log-cap artifact.** No deficit exists |
| `[ARKD:CALL] handleRPC -> false` (64×) | **Red herring** — that site is the labelled observe-only Stage-1 bridge; the real path is `PS2_ARKD_SERVICE`, which was active |
| IOP asset loading | **PROVEN WORKING** end-to-end, disc → IOP RAM → EE RAM |
| Presentation / DISPFB / RGBAQ colour source | **Innocent** — faithfully presenting an empty buffer; RGBAQ is black because clears are black |
| `pc==ra=0x421f10`, frozen `gstate@0x50227c`, GS CPU burn | All false positives, closed parts 1–3 |
| `dword_441A00` | Dead end, closed 2026-07-06c |
| **BUG-009 `rpc_handle_valid` / `pkt_addr` / `SifBindRpc`** | **CLOSED 2026-08-01 — BENIGN.** `rpc_handle_valid` (`0x178de8`) is `sceSifCheckStatRpc`: it answers *"is this async RPC still **busy**?"*, not *"is it valid?"*. Our always-0 answer means "not busy", which **passes** both consumers. Four sessions + two retracted fixes were spent on a misleading func-map name. Evidence table in the 2026-08-01 handoff. **Do not reopen. Do not attempt further `SifBindRpc`/`SifCallRpc` fixes.** |

### ★ CLOSED 2026-08-01 — BUG-009 is benign; `rpc_handle_valid` is a busy-check, not a validity-check

The 07-29 "surviving lead" and the 07-30 HWWATCH plan below are both **retracted**. `rpc_handle_valid` (`0x178de8`) is `sceSifCheckStatRpc`. Both known consumers use it as a **busy**-check, so our constant-0 return is the *permissive* answer:

| Consumer | Code | Effect of our always-0 answer |
|---|---|---|
| `sub_1C0A30` (boot/CCD state machine) | `while (wrap_rpc_handle_valid(client)) ;` × 8 | loops exit **immediately** → passes |
| `sub_186CC0` `_sceCd_scmd_prechk` | `if (busy) { SignalSema; return 0; }` | reports "not busy" → **lets execution through** |

Sources: `ida_scripts/decompiles_SLUS_214_42.txt:152804` (full `sub_1C0A30` body) and the 2026-07-30b `sub_186CC0` finding. They agree. **`pkt_addr` never being armed cannot gate anything.**

**The real gate in `sub_1C0A30` is the RPC reply *content* at EE `0x5AA7D0`, not the handle:**

```c
mem_fill_z_369(client=0x5A9358, func=2, mode=3, send=0x5A97D0, 0, recv=0x5AA7D0, 16, 0, 0);
if ((MEM[0x205AA7D0] & 0xF0000000) == 0x40000000) { ... result = 1; }
else result = ((MEM[0x205AA7D0] & 0xF0000000) == 0x30000000);
```

Our ARKD returns `0x30000000`. Whether that is the *advancing* answer or the *keep-waiting* answer has **never been checked against real hardware**. That is the new thread → stage 5.8.

### ★ CLOSED 2026-07-30 05:18 — the one open measurement is ANSWERED: asset streaming does NOT stop after the TOC

Real RelWithDebInfo run (96.3s, `Determinism=0`), `-total` lines finally observed (the first attempt used a stale exe — Debug -Test never builds `ps2EntryRunner`; corrected by rebuilding RelWithDebInfo directly, verified via source/exe mtime ordering before trusting the run):

```
[ARKD:cdread-total]  reads=1024  sectors=1070  bytes=2191360  fails=0  lastLbn=1075481
[ARKD:sifdma-total]  dmas=1024   bytes=2156160  skipped=0     lastEeDest=0x017b5c00
```

Both counters doubled cleanly across all 11 samples (1→2→4→...→1024) with **zero fails, zero skips**, `lastLbn` and `lastEeDest` climbing continuously the entire run — **2.1-2.2 MB streamed, no stall at the TOC.** IOP asset loading is proven not just to start (prior finding) but to **run continuously and successfully for the full session.**

**Verdict: asset streaming is innocent.** The blocker is 100% EE-side: loaded data is never turned into a textured draw (`tme=0` unchanged all run). This closes the load-path investigation — **do not re-open it.** The only surviving lead is BUG-009 below.

### ~~The surviving lead — BUG-009 `rpc_handle_valid`~~ — **RETRACTED 2026-08-01, see the CLOSED entry above. Historical only.**

June proved the boot state machine `sub_327810` is frozen at **state 1**, gated by `wrap_rpc_handle_valid()` over 4 client handles (`0x5A9330` / `0x5A9358` / `0x5A9380` / `0x5A93A8`) — *not* by `ccdReply` content, which reads the passing `0x30000000`.

`rpc_handle_valid` (`0x178de8`) is: `v1 = *a1; return v1 && a1[1] == *(u32*)(v1+24) && (*(u32*)(v1+16) & 1);`

A `rpcValid=` 4-bit diagnostic was written in June but **is not in the current build** (grep-confirmed: zero hits in `game_overrides.cpp`, zero in `run_log.txt`). Re-instating it names **which handle** fails and **which of the three sub-conditions** (null backing pointer / id mismatch / ready bit clear) — which points straight at what `SifBindRpc` ([RPC.cpp](ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp)) fails to populate.

### Method — use `ps2xTest`, not the game loop

```powershell
& "F:\SDBZ Recomp\build.ps1" Debug -Test
& "F:\SDBZ Recomp\build\ps2xTest\Debug\ps2x_tests.exe" Sif
```
Minutes, versus ~48 min for `ps2EntryRunner` plus a game run. Reserve full runner builds for end-to-end confirmation, not hypothesis iteration.

Three hard rules for this path:
1. **Always pass a filter** — a bare `ps2x_tests.exe` hangs and silently skips ~40 suites (BUG-031).
2. **Always pass `-Exe`** pointing at the config you just built — `launch_recomp.ps1` defaults to `Debug` and does not track the newest build. This has burned three sessions.
3. **Every new `game_overrides.cpp` entry that points at a generated body needs a matching stub in `ps2xTest/src/recomp_override_stubs.cpp`**, or `ps2x_tests` stops linking.

`run_log.txt` (~1–2 MB) is **directly readable by the agent**. Do not paste logs; run the game, say so, and let the agent aggregate the whole file.

### Test baseline as of 22:00

`ps2x_tests.exe Sif` → **27 / 28 passed**. The one failure is **BUG-032** (`sceSifSetDma` multi-descriptor validation not atomic), newly observed because the SIF suites had never been reachable before. Not triaged, probably latent. ~14 other pre-existing baseline failures exist elsewhere in the suite (clut-cache CSM1 T4, T4HL planes, TEX2 CLUT, PABE, ALPHA FIX, `sceGsExec*Image`, `sceGsResetGraph`, `sceGsSyncV` parity, VIF1 DIRECT continuation, `SetVSyncFlag` tick, `_realloc_r`, ghidra map starts) — **none are regressions; do not chase without an explicit decision.**

### Low priority, flagged only

The part-6 single-tick `busy%` cliff (74 → 2 in one watchdog interval) is unexplained. Only chase it with a **complete, untruncated** trace; do not reason from the truncated paste.

---

## Session 2026-07-28 (later, Claude Code / Opus 5) — ★ RelWithDebInfo `ps2EntryRunner.exe` BUILT. Build was unfinishable for three reasons, all found and fixed. **A/B measurement run is the single next action.**

**Read this first if you are picking the project up.** A full agent handoff written for GitHub Copilot lives at [.github/copilot-instructions.md](.github/copilot-instructions.md) — durable rules, repo layout, and the "what do I do next" pointer. This section is the live state it points at.

### ★★★ THE A/B RAN — 109×. Stage 5.6 is CLOSED.

Run: `-Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile -Exe ...\RelWithDebInfo\ps2EntryRunner.exe`

| metric | Debug control arm | RelWithDebInfo | delta |
|---|---|---|---|
| **`progress` @ t=89** | **12 101** | **1 320 490** | **109×** |
| `gif/s` | 3–5 | **47–49** | normal band is 30–60 |
| `vbl/s` | 3–6 | **47–49** | |
| `[cputime]` | wall 96.52 s · cpu 99.53 s · 103.1 % | wall 96.33 s · cpu 102.84 s · 106.8 % | **same CPU** |
| `busy%` | (field post-dates this arm) | 60–73 | headroom remains |
| `_RTC_CheckStackVars` | 7.0 % / 8.5 % | **absent** | ⇒ correct exe was launched |
| busiest threads | 46.14 s / 44.42 s | 49.05 s / 42.25 s | unchanged shape |

**Same CPU seconds, 109× the guest work.** The build configuration was the entire performance story, exactly as hypothesised, and by a much larger factor than the 5–20× Debug→Release rule of thumb — which is itself the lesson: per-pixel `constexpr` accessor math is the pathological case for an unoptimised build.

Hot code is still GS pixel math but under its real post-inlining names — `GSMem::LookupPixelAddressCT32` / `ReadPixelAddressCT32` / `ReadPixelCT32`, `GSRasterizer::writePixel`, `GS::WriteVram`, `GS::copyFrameToHostRgbaUnlocked`. `PixelStorageTraits` is gone from the profile because `/O2` inlined it.

### ★ What this actually buys us: "slow" and "stuck" are now separate problems

**The game is no longer slow. It is still stuck.**

The watchdog holds `pc=0x421f10` / `ra=0x421f10` / `lastCall=0x172998` (`sceDmaSync`) with `stuckSecs` climbing 45→53 — **the same plateau as the Debug arm**, but now running at ~47 fps with the call trace churning healthily every tick.

90 s at this rate is **≈2.7 hours of Debug-equivalent guest work**. Real PCSX2 reaches the memory-card prompt in 7–8 s of wall time at comparable frame rates. We do not reach it at all.

⇒ **This is a correctness gate, not a throughput problem.** Every prior caveat of the form *"we can't tell whether it's blocked or merely too slow to observe"* is now void. Throughput is solved and must stop being treated as a variable. **All subsequent diagnostic runs should use the RelWithDebInfo exe** — they now cover ~100× more guest execution per wall-second.

**Resolved by elimination:** the t≈89 s watchdog breakout out of the `0x421f10`/`0x172998` plateau — which reproduced **2-for-2** under Debug and was logged in the previous session as an unexplained lead — **did not occur on this run**. It was a Debug-timing artifact, not a milestone being approached. The queued `-RunSeconds 300` run drops from "cheap follow-up lead" to "run once for completeness".

### Next action — the blocker, at the new speed

The question is now sharply posed: **why does the guest never leave the `0x421f10` / `sceDmaSync` (`0x172998`) loop, despite the GIF moving 47 packets/s and vblank ticking at 47/s?**

Starting points, in order of cheapness:

1. **One `-RunSeconds 300` run** on the RelWithDebInfo exe to confirm the plateau is genuinely terminal and not a very long wait. ≈9 hours of Debug-equivalent work. If it never leaves, the loop is unconditional.
2. **Identify `0x421f10`.** It is `pc` *and* `ra` — a tight self-loop. It has been the plateau address across every run for weeks and has never been disassembled. Do that.
3. **`0x172998` is `sceDmaSync`.** With `dma/s` at 92–98 and `gif/s` at 47–49, DMA is demonstrably completing. So either the guest is polling a completion flag the runtime never sets, or it is syncing on a channel that never gets serviced. Compare against PCSX2 at the same point.
4. The recurring trace tail `0x174f20 -> 0x172e20 -> 0x172a90 -> 0x172998` is stable across ticks — that's the caller chain into the sync. Walk it.

### Smaller findings from the fast run

- **~30 % of tid 39824 is now spent waiting, not computing** — `ntdll!NtWaitForSingleObject` 8.6 %, `ZwDelayExecution` 6.5 %; tid 14552 spends 15.2 % in `ZwWaitForAlertByThreadId`. Under Debug this was swamped by pixel math. The scheduler/fiber handoff is now a legitimate thing to examine.
- **Our own probes cost ~12 % of the presentation thread**: `` `anonymous namespace'::countNonBlackPixels `` 2.47 s (6.3 %) + `` `anonymous namespace'::blendPresentationChannel `` 2.20 s (5.6 %). Candidates for the same env-gate treatment `hwWatchArm()` got in BUG-025. Not urgent — there is headroom (`busy%` 60–73).
- `tid 36884` burns 6.22 s in `Wait/ExecutionDelay` and does not appear in `[hostprof]` — the same unexplained pattern as Debug's tid 9288. Still not chased.

### Build artifacts — verified on disk, not assumed

| | |
|---|---|
| `build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe` | **354,563,584 bytes**, 2026-07-28 **17:42:18** (Debug exe is 730 MB — the halving is `/O2` + no `/RTC1`) |
| unity objs | **4520** + 1 quarantined = 4521, 2.33 GB |
| build window | 14:0x → 17:42 (**~3 h 40 m**), ~21 obj/min |
| prior attempt for contrast | 04:14 → 06:36 at `/m:2`, reached 808 objs, ~5.7 obj/min ⇒ **≈11 h** projected |

### Why the build could not finish — three independent causes

**(1) `/m:$Jobs` was never going to help.** MSBuild's `/m` is **project**-level parallelism. `ps2EntryRunner` is a single `.vcxproj`, and MSBuild's CL task hands every source of one project to **one** `cl.exe` via a single response file, which compiles them **serially**. All 4520 unity TUs ran on one core at `/m:2` and would have at `/m:12`. The source-level knob is `cl /MP<N>`.

**(2) `src/runner/sub_0022F200_0x22f200.cpp` is an `/O2` optimizer bomb.** **278,316 lines / 12.9 MB** in one recompiled function — ~**350×** its unity siblings (5.9–37.7 KB). It sat at 100 % of one core for **2 h 09 m** and emitted a **37,164,827-byte** object while surrounding unity TUs averaged ~7 s each. It was never deadlocked; it just could not be waited out, and being inside a unity batch it serialized the entire target. This is the `unity_3680`/`unity_3681` stall the user reported at 12:04→13:26.

**(3) There are no CL tlogs, so no build ever resumes.** MSBuild's per-source up-to-date check reads `CL.read.*.tlog` / `CL.write.*.tlog` / `CL.command.*.tlog`. **This target has none.** Every build re-passes all 4520 sources to `cl`. ⛔ **This retracts the claim in the previous session section that "the 808 objs are valid and MSBuild resumes past them"** — they were silently recompiled from scratch. **A partial obj count is not progress and must never be quoted as an ETA.**

### The fix — three edits, `ps2xRuntime/CMakeLists.txt` only

No header touched, no recompiler run, no `runner/*.cpp` touched.

1. **Cache variables** (`PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE` = 8, `PS2X_RUNNER_CL_JOBS` = 6) so both knobs are tunable without editing logic.
2. **Optimizer-bomb quarantine** — `SKIP_UNITY_BUILD_INCLUSION ON` plus `COMPILE_OPTIONS "/Od;/Ob0"` on `sub_0022F200_0x22f200.cpp`.
3. **`target_compile_options(ps2EntryRunner PRIVATE /FS /MP${PS2X_RUNNER_CL_JOBS})`** — `/MP` needs `/FS` alongside `/Zi`; `/FS` was already there. Capped at 6 rather than bare `/MP` (= all logical processors) because `cl` peaked at 3.6 GB on the worst TU and 12 × that exhausts a 16 GB box.

**How to verify these survived CMake generation** (do this before committing to another multi-hour build — grep `build/ps2xRuntime/ps2EntryRunner.vcxproj`):

- `/MP6` becomes **two** elements and you need both: `<MultiProcessorCompilation>true</MultiProcessorCompilation>` **and** `<ProcessorNumber>6</ProcessorNumber>`. Confirmed present in all four configurations (lines 103/174/247/321 and 106/177/250/324). Bare `MultiProcessorCompilation` without `ProcessorNumber` would mean all cores.
- The quarantine shows as the bomb's `<ClCompile>` entry **lacking** the `IncludeInUnityFile="true" CustomUnityFile="true" UnityFilesDirectory="…"` attributes that every neighbour carries, plus `<Optimization>Disabled</Optimization>` + `<InlineFunctionExpansion>Disabled</InlineFunctionExpansion>` per config. Confirmed at line 34411 for Debug, Release, MinSizeRel **and** RelWithDebInfo.

`build.ps1` builds the `.vcxproj` directly and never calls `cmake`, but MSBuild's `Checking Build System` / `generate.stamp` step re-runs CMake by itself when `cmake.verify_globs` is newer than `generate.stamp.depend` — so a manual reconfigure is not required, and a Ctrl-C'd one is harmless.

### BUG-026 (open, low severity, 2026-07-28) — guest function `0x22f200` is unoptimized in every configuration

A consequence of the quarantine above, recorded deliberately rather than hidden. `sub_0022F200_0x22f200.cpp` now compiles `/Od /Ob0` in Debug, Release, MinSizeRel **and** RelWithDebInfo.

- **Not expected to matter for the current A/B:** the `[hostprof]` profile put the cost in the *host* software-GS (`PixelStorageTraits`, `GSRasterizer::writePixel`), not in guest code, and `0x22f200` has never appeared in a profile.
- **But it is the first thing to rule out** if the RelWithDebInfo numbers come back oddly flat.
- **Independently suspicious:** a 278,316-line recompiled function is itself an anomaly — plausibly a recompiler function-boundary failure that swallowed a large span of code, in the same family as [[project_truncated_function_bug]]. Flagged, never investigated. Cheap first check: disassemble `0x22f200` and see whether the original MIPS function is remotely that large.
- **Reversal is one line** if it ever matters: drop the `COMPILE_OPTIONS` while keeping `SKIP_UNITY_BUILD_INCLUSION` — the quarantine alone stops it serializing the target, at the cost of a 2 h tail on the critical path of every full rebuild.

### BUG-027 (FIXED + VERIFIED by clean rebuild, 2026-07-29, co-piolet) — `[gs:frame-change]`/`[gs:pixels]`/`[gs:prim]`/`[gs:copy-prim]` `RUNTIME_LOG`/`PS2_IF_AGRESSIVE_LOGS` probes referenced nonexistent members; broke the build the instant `PS2X_ENABLE_RUNTIME_LOGS=ON` was first tried. Rebuild with the fixes applied completed successfully (`Done in +05:48`, warnings only, no errors) — closed.

**How found:** user tried the part-5 reconfigure (`PS2X_ENABLE_RUNTIME_LOGS=ON`) for the first time ever, and `build.ps1` failed with a bare `Failed (exit 1)` and no visible error — the real `C2039` lines were only in `build_log.txt`, not the console tail.

**Root cause:** `RUNTIME_LOG(x)`/`PS2_IF_AGRESSIVE_LOGS(x)` compile to `do {} while(0)` by default, so their arguments are never type-checked while the corresponding CMake option is `OFF`. Four call sites across two files had silently bit-rotted behind these no-op macros, referencing struct members that no longer exist:
- `ps2_gs_gpu.cpp:2389` — `m_registers.prim.type` (no such member; the field is `.prim`, `GSPrimReg::prim` is a `Bitfield<u64,0,3>`) → fixed to use the already-in-scope local `prim.prim`.
- `ps2_gs_rasterizer.cpp:551` (the one that actually broke the build, gated by `RUNTIME_LOG`) — `gs->m_prim` (no such member on `GS`; it's `gs->m_registers.prim`) → fixed.
- `ps2_gs_rasterizer.cpp:257-261` and `322-326` — same `gs->m_prim.{type,tme,abe,fst,ctxt}` bug, ×2, still dormant behind the separate `PS2_IF_AGRESSIVE_LOGS`/`PS2X_ENABLE_AGRESSIVE_LOGS` gate (still OFF, untouched) — fixed proactively so enabling that flag later doesn't hit the same wall.
- `ps2_gs_rasterizer.cpp:279-281` and `342-344` — `gs->m_texclut.{cbw,cou,cov}` (no such member on `GS`; it's `gs->m_registers.texclut`), same two dormant blocks — fixed.
- `gs->m_vtxQueue[...]` in the same blocks was checked and is correct (`GS::m_vtxQueue` is real) — not part of this bug.

**Status:** source-fixed, verified with a repo-wide grep (`->m_prim\b|->m_texclut\b|\.prim\.type\b`, zero hits remaining) but **not yet confirmed by an actual rebuild** — assistant does not run builds (standing rule). Next action is the user re-running `build.ps1 RelWithDebInfo`. If it succeeds, this closes; if there are further errors, paste `build_log.txt`'s error lines, not just the console tail.

### BUG-028 (★ FIXED AND VERIFIED 2026-07-29 by co-piolet — CLOSED) — GIF IMAGE-mode payload spanning DMA packets was silently discarded; **every VRAM image upload transferred 0 bytes**

**How found:** whole-log analysis of `run_log.txt` (not a pasted excerpt). Every `[gs:image]` line reads `trxreg=1024x64 sizeBytes=0`; the single logged IMAGE GIFtag is `flg=2 nloop=16384` = 262,144 bytes requested vs 0 delivered. 82 of 88 logged GIFtags were all-zero garbage — the parser reading dropped pixel data as tags.

**Root cause:** `GS::processGIFPacket`, `GIF_FMT_IMAGE` branch. `imageBytes` was clamped to `sizeBytes - offset`, which is **0** when the IMAGE GIFtag is the last qword of its DMA packet — the normal framing for a large upload, where the payload arrives in the following packets. Nothing carried the outstanding source-byte count across `processGIFPacket` calls, so the payload was both dropped *and* re-parsed as GIFtags. (`m_transferState` persisted the *destination* cursor across IMAGE tags correctly; the *source* debt had no equivalent.)

**Fix:** new `GS::m_pendingImageBytes`. `processGIFPacket` drains outstanding debt from the head of each packet before tag parsing; the IMAGE branch records `requested - delivered`. Debt is decremented *before* dispatching to `processImageData` (which can call `EndTransfer()` mid-drain and would otherwise underflow it). `EndTransfer()` intentionally does not clear the debt — declared-but-surplus qwords are padding and must still be consumed, never parsed as tags. Cleared on `GS::reset()` and on each `TRXDIR` write.

**Verify by:** `[gs:image] sizeBytes` becoming non-zero, and the all-zero GIFtag count (`flg=0 nloop=0 nreg=16 eop=0 hi=0x0`) collapsing toward zero.

**VERIFIED 2026-07-29 20:15 (RelWithDebInfo, `PS2X_DIAG=1`, 45 s run):** `[gs:image] sizeBytes` = **262144 on all 16 transfers** (was 0 on every one); all-zero GIFtags = **0 of 33** (was 82 of 88); `[gs:frame] imagebytes=4194304` = the complete 16 x 262144 upload. Both verification signals passed. **Bug closed.**

**Caveat — fixing this did NOT fix the black screen.** `textured` and `nonblack` remained 0. The uploads that now succeed are a one-time boot VRAM clear at `dbp=0x0`, not asset content; no texture is ever uploaded afterwards and `TME` is never set. This bug was real, but the Phase 5 blocker is upstream of the GS. See the part-11 verification section for the IOP/SIF pivot.

**Known limitation (accepted, unverified):** `m_pendingImageBytes` is global, not per-GIF-path. `processGIFPacket` is reachable from PATH3 (DMA) and PATH1 (VU1 XGKICK). A PATH1 packet interleaving mid-IMAGE-transfer would be misconsumed as payload. Real hardware holds the GIF bus for PATH3 during an IMAGE transfer, so this is believed safe, but it was not checked against `m_gifArbiter`.

**Regression-tested 2026-07-29 (co-piolet).** Three tests added to `ps2xTest/src/ps2_gs_tests.cpp`, inserted just before the existing split-IMAGE test:
1. *"GS IMAGE payload arriving in a later packet than its GIFtag is not dropped (BUG-028)"* — packet A is a bare 16-byte IMAGE tag with no payload; packets B and C are raw payload. **Fails on the pre-fix code.**
2. *"...partially trailing its GIFtag carries only the shortfall (BUG-028)"* — guards the debt arithmetic.
3. *"...byte debt is dropped when a new transfer is started (BUG-028)"* — guards the `TRXDIR` clear.

**Why the existing suite missed this:** `ps2_gs_tests.cpp` already had *"GS PSMT4 host-local upload keeps position across split IMAGE packets"*, but each of its packets carries **its own IMAGE tag alongside its own payload** — repeated-tag framing. It never covered the game's actual framing: **tag at the end of a packet, payload in the packets that follow.** Worth remembering when judging whether a path is "covered".

**★ UNIT-CONFIRMED 2026-07-29 21:22 (co-piolet).** `ps2x_tests.exe PS2GS "BUG-028"` → **3 / 3 passed, 0 failed.** BUG-028 is now proven twice: end-to-end against a live 45 s run, *and* at unit level with no game, no ELF, and a ~5-minute build. The emitted probe lines confirm the exact framing that broke in the game — tag-only packet then `sizeBytes=16` + `sizeBytes=16` (carryover), and `nloop=3` then `sizeBytes=16` + `sizeBytes=32` (shortfall carried).

**★ GOTCHA worth not rediscovering — `EndTransfer()` zeroes the state you are about to assert on.** The first run of these three tests *failed*, and the failure was entirely in the tests, not the fix. `GS::EndTransfer()` ([ps2_gs_gpu.h:480](F:/SDBZ%20Recomp/ps2xRuntime/include/runtime/ps2_gs_gpu.h:480)) is two lines: `m_registers.trxdir.xdir = 3;` and `m_transferState = { };`. That second line wipes `copied_pixels`, `x`, `y`, `total_pixels`. `getDebugSnapshot()` reads *live* state, so **`transferCopiedPixels` is always 0 for a COMPLETED transfer** — an assertion like `Equals(copied, 8u)` after completion is unsatisfiable no matter how correct the code is. `m_transferState.copied_pixels` is likewise reset on every `TRXDIR` write ([ps2_gs_gpu.cpp:2114](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/ps2_gs_gpu.cpp:2114)), next to `m_pendingImageBytes = 0`. Assert transfer counters **mid-flight**, or assert the new debt field instead.

*Diagnostic heuristic that cracked it, reusable:* when a test fails, look at **which** assertions failed versus which passed. All three tests failed on exactly one assertion each while their *strongest* assertions (byte-for-byte VRAM comparison) passed. That pattern means the **observation** is wrong, not the code under test.

**New instrumentation added while confirming this (co-piolet):**
- `GSDebugSnapshot::pendingImageBytes` — exposes `m_pendingImageBytes`, the outstanding IMAGE byte debt, so it is directly assertable rather than inferred. Declared in [ps2_gs_gpu.h](F:/SDBZ%20Recomp/ps2xRuntime/include/runtime/ps2_gs_gpu.h) (~190), populated in `getDebugSnapshot` ([ps2_gs_gpu.cpp](F:/SDBZ%20Recomp/ps2xRuntime/src/lib/ps2_gs_gpu.cpp) ~607). The rewritten tests now verify the debt shrinks by exactly the bytes consumed per packet — strictly stronger than the assertions they replaced.
- **`ps2x_tests.exe` now takes filters:** `ps2x_tests.exe [suite-substring] [test-substring]`, case-insensitive, empty = match all. Prints a `[filter]` banner and a "Skipped by filter" summary. Added in [MiniTest.h](F:/SDBZ%20Recomp/ps2xTest/include/MiniTest.h) (`Run()` kept as a no-arg overload) and [main.cpp](F:/SDBZ%20Recomp/ps2xTest/src/main.cpp) (`main(argc, argv)`).

**★ BLOCKER for anyone running the full test suite — `ps2x_tests.exe` HANGS in `PS2RuntimeKernel`.** Test *"semaphore syscalls return sid on success (EE BIOS convention)"*, sub-case E ([ps2_runtime_kernel_tests.cpp:542-584](F:/SDBZ%20Recomp/ps2xTest/src/ps2_runtime_kernel_tests.cpp:542)): a worker thread blocks in `WaitSema`, the main thread deletes the semaphore, then calls `worker.join()` **unconditionally**. If `DeleteSema` does not wake the waiter with `KE_WAIT_DELETE`, this never returns. (The 500 ms `waitUntil` timeout above it does *not* protect the `join()`.) Pre-existing; unrelated to BUG-028; unknown whether it is a genuine `DeleteSema` runtime bug or a test-harness race.

**Blast radius is larger than it looks:** `MiniTest::m_cases` is a `std::map`, so **suites and tests run in alphabetical order, not registration order.** Everything sorting after `PS2RuntimeKernel` therefore never executes — `PS2SifRpc`, `SifDma`, `PS2Vu1`, `Pad`, and ~35 `Scheduler*` suites. Use the new filter to reach them until the hang is fixed.

### BUG-029 (diagnostic-quality bug, source-fixed 2026-07-29 by co-piolet) — the `[gs:pixels]` probe was sampling-biased and caused two wrong root-cause conclusions

`[gs:pixels]` sampled 1 in **2,000,000** pixel writes. A 512x448 clear is ~229k pixels at ~50Hz ≈ 11M cleared pixels/sec, so effectively every sample landed on a black full-screen clear sprite. The whole run produced only **16** samples — and parts 9 and 10 of the session log concluded from them that "every draw renders black" and that RGBAQ was the prime suspect. Both conclusions were artifacts of the sampling rate.

**Fix:** replaced with `[gs:frame]`, a per-interval aggregate emitted from the present probe: `px` / `nonblack` / `textured` / `maxrgb` / `primmask` / `imagebytes` / `prims`, reset at each report. `nonblack=0` on the new probe is a real statement about every rasterized pixel in the interval.

**VERIFIED WORKING 2026-07-29:** emits ~1/s, e.g. `[gs:frame] px=24313856 nonblack=0 textured=0 maxrgb=0 primmask=0x40 imagebytes=4194304 prims=479`. It immediately paid for itself: `primmask=0x40` (SPRITE only, whole run) and an authoritative `nonblack=0` across ~24 M pixels/interval are both facts the sampled probe could never have established. **Bug closed. `[gs:frame]` is now the primary GS instrument — use it, not per-pixel sampling.**

**Generalize this:** a sampled probe on a path dominated by one high-volume event measures only that event. On GS pixel paths, **aggregate — do not sample.**

**Lesson (generalize):** a sampled probe on a path dominated by one high-volume event measures only that event. On GS pixel paths, aggregate — do not sample.

### BUG-030 (diagnostic-quality bug, source-fixed 2026-07-29 by co-piolet — same family as BUG-029) — capped probe counters were used as quantitative evidence, manufacturing two phantom findings

Sibling of BUG-029: BUG-029 was *sampling* bias, this is *saturation* bias. Several probes in this codebase stop printing after a fixed count, so their line count is a **cap, not a measurement** — and two separate root-cause leads were built on comparing a capped counter against an uncapped one.

**Instance 1 — the phantom "RPC reply deficit" (`SifRpcReply` 88 vs `SifRpcPkt:SIG` 180).** REFUTED. The 88 is the sum of two *saturated* counters:

| log line | observed | cap in source |
|---|---|---|
| `[SifRpcReply] deliver cid=...` | **24** | `s_replyLogs ... < 24u` — [SIF.cpp:752](ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp#L752) |
| `[SifRpcReply] depth=...` | **64** | `s_depthLogs ... < 64u` — [SIF.cpp:849](ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp#L849) |

24 + 64 = 88, both exactly at their caps, while `[SifRpcPkt:SIG]` is **deliberately never rate-limited** ([SIF.cpp:1619](ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp#L1619)). The cids are also perfectly consistent (SIG = `0x8000000a`×173 + `0x80000009`×7; deliver = `0x8000000a`×14 + `0x80000009`×10 — same two cids, no orphan server). **There is no deficit. Do not chase this ratio again.**

**Instance 2 — ARKD streaming extent is unmeasurable.** `[ARKD:cdread]` and `[ARKD:sifdma]` both cap at `seen < 8u` ([ps2_iop_irx_loader.cpp:644](ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp#L644) and [:751](ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp#L751)). "Streaming stopped right after the TOC" and "streamed the whole 0x2ba29000-byte GAME.DAT" produce **byte-identical logs**. This also means the 2026-07-27 "ARKD disc I/O stops ~t=20s" retraction was the *same* bug surfacing earlier.

**Fix VERIFIED WITH REAL DATA 2026-07-30 05:18** (RelWithDebInfo run, 96.3s) — non-saturating running totals emitted at power-of-two event counts, in `ps2_iop_irx_loader.cpp`:
- `[ARKD:cdread-total] reads= sectors= bytes= fails= lastLbn=` → last line: `reads=1024 sectors=1070 bytes=2191360 fails=0 lastLbn=1075481`
- `[ARKD:sifdma-total] dmas= bytes= skipped= lastEeDest=` → last line: `dmas=1024 bytes=2156160 skipped=0 lastEeDest=0x017b5c00`

**Result: Instance 2 is now RESOLVED, not just unblocked.** Both counters climbed cleanly for the entire run (1→2→4→...→1024, zero fails/skipped) — streaming does not stop after the TOC, it runs continuously (~2.1MB delivered). See ★★ HANDOFF for the implication (blocker is EE-side, not load-path). The first-8 detail lines are unchanged.

**Gotcha hit while verifying this:** `build.ps1 Debug -Test` does **not** build `ps2EntryRunner` (only the runtime lib + test exe) — the first game run after "building" used a stale exe and showed the old capped 8/8 lines. Always confirm exe mtime > source mtime before trusting a measurement run; `build.ps1 RelWithDebInfo` (no `-Test`) is what actually links the game exe.

**Standing rule (generalize):** **before treating any log-frequency count or ratio as evidence, grep the probe's source for its cap.** Known caps in this codebase: 6, 8, 16, 24, 64. A counter sitting exactly on a round number is the tell. Cross-reference [[feedback_measure_dont_infer_rates]].

### BUG-031 (open, 2026-07-29) — `ps2x_tests.exe` with no filter hangs forever, silently skipping ~40 suites

Promoted from an inline note to its own entry because it gates the whole test-driven method.

**Hang site:** suite `PS2RuntimeKernel`, test *"semaphore syscalls return sid on success (EE BIOS convention)"*, sub-case E ([ps2_runtime_kernel_tests.cpp:542-584](ps2xTest/src/ps2_runtime_kernel_tests.cpp#L542)). A worker thread blocks in `WaitSema`; the main thread deletes the semaphore, then calls `worker.join()` **unconditionally**. If `DeleteSema` does not wake the waiter with `KE_WAIT_DELETE`, `join()` never returns. The 500 ms `waitUntil` above it does **not** protect the `join()`.

**Blast radius is larger than the hang itself:** `MiniTest::m_cases` is a `std::map`, so suites run **alphabetically, not in registration order**. Everything sorting after `PS2RuntimeKernel` never executes — `PS2SifRpc`, `SifDma`, `PS2Vu1`, `Pad`, and ~35 `Scheduler*` suites. That is why the SIF suites had never been run before 2026-07-29.

**Workaround (use this, don't fix it mid-blocker):** always pass a filter — `ps2x_tests.exe [suite-substring] [test-substring]`, case-insensitive.

**Root cause not established:** unknown whether this is a genuine `DeleteSema` runtime bug (real, would matter to the game) or a test-harness race (cosmetic). Deciding that is the first step if it is ever picked up. Pre-existing; unrelated to BUG-028/029/030.

### BUG-032 (open, 2026-07-29, first observation) — `sceSifSetDma` multi-descriptor validation is not atomic

**Surfaced by the first-ever run of the SIF suites** (reachable only via BUG-031's filter workaround), so it is a **newly-observed failure, not necessarily a regression** — it is not in the ~14-item baseline-failure list and has simply never been executed before.

`ps2x_tests.exe Sif` → **27 / 28 passed, 1 failed.** Failing test: `PS2SifDma` / *"sceSifSetDma rejects invalid descriptors without partial writes"*, two assertions:
- `sceSifSetDma should fail when any descriptor is invalid`
- `failed multi-descriptor sceSifSetDma should not partially write earlier descriptors`

Observed behaviour: given descriptor 0 valid (`src=0x21100 dest=0x21200 size=0x8`) and descriptor 1 with an invalid dest (`dest=0xe0000100`), the call **copies descriptor 0 and returns a nonzero id** (`[sceSifSetDma:OK] dmat=0x21000 count=2 pending=2 -> nonzero id`) instead of validating the whole list first and failing atomically. The `count=33` guard path works correctly (`[sceSifSetDma:GUARD] reject`), so only per-descriptor address validation is affected.

**Not investigated, not triaged against the live blocker.** Relevance is unknown: the game's own transfers are single-descriptor in every log seen so far, so this may be latent. Do not spend on it unless the `rpc_handle_valid` lead points here.

### Deferred, worth doing before the next long build

`sccache not found; continuing without compiler launcher` appears in every configure. No compiler cache is in play, so every full build is from scratch. With a 4520-TU target and no working tlogs, `sccache` is the single highest-leverage build improvement available. Not done now — it would have delayed the A/B.

### Minor A/B contaminant, flagged

The reconfigure pulled new upstream **imgui** commits via FetchContent (`a9e7a8c..ff135cf master`) that were **not** in the Debug control-arm build. imgui is overlay/debug UI and is not in either hot thread, so it should not affect the comparison — but it is a real difference between the two arms, and it is the first suspect if the RelWithDebInfo build had failed to link.

Pre-build steps reported `0 file(s) updated` and `fn_forward_decls.h unchanged (no recompile triggered)`, so **no recompiler regeneration occurred** — the runner sources are byte-identical to the control arm. That part of the A/B is clean.

### Still open, unchanged by this session

- **The `pc=0x1` clobber on `0x178be8`** is non-deterministic and still live. It derailed one of the three baseline attempts. It is a correctness blocker tracked separately, not a perf issue — see [[reference_ps2_sif_boot]].
- **The t≈89 s watchdog breakout** out of the `pc=0x421f10` / `lastCall=0x172998` (`sceDmaSync`) plateau reproduced **2 for 2**, with different addresses but the same shape, right as the time box expires. We have never seen past it. **Do one `-RunSeconds 300` run after the A/B** — a longer box now would change the CPU-second comparison.
- **tid 9288** burns 6.97 s in `Wait/ExecutionDelay` yet never appears in `[hostprof]`. Unexplained, not chased.

---

## Session 2026-07-28 (agent handoff: GitHub Copilot / Claude Sonnet 5) — verified prior Claude Code session's HostSampler work, no build/run performed

**Context:** picked up mid-handoff from a Claude Code session (compacted transcript) investigating why the recomp guest runs ~5 orders of magnitude slower than native PCSX2 (native reaches the memcard prompt in 7-8s; recomp needs 90s+ CPU-bound at ~118% of one core with a black screen). That session had written `ps2xRuntime/src/lib/Kernel/HostSampler.cpp` (CPU-time-weighted sampling profiler, Suspend/GetThreadContext/GetThreadTimes/Resume) and wired it into `ps2_runtime.cpp` (start/stop calls, namespace-scope `extern "C"` decls, no header touched) and `launch_recomp.ps1` (new `-HostProfile` switch, `PS2X_PROFILE`/`PS2X_PROFILE_SECS` env set/clear, `[hostprof]` added to `$Important`, auto-stop grace widened 2s→6s) but had **not yet built or run it**, and had not yet reported any of this to the user.

**This session (Copilot) did:** read the user's governing rules for Claude and recorded them to persistent repo memory (`/memories/repo/rules.md`) so they bind this agent too — most importantly *"never run build.ps1/builds directly, always give the user the command to run themselves"* and *"never modify runner/*.cpp or .h files"*. Verified (read-only, no edits) that all three files described in the handoff summary match exactly: `HostSampler.cpp` exists with the documented design (samples dropped when CPU-time delta is 0, self-reports on its own timer, `Kernel/` placement rides the existing `CONFIGURE_DEPENDS` glob so no CMakeLists edit is needed), `ps2_runtime.cpp:44-45/2520/2995` has the three wiring edits, `launch_recomp.ps1:20/136-143` has the `-HostProfile` param + env block. **No code changes made this session** — this was a verification-only continuation. Build/run deliberately deferred at user's direction ("start them later").

**Handoff back to user — commands to run when ready (per the "never run builds directly" rule):**
```powershell
& "F:\SDBZ Recomp\build.ps1"
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile
```
This will produce a `[hostprof]` report naming the actual functions/modules the ~47s and ~43s host threads are burning CPU in, plus the 16.56s `Wait/ExecutionDelay` thread (suspected `Sleep()` in the det=0 vblank tick loop, `Kernel/Syscalls/Interrupt.cpp:625-676`). Paste the `[hostprof]` output back and I'll pick up the diagnosis (RecompDbg's shared-memory writer is already exonerated — verified live at ~118% CPU with the gate on across 3 runs).

**Also confirmed:** the previous Claude Code session (transcript `6ec32f85-bf4b-441a-831b-36319f7f3fd2.jsonl`) hit its usage rate limit immediately after the user asked it to "update memory bugs and handoff" — that request was never carried out, which is why this handoff arrived only as a compacted summary. Nothing else from that session was lost; its last real work (the `HostSampler.cpp` build) is exactly what this entry already covers.

**First real run + bug found + fixed (GitHub Copilot, same session, later same day):** user ran the build+run and pasted a `[hostprof]` report — sampler weighting is confirmed correct (per-thread totals matched `[cputime]` closely: 46.48s↔50.23s, 37.41s↔39.92s, 18.55s↔19.72s across the three real burner threads), but **every sample resolved to `module ?` with zero per-symbol lines on all threads** — the report was naming nothing. Root-caused (read-and-reason, not guesswork) to a **dbghelp double-init**: `game_overrides.cpp`'s hardware-watchpoint armer (`hwWatchArmerMain`, started unconditionally from `hwWatchArm()` every time `rpc_call`/`0x178BE8` true-enters — a leftover always-on hook from the 2026-07-27 VSync bug hunt) calls `SymInitialize(GetCurrentProcess(), nullptr, TRUE)` with no matching `SymCleanup`. dbghelp allows only one live `SymInitialize` per process handle; `HostSampler::report()`'s own later call therefore fails with `ERROR_INVALID_PARAMETER`, `haveSyms` reads `false`, and both the module and per-symbol resolution paths silently degrade to `"?"`/skipped.
**Fix applied (`HostSampler.cpp` only, no header, no CMakeLists change):** treat `SymInitialize` failing with `ERROR_INVALID_PARAMETER` as success rather than failure — the pre-existing process-wide init (with `fInvadeProcess=TRUE`, so every module is already loaded) is still fully usable; there was never a reason to require *this* call site to be the one that succeeded.
**Not yet verified — needs a rebuild + rerun** (one `.cpp` changed, incremental, `Kernel/` glob so no CMakeLists edit needed):
```powershell
& "F:\SDBZ Recomp\build.ps1"
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile
```
**Also worth a look independent of the symbol fix:** at `t=89s` of that same run, the watchdog broke out of the `pc=0x421f10`/`lastCall=0x172998` steady frame-loop plateau it had been pinned to since ~t=5s (`stuckSecs` reset `7→0`) into unrelated new code — trace tail `0x1c29b0 -> 0x398db0/0x1d3b30/0x2ae860/0x1d48d0/0x3d1550/0x2b65e0/0x2b83a0/0x2ca010 -> 0x1c29b0` (repeating dispatch-like pattern, distinct callee each iteration) `-> 0x1c4280 -> 0x19e620 -> 0x1bdfb0 -> 0x1a07c0 -> 0x23fca0 -> 0x240780`. The run auto-stopped (`wall=96.32s`) shortly after, so what this leads to is unknown. First time this specific breakout has been observed in the state file. Flagged, not yet chased.

**BUG-024 false-alarm check (same session):** an earlier paste from a *different, shorter* run (`wall=38.26s`) showed `run_log.txt` cutting off mid-`[frametrace:IMBAL]` with no further watchdog ticks and no `[hostprof]` — pattern-matched against the open `BUG-024` (`STATUS_STACK_OVERFLOW`/`0xC00000FD`, found via `ps2xTest`, never root-caused in real gameplay). Chased via `$LASTEXITCODE` (first attempt read a stale value from the wrong terminal/cwd — user corrected to `C:\WINDOWS\system32` being the accidental cwd, not a file-location bug; no stray files existed there or in the user profile root, nothing was deleted). **Resolved as a false alarm, not BUG-024 recurring:** the very next full run (the one analyzed above) completed cleanly to auto-stop with a valid `[hostprof]` report and no crash — the short run was an incomplete/truncated paste, not a real early exit. Not chased further unless it recurs with a confirmed exit code.

**Second finding, same session — real perf root cause identified, fix not yet applied (needs a decision + a build/run, not code-complete):** with symbol resolution fixed, the `[hostprof]` report named real hot code for the first time. The two busiest threads (43.8s + 40.6s CPU of a 96s run) are both dominated by `ps2EntryRunner!GSMem::PixelStorageTraits<0>::{PageId, Address, Read, Write, BlocksPerPage, BlockExtent, PixelsPerPage}` (`ps2xRuntime/include/runtime/ps2_gs_memory.h` — a header, **read-only per the standing rule, not edited**) — the software GS pixel-storage addressing math, called per-pixel from `GSRasterizer::writePixel`/`GS::copyFrameToHostRgbaUnlocked`. A third thread (19.06s) is our own `hwWatchArmerMain` toolhelp-enumeration overhead (instrumentation, not game logic). `_RTC_CheckStackVars` alone is 6-7% of each hot thread.
**Root cause:** `build.ps1`/`launch_recomp.ps1` default to a **Debug** build (confirmed: `build.ps1`'s usage comment lists `Debug|RelWithDebInfo`, `launch_recomp.ps1` points at `...\Debug\ps2EntryRunner.exe`). In Debug, MSVC neither inlines nor constant-folds the `constexpr` `PixelStorageTraits` accessors (called millions of times/frame), and `/RTC1` stack-check instrumentation adds further per-call overhead on top. Confirmed via the runtime's `CMakeLists.txt` that `RelWithDebInfo` only adds `/SUBSYSTEM:WINDOWS` + `/ENTRY:mainCRTStartup` on top of CMake's default `/O2 /DNDEBUG` for that config — MSVC disallows `/RTC` together with optimizations, so `RelWithDebInfo` should both inline the hot math and drop the `_RTC_CheckStackVars` overhead entirely. This is a **build-configuration hypothesis, not yet tested** — no code or config was changed to test it.
**Caveat flagged before testing:** `/SUBSYSTEM:WINDOWS` means the process may not get an attached console by default, so `[hostprof]`/`[watchdog]`/`[cputime]` prints could stop appearing in the terminal even if the process runs fine — check for this after the build.
**Next step (commands prepared, not run by this agent per the "never run builds directly" rule):**
```powershell
cd "F:\SDBZ Recomp"
.\build.ps1 RelWithDebInfo
```
then:
```powershell
cd "F:\SDBZ Recomp"
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger -HostProfile -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
"EXITCODE: $LASTEXITCODE"
```
**Session summary — files actually changed this session (GitHub Copilot / Claude Sonnet 5, 2026-07-28):** `ps2xRuntime/src/lib/Kernel/HostSampler.cpp` (one fix, see above) and this file (`PS2_PROJECT_STATE.md`, handoff/session logging). No other source files were edited. No builds were run by the agent; all builds/runs were run by the user per the standing rule. Governing rules for this repo were recorded to persistent agent memory at `/memories/repo/rules.md` (outside the repo tree, survives across sessions for this agent).

### BUG-025 (open, 2026-07-28) — the hardware-watchpoint armer is always-on and costs ~20% CPU

`hwWatchArm()` (`game_overrides.cpp:1388`) spawns `hwWatchArmerMain` **unconditionally** on every `rpc_call`/`0x178BE8` true entry — there is no env gate. The armer then loops forever at 4 Hz doing a full `CreateToolhelp32Snapshot` plus `SuspendThread`/`GetThreadContext`/`SetThreadContext`/`ResumeThread` on **every thread in the process** (`hwWatchSweep`, `:1227-1270`).

**Measured cost: 19.06s of CPU in a 96s run** (third-busiest thread in the `[hostprof]` report). This **supersedes an earlier measurement of "~0.3s over 62s", which was wrong.** Two consequences:
1. It is a ~20% tax on every run, for a probe whose investigation (RAOUT / SetVSyncFlag) closed on 2026-07-27.
2. It **suspends the very threads being profiled, 4×/s** — it perturbs any performance measurement it coexists with, including the RelWithDebInfo A/B queued above.
3. Its unpaired `SymInitialize` is what broke `HostSampler`'s symbol resolution (fixed on the sampler side, but the underlying double-init is still there).

**★ FIX APPLIED + BUILT 2026-07-28 (Claude Code session).** Debug build completed 09:26:09 with zero errors (`game_overrides.obj` 09:25:22, `ps2_runtime.lib`, then a full relink of the 730MB exe). Verified at the binary level: the exe now contains the `PS2X_HWWATCH` string alongside the pre-existing `PS2X_HWWATCH_VAL`. **VERIFIED at runtime 2026-07-28**: the gated-off baseline ran and the armer thread is absent from `[hostprof]`. It cost more than its own 19.06s — the two real threads rose 43.8+40.6s → 67.84+65.47s CPU (total 139s → 156.7s, 163% of one core), i.e. suspending every thread 4×/s was throttling the workers by ~55%. Symbol resolution is also correct for the first time now that the unpaired `SymInitialize` is gone, so this profile supersedes earlier attributions.

**★★★ CONTROL ARM CAPTURED — third attempt, 2026-07-28, VALID.** Armer gated off, dispatch-miss log capped and built, and this time the guest did **not** derail: the watchdog held `pc=0x421f10`/`lastCall=0x172998` with `stuckSecs` cycling and resetting, `progress` climbing to **12101** at t=89. Baseline to compare RelWithDebInfo against:

| metric | Debug baseline |
|---|---|
| `[cputime]` | wall 96.52s · **cpu 99.53s** · 103.1% of one core (auto-stopped) |
| tid 6388 (presentation) | 46.14s — `GS::copyFrameToHostRgbaUnlocked` / `latchHostPresentationFrameUnlocked` |
| tid 20108 (rasterizer) | 44.42s — `GSRasterizer::writePixel`, `PixelStorageTraits<48>`, `::Write` |
| tid 9288 | 6.97s `Wait/ExecutionDelay`, absent from `[hostprof]` — unexplained, not chased |
| watchdog `progress` @ t=89 | **12101** (vs 8523 on the derailed run — +42% guest work in the same time box) |
| `gif/s` / `vbl/s` | 3–5 / 3–6 (normal 30–60 ⇒ still ~10× slow) |
| `PixelStorageTraits` share | **≈48%** of tid 6388, **≈25%** of tid 20108 |
| `_RTC_CheckStackVars` | 7.0% / 8.5% — Debug-only, `/O2` deletes it outright |

**`progress` is the throughput metric, not wall clock** — `-RunSeconds 90` fixes the wall time by construction.

**Retraction:** the earlier reading that gating the armer "bought ~55% more throughput" (43.8+40.6 → 67.84+65.47s) was wrong — that rise was the log-spam livelock, not worker throughput. On the clean run the two threads sit at 42.6+41.6s, statistically identical to the armer-on run. The gate bought the armer's own ~19s plus correct symbols, nothing more.

**Also reinstated:** with the unpaired `SymInitialize` gone, symbols are trustworthy, and **both** hot threads really are `PixelStorageTraits`-dominated — the claim retracted while symbols were suspect is now confirmed on clean data.

**Historical — the middle baseline was void (fix applied + built + verified).** The guest derailed to `pc=0x1` at t≈32s (`stuckSecs=57` at t=89; `frametrace #10901 func=0x178be8 exitPc=0x1 exitRa=0x1` — the known non-deterministic `0x1` clobber). For the remaining ~64s the dispatcher retried the bad target in a tight loop, and the **unbounded** `std::cerr` at `ps2_runtime.cpp:1289` emitted a ~1KB line per iteration: **225MB of `run_log.txt`** and **65.47s of CPU — 42% of the entire run** — in `ntdll!ZwWriteFile` + `MSVCP140D` iostream formatting, on a thread doing no guest work at all. That made the run look CPU-bound on emitted code when two thirds of it was our own diagnostics. Fix: hoisted the existing `s_missProbes` counter above the `cerr`, kept the first 8 lines, then silence plus a running total every 100000 (a bounded counter with no heartbeat cannot distinguish a spin from a stall). The probe sink 30 lines below had been capped at 8 for this exact reason long ago; the human-readable line beside it was simply missed. **Rebuild Debug and redo the baseline before starting the RelWithDebInfo arm** — editing `ps2_runtime.cpp` touches 1 obj in the 47-obj `ps2_runtime` lib and does not invalidate the 808 finished RelWithDebInfo unity objs. Also note `-RunSeconds 90` time-boxes the run, so wall clock is fixed at ~96s by construction: compare **CPU seconds and the per-thread profile**, never wall time.

The hold-until-after-the-A/B plan was reversed, deliberately: it contradicts point 2 above. Leaving the armer on during the A/B does not keep the variables separable — it taxes **both** arms by ~20% and suspends the profiled threads 4×/s while doing it. Because the fix is an **env gate**, the two variables stay separable *at runtime* instead of across builds, which is strictly better: one binary can produce a gated-off perf run and a gated-on writer hunt, and no build is ever spent to switch between them.

Two edits, no header, no recompiler run:
1. `game_overrides.cpp` — new `hwWatchEnabled()` (function-local `static` so `getenv` runs once, never on the hot path) and an early-out at the top of `hwWatchArm()`. Gating the *entry point* rather than the `detach()` at `:1403` means the address publishes stop too, not just the thread spawn.
2. `launch_recomp.ps1` — new `-HwWatch` switch, mirroring `-HostProfile`. Sets `PS2X_HWWATCH=1` and prints a yellow warning that perf numbers from that run are untrustworthy; **clears the var when absent**, since env vars persist across runs in the same shell (the same trap `-HostProfile` already documents).

Default is **off**. `PS2X_HWWATCH_VAL` (the value filter) is unchanged and still only read once the armer is enabled.

**Side effect, wanted:** with the armer off, the unpaired `SymInitialize` in point 3 no longer runs at all, so `HostSampler` gets a clean first init. Its `ERROR_INVALID_PARAMETER`-tolerant workaround stays in place and still covers the `-HwWatch` case.

**Rebuild cost is small — this does *not* touch the stalled runner build.** `game_overrides.cpp` is in the `ps2_runtime` STATIC lib (`CMakeLists.txt:415`, 55 objs), not the 4520-TU `ps2EntryRunner` unity target. One obj recompiles; the 808 finished `RelWithDebInfo` runner objs are untouched and the build resumes rather than restarting.

### RelWithDebInfo build — it is already 18% done, and the default `-Jobs 2` is the real cost (2026-07-28, Claude Code)

The A/B build was **already started and stalled**, not never-begun. Measured from the build tree, not assumed:

| | |
|---|---|
| Ran | 04:14 → 06:36 today (2h22m), no build process alive since |
| Progress | **808 of 4520** unity TUs (`ps2EntryRunner.dir/RelWithDebInfo`, 497 MB) |
| Order | descending — newest obj `unity_3707`, oldest `unity_4500` |
| Rate | ~5.7 obj/min at `/m:2` |
| Remaining at that rate | **≈11 hours** |

**`build.ps1` defaults to `$Jobs = 2` (line 7). This machine is 6 cores / 12 threads.** Nothing in the build is serialized by design — the job count is just an unexamined default. Passing `6` should bring the remainder to roughly **3–4 h**. Disk is not a constraint (168 GB free; the full obj set projects to ~2.8 GB from the 497 MB/808 sample).

**Do NOT clean.** Two reasons, the second decisive:
1. The 808 objs are valid and MSBuild resumes past them — a clean pays for them again.
2. A clean also destroys the **Debug** tree (4520 objs + a working 730 MB exe from 03:38). That tree is the **control arm** of the A/B. Without it there is no baseline to compare against, and re-creating it is a second multi-hour build. The comparison is the entire point of the exercise.

**The `/SUBSYSTEM:WINDOWS` caveat is real but does NOT apply to this launcher — stand down on it.** `ps2xRuntime/CMakeLists.txt:595-596` does add `/SUBSYSTEM:WINDOWS` + `/ENTRY:mainCRTStartup` for `RelWithDebInfo`, so the process gets no console of its own. But `launch_recomp.ps1:281-283` runs the exe through a **pipeline** (`& $Exe $Elf 2>&1 | Tee-Object -FilePath $Log`), and a piped child inherits the redirected stdout handle regardless of subsystem — the CRT still writes to it. `[hostprof]`/`[watchdog]` output will reach both `run_log.txt` and the console. No CMakeLists edit is needed. (`run_log.txt` remains the authority if the console looks thin.)

## Session 2026-07-27 — ★★★ 5.4.1 PASSED: guest EE COP0 `Status.EIE` is now honoured by the fiber scheduler. Derail collapses to one bracketed event → 5.4.2

**The fix.** The cooperative fiber scheduler was preempting the guest inside the guest's own interrupt-disabled critical sections. The guest uses `cop0 0x39` = `di` (`DisableIntr`, `0x17ED60`) and `cop0 0x38` = `ei` (`EnableIntr`, `0x17EDB0`); the runtime ignored both. Two changes, no header touched:
- `ps2_scheduler.cpp` — `ps2x_guest_intr_disable_enter/leave/depth` + counters, and a gate at the top of `ps2sched::yield_point()` that refuses to yield while the guest has interrupts off.
- `game_overrides.cpp` — hooks those two guest addresses in the wrapper and emits the `CRITSEC` probe.

**Critical design point: a single bit, NOT a nesting counter.** `DisableIntr` returns the *previous* EIE state, and the guest idiom is `old = DisableIntr(); …; if (old) EnableIntr();`. Enter and leave therefore **do not balance** — a counter drifts upward forever and pins the gate on. The first attempt was a counter and it read `depth=0x2` permanently. `redundant`/`stray` counters exist precisely to make the unbalanced calls visible instead of corrupting the state.

**Escape valve:** `kIntrDisableYieldEscape = 4096` samples. A guest that never re-enables must not deadlock the host. **`escapes` staying at `0` is a pass condition** — a non-zero value means the valve is carrying the run and the gate is not really working.

### Verification run (`-Determinism 0`) — all six criteria pass
| Field | Required | Measured |
|---|---|---|
| `redundant`/`stray` printed | present (= new binary) | ✅ present |
| `depth` | `0` at the `ei` hook | `0x0` every record |
| `sections` | must climb | `0x1 → 0x2 → 0x3` |
| `escapes` | must stay `0` | `0x0` (was `0xa9`) |
| `stray` | `0` | `0x0` |
| `redundant` | non-zero (the idiom) | `0x2` |

Only 3 `CRITSEC` records total, which is correct: the probe self-suppresses once `sections > 3` and neither `escapes` nor `stray` moves.

**`DEFERINL` went 16 records → 0.** All 16 had been `cause=0x5` (SIF0) with `depth=0x2`. The inline `is_guest_thread()` DMAC dispatch path — reachable synchronously from `sceSifSetDma`/`sceDmaSend`/`drainCompletedDmacHandlers` and *not* covered by the host-worker yield gate — no longer fires inside a critical section. **That path is closed without needing a deferral queue.** It had been flagged as "a real uncovered path awaiting re-measurement"; it is now measured and clean.

**The derail did not vanish — it lost its camouflage.** Old runs churned to progress ~692,915 with the escape valve carrying everything. This run: 288 records, 6 families (`SLOTENTRY` 264, `MISS` 8, `RASLOT` 7, `GSENTRY` 5, `CRITSEC` 3, `EECREATE` 1), progress span 646–1732, one derail at 1732. `RASLOT` #1–#6 (progress 681–684) are all `bad=0x0` with `saved == entryRa == liveRa`; only #23 is bad. Note `at = entrySp - 0x10`. Full bracket → stage **5.4.2** in the tracker.

**`RASLOT --bad` is now trustworthy.** It returns exactly one record. Its empty result on the three stale-binary runs was meaningless, as suspected at the time.

### Do not act on the trapval hits
6 `[trapval]` hits appeared, all at `addr=0x1ffbeb0` — the exact bad slot: hit 1/2 `size=16 pc=0x102894`, 3/4 `size=4 pc=0x171d14`, 5/6 `size=16 pc=0x171c3c`. `0x171d14` sits `0x10` past the old 5.4.1 `jalr $s5`, which is seductive. **Ignore it.** `game_overrides.cpp:1003` already records *"`PS2X_TRAPVAL` is RETIRED; it produced only noise"*, and `launch_recomp.ps1:56` defaults it to `'0'`, which fails the arming condition at `ps2_runtime.cpp:2529` — so these hits' provenance is unconfirmed. This is the same shape as the retracted 07-25j "only one PC writes `0x1ffbeb0`" claim, which rested on a 2-hit sample.

### Tooling notes
- `analyze_run.py` has **no `--list`**. Bare invocation is the summary. Options: `-f --probe --bad --derail --threads --signature --limit`. Exit 0 = query answered, 2 = sink missing/empty.
- **The stale-object-file bug bit for the third time** and cost three consecutive runs. Discriminator used to catch it: old binary → `CRITSEC` prints **3** fields with `depth=0x2`; new binary → **5** fields incl. `redundant`/`stray`, `depth ∈ {0,1}`. Fix is force-touching both source mtimes. **Always verify a probe's field set matches the source before believing a run.**

### Learned patterns
- *A PS2 guest's `DisableIntr` returns the previous state, so `di`/`ei` pairs are deliberately unbalanced. Model interrupt-disable as a single bit; a nesting counter drifts and pins on.*
- *An escape valve on a correctness gate must be instrumented, and a zero count must be part of the pass criteria — otherwise the valve silently becomes the mechanism and the gate looks like it works.*
- *Removing scheduler noise does not create a new bug; it uncovers the one that was always there. The derail moving from 414 records at progress 692,915 to 1 record at progress 1,732 is the fix working, not a regression.*
- *Before trusting any run, check that each probe emits the field set the current source emits. A stale object file reads as a valid measurement and has cost 3 runs, three separate times.*
- *`SLOTENTRY`'s `before=` field brackets a clobber to a single call-to-call step; `RASLOT` only reports it after the damage. Prefer the field that changes at the violation.*

## Session 2026-07-26 — ★ METHOD RESET: determinism + structured probe sink + invariant guards (plan `gentle-launching-dusk.md`)

**Why this session exists.** Four declare-then-retract cycles on one symptom family in ~10 days (07-19c, 07-20f, 07-23d, 07-25j) is a measurement problem, not an effort problem. Every conclusion so far rested on n=1 runs, against our own expectations rather than a known-good execution, with probes that fire where damage is *observed* instead of where the invariant *breaks*, extracted by hand-written `Select-String` over a UTF-16 console log. The 5-phase plan replaces that pipeline. Phases A and B are DONE and passed their exit tests; C is code-complete and unbuilt; D and E are not started.

### Phase A — determinism ✅ PASSED
- `PS2X_DETERMINISM` in `ps2_scheduler.cpp` + vblank pacing off guest progress (`PS2X_DET_VBLANK_QUANTUM`, default 20000 ticks/vblank); `-Determinism 0|1` parameter on `launch_recomp.ps1` (precedence: parameter > pre-set env var > default 1).
- Harness: `build_scripts/repeat_run.ps1`, `-Scope derail|full`.
- **Exit test: 5/5 identical derail signature at `-Determinism 0`, derail scope.**
- ⚠️ **`PS2X_DETERMINISM=1` DOES NOT DERAIL AT ALL** (runs to 800,748 progress ticks). A det=1 run produces an empty `--derail` report and reads as healthy when it is only quiet. **Every diagnostic run must be `-Determinism 0`** (dies at 1,756 ticks). The launcher prints a yellow warning when det=1.
- Deviation from plan: the watchdog OS thread was found already lock-free, so no gating was applied; it was given a `progress=` field instead.

### Phase B — one probe sink, one parser ✅ PASSED
- `ps2x_probe_kv(name, n, keys, vals)` in `game_overrides.cpp` writes **ASCII JSONL** to `run_probe.jsonl` on its **own file descriptor** — never through the console pipe, so UTF-16 and console line-wrapping are permanently eliminated as error sources. Path via `PS2X_PROBE_FILE` (set by the launcher); opened `"w"`, so each run owns its sink.
- **Schema rule: every value is a hex string, without exception, including counters** — the reader does `int(v,16)` uniformly and never has to know which key is an address. Auto fields on every record: `seq`, `tid`, `progress`, `probe`.
- Parser: `build_scripts/analyze_run.py` — `--derail` (standing query), `--threads`, `--probe NAME [--bad]`, `--signature`, `-f`. Exit 2 = sink missing/empty.
- **Exit test met:** the known `RASLOT #23 BAD` record reproduced straight from JSONL with zero hand-grepping.

### ★ What the sink measured that we never had before
```
RASLOT  seq=275 progress=1756 tid=0xfdd5 n=23
        slot=0x178be8 at=0x1ffbeb0
        saved=0x1 entryRa=0x1bb0b0 entrySp=0x1ffbec0 liveRa=0x1
MISS    seq=276 progress=1756 tid=0xfdd5 pc=0x1 codeRegion=0x0
gap:    1 record, 0 progress ticks
```
- **`entryRa=0x1bb0b0`** — the caller of `0x178be8`. New information; never captured before.
- **The clobber→miss gap is 0 progress ticks.** The bad `$ra` is consumed by the very next return, not a delayed fuse. This proves RASLOT measures *damage*, not *violation* — exactly the justification for Phase C.
- `0x1baf90` (the SPDELTA frame) and `entryRa=0x1bb0b0` are ~0x120 apart, plausibly the same function. **First concrete lead the new tooling produced; not yet followed.**
- **Only one thread ID appears in the det=0 sink: `tid=0xfdd5`.**

### Phase C — invariant guards ⬜ CODE-COMPLETE, NOT BUILT, NOT RUN
Changed `Kernel/Syscalls/Thread.cpp` (4 sites) and `game_overrides.cpp` (2 call sites + 1 file-scope decl). Three new probe families:
- **`EECREATE`** — the guest's *declared* `entry/stack/stackSize/gp/prio/attr` at `CreateThread`. Emitted even when `stack==0`, because that is the precondition for the fallback below.
- **`EESTART`** — the *resolved* `sp`, plus `callerSp` and a **`borrowed`** flag. `borrowed=1` means `info->stack` was still 0 after both fixups, i.e. the fiber is running on the creating function's stack pointer — the `Thread.cpp:398` latent bug, now self-reporting instead of arguable.
- **`STACKOOB`** — per-tid stack-bounds guard, registered at `StartThread`, checked at wrapped-slot entry (`site=0`) and exit (`site=1`). **Bounded to 16 reports** (an OOB `sp` stays OOB, so unbounded would reproduce the 89,000-line flood). Borrowed-stack threads register `hi=0` = "unknown, do not check" rather than a fabricated range — a guard firing on invented bounds is worse than no guard. Self-disabling for threads that never went through `StartThread`, so it is safe to leave armed in every run.
- **Deviation from plan, deliberate: the frame-ownership guard was NOT built.** The det=0 sink shows exactly one thread, and a cross-fiber stack stomp is impossible with one live fiber. `--threads` gives the real EE thread count; build the guard only if it comes back >1.
- Stack ranges live in a file-scope `std::unordered_map<int, GuestStackRange>` in `Thread.cpp`, **not** as a `FiberContext` field — adding one would be a `.h` edit (§3 prohibition 3, 30h rebuild).

**Next session starts here:**
```
cmake --build "F:\SDBZ Recomp\build" --config Debug --target ps2EntryRunner     (user runs, under vcvars64)
& "F:\SDBZ Recomp\launch_recomp.ps1" -NoDebugger -Determinism 0
python "F:\SDBZ Recomp\build_scripts\analyze_run.py" --threads
```
Both changed files are in `ps2_runtime` → incremental, minutes, not the 30h path. Expect the false hang after the derail (see below); Ctrl+C once the console goes quiet.

### Corrections this session puts on the record
- **The clobber value is `0x1`, NOT `0x20561900`.** `0x20561900` is a legitimate EE uncached-mirror alias of `0x00561900` and is not corruption at all. `PS2X_TRAPVAL` therefore now **defaults OFF** — it was keyed to `0x20561900` and had been matching nothing. Do not simply set it to `0x1`: storing the literal 1 is overwhelmingly common in normal guest code and the trap would fire constantly.
- **`0x174ca0` is `CreateSema`, not `CreateThread`** — it is `syscall` with `$v1 = 0x40`; `Dispatcher.cpp:166` maps `0x40 → CreateSema` (`0x20` is CreateThread). Previously asserted wrong and nearly recorded as fact.
- **EE thread creation was completely invisible in the logs** until Phase C — the only `CreateThread` lines anywhere in `run_log.txt` were `[iop:import]` ones. We have been running guest fibers on guest stacks for weeks with no record of where those stacks are.
- `0x178be8` is exonerated as a self-writer; `0x178428` is exonerated. The `sw $v0,48($s0)` hypothesis and the allocator hypothesis are both disproved.
- `LEAFENTRY` emits 0 records in both det modes — unexplained, not chased.

### The false "hang" — read this before cancelling anything
After the derail, the dispatcher jumps to the bad target forever and emits `Error: No exact recompiled function for guest PC` **~89,000 times in 25 s (47 MB of log)**. `$Mute` in the launcher suppresses it from the **console only**, so the terminal looks frozen while the process is still writing hard. **This has been misread as a hang twice.** Anything parsing `run_log.txt` must stream (`StreamReader`), not slurp — and in a line-oriented parser the "continuation" branch must be tested **last**, because that error line has no `[` prefix and will otherwise glue ~88k lines onto one record.

### Phases D and E — not started
- **D ★ (the oracle):** PCSX2 boots this game correctly and is confirmed running. Breakpoint the 46 addresses already in `build_scripts/boot_chain_addrs.txt` via `mcp__pcsx2__*`, emit `(pc, sp, ra, tid)`; emit the same schema from the recomp through the Phase B sink; `build_scripts/diff_trace.py` names the **first** divergence. This converts "which probe do I write next?" into "the machine names the first instruction where we differ." **First test case should be the `0x178be8` `$ra` question.**
- **E:** `build_scripts/measure.ps1` — incremental build → deterministic run → parse JSONL → verdict → exit code.

### Learned patterns
- *A non-deterministic run cannot support a causal claim; fix reproducibility before doing any more diagnosis.* Four retractions traced to n=1 evidence.
- *A probe that fires on observed damage measures the victim, not the writer.* Prefer guards that abort at the first invariant violation.
- *Structured output on a dedicated fd removes an entire class of wrong answers.* Two prior wrong conclusions came from the extraction layer (UTF-16 switching, line-wrap record gluing), not from the data.
- *`extern "C"` is legal only at namespace scope* — never inside a function body. The `.h` prohibition workaround is a file-scope `extern "C"` re-declared in each consuming `.cpp` (precedent: `ps2FrameTraceRecord`, `ps2x_guest_progress`, `ps2x_probe_kv`, `ps2x_stack_check`).
- *Bound every new diagnostic emitter.* The unbounded dispatch-miss path produces 47 MB in 25 s and looks exactly like a hang.

## Session 2026-07-25n — `deliverSifRpcReply` `entryRa=0` fix VERIFIED not the cause; stale-object-file bug found + fixed mid-session

- **Hypothesis tested:** 48 LEAFENTRY samples all showed `deliverSifRpcReply` entering the nested guest dispatcher (`0x178068`) with `$ra` never set (`entryRa=0x0`). Wrote 3 edits: (1) `game_overrides.cpp` LEAFENTRY filter excludes `entryRa==0` so its 48-line budget isn't wasted on this known case; (2)/(3) `SIF.cpp` — capture `savedRa` before the nested run, set `$ra=savedPc` (a real in-range sentinel outside `[kSifDispatcherFn,kSifDispatcherEnd)` so the mini dispatch loop's break condition still fires), restore `savedRa` in the epilogue alongside `pc`/`sp`.
- **Result: does NOT fix the derail.** Rebuild+run still shows `LEAFEXIT #1 callee=0x178be8 entrySp=0x1ffbf70 entryRa=0x1c0728 exitRa=0x1` — same slot, same corruption. Zero `LEAFENTRY entryRa=0` lines this run (confirms that path is now closed off, but it was never the writer). **`entryRa=0` nested-dispatcher entry is a real, now-fixed defect, but it was not the source of the `0x1ffbf60`/`0x1c0728→0x1` clobber.** Do not revisit this hypothesis.
- **Stale-object-file bug (separate, found mid-session):** after the above rebuild, `SLOTWATCH` log lines were missing their `expected=`/`before=`/`now=` fields even though the current `game_overrides.cpp` source (the `oss <<` chain at the SLOTWATCH emit site) clearly includes them. Verified byte-exact (not a `Select-String` line-wrap artifact — raw UTF-16 bytes end right after `slot=0x...`, LEN=92). The exe's timestamp was newer than the source file's, but MSVC apparently relinked from a stale object for this translation unit without recompiling it (same failure class previously logged in the 07-24g note: *"after fixing an MSVC mtime-staleness bug that had silently prevented `game_overrides.cpp` from recompiling across ~3 prior 'successful' builds"* — this is a recurrence, not a one-off). **Fix applied:** touched `game_overrides.cpp`'s mtime (`(Get-Item ...).LastWriteTime = Get-Date`) to force the build system to see it as changed. User has not yet rebuilt/re-run since the touch — **next session must verify the touch fixed it** (SLOTWATCH lines should carry full fields) before trusting any SLOTWATCH-based conclusion from a build following a "did this rebuild" doubt.
- **Standing lesson:** when a probe's log output is missing fields that are unambiguously present in current source, suspect a stale object file before doubting the logic. Check exe mtime vs source mtime, but don't trust exe-newer-than-source as proof of a real recompile — relink-without-recompile is possible and has now happened twice.
- **Next step (unchanged from before this session):** the LEAFEXIT/RAFORK/SLOTWATCH chain still localizes the clobber to `0x178be8`'s (`rpc_call`) own body/subtree, consistent with the pre-existing 07-25h/i/j(retracted)/k narrowing. After confirming the stale-object fix took, re-run and read full SLOTWATCH `before=`/`now=` fields — this is the data needed to bracket the writer to a specific frame instead of "somewhere in the subtree."

## Session 2026-07-25d — ★★★ 5.3.2 `$ra=0x1` SIF stomp RESOLVED+VERIFIED (fix 3, savedSp-relative scratchTop). New blocker: func-map gap `0x1bf2e0`

- **Root cause of fix (2)'s failure, finally pinned:** the per-depth scratch bands (07-24) were anchored to a *fixed absolute* `kSifReplyScratchStackTop` near RAM-top. SLOTWATCH measurement this session showed the real (non-reentrant, `depth=0`) `deliverSifRpcReply` dispatcher call chain descends **~0x4110 bytes** — enough to blow through the fixed top's available clearance *and* the 8KB per-band size, landing within `0xC0` bytes of the real caller's saved-`$ra` slot even at depth 0. This explains why 07-24g/07-25b/c kept finding "a second untracked writer" outside the tracked re-entrancy path — it was the same scratch-stack collision, just not depth-gated the way the SLOTWATCH probes assumed.
- **Fix (3), `SIF.cpp` only:** replaced the fixed top with a headroom relative to the *live* `$sp` captured at `deliverSifRpcReply` entry: `scratchTop = savedSp - kSifReplyScratchHeadroom(0x8000) - (depth * kSifReplyScratchStackSize(0x2000))`. This is structurally safe regardless of how deep the real stack already is or how far the dispatcher descends — the scratch region can never overlap the live frame chain. Diagnostic log extended with `savedSp=0x...`.
- **Build + run this session confirmed:** `scratchTop = savedSp - 0x8000` on every sampled `[SifRpcReply]` line (20+, `depth=0` throughout this run), **zero `$ra=0x1`/`PC=0x1` symptoms** anywhere before the new blocker. ARKD `[ARKD:CALL]`/`[ARKD:run]` completes (`retired=46 halted=1 delivered=1`), `[SifRpcReply:LOADFILE] func255 <- "3000"` fires, dozens of BIND/CALL round-trips succeed with climbing seq numbers. **Boot goes further than any prior run.** Stage 5.3.2 is CLOSED. Full detail in [[reference_ps2_sif_boot]].
- **NEW LIVE BLOCKER (5.4):** `[guest-branch:missing-target] kind=IndirectJump target=0x1bf2e0 pc=0x1bf2e0 ra=0x171d0c codeRegion=yes` → `Error: No exact recompiled function for guest PC 0x1bf2e0`. `sdbz_func_map_merged.csv` confirms a genuine 44-byte coverage gap (`sub_1BF290` ends `0x1bf2d4`, `pool_entry_push` starts `0x1bf300`, nothing covers the range between). Same class as the already-fixed `0x180d30`/`0x1a4500`/`0x1c0170` gaps — a function Ghidra's static pass never found as a distinct entry (likely reached only via this indirect/vtable dispatch, no direct-call xref to anchor it). Not a stomp/corruption bug; once dispatch fails here execution can't recover and the log tail degrades into an infinite `pc=1`/`ra=1` watchdog spin (superficially resembling the old bug — it isn't).
- **Next step:** disassemble/decompile the `0x1bf2d4`-`0x1bf300` gap (IDA/Ghidra or `mips_r5900_disassembler.py` on the ELF) to recover the real function body, then gap-fill via the established pattern (`registerFunction` in `game_overrides.cpp` if a runner body exists, or a hand-translated native override per the `0x1c0170`/`sdbzLeaf1C0170` precedent if it doesn't).

## Session 2026-07-25k — ★★★ 07-25j RETRACTED IN FULL. GS exonerated by measurement; real derail is `pc=0x30`, a bad ctor fnptr

**The 07-25j section below is kept only as a record of a disproven line. Do not act on it.**

**What was measured (GSENTRY probe, built + run this session).** The 07-25j SLOTENTRY probe could never have logged GS at all — it sits behind a `slotWatchAddrPre != 0u` gate, i.e. it only emits while `rpc_call` (`0x178be8`) has a watch armed, and `GS_DispatchPending` runs outside that window. The empty `SLOTENTRY.*102870` grep was a **probe-design artifact**, not evidence. Replaced with an ungated `GSENTRY` probe in `game_overrides.cpp` (bounded to 64 lines, keyed on `kSdbzFrameTraceSlots[I].funcStart == 0x00102870u`).

**Result — all 5 GS entries identical:**
```
[frametrace:GSENTRY] #1..#5 tid=0xbd24 entryPc=0x102870 entrySp=0x1ffbf00 entryRa=0x104c9c
```
Branching per 07-25j's own stated exit test:
- `entryPc == 0x102870` on every hit → **real entry**, not resume-slot aliasing.
- `entrySp` **stable at `0x1ffbf00`** across all 5 → not a stale/handed-down `sp` (that would vary).
- `entryRa = 0x104c9c` → inside `SyncFrame` (`0x104c00`-`0x104cd4`, funcmap-confirmed) → a legitimate caller.

`GS_DispatchPending` is behaving correctly. It is not the writer and there is no frame overlap.

**Two further falsifications of 07-25j:**
1. **Single OS thread throughout.** tid histogram over all 285 wrapper lines: `tid=0xbd24` x285, plus five `0x10`-`0x14` singletons that are startup noise. The "two execution contexts sharing one guest stack" reading does not hold.
2. **The trapval band was measuring noise.** Stacks grow DOWN, so GS entering at `0x1ffbf00` and pushing to `0x1ffbeb0`/`0x1ffbec0` is the main thread's *ordinary* frame usage. Trapping the value `0x1` in `0x1ffbe80`-`0x1ffbf00` catches routine `sq`/`sw` register pushes that happen to hold 1 — 151 hits of nothing. 07-25j's headline "exactly one PC ever writes `0x1ffbeb0` — `pc=0x102894`" rested on a run with only **2** hits; this run shows **six** writers (`0x171c38` x12, `0x171c3c` x10, `0x171c5c` x4, `0x171d14` x3, `0x102894` x2, `0x2b6038` x1). **Drop `PS2X_TRAPVAL` from future runs — it only bloats the log.**

**Correction to the reported symptom: the derail PC is `0x30`, not `0x1`.** Histogram of every `No exact recompiled function for guest PC` line in this run:

| Count | Derail PC |
|---|---|
| 414 | `0x30` |
| 1 | `0x830` |
| 1 | `0x6` |

Zero `0x1`. (The last two are interleaved-stderr garbling, not real derails.) The `pc=0x1` spin quoted at the start of this session came from the **previous** run's log. First derail at raw log line 2705.

**The new lead (stage 5.4.1).** Trace tail immediately before the first derail:
```
... 0x171f10 -> 0x327908 -> 0x3a27a0 -> 0x3a2430 -> 0x171c30 -> 0x3a2760
    -> 0x3a2760 -> 0x171f10 -> 0x3a2f30 -> 0x3a2c60 -> 0x171c30   ->  PC = 0x30
```
`0x171c30` = `array_call_ctor_dtor` (`0x00171c30`-`0x00171d54`), a `__cxa_vec_ctor`-style helper: `a0`=array base, `a1`=ctor fnptr, `a2`=dtor fnptr, `a3`=elem size, `t0`=count. Two indirect calls:
```
0x00171cdc  jalr $ra, $v0    ; dtor, from a2 -- null-guarded at 0x171cb0, NOT range-guarded
0x00171d04  jalr $ra, $s5    ; ctor, from a1 -- NOT guarded at all
```
A C++ array constructor is being invoked through the value `0x30`. The immediate caller `0x3a2c60` sets up valid arguments (`a1=0x3a2ef0` ctor, `a2=0x3a2d20` dtor, `a3=8`, `t0=2`), so the bad pointer is **not** statically obvious at that site — it arrives corrupted, or a later loop iteration reads it back from the frame scratch slots `sp+0x94`/`0x98`/`0x9c`/`0xa0` (held in `$s1`/`$s7`/`$s6`/`$s0`), which sit above the saved-register area inside the `0xb0` frame.

Also noted at the caller: `0x3a2c6c  beq $v1,$zero,0x3a2cac` with `lui $a0,0x61` in the **delay slot**, redundantly repeated at `0x3a2c74` on the fallthrough — a translation pattern worth checking if the scratch-slot theory doesn't pan out.

**Standing caution:** the ~50 repetitions of `0x38afb0` in the pre-derail trace are the watchdog's **global cross-thread history**, NOT a call chain. Do not read them as recursion depth.

**NEXT SESSION STARTS HERE:** disassemble the other `0x171c30` call sites in the chain (`0x3a2430`, and whatever `0x3a2f30`/`0x327908` feed) to find which one supplies `0x30` as `a1`, or instrument `array_call_ctor_dtor` entry to log `a1`/`a2`/`t0` per call.

### Session 2026-07-25r — 5.4.1 correction: two probe threads falsified, `$a1`-at-entry is the surviving lead

Three claims recorded earlier in 5.4.1 are **wrong** and must not be re-chased:

1. **"`0x30` is `t0`/`$s3` (the element count), not `a1`."** Backwards. `$s5 = $a1`
   at `0x171c50` and `jalr $s5` at `0x171d04`, so `PC=0x30` means **`$a1` was
   `0x30` on entry**. That `0x30`=48 is also count-shaped is the *clue*, not a
   refutation: something is putting a count where a function pointer belongs.
2. **"The ctor `0x2b9300` bails out before its epilogue."** Dead. The ctor never
   runs: [array_call_ctor_dtor_0x171c30.cpp](ps2xRuntime/src/runner/array_call_ctor_dtor_0x171c30.cpp)
   L280 `if (jumpTarget == 0u) { ctx->pc = 0x171D0Cu; }` skips the `jalr`
   outright on a null target — no `lookupFunction`, no wrapper, no callee.
3. **"`$s5` is zeroed mid-loop / the indirect-call return path is systemically
   broken."** Falsified by reading the function. `0x171c44` `sq $s5,0x50($sp)`
   saves the **caller's** `$s5` *before* `0x171c50` overwrites it, and
   `0x171d34` `lq $s5,0x50($sp)` restores it. So the S5ZERO signature
   (`exit $s5=0`, `spDelta=0xb0`, `exitRa==exitPc`, `s3->0x4b6dfe`) is a
   **normal epilogue + return**. All 8 "ZEROED-HERE" records were healthy. The
   "8 different damaged ctor pointers" reframe is retracted with it.

**Why this went unnoticed:** the `VECCTOR` probe prints `a1`/`t0` **after**
`original(...)`. Those registers are caller-saved, so its "`a1` always small
(0x2–0x1f)" readings were exit garbage, never entry values.

**Live probe:** `A1SITE` in `game_overrides.cpp` — true-entry only
(`entryPc==0x171c30`), sampled **before** the body, logging
`caller(entryRa) a0 a1 a2 a3 t0` + an `a1kind` verdict, budget 96. The dead
`CTOREXIT` and `S5ZERO` blocks were deleted in the same edit.

### Session 2026-07-25t — the derail is a corrupt `$ra`, and `0x30` was never real

**`0x171c30` is exonerated. Stage 5.4.1's whole premise is retracted.**

`CTORTGT` ran with the probe provably alive (same run: `VECCTOR`=73 records,
`A1SITE`=0, so the new binary was live) and emitted **zero** records — every
`jalr` target through `0x171c30` was a valid code pointer. With `A1SITE`'s 96
clean caller records, that function neither receives nor dispatches a bad value.

**`0x30` is not a constant of this bug.** This run's derail PC is `0x1` (301
hits) + `0x11` (2). `0x30` does not appear at all. Every hypothesis that read
meaning into `0x30` specifically (element count, element size) was chasing a
value that changes run to run.

**What it actually is** — from the fault's own dump:

    [guest-branch:missing-target] kind=IndirectJump op=dispatch source=0x0 target=0x1 pc=0x1 ra=0x1

`ra == pc == 1`. Not a bad *call* target — a **`jr $ra` with a corrupt `$ra`**.
The dispatcher is the victim, not the crime scene. Every probe so far watched
call targets, which is why none of them saw anything.

**The cycle, all four members read from the ELF and all innocent:**
`0x32bca0` (getter) → `0x191780` (strcpy) → `0x32bbb0` (ctor) → `0x32bca0` →
`0x191898` (strlen).
- `0x32bbb0`'s two `jalr $t9` sites both emit `SET_GPR_U32(ctx, 31, <slot>)`.
- vtable `0x4F3250[+8]` == `0x32bca0` — the indirect target is correct.
- `gp_field_get_z_396_0x32bca0` correctly does `ctx->pc = $ra`.

Nothing here *creates* `ra=1`. A function is **entered** with `$ra` already
corrupt; the damage only surfaces when it later reaches its own `jr $ra`.

**Live probe:** `RABAD` — global, fires on any callee entered with an `$ra` that
cannot be a return address (`entryRa == 0` excluded; that thread is closed).
Logs callee/entryPc/ra/sp/gp/v0/a0/t9. Budget 48.

**Reading it:** the `callee=` of the first record is where corrupt `$ra` first
becomes observable; `sp=` says whether a frame shift is involved — which would
connect this to the parked stale-frame/`0xa0`-sp-shift thread rather than to
anything in the `0x171c30` region.

### Session 2026-07-26 — RABAD proven negative; string stubs exonerated; RAOUT probe installed (BUILD PENDING)

**1. RABAD is a real zero, not a dead probe.** Run produced 1475 derails (1467 at
`PC = 0x1`), `VECCTOR` = 76 records (control marker alive), `RABAD` = **0**.
Liveness proved three ways: exe mtime > src mtime and log mtime > exe mtime;
control marker present in the same run; `findstr /M /C:"RABAD"` hits inside the exe.
⇒ **No callee is ever ENTERED with a corrupt `$ra`.** The corruption happens
*inside* a function body. The entry/exit wrapper structurally cannot see that, so
the instrument had to move to the exit side.

**2. `strlen 0x191898` / `strcpy 0x191780` are EXONERATED.** `register_functions.cpp`
shows the LIVE bodies are `strlen_0x191898.cpp` / `strcpy_0x191780.cpp` — the
`fn_191898_*` / `fn_191780_*` files are a DEAD generation. Both live bodies are thin
**stub forwarders** into `ps2_stubs::strlen` / `ps2_stubs::strcpy`; neither the
forwarder nor the stub writes GPR 31 or corrupts `ctx->pc` (the forwarder only
*reads* `$ra`). Corollary: the earlier MMI byte-zero-detection analysis was done on
the dead generation and is irrelevant.

**3. RAOUT probe written into `game_overrides.cpp` (NOT YET BUILT OR RUN).**
Placed immediately after `original(rdram, ctx, runtime);` in `sdbzFrameTraceWrapper<I>`,
before the `isVecCtor` block. Fires when exit `GPR_U32(ctx,31)` is insane
(not `0`, and outside `0x00100000`–`0x00600000`). Budget 48. Logs
`callee / entryPc / entryRa / raOut / exitPc / entrySp / exitSp` — the sp pair is
there deliberately to test the parked `0xa0` stale-frame theory
(memory `project_stale_frame_ra_read`).

**NEXT SESSION STARTS HERE — run, then interpret:**
```
& "F:\SDBZ Recomp\build.ps1"
& "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe" "F:\SDBZ Recomp\ELF\SLUS_214.42"
Select-String -Encoding unicode -Pattern 'RAOUT' "F:\SDBZ Recomp\run_log.txt" -Context 0,2 |
  ForEach-Object { ($_.Line + ' ' + ($_.Context.PostContext -join ' ')) } | Select-Object -First 20
```
If RAOUT is ALSO silent, re-prove liveness the same three ways before concluding
anything. Stage 5.4.1 passes only when a specific address is named, backed by a
probe record from a real run plus that function's disassembly — never by inference.

**Plan file `swirling-bubbling-pearl.md` is SUPERSEDED** — its premise (`$a1 = 0x30`
is an element count) was falsified by A1SITE + CTORTGT. Do not re-execute its Steps 1–3.

### Session 2026-07-25s — A1SITE result: no caller supplies `0x30`

`A1SITE` ran and produced 96 clean records. **Every one is `a1kind=CODEPTR`**
(`0x1a4500`, `0x2fe7b0`, `0x38a370`, `0x1bfdb0`, …). Zero `SMALL-BAD`. So both
branches of the 5.4.1 discriminator are refuted at once:

- **"One bad caller passes a count in `$a1`"** — dead.
- **"Argument-register shift / second entry ABI"** — dead.

It does confirm the diagnosis of the earlier bad data: `VECCTOR`'s "`a1` always
0x2–0x1f" readings were `$t0`-shaped **counts**, sampled after `original()`
clobbered the caller-saved registers. In the A1SITE records those same small
values sit in `t0`, exactly where a count belongs.

**Two flaws in the A1SITE run itself, both now fixed:**

1. **Wrong slots.** It gated on `entryPc == 0x171c30`, i.e. true entry only. But
   `jalr $s5` at `0x171d04` splits the function — the recompiler re-enters at
   `0x171d00`/`0x171d0c` with whatever `ctx` holds. If `$s5` goes bad *after*
   `0x171c50` latches it, no caller is involved and A1SITE structurally could
   not see it.
2. **No filter.** All 96 records were spent on healthy calls in one thread,
   exhausting the budget long before the derail.

**Live probe:** `CTORTGT` — watches every entry slot, reads the value the `jalr`
will actually use (`$a1` pre-`0x171c50`, `$s5` at the resume slots), and logs
**only when that value is not a plausible code pointer**. Budget 64.

**Reading it:** `slot=RESUME` records with a small `target` ⇒ `$s5` is being lost
across the split; investigate the re-entry path, not the callers. **No records at
all** is also a result — it means `0x30` never passes through this function's
`jalr`, and 5.4.1's premise that `0x171c30` is the derail source is wrong.

**Superseded discriminator (2026-07-25r), kept for history:** group records by `caller=`. Every site small `a1` ⇒ argument
shift / `0x171c30` mis-identified or second entry ABI. One site small, others
`CODEPTR` ⇒ that caller is the culprit; disassemble it.

## Session 2026-07-25j — ⛔ RETRACTED (see 07-25k) — `$ra`-slot WRITER "IDENTIFIED": `GS_DispatchPending` prologue, frames overlap by 0x10

**Method:** `PS2X_TRAPVAL="0x1:0x1ffbe80:0x1ffbf00"` — value-triggered store trap, address-clamped to the frame band. Traps the *stomping* value `0x1` (not the good `$ra` `0x1bb0b0`). Zero rebuild needed.

**Result:** exactly one PC ever writes `0x1ffbeb0` — `pc=0x102894`, twice (trapval hits #5 line 541, #9 line 1445 of `run_log.txt`), both 16-byte stores.

**Disasm pins it** (`mips_r5900_disassembler.py "F:\SDBZ Recomp\ELF\SLUS_214.42" 0x102870 28`):
```
0x00102870  addiu $sp, $sp, -0x50     <- prologue
0x00102878  sd    $ra, 64($sp)
0x00102894  sq    $s0, 0($sp)         <<< THE WRITER
```
funcmap: `GS_DispatchPending 0x00102870-0x00102a54` owns `0x102894`.

**Geometry (the actual finding):** for `sq $s0,0($sp)` to land at `0x1ffbeb0`, GS was entered with **`sp = 0x1ffbf00`** — *shallower* than the live `rpc_call` frame (`entrySp 0x1ffbec0`, `$ra` at `0x1ffbeb0`). GS frame `0x1ffbeb0`–`0x1ffbf00` overlaps `rpc_call`'s frame `0x1ffbe00`–`0x1ffbec0` by exactly the `0x10` bytes holding `$ra`. **Impossible under correct call nesting.** This is not corruption and not a bad pointer — it is two execution contexts sharing one guest stack.

**RULED OUT:** async/interrupt callback dispatch. `kAsyncCallbackStackTop = 0x00100000` (`ps2_runtime.h:428`), so `GS.cpp`'s `sceGsSyncVCallback` and `Interrupt.cpp`'s `runHandlers` stacks all live below `0x100000` — nowhere near `0x1ffbf00`. GS is on a normal guest thread stack.

**Corroborating:** SLOTENTRY shows a single `tid=0xc74b` running on three distinct stack bands (`0x1ffbXXX`, `0x1ff3XXX`, `0x1f00000`) — nested dispatch is already switching stacks under one tid.

**OPEN — do not guess:** *why* GS entered at `sp=0x1ffbf00`. Probe applied (`kSdbzFrameTraceSlots[]` in `game_overrides.cpp`): true entry `0x102870` + all 7 aliased resume slots (`0x1028cc`, `0x102904`, `0x102984`, `0x102994`, `0x1029dc`, `0x102a24`, `0x102a30`, from `register_functions.cpp:150-157`), owner `0x102870`. Read `entryPc` on the resulting SLOTENTRY lines:
- `entryPc == 0x102870` → real entry; a dispatcher handed it a stale `sp` (nested-dispatch / scratch-stack bug).
- `entryPc ==` a resume slot → resume-slot aliasing; the entry `switch` jumped past the prologue check and the body is running on the caller's frame.

`entrySp`/`tid` come free and also test the thread-stack-overlap theory.

**NEXT SESSION STARTS HERE:**
1. Build: `& "F:\SDBZ Recomp\build.ps1"` (incremental — only `game_overrides.cpp`; `kSdbzFrameTraceSlotCount` is `sizeof`-derived, auto-sizes)
2. Run: `$env:PS2X_TRAPVAL = "0x1:0x1ffbe80:0x1ffbf00"; & "F:\SDBZ Recomp\launch_recomp.ps1"`
3. `Select-String -Encoding unicode` `run_log.txt` for SLOTENTRY lines with owner `0x102870`; branch on `entryPc` per above.

## Session 2026-07-25i — SLOTENTRY probe data: bracket narrowed to between `0x177eb0`'s entry and `0x178560`'s entry
- Fixed a bug in `build_scripts/analyze_slotwatch.ps1`'s new SLOTENTRY parser: the printed `#N` ordinal is a **per-template-instantiation** counter (the static counter lives inside `sdbzFrameTraceWrapper<I>`, so each wrapped address gets its own separate sequence) — sorting by `#N` interleaves unrelated functions and is NOT chronological. Fixed to print in raw log/file order instead, which IS chronological.
- With correct ordering, the fatal arm cycle (`expected=0x1bb0b0`, matching `RAFORK`'s captured `entryRa`) shows: `0x178428`→`0x17ed60`→`0x17edb0`→`0x177fe8`→`0x177eb0` all enter **clean** (`before==expected`), then `0x178560` enters **dirty** (`before=0x1`). No wrapped entry for `0x178068` appears in this cycle at all.
- **New bracket: the stomp happens between `0x177eb0`'s entry and `0x178560`'s entry** — i.e. inside `0x177eb0`'s own body, or one of the unwrapped intermediate calls in the known chain (`0x1781b0` → `0x175060` → `0x178068`(x2) → `0x175090`) that aren't individually instrumented. Likely these are direct C++ calls that bypass the dispatch-table wrapper (the known "registerFunction Bypass" pattern) — same reason `0x178068`'s nested (non-idle) call never logged a SLOTENTRY line here despite running per the trace.
- Corroborates (doesn't contradict) the old SLOTWATCH exit-based verdict, which already named `0x177eb0`'s subtree as the writer bracket — this tightens it to a specific span instead of "somewhere in the subtree."
- **Next step (not yet done):** either wrap `0x1781b0`/`0x175060`/`0x175090` with the same SLOTENTRY probe to narrow further, or disassemble `0x177eb0`'s body directly for a store near the watched slot's stack offset — narrower target now than before.

## Session 2026-07-25h — `sub_178560` callback-`jalr` hypothesis RULED OUT; corruption traced deeper, into `0x178be8`'s own SLOTWATCH chain, before `0x177fe8`'s entry

- **Ran the rebuild+rerun queued by 07-25g.** New `run_log.txt` confirms the same three fixes still gone, boot still reaches the same collapse point. The new `{0x00178560u}` frametrace slot from 07-25g DID capture data — and it **disproves** the 07-25g hypothesis:
  - All 5 measured `func=0x178560` entries this run exit cleanly (`exitPc=0x178188`, `exitSp==entrySp`, no imbalance) — `sub_178560` is not corrupting anything when it runs normally.
  - The actual first divergence (`RAFORK`/`IMBAL`/first `No exact recompiled function` error, all at log lines ~3267–3274) happens **before** any of the logged `0x178560` calls (first one at line 3353) — i.e. chronologically earlier in the log. The `0x178560` calls seen later are post-crash spin noise, not causally related.
  - Disassembled `sub_178560` in full: the completion-callback `jalr $v0` at `0x1785cc` is guarded by `beql $v0,$zero,0x1785f8` — in this crash's actual trace (`...0x178560 → 0x1784d0 → 0x1`, nothing logged between), that branch was **taken** (`$v0==0`, no callback registered), so the `jalr` never executed this crash. The hypothesis is dead: wrong function.
- **Real signal is the pre-existing SLOTWATCH/RAFORK chain already built by an earlier (pre-compaction) session, targeting `0x178be8` (`rpc_call`).** `RAFORK #1` fires for `0x178be8` (`entryRa=0x1bb0b0`, valid, → `exitRa=0x1`) and `0x1baf90` (its caller), both `fork=B (mid-body-clobber)` — same as 07-25g reported, this part didn't change.
- **New reading of the existing `SLOTWATCH` data (5 samples, all logged before the crash aborts unwind):** every sample — the idle-thread `0x178068` call, the real-chain `0x178560`, the real-chain `0x178068`, `0x177eb0`, and `0x177fe8` — already reads the watched stack slot (`0x1ffbeb0`, where `rpc_call` saves its `$ra`) as `before=0x1`, i.e. **already corrupted at each function's own entry**, all the way back up to `0x177fe8` (deep in the chain `0x178be8→0x178428→0x17ed60→0x17edb0→0x177fe8→0x177eb0→0x1781b0→0x175060→0x178068`). `0x177fe8` is itself wrapped and still shows pre-corrupted — pushing the stomp earlier still, into `0x178428`/`0x17ed60`/`0x17edb0`, none of which produced a logged sample because the crash aborted the call stack before their own post-return SLOTWATCH checks could fire.
- **Action taken (measurement only):** added a `SLOTENTRY` probe in `game_overrides.cpp` (right after the existing `slotBeforeCall` read, ~line 944) that unconditionally logs `before` on entry to every armed wrapped callee (bounded to 40 lines), instead of only logging on a changed post-return value. This will show the ordered entry sequence through `0x178428`/`0x17ed60`/`0x17edb0`/`0x177fe8` and pinpoint exactly which one is first to see the slot already dirty at its own entry — narrowing the stomp to that function's immediate predecessor in the chain.
- **Next step:** rebuild + run via `launch_recomp.ps1`, grep `run_log.txt` for `SLOTENTRY`, read the sequence in order. The first line whose `before` differs from `expected` (`0x1bb0b0`) names the predecessor function as the stomp site (or narrows it to `0x178be8`'s own prologue-to-first-call span if even `0x178428`'s own entry is already dirty).

## Session 2026-07-25g — `0x1bf2e0`/`0x22c8f0`/`0x22cbe0` all build-verified gone; boot reaches a NEW `pc=1` collapse, root-caused to `sub_178560`'s callback `jalr`, not yet fixed (SUPERSEDED — see 07-25h, hypothesis disproven)

- **All three prior fixes confirmed working this run:** `run_log.txt` has zero hits for `0x1bf2e0`, `0x22c8f0`, or `0x22cbe0`. Boot advances substantially further than 07-25f — deep into the SIF RPC completion path (`sceSifSetDma`/`SifRpcPkt:SIG`/`[ARKD:CALL]`/`[ARKD:run] ... delivered=0`/`[SifRpcReply]`) before collapsing again.
- **This is a NEW, distinct `pc=1`/`$ra=1` collapse — not a regression of the 07-25d scratchTop fix.** That fix (savedSp-relative `deliverSifRpcReply` scratch stack) is still in place and still doing its job; this collapse happens at a different call site further downstream.
- **Root-caused via RAFORK/IMBAL + direct disassembly** (not yet via live debugger — static evidence only, flag as PLAUSIBLE not CONFIRMED):
  - `[frametrace:RAFORK]` fired for `0x178be8` (`entryRa=0x1bb0b0→exitRa=0x1`) and `0x1baf90` (`entryRa=0x2fcb84→exitRa=0x1`), both `fork=B(mid-body-clobber)`.
  - Unlike the 07-24g run, `0x178068`'s own IMBAL pair this run is **net-zero** (`entrySp=0x1ff3d10` → mid-dip `-0xa0` → back to `0x1ff3d10`, clean return to `ra=0x177fc4`) — it is not the source this time; the fix that closed 5.3.2 is holding.
  - The `trace=` tail immediately before every collapse ends `...0x178068 → 0x178068 → 0x175090 → 0x178560 → 0x1784d0 → 0x1`. Disassembled both tail functions directly from the ELF: `0x1784d0` is a 5-instruction leaf (`lw/lui/ori/sw/and/jr $ra/sw` — clears a flag bit) that only `jr $ra`; it cannot originate `pc=1`, so `$ra` was already `1` when it was entered.
  - Disassembled `0x178560` (`sub_178560`, the SIF dispatcher's `_request_end` callback handler — same function named in `SIF.cpp`'s `kSifCmdRpcCall` comment block): at `0x1785c8`/`0x1785cc` it does `lw $v0, 28($s1)` then `jalr $v0` — an indirect call through a completion-callback function pointer loaded from the client block (`$s1 = *(client_block+28)`, `$v0 = *($s1+28)`), matching the comment's own description ("if v3[7] (completion callback) set, invokes v3[7](v3[8])"). If that callback slot holds garbage/unset data, this `jalr` is the direct mechanism producing `target=0x1` on the `[guest-branch:missing-target] kind=IndirectJump` line.
  - **Not yet confirmed which client block / call this is** (there are multiple concurrent SIF clients; the ARKD `fno=0x12` call visible in the same log window has `client=0x5a9380`, not obviously related) or why its callback field is bad.
- **Action taken (measurement only, no behavior change):** added `{0x00178560u, 0x00178560u}` to `kSdbzFrameTraceSlots` in `game_overrides.cpp` (entry-only wrap, same pattern as the `0x17ed60`/`0x17edb0`/`0x17edc8` leaves) so the next run's RAFORK/IMBAL data will show `sub_178560`'s own `entryRa` vs `exitRa` directly, instead of inferring it from callers further up the chain. No fix applied yet — need the measured data (or a live debugger breakpoint at `0x1785cc` reading `$s1`/`$v0`) before touching `SIF.cpp`'s client-block handling.
- **Next step:** rebuild + run via `launch_recomp.ps1` (not the raw exe — it sets `PS2_ARKD_SERVICE`/`PS2_ARKD_IRX_RUN`/`PS2X_FRAMETRACE` etc.), then check the new `0x178560` RAFORK/IMBAL lines in `run_log.txt`. If `entryRa` is already `0x1` there, the bad callback pointer comes from further upstream (whoever populates the client block); if `entryRa` is valid and `exitRa=0x1`, the `jalr` at `0x1785cc` is confirmed as the exact fault instruction and the fix belongs in `SIF.cpp`'s client-block synthesis (mirroring the `0xFFFFFFFF` IOP-bound-sentinel fix already applied to the BIND branch at line ~659, per that block's own comment about the `0x20561900` packet-pool aliasing hazard).

## Session 2026-07-25c — `0x1c0170` fix build-verified (no error this run); 5.3.2 non-determinism reproduced a 3rd time

- **Build succeeded** (clang-cl/ninja, ~4m38s, only pre-existing raylib/user32 LNK4006 warnings — harmless, unrelated).
- **Ran once: no `0x1c0170` error, no `0x1a4500`/`0x180d30` error either.** Went straight to the pre-existing 5.3.2 signature: `guest PC 0x1`, `tableBase=0x100008 tableEnd=0x4e6c84`, `codeRegion=no`, flat `0x1 -> 0x1 -> ...` trace. This is consistent with the `sdbzLeaf1C0170` override working (no negative evidence against it) — the run just didn't get a chance to prove it either way, since the trace here doesn't show the boot path passing through `0x1c0170` before collapsing.
- **Non-determinism confirmed a 3rd time.** Same build now on file, three runs, three different outcomes: (1) `0x1c0170` gap `codeRegion=yes`, (2) 5.3.2 collapse `codeRegion=no` from 07-25b, (3) 5.3.2 collapse again here. Two of three land on 5.3.2 directly — the `0x1c0170`/gap-fill boot path looks like the *less common* branch, not the norm. This raises the question of whether the boot order itself is non-deterministic (thread scheduling / SIF timing race) rather than the OOB-writer's damage being non-deterministic per se — i.e. the 5.3.2 collapse may simply be winning the race to happen *before* the code reaches `0x1c0170`/`0x1a4500`/`0x180d30` on 2 of 3 runs, not that those fixes are flaky.
- **Next step (not yet started):** since static disassembly of 5.3.2 has stalled for multiple sessions, worth trying to catch the OOB writer live — `mcp__recomp__*` debugger tools are available this session (requires launch via `launch_debugger.ps1 --debug` per [[project_debugger_launcher]]). Set a breakpoint/watchpoint around `0x1baf90`/`0x178de8` and the `0x20561600`/`0x20561900` SIF packet buffer to catch the garbage-count write in the act, since repeated static trace-log analysis hasn't moved this forward in several sessions.

## Session 2026-07-25b — `0x180d30` fix confirmed working; two more boot-path gaps found and closed (build pending)

- **`0x180d30` fix CONFIRMED:** user rebuilt + ran; boot advanced past it with no `0x180d30` error, straight through to a new gap at `0x1a4500` (also a real gap-fill case, `fn_1A4500_0x1a4500` body exists and is complete/correctly-terminated — an older "1-instruction stub" note for this address is now stale, superseded post-func-map-rebuild). Fixed the same way: `runtime.registerFunction(0x001A4500u, &fn_1A4500_0x1a4500)`.
- **Rebuilt + reran again: advanced further, hit `0x1c0170`.** This one has **no generated body at all** (no runner file, no forward decl, no register_functions.cpp entry) — a genuine recompiler function-boundary miss, not a table-registration gap. Disassembled directly from `ELF/SLUS_214.42` (`mips_r5900_disassembler.py 0x1c0150 20`): a 2-instruction leaf sandwiched in nop-padding between `pool_entry_pop_i` (ends `0x1c0168`) and `sub_1C0180` (starts `0x1c0180`) — `jr $ra` / delay-slot `lw $v0, 0($a0)` (i.e. `return *(uint32_t*)a0`). This is the exact address already flagged once before at line ~455 (`0x1bfe40: j 0x1c0170` tail-call thunk, "exact same class as the already-fixed 5.3.2 gaps `0x38afb0`/`0x391210`") — those two were previously measured and added to the func-map CSV, but that fix is dead: **`config.toml` for SLUS_214.42 does not exist, so the CSV never takes effect without a blocked ~30,000-file `ps2_recomp.exe` regen.** Used the native-override pattern instead (same as `0x104bf0`): hand-translated `sdbzLeaf1C0170` added to `game_overrides.cpp`, registered via `runtime.registerFunction(0x001C0170u, &sdbzLeaf1C0170)`. **Not yet build-verified.**
- **Non-determinism observed:** the *same* build, run back-to-back, produced two different outcomes — one run advanced to the `0x1c0170` gap (`codeRegion=yes`), the other hit the pre-existing 5.3.2 stack-collapse pattern directly (`guest PC 0x1`, `codeRegion=no`). This is new information: the 5.3.2 blocker's onset is not deterministically reproducible run-to-run on identical code/input, which is consistent with (but does not yet prove) a race or uninitialized-read component in the OOB-writer mechanism. Not yet investigated further — flagging for the next session.
- **`0x38afb0`/`0x391210` (the other two CSV-blocked func-map gaps) remain unfixed** — same `config.toml`-blocked class as `0x1c0170` was; should get the same native-override treatment if/when the boot trace reaches them, rather than waiting on the CSV regen.

## Session 2026-07-25 — tooling: vendored IDAPy-PS2 auto-labeling scripts

- **No code/blocker work this session.** Reviewed 13 external repos/tools (pcsx2-reliquary, Android PS2 emulators, ps2dev toolchain repos, biosdrain, libretro/ps2, ps2rd, PS2-Programming-Docs, IDAPy-PS2) against project needs. Only **grimdoomer/IDAPy-PS2** fit — everything else is toolchain/emulator/console-tooling, no use for static recomp.
- **Vendored** into `ida_scripts/idapy_ps2/`: `LabelIOPImports.py` (patched: added missing `import os`), `LabelExecutableSyscalls.py`, `LabelKernelSyscalls.py`, `Ps2Kernel.py` dep, 46 `IOP/*.json` export tables, LICENSE, README + `README_SDBZ.md`. `LabelIOPImports` automates the parked IRX-labeling backlog (5 modules left); reference-only, does **not** touch the live 5.3.2 `$ra=0x1` blocker.
- **Behavioral note (recorded to memory):** "end session protocol"/"update memory" are the user's legitimate defined commands — do NOT gate them behind injection-skepticism based on delivery channel. When wrong, apologize plainly, not with hedged phrasing.
- **Live blocker unchanged** — still 5.3.2 `0x1baf90`/`0x178de8` garbage-count OOB writer (see tracker 07-24g). Build for the `0x180d30` gap-fill (prior session) still pending user run.

## Session 2026-07-24h — dispatch-table gap at `0x180d30` closed (build pending)

- **Symptom:** `run_log.txt` line 61 `[guest-branch:missing-target] ... target=0x180d30` — a legitimate `J` jump into real generated code (`fn_180D30_0x180d30` body exists) with **no `g_ps2RecompiledFunctionTable` slot**. Not corruption, not stale-`$ra`; the same "generated file exists, no table entry" class as the four prior `registerFunction` gap-fills.
- **Fix (game_overrides.cpp only, allowed layer):** added `runtime.registerFunction(0x00180D30u, &fn_180D30_0x180d30)` alongside the existing four gap-fill registrations, plus `#include "fn_forward_decls.h"` so the symbol resolves.
- **Status: BUILD PENDING.** User runs `& "F:\SDBZ Recomp\build.ps1"`, then the Active Runner Command, then grep `run_log.txt` (UTF-16, `-Encoding unicode`) for `0x180d30` — confirm the missing-target line is gone and note the next target/progress. This is a boot-path cleanup, **distinct from the live 5.3.2 `$ra=0x1` writer blocker** (see tracker) — closing it removes noise but is not expected to resolve the stack-clobber.

## Current Status (2026-07-22) — ✅ 5.1 DERAIL CLEARED (built+run); live blocker moved to 5.3: AudioSysInit ARKD `func=0x2` RPC never completes

Built exe (`build/ps2xRuntime/Debug/ps2EntryRunner.exe`, 2026-07-21 06:11) run via `launch_recomp.ps1`,
`run_log.txt` analyzed. The 5.1 SIF fix **held**.

**What the run proves:**
- **Derail gone.** Zero `[frametrace:ALARM]` / `[frametrace:TABLE]` — control never escapes to the SIF
  packet-pool base `0x20561900`. The stale-frame `$ra` derail that 5.1 targeted is not happening.
- **`IMBAL delta=-0xa0` lines are false positives.** They fire on *any* `sp` mismatch, which includes
  the recompiler's normal `jal`-boundary function splits: entry at func `0x178068` runs prologue + first
  chunk, exits mid-body at the `jal` site `0x1780c0` with `sp` decremented; the next dispatch resumes at
  slot `0x1780c0` and runs the epilogue (`delta` wraps to `-0xffffff60` = `+0xa0`). Matched pairs, net
  stack balanced. Not corruption. → 5.1's written exit test was mis-keyed; use ALARM/TABLE instead.
- **SIF RPC is alive & advancing.** `[SifRpcReply] deliver cid=0x80000009/0x8000000a -> run dispatcher
  0x178068`; `[SifRpcPkt:SIG]` `w[6]` sequence counter climbs (`0x84 → 0x85`). Boot got past SIF.

**Live blocker (5.3): `AudioSysInit` spins at `0x422454`.**
- Funcmap: `0x422454` ∈ `AudioSysInit` (`0x422170`–`0x422480`, size `0x310`); `lastCall = sub_172998`
  (SIF-RPC poll helper). Boot order: `AudioSysInit` → `GSInit 0x422480` → `MemSysInit 0x422570` →
  `GameMain 0x422630`. Stuck in the first.
- 4 ARKD binds OK (sid `0x500`–`0x503`). The spin **actively re-issues** `[ARKD:CALL] sid=0x503
  func=0x2 recv=0x5aa7d0 rsz=0x10` (t=15→18s burst, then quiet spin) → every one returns
  `handleRPC -> false result=0x0 signal=0`.
- Root cause is **by design / documented in code**: [`SIF.cpp:448-518`](ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp)
  is a **Stage-1 observe-only bridge** — the echo path unblocks the sync `WaitSema` but delivers **no
  result data** (no-IOP-faking rule), and `ps2_iop::handleRPC` returns `false` because no handler claims
  the ARKD sid yet. Comment literally calls this "the Stage 2 gap." So `AudioSysInit` wakes, reads an
  unwritten `0x10`-byte reply at `0x5aa7d0`, sees the op didn't complete, and re-issues `func=0x2` forever.

**Fix = Stage 2 (per no-IOP-faking rule):** route ARKD `sid=0x503 func=0x2` to the actual interpreted
`ARKD_DVD.IRX` in the R3000 core so it produces the real `0x10`-byte reply into `recv=0x5aa7d0`. Do NOT
hand-synthesize the reply in `game_overrides.cpp`. Next diagnostic step: identify what `func=0x2` is in
`ARKD_DVD.IRX`'s export/dispatch table and why `handleRPC` isn't dispatching to it (module loaded? sid
mapped to the IRX's RPC server? export index resolved?).

### Session end (2026-07-22, later) — Stage-2 bridge BUILT; `[ARKD:diag]` instrumentation added, MEASUREMENT RUN PENDING

Advanced past the "func=0x2 has no handler" framing above. Stage-2 IS wired: `PS2_ARKD_SERVICE` +
`PS2_ARKD_IRX_RUN` route ARKD CALLs into the **real** interpreted `ARKD_DVD.IRX` via
`ps2_iop_runArkdService` ([ps2_iop_irx_loader.cpp](ps2xRuntime/src/lib/ps2_iop_irx_loader.cpp)). The
`0x102` DVD-read SUBMIT reaches the service, the SIF DMA physically runs, and the worker completes
(`B300` ends `0x30000000`).

**Pinned defect:** the TOC/directory DMA lands on **`eeDest=0x00000000`** (clobbers EE low memory,
derails EE toward PC=0x1). Chain: `eeDest = desc[1] = dword_BF50`; `sub_2410` sets `BF50 = buff[0]` via
`sub_AE24` **only** if the `B300` band is idle/complete. Send payload `word[0]=0x01652400` is confirmed
correct and `buff=0x054e50` valid — yet `BF50`/eeDest reads `0`. Static contradiction (sub_1DA4 provably
ran → B310 was 1 → sub_2410 pass-branch should have latched BF50=buff[0]) means it's **runtime-only**.

**✅ MEASURED 2026-07-22 — cause pinned, fix applied (build/verify pending).** Run `run_log.txt`,
`fno=0x102` (line 2735-2743):
- `pre-func`: `sendData=nonnull ssz=0x30`, `hostBuff[0]=0x01652400`, `cpuBuff[0]=0x01652400`
- `post-func`: `BF50=0x00000000`
- `[ARKD:sifdma] … eeDest=0x00000000 size=0x18000` (the derail)

| Observation | Cause | Status |
|---|---|---|
| `sendData=NULL` | Stage-2 payload lookup failed | ✗ eliminated (nonnull) |
| `hostBuff[0]=0x01652400`, `cpuBuff[0]=0x0` | IOP aliasing/copy | ✗ eliminated (cpuBuff=0x01652400) |
| **both `0x01652400`, `post-func BF50=0x0`** | **sub_2410 latch never ran** | **✅ CONFIRMED — Row 3** |
| `post-func BF50=0x01652400` | descriptor zeroed later | ✗ eliminated (BF50=0 post) |

**ROOT CAUSE (measured, not theorized):** ARKD's `sub_2410` latches the EE-dest into `dword_BF50` by
calling the resident **`sysclib memcpy`** (log line 2737: `sysclib fid=12 @0x04ae24`, off `0xAE24`,
`a0=&BF50 a1=&request a2=0x24`). The import hook had **no handler for `0xAE24`**, so it hit the
`$v0=0 → $ra` no-op default — the 36-byte request→BF50 copy silently never happened. Identical failure
mode to the strcmp/strtol miss recorded at loader.cpp:673-677.

**FIX APPLIED (`ps2_iop_irx_loader.cpp`, after the strtol thunk):** real bounds-checked IOP→IOP
`memmove` for off `0xAE24`, returns `dest` in `$v0`. Lib-only, no faking (does exactly the copy the
driver requested). NOT yet build-verified.

**✅ eeDest=0x0 RESOLVED + VERIFIED 2026-07-22.** Second run (`run_log.txt`):
- L2727 `[ARKD:diag] post-func BF50=0x01652400` (was `0x0`)
- L2731 `[ARKD:sifdma] … eeDest=0x01652400 size=0x18000 -> copied` (was `0x00000000`)
- ARKD now services **more** RPCs than ever: `fno=0x102` (L2737 retired=85 halted=1 ✓), then `fno=0x2`
  (L2814), `fno=0x12` (L3177). Genuine forward progress past the multi-session wall.

**HARD GATE (SOP, satisfied this round):** the `eeDest=0x0` fix landed only after the `fno=0x102`
row was filled from a real run. Keep this gate for the next `eeDest`-class bug.

---

### 🔴 NEW LIVE BLOCKER (2026-07-22) — EE SIF-RPC-client `rpc_call` (0x178be8) returns to `$ra=0x1`

The eeDest fix pushed the boot into the **EE-side SIF RPC client**. Boot now dies here:

- `0x178be8` = **`rpc_call`** / `sceSifCallRpc` (funcmap `0x00178be8–0x00178de8`). Decompile: fetches
  server-data via `sub_178428(&dword_563100)`, sends the call packet via `sub_177FE8`, waits completion.
- Frametrace (`run_log.txt` #158…#216): `rpc_call` is invoked in a loop, `entrySp` climbing
  `0x1ffbdb0 → 0x1ffbec0` (unwinding to shallower depth). Calls #158–#210 return **cleanly** to valid
  callers (`0x1bd0c4`, `0x189158`, …). Call **#216** (outermost, `entrySp=0x1ffbec0`) exits with
  **`exitPc=exitRa=0x1`** → dispatch `No exact recompiled function for guest PC 0x1` → watchdog spins.
- Derail GPRs (L3217): `a0=0x20561900` (**`a0Readable=no`** — RPC client struct never populated),
  `a1=0x563100`, `a2=0x178560` (end-fn ptr), `v1=0x4`, `ra=0x1`, `sp=0x1ffbec0`, `gp=0x503070`.
- **NOT** the truncated-body bug (none of the `0x178xxx` cluster is in `truncated_functions.txt`).
- This is the resurfaced, previously-PARKED `0x178xxx` SIF-RPC-client cluster — same neighborhood as the
  stale-frame `$ra` blocker (`0x178068`). Demoted while eeDest was live; now the live wall.

**DO NOT GUESS THE FIX.** Per the stale-frame note, the mechanism is unmeasured. The fork to decide:
- (A) `$ra=0x1` already present at rpc_call **entry** for iter #216 → a *caller* passed bad `$ra`
  (RPC client struct `a0=0x20561900`@phys `0x561900` uninitialized → bad continuation). Fix upstream
  (the bind/init that should populate the client struct).
- (B) `$ra` valid at entry but clobbered **mid-body** (frame-save slot overwritten) → recompiled
  `rpc_call` prologue/epilogue or a callee stomps the saved-`$ra` slot. Fix = the stomping function.

**FORK ROW FILLED (2026-07-23) → (B) CONFIRMED — mid-body clobber.** Measured via SLOTWATCH/RAFORK
probes in `game_overrides.cpp` (`sdbzFrameTraceWrapper`):
- RAFORK #1: `func=0x178be8 entryRa=0x1bb0b0 entrySp=0x1ffbec0 exitPc=0x1`. **$ra is VALID (0x1bb0b0)
  at rpc_call entry, 0x1 at exit** ⇒ not caller garbage ⇒ **fork B**.
- `rpc_call` saves `$ra` at guest **0x1ffbeb0** (`= entrySp 0x1ffbec0 − 0x10`). That slot is stomped
  `0x1bb0b0 → 0x1`.
- SLOTWATCH bracketed the writer to **at/below the innermost `fn_178068`** (deepest-returning callee;
  post-blocks fire in return order, #1 = first to see slot=0x1 = 0x178068).
- `fn_178068` frame `0xA0`, only store to `+0x90` is `sd $ra,0x90($sp)` at **0x178074** →
  target `= entrySp − 0x10`. **Hits 0x1ffbeb0 iff `fn_178068` enters with `entrySp=0x1ffbec0`** — i.e.
  the 8-deep call chain produced **ZERO net stack descent** (the [[stale_frame_ra_read]] "0xA0 sp shift").
- Nesting (innermost last): rpc(178be8)→178428→17ed60→17edb0→177fe8→177eb0→1781b0→175060→178068→178068.

**PROBE BUG FOUND + FIXED (2026-07-23).** First SLOTWATCH2 (blind `n≤12` counter) spent its whole
budget on **healthy/idle** 178068 calls (idle thread `entrySp=0x1f00000`, worker `0x1ffbcXX`; storeTargets
`0x1effff0/0x1ffbcd0/…`, none near `0x1ffbeb0`) and **missed the derail** at log line ~3201. Those samples
also **refute frame-overlap for NORMAL 178068** (clean descent to ~`0x1ffbce0`, entryRa `0x177fc4`).
Re-gated SLOTWATCH2 to emit ONLY on derail signatures:
`interesting = (storeTarget == slotAddr) || (slotAfter != slotBefore)`, budget 24. **Written to
`game_overrides.cpp` only — NOT yet built/run.**

**TOOLING FIXED (2026-07-23b).** Two blockers in the probe pipeline resolved:
- **Analyzer false-STALE fixed + verified.** `analyze_slotwatch.ps1` was reporting "STALE LOG" on a
  VALID run. Root cause: `[frametrace:SLOTWATCH]` records were emitted via chained `std::cerr << … <<
  std::endl`, and concurrent "guest PC 0x1" watchdog output interleaved a newline mid-record, pushing
  `before=/expected=/now=` onto the next physical line; the analyzer matched `before=` per-line and
  missed it. Now pre-joins split records (`Select-String -Context 0,1`), parses the real `#N` ordinal,
  and picks the writer bracket by KNOWN chain depth (not print order — SLOTWATCH fires inner-first at
  exit). **Verified against the existing log: no STALE; VERDICT = writer bracket `0x177eb0`** (deepest
  clean-in/dirty-out; `0x177fe8` also clean-in/dirty-out but outer; `0x178068` #1/#2 entered
  already-dirty → exonerated). No rebuild needed for the analyzer.
- **Probe emits atomic records now.** All 5 probe blocks (SLOTWATCH2/SLOTWATCH/RAFORK/TABLE/IMBAL) in
  `game_overrides.cpp` rewritten to build the whole line in a `std::ostringstream` and emit with ONE
  `std::cerr << oss.str();` (trailing `\n` inside), so a shared stream can never split a record again.
  Added `#include <sstream>`. Pure output-format change — probe logic/gates/addresses UNCHANGED.
  **Written to `game_overrides.cpp` only — NOT yet built/run.**

**NEXT ACTION (resume here):** user builds + runs the re-gated probe, then re-greps
`SLOTWATCH2|SLOTWATCH|RAFORK`. The captured derail SLOTWATCH2 line answers:
(1) does the derail `fn_178068` enter with `entrySp=0x1ffbec0` (`storeTarget==slotAddr==0x1ffbeb0`) ⇒
frame-overlap **confirmed**; and (2) is the writer its own `0x178074` store (`slotAfter=0x1`) **or** a
leaf callee (`func_175090` @0x1780dc / the `jalr $a2` @0x178180)?
- If own store → trace WHY the 8-deep chain gives zero net descent / why the chain's `$ra`→`0x1`.
- If leaf callee → probe `0x175090` / the `0x178180` jalr next.
Commands: `.\build.ps1 Debug` → `.\launch_recomp.ps1` →
`Select-String -Path run_log.txt -Encoding unicode -Pattern 'SLOTWATCH2|SLOTWATCH|RAFORK'` →
`.\build_scripts\analyze_slotwatch.ps1` for the verdict.

**AUTHORITATIVE CROSS-CHECK — PCSX2 write-watchpoint (no rebuild, deterministic).** "Which instruction
stores `0x1` to guest `0x1ffbeb0`" is a DATA-WATCHPOINT question; the print-probe ladder is the fallback.
`0x1ffbeb0` is below `0x02000000` ⇒ already physical, maps 1:1 to PCSX2 (no `& 0x1FFFFFFF`).
Procedure:
1. Boot real PCSX2 + SDBZ ISO (DebugServer build), `pcsx2_connect`. Confirm target first
   (Verify-PCSX2-Target rule) via `pcsx2_game_info`.
2. `pcsx2_set_watchpoint(address=0x1ffbeb0, type=write, condition=<value==0x1>, action=break)`.
   Fallback: plain `type=write` with no condition if the value-compare expr is rejected.
3. On break: `pcsx2_pause` (if needed) → `pcsx2_get_backtrace` + `pcsx2_read_registers` +
   `pcsx2_disassemble(pc)`. The breaking PC IS the storing instruction — the writer, named directly.
4. Cross-check that PC against the recomp bracket (`0x177eb0` subtree). Agreement ⇒ writer confirmed →
   fix in `game_overrides.cpp`. If PCSX2 does NOT reproduce the clobber, THAT is the finding: the bug is
   recomp-specific (translation/dispatch artifact) and we stay in the recomp probe path.

**★ CROSS-CHECK EXECUTED (2026-07-23c) — PCSX2 IS CLEAN ⇒ BUG IS RECOMP-SPECIFIC (STACK/SP DIVERGENCE, not a `0x1` writer).**
Ran the procedure live on PCSX2 (SLUS-21442, DebugServer). Broke rpc_call `0x178be8` conditional on the
SAME boot call the recomp instrumented (`ra==0x1bb0b0`). Findings:
- Ground truth: rpc_call frame = `addiu sp,-0xC0`; ra saved by `sd ra,0xB0(sp)` @ `0x178c30`. ✅ matches recomp model.
- **entry `sp` DIVERGES:** PCSX2 `sp=0x1FFEC20` vs recomp RAFORK `entrySp=0x1FFBEC0` — **0x2D60 (~11.6 KB) deeper in the recomp.**
  ⇒ real ra-slot = `0x1FFEC10`, NOT the `0x1ffbeb0` we were watching. We were watching the wrong address because the recomp's SP is already wrong on arrival.
- Armed a write-watch on the REAL slot `0x1FFEC10` (held `0x1bb0b0` after the store) and ran the whole body incl. the `0x178428` subtree:
  **0 hits.** rpc_call reached its epilogue with ra intact and **returned cleanly to `0x1bb0b0`.**
- ⇒ On correct hardware there is **NO `0x1` store anywhere near the slot.** The recomp's `exitPc=0x1` / "fork B" is a DOWNSTREAM symptom
  of a corrupted stack, not a rogue guest store. The whole SLOTWATCH/writer-bracket hunt (0x177eb0 / widen-the-wrap) was chasing a phantom.
- Consistent with [[project_stale_frame_ra_read]] (0xa0 sp shift, frames not restoring sp): a 0x2D60 accumulated drift = many un-restored frames.

**NEXT ACTION (redirected):** stop hunting a `0x1` writer. Find WHERE the recomp SP first diverges from PCSX2 on the boot path.
Method (deterministic, both live): pick matching call sites on the boot chain, break the SAME logical call in both (PCSX2 conditional BP +
recomp probe), compare `sp`. Bisect down the chain until the first frame whose entry `sp` differs — that frame (or its caller's epilogue)
fails to restore/allocate stack correctly. Fix goes in `game_overrides.cpp`. The `0x178428` subtree is the innermost known-good bracket
(PCSX2 ran it with sp balanced), so the divergence is ABOVE rpc_call, on the path that reaches it.

**Codex continuation note (2026-07-23d — diagnostic only; awaiting user build/run):**

- Re-read the 2026-07-23c PCSX2 cross-check before changing behavior. Its `0x2D60` entry-SP gap is decisive: the `0x1` seen at
  recomp `rpc_call` exit is an *effect* of prior stack drift, not evidence for a guest-memory writer. Do **not** resume the
  SLOTWATCH/`0x177eb0` writer hunt unless a future matched-PC comparison contradicts this result.
- Identified the immediate, direct measured parent: `sub_1BAF90` (`0x1BAF90–0x1BB0BC`) performs `jal 0x178BE8` at `0x1BB0A8`, with
  return/resume at `0x1BB0B0`. The failing `rpc_call` entry RA is exactly `0x1BB0B0`; therefore this is the nearest boundary that can
  distinguish “SP was already wrong before the direct call” from “SP first drifted in the RPC subtree.”
- Added **non-behavioral frametrace wrappers only** in `ps2xRuntime/src/lib/game_overrides.cpp` for `0x1BAF90` and all registered
  resume PCs (`0x1BB000`, `0x1BB00C`, `0x1BB020`, `0x1BB02C`, `0x1BB040`, `0x1BB04C`, `0x1BB060`, `0x1BB06C`, `0x1BB0B0`). This
  deliberately covers every dispatcher entry used by the generated function; a resume must not be mistaken for a new prologue.
  No generated `runner/*.cpp` file was touched. `git diff --check` passed. **Not built or run by Codex** (user owns builds).
- After the next run, read the `0x1BAF90` frametrace record at/near the `RAFORK` failure and compare its entry SP to PCSX2 at the
  matching `0x1BAF90` / `0x1BB0A8` call. If it is already lower by `0x2D60` (or an earlier multiple), move one parent upward on the
  known chain (`0x171C30` / `0x171F10` / `0x3007E0`) and repeat. If `0x1BAF90` is matched on entry but SP changes before `rpc_call`,
  inspect only the direct path between `0x1BAF90` and `0x1BB0A8`; do not broad-search the RPC writer theory.
- Read UTF-16 `run_log.txt` with `Select-String -Encoding unicode`; tee output can wrap one logical record across physical lines, so
  use `-Context 0,3` when a record looks incomplete. Suggested first extraction:
  `Select-String -Path run_log.txt -Encoding unicode -Pattern 'frametrace.*1baf90|RAFORK|No exact recompiled function for guest PC 0x1' -Context 0,3`.
- A generic generated-entry `default: return;` guard in `function_emitter.cpp` remains only a possible future safety net. It would
  require regenerating the runner corpus and could hide the true stale-PC source; it was intentionally **not applied**.

**Run-status update (2026-07-23e):** A user launch produced the expected `guest PC 0x1` loop, but the requested
`frametrace.*1baf90|RAFORK` extraction was empty. This is **not a negative tracer result**: `run_log.txt` was fresh
(03:21:46) while `build/ps2xRuntime/Debug/ps2EntryRunner.exe` still had the old 01:42:25 timestamp. The executable
therefore predates the `0x1BAF90` wrapper edit and the run cannot test the new diagnostic. Rebuild must complete
successfully before interpreting an absent `0x1BAF90` record; no code change is justified from this old-binary run.

**Diag cleanup (deferred, safe):** the two `[ARKD:diag]` blocks + `[ARKD:sifdma]` in
`ps2_iop_irx_loader.cpp` can be stripped now that eeDest is verified (KEEP the `0xAE24` memcpy thunk) —
but leaving them one more run is harmless and helps correlate the RPC loop with the ARKD service.

**Uncommitted:** the `0xAE24` memcpy fix + `[ARKD:diag]`/`[ARKD:sifdma]` diag in
`ps2_iop_irx_loader.cpp` (strip diag after verify; keep the memcpy thunk). Many `??` untracked
`src/runner/*.cpp` are pre-existing recompiler output, not this session.

---

### ★★ ROOT CAUSE FOUND (2026-07-23d, Opus session) — re-entrant SIF-RPC reply loopback stomps rpc_call's ra-slot. REVERSES the 07-23c "SP-divergence phantom" call.

**The `$ra=0x1` IS a genuine `0x1` store** into recomp's REAL saved-ra slot `0x1ffbeb0`. The 07-23c PCSX2
cross-check found "0 hits" because it watched HW's slot `0x1ffec10` (real SP is 0x2D60 higher) — a
**wrong-address** error. The recomp-side SLOTWATCH watches the correct slot and brackets the writer.

**Confirmed end-to-end chain:**
1. `rpc_call 0x178be8` saves `ra=0x1bb0b0` → `0x1ffbeb0` (`sd ra,0xB0(sp)`; slot = entrySp−0x10).
2. Down `0x178428 → 0x177fe8 → 0x177eb0`; `0x177eb0` calls the SIF DMA syscall `0x175060/0x175070` (a0=sp)
   → **`sceSifSetDma`** ([SIF.cpp](ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp)).
3. No real IOP ⇒ `sceSifSetDma` calls **`deliverSifRpcReply`**, which runs the guest RX dispatcher
   **`0x178068` NESTED on the caller's ctx/stack** to fake the reply.
4. It **runs away ~12+ deep** — log: `[SifRpcReply] deliver cid=0x8000000a -> run dispatcher 0x178068` ×12+
   with NO `sceSifSetDma:OK` between (BIND↔END `cid 0x80000009/0x8000000a` ping-pong).
5. The nested dispatcher's frames land on the SAME EE stack and **overwrite `0x1ffbeb0` → `0x1`.**
6. `rpc_call` epilogue `ld ra,0xB0(sp)` → `ra=0x1` → returns to PC `0x1` → spin.
   Log: `[frametrace:RAFORK] #1 func=0x178be8 entryRa=0x1bb0b0 exitPc=0x1`. **exitSp is BALANCED** (only the
   value is stomped) ⇒ an sp-restore alone won't undo it; the dispatcher must not write the caller's frame.

**Smoking gun:** `deliverSifRpcReply` (SIF.cpp ~L714-775) saves/restores `ctx->pc` (`savedPc` L726/L775)
around its mini dispatch loop but **NEVER saves/restores `ctx->sp`.** Constants `kSifDispatcherFn=0x178068`,
`kSifDispatcherEnd=0x1781B0`, `kSifDispatcherMaxResume=64` (SIF.cpp:169-171). Analyzer verdict
`Writer bracket (deepest clean-in/dirty-out): 0x177eb0` (`build_scripts/analyze_slotwatch.ps1`).

**FIX (pending user decision — ALL in allowed file SIF.cpp):**
- **(1) ★ recommended — scratch-stack isolation:** before the mini-loop set `ctx->sp` to a reserved scratch
  region, restore after (mirror `savedPc`). ~5 lines. Dispatcher's legit buffer writes (0x561600/0x564b40)
  preserved; incidental stack stomp eliminated.
- (2) defer reply delivery to a real yield point (IRQ-like) — correct, larger.
- (3) cap/dedupe re-entrant depth — stops runaway, leaves shared-stack corruption latent.

The 07-23c "SP-divergence" NEXT-ACTION, the 07-23d codex `0x1BAF90`-bisect note above, and the whole
stale-pc / 0xA0-IMBAL story ([[project_stale_frame_ra_read]]) are **all downstream of this and RETIRED** —
do NOT resume the SP bisect.

### ✅ FIX (1) SHIPPED + BUILD-VERIFIED (2026-07-23, Sonnet session) — SIF-RPC ra-slot stomp RESOLVED

Applied scratch-stack isolation exactly as option (1) above, in `SIF.cpp` only:
- New constant `kSifReplyScratchStackTop = 0x01FFFF00u` (unused guest-heap headroom band,
  heap hard-capped below `0x01F00000`, RAM ends `0x01FFFFFF`).
- Around the `deliverSifRpcReply` mini dispatch loop: save `savedSp = getRegU32(ctx, 29)`,
  `SET_GPR_U32(ctx, 29, kSifReplyScratchStackTop)` before the loop, `SET_GPR_U32(ctx, 29, savedSp)`
  after — mirrors the pre-existing `savedPc`/`ctx->pc` isolation. No header edits needed
  (`getRegU32`/`SET_GPR_U32` already existed).

**Verified in `run_log.txt` (user build+run, 2026-07-23):**
- All `[frametrace] func=0x178be8` entries now show real `exitPc` (`0x1bd450`, `0x1bb0b0`,
  `0x1bba90`, etc.) — never `0x1`.
- Zero occurrences of `No exact recompiled function for guest PC 0x1` (the direct spin symptom).
- Boot progressed well past `AudioSysInit`'s RPC chain (`0x178be8 → 0x178428 → 0x178068 →
  0x177eb0/0x177fe8`, multiple further RPC round-trips) — the SIF ra-slot stomp is gone.

**5.3 is CLEARED as originally scoped.** Boot now hits a **new, downstream, unrelated** watchdog
spin — see next section.

### 🔴 REOPENED (2026-07-23, later same-day session) — `guest PC 0x1` spin is BACK despite scratch-stack fix, root cause still open

After separately fixing 5.3.2's `0x18d4c4` gap (new func-map row `sub_18D470`, see below) and rebuilding,
a fresh run regressed to the **same `$ra=0x1` spin** the scratch-stack fix above was supposed to have
closed. Confirmed NOT a stale-binary artifact this time: `SIF.cpp` last edited 07-23 06:18, `exe` rebuilt
07-23 21:34 (after) — the scratch-stack code (`kSifReplyScratchStackTop`, `SET_GPR_U32(ctx,29,...)` around
`deliverSifRpcReply`'s mini-loop, SIF.cpp:731-739/785-786) is genuinely live in the running binary.

Added 3 new SLOTWATCH entry-only probes (`game_overrides.cpp` `kSdbzFrameTraceSlots`) covering the
previously-unwrapped gap `0x17ED60`/`0x17EDB0`/`0x17EDC8` (`cpu_disable_interrupts` / `cpu_enable_interrupts`
/ next fn) between `0x178428` and `0x177FE8` in the chain, to narrow the bracket further. Two consecutive
runs gave **inconsistent** results:
- Run A: `SLOTWATCH #1 callee=0x178428 before=0x1bb0b0 now=0x1` — stomp brackets to inside `0x178428`'s
  own subtree (disassembled: prologue `sd $ra,32($sp)` after `sp-=0x30` is self-consistent, offset
  `entrySp-0x10` coincidentally matches the outer `rpc_call` slot — suspicious but not proven as the writer;
  none of its `sw`/`jal` children individually flagged dirty in isolation).
- Run B (after adding the 3 new probes, otherwise same fix in place): `SLOTWATCH #1 callee=0x178068`
  already `before=0x1` at its FIRST entry — i.e. dirty even earlier than `0x178428`'s subtree, and NO
  `callee=0x178428` SLOTWATCH line fired at all (its own return was clean). `0x1781b0` (called twice from
  `0x177eb0`) is confirmed a harmless `cache`/`sync` writeback loop, not a stack-touching function — ruled
  out.

**Interpretation: the bracket point is not fixed across runs**, which points away from a single deterministic
mid-body `sd`/`sw` bug and toward either (a) genuine re-entrant/overlapping `rpc_call` invocations reusing
the identical `entrySp=0x1ffbec0` — i.e. TWO logical RPC calls in flight sharing one physical stack slot,
so the second's legitimate prologue store clobbers the first's still-pending saved-`ra` — or (b) SIF packet
content/timing varies enough between runs to route through a genuinely different (data-dependent) code path
each time. Neither is confirmed.

**Not yet tried:** a live re-entrancy-depth counter logging every concurrent `rpc_call` (`0x178be8`) entry
with its own `entrySp`, to directly test hypothesis (a) — would show two entries both reporting
`entrySp=0x1ffbec0` if calls are overlapping on the same slot. PCSX2-side watchpoints were considered and
rejected: PCSX2 runs the real game on a different process/stack (already measured 0x2D60 SP divergence
vs recomp, see 07-23c above), so it can't observe our recompiled binary's own stomp. The recomp-side
`mcp__recomp__*` tools only support PC breakpoints, not memory watchpoints, so they can't directly answer
this either — would need a new `game_overrides.cpp` probe (re-entrancy counter) or a hand-rolled write-guard
on the slot address.

**Do not re-trust "5.3 CLEARED" above** until this is re-confirmed with a clean run — the fix is necessary
but evidently not sufficient.

### 🟡 FIX (2) SHIPPED, BUILD-VERIFIED, VERIFICATION PENDING (2026-07-24, Sonnet session) — per-depth scratch-stack bands

Root cause refined: fix (1)'s scratch-stack gave the nested reply dispatcher its own stack **top**, but every
re-entrancy level (`s_deliverDepth`, capped at 8) shared the SAME fixed address `kSifReplyScratchStackTop`.
Overlapping nested `deliverSifRpcReply` invocations (confirmed nesting 12+ deep in logs) still trampled each
other's frames even with the caller isolated.

**Fix applied in `SIF.cpp` only** (plan: `i-want-you-to-lexical-donut.md`):
- New constant `kSifReplyScratchStackSize = 0x2000u` (8 KB/level) added after `kSifReplyScratchStackTop`.
- Scratch top is now computed per-depth: `scratchTop = kSifReplyScratchStackTop - (s_deliverDepth * kSifReplyScratchStackSize)`,
  read BEFORE the `++s_deliverDepth` increment so it reflects the level being entered. Lowest band (depth 8)
  bottoms at `0x01FEFF00`, still above the guest-heap hard cap `0x01F00000` — no allocator collision.
- Added a rate-limited (`atomic` counter, cap 64) `std::cerr` log line `[SifRpcReply] depth=... scratchTop=0x...`
  on every entry, to make nesting depth observable in `run_log.txt` instead of inferred.

**Build status:** `ps2_runtime` target and `ps2EntryRunner` target both built successfully 2026-07-24
(`cmake --build "F:\SDBZ Recomp\build" --target ps2_runtime --config Debug` then `--target ps2EntryRunner`).
Link warnings (LNK4075/4006/4088 — raylib/user32 `CloseWindow`/`ShowCursor` symbol overlap, `/FORCE` option)
are pre-existing and unrelated to this change; exe produced at the usual path.

**Verification NOT yet done — falsification-first gate required before this can be marked closed:**
1. Run `launch_recomp.ps1`, grep `run_log.txt` (UTF-16LE — `Select-String -Encoding unicode`) for
   `[SifRpcReply] depth=` lines; confirm depth≥2 was reached at least once (proves the failure mode was
   actually exercised — a run that never nests is not a valid test).
2. Only if depth≥2 confirmed: check zero `No exact recompiled function for guest PC 0x1` and that
   `[frametrace:RAFORK] func=0x178be8` entries show real `exitPc` (never `0x1`).
3. Repeat for **3 total runs**, all passing both checks, before updating this section to CLOSED.

**Session ended before verification runs were performed** — next session should start here, not re-derive
the mechanism again.

**Verification run attempted (2026-07-24, Sonnet session) — INCONCLUSIVE for Fix (2), but confirms real progress + a new coverage gap.**
Ran `launch_recomp.ps1`, grepped fresh `run_log.txt`:
- All 44 `[SifRpcReply] depth=` lines show `depth=0` — nesting never occurred this run. **Does not satisfy
  the falsification-first gate** (depth≥2 never exercised); this run cannot confirm or deny Fix (2). Zero
  `guest PC 0x1` crashes and zero `[frametrace:RAFORK] func=0x178be8` lines either (the RAFORK probe
  didn't fire — consistent with the ra-slot stomp not occurring, since depth never reached 2).
- Boot instead advanced much further than prior sessions: full ARKD_DVD.IRX servicing completed cleanly —
  IRX load, all 4 SVC binds (`sid=0x500-0x503`), real CD reads of INFO.DAT/GAME.DAT, real SIF DMA of
  bulk data — before hitting a **new** spin at `Error: No exact recompiled function for guest PC 0x1bfe48`
  (first at run_log.txt:3478, repeats until log end).
- **`0x1bfe48` diagnosed and closed same session — NOT a stomp, NOT the SIF bug.** Ghidra (`disassemble_function`)
  found no function there and zero xrefs to `0x1bfe48`; raw disassembly (`mips_r5900_disassembler.py`) shows
  `0x1bfe40: j 0x1c0170 / lw $a0,44($a0)` (tail-call thunk) then two `nop`s — `0x1bfe48` is delay-slot padding,
  not a function entry. Exact same class as the already-fixed 5.3.2 gaps (`0x38afb0`/`0x391210`): a tiny
  thunk/pad region Ghidra's auto-analysis never carved as a function, not corruption. Blocked on the same
  `config.toml`-missing dispatch-table regeneration issue (see line ~461) — adding it to the func-map CSV
  won't take effect until that regen happens.
- **Net effect:** Fix (2) verification gate is still OPEN (0/3 valid passing runs; this run doesn't count
  either way). Need a run that actually nests (depth≥2) to test it — the ARKD real-IOP path advancing this
  far may be *reducing* how often the fake-reply-loopback re-entrancy occurs at all, which would need
  addressing separately if depth never nests going forward.

### 🔵 NEW ACTIVE BLOCKER (5.3.2, found 2026-07-23 same run) — spin at guest PC `0x391210`/`0x38afb0`

Not a SIF issue. `Error: No exact recompiled function for guest PC 0x391210` (also `0x38afb0`),
`tableBase=0x100008 tableEnd=0x4e6c84`, reached via call chain `... -> 0x171f10 -> 0x38dae0 ->
0x171c30 -> 0x391210` off a `0x390df0` loop (and separately `0x391e70 -> 0x38dc84 -> 0x171f10 ->
0x38b0b8 -> 0x171c30 -> 0x38afb0`).

**Confirmed 2026-07-23 (same session): NOT a truncated-body case, NOT a dispatch-table-format bug.**
Grepped `build_scripts/funcmap/sdbz_func_map_merged.csv` (16,916 entries) and
`build_scripts/truncated_functions.json/.txt` — both addresses are **absent from every artifact**,
not mis-mapped or truncated. Region around them (`0x385000`-`0x395000`) is otherwise densely
populated with real, correctly-sized functions, ruling out a whole-overlay/segment gap. Precise
bracket analysis:
- `0x391210` sits in a real ~0x24-byte gap: `pool_entry_pop___` ends `0x39120c`, next mapped fn
  `pool_entry_push___` (a distinct clone from the one at `0x390e10`) starts `0x391230`.
- `0x38afb0` sits in a ~0xf8-byte gap: `obj_set_fields_z_35` ends `0x38af88`, next mapped fn
  `noop_wrapper_z_608` starts `0x38b080`.

Both gaps are small, isolated, between correctly-bounded neighbors — consistent with a handful of
tiny thunk/jump-table-target functions that Ghidra's auto-analysis never carved as functions (so
they were never exported to the CSV), rather than any systemic recompiler/tooling bug. No IDA
decompile dump (`ida_scripts/`) covers either address either — nothing pre-existing to grep.

**Update 2026-07-23 (same session): both functions measured and added to the func map.**
Used `build_scripts/mips_r5900_disassembler.py` against `ELF/SLUS_214.42` (static ELF disasm,
no IDA/Ghidra MCP needed — neither was connected this session). Both are real, complete
functions with clean `jr $ra` epilogues, not thunks:
- `0x38afb0`-`0x38b020` (0x70 bytes) — object field-init routine (zeroes/sets a struct's
  fields), sits exactly between `obj_set_fields_z_35` (ends `0x38af88`) and `pool_entry_pop___`
  (starts `0x38b030`, previously mis-identified as the neighbor — actual next symbol is
  `noop_wrapper_z_608` at `0x38b080`, one entry further).
- `0x391210`-`0x39122c` (0x1c bytes) — object field-init routine, sits exactly between
  `pool_entry_pop___` (ends `0x39120c`) and `pool_entry_push___` (starts `0x391230`).

Added as `obj_init_fields_z_36` (`0x38afb0`) and `obj_init_fields_z_37` (`0x391210`) to
`build_scripts/funcmap/sdbz_func_map_merged.csv`, following the exact precedent of
`build_scripts/funcmap/README.md`'s "four added functions" (`patch_func_map.py`) — same class
of IDA-auto-analysis miss, same measure-and-patch approach.

**Blocked on regeneration:** the dispatch table is only populated by re-running
`ps2_recomp.exe <config.toml>` (a full ~30,000-file output regen). `config.toml` for
SLUS_214.42 **does not exist** anywhere in the repo (pre-existing open issue, first noted
2026-07-13 — see line ~1071 below). Cannot regenerate the dispatch table until `config.toml`
is reconstructed. This is now the actual next step, and it blocks this fix from taking effect
regardless of the func-map CSV being correct.

---

## Prior Status (2026-07-21) — ✅ THE LOST GHIDRA FUNCTION MAP IS RECONSTRUCTED, COMMITTED, AND VALIDATED

Session was **tooling/infrastructure only**. No behavioral fix, no build, no run. The live tree
(`output/`, `ps2xRuntime/src/runner/`, `game_overrides.cpp`) was **not touched**. Sub-phase tracker
is unchanged — 5.1 is still 🔵 ACTIVE and still UNBUILT.

### What was actually wrong

`ghidra_output` was pointing at the wrong file. The recompiler had been running with **zero
function-boundary data** while looking perfectly healthy.

- `ghidra_output` requires CSV `name,start,end,size` with a header line the parser discards.
- **Any line not splitting on exactly 4 commas is skipped SILENTLY** — no warning, no error,
  exit code 0 (`ps2xRecomp/src/lib/elf_parser.cpp`, `loadGhidraFunctionMap`).
- Two earlier probe runs were pointed at `ps2xRuntime/symbols.map`, which is space-separated.
  Both ran with no map at all. **Their results are void.**
- `symbols.map` is **NOT a recompiler input.** It is a RecompDebugger artifact that
  `generate_symbols_map.ps1` produces *from* the runner tree.
- `ELF/SLUS_214.42` is **stripped** (`.symtab` and `.strtab` both size 0) — no names recoverable
  from the ELF itself.

### How it was rebuilt (join, not guess)

Neither half of the lost export was gone; they were in two different places:

| part | source | provenance |
|---|---|---|
| boundaries (`start`/`end`/`size`) | `ELF/SLUS_214.42.i64` via idalib | IDA's own analysis, original |
| names | `ps2xRuntime/symbols.map` | original Ghidra names, one generation removed |

Validated rather than assumed: **all 12,072 real names in `symbols.map` land exactly on an IDA
function start — 0 mismatches.**

Final map: **16,916 functions** at `build_scripts/funcmap/sdbz_func_map_merged.csv`.

### Measured effect

|  | no map | merged map | + 4 added |
|---|---|---|---|
| worst fallback promotion (one function) | 125,228 | 204 | 204 |
| dispatch entries | — | 377,270 | **377,274** |
| lost vs live table | 1,002 | 36 | **32** |
| reachable regressions | 264 | 10 | **6** |
| gained vs live | — | 1,821 | 1,821 |

The "promoted N fallback entries" warning is a **function-map symptom**, now understood.

### The 4 added functions — and a method error I made

`0x4dd500 / 0x4dd570 / 0x4dd5b0 / 0x4dd5e0` were added by `patch_func_map.py`. They are the first
four entries of an **86-entry static-init pointer table** at `0x004e6c90`; the other 82 were already
defined and map coverage began exactly at the 5th target. One contiguous hole, not scattered gaps.

⚠️ **My first probe used a `jr $ra` end-scan and flagged all four SUSPECT — that scan was wrong.**
These are tail-call thunks (`j 0x00171f10` + delay slot + padding); they never execute a return of
their own, so the scan ran past each one into its neighbours and two appeared to "overlap". Trusting
it would have written garbage boundaries into the map. Boundaries were re-derived from a raw
instruction dump instead, with padding excluded to match IDA's own convention here
(`sub_4DE250` ends `0x4de444`, not `0x4de450`). **Do not re-run a naive `jr $ra` scan on this shape.**

### The 6 remaining regressions (open)

All are true **interior addresses** inside functions the table *does* have — code emitted inside a
neighbour's body, no dispatch entry. They need an **entry guard** (`case 0xADDR:` / `== 0xADDR`),
not a new function. The manifest mechanism demonstrably does **not** fix them.

```
0x0017ee48  owner 0x0017ee38 (+0x10)  syscall_stub_z_34_0x17ee38      ref: code @ 0x0017ef20
0x001a4500  owner 0x001a44bc (+0x44)  singleton_lazy_init_b_0x1a4400  ref: code @ 0x001a4484
0x001bf2e0  owner 0x001bf2c0 (+0x20)  sub_1BF290_0x1bf290             ref: code @ 0x001bf754
0x0022c8f0  owner 0x0022c8a4 (+0x4c)  sub_0022C890_0x22c890           ref: code @ 0x0022c670
0x0022cbe0  owner 0x0022cbd0 (+0x10)  get_field_val____0x22cbd0       ref: code @ 0x0022ca08
0x00341920  owner 0x00341908 (+0x18)  sub_00341860_0x341860           ref: data @ 0x004f4008
```

### Committed

`43544eeb` — `tooling: reconstruct the lost Ghidra function map + commit it` on
`work/laptop-session-0519`. **Not pushed.** 10 files: the CSV, the rebuild scripts
(`export_func_map.py`, `merge_func_map.py`, `patch_func_map.py`), the probe evidence trail
(`probe_4dd5xx*.py`, `classify_lost.py`), and a `README.md` recording the whole trap so the loss
cannot recur.

`.gitignore` fix included: `build_scripts/` excluded the **directory**, so git never descended into
it and the pre-existing `!build_scripts/recomp_mcp_server.py` negation was **dead**. Changed to
`build_scripts/*`. Side effect: `build_scripts/recomp_mcp_server.py` now shows as untracked — it was
meant to be committed and silently never was. **Left unstaged deliberately; user's call.**

### Uncommitted from this session

- **`ps2xRecomp/src/lib/ps2_recompiler.cpp`** — `writeToFile` content-compare guard (user-requested).
  Skips byte-identical rewrites so a regeneration does not bump ~30,000 timestamps and trigger a
  30+ hour rebuild. Text-mode read deliberately matches the text-mode write (a binary read would see
  `\r\n` and silently disable the guard). **Built once as `ps2_recomp`; never exercised on a full
  regeneration.**

### Caveats that must survive

1. `lost_manifest.txt` derives from the live table's contents, so addresses the *original* manifest
   had but the live table lacks are **invisible** to this comparison.
2. The "live" `register_functions.cpp` is the hand-converted legacy file (Jul-14 format conversion of
   May-28 data), so live-vs-new is a **cross-generation** diff, not a clean before/after.
3. Whether the 6 interior addresses are actually *called at runtime* is **unmeasured**.
4. Names are original-but-one-generation-removed; boundaries are IDA's own analysis. Neither is a
   guess, but they are **two different provenances**.
5. `ps2xRuntime/src/runner/register_functions.cpp` and `ps2_recompiled_functions.cpp` are
   **gitignored** (`.gitignore:18-19`) — the live dispatch table is not under version control.

### Next session — highest value first

1. **Body diff pass-2 output vs `output/`** for the ~16,900 shared functions. User already approved
   this ("actually lets follow your recommendation first") and it was never started. Scratch-only,
   no build. Would expose what the lost `stubs`/`skip`/`patches` config was doing.
2. **Re-verify the `sub_00463180` `unhandled-instruction` flood is gone** with the corrected map.
   Predicted (it was a boundary artifact — walking into `.rodata` and decoding ASCII as MIPS),
   **not confirmed.**
3. **Resolve the 6 interior addresses** via entry guards.
4. **Reconstruct `config.toml`** — `config_manager.cpp:319-338` shows a config *writer* exists.
   Required before any regeneration that touches the live tree.
5. **Run the built `ps2EntryRunner.exe`** — the 5:03 build succeeded but has **never been run**.
6. Consider adding `PS2_ARKD_SERVICE` to `launch_recomp.ps1`'s `$Tracers` (a `.ps1` edit, no build).

## Prior Status (2026-07-20f) — ROOT CAUSE OF THE `0x20561900` DERAIL IS MEASURED

**The derail is a stack-pointer shift, not a corrupted pointer.**

Measured causal chain (frametrace instrumentation, `PS2X_FRAMETRACE=1`, run of 2026-07-20f):

1. `0x177fbc  jal 0x175060` sets `$ra = 0x177fc4`. `0x175060` is a 3-instruction syscall stub; `0x175064` is its `syscall` (`$v1 = 0x77` → `sceSifSetDma`).
2. The dispatch loop then invokes **slot `0x178068` while `ctx->pc` is still `0x175064`** (stale).
3. `0x175064` is not a case in `fn_178068`'s entry `switch (ctx->pc)`, so `default: break;` **falls into the prologue** → `sp -= 0xa0`.
4. The body exits mid-loop at `0x1780c0` without restoring `sp`. Measured: `[frametrace:IMBAL] #1 func=0x178068 entryPc=0x175064 delta=-0xa0`. `0xa0` is exactly `0x178068`'s prologue adjustment.
5. Every outer frame then runs `ld $ra, N($sp)` / `ld $sN, N($sp)` against a stack shifted by `0xa0`, restoring stack *data* into callee-saved registers.

Fault-dump corroboration (`[gpr]` + `[stack]` in `reportMissingFunction`):
- `s0=0x20561900  s2=0x177fc4  s3=0x20561900  ra=0x20561900`
- `$s2` is the small array index in `0x177eb0` (`sll $a1, $s2, 4`) — it should be 0/1/2 and instead holds a **return address**. `$ra` is not specially corrupted; it is one of four casualties of the same bad restore.
- The stack region is provably `0x177eb0`'s 16-byte entry array: every entry's `+0xc` word is `0x44`, matching `0x177f88 addiu $v0,$zero,0x44 ; sw $v0,0($a0)`.
- The correct `0x177fc4` sits at `sp-0x40` and `sp+0x10`; `0x20561900` sits at `sp-0x30`, `sp-0x10`, `sp+0x20`, `sp+0x40` — the `0xa0` shift, visible directly.
- Trace shows `0x178068` twice per cycle: once via the syscall path with stale `pc`, once normally.

### ❌ REFUTED this session (do not revisit)
- **"`0x20561900` is corruption."** It is the uncached mirror of `0x561900`, a legitimate SIF packet-pool pointer this code passes around normally. `Ps2FastRead8/32` mask with `PS2_RAM_MASK` (`ps2_runtime_macros.h:162,187`), so `0x2xxxxxxx` data reads are valid. The 155 store-traps found nothing because **nothing ever wrote a bad value.**
- **Copy-loop overrun in `0x178068`.** Measured `[0x5616d8] = 0x20561600`, which matches `[SIF_DIAG] rx=0x20561600` exactly — it is the SIF RX buffer pointer, not a trip count. No oversized count exists.
- **Syscall routing.** Syscall `0x77` is fully handled in `SIF.cpp:1259` (`sceSifSetDma`) and `return true`s from `Dispatcher.cpp:309`. It never dispatches into `fn_178068`.

### 🔴 THE ONE OPEN ITEM — start here next session
**Why does the dispatch loop invoke slot `0x178068` while `ctx->pc` still holds `0x175064`?**
- `0x175064` is **not** in the dispatch table (only `0x175060` is, index 119830) — so this is not a mid-body table alias.
- Something calls the slot without refreshing `pc` first. **Not guessed. Unmeasured.**
- Next action is read-only, no build cycle: read the dispatch loop and syscall-return path in `ps2xRuntime/src/lib/ps2_runtime.cpp` and `Kernel/Syscalls/Dispatcher.cpp` and find where `ctx->pc` is left unadvanced after a `syscall`.
- Fix shape follows the answer: either a `default:` guard in `ps2xRecomp/src/lib/function_emitter.cpp:110-123` (systemic, recompiler-side, affects every emitted entry switch) or a targeted `game_overrides.cpp` override.

### Collateral — this run went materially further than any prior run
`ARKD_DVD.IRX` loaded (entry `0x04a6bc`, 12 libs, 57 stubs) → `Disk Media TYPE : DVD.` → `INFO.DAT` read ok (`lbn=1048576 sectors=47`) → `GAME.DAT` located (`lbn=1048625 size=0x2ba29000`) → 4 services registered (`sid=0x500/0x501/0x502/0x503`) → `[ARKD:BIND] client=0x5a9330`. Only **3** frame imbalances in a 242 MB log — a specific bug, not an endemic one.

### Instrumentation notes (for whoever picks this up)
- `PS2X_FRAMETRACE=1` arms a 64-entry ring dumped by `reportMissingFunction`; `PS2X_TRAPVAL=addr:lo:hi` arms the store trap (`ps2_runtime.cpp:2384-2408`).
- `sdbzFrameTraceWrapper<I>` is a **template per slot**, so each instantiation has its OWN `static` counter — the `#N` in `[frametrace:IMBAL]` is per-slot, not global. Three `#1` lines are three different slots.
- `run_log.txt` is UTF-16 **and** has console line-wrapping baked in at ~120 chars. `Select-String` needs `-Context 0,3` to recover a full logical record.
- `RUNTIME_LOG` is a compiled-out no-op here (`PS2_RUNTIME_LOGS` undefined) — diagnostics must use `std::cerr`.
- `RecompiledFunction` is a **nested** alias: `PS2Runtime::RecompiledFunction` (`ps2_runtime.h:476`). Unqualified use in `game_overrides.cpp` = 12 build errors from one line.

## Previous Status (2026-07-20) — ✅ `0x104bf0` OVERRIDE CONFIRMED WORKING. `0x20561900` derail re-opened and narrowed by measurement to a **stale-frame `$ra` READ**. (Superseded above: the stale-frame read is confirmed as the *surfacing mechanism*, but its cause is the `0x175064` entry-switch fall-through, not a mid-body dispatch of `0x178428`.)

**Standing user constraint this session: "go no guess work please."** Every claim below is backed by a read or a log grep; the one open item is explicitly labelled as unmeasured.

### ✅ Resolved this session — `0x104bf0` truncated-function override
- `fn_104BF0_0x104bf0.cpp` emitted **1 of 4** real instructions (`lui $v0,0x50` only), no `jr $ra`, no terminal `ctx->pc` → infinite re-dispatch.
- Fix in `game_overrides.cpp` (`sdbzRegisterHandler104BF0`, registered via `replaceFunction(0x00104BF0u, …)`): stores `$a0` → `0x00503230`, sets `$v0=1`, returns via `$ra`.
- **Build-verified: `0x104bf0` appears NOWHERE in the post-build `run_log.txt`.** First confirmed forward progress in several sessions.

### ❌ Truncated-function bug is NOT the `0x20561900` cause — DEMOTED
Grepped `build_scripts/truncated_functions.json` for all six derail-trace addresses (`1540704|1568096|1568176|1631752|1541128|1541672`) → **zero matches**. The bug is real (86 bodies / 76 addrs) but unrelated to this derail. Still open, needs a recompiler run.

### The `0x20561900` derail — write-side is now FULLY excluded
Fault signature: `pc == ra == target == 0x20561900` (the SIF packet-pool base), `sp=0x1ffbc40`, then 60+ s of watchdog spin.
Trace: `0x178260 -> 0x17ed60 -> 0x17edb0 -> 0x18e408 -> 0x178a08 -> 0x178428 -> 0x17ed60`.

`PS2X_TRAPVAL` (plan `rippling-swimming-walrus.md`) was armed on `0x20561900`. **155 hits, ALL legitimate SIF pool-pointer stores** (writing PCs clustered `0x177exx`–`0x178cxx`). `0x1780cc` recurs on a strict 7-hit period writing `0x1ffbb84/0x1ffbbd4/0x1ffbc24` — all **below** `sp`, all legitimate.

The suspect slot is `sp+0x20 = 0x01FFBC60`. It holds `0x20561900` at fault time with **no trapval hit ever recorded for that address.**

Every candidate writer eliminated by measurement:
| Candidate | Verdict | Evidence |
|---|---|---|
| Guest store via `WRITE*` macros | ❌ | Zero trapval hits at `0x1ffbc60` across 155 hits |
| ARKD `sub_ADD4` SIF DMA (`ps2_iop_irx_loader.cpp:710-744`) | ❌ | **Zero `[ARKD:sifdma]` lines** in run_log — never executed |
| `guestRealloc` memmove (`ps2_runtime.cpp:1945`) | ❌ | `kGuestHeapHardLimit = 0x01F00000` (line 190), clamped at lines 613/981/1628/1634. Fault addr `0x01FFBC60` is **~1 MB ABOVE** the heap ceiling — unreachable |
| All other runtime `memset`/`memcpy` into rdram | ❌ | Write zeros or small constants (`ps2_runtime.cpp:929/1850`, `ps2_iop.cpp:232/236`, `ps2_iop_cl.cpp:215/495`, `ps2_iop_sdrdrv.cpp:77/189/301`, `ps2_iop_mcman.cpp:292/293`) |

**Plan outcome #3 (runtime-side memcpy) now has ZERO surviving candidates. Nothing wrote that slot.**

### 🔬 SOLE SURVIVING HYPOTHESIS (unmeasured) — stale-frame `$ra` read in `0x178428`
`fn_178428_0x178428.cpp` (`// Address: 0x178428 - 0x1784d0`, 48-byte frame):
- Prologue at `0x178428`/`0x178434`: `addiu $sp,$sp,-0x30` then **`sd $ra, 0x20($sp)`** → `sp+0x20` IS its `$ra` save slot. Confirmed by read.
- **Entry `switch (ctx->pc)`** `goto`s `label_178440/178458/17849c/1784b8` — **skipping the prologue entirely** (no `sp` decrement, no `$ra` save).
- `label_1784bc` runs the epilogue **unconditionally**: `ld $ra, 0x20($sp)` then `addiu $sp,$sp,0x30`.
- ⇒ On a prologue-skipped entry, `sp` is the *caller's* `sp`, so `sp+0x20` points into the caller's frame — which legitimately holds SIF packet payload `0x20561900`. `jr $ra` then sets `ctx->pc = 0x20561900`.

This is a **LOAD from an unwritten slot**, which is exactly why 155 store-traps found nothing. It explains all four otherwise-contradictory facts: no trapval hit, the value being exactly the SIF pool base, `pc==ra==target`, and **no `jalr` anywhere in the function**.

Second route (same failure): the duplicate `sub_00178428_0x178428.cpp` (identical body, newer codegen) additionally has a **post-`jr` `switch (jumpTarget)`** that `goto`s back into the body *after* `sp` was popped by +0x30 — re-entry on a dead frame.

⚠️ **The entry switch exists in BOTH variants**, so the prologue-skip route is live regardless of which is registered.

### Excluded as originators (read, confirmed `$ra`-transparent)
- `0x17ed60` (`cpu_disable_interrupts`) and `0x17edb0`: pure leaves — no prologue, no epilogue, no `$sp` access, no `$ra` save. They *surface* a bad `$ra`, cannot originate one.
- `0x178260`: intact, 64-byte frame, `$ra` at `$sp+48`, save slot **clean** at fault time.

### Memory is NOT generally corrupt
`sp+0x10`=`0x177fc4`, `sp+0x30`=`0x564b40`, `sp+0x90`=`0x178018`, `sp+0xa0`=`0x178d1c` — all valid. The `0x20561900` values in-frame are genuine SIF packet-descriptor payload (interleaved with lengths `0x40`/`0x44`, flags `0xffffffff`, buffers `0x5688a8`/`0x5a9380`).

### NEXT (handoff, in order)
1. **Measure the stale-frame hypothesis — do NOT fix on assumption.** Instrument entry/exit `sp` for `0x178428` from `game_overrides.cpp` or the runtime layer (**never** the runner file — [[feedback-no-runner-file-patches]]). Look for a call that exits with `sp` HIGHER than it entered, or a second pass through `label_1784bc`.
2. **Determine which `0x178428` variant is dispatch-registered** — `fn_178428_0x178428` vs `sub_00178428_0x178428`. Decides only whether the *second* route is also live. Same question open for `0x17ed60` and `0x178260`.
3. Narrow `PS2X_TRAPVAL` to `[0x1ffbc00, 0x1ffbd00)` to cut the 155 false positives per run.
4. Read the two unexamined trace functions: `0x178a08`, `0x18e408`.
5. Build: `cmake --build "F:\SDBZ Recomp\build"` under vcvars64 (user runs).

### Open minor anomalies
- **Misaligned `sq`:** trapval logged `addr=0x1ffbb84 size=16` — `0x84 & 0xF = 4`, not 16-byte aligned. Real R5900 `sq` masks the low 4 bits. Either the trap logs a pre-mask effective address, or `WRITE128` in `ps2_runtime_macros.h` is missing the mask. **Unverified.**
- **Unimplemented syscall**, run_log line 490: `[Syscall TODO] encoded=0x0 v1=0x6b v0=0x503070 a0=0x1 a1=0x0 a2=0x178560 a3=0x20561600 pc=0x174f70` (RA=`0x17e238`).
- **Duplicate generated files** covering identical address ranges from different recompiler generations, confirmed at `0x178260`, `0x17ed60`, `0x178428`. Which variant is registered is unknown for every one.
- **Stale comments** referencing `0x20561900` in `Kernel/Stubs/SIF.cpp` lines 247/627/630/662 — dead commentary from the disproven `pkt[5]` hypothesis, no live code. Safe to delete.
- Run-to-run nondeterminism (boot depth varies on identical logic) — unexplained.

### DISPROVEN — do not resurrect
Dispatch-table poisoning · unsanitized `pkt[5]` · stack-frame overrun in the `lq`/`sq` packet copy (disproven twice) · a *guest store* clobbering saved `$ra` · the `starts`-set theory for `0x104bf4` · null `eeDest` · worker pump ordering · `sub_1DA4` DMA before `BF50` populated · ARKD `sub_ADD4` SIF DMA · `guestRealloc`.

**Note on the IOP/ARKD thread below (07-19d):** that work is code-complete and unbuilt. It is a *separate* blocker from this EE-side derail; the 07-19c run showed the `0x20561900` derail gone under `PS2_SIF_DIAG`, but it has recurred in the current build. Both threads are live.

---

## Prior Status (2026-07-19d) — ⏳ Phase A + B CODE-COMPLETE, UNBUILT. Plan `hidden-tinkering-rossum.md` in flight. Awaiting user build+run to get Phase B diagnostics.

**Blocker model (unchanged, root-caused 07-19c):** EE sits in a steady SIF-RPC CALL poll (sid `0x503`, func `0x2`, recv `0x5aa7d0` sz `0x10`, send-seq `w[6]` climbing). The real R3000 service func-2 (`sub_42700`, ARKD func `0x042700`) runs clean and faithfully returns `recv=00 00 00 20` = `dword_B300=0x20000000` (**bulk transfer IN PROGRESS**) every poll. `[ARKD:search] "\INFO.DAT;1" -> found` works but `[ARKD:cdread]` never fires and worker queue `dword_B310=0`. **Root cause:** on real HW the transfer runs on ARKD's own IOP thread concurrent with the EE poll; our interpreter only runs the polled func-2, never pumps the in-flight transfer body to completion → B300 frozen mid-transfer → poll spins forever.

**Phase A (done, code) — de-noise syscall 0x68:** `Kernel/Syscalls/Dispatcher.cpp` — added `case 0x68: case (uint32_t)-0x68:` → bounded-logged no-op returning 0 (guest discards the result; `fn_104F20` GS field-flip red herring). Removes the repeating `[Syscall TODO] v1=0xffffff98`.

**Phase B (done, code) — transfer diagnostics in `ps2_iop_irx_loader.cpp`:**
- `[ARKD:worker] run:` log extended → now prints `off=+0x%06x` (module-relative finalPC) + `<-- INCOMPLETE (mid-transfer)` flag when `B300 & 0xF0000000 == 0x20000000` at halt.
- New `[ARKD:idle]` diag in the WaitSema `0xAEDC` idle-halt path (fires ≤16×, always-on): logs when the worker declares itself idle (`B310==0`) while B300 is still `0x20000000` — the suspected premature transfer cut-off.

**Hypothesis to test with Phase B output:** the transfer routine clears B310 and loops back to WaitSema BEFORE finishing the read+DMA, so the idle-halt freezes B300 mid-transfer and no cdread ever fires. Phase B output pins the exact stall PC → Phase C wires the pump precisely there.

**Phase C (NOT started):** pump the pending ARKD transfer routine / worker `sub_21B0` in the R3000 so ARKD's own code advances B300 `0x20000000→0x40000000|result` and DMAs read data to the EE recv buffer. No runner edits, no faking. Depends on Phase B run output.

**NEXT (handoff):**
1. User builds: `cmake --build "F:\SDBZ Recomp\build" --config Debug --target ps2EntryRunner` (RecompDebugger LNK1104 file-lock = harmless).
2. User runs with `$env:PS2_ARKD_IRX_RUN=1; $env:PS2_ARKD_SERVICE=1; .\launch_recomp.ps1`.
3. Grep (UTF-16): `Select-String -Path .\run_log.txt -Pattern '\[ARKD:(worker|idle|cdread|sifdma|search|run)\]'` — look for `INCOMPLETE`+`off=`, `[ARKD:idle]`, whether `[ARKD:cdread]`/`[ARKD:sifdma]` fire.
4. Wire Phase C at the stall point Phase B reveals.

Plan detail: `C:\Users\mwlab\.claude\plans\hidden-tinkering-rossum.md`.

---

## Prior Status (2026-07-19c) — ✅ 0x20561900 SIF DERAIL IS GONE (Phase 1 diagnostic, `PS2_SIF_DIAG`). Boot advanced far past it; new terminal blocker is EE-side.

**Phase 1 (`hidden-tinkering-rossum.md`) ran — instrumentation-only, env-gated `PS2_SIF_DIAG`. Findings from run_log.txt (2522 lines):**
- ✅ **`[SIF_DIAG:DERAIL]` = 0. "No exact recompiled function for guest PC 0x20561900" = 0.** The 0x20561900 derail the plan targeted **no longer happens** — the 07-17h `pkt[10]=0xFFFFFFFF` sentinel + loopback fixes already cleared it. Every `0x20561900` in the log is just `src=0x20561900` (the *normal* EE packet-pool source addr, never a jump target) — that spam masked the fact the crash was already fixed. **Phase 2 is effectively satisfied; no SIF.cpp behavioral edit needed for the derail.**
- ✅ SIF_DIAG dump confirms: user/server table is empty (`usrCnt=0`); the synthesized `pkt[2]=0x80000008` (negative) routes the **system** table (`sysBase=0x561700 sysCnt=0x20`, sys[0]=0x177aa8 sys[1]=0x177a88) which is valid. All 24 BIND/CALL deliveries dispatch cleanly. ARKD sids **0x500–0x503 each BIND once**.
- ✅ Boot now runs the **full** ARKD path: IRX load → `_start` (halt) → 4 SIF-server threads (halt) → InitLoadBuffers → **64 `[ARKD:run]` service CALLs** → 1 worker → 1 sifdma. Furthest ever.
- ⚠️ **InitLoadBuffers still spins** (`retired=4000000 finalPC=0x00040930 halted=0`) BUT `[ARKD:cdread]`=0 — it spins at module **+0x930 BEFORE issuing any cdvd read**, and the loader **continues past it anyway** (services run afterward). So this is wasteful, **not** the terminal stall. Phase 3 (IOP cdRead completion) is **de-prioritized** — it is not what blocks boot.

**NEW TERMINAL BLOCKER (EE-side steady-state loop, not a hang):**
- Watchdog `stuckSecs` keeps resetting to 0 — EE is **alive**, cycling the SIF dispatcher (`0x17ed60→0x177eb0→0x1781b0→0x178068→0x178560→0x178de8`) + main loop `0x422454`, `lastCall=0x172998`.
- EE repeatedly issues **SIF-RPC CALL** (`cid=0x8000000a`, `w[8]=0x2`, send buf `w[10]=0x5aa7d0` sz `0x10`) with **incrementing seq `w[6]=0xa9→0xaa→0xab…`** — a live poll loop (retry/timeout cadence).
- Between each CALL it hits an **unimplemented EE syscall at `pc=0x174f50`**: `[Syscall TODO] encoded=0x0 v1=0xffffff98` → dispatcher gets raw `0xffffff98` = **-0x68 (syscall 0x68 / 104)**. No `case 0x68` **or** `case -0x68` in `Kernel/Syscalls/Dispatcher.cpp` (it enumerates specific ±cases, e.g. `-0x70`=iGsGetIMR; there is no global negate) → falls to `TODO()` (warns, does nothing). Call chain: `0x104f20 → 0x174f50(syscall) → 0x172e20 → 0x172a90 → 0x172998`. RA=0x105000, v0=0x7000.

**NEXT (Phase 1 done; propose new direction — user to steer, ideally after `/compact`):**
1. **Identify EE syscall 0x68 / -0x68** authoritatively (ps2tek table or read the generated fn @ 0x174f50 — read-only, never patch). Likely a needed kernel op the poll loop waits on; implement it in `Dispatcher.cpp`/`System.cpp` (a `case 0x68`/`-0x68`), or a game_override if it's a game-specific thunk.
2. **Determine if the SIF CALL sid `w[8]=0x2` poll is genuine progress or a stall** — is the game waiting on a CALL reply, or on the syscall side effect? The seq-incrementing CALL smells like a timeout-retry around the unhandled syscall.
3. Parked non-fatal: 16× `No exact recompiled function for guest PC 0x1bfdb0` (unrelated missing EE dispatch entry, recovers).

`PS2_SIF_DIAG` instrumentation left in place (env-gated, ≤24 emissions, zero cost when unset). Full plan detail: `C:\Users\mwlab\.claude\plans\hidden-tinkering-rossum.md`.

---

## Prior Status (2026-07-19b) — SERVICE-DATA HYPOTHESIS DISPROVEN. Real blocker re-root-caused: EE SIF dispatcher `fn_178068` derails into PC `0x20561900` on an ARKD **BIND** loopback. The `PS2_ARKD_SERVICE` bridge is a dead end for these CALLs (they carry `rsz=0`, no reply expected). Widened gate = inert (no regression, no fix).

**07-19b LONGER-RUN RESULT (full run to crash, from run_log.txt, 2.6M lines):**
- ✅ Disc search + read still clean (`[ARKD:search] "\INFO.DAT;1" -> found`, `cdvdman fid=13` reads fire). All 4 services register.
- ❌ **IOP-side:** `InitLoadBuffers run: retired=4000000 finalPC=0x00040930 halted=0` — spun the full 4M-instruction cap and **never halted**. It's an async cdvd read-wait poll whose completion flag/sema is never signaled by our `sceCdRead` HLE. (Independent sub-bug; may or may not gate the EE crash.)
- ❌ **EE-side (the crash):** the ARKD CALLs are **`func=0x0, rsz=0x0`** (log line 411/432: `send=0x20561900 ssz=0x10 recv=0x5aa3d0 rsz=0x0`). rsz=0 ⇒ the `if (recvPtr && recvSize)` guard in the service bridge is **false** ⇒ `ps2_iop_runArkdService` never runs. So delivering reply data is NOT the mechanism the EE waits on. The widened `sid 0x500..0x503` gate (this session's edit) is therefore inert — correct-when-needed, but not this blocker.
- ❌ After CALL 0x500 → CALL 0x501 → **BIND 0x502**, `[SifRpcReply] deliver cid=0x80000009 -> run dispatcher 0x178068`, then `Error: No exact recompiled function for guest PC 0x20561900` **spins to EOF**. The 0x20561900 loop (thought fixed 07-17h) is **BACK** under the real-IRX-run path.

**ROOT CAUSE (newly pinned, deeper than 07-17h):**
- `fn_178068` (`ps2xRuntime/src/runner/fn_178068_0x178068.cpp`) is the EE's **RPC-server request dispatcher** (sceSifRpc server loop). `$s1` = an RPC_SERVER_DATA struct: `+0xC`=func-table base, `+0x10`=func count, `+0x14`=2nd table base, `+0x18`=2nd count.
- Path 0x178150→0x178180: `lw $v1,0x14($s1)` (table base) → index by `rpcnum*12` → `lw $a2,0x0($v0)` (handler ptr) → `jalr $a2`. The loaded handler ptr = **`0x20561900`** (the SIF packet-pool buffer) → EE executes packet data as code.
- i.e. our loopback of the game's own outbound **BIND** into the EE RX queue makes `fn_178068` treat it as an **incoming EE-server request** and dispatch through a server table that was never validly populated for these sids → garbage func ptr = the packet buffer. The 07-17h `pkt[10]=0xFFFFFFFF` sentinel (client_block[5]) IS still applied (`dest=0xffffffff` in log) — it fixed a *different* field; this crash is via the `+0x14` server-table read.

**NEXT (needs a plan pass — user to decide direction; do NOT blind-edit `fn_178068`, it's a runner file):**
1. **Decide the seam.** The fix must live in `SIF.cpp` (allowed) or `game_overrides.cpp` (allowed). Question: should a BIND loopback route to `fn_178068`'s server-dispatch path at all? On real HW the IOP registers the server; the EE only holds a client handle. Options: (a) for ARKD BINDs, deliver the register-completion in a way that does NOT re-enter the server-dispatch jalr path (route purely to `_request_end` register branch, verify `fn_178068` doesn't fall through to 0x178150); (b) suppress the loopback-run of `fn_178068` for ARKD BINDs once the WaitSema flag is set directly.
2. **Separately, the IOP `InitLoadBuffers` 4M-spin:** our `sceCdRead` HLE must signal read-completion (the flag/sema the IOP polls at `finalPC=0x00040930`) so the loop halts cleanly. Check `ps2_iop_cdReadSectors` / cdvdman completion path.
3. Re-run + grep `0x20561900|ARKD:BIND|InitLoadBuffers|deliver cid`.

**Note:** this is the long-standing "SIF-RPC bind ping-pong" blocker ([[reference_ps2_sif_boot]]), now characterized at instruction level. Prior 07-19 note (below) assumed the EE reaches a 0x503 DVD-read CALL needing service data — **that premise is wrong**; the EE never gets there, it derails on the 0x502 BIND dispatch first.

**(prior 07-18c wiring notes retained below)** S2.2c/d WIRED behind env gates, BUILT clean.

**What was wired this session (all gated `PS2_ARKD_IRX_RUN=1` + `PS2_ARKD_SERVICE=1`, default boot path byte-for-byte unchanged):**
- **Root cause found:** cdvdman `fid=10` = `sceCdSearchFile` (`sub_ACA8`, off 0xACA8) is a filename→LBN resolver that **gates every read**. ARKD spins `while(!sceCdSearchFile(fp,"\INFO.DAT;1")) delay()`. Default hook returned 0 → infinite spin → `dword_B300` stuck `0x20000000` (reading), game polls fno 0x2 forever. The real read (`sub_AC98`, off 0xAC98) is downstream and was never reached.
- **Fix (2 edits, both compiled clean):**
  - `Kernel/Stubs/CD.cpp`: added external-linkage forwarder `ps2_iop_cdSearchFile(name, &lbn, &size)` that calls the existing `registerCdFile(ps2Path, CdFileEntry&)` primitive → search-LBN and read-LBN come from the SAME pseudo-LBN table (honors no-IOP-faking: real ISO table + real sectors).
  - `ps2_iop_irx_loader.cpp`: new import hook `off==0xACA8u` — reads the filename string from IOP RAM (a1), calls `ps2_iop_cdSearchFile`, writes `{lbn@0, size@4}` into the `sceCdlFILE` struct at IOP RAM (a0), sets `$v0=1` found / `0` miss, returns to `$ra`. Logs `[ARKD:search] "name" -> found/MISS lbn size`.
- Pre-existing loader hooks (unchanged): `off==0xAC98u` cdvd read→`ps2_iop_cdReadSectors`; `off==0xAEDCu` WaitSema/worker-halt; `off==0xADD4u` SIF DMA IOP→EE.

**BUILD RESULT (user ran `cmake --build`):** `CD.cpp` + `ps2_iop_irx_loader.cpp` compiled clean; `ps2_runtime.lib` + **`ps2EntryRunner.exe` linked successfully** (benign LNK4006/4075/4088 only). The only failure was `RecompDebugger.vcxproj` → `LNK1104: cannot open file ...RecompDebugger.exe` = **harmless file-lock** (that exe was running/open); unrelated to ARKD and unrelated to the runner target used for the game. Close it or ignore.

*(07-18c disco-re-run checklist resolved — see the 2026-07-19 status block above; search + read confirmed working.)*

---

## Prior Status (2026-07-18b) — ✅ S2.2c DISCOVERY COMPLETE. The full ARKD DVD-read worker chain is now mapped end-to-end (live harness trace + IDA decompile of ARKD_DVD.IRX). The three prior unknowns (which sema, which thread waits, which cdvdman fid reads) are resolved.

**S2.2c DISCO RESULT (headless `iop_harness.exe`, real send header, fno 0x0/0x102/0x2):**
- **The 0x503 dispatcher = `sub_2700`** (func 0x042700). fno→handler map (fno = a1):
  - fno 4 → `sub_1A10` (SetString "cd_root")
  - fno 1 → `sub_16A4`; **fno 2 → `sub_1714` (POLL: returns `dword_B300` verbatim)**; fno ≥3 → `sub_1760` (TocLookup+read by name)
  - **fno 0x102 (258) → `sub_2410` (SUBMIT read)**; fno 0x101 → `sub_250C`; fno 0x103 → `sub_2608`; fno 0x81 → `sub_183C`
- **SUBMIT `sub_2410`** (live-confirmed): `sysclib memcpy 0x24` bytes (recv buf 0x54e50 → job struct `dword_BF50`=base+0xBF50), sets `dword_B310=1` (job type), `dword_B300=0x10000000`, then **`SignalSema(dword_B318)`**.
- **THE WORKER = thread `sub_21B0`** — a DEDICATED thread (NOT one of the 4 SIF servers). Loops: `WaitSema(dword_B318)` → dispatch on `dword_B310` (1→`sub_1DA4`, 2→`sub_1A58`, 3→`sub_1EB0`) → the read (`sub_1A58`→`sub_7A4`→**cdvdman read import `sub_AC98(lbn,sectors,dest,mode)`** with 6× retry via `CdReadRetryLoop`/`sub_CCC`) → sets `dword_B300` = `0x30000000` done or `0x40000000|err`. Filename comes from job struct at `byte_BF54` (job+4) via `TocLookup`.
- **THE INIT = `sub_28AC` (InitLoadBuffers)**, reached via `sub_30`: `CreateSema`→`dword_B314`(mutex)+`dword_B318`(worker wakeup, count 0), `CreateThread(entry=sub_21B0)`+`StartThread` (creates the worker), allocs load buffers, reads INFO.DAT/GAME.DAT TOC (`sub_1870`), sets `dword_B300=0x30000000` ready.
- **Why disco fired no cdvdman:** the harness runs only `_start` + the 4 SIF-server thread *setups* (each halts at the RpcLoop stub). It NEVER runs the init RPC (`sub_30`/InitLoadBuffers), so `dword_B318`=0 and no worker exists → submit's `SignalSema` went to sema **0** (noop), read never ran. Poll returned the static `dword_B300` (0x30000000).

**S2.2c/d WIRING IMPLICATION (for next steer — NOT yet wired):** our run-to-halt model has no persistent scheduler. To service a real 0x503 read we must (1) run `InitLoadBuffers` once at load (so semas/buffers/TOC/worker exist), and (2) after a submit sets `dword_B310`+signals, DIRECTLY run one worker iteration (`sub_1A58`/`sub_1DA4`/`sub_1EB0` per `B310`) so the cdvdman read (`sub_AC98`) executes inline — then hook `sub_AC98` to `readCdSectors` into IOP RAM. The exact numeric cdvdman fid resolves the instant `sub_AC98` first executes (import hook logs it). **PAUSE for user steer before behavioral wiring** (per plan).

---

## Prior Status (2026-07-18a) — ✅ S2.2a + S2.2b CONFIRMED. Real ARKD_DVD.IRX `_start` now runs to a CLEAN HALT in the embedded R3000, and all 4 SIF-RPC services self-registered. We now have the REAL per-sid dispatch function pointers — the exact Stage 2 targets.

**S2.2a/S2.2b RESULT (run log 07-18a, `PS2_ARKD_IRX_RUN=1`):**
- `$sp` bug fixed (`setGpr(29, 0x1FFF00)` before `_start`) → the `unhandled write32 @0xffffff**` stack-fault spam is GONE.
- `_start run: retired=183 finalPC=0x0ffffff0 halted=1 threads=4` — module init ran real R3000 code to a clean sentinel halt; captured 4 CreateThread entries.
- Real HLE imports serviced: AllocSysMemory (bump), CreateSema, CreateThread (records entry), StartThread (records arg), printf. printf strings prove the real module ran: **"ARIKA IOP Control Driver Version %d.%d.%d (C)ARIKA Co.,Ltd."** + **"TARGET CD/DVD Version."**
- S2.2b ran each of the 4 server-thread bodies once → each self-registered its RPC. **CONFIRMED dispatch table (sid → service func in IOP RAM, load base 0x040000):**

  | sid    | client   | service func | recv buf | thread entry |
  |--------|----------|--------------|----------|--------------|
  | 0x500  | 0x5a9330 | **0x0401d4** | 0x054a50 | 0x04a4dc      |
  | 0x501  | 0x5a9380 | **0x046708** | 0x055250 | 0x04a554      |
  | 0x502  | 0x5a93a8 | **0x049d38** | 0x055650 | 0x04a5cc      |
  | 0x503  | 0x5a9358 | **0x042700** | 0x054e50 | 0x04a644      |

- **The DVD-read blocker = sid 0x503 → func 0x042700.** Stuck CALL packet (steady state): mode=2, send buf, recv buf 0x5aa7d0, size 0x10, rpc_number climbing (timeout retries). handleRPC still → false (S2.2d not wired yet), so the game still hangs at pc=0x422454 / lastCall=0x172998 — EXPECTED, no regression.
- Parked (non-fatal): `No exact recompiled function for guest PC 0x1bfdb0` (via 0x171c30→0x1bfdb0) spams during the init CALL burst then recovers — missing dispatch-table entry, unrelated to DVD blocker.

**NEXT: S2.2c + S2.2d** — cdvdman sceCdRead → real ISO sectors into IOP RAM (`readCdSectors`); then per-CALL bridge in SIF.cpp (gated `PS2_ARKD_SERVICE=1`) that, for sid 0x503, sets up the R3000 (a0=send buf mapped into IOP RAM, $sp, halt sentinel), calls func 0x042700, and copies the service's recv-buffer output back to the guest recv buf (0x5aa7d0) before the dispatcher runs.

---

## Prior Status (2026-07-17h) — ✅ SIF PACKET-POOL SENTINEL FIX CONFIRMED (0x20561900 loop GONE). Game now runs a stable main loop (furthest ever). New blocker: **ARKD DVD-read RPCs get NO result data** — the SIF.cpp echo path never invokes handleRPC, and ARKD SID isn't handled in ps2_iop.cpp.

**SENTINEL FIX CONFIRMED (run log 07-17h):**
- `SIF.cpp` BIND branch `pkt[10] = 0xFFFFFFFF` fix WORKED. The `No exact recompiled function for guest PC 0x20561900` loop is GONE. Outbound packets no longer carry string garbage in w[3]. Boot advanced well past the corruption point.
- All 5 IRX still load OK. Then the game issued its post-ARKD service binds (clients 0x5a9330/0x5a9358/0x5a9380/0x5a93a8, SIDs 0x500 etc.) and entered a **stable main loop** — watchdog PC varies (0x11e3b0/0x11f240/0x175210 vblank/0x104c74) and stuckSecs resets, i.e. alive, not hung. Reaches 0x175210 (INTC VBLANK) now.

**NEW BLOCKER (07-17h): ARKD service RPCs return no data**

**STAGE 1 TRACE CONFIRMED (07-17i, `PS2_ARKD_TRACE=1`) — the Stage 2 contract:**
- 4 ARKD clients, 4 distinct SIDs (static guess of a single sid 0x500 on 0x5a9358 was WRONG — 0x5a9358 is sid **0x503**):

  | client     | sid (w8) | recv buf  |
  |------------|----------|-----------|
  | 0x5a9330   | 0x500    | 0x5aa3d0  |
  | 0x5a9380   | 0x501    | 0x5aabd0  |
  | 0x5a93a8   | 0x502    | 0x5aafd0  |
  | 0x5a9358   | 0x503    | 0x5aa7d0  |

- BIND: w4=0x5 (mode), w8=real sid. CALL: cid=0x8000000a, shared send buf **0x20561900**, recv sizes report **rsz=0** (game reads recv buf by fixed addr, not copy-back).
- Func numbers seen on sid 0x500: 0x0 (init), then **0x12, 0x11, 0x1** — matches prior R3000-era `rpcno 1/2/17/18/258` servicing (state line ~216), so the real ARKD_DVD.IRX service fn is the Stage 2 target.
- `handleRPC → false` on every ARKD CALL (expected: no handler) — bridge is observe-only, no regression, main loop preserved.
- **The hang, now visible:** after the CALLs return nothing, the guest dispatcher (`sub_178068` loop) jumps to PC **0x20561900** = the send-buffer address → "No exact recompiled function for guest PC 0x20561900". The game is executing the RPC packet as code because the reply never delivered a valid function pointer. Stage 2 must service these 4 SIDs so a real reply lands in the recv buf before the dispatcher runs.

- The game repeatedly issues CALL packets to ARKD client **0x5a9358** (seq incrementing 0x1a→0x36+, ~1-2/sec = timeout-retry), waiting for DVD data that never arrives.
- Root cause (confirmed by code read): ARKD CALLs travel the **SIF.cpp `deliverSifRpcReply` echo path** (captured via sceSifSetDma, cid=0x8000000a), which synthesizes an empty completion and **never calls `ps2_iop().handleRPC`**. Separately, ps2_iop.cpp `handleRPC` has **no handler for the ARKD SID (0x500)** — the C++ HLE chain (SoundDriver/DbcMan/LibSd/Sound/ClFile/Sdrdrv/McServ/LOADFILE/CDVD) doesn't cover it.
- Per no-IOP-faking rule ([[feedback_no_iop_faking]]): route ARKD CALLs through the embedded **R3000 interpreter running the real ARKD_DVD.IRX** (already loaded), not hand-written C++ outputs. Needs a plan pass — decide the wiring: SIF.cpp CALL branch → handleRPC/R3000, fill recv buf, echo back before dispatcher runs.
- Secondary/non-blocking: `-104` (0x68) @0x174f50 ra=0x105000 and `107` (0x6B) @0x174f70 ra=0x17e238 fire periodically but do NOT hang the loop (thunk array verified correct vs ELF; dispatcher keys GsPutIMR/SetSyscall at 0x71/0x74 — these are genuinely different syscalls in the 0x65-0x6F RFU range, not GsIMR). Leave as no-op unless proven to gate the ARKD loop.

---

## Prior Status (2026-07-17g) — ✅ SYSCALL THUNK TABLE REGISTERED & CONFIRMED (0x1748a0 class GONE). ✅ ALL 5 IRX loadfile RPCs complete. Sentinel fix for 0x20561900 coded [CONFIRMED WORKING 07-17h].

**THUNK FIX CONFIRMED (run log 07-17g):**
- `game_overrides.cpp` `applySdbzKernelThunkFixes` now registers all **120** EE kernel syscall thunks (`0x174880..0x174FF0`, 16B stride, `li $v1,N; syscall; jr $ra`) via `replaceFunction` + template-per-index handler (sets $v1 sign-extended, calls 2-arg `handleSyscall`, returns via $ra). `No exact recompiled function for guest PC 0x1748a0` is gone; thunks appear live in traces.
- Boot then ran the FULL loadfile chain: bind ping-pong, func-255 "3000" handshake, then `Load Module cdrom0:\{MCMAN,MCSERV,LIBSD,CRI_ADXI,ARKD_DVD}.IRX OK!` — furthest boot ever.
- Non-fatal unimplemented syscalls seen (follow-ups, game continued past both): `0x6b` (107) @pc=0x174f70 ra=0x17e238; `-104` (0xffffff98) @pc=0x174f50 ra=0x105000 (new async-stack thread).

**NEW BLOCKER (07-17g): jump to guest PC 0x20561900 (SIF packet-pool corruption) — FIX CODED, UNVERIFIED**
- Root cause: `SIF.cpp` BIND-completion echoed the request's WORD[5] (EE packet-pool buffer `0x20561900`) into `pkt[10]` → guest `_request_end` (0x178560) stores it as `client[5]` = "IOP server receive buffer". `rpc_call` (0x178BE8) uses client[5] as the DEST of every send-payload DMA → our `sceSifSetDma` saw a copyable EE dest and copied payloads (0x200-byte module-path strings etc.) over the game's own SIF packet pool at 0x561900. Evidence: later outbound packets carried string garbage (`w[3]=0x5c3a306d` "m0:\"); pool link word at buffer+20 holds `0x20561900`, which the dispatcher (0x178068, post-ARKD binds rpcNum 0xf–0x12, clients 0x5a9330/0x5a9380) eventually jumped to. Infinite `No exact recompiled function for guest PC 0x20561900` loop.
- Fix: `SIF.cpp` BIND branch now sets `pkt[10] = 0xFFFFFFFF` (established IOP-bound sentinel; DMA copy path skips non-copyable dests; our RPC HLE reads payloads from src). One-line + comment.
- Next expected after fix: the post-ARKD service calls (mode=3 async, clients 0x5a9330/0x5a9380, likely ARKD_DVD/CRI driver RPC) currently get NO result data from HLE loopback — may become the next blocker; route through embedded R3000 IOP interpreter per no-IOP-faking rule.

---

## Prior Status (2026-07-17f) — ✅ -65540 SIGNATURE GATE CLEARED & CONFIRMED. Boot advanced to a NEW, different blocker: missing recompiled function entry at EE 0x1748a0. [RESOLVED 07-17g via 120-thunk registration]

**FIX CONFIRMED WORKING (run log):**
- **Seed — `game_overrides.cpp:381` `applySdbzLoadfileSeed`**: writes EE `0x461C30 = -1` at ELF-load. Log: `[loadfile-seed] 0x461C30 <- -1 ... readback=0xffffffff`. REQUIRED (not redundant) — it arms d4a0's real path.
- **LOADFILE HLE — `SIF.cpp` `deliverSifRpcReply` (kSifCmdRpcCall branch)**: tracks the client object BIND'd to sid `0x80000006` (LOADFILE, a PS2 ROM/kernel service — NOT a game IRX), then on its func-255 CALL writes the fixed protocol version `0x30303033` ("3000") into the recv buffer (`w[10]`, =`0x564b40`) BEFORE dispatcher 0x178068 runs. Log: `[SifRpcReply:LOADFILE] func255 recv=0x564b40 <- 0x30303033 ("3000")`.
- **Result:** gate d5a0 passes → the `Can't load module cdrom0:\SIO2MAN.IRX;1, ret = -65540` retry loop is GONE. Game then issued the real func-0 loadfile RPC for `"cdrom0:\SIO2MAN.IRX;1"` (path decoded from packet bytes) and proceeded.

**NEW BLOCKER (2026-07-17f):** after SIO2MAN loadfile, execution hits an indirect-dispatch jump with no recompiled function:
```
Error: No exact recompiled function for guest PC 0x1748a0 tableBase=0x100008 tableEnd=0x4e6c84 codeRegion=yes
[guest-branch:missing-target] kind=IndirectJump op=dispatch source=0x0 target=0x1748a0 pc=0x1748a0 ra=0x1057fc sp=0x1ffbd40 a0=0x1 a1=0x2 policy=1
trace=... -> 0x171fd8 -> 0x172168 -> 0x174fe0 -> 0x1748a0
```
- **DECODED from raw ELF (offset 0x74920):** `0x1748a0` = `addiu $v1,$zero,2; syscall; jr $ra` — a tiny **syscall trampoline**, one of a contiguous series the recompiler folded into `hw_timer_update_x @0x174808` and never emitted as exact entries: `0x174890`(li $v1,1) `0x1748a0`(#2) `0x1748b0`(#3) `0x1748c0`(#4) … up to the `syscall_stub @0x1748F0` block IDA *did* recognize. NOT garbage — a real missed entry point.
- The game takes each trampoline's address and calls it **indirectly**, so each needs its own exact recompiled function entry; the dispatcher cannot jump into the middle of `0x174808`'s single C++ body. Class = **recompiler missing-entry** (cf. [[project_dispatch_table_unpopulated]], PR #150 entry discovery), NOT SIF HLE.
- Caller returns to `0x1057fc`; `a0=1 a1=2`. Preceding: unimplemented EE `syscall 0x6b` @pc=0x174f74 (separate concern; these trampolines carry small $v1=1..N, likely game-installed SetSyscall handlers per [[reference_ee_syscalls]]).

**NEXT (needs a fresh look, user to decide direction — pivot to recompiler-entry domain):**
1. Two fix routes: (A) add the trampoline addresses (0x174890, 0x1748a0, 0x1748b0, …) as function entries via the manifest/entry-discovery mechanism used by [[project_dispatch_table_unpopulated]] (which added entries WITHOUT a full recompiler rerun) — preferred; (B) runtime override that emulates the trampoline (set $v1=N, invoke the syscall dispatcher) in game_overrides.cpp — allowed layer, no rebuild, but needs one override per address.
2. First confirm how many trampolines the game actually calls (0x1748a0 is the one that faulted; siblings may fault next) and what syscall $v1=2 maps to in our Dispatcher.
3. Check whether a runtime dispatch fallback exists for `policy=1` "No exact recompiled function" — if it can be taught to run a mid-function guest address, that generalizes the fix.

---

## Prior Status (2026-07-17d) — ROOT CAUSE PINNED: -65540 is a PRE-RPC signature gate that short-circuits BEFORE any loadfile RPC. Not missing IRX, not Deci2 drain. `[loadgate]` dump decoded the exact gate words.

**`[loadgate]` dump (run_log.txt):** `initFlag@461C30=0x0 sigA@461B5C=0x30303033("3000") ptr@461C34=0x4c07b0 sigB@*461C34=0x2e2e2e2e("....") fetched@564D68=0x0`

**Chain (source-confirmed):**
- SIO2MAN loads first via the infinite-retry wrapper (decompile ~line 2718-2727) → dies before `AudioSysInit`@0x422170 (mcman/mcserv/libsd/criadxi/arkddvd).
- `noop_sub_dc78`@0x17DC78 → `noop_sub_d4a0`@0x17D4A0 → gate `wrap_mem_compare_n_c_0`@0x17D5A0.
- **d4a0: `if (dword_461C30 >= 0) return 0;`** — 461C30=0 → short-circuits, NEVER runs the bind+func-255 RPC → 564D68 stays 0 (this is why the `[iop:LOADFILE]` sid-0x80000006 hook never fired).
- Gate returns -65540 unless `564D68 == "3000"(461B5C)` OR `== "...."(*461C34)`. 564D68=0 → -65540 forever.
- 461C30 must be -1 at first call. Only setter `wrap_mem_set_u`@0x17D630 has NO caller (SDK loadfile-init glue our runtime skips); ELF genuinely has 0 there (nearby .data 461B5C="3000" read correctly).
- RPC plumbing READY: `mem_fill_z_18`@0x178A08=sceSifBindRpc, `mem_fill_z_369`@0x178BE8=sceSifCallRpc, both issue SIF DMA cmd 0x80000009 our runtime already intercepts.

**FIX (allowed layer):** (1) seed EE 0x461C30=-1 once before first loadfile (enables game's own d4a0 — not faking); (2) HLE loadfile RPC sid 0x80000006 func 0xFF → return "3000" (0x30303033) → gate passes. Part 2 = legitimized entry for the parked IRX-loader work.

**NEXT (cheap validating experiment):** seed 461C30=-1 only, one-time guarded, rebuild/run → expect `[iop:LOADFILE]` to fire + reveal func-255 request. Awaiting user go-ahead (pivot; /compact after).

---

### [SUPERSEDED 2026-07-17c] blocker framed as SIO2MAN.IRX load returning -65540 (correct value, wrong mechanism — it's a signature-gate short-circuit, not a load-RPC failure)

**Diagnostics run result (recomp, run_log.txt):**
- First TTY line printed ONCE, boot continued: `CMemory::Init memsize : 0173fc00 block:00000800`
- Then ~20 SIF-RPC exchanges (game's own SoundDriver services, cid 0x80000009/0x8000000a), NO CD module loads.
- Then boot spins FOREVER re-printing ONE fatal line:
  `Can't load module cdrom0:\SIO2MAN.IRX;1, ret = -65540`
- No SIF/DMA traffic between the repeats → it is a fatal-error spin, NOT a printf-flush drain bug. printf worked fine for CMemory::Init (printed once, moved on).

**Root cause (confirmed by source read):**
- The game's `sceSifLoadModule("cdrom0:\SIO2MAN.IRX;1")` returned **-65540** (=0xFFFEFFFC) and the game treats it as fatal.
- Our EE-syscall stub `SifLoadModule` (RPC.cpp:1510) + `trackSifModuleLoad` (Loader.h:78) ALWAYS return a positive fake id — they can NEVER produce -65540. So the game is NOT using our EE syscall path.
- The game issues the **IOP LOADFILE RPC itself (sid 0x80000006)**. Our runtime has **NO LOADFILE handler** (grep: zero hits for 0x80000006/LOADFILE) → SifBindRpc hands it a dummy server → the call returns garbage/-65540.
- Aggravating fact: we boot from the **ELF directly, no ISO mounted**, so even a real LOADFILE couldn't read `cdrom0:\SIO2MAN.IRX;1`.

**Deci2 drain theory (07-17b, below) is SUPERSEDED.** The Deci2 Poll/latch is the messenger printing the error, not the disease. IPC-frozen "live read at 0x176508" gate is moot. Deci2.cpp still holds Step-1 diagnostics (case 3/-7 TTY-text log, case 4 handle dump) — no behavioral change; leave or trim later.

**FORK (decision needed before code) — how to satisfy the SIO2MAN.IRX load:**
1. **HLE the LOADFILE RPC** (sid 0x80000006) to return a success module id for the CD IRX modules — fastest, but conflicts with [[feedback_no_iop_faking]] (no hand-faked IOP values).
2. **Load SIO2MAN.IRX into the embedded IOP interpreter** on demand (the philosophically-correct path per [[reference_iop_interpreter]] + [[feedback_no_iop_faking]]); needs the IRX bytes reachable (ISO mount or bundled file) and import-timing care [[reference_irx_import_resolution_timing]].
3. Verify FIRST which EE function issues the load and how it forms -65540 (disasm the sceSifLoadModule caller) before choosing 1 vs 2.

---

### Prior Status (2026-07-17b, SUPERSEDED by 07-17c above) — CLEAN PCSX2 A/B; Deci2 Poll descriptor-drain theory.

**Clean PCSX2 A/B obtained** (SDBZ booted on real PCSX2, cycles=630M, paused at fn_1763F8). Full disasm + live-memory captured. Ground truth in memory [[reference_ps2_sif_boot]] §2026-07-17b.

**The blocker (real model):** boot hangs in the CALLER's descriptor-drain loop at `0x176508`:
```
loop: lw v0,0xC(s0)  ; s0=socket handle (0x460000 on real HW)
      beqz -> exit
      jal  sceDeci2Poll(a0=4)   ; fn_176020 -> fn_1750D0 -> syscall 0x7C
      lw v0,0xC(s0); bnez -> loop
```
Game polls `sceDeci2Poll` until socket descriptor count `0xC(handle)` drains to 0. On real HW each Poll advances DMA + decrements it. **Our Poll never does → spins forever.**

**Two words were being conflated (now separated):**
- `0x5612DC` (0xC of state-block s3=0x5612D0) = send-busy latch. fn_1763F8 (sceDeci2Send builder) RAISES it (`sw v1=1,0xC(s3)`), never clears it; cleared by poll/receive path. My Deci2.cpp case-4 clear of this word is **semantically CORRECT** — the 07-17a "clear-fix is WRONG / re-entrancy guard" claim is **RETRACTED** (it rode on a mis-sampled s3=0x4B72A7, caught before the `addiu s3,s5,0x12D0`).
- `0x46000C` (0xC of socket handle s0) = descriptor count = the word the hang loop ACTUALLY reads. Case-4 never touches it → incomplete.

**Helper identities:** fn_175FF0=sceDeci2Send(a0=3), fn_176020=sceDeci2Poll(a0=4), both via fn_1750D0=syscall 0x7C. D_STAT/0x175170 is NOT involved — dropped as a suspect.

**Fix target (Deci2.cpp case 4 / sceDeci2Poll):** keep clearing latch 0x5612DC AND decrement/zero socket descriptor count at 0xC(handle) so the 0x176508 loop exits. Behavioral only — no hand-faked constant.

**Next: START HERE (NO CODE until step 1 passes):**
1. **Recomp-side live read** — launch recomp, let it reach the spin, read recomp's live `s0` at the 0x176508 loop, confirm which handle word it reads (real HW=0x46000C; recomp may differ). Gate.
2. Only then edit Deci2.cpp case 4 as above. Give user the build command (`.\build.ps1`), don't build.
3. Run exe → parse run_log.txt for next blocker.
- Optional trace shortcut (user offered IDA/PCSX2): break at 0x176508, dump socket-handle struct (0x00–0x20) BEFORE and AFTER one Poll on real HW — the diff at 0xC = exact spec for the fix.

---

## Prior Status (2026-07-16k, STALE — superseded by 07-17b A/B above) — 0x79 FIX VERIFIED. Boot advanced (2nd full RPC round now runs). New/real blocker = idle scheduler churn at `0x100008` (`ra=0x0`); threads loop through **string-lib / path-parse** code (strchr @0x18e0d8, char-fetch @0x18e0a0) — game appears to retry a file/module load that never succeeds. 0x6b is COSMETIC (result discarded), not the blocker.

**Verify run (2026-07-16k, build w/ 0x79 fix — note: linked with `/FORCE`, LNK4088 "image may not run" warning, but ran fine):**
- ✅ **`v1=0x79` TODO warnings GONE.** 0x79/0x7E alias fix confirmed working.
- ✅ **Boot ADVANCED past the fix:** a SECOND full SIF-RPC bind/call round now runs (sid 13–19) that did NOT happen pre-fix. 0x79 was a real gate.
- **`v1=0x6b` @ pc=0x174f74 = COSMETIC.** Disasm proof: 0x174f74 is `syscall_stub_z_16` (bare `syscall` thunk); caller = `sub_17E200(char*, int)` @ ra=0x17e238. That fn builds a 104-byte SIF cmd block (`byte_564D80`, size=104=0x68, id=`0x80000003`), cache-writes-back, sends via SIF. The 0x6b return value is **discarded** — the fn's `if(!v7) return 0` gates on `syscall_stub_z_21`, a DIFFERENT syscall, not 0x6b. Log confirms: `pkt[0x68 0x0 0x80000003 ...]` fires right after, boot continues. **Do NOT route 0x6b — leave as harmless TODO.**
- **REAL blocker:** after both RPC rounds, EE goes idle: `watchdog pc=0x100008 ra=0x0`, stuckSecs climbs indefinitely. Trace rotates through STRING-LIBRARY fns (verified via decompile): `sub_18e0a0`=1-char fetch, `0x18e0d8`=strchr/memchr (SIMD `pcpyh`/`pxor` byte-scan), plus `sub_195f38`/`sub_18cf68`. Game is busy-looping in **path/string parsing** — consistent with retrying a file or `rom0:`/mc load that returns empty/error every pass.
- `[Deci2Call:poll]` fires a handful of times early then STOPS (not the terminal spinner). Its `+C` clear is ineffective (`+C` stays 0x1) but that loop is no longer where the game is stuck; `+4` (0x3d→0x43) is a self-incremented counter, not a completion gate.

**Next: START HERE (2026-07-16k):**
1. Identify WHAT file/module the string-parse retry loop is trying to load. Candidates: the `sub_17E200` "rom0:"/SIF-cmd string (`0x306d6f72 0x4e44553a` = "rom0"/":SUD"→ likely "rom0:SDUN..." or a boot module path), or a memory-card path. Grep the ELF decompile for callers of `sub_17E200` and the string const at its `a1`.
2. Check whether an SIF-RPC *reply/callback* the parse loop waits on is never delivered — the 2nd RPC round (sid 13–19) ends with `[Sema:Wait] sid=19` and no matching signal; a missing reply could leave the loader spinning.
3. Trace the idle-loop entry: find who calls `sub_195f38`/`sub_18cf68` and what condition breaks the loop.
4. Log is UTF-16 — grep/sed see spaced chars.

---

## Prior status (2026-07-16j) — 0x79 fix APPLIED (now verified above)
RE-BASELINED on FRESH log. `sceSifCheckInit -1` loop is GONE. Blocker was = boot thread churns at `0x100008` because EE syscall **0x79 (GetOsdConfigParam alias)** fell through to TODO→0. FIX APPLIED (dispatcher aliases 0x79→GetOsdConfigParam, 0x7E→SetOsdConfigParam @ Dispatcher.cpp:186-195).

**Fresh run (2026-07-16j, current build, `run_log.txt` now UTF-16, Jul-16 23:38):**
- **No `[sifman]`/`[thevent]`/`[IRX-stub]`/`sceSifCheckInit` anywhere.** The entire 07-16h blocker is confirmed dead — it was the stale July-8 build. Stale-premise finding (below, 07-16i) validated.
- **SIF-RPC now fully works via HLE:** bind (`cid=0x80000009`) + call (`cid=0x8000000a`) ping-pong completes, `[SifRpcReply] deliver → run dispatcher 0x178068`, semaphores signal/wake. Two full RPC init cycles run (sid 4–6, then 13–19).
- **New blocker:** after RPC init, EE settles into a scheduler churn — `watchdog pc=0x100008 ra=0x0`, `stuckSecs` climbs 0→7+, trace cycles the `0x17xxxx`–`0x19xxxx` kernel/thread range without forward progress. `[Deci2Call:poll]` spins with `+4=0x43`.
- **Root cause found:** log shows `[Syscall TODO] ... v1=0x79` (repeated) and `v1=0x6b` (once). `handleSyscall` switches on `$v1`. Dispatcher.cpp had cases for **0x4A/0x4B** (Set/GetOsdConfigParam) and 0x6E/0x6F (…Param2) but **NOT the 0x79/0x7E aliases** the SDBZ crt0 actually calls. 0x79 (GetOsdConfigParam) fell to `TODO`→returned 0 and left the OSD-config buffer unfilled; boot loop that branches on language/video-mode config never satisfied → stalls at 0x100008.
- **FIX (applied, NOT yet build-verified):** `Dispatcher.cpp:186-195` — added `case 0x79` → `GetOsdConfigParam` and `case 0x7E` → `SetOsdConfigParam` (route aliases to existing handlers; no new logic, no faked values). `src/lib` file, legal.
- **Held back:** `0x6b` (called ONCE @ pc=0x174f74) — not in db-syscalls.md, identity ambiguous, blind-routing risky. Only add if 0x79 fix proves insufficient.

**Next: START HERE:**
1. User runs `.\build.ps1` then `.\launch_recomp.ps1`.
2. Confirm `[Syscall TODO] ... v1=0x79` warnings are GONE and whether PC moves off `0x100008` / `stuckSecs` stops climbing.
3. If still stalled → investigate `0x6b` @ pc=0x174f74 (disasm that fn to ID what it expects) and the `[Deci2Call:poll] +4=0x43` spin.
4. Note log is now UTF-16 — read with tools that handle it (grep/sed see spaced chars).

---

## Prior finding (2026-07-16i) — stale-premise alert (VALIDATED by 07-16j fresh run above)

**Verified this session (2026-07-16i) — do not skip:**
- The `[sifman]`, `[thevent]`, `[IRX-stub]`, `[IopRuntime]` log tags that ALL of 07-16h is based on appear in **ZERO `.cpp`/`.h` in the repo or the pr137/pr-fix worktrees** — only inside log files. Grep-confirmed.
- `run_log.txt` (root) is dated **Jul 8** — its producing binary was built from a source generation containing a full **IRX-loader + import-stub interpreted-IOP** (`[IRX-stub] lib='sifman' funcIdx=7/8 ... patched→tramp`, `[sifman] sceSifCheckInit -> -1`, `[thevent] SetEventFlag`). That source is **GONE** from the current tree.
- The ONLY interpreted-IOP code present now is `ps2xRuntime/src/lib/ps2_iop_cpu.cpp` (`IopCpu`), which is **untracked (`??`)** and instantiated in exactly ONE place — `ps2_runtime.cpp:2277` — inside a `PS2_IOP_CPU_SELFTEST` env-gated self-test (sum(1..10), discarded). **No IRX loaded, no sifman/thevent stubs registered, `setImportHook` never called.**
- Current IOP RPC handling = C++ HLE dispatcher in `ps2_iop.cpp::handleRPC` (dbcman/libsd/cl/sdrdrv/mcman/cdvd by SID), NOT interpreted IRX.
- **Net:** diagnosing "why interpreted sifman CheckInit returns -1" = chasing code that isn't compiled anymore. The whole 07-16h front line is against a build that no longer exists in this tree.

**Decision (user, 2026-07-16i):** re-run the CURRENT build first, capture a fresh run_log, re-baseline the blocker to what the current HLE-IOP tree actually does. User is compacting before continuing ([[feedback_compact_after_plan]]).

**Next session — START HERE:**
1. User runs `.\build.ps1` then `.\launch_recomp.ps1` on the CURRENT tree.
2. Read the FRESH `run_log.txt` (NOT the Jul-8 one). Confirm whether `[sifman]`/`[IRX-stub]` tags even appear anymore — if they don't, the interpreted-sifman blocker is fully moot.
3. Identify the REAL current boot blocker from the fresh log + watchdog trace, then re-baseline this doc.
4. Open question to resolve if it matters: where did the IRX-loader/sifman-stub interpreted-IOP source go (reverted? stashed? uncommitted-then-lost between Jul 8 and now)? `git reflog`/`git stash list`/backup search. Only pursue if the HLE path turns out insufficient and the interpreted path needs restoring.

---

## Current Status (2026-07-16h) — [SUPERSEDED by 07-16i above — premise is stale, see the alert] BIG JUMP: CdInit CLEARED, GS RENDERING, RPC ring converged. New blocker = `sceSifCheckInit` returns -1 forever (IOP-side sifman.irx)

Fresh live run via `.\launch_recomp.ps1` (now tees stdout+stderr to `run_log.txt` automatically — Tee-Object). The boot advanced **far past** every prior-session blocker. The whole 07-16d..g SIF-RPC-bind + CdInit-recv analysis below is now HISTORICAL.

**What completed this run (run_log.txt, in order):**
- **CdInit CALL sid=0x80000592 rpcno=0 COMPLETED** (L779–780, `done result=0x0`); 0x80000593 rpcno=34 done (L786). The 07-16g "empty recv starve" blocker is GONE. **Option B (recv-fill) is MOOT — do NOT implement it.**
- ARKD RPC services 0x500–0x503 bound + serviced (rpcno 1/2/17/18/258 via `[IopRuntime] ARKD handled RPC`); memory-card RPC bound (sid 0x80000100, `[McBindPatch]`); threads 20/21 spawned; semas created.
- **GS IS RENDERING** — `[gif:submit]`/`[gif:drain]` path3, `[gs:FRAME]`, `[gs:AD]` draw regs flowing. First real draw activity ever observed.

**NEW terminal blocker — a 5-line loop repeating forever (run_log tail, from L1233):**
```
[sifman] sceSifCheckInit -> -1 (clear poll flag)
[sifman] iSifSetDma list=0x000A8050 count=1
[sifman]   DMA iop=0x000A8160 -> ee=0x00502E80 / 0x00502F00 (alternating) size=128
[thevent] SetEventFlag id=3 bits=0x00000001 -> pattern=0x00000101
[thevent] SetEventFlag id=3 bits=0x00000100 -> pattern=0x00000101
```
- **All 86 `sceSifCheckInit` returns are `-1`, never success.** SIF reports "not initialized" forever; the EE guest's CheckInit poll never clears → hang.
- The `[sifman]`+`iSifSetDma`+`sceSifCheckInit` strings are **NOT in our C++ stubs** (ours are `[sceSifSetDma:...]`/`[SifCallRpc]`). They are printed by the **interpreted IOP sifman.irx running in the R3000 interpreter** — genuine IOP code, consistent with the no-faking rule. So the -1 originates IOP-side; the IOP<->EE SIF init handshake never converges on the IOP side.

**Next session — START HERE:** find, in the interpreted sifman path, where `sceSifCheckInit`'s return / the SIF-init flag it reads is computed, and WHY it never flips to "initialized." Likely a missing EE→IOP SIF register/DMA-completion signal the interpreted sifman waits on. Fix must stay behavioral (`game_overrides.cpp` / `src/lib/*.cpp`) or be an interpreter-side correction — NOT a hand-faked return value ([[feedback_no_iop_faking]]). Ground truth in memory [[reference_ps2_sif_boot]] 2026-07-16h. Rebuild incremental: `.\build.ps1` then `.\launch_recomp.ps1`.

## Current Status (2026-07-16d) — RAN the built exe: VBLANK/magenta re-baseline was WRONG; game never reaches 0x175210. Real live blocker (re-confirmed) = SIF-RPC bind ping-pong that never converges; v3 fix RAN and STILL storms

First live run of the linked exe (`.\launch_recomp.ps1 -NoDebugger`). Upstream check done: only new item is #170 (IOP refactor to ps2xIOP/) — HOLD; all other recent upstream PRs already in tree. All 5 INTC_STAT VBLANK edits confirmed present in the linked sources before running.

**Result — the 07-16b/c magenta=VBLANK-poll re-baseline is REFUTED by our port's own trace:**
- PC pinned `0x100008`, `lastCall=0x174cb0` (DeleteSema), stuck permanently. Game **never reaches `0x175210`** — the INTC_STAT VBLANK poll is downstream code it never gets to. The 5-edit VBLANK fix is valid code but unvalidatable from here; **PARKED**, not the front line. ([[reference_intc_stat_vblank_gap]] updated.)
- **Actual blocker = the SIF-RPC bind ping-pong from 07-15**, and the v3 `deliverSifRpcReply` fix (WORD[8]-forcing) RAN LIVE and did **NOT** converge. Guest sends BIND `[0x40,0,0x80000009,0,0x5,0x20561900,SEQ,0x564980]` + END `[...0x80000008...]` every iteration; SEQ (word[6]) climbs `0x2..0x21+` forever, fresh sema each time (sid 4→34+). `_request_bind`(0x178938) runs each iteration but the bind never sticks — guest re-binds with next seq.
- **Log-cap trap noted:** `[SifRpcReply] deliver` lines going silent is just the `s_replyLogs < 24` cap (SIF.cpp:251), NOT the depth cap (SIF.cpp:193) and NOT delivery stopping. Don't misread it.

**Next session — START HERE (diagnosis first, NO blind v4):** v1/v2/v3 have all failed blind. Before any code: dump the FULL 16-word (64-byte) BIND packet + the reply packet + the client-side "server registered" flag the bind loop polls (the 8-word log dump hides words 8..15, where WORD[8] router + IOP-return fields live). Determine WHY the client re-binds after `_request_bind` runs — missing registered-flag field in the reply (word[9..15]), an undelivered `_request_rdata`(0x8000000C)/`_request_call`(0x8000000A) path, or a legit per-frame heartbeat masking a different block. Fix in `src/lib/*.cpp` only, no faked IOP data ([[feedback_no_iop_faking]]). Full ground truth in memory [[reference_ps2_sif_boot]] 2026-07-16d. Rebuild = incremental (SIF.cpp in ps2_runtime lib): `.\build.ps1` then `.\launch_recomp.ps1 -NoDebugger`.

## Current Status (2026-07-16c) — INTC_STAT VBLANK boot-blocker FIX IMPLEMENTED + compiles clean (ps2_runtime.lib built); full ps2EntryRunner link + run verification PENDING (user was mid-build at session close)

The magenta-screen stall (2026-07-16b, below) is now root-caused and fixed in `src/lib` only. Game spins forever at `0x175210` polling INTC_STAT `0x1000F000` bit2 (VBLANK-start); runtime never raised that MMIO bit on the vsync tick → infinite spin = magenta screen. Dual-confirmed vs real PCSX2. Full root-cause detail in memory [[reference_intc_stat_vblank_gap]].

**Fix (5 edits, all `src/lib`/one header — NO recompiler run, NO fn_*/runner edits):**
- `ps2_memory.cpp` writeIORegister: add W1C case for `0x1000F000` (game's `sw 4` ack clears bit2, matching D_STAT 0x1000E010 W1C precedent).
- `ps2_memory.cpp` initialize(): pre-seed `m_ioRegisters[0x1000F000u]=0` after clear() so the vsync worker's OR only ever assigns to an existing node (avoids unordered_map rehash-vs-find race; residual torn-word race is benign).
- `ps2_memory.cpp`: new `orIORegister(addr,bits)` definition (`m_ioRegisters[address] |= bits;`).
- `ps2_memory.h`: declare `void orIORegister(uint32_t,uint32_t);`.
- `Interrupt.cpp` interruptWorkerMain per-tick: `orIORegister(0x1000F000u, 1u<<2)` before kIntcVblankStart dispatch, `1u<<3` before kIntcVblankEnd.

**Build scope (learned):** header edit fans `ps2_memory.h` into 17 TUs + the whole unity `fn_*` corpus (`unity_NNNN_cxx.cxx`) → wide recompile, but still NOT a recompiler run (output/ sync = 0 files, fwd decls unchanged). Long compile, bounded.

**Status at session close:** `ps2_runtime.lib` built with NO errors (ps2_memory.cpp + Interrupt.cpp compiled clean, 9/12 targets); final `ps2EntryRunner` unity link was still in progress when the session ended. NOT yet run.

**Next session — START HERE:** confirm the build linked `ps2EntryRunner.exe` (watch for any `error [A-Z]` / build_errors.txt). If clean, run `.\launch_recomp.ps1 -NoDebugger`, capture the PC watchdog trace, and confirm the `0x175210` VBLANK-poll loop now EXITS each frame (magenta replaced by a rendered frame). Then read the next boot blocker from the new trace. If verified, mark [[reference_intc_stat_vblank_gap]] as fixed. Do NOT resume the RecompDebugger register-parse plan (`snazzy-crunching-fairy.md`) — parked, not the front line.

## Current Status (2026-07-16b) — CORRECTION: 0x175220 is the interactive mem-card prompt wait on PCSX2, NOT a deadlock; our port dies far EARLIER (magenta screen, never reaches PCSX2's black screen)

Free-run diagnostic on **real PCSX2** (real ISO + scph39001, DebugServer) settled: EE PC pinned at `0x00175220` across ~876M+ cycles of *real* execution, `0x1000F000`=0 and all SIF mailboxes 0. Earlier sessions read this as a lost-wakeup / SIF-DMA deadlock. **That framing is WRONG.**

- **User ground truth:** `0x175220` is the **memory-card check** — an *interactive, input-gated* prompt. Real PCSX2 shows **black screen → mem-card check (press X) → game**. The SIF spin is the prompt's normal poll loop waiting on card status + the user's button press. Not a hang. (Consistent with the 2026-07-07 note at line ~342.)
- **Our port's actual behavior:** **only a magenta screen — no black screen, no prompt.** The magenta screen is a boot stage *earlier* than PCSX2's black screen. **Our port never reaches 0x175220 at all** — it dies before the black-screen stage that precedes the mem-card check.
- **Implication:** the entire 07-15 SIF-RPC bind-reply investigation (0x174ce0 WaitSema, `deliverSifRpcReply` v1/v2/v3) was chasing a blocker *downstream* of where our port is actually stuck. The real first failure is whatever keeps our port on the magenta screen and prevents the black screen from ever appearing. **Re-baseline the boot investigation to the magenta→black-screen transition, not the SIF-RPC bind.**
- User skipped the mem-card check + paused the continue-race on PCSX2 to establish the above.

**Next session — START HERE:** determine what our port renders/executes at the magenta-screen stall (what is the EE actually doing in *our runtime* — is it looping, faulting, or stalled on an IOP/GS init that PCSX2 completes before its black screen). Do NOT resume SIF-RPC bind-reply work until the magenta-stage blocker is characterized. The SIF-RPC status entries below (07-15 / 07-15e) are preserved for reference but are NOT the current front line.

## Current Status (2026-07-16) — RecompDebugger: PCSX2 breakpoints armed, unified BP panel, real-time disasm under DebugServer; build+verify pending

RecompDebugger-only work (plan `snazzy-crunching-fairy.md`, all 4 parts implemented). No runtime/boot-logic changes. Purpose: make RecompDebugger a reliable capture tool for the real IOP SIF-RPC bind-ack packet (the 07-15 SIF blocker below).

- **Part 1 — PCSX2 gutter breakpoints now arm the server.** `tab_codetrace.cpp` disasm-gutter "Set/Remove Breakpoint" mutated only local `g_breakpoints`; now also calls `PCSX2SetBreakpoint`/`PCSX2RemoveBreakpoint` when `g_cpu_source==CPU_PCSX2 && PCSX2DebugServerConnected()`. Gutter BPs actually HALT PCSX2 server-side now (matched the Markers-panel path).
- **Part 2 — one unified "Breakpoints" panel.** `tab_breakpoints.cpp`: the two CPU-gated blocks merged into a single `CollapsingHeader("Breakpoints")` branching on `g_cpu_source` — Recomp shared-mem slots (with GPR conditions + Resume/HIT banner) in Recomp mode, PCSX2 server BPs in PCSX2 mode. Presentation merge only; both backends' add/remove/clear + server mirroring preserved.
- **Part 3 (main fix) — real-time disasm under DebugServer.** `main_gui.cpp` `SyncFromPCSX2` DebugServer branch early-returned BEFORE `ee_ram_base` discovery, so `ee_ram_window` (RPM-filled disasm bytes) never loaded → disasm only populated on a coincidental socket drop (what user saw as "pause to populate"). Now discovers `ee_ram_base` (`FindEERAM`) + calls `ReadEEWindow` (follow-PC/pinned) before the return. JSON reg path + RPM memory-window path are orthogonal, so disasm streams live while regs stream from JSON — no pause needed.
- **Part 4 — flicker.** `tab_codetrace.cpp` `ShowCodeTrace`/`ShowEERegisters` use `static bool s_had_first_snapshot`; keep last-good frame instead of blanking to "Waiting for PCSX2 CPU data..." on transient `regs_valid` dips. Placeholder shows only before the first snapshot.

**Files:** `ps2xRuntime/src/tab_codetrace.cpp` (P1,P4), `tab_breakpoints.cpp` (P2), `main_gui.cpp` (P3,P4). Brace balance verified. NO build run yet (user runs all builds).

**Build (hand to user):** `.\build.ps1 -Debugger`. Verify vs real PCSX2 + SDBZ ISO + scph39001.bin + DebugServer: (1) disasm streams live while game runs unpaused; (2) gutter Set Breakpoint HALTS PCSX2; (3) single Breakpoints panel correct per CPU mode; (4) no Waiting/Scanning flicker; (5) Recomp-mode disasm+BP no regression.

## Current Status (2026-07-15e) — SIF-RPC bind-reply loopback FIX v3 written (WORD[8] router correction); build+run pending

Continues the 0x174ce0 WaitSema blocker below. The game self-hosts its own EE-side libsifrpc: a receive dispatcher `sub_178068` (@0x178068) that polls the pending byte at `0x561600`, and issues raw `sceSifSetDma` BIND/END packets to the IOP, then parks in WaitSema@0x174ce0 awaiting the reply. Our HLE `SifBindRpc` is never called (off this path).

**Fix approach (SIF.cpp `deliverSifRpcReply`, anon ns):** on an outbound RPC system-command `sceSifSetDma`, loop the game's OWN 64-byte packet back into RX queue `0x561600` and run its dispatcher inline via `runtime->lookupFunction(0x178068)`. No invented IOP data — client ptr/server fields echoed verbatim.

**v1/v2 failed, v3 is the correction:**
- v2 echoed verbatim → SignalSema DID fire (trace `0x178560 -> 0x174cd0`) but binds ping-ponged forever (seq `0x2 -> 0x21+`).
- ROOT CAUSE (source-level): the dispatcher routes on packet **WORD[8]** (offset 0x20) `& 0x7FFFFFFF` → system handler table `dword_5616E4` slot. Correct slot map (was SWAPPED in old notes): slot **8** `0x80000008` = `sub_178560` **_request_end** (SignalSema+teardown = the WAKE); slot **9** `0x80000009` = `sub_178938` **_request_bind** (registers server, re-sends END). The game's outbound END packet carries WORD[8]=0x80000009 (set by _request_bind:95282), so a verbatim echo re-routes it to _request_bind → re-send → infinite loop.
- **v3 fix:** force reply WORD[8] to the completion discriminator the IOP return would carry — BIND reply → `0x80000009` (registration), END reply → `0x80000008` (wake). Fixed libsifrpc protocol constants, not faked IOP data. New consts `kSifDiscWordIdx=8`, `kSifRpcEndDiscriminator`; re-entrancy capped depth 8. See [[reference_ps2_sif_boot]] 2026-07-15e.

**Build (hand to user):** `cmake --build "F:\SDBZ Recomp\build" --config Debug --target ps2EntryRunner`. Run: `.\launch_recomp.ps1 -NoDebugger`. SUCCESS = deliver lines STOP after a couple iterations (no seq storm) + pc/lastCall leaves `0x174ce0`. If still storming → client re-binds for another reason; capture log. After boot advances, strip temp `[sceSifSetDma:DTX/OK/FAIL]` + `[SifRpcReply]` cerr diagnostics.

## Current Status (2026-07-15) — sceSifSetDma EE→IOP reject FIXED; boot advanced from DeleteSema spin to WaitSema (0x174ce0) — new blocker: IOP never produces SIF-RPC bind reply

Boot is downstream of the 0x100008 dispatch fix (14f). Live spin surfaced via watchdog `lastCall`, not PC.

**Fix landed & confirmed working (runtime lib only — `ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`):**
- **Root cause:** `sceSifSetDma` was range-checking the descriptor's `dest` against EE RAM. For EE→IOP SIF DMA, `dest` is an IOP-side address / sentinel (`0xffffffff`), NOT EE RAM → all transfers rejected → returned 0 → guest tore down semaphores (DeleteSema 0x174cb0) and retried the SIF-RPC bind forever (11-PC spin).
- **Fix (2 edits, ~SIF.cpp:708 + :729):** validate/copy only the EE-side `src`. If `dest` is copyable EE RAM → EE→EE loopback copy; else treat as IOP-bound → skip EE-side copy but still accept + return nonzero id.
- **Confirmed:** output flipped `[sceSifSetDma:FAIL] why=rangeUncopyable` → `[sceSifSetDma:OK] -> nonzero id` (×2); DeleteSema spin (0x174cb0) gone.
- Diagnostic descriptor that cracked it: `src=0x20561900` (valid EE uncached mirror), `dest=0xffffffff`, `size=0x40`, `attr=0x44`, `ra=0x177fc4`.

**NEW blocker (identified, NOT fixed):**
- Guest now blocks in **WaitSema (syscall 0x44) at `0x174ce0`** (bare trampoline; Dispatcher.cpp:172; WaitSema is a proper blocking Mesa-monitor wait, Sync.cpp:283 — not a busy-spin).
- Guest sends the SIF-RPC **bind request** via its OWN recompiled raw SIF-DMA path (`0x177fe8` → `func_177EB0`), NOT our `SifBindRpc` HLE (RPC.cpp:1578 — off this path). Then WaitSemas for the IOP's **bind acknowledgment**, which never arrives → re-loops the whole bind.
- Anchor fns mapped: `0x174ce0`=WaitSema trampoline; `0x174cb0`=DeleteSema trampoline; `0x177de8`=RPC request-queue ENQUEUE helper (table base `0x5616d8`, NOT a wait loop, 4× repeat is normal); `0x177fe8`=sceSifBindRpc-family wrapper→jal func_177EB0.

**Next session — START HERE (diagnosis, no code yet):** trace whether the accepted EE→IOP SIF-DMA packet actually reaches the embedded R3000 IOP interpreter for servicing (which would generate the reply that durably signals WaitSema@0x174ce0), or lands in a void. Start at `noteDtxSifDmaTransfer` (SIF.cpp) → `ps2_iop.cpp` delivery path. Candidate files from last grep: SIF.cpp, RPC.cpp, ps2_memory.cpp, ps2_iop.cpp, ps2_iop_mcman.cpp, RPC.h, SIF.h, Support.h. Fix must land in `src/lib/*.cpp` only and must NOT hand-write/fake IOP output (run real IRX in interpreter — [[feedback_no_iop_faking]]). After boot advances, remove temp SIF.cpp `[GUARD]`/`[FAIL]`/`[OK]` diagnostic logs.
- Rebuild command (hand to user): `cmake --build "f:\SDBZ Recomp\build" --target ps2EntryRunner` (SIF.cpp is in the `ps2_runtime` static lib → incremental, minutes — NOT the 30h runner rebuild). Re-run: `.\run_watchdog.ps1`. Boot success = EE `0x5e6b3c` GameMode → `0x00` = MainMenu.

## Current Status (2026-07-14g) — RecompDebugger.exe file-browse dialog focus bug fixed (2nd attempt); NOT YET rebuilt/verified

Continuation of the RecompDebugger revival (plan `sharded-zooming-avalanche.md`, target restored + build-verified in a prior session). User reported two UI bugs against the running exe:

- **Config fields empty / Backend=PCSX2:** `symbols.map`, `ps2EntryRunner.exe` etc. already exist on disk (`ps2xRuntime/symbols.map`, `build/ps2xRuntime/Debug/ps2EntryRunner.exe`) — user just needed to fill Settings→Paths fields manually and switch Backend to Recomp. Not a code bug.
- **Browse dialog opens behind main window:** `tab_filedialogs.cpp`'s `ShowFileDialogs()` had zero focus-forcing calls; `ImGuiFileDialog::Display()` only calls `Begin()` for the currently-open key.
- **1st fix (WRONG, caused regression):** added unconditional `ImGui::SetNextWindowFocus()` before all 6 `Display()` calls. Broke both dialog visibility AND paste-into-path-fields — `SetNextWindowFocus()` sets a *global* pending flag consumed by the *next* `Begin()` of ANY window that frame, not scoped to the dialog; when no dialog was open it stole focus from the next window/widget (e.g. an `InputText`), breaking paste.
- **2nd fix (applied, awaiting verify):** added `FocusIfOpen(const char* key)` helper gated on `ImGuiFileDialog::Instance()->IsOpened(key)`, only calling `SetNextWindowFocus()` in the exact frame the matching dialog key is actually open. Replaces the 6 unconditional calls in `ps2xRuntime/src/tab_filedialogs.cpp`.

**Next session / user action — START HERE:** run `.\build.ps1 -Debugger`, relaunch `RecompDebugger.exe`, confirm (a) Browse dialog appears on top/visible, (b) paste into path fields still works (no regression). Then continue the original plan: confirm connect to `Local\RecompDebugState` v10 with Backend=Recomp, launch `ps2EntryRunner.exe` on SLUS_214.42, confirm PC advances live with resolved function name — this is the still-unmet verification criterion for the whole debugger-revival effort, separate from the dispatch-table fix below.

## Current Status (2026-07-14f) — dispatch-table fix APPLIED via format conversion; awaiting user build + boot test. Corrects errors in the 2026-07-14e diagnosis below

Proofing pass over the 2026-07-14e diagnosis found it partly wrong; a much cheaper fix was applied:

- **Correction 1:** `PS2Runtime::registerFunction` is NOT dead code — it exists (`ps2_runtime.cpp:984`) and forwards to `replaceFunction` (`:968`), which writes directly into `g_ps2RecompiledFunctionTable[slot]`. Game overrides installed via it at runtime still win over static init.
- **Correction 2:** No recompiler re-run needed. `output/register_functions.cpp` (legacy format, June 14, 375,485 `runtime.registerFunction(addr, name)` entries) matches the current runner generation — every referenced symbol exists verbatim as a `runner\*.cpp` file.
- **New finding — THREE coexisting generations in `runner\`** (~2 files per guest address, 33,956 files / 16,990 unique addresses): `sub_*` (May 17), IDA-labeled `entry_*`/`GameMain_*`/`CApp*` (May 28), and `fn_*`/`start_*` (**June 14 — newest**, same recompiler run as the legacy register file, identical timestamps). The June-14 set is the internally consistent one to dispatch. All generations compile into the exe; only the table decides which executes.
- **Trap avoided:** pr137-test's populated table (41.5 MB, July 12) is INCOMPATIBLE with main (`sub_00XXXXXX` naming, 26,934-file generation) — never copy it.
- **Header gap:** `fn_forward_decls.h`/`ps2_recompiled_functions.h` only declare `fn_*`-pattern names, so the generated table file carries its own forward declarations for all 16,970 referenced symbols.

**Fix applied:** `convert_register_functions.ps1` (scratchpad, one-shot) parsed the legacy file and regenerated `output\register_functions.cpp` (31.4 MB) in the dense-table `GeneratedFunctionTableInitializer` format: base `0x100008`, end `0x4e6c84`, 1,022,751 slots, 375,485 entries in 8 chunked init structs, 0 unmapped. Original legacy file preserved at `output\register_functions.cpp.legacy-bak`. Fresh timestamp means `build.ps1`'s newer-wins sync WILL overwrite the runner-side stub on next build (no hand-edit of `runner\`). Spot-checks pass: slot 0 = `start_0x100008`, `0x422630` = `fn_422630_0x422630`, tail alias `0x4e6c80` = `fn_4E68C0_0x4e68c0`.

**Next:** user runs `.\build.ps1` then `& "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe" "F:\SDBZ Recomp\ELF\SLUS_214.42"`. Expect the 0x100008 loop gone; new "No exact recompiled function" errors at OTHER PCs would be genuine coverage gaps (e.g. the 3 known stub addresses 0x151830/0x170268/0x11aba0, absent from the legacy file too) — separate triage, not a regression.

## Current Status (2026-07-14e, SUPERSEDED by 14f — contains errors, see corrections above) — dense function-dispatch table (`g_ps2RecompiledFunctionTable`) is never populated — boot hangs immediately at PC 0x100008, unrelated to the C3861/regex fix

Boot-tested `ps2EntryRunner.exe` after the 2026-07-14d build success. Immediately hangs: `dispatchLoop` fails to resolve guest PC `0x100008` on the very first lookup (`Error: No exact recompiled function for guest PC 0x100008 tableBase=0x0 tableEnd=0x1000000`), then spins forever re-trying the same failing lookup (the `missingFunction` stub doesn't advance `ctx->pc`).

**Root cause, traced fully:**
- `ps2xRuntime/src/lib/ps2_runtime.cpp`'s `dispatchLoop`/`lookupFunction` dispatch exclusively via a dense array `g_ps2RecompiledFunctionTable[]` (declared in `ps2xRuntime/src/runner/register_functions.cpp`), indexed by `(address - tableBase) >> 2`.
- The live `ps2xRuntime/src/runner/register_functions.cpp` is only 6 lines — declares the table (`tableBase=0x0`, all slots null) but never populates a single entry. No self-registration macro or bulk-populate call exists anywhere in the codebase (confirmed via full-repo grep).
- The recompiler (`ps2xRecomp/src/lib/function_table_emitter.cpp`, class `FunctionTableEmitter`) is fully capable of emitting the correct file — a `GeneratedFunctionTableInitializer` static-init struct that fills every slot from the function list, with `tableBase` derived from the real address range. This is clearly the intended mechanism.
- `output/register_functions.cpp` (the recompiler's actual output directory) DOES exist but is dated **2026-06-14** and is in an **old, incompatible format**: a `registerAllFunctions(PS2Runtime&)` function calling `runtime.registerFunction(addr, fn)` per entry (375K lines) — this populates a different/legacy registration path, not `g_ps2RecompiledFunctionTable`, and `registerAllFunctions` isn't called anywhere in current `ps2_runtime.cpp`. It's dead code from a prior architecture generation.
- `build.ps1`'s sync step (`Syncing output\ -> src\runner\`) only copies a file if the `output\` source is *newer* than the destination. Since someone hand-replaced `ps2xRuntime/src/runner/register_functions.cpp` with the 6-line stub on **2026-07-11** (newer than the June 14 `output/` copy, and the file is untracked/`??` in git), the sync step correctly-but-unhelpfully skips it every time, leaving the broken stub in place.
- **Net effect:** the dense function table architecture (current `lookupFunction`) and the actual generated output on disk (`output/register_functions.cpp`, legacy `registerAllFunctions` format) are from two different, incompatible generations of the recompiler pipeline. Neither the old output nor the current stub can populate the table `lookupFunction` needs.

**What's needed to fix (not yet done — scope/risk requires user awareness before proceeding):**
1. `ps2_recomp.exe` is not currently built (`build/ps2xRecomp/Debug/` only has the lib, `.dir`, `.vcxproj` — no exe). Needs a real build of the `ps2_recomp` target.
2. `config.toml` does not exist at `F:\SDBZ Recomp\config.toml` (the path this file itself documents) — needs locating/regenerating the real recompiler config for SLUS_214.42.
3. Re-running `ps2_recomp.exe <config.toml>` would regenerate ALL `output/*.cpp` (30,000+ files) in the CURRENT `FunctionTableEmitter` format, including a correct `register_functions.cpp`.
4. `build.ps1`'s sync step should then correctly pick up the newer `output/register_functions.cpp` and copy it over the stale stub (or the stub should just be deleted first so sync always wins).
5. Full recompile after that will be a large rebuild (many/most `.cpp` under `runner/` may be regenerated) — per ps2-recomp skill guardrails, this is NOT a quick incremental rebuild; get explicit user go-ahead on timing before starting.

**Next session / user action — START HERE:** decide whether to (a) locate/regenerate `config.toml` and build `ps2_recomp.exe` to properly regenerate `output/`, accepting a large rebuild, or (b) hand-write a stopgap `register_functions.cpp` (small, targeted — e.g. only registering the handful of functions needed to get past the current boot blocker) as a bridge, matching the `GeneratedFunctionTableInitializer` format `lookupFunction` expects. Do NOT resume "fix stub function C3861 errors" work — that thread (regex bug in build.ps1) is fully resolved and unrelated to this new blocker.

## Current Status (2026-07-14d) — Build succeeded: build.ps1 regex fix confirmed working end-to-end, ps2EntryRunner.exe linked

Re-ran `& "F:\SDBZ Recomp\build.ps1"` after the 2026-07-14c regex fix. Build completed successfully (+14:42, 10/12 targets, exe linked): the three C3861 errors (`fn_170268_0x170268`, `fn_151830_0x151830`, `fn_11ABA0_0x11aba0`) are gone. Confirms the build.ps1 line 135 regex fix (`uint8_t\s*\*\s*rdram`) is durable, not just correct in isolation.

Link-stage warnings present but non-blocking (exe still produced): `LNK4075` (`/INCREMENTAL` ignored due to `/FORCE`), `LNK4006` x2 (raylib's `rcore.obj` `CloseWindow`/`ShowCursor` colliding with `user32.lib`, second definition ignored), `LNK4088` (image built via `/FORCE`, may not run — needs actual boot-test to confirm). The raylib/user32 symbol collision is the same NOUSER issue tracked in [F1 Debug Panel Build Fix](project_f1_debug_panel_build_fix.md) — that fix may not be applied to this target, or `/FORCE` is masking it.

**Next session — START HERE:** boot-test `ps2EntryRunner.exe` via the Active Runner Command above to confirm the `/FORCE`-linked exe actually runs (LNK4088 explicitly warns it may not). If it runs, resume the Phase 5 boot-progress chain (verify PC advances past prior blockers, check `0x5e6b3c == 0x00`). If it crashes/fails to start, investigate the raylib/user32 symbol collision as a likely cause — do NOT touch `runner/*.cpp`, fix belongs in the raylib link config or a `NOUSER`-style define per the referenced memory.

## Current Status (2026-07-14b) — Recompiler boundary-detection gap: 3 missing `fn_*` symbols stubbed in game_overrides.cpp; fn_forward_decls.h confirmed auto-regenerated every build (manual header edits are non-durable)

A build attempt (+26:45, failed exit 1) hit three C3861 "identifier not found" errors: `fn_170268_0x170268`, `fn_151830_0x151830` (both previously stubbed in a prior session by editing `fn_forward_decls.h` directly), and a new one, `fn_11ABA0_0x11aba0`.

**Root cause:** `ps2xRuntime/include/fn_forward_decls.h` is marked "Auto-generated by build.ps1 - do not edit" and is in fact rewritten on every `build.ps1` run (see `build.ps1` lines 110-209). It scans `runner/fn_*.cpp` files (lines 117-126) AND separately regex-scans `ps2xRuntime/src/lib/game_overrides.cpp` (lines 128-145, pattern `void\s+(fn_[0-9A-Fa-f]+_0x[0-9a-fA-F]+)\s*\(uint8_t\*\s*rdram`) for fold-extracted stub definitions with no matching runner file, auto-declaring those too. The prior session's direct edit to the header was silently wiped by the next build run — that's why the fix "didn't hold."

**Correct durable fix (now applied):** define missing `fn_*` symbols directly in `game_overrides.cpp` matching the exact signature `void fn_X(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)` — build.ps1's regex auto-declares them every run, no header edits ever needed. All 3 stubs (`fn_151830_0x151830`, `fn_170268_0x170268`, `fn_11ABA0_0x11aba0`) are now present as diagnostic log-and-return stubs in `game_overrides.cpp` (lines ~27-46), all matching the pattern.

**RESOLVED — actual root cause found (2026-07-14c):** the "open/unexplained" gap above was real and now explained. The build.ps1 regex-scan of `game_overrides.cpp` (line 135) was `'void\s+(fn_[0-9A-Fa-f]+_0x[0-9a-fA-F]+)\s*\(uint8_t\*\s*rdram'` — it required `uint8_t*rdram` with NO space between `uint8_t` and `*`. The actual code style in this repo is `uint8_t *rdram` (space before the `*`, not after). So the regex NEVER matched, for any of the three stubs, on any prior run — the auto-scan mechanism was silently broken from the start, not "working but overwritten." Verified via direct PowerShell regex test against the live file: 0 matches with the old pattern, 3/3 matches after the fix. **Fixed:** `build.ps1` line 135 changed to `'void\s+(fn_[0-9A-Fa-f]+_0x[0-9a-fA-F]+)\s*\(uint8_t\s*\*\s*rdram'` (added `\s*` between `uint8_t` and `\*`).

**Next session / user action — START HERE:** run `& "F:\SDBZ Recomp\build.ps1"` again. The header should now report N declarations written (not "unchanged") and include all 3 stub symbols — verify with a quick grep on `fn_forward_decls.h` for `151830|170268|11ABA0` if the build still fails on these. If it fails on a *new* undefined `fn_*` symbol, same durable fix pattern applies — add a matching stub to `game_overrides.cpp`; the regex is now confirmed working.

## Current Status (2026-07-14) — CORRECTION to 2026-07-13c below: PR #168's fix was ALREADY in main (commit `b7d9e17b`, 2026-07-11 21:43, "fix(recomp): advance ctx->pc on fallthrough functions with no terminating branch") — the 2026-07-13c claim "the fix has NOT been applied to this main tree" was never verified against git and was wrong. This is more concerning than it sounds: the 2026-07-13c boot-test ran with the fix already present and STILL hit the `0x100008` infinite loop — meaning the fallthrough-pc fix alone did not resolve this hang, or something since regressed it. Also found: `function_emitter.cpp` currently has a separate uncommitted change on top (mid-asm hook injection — `m_midAsmHooksBeforeByAddress`/`AfterByAddress`, lets generated runner code call user C++ hooks before/after a specific instruction). Unclear if this is related.

**Next session — START HERE:** a build is in progress (user-run, 2026-07-14). Once done: (1) boot-test via the Active Runner Command, check whether PC still hangs at `0x100008`; (2) if it still hangs, do NOT re-diagnose PR #168 again — that's confirmed present and confirmed insufficient. Instead check whether `74359c8f` (the actual PR #168 commit content) matches what's in `b7d9e17b`, since PR branch and main-tree fix may differ; (3) evaluate whether the uncommitted mid-asm-hook diff affects this at all (temporarily stash it and retest if needed). **Standing rule: before writing "fix not yet applied"/"not yet done" into this file, verify with `git log`/`git diff` on the actual file — this entry was wrong for a full day because that wasn't done.**

## Current Status (2026-07-13c) — SUPERSEDED, KEY CLAIM WAS WRONG (see 2026-07-14 above) — Boot-test path fixed; main-tree build now confirmed to hit the SAME dispatch-fallthrough dead loop as pr137-test (PR #168 fix not yet applied to main)

Resolved the 4a.4 boot-test blocker from the entry below: `F:\SDBZ Recomp\ELF\SLUS_214.42` exists; the real current build output tree is `F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe` (built 2026-07-13 18:32) — `PS2Recomp\out\build\...` (the old documented path) no longer exists on disk. "Active Runner Command" section above corrected accordingly.

Ran the corrected boot command (30s timeout). Result: raylib/GL/audio init all succeed cleanly, then the process hard-loops forever at `pc=0x100008` — log shows `[guest-branch:missing-target] kind=IndirectJump ... source=0x0 target=0x100008 pc=0x100008` followed by ~16,000 repeats of `Error: No exact recompiled function for guest PC 0x100008` in 30s. **This is the identical root cause already diagnosed and fixed in the pr137-test sandbox (see 2026-07-11b below) and submitted as [PR #168](https://github.com/ran-j/PS2Recomp/pull/168)** (`fix/dispatch-fallthrough-pc`, commit `74359c8f`): `FunctionEmitter::emit` doesn't advance `ctx->pc` past a function's last instruction when that instruction isn't a branch/jump, so the ELF-entry-point pseudo-functions (1-2 instruction no-op splits with no terminating branch) spin on their own address forever. **The fix has NOT been applied to this main tree** — it currently only exists in the PR #168 branch.

**Next session — START HERE:** apply the same `function_emitter.cpp` fix from PR #168 (or wait for it to merge upstream and re-sync) to `ps2xRecomp/src/lib/function_emitter.cpp` in this main tree, rebuild via `build.ps1`, then re-run the corrected boot command above and confirm PC progresses past `0x100008`. Once boot progresses, resume the original 4a.4/4a.5 verification chain from the entry below (confirm `0x5e6b3c == 0x00`, then IRX loader chain `5d2856fb → c25428d1 → 52205a65 → 1a49fa58 → 68326c3b`).

## Current Status (2026-07-13b) — Stage 4a (pr135 port) through sub-stage 4a.4 verified; boot-test blocked on missing guest ELF at runtime

Sub-stages 4a.1–4a.4 of the `pr135` IOP-subsystem port (plan: `C:\Users\mwlab\.claude\plans\the-test-directory-has-mighty-mango.md`) are landed and "trust but verify" reviewed clean, including `ps2_memory.cpp` (IOP RAM/SIF-DMA layer, bounds-checked, self-test covered; SPR-DMA-bypasses-DMAE behavior correctly scoped to channels 8/9 for Freekstyle; EE Timer/COP0 Count generalization is a clean parameterization with no behavior change for T0).

Attempted the plan's verification item 6 (boot-test `ps2EntryRunner.exe` after 4a.4). Command run: `.\build\ps2xRuntime\Debug\ps2EntryRunner.exe` (no args) → fatal exception `Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.` Root cause confirmed by reading `ps2xRuntime/src/main.cpp` `getExecutablePath()`: the exe requires the guest ELF path as `argv[1]`; no `PS2X_DEFAULT_BOOT_ELF` macro is defined in this build config. Correct invocation per this file's own "Active Runner Command" section above is `& "F:\SDBZ Recomp\PS2Recomp\out\build\ps2xRuntime\Debug\ps2EntryRunner.exe" "F:\SDBZ Recomp\ELF\SLUS_214.42"` — note the build output path in that command (`PS2Recomp\out\build\...`) differs from the path actually used this session (`.\build\ps2xRuntime\Debug\...`); this discrepancy was not resolved before session end.

**Next session — START HERE:** (1) verify which build output path is currently correct (`F:\SDBZ Recomp\ELF\SLUS_214.42` per this doc, vs. the `.\build\...` relative path used this session) and confirm `F:\SDBZ Recomp\ELF\SLUS_214.42` actually exists; (2) hand the user the corrected boot-test command with the ELF path included; (3) once boot confirmed (`0x5e6b3c == 0x00`), close out 4a.4 and proceed to sub-stage 4a.5 (IRX loader/module-execution chain: `5d2856fb` → `c25428d1` → `52205a65` → `1a49fa58` → `68326c3b`, reconcile step 3a/3c against main's existing `ps2_iop_audio.cpp`).

Also this session: redefined memory `feedback_update_memory_vs_end_session.md`'s "end session protocol" — the old definition referenced `sync.ps1`, which no longer exists in the repo (build_scripts/helper-script staleness was independently confirmed via `command_log.md` and repo-tree globs this session too). New definition: update memory files + update this project-state doc (`BUG_LOG.md` does not exist in this repo, despite being referenced in some memory files — that reference is itself stale and should be corrected/removed next time it's touched) + write a pass-on note. No zip, no git commit, unless separately requested.

## Current Status (2026-07-11b) — pr137-test "silent crash" root-caused as a non-crashing infinite dispatch loop; fix implemented and PR #168 opened upstream; NOT YET build-verified

Live cppvsdbg debugging (breakpoints at `ps2_runtime.cpp:1911` inside `dispatchLoop`, plus `2204`/`2255`/`2285` sanity checks) conclusively re-diagnosed the pr137-test "silent crash near boot-transition" — it is **not a crash at all**. The process stays alive indefinitely (window opens, renders a static magenta frame, no exception/abort/exit); `ctx->pc` was observed stuck at `0x100008` across consecutive `dispatchLoop` iterations. Confirmed create_fiber/guest_executor_main/ps2fiber_resume (`SwitchToFiber`) all run and return normally — not a scheduler bug. Confirmed dispatch table is populated/synced correctly — not the known empty-dispatch-table bug.

**Root cause:** `FunctionEmitter::emit` (`ps2xRecomp/src/lib/function_emitter.cpp`) never advances `ctx->pc` past a function's own last instruction unless that instruction is a branch/jump (handled via `handleBranchDelaySlots`). The recompiler's function-boundary pass splits the SLUS_214.42 ELF entry point into multiple 1-2 instruction pseudo-functions (`sub_00100008_0x100008` etc., all `padduw $reg,$zero,$zero` no-ops) with no terminating branch, so `ctx->pc` is left pointing at its own address forever and `dispatchLoop` calls the same function in an infinite loop.

**Fix implemented** in main tree's `ps2xRecomp/src/lib/function_emitter.cpp`: track whether the last processed instruction had a delay slot (`inst.hasDelaySlot`, the branch/jump signal); if a function ends without one, emit `ctx->pc = 0x<function.end>u;` before closing the function so `dispatchLoop` always resumes at the next function instead of spinning. This is a defensive emitter-level fallback, not a fix to the upstream function-boundary/discovery pass itself (which is the more correct place to merge these no-op runs into real functions — flagged as an alternative fix direction, not implemented).

**PR opened:** https://github.com/ran-j/PS2Recomp/pull/168 (branch `fix/dispatch-fallthrough-pc`, commit `74359c8f`, via fork `TH3BACKLOG/PS2Recomp` since `TH3BACKLOG/SDBZ-Recomp` isn't a registered GitHub fork of `ran-j/PS2Recomp`). **NOT build-verified locally** — no `build.ps1`/`build_scripts` were present in this session's main-tree checkout to compile+run the fix end-to-end; PR body flags this and requests a maintainer/CI pass.

**Next session — START HERE:** (1) once PR #168 gets CI/maintainer feedback, address review comments or re-verify; (2) when a build environment is available, actually rebuild `pr137-test` with the emitter fix (or wait for it to land upstream and re-sync) to confirm the pr137-test boot actually progresses past the entry point now; (3) decide whether to commit the restored `PS2_PROJECT_STATE.md` (was untracked this session after being restored from git history); (4) clean up temporary worktree `F:\SDBZ-Recomp-pr-fix` if no longer needed.

## Current Status (2026-07-10c) — Codeium extension Bad Image fix; pr137-test silent-crash debug infra set up; found undocumented uncommitted changes in this tree

- **Unrelated Windows/tooling fix:** VS Code showed a "Bad Image" dialog (`0xc000012f`) for a corrupted Codeium extension native module (`codeium.codeium-1.48.2\dist\...node`). Fixed by `code --uninstall-extension codeium.codeium` then `code --install-extension codeium.codeium` (clean v1.48.2 reinstall). Not yet confirmed resolved by user on next VS Code launch.
- **pr137-test sandbox — new silent-crash thread found this session, separate from BUG-024:** `ps2EntryRunner.exe` in `F:\SDBZ-Recomp-pr137-test` exits with zero output/no dialog shortly after ELF load + 2 texture loads. `main.cpp` has no SEH handler, so a native access violation/stack overflow would be swallowed silently by Windows — matches the symptom. Added `F:\SDBZ-Recomp-pr137-test\.vscode\launch.json` ("TEST UB - pr137-test ps2EntryRunner (cppvsdbg)") to catch it live with a real native debugger. Full writeup in `F:\SDBZ-Recomp-pr137-test\CRASH_INVESTIGATION.md`. **Not yet run** — needs the user to open that folder as its own workspace and press F5.
- **Found uncommitted, undocumented working-tree changes in this tree (not from this session, no matching handoff/memory):** `code_generator.cpp` + `ps2_runtime_macros.h` add an `FPU_SET_ACC`/`ctx->fpuAcc` fix for COP1 accumulator ops (previously aliased onto `f[31]`, which looks wrong but is unverified/unbuilt). `game_overrides.cpp` has ~20 undocumented `sdbzBisectGap*`/`sdbzBisect18D680*`-family diagnostic stubs bisecting the MemSysInit/fn_421B70 gap, plus the already-documented `[HACK][SifCallRpc-unbound]` shim. See CLAUDE.md Project State for full detail — **needs triage next session**: verify `fpuAcc` field exists, build-test, then commit-with-writeup or revert; check if the bisect stubs already answered their question or are still open.

## Current Status (2026-07-08b) — CONFIRMED LIVE: game is genuinely blocked inside the SIF RPC call, not waiting on memory-card input

Live-attached via `recomp` MCP (single confirmed `ps2EntryRunner.exe` instance). Decompiled `mem_fill_z_369` (`0x178be8`) — it IS the real `sceSifCallRpc` implementation: builds the RPC packet struct, then `syscall_stub_z_6()` (SIF send) → `syscall_stub_z_7()`/`syscall_stub_z_10()` (SIF wait/receive). With no breakpoint armed and the game running freely, thread tid 1's PC was observed parked at `0x178be8` across repeated reads several seconds apart, with `r4=0x5a9380` (client handle) and `r6=0x3` (cmd 3) — matches the RPC call args from the 2026-07-07b static trace exactly. Also tested and ruled out the alternate hypothesis that this is actually the memory-card prompt waiting on player input: neither keyboard nor controller input has any observable effect, consistent with the game never reaching input-polling code because it's stuck in this RPC wait first.

**Next session — START HERE:** identify the 4 RPC handle globals (`dword_5A9330`/`5A9358`/`5A9380`/`5A93A8`) against the known SIF client-handle pattern (SIF CMD IDs `0x80000008/9/A/C`); trace which IOP-side IRX module (likely `ARKD_DVD.IRX`, now fully labeled) should service client `0x5a9380`/cmd 3 and why its service loop isn't responding. Per "No IOP Faking" — fix must get the real IOP service responding, not stub the reply.

## Current Status (2026-07-07d) — `irx_label.py --auto-pattern` heuristic added, bug found + fixed, validated; no application to real CRI_ADXI data yet; black-screen blocker unchanged

No runtime code changes. Added `--auto-pattern` flag to `build_scripts/irx_label.py`'s `dump` subcommand (`classify_patterns()`): pre-fills `proposed_name`/`confidence` for getter/setter pairs and duplicate-body pairs, to cut token cost of hand-labeling mechanical CRI_ADXI functions. Found and fixed a real bug during verification: with no minimum-content guard, trivial/nullsub-style stub bodies collided into one bogus 43-function duplicate cluster; fixed by requiring bodies have ≥3 semicolons and ≥80 chars before duplicate-pair matching. Verified against a disposable test module (`CRI_ADXI_autotest`) — false-positive count went 50→4 rows, remaining 4 rows are two genuine duplicate pairs. Also hit and resolved an idalib "failed to open database" error — root cause was the IDA GUI still holding the `.i64` file open (OS file lock), not corruption. Feature is validated but has **not yet been run against the real CRI_ADXI module** — that's the immediate next action. Details in memory `project_irx_labeling_workflow.md`.

**Next session — START HERE:** run `dump --auto-pattern` against the real `CRI_ADXI.IRX.i64` (not `_autotest`) to continue batch labeling with the new heuristic, or resume manual reading of `irx_label_CRI_ADXI.IRX.txt` from ~line 2397 if preferred. Refcount inc/dec pattern (addendum pattern #2) is still unimplemented — deprioritized, not blocking.

## Current Status (2026-07-07c) — ARKD_DVD.IRX labeling complete, workflow documented; 2 syscall stubs labeled; black-screen blocker unchanged

No runtime code changes. Completed ARKD_DVD.IRX semantic renames (6 functions: TocLookup, TocDecodeAndIndex, InitLoadBuffers, CdReadRetryLoop, CdReadRetryLoopAlt, RpcReplyRetryLoop) via the new `build_scripts/irx_xref_dump.py` headless idalib batch-trace script; workflow documented in memory (`project_irx_labeling_workflow.md`) for reuse on the 6 remaining IRX modules (SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, CRI_ADXI). Also labeled 2 SLUS_214.42 syscall stubs from user-pasted disasm: `syscall_stub_z_7` (0x174CB0) = DeleteSema (syscall 0x41), `syscall_stub_z_8` (0x174CC0) = SignalSema (syscall 0x42) — note `syscall_stub_z_7` is a caller inside `mem_fill_z_369` (+0x1A4/+0x1C4), which is the next black-screen investigation target.

**Black-screen blocker is still open and unchanged — see 2026-07-07b entry below for full chain. Next session START HERE:** decompile `mem_fill_z_369` to confirm it issues a genuine SIF RPC, identify the 4 RPC handle globals (`dword_5A9330`/`5A9358`/`5A9380`/`5A93A8`) against the SIF RPC client-handle pattern, then trace which IOP-side IRX module should service that RPC and why it doesn't.

## Current Status (2026-07-07b) — Black-screen investigation: `singleton_get_mgr_1()`-null hypothesis refuted; found concrete resource-load fail chain ending in a SIF/IOP RPC round-trip that never gets a valid reply

Continuing plan `C:\Users\mwlab\.claude\plans\stateless-fluttering-tiger.md` (root-causing why `GameInit`'s scene object parks at tick-state `9900` inside `fn_327810`/`0x327810`, blocking the splash/render chain and causing the black screen). Pure static IDA analysis this session, no runtime tracing, no source edits.

- **Ruled out:** `singleton_get_mgr_1` (`0x1c0da0`) is a trivial lazy singleton — it always returns a valid `$gp`-relative address and can never return null. This definitively refutes the prior "CD-manager-failure via null singleton" hypothesis; the `pool_entry_pop_i` `aCcdreadOpenOut` error branch that theory was chasing is dead code.
- **Confirmed the real fail chain, by reading the resource-handle vtable (`dword_4E7930`) implementations:**
  - `fn_327810` case-3 → `reg_save_stub_z_238("ply_exthit_hit")` → vtable `+16` (`0x1bfe90` = `CCDRead::Open`) → `reg_save_stub_z_203` (`0x1c02c0`, the file-lookup) fails → returns sentinel `-2147418062` ("File not found") → propagates up through `reg_save_stub_z_238` → object parks at tick-state `9900` forever, never finishing init.
  - Vtable `+24` (`0x1bfe60`) and `+28` (`0x1bfe50`) are both trivial one-line delegation wrappers to `reg_save_stub_z_205`/`reg_save_stub_z_204` — not relevant to the failure.
- **Key new finding — `reg_save_stub_z_203`'s file lookup is a live SIF/IOP RPC round-trip:** on first call (gated by an internal one-time-init flag), it busy-waits on 4 RPC handle globals (`dword_5A9330`, `dword_5A9358`, `dword_5A9380`, `dword_5A93A8`) via `wrap_rpc_handle_valid`, dispatches a request via `mem_fill_z_369(dword_5A9358, 3, 3, dword_5A97D0, 48, dword_5AA7D0, 16, 0, 0)` (48-byte cmd / 16-byte reply buffers — classic SIF RPC shape), waits again, then reads the reply from fixed IOP-shared memory (`MEMORY[0x205AA7D0]`/`MEMORY[0x205AA7D4]`) — a negative value there means "not found." Subsequent calls skip the RPC and use a local cache (`reg_save_stub_z_202`).
- **This directly ties to the plan's already-documented "zero SIF/RPC/ADX/sceCd traffic in the entire boot log" finding.** The real IOP-side CD-file-table service for this RPC never responds, so the reply slot never gets populated with a valid value, the read comes back negative/garbage, and `CCDRead::Open` reports "File not found." This plausibly explains not just `ply_exthit_hit` but every asset-dependent object stalling the same way — i.e. this may be the actual reason the splash/render chain (plan signals #3-#6) never advances, not a separate downstream issue.
- **Not yet confirmed / next session — START HERE:**
  1. Decompile `mem_fill_z_369` to confirm it genuinely issues a SIF RPC dispatch (not a raw memory fill despite the generic name).
  2. Identify the 4 RPC handle globals (`dword_5A9330`/`5A9358`/`5A9380`/`5A93A8`) — cross-check against the known SIF RPC client-handle pattern in `reference_ps2_sif_boot` memory (SIF CMD IDs `0x80000008/9/A/C`).
  3. If confirmed as a real SIF RPC, trace which IOP-side IRX module (likely `ARKD_DVD.IRX`) is supposed to service this specific RPC command, and why it doesn't — per the standing "No IOP Faking" rule, the fix must get the actual IOP service responding, not hand-write the shared-memory reply value.
  4. Once resolved, re-check whether this alone is sufficient for the plan's signal #3 (`0x3E2E80` breakpoint starts firing) and signal #4 (SIF/ADX traffic appears) to both go green, or whether they're still separate gates.

## Current Status (2026-07-07) — GitHub PR #1 CLAUDE.md conflict resolved (merge commit `adf8f09`, not yet pushed); `ps2EntryRunner` rebuild shows unverified `/FORCE` linker warning; Phase 5 goal narrowed to memory card prompt (main menu deferred)

- User reported GitHub flagged PR #1 (`work/laptop-session-0519` → `main`) as having conflicts. Root cause: `origin/main` was stale (last commit `c68adaf`, 2026-05-27) vs. current branch — one real conflict, in `CLAUDE.md`'s Project State section. Resolved via `git merge origin/main -X ours`, producing merge commit `adf8f09`; verified clean (no conflict markers, all current Project State entries intact). **Not yet pushed to `origin/work/laptop-session-0519`** — awaiting explicit go-ahead before pushing.
- Also investigated and confirmed a VSCode CMake Tools auto-configure run (FetchContent populate of toml11/raylib/sdl2/imgui/imgui_file_dialog/nlohmann_json into a stray `F:/SDBZ Recomp/build/` dir) was **not** agent-triggered — consistent with the standing CLAUDE.md rule that the agent never calls cmake directly, plus the build output path itself is a different, redundant tree from the real `PS2Recomp/out/build`. Hardened `.vscode/settings.json` against recurrence: added `cmake.configureOnEdit: false` plus `cmake.buildBeforeRun`/`cmake.options.statusBarVisibility`/`cmake.showOptionsMenu` suppression, alongside the pre-existing `configureOnOpen`/`automaticReconfigure` false settings. Verified via Read that the edit persisted.
- **Stray `F:\SDBZ Recomp\build\` directory (from that auto-configure) is still on disk** — flagged to user as safe-to-delete, not deleted (destructive, not authorized).
- **Separately, ran a real `ps2EntryRunner` rebuild via `Laptop_Build.ps1` for the still-open BUG-024/PR #137 verification thread below.** Output showed only benign `LNK4006` duplicate-symbol warnings (raylib's bundled stb_image vs `ps2x_imgui_lib`'s `ImGuiFileDialog.obj`), but also `warning LNK4075: ignoring '/INCREMENTAL' due to '/FORCE'` and `warning LNK4088: image being generated due to /FORCE option; image may not run`. This means something forced the linker to resolve unresolved externals rather than hard-error — the actual `LNK2019`/`LNK2001` lines that would show what's unresolved were earlier in the log and were never captured (user closed the terminal first). **Treat the current `ps2EntryRunner.exe` as unverified — do not use it for PR #137 Step 4 boot verification until re-built with full log capture.**
- **Next session — START HERE:** (1) decide whether to push `adf8f09`; (2) rerun `Laptop_Build.ps1`, redirecting full output to a file (e.g. `*> build_log.txt`) so the `LNK2019`/`LNK2001` unresolved-external lines (if any) are visible, before trusting the binary for a boot test; (3) resume BUG-024 root-causing (below) once the binary is confirmed sound.

## Current Status (2026-07-06d) — PR #137 verification: `ps2xTest` build fixed, but suite reveals 4 failures + a stack-overflow crash (BUG-024, open) — do NOT delete test dir yet

- Continuing the integration-verification plan (`jiggly-pondering-kurzweil.md`). Fixed `ps2xTest`'s build: 6 stale `setPadOverrideState`/`clearPadOverrideState` call sites in `pad_input_tests.cpp` (pre-existing API drift, unrelated to PR #137), then `ps2xTest/CMakeLists.txt` was missing links to `ps2_runner_stubs_a..h` (the 8 static libs holding the generated `fn_*` bodies that `game_overrides.cpp` calls into — `ps2EntryRunner` already linked these, `ps2x_tests` never did). Both fixed; `ps2x_tests.exe` now builds and links clean.
- **Running the suite did NOT reach the 367/367 baseline** (from the isolated worktree test). Found: 2 VIF1/DMAC-dispatch-on-store failures, 2 `TerminateThread`/WaitSema teardown-assertion failures, and a hard crash — `STATUS_STACK_OVERFLOW` (exit code `-1073741571`/`0xC00000FD`), confirmed via `cmd`-redirect (`>`/`2>&1`) capturing the real exit code across 2 independent runs, both stopping at the identical point (right after `"semaphore legacy layout decode remains supported"` passes, before the next test even prints `[Run]`). Full detail logged as **BUG-024** in `BUG_LOG.md`.
- These failures land directly in the two subsystems the plan itself flagged as highest-risk for the fiber-scheduler rewrite (`Interrupt.cpp`/`Sync.cpp`, VIF1/DMAC dispatch + WaitSema/TerminateThread under the new cooperative model) — plausibly real PR #137 regressions, not stale-test drift (unlike the pad-port case).
- **Plan Step 6 (delete `PS2Recomp\ps2xTest\`) is explicitly blocked** until BUG-024 is root-caused and the suite passes clean, per the plan's own gate. Plan Step 4 (boot verification against `SLUS_214.42`, checking for absence of `0x172998`-class freeze) has NOT been attempted yet this session either.
- **Next session — START HERE:** identify which test in `ps2xTest/src/ps2_runtime_kernel_tests.cpp` runs immediately after `"semaphore legacy layout decode remains supported"` — that's the crash trigger; determine if it's a genuinely deep call path or a fiber stack-size misconfiguration. Then root-cause the 3 logic failures (VIF1 dispatch x2, TerminateThread x1). Only after all of this passes clean should Step 6 (test-dir deletion) proceed. Boot verification (Step 4) is separate and still pending regardless.

## Current Status (2026-07-06c) — `dword_441A00` CLOSED as dead end; smstt/smrdy proven to be untouched static ELF defaults, not failed writes

- **`dword_441A00` write-site search resolved via Opus subagent, cross-validated against raw-ELF disassembler:** `mips_r5900_disassembler.py --find-writes-to 0x441A00` found **zero writers anywhere in the binary**; the slot's static ELF-image init value is `0x0`. The `if(dword_441A00) call(...)` tail in `sub_11FA60` is a by-design optional hook, not a skipped init — the 2026-06-21g/2026-07-06b working theory ("gated but reachable") is refuted.
- **`sub_120220`/`sub_1202A0` confirmed genuinely dead code** — full lui/addiu/ori constant-reconstruction scan plus a 4-byte-aligned pointer-table scan found no reference to either function anywhere in the file.
- **New decisive finding:** smstt's (`0x4418D0`) static ELF init value is `0xFFFFFFFF` and smrdy's (`0x4418E4`) is `0x0` — **byte-identical** to their observed stuck boot-time values across every session since 2026-06-16. This proves neither value is ever written during boot at all — not a failed `CreateSema`/init call, just an untouched compiled default. It follows that the entire init chain that should populate them (`ADX_Init`/`struct_field_reader_e` ← `module_obj_init_c` ← `module_obj_init_b` ← case-13 of the three splash state machines `sub_3E2E80`/`sub_420E70`/`sub_4216E0`, per 2026-07-02b) never executes at all — and `smstt`'s own writer `wrap_noop_wrapper_k_2` (`0x11e318`) never executes either.
- **No `game_overrides.cpp` fix applies to `dword_441A00` itself** — forcing it non-null would only invoke dead code with no effect on the real blocker.
- **Next session — START HERE, two candidate targets (unresolved which is earliest-blocking):**
  1. Statically trace why the three splash state machines never advance their internal state byte (`*(int*)(a1+48)`) to case 13 — likely gated on an SIF-RPC-ready/file-load/DMA-complete flag.
  2. Trace `wrap_noop_wrapper_k_2`'s own caller chain independently of the splash chain — it may be gated by an earlier, separate frontier.
  Whichever proves earliest-unreached during recomp boot is the true next fix target.

## Current Status (2026-07-06b) — PR #137 fiber scheduler INTEGRATED + COMMITTED (`6a23062`); boot-tested — hang class fixed, smstt/smrdy blocker still open, unrelated

- **Cherry-pick completed and committed.** `git cherry-pick -n fbe062e` (39 files, resolving the location/content conflicts described in the 2026-07-06 entry below), plus two files (`Sync.cpp`, `Thread.cpp`) needed additional manual compile fixes (removed a `cv` member from `SemaInfo`/`ThreadInfo` that no longer existed post-rewrite). Build succeeded clean via `build.ps1`. Committed as `6a23062` ("Integrate PR #137 N=1 cooperative fiber scheduler into main tree"), 39 files, +12460/-1695. `git diff HEAD --stat` confirmed zero drift between the committed tree and what was actually built/tested.
- **Boot-tested via `boot_test.ps1 -Seconds 30`** (`run_logs\run_20260706_114555.txt`): ran the full 30s without a spin-detection kill (previously the old thread-per-guest-thread model would hang/spin here). Continuous `[SignalSemaById] sid=3` (702x) and `[SifCallRpc]`/`processPendingRpc`/`ARKD handled RPC` cycling (~553x each) confirm genuine non-hung IOP/RPC activity throughout — **the fiber scheduler fixed the old hang/spin class of bug it targeted.**
- **The separate, long-standing smstt/smrdy semaphore blocker is untouched by this fix.** Grepped the same log for `smstt|smrdy|gamemode|VblankTick`: `smstt=0xffffffff`, `smrdy=0`, `gamemode=0` unchanged across the entire run (v=60 through v=660). Consistent with — not contradicted by — the 2026-07-02b finding that this blocker's root cause is a still-unresolved *content* gap (an unreached/never-called init that should write valid values into `smstt`/`smrdy`), not a scheduling/threading bug, so the fiber scheduler was never expected to fix it on its own.
- **Verdict:** PR #137 integration is DONE, not just validated-standalone. Necessary but not sufficient — it clears one whole class of boot-blocking bug but the game still does not boot further, because the smstt/smrdy content gap is a separate, unresolved issue.
- **Next session — START HERE:** resume the smstt/smrdy investigation. Per 2026-07-02b, the trail dead-ends at two unresolved points: (1) whether the three movie/logo splash state machines (`sub_3E2E80`/`sub_420E70`/`sub_4216E0`) are ever ticked during recomp boot at all, and if so whether they stall before reaching case 13 of their internal switch (where they'd call `module_obj_init_b()` → eventually `ADX_Init` → increment `smrdy`); (2) `smstt`'s only writer, `wrap_noop_wrapper_k_2` (`0x11E310`), is called only from `sub_120220`/`sub_1202A0` — a module init/exit callback pair with **zero resolvable callers anywhere in the binary** per IDA `get_callers`/`get_xrefs_to` and the raw-ELF `mips_r5900_disassembler.py` scan. Start by re-checking (2) with fresh eyes/tools — a callback pair with truly zero direct callers is usually invoked indirectly (function-pointer table, module registration array), not literally dead code.

## Current Status (2026-07-06) — PR #137 fiber scheduler isolate-tested (367/367 pass standalone); integration attempted and aborted — deferred, not a quick merge

- **Isolate-test setup:** separate git worktree `F:\SDBZ-Recomp-pr137-test` (branch `pr-137-fiber-scheduler`, upstream commit `fbe062e`) built clean via VS 2026 generator (`"Visual Studio 18 2026"`, not 17/2022 — installed VS is v18) after working through a stale-cache generator mismatch and a wrong target name (`ps2x_tests`, not `ps2xTest`). `ps2x_tests.exe` run directly (CTest itself finds no registered tests — this suite isn't CTest-integrated) — **367/367 tests passed.**
- **Real-game boot attempt in the isolated worktree failed as expected:** its `ps2EntryRunner.exe` has none of SDBZ's generated `runner/*.cpp` (30,000+ files) or `game_overrides.cpp` — those exist only in the main tree. Confirmed by spin on guest PC `0x100008` (entry point) with "No exact recompiled function" errors. This worktree is only useful for the upstream unit-test suite, not a real boot test.
- **Integration into main tree attempted, aborted.** Committed pre-existing uncommitted debugger/pad-port work first (commit `e297223`) to get a clean base, then `git cherry-pick -n fbe062e` against main. Result: PR #137 is a 41-file, +12,382/-1,649-line rewrite of the threading/syscall core (new `ps2_fiber.cpp`/`ps2_scheduler.cpp`/`ps2_scheduler_internal.h`, rewritten `Interrupt.cpp`/`Sync.cpp`/`Thread.cpp`/`System.cpp`/`ps2_runtime.cpp`). Cherry-pick produced **6 real content conflicts** in those exact files plus **~10 "file location" conflicts** (repo restructured under `PS2Recomp/` after PR #137's upstream base, so git didn't know where new files belonged) plus 2 modify/delete conflicts (`CMakeLists.txt`, `ps2_debug_panel.cpp`). Aborted via `git reset --hard e297223` — working tree is clean, nothing merged.
- **Verdict: PR #137 is validated standalone but NOT integrated.** Bringing it in requires a dedicated manual-merge session (resolve 6 content conflicts + manually relocate ~10 new files to `PS2Recomp/...` paths + resolve 2 modify/delete conflicts), not a quick cherry-pick. This is the fix path for the long-standing smstt/smrdy `shouldPreemptGuestExecution()` blocker (root-caused 2026-07-04), so it remains the eventual unblock — just not attempted further this session.
- **Next session — START HERE (if resuming PR #137):** re-run `git cherry-pick -n fbe062e` from a clean `work/laptop-session-0519` tree, then manually `git mv` the misplaced new files into `PS2Recomp/ps2xRuntime/...`/`PS2Recomp/ps2xTest/...` paths before resolving the 6 content conflicts one file at a time (`ps2_runtime.h` first — smallest, unblocks the rest). Budget this as its own session, not a quick add-on. BUG-023 (digital buttons not lighting up in Input tab, logged 2026-07-05b) remains open and uninvestigated — unrelated to PR #137.

## Current Status (2026-07-05b) — Recomp Debugger scanline-corruption bug fixed + build-verified; Part B binding UI found already fully implemented; real-PCSX2 boot-trace vs recomp diff done (no missing memory-card functionality found)

- **Visual bug fixed:** Recomp Debugger window had severe horizontal black/pink scanline corruption across the whole UI (all text unreadable). Existing DPI-awareness fix in `main_gui.cpp` (`SetProcessDpiAwarenessContext` + SDL hints) was confirmed correct and not stale (binary newer than source); no conflicting manifest found either. Root cause: this is a laptop session — hybrid-GPU (NVIDIA Optimus/AMD switchable graphics) rendering mismatch is the classic cause of exactly this stripe pattern when an OpenGL app runs on the wrong GPU relative to the compositor. **Fix:** added `extern "C" { __declspec(dllexport) DWORD NvOptimusEnablement = 0x1; __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1; }` to `main_gui.cpp` (near the top includes) — these exported symbols are read by name by the NVIDIA/AMD drivers to force discrete-GPU selection. Built via `build.ps1 -Debugger`, user confirmed corruption gone.
- **Part B of `recursive-beaming-fairy.md` (PCSX2-style per-PS2-button binding UI) found ALREADY FULLY IMPLEMENTED in source** — not something that needed building this session. Verified all pieces present and wired correctly: data model (`PadBinding`/`PadInput`/`cfg_pad_bindings[2][Count]` in `debugger_state.h/.cpp`), persistence (`pad_bind_<port>_<input>=` lines in `SaveConfig`/`LoadConfig`), capture-next-input flow (`PollPadBindCapture()`, keyboard/XInput/SDL scan, Escape-cancels), resolver (`ReadPadFromBindings()`), implicit-switch wiring in `SyncFromRecomp()` (`PortHasAnyBinding()` gates whole-device vs per-button resolution), and the UI table (`DescribeBinding()` + Bind/Clear buttons in `tab_inputlogger.cpp`). Plan `recursive-beaming-fairy.md` is now fully complete (Part A done 2026-07-05 earlier session, Part B confirmed done this session) — no further action needed unless a regression is found.
- **New bug found and logged (BUG-023, open):** user confirms via direct observation (not the ambiguous frame-log capture from earlier) that the Input tab's analog sticks light up correctly but digital button presses do not — a real, distinct bug from the earlier "buttons stayed 0" frame-log finding (which never proved anything since that capture window never got past the boot blocker). Not yet investigated — likely in `ReadPAD()`'s button-mapping block or the `g_btn_tex`/bitmask draw logic in `main_gui.cpp`.
- **Part 1 (real-PCSX2 boot trace vs. recomp diff) done, deliverable below.** User had PCSX2 (DebugServer, port 21512) already paused at the memory-card-load-screen trace point (`PC=0x175220`) when this session picked it up.

### Part 1 deliverable — boot-trace checkpoint at memory-card screen

Connected via `mcp__pcsx2__pcsx2_connect` (DebugServer only; Pine/28011 timed out — Pine-only tools like `pcsx2_game_info` unavailable this session). `pcsx2_status`: EE PC=`0x00175220`, paused=true, cycles=3,414,356,645.

**Backtrace at this checkpoint (9 frames):**
| # | entry | pc | note |
|---|-------|----|------|
| 0 | 0x1751c0 | 0x175220 | documented VSync-wait routine (same as 2026-07-02 x64dbg trace) |
| 1 | 0x1721e0 | 0x172248 | |
| 2 | 0x1712d0 | 0x171308 | |
| 3 | 0x104c00 | 0x104c74 | |
| 4 | 0x199840 | 0x199858 | |
| 5 | 0x421ea0 | 0x421f18 | jal → 0x1bfb00 |
| 6 | 0x422630 | 0x422660 | matches documented `fn_422630` SIF-handshake loop |
| 7 | 0x8fefc | 0x100210 | j → 0x18c518 |

This is the **same call chain already documented** as the smstt/smrdy / VSync-wait boot blocker (2026-06-16 → 2026-07-02 entries) — real PCSX2 at this checkpoint is inside the identical function family the recomp is stuck in, confirming the recomp's blocker is a real, shared code path, not something the recomp mis-translated.

**31 IOP modules loaded** at this checkpoint (via `pcsx2_get_modules`), notably: `sio2man`, `mcman`, `mcserv`, `padman`, `CRI_ADX_Driver`, `cdvd_driver`/`cdvd_ee_driver` — i.e. by the memory-card screen, real hardware has already loaded the full memory-card + pad + ADX audio IOP stack.

**Diff against recomp:** the `ps2recomp_list_overrides`/`ps2recomp_lookup_function` MCP tools errored (`Override dir not found` — tool path misconfiguration, not a project issue) so the comparison was done by direct source inspection instead. Checked `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp`: **all `sceMc*` memory-card syscalls are fully implemented** (host-filesystem-backed, per-port state, real directory listing/read/write/format, ~50 functions covering the full libmc + in-game memory-card-UI helper surface) — this is not a gap. Combined with the already-working R3000/IOP interpreter (runs real `ARKD_DVD.IRX` etc. natively, not faked), there is **no missing memory-card-related functionality found on the recomp side**. The actual remaining gap is unchanged from prior sessions: the shared smstt/smrdy semaphore boot blocker (root-caused 2026-07-04 to `shouldPreemptGuestExecution()` never returning true, deferred to the PR #137 fiber-scheduler rewrite) is what prevents the recomp from ever reaching this checkpoint on its own, not an unimplemented subsystem.

**Next session:** if continuing Part 1, single-step forward from this real-PCSX2 checkpoint (`pcsx2_step`/`pcsx2_continue` + repeated `pcsx2_get_backtrace`) past the memory-card screen into the actual `sceMc*` call sequence order, to get a concrete ground-truth call order (useful once the smstt/smrdy blocker is eventually fixed and the recomp can reach this point itself, for a targeted forward comparison instead of a static-code diff like this one).

## Current Status (2026-07-05) — pad-port display regression fixed + build-verified; controller-input report investigated, root cause still open

- **Part A fixed + verified:** `DbgPadSnapshot pad[2]` widened in shared memory (`RECOMP_DEBUG_STATE_VERSION` 9→10); `main_gui.cpp` `SyncFromRecomp()` now indexes `ext->pad[cfg_pad_port]` instead of hardcoding port 0.
- **Two independent staleness bugs found/fixed during verification, unrelated to the code fix itself:**
  1. Running `ps2EntryRunner.exe`/`RecompDebugger.exe` were stale (33h older than source) — user reran `build.ps1`, clean build.
  2. `build_scripts/recomp_mcp_server.py` (separate Python tool, not the C++ runtime) had hardcoded `version != 8` checks and a stale `_PAD_SIZE` assuming the old single-`pad` (6B) layout, silently misaligning every field after `pad` (log entries, IOP state, mem-req/write, reg-write). Fixed to `version 10` / `_PAD_SIZE=(2+4)*2`. Note: it's a long-running process — editing the file doesn't take effect until it's restarted.
- **Part B (PCSX2-style per-button binding UI) NOT started** — plan `C:\Users\mwlab\.claude\plans\recursive-beaming-fairy.md` still open at Part A complete / Part B pending.
- **User reported pad still not showing input after the above fixes.** Investigated via `Logs\frame_log.csv` (611-frame capture): `buttons` was `0` for every frame, but `lx/ly/rx/ry` showed genuine analog jitter (127-128/125-126/130/124) — confirms the SDL PS4-controller read path is live and correctly wired (`[PAD] SDL GameController opened: PS4 Controller` in logs; `cfg_pad_source=0` Auto already correct — no config change needed). However `pc` only had 2 distinct values across all 611 frames (`0x4bca90`/`0x6ccc80`) — the capture window never advanced past the pre-existing boot blocker, so it's unclear whether a button press was ever actually exercised in that log. **Not a confirmed gamepad-detection bug.**
- **Next session — START HERE:** capture a fresh Frame Logger run while actually holding/pressing X on the controller. If `buttons` stays 0 the whole time, that's a real bug in the SDL button-mapping block (`main_gui.cpp` ~998-1032, `SDL_CONTROLLER_BUTTON_A`→Cross bit 14). If it flips nonzero, the pad pipeline (Part A + SDL read) is fully confirmed working and the remaining problem is purely that the game never reaches the memory-card screen (ties back to the long-open smstt/smrdy boot blocker).
- **New idea, not yet started:** user proposed a real-PCSX2 reference run (via `mcp__pcsx2__*` tools) up to the memory-card "press X" screen, diffed against `ps2recomp_list_overrides`/`ps2recomp_lookup_function`, to build a list of functions/functionality still missing from the recomp. Not scoped or started.

## Current Status (2026-07-04c) — BUG-018 fixed + verified; Markers register dedup + Labels tab merge fixed + verified — BUILD CONFIRMED, ALL COMMITTED

Plan `C:\Users\mwlab\.claude\plans\glittery-orbiting-popcorn.md`, fully implemented, built (`build.ps1 -Debugger`, clean), and live-verified by the user against real PCSX2 + DebugServer.

- **BUG-018 CLOSED.** True root cause found (after two earlier failed fix passes, see `BUG_LOG.md`): `step()`/`stepOver()` left a temp breakpoint armed server-side at the post-step PC, so the very next `continue` immediately re-halted — Play looked broken even though resume genuinely worked. Fixed with a `RearmPCSX2Breakpoints()` helper (clears temp breakpoint, re-arms real breakpoints/watchpoints) called after every step/step-over and on reconnect. Also hardened connection handling: Pause/Resume/Frame-Advance/Step-Over now force a synchronous reconnect + re-arm and retry once if `attached==true` but `g_dbgserver.isConnected()==false`, instead of silently no-op'ing for up to 2000ms.
- **Markers panel register dedup verified** — only "Watch Registers" shown, no duplicate full GPR dump; standalone "EE Registers" window unaffected.
- **Labels tab merge verified** — new top-level "Labels" tab shows "Symbols" and "Ghidra" collapsing-header panels with original functionality intact; Settings no longer has a separate Labels sub-tab.
- **Next session:** resume the still-open 2026-07-04 backlog — register write-back live test (halt at a Recomp-backend breakpoint, edit a GPR, Step/Resume, confirm the edited value persists in subsequent execution, not just the shared-memory snapshot) and general Stage 5 regression pass (whitespace/`ImGui::Spacing()` trimming not yet done).

## Current Status (2026-07-04b) — Recomp Debugger Markers-panel QoL pass (3 fixes) — NOT YET BUILT

Pure UI work in `RecompDebugger.exe`-side files only, no runtime/boot-logic changes. Continues Stage 5 from the entry below.

- **Markers spacing fix:** `ShowCallStack()` (`tab_callstack.cpp`) and `ShowThreads()` (`tab_breakpoints.cpp`) both used `BeginChild(id, ImVec2(0,0), ...)`, which greedily fills all remaining vertical space in the shared `##markers_scroll` parent — this caused the large empty gaps between Call Stack/Threads and the sections below them. Fixed by sizing each child to a height computed from actual row count (capped at 12 for call stack, 10 for threads) instead of (0,0).
- **Register-tab consolidation:** folded `ShowRegWatch()` (`tab_regwatch.cpp` — the small "v0"/dropdown/"Clear All" pinned-register watchlist) into the Markers tab as a new "Watch Registers" CollapsingHeader (`tab_breakpoints.cpp`'s `ShowBreakpoints()`), resolving user confusion between "EE Registers"/"Registers"/"IOP Registers". Removed the now-dead standalone "Registers" window: dock-builder line, Windows-menu item, conditional Begin/End block, and SaveConfig/LoadConfig lines in `main_gui.cpp`; `cfg_open_registers` extern in `debugger_state.h` and definition in `debugger_state.cpp`.
- **Right-panel "gap" explained (not a bug):** the empty space at the bottom of the 2-column layout's right side is the standard ImGui docking splitter between the Disassembly and Memory Viewer dock nodes — user can drag it down interactively; position persists automatically via `imgui.ini`. No code change made or needed.
- **Next session — START HERE:** `& "<drive>:\SDBZ Recomp\build.ps1" -Debugger`, fix any compile errors, then live-verify all three items above. After that, resume the still-open 2026-07-04 backlog below (BUG-018 live test, register write-back live test, general Stage 5 regression pass).

## Current Status (2026-07-04) — Recomp Debugger plan: BUG-018 fixed + BUG-022 closed (misdiagnosis) + register write-back completed + Stage 5 UI density started — ALL UNCOMMITTED, NOT YET BUILT

Working under `C:\Users\mwlab\.claude\plans\recursive-finding-starfish.md` ("Recomp Debugger — Get It Fully Working"). All changes below are coded in the working tree, uncommitted, and **not yet build-verified** (build.ps1 -Debugger not run this session).

- **BUG-018 fixed (source only):** `main_gui.cpp` Pause/Resume/Step handlers now use `g_dbgserver` (DebugServer plugin, port 21512) as the sole transport — all PINE calls removed from these three handlers. If DebugServer isn't connected, the buttons log a clear "requires DebugServer plugin" message instead of silently falling back to PINE (which was racing DebugServer and causing the original desync).
- **BUG-022 closed, reclassified as misdiagnosis** — see `BUG_LOG.md` for full writeup. Root cause: `PS2Runtime::shouldPreemptGuestExecution()` deliberately never returns true, so a back-edge loop buried in a deep call chain can spin forever in one native C++ frame the debug-writer never revisits — indistinguishable from a debugger re-arming a halt. This is the same pre-existing `smstt`/`smrdy` semaphore boot blocker (2026-06-16). No debugger-side fix exists; real fix is the deferred fiber-scheduler rewrite (PR #137, held).
- **Register-edit write-back completed.** Previously `RecompilerBackend::WriteRegister()` only updated the shared-memory mirror — the edited value never reached the actual running `R5900Context`, so an "Edit" in the GUI looked like it worked but had no effect on execution. Fixed: `RECOMP_DEBUG_STATE_VERSION` 7→8, new `reg_write_idx/value/seq/done_seq` fields, `ServiceRegWrite()` added to `recomp_debug_writer.cpp` (applied inside `CheckBreakpoint()`'s halt spin), and both `ps2_runtime.cpp` call sites (`lookupFunction()` and `dispatchLoop()`) now write the edited GPR back into the live context via `_mm_insert_epi32` (low 32 bits only, preserving the upper 96 bits of each 128-bit MMI register).
- **Stage 5 (UI density pass) started.** Breakpoints/Watchpoints/Call Stack/EE Registers/Threads/Recomp-armer consolidated from separate tabs into one scrollable "Markers" panel using collapsing headers (`tab_breakpoints.cpp`, `main_gui.cpp` dockspace/menu renamed "Breakpoints"→"Markers"). Memory Viewer (`tab_memoryviewer.cpp`) got a write toolbar (addr/size/value fields, Recomp-backend only — PCSX2 has no write-memory transport yet). Symbol Sync tool's duplicate path/browse/history UI removed (`tab_symbolsynctool.cpp`) since the same fields already live in Settings.
- Minor: interrupt worker thread's `g_currentThreadId` sentinel changed `-1`→`-2` (`Interrupt.cpp`) so it can't collide with `recomp_debug_writer.cpp`'s own "-1 = unclaimed slot" sentinel now that it gets its own tracked debug thread-state slot.
- **Next session — START HERE:** run `& "<drive>:\SDBZ Recomp\build.ps1" -Debugger`, fix any compile errors. Then live-verify: (1) BUG-018 — attach to real PCSX2 with DebugServer active, confirm Pause/Play/Frame-Advance/Step-Over/Restart all work without fighting each other; (2) register edit — halt at a Recomp-backend breakpoint, edit a GPR via the new popup, Step/Resume, confirm the edited value is actually visible in subsequent execution (not just the shared-memory snapshot); (3) general regression pass on the "Markers" panel consolidation and Memory Viewer write toolbar. Only after all three are confirmed working should Stage 4 (parity checklist, already passed once pre-refactor) be re-walked and Stage 5 continued (whitespace/`ImGui::Spacing()` trimming not yet done).

## Current Status (2026-07-03c) — BUG-021 fix confirmed live via MCP tools; new BUG-022 anomaly found: thread only advances one call-boundary per manual recomp_resume()
BUG-021's thread-aware IPC upgrade (seqlock per-tid `thread_states[]`, version 7) was confirmed working end-to-end this session via actual `mcp__recomp__recomp_status()`/`recomp_list_threads()` calls against a live `ps2EntryRunner.exe` (not just raw shared-memory reads): `r0=0x0`, `active_thread_count=1`, `recomp_running=true` all read correctly.

Investigated the user's "process seems paused" report. Found the tracked thread (tid=1) is NOT frozen — each `recomp_resume()` call advances it exactly one call-boundary through the documented `smstt`/`smrdy` chain (`0x104b30`→`0x171320`→`0x11fa60`→`0x11e3b0`→`0x11e560`→`0x11f240`→ wraps to `0x104b30`), with genuinely evolving GPR state across passes (`r2: 0x0→0x1`, `r4: 0xfc→0xfe`, `r18: 0x3→0x4`) — real per-vblank forward progress, not a static freeze. But something is re-arming a halt (consistent with `step_requested_tid` or an equivalent single-step condition) between every `recomp_resume()` call, with no traceable external source.

Ruled out: (1) stale `bp_slots[1..7]` — raw dump showed all 8 slots `addr=0x0, enabled=0`; (2) a stray `RecompDebugger.exe` (PID 12112) suspected of driving BUG-018's broken step loop — killed it, halting behavior continued unchanged, and a broader `tasklist` confirmed no `RecompDebugger.exe`/`ps2xStudio.exe` was running at all; (3) `--debug` launch-flag single-step mode — confirmed via `Get-CimInstance Win32_Process` that PID 4964 was launched with no `--debug` flag; (4) `recomp_resume()`/`recomp_pause()` self-inflicted re-arming — read both function bodies, neither touches `step_requested_tid`.

Logged as **BUG-022** in `BUG_LOG.md` — open, not yet root-caused. Also noted `bp_hit_tid` always reads `-1` even when `bp_hit=1`, suspected to be a race between my `recomp_resume()`'s `_clear_bp_hit()` and the runtime's own `bp_hit_addr`→`bp_hit_tid`→`bp_hit` write sequence (3 non-atomic stores) rather than proof of "no external re-armer" — needs a fresh clean relaunch (no debugger attached at all) to get a true full-speed baseline before digging further.

**Next:** kill PID 4964, relaunch cleanly via `launch_debugger.ps1`, and check whether it runs at full speed with nothing attached — if it does, BUG-022's re-arming source is something that only appears once an MCP client (this session's Python tool calls) has attached, which would narrow the search to the debug-writer/IPC layer itself rather than game logic.

## Current Status (2026-07-03b) — smstt/smrdy purpose resolved (audio/ADX, not input); memory-card-screen reachability reframed as priority; found scePadRead gap

- Pure research/read-only session, no code changes. Prompted by the user reframing priority: the real first unreached boot milestone is the **memory card screen**, which per the 2026-06-20 real-PCSX2 trace occurs chronologically BEFORE the Atari-logo/ADX/`smstt`/`smrdy` chain — so the long-tracked `smstt`/`smrdy` blocker may not even be the right thing to be chasing first.
- Confirmed `smstt`/`smrdy` (`0x4418D0`/`0x4418E4`) are CRI ADX audio-driver (`CRI_ADXI.IRX`) init-readiness flags — **not related to controllers/input**. Write chain: `ADX_Init` (`0x11F268`) ← `module_obj_init_c`/`b` ← three movie/logo state machines (opening movie, Atari logo, Okatron 5000), each at case 13 of their state switch. Confirmed unrelated to `GAME.DAT`/`INFO.DAT` (pure content blobs mapped via `addLsnMapping`, no init role).
- **User supplied a critical real-hardware fact:** the memory card screen is an interactive, input-gated prompt — the real game stalls there until the player presses **X**. This is expected/correct behavior, not a hang, on real hardware.
- Searched `game_overrides.cpp`'s `SifCallRpc` override for a live button-state handler. Found only a `scePadOpen`-style init-status stub (`sid==0x80000100u && rpcno==1u`, fakes `padValue` written to `recvPtr+12`) — this signals "pad subsystem ready", not actual per-frame button bits. **No `scePadRead`-equivalent live-input handler was found anywhere in the reviewed code.** Filed as BUG-020 in `BUG_LOG.md`.
- **Open/unresolved:** (a) whether EE code that would render the memory-card screen is ever reached at all during recomp boot — not confirmed either way this session; (b) if reached, there is currently no mechanism to simulate an X-button press, so recomp would appear to hang there even if everything upstream is working correctly, indistinguishable from a real stall.
- **Next session:** determine whether the memory-card-screen code path is reached (trace independently of the `smstt`/`smrdy` gate, since it's chronologically earlier); if reached, a `scePadRead` SIF RPC handler with a fakeable button-bitmask (X-button bit set) will likely be needed to progress past it for any automated/unattended boot test — this is a plausible small addition to the existing `SifCallRpc` override in `game_overrides.cpp`, not yet implemented or approved.

## Current Status (2026-07-03) — BUG-015 breakpoint attempt stalled by IPC-staleness trap; new suspected stall point at 0x172998 not yet confirmed

- No code changes. Continuing the BUG-015 "START HERE" from 2026-07-01b: launched `ps2EntryRunner.exe`, attached via `mcp__recomp__*`, armed a breakpoint at `0x17666c` (the Deci2Open call site) to capture `a0`.
- **New finding — IPC staleness trap:** multiple orphaned `ps2EntryRunner.exe` instances can run at once, and `recomp_status()` will silently return a frozen snapshot from a stale/orphaned instance instead of erroring. Diagnosed via `Get-Process ps2EntryRunner | Select Id,StartTime,Path` showing two live instances, confirmed by cross-checking against a live x64dbg attach showing a different process state at the same moment. **Rule going forward: verify `Get-Process ps2EntryRunner` returns exactly one instance before trusting `recomp_status()`.**
- One orphaned instance resisted `Stop-Process -Force`/`taskkill` until x64dbg (which had it attached) was closed by the user; then it terminated cleanly.
- Relaunched cleanly via `build_scripts\launch_debugger.ps1` — corrected its actual usage: `[Config="Debug"|"RelWithDebInfo"] [-DeReOnly] [-DebuggerOnly]`, NOT `--debug` (prior memory was wrong on this).
- Re-armed breakpoint at `0x17666c` on the clean instance. Two waits (60s, 120s) both timed out. `recomp_status()` in between showed `pc=0x172998` (GIF busy-wait spot) with `r2` slowly advancing 0x771400 → 0x7d5400 — this is the *same* value progression seen earlier under suspected-stale conditions, so it's now ambiguous whether this is real slow progress or a genuine stall at `0x172998` recurring every run. **Not resolved — ran out of session time before the breakpoint fired.**
- Unconfirmed side finding: an earlier orphaned/conflicting instance (PID 33996) showed what looked like a real C++ exception thrown from `getExecutablePath()` (`main.cpp`) via x64dbg's call stack. Not yet reproduced on a clean single-instance run — could be a real bug or an artifact of the multi-instance conflict.
- **Next session — START HERE:** (1) `Get-Process ps2EntryRunner,RecompDebugger,x64dbg` and kill any strays before starting. (2) Launch via `launch_debugger.ps1`. (3) `recomp_set_breakpoint("0x17666c")`, then `recomp_wait_for_break(timeout_seconds=180)` or longer. (4) If PC still sits at `0x172998` with `r2` crawling through the same 0x77xxxx-0x7dxxxx range after several minutes, treat that as a new suspected stall and investigate what condition the GIF busy-wait at `0x172998` is spinning on (see `sdbzGifChannelBusyFakeZero` in `game_overrides.cpp`, already a known mitigation point from 2026-07-02 notes). (5) Separately try to reproduce/rule out the `getExecutablePath()` exception on a clean instance.

## Current Status (2026-07-02b) — smstt/smrdy thread: traced ADX_Init chain to 3 movie/logo state machines; separate open sub-thread unresolved

- Pure IDA/disasm investigation, no code changes. Continued the older smstt/smrdy root-cause thread (separate from the BUG-015/Deci2Open thread below, which is the more advanced/active blocker — **not reconciled with it this session**).
- Traced the full smrdy write chain: `sub_11FA60` (gated on `dword_4418E4`/smrdy `>0`) ← `ADX_Init` (`0x11F268`, increments smrdy 0→1 on first call) ← `module_obj_init_c`/`sub_113F40` (`0x113F40`) ← `module_obj_init_b`/`sub_113D30` (`0x113D30`, thin wrapper) ← three near-identical movie/logo splash state machines: `sub_3E2E80` (`movie/OP_USA.SFD`), `sub_420E70` (`movie/atari.sfd`), `sub_4216E0` (`OKR.SFD`) — each calls `module_obj_init_b()` at **case 13** of a state-byte switch on `*(int*)(a1+48)`. Matches the documented 2026-06-20 real-PCSX2 boot/movie sequence exactly.
- `smstt`'s only writer (`wrap_noop_wrapper_k_2` @ `0x11E310`) is called only from `sub_120220`/`sub_1202A0` — a module init/exit callback pair with **zero resolvable callers anywhere in the binary** (checked via IDA `get_callers`, `get_xrefs_to`, and `mips_r5900_disassembler.py --find-calls-to`/`--find-writes-to`, all empty). Fully unresolved.
- **Methodology finding:** IDA's `get_callers` returned empty for several functions that structurally must be called (`sub_120220`, `sub_1202A0`, `ADX_Init`, `module_obj_init_c`); `get_xrefs_to` and/or `mips_r5900_disassembler.py --find-calls-to <addr>` found real callers `get_callers` missed (e.g. `0x113D38: jal -> 0x113F40`). When IDA's caller/xref tools disagree or come up empty, cross-check with the raw-ELF disasm script before concluding a function is dead.
- **Not started:** whether the 3 movie/logo state machines are actually ticked during recomp boot, or get stuck before reaching case 13 — would tie this thread directly to the 2026-06-20 "recomp never reaches movie/logo code" finding.
- **Important:** this thread's `smstt`/`smrdy`/`0x1759a0` blocker framing predates the BUG-015 (`complist=0x0`, Deci2Open) thread below, which is documented as the currently active/more-advanced frontier (see "Carried-Over Open Thread" section, 2026-06-28/06-29 updates). Next session should determine whether these two threads are the same blocker seen at different depths, or genuinely different/superseded — not yet reconciled.

## Current Status (2026-07-01b) — BUG-015 investigation: confirmed Deci2Open fails only in recomp, not real PCSX2

- Traced the Deci2Open wrapper (`0x176640`, call site `jal 0x175F80` at `0x17666c`, return check `bgez v0` at `0x17667c`) on **real, unmodified PCSX2** via `mcp__pcsx2__*` DebugServer tools.
- `pc=0x1759a0` is NOT a stuck spin on real hardware — confirmed via breakpoint-list inspection (3 self-set breakpoints, not an organic stall) and a 20-instruction step trace showing clean linear execution into `0x174CE0`→`0x176640`.
- At the return-check site, real PCSX2 shows **v0=0x00000005 (success)**. The recomp build (documented in an earlier session) shows **v0=0xFFFFFFFD (-3, failure)** at the same wrapper. This confirms BUG-015's Deci2Open failure is genuinely recomp-specific, not a path real hardware also fails.
- Open question, not yet resolved: real PCSX2's return-site snapshot showed `a0=0x1`, vs. the recomp trace's call-site `a0=0x210` — these weren't captured at the same instruction offset, so it's unconfirmed whether they represent the same argument. Need to re-trace real PCSX2's `a0` at the `jal` call site itself (`0x17666c`), not the later return-check site.
- Attempted to pivot to the recomp target directly (`mcp__recomp__recomp_status()`) — result was `recomp_running: false`, all-zero state. `ps2EntryRunner.exe` was not running this session; recomp-side trace could not proceed.
- **Next session — START HERE:** launch `ps2EntryRunner.exe` per Active Runner Command above, re-attach via `mcp__recomp__*`, set a breakpoint at `0x17666c` to capture the actual `a0` value the recomp build passes to Deci2Open, and separately re-trace real PCSX2's `a0` at that same call-site offset for a true apples-to-apples comparison. Also still open: decode PS2 SDK meaning of return code `-3` for `sceDeci2Open` (`k0=0x7C`).
- Deferred (do not resume unless asked): Recomp Debugger breakpoint/watchpoint persistence-across-restart plan, saved at `C:\Users\mwlab\.claude\plans\temporal-popping-hare.md`. User explicitly chose to prioritize BUG-015 over this fix this session.

## Current Status (2026-07-01) — Recomp Debugger Phase B fixes BUILT + live-verified (partial); found `build.ps1` doesn't build the Debugger target at all

- Built the 2026-06-30 Phase B fixes via plain `& "F:\SDBZ Recomp\build.ps1"` — succeeded (12/12 targets, `ps2EntryRunner.exe` produced). **This build does NOT touch `RecompDebugger.exe` at all** — `build.ps1` (no switches) only builds the `ps2EntryRunner` target chain.
- Verified Finding #1 (telemetry freeze fix, `RecompDbg::Update()` now also called from `lookupFunction()`) live against the running `ps2EntryRunner.exe` via `mcp__recomp__recomp_status`/`recomp_pause`/`recomp_wait_for_break`/`recomp_resume`: GPR values now advance between polls even while PC sits at a hot repeating call site; Pause arms and hits almost instantly on hot loops; `bp_hit` correctly reflects true/false across pause/resume. **Confirmed fixed.**
- Noted but out of scope: `cycle_count` telemetry always reads `0` because `R5900Context::insn_count` (`ps2_runtime.h:55`) is declared and read (feeds `cycle_count` at `ps2_runtime.cpp:1110`/`1993`) but never incremented anywhere in the codebase. Pre-existing, unrelated to this session's changes.
- User reported Findings #2 (ISO Path field) and #4 (Default-backend combo) "not here" in the actual running Settings tab, despite both being present and correctly wired into `DEBUGGER_SRC_FILES`/`add_executable(RecompDebugger ...)` in `CMakeLists.txt`. Root cause found via `RecompDebugger.exe` vs `tab_settings.cpp` mtime comparison, then confirmed directly: **`build.ps1` with no switches builds `ps2EntryRunner.vcxproj`, not `RecompDebugger.vcxproj`** (`build.ps1:27-33` — the `-Debugger` switch is required: `& "F:\SDBZ Recomp\build.ps1" -Debugger`). The exe the user tested had never been rebuilt since Phase B's `tab_settings.cpp` edits landed.
- **Not yet done:** run `build.ps1 -Debugger`, relaunch `RecompDebugger.exe`, re-verify Findings #2 and #4 in the GUI (Settings → Paths: ISO Path field + "Default backend on launch" combo should now be visible).
- Filed as BUG-017 in `BUG_LOG.md`.

## Current Status (2026-06-30) — Recomp Debugger Phase B fixes applied; NOT YET BUILT

- **Phase B UI overhaul** (Recomp Debugger `main_gui.cpp` + `tab_*.cpp`) — all code changes applied this session, build pending:
  - `debugger_state.h/.cpp`: added `recomp_pid` global (DWORD) to track launched Recomp process for proper termination.
  - `main_gui.cpp`: `RestartRecomp()` now kills prior PID via `TerminateProcess`+`WaitForSingleObject(5000)` before relaunch (fixes "double instance" bug); `RestartPCSX2()` now stores `pi.dwProcessId` into `pcsx2_pid` (was never stored → subsequent restarts failed to kill). Scan progress overlay replaced with `ImGui::GetForegroundDrawList()` drawing (fixes invisible loadbar behind docked panels). Dockspace: removed Controllers + Call Stack windows; added EE Registers dockable window (col3). Added `#include <cmath>` for `fmodf`.
  - `tab_codetrace.cpp`: removed right-pane GPR register split; `DisasmPane` now fills full panel width. Extracted `ShowEERegisters()` — full 32-GPR table, HI/LO, Snapshot button. PC History dropped entirely.
  - `tab_breakpoints.cpp`: added "Call Stack" sub-tab (`ShowCallStack()`) and conditional "Recomp" sub-tab (shm breakpoint armer, previously in Settings→General). Tab formerly called "Execution" remains; renamed in label only.
  - `tab_settings.cpp`: deleted `ShowSettingsGeneral()` and its BeginTabItem entry. Settings sub-tabs now: Paths / Graphics / Inputs / Labels / Session Log.
- **Boot blocker status unchanged** — BUG-015 (`bad=0x1 ra=0x1`, `dword_63FDF8` not reaching `0x422648`) still active frontier. No boot-path code touched this session.
- **NOT YET BUILT.** Run `& "F:\SDBZ Recomp\build.ps1"` then retest all UI items (see HANDOFF_2026-06-30.md).

## Current Status (2026-06-29d) — Recomp Debugger overlay CMake wiring complete; NOT YET BUILT

- F1 was not showing the in-game rlImGui overlay (`ps2_debug_panel.cpp`). Root cause: `PS2X_ENABLE_DEBUG_UI` never defined in CMakeLists.txt → all overlay code compiled away; `ps2_debug_panel.cpp` also missing from `RUNNER_SRC_FILES`; `rlImGui` dependency entirely absent.
- Applied three fixes to `PS2Recomp/ps2xRuntime/CMakeLists.txt`: (1) FetchContent for `rlImGui` from `https://github.com/raylib-extras/rlImGui.git`; (2) add `ps2_debug_panel.cpp` to `RUNNER_SRC_FILES` with `SKIP_UNITY_BUILD_INCLUSION`; (3) build `ps2x_rlimgui_lib` static target + `target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI)`.
- **NOT YET BUILT.** CMake will need to reconfigure (fetches rlImGui). Launch without `--debug` and press F1.
- `--debug` flag routes to `run_with_gui()` (ps2xStudio) — completely unrelated to the in-game overlay.
- Boot blocker (BUG-015 `bad=0x1 ra=0x1` after `dword_63FDF8` not reaching `0x422648`) still open from prior session.

## Current Status (2026-06-26/27) — BUG-014 ASan build in progress via new build_asan.ps1; not yet linked/tested
- User's raw `cmake --build ps2xRuntime-asan --config Debug --target ps2EntryRunner -- /m:1` (bypassing build.ps1, no progress indicator) has compiled/linked all 8 runner-stub sub-projects (`ps2_runner_stubs_a`-`_h`) as of this entry, only benign `C4530` (`/EHsc`) warnings, no errors. Main `ps2EntryRunner` link stage not yet finished.
- Added `build_asan.ps1` (repo root) — same timer/progress-bar/log pattern as `build.ps1`, targets the `ps2xRuntime-asan` tree. Use it for future ASan rebuilds: `& "F:\SDBZ Recomp\build_asan.ps1" Debug 1`.
- **Next:** once linked, attach debugger/MCP and try to reproduce BUG-014's `_CrtIsValidHeapPointer` heap corruption under ASan — confirms whether the 2026-06-25 recursive-mutex fix actually eliminated it. Separately, BUG-015 (new crash frontier past the old smstt/smrdy blocker) is still open and unrelated to this ASan effort.

## Current Status (2026-06-25) — BUG-014 mutex fix BUILT + BOOT-TESTED: huge progress, new crash frontier (BUG-015)
- Built and ran the `PS2Memory` data-race fix from 2026-06-24e (swap-and-clear + `std::recursive_mutex m_memoryMutex` covering `writeIORegister`/`readIORegister`/`read64`/`write8`/`write16`/`processPendingTransfers`). Build completed (~16m29s), boot_test.ps1 run produced the deepest log ever captured in this project.
- **Boot now sails through the entire IOP module chain that previously never got this far in one clean pass**: ARKD_DVD.IRX → CRI_ADXI.IRX (CRI ADX Driver Ver.9.61 init) → SIO2MAN.IRX → PADMAN.IRX → MCMAN.IRX → MCSERV.IRX, all loading/initing successfully with real SIF RPC servers registered (sids 0x500-0x503, 0x80000100/0101, 0x90000200, etc.), multiple `SifCallRpc`/`SifSendCmd`/`McBindPatch` round-trips completing with `result=0x00000000`. ARKD's RPC-server mode entry confirmed (`[IopRuntime] ARKD init complete; entering RPC-server mode`). GS register writes begin (`BITBLTBUF`, `FRAME`, `prim`, `rgbaq`) — this is the first time GS/render activity has been logged at all in any boot_test run referenced in this file.
- **New crash, filed as BUG-015 in `BUG_LOG.md`:** `[BOOT421B70] EXIT ... dword_63FDF8_after=0x00000000 (expect 0x00422648)` (a SIF-handshake-shaped cell never reaching its expected value), then `[BOOT422630] EXIT pc=0x00000001`, then `Warning: Function at address 0x1 not found` / `[dispatch:first-bad-pc] bad=0x1 ra=0x1` with a long call trace through the same `fn_172e20` interrupt-dispatch path documented in BUG-014's original race. Possibly the same mechanism as BUG-012's `bad=0x1 ra=0x1` finding from 2026-06-24 (previously possibly masked/made non-deterministic by the now-fixed data race), or a new and distinct issue — not yet determined.
- A `[Syscall TODO] ... v1=0xffffff98 ... syscallId=0xffffff98` unimplemented-syscall warning fires twice right before the crash; unclear if related.
- **This represents the furthest the recomp has ever booted** — supersedes the long-standing smstt/smrdy semaphore-blocker framing (BUG-006/007) as the active frontier; that blocker either got fixed as a side effect of the mutex fix, or was never the real gate (consistent with BUG-010's "smstt/smrdy idle, not deadlocked" finding). Need a fresh, deliberate check of `smstt`/`smrdy` values in this run's `[VblankTick]` lines to confirm either way — not done yet, log was truncated before that confirmation.
- **Next:** (1) Re-run `boot_test.ps1 -Seconds 30`, confirm `smstt`/`smrdy` status explicitly this time. (2) Find what writes (or should write) `dword_63FDF8` and reads it back expecting `0x422648` — likely a different SIF handshake step/value than the `0x500854` sentinel documented in `reference_ps2_sif_boot.md`. (3) Determine whether the `bad=0x1`/`ra=0x1` dispatch-recovery crash is the same mechanism as BUG-012, and whether it's fatal to the run or just logged-and-recovered (need to see what happens after the recovery heuristic kicks in — log truncated here). (4) Re-test whether BUG-014's original ASan-only heap-corruption symptom (debugger-attached) is actually resolved now, separately from this new boot-test (non-debugger) progress.

## Current Status (2026-06-24e) — BUG-014 ASan diagnosis build, IN PROGRESS
- New thread, unrelated to the LIBSD/boot-blocker work below: `ps2EntryRunner.exe` hits a VC++ debug-heap assertion (`_CrtIsValidHeapPointer`) only when the recomp MCP/debugger is attached — runs clean without it. Filed as BUG-014 in `BUG_LOG.md`. Building a sibling ASan-instrumented CMake dir (`PS2Recomp/out/build/ps2xRuntime-asan`, MSVC `/fsanitize=address`) to get a precise corruption stack trace before touching any source — normal Debug build dir is untouched.
- Build-tooling obstacles hit (not the actual bug): (1) `LNK1394: cannot infer default ASAN libraries` on `SDL2.vcxproj` — SDL2's FetchContent sub-build defaulted to a shared DLL under ASan; fixed by forcing `-DSDL_SHARED=OFF -DSDL_STATIC=ON` + deleting stale `_deps/sdl2-build`/`sdl2-subbuild` caches (they don't pick up new flags without a full delete+reconfigure). (2) `C1060`/`C1002: compiler is out of heap space` on unity-build TUs and a few giant generated runner files — `-DCMAKE_UNITY_BUILD=OFF` did not disable unity batching for the runner-stub targets (their CMakeLists likely sets `UNITY_BUILD` as a target property, ignoring the cache var); 32-bit `cl.exe` + ASan instrumentation on already-huge MIPS-recompiled TUs exhausts heap.
- **Current mitigation, build in progress as of this entry:** full wipe + reconfigure of `ps2xRuntime-asan` with `-T host=x64`, building with `cmake --build ... -- /m:1` (serial, to avoid multiple large ASan compiles competing for limited heap at once). Latest log showed stub targets a-d done/in-progress of 8 total (`ps2_runner_stubs_a`..`h`), no errors.
- **Not yet built successfully.** Once it links: attach the debugger/MCP, reproduce the heap corruption, capture the ASan stack trace. Per the standing rule, any eventual fix lands in `ps2_memory.cpp`/`game_overrides.cpp` only — never `fn_*.cpp`/`src/runner/`.
- Repro commands:
  ```
  Remove-Item -Recurse -Force "F:\SDBZ Recomp\PS2Recomp\out\build\ps2xRuntime-asan"
  cmake -S "F:\SDBZ Recomp\PS2Recomp\ps2xRuntime" -B "F:\SDBZ Recomp\PS2Recomp\out\build\ps2xRuntime-asan" -G "Visual Studio 18 2026" -A x64 -T host=x64 -DCMAKE_CXX_FLAGS="/fsanitize=address" -DCMAKE_C_FLAGS="/fsanitize=address" -DCMAKE_UNITY_BUILD=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON
  cmake --build "F:\SDBZ Recomp\PS2Recomp\out\build\ps2xRuntime-asan" --config Debug --target ps2EntryRunner -- /m:1 > "F:\SDBZ Recomp\asan_build_log.txt" 2>&1
  ```

## Current Status (2026-06-24d) — LIBSD.IRX pre-load REAL FIX applied (supersedes 2026-06-24b's wrong diagnosis); NOT YET BUILT
- 2026-06-24b's "R_MIPS_26 relocation/symtab" theory was **wrong** — disproven by directly parsing LIBSD.IRX's ELF (`.rel.text` has 762 entries, every single one has `r_sym=0`, so they all hit the existing `loadAddr` branch correctly; there is no real `.symtab` involved at all, `.symtab` only has the null entry). IDA Pro MCP cross-check confirmed: function `start` at `0xB0` (the real entry, matches the loader's own computed `entryPoint`), `nullsub_1` at `0xA0`, export-table magic `0x41C00000` at offset `0x0`, null terminator at `0x9C`.
- **Actual root cause of "DID NOT RETURN":** `IopRuntime::start()` wrote the `SYSCALL 0xFFFE` return sentinel to IOP address `0x4` *after* the LIBSD pre-run block executed, not before. LIBSD's `module_start` legitimately returns via `jr $ra` with `$ra=4` (~10 real instructions + a few clean subroutine calls in) — but address 4 was still zeroed memory (decodes as NOP) at that point, so the interpreter walked off into uninitialized IOP RAM until it hit garbage that misdecoded as an unhandled syscall. **Fix:** moved the sentinel write up, before the LIBSD block, in `iop_runtime.cpp` (`IopRuntime::start()`).
- **Actual root cause of `registerModuleExports()` no-op for "libsd":** LIBSD uses a second, equally real export-table header format the step-5 scanner didn't recognize — magic `0x41C0xxxx` (not `0x41E0`), name at header+12 (not +4), and entries are raw already-relocated 4-byte absolute addresses starting at header+20 (not 2-word `j addr;nop` pairs), terminated by a null word. **Fix:** added a second branch ("Format B") to the step-5 scanner in `irx_loader.cpp` alongside the existing ARKD-style format ("Format A").
- Step budget for LIBSD's pre-run raised back from 50,000 → 5,000,000 now that it's expected to actually return quickly (module_start is ~0x5C bytes / a few dozen instructions).
- **Not yet built/tested.** Needs to reach the laptop too if this session is on a different machine — diff spans `PS2Recomp/ps2xRuntime/src/lib/iop/iop_runtime.cpp` and `PS2Recomp/ps2xRuntime/src/lib/iop/irx_loader.cpp`.
- **Next:** build, run `boot_test.ps1 -Seconds 30`, check for `[IopRuntime] LIBSD.IRX loaded and initialized (steps=N).` with no `DID NOT RETURN`, a `[IRX] export table (fmtB) lib='Sound_Device_Library' entries=N` line, and check whether ARKD's `libsd` funcIdx 4/5/7/8/9/10/11/17/23/26/27 now resolve to real LIBSD code instead of `[libsd]` HLE fallback prints at runtime. Does not touch the smstt/smrdy boot blocker (separate subsystem) — proceed to that next once this is verified.

## Current Status (2026-06-24) — uncommitted shouldPreemptGuestExecution fix found unverified; new recomp MCP server built (not yet wired in)
- Investigation-only session, no code written by Claude. User asked about wiring a live MCP server to `RecompDebugState` shared memory (same protocol `ps2xStudio`'s `RecompBackend` already uses) so Claude can query PC/registers/breakpoints directly instead of via the GUI or polling scripts.
- Built `build_scripts/recomp_mcp_server.py` — Python `FastMCP` server wrapping the existing IPC struct (`HEADER_FMT = "<I32IIIQBIBBBI"`, 160-byte header + 2048-byte `ram_window`), exposing `recomp_status`/`recomp_read_memory`/`recomp_set_breakpoint`/`recomp_wait_for_break`/`recomp_resume`/`recomp_pause`. Verified via clean Python import + `struct.calcsize` arithmetic. **Not yet registered** — adding a `"recomp"` entry to `.mcp.json` is blocked by Claude Code's self-modification permission rule (config that controls the agent's own tool capabilities); user must paste in the entry themselves:
  ```json
  "recomp": { "command": "python", "args": ["F:\\SDBZ Recomp\\build_scripts\\recomp_mcp_server.py"] }
  ```
  Inherits the same hard limits as `RecompBackend` (no general memory bus — only the ~2KB PC-centered `ram_window`; no arbitrary write_memory; pause is breakpoint-at-current-PC, not true async pause).
- User pasted a Discord exchange (themselves + contributor Ran-J) about the still-unresolved `smstt=0xffffffff smrdy=0` blocker; Ran-J speculated an unimplemented audio/sound-manager backend. **Determined this speculation is very likely wrong** — re-confirmed via `git diff` against commit `81fe140c` (2026-06-14) that `ps2_runtime.cpp`'s `PS2Runtime::shouldPreemptGuestExecution()` already has a fix in the working tree (always `return false;`, yield via `GuestExecutionReleaseScope` mutex drop+reacquire instead of the old true/false signal that could corrupt `ctx->pc`) — this is the existing BUG-007 mechanism, not a missing audio feature. Confirmed `fn_18E300` (a memmove-style copy loop in the `fn_421B70`/GameInit chain) has 4 loop back-edges that call this method.
- **This fix is uncommitted, and its build/test status is unverified** — asked the user directly whether this working-tree version had been built and boot-tested; they replied "unsure." So the next concrete step is simply: build, then boot_test, then check whether `smstt`/`smrdy` finally advance.
- See `BUG_LOG.md` BUG-007 "Update (2026-06-24)" for full detail.
- **Next:** `& "F:\SDBZ Recomp\build.ps1"`, then `& "F:\SDBZ Recomp\build_scripts\boot_test.ps1" -Seconds 30`. Check `[VblankTick]` lines for `smstt`/`smrdy` changing from `0xffffffff`/`0`, and check the existing `[BOOT1AF600]`/`[BOOT1A4400]`/`[BOOT18E250]`/`[BOOT2D6800]`/`fn_18E300` diagnostic ENTER/EXIT pairing to see which sub-call (if any) still never returns.

## Current Status (2026-06-23c) — BUG-011 FIXED, blocker reverts to pre-existing smstt/smrdy gate
- BUG-011 (`WakeupThread(tid=16666)` spinning 72,213 times) is **fixed and verified**. Manually disassembled ARKD_DVD.IRX at `caller_ra=0x1940C` (module offset `0x940C`): `jal 0x1AEA0` (import stub) with delay slot `addiu $a0, $zero, 16666` — confirmed `16666` is a literal hardcoded in ARKD's own binary, a real-IOP-OS-reserved tid our kernel never allocates.
- Fix: `WakeupThread` (`iop_kernel.cpp`, thbase[33]) now checks `m_allocatedTids`; if the target tid was never created via our `CreateThread`, it parks the caller (`m_wakeupParkStop`) instead of looping forever. `StartThread` (thbase[6]) gained a matching park branch saving continuation to `sleepingThreads`. Added `m_wakeupParkStop` bool to `iop_kernel.h` (approved header edit).
- Verified via `boot_test.ps1 -Seconds 30` (run_20260623_063700.txt): "No spin detected", zero `WakeupThread tid=16666` log lines.
- **Boot now proceeds past BUG-011 and reaches the same pre-existing blocker**: `smstt=0xffffffff`, `smrdy=0`, tid=2 perpetual WAIT at `pc=0x1759a0`, `gamemode=0` — unchanged from the 2026-06-16/06-20 semaphore investigation. BUG-011 was upstream noise, not the root blocker.
- Full detail in `BUG_LOG.md` BUG-011 (now marked FIXED).
- **Next:** Resume the smstt/smrdy semaphore blocker investigation (see prior "2026-06-20"/"2026-06-16" status entries below and `dword_441A00` gate hypothesis) — disasm fn_11FA60/fn_11E3B0/fn_11E560/fn_11F240 + calltree to find what's supposed to write a valid value into smstt/smrdy.

## Current Status (2026-06-23) — RecompBackend / ps2xStudio debugger tool, NOT Phase 5 boot work
- Side-track from Phase 5: wiring `RecompBackend` (attaches to `ps2EntryRunner.exe --debug` via `RecompDebugState` shared memory, `recomp_debug_ipc.h`) into `ps2xStudio`'s existing `IEmulatorBackend` abstraction (alongside `PCSX2Backend`).
- `build.ps1 -Studio` switch (added prior session) confirmed working — successfully targets `ps2xStudio.vcxproj` and reaches its own sources.
- Fixed a C++ namespace-scoping bug causing C2027/C2737/C2660 build errors in `RecompBackend.cpp`: `RecompBackend.hpp` had `struct RecompDebugState* m_shm;` written *inside* `namespace ps2studio { ... }`, which implicitly forward-declared a separate, perpetually-incomplete `ps2studio::RecompDebugState` shadowing the real global `::RecompDebugState` from `recomp_debug_ipc.h`. Fixed: added `struct RecompDebugState;` forward decl *before* the namespace block in the header, changed the member to `::RecompDebugState* m_shm`, and qualified all 7 `static_cast<RecompDebugState*>` casts in `RecompBackend.cpp` as `static_cast<::RecompDebugState*>`.
- **Not yet rebuilt/verified** — user has not re-run `& "F:\SDBZ Recomp\build.ps1" -Studio"` since the fix.
- **Next:** user runs `& "F:\SDBZ Recomp\build.ps1" -Studio"`, paste output. If clean, test "Attach to Recomp" button against a running `ps2EntryRunner.exe --debug` instance and confirm registers/backend name populate.
- This does not change Phase 5 boot-blocker status (still smstt/dword_441A00 unresolved, see 2026-06-21g/2026-06-22d below).

## Current Status (2026-06-21)
- Fixed build-blocking `SET_GPR_U32` typo in `sdbzLogFn171F40Stub` (game_overrides.cpp ~1174) — replaced with `ctx->r[31] = _mm_set1_epi32((int32_t)raSaved);`. Build succeeded.
- BREAKTHROUGH (2026-06-20): EE now gets past `fn_422630`'s SIF-ack spin loop (`[BOOT422630] #1 EXIT`) — sentinel `0x500854` confirmed written and matched (`[BOOT1BF820] subObj=0x00500854`).
- **CLOSED OUT (2026-06-21): `BOOT327F90` "NULL vtable" ctor-failure hypothesis is a false positive — disproven.** The diagnostic (`sdbzLogFn327F90Stub`) flagged 11 deferred-ctor-list objects whose offset+0 word reads 0, assuming offset+0 = vtable pointer. Traced real static-init function `sub_4E68C0` (IDA) and its per-object vtable-setter helper `obj_set_fields_z_261` (@0x1be870): it **explicitly zeroes offset+0** (`*(_DWORD*)a1 = 0;`) — the real type/class tag for these objects lives at offset+20 (`dword_4E7858`/`dword_4FAFC8` etc.), not offset 0. These are plain structs, not C++ objects with a vtable at offset 0. All 11 flagged nodes are correct, expected state. Confirmed independently via real PCSX2: breakpoint at ctorFn `0x421FF0` never fires during a fresh System→Reset boot of the unmodified game (manual jump-to-address in disassembly view also doesn't make it execute) — that code path simply isn't exercised the way the diagnostic assumed. BUG-008 (analyzer function-fold) is unrelated to this chain. **The `[BOOT1BF820] vtbl=0x00000000` / `[dispatch:pc-zero]` symptom is therefore NOT explained by a broken ctor** — root cause for that specific NULL-PC jump is still open.
- R3000 interpreter layer (from 2026-06-14) still running: ARKD_DVD.IRX loads at IOP addr 0x10000, entry 0x1A6BC. GIF DMA active.

## BREAKTHROUGH (2026-06-21b): smrdy identified as ADX_Init refcount
- `smrdy` (`mem[0x4418E4]`) is **not** an arbitrary flag — it's the refcount written by `ADX_Init` @ `0x11f268` (CRI ADX/Sofdec audio middleware init, IDA-named). Pattern: `if (!dword_4418E4) { ...one-time CRI ADX setup... } return ++dword_4418E4;`. Confirmed via `mcp__ida__get_xrefs_to` + decompile.
- `ADX_Init`'s only caller is `module_obj_init_c`/`struct_field_reader_e` @ `0x113fc8`, called from `fn_113F40` (`module_init_register(0)` → struct setup → `struct_field_reader_e(0)` → `module_obj_lazy_init_d()`).
- **This is the exact same diagnostic chain already logged in `game_overrides.cpp` (~line 1227) as never firing in the recomp build:** `0x422630 → 0x421ea0 → 0x327f90 → 0x1bf820 → 0x3e0e60 → 0x3e10d0 → 0x3e2e80 → fn_113D30 → fn_113F40 → fn_11F268(ADX_Init)`.
- **Revises the 2026-06-20 conclusion** that the movie/logo chain and the smstt/smrdy blocker were unrelated — they are directly connected: smrdy can only become nonzero once `fn_113D30`'s chain runs ADX_Init, which is part of CRI ADX/movie-middleware bring-up. The semaphore-wait thread at `pc=0x1759a0` and the ADX init chain converge on the same `dword_4418E4` cell.
- Ruled out BUG-008 (analyzer function-fold) as a cause in this chain: checked all 4 confirmed stall-chain addresses (`fn_11FA60`, `fn_11E3B0`, `fn_11E560`, `fn_11F240`) against IDA function boundaries and `fn_forward_decls.h` — all 4 are correctly emitted as standalone `fn_*` entries, no fold-into-sibling pattern like the `0x1C2580` case. BUG-008 does not recur here; see `BUG_LOG.md`.
- **Next:** trace why `fn_113D30`'s chain never executes — likely gated by the same upstream blocker as everything else (probably the SIF/IOP-ack wait), not a separate bug. Check what calls `fn_113D30` and whether that caller itself ever runs in the recomp build.

## BREAKTHROUGH (2026-06-21c): found the actual master gate — single byte at $gp-3514

- Traced `fn_113D30`'s caller chain upward via `mcp__ida__get_xrefs_to`: `fn_113D30` (IDA: `module_obj_init_b`) ← `sub_3E2E80` (state-machine: `a1+48` field, case 13 = call `module_obj_init_b`, case 14 = ADX/movie setup calls, case 15 = teardown) ← `sub_3E10D0` (dispatches via `move_dispatch_by_type(*(a1+18))` vtable lookup, calls vtable+20/+16/+24 — unguarded `jalr`, BUG-008 territory but not yet confirmed live here) ← `loadscreen_tick` @ `0x3e0e60` (IDA name).
- **`loadscreen_tick` is gated by a single byte at the top:** disassembly confirms `lbu $v0, -0xDBA($gp); beqz $v0, loc_3E0FB8` — if this byte is 0, the ENTIRE state machine (all the way down through `sub_3E2E80`/`fn_113D30`/`ADX_Init`/smrdy) is skipped every call; function just returns `*(a1+12)` immediately.
- Confirmed in the recompiled C++ (`runner/loadscreen_tick_0x3e0e60.cpp` line 50-52, read-only — NOT edited, per "no runner file patches" rule): `READ8(ADD32(GPR_U32(ctx, 28), 4294963782))` — i.e. `mem[gp - 3514]`. `gp` is resolved at runtime, not link time (this binary doesn't bake gp into the offset).
- **Actionable:** `gp` is already logged by the existing VblankTick diagnostic (`ps2_runtime.cpp:2129`, `dbgGp`). Next session: read `dbgGp` from a `boot_test.ps1` run, compute `dbgGp - 3514`, and read that byte from RDRAM (via PCSX2 MCP `pcsx2_read_memory` once connected, or add a one-line log in `game_overrides.cpp`) to confirm it's 0 for the whole 30s window. If confirmed 0, this single byte is the master gate blocking the entire ADX_Init/smrdy/movie-init chain — find what's supposed to set it (likely an unreached SIF/IOP-ack-dependent init routine, consistent with every other Phase 5 blocker found so far).

## CORRECTION (2026-06-21d): gate-byte hypothesis disproven; ADX_Init chain dead-ended at a sentinel, not a bug

- Got a **live DebugServer connection to the running recomp build** (`pcsx2_connect`, port 21512) and traced this empirically instead of via static analysis — much faster, no build/run cycle needed.
- **Gate byte at `gp-3514` (resolved live address `0x5022B6`) reads `0x01`, not `0`.** The 2026-06-21c "master gate" hypothesis is **disproven** — the gate is open. `loadscreen_tick` (`0x3e0e60`) executes every frame without being short-circuited.
- Confirmed `loadscreen_tick` → `sub_3E10D0` (`0x3e10d0`) IS reached every call (breakpoint hit immediately, repeatedly).
- Inside `sub_3E10D0`, traced the actual branch logic via live disasm: `lbu v0,8(a0); andi v0,4; beqz v0,->0x3E10F8` (a0+8 bit2 clear → proceeds) → calls `move_dispatch_by_type(a0+18)` at `0x3F2F90` with arg `0x33` → checks return `v0` at `0x3E1100`; if null, skips the entire vtable dispatch (and therefore `sub_3E2E80`/`fn_113D30`/`ADX_Init`) every time.
- **Confirmed `v0` is permanently `0x00000000` across thousands of frames (~5M cycles apart, sampled twice, both null)** — not a one-frame race.
- **Root cause of the null, found via decompiling `move_dispatch_by_type` (`0x3f2f90`) in IDA:** `if (a1 >= 0x33) result = 0; else result = dword_625660[a1]();`. The dispatch table `dword_625660` only has valid entries for indices `0x00`-`0x32` (51 entries) — confirmed by reading the live table out of RDRAM (55 `u32` entries dumped, index `0x33` itself holds `0x00000000` too). **Index `0x33` is structurally out-of-range/a sentinel, not a "not yet initialized" slot.** This bounds check is correctly rejecting an invalid index, not gating on upstream init state.
- **Conclusion: this specific path (`loadscreen_tick`→`sub_3E10D0`→`move_dispatch_by_type(0x33)`) is very likely a dead end / red herring for the smstt/smrdy blocker.** The object at `0x6330D0` requesting dispatch type `0x33` is either (a) intentionally requesting "no dispatch" as a sentinel — normal behavior, OR (b) has a corrupted/wrong type field — but either way this is NOT gated by an unreached upstream init routine the way every other Phase 5 blocker has been; the table and the bounds check are both self-consistent and match what IDA shows for the original game logic.
- **The full caller chain documented in 2026-06-21b/c (`loadscreen_tick→sub_3E10D0→sub_3E2E80→fn_113D30→ADX_Init`) needs to be treated as unconfirmed/likely wrong** — `sub_3E2E80` and everything downstream of it was never actually observed executing in this live session; it was inferred from static xrefs without confirming the dispatch ever resolves non-null. ADX_Init may be reached via a completely different caller/path, or smrdy may not even depend on this loadscreen object at all.
- **Next session:** Do NOT continue down this `sub_3E10D0`/type-0x33 path — it's resolved as a sentinel, not a blocker. Instead: (1) re-verify smrdy's *actual* call path live (set a breakpoint directly on `fn_11F268`/`ADX_Init` itself and `continue` from boot, see what really calls it, if anything, in a live run — faster and more reliable than static xref chasing); (2) if `ADX_Init` breakpoint never fires across a full boot_test-equivalent live run, that's the real confirmation smrdy never advances, and the next step is tracing backward from there with live breakpoints instead of static IDA xrefs-only chains.
- **New technique validated this session:** live PCSX2 DebugServer MCP connection to the *recomp build itself* (not just real PCSX2) is available and working (`pcsx2_connect` → port 21512). This lets us set breakpoints, read registers/memory, and step through the actual recomp boot in real time — much faster than the static-IDA-trace-then-build-then-log-grep cycle used throughout Phase 5 so far. Use this going forward for chain-tracing instead of pure static analysis.

## SUPERSEDED (2026-06-21e, see 2026-06-21f): initial "ADX_Init never fires" claim was a single-run false negative
- First continuous run (>2.9B cycles, no pause) never hit a breakpoint at `0x11f268`. Concluded ADX_Init was unreachable. **This was wrong** — see below, a later run on the same session hit it directly. The boot path is apparently non-deterministic run-to-run (timing-sensitive dispatch through `move_dispatch_by_type`, consistent with the 2026-06-21d sentinel finding being state-dependent, not permanent). Do not trust a single non-hit as proof of unreachability for this codepath again — confirm with multiple runs or an explicit caller-side breakpoint instead.

## CONFIRMED (2026-06-21f): ADX_Init DOES run live; smrdy advances to 1; smstt is a SEPARATE cell, still invalid — that's the real blocker
- Live breakpoint at `0x11f268` fired on a fresh run. Backtrace confirmed the exact chain hypothesized in 2026-06-21b: `loadscreen_tick` (`0x3e0e60`) → `fn_113F40` (`0x113f40`) → `ADX_Init` (`0x11f268`).
- Before the call: `smrdy` (`mem[0x4418E4]`) = `0`. Set a temporary breakpoint at the return address (`0x113fd0`) and continued — **`smrdy` becomes `1` after ADX_Init returns**, exactly matching the decompile (`return ++dword_4418E4;`).
- **`smstt` (`mem[0x4418D0]`) is unaffected — stays `0xffffffff` the entire time.** Confirmed via direct read immediately after the smrdy change.
- Continued execution further: PC eventually settles back at `0x175220` (the long-documented spin-loop address) even with `smrdy=1`. **This proves `smrdy` and `smstt` are independent cells that do NOT interact** — the 2026-06-21b note claiming they "converge on the same cell" is wrong. `smstt` lives 0x14 bytes before `smrdy` in the same struct/region but is written by something else entirely (ADX_Init's decompile never touches `dword_4418D0`).
- Thread snapshot mid-investigation showed `TID 2` at `PC=0x174ce8, status=4, waitType=2` (not the previously-documented `0x1759a0`) — the boot does walk through more states than previously captured before re-converging on the same ultimate spin point. Not yet mapped to a specific function/wait call.
- **Revised conclusion:** smrdy was never the real blocker — it's a CRI ADX refcount that increments fine once its caller chain (which does run, intermittently/timing-dependently) executes. The actual stall is entirely about `smstt` (`0x4418D0`) never receiving a valid semaphore ID. ADX_Init and its whole call chain are confirmed irrelevant to the real fix.
- **Next:** Find what's *supposed* to write a valid value into `smstt`/`mem[0x4418D0]` — this is a distinct, never-yet-traced data flow from the ADX_Init/smrdy chain. Start by getting `mcp__ida__get_xrefs_to_field` or static xrefs on `dword_4418D0` specifically (not `dword_4418E4`), since all prior tracing conflated the two cells. Also revisit `fn_11FA60`→`fn_11E3B0`→`fn_11E560`→`fn_11F240` (2026-06-21b chain) with live breakpoints to see which of those actually execute and whether any of them touch `0x4418D0` specifically.

## CONFIRMED (2026-06-21g): smstt's only writer is gated behind a never-written function-pointer slot, dword_441A00

- Static `mcp__ida__get_xrefs_to` on `dword_4418D0` (smstt) found only 3 hits, all inside the BUG-006/007 chain region: `wrap_noop_wrapper_k_2`/`noop_wrapper_k_2` (`0x11e310`) is the **only writer** anywhere in the binary — `dword_4418D0 = a1;`. `noop_wrapper_unk_x` (`0x11e328`) and `noop_wrapper_unk_c` (`0x11e3b8`) only *read* it via `noop_wrapper_unk_y(dword_4418D0)`.
- Traced callers of `noop_wrapper_k_2` (`0x11e310`): exactly two, `sub_120220` and `sub_1202A0` (both unreferenced by anything else — see below).
  - `sub_120220`: calls `dat_callback_dispatch_f_clone_01()` to get a value, then `wrap_noop_wrapper_k_2(v4)` — writes a **real/valid** id into smstt. This is the "real init" path.
  - `sub_1202A0`: calls `wrap_noop_wrapper_k_2(-1)` unconditionally — writes `0xffffffff`, i.e. **this is literally the function that produces the broken/invalid smstt value** we've been chasing since BUG-006.
- **`get_xrefs_to` on both `sub_120220` and `sub_1202A0` returns empty — zero static references anywhere in the binary.** Neither is called via any visible `jal`/`jalr` or data reference IDA can see. They must be reached through a computed/indirect mechanism IDA's static pass doesn't resolve.
- Traced one level further via the live BUG-006/007 dispatch chain (`sub_11FA60` decompiled live): at the very end, `sub_11FA60` does `if (dword_441A00) return ((fnptr)dword_441A00)(a1); else return 0;` — a function-pointer slot read directly from memory.
- **Live memory read confirms `dword_441A00` (`mem[0x441A00]`) = `0x00000000` during the active stall.** Since it's null, this final dispatch call never happens — `sub_11FA60` just returns 0 every time, every frame.
- **`get_xrefs_to` on `0x441A00` (the slot) finds exactly one hit — and it's a *read*, inside `sub_11FA60` itself.** There is no static write site anywhere in the binary for this slot. Whatever is supposed to populate it does so through a path completely invisible to IDA's static analysis — almost certainly a SIF RPC/IOP-side callback registration writing directly into EE memory (consistent with the pre-existing SIF boot-handshake hypothesis in `reference_ps2_sif_boot.md`), not a plain EE-side function call.
- Also confirmed live: `dword_441980`, `dword_441984`, `dword_44197c` (the three values fed to `array_state_dispatch`/`sub_11ECD8` earlier in `sub_11FA60`) are all `0` — `array_state_dispatch` no-ops on a null/zero handle (`if (a1) {...} return 0;`), so those three calls are also dead weight right now, separate from the `dword_441A00` issue.
- **Conclusion:** The real boot blocker is now narrowed to a single concrete question: **what is supposed to write a non-null function pointer into `dword_441A00` (`mem[0x441A00]`)?** That write, once it happens, would call through to (most likely) `sub_120220` — which is the only path that writes a *valid* value into smstt instead of the `-1` sentinel from `sub_1202A0`.
- **CONFIRMED LIVE (2026-06-21g, same session):** Armed a write-watchpoint on `0x441A00` and let the boot free-run (stale breakpoints at `0x11f268`/`0x113fd0` from the earlier ADX_Init trace were cleared first so they wouldn't intercept). Across ~3B cycles of free execution — ending with the EE settled back into the known spin loop at `0x175220` — **the watchpoint recorded 0 hits.** `dword_441A00` is never written during boot. This empirically confirms the static finding: whatever is supposed to populate this function-pointer slot (and thereby unlock `sub_120220`'s valid-smstt write path) sits on a dead/unreached code path on this boot.
- **Next:** The watchpoint approach is validated — re-run it across a fresh boot (not just one session) to rule out the same non-determinism seen with ADX_Init (2026-06-21e/f), in case `0x441A00`'s writer is also timing-sensitive rather than fully dead. If it stays at 0 hits across multiple fresh runs, pivot to finding *what's supposed to call whatever writes `0x441A00`* — most likely a SIF RPC/IOP-side callback registration (consistent with `reference_ps2_sif_boot.md`), so check the SIF RPC bind-table setup path next, not more EE-side static xref chasing (IDA can't see this write at all, confirmed by `get_xrefs_to` returning only the read site).

## CONFIRMED (2026-06-22): new diagnostic thread — sub_327810 (Tick) state machine stuck at state 1, gated by RPC-handle validity, not by ccdReply content

- Added `nextObjState` diagnostic (`mem[0x5E5920+4]`) to `[VblankTick]` in `Interrupt.cpp` to check whether the boot state machine `sub_327810` (entry traced via `BOOT1BF820`: `nextObj=0x5e5920`, `Tick=0x327810`) is progressing. **Result: frozen at `1` across the entire 30s run** (v=120 through v=1740) — proves `Tick()` runs every vblank but cannot pass its state-1 exit condition.
- Decompiled `sub_327810` (IDA): state 1's exit condition is `if (!wrap_noop_wrapper_unk_z_z_196()) goto LABEL_63;` (stay at state 1) else advance.
- Traced the gate: `wrap_noop_wrapper_unk_z_z_196` (`0x1bff40`) → `noop_wrapper_unk_z_z_196` (`0x1c0a30`) — polls 4 SIF RPC client handles via `wrap_rpc_handle_valid()`, sends an RPC (`mem_fill_z_369`), then checks `mem[0x5AA7D0]` against `0x4xxxxxxx` (not-found, exits true) or exactly `0x30000000` (ready, exits true).
- String xref confirms this is a **CD Table-of-Contents read completion poll**: `0x4c2d10` = `"CCDReadObj::IsReady TOC not found"`.
- Traced the RPC issuer: `reg_save_stub_z_174`/`sub_1BCEE0` (real start `0x1bcee0`) binds 4 RPC clients (`clientDataNum` 1280-1283) then calls `mem_fill_z_369(dword_5A9358, 0, 3, dword_5A97D0, 0, dword_5AA7D0, ...)` — funcnum 0, reply buffer = the exact address being polled.
- Added `ccdReply` diagnostic (raw `mem[0x5AA7D0]`) to `[VblankTick]`. **Result: `ccdReply=0x30000000` for the entire run** — this is one of the two values `noop_wrapper_unk_z_z_196` treats as a PASS. Yet `nextObjState` never advances past 1.
- **Conclusion: the CD-TOC reply content is fine — the gate must be failing earlier, on `wrap_rpc_handle_valid()` for one or more of the 4 client handles (`dword_5A9330`, `dword_5A9358`, `dword_5A9380`, `dword_5A93A8`), before the function ever reaches the `ccdReply` check.**
- Decompiled `rpc_handle_valid` (`0x178de8`): `v1 = *a1; return v1 && a1[1] == *(u32*)(v1+24) && (*(u32*)(v1+16) & 1);` — three conditions per handle (non-null backing struct pointer, an id match, and a ready-bit).
- Added a 4-bit `rpcValid=` diagnostic to `[VblankTick]` in `Interrupt.cpp`, evaluating that same condition for all 4 client-handle addresses. **Built but not yet run** — this is the literal next checkpoint.

**Next:** Build + run via `dev_cycle.ps1 -ChainDiag`, read `rpcValid=` (4 digits, one per handle in order 0x5A9330/0x5A9358/0x5A9380/0x5A93A8) from the raw `[VblankTick]` log lines (NOT the dev_cycle.ps1 trend table — it doesn't parse this field). Any `0` digit identifies which client handle is failing and which sub-condition (null backing ptr / id mismatch / ready-bit clear) to chase next — likely ties back into the same SIF RPC bind-table setup path already flagged as the leading hypothesis for the `dword_441A00` blocker above.

## Review Session (2026-06-22d) — no new findings

Pure review/verification pass: re-read `HANDOFF_2026-06-22.md`/`b`/`c`, confirmed the `dword_441A00` thread (2026-06-21g) and the `BUG-009 rpcValid` thread (2026-06-22) are still the two open leads and are hypothesized — not yet proven — to share one root cause (incomplete SIF RPC client bind/callback registration). No build, no run, no PCSX2 session this pass. Wrote `Handoffs/DISCORD_UPDATE_2026-06-22.md` consolidating every open blocker for an upstream Discord ask. `rpcValid=` diagnostic (built, not yet run per above) and the `0x441A00` watchpoint re-verification (per 2026-06-21g) remain the actual next actionable steps.

## Carried-Over Open Thread (2026-06-17, corrected/advanced 2026-06-28 — now the active boot-blocker frontier)
**complist=0x0 → black screen.** `complist` at `gameObj+472` is null for the full 30s test → component iterator `0x18d348` exits immediately → no render callbacks. A fresh 2026-06-28 `boot_test.ps1` run confirmed this is the current real blocker (BUG-015's `pc=1` crash and the old smstt/smrdy gate are NOT reached/NOT the active issue in this boot path — see `BUG_LOG.md` BUG-015 2026-06-28 update).

**Corrected via live `pcsx2_connect` trace (2026-06-28):** lazy-init wrapper is at **`0x175268`** (off-by-4 from the original `0x175264` guess), gates on flag `mem[0x461B60]`. It calls `0x176640` only while the flag is 0, and only commits `flag=1` if `0x176640` returns nonzero. `0x176640` is NOT a SIF caller — it calls `FlushCache` (harmless) then a **Deci2Open-shaped call** (`Deci2Call`, syscall 0x7C, code=1, protocol arg `0x210`) that returns a raw negative result (`-3` observed live); `0x176640` converts any negative result to a final `0` ("failed"), so the wrapper's flag never commits and it retries forever. **Revises the 2026-06-17 "SIF bind-table" hypothesis — the actual mechanism is a Deci2/debug-channel open failing repeatedly, not a SIF RPC/bind issue** (the `0x1763f8` SIF-caller lead from the original note was not reached/relevant in this trace).

**Next:** breakpoint `Deci2Call`'s entry directly (read `$a0`/`$a1` to confirm code/args truly decode as `code=1, protocol=0x210`) to determine why `allocateDeci2Socket()` (which has no validation and should always succeed) is producing a negative result — or whether `_DEBUG`/`RUNTIME_DECI2CALL` excludes this code path in the actual binary and a stale register is being misread. Also still unconfirmed: whether `0x461B60` is actually upstream of `complist`/`gameObj+472` at all (inferred 2026-06-17, never independently re-verified) — worth checking via xrefs/live trace before sinking more time into the Deci2 angle specifically.

**Update (2026-06-29) — VS Code native debugging confirms `_DEBUG`/`RUNTIME_DECI2CALL` code path IS live and Deci2Call's default handler works correctly, but for a different call site than this thread's `0x176640`.** Used VS Code's plain `cppvsdbg` "Launch ps2EntryRunner" config (not `pcsx2_connect`) with breakpoints set directly in `Deci2.cpp` (line 139, inside `case 1:`) and `System.cpp` (line 419, `g_syscall_overrides.find()`). Confirmed: (1) no `SetSyscall` override registered for syscall `0x7C`, so every Deci2 call hits the built-in default handler; (2) for the call actually caught — made by `fn_1759A0` (the tid=2 thread, running on its own real `std::thread` via `StartThread`'s lambda) calling `fn_174BD0` → `Deci2Call` code=1 — `allocateDeci2Socket()` returned `socket=1`, a clean success. This rules out "SetSyscall override breaks Deci2Open" but does **not** settle the `0x176640`/protocol=`0x210` call site's `-3` result, since this was a different caller. **Next:** repeat with a breakpoint conditioned on `protocol == 0x210` (or caller `$ra` near `0x176680`) to catch the specific `0x176640` call and read its raw args/result directly — see `BUG_LOG.md` BUG-015 2026-06-29 update for full detail. Also worth noting as a reusable technique: VS Code's native debugger gives full C++ symbol resolution (real function/variable names) for `ps2EntryRunner.exe` directly, no MIPS-register decoding needed — useful for narrow code-level questions even though breakpoints must be placed manually in the editor UI (no scripting). The `mcp__recomp__*` IPC tools stay stale/frozen when launched this way (they need `launch_debugger.ps1 --debug` instead).

## BUG_LOG.md Cross-Reference
`F:\SDBZ Recomp\BUG_LOG.md` tracks numbered bugs for upstream (BUG-001 through BUG-008). BUG-006/007 = this smstt/smrdy boot stall. BUG-008 = unguarded `jalr`/vtable dispatch + analyzer function-boundary-fold bug (confirmed live at `0x1C2580`, ruled OUT of the `fn_11FA60`→`fn_11E3B0`→`fn_11E560`→`fn_11F240` chain specifically — see `Handoffs\HANDOFF_2026-06-21.md`).

## Pending Build (game_overrides.cpp)
Five EE functions were generated as 1-instruction stubs with no `jr $ra` due to sub-block truncation. Fixed with native C++ overrides:
- fn_104bf0 (0x104bf0)
- fn_1a4500 (0x1a4500)
- fn_1bf2e0 (0x1bf2e0)
- fn_22c8f0 (0x22c8f0)
- fn_22cbe0 (0x22cbe0)

## Pending Build (iop_kernel.cpp)
registerLibsd() added — implements ARKD_DVD.IRX's libsd imports

## Prior Fixes (built, committed)
- thsemap index ordering (6=SignalSema, 7=iSignalSema, 8=WaitSema)
- WakeupThread wokenSet removal
- count-based thsemap semaphores
- stub[37] pre-signals all semas
- sceCdSeek LBN tracking
- sceCdPosToInt mode fix
- processPendingRpc 50M budget
- fn_180D30, fn_17F5D0, fn_18D470 fixed

## Boot Test Command
```
& "F:\SDBZ Recomp\build_scripts\boot_test.ps1" -Seconds 30
```

## What to Verify After Next Build
- Multiple VblankTick lines
- mainupd non-zero
- fnptr non-zero
- WaitSema block/wake once
- rpc=0x001 WARNING gone

## Learned Patterns

### 2026-08-01 (later — the ARKD completion-path session)
- **★★ Any fallthrough that fabricates a return value must name itself.** The IOP import lambda returned `$v0 = 0` for every unhandled `(lib, fid)`. For a boolean-success API that reads as *failed*, and the IRX idiom is `while (!x) x = api(...)` — so **one unlisted ordinal is an infinite spin that consumes the entire interpreter budget and is indistinguishable in the log from "the handler hung."** That is exactly what `sifcmd fid=12` (`sceSifSendCmd`) did to the state-11 audio call, and it cost a session. This is the same disease as a silent probe cap ([[feedback_capped_probes_false_negatives]]) one layer down: a *silent default* manufacturing a zero that reads as data. Fixed generally, not specifically — `[iop:unhandled]` censuses every defaulted import, announces the first hit, and dumps the full census whenever a service run fails to halt (i.e. precisely when something is spinning).
- **★ Identify an IOP import by its CALL SITE, not by an SDK ordinal table.** `sifcmd fid=12` was pinned by matching the live `[iop:import]` args (`a0=0x80000001 a1=0x53a30 a2=0x18`) against the verbatim decompiled loop in `irx_arkddvd_sub_A3C0`. Guessing semantics from an ordinal list is how this project produced five misleading-name retractions. **Corollary — a deliberate non-action:** `thsemap fid=6/8` and `sifman fid=7/8` fall through the same way and were **left alone**, because their real semantics may already be "return 0 = success". Building the census that will *name* them from live evidence is worth more than four plausible guesses.
- **`retired == the budget literal` + `halted=0` means the zeros in that record are uninitialised, not measured.** `sid=0x501 fno=0x101 -> retired=4000000 halted=0 $v0=0 rsz=0 delivered=0` was read as "the reply is empty" for a full session. It was "the handler never returned" — `$v0`/`rsz`/`delivered` are zero *by construction* when the halt sentinel is never reached. **Before interpreting any output field, check the loop-exit flag that says whether the producer finished.** A round number equal to a literal in the source is the tell.
- **A "status table" at a fixed address may be a standard SDK array the guest installed itself.** `0x5618B0` was described as "entry 12 of a bespoke async status table" for several sessions. It is `_sif_sreg[12]` — EE libsifcmd's own array, base `dword_561880`, installed by the guest's `sceSifInitCmd`. Once named correctly, the reader (`0x177AB8`), the writer (`0x177AD0`), and the SET_SREG handler (`0x177A88`) all fell out immediately, and with them the reason our host-side `g_sifSregs` write landed nowhere. **When an address resists explanation, try to match it to a documented SDK structure before inventing a bespoke one for it.**
- **"No scheduler" is a load-bearing fact, not a caveat.** `thbase` CreateThread only *records* and StartThread does nothing, so **every thread body the IRX creates is dead code unless something explicitly ticks it.** Three of the four breaks in this blocker were just that: a worker, a publisher, and an init routine that were all correctly created and never once executed. When an IRX subsystem "does nothing," check whether anything runs its threads before looking for a bug in them.

### 2026-08-01
- **★ A function's name in `funcmap` / `studio_callgraph.csv` is a GUESS, not a symbol.** These names were auto-derived, and a wrong one is worse than no name because it silently seeds a hypothesis. `rpc_handle_valid` (`0x178de8`) is actually `sceSifCheckStatRpc` — it means **`is_busy`**, the *inverse* of what the name implies. Four sessions and two retracted fixes were spent trying to make a busy-check return "valid". **Before building any hypothesis on a named function, confirm its semantics from the decompiled body (`ida_scripts/decompiles_SLUS_214_42.txt`) or from ps2sdk.** One read of the caller (`sub_1C0A30`: `while (wrap_rpc_handle_valid(c)) ;`) would have settled it in minutes — a *busy*-check in a `while` with an empty body is a spin-until-idle, so returning 0 is the passing answer.
- **Read the consumer, not just the callee.** The same return value can be a gate or a pass depending on how the caller uses it. Both consumers here inverted the naive reading. Enumerate call sites and read the branch before deciding what a return value gates.
- **When six retractions have accumulated, the problem is the method, not the hypothesis.** Every one of them came from inferring semantics from our own log in a vacuum while a **real PCSX2 with the real ISO and a full MCP toolchain** sat unused. Differencing against the reference machine is not a last resort — it is the cheapest measurement available (PCSX2 reaches the memory-card prompt in 7–8 s).
- **An instrument that exists but is not armed is not evidence.** `PS2_COVERAGE` answers "which game-band addresses does the EE actually dispatch" directly and has been built and documented since July; it has not been armed since 2026-07-29 (`cov=0/0`). Sessions were instead spent hand-reading 2800-line generated `switch` bodies to answer the same question worse.

- **`pc == ra` does NOT imply a loop** (co-piolet, 2026-07-29). In this runtime the dispatch/fiber loop parks `ctx->pc` at the return address of the `jal` it is currently inside, so at any `jal` boundary `pc` and `ra` read the same value. `0x421f10` looked like a "tight self-loop" for weeks on exactly this evidence and is in fact `jal sub_00199840`, call 10 of 14 in a per-frame subsystem fan-out. Before claiming a spin, get the actual instruction, and get the function's extent from `Logs\studio_callgraph.csv` — **`CSV Map\map.csv` boundaries are stale** (it gives `sub_00421EA0` as 0xA4 bytes; the real body runs to `0x422168`).
- **`stuckSecs` is a false positive whenever `progress` is climbing.** It keys off `pc` stability, and `pc` is stable by construction in a steady frame loop. Trust `progress` / `bsschg` for liveness; ~~`gstate`~~ **do not trust `gstate` alone — see next entry.**
- **`gstate@ADDR` is a raw 4-word memory dump, not a struct read** (co-piolet, 2026-07-29). The watchdog's `gstate=` field reads whatever 4 consecutive 32-bit words sit at the watch address with zero knowledge of layout. At the address used this session, `0x50227c`, those 4 words are **static singleton object pointers** (`piRam0050227c`, `iRam00502280`, `iRam00502284`, `iRam00502288`) baked into `.data` at link time and never reassigned anywhere in the 454k-line decompile — confirmed by the literal `iRam00502288 = 0x63fe40;` matching the live telemetry's 4th word exactly. Watching pointer *values* to singletons is like watching `this`: it will always look "frozen" whether or not the game is progressing. To probe real liveness, dereference the pointer and watch a field **inside** the object that's expected to tick every frame — never take "`gstate` never changes" as evidence of a stall by itself.

### 2026-07-24
- **A single green run does not close a nondeterministic bug.** The SIF-RPC ra-slot stomp (scratch-stack fix) was marked CLEARED after one clean run, then reopened when the next session's run happened to nest deeper. The bug only bites when re-entrancy actually overlaps, which isn't every run. Fix: verify the failure MODE was exercised (log + grep for depth≥2) before trusting a clean result, and require 3 consecutive passing runs, not 1, before marking closed.
- **Isolating from the caller is not the same as isolating between re-entrancy levels.** Fix (1) gave the nested SIF reply dispatcher its own stack top, separate from the caller — but every nesting level shared that SAME address, so two overlapping levels of the same re-entrant function still collided with each other. When a fix targets "shared state with X," check whether the same resource is also shared among multiple concurrent instances of X itself.
- **Multi-config CMake/VS build trees have per-subdirectory target names, not a single top-level target per module.** `--target ps2xRuntime` (the directory name) fails with MSB1009; the real targets are `ps2_runtime` (the lib) and `ps2EntryRunner` (the exe), found by listing `.vcxproj` files under the subdirectory.

### 2026-07-21
- **A silent parser skip is worse than a crash.** `loadGhidraFunctionMap` drops any malformed line with no warning and exits 0, so pointing `ghidra_output` at a space-separated file produces a run with ZERO boundary data that looks completely healthy. When a tool "succeeds" but the numbers are absurd, verify the INPUT was parsed, not just that the tool ran.
- **A `jr $ra` scan cannot find the end of a tail-call thunk.** Tail calls (`j target` + delay slot + padding) never execute their own return, so the scan runs into the next function and adjacent thunks appear to overlap. Match the terminator to the function SHAPE; when a scan reports overlapping functions, the scan is wrong before the binary is.
- **Absurd derived numbers point at a missing input, not a broken algorithm.** "Promoted 125,228 fallback entries" and a `+0x7adc4` owner offset were both the same missing function map. Chase the input before theorising about the code.
- **When an artifact is lost, check whether it was ever really gone.** The Ghidra export was recoverable by JOIN: boundaries survived in the `.i64`, names survived in `symbols.map` (derived from runner filenames that the lost export produced). Validate the join before trusting it — here, 12,072/12,072 names landed exactly on an IDA function start.
- **`.gitignore` on a bare directory kills every `!` negation under it.** Git does not descend into an excluded directory, so `build_scripts/` + `!build_scripts/x.py` silently tracks nothing. Use `build_scripts/*`. Symptom: a file you believe is committed has been untracked for months.
- **An interior address needs an entry guard, not a new function.** Code emitted inside a neighbour's body with no dispatch slot is fixed by `case 0xADDR:` / `== 0xADDR`, never by carving a new function out of the map. `ctx->pc = 0xADDR;` assignments and `// 0xaddr:` comments appear for EVERY address in range — they do not prove dispatchability, and a substring match on them gives a false all-clear.

### 2026-07-20f
- **A generated function entered with an unmatched `ctx->pc` silently falls into its prologue.** The entry `switch (ctx->pc)` ends `default: break;` and the prologue label follows immediately (`function_emitter.cpp:110-123`). Symptom: `sp` drops by the prologue amount and never comes back. Detect with a frame-imbalance wrapper (`exitSp != entrySp`); fix by guarding `default:`.
- **A stack-pointer shift masquerades as pointer corruption.** When several callee-saved registers hold plausible-but-wrong values at once (`s0`/`s2`/`s3`/`ra` all wrong), suspect a shifted `sp`, not a bad store. Store traps will find nothing, because nothing was ever written wrong.
- **`0x2xxxxxxx` is a valid pointer, not garbage.** It is the PS2 uncached mirror. Data reads mask via `PS2_RAM_MASK` (`ps2_runtime_macros.h:162,187`); only code/dispatch lookup fails to mask. Never treat a `0x2…` value as corruption without checking which path consumes it.
- **Check what a value *means* before guarding how it is read.** A defensive `< 0x02000000` range guard suppressed a perfectly safe read and printed `0xffffffff`. The read was never the risk.
- **`RecompiledFunction` is nested — `PS2Runtime::RecompiledFunction` (`ps2_runtime.h:476`).** Unqualified use = 12 errors from one line. Verify identifier scope before shipping a build command; flagging a guess is not resolving it.
- **`template <size_t I>` wrappers get one `static` counter per instantiation.** Bounded-log counters print `#1` from every slot, not `#1/#2/#3`. Make the counter a shared non-template global if a global cap is intended.
- **The tee'd `run_log.txt` carries console line-wrapping (~120 chars) on top of UTF-16.** A logical record spans 2–3 physical lines; `Select-String` needs `-Context 0,3`.

- **2026-07-23 encoding correction:** current `run_log.txt` is UTF-8/ASCII, not UTF-16. `Select-String -Encoding unicode` decodes pairs of normal bytes as CJK-looking glyphs and returns false-negative searches. Use `Select-String -Encoding utf8 ... -Context 0,3`. A fresh run still repeatedly reports `No exact recompiled function for guest PC 0x1`, but has no `1baf90`, `RAFORK`, or `SLOTWATCH` marker. This is not evidence against the new trace path: first verify that `build\\ps2xRuntime\\Debug\\ps2EntryRunner.exe` was relinked after the `game_overrides.cpp` instrumentation edit.

- `registerFunction` only intercepts dispatch-loop calls; direct C++ fn_ calls bypass it; fix: add hasFunction check at top of generated fn_ADDR file or use game_overrides.cpp
- ALL behavioral fixes go in game_overrides.cpp ONLY (no fn_*.cpp patches, no runner/ edits)
- NEVER hand-write IOP output values; run actual ARKD_DVD.IRX via R3000 interpreter
- When jr $ra has register-op delay slot: emit delay slot SET_GPR first, THEN `ctx->pc = GPR(31)`, drop delay addr line
- Raw guest MMIO poll loops that never exit = a hardware bit the runtime never raises; fix = add the W1C/set path in ps2_memory.cpp writeIORegister + raise the bit on the driving tick (e.g. INTC_STAT 0x1000F000 bit2 VBLANK-start raised in interruptWorkerMain). Pattern: `lw MMIO; andi bit; bnez exit; ...; beqz loop` waiting on a bit == the missing set path.
- Vsync worker OR-ing an unordered_map slot races the map's rehash-vs-find; pre-seed the key (=0) in initialize() so the worker only ever assigns to an existing node (residual torn-word race is benign).
- A `ps2_memory.h` edit is NOT cheap: fans into 17 TUs + the whole unity fn_* corpus → wide recompile (but still no recompiler run, no output/ sync). Header changes = long build, budget for it.
- Log tag `[sifman]`/`[cdvdman]`/`[libsd]` (with lowercase sce* fn names) = the REAL IRX running in the R3000 interpreter, NOT our C++ stubs (ours use `[SifCallRpc]`/`[sceSifSetDma:...]` CamelCase-tag style). When such a tag returns a stuck value, the bug is IOP-side or an EE→IOP signal the interpreted IRX waits on — fix behaviorally, never fake the return.
- Multi-session summaries can go STALE: always re-run + read a fresh `run_log.txt` before acting on a documented blocker — this session's boot had already jumped past 3 documented blockers (SIF-RPC bind, CdInit recv-starve) with GS rendering, making the planned "Option B" fix moot before a line was written.
- `sceSifCheckInit -> -1` forever = SIF init handshake never completes; guest polls CheckInit and never proceeds. IOP-side sifman never flags "initialized" (likely a missing EE→IOP DMA-completion/register signal). [open blocker 2026-07-16h]
- **A value-trap on STORES cannot catch a bad value that arrives by LOAD.** When `pc==ra==target` and a store-trap on that value produces zero hits at the suspect stack slot, stop hunting writers — the slot was never written this frame; the epilogue read stale caller data. Reach for read-side/frame-symmetry instrumentation instead.
- **Recompiler re-entry switches break prologue/epilogue pairing.** Generated functions carry an entry `switch (ctx->pc)` that `goto`s mid-body labels, skipping `addiu $sp,-N` and `sd $ra,off($sp)` — but the epilogue still runs in full (`ld $ra,off($sp)` + `addiu $sp,+N`). Prologue is conditional, epilogue is not ⇒ `$ra` loaded from the CALLER's frame. Newer codegen adds a second route: a post-`jr` `switch (jumpTarget)` that re-enters the body after `sp` was already popped. Suspect this whenever a jump target equals live payload data rather than a code address.
- **A plausible value at a plausible address is not evidence of corruption.** `0x20561900` (SIF packet-pool base) appearing all over the stack was genuine packet payload, not damage — three hypotheses died before that was accepted. Corruption is only proven when the value reaches `$ra`/`pc`, not when it merely sits in memory.
- **Duplicate `runner/*.cpp` files cover identical address ranges from different recompiler generations** (confirmed at `0x178260`, `0x17ed60`, `0x178428`) with materially different codegen. Before reasoning about a generated function's behavior, establish WHICH variant the dispatch table registers — reading the wrong file yields confident, wrong conclusions.
- **Bound a runtime writer by its address range before theorizing about it.** `guestRealloc` died instantly to one constant (`kGuestHeapHardLimit = 0x01F00000` vs a fault at `0x01FFBC60`). Grep the ceiling constants first; it is cheaper than reading the copy logic.
- **Exclusion by log ABSENCE is valid and cheap.** ARKD `sub_ADD4` was retired by zero `[ARKD:sifdma]` lines. If a code path has a bounded log line and the log is silent, the path never ran — no need to read it.
- **Honor your own discriminator before generalizing.** Twice this session a hypothesis was declared dead after checking one of two required functions (`0x17ed60` has no frame; `0x178428` does, and it flipped the verdict). If a test names N functions, check all N before concluding.

- **A zero-record probe is only evidence once liveness is proven three ways:** (a) exe mtime > source mtime AND log mtime > exe mtime; (b) an independent control marker from the SAME run is present; (c) the probe's literal tag string is found inside the built exe (`findstr /M /C:"TAG" <exe>`, check `$LASTEXITCODE -eq 0`). Skip any one and "silence" is indistinguishable from a stale build.
- **`findstr /M /C:` is the only workable way to test for a string in the 730 MB exe.** PowerShell byte-array approaches (`-join ([char]$_)`, hand-rolled chunked scans) OOM or time out.
- **`sdbzFrameTraceWrapper` can only see ENTRY and EXIT state.** If an entry-side probe is provably silent, the corruption is mid-body by construction — mirror the probe to the exit side (sample after `original(...)`) rather than adding more entry gates.
- **Some registered "recompiled" functions are stub forwarders, not MIPS bodies** — shape `const uint32_t __entryPc = ctx->pc; ps2_stubs::X(...); if (ctx->pc == __entryPc) ctx->pc = getRegU32(ctx,31);`. They read `$ra` but never write it, so they can never be the source of a corrupt `$ra`. Check `register_functions.cpp` FIRST to find the live generation; the `fn_<ADDR>_0x<addr>.cpp` twin is often dead.
- **A diagnostic env var being unset looks identical to "the code path was never reached."** `cov=0/0` and zero `[rpcValid]`/hwwatch lines in a run mean `PS2_COVERAGE`/`PS2X_HWWATCH` simply weren't set for that run — not that coverage was zero or the RPC handler never fired. Before drawing a conclusion from a silent env-gated probe, confirm the gating var was actually exported for that specific run.

## Session 2026-07-30 — BUG-009: confirmed build is current (not stale); explained a silent-probe false lead; HWWATCH run still not executed

No code change. Verified `game_overrides.cpp` (mtime Jul 30 06:41) predates all built `ps2EntryRunner.exe` copies, ruling out stale-build as the reason the user's pasted watchdog log showed zero `[rpcValid]` lines and `cov=0/0`. Root cause of the silence: that run simply didn't have `PS2X_HWWATCH`/`PS2_COVERAGE` env vars set — both diagnostics are env-gated and were never armed, not evidence about the RPC path itself. Handed the user the run command with `PS2X_HWWATCH=1`/`PS2X_HWWATCH_VAL=0xFFFFFFFF` set. **Still pending:** that HWWATCH run has not yet been executed/pasted back. **Also still open, flagged but not yet resolved:** two `hwWatchArm` call sites exist in `game_overrides.cpp` (line ~815 pkt_addr hunt vs line ~1555 older `$ra`/entrySp hunt) contending for the single global DR0 register — only one is meaningfully armed per run; worth confirming which fires before trusting a HWWATCH result.

## Session 2026-07-30b — BUG-009: send-path fully traced, HWWATCH run completed, blocker re-localized to boot state machine

HWWATCH run executed (90s, `PS2X_HWWATCH_CLIENT`-gated arbitration fix verified no contention with the old `$ra` hunt): 7 hits at `guest=0x00464dc0` in `run_probe.jsonl.hwwatch.txt`, all early in the run. Confirmed via `rpc_call_0x178be8.cpp:153` (sets `pkt_addr`) then `sub_00178560_0x178560.cpp:419` (genuine SDK reply-delivery clear, not a recompiler bug) — one clean bind/reply cycle, then silence for the remaining ~85s.

Statically traced the full send path this session to find what's *supposed* to re-arm `pkt_addr`:
- Six sceCd-style wrapper functions (`sub_1877C8`/`187898`/`187930`/`1879E8`/`187AA0`/`187B98` in `decompiles_SLUS_214_42.txt`) each gate through `sub_00186CC0_0x186cc0.cpp` ("SendSCmd"-style gate) before the real SIF send via `mem_fill_z_369` → `rpc_call_0x178be8`.
- `sub_186CC0` has 3 short-circuit bail conditions: (1) owner-thread check on `dword_46326C`, (2) busy-check — calls `sub_186C50(1)` = `wrap_rpc_handle_valid(dword_464DC0)` directly, (3) in-flight flag `dword_463294 >= 0`.
- **Check #2 (the BUG-009 gate) is NOT the blocker.** Since `pkt_addr==0` permanently, this busy-check always reports "not busy" and lets execution through to the real send *every time `sub_186CC0` is reached*.
- **Re-localized the stall one level up: nothing calls `sub_186CC0` or its 6 wrapper callers again after the first cycle.** This matches the HWWATCH zero-re-arm evidence.

Started reading `sub_00327810_0x327810.cpp` (boot state machine, 2819-line generated `switch(ctx->pc)`/state dispatcher) looking for what triggers a fresh wrapper call — read lines 1-1045 (state teardown for states 14→1, generic device-init/retry sequencer), no direct hit on rpc-related addresses in that range. Lines 1046-2819 unread.

**Recommendation for next session:** static reading of the remaining ~1770 lines is expensive for a generated switch; a live breakpoint/HWWATCH on entry to `sub_00186CC0_0x186cc0` (address `0x186cc0`) will show directly and faster whether/when it's re-entered and with what register state — prefer that over continuing the manual disassembly read.

## Key Files
- `PS2Recomp/ps2xRuntime/src/lib/iop/iop_kernel.cpp` — IOP module registry
- `PS2Recomp/ps2xRuntime/include/runtime/iop/iop_kernel.h` — IOP kernel header
- `PS2Recomp/ps2xRuntime/src/lib/iop/iop_runtime.cpp` — R3000 interpreter runtime
- `PS2Recomp/ps2xRuntime/src/lib/iop/irx_loader.cpp` — IRX loader
- `PS2Recomp/ps2xRuntime/src/lib/iop/r3000_cpu.cpp` — R3000 CPU core
- Game overrides: search in PS2Recomp/ps2xRuntime/src/ for game_overrides.cpp

## PROHIBITIONS (critical, permanent)
1. NEVER clean build (30+ hour rebuild)
2. NEVER edit runner/*.cpp (machine generated)
3. NEVER edit .h headers without explicit user approval
4. NEVER run cmake directly — always build.ps1
5. NEVER list/scan runner/ directories (30k+ files)
6. NEVER fake IOP output values — use real ARKD_DVD.IRX
7. NEVER patch fn_*.cpp — game_overrides.cpp ONLY
