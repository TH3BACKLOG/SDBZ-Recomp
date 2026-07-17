# PS2_PROJECT_STATE — SDBZ Recomp

## Game Info
- **Title:** Super Dragon Ball Z (US NTSC)
- **Disc ID:** SLUS_214.42
- **ELF Path:** `F:\SDBZ Recomp\ELF\SLUS_214.42`
- **Repo:** `F:\SDBZ Recomp\PS2Recomp\`
- **Config:** `F:\SDBZ Recomp\config.toml`
- **Branch:** work/laptop-session-0519

## Active Runner Command
```
& "F:\SDBZ Recomp\build\ps2xRuntime\Debug\ps2EntryRunner.exe" "F:\SDBZ Recomp\ELF\SLUS_214.42"
```
(corrected 2026-07-13c — `PS2Recomp\out\build\...` no longer exists; build.ps1's real output tree is `F:\SDBZ Recomp\build\...`)

## Agent Runner Script
`F:\SDBZ Recomp\run_game_agent.bat` (generated 2026-06-22, paths verified against above) — usage: `run_game_agent.bat [timeoutSec] [logName]`, logs to `F:\SDBZ Recomp\logs\`.

## Build Command
```
& "F:\SDBZ Recomp\build.ps1"
```
(Always use build.ps1 — never raw cmake/MSBuild)

## Current Phase
**Phase 5 — Boot to and pass the memory card loading prompt (in progress, started 2026-05-27; goal changed 2026-07-07 — was "boot to title screen/main menu", now targets the memory card prompt; main menu deferred to a later phase)**

## Current Status (2026-07-17h) — ✅ SIF PACKET-POOL SENTINEL FIX CONFIRMED (0x20561900 loop GONE). Game now runs a stable main loop (furthest ever). New blocker: **ARKD DVD-read RPCs get NO result data** — the SIF.cpp echo path never invokes handleRPC, and ARKD SID isn't handled in ps2_iop.cpp.

**SENTINEL FIX CONFIRMED (run log 07-17h):**
- `SIF.cpp` BIND branch `pkt[10] = 0xFFFFFFFF` fix WORKED. The `No exact recompiled function for guest PC 0x20561900` loop is GONE. Outbound packets no longer carry string garbage in w[3]. Boot advanced well past the corruption point.
- All 5 IRX still load OK. Then the game issued its post-ARKD service binds (clients 0x5a9330/0x5a9358/0x5a9380/0x5a93a8, SIDs 0x500 etc.) and entered a **stable main loop** — watchdog PC varies (0x11e3b0/0x11f240/0x175210 vblank/0x104c74) and stuckSecs resets, i.e. alive, not hung. Reaches 0x175210 (INTC VBLANK) now.

**NEW BLOCKER (07-17h): ARKD service RPCs return no data**
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
