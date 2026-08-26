#include "ps2_runtime.h"
#include "runtime/ps2_pipeline_stats.h"
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

// Stage 5.12: emulates padman's per-vsync IOP->EE pad-state push straight into
// the guest libpad buffer. Defined in ps2_pad.cpp; declared here for the same
// header-cost reason as above. Must be called on the thread that polls raylib
// input, i.e. right after EndDrawing().
extern "C" void ps2x_pad_push_frame(uint8_t *rdram);

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
    // IE (bit 0) and EIE (bit 16) are the only two Status bits the game's own
    // kernel library ever tests -- 42 mfc0 $12 sites in the ELF, and every one
    // in the 0x175xxx/0x17exxx SDK range masks with either `andi 0x1` or
    // `lui 0x1, and`. Both must read as "interrupts enabled" for a thread to
    // start; see the boot-state comment in PS2Runtime::PS2Runtime().
    constexpr uint32_t COP0_STATUS_IE = 0x00000001u;
    constexpr uint32_t COP0_STATUS_EIE = 0x00010000u;
}

// Address of the callback currently executing inside the 0x13c4f8 callback-list
// runner, or 0 when that loop is not in a callback. Written by the override in
// game_overrides.cpp, read by the watchdog below.
//
// Deliberately a single in-flight slot rather than a per-call log line: that
// loop runs six slots at 60Hz, so logging each entry would emit six figures'
// worth of lines and then hit a cap -- and a capped tag's absence proves
// nothing. This costs one store per callback, cannot flood, and answers the
// only question worth asking: if the guest stops, was it inside a callback,
// and which one? A nonzero value on a parked watchdog names the callback that
// entered and never came back; zero rules the loop out entirely.
//
// Defined here rather than in game_overrides.cpp so every target that links
// ps2_runtime resolves it without depending on the overrides TU.
std::atomic<uint32_t> g_sdbzCb13C4F8InFlight{0u};

namespace
{
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

    // Same no-header rule. Defined in ps2_scheduler.cpp. Returns 1 when no guest
    // thread is runnable (run queue empty AND no running fiber). [thsync] uses it
    // to separate "the worker is not runnable" from "the worker is runnable but
    // never scheduled" -- those are different bugs with different fixes.
    extern "C" int ps2x_guest_idle();

    // Vblank tick provenance (stage 5.17). vbl/s alone cannot distinguish "the
    // quantum path is pacing slowly" from "the quantum path fired zero times and
    // a fallback carried the run" -- those want opposite fixes. Split counters,
    // reported as vblSrc=q/i/s (quantum / guest-idle / frozen-stall).
    extern "C" void ps2x_vblank_tick_sources(uint64_t *quantum, uint64_t *idle,
                                             uint64_t *stall);

    // Same no-header rule. Defined in ps2_scheduler.cpp, where they have existed
    // since the EIE gate went in but were never printed anywhere -- so the one
    // question they answer has never been asked of a run.
    //
    // yield_point() step 2b refuses to surrender the guest slot while the guest
    // holds interrupts disabled, and only gives up after kIntrDisableYieldEscape
    // (4096) samples == ~512K guest back-edges. Every escape is therefore a
    // stretch where no fiber could be scheduled no matter what was Ready. On a
    // healthy run intrEsc reads 0; any non-zero value means a critical section
    // ran long enough that the gate stopped protecting and started stalling.
    // intrStray > 0 would mean EIE is being cleared by something other than the
    // section that set it -- the one way this single-bit model under-protects.
    extern "C" uint64_t ps2x_guest_intr_disable_escapes();
    extern "C" uint64_t ps2x_guest_intr_disable_sections();
    extern "C" uint64_t ps2x_guest_intr_disable_stray();

    // Structured probe sink (Phase B), defined in game_overrides.cpp. Same
    // extern-between-.cpp rule as above -- no header, no 30h rebuild.
    extern "C" void ps2x_probe_kv(const char *name, int n,
                                  const char *const *keys, const uint64_t *vals);

    // Periodic SRD histogram dump, also defined in game_overrides.cpp. The
    // watchdog is the only thing in the process guaranteed to keep ticking when
    // the guest stalls, so it -- not the SRD call sites -- is where a dump has
    // to be driven from if it is to survive a run that gets killed rather than
    // exiting (stage 5.17). No-op unless the SRD probe is enabled.
    extern "C" void ps2x_srd_stat_tick();

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

    // ---- [order] ordered dispatch trace (env PS2X_ORDER=addr,addr,...) ------
    //
    // Coverage answers "how many times", and for Stage 5.17 that turned out to
    // be the wrong question. The movie state ladder is a two-variable
    // handshake: sub_165300 advances dec+72 (the state) only while dec+76 is
    // 2..4 or 6, and every producer of dec+76 is itself gated on dec+72 having
    // already advanced. Counts alone cannot distinguish "the value was never
    // written" from "it was written and something reset it before the state
    // machine next ticked" -- and the second reading is now the live one,
    // because 0x164F78 (the sole writer of dec+76 = 3) DID run three times
    // with its guard passing, while sub_165300 still read dec+76 < 2 on all
    // three of its own ticks.
    //
    // A 1 Hz sampler cannot settle that either: a value that is written and
    // overwritten between two samples is invisible to sampling, which is the
    // exact false-negative shape this project has already paid for more than
    // once. What is needed is ORDER, and order needs no memory reads at all --
    // just the sequence in which these specific functions were dispatched:
    //
    //   0x164F78  writes dec+76 = 3   (bootstrap; guard 0x15B560)
    //   0x166998  writes dec+76 = 4   (requires dec+72 == 3)
    //   0x166190  writes dec+76 = 6   (state-4 path)
    //   0x1663D0  writes dec+72 = 1, dec+76 = 1  AND dec+76 = 0
    //   0x166A28  writes dec+76 = 0
    //   0x166AB0  writes dec+72 = 1, dec+76 = 1
    //   0x1668F0  writes dec+76 = 0
    //   0x165300  READS dec+72 and dispatches the ladder
    //   0x165458  state-1 handler (ran 3x)
    //   0x165488  state-2 handler (never ran -- the thing we want to see fire)
    //
    // Interleave those and the answer is immediate: if a reset writer lands
    // between 0x164F78 and the next 0x165300, the bug is ordering. If 0x164F78
    // never appears before a 0x165300 at all, the bug is that the bootstrap
    // runs too late. If 0x165300 simply stops appearing, the pump died.
    //
    // Address list is env-driven rather than hard-coded so the NEXT ordering
    // question costs a run instead of a build -- the whole point of the method
    // change this stage is built on. Cost when unarmed is one relaxed bool
    // load, identical to the coverage contract above.
    constexpr uint32_t kOrderMaxAddrs = 24u;
    constexpr uint32_t kOrderLogSize = 16384u;
    std::atomic<bool> g_orderArmed{false};
    uint32_t g_orderAddrs[kOrderMaxAddrs] = {};
    uint32_t g_orderAddrCount = 0;
    std::atomic<uint32_t> g_orderNext{0};
    std::array<std::atomic<uint32_t>, kOrderLogSize> g_orderLog{};

    void orderRecord(uint32_t pc)
    {
        for (uint32_t i = 0; i < g_orderAddrCount; ++i)
        {
            if (g_orderAddrs[i] != pc)
            {
                continue;
            }
            const uint32_t n = g_orderNext.fetch_add(1u, std::memory_order_relaxed);
            if (n < kOrderLogSize)
            {
                g_orderLog[n].store(pc, std::memory_order_relaxed);
            }
            return;
        }
    }

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

        if (g_orderArmed.load(std::memory_order_relaxed))
        {
            orderRecord(pc);
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
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
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
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

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
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
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
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Boot Status: interrupts enabled, kernel mode, BEV clear (post-BIOS).
    //
    // The memset above wipes whatever R5900Context's constructor set, so this
    // is the only place the boot value is actually decided -- editing the
    // header's initializer would have had no effect at all.
    //
    // Leaving Status at zero silently breaks thread startup. The SDK's
    // StartThread wrapper at 0x175de0 opens with two gates against this
    // register, and a zeroed Status fails both:
    //
    //   0x175770  mfc0 $v0,$12 / xori 1 / andi 1  -> returns (IE^1)&1, i.e. 1
    //             when IE is clear. 0x175e00 branches on nonzero straight to
    //             the epilogue with $v0 = -1, so syscall 0x22 is never issued.
    //   0x17ed60  DIntr(): masks Status & 0x10000 (EIE) and returns 0 when
    //             already clear; 0x175e10 treats that 0 as failure and also
    //             returns -1.
    //
    // Both paths return through the normal epilogue with no print, which is
    // why run 52 showed four CreateThread calls, zero StartThread calls, and
    // not one line of diagnostic output.
    //
    // Only these two bits are set, because only these two are ever read: a
    // sweep of all 42 mfc0 $12 sites in the ELF found every SDK-range site
    // masking with `andi 0x1` or `lui 0x1, and` (sites above 0x480000 are
    // float data misdecoded as COP0 -- `lui 0x3333`, `lui 0x6666`). Nothing in
    // this runtime gates interrupt delivery on IE/EIE; only BEV and EXL are
    // consulted, in selectExceptionVector()/exception entry. So this is inert
    // to the host side and satisfies exactly the guest's own checks.
    m_cpuContext.cop0_status = COP0_STATUS_IE | COP0_STATUS_EIE;

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

namespace
{
    // Counts nonzero bytes in a buffer. Called once per MSCAL (~30/frame) over
    // 16 KB + 16 KB, which is negligible next to the 65536-cycle VU1 run it
    // precedes -- and unlike a log line it cannot be throttled into a false zero.
    uint64_t countNonzeroBytes(const uint8_t *p, size_t n)
    {
        if (!p)
            return 0;
        uint64_t count = 0;
        for (size_t i = 0; i < n; ++i)
            count += (p[i] != 0) ? 1u : 0u;
        return count;
    }
}

void PS2Runtime::probeVu1MemoryOccupancy()
{
    ps2_pipeline_stats::noteMax(ps2_pipeline_stats::g_vu1CodeNonzero,
                                countNonzeroBytes(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE));
    ps2_pipeline_stats::noteMax(ps2_pipeline_stats::g_vu1DataNonzero,
                                countNonzeroBytes(m_memory.getVU1Data(), PS2_VU1_DATA_SIZE));
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
                                 {
                                     ps2_pipeline_stats::g_vu1Runs.fetch_add(1, std::memory_order_relaxed);
                                     ps2_pipeline_stats::g_lastMscalPC.store(startPC, std::memory_order_relaxed);
                                     probeVu1MemoryOccupancy();
                                     m_vu1.state().dBitEnabled =
                                         (m_cpuContext.vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (m_cpuContext.vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     m_cpuContext.vu0_vpu_stat =
                                         (m_cpuContext.vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     ps2_pipeline_stats::g_vu1Runs.fetch_add(1, std::memory_order_relaxed);
                                     m_vu1.state().dBitEnabled =
                                         (m_cpuContext.vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (m_cpuContext.vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     m_cpuContext.vu0_vpu_stat =
                                         (m_cpuContext.vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
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
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    // ---- per-target hole census -------------------------------------------
    // `firstReport` above is ONE global bool for the whole process, so the big
    // [guest-branch:missing-target] dump names exactly one hole per run no
    // matter how many are hit. That makes the absence of a second record look
    // like "there is only one hole" when it actually means "the probe already
    // fired". Run 61 hit 0x186310 and stopped reporting; nothing downstream of
    // it could ever have been seen.
    //
    // This is the cheap complement: one short line per DISTINCT target, so a
    // single run enumerates the whole set. The heavy GPR/stack/trace dump stays
    // one-shot -- it is ~20 lines and only the first one is worth that.
    {
        static constexpr size_t kMaxDistinctHoles = 64;
        static std::mutex s_holeCensusMutex;
        static std::array<uint32_t, kMaxDistinctHoles> s_holeTargets{};
        static size_t s_holeCount = 0;
        static bool s_holeCapReported = false;

        bool isNewTarget = false;
        size_t index = 0;
        bool capJustHit = false;
        {
            std::lock_guard<std::mutex> lock(s_holeCensusMutex);
            for (; index < s_holeCount; ++index)
            {
                if (s_holeTargets[index] == targetPc)
                {
                    break;
                }
            }
            if (index == s_holeCount)
            {
                if (s_holeCount < kMaxDistinctHoles)
                {
                    s_holeTargets[s_holeCount++] = targetPc;
                    isNewTarget = true;
                }
                else if (!s_holeCapReported)
                {
                    s_holeCapReported = true;
                    capJustHit = true;
                }
            }
        }

        if (capJustHit)
        {
            std::cerr << "[cap] tag=hole saturated at " << kMaxDistinctHoles
                      << " distinct targets -- absence of a [hole] line past this"
                         " point is NOT evidence."
                      << std::endl;
        }
        if (isNewTarget)
        {
            std::ostringstream holeLine;
            holeLine << "[hole] n=" << index
                     << " target=0x" << std::hex << targetPc
                     << " source=0x" << sourcePc
                     << " ra=0x" << ra
                     << " op=" << (debugName ? debugName : "<unknown>")
                     << " kind=" << describeGuestBranchKind(kind)
                     << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
                     << std::dec;
            std::cerr << holeLine.str() << std::endl;
        }
    }

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

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

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
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
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

    // Ordered dispatch trace (env PS2X_ORDER=164f78,165300,...). Parsed once.
    // A list that parses to zero usable addresses prints a loud line rather
    // than arming a probe that would then report a confident, empty result --
    // an empty trace must never be readable as "none of these ever ran".
    if (const char *ord = std::getenv("PS2X_ORDER"); ord && *ord)
    {
        const char *c = ord;
        while (*c != '\0' && g_orderAddrCount < kOrderMaxAddrs)
        {
            while (*c == ',' || *c == ' ')
            {
                ++c;
            }
            if (*c == '\0')
            {
                break;
            }
            char *end = nullptr;
            const unsigned long v = std::strtoul(c, &end, 16);
            if (end == c)
            {
                break;
            }
            if (v != 0ul)
            {
                g_orderAddrs[g_orderAddrCount++] = static_cast<uint32_t>(v);
            }
            c = end;
        }
        if (g_orderAddrCount == 0u)
        {
            std::cerr << "[order] PS2X_ORDER set but parsed 0 addresses -- NOT"
                         " armed. Expected bare hex, comma separated, e.g."
                         " 164f78,165300"
                      << std::endl;
        }
        else
        {
            g_orderArmed.store(true, std::memory_order_relaxed);
            std::cerr << "[order] armed -- tracing dispatch ORDER of "
                      << std::dec << g_orderAddrCount << " addresses:"
                      << std::hex;
            for (uint32_t i = 0; i < g_orderAddrCount; ++i)
            {
                std::cerr << " 0x" << g_orderAddrs[i];
            }
            std::cerr << std::dec << " (log capacity " << kOrderLogSize << ")"
                      << std::endl;
        }
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
                uint64_t prevVblQ = 0, prevVblI = 0, prevVblS = 0;
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

                // Counts, not addresses. envHex would read a probe budget of
                // "1000" as 0x1000, so log caps get their own base-0 parser --
                // a cap that silently means something other than what the user
                // typed is the same false-negative machine the caps exist to
                // stop. 0 means unlimited.
                auto envDec = [](const char *name, uint32_t fallback) -> uint32_t
                {
                    const char *v = std::getenv(name);
                    if (v == nullptr || *v == '\0')
                    {
                        return fallback;
                    }
                    return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
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

                // ---- Stage 5.15: the ADX stream table ---------------------
                //
                // The exit test for the whole .SFD movie stage is "entry 1 of
                // the ADX stream table reaches state 3 with a live fd", and
                // until now that has only ever been read in PCSX2 -- this
                // runtime had no instrumentation for it at all. Every claim in
                // the state file that we sit at "state 1, fd 0" is borrowed
                // ground truth, not our own measurement.
                //
                // Layout, from the PCSX2 dump: base 0x0044beb8, 40 entries of
                // 0x60. Word 0 packs three bytes little-endian --
                //   [+0] active  [+1] state (1 init/idle, 3 streaming, 4 error)
                //   [+2] pause
                // -- so real hardware's first word reads 0x01000301 (state 3)
                // and ours read 0x01000101 (state 1). [+8] is the fd, [+0x45]
                // the command byte and [+0x49] the busy flag.
                //
                // Two deliberate design choices, both paid for by past runs:
                //
                // 1. SWEEP, DON'T SAMPLE. Gating on the single inferred address
                //    0x44bf18 would report confidently about the wrong bytes if
                //    the base or stride were off by anything. Walking all 40
                //    entries and counting the nonzero ones means a wrong layout
                //    shows up as nonzero=0/40 -- which convicts the probe, not
                //    the game.
                // 2. CARRY THE RIVAL READING. raw= is printed next to the
                //    decoded fields so a mis-split of word 0 is visible in the
                //    same line rather than being silently absorbed.
                //
                // The summary line is emitted every tick even when nothing is
                // nonzero, so this probe can never be quietly absent.
                const uint32_t adxBase = envHex("PS2_ADX_BASE", 0x0044beb8u) & ~3u;
                const uint32_t adxStride = envHex("PS2_ADX_STRIDE", 0x60u);
                const uint32_t adxCount = envHex("PS2_ADX_COUNT", 0x28u); // 40
                // Watchdog thread only, so a plain counter is enough. Bounded
                // for the same reason every other probe here is: a 10-minute
                // run must not be able to bury the interesting first seconds.
                uint32_t adxLogs = 0;
                // Raise with PS2X_ADX_STREAM_MAX (0 = unlimited). At 1 Hz the
                // fixed 600 was exactly 10 minutes, so any run longer than that
                // went blind without the log saying so.
                const uint32_t kAdxMax = envDec("PS2X_ADX_STREAM_MAX", 600u);
                // High-water mark across the whole run. A 1 Hz sampler cannot
                // see a state that comes and goes between ticks, so the peak is
                // carried separately -- otherwise a stream that really did
                // reach 3 for half a second would be indistinguishable from one
                // that never left 1.
                uint32_t adxMaxState = 0;
                // Same reasoning as adxMaxState, for the two fields the static
                // trace named as the actual gate.
                //
                // adxstmf_stat_exec (0x125898) reaches the file open ONLY when
                // byte [+0x45] == 1:
                //     v9 = *(char *)(a1 + 69);
                //     if ( v9 != 1 ) goto LABEL_38;      // return state, do nothing
                //     ...
                //     *(_DWORD *)(a1 + 8) = cvfsOpen(*(int *)(a1 + 80), ...);
                // and [+0x45] is written to 1 in exactly two places -- 0x124E38
                // (which also stores the filename pointer into [+80]) and
                // 0x1263B8. Both then SPIN until it clears, so a 1 Hz sample
                // ought to catch it latched -- but "ought to" is not a probe.
                // Peak-hold both, and read [+80]/[+84] so "the command never
                // came" is distinguishable from "it came and was cleared".
                uint32_t adxMaxCmd = 0;
                uint32_t adxMaxFd = 0;

                // Movie-player globals. Fixed addresses out of MovieCreate
                // (0x113AA0) and the movie poll (0x113920):
                //   0x54BD78/0x54BD7C  work adrs / size -- POSITIVE CONTROL, the
                //                      game TTY already printed 0x1806c00/0x332100
                //   0x54BD90           SofDec object ptr (0 => create failed)
                //   0x54BE2C           second create result (0 => create failed)
                //   0x54BE30           set to 1 once mwPlyGetMovieInfo returns a
                //                      header -- i.e. once the .SFD was actually
                //                      read. The render path (0x113860) is gated
                //                      on `dword_54BE2C && dword_54BE30`.
                //   0x54BE28           movie status byte; the caller treats
                //                      (status - 1) >= 2 as "finished".
                // Both create prints appeared in run 56 and neither failure
                // print did, so 0x54BD90 and 0x54BE2C should read nonzero. If
                // they don't, this probe is wrong before the game is.
                const uint32_t movBase = envHex("PS2_MOVIE_BASE", 0x0054bd78u) & ~3u;
                uint32_t movLogs = 0;
                // Raise with PS2X_MOVIE_MAX (0 = unlimited). Same 1 Hz / 600
                // = 10 minute blindness as [adx:stream].
                const uint32_t kMovMax = envDec("PS2X_MOVIE_MAX", 600u);
                uint32_t movMaxInfo = 0;
                uint32_t movMaxStat = 0;

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
                    uint64_t vblQ = 0, vblI = 0, vblS = 0;
                    ps2x_vblank_tick_sources(&vblQ, &vblI, &vblS);
                    const uint64_t dVblQ = vblQ - prevVblQ;
                    const uint64_t dVblI = vblI - prevVblI;
                    const uint64_t dVblS = vblS - prevVblS;
                    prevVblQ = vblQ;
                    prevVblI = vblI;
                    prevVblS = vblS;
                    prevBusyNs = curBusyNs;
                    prevResumes = curResumes;
                    prevVbl = curVbl;
                    ps2x_srd_stat_tick();
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

                    // Stage 5.15 ADX stream sweep. Same guard discipline as the
                    // watch/BSS reads above: plain RDRAM, and wrapped anyway
                    // because the watchdog must never be what takes a run down.
                    // t is incremented by the watchdog emit further below, so
                    // t+1 here labels the same tick as that line.
                    if ((kAdxMax == 0u || adxLogs < kAdxMax) && adxCount != 0u &&
                        adxStride != 0u)
                    {
                        std::ostringstream adx;
                        uint32_t nonzero = 0;
                        uint32_t detailed = 0;
                        try
                        {
                            for (uint32_t i = 0; i < adxCount; ++i)
                            {
                                const uint32_t ent = adxBase + (i * adxStride);
                                const uint32_t raw = m_memory.read32(ent);
                                if (raw == 0u)
                                {
                                    continue;
                                }
                                ++nonzero;

                                const uint32_t state = (raw >> 8) & 0xFFu;
                                if (state > adxMaxState)
                                {
                                    adxMaxState = state;
                                }

                                // Only the first few get expanded; nonzero=
                                // still counts them all, so a table that
                                // suddenly fills up is visible even when the
                                // detail is truncated.
                                const uint32_t fd = m_memory.read32(ent + 8u);
                                const uint32_t cmd = (m_memory.read32(ent + 0x44u) >> 8) & 0xFFu;
                                if (cmd > adxMaxCmd)
                                {
                                    adxMaxCmd = cmd;
                                }
                                if (fd > adxMaxFd)
                                {
                                    adxMaxFd = fd;
                                }

                                if (detailed < 4u)
                                {
                                    ++detailed;
                                    const uint32_t bsyWord = m_memory.read32(ent + 0x48u);
                                    // [+80] is the filename pointer 0x124E38
                                    // stores alongside cmd=1, and [+84] the
                                    // companion size. Nonzero [+80] with cmd=0
                                    // means the read WAS requested and cleared;
                                    // both zero means it never was.
                                    const uint32_t fname = m_memory.read32(ent + 80u);
                                    const uint32_t fsize = m_memory.read32(ent + 84u);
                                    adx << " | i=" << std::dec << i
                                        << " raw=0x" << std::hex << raw
                                        << " act=" << std::dec << (raw & 0xFFu)
                                        << " st=" << state
                                        << " pau=" << ((raw >> 16) & 0xFFu)
                                        << " fd=0x" << std::hex << fd
                                        << " cmd=0x" << cmd
                                        << " bsy=" << std::dec << ((bsyWord >> 8) & 0xFFu)
                                        << " f80=0x" << std::hex << fname
                                        << " f84=0x" << fsize << std::dec;
                                }
                            }
                        }
                        catch (const std::exception &)
                        {
                        }

                        ++adxLogs;
                        if (kAdxMax != 0u && adxLogs == kAdxMax)
                        {
                            std::cerr << "[cap] tag=adx:stream saturated at "
                                      << std::dec << kAdxMax << " --"
                                         " later sweeps are invisible; absence past"
                                         " this point is NOT evidence. Raise with"
                                         " PS2X_ADX_STREAM_MAX (0 = unlimited)."
                                      << std::endl;
                        }
                        // Emitted unconditionally, including the all-zero case.
                        // nonzero=0/40 is the signature of a wrong base or
                        // stride, and it must not look like "the probe was
                        // never reached".
                        std::cerr << "[adx:stream] t=" << std::dec << (t + 1) << "s"
                                  << " nonzero=" << nonzero << "/" << adxCount
                                  << " maxst=" << adxMaxState
                                  << " maxcmd=" << adxMaxCmd
                                  << " maxfd=0x" << std::hex << adxMaxFd
                                  << " base=0x" << adxBase
                                  << " stride=0x" << adxStride << std::dec
                                  << adx.str()
                                  << std::endl;
                    }

                    // Movie-player globals -- the layer ABOVE the ADX stream
                    // table. Unconditional emit, same rule as [adx:stream]: an
                    // all-zero line is a claim about the game only once the two
                    // work-area fields (which the game itself printed) match.
                    if (kMovMax == 0u || movLogs < kMovMax)
                    {
                        uint32_t wrkAdr = 0;
                        uint32_t wrkSiz = 0;
                        uint32_t obj = 0;
                        uint32_t crt2 = 0;
                        uint32_t info = 0;
                        uint32_t stat = 0;
                        uint32_t objState = 0;
                        uint32_t objFile = 0;
                        // mwPlySetFile's filename path (0x14F860) stamps a
                        // fixed 4-word signature into the player object:
                        //     a1[107] = 1;  a1[108] = 0;
                        //     a1[109] = 0;  a1[110] = 0xFFFFF;
                        // i.e. word offsets 428/432/436/440. The AFS path
                        // (0x14F8D0) sets [107]=1 too but fills 108..110 with
                        // real partition/file/size values, so the pair
                        // (src=1, len=0xfffff) is specifically "a plain
                        // filename was accepted". This is the cheapest
                        // yes/no on whether the strcpy chain actually ran,
                        // as opposed to the TTY merely not complaining.
                        uint32_t objSrc = 0;
                        uint32_t objLen = 0;
                        try
                        {
                            wrkAdr = m_memory.read32(movBase + 0x00u);  // 0x54BD78
                            wrkSiz = m_memory.read32(movBase + 0x04u);  // 0x54BD7C
                            obj = m_memory.read32(movBase + 0x18u);     // 0x54BD90
                            stat = m_memory.read32(movBase + 0xB0u) & 0xFFu; // 0x54BE28
                            crt2 = m_memory.read32(movBase + 0xB4u);    // 0x54BE2C
                            info = m_memory.read32(movBase + 0xB8u);    // 0x54BE30
                            if (info > movMaxInfo)
                            {
                                movMaxInfo = info;
                            }
                            if (stat > movMaxStat)
                            {
                                movMaxStat = stat;
                            }
                            // Only chase the object pointer when it looks like
                            // RDRAM; a garbage value must not be reported as if
                            // it were player state.
                            if (obj >= 0x00080000u && obj < 0x02000000u)
                            {
                                objState = m_memory.read32(obj + 8u);
                                objFile = m_memory.read32(obj + 444u);
                                objSrc = m_memory.read32(obj + 428u);
                                objLen = m_memory.read32(obj + 440u);
                            }
                        }
                        catch (const std::exception &)
                        {
                        }

                        ++movLogs;
                        if (kMovMax != 0u && movLogs == kMovMax)
                        {
                            std::cerr << "[cap] tag=movie saturated at "
                                      << std::dec << kMovMax << " --"
                                         " later samples are invisible; absence"
                                         " past this point is NOT evidence. Raise"
                                         " with PS2X_MOVIE_MAX (0 = unlimited)."
                                      << std::endl;
                        }
                        std::cerr << "[movie] t=" << std::dec << (t + 1) << "s"
                                  << " base=0x" << std::hex << movBase
                                  << " wrkAdr=0x" << wrkAdr
                                  << " wrkSiz=0x" << wrkSiz
                                  << " obj=0x" << obj
                                  << " crt2=0x" << crt2
                                  << " info=" << std::dec << info
                                  << " stat=" << stat
                                  << " maxinfo=" << movMaxInfo
                                  << " maxstat=" << movMaxStat
                                  << " objSt=" << objState
                                  << " objFile=0x" << std::hex << objFile
                                  << " objSrc=0x" << objSrc
                                  << " objLen=0x" << objLen << std::dec
                                  << std::endl;

                        // ---- [sfdc] the 144-byte SofDec info block ----------
                        //
                        // Run 75 closed the supply question the way run 74
                        // closed the server question -- by clearing the layer.
                        // [cvfs:stat] read calls=2 passable=2 retNz=2 armed=2
                        // with all five rejection counters at zero and
                        // devAfter=0x1, so 0x131190 -> 0x12E7B0 accepted every
                        // request it was handed and armed the SRD object both
                        // times. The devtype=0 that [crisrv:stat] reports is
                        // the RESTING value after completion, not a stuck one.
                        // Nothing below the movie layer is refusing work.
                        //
                        // What the movie layer itself reports has been the same
                        // in every run and was never chased: info=0 maxinfo=0,
                        // for all 298 samples of run 75 and all 298 of run 74.
                        // That field is dword_54BE30, and MovieUpdate (0x113920)
                        // writes it in exactly one place:
                        //
                        //     module_obj_init(&dword_54BD98, 0, 144);   // clear
                        //     if ( dword_54BD90 ) {
                        //         sub_14D2C0(dword_54BD90, &v6);        // fill
                        //         if ( v6[0] ) {                        // GATE
                        //             dword_54BD98 = v6[0];
                        //             dword_54BE30 = 1;                 // info
                        //             ...
                        //         }
                        //     }
                        //
                        // info==1 is therefore equivalent to v6[0]!=0, and
                        // info==0 for an entire run says v6[0] was zero on
                        // every single tick -- i.e. 0x14D2C0, the SofDec status
                        // query, never once reported a decoded stream. The whole
                        // body of MovieUpdate is skipped, which is why stat is
                        // pinned at 1 and why the movie is torn down by its own
                        // safety net after ~12 s (t=136..148) and again after
                        // ~16 s (t=175..191).
                        //
                        // This dump is deliberately NOT a wrapper on 0x14D2C0.
                        // The caller 0x113920 has two generated bodies --
                        // fn_113920_0x113920.cpp with ZERO dispatchGuestBranch
                        // calls and sub_00113920_0x113920.cpp with six -- and
                        // which one the dispatch table actually holds is not
                        // known. A replaceFunction wrapper that loses that coin
                        // flip reports 0/1 forever and reads exactly like "the
                        // status query is never called"
                        // ([[feedback_registerfunction_bypass]]). Reading the
                        // destination block instead has no such dependency: the
                        // block is cleared to zero and refilled every tick, so
                        //
                        //   w0!=0 at any sample  -> the query DOES fill it and
                        //                           the gate is passing; look
                        //                           higher, at the consumer
                        //   w0==0 with the rest
                        //   also all-zero        -> 0x14D2C0 wrote nothing, or
                        //                           was never reached
                        //   w0==0 but w1..wB set -> the query ran and reported a
                        //                           stream with status 0, which
                        //                           is a DIFFERENT bug from the
                        //                           two above and is invisible
                        //                           without the extra words
                        //
                        // The last reading is the reason this prints twelve
                        // words rather than one: a single-field probe cannot
                        // tell "nobody wrote" from "somebody wrote a zero"
                        // ([[feedback_degenerate_result_convicts_the_probe]]).
                        // nz counts the nonzero words so that verdict is on the
                        // line without decoding twelve hex fields by eye.
                        {
                            uint32_t w[12] = {0};
                            uint32_t nz = 0;
                            try
                            {
                                for (uint32_t i = 0; i < 12u; ++i)
                                {
                                    w[i] = m_memory.read32(movBase + 0x20u + i * 4u);
                                    if (w[i] != 0u) { ++nz; }
                                }
                            }
                            catch (const std::exception &)
                            {
                            }
                            std::cerr << "[sfdc] t=" << std::dec << (t + 1) << "s"
                                      << " nz=" << nz
                                      << " blk=0x" << std::hex << (movBase + 0x20u)
                                      << " w0=0x" << w[0]
                                      << " w1=0x" << w[1]
                                      << " w2=0x" << w[2]
                                      << " w3=0x" << w[3]
                                      << " w4=0x" << w[4]
                                      << " w5=0x" << w[5]
                                      << " w6=0x" << w[6]
                                      << " w7=0x" << w[7]
                                      << " w8=0x" << w[8]
                                      << " w9=0x" << w[9]
                                      << " wA=0x" << w[10]
                                      << " wB=0x" << w[11]
                                      << std::dec << std::endl;
                        }

                        // ---- [sfdcp] which gate inside 0x14D2C0 says no ------
                        //
                        // Run 76 answered [sfdc] with nz=0 in all 298 samples --
                        // and in doing so proved the probe was REDUNDANT rather
                        // than decisive. Re-reading MovieUpdate, every write into
                        // the 144-byte block sits inside the same if (v6[0]) gate
                        // that sets info, so "nz=0" and the info=0 we already had
                        // are the same measurement wearing two hats. The third
                        // reading that block was supposed to separate -- w0==0
                        // with w1..wB set -- is unreachable by construction. That
                        // is on the probe, not the run: a rival-reading field has
                        // to be fed by a DIFFERENT writer to be a rival at all
                        // ([[feedback_degenerate_result_convicts_the_probe]]).
                        //
                        // So walk into 0x14D2C0 instead. It has three separate
                        // ways to zero the caller's block, and they mean entirely
                        // different things:
                        //
                        //   if ( sub_1505D0(a1) != 1 )        // *(u32*)obj != 1
                        //       err(aE1122614Mwplyg); *a2 = 0; return;
                        //   v5 = sub_150008(a1);              // = *(int*)(obj+60)
                        //   if ( !v5 ) { *a2 = 0; return; }
                        //   sub_167320(v5, v11);              // decode one frame
                        //   if ( !v11[0] ) { *a2 = 0; return; }
                        //   ...
                        //   *(obj+120) = v11[0];  ++*(obj+124);   // FRAME COUNTER
                        //
                        // Note what did NOT happen in run 76: aE1122614Mwplyg
                        // never appears in the log. That is not evidence the first
                        // gate passed. The error printer is 0x1548B8, which
                        // formats into dword_55A780 and hands it to
                        // callback_dispatch_fmt_c_0 -- a user-installed handler.
                        // If the game never installs one the message is formatted
                        // and dropped, so its absence says nothing at all. Read
                        // the gate inputs directly rather than waiting for a
                        // complaint that may have nowhere to go.
                        //
                        //   p0 != 1              -> gate 1: the player object is
                        //                           not in the live state; the
                        //                           handle is stale or was torn
                        //                           down under us
                        //   p0 == 1, dec == 0    -> gate 2: obj+60 was never set,
                        //                           i.e. the decoder was never
                        //                           attached to the player -- the
                        //                           failure is at OPEN time, one
                        //                           layer above the streaming
                        //   dec != 0, fcnt flat  -> gate 3: the decoder exists and
                        //                           is being asked every tick but
                        //                           yields no frame -- starved, so
                        //                           the question returns to who
                        //                           feeds it
                        //   fcnt RISING          -> frames ARE decoding and the
                        //                           bug is above 0x14D2C0 in the
                        //                           consumer; every conclusion
                        //                           drawn from info=0 so far would
                        //                           need re-reading
                        //
                        // fcnt (obj+124) and drop (obj+132) are the two counters
                        // that only ever move on a real decode, so they are the
                        // fields that can tell "slow" from "dead" -- an unsampled
                        // counter cannot ([[feedback_measure_dont_infer_rates]]).
                        // Everything is read from the same cached m_memory as the
                        // rest of this block, so a zero obj still prints a full
                        // row rather than vanishing.
                        if (obj >= 0x00080000u && obj < 0x02000000u)
                        {
                            uint32_t p0 = 0, dec = 0, skip = 0, n24 = 0;
                            uint32_t frm = 0, fcnt = 0, drop = 0, p184 = 0, lat = 0;
                            uint32_t d2408 = 0, d2412 = 0, d0 = 0;
                            uint32_t cb = 0;
                            try
                            {
                                p0 = m_memory.read32(obj + 0u);
                                n24 = m_memory.read32(obj + 24u);
                                dec = m_memory.read32(obj + 60u);
                                skip = m_memory.read32(obj + 84u);
                                frm = m_memory.read32(obj + 120u);
                                fcnt = m_memory.read32(obj + 124u);
                                drop = m_memory.read32(obj + 132u);
                                p184 = m_memory.read32(obj + 184u);
                                lat = m_memory.read32(obj + 724u);
                                cb = m_memory.read32(0x004611ACu);
                                if (dec >= 0x00080000u && dec < 0x02000000u)
                                {
                                    d0 = m_memory.read32(dec + 0u);
                                    d2408 = m_memory.read32(dec + 2408u);
                                    d2412 = m_memory.read32(dec + 2412u);
                                }
                            }
                            catch (const std::exception &)
                            {
                            }
                            std::cerr << "[sfdcp] t=" << std::dec << (t + 1) << "s"
                                      << " obj=0x" << std::hex << obj
                                      << " p0=0x" << p0
                                      << " dec=0x" << dec
                                      << " skip=0x" << skip
                                      << " n24=0x" << n24
                                      << " frm=0x" << frm
                                      << std::dec << " fcnt=" << fcnt
                                      << " drop=" << drop
                                      << std::hex
                                      << " p184=0x" << p184
                                      << " lat=0x" << lat
                                      << " d0=0x" << d0
                                      << " dRd=0x" << d2408
                                      << " dWr=0x" << d2412
                                      << " cb=0x" << cb
                                      << std::dec << std::endl;
                        }

                        // ---- [sfdvt] the two-level table 0x14D2C0 dies in ------
                        //
                        // Run 77 answered [sfdcp] and cleared two of the three
                        // gates outright, in all 52 samples across all three
                        // movie sessions (t=82..93, t=117..132, t=275..298):
                        //
                        //   p0  = 0x1          gate 1 passes; the player object
                        //                      IS in the live state
                        //   dec = 0x1b12cc0    gate 2 passes; a decoder IS
                        //                      attached, and the address sits
                        //                      inside the movie work buffer
                        //                      (wrkAdr=0x1806c00 + wrkSiz=
                        //                      0x332100 -> ..0x1b38d00), so it
                        //                      is a real allocation, not junk
                        //   fcnt= 0, drop = 0  gate 3 FAILS, every tick, and it
                        //                      is not even dropping frames -- it
                        //                      produces nothing at all
                        //
                        // So the failure is exactly one call deep now:
                        //
                        //     v5 = vtable_dispatch(dec, 6, 11, a2, 0);
                        //     if ( !*(_DWORD *)a2 ) -> caller writes info=0
                        //
                        // and vtable_dispatch (0x16B318) is a TWO-LEVEL table:
                        //
                        //     slot = *(u32 *)(a1 + 68 * a2 + 7996);   // a2 = 6
                        //     if ( !slot ) return 0;                  // silent
                        //     return ((fn)*(u32 *)(slot + 4 * a3))(); // a3 = 11
                        //
                        // 68*6 + 7996 = 8404, so the handler-table pointer for
                        // stream slot 6 is dec+8404 and the method pointer is
                        // slot+44. Both levels return "no frame" the same way
                        // from the caller's side, and they are completely
                        // different bugs:
                        //
                        //   vt6 == 0           the demux never registered a
                        //                      handler for this stream type --
                        //                      an SFD-header / stream-open bug,
                        //                      nothing to do with decoding
                        //   vt6 != 0, fn11 = 0 the table exists with a hole in
                        //                      it; registration ran but partly
                        //   fn11 != 0, body=0  fn11 is a real guest address that
                        //                      HAS NO RECOMPILED BODY -- the
                        //                      Stage 5.11 class exactly
                        //                      ([[project_stage511_gsdump_replay]]):
                        //                      a function only ever taken as a
                        //                      pointer, never a jal target, so
                        //                      the recompiler emitted nothing
                        //                      and the JALR returns a stale v0.
                        //                      That cost ~38 run cycles last
                        //                      time because the symptom pointed
                        //                      at the consumer, not the callee.
                        //   fn11 != 0, body=1  the pointer is live and callable;
                        //                      the frame really is being asked
                        //                      for and refused inside real
                        //                      guest code, and the question
                        //                      moves into that function
                        //
                        // body is answered with hasFunction() rather than left
                        // for a later run: the runtime already knows whether an
                        // address has a body, so asking costs nothing and turns
                        // a whole run cycle into a field on this line.
                        //
                        // slots= dumps the same pointer for a2 = 0..11 so a
                        // vt6 == 0 reading arrives with its own context. If
                        // every slot is zero the table was never built at all;
                        // if slots 0..5 are populated and only 6 is empty then
                        // registration ran and skipped this stream type, which
                        // are again different repairs. A single-slot probe
                        // could not tell those apart
                        // ([[feedback_degenerate_result_convicts_the_probe]]),
                        // and the geometry -- 68 bytes per slot, +7996 -- comes
                        // from the decompiled accessors at 0x16B380/0x16B3B0/
                        // 0x16B3C8, not from an inferred constant
                        // ([[feedback_probe_gate_on_shape_not_address]]).
                        if (obj >= 0x00080000u && obj < 0x02000000u)
                        {
                            uint32_t dcx = 0, vt6 = 0, fn11 = 0;
                            uint32_t f7984 = 0, f7988 = 0;
                            uint32_t slots[12] = {0};
                            int body = -1;
                            try
                            {
                                dcx = m_memory.read32(obj + 60u);
                                if (dcx >= 0x00080000u && dcx < 0x02000000u)
                                {
                                    for (uint32_t i = 0; i < 12u; ++i)
                                    {
                                        slots[i] = m_memory.read32(dcx + 68u * i + 7996u);
                                    }
                                    vt6 = slots[6];
                                    f7984 = m_memory.read32(dcx + 68u * 6u + 7984u);
                                    f7988 = m_memory.read32(dcx + 68u * 6u + 7988u);
                                    if (vt6 >= 0x00080000u && vt6 < 0x02000000u)
                                    {
                                        fn11 = m_memory.read32(vt6 + 4u * 11u);
                                        if (fn11 != 0u)
                                        {
                                            body = hasFunction(fn11) ? 1 : 0;
                                        }
                                    }
                                }
                            }
                            catch (const std::exception &)
                            {
                            }
                            std::cerr << "[sfdvt] t=" << std::dec << (t + 1) << "s"
                                      << " dec=0x" << std::hex << dcx
                                      << " vt6=0x" << vt6
                                      << " fn11=0x" << fn11
                                      << std::dec << " body=" << body
                                      << std::hex
                                      << " f7984=0x" << f7984
                                      << " f7988=0x" << f7988
                                      << " slots=";
                            for (uint32_t i = 0; i < 12u; ++i)
                            {
                                std::cerr << (i != 0u ? "," : "") << "0x" << slots[i];
                            }
                            std::cerr << std::dec << std::endl;
                        }

                        // ---- [sfdst] the SofDec state machine at 0x165300 -------
                        //
                        // Run 78 answered [sfdvt] and it came back the GOOD way,
                        // identically in all 53 samples:
                        //
                        //   vt6  = 0x4bf780   the handler table for stream slot 6
                        //                     IS registered
                        //   fn11 = 0x16c620   the method pointer is real
                        //   body = 1          and it HAS a recompiled body, so
                        //                     this is NOT the Stage 5.11 class
                        //                     ([[project_stage511_gsdump_replay]])
                        //                     -- the call really is being made and
                        //                     really is returning
                        //   slots= 0x4bf220,0x4bf258,0x4bf2a0,0x4befe8,0,0,
                        //          0x4bf780,0x4bf040,0x4bf748,0,0,0
                        //                     the table is built, not half-built
                        //
                        // So read what 0x16C620 actually does:
                        //
                        //     if ( (u64)(*(u32 *)(a1 + 72) - 3) < 2 )
                        //         return wrap_sub_unk_k(a1, *(u32 *)(a1 + 8408));
                        //     *a2 = 0;                      // <-- our symptom
                        //     return 0;
                        //
                        // dec+72 must be 3 or 4. Anything else zeroes the caller's
                        // word silently, which is exactly the "no frame, no error,
                        // no drop" reading [sfdcp] gave. dec+72 is the CURRENT
                        // state of the machine at 0x165300 and dec+76 is the
                        // REQUESTED one:
                        //
                        //     if ( (u64)(state - 1) < 4 && *(u32 *)(a1 + 68) ) {
                        //         *(u32 *)(a1 + 68) = 0;    // one-shot tick flag
                        //         switch ( state ) { 1: ..f  2: ..g  3: ..m
                        //                            4: ..s2 6: ..129 }
                        //     }
                        //
                        // Note the machine does not run at all unless state is in
                        // 1..4, so a state of 0 or 5 is terminal by construction --
                        // no amount of ticking recovers it.
                        //
                        // 1 -> 2 needs only a request (0x165458: req in {2,3,4,6}).
                        // 2 -> 3/4 is the real gate (0x165488): it first calls the
                        // preroll-ready check 0x165548, and STAYS AT 2 forever if
                        // that returns 0. And that check reads precisely the two
                        // fields [sfdvt] already measured as zero:
                        //
                        //     v3 = 1;
                        //     if ( *(u32 *)(a1 + 2572 + 4*5) )
                        //         v3 = f7984(6) | f7988(6);   // BOTH 0 in run 78
                        //     v6 = 1;
                        //     if ( *(u32 *)(a1 + 2572 + 4*6) )
                        //         v6 = f7984(7) | f7988(7);
                        //     if ( v3 ) return v6 != 0;
                        //     return 0;
                        //
                        // If en5 is nonzero then v3 = 0|0 = 0 and the check returns
                        // 0 on every tick -- state pins at 2, dec+72 never reaches
                        // 3, and 0x16C620 zeroes the word for the rest of the run.
                        // That would explain the whole stall end to end. But it is
                        // a hypothesis with an untested premise (en5), and a probe
                        // that only printed the fields supporting it would confirm
                        // itself no matter what
                        // ([[feedback_degenerate_result_convicts_the_probe]]).
                        //
                        // So print the state FIRST and let it referee:
                        //
                        //   st == 3 or 4      -> the premise above is WRONG. The
                        //                        gate is passing and the refusal is
                        //                        inside wrap_sub_unk_k (0x159090),
                        //                        one layer further in. sIdx says
                        //                        which stream it was asked about
                        //   st == 2           -> pinned in preroll; en5/en6 and the
                        //                        four a*_84/a*_88 counters say which
                        //                        arm of 0x165548 is the zero, and
                        //                        rdy re-evaluates it here so the
                        //                        verdict does not depend on reading
                        //                        six hex fields by eye
                        //   st == 1           -> stopped and never asked to play;
                        //                        req names who should have asked
                        //   st == 0 or 5      -> terminal, the machine is not even
                        //                        eligible to tick -- a teardown ran
                        //   tick == 0 always  -> the state is irrelevant: nobody is
                        //                        driving 0x165300 at all, and the
                        //                        question moves to its caller
                        //
                        // tick is the one field here that is a one-shot: 0x165300
                        // clears dec+68 the moment it runs, so sampling it once a
                        // second will read 0 on a HEALTHY machine too. It is dumped
                        // for the all-zero-forever case only, and must not be read
                        // as "not ticking" on its own
                        // ([[feedback_probe_the_final_value]]).
                        if (obj >= 0x00080000u && obj < 0x02000000u)
                        {
                            uint32_t dcx = 0;
                            uint32_t st = 0xFFFFFFFFu, req = 0xFFFFFFFFu, tick = 0;
                            uint32_t sIdx = 0, d8392 = 0, d8396 = 0;
                            uint32_t en[8] = {0};
                            uint32_t a5_84 = 0, a5_88 = 0, a7_84 = 0, a7_88 = 0;
                            int rdy = -1;
                            try
                            {
                                dcx = m_memory.read32(obj + 60u);
                                if (dcx >= 0x00080000u && dcx < 0x02000000u)
                                {
                                    st = m_memory.read32(dcx + 72u);
                                    req = m_memory.read32(dcx + 76u);
                                    tick = m_memory.read32(dcx + 68u);
                                    sIdx = m_memory.read32(dcx + 8408u);
                                    d8392 = m_memory.read32(dcx + 8392u);
                                    d8396 = m_memory.read32(dcx + 8396u);
                                    for (uint32_t i = 0; i < 8u; ++i)
                                    {
                                        en[i] = m_memory.read32(dcx + 2572u + 4u * i);
                                    }
                                    a5_84 = m_memory.read32(dcx + 68u * 6u + 7984u);
                                    a5_88 = m_memory.read32(dcx + 68u * 6u + 7988u);
                                    a7_84 = m_memory.read32(dcx + 68u * 7u + 7984u);
                                    a7_88 = m_memory.read32(dcx + 68u * 7u + 7988u);

                                    // Re-run 0x165548 exactly as written, so the
                                    // verdict on the line is the guest's own
                                    // arithmetic and not a paraphrase of it.
                                    uint32_t v3 = 1u;
                                    if (en[5] != 0u) { v3 = a5_84 | a5_88; }
                                    uint32_t v6 = 1u;
                                    if (en[6] != 0u) { v6 = a7_84 | a7_88; }
                                    rdy = (v3 != 0u) ? ((v6 != 0u) ? 1 : 0) : 0;
                                }
                            }
                            catch (const std::exception &)
                            {
                            }
                            std::cerr << "[sfdst] t=" << std::dec << (t + 1) << "s"
                                      << " dec=0x" << std::hex << dcx
                                      << std::dec
                                      << " st=" << (int)st
                                      << " req=" << (int)req
                                      << " tick=" << tick
                                      << " rdy=" << rdy
                                      << std::hex
                                      << " sIdx=0x" << sIdx
                                      << " d8392=0x" << d8392
                                      << " d8396=0x" << d8396
                                      << " a5_84=0x" << a5_84
                                      << " a5_88=0x" << a5_88
                                      << " a7_84=0x" << a7_84
                                      << " a7_88=0x" << a7_88
                                      << " en=";
                            for (uint32_t i = 0; i < 8u; ++i)
                            {
                                std::cerr << (i != 0u ? "," : "") << "0x" << en[i];
                            }
                            std::cerr << std::dec << std::endl;
                        }

                        // ---- [sfdbuf] is there any MOVIE in the movie buffer ----
                        //
                        // Run 79 answered [sfdst] and the hypothesis held, with the
                        // probe's own referee field agreeing. 30 of 32 samples read
                        // identically:
                        //
                        //   st=2 req=3 rdy=0 sIdx=0x3
                        //   a5_84=0 a5_88=0 a7_84=0 a7_88=0
                        //   en=0,1,1,1,1,1,1,1
                        //
                        // st printed FIRST and it came back 2, not 3/4 -- so the
                        // premise was not assumed, it was measured. req=3 means play
                        // WAS requested. en5=1 and en6=1 mean both arms of 0x165548
                        // are live, and all four counters they read are zero, so
                        //
                        //     v3 = a5_84 | a5_88 = 0   ->  return 0, every tick
                        //
                        // and the machine can never leave state 2. dec+72 stays 2,
                        // 0x16C620 keeps taking its *a2 = 0 path, MovieUpdate keeps
                        // skipping its whole body, info stays 0, and the movie layer
                        // times out after ~12-16 s. That is the entire stall, end to
                        // end, and it is now measured rather than inferred.
                        //
                        // One sample read st=1 req=1 and one read tick=0. Those are
                        // the pre-roll and the teardown edges, not a second failure.
                        //
                        // So the question is who was supposed to fill slot 6 and
                        // slot 7. The slot record is dec + 7984 + 68*i; +0 and +4 are
                        // zeroed by 0x16ADD0 at construction and never written by any
                        // expression of the form (base + 68*n + 7984) anywhere in the
                        // image -- they are filled by the per-stream handler in the
                        // vtable at slot+12, i.e. by the DEMUXER as it consumes data.
                        // Both being zero says no stream ever received a byte.
                        //
                        // And that lines up with the one number in the supply layer
                        // that has been odd since run 74 and was never chased: 365 of
                        // 22659 sectors, 1.6%, with oReq=0x0. Those 365 sectors were
                        // issued=3 done=3 err=0 -- the read layer says it delivered
                        // them. But "the transfer completed" and "the bytes arrived
                        // in EE RAM" are different claims, and this project has
                        // already been caught conflating them once: mcserv reported
                        // success while writing nothing, because the payload goes to
                        // an EE block named in the send struct and our recv path was
                        // only four bytes wide ([[project_stage512_memory_card]]).
                        // A starved demuxer downstream of a "successful" read is
                        // exactly that shape again.
                        //
                        // So look at the bytes. An .SFD is an MPEG program stream:
                        // every 2048-byte pack begins 00 00 01 BA, which read32 sees
                        // as 0xBA010000 on this little-endian host. The scan is
                        // 4-aligned across the whole work buffer rather than only at
                        // sector boundaries, because the read target (0x1868a40 in
                        // run 78) is not 2048-aligned relative to wrkAdr and an
                        // aligned-only scan would report zero packs on a buffer that
                        // is completely fine ([[feedback_probe_gate_on_shape_not_address]]).
                        //
                        //   nzw == 0            -> the buffer is empty. The read
                        //                          "completed" and delivered nothing,
                        //                          and the whole SofDec chain is
                        //                          starved for a reason that has
                        //                          nothing to do with SofDec
                        //   nzw > 0, ba == 0    -> bytes are present but they are not
                        //                          a program stream: wrong LSN, wrong
                        //                          sector geometry, or a buffer that
                        //                          only ever held decoder scratch
                        //   ba > 0              -> the data IS there and IS valid.
                        //                          Nothing is wrong with the read
                        //                          path, and the question becomes who
                        //                          was supposed to hand the buffer to
                        //                          the demuxer and never did
                        //
                        // nzw alone proves nothing in either direction -- the work
                        // buffer holds decoder state as well as stream data, so it is
                        // nonzero on a starved machine too. ba is the discriminator;
                        // nzw is there so a ba==0 reading arrives knowing whether it
                        // is looking at an empty buffer or a wrong one
                        // ([[feedback_degenerate_result_convicts_the_probe]]). bb/bd/
                        // e0 are the system-header and PES codes, carried so that a
                        // buffer holding stream data with a damaged FIRST pack still
                        // reports as a program stream instead of as garbage.
                        //
                        // The gate is on geometry, and a failed gate still prints a
                        // full row with gate=0 rather than vanishing.
                        {
                            uint32_t nzw = 0, ba = 0, bb = 0, bd = 0, e0 = 0;
                            uint32_t fnz = 0xFFFFFFFFu, fba = 0xFFFFFFFFu;
                            uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
                            int gate = 0;
                            uint32_t lim = 0;
                            if (wrkAdr >= 0x00080000u && wrkAdr < 0x02000000u &&
                                wrkSiz >= 0x1000u && wrkSiz <= 0x00400000u &&
                                (wrkAdr + wrkSiz) <= 0x02000000u)
                            {
                                gate = 1;
                                lim = wrkSiz & ~3u;
                                try
                                {
                                    for (uint32_t o = 0; o < lim; o += 4u)
                                    {
                                        uint32_t w = m_memory.read32(wrkAdr + o);
                                        if (w == 0u) { continue; }
                                        ++nzw;
                                        if (fnz == 0xFFFFFFFFu) { fnz = o; }
                                        if (w == 0xBA010000u)
                                        {
                                            ++ba;
                                            if (fba == 0xFFFFFFFFu) { fba = o; }
                                        }
                                        else if (w == 0xBB010000u) { ++bb; }
                                        else if (w == 0xBD010000u) { ++bd; }
                                        else if (w == 0xE0010000u) { ++e0; }
                                    }
                                    uint32_t hb = (fba != 0xFFFFFFFFu)
                                                      ? fba
                                                      : ((fnz != 0xFFFFFFFFu) ? fnz : 0u);
                                    h0 = m_memory.read32(wrkAdr + hb + 0u);
                                    h1 = m_memory.read32(wrkAdr + hb + 4u);
                                    h2 = m_memory.read32(wrkAdr + hb + 8u);
                                    h3 = m_memory.read32(wrkAdr + hb + 12u);
                                }
                                catch (const std::exception &)
                                {
                                }
                            }
                            std::cerr << "[sfdbuf] t=" << std::dec << (t + 1) << "s"
                                      << " gate=" << gate
                                      << " wA=0x" << std::hex << wrkAdr
                                      << " wS=0x" << wrkSiz
                                      << std::dec
                                      << " nzw=" << nzw
                                      << " ba=" << ba
                                      << " bb=" << bb
                                      << " bd=" << bd
                                      << " e0=" << e0
                                      << std::hex
                                      << " fnz=0x" << fnz
                                      << " fba=0x" << fba
                                      << " h0=0x" << h0
                                      << " h1=0x" << h1
                                      << " h2=0x" << h2
                                      << " h3=0x" << h3
                                      << std::dec << std::endl;
                        }

                        // ---- [sfdsup] does anyone HAND the data to the demuxer --
                        //
                        // Run 80 answered [sfdbuf] and it came back the third way,
                        // the one that moves the whole investigation:
                        //
                        //   gate=1 wA=0x1806c00 wS=0x332100
                        //   nzw=256207 ba=366 bb=3 e0=307
                        //   fba=0x61e40
                        //   h0=0xba010000 h1=0x10021 h2=0x5278801 h3=0xbb010000
                        //
                        // 366 MPEG pack headers, 3 system headers, 307 video PES.
                        // fba=0x61e40 is EXACTLY the offset of the read target
                        // (0x1868a40 - 0x1806c00), so the sectors landed precisely
                        // where they were aimed. And the first sixteen bytes decode
                        // cleanly by hand: 00 00 01 BA, then 0x21, whose top nibble
                        // 0010 is the MPEG-1 pack marker, then the SCR, then at
                        // +12 -- the exact length of an MPEG-1 pack header -- the
                        // system header 00 00 01 BB. This is not "some bytes are
                        // present". This is a byte-exact, well-formed MPEG-1
                        // program stream sitting in RAM.
                        //
                        // It is also LIVE. The counts step at t=128 (ba 360->366,
                        // e0 246->307), drop to ba=0 at t=289, and refill at t=290
                        // (ba=366, e0=313). Three movies, three fills. The read
                        // path works, the SRD layer works, the sectors are real.
                        //
                        // So every one of the "starved" readings is dead. The data
                        // is there and nobody eats it. That kills the reading the
                        // 365-of-22659 oReq=0x0 anomaly was pointing at, and it is
                        // worth saying plainly rather than quietly dropping it: the
                        // supply layer was not the bug, and the memory-card-shaped
                        // theory built on top of it was wrong.
                        //
                        // Which makes the question narrow and mechanical. The
                        // decoder is told about new data in exactly one place --
                        // sub_168320 @ 0x168320:
                        //
                        //     v5 = *(u32 *)(a1 + 14000);            // stream info
                        //     if ( v5 && !obj_set_unk_z_j(a1) ) {
                        //         table_set_by_stride(a1, 47, 0);
                        //         if ( !*(u32 *)(v5 + 3512) ) table_set_by_stride(a1, 5, 0);
                        //         if ( !*(u32 *)(v5 + 3516) ) table_set_by_stride(a1, 6, 0);
                        //         *(u64 *)(a1 + 14004) = *(u64 *)a2;   // buf, size
                        //         *(u32 *)(a1 + 14012) = *(u32 *)(a2 + 8);
                        //         return noop_sub_b280(a1, 13);        // BROADCAST
                        //     }
                        //
                        // That is the only writer of dec+14004/14008/14012 anywhere
                        // in the image, and method 13 broadcast to all nine slots is
                        // the only thing that could plausibly move slot+7984. If it
                        // never runs, the slots stay zero, 0x165548 keeps returning
                        // 0, dec+72 stays 2, and every symptom from run 74 onward
                        // follows -- with a perfectly good buffer sitting untouched.
                        //
                        // There is already circumstantial evidence it never runs:
                        // en[] read 0,1,1,1,1,1,1,1 in runs 79 and 80, and en[] is a
                        // 400-byte template memcpy'd from dword_460F60 at
                        // construction. If 0x168320 had ever executed it would have
                        // called table_set_by_stride on index 5 or 6 (or 47) and we
                        // would expect to see the template disturbed. But that is an
                        // inference from an unread template, which is precisely the
                        // kind of reasoning that has cost this project run cycles
                        // before ([[feedback_gate_probe_on_shape_not_address]]), so
                        // read the single-writer fields directly instead:
                        //
                        //   b0/b1/b2 all zero    -> 0x168320 NEVER RAN. The decoder
                        //                           was never told the data exists.
                        //                           The bug is entirely above it, in
                        //                           whoever owns the read completion
                        //                           and should be forwarding it
                        //   b0 nonzero, slots 0  -> it DID run, the broadcast went
                        //                           out, and the per-slot method 13
                        //                           refused or dropped the data. The
                        //                           bug moves inside that handler
                        //   b0 nonzero and MOVING-> supply is running repeatedly and
                        //                           being consumed, which would mean
                        //                           slot+7984 is not the counter we
                        //                           think it is and the geometry
                        //                           needs re-deriving
                        //
                        // m[] and the mb bitmask answer the other half in the same
                        // line. [sfdvt] proved method 11 has a recompiled body, but
                        // method 11 is the frame FETCH; method 13 is the data
                        // SUPPLY, and nothing has ever checked it. A method that is
                        // only ever reached through a vtable and never as a jal
                        // target is exactly the Stage 5.11 shape -- the unrecompiled
                        // qsort comparator that returned a stale v0 for every call
                        // and cost ~38 run cycles
                        // ([[project_stage511_gsdump_replay]]). Dumping all sixteen
                        // pointers with a hasFunction bit each settles it for the
                        // whole handler at once instead of one method per run.
                        //
                        // sup=1 means dec+14000 is populated, i.e. 0x168320 would
                        // pass its own first gate if it were called. s3512/s3516 are
                        // the two fields that decide whether en[5]/en[6] get cleared,
                        // so a b0==0 reading still arrives knowing what WOULD have
                        // happened. The gate is on address shape and a failed gate
                        // prints a full row rather than vanishing.
                        if (obj >= 0x00080000u && obj < 0x02000000u)
                        {
                            uint32_t dcx = 0, sup = 0, vt6 = 0;
                            uint32_t b0 = 0, b1 = 0, b2 = 0;
                            uint32_t s3496 = 0, s3512 = 0, s3516 = 0, s3520 = 0;
                            uint32_t m[16] = {0};
                            uint32_t mb = 0;
                            int gate = 0;
                            try
                            {
                                dcx = m_memory.read32(obj + 60u);
                                if (dcx >= 0x00080000u && dcx < 0x02000000u)
                                {
                                    gate = 1;
                                    b0 = m_memory.read32(dcx + 14004u);
                                    b1 = m_memory.read32(dcx + 14008u);
                                    b2 = m_memory.read32(dcx + 14012u);
                                    sup = m_memory.read32(dcx + 14000u);
                                    if (sup >= 0x00080000u && sup < 0x02000000u)
                                    {
                                        s3496 = m_memory.read32(sup + 3496u);
                                        s3512 = m_memory.read32(sup + 3512u);
                                        s3516 = m_memory.read32(sup + 3516u);
                                        s3520 = m_memory.read32(sup + 3520u);
                                    }
                                    vt6 = m_memory.read32(dcx + 68u * 6u + 7996u);
                                    if (vt6 >= 0x00080000u && vt6 < 0x02000000u)
                                    {
                                        for (uint32_t i = 0; i < 16u; ++i)
                                        {
                                            m[i] = m_memory.read32(vt6 + 4u * i);
                                            if (m[i] != 0u && hasFunction(m[i]))
                                            {
                                                mb |= (1u << i);
                                            }
                                        }
                                    }
                                }
                            }
                            catch (const std::exception &)
                            {
                            }
                            std::cerr << "[sfdsup] t=" << std::dec << (t + 1) << "s"
                                      << " gate=" << gate
                                      << " dec=0x" << std::hex << dcx
                                      << " b0=0x" << b0
                                      << " b1=0x" << b1
                                      << " b2=0x" << b2
                                      << " sup=0x" << sup
                                      << " s3496=0x" << s3496
                                      << " s3512=0x" << s3512
                                      << " s3516=0x" << s3516
                                      << " s3520=0x" << s3520
                                      << " vt6=0x" << vt6
                                      << " mb=0x" << mb
                                      << " m13=0x" << m[13]
                                      << " m=";
                            for (uint32_t i = 0; i < 16u; ++i)
                            {
                                std::cerr << (i != 0u ? "," : "") << "0x" << m[i];
                            }
                            std::cerr << std::dec << std::endl;
                        }

                        // ---- [sfdapi] body check + pointer hunt ---------------
                        //
                        // Run 81 closed [sfdsup] as cleanly as this project has
                        // ever closed anything. The reading, all 69 samples,
                        // t=80 through t=295, not one variation:
                        //
                        //   gate=1 dec=0x1b12cc0
                        //   b0=0x0 b1=0xfffffffd b2=0x1 sup=0x0
                        //   vt6=0x4bf780 mb=0x3fff m13=0x16c690
                        //
                        // Compare that against the constructor for those exact
                        // four words, sub_1679D8 @ 0x1679D8, which the object
                        // constructor calls as mem_fill_unk_z_z_8(dec + 14000):
                        //
                        //     a1[0] = 0;   a1[1] = 0;
                        //     a1[2] = -3;  a1[3] = 1;
                        //
                        // dec+14000 = 0, dec+14004 = 0, dec+14008 = 0xfffffffd
                        // (= -3), dec+14012 = 1. Word for word, sign and all,
                        // the values sub_1679D8 leaves behind. Not "looks
                        // uninitialised" -- provably untouched since
                        // construction. Nothing in the guest has written a
                        // single one of those four words in five minutes of
                        // runtime, across three separate movies.
                        //
                        // So the three-way split resolves to branch one, and it
                        // resolves harder than expected. sub_168320 never ran.
                        // But neither did sub_167B68 @ 0x167B68, the only writer
                        // of dec+14000 in the image -- so even if 0x168320 were
                        // called this instant it would read v5 = 0, fail its
                        // first gate, and return without storing the buffer. The
                        // supply path is not merely idle, it is structurally
                        // unable to run: its precondition was never established.
                        //
                        // Two things are now ruled out and should not be
                        // re-derived. mb=0x3fff means methods 0..13 of the slot
                        // 6 vtable all have recompiled bodies, so method 13 is
                        // not a Stage 5.11 casualty. And m[11]=0x16c620 is
                        // wrap_sub_159090, the frame-fetch we identified
                        // independently runs ago -- which means the slot
                        // geometry (dec + 7984 + 68*i, +12 vtable) is correct.
                        // Two hypotheses dead, one of them cheaply.
                        //
                        // Where it goes next is the awkward part. Both
                        // 0x167B68 and 0x168320 have zero call sites in the
                        // decompile, and so does 0x1679F8, which walks a pool at
                        // stride 3544 -- exactly the size of the object
                        // dec+14000 points to, given its fields run to +3540.
                        // Three functions of one cluster, none of them called by
                        // anything. That is not three separate accidents; that
                        // is an entry-point family reached some other way. The
                        // slot 6 vtable holds 0x16Cxxx thunks, not these, so
                        // it is some other table.
                        //
                        // Which raises the question that has to be answered
                        // before another theory gets built: does 0x168320 even
                        // HAVE a body? Every hasFunction check so far has gone
                        // through the slot vtables. This cluster has never been
                        // asked. If the answer is no, then a JALR through
                        // whatever table holds the pointer returns a stale v0
                        // and writes nothing -- which is precisely the shape
                        // that cost this project thirty-eight run cycles at
                        // Stage 5.11, and it would explain the virgin fields
                        // without needing a caller at all
                        // ([[project_stage511_gsdump_replay]]).
                        //
                        // The scan is the other half. If a body exists, the
                        // pointer has to live somewhere, and the address holding
                        // it names the table -- and the table names the caller.
                        // One pass over RDRAM, once per process, is cheap
                        // compared to another run spent guessing. Both target
                        // words are scanned rather than one, because finding
                        // 0x167B68 in a table 0x168320 is absent from (or the
                        // reverse) is itself a strong signal about which entry
                        // point the game is wired to reach.
                        {
                            static const uint32_t kApi[16] = {
                                0x1679D8u, 0x1679F8u, 0x167B68u, 0x167BC0u,
                                0x167D38u, 0x168288u, 0x168320u, 0x168938u,
                                0x16B280u, 0x16B318u, 0x165300u, 0x165458u,
                                0x165488u, 0x165548u, 0x16C620u, 0x16C690u};
                            static bool apiDone = false;
                            if (!apiDone)
                            {
                                apiDone = true;
                                uint32_t hb = 0;
                                for (uint32_t i = 0; i < 16u; ++i)
                                {
                                    if (hasFunction(kApi[i]))
                                    {
                                        hb |= (1u << i);
                                    }
                                }
                                std::cerr << "[sfdapi] t=" << std::dec << (t + 1)
                                          << "s hb=0x" << std::hex << hb
                                          << " (bit6=0x168320 bit2=0x167B68)"
                                          << std::dec << std::endl;

                                // One-shot pointer hunt. A word equal to an
                                // entry point is either a dispatch table slot or
                                // a jal-encoded immediate; either way the
                                // address is a lead, and zero hits is itself the
                                // answer that nothing in RAM refers to it.
                                uint32_t n320 = 0, nB68 = 0;
                                uint32_t a320[8] = {0}, aB68[8] = {0};
                                try
                                {
                                    for (uint32_t a = 0x00080000u;
                                         a < 0x02000000u; a += 4u)
                                    {
                                        uint32_t w = m_memory.read32(a);
                                        if (w == 0x00168320u)
                                        {
                                            if (n320 < 8u)
                                            {
                                                a320[n320] = a;
                                            }
                                            ++n320;
                                        }
                                        else if (w == 0x00167B68u)
                                        {
                                            if (nB68 < 8u)
                                            {
                                                aB68[nB68] = a;
                                            }
                                            ++nB68;
                                        }
                                    }
                                }
                                catch (const std::exception &)
                                {
                                }
                                std::cerr << "[sfdptr] n320=" << std::dec << n320
                                          << " nB68=" << nB68 << std::hex;
                                std::cerr << " a320=";
                                for (uint32_t i = 0; i < 8u; ++i)
                                {
                                    std::cerr << (i != 0u ? "," : "")
                                              << "0x" << a320[i];
                                }
                                std::cerr << " aB68=";
                                for (uint32_t i = 0; i < 8u; ++i)
                                {
                                    std::cerr << (i != 0u ? "," : "")
                                              << "0x" << aB68[i];
                                }
                                std::cerr << std::dec << std::endl;
                            }
                        }
                    }

                    // ---- [mvgate] which gate stops the movie tick ------------
                    //
                    // Run 84 moved this from a value question to a structural
                    // one, and the numbers are not close. The class-5/6 callback
                    // registry is alive and hammering: sub_155320 -- the movie
                    // per-object update -- was dispatched 909,186 times in 300 s
                    // (8 slots x 113,719 ticks). It reached the state machine
                    // sub_165300 exactly THREE times. Once per movie.
                    //
                    // That kills the run-83 reading outright, and it should be
                    // said plainly rather than quietly dropped: we were never
                    // "stuck in state 1 with dec+76 < 2". [sfdst] reads dec+72=2
                    // from t=83 s onward, which is exactly what PCSX2 shows on
                    // real hardware. The single tick we get DOES advance the
                    // ladder 1 -> 2, correctly. State 2's handler 0x165488 then
                    // never runs for one reason only: dispatching it requires a
                    // SECOND tick, and no second tick ever happens.
                    //
                    // Five gates stand between sub_155320 and the tick:
                    //
                    //   1. [0x45F674] == 1        global "movie system enabled"
                    //   2. obj != 0
                    //   3. obj->+0  == 1          this slot is playing
                    //   4. obj->+96 != 1          per-object re-entrancy latch
                    //   5. [0x45F69C] != 1        global pause  (0x14E4D0()+36)
                    //
                    // Coverage places the failure precisely. 0x155520 -- the
                    // one-instruction getter for gate 4 -- ran 113,711 times, so
                    // gates 1..3 pass on essentially every tick. And 0x155518,
                    // the ONLY writer of +96, ran 9 times: three from 0x14C598
                    // (which passes a1=0, always a clear) and 2N from the tail
                    // block, one set and one clear per entry. 2N + 3 = 9 gives
                    // N = 3, matching the three dispatches of 0x165250 exactly.
                    // So the tail was entered 3 times out of 113,711 chances,
                    // and gate 4 or gate 5 ate the other 113,708.
                    //
                    // Static cannot finish this one. Both gates read balanced:
                    // every setPause site (0x14E92C, 0x154A1C, 0x155648) is a
                    // matched set(1) ... work ... set(0) pair, 0x1555A0 ran an
                    // even 18 times, and 0x14C598 only ever clears. On paper
                    // both words end at 0. One of them does not, at runtime.
                    //
                    // Counts cannot say which -- 0x155520 and 0x1555E8 each have
                    // two call sites, and BOTH attributions fit the totals to the
                    // digit (113,711 + 9 = 113,720). That is the same wall the
                    // order trace was built for, except order cannot separate two
                    // callers of one address either. So read the words.
                    //
                    // The geometry is unusually safe here, which is the point.
                    // sub_155210 computes the slot array as s1+108 where s1 is
                    // the return of 0x14E4D0 -- and 0x14E4D0 is three
                    // instructions, "lui v0,0x46; jr ra; addiu v0,v0,-2440", a
                    // constant. So the eight objects sit at 0x45F6E4 + 772*i,
                    // fixed, with no pointer chase and nothing inferred from a
                    // live value ([[feedback_probe_gate_on_shape_not_address]]).
                    //
                    // f0 is the shape gate and it referees the whole probe: gates
                    // 1..3 passing 113,711 times per run means EXACTLY ONE slot
                    // must read f0=1. If none do, or several do, the stride or
                    // the base is wrong and every other column here is fiction --
                    // so print all eight rows and let them convict the probe if
                    // they disagree ([[feedback_degenerate_result_convicts_the_probe]]).
                    //
                    // Printed every sample rather than once, because the failure
                    // mode we are chasing IS a value that changes: a latch that
                    // is set and never cleared looks identical to one that was
                    // never set if you only ever see the end state.
                    //
                    //   g36 == 1 persistently  -> gate 5. The global pause is
                    //                             stuck, and the culprit is
                    //                             whichever setPause pair had its
                    //                             middle call fail to return
                    //   +96 == 1 persistently  -> gate 4. The re-entrancy latch
                    //                             never got cleared; a tick that
                    //                             unwound early would do it
                    //   both 0, f0 == 1        -> gates 1..5 all pass and the
                    //                             tick SHOULD be happening; the
                    //                             bug is then in our dispatch of
                    //                             0x165250, not in the guest
                    //   no slot with f0 == 1   -> probe geometry is wrong, stop
                    //
                    // Added after the 08-21 PCSX2 session, which turned the
                    // gate-4-or-gate-5 question into a sharper one. Counting
                    // settled it on paper: 0x1551E0 and 0x155178 each have
                    // exactly ONE caller, so their counts are clean, and every
                    // consistent split of 0x1555E8's 113,720 reads ends the
                    // same way -- [0x45F69C] returns 1 for essentially every
                    // read of the run. Gate 5 is shut.
                    //
                    // But the WRITE side flatly contradicts that, and the
                    // contradiction is the actual finding. 0x1555A0 is the only
                    // writer of that word in the whole image (eeref finds no
                    // direct-addressed store either), it ran 18 times, and 18
                    // is exactly what its three call sites account for:
                    // 0x155630 ran 7 (x2 = 14) and sub_14E8B0 ran 2 (x2 = 4),
                    // with sub_1549C0 never running at all. Every set(1) has
                    // its matching set(0). The game cannot be leaving its own
                    // pause flag at 1.
                    //
                    // So something that is not the game is writing 0x45F69C,
                    // and rate/g80 are here to say so without costing a run.
                    // PCSX2 at the main menu holds these EXACT values:
                    //
                    //   [0x45F674] = 1           [0x45F678] = 0
                    //   [0x45F67C] = 0x426FC28F  (59.94f, the frame rate)
                    //   [0x45F680] = 1           [0x45F69C] = 0
                    //
                    // 0x426FC28F is a specific float sitting six words from the
                    // flag, so it is a good canary: a stray memset, a misaimed
                    // DMA or an OOB store big enough to reach the flag will
                    // almost certainly take the frame rate with it.
                    //
                    //   rate wrong (REGION-CLOBBERED) -> foreign bulk write;
                    //                                   go arm a data
                    //                                   breakpoint on 0x45F69C
                    //   rate right but g36 == 1       -> a single targeted store;
                    //                                   the imbalance is real and
                    //                                   0x1555A0's own count is
                    //                                   what to distrust next
                    //   g36 == 0 throughout          -> the read-side arithmetic
                    //                                   above is wrong; gate 4 is
                    //                                   back on the table
                    {
                        uint32_t g36 = 0, gEn = 0, gRate = 0, g80 = 0;
                        uint32_t f0[8] = {0}, f92[8] = {0}, f96[8] = {0}, f100[8] = {0};
                        int live = -1;
                        int nLive = 0;
                        try
                        {
                            gEn = m_memory.read32(0x0045F674u);
                            g36 = m_memory.read32(0x0045F69Cu);
                            // Neighbours of the pause word, used as a
                            // clobber canary -- see the note above.
                            gRate = m_memory.read32(0x0045F67Cu);
                            g80 = m_memory.read32(0x0045F680u);
                            for (uint32_t i = 0; i < 8u; ++i)
                            {
                                const uint32_t o = 0x0045F6E4u + 772u * i;
                                f0[i] = m_memory.read32(o + 0u);
                                f92[i] = m_memory.read32(o + 92u);
                                f96[i] = m_memory.read32(o + 96u);
                                f100[i] = m_memory.read32(o + 100u);
                                if (f0[i] == 1u)
                                {
                                    ++nLive;
                                    if (live < 0)
                                    {
                                        live = static_cast<int>(i);
                                    }
                                }
                            }
                        }
                        catch (const std::exception &)
                        {
                        }
                        std::cerr << "[mvgate] t=" << std::dec << (t + 1) << "s"
                                  << " en=" << gEn
                                  << " g36=" << g36
                                  << " rate=0x" << std::hex << gRate << std::dec
                                  << " g80=" << g80
                                  << (gRate != 0x426FC28Fu ? " REGION-CLOBBERED" : "")
                                  << " nLive=" << nLive
                                  << " live=" << live;
                        if (nLive != 1)
                        {
                            std::cerr << " GEOMETRY-SUSPECT(expected exactly one"
                                         " f0==1; treat every field below as"
                                         " unverified)";
                        }
                        if (live >= 0)
                        {
                            std::cerr << " L.f92=" << f92[live]
                                      << " L.f96=" << f96[live]
                                      << " L.f100=" << f100[live];
                        }
                        std::cerr << " f0=";
                        for (uint32_t i = 0; i < 8u; ++i)
                        {
                            std::cerr << (i != 0u ? "," : "") << f0[i];
                        }
                        std::cerr << " f96=";
                        for (uint32_t i = 0; i < 8u; ++i)
                        {
                            std::cerr << (i != 0u ? "," : "") << f96[i];
                        }
                        std::cerr << std::endl;
                    }

                    // ---- [mvslot] MovieCreate's own failure ladder, not the
                    // MovieUpdate gates above --
                    //
                    // 0x54BD90 (our [movie] obj field) reads 0 across every
                    // sample despite wrkAdr=0x1806c00 being populated, so
                    // MovieCreate (0x113AA0) is running and its own callee,
                    // module_obj_init_z_42 (0x14C328), is returning 0. That
                    // function has one silent failure path (complex_init_m()
                    // != 1, no Printf at all) plus several that DO Printf --
                    // but the game's own Printf() is not wired into our log
                    // (confirmed: none of its E-codes, e.g. "E4061801",
                    // appear anywhere in a run that shows the [movie] probe's
                    // OWN hardcoded cerr text), so an absent error string is
                    // not evidence either way here.
                    //
                    // What IS readable: module_obj_init_z_42 allocates its
                    // object at a FIXED pool slot, not a heap pointer. The
                    // pool base is a compiled-in constant (0x45F678, see the
                    // [mvgate] note above -- get_data_ptr() is a 3-instruction
                    // "return literal", not a load), slot stride is 772, and
                    // MovieCreate's own free-slot search enters its walk only
                    // when slot 0 already reads occupied (f0[0]==1); every
                    // [mvgate] sample this run shows f0[0]==0, so it takes the
                    // fast path and uses slot 0 (0x45F6E4) directly -- no
                    // pointer chase needed here either.
                    //
                    // Inside module_obj_init_z_42, once complex_init_m()
                    // passes, execution writes breadcrumbs into that same
                    // slot IN ORDER before any of them are checked:
                    //   +60   = mem_fill_z(v7,a1)              result
                    //           (also reused as a SIF bind handle two lines
                    //           later -- 0 here means the very first
                    //           allocation/bind call already failed)
                    //   +448  = wrap_noop_wrapper_k_0(v7)       result
                    //   +480  = noop_wrapper___315(0,0)         result
                    //   +64   = noop_wrapper___480(v7+448)      result (silent
                    //           fail if 0 -- no error string exists for this
                    //           one in the ELF)
                    //   +168  = wrap_noop_wrapper_unk_z_z_152(...) result
                    // and on full success the function finally does
                    // `*(u32*)v7 = 1`, i.e. f0[slot] flips to 1 -- which we
                    // already know never happens this run.
                    //
                    // These are unconditional stores made before each gate
                    // reads them back, so whichever of the five reads 0 while
                    // everything before it is nonzero is the exact failing
                    // call -- no build-time branch tracing needed, just RAM.
                    {
                        uint32_t mc60 = 0, mc448 = 0, mc480 = 0, mc64 = 0, mc168 = 0;
                        try
                        {
                            const uint32_t slot = 0x0045F6E4u;
                            mc60 = m_memory.read32(slot + 60u);
                            mc448 = m_memory.read32(slot + 448u);
                            mc480 = m_memory.read32(slot + 480u);
                            mc64 = m_memory.read32(slot + 64u);
                            mc168 = m_memory.read32(slot + 168u);
                        }
                        catch (const std::exception &)
                        {
                        }
                        std::cerr << "[mvslot] t=" << std::dec << (t + 1) << "s"
                                  << " slot0+60(allocBind)=" << mc60
                                  << " +448(k0)=" << mc448
                                  << " +480(315)=" << mc480
                                  << " +64(480,silent)=" << mc64
                                  << " +168(152)=" << mc168
                                  << std::endl;
                    }

                    // ---- [thsync] the handshake sub_11E690 actually spins on --
                    //
                    // Stage 5.17, run 86. This replaces the run-85 reading, which
                    // said the game was "alternately resuming and waking a worker
                    // thread indefinitely" and sent the next lane at Thread.cpp.
                    // The log it was drawn from says otherwise: across t=132..137
                    // the trace contains 0x174ba0 (ReferThreadStatus) and NEITHER
                    // 0x174bd0 (WakeupThread) NOR 0x174c30 (ResumeThread). No kick
                    // was ever issued, so "the kick never lands" was never tested.
                    //
                    // Decoded from the ELF, the two helpers are conditional:
                    //   0x11ed28  if (status == 4 || status == 0xc) WakeupThread
                    //   0x11ed90  if (status == 8 || status == 0xc) ResumeThread
                    // Neither firing means the worker's status is not WAIT and not
                    // SUSPEND -- i.e. the game believes it is already runnable.
                    //
                    // sub_11E690 is a synchronous cross-thread handshake:
                    //   [0x441924] = 1                 raise request
                    //   ChangeThreadPriority(tid, [0x4418F0])   boost worker
                    //   loop: kick; if ([0x441924] == 0) break;
                    //         if (++n > 199,999,999) -> error 0x4B8720
                    // and sub_11EAC8 is the other half -- the acknowledger:
                    //   if ([0x4419D8] != 0) return;   <-- skips the ACK entirely
                    //   [0x441960]++; [0x441934] = 1;
                    //   jal 0x13c6e8                   <-- the work (calls 0x155320)
                    //   [0x441934] = 0;
                    //   if ([0x441924] == 1) [0x441924] = 0;   <-- the ACK
                    //
                    // So the movie gate and this spin are ONE mechanism: 0x13c6e8
                    // is what drives the movie pump. The spin's only syscall is
                    // ReferThreadStatus, which does not yield, and the one call
                    // that does (ChangeThreadPriority) sits OUTSIDE the loop. On
                    // real EE the boosted worker preempts the spinner; we have no
                    // preemption, and yield_point's maybe_yield() only switches to
                    // a higher-priority READY fiber -- a Blocked worker is
                    // invisible to it. Corroborated in the same log: res/s falls to
                    // 1-3 while progress climbs ~40x, i.e. yield_point is sampled
                    // ~21,000x/s and yields ~1-3x/s.
                    //
                    // Every address here is a fixed absolute materialized by a
                    // lui+addiu pair in the image (eeref: 3 sites, all lui+lo), so
                    // there is no pointer chase and nothing inferred from a live
                    // value ([[feedback_probe_gate_on_shape_not_address]]).
                    //
                    // Printed unconditionally every sample, with no gate: a probe
                    // that only speaks when it thinks something is wrong cannot be
                    // told from a probe that did not compile in
                    // ([[feedback_capped_probes_false_negatives]]). inWork is the
                    // rival-reading field -- it is set only for the duration of the
                    // work call, so it separates "worker never ran" from "worker
                    // ran and is stuck inside" without a second run
                    // ([[feedback_degenerate_result_convicts_the_probe]]).
                    {
                        uint32_t req = 0u, inWork = 0u, gateLo = 0u, gateHi = 0u;
                        uint32_t tickLo = 0u, tickHi = 0u, boost = 0u;
                        bool memOk = true;
                        try
                        {
                            req    = m_memory.read32(0x00441924u); // request flag
                            inWork = m_memory.read32(0x00441934u); // set around the work call
                            gateLo = m_memory.read32(0x004419D8u); // 64-bit early-out gate
                            gateHi = m_memory.read32(0x004419DCu);
                            tickLo = m_memory.read32(0x00441960u); // 64-bit worker tick counter
                            tickHi = m_memory.read32(0x00441964u);
                            boost  = m_memory.read32(0x004418F0u); // priority handed to the boost
                        }
                        catch (const std::exception &)
                        {
                            memOk = false;
                        }

                        const uint64_t tick =
                            (static_cast<uint64_t>(tickHi) << 32) | tickLo;
                        const bool gated = (gateLo != 0u) || (gateHi != 0u);

                        static bool s_thsyncArmed = false;
                        static uint32_t s_prevReq = 0xFFFFFFFFu;
                        static uint64_t s_prevTick = 0ull;
                        static bool s_havePrev = false;

                        if (!s_thsyncArmed)
                        {
                            s_thsyncArmed = true;
                            std::cerr << "[thsync] armed req@0x441924 inWork@0x441934"
                                         " gate@0x4419D8 tick@0x441960 boost@0x4418F0"
                                         " spinner=sub_11E690 acker=sub_11EAC8"
                                      << std::endl;
                        }

                        const uint64_t dTick = s_havePrev ? (tick - s_prevTick) : 0ull;

                        std::cerr << "[thsync] t=" << std::dec << (t + 1) << "s"
                                  << (memOk ? "" : " MEM-UNREADABLE")
                                  << " req=" << req
                                  << " inWork=" << inWork
                                  << " gate=0x" << std::hex << gateHi << "_"
                                  << gateLo << std::dec
                                  << " tick=" << tick
                                  << " dTick=" << dTick
                                  << " boost=" << boost
                                  << " idle=" << ps2x_guest_idle()
                                  << " tokW=" << ps2sched::host_token_waiters();

                        if (s_havePrev && req != s_prevReq)
                        {
                            std::cerr << " REQ-CHANGED(" << s_prevReq << "->" << req << ")";
                        }

                        // The pre-committed decision table, evaluated inline so the
                        // log states its own conclusion. Written before the run, so
                        // a later reading cannot be fitted to whatever came back.
                        if (memOk && req == 1u)
                        {
                            if (gated)
                            {
                                std::cerr << " VERDICT=WORKER-GATED"
                                             "(acker early-outs before the ACK;"
                                             " fix the gate's producer, not the scheduler)";
                            }
                            else if (inWork == 1u)
                            {
                                std::cerr << " VERDICT=WORKER-INSIDE-WORK"
                                             "(stuck in 0x13c6e8; blocker moved into"
                                             " the movie pump)";
                            }
                            else if (dTick == 0ull && s_havePrev)
                            {
                                std::cerr << " VERDICT=WORKER-NOT-RUNNING"
                                             "(request up, worker never ticked;"
                                             " read the thread table below)";
                            }
                        }

                        // Thread table. getThreadDebugSnapshot() already exists for
                        // the RecompDebugger and returns exactly these fields, but
                        // its only caller sits inside the RecompDbg block, which is
                        // dead under -NoDebugger -- so the data source was already
                        // here and only the emit was missing. status/waitType are
                        // what separate "Blocked on something" from "Ready but never
                        // scheduled"; currentPriority checks whether the guest's
                        // boost actually outranks the spinner.
                        const std::vector<ps2_syscalls::ThreadDebugSnapshot> th =
                            ps2_syscalls::getThreadDebugSnapshot();
                        std::cerr << " nTh=" << th.size();
                        for (const ps2_syscalls::ThreadDebugSnapshot &s : th)
                        {
                            std::cerr << " [" << std::dec << s.tid
                                      << ":st=" << s.status
                                      << ",wt=" << s.waitType
                                      << ",wid=" << s.waitId
                                      << ",pri=" << s.currentPriority
                                      << ",pc=0x" << std::hex << s.currentPc
                                      << std::dec << "]";
                        }
                        std::cerr << std::endl;

                        s_prevReq = req;
                        s_prevTick = tick;
                        s_havePrev = true;
                    }

                    // ---- [cblist] what 0x13c6e8 actually runs -----------------
                    //
                    // Stage 5.17, runs A-D (2026-08-24). [thsync] reports
                    // inWork=1 for seconds at a time and its verdict string calls
                    // 0x13c6e8 "the movie pump". Decoding it says otherwise:
                    //
                    //   0x13c6d0  addiu $a0,$zero,5 ; j 0x13c4f8   (24 bytes)
                    //   0x13c6e8  addiu $a0,$zero,6 ; j 0x13c4f8   (24 bytes)
                    //   0x13c700  addiu $a0,$zero,7 ; j 0x13c4f8   (24 bytes)
                    //
                    // 0x13c6e8 is a thunk. The real body is 0x13c4f8, a generic
                    // "run callback list N" dispatcher, decoded field by field
                    // ([[feedback_verify_translations_by_decoding]]):
                    //
                    //   v0 = ((a0<<3)+a0)<<3        = a0*72   list stride
                    //   s0 = 0x54E960 + a0*72                 list base
                    //   s1 = 0x45EFE8 + a0*4                  per-list busy flag
                    //   s2 = 5 ; loop while --s2 >= 0         => SIX entries
                    //   each entry is 12 bytes: {fn, arg, ?}
                    //   if (fn) { [s1]=1; v0 = fn(arg); [s1]=0; s3 |= v0; }
                    //   [0x45EFC8 + a0*4]++                   per-list tick
                    //   return s3
                    //
                    // So the six function pointers in list 6 ARE the work, they are
                    // registered at runtime, and no run has ever read them. That is
                    // the difference between "the pump is slow" and "the pump has a
                    // null slot where a callback should be" -- and [ipu:cmd] says
                    // the IPU gets SETIQ/SETVQ/SETTH and then never a decode, which
                    // is exactly what a missing pump callback would look like.
                    //
                    // Every address here is a fixed absolute materialized by lui+addiu
                    // in the image, not a live pointer chase
                    // ([[feedback_probe_gate_on_shape_not_address]]). Printed
                    // unconditionally so a silent probe cannot pass for a null result
                    // ([[feedback_capped_probes_false_negatives]]).
                    {
                        constexpr uint32_t kListBase = 0x0054E960u; // + N*72
                        constexpr uint32_t kTickBase = 0x0045EFC8u; // + N*4
                        constexpr uint32_t kBusyBase = 0x0045EFE8u; // + N*4

                        static bool s_cbArmed = false;
                        if (!s_cbArmed)
                        {
                            s_cbArmed = true;
                            std::cerr << "[cblist] armed dispatcher=0x13c4f8"
                                         " lists@0x54E960 stride=72 entries=6 esz=12"
                                         " tick@0x45EFC8 busy@0x45EFE8"
                                         " thunks 0x13c6d0/0x13c6e8/0x13c700 = list 5/6/7"
                                      << std::endl;
                        }

                        bool cbOk = true;
                        auto rd = [&](uint32_t a) -> uint32_t {
                            try { return m_memory.read32(a); }
                            catch (const std::exception &) { cbOk = false; return 0u; }
                        };

                        std::cerr << "[cblist] t=" << std::dec << (t + 1) << "s"
                                  << (cbOk ? "" : " MEM-UNREADABLE");

                        // All eight lists, compactly: tick + how many of the six
                        // slots hold a non-null fn. A list whose tick climbs but
                        // whose fill is 0 is being dispatched into nothing.
                        for (uint32_t n = 0; n < 8u; ++n)
                        {
                            uint32_t fill = 0u;
                            for (uint32_t e = 0; e < 6u; ++e)
                            {
                                if (rd(kListBase + n * 72u + e * 12u) != 0u) ++fill;
                            }
                            std::cerr << " L" << n << "=" << std::dec
                                      << rd(kTickBase + n * 4u) << "/" << fill
                                      << (rd(kBusyBase + n * 4u) ? "*" : "");
                        }

                        // List 6 in full -- the one [thsync] catches the worker in.
                        std::cerr << " L6:";
                        for (uint32_t e = 0; e < 6u; ++e)
                        {
                            const uint32_t base = kListBase + 6u * 72u + e * 12u;
                            std::cerr << " [" << std::dec << e << "]fn=0x" << std::hex
                                      << rd(base) << ",a=0x" << rd(base + 4u)
                                      << std::dec;
                        }
                        std::cerr << std::endl;

                        // ---- [pump] which gate the movie pump bails at ----------
                        //
                        // Run 2026-08-24 15:57 proved [cblist] L6 tick IS [thsync]
                        // tick (t=124->1/1, 127->3/3, 142->5/5, 147->6/6), and that
                        // list 6 holds exactly ONE callback: 0x154fa8. So the entire
                        // "movie pump" is that one function. Decoded field by field
                        // ([[feedback_verify_translations_by_decoding]]):
                        //
                        //   0x14e4d0: return 0x45F678                  <- movie base
                        //   0x154ff0: return [base+0x10]               = [0x45F688]
                        //   0x1556f8: return [base+0x188C]             = [0x460F04]
                        //
                        //   0x154fa8 (the registered callback):
                        //     if ([0x45F688] == 1) return 0;           // SKIP flag
                        //     return f_155210();
                        //
                        //   0x155210 (the body):
                        //     if ([0x45F674] != 1) return 0;           // master enable
                        //     if (f_1548a0(base+0x58) != 1) return 0;  // object state
                        //     f_155148();
                        //     if (f_1556f8() == 1) skip stream loop;   // [0x460F04]
                        //     for (i=7;i>=0;--i) f_155320(0x45F6E4 + i*0x304);
                        //
                        //   0x1548a0 tail-jumps 0x13c880, which is:
                        //     v0 = [0x54EBF8];                         // runtime fn ptr
                        //     if (v0) return v0(a0); else return f_13bc10(a0);
                        //
                        // The t=190 watchdog trace reads "0x13c880 -> 0x13bc10",
                        // i.e. the NULL-pointer fallback -- hypothesis, and hook=0x0
                        // below is what confirms or kills it.
                        //
                        // Every address is a fixed absolute materialized by lui+addiu
                        // in the image, not a live pointer chase
                        // ([[feedback_probe_gate_on_shape_not_address]]). Printed
                        // unconditionally so silence cannot pass for a null result
                        // ([[feedback_capped_probes_false_negatives]]).
                        {
                            constexpr uint32_t kMovieBase = 0x0045F678u;
                            constexpr uint32_t kEnable    = 0x0045F674u; // base-4
                            constexpr uint32_t kSkip      = kMovieBase + 0x10u;
                            constexpr uint32_t kNoStream  = kMovieBase + 0x188Cu;
                            constexpr uint32_t kObj       = kMovieBase + 0x58u;
                            constexpr uint32_t kStream0   = kMovieBase + 0x6Cu;
                            constexpr uint32_t kHook13c880 = 0x0054EBF8u;

                            // ---- second hook, added after the 16:13 run --------
                            //
                            // hook(0x54EBF8)=0x0 held for all 202 samples, but that
                            // is the DESIGNED state, not a missing registration:
                            // its only writer 0x13c8fc sits in 0x13c8e8, whose only
                            // caller 0x13c920 passes a0=0, and eeref reports 0x13c910
                            // UNREACHABLE. So 0x13c880 always takes its fallback
                            // 0x13bc10 -> 0x13bb20, and THAT is the real gate:
                            //
                            //   0x13bb20:  v0 = 0x54EBE0
                            //              if ([v0] == 0) goto 0x13bb70;  // v0 kept
                            //              jalr [v0] (a0 = [v0+4])
                            //              if ([0x45EFC0] == 0) [0x45EFC4] = arg;
                            //              v0 = [0x45EFC0] + 1; [0x45EFC0] = v0;
                            //   0x13bb70:  jr $ra          // $v0 NEVER set here
                            //
                            // Null  -> returns 0x54EBE0 (the address itself).
                            // Bound -> first call returns exactly 1.
                            // 0x155264 daddu $s0,$v0 then bne $s0,$s2 (s2=1), so the
                            // movie body bails to 0x155308 on anything but 1.
                            //
                            // Registrar is 0x13c4c8 (sw a0,0(v0); sw a1,4(v0)),
                            // called from ADX_Init+0x50, sub_11F448+0x64,
                            // sub_11FE90+0x44, sub_120080+0x74. 0x13c750 memsets
                            // 0x54EBE0 for 8 bytes -- if that init runs AFTER the
                            // ADX registration it wipes it, which h2/h2a distinguish
                            // from "never registered" by whether they are ever
                            // non-zero at any sample.
                            constexpr uint32_t kHook13bb20 = 0x0054EBE0u;
                            constexpr uint32_t kHook13bb20Arg = 0x0054EBE4u;
                            constexpr uint32_t kCallCount  = 0x0045EFC0u;

                            std::cerr << "[pump] t=" << std::dec << (t + 1) << "s"
                                      << " en=" << rd(kEnable)
                                      << " skip=" << rd(kSkip)
                                      << " nostream=" << rd(kNoStream)
                                      << " hook=0x" << std::hex << rd(kHook13c880)
                                      << " h2=0x" << rd(kHook13bb20)
                                      << " h2a=0x" << rd(kHook13bb20Arg)
                                      << " n=0x" << rd(kCallCount)
                                      << " obj=0x" << rd(kObj) << std::dec;
                            std::cerr << " s:";
                            for (uint32_t i = 0; i < 8u; ++i)
                            {
                                std::cerr << " " << std::hex
                                          << rd(kStream0 + i * 0x304u) << std::dec;
                            }
                            std::cerr << std::endl;
                        }
                    }

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
                              << " vblSrc=" << dVblQ << "/" << dVblI << "/" << dVblS
                              << " progress=" << ps2x_guest_progress()
                              << " gif/s=" << dGif
                              << " dma/s=" << dDma
                              << " stuckSecs=" << stuck
                              // Cumulative, not per-second: intrEsc is expected
                              // to be 0 for a whole run, so a running total is
                              // the readable form. Placed before trace= so
                              // console truncation cannot clip it.
                              << " intrEsc=" << ps2x_guest_intr_disable_escapes()
                              << " intrSec=" << ps2x_guest_intr_disable_sections()
                              << " intrStray=" << ps2x_guest_intr_disable_stray()
                              << std::hex << " pc=0x" << pc
                              << " ra=0x" << ra << " lastCall=0x" << lastCall
                              // 0 unless the 0x13c4f8 callback list is mid-callback
                              // right now; nonzero on a parked watchdog names the
                              // callback that entered and never returned.
                              << " cb=0x"
                              << g_sdbzCb13C4F8InFlight.load(std::memory_order_relaxed)
                              << std::dec
                              << " trace=" << formatGlobalDispatchHistory() << std::endl;
                }

                // Final dump, with fullGameBand=true. This is the Stage 5.7
                // payload: every distinct game-band address the recomp actually
                // dispatched. Written here, on the way out, so a run stopped by
                // -RunSeconds still leaves the set on disk instead of only in a
                // console buffer that gets clipped.
                // Ordered dispatch trace, emitted before the coverage
                // summary so the two land together in the same tail. Printed
                // as a run-length-compressed sequence: a movie tick that hits
                // the same node 400 times in a row is one line, and the thing
                // being looked for -- a producer landing between two ticks of
                // the state machine -- stays visible instead of being buried.
                //
                // The saturation line is not optional. A trace that filled its
                // log is a trace whose TAIL is missing, and absence in a
                // truncated trace is not evidence of anything.
                if (g_orderArmed.load(std::memory_order_relaxed))
                {
                    const uint32_t raw = g_orderNext.load(std::memory_order_relaxed);
                    const uint32_t have = raw < kOrderLogSize ? raw : kOrderLogSize;
                    std::cerr << "[order] total=" << std::dec << raw
                              << " logged=" << have;
                    if (raw > kOrderLogSize)
                    {
                        std::cerr << " [cap] TRUNCATED -- the tail of this"
                                     " sequence is missing; do not read"
                                     " absence from it";
                    }
                    std::cerr << std::endl;

                    uint32_t i = 0;
                    while (i < have)
                    {
                        const uint32_t pc = g_orderLog[i].load(std::memory_order_relaxed);
                        uint32_t run = 1;
                        while (i + run < have &&
                               g_orderLog[i + run].load(std::memory_order_relaxed) == pc)
                        {
                            ++run;
                        }
                        std::cerr << "[order] #" << std::dec << i
                                  << " 0x" << std::hex << pc << std::dec;
                        if (run > 1u)
                        {
                            std::cerr << " x" << run;
                        }
                        std::cerr << std::endl;
                        i += run;
                    }
                }

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

        // EndDrawing() has just run raylib's PollInputEvents(), so host key /
        // gamepad state is fresh on this thread. Push it to the guest now.
        ps2x_pad_push_frame(m_memory.getRDRAM());

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
