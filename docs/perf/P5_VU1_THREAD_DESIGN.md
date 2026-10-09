# P5 design study: VU1 (+ VIF1) on its own thread

Status: **DESIGN ONLY, for user review.** No P5 code exists. Written 10-09 (plan `steady-dreaming-lemon.md`, step 3).
Tags: **V** = verified (code read, static scan or measured log), **H** = hypothesis.

## 1. Why

- **V** Real fight (`launch_recomp -PerfFight`, logs `logs/p4_*`): median 30-33 vbl/s. The game thread is the wall: ~80-85 % busy.
- **V** Game-thread profile: VU1 recompiled code (`vu1rc`) **32 %**, `processVIF1Data` **3.8 %**, plus the XGKICK hand-off (`vu1KickUs` ~0.75 ms/vbl) and GIF packet copies. All of it runs inside one EE store instruction (section 2).
- **V** Removing every non-VU1 overhead still caps at ~44 vbl/s (plan estimate). Small VU1 codegen tweaks have hit diminishing returns (store16 neutral).
- **H** Moving VU1 + VIF1 off the game thread removes ~36 % of its time: 27 ms/vbl -> ~17.5 ms/vbl, i.e. the game thread alone would allow ~55 vbl/s.

## 2. How it works today (V, code read)

1. The EE writes D1_CHCR with STR set (`ps2_memory.cpp` ~2100-2239). Chain mode gathers the whole chain into `chainBuf` (**a copy**), queues it in `m_pendingVif1Transfers`, then calls `processPendingTransfers()` **right away**.
2. `processPendingTransfers` drains GIF -> VIF0 -> VIF1 in that fixed order.
3. `PS2Memory::processVIF1Data` (`ps2_vif1_interpreter.cpp`) walks the VIF codes: UNPACK into VU1 data memory, MPG into VU1 code memory, DIRECT/DIRECTHL to the GIF (PATH2), and MSCAL/MSCALF/MSCNT -> `m_vu1MscalCallback` (`ps2_runtime.cpp:1700-1757`).
4. That callback runs VU1 **to completion** (recompiled program, or the interpreter as fallback). XGKICK packets go to `submitGifPacket(Path1)` -> GifArbiter -> GS-thread queue.
5. FLUSH/FLUSHE/FLUSHA are no-ops (`ps2_vif1_interpreter.cpp:568`): VU1 is always idle by the time the next VIF code runs.
6. The DMA is complete before the EE's store retires. D1_CHCR STR, D_STAT CIS and any DMA-end interrupt all see "done" at kick time.

**Result:** the guest can never see VU1 busy, and every VU1 cycle is game-thread time.

## 3. What the EE can observe (the sync points)

| # | Observation | SDBZ uses it in fights? | Evidence |
|---|---|---|---|
| S1 | D1_CHCR / D_STAT / DMA-end IRQ | yes, ~1 VIF1 + 1 GIF kick per vblank | **V** `[watchdog] dma/s` = 2 x `vbl/s` (`logs/p4_cheap_log2.txt`) |
| S2 | VIF1 registers 0x10003C00-0x10003DFF (STAT, TOP, ITOP, R0-R3...) | not in the fight frame | **V** `eeref refs 0x10003C00`: only in `sub_172278` (GS readback), `sub_173838`/`sub_173888` (register dump, reached from SyncFrame via `sub_102560`; **H** debug-only, measure in P5a) |
| S3 | VU1 memory 0x11008000 / 0x1100C000 | **no** | **V** `eeref refs` 0x11000000, 0x11008000, 0x1100C000: 0 refs |
| S4 | VPU_STAT (`cfc2 vi29`) / FBRST (`ctc2 vi28`) | not in the fight frame | **V** ELF scan: 5 x cfc2 vi29, all in 0x1721xx-0x1734xx (GS readback + VU reset/debug code) |
| S5 | GS results: CSR, SIGNAL/FINISH, local->host | **no syncs in fights** | **V** `[gsthread] sync{csr=0 siglbl=0 local2host=0 gsdirect=0 nativechain=0 other=0}` over a 314 s fight run (`logs/p3_tight_log.txt`) |
| S6 | Presented frame (vblank latch) | every vblank | **V** `ps2xGsThreadVblank` pushes an empty-packet marker; the GS thread latches the frame at it |
| S7 | VIF1 `i`-bit interrupt | not modelled as an EE IRQ | **V** `ps2_vif1_interpreter.cpp:516` only sets STAT.INT; no INTC raise anywhere |
| S8 | VU1 cycle counts feeding EE timing | no | **V** VU1 cycles stay in `m_vu1` / vucap; DMA completion is not delayed by them |

**Key point (V):** in a fight the EE observes nothing of VU1/VIF1/GS except "DMA done" (S1) and the presented frame (S6). The chain is already a snapshot copy (`chainBuf`), so the guest can reuse its source buffers right after the kick.

## 4. Proposed design

### 4.1 The VU worker ("VifVuThread")
- One host thread owns, **in kick order**: VIF1 processing (UNPACK, MPG, DIRECT, MSCAL/MSCNT -> VU1 run), the PATH1/PATH2 GIF submissions, **and PATH3 GIF DMA packets**. Same object state as today (`vif1_regs`, VU1 code/data, `m_vu1`, GifArbiter), touched by one thread at a time.
- The EE side of a D1/D2 kick: gather or snapshot the data exactly as today, set the CHCR/D_STAT/IRQ results **exactly as today** (instant "done"), then push the job onto a ring instead of calling `processPendingTransfers()` for that channel.
- **Normal-mode transfers** (`enqueueTransfer(madr, qwc)`, `fromScratchpad`) read guest memory at process time today. They must be **snapshotted at kick time** when async (one copy; chain mode already copies).
- **Ordering:** PATH3 (GIF DMA) and the vblank marker (S6) go through the **same ring**. So the GS sees exactly today's packet order, and each frame latches with exactly the packets kicked before that vblank.

### 4.2 Sync = wait for the worker to go idle, at fixed guest points
The EE blocks until the ring is empty and the worker is idle (`vuWorkerSync(reason)`) before:
- S2: any read or write of 0x10003C00-0x10003DFF (`PS2Memory` IO handler, a `.cpp`).
- S5: every existing `ps2xGsThreadSync(reason)` call (it waits for the VU worker first, then the GS queue).
- VIF0/VU0 paths that touch shared state (audit item; **H** none in fights).
- Any `PS2Memory` entry that reads or writes VIF1 state or VU1 memory from the EE side: the debug panel, vucap, `getVU1Data()` users (audit list in 4.4).
- Shutdown, save state, tool replays (`gs != t.gs` already syncs).

**Determinism:** the guest sees **the same values at the same guest instruction** as today (DMA "done" at kick, VIF1 state after full processing at any S2 read). Waits only cost host time and never change what the guest reads. The vblank pacer must treat this wait like `g_eeWaitingOnGs`: set a "waiting" flag so the deterministic pacer does not count it as a frozen guest (`ps2x_ee_waiting_on_gs` pattern).

### 4.3 Things that cannot be intercepted cheaply (and the answer)
- **S3/S4 inside generated runner code:** `cfc2 vi29` and direct pointer loads into VU memory compile to plain context or pointer reads in `runner/*.cpp`; hooking them needs the 30 h rebuild. Answer:
  - (a) **V** SDBZ has no S3 refs.
  - (b) S4 readers are not in the fight frame (**H**: confirm with the P5a counter).
  - (c) VU1's VPU_STAT writes (D/T stop bits, `ps2_runtime.cpp:1727/1754`) only matter when FBRST enables D/T bits. If `fbrst` has D/T enabled at kick, **run that job synchronously** (fallback path). VBS1 (busy) stays 0, as today.
- **`cpuContext->vu0_vpu_stat` / `vu0_fbrst`** are read and written by the MSCAL callback. The worker must take a snapshot of `fbrst` at kick time and must not write the EE context. The EE applies the stop bits only in the synchronous fallback.

### 4.4 Thread-safety audit (must be done before code; H until done)
- `PS2Memory` members touched by `processVIF1Data` + VU1 + arbiter: `vif1_regs`, `m_vu1Code/m_vu1Data` (+ code generation counter), `m_path3Masked*`, `m_gifArbiter`, pipeline stats atomics, diag probes (`g_vumat*`, `[vifsrc]`, `[drawsrc]`, `[chainord]` ring: `ps2diag_chainord::resolveSrc` reads an EE-side ring, so it must move into the job or be disabled when async).
- `m_ioRegisters`: the worker must not write it (today VIF1 DMA completion fields are written at kick, EE side **V** lines 2197-2202).
- GifArbiter + the new `g_arbPool` / `g_drainingPkt` (commit `85a998c8`): single-thread assumption. After P5 that thread is the worker, so all GIF submission must move with it (PATH3 included, 4.1).
- vucap recording (`PS2X_VUCAP`) and recomp verify mode (`PS2X_VU1_RECOMP=2`): force synchronous mode.

### 4.5 Switch and fallback
- `PS2X_VU1_THREAD=0` (default **on** only after gates pass) = today's path, unchanged code.
- Forced synchronous when: vucap hot, verify mode, D/T bits enabled, diag probes that read EE state, `PS2X_GS_THREAD=0`.

## 5. Expected gain and the next wall (H, from measured numbers)
- Game thread: about -36 % -> ~17.5 ms/vbl.
- VU worker: VU1 15.5 ms/vbl + VIF1 ~1 ms/vbl + kick ~0.75 ms/vbl = ~17 ms/vbl, so it also caps near **55-58 vbl/s**.
- **Raster becomes the wall.** Today 4 workers x ~210 s CPU in a 314 s run at ~32 vbl/s is ~67 % busy each. Linear scaling saturates near **45-48 vbl/s**. The GS thread already spends ~50 % waiting on raster (`rasterWaitMs` 159 s / 314 s).
- **So P5 alone: ~33 -> ~45 vbl/s (H).** 60 needs P5 **plus** raster throughput (P1 raster v2, more workers, or both).
- Cores: game + GS + 4 raster + VU worker = 7 busy threads on 6 cores / 12 HT (i7-10750H). Expect HT contention. Raster worker count is a knob to re-tune after P5.

## 6. Risks
1. **Hidden EE observation** of VIF1/VU1 state outside S1-S8 (e.g. a VIF1 STAT poll in a menu or loading path) -> a stall, not a wrong value, if the sync hook is in place. Only missing hooks are dangerous, so the audit (4.4) is the gate.
2. **Data races** in `PS2Memory`: the worker and EE share one object. Needs a strict list of worker-owned fields and a debug assert (thread id check) on each.
3. **Presentation timing:** the vblank marker through the ring keeps frames exact. The GS-thread pacing (<= 1 frame behind) now also bounds the worker, so latency stays the same.
4. **Measurement noise:** the pad-script fight's content varies run to run (rastKpx 819-991 at the same tris/vbl, Part 182). P5 gains must be judged on game-thread ms/vbl and on a fixed replay, not only on wall vbl/s.
5. Header churn: all changes should fit in `.cpp` files (`ps2_memory.cpp`, `ps2_vif1_interpreter.cpp`, `ps2_runtime.cpp`, `ps2_gif_arbiter.cpp`). **V** `ps2_memory.h` is included widely; new members there would trigger the long rebuild. Keep worker state in file-static objects keyed by the `PS2Memory*`.

## 7. Reference: PCSX2 MTVU (H, from memory: verify in source before relying)
PCSX2's "MTVU" runs VU1 on its own thread. The EE/VIF side writes unpacked data, microcode and MSCAL requests into a ring. The VU thread executes and sends XGKICK output to the MTGS ring. The EE waits for the VU thread (`WaitVU`) when it reads VU1 state or memory. PCSX2 estimates VU cycles for EE-visible timing; we don't need that, because our DMA completion is already instant (S1, S8). Our design differs: VIF1 unpack also moves to the worker, because our guest never reads VIF1 state in fights, and the whole chain is already a copy.

## 8. Proposed steps (each gated; stop points marked)
- **P5a, measure only (no behaviour change):**
  - Counters for each S1-S5 site: host-us of EE work from each VIF1 kick to the next sync-forcing access, plus a count of S2/S4 hits per fight second.
  - Run `-PerfFight`. **Go/no-go:** S2/S4/S5 hits ~0 per fight second, and the kick-to-sync gap at least ~10 ms.
- **P5b:** `.cpp`-only skeleton behind `PS2X_VU1_THREAD=1` (default off):
  - the ring, the worker, snapshotting of normal-mode transfers, PATH3 + vblank marker through the ring, sync hooks, synchronous fallbacks;
  - thread-id asserts in Debug.
- **P5c gates:**
  - verify_fix (conformance, gsfeature 96, boot);
  - `gs_bench` hash `cfc30917d1085d7f`;
  - `vu1_bench -Verify` (forces sync mode, must stay 0 mismatches);
  - a **frame-hash A/B** between `PS2X_VU1_THREAD=0/1` on a deterministic boot/attract window (latched frames must be identical);
  - `-PerfFight` game-thread ms/vbl.
- **P5d:** flip the default on, then re-tune the raster worker count. Next: raster throughput (P1).

**STOP: review this design before any P5 code.**

## 9. P5a result (10-09, user approved P5a): GO
- **V** `-PerfFight` with `PS2X_P5A=1` (`logs/p5a_fight_log.txt`, 284 fight seconds, median 29 vbl/s, heavy content rastKpx 1055):
  - S2 VIF1 register reads/writes: **0** (max 0). S4 calls of 0x172278 / 0x1731e0 / 0x1733e0: **0**. S5 GS syncs: **0**.
  - 1 VIF1 kick per vblank. Frame 34.7 ms = kick **18.0 ms** + outside **16.8 ms**; the kick ends ~11 ms before the vblank.
- **V** (static) 0x1731e0 is reachable from SyncFrame (`sub_102560` -> `sub_173460`), but only on the 3000-poll timeout path (debug register dump, then a hang loop).
- **H** With the worker: game thread ~17 ms/vbl, worker ~18 ms/vbl -> ~55 vbl/s before raster; raster saturates first (~43-45), as in section 5.
- Next: P5b (needs the user's go).
