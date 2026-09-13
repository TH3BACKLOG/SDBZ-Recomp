# Handoff note -- 2026-09-12 session end

## Headline
**The loadscreen wall is root-caused and experimentally fixed. The SofDec teardown now
completes and matches PCSX2. The game then stalls again one step later -- almost certainly
the same defect class, scheduler-side.**

Full detail: `PS2_PROJECT_STATE.md` **Part 113** and
`memory/project_savepri_poisoned_by_nested_boost.md` (cont.1, cont.2, Next action).

## Root cause (verified: TRACE + CHGPRI probes + oracle A/B)
- `sub_11E598` / `sub_11E620` (CRI enter/exit) boost the CURRENT thread to `[0x4418F0]`=1
  and save its old priority in ONE global, `[0x449210]`.
- `noop_sub_e690` @0x11E690 boosts the SofDec worker from OUTSIDE that bracket
  (`CHGPRI ra=0x11e6e8`). The worker then enters the CRI section already boosted, saves
  **1** as its "original", and every later restore re-pins it at priority 1.
- Worker at pri 1 never sleeps (BAIL C, g36=1) and starves main (pri 24), which holds g36
  inside `sub_155630` and so never clears it. Self-sustaining livelock.
- Oracle `[0x449210]` = 0x19 (25). Ours = 0x1.

## Fix B (in place, keep it)
- `ps2xRuntime/src/lib/ps2_runtime.cpp`, inside the 1 Hz `[thsync]` sampler (~line 5360):
  guarded write `[0x449210] = [0x441908]` (the game's own original).
- `PS2X_FIX_SAVEPRI=0` disables it -- the A/B switch.
- Result: fired once (`was=1 now=25`); nLive 1->0, g36 1->0, slots all-zero (oracle
  signature), w6tick 107 M -> frozen, worker at pri 25.
- Built into `RelWithDebInfo` exe 2026-09-12 12:45. **Not committed.**

## Where it stops now
- `[warn:stat] acc` froze at **4.0167** at t~123 (SofDec init), BEFORE teardown finished.
- Host `progress` collapses ~65x after t=274; **no thread RUNNING** -- t1 and t6 both READY,
  parked at `0x174B30` (ChangeThreadPriority).
- t=296: worker re-boosted to pri 1, `savepri=24 savetid=1` -- same bug, thread 1 as victim.
  Fix B's guard cannot catch it (requires `saveTid == wAtid`).

## Next step
Scheduler side: **why do two READY threads sit parked inside ChangeThreadPriority with none
running?** Start in `ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp` and
`ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` -- both locally modified, diff them first.
Read-only work; no build needed to start. Part 88's "EE scheduler exonerated" is now only
half true.

## Do NOT
- **Do not run long to "wait out" the 83-s timer.** It is frozen, not slow.
- Do not patch BAIL C -- it is a correct re-entrancy guard.
- Do not chase `0x14C8C8` / `0x14E8B0` -- they fired n=0; the hang was upstream in the stop.
- Do not trust `[thsync]` VERDICT strings -- written for the old livelock, now stale.

## Superseded this session
- Part 111's `[0x500728]` gate: NOT the wall -- `rgate=1` measured with the wall still up.
- Previous handoff's stuck fade byte `[0x500E50]` (Part 112): **not re-checked this session.**
  It may be a downstream symptom of this livelock, or it may be the new stall. Unverified --
  worth one read of `[0x500E50]` in the next run's logs before assuming either.
- `PS2X_SKIPFMV`: EXONERATED -- the player leaked with movies played for real.

## Standing rules unchanged
User runs all builds/launches. No runner-file or `.h` edits. See the STOP section of
`PS2_PROJECT_STATE.md`.
