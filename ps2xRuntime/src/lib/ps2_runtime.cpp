#include "ps2_runtime.h"
#include "ps2_dispatch_history.h"
#include "ps2_scheduler.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_iop_cpu.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "Kernel/Stubs/Pad.h"
#include "Kernel/Syscalls/Thread.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_diag.h"
#include "runtime/ps2_guestwatch.h"
#include "recomp_debug_ipc.h"
#include "recomp_debug_writer.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <sstream>
#include <vector>

namespace ps2_stubs
{
    void resetSifState();
}

// Host CPU sampling profiler (src/lib/Kernel/HostSampler.cpp). Declared here
// rather than in a header because a header change forces all ~30,000 runner TUs
// to recompile. No-ops unless PS2X_PROFILE is set.
extern "C" void ps2x_host_sampler_start(void);
extern "C" void ps2x_host_sampler_stop(void);

namespace
{
    // Defined further down in this TU's anonymous namespace; forward-declared
    // here so the ps2_watch value trap below can tag each hit with the calling
    // chain.
    std::string formatDispatchHistory();
}

namespace ps2_watch
{
    // Out-of-line definitions for the forward decls in ps2_runtime.h. Kept
    // here (rather than header-only in ps2_guestwatch.h) so ps2_runtime.h
    // only needs a tiny forward declaration and never pulls in
    // runtime/ps2_guestwatch.h -- that header is included only by this .cpp.
    std::atomic<bool> g_writeWatchActive{false};

    // [trapval] value-triggered store trap. Armed from PS2X_TRAPVAL (see
    // PS2Runtime::run). Unlike the address-keyed Watch registry above, this
    // fires on the VALUE being stored, so it can find a writer whose
    // destination address is not known in advance (e.g. a clobbered $ra slot
    // on a stack frame whose address moves run-to-run).
    //
    // Accuracy note: ctx->pc is set at function entry / midasm hooks /
    // control-flow points, NOT per instruction. A hit therefore names the
    // writing FUNCTION (usually the basic block), not the exact store.
    std::atomic<uint32_t> g_trapValue{0};
    std::atomic<uint32_t> g_trapAddrLo{0};
    std::atomic<uint32_t> g_trapAddrHi{0xFFFFFFFFu};
    std::atomic<bool> g_trapArmed{false};
    std::atomic<uint32_t> g_trapHits{0};
    static constexpr uint32_t kTrapHitLimit = 256u;

    void onGuestWrite(uint32_t addr, uint32_t size, uint64_t lo, uint64_t hi, uint32_t pc) noexcept
    {
        if (g_trapArmed.load(std::memory_order_relaxed))
        {
            const uint32_t want = g_trapValue.load(std::memory_order_relaxed);
            const uint64_t lanes[4] = {lo & 0xFFFFFFFFu, lo >> 32, hi & 0xFFFFFFFFu, hi >> 32};
            const uint32_t laneCount = (size + 3u) / 4u;
            for (uint32_t i = 0; i < laneCount && i < 4u; ++i)
            {
                if (static_cast<uint32_t>(lanes[i]) != want)
                {
                    continue;
                }

                const uint32_t laneAddr = addr + (i * 4u);
                if (laneAddr < g_trapAddrLo.load(std::memory_order_relaxed) ||
                    laneAddr >= g_trapAddrHi.load(std::memory_order_relaxed))
                {
                    continue;
                }

                const uint32_t hit = g_trapHits.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (hit > kTrapHitLimit)
                {
                    break;
                }

                std::cerr << "[trapval] hit#" << std::dec << hit
                          << " addr=0x" << std::hex << laneAddr
                          << " size=" << std::dec << size
                          << " lane=" << i
                          << " pc=0x" << std::hex << pc
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
                break;
            }
        }

        // Bail before the mutex when no address-keyed watch is registered --
        // otherwise arming the value trap alone would serialize every store.
        if (watchCountRef().load(std::memory_order_relaxed) == 0u)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        const uint32_t writeEnd = addr + size;
        for (Watch &w : registry())
        {
            const uint32_t watchEnd = w.addr + w.byteWidth;
            if (addr < watchEnd && w.addr < writeEnd)
            {
                w.writerPc = pc;
                w.hasWriterPc = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// [frametrace] bounded call-frame ring.
//
// The recompiler registers interior ("resume") addresses of a function as
// their own dispatch-table slots aliasing back to the same function, and the
// generated entry `switch (ctx->pc)` then goto's PAST the prologue while the
// epilogue still runs unconditionally. On a legitimate resume that is correct
// -- $sp is still the already-decremented frame. If dispatch reaches one of
// those slots OUTSIDE a legitimate resume, the epilogue's `ld $ra, N($sp)`
// reads the CALLER's frame instead.
//
// Mid-body resume entries are normal and frequent (every `jal` returns through
// one), so per-call logging would flood the log. Instead we keep a bounded ring
// and dump it once, at fault time, next to the [gpr]/[stack] windows.
//
// Symbols are intentionally non-static: game_overrides.cpp declares them
// `extern` (the sanctioned cross-TU pattern -- touching a header would force a
// 30+ hour rebuild of every generated runner TU).
// ---------------------------------------------------------------------------
namespace
{
    struct FrameTraceEntry
    {
        uint32_t funcStart;
        uint32_t entryPc;
        uint32_t entrySp;
        uint32_t exitPc;
        uint32_t exitSp;
        uint32_t exitRa;
    };

    constexpr uint32_t kFrameTraceCount = 64u;
    FrameTraceEntry g_frameTraceRing[kFrameTraceCount] = {};
    std::atomic<uint32_t> g_frameTraceWrite{0};
    std::atomic<uint32_t> g_frameTraceAlarms{0};
    constexpr uint32_t kFrameTraceAlarmLimit = 16u;
}

std::atomic<bool> g_ps2FrameTraceArmed{false};

void ps2FrameTraceRecord(uint32_t funcStart, uint32_t entryPc, uint32_t entrySp,
                         uint32_t exitPc, uint32_t exitSp, uint32_t exitRa) noexcept
{
    if (!g_ps2FrameTraceArmed.load(std::memory_order_relaxed))
    {
        return;
    }

    const uint32_t slot = g_frameTraceWrite.fetch_add(1, std::memory_order_relaxed) % kFrameTraceCount;
    g_frameTraceRing[slot] = FrameTraceEntry{funcStart, entryPc, entrySp, exitPc, exitSp, exitRa};

    // The derail signature itself: control leaving into the SIF packet-pool
    // base. Report immediately (bounded) so the line survives even if the run
    // never reaches reportMissingFunction.
    if (exitPc == 0x20561900u || exitRa == 0x20561900u)
    {
        const uint32_t n = g_frameTraceAlarms.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (n <= kFrameTraceAlarmLimit)
        {
            std::cerr << "[frametrace:ALARM] #" << std::dec << n
                      << " func=0x" << std::hex << funcStart
                      << " entryPc=0x" << entryPc
                      << " entrySp=0x" << entrySp
                      << " exitPc=0x" << exitPc
                      << " exitSp=0x" << exitSp
                      << " exitRa=0x" << exitRa
                      << (entryPc != funcStart ? " MIDBODY" : "")
                      << (exitSp != entrySp ? " SPDELTA" : "")
                      << std::dec << std::endl;
        }
    }
}

namespace
{
    // Chronological dump of the ring (oldest surviving entry first).
    void dumpFrameTrace(std::ostringstream &oss)
    {
        if (!g_ps2FrameTraceArmed.load(std::memory_order_relaxed))
        {
            return;
        }

        const uint32_t written = g_frameTraceWrite.load(std::memory_order_relaxed);
        if (written == 0u)
        {
            oss << "\n[frametrace] ring empty -- no wrapped function was entered before the fault";
            return;
        }

        const uint32_t live = (written < kFrameTraceCount) ? written : kFrameTraceCount;
        const uint32_t first = written - live;
        oss << "\n[frametrace] " << std::dec << live << " of " << written << " calls (oldest first)";
        for (uint32_t i = 0; i < live; ++i)
        {
            const FrameTraceEntry &e = g_frameTraceRing[(first + i) % kFrameTraceCount];
            oss << "\n[frametrace] #" << std::dec << (first + i)
                << " func=0x" << std::hex << e.funcStart
                << " entryPc=0x" << e.entryPc
                << " entrySp=0x" << e.entrySp
                << " exitPc=0x" << e.exitPc
                << " exitSp=0x" << e.exitSp
                << " exitRa=0x" << e.exitRa
                << (e.entryPc != e.funcStart ? " MIDBODY" : "")
                << (e.exitSp != e.entrySp ? " SPDELTA" : "");
        }
        oss << std::dec;
    }
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    // -----------------------------------------------------------------------
    // Async callback stack pool: [kAsyncCallbackStackFloor, kAsyncCallbackStackTop)
    // — KERNEL-RESERVED memory, carved downward from the top.
    //
    // Placed below the ELF load base so it is disjoint from the game-chosen
    // main stack at top-of-RAM — SDK crt0 calls SetupThread to place that
    // stack at the top of user RAM — so
    // host-dispatched guest callbacks (the sceGsSyncVCallback chain, INTC
    // handlers, alarms) never interleave, via the N=1 scheduler's token
    // handoff, with the game's own live stack frames.
    //
    // On real hardware these contexts run on KERNEL stacks in kernel-reserved
    // memory — never on the interrupted user thread's stack. The EE kernel
    // owns phys [0, 0x00100000): this runtime's kernel-mirror state sits
    // below 0x00012000, the ELF loads at 0x00100000+, the guest heap starts
    // at the ELF's bss end, and guest thread stacks are game-chosen addresses
    // >= 0x00100000. [kAsyncCallbackStackFloor, kAsyncCallbackStackTop) is untouched by both sides, so
    // callback stacks there are disjoint from ALL guest memory by
    // construction.
    //
    // kAsyncCallbackStackFloor/Top themselves live in ps2_runtime.h (next to
    // kAsyncCallbackFallbackSp) so the member default initializers below and
    // this file's re-arm site share one definition instead of three.
    // -----------------------------------------------------------------------

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    // Fiber-owned when running inside a fiber; per-OS-thread fallback otherwise
    // (borrowed host workers, executor between fibers, direct non-fiber callers).
    DispatchHistory &currentDispatchHistory()
    {
        return ps2sched::current_dispatch_history();
    }

    // ps2sched's guest clock: one tick per 128 guest back-edges. Defined in
    // ps2_scheduler.cpp and declared here rather than in ps2_scheduler.h,
    // because editing that header would force a full rebuild of the ~30,000
    // generated runner translation units (§3 prohibition). Used by the watchdog
    // to report whether the guest is executing at all.
    extern "C" uint64_t ps2x_guest_progress();

    // Same no-header rule. ps2x_guest_busy_ns/ps2x_guest_resumes are defined in
    // ps2_scheduler.cpp (executor resume bracket); ps2x_vblank_ticks in
    // Kernel/Syscalls/Interrupt.cpp (vblank delivery point). Together with
    // ps2x_guest_progress they let the watchdog separate "the guest executes
    // slowly" from "the guest is idle waiting" -- see the watchdog comment.
    extern "C" uint64_t ps2x_guest_busy_ns();
    extern "C" uint64_t ps2x_guest_resumes();
    extern "C" uint64_t ps2x_vblank_ticks();

    // Structured probe sink (Phase B), defined in game_overrides.cpp. Same
    // extern-between-.cpp rule as above -- no header, no 30h rebuild.
    extern "C" void ps2x_probe_kv(const char *name, int n,
                                  const char *const *keys, const uint64_t *vals);

    // Diagnostic-only (PS2_PC_WATCHDOG): the most recent guest PC handed to
    // lookupFunction(), globally across all threads. The outer-dispatch snapshot
    // (m_debugPc) freezes at a function's entry PC while that function's whole
    // call tree runs, so it cannot reveal WHERE inside the tree a spin lives.
    // This atomic keeps advancing on every table-dispatched call, so the last
    // value observed before a freeze names the deepest function reached.
    std::atomic<uint32_t> g_lastDispatchPc{0u};

    // Cross-thread snapshot ring (diagnostic-only, PS2_PC_WATCHDOG). The per-thread
    // DispatchHistory above is fiber/OS-thread-owned, so the watchdog (its own OS
    // thread) reads its own empty history. This global ring keeps the last N
    // table-dispatched PCs written by ANY thread so the watchdog can print the
    // guest's actual spin loop body. Racy by design (approximate ordering is fine
    // for a stuck-loop trace); relaxed atomics, no locking on the hot path.
    constexpr uint32_t kGlobalDispatchRingSize = 32u;
    std::array<std::atomic<uint32_t>, kGlobalDispatchRingSize> g_globalDispatchRing{};
    std::atomic<uint32_t> g_globalDispatchNext{0u};

    // ---- Stage 5.7 execution coverage (arm with PS2_COVERAGE=1) -------------
    //
    // WHY THIS EXISTS. The ring above answers "what ran in the last
    // millisecond". It cannot answer "what ran AT ALL", and that is the only
    // question Stage 5.7 actually has: the frame loop is healthy, the screen is
    // black, and the sole game-band address ever observed executing is
    // 0x421f10. Guessing one candidate address per rebuild is a bit of
    // information per multi-hour cycle. Counting every dispatch gives the whole
    // executed SET for the price of one increment.
    //
    // DIRECT-MAPPED, NOT HASHED. Guest code occupies 0x100000..0x640380 and
    // every dispatched PC is 4-byte aligned, so (pc - base) >> 2 is a perfect
    // index -- no hashing, no collisions, no allocation, no locking on the hot
    // path. 344,286 slots x 4 bytes = ~1.4 MB of host BSS, and one relaxed
    // fetch_add is cheaper than the ring store it sits beside.
    //
    // LOWER BOUND, NOT EXACT -- read the dump with this in mind. pushDispatchPc
    // only sees TABLE-DISPATCHED calls; a direct fn_* C++ call bypasses
    // lookupFunction entirely. A nonzero count therefore proves "this ran"; a
    // zero count proves only "this was never dispatched", NOT "this never
    // executed". Never argue absence from this array alone.
    constexpr uint32_t kCoverageBase = 0x00100000u;
    constexpr uint32_t kCoverageEnd = 0x00640380u; // ELF mapped-segment end
    constexpr uint32_t kCoverageSpan = kCoverageEnd - kCoverageBase;
    constexpr uint32_t kCoverageSlots = kCoverageSpan / 4u;
    // Everything below this is Sony SDK (libdma/libgraph/libpad/libcdvd);
    // game code lives above it. Splitting the dump on this line is what makes
    // it interpretable at a glance -- "3000 distinct, all of them SDK" and
    // "3000 distinct, 900 of them game" are opposite diagnoses.
    // 2026-07-29: was 0x00400000, which was measured wrong. The first coverage
    // run showed heavy per-frame game code at 0x2ca270 (147k dispatches),
    // 0x2f44d0 (138k), 0x2d5d90, 0x2b6840, 0x240780, 0x337420 -- all of which
    // 0x400000 misfiled as "SDK" and, worse, dropped entirely from the COVGAME
    // shutdown dump. Env-driven now (PS2_COV_GAMELO) so re-calibrating this
    // line never costs another rebuild.
    uint32_t kGameBandStart = 0x00200000u;

    std::atomic<bool> g_coverageArmed{false};
    std::array<std::atomic<uint32_t>, kCoverageSlots> g_coverage{};

    void pushDispatchPc(uint32_t pc)
    {
        g_lastDispatchPc.store(pc, std::memory_order_relaxed);

        if (g_coverageArmed.load(std::memory_order_relaxed))
        {
            // Unsigned wrap makes the single < test reject pc < kCoverageBase
            // as well as pc >= kCoverageEnd, so this is one compare, not two.
            const uint32_t off = pc - kCoverageBase;
            if (off < kCoverageSpan)
            {
                g_coverage[off >> 2].fetch_add(1u, std::memory_order_relaxed);
            }
        }

        const uint32_t slot = g_globalDispatchNext.fetch_add(1u, std::memory_order_relaxed);
        g_globalDispatchRing[slot % kGlobalDispatchRingSize].store(pc, std::memory_order_relaxed);

        DispatchHistory &h = currentDispatchHistory();
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    // Reads the cross-thread ring (see above). Safe to call from the watchdog
    // thread; formatDispatchHistory() is NOT (it reads per-thread state).
    std::string formatGlobalDispatchHistory()
    {
        const uint32_t total = g_globalDispatchNext.load(std::memory_order_relaxed);
        if (total == 0u)
        {
            return "(empty)";
        }
        const uint32_t count = std::min(total, kGlobalDispatchRingSize);
        const uint32_t start = total - count; // absolute index of oldest kept entry

        std::ostringstream oss;
        for (uint32_t i = 0u; i < count; ++i)
        {
            if (i != 0u)
            {
                oss << " -> ";
            }
            const uint32_t pc = g_globalDispatchRing[(start + i) % kGlobalDispatchRingSize].load(std::memory_order_relaxed);
            oss << "0x" << std::hex << pc;
        }
        return oss.str();
    }

    // How many hottest addresses the periodic dump names. Small on purpose --
    // the summary counts are the diagnosis; the top list is only there to say
    // WHICH loop is hot without re-reading a 40 MB console log.
    constexpr uint32_t kCoverageTopN = 48u;

    struct CoverageSummary
    {
        uint64_t distinct = 0; // addresses dispatched at least once
        uint64_t game = 0;     // ... of those, in the game band
        uint64_t sdk = 0;      // ... of those, below kGameBandStart
        uint64_t calls = 0;    // total dispatches
    };

    // One pass over the counter array. Optionally emits to the structured probe
    // sink (run_probe.jsonl) rather than the console, because the console log
    // has twice produced wrong answers via encoding and line-wrapping -- see
    // the header comment in build_scripts/analyze_run.py.
    //
    // fullGameBand dumps EVERY distinct game-band address, not just the top N.
    // That is the actual Stage 5.7 payload -- the set of game functions the
    // recomp entered -- so it is written once at shutdown rather than every
    // tick. Cost is one row per address, bounded by what actually executed.
    CoverageSummary scanCoverage(bool emit, uint32_t phase, bool fullGameBand)
    {
        CoverageSummary s;
        if (!g_coverageArmed.load(std::memory_order_relaxed))
        {
            return s;
        }

        // Fixed-size insertion selection instead of sorting 344k entries: no
        // allocation, and this runs on the watchdog thread, which must never
        // become the reason a run is slow.
        uint32_t topAddr[kCoverageTopN] = {};
        uint32_t topCount[kCoverageTopN] = {};
        uint32_t topUsed = 0;

        for (uint32_t i = 0; i < kCoverageSlots; ++i)
        {
            const uint32_t c = g_coverage[i].load(std::memory_order_relaxed);
            if (c == 0u)
            {
                continue;
            }
            const uint32_t addr = kCoverageBase + (i * 4u);
            ++s.distinct;
            s.calls += c;
            if (addr >= kGameBandStart)
            {
                ++s.game;
            }

            if (topUsed < kCoverageTopN || c > topCount[kCoverageTopN - 1])
            {
                uint32_t at = (topUsed < kCoverageTopN) ? topUsed++ : (kCoverageTopN - 1);
                while (at > 0u && topCount[at - 1] < c)
                {
                    topCount[at] = topCount[at - 1];
                    topAddr[at] = topAddr[at - 1];
                    --at;
                }
                topCount[at] = c;
                topAddr[at] = addr;
            }
        }
        s.sdk = s.distinct - s.game;

        if (!emit)
        {
            return s;
        }

        {
            const char *keys[] = {"phase", "distinct", "game", "sdk", "calls", "slots"};
            const uint64_t vals[] = {phase, s.distinct, s.game, s.sdk, s.calls,
                                     static_cast<uint64_t>(kCoverageSlots)};
            ps2x_probe_kv("COVERAGE", 6, keys, vals);
        }

        for (uint32_t i = 0; i < topUsed; ++i)
        {
            const char *keys[] = {"rank", "addr", "count", "game"};
            const uint64_t vals[] = {i, topAddr[i], topCount[i],
                                     topAddr[i] >= kGameBandStart ? 1ull : 0ull};
            ps2x_probe_kv("COVTOP", 4, keys, vals);
        }

        if (fullGameBand)
        {
            for (uint32_t i = 0; i < kCoverageSlots; ++i)
            {
                const uint32_t c = g_coverage[i].load(std::memory_order_relaxed);
                if (c == 0u)
                {
                    continue;
                }
                const uint32_t addr = kCoverageBase + (i * 4u);
                if (addr < kGameBandStart)
                {
                    continue;
                }
                const char *keys[] = {"addr", "count"};
                const uint64_t vals[] = {addr, c};
                ps2x_probe_kv("COVGAME", 2, keys, vals);
            }
        }

        return s;
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = currentDispatchHistory();
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }


    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = ps2_syscalls::GetCurrentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    std::memset(&m_cpuContext, 0, sizeof(m_cpuContext));

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    // m_asyncCallbackStack (KernelStackPool) keeps its kernel-pool default
    // member initializer here; loadELF() re-arms it for the pool's next load
    // (see the layout comment at kAsyncCallbackStackFloor).

    // Claim the reserved main-thread identity (tid 1 — see State.h's
    // g_nextThreadId starting at 2, and run()'s create_fiber(1, 1, ...) for the
    // guest boot fiber, both of which reserve this same id) for the host thread
    // that constructs this runtime. g_currentThreadId == -1 here means this
    // host thread has never been assigned a guest identity: it is neither a
    // running guest fiber (which carries its own tid, set by the scheduler on
    // its own dedicated executor thread — a different OS thread from this one)
    // nor a borrowed IRQ/alarm/RPC worker (those self-assign -1 as the first
    // statement of their thread function, before any PS2Runtime is reachable).
    // ensureCurrentThreadInfo() lazily creates tid 1's ThreadInfo (THS_RUN,
    // wakeupCount 0) the first time a syscall needs it, and the guest boot
    // fiber (also tid 1, but on the separate executor thread) later finds and
    // reuses that same g_threads entry — so tid 1 never has two ThreadInfos.
    if (g_currentThreadId == -1)
    {
        g_currentThreadId = 1;
    }
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        // Fiber pool is cleaned up by scheduler_shutdown() in run().
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();

    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 { m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                 m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                 m_gs, &m_memory, startPC, top, itop, 65536); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 { m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                m_gs, &m_memory, top, itop, 65536); });
    m_iop.init(rdram);
    m_iop.reset();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        // Re-arm the kernel pool for this load (see the layout comment at
        // kAsyncCallbackStackFloor); a prior run may have carved it down.
        m_asyncCallbackStack.reset();
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    ps2_game_overrides::applyMatching(*this, elfPath, m_cpuContext.pc);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    // Optional override: point the pseudo-disc root at a full flat disc extraction
    // (SLUS + IRX + INFO.DAT/GAME.DAT + MOVIE) so ARKD's sceCdSearchFile/read and
    // later asset loads resolve against real disc files instead of the ELF-only dir.
    if (const char *cdRootEnv = std::getenv("PS2_CD_ROOT"); cdRootEnv && *cdRootEnv)
    {
        paths.cdRoot = std::filesystem::path(cdRootEnv);
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    // 2026-07-28 -- the std::cerr below is now bounded too.
    //
    // The probe sink further down was capped at 8 long ago for exactly this
    // reason, but the human-readable line above it was left unbounded, and it
    // is by far the more expensive of the two: formatDispatchHistory() builds
    // a ~1KB "0x1 -> 0x1 -> ..." string, and std::endl flushes it to a synced
    // stdio handle every single time. In the 2026-07-28 gated-off baseline the
    // guest derailed to pc=0x1 at t~32s and this one statement produced
    // **225 MB of run_log.txt in 64 seconds** -- 65.5s of CPU (42% of the whole
    // run) in ZwWriteFile + iostream formatting, on a thread that was doing no
    // guest work at all. It made the run look CPU-bound on emitted code when it
    // was really spinning on our own diagnostics.
    //
    // Keep the first 8 -- only those carry information; every later one is the
    // same event. After that stay silent but keep counting, and emit a rate
    // line every 100k so a spin is still distinguishable from a stall (an
    // unsampled counter cannot tell slow from dead).
    static std::atomic<uint32_t> s_missProbes{0};
    const uint32_t n = s_missProbes.fetch_add(1, std::memory_order_relaxed) + 1u;

    if (n <= 8u)
    {
        std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
                  << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
                  << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
                  << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;

        if (n == 8u)
        {
            std::cerr << "Error: dispatch-miss reporting suppressed after 8 hits;"
                         " a running total prints every 100000"
                      << std::endl;
        }
    }
    else if ((n % 100000u) == 0u)
    {
        std::cerr << "Error: dispatch-miss x" << std::dec << n
                  << " (still spinning, latest guest PC 0x" << std::hex << address << std::dec << ")"
                  << std::endl;
    }

    // Phase B: the derail terminus, structured. Hard-bounded -- once $ra is
    // clobbered the dispatcher retries the bad target forever and this site is
    // reached ~89,000 times in a 25s run. Only the FIRST few carry information;
    // the rest are the same event and would bloat the sink to no purpose.
    {
        if (n <= 8u)
        {
            static const char *const k[] = {"n", "pc", "tableBase", "tableEnd", "codeRegion"};
            const uint64_t v[] = {n, address, g_ps2RecompiledFunctionTableBase,
                                  g_ps2RecompiledFunctionTableEnd,
                                  m_memory.isCodeAddress(address) ? 1u : 0u};
            ps2x_probe_kv("MISS", 5, k, v);
        }
    }

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

std::string PS2Runtime::debugCurrentDispatchTrace() const
{
    return formatDispatchHistory();
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        // Full GPR set + a window of the current stack frame. Cross-referenced
        // with any [trapval] addr=, this gives the byte offset of the clobbered
        // slot relative to sp.
        static const char *const kGprNames[32] = {
            "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
            "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
            "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
            "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

        oss << "\n[gpr]";
        for (int i = 0; i < 32; ++i)
        {
            oss << ' ' << kGprNames[i] << "=0x" << std::hex
                << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[i], 0));
        }

        const uint32_t stackLo = (sp >= 0x40u) ? (sp - 0x40u) : 0u;
        for (uint32_t off = 0; off < 0x100u; off += 0x10u)
        {
            const uint32_t rowAddr = stackLo + off;
            oss << "\n[stack] 0x" << std::hex << rowAddr << " (sp";
            if (rowAddr >= sp)
            {
                oss << "+0x" << (rowAddr - sp);
            }
            else
            {
                oss << "-0x" << (sp - rowAddr);
            }
            oss << ")";
            for (uint32_t w = 0; w < 4u; ++w)
            {
                uint32_t word = 0u;
                const bool ok = readGuestU32At(rowAddr + (w * 4u), word);
                oss << ' ' << (ok ? "" : "?") << "0x" << word;
            }
        }
        oss << std::dec;

        dumpFrameTrace(oss);

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    if (kind == GuestBranchKind::Return)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        // Prevent nested dispatch.
        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            return true;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (!isCall)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        if (static_cast<uint64_t>(allocSize) > block.size)
        {
            continue;
        }

        // Allocate from the TOP of the free block (high addresses). The guest's own
        // libc heap (configured by SetupHeap over the SAME region) grows up from the
        // base and keeps its free-list control node there, so handing runtime-internal
        // scratch (GS/Font GIF packets, thread stacks, ...) from the base can clobber
        // it. Allocating high keeps the two allocators clear of each other.
        const uint64_t rawTop = blockEnd - static_cast<uint64_t>(allocSize);
        const uint64_t alignedStart = rawTop & ~(static_cast<uint64_t>(normalizedAlignment) - 1u);
        if (alignedStart < blockStart)
        {
            continue;
        }

        const uint32_t alignedAddr = static_cast<uint32_t>(alignedStart);
        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

// Carve arithmetic lifted verbatim out of reserveAsyncCallbackStack(). Two
// clamps present in the pre-encapsulation version were confirmed dead and
// dropped here rather than ported:
//   - `if (top > PS2_RAM_SIZE) top = PS2_RAM_SIZE;` — top starts at
//     kAsyncCallbackStackTop (0x00100000) and every successful carve strictly
//     decreases it (base < top is required to succeed), so top can never
//     approach PS2_RAM_SIZE (0x02000000); the branch never fired.
//   - the per-call `top &= ~(kGuestHeapDefaultAlignment - 1u)` re-align —
//     top starts 16-aligned (0x00100000) and every stored top is a `base`
//     that was itself masked to `align`, which normalizeGuestHeapAlignment()
//     guarantees is a power of two >= kGuestHeapDefaultAlignment (16); a
//     value aligned to a multiple of 16 is already 16-aligned, so the
//     re-align was always a no-op.
uint32_t KernelStackPool::carve(uint32_t size, uint32_t align)
{
    if (top <= size)
    {
        return 0u;
    }

    uint32_t base = top - size;
    base &= ~(align - 1u);
    if (base < floor || base >= top)
    {
        return 0u;
    }

    const uint32_t reservedTop = top;
    top = base;
    // One line per reservation (a handful per boot): permanent evidence of
    // where host-dispatched callback stacks live, so any future overlap with
    // guest memory is visible in the boot log.
    std::cerr << "[async-stack] reserved [0x" << std::hex << base
              << ", 0x" << reservedTop << ") stackTop=0x" << (reservedTop - 0x10u)
              << std::dec << '\n';
    return reservedTop - 0x10u;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    return m_asyncCallbackStack.carve(allocSize, normalizedAlignment);
}

void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t lastPc = std::numeric_limits<uint32_t>::max();
    uint32_t samePcCount = 0;
    constexpr uint32_t kSamePcYieldInterval = 0x4000u;

    while (!isStopRequested())
    {
        // Cooperative scheduling point. The recompiler emits the
        // shouldPreemptGuestExecution() hook only at INTRA-function back-edges;
        // a guest loop that spins ACROSS function dispatches (call/return
        // chains, recover-pc storms) has its back-edge HERE, not inside any
        // recompiled function, so without this call such a loop never reaches
        // yield_point() and holds the guest token forever, starving host
        // workers (interrupt worker VBlank/INTC delivery) parked in
        // async_guest_begin(). The fast path is a counter test, so this is as
        // cheap as the emitted per-back-edge checks. The return value is
        // irrelevant: whether or not we yielded, ctx->pc is a clean
        // function-boundary resume point.
        (void)shouldPreemptGuestExecution();

        const uint32_t pc = ctx->pc;

        if (pc == lastPc)
        {
            ++samePcCount;
            if ((samePcCount % kSamePcYieldInterval) == 0u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    RUNTIME_LOG("CPU is doing some work at PC 0x" << std::hex << pc << ". PC not updating.");
                });
                std::this_thread::yield();
            }
        }
        else
        {
            samePcCount = 0;
            lastPc = pc;
        }

        m_debugPc.store(pc, std::memory_order_relaxed);
        m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), std::memory_order_relaxed);
        m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)), std::memory_order_relaxed);
        m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)), std::memory_order_relaxed);

        // RecompDebugger IPC: publish this thread's pc/gpr/hi/lo into the
        // shared-memory RecompDebugState once per outer dispatch-loop
        // iteration, and service any armed breakpoint for this thread.
        // Restored 2026-07-14 (writer was fully stripped when the repo was
        // flattened; see recomp_debug_writer.h/.cpp). No-ops on non-Windows
        // and when RecompDebugger isn't attached (Init() never called or the
        // shm couldn't be opened).
        {
            uint32_t dbg_gpr[32];
            for (int i = 0; i < 32; ++i)
                dbg_gpr[i] = static_cast<uint32_t>(_mm_cvtsi128_si64(ctx->r[i]));
            RecompDbg::Update(g_currentThreadId, pc, dbg_gpr,
                               static_cast<uint32_t>(ctx->hi),
                               static_cast<uint32_t>(ctx->lo),
                               ctx->insn_count,
                               rdram, PS2_RAM_SIZE);
            if (RecompDbg::CheckBreakpoint(g_currentThreadId, pc & 0x1FFFFFFFu, dbg_gpr))
            {
                // Breakpoint/step handling may have edited dbg_gpr (a debugger-armed
                // register write); mirror only the low 32-bit lane back into the live
                // 128-bit MMI register so the other lanes are left untouched.
                for (int i = 1; i < 32; ++i)
                    ctx->r[i] = _mm_insert_epi32(ctx->r[i], static_cast<int>(dbg_gpr[i]), 0);
            }
        }

        RecompiledFunction fn = lookupFunction(pc);
        const uint32_t dispatchedPc = pc;
        const uint32_t dispatchedRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
        fn(rdram, ctx, this);

        if (ctx->pc == 0u)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[dispatch:pc-zero] from=0x" << std::hex << dispatchedPc
                          << " fromRa=0x" << dispatchedRa
                          << " ra=0x" << ra
                          << " sp=0x" << sp
                          << " gp=0x" << gp
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            });

            // PC=0 means this guest thread returned (usually via jr $ra with RA=0).
            // Do not request a global runtime stop here: other guest threads may still run.
            break;
        }
    }
}

bool PS2Runtime::shouldPreemptGuestExecution()
{
    return ps2sched::yield_point();
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    ps2_syscalls::notifyRuntimeStop();
}

void PS2Runtime::requestStopFlagOnly()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    ps2_syscalls::resetSoundDriverRpcState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetGsSyncVCallbackState();
    ps2_stubs::resetMpegStubState();
    ps2_syscalls::initializeGuestKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    // Bootstrap $sp at top of RAM, as the hardware loader does; the guest's
    // crt0 immediately replaces it via SetupThread, which points $sp at the
    // top of the game-chosen main stack. Callback stacks live in the
    // kernel-reserved pool (see kAsyncCallbackStackFloor), not here, so this
    // cannot collide with them.
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // The debug shared-memory writer is NOT free: RecompDbg::Update() runs on
    // every dispatch-loop iteration (i.e. every guest function call) and copies
    // a 2 KB RAM window into a cross-process section. Init() previously ran
    // unconditionally, so that cost was paid on every run whether or not
    // RecompDebugger was attached. Gated 2026-07-28 while measuring the guest
    // execution rate; leaving s_shm null makes Update()/CheckBreakpoint()
    // collapse to a single early-out branch. launch_recomp.ps1 sets this when
    // it launches RecompDebugger.
    if (const char *dbgShm = std::getenv("PS2X_DEBUGSHM"); dbgShm && *dbgShm && *dbgShm != '0')
    {
        RecompDbg::Init();
    }

    // Started here so the profile window covers the whole guest run, including
    // the 4.4s stall at t=2-6s that the watchdog sees but cannot explain.
    ps2x_host_sampler_start();

    // Ground-truth dump of every function address actually registered by the
    // recompiler, so RecompDebugger (a separate process with no PS2Runtime
    // pointer) can diff it against the ELF symbol table to flag functions
    // that were never translated. Restored 2026-07-14; rewritten against the
    // dense-table function-registry format (g_ps2RecompiledFunctionTable[])
    // that replaced the old std::map<uint32_t, RecompiledFunction> table.
    {
        std::ofstream dumpFile("recomp_function_table.txt", std::ios::trunc);
        if (dumpFile.is_open())
        {
            for (uint32_t slot = 0; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
            {
                if (g_ps2RecompiledFunctionTable[slot] != nullptr)
                {
                    const uint32_t addr = g_ps2RecompiledFunctionTableBase + (slot << 2);
                    dumpFile << std::hex << addr << "\n";
                }
            }
        }
    }

    // [watch] env hook: PS2X_WATCH=ADDR[:SIZE][:LABEL][,...] arms guest-memory
    // watches before guest code starts. Arming a watch also enables the
    // diagnostics gate -- this turns on the FULL diagnostics stream, i.e. ALL
    // PS2X_DIAG probe output (every [gs:*], [dma:*], [watch], etc. line),
    // exactly as if PS2X_DIAG=1 had been set -- not just [watch] lines.
    // Costs nothing when PS2X_WATCH is unset.
    if (const size_t armed = ps2_watch::armWatchesFromEnv(std::getenv("PS2X_WATCH")))
    {
        ps2_diag::set_enabled(true);
        RUNTIME_LOG("[watch] armed " << armed << " guest-memory watch(es) from PS2X_WATCH");
    }

    // [trapval] env hook: PS2X_TRAPVAL=VALUE[:ADDRLO:ADDRHI] arms a
    // value-triggered store trap (see ps2_watch::onGuestWrite). Diagnostic
    // only; unset => zero behavioural change and the per-store residual cost
    // stays exactly one relaxed bool load.
    if (const char *tv = std::getenv("PS2X_TRAPVAL"); tv && *tv && std::strcmp(tv, "0") != 0)
    {
        char *end = nullptr;
        const uint32_t value = static_cast<uint32_t>(std::strtoull(tv, &end, 0));
        uint32_t lo = 0u;
        uint32_t hi = 0xFFFFFFFFu;
        if (end && *end == ':')
        {
            lo = static_cast<uint32_t>(std::strtoull(end + 1, &end, 0));
            hi = (end && *end == ':') ? static_cast<uint32_t>(std::strtoull(end + 1, &end, 0)) : 0xFFFFFFFFu;
        }

        ps2_watch::g_trapValue.store(value, std::memory_order_relaxed);
        ps2_watch::g_trapAddrLo.store(lo, std::memory_order_relaxed);
        ps2_watch::g_trapAddrHi.store(hi, std::memory_order_relaxed);
        ps2_watch::g_trapArmed.store(true, std::memory_order_relaxed);
        ps2_watch::g_writeWatchActive.store(true, std::memory_order_relaxed);

        RUNTIME_LOG("[trapval] armed value=0x" << std::hex << value
                                               << " range=[0x" << lo << ",0x" << hi << ")" << std::dec);
    }

    // [frametrace] env hook: PS2X_FRAMETRACE=1 arms the bounded call-frame ring
    // (see ps2FrameTraceRecord above). The wrappers that feed it are installed
    // in game_overrides.cpp. Unset => zero behavioural change: the wrappers
    // still pass through, and each records exactly one relaxed bool load.
    if (const char *ft = std::getenv("PS2X_FRAMETRACE"); ft && *ft && *ft != '0')
    {
        g_ps2FrameTraceArmed.store(true, std::memory_order_relaxed);
        std::cerr << "[frametrace] armed" << std::endl;
    }

    // Execution coverage (env PS2_COVERAGE=1). Armed HERE rather than inside the
    // watchdog thread on purpose: the watchdog starts late, and the boot path is
    // exactly the stretch of execution Stage 5.7 needs counted. Unset => the
    // per-dispatch cost stays one relaxed bool load, same contract as the two
    // knobs above.
    if (const char *cov = std::getenv("PS2_COVERAGE"); cov && *cov && *cov != '0')
    {
        g_coverageArmed.store(true, std::memory_order_relaxed);
        if (const char *gl = std::getenv("PS2_COV_GAMELO"); gl && *gl)
            kGameBandStart = static_cast<uint32_t>(std::strtoul(gl, nullptr, 16));
        std::cerr << "[coverage] armed -- counting table-dispatched calls in [0x"
                  << std::hex << kCoverageBase << ",0x" << kCoverageEnd << ")"
                  << std::dec << ", game band >= 0x" << std::hex << kGameBandStart
                  << std::dec << std::endl;
    }

    // Optional R3000A IOP-core self-test (env PS2_IOP_CPU_SELFTEST=1): runs a
    // hand-assembled program in (still-zeroed) IOP RAM before the guest starts.
    if (const char *st = std::getenv("PS2_IOP_CPU_SELFTEST"); st && *st && *st != '0')
    {
        IopCpu iopCpu(&m_memory);
        iopCpu.selfTest();
    }

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    // Initialize the fiber/pool scheduler.
    ps2sched::scheduler_init();
    ps2sched::scheduler_set_stop_callback(+[](void* p) { static_cast<PS2Runtime*>(p)->requestStopFlagOnly(); }, this);

    // Create the main guest fiber (tid=1).
    uint8_t *rdram = m_memory.getRDRAM();
    {
        const uint32_t entry = m_cpuContext.pc;
        const uint32_t sp    = static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0));
        const uint32_t gp    = static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0));
        ps2sched::create_fiber(1, 1, entry, sp, gp, 0u, this, rdram);
    }

    ps2_syscalls::EnsureVSyncWorkerRunning(m_memory.getRDRAM(), this);

    // Optional PC watchdog (enable with env PS2_PC_WATCHDOG=1): logs the guest PC
    // once a second so an external observer can tell whether execution is
    // advancing or spinning on a wait loop. Diagnostic only; off by default.
    std::thread watchdogThread;
    {
        const char *wdEnv = std::getenv("PS2_PC_WATCHDOG");
        if (wdEnv && *wdEnv && *wdEnv != '0')
        {
            watchdogThread = std::thread([&]()
                                         {
                ThreadNaming::SetCurrentThreadName("PcWatchdog");
                uint32_t last = 0xFFFFFFFFu;
                int stuck = 0;
                int t = 0;
                // Per-second render rates. These four counters already existed and
                // are already maintained on the hot paths; only their *rate* was
                // never observable, so "the game renders slowly" and "the game does
                // not render" were indistinguishable. The watchdog already ticks at
                // exactly 1Hz, so a delta here is a rate in Hz with no new state.
                //
                // How to read them:
                //   dma/s  = CHCR.STR=1 kicks accepted (all channels)
                //   gif/s  = packets actually handed to the rasterizer
                //            (PS2Memory::m_gifCopyCount, bumped only where
                //            submitGifPacket is really called)
                // gif/s is the load-bearing one: if dma/s is healthy while gif/s is
                // 0, packets are being queued and dropped rather than drawn -- that
                // is the m_gifPacketCallback/m_gifArbiter guard in
                // PS2Memory::writeIORegister, not a guest-side fault.
                uint64_t prevDma = 0, prevGif = 0;
                // Stage 5.6.2. progress=~117/s means ~15,000 guest back-edges
                // per second (one tick per 128), which is orders of magnitude
                // below what statically recompiled code should manage -- but
                // that single number cannot tell "executes slowly" from "idle,
                // waiting most of the second", because both produce a low rate.
                //
                //   busy%  = share of the wall clock the executor spent inside
                //            ps2fiber_resume, i.e. guest code actually on-CPU.
                //            High + low progress => execution really is slow.
                //            Low                 => the guest is blocked, and
                //            the fault is wake latency, not throughput.
                //   res/s  = fiber resumes; pairs with busy% to give mean time
                //            on-CPU per resume.
                //   vbl/s  = vblank ticks delivered. Expected ~60 under
                //            -Determinism 0 (wall-clock paced). If vbl/s is ~60
                //            while gif/s is ~3 the guest needs ~20 vblanks per
                //            frame; if vbl/s is itself low the pacing thread is
                //            the bottleneck and gif/s merely follows it.
                uint64_t prevBusyNs = 0, prevResumes = 0, prevVbl = 0;
                // Stage 5.7. Every rate field above measures the *render* loop,
                // and they all read healthy while the screen stays black -- so
                // none of them can answer the question that is actually open:
                // does the guest's game-state machine ever tick? The community
                // EE map puts SDBZ's GameMode word at 0x005e6b3c, with 0x00 =
                // MainMenu, i.e. boot success. Sampling it at the same 1Hz as
                // everything else turns "black screen" into a number: a value
                // that never changes across a whole run means the frame loop is
                // spinning on a state machine that is not advancing, which is a
                // different fault from anything the DMA/GIF counters can see.
                //
                // The address is env-overridable (PS2_WATCH_ADDR, hex, with or
                // without 0x) because moving a hardcoded probe costs a rebuild
                // and a rebuild here is measured in hours. gstate= prints four
                // consecutive words so neighbouring state (submode, timers) is
                // visible without a second run. gchg= counts how many samples
                // differed from the previous one -- 0 is the damning result.
                //
                // 2026-07-29 correction: gchg originally compared only w[0], so
                // on a constant address (the 0x421f10 positive control) it read
                // 0 by construction and carried no information. It now compares
                // all four sampled words.
                auto envHex = [](const char *name, uint32_t fallback) -> uint32_t
                {
                    const char *v = std::getenv(name);
                    if (v == nullptr || *v == '\0')
                    {
                        return fallback;
                    }
                    const char *p = (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) ? v + 2 : v;
                    return static_cast<uint32_t>(std::strtoul(p, nullptr, 16));
                };

                const uint32_t watchAddr = envHex("PS2_WATCH_ADDR", 0x005e6b3cu);
                uint32_t prevWatch[4] = {0, 0, 0, 0};
                bool havePrevWatch = false;
                uint32_t watchChanges = 0;

                // Stage 5.7 BSS liveness scan.
                //
                // Watching one guessed address answers one guess. readelf -l on
                // SLUS_214.42 gives a single LOAD with FileSiz 0x400680 but
                // MemSiz 0x540380, so 0x500680..0x640380 is BSS: zero-filled at
                // load, no file bytes. Every game global that is not statically
                // initialised lives in there. Sweeping the whole range converts
                // "is THIS address ticking?" into a question with three
                // distinguishable answers, none of which depend on trusting a
                // community memory map:
                //
                //   bssnz=0                  the guest writes no globals at all
                //   bssnz large, bsschg=0    globals initialised once, then the
                //                            state machine is genuinely frozen
                //   bsschg>0                 state IS advancing, and the address
                //                            we were watching was simply wrong
                //
                // Bounds are env-overridable (PS2_BSS_LO/PS2_BSS_HI) so the next
                // question costs a run, not a rebuild. Sampling the previous
                // snapshot costs one heap buffer of span/4 words (~330 KB), read
                // once per second on the watchdog thread.
                const uint32_t bssLo = envHex("PS2_BSS_LO", 0x00500680u) & ~3u;
                const uint32_t bssHi = envHex("PS2_BSS_HI", 0x00640380u) & ~3u;
                const size_t bssWords = (bssHi > bssLo) ? ((bssHi - bssLo) / 4u) : 0u;
                std::vector<uint32_t> bssPrev(bssWords, 0u);
                bool haveBssPrev = false;
                while (!isStopRequested())
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    const uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
                    const uint32_t ra = m_debugRa.load(std::memory_order_relaxed);
                    const uint32_t lastCall = g_lastDispatchPc.load(std::memory_order_relaxed);
                    stuck = (pc == last) ? (stuck + 1) : 0;
                    last = pc;
                    const uint64_t curDma = m_memory.dmaStartCount();
                    const uint64_t curGif = m_memory.gifCopyCount();
                    const uint64_t dDma = curDma - prevDma;
                    const uint64_t dGif = curGif - prevGif;
                    prevDma = curDma;
                    prevGif = curGif;
                    const uint64_t curBusyNs = ps2x_guest_busy_ns();
                    const uint64_t curResumes = ps2x_guest_resumes();
                    const uint64_t curVbl = ps2x_vblank_ticks();
                    // The watchdog sleeps 1s, so the deltas are already per-second
                    // and busy% is (ns on-CPU / 1e9) * 100 with no extra timing.
                    const uint64_t dBusyNs = curBusyNs - prevBusyNs;
                    const uint64_t dResumes = curResumes - prevResumes;
                    const uint64_t dVbl = curVbl - prevVbl;
                    prevBusyNs = curBusyNs;
                    prevResumes = curResumes;
                    prevVbl = curVbl;
                    const uint64_t busyPct = dBusyNs / 10000000ull; // ns -> % of 1s
                    // Plain RDRAM, so read32 cannot re-enter the MMIO handlers;
                    // guarded anyway because the watchdog must never be the
                    // thing that takes the run down.
                    uint32_t w[4] = {0, 0, 0, 0};
                    uint64_t bssNonZero = 0;
                    uint64_t bssChanged = 0;
                    try
                    {
                        for (int i = 0; i < 4; ++i)
                            w[i] = m_memory.read32(watchAddr + (i * 4));

                        for (size_t i = 0; i < bssWords; ++i)
                        {
                            const uint32_t v = m_memory.read32(bssLo + static_cast<uint32_t>(i * 4u));
                            if (v != 0u)
                            {
                                ++bssNonZero;
                            }
                            if (haveBssPrev && v != bssPrev[i])
                            {
                                ++bssChanged;
                            }
                            bssPrev[i] = v;
                        }
                        haveBssPrev = (bssWords != 0u);
                    }
                    catch (const std::exception &)
                    {
                    }
                    if (havePrevWatch &&
                        (w[0] != prevWatch[0] || w[1] != prevWatch[1] ||
                         w[2] != prevWatch[2] || w[3] != prevWatch[3]))
                    {
                        ++watchChanges;
                    }
                    for (int i = 0; i < 4; ++i)
                        prevWatch[i] = w[i];
                    havePrevWatch = true;

                    // Coverage is scanned every tick for the inline cov= fields,
                    // but only written to the structured sink every 10s -- the
                    // summary is what the watchdog line needs, and the sink is
                    // for analyze_run.py afterwards.
                    const CoverageSummary cov =
                        scanCoverage((static_cast<uint32_t>(t) % 10u) == 9u, 1u, false);
                    // progress = ps2sched's guest clock (one tick per 128 guest
                    // back-edges). Two uses: it distinguishes "the guest is
                    // spinning in a loop" from "the guest is not executing at
                    // all" -- pc alone cannot -- and its rate per second is what
                    // PS2X_DET_VBLANK_QUANTUM should be tuned against.
                    // Field order is defensive, not cosmetic. Long lines get
                    // clipped somewhere between here and the log (see the console
                    // buffer note in launch_recomp.ps1), so the rate fields this
                    // stage is actually measuring go FIRST and the bulky trace=
                    // stays last. vif/s, gsw/s and gifTot were dropped: the first
                    // two only count direct MMIO register writes and this game
                    // feeds VIF1 by DMA, so both read 0 by design and neither ever
                    // discriminated anything; gifTot is just the running sum of
                    // gif/s.
                    std::cerr << "[watchdog] t=" << (++t) << "s"
                              // cov=<distinct>/<game band>. A game-band count
                              // that never rises is the Stage 5.7 answer.
                              << " cov=" << cov.distinct << "/" << cov.game
                              << " bssnz=" << bssNonZero
                              << " bsschg=" << bssChanged
                              << " gchg=" << watchChanges
                              << std::hex
                              << " gstate@0x" << watchAddr << "="
                              << w[0] << "," << w[1] << "," << w[2] << "," << w[3]
                              << std::dec
                              << " busy%=" << busyPct
                              << " res/s=" << dResumes
                              << " vbl/s=" << dVbl
                              << " progress=" << ps2x_guest_progress()
                              << " gif/s=" << dGif
                              << " dma/s=" << dDma
                              << " stuckSecs=" << stuck
                              << std::hex << " pc=0x" << pc
                              << " ra=0x" << ra << " lastCall=0x" << lastCall
                              << std::dec
                              << " trace=" << formatGlobalDispatchHistory() << std::endl;
                }

                // Final dump, with fullGameBand=true. This is the Stage 5.7
                // payload: every distinct game-band address the recomp actually
                // dispatched. Written here, on the way out, so a run stopped by
                // -RunSeconds still leaves the set on disk instead of only in a
                // console buffer that gets clipped.
                const CoverageSummary fin = scanCoverage(true, 2u, true);
                if (g_coverageArmed.load(std::memory_order_relaxed))
                {
                    std::cerr << "[coverage] final: distinct=" << fin.distinct
                              << " game=" << fin.game
                              << " sdk=" << fin.sdk
                              << " calls=" << fin.calls
                              << "  (table-dispatched only -- a zero count means"
                                 " never DISPATCHED, not never executed)"
                              << std::endl;
                } });
        }
    }

    uint64_t tick = 0;
    while (!isStopRequested())
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const int activeThreads = g_activeThreads.load(std::memory_order_relaxed);

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << activeThreads
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
            }
        });

        if (ps2_diag::enabled())
        {
            // [gs-activity]: opt-in headline distinct from the AGRESSIVE_LOGS
            // [run:tick] block above. Emits once on the first sign of GS
            // activity, then at most once every 600 iterations and only
            // while the underlying counters actually changed.
            static bool s_anyActivitySeen = false;
            static bool s_havePrevCounters = false;
            static uint64_t s_activityFrame = 0;
            static uint64_t s_prevDma = 0;
            static uint64_t s_prevGif = 0;
            static uint64_t s_prevGsw = 0;
            static uint64_t s_prevVif = 0;

            ++s_activityFrame;

            const uint64_t curDma = m_memory.dmaStartCount();
            const uint64_t curGif = m_memory.gifCopyCount();
            const uint64_t curGsw = m_memory.gsWriteCount();
            const uint64_t curVif = m_memory.vifWriteCount();
            const GSRegisters &gsRegs = m_memory.gs();

            const bool anyActivityNow = (curDma != 0u) || (curGif != 0u) || (curGsw != 0u) || (curVif != 0u);
            const bool countersChanged = !s_havePrevCounters ||
                                         curDma != s_prevDma || curGif != s_prevGif ||
                                         curGsw != s_prevGsw || curVif != s_prevVif;

            bool emitFirstActivity = false;
            if (!s_anyActivitySeen && anyActivityNow)
            {
                s_anyActivitySeen = true;
                emitFirstActivity = true;
            }

            const bool emitPeriodic = s_anyActivitySeen && ((s_activityFrame % 600u) == 0u) && countersChanged;

            if (emitFirstActivity || emitPeriodic)
            {
                // NOTE: prims/imgbytes/lastfbp below are three independent relaxed
                // loads while the GS thread updates them; the printed triple may be
                // torn (values from slightly different instants). This is a
                // human-read diagnostic headline, not a consistent snapshot — a torn
                // read is acceptable and never affects control flow.
                RUNTIME_LOG("[gs-activity] frame=" << s_activityFrame
                                                   << " dma=" << curDma
                                                   << " gif=" << curGif
                                                   << " gsw=" << curGsw
                                                   << " vif=" << curVif
                                                   << " dispfb1=0x" << std::hex << gsRegs.dispfb1
                                                   << " dispfb2=0x" << gsRegs.dispfb2
                                                   << " display1=0x" << gsRegs.display1
                                                   << std::dec
                                                   << " prims=" << m_gs.drawStatPrims()
                                                   << " imgbytes=" << m_gs.drawStatImageBytes()
                                                   << " lastfbp=0x" << std::hex << m_gs.drawStatLastFbp()
                                                   << std::dec
                                                   << (emitFirstActivity ? "  (FIRST ACTIVITY)" : ""));

                // Only update the "last emitted" snapshot when a line is
                // actually emitted, so countersChanged means "changed since
                // the last emitted line" rather than "changed since the last
                // loop iteration" (which could wrongly suppress a periodic
                // line if activity paused right at the 600-frame boundary).
                s_prevDma = curDma;
                s_prevGif = curGif;
                s_prevGsw = curGsw;
                s_prevVif = curVif;
                s_havePrevCounters = true;
            }
        }

        // Guest-memory watch polling (~60Hz, frame-paced by this loop). Bails
        // out immediately when no watches are registered or the gate is off.
        ps2_watch::pollWatches(m_memory.getRDRAM());

        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);

        // [STEP 6] Frame recorder: separately env-gated via PS2X_REC (not
        // PS2X_DIAG). Dumps a PNG only when the presented frame's hash
        // changes, so unchanged frames never touch disk.
        static const bool s_recEnabled = ps2_diag::env_int("PS2X_REC", 0) != 0;
        if (s_recEnabled)
        {
            static const int s_recInterval = std::max(1, ps2_diag::env_int("PS2X_REC_INTERVAL", 1));
            static uint64_t s_recFrameIndex = 0;
            static uint64_t s_recLastHash = 0;
            static bool s_recHaveLastHash = false;

            ++s_recFrameIndex;
            if ((s_recFrameIndex % static_cast<uint64_t>(s_recInterval)) == 0u)
            {
                std::vector<uint8_t> recPixels;
                uint32_t recWidth = 0u;
                uint32_t recHeight = 0u;
                if (m_gs.copyLatchedHostPresentationFrame(recPixels, recWidth, recHeight) &&
                    !recPixels.empty() && recWidth != 0u && recHeight != 0u)
                {
                    // FNV-1a over the raw pixel bytes.
                    uint64_t hash = 1469598103934665603ull;
                    for (const uint8_t byteValue : recPixels)
                    {
                        hash ^= static_cast<uint64_t>(byteValue);
                        hash *= 1099511628211ull;
                    }

                    if (!s_recHaveLastHash || hash != s_recLastHash)
                    {
                        s_recLastHash = hash;
                        s_recHaveLastHash = true;

                        // Build the Image struct manually so it points at our
                        // own vector's storage. Raylib did not allocate this
                        // memory, so it must NOT be freed via UnloadImage.
                        Image recImage{};
                        recImage.data = recPixels.data();
                        recImage.width = static_cast<int>(recWidth);
                        recImage.height = static_cast<int>(recHeight);
                        recImage.mipmaps = 1;
                        recImage.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

                        std::ostringstream recNameStream;
                        recNameStream << "ps2x_rec_" << s_recFrameIndex << "_"
                                      << std::hex << hash << ".png";
                        ExportImage(recImage, recNameStream.str().c_str());
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        // RecompDebugger IPC: once-per-video-frame extended telemetry (GS
        // regs, pad state, runtime log ring, guest thread scheduler snapshot).
        // Restored 2026-07-14. NOTE: the debugger's Input-tab pad-injection
        // (RecompDbg::GetPadOverride -> ps2_stubs::setPadOverrideState) is
        // deliberately NOT wired back in here -- the current
        // setPadOverrideState()/clearPadOverrideState() are single-port
        // (no `port` parameter), unlike the per-port overrides this call
        // expected when it was dropped. Re-plumbing Pad.cpp to be per-port
        // was out of scope for this restore; read-only telemetry (PC/GPR,
        // breakpoints, GS/pad/thread/log display) is unaffected.
        {
            const GSRegisters &gsRegs = m_memory.gs();
            DbgGsSnapshot dbgGs{gsRegs.pmode, gsRegs.smode2, gsRegs.dispfb1,
                                gsRegs.display1, gsRegs.dispfb2, gsRegs.display2,
                                gsRegs.csr.load(std::memory_order_relaxed)};

            const ps2_stubs::PadDebugSnapshot padSnap = ps2_stubs::getPadDebugSnapshot();
            DbgPadSnapshot dbgPad[2];
            for (int p = 0; p < 2; ++p)
            {
                const ps2_stubs::PadDebugPortSnapshot &row = padSnap.ports[p][0];
                dbgPad[p] = DbgPadSnapshot{row.lastButtons, row.lx, row.ly, row.rx, row.ry};
            }

            const std::vector<ps2_log::RuntimeLogEntry> logSnap = ps2_log::snapshot_runtime_log_entries();
            const uint32_t logCount = static_cast<uint32_t>(
                std::min<size_t>(logSnap.size(), kDbgMaxLogEntries));
            std::array<DbgLogEntry, kDbgMaxLogEntries> dbgLogs{};
            for (uint32_t i = 0; i < logCount; ++i)
            {
                const ps2_log::RuntimeLogEntry &src = logSnap[logSnap.size() - logCount + i];
                dbgLogs[i].seq = src.seq;
                std::snprintf(dbgLogs[i].text, kDbgLogTextSize, "%s", src.text.c_str());
            }
            const uint64_t nextSeq = logSnap.empty() ? 1 : (logSnap.back().seq + 1);
            RecompDbg::UpdateExtended(dbgGs, dbgPad, dbgLogs.data(), logCount, nextSeq);

            const std::vector<ps2_syscalls::ThreadDebugSnapshot> threadSnap = ps2_syscalls::getThreadDebugSnapshot();
            const uint32_t threadCount = static_cast<uint32_t>(
                std::min<size_t>(threadSnap.size(), kDbgMaxThreads));
            std::array<DbgThreadInfo, kDbgMaxThreads> dbgThreads{};
            for (uint32_t i = 0; i < threadCount; ++i)
            {
                const ps2_syscalls::ThreadDebugSnapshot &src = threadSnap[i];
                dbgThreads[i].tid             = src.tid;
                dbgThreads[i].entry           = src.entry;
                dbgThreads[i].currentPc       = src.currentPc;
                dbgThreads[i].stack           = src.stack;
                dbgThreads[i].status          = src.status;
                dbgThreads[i].waitType        = src.waitType;
                dbgThreads[i].waitId          = src.waitId;
                dbgThreads[i].currentPriority = src.currentPriority;
            }
            RecompDbg::UpdateThreads(dbgThreads.data(), threadCount);
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();

    if (watchdogThread.joinable())
    {
        watchdogThread.join();
    }

    // Signal all guest fibers to stop and join the pool threads.
    ps2sched::scheduler_shutdown();
    ps2sched::scheduler_set_stop_callback(nullptr, nullptr);

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    // Backstop only: the sampler normally reports itself on its own timer, since
    // launch_recomp.ps1's auto-stop can Kill() before this point is reached.
    ps2x_host_sampler_stop();

    RecompDbg::Shutdown();
    UnloadTexture(frameTex);
    CloseWindow();
}
