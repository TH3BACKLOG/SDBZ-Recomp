# Upstream catch-up inventory

**Baseline:** `upstream/main` = `d74a3ce1` — *Feature/gs refactor (#204)*, 2026-08-13
**Swept:** 2026-08-15
**Method:** `git diff --name-status HEAD upstream/main` + per-file `--numstat`, read-only.

> `git diff HEAD upstream/main` marks a file `D` when it is present in HEAD and absent
> upstream. That is **not** proof upstream deleted it — most of those files are ours and
> upstream never had them. Each entry below states which it is, verified with
> `git log --diff-filter=D upstream/main -- <file>`.

---

## 1. TAKEN — `ps2xIOP/` (21 files, 5,989 lines)

Upstream PR #170. A self-contained static library of IOP HLE service modules.

| module | lines | what it is |
|---|---|---|
| `src/modules/cri_dtx.cpp` | 1299 | **URPC `0x400`–`0x500` command channel** — the exact protocol we hand-mint for sid `0x90000200` |
| `src/modules/clfile.cpp` | 635 | CRI file layer (open/read/close/setroot/directload) |
| `src/modules/tsnddrv.cpp` | 628 | sound driver |
| `src/modules/mcserv.cpp` | 402 | memory card |
| `src/modules/sdrdrv.cpp` | 335 | SPU2-side driver |
| `src/modules/sound_update_stub.cpp` | 236 | audio tick stub |
| `src/modules/dbcman.cpp` | 113 | DBC manager |
| `src/modules/libsd.cpp` | 63 | EE→IOP SIF-RPC forwarder for sid `0x80000701` |
| `src/plugin_loader.cpp` | 956 | dynamic plugin loading (off by default) |
| `src/iop_subsystem.cpp` | 339 | service registry / dispatch |
| `src/builtin_profiles.cpp` | 150 | per-game bindings |

**Why it was safe to take verbatim:** every include across all 21 files is either stdlib or
its own headers — zero coupling to `ps2xRuntime`. Verified by extracting the union of
`#include` lines. It requires `cxx_std_20`; we are on `CMAKE_CXX_STANDARD 20`.

**Wired in** at `CMakeLists.txt` — one `add_subdirectory("ps2xIOP")` line, guarded by the
existing `PS2X_BUILD_RUNTIME`. It **builds standalone and links to nothing**. Our own R3000
interpreter (`ps2_iop_cpu.cpp` + `ps2_iop_irx_loader.cpp`) still owns the IOP path.

### What it does NOT give us

- **No SDBZ profile.** `builtin_profiles.cpp` covers `slus_201.84`, `SLUS_205.78`,
  `SLUS_203.88`, with sids `0x7D000000`, `0x0000FF01`, `0x00012345`, `0x19740512`.
  None of ours (`0x90000200`, `0x80000003`, `0x80000701`). We would supply a
  `CriDtxBindings`: sid, `urpcObjectBase/Limit/Stride`, `urpcFunctionTableBase`,
  `urpcObjectTableBase`, `dispatcherFunctionAddress`, `rpcServerPoolBase`, `rpcServerStride`.
- **No IOP-side libsd IRX import stubs.** `libsd.cpp` is an EE-RPC forwarder, not the
  fids `[4,5,6,11,17,18,19,20,26,28]` that `CRI_ADXI.IRX` imports. That gap is untouched.

---

## 2. LEFT OUT — and why

### 2a. The bridge: `ps2_iop_host.cpp/.h`, `ps2_iop_transport.h`

The glue that would let `ps2_runtime` actually call into `ps2xIOP`.
Includes `ps2_runtime.h`, `ps2_stubs.h`, `Kernel/Stubs/SIF.h`, `Kernel/Stubs/MemoryCard.h`,
`Kernel/Syscalls/Common.h` — **every one of those has diverged heavily on our side.**
Dropping it in would not compile, and would collide with our IOP path.

This is the integration step, not a copy. It is the gate on using anything in §1.

### 2b. GS refactor (#204) — **do not take**

Upstream **genuinely removed** `ps2_gs_gpu.cpp` in `d74a3ce1` (confirmed via
`--diff-filter=D`), along with `ps2_gs_rasterizer.cpp`, `ps2_gs_memory.cpp`,
`ps2_gs_gpu.h` (489 ln), `ps2_gs_gpr.h` (738 ln), replacing them with
`gs_frontend.cpp` / `gs_cpu_backend.cpp` / `runtime/gs/*`.

Our GS layer is the one validated across ~38 run cycles and through the Stage 5.11 close.
Swapping it wholesale would throw that away to fix nothing currently broken.

### 2c. `ps2_runtime.h` — **do not take (30-hour tripwire)**

67 lines added upstream, **299 of ours removed**. This header is included by all ~4,520
runner TUs. Touching it = full rebuild *and* loss of our work. Hard no.

### 2d. EE scheduler (#184): `ee_scheduler.h` + `Kernel/EeScheduler.cpp`

`ps2_scheduler.cpp` is **ours** — upstream never had it. Their `EeScheduler.cpp` replaces it.

⚠️ `ps2xRuntime/CMakeLists.txt:470` does `GLOB_RECURSE` over `src/lib/Kernel/*.cpp`, so
dropping `EeScheduler.cpp` into that tree **auto-compiles it** and collides with our
scheduler. (`src/lib/*.cpp` is *not* globbed, which is why §2a would merely be inert.)

### 2e. VU1 refactor (#191)

`ps2_vu1_core.cpp` 1698/61, `ps2_vu1.h` 221/10. Previously deferred; our VU1 work is what
closed Stages 5.8 and 5.10. No reason to risk it.

### 2f. `MPEG.cpp` — a merge, not a grab

689 added / 349 deleted across ~50 hunks spanning the whole file, both directions.
Relevant on paper (the `feature/mpeg-decoder` branch carries *"MPEG decoder now identify
that movie has ended and can play again anytime"*), but it needs hunk-by-hunk judgement.
Same for `Audio.cpp` (111/45).

**Also: not on the critical path yet.** The `.sfd` never gets *opened* — the ADX stream
stays in state 1 — so MPEG decode is downstream of the current blocker.

### 2g. Platform ports — `android/`, `vita/`, `ps2_android_runtime.cpp`, `ps2_vita_runtime.cpp`

Additive and harmless, but no value on a Windows x64 target. Skipped deliberately.

### 2h. `ps2xTest` additions

`ps2_iop_tests.cpp` (879), `ps2_vu_tests.cpp` (1045), `fake_iop_plugin.cpp` (350),
`fake_iop_bad_abi.c`, `fake_iop_missing_symbol.cpp`. These test §1 and §2a. They cannot
link without the bridge, and our test suite already has pre-existing failures.

---

## 3. RE-CONFIRMED: our recompiler is ahead

`git diff --numstat HEAD upstream/main -- ps2xRecomp/` is still net-negative toward
upstream. Re-verified against `#204`, and specifically:

- **#194** (MMI2 opcode mapping for PMADDH/PHMADH/PMSUBH/PHMSBH) — our `instructions.h`
  already has the corrected `0x10/0x11/0x14/0x15`.
- **#168** *"advance ctx->pc on fallthrough functions with no terminating branch"* — the
  **truncated function bug** we logged separately. Nothing to take: the diff shows we hold
  the `__entryPc` guard form, plus giant-function `-O1` handling and midasm hooks that
  upstream has since **removed**. Upstream's `shouldPreemptGuestExecution()` →
  `eeCheckpointDue()` rename is part of #184 (§2d), not a fix.

Nothing to take from `ps2xRecomp/`.
