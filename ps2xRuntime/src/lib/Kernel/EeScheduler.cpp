#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "recomp_debug_writer.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <cstdint>

// 2026-09-03 part 55 -- DISPATCH: which threads actually get the CPU.
//
// The 400s run leaves thread 6 RUNNING at priority 1 while threads 1, 4 and 5
// sit READY for 250+ seconds with gif/s=0. Two readings fit that and [thsync]
// cannot separate them, because it samples state at 1 Hz: a thread that is
// briefly Ready between two dispatches looks exactly like a starved one.
//
//   (a) thread 6 never yields at all -> zero context switches
//   (b) thread 6 yields, but is re-dispatched before anyone else runs
//
// makeRunning is the single point where a thread becomes Running, so counting
// per id there is exact. sleepCurrent is counted too: "blocked" is the path
// that actually parks the thread, "fast" is the wakeupCount>0 path that
// returns immediately and parks nothing -- a SofDec idle loop whose sleeps all
// take the fast path would spin forever while still looking like it sleeps.
//
// Emitted on a wall clock so the records line up with [watchdog]/[thsync]
// seconds; the clock is read only every 4096 events to keep the hot path cheap,
// and the emit is also driven from sleepCurrent so a run with no switches at
// all still produces records.
//
// Read it as: d1 (main) staying 0 across the stall while d6 climbs proves
// genuine starvation and makes this a scheduler-fairness bug. d1 non-zero kills
// that and moves the question to why main makes no progress when it does run.
// blocked==0 with fast climbing identifies the wakeupCount fast path as the
// reason thread 6 never parks.
extern "C" void ps2x_probe_kv(const char *name, int n,
                              const char *const *keys, const uint64_t *vals);

namespace
{
    constexpr int kDispatchMaxId = 10;
    uint64_t g_dispatchTotal[kDispatchMaxId] = {};
    uint64_t g_dispatchPrev[kDispatchMaxId] = {};
    uint64_t g_dispatchOther = 0;
    uint64_t g_sleepBlocked = 0;
    uint64_t g_sleepFast = 0;
    // 2026-09-06 part 87 -- the wakeupCount fast path is the last unmeasured
    // link in the SofDec stall. main's 0x11E690 spin calls WakeupThread once
    // per iteration (0x11E6F0 -> 0x11ED28); every call that lands on an
    // already-Ready thread takes wakeupThread's else branch below and bumps
    // wakeupCount with no ceiling. If th6 then reaches SleepThread it consumes
    // exactly ONE and returns immediately, so it never parks and main (pri 24)
    // never runs again. These ride the 1 Hz WATCH sampler, which is the only
    // probe that provably keeps emitting through the stall -- DISPATCH is
    // driven from sleepCurrent and goes silent exactly when we need it.
    uint64_t g_wakeAcc[kDispatchMaxId] = {};    // ++wakeupCount events per tid
    uint64_t g_wakeReady[kDispatchMaxId] = {};  // makeReady events per tid
    uint32_t g_wakeCountLast[kDispatchMaxId] = {}; // last seen wakeupCount
    uint32_t g_sleepCountLast[kDispatchMaxId] = {}; // wakeupCount at SleepThread
    unsigned g_dispatchSinceClock = 0;
    bool g_dispatchClockSet = false;
    std::chrono::steady_clock::time_point g_dispatchLastEmit{};

    void maybeEmitDispatch()
    {
        // 2026-09-03 part 56 -- the 4096 batch was the reason the 09-03 00:44
        // run produced ZERO DISPATCH records over 394s even though the probe
        // strings are verifiably in the exe. It needed 4096 events just to set
        // the clock and another 4096 before the first emit, so anything under
        // ~8192 total context switches for the whole run is invisible -- and
        // "no records" then reads as "the probe is broken", not as data. 64 is
        // still cheap (one steady_clock read per 64 events) and the first batch
        // now seeds the clock WITHOUT being thrown away.
        if (++g_dispatchSinceClock < 64u)
        {
            return;
        }
        g_dispatchSinceClock = 0;
        const auto now = std::chrono::steady_clock::now();
        if (!g_dispatchClockSet)
        {
            g_dispatchClockSet = true;
            g_dispatchLastEmit = now;
        }
        if (now - g_dispatchLastEmit < std::chrono::seconds(1))
        {
            return;
        }
        g_dispatchLastEmit = now;
        static const char *const k[] = {"d1", "d2", "d3", "d4",     "d5",
                                        "d6", "dOther", "blocked", "fast", "t6"};
        const uint64_t v[] = {g_dispatchTotal[1] - g_dispatchPrev[1],
                              g_dispatchTotal[2] - g_dispatchPrev[2],
                              g_dispatchTotal[3] - g_dispatchPrev[3],
                              g_dispatchTotal[4] - g_dispatchPrev[4],
                              g_dispatchTotal[5] - g_dispatchPrev[5],
                              g_dispatchTotal[6] - g_dispatchPrev[6],
                              g_dispatchOther,
                              g_sleepBlocked,
                              g_sleepFast,
                              g_dispatchTotal[6]};
        for (int i = 0; i < kDispatchMaxId; ++i)
        {
            g_dispatchPrev[i] = g_dispatchTotal[i];
        }
        ps2x_probe_kv("DISPATCH", 10, k, v);
    }

    void probeDispatch(int id)
    {
        if (id >= 0 && id < kDispatchMaxId)
        {
            ++g_dispatchTotal[id];
        }
        else
        {
            ++g_dispatchOther;
        }
        maybeEmitDispatch();
    }
} // namespace


namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Watchdog/determinism diagnostics ported from the retired ps2_scheduler.cpp
// (Phase 3d). extern "C" rather than declared in ee_scheduler.h, for the same
// reason as before: ps2_runtime.cpp and Kernel/Syscalls/Interrupt.cpp
// re-declare these symbols locally instead of paying a header-wide rebuild.
//
// ps2x_guest_progress() MUST keep the exact "one tick per 128 guest
// back-edges" cadence: Interrupt.cpp's PS2X_DET_VBLANK_QUANTUM pacing
// (Stage 5.17, [[reference_det_vblank_quantum]]) divides real time by this
// counter's rate, and that stage's determinism validation was measured
// against that exact cadence. checkpointDue() is called from the same
// call sites yield_point() used to be (recompiler-emitted intra-function
// back-edges, now via eeCheckpointDue(); the former dispatchLoop's
// per-iteration call, now EeScheduler::run()'s own checkpointDue() call),
// so gating this counter's increment behind the identical "128 calls" fast
// path reproduces the old cadence rather than guessing a new one.
static std::atomic<uint64_t> g_guest_progress{0};
static thread_local uint32_t tls_progress_backedge_counter = 0u;

extern "C" uint64_t ps2x_guest_progress()
{
    return g_guest_progress.load(std::memory_order_relaxed);
}

// Guest execution duty cycle (ported from ps2_scheduler.cpp's stage 5.6.2
// counters). Timed around EeScheduler::run()'s function(...) dispatch
// bracket -- the direct successor of the old fiber-resume bracket.
static std::atomic<uint64_t> g_guest_busy_ns{0};
static std::atomic<uint64_t> g_guest_resumes{0};

extern "C" uint64_t ps2x_guest_busy_ns()
{
    return g_guest_busy_ns.load(std::memory_order_relaxed);
}

extern "C" uint64_t ps2x_guest_resumes()
{
    return g_guest_resumes.load(std::memory_order_relaxed);
}

// "No guest thread runnable" signal (ported from ps2sched::ps2x_guest_idle()).
// s_activeScheduler is set once by the constructor below; there is only ever
// one EeScheduler/PS2Runtime instance per process, same assumption the old
// singleton-style ps2sched code made.
static EeScheduler *s_activeScheduler = nullptr;

// Current guest thread id, or 0 if none is running (ported off ps2sched's
// thread_local g_currentThreadId, which used -1 for "none" -- 0 works
// identically here since real guest tids start at 1 and never collide with
// either sentinel). Unlike the old value this is NOT thread_local: only one
// OS thread ever runs guest code under EeScheduler, so a single instance
// member read through the scheduler is equivalent and needs no per-thread
// storage. Kept as a free function (not a header-declared method call) so
// callers with no PS2Runtime*/EeScheduler* in scope (e.g. Thread.cpp's
// ps2x_stack_check(), called from deep inside dispatch) can still read it.
extern "C" int ps2x_guest_current_thread_id()
{
    return s_activeScheduler ? s_activeScheduler->currentThreadId() : 0;
}

extern "C" int ps2x_guest_idle()
{
    return (s_activeScheduler && s_activeScheduler->isIdle()) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// EIE gate (ported verbatim from ps2_scheduler.cpp, 2026-07-26): the SDBZ pool
// allocator at 0x178428 relies on DisableIntr()/EnableIntr() (game_overrides.cpp)
// meaning what they mean on hardware -- while the guest holds interrupts
// disabled, no interrupt handler may run and no other guest thread may be
// scheduled in. Under EeScheduler, both of those only happen when
// checkpointDue() reports a checkpoint due (EeScheduler::run() then calls
// processPendingEvents() / reschedules), so the gate now lives there instead
// of in ps2sched::yield_point(). Escape hatch preserved unchanged: after
// kIntrDisableYieldEscape samples inside one critical section the gate opens
// anyway, so a runaway section surfaces as data (ps2x_guest_intr_disable_escapes()
// nonzero) rather than a hang.
static constexpr uint32_t kIntrDisableYieldEscape = 4096u;
static thread_local bool tls_intr_disabled = false;
static thread_local uint32_t tls_intr_disable_samples = 0u;
static std::atomic<uint64_t> g_intr_disable_escapes{0};
static std::atomic<uint64_t> g_intr_disable_sections{0};
static std::atomic<uint64_t> g_intr_disable_redundant{0};
static std::atomic<uint64_t> g_intr_disable_stray{0};

extern "C" void ps2x_guest_intr_disable_enter()
{
    if (tls_intr_disabled)
    {
        g_intr_disable_redundant.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    tls_intr_disabled = true;
    tls_intr_disable_samples = 0u;
    g_intr_disable_sections.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void ps2x_guest_intr_disable_leave()
{
    if (!tls_intr_disabled)
    {
        g_intr_disable_stray.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    tls_intr_disabled = false;
    tls_intr_disable_samples = 0u;
}

extern "C" uint32_t ps2x_guest_intr_disable_depth()
{
    return tls_intr_disabled ? 1u : 0u;
}

extern "C" uint64_t ps2x_guest_intr_disable_escapes()
{
    return g_intr_disable_escapes.load(std::memory_order_relaxed);
}

extern "C" uint64_t ps2x_guest_intr_disable_sections()
{
    return g_intr_disable_sections.load(std::memory_order_relaxed);
}

extern "C" uint64_t ps2x_guest_intr_disable_redundant()
{
    return g_intr_disable_redundant.load(std::memory_order_relaxed);
}

extern "C" uint64_t ps2x_guest_intr_disable_stray()
{
    return g_intr_disable_stray.load(std::memory_order_relaxed);
}

// Restored 08-27 (Phase 3 build-gate): de9288f8 ported every other
// ps2x_guest_* diagnostic above out of the retired ps2_scheduler.cpp, but
// missed this one -- Interrupt.cpp's interruptWorkerMain() still forward-
// declares and calls it (PS2X_DET_VBLANK_QUANTUM pacing gate, see
// [[reference_det_vblank_quantum]]), so it was left link-broken.
extern "C" int ps2x_determinism_enabled()
{
    // Read once; the env var is fixed for the lifetime of the process.
    static const int enabled = []() -> int
    {
        const char *e = std::getenv("PS2X_DETERMINISM");
        return (e && *e && *e != '0') ? 1 : 0;
    }();
    return enabled;
}

// ---------------------------------------------------------------------------
// 2026-08-31 -- general dispatch diagnostic, replacing the per-address guesswork.
//
// Parts 37-42 each added a hand-written `if (context.pc == <label>)` block to
// the dispatch loop below, one per freeze site, because we could not see WHICH
// dispatch had gone wrong -- only that ctx->pc ended up 0. Every one of those
// blocks was encoding the same latent fact by hand: "this pc is a mid-function
// resume label, and it was entered fresh rather than by a live nested call."
//
// That fact is derivable at runtime from the generated dense function table, so
// derive it once here instead of hardcoding another address each session. The
// table aliases every resume label to its owner's entry (see
// ps2xRecomp/src/lib/function_table_emitter.cpp:108-120), which is exactly the
// signal needed: walk back from pc while the slot still holds the same function
// pointer, and the lowest such address is the owner's entry.
// Defined at file scope in game_overrides.cpp. Maps an address that has a
// frametrace wrapper installed to the guest function it really belongs to.
// Returns false for addresses that are not wrapped. See the note in
// eeResolveOwnerEntry() for why the pointer walk alone is not sufficient.
bool sdbzLookupFrameTraceOwner(uint32_t address, uint32_t &funcStart);

namespace
{
    // Mirrors generatedFunctionTableSlot() in ps2_runtime.cpp -- that one lives
    // in an anonymous namespace there, so it cannot be shared. The externs it
    // reads are public (ps2_runtime.h:781-784).
    bool eeTableSlot(uint32_t address, uint32_t &slot)
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

    // Lowest address that still dispatches to the same function as `pc`.
    // Returns `pc` itself when that cannot be established, so a caller
    // comparing (owner != pc) never gets a false "mid-function" verdict.
    //
    // 2026-08-31: the pointer-identity walk below is DEFEATED wherever
    // game_overrides.cpp has installed a frametrace wrapper, because every slot
    // -- entry and interior resume label alike -- gets its own distinct
    // sdbzFrameTraceWrapper<I> instantiation. Two addresses in the same guest
    // function then hold different pointers, the walk sees (fn != self) at the
    // first wrapped probe, and the resume label is reported as its own entry.
    // Measured: 0x17CFA4 (a known resume label inside sub_17CF50) resolved to
    // itself, while the unwrapped 0x178AC4 resolved correctly to 0x178A08.
    // So consult that table first -- it is authoritative for those addresses.
    uint32_t eeResolveOwnerEntry(uint32_t pc)
    {
        uint32_t wrapped = 0u;
        if (sdbzLookupFrameTraceOwner(pc, wrapped))
        {
            return wrapped;
        }

        uint32_t slot = 0u;
        if (!eeTableSlot(pc, slot))
        {
            return pc;
        }
        const PS2Runtime::RecompiledFunction self = g_ps2RecompiledFunctionTable[slot];
        if (self == nullptr)
        {
            return pc;
        }

        // Bounded: the largest recompiled bodies in this title are well under
        // 16 KiB, and an unbounded walk would run to the table base on a miss.
        constexpr uint32_t kMaxScanBytes = 0x4000u;
        uint32_t owner = pc;
        for (uint32_t back = 4u; back <= kMaxScanBytes && back <= pc; back += 4u)
        {
            const uint32_t probe = pc - back;
            uint32_t probeSlot = 0u;
            if (!eeTableSlot(probe, probeSlot))
            {
                break;
            }
            const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[probeSlot];
            if (fn == nullptr)
            {
                // Interior address with no slot of its own -- normal, since only
                // resume labels get aliased. Keep walking.
                continue;
            }
            if (fn != self)
            {
                // Reached the previous function's territory.
                break;
            }
            owner = probe;
        }
        return owner;
    }

    struct EeDispatchRecord
    {
        uint64_t eeCycle = 0u;
        uint32_t pc = 0u;
        uint32_t ra = 0u;
        uint32_t sp = 0u;
        uint32_t ownerEntry = 0u;
        // 2026-09-01 part 44 -- the 32-bit word living at guest [$sp+0] at the
        // moment of dispatch. 0x177fe8's prologue is `sd $ra, 0x0($sp)` and its
        // epilogue is `ld $ra, 0x0($sp)`, so for that freeze this IS the slot
        // that decides the return. Sampling it per dispatch turns "the slot is
        // zero at the freeze" (a single end-state observation, which cannot
        // distinguish "the prologue stored a zero $ra" from "the prologue never
        // ran and this is untouched caller memory") into a HISTORY: if an
        // earlier dispatch at the same sp shows a non-zero slot, something
        // zeroed it; if it is zero from the first sighting, no store ever
        // happened there. Offsets other than +0 are not sampled -- this is one
        // read, and widening it would put real cost in the hot dispatch path.
        uint32_t raSlot = 0u;
        int      tid = 0;
        uint16_t invocationDepth = 0u;
        bool     midFunctionResume = false;
        bool     valid = false;
    };

    // 2026-09-01 part 44 -- 16 was too short to answer the question the ring
    // was built for. At the part-43 freeze all 16 slots were already spent on
    // the final cascade, so the ring could not show whether 0x177fe8 had ever
    // been dispatched at its TRUE entry (pc == owner, i.e. prologue runs)
    // before the fatal mid-function resume at 0x178018. 256 entries is 6 KB of
    // static storage and the push cost is unchanged.
    constexpr uint32_t kEeDispatchRingSize = 256u;
    EeDispatchRecord g_eeDispatchRing[kEeDispatchRingSize];
    uint32_t         g_eeDispatchRingPos = 0u;

    // Only the tail is printed verbatim; the full 256 are still scanned by the
    // frame-history pass below, which is where the answer actually lives.
    constexpr uint32_t kEeDispatchRingPrint = 48u;

    // ------------------------------------------------------------------
    // 2026-09-01 part 45 -- COLD-RESUME WITNESS TABLE.
    //
    // A static scan (build_scripts/find_resume_label_hazards.py) shows 10,325
    // of the 11,081 functions carrying a resume label put one AFTER the
    // prologue's addiu $sp,-N / sd $ra,off($sp). That is 93%, so the SHAPE
    // cannot be the bug -- nothing would boot at all. The shape is the intended
    // design: on a genuine resume the prologue ran during an EARLIER
    // invocation and the guest frame is still live, so skipping it is correct.
    //
    // The defect is therefore not "a resume label exists" but "THIS resume is
    // COLD" -- dispatched with no prior true entry, so the frame it is about to
    // tear down was never built. Not decidable statically. Trivial here.
    //
    // Every dispatch at a TRUE entry (pc == ownerEntry, prologue runs) records a
    // witness keyed by (thread, ownerEntry). Every mid-function resume looks it
    // up:
    //   witness present -> prologue really did run on this thread; legitimate.
    //   witness ABSENT  -> prologue has NEVER run on this thread, so
    //                      ld $ra,off($sp) reads memory this function never
    //                      wrote and jr $ra jumps there. That is the freeze,
    //                      caught at its cause rather than at the pc==0 symptom
    //                      several dispatches downstream.
    //
    // Unlike the ring this is not bounded by recency, so absence is a real
    // negative rather than "outside the window" (cf. the ring caveat and
    // feedback_capped_probes_false_negatives). Open addressing, fixed storage,
    // no allocation; lookup is a few loads on the dispatch path.
    struct EeEntryWitness
    {
        uint64_t key = 0u;      // (tid << 32) | ownerEntry; 0 == empty slot
        uint32_t lastSp = 0u;
        uint32_t entries = 0u;
    };
    constexpr uint32_t kEeWitnessSlots = 16384u;   // power of two
    EeEntryWitness g_eeWitness[kEeWitnessSlots];
    uint64_t       g_eeWitnessFull = 0u;           // insert failures; see below

    inline uint64_t eeWitnessKey(int tid, uint32_t ownerEntry)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tid)) << 32) |
               static_cast<uint64_t>(ownerEntry);
    }

    // Returns the slot for key, or nullptr when not found / cluster saturated.
    // Saturation is counted separately in g_eeWitnessFull: a full table must
    // never be allowed to manufacture a false "cold resume" report.
    inline EeEntryWitness *eeWitnessFind(uint64_t key, bool insert)
    {
        uint64_t h = key * 0x9E3779B97F4A7C15ull;
        h ^= (h >> 29);
        const uint32_t idx = static_cast<uint32_t>(h) & (kEeWitnessSlots - 1u);
        for (uint32_t probe = 0u; probe < 64u; ++probe)
        {
            EeEntryWitness &slot = g_eeWitness[(idx + probe) & (kEeWitnessSlots - 1u)];
            if (slot.key == key) { return &slot; }
            if (slot.key == 0u)
            {
                if (!insert) { return nullptr; }
                slot.key = key;
                return &slot;
            }
        }
        if (insert) { ++g_eeWitnessFull; }
        return nullptr;
    }

    // Cold resumes seen so far. Always counted; the first few are printed so a
    // run that does NOT freeze still reports whether the class is live.
    uint64_t g_eeColdResumeCount = 0u;
    uint32_t g_eeColdResumePrinted = 0u;
    constexpr uint32_t kEeColdResumePrintMax = 24u;

    // 2026-09-01 part 46 -- INVOCATION LIFECYCLE TRACE.
    //
    // The part-45 run froze with the dispatch ring showing 0x178068 entered at
    // its TRUE entry with depth=1, yet the pc==0 handler reported "no
    // invocation to pop". Those two facts cannot both be right unless something
    // popped (or never pushed) the invocation in between. The pre-existing
    // [semwatch:zeropc] probe cannot arbitrate: it is capped at 32 and all 32
    // slots were spent by eeCycle 103M, while the freeze is at 20.6 BILLION --
    // a saturated probe, so its "invocationsEmpty=0" says nothing about the
    // failure (feedback_capped_probes_false_negatives).
    //
    // There are FIVE places that touch the invocation stack and they are spread
    // across three member functions, two of which unwind by throwing
    // EeDispatcherTransfer. Recording all of them in one ordered ring is the
    // only way to see the actual sequence.
    enum class EeInvKind : uint8_t { PushPending, PushDispatcher, PushInvoke, PushSequence, Pop };
    struct EeInvEvent
    {
        uint64_t  eeCycle = 0u;
        uint32_t  pc = 0u;
        uint32_t  sp = 0u;
        uint32_t  depthAfter = 0u;
        int       tid = 0;
        EeInvKind kind = EeInvKind::Pop;
        uint8_t   gkind = 0xFFu;   // GuestInvocationKind, 0xFF == not applicable
        bool      valid = false;
    };
    constexpr uint32_t kEeInvRingSize = 128u;
    EeInvEvent g_eeInvRing[kEeInvRingSize];
    uint32_t   g_eeInvRingPos = 0u;

    const char *eeInvKindName(EeInvKind k)
    {
        switch (k)
        {
        case EeInvKind::PushPending:    return "push/pending";
        case EeInvKind::PushDispatcher: return "push/dispatcher";
        case EeInvKind::PushInvoke:     return "push/invoke";
        case EeInvKind::PushSequence:   return "push/sequence";
        case EeInvKind::Pop:            return "POP";
        }
        return "?";
    }

    const char *eeAsyncKindName(uint8_t g)
    {
        switch (static_cast<GuestInvocationKind>(g))
        {
        case GuestInvocationKind::Interrupt:       return "Interrupt";
        case GuestInvocationKind::Alarm:           return "Alarm";
        case GuestInvocationKind::GsCallback:      return "GsCallback";
        case GuestInvocationKind::RpcCallback:     return "RpcCallback";
        case GuestInvocationKind::SyscallOverride: return "SyscallOverride";
        case GuestInvocationKind::ExitHandler:     return "ExitHandler";
        case GuestInvocationKind::HleCall:         return "HleCall";
        }
        return "?";
    }

    // 2026-09-01 part 46 -- ROOT CAUSE of the parts 37-45 freeze family.
    //
    // Syscalls/Interrupt.cpp:146 addHandler() captures getRegU32(ctx, 29) --
    // the REGISTERING function's $sp -- and stores it as the IRQ handler's
    // stack. Every later dispatch of that handler then runs on that address.
    // On real hardware AddIntcHandler takes no stack; the kernel runs the
    // handler on its own interrupt stack.
    //
    // Measured at the freeze (run 2026-09-01 01:59):
    //   thread 1 executing 0x178a08, $sp=0x1fef9c0, frame 0x70,
    //     saved $ra at 0x1fef9c0+0x60 = 0x1fefa20
    //   IRQ invocation 0x178068 dispatched with $sp=0x1fefa40,
    //     prologue `addiu $sp,$sp,-0xA0` -> writes 0x1fef9a0..0x1fefa40
    //   => the handler frame COVERS the interrupted frame's $ra slot.
    //   The dormant dump's guest-stack read confirms +0x60 == 0x0, and the
    //   epilogue `ld $ra,0x60($sp); jr $ra` then jumps to 0 -> pc==0 with no
    //   invocation to pop -> silent permanent hang.
    //
    // This is why parts 37-42's per-address guards kept whack-a-moling: each
    // was a different victim of one stack overlap, not a distinct bug.
    //
    // Fix: async invocations (Interrupt / Alarm / GsCallback) always run on a
    // reserved callback stack instead of a guest-supplied $sp. Set
    // PS2X_IRQ_OWN_STACK=0 to restore the old behaviour for A/B testing.
    bool eeIrqOwnStack()
    {
        static const bool enabled = []()
        {
            const char *e = std::getenv("PS2X_IRQ_OWN_STACK");
            return (e == nullptr) || (*e != '0');
        }();
        return enabled;
    }

    // 2026-09-02 (session 5, part 49) -- the t~140s wall, root-caused.
    //
    // THCREATE (added part 48) caught it: CreateThread is called 5 times in a
    // 200s run and EVERY call is refused with -403 (KE_ILLEGAL_PRIORITY),
    // because every call passes initial_priority == 0:
    //
    //   seq=2     func=0x1759a0 prio=0 stksz=0x400
    //   seq=1303  func=0x11e7e0 prio=0 stksz=0x800    <-- the four at the wall
    //   seq=1306  func=0x11e8d0 prio=0 stksz=0x1000
    //   seq=1308  func=0x11e9d8 prio=0 stksz=0x1000
    //   seq=1310  func=0x11eac8 prio=0 stksz=0x2000
    //
    // PRIOREJECT then fires 15 more times, all ChangeThreadPriority(thid=1,
    // prio=0) from the same code (ra=0x11e670). ROTREJECT never fires, so
    // rotateReadyQueue is not involved.
    //
    // The guest stores the -403 as a thread id and polls it forever:
    // ReferThreadStatus(-403) -> not found -> SleepThread. PSEUDOREFER
    // confirms 653 such polls, thid=0xfffffe6d and found=0 on every one.
    //
    // The refusal is OURS. The lower bound `priority < 1` was never validated
    // against real EE -- rotateReadyQueue below uses `priority < 0` for the
    // same range, so the two already disagree with each other. ps2tek
    // documents no lower bound for CreateThread (only "returns -1 if the
    // function fails"), and the game shipped on real hardware making exactly
    // these calls, so priority 0 cannot have been rejected there.
    //
    // Priority 0 is structurally fine here: m_readyQueues is indexed 0..127
    // and the ready-queue invariant assert is already `>= 0`.
    //
    // Set PS2X_EE_PRIO0=0 to restore the old strict check for A/B testing.
    // NOT yet cross-checked against PCSX2 -- do that before deleting the gate.
    bool eeAllowPriorityZero()
    {
        static const bool enabled = []()
        {
            const char *e = std::getenv("PS2X_EE_PRIO0");
            return (e == nullptr) || (*e != '0');
        }();
        return enabled;
    }

    // Lowest priority value the scheduler will accept from the guest.
    int eeMinGuestPriority() { return eeAllowPriorityZero() ? 0 : 1; }

    bool eeAsyncNeedsOwnStack(GuestInvocationKind k)
    {
        if (!eeIrqOwnStack()) { return false; }
        return k == GuestInvocationKind::Interrupt ||
               k == GuestInvocationKind::Alarm ||
               k == GuestInvocationKind::GsCallback;
    }

    uint64_t g_eeOwnStackForced = 0u;

    void eeRecordInv(EeInvKind kind, int tid, uint32_t pc, uint32_t sp,
                     size_t depthAfter, uint64_t eeCycle, uint8_t gkind)
    {
        EeInvEvent &e = g_eeInvRing[g_eeInvRingPos % kEeInvRingSize];
        e.eeCycle = eeCycle;
        e.pc = pc;
        e.sp = sp;
        e.depthAfter = static_cast<uint32_t>(depthAfter);
        e.tid = tid;
        e.kind = kind;
        e.gkind = gkind;
        e.valid = true;
        ++g_eeInvRingPos;
    }

    // Abort (instead of going silently dormant) on the zero-pc freeze, so the
    // failure becomes an attachable stack instead of a hang. Off by default.
    bool eeFatalOnZeroPcDormant()
    {
        static const bool enabled = []() -> bool
        {
            const char *e = std::getenv("PS2X_FATAL_ON_ZERO_PC");
            return e && *e && *e != '0';
        }();
        return enabled;
    }
}

// 2026-09-01 part 45 -- true-entry witness recorder, called from
// PS2Runtime::dispatchGuestBranch (ps2_runtime.cpp). That is the ONLY
// chokepoint that observes a function being entered at its start: a JAL runs
// there as a nested C++ call and never round-trips through the scheduler loop
// (see the part-31 note in ps2_runtime.cpp, which established exactly this for
// mem_fill_z_18 at 0x178a08). Recording solely from the scheduler loop would
// witness almost no true entries and would therefore label nearly every
// LEGITIMATE resume "cold" -- a degenerate probe of precisely the kind
// feedback_degenerate_result_convicts_the_probe warns about.
extern "C" void ps2x_witness_true_entry(uint32_t entryPc, uint32_t sp)
{
    if (entryPc == 0u) { return; }
    EeEntryWitness *w = eeWitnessFind(
        eeWitnessKey(ps2x_guest_current_thread_id(), entryPc), true);
    if (w != nullptr) { w->lastSp = sp; ++w->entries; }
}


EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
    s_activeScheduler = this;
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    main.stack = getRegU32(&mainContext, 29);
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    // SDBZ: do NOT self-seed the wall-clock vblank timer here. Upstream drives
    // vblank purely off kVBlankPeriodCycles/kVBlankPeriod real time, with no
    // awareness of PS2X_DETERMINISM's guest-progress-quantum pacing (Stage
    // 5.17, see Interrupt.cpp's deterministicVblankQuantum()/
    // interruptWorkerMain()). SDBZ's IRQ worker thread is the sole source of
    // EeEventType::VBlankStart via postEvent() instead - seeding one here too
    // would create a second, uncoordinated vblank source racing the real one.
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                if (owner->id >= 0) { m_lastRealThreadId = owner->id; }
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u ||
                    eeAsyncNeedsOwnStack(invocation.kind))
                {
                    if (getRegU32(&invocation.context, 29) != 0u) { ++g_eeOwnStackForced; }
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                {
                    const uint32_t invPc = invocation.context.pc;
                    const uint32_t invSp = getRegU32(&invocation.context, 29);
                    const uint8_t invKind = static_cast<uint8_t>(invocation.kind);
                    owner->invocations.push_back(std::move(invocation));
                    eeRecordInv(EeInvKind::PushDispatcher, owner->id, invPc, invSp,
                                owner->invocations.size(), m_eeCycle, invKind);
                }
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
        m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
        m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
        m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);

        // RecompDebugger IPC: publish this thread's pc/gpr/hi/lo into the
        // shared-memory RecompDebugState once per scheduler iteration, and
        // service any armed breakpoint for this thread. Ported here (Phase 3d)
        // from the retired dispatchLoop() -- this is now the sole per-iteration
        // point where a guest thread's context is live just before dispatch.
        {
            uint32_t dbg_gpr[32];
            for (int i = 0; i < 32; ++i)
                dbg_gpr[i] = getRegU32(&context, i);
            RecompDbg::Update(m_currentThreadId, context.pc, dbg_gpr,
                               static_cast<uint32_t>(context.hi),
                               static_cast<uint32_t>(context.lo),
                               context.insn_count,
                               m_rdram, PS2_RAM_SIZE);
            if (RecompDbg::CheckBreakpoint(m_currentThreadId, context.pc & 0x1FFFFFFFu, dbg_gpr))
            {
                // Breakpoint/step handling may have edited dbg_gpr (a debugger-armed
                // register write); mirror only the low 32-bit lane back into the live
                // 128-bit MMI register so the other lanes are left untouched.
                for (int i = 1; i < 32; ++i)
                    SET_GPR_U32(&context, i, dbg_gpr[i]);
            }
        }

        static uint32_t s_semwatchLastDispatchPc = 0u;
        static uint32_t s_semwatchLastDispatchTid = 0u;
        // 2026-08-30 part 36 -- [semwatch:zeropc] keeps naming lastDispatchPc
        // as an address (0x178aec) that [semwatch:fillz18mid], placed
        // unconditionally right after the SAME s_semwatchLastDispatchPc
        // assignment this reads, logged ZERO hits for -- despite there being
        // no code between the two that can touch context.pc. That's a real
        // contradiction, not a hypothesis; before trusting either probe
        // again, capture the actual last-4 dispatch PCs in a small ring
        // buffer written at the identical assignment site, so zeropc's next
        // fire prints the true sequence instead of relying on single-address
        // gates that may be missing something about how this loop reaches
        // them. Capped implicitly by the outer zeropc cap.
        static uint32_t s_dispatchHist[4] = {0u, 0u, 0u, 0u};
        static uint32_t s_dispatchHistPos = 0u;
        if (context.pc == 0u)
        {
            // 2026-08-29 part 29 -- [semwatch:zeropc]: after the SIF.cpp
            // trampoline-follow fix, the WaitSema(4)/(5) deadlock is gone
            // (both now SignalSema+WaitSema cleanly, see [semwatch:reqendgate]
            // ranToOwnReturn=1), but the run still goes totally idle right
            // after -- eeCycle freezes and busy%=0 for the rest of the run.
            // [semwatch:dispexit] #15 showed exitPc=0x0 from the OUTER stack
            // (entrySp=0x1ffbe40, not the nested SIF-reply frames), landing
            // right here. Logging what dispatched last and whether this
            // thread had any invocations pending settles whether this is a
            // legitimate invocation-stack pop (parent resumes) or the guest's
            // only thread (nTh=1 per [thsync]) going dormant with nothing
            // left to schedule it back. Capped; unconditional read only.
            {
                static std::atomic<uint32_t> s_zeroPcLogs{0u};
                const uint32_t n = s_zeroPcLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u)
                {
                    std::cerr << "[semwatch:zeropc] #" << n
                              << " tid=" << running->id
                              << " invocationsEmpty=" << running->invocations.empty()
                              << " lastDispatchPc=0x" << std::hex << s_semwatchLastDispatchPc
                              << " lastDispatchTid=" << std::dec << s_semwatchLastDispatchTid
                              << " hist=0x" << std::hex << s_dispatchHist[(s_dispatchHistPos + 0u) % 4u]
                              << ",0x" << s_dispatchHist[(s_dispatchHistPos + 1u) % 4u]
                              << ",0x" << s_dispatchHist[(s_dispatchHistPos + 2u) % 4u]
                              << ",0x" << s_dispatchHist[(s_dispatchHistPos + 3u) % 4u]
                              << std::dec
                              << " eeCycle=" << m_eeCycle
                              << std::endl;
                }
            }
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                eeRecordInv(EeInvKind::Pop, running->id, completed.context.pc,
                            getRegU32(&completed.context, 29),
                            running->invocations.size(), m_eeCycle,
                            static_cast<uint8_t>(completed.kind));
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }

            // 2026-08-31 -- pc==0 with no invocation to pop. This is TWO different
            // events sharing one code path, which is a large part of why parts
            // 37-42 kept mistaking one for the other:
            //
            //   EXPECTED  the thread's own entry function returned. startThread()
            //             deliberately seeds the base context with $ra=0 (see
            //             ~line 1089 below), so a clean thread exit lands here.
            //   SUSPECT   anything else returned to $ra==0 -- a `jr $ra` that read
            //             a stale/zero register. For a single-threaded guest the
            //             makeDormant() below is then a permanent silent hang, and
            //             it also DESTROYS the evidence, because makeDormant()
            //             clears `invocations`.
            //
            // Telling them apart is just `ownerEntry == running->entry`. Dump the
            // ring either way (this path is terminal per thread, so it is not a
            // spam risk) but only escalate on the SUSPECT case.
            //
            // This replaces the parts 37-42 approach of hardcoding one guard per
            // freeze address: the ring names the offending resume label, its
            // owning function, and the $ra it was entered with, so the next
            // occurrence is identified rather than guessed at.
            const EeDispatchRecord &lastDispatch =
                g_eeDispatchRing[(g_eeDispatchRingPos + kEeDispatchRingSize - 1u) % kEeDispatchRingSize];
            // 2026-09-01 part 46 -- a THIRD expected case, found the moment the
            // IRQ-stack fix let the run reach 130s: acquireInvocationThread()
            // (EeScheduler.cpp:2920) mints a pseudo-thread with id < 0 and
            // entry == 0 whose only job is to host a pending invocation when no
            // real thread is runnable. Its invocation completing with pc==0 and
            // an empty stack is exactly how it is recycled -- makeDormant() is
            // correct there. Comparing ownerEntry against entry==0 flagged that
            // normal recycle as SUSPECT and aborted the run.
            const bool zeroPcExpected =
                lastDispatch.valid &&
                (lastDispatch.ownerEntry == running->entry || running->id < 0);
            // 2026-09-01 part 47 -- volume control. The part-46 run logged 379
            // EXPECTED dormant events; at ~368 lines of ring dump each that was
            // 139,596 lines, 87% of a 39 MB run_log. The pseudo-thread recycle is
            // normal and its ring is identical every time, so only the first few
            // get the full dump. SUSPECT is NEVER suppressed -- that is the case
            // worth reading and it must not be lost inside a rate limiter.
            static uint64_t s_zeroPcExpectedSeen = 0u;
            const uint64_t expectedSeen = zeroPcExpected ? ++s_zeroPcExpectedSeen : 0u;
            if (zeroPcExpected && expectedSeen > 3u)
            {
                if (expectedSeen == 4u)
                {
                    std::cerr << "[ee:zero-pc-dormant] further EXPECTED events "
                                 "(pseudo-thread recycle) collapse to one line each; "
                                 "SUSPECT is never suppressed." << std::endl;
                }
                std::cerr << "[ee:zero-pc-dormant] EXPECTED #" << std::dec << expectedSeen
                          << " tid=" << running->id
                          << " eeCycle=" << m_eeCycle << std::endl;
            }
            else
            {
                std::cerr << "\n[ee:zero-pc-dormant] " << (zeroPcExpected ? "EXPECTED" : "SUSPECT")
                          << " -- guest pc reached 0 with no invocation to pop.\n"
                          << "[ee:zero-pc-dormant]   tid=" << running->id
                          << " entry=0x" << std::hex << running->entry << std::dec
                          << " liveThreads=" << m_threads.size()
                          << " eeCycle=" << m_eeCycle << "\n"
                          << "[ee:zero-pc-dormant]   dispatch ring (newest first, capped at "
                          << kEeDispatchRingPrint << " of " << kEeDispatchRingSize << "):\n";
                for (uint32_t i = 0u; i < kEeDispatchRingPrint; ++i)
                {
                    const uint32_t idx =
                        (g_eeDispatchRingPos + kEeDispatchRingSize - 1u - i) % kEeDispatchRingSize;
                    const EeDispatchRecord &rec = g_eeDispatchRing[idx];
                    if (!rec.valid)
                    {
                        continue;
                    }
                    std::cerr << "[ee:zero-pc-dormant]   ring" << std::dec << i << " pc=0x" << std::hex << rec.pc
                              << " owner=0x" << rec.ownerEntry
                              << (rec.midFunctionResume ? " MID-RESUME" : "          ")
                              << " ra=0x" << rec.ra
                              << " sp=0x" << rec.sp
                              << " slot=0x" << rec.raSlot
                              << std::dec
                              << " tid=" << rec.tid
                              << " depth=" << rec.invocationDepth
                              << " eeCycle=" << rec.eeCycle
                              << std::endl;
                }

                // Verdict: the most recent dispatch is the one whose `jr $ra`
                // produced the 0, so name it explicitly rather than making the
                // reader re-derive it from the ring every time.
                if (lastDispatch.valid)
                {
                    std::cerr << "[ee:zero-pc-dormant]   verdict: last dispatch was "
                              << (lastDispatch.midFunctionResume ? "a MID-FUNCTION RESUME" : "a normal function entry")
                              << " at 0x" << std::hex << lastDispatch.pc
                              << " inside 0x" << lastDispatch.ownerEntry
                              << ", entered with $ra=0x" << lastDispatch.ra
                              << std::dec;
                    if (zeroPcExpected)
                    {
                        std::cerr << " -- matches this thread's entry, so this is a normal exit.";
                    }
                    else
                    {
                        // NOTE: this does NOT prove $ra was corrupt -- a return
                        // address is not required to equal the thread entry. What
                        // it does establish is that the final dispatch was not
                        // this thread's own top frame, so the 0 did not come from
                        // a normal thread exit. Naming it SUSPECT is the claim;
                        // the mechanism still has to be read off the ring.
                        std::cerr << " -- owner does NOT match this thread's entry (0x" << std::hex
                                  << running->entry << std::dec
                                  << "), so this is not a normal thread exit.";
                    }
                    std::cerr << std::endl;
                }
                else
                {
                    std::cerr << "[ee:zero-pc-dormant]   verdict: dispatch ring is empty -- pc hit 0 before any dispatch."
                              << std::endl;
                }

                // The register $ra recorded in the ring is sampled at DISPATCH
                // time and is usually CORRECT -- for a resume label it is just
                // the return address the JAL wrote. It is NOT what breaks.
                //
                // What breaks is the RDRAM stack slot the callee's epilogue
                // reloads: mem_fill_z_18's epilogue is `ld $ra, 0x60($sp)`
                // (mem_fill_z_18_0x178a08.cpp:435), and parts 37/39 both fixed
                // exactly that class by writing a slot, not a register. The
                // offset differs per function, so rather than hardcode one,
                // dump a window of the guest stack at the last dispatch's sp
                // and let the reader see which slot reads 0.
                // 2026-09-01 part 44 -- FRAME HISTORY. The window below is a
                // single end-state sample and cannot, on its own, separate the
                // two readings of a zero $ra slot:
                //   A  the prologue DID run and stored an already-zero $ra
                //   B  the prologue never ran and this is stale caller memory
                // Both produce identical sp arithmetic, so sp cannot adjudicate
                // (an earlier note claiming the sp delta showed a leak was
                // wrong: +0x10 out of a 0x10 frame is exactly correct).
                //
                // Replaying every ring entry that touched either the failing
                // FRAME (same $sp) or the failing FUNCTION (same owner) does
                // separate them. If a dispatch at this sp ever shows slot!=0,
                // the slot was live and got zeroed -> reading A, and the store
                // is findable. If the slot reads 0 at every sighting AND the
                // owner never appears at pc==owner (its true entry), the
                // prologue genuinely never ran -> reading B, and the fault is
                // the cold depth-0 re-dispatch into a resume label.
                if (lastDispatch.valid)
                {
                    std::cerr << "[ee:zero-pc-dormant]   frame history -- every ring entry with sp=0x"
                              << std::hex << lastDispatch.sp << " or owner=0x"
                              << lastDispatch.ownerEntry << std::dec
                              << " (oldest first):" << std::endl;
                    uint32_t shown = 0u;
                    uint32_t trueEntryHits = 0u;
                    for (uint32_t i = 0u; i < kEeDispatchRingSize; ++i)
                    {
                        const uint32_t idx = (g_eeDispatchRingPos + i) % kEeDispatchRingSize;
                        const EeDispatchRecord &rec = g_eeDispatchRing[idx];
                        if (!rec.valid)
                        {
                            continue;
                        }
                        if (rec.sp != lastDispatch.sp && rec.ownerEntry != lastDispatch.ownerEntry)
                        {
                            continue;
                        }
                        if (!rec.midFunctionResume && rec.ownerEntry == lastDispatch.ownerEntry)
                        {
                            ++trueEntryHits;
                        }
                        ++shown;
                        std::cerr << "[ee:zero-pc-dormant]     #" << std::dec << i
                                  << " pc=0x" << std::hex << rec.pc
                                  << " owner=0x" << rec.ownerEntry
                                  << (rec.midFunctionResume ? " MID-RESUME" : " TRUE-ENTRY")
                                  << " ra=0x" << rec.ra
                                  << " sp=0x" << rec.sp
                                  << " slot=0x" << rec.raSlot
                                  << std::dec
                                  << " depth=" << rec.invocationDepth
                                  << " eeCycle=" << rec.eeCycle
                                  << std::endl;
                    }
                    std::cerr << "[ee:zero-pc-dormant]     -> " << std::dec << shown
                              << " matching dispatches; " << trueEntryHits
                              << " of them entered 0x" << std::hex << lastDispatch.ownerEntry
                              << std::dec << " at its TRUE entry (prologue would have run)."
                              << std::endl;
                    if (trueEntryHits == 0u)
                    {
                        std::cerr << "[ee:zero-pc-dormant]     NOTE: zero true entries within the ring's "
                                     "reach is NOT proof the prologue never ran -- the ring only holds "
                                  << kEeDispatchRingSize << " dispatches." << std::endl;
                    }

                    // Ring-independent verdict. The witness table has no recency
                    // bound, so unlike the NOTE above this absence IS conclusive
                    // -- provided the table never saturated, which is why the
                    // insert-failure count is printed alongside it.
                    const EeEntryWitness *w = eeWitnessFind(
                        eeWitnessKey(lastDispatch.tid, lastDispatch.ownerEntry), false);
                    std::cerr << "[ee:zero-pc-dormant]     WITNESS: owner 0x" << std::hex
                              << lastDispatch.ownerEntry << std::dec
                              << (w != nullptr ? " HAS" : " has NO")
                              << " recorded true entry on tid " << lastDispatch.tid;
                    if (w != nullptr)
                    {
                        std::cerr << " (" << w->entries << " true entries, last sp=0x"
                                  << std::hex << w->lastSp << std::dec << ")";
                    }
                    std::cerr << std::endl;
                    std::cerr << "[ee:zero-pc-dormant]     cold resumes this run: "
                              << g_eeColdResumeCount
                              << "; witness-table insert failures: " << g_eeWitnessFull
                              << (g_eeWitnessFull != 0u
                                      ? "  <-- NONZERO: table saturated, counts unreliable"
                                      : "")
                              << std::endl;

                    // Invocation lifecycle, newest first. The freeze signature
                    // is a Pop (or a missing Push) between the dispatch of the
                    // failing function and its return to pc==0.
                    std::cerr << "[ee:zero-pc-dormant]     async invocations forced onto a "
                             "reserved callback stack (PS2X_IRQ_OWN_STACK): "
                          << g_eeOwnStackForced
                          << (eeIrqOwnStack() ? "  [fix ENABLED]" : "  [fix DISABLED]")
                          << std::endl;
                std::cerr << "[ee:zero-pc-dormant]     invocation events (newest first):" << std::endl;
                    for (uint32_t i = 0u; i < kEeInvRingSize; ++i)
                    {
                        const uint32_t idx =
                            (g_eeInvRingPos + kEeInvRingSize - 1u - i) % kEeInvRingSize;
                        const EeInvEvent &e = g_eeInvRing[idx];
                        if (!e.valid) { continue; }
                        std::cerr << "[ee:zero-pc-dormant]       " << eeInvKindName(e.kind)
                                  << " tid=" << std::dec << e.tid
                                  << " pc=0x" << std::hex << e.pc
                                  << " sp=0x" << e.sp << std::dec
                                  << " depthAfter=" << e.depthAfter
                                  << " kind=" << (e.gkind == 0xFFu ? "-" : eeAsyncKindName(e.gkind))
                                  << " eeCycle=" << e.eeCycle << std::endl;
                    }
                }

                if (lastDispatch.valid && m_rdram != nullptr)
                {
                    std::cerr << "[ee:zero-pc-dormant]   guest stack at sp=0x"
                              << std::hex << lastDispatch.sp << std::dec
                              << " (epilogues reload $ra from one of these):" << std::endl;
                    for (uint32_t off = 0u; off <= 0x80u; off += 16u)
                    {
                        std::cerr << "[ee:zero-pc-dormant]     +0x" << std::hex << off << "  ";
                        for (uint32_t w = 0u; w < 4u; ++w)
                        {
                            const uint32_t addr = lastDispatch.sp + off + (w * 4u);
                            if (addr > PS2_RAM_SIZE - 4u)
                            {
                                std::cerr << "........ ";
                                continue;
                            }
                            std::cerr << "0x" << Ps2FastRead32(m_rdram, addr) << ' ';
                        }
                        std::cerr << std::dec << std::endl;
                    }
                }
                std::cerr << std::endl;
            }

            if (!zeroPcExpected && eeFatalOnZeroPcDormant())
            {
                std::cerr << "[ee:zero-pc-dormant] PS2X_FATAL_ON_ZERO_PC set -- aborting so the "
                             "freeze can be debugged as a live stack." << std::endl;
                std::abort();
            }

            makeDormant(*running);
            m_currentThreadId = 0;
            continue;
        }

        if (!m_pendingInvocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u ||
                eeAsyncNeedsOwnStack(invocation.kind))
            {
                if (getRegU32(&invocation.context, 29) != 0u) { ++g_eeOwnStackForced; }
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            {
                const uint32_t invPc = invocation.context.pc;
                const uint32_t invSp = getRegU32(&invocation.context, 29);
                const uint8_t invKind = static_cast<uint8_t>(invocation.kind);
                running->invocations.push_back(std::move(invocation));
                eeRecordInv(EeInvKind::PushPending, running->id, invPc, invSp,
                            running->invocations.size(), m_eeCycle, invKind);
            }
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);
        s_semwatchLastDispatchPc = context.pc;
        s_semwatchLastDispatchTid = running->id;
        s_dispatchHist[s_dispatchHistPos % 4u] = context.pc;
        ++s_dispatchHistPos;

        // General dispatch ring (see the eeResolveOwnerEntry block near the top
        // of this file). Unconditional, no I/O, no allocation -- this is the only
        // place the loop dispatches, so the ring is a complete tail of guest
        // control flow at the moment of any later failure.
        {
            const uint32_t ownerEntry = eeResolveOwnerEntry(context.pc);
            EeDispatchRecord &rec = g_eeDispatchRing[g_eeDispatchRingPos % kEeDispatchRingSize];
            rec.eeCycle = m_eeCycle;
            rec.pc = context.pc;
            rec.ra = GPR_U32((&context), 31);
            rec.sp = GPR_U32((&context), 29);
            rec.ownerEntry = ownerEntry;
            rec.raSlot = (m_rdram != nullptr && rec.sp <= PS2_RAM_SIZE - 4u)
                             ? Ps2FastRead32(m_rdram, rec.sp)
                             : 0xFFFFFFFFu;
            rec.tid = running->id;
            rec.invocationDepth = static_cast<uint16_t>(running->invocations.size());
            rec.midFunctionResume = (ownerEntry != context.pc);

            // Cold-resume decision (see EeEntryWitness above). This is the
            // generic replacement for the per-address guards -- it names any
            // instance of the class, not the four we happened to hit.
            if (ownerEntry != 0u)
            {
                const uint64_t wkey = eeWitnessKey(running->id, ownerEntry);
                if (!rec.midFunctionResume)
                {
                    EeEntryWitness *w = eeWitnessFind(wkey, true);
                    if (w != nullptr) { w->lastSp = rec.sp; ++w->entries; }
                }
                else if (eeWitnessFind(wkey, false) == nullptr)
                {
                    ++g_eeColdResumeCount;
                    if (g_eeColdResumePrinted < kEeColdResumePrintMax)
                    {
                        ++g_eeColdResumePrinted;
                        std::cerr << "[ee:cold-resume] #" << std::dec << g_eeColdResumeCount
                                  << " tid=" << running->id
                                  << " resume pc=0x" << std::hex << context.pc
                                  << " owner=0x" << ownerEntry
                                  << " ra=0x" << rec.ra
                                  << " sp=0x" << rec.sp
                                  << " slot=0x" << rec.raSlot
                                  << std::dec << " depth=" << rec.invocationDepth
                                  << " eeCycle=" << m_eeCycle
                                  << "  (owner never entered at its true entry on this thread)"
                                  << std::endl;
                    }
                }
            }
            rec.valid = true;
            ++g_eeDispatchRingPos;
        }

        // 2026-08-30 part 30 -- [semwatch:zeropc] #9 showed the freeze is
        // `mem_fill_z_18_0x178a08` (a widely-shared helper, eeref.py up
        // shows callers=19 via sub_179B28 alone -- NOT a thread-entry
        // trampoline) reaching its own `jr $ra` at 0x178b4c with $ra==0,
        // which is why ctx->pc lands on 0 with invocations empty: nothing
        // corrupted it mid-function (it saves/restores $ra from its own
        // stack frame at 0x178a28/0x178b38), so $ra must already have been
        // 0 in GPR31 at the moment this specific call was made. Logging
        // $ra/$sp on entry (ctx->pc==0x178a08 exactly, i.e. a fresh call,
        // not a mid-function label resume) across the run's first N calls
        // will show whether earlier calls into this function had a real
        // return address and only the fatal one didn't -- which would
        // point at whoever's `jal 0x178a08` (or a `dispatchGuestBranch`
        // DirectCall path standing in for it) failed to set $ra, rather
        // than at this function itself. Capped; unconditional read only.
        // 2026-08-30 part 35 -- [semwatch:fillz18mid]: [semwatch:zeropc] #9
        // (this run) showed the fatal dormancy came right after
        // lastDispatchPc=0x178aec -- NOT mem_fill_z_18's real entry
        // (0x178a08, already cleared as normal by fillz18entry/
        // fillz18dispatch), but an INTERNAL join point 0xe4 bytes into the
        // same function (static disasm: `jal 0x174cb0` at 0x178aec, falling
        // through to `ld $ra,96($sp) ... jr $ra` at 0x178b38/0x178b4c).
        // hasFunction(0x178aec) is true (no reportMissingFunction line in
        // the log) and fn_forward_decls.h/game_overrides.cpp have no entry
        // for it, so it must be a runner-generated indirect-jump stub tied
        // to mem_fill_z_18's OWN stack frame ($s0-$s3/$ra saved at function
        // entry, restored via `ld $ra,96($sp)` right before this stub's
        // final jr). If the scheduler ever dispatches 0x178aec as a FRESH
        // top-level call (this loop, via lookupFunction) rather than as a
        // nested C++ call sharing mem_fill_z_18's live stack, `96($sp)`
        // reads whatever is actually in RDRAM at that offset of a possibly
        // unrelated stack -- easily 0 -- which would make `jr $ra` land on
        // ctx->pc=0 and explain the zeropc dormancy directly. Logging sp,
        // ra-on-entry, and the raw word at sp+96 (what jr $ra will load)
        // settles whether this dispatch is reusing a valid frame or reading
        // garbage. Capped; unconditional read only.
        if (context.pc == 0x178aecu)
        {
            const uint32_t sp = GPR_U32((&context), 29);
            const uint32_t raOnEntry = GPR_U32((&context), 31);
            const uint32_t raAtSpPlus96 = Ps2FastRead32(m_rdram, sp + 96u);

            static std::atomic<uint32_t> s_fillZ18MidLogs{0u};
            const uint32_t n = s_fillZ18MidLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u)
            {
                std::cerr << "[semwatch:fillz18mid] #" << n
                          << " tid=" << running->id
                          << " sp=0x" << std::hex << sp
                          << " raOnEntry=0x" << raOnEntry
                          << " raAtSpPlus96=0x" << raAtSpPlus96
                          << std::dec
                          << " eeCycle=" << m_eeCycle
                          << std::endl;
            }

            // 2026-08-31 part 37 -- WORKAROUND, not a fix (explicit user
            // directive; do not chase the mistranslation itself). This join
            // point is confirmed UNREACHABLE via any static jal/j in the EE
            // ELF (`eeref.py refs 0x178aec` -> "nothing links to this
            // address") -- it is only ever a valid resume point when reached
            // through mem_fill_z_18's real entry prologue at 0x178a08, which
            // is what stashes a real $ra at sp+96 before this label's `ld
            // $ra,96($sp); jr $ra` epilogue runs. When that slot reads 0 the
            // live invocation never went through that prologue; letting the
            // generated stub run anyway sends ctx->pc to 0 with the
            // invocation stack empty, and since this is the guest's sole
            // thread (nTh=1) that is a permanent freeze (see
            // [[project_session5_checkpointdue_bounce]] parts 36-37).
            // Patch just the one bad memory slot with the return address the
            // SAME call site (sub_17CF50's `jal 0x178a08` at 0x17cf9c) used
            // successfully moments earlier in this exact run
            // ([semwatch:zeropc] #8's healthy sibling case) rather than
            // inventing new register state or redirecting control flow --
            // keeps the fix-up local to the one corrupted value instead of
            // guessing at broader guest state.
            if (raAtSpPlus96 == 0u)
            {
                constexpr uint32_t kFallbackReturn = 0x17cfa4u;
                Ps2FastWrite32(m_rdram, sp + 96u, kFallbackReturn);
                static std::atomic<uint32_t> s_fillZ18WorkaroundLogs{0u};
                const uint32_t wn = s_fillZ18WorkaroundLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (wn <= 32u)
                {
                    std::cerr << "[workaround:fillz18aec] #" << wn
                              << " tid=" << running->id
                              << " sp=0x" << std::hex << sp
                              << " patchedRaTo=0x" << kFallbackReturn
                              << std::dec
                              << " eeCycle=" << m_eeCycle
                              << std::endl;
                }
            }
        }

        // 2026-08-31 part 38/39 -- [semwatch:cf50resume]: same corruption CLASS as
        // part 37, one call-frame up, confirmed by reading the generated code
        // rather than guessing (output/fn_17CF50_0x17cf50.cpp:105-126). sub_17CF50
        // calls mem_fill_z_18 via an INLINE C++ call and guards the return with
        // `if (ctx->pc != 0x17CFA4u) { return; }` -- when mem_fill_z_18 preempts
        // mid-body, that bail-out unwinds sub_17CF50's own C++ frame too, so the
        // eventual re-dispatch at 0x17cfa4 (whether from a real return or from
        // part 37's patched fallback) goes through the SCHEDULER's fresh
        // lookupFunction(0x17cfa4), entering fn_17CF50_0x17cf50 at label_17cfa4
        // directly -- skipping sub_17CF50's own real prologue at 0x17cf50, which
        // is what writes a valid $ra to sp+0x20 (see fn_17CF50_0x17cf50.cpp:35-37
        // `sd $ra,0x20($sp)` and :184-186 `ld $ra,0x20($sp)` in the epilogue).
        // [semwatch:zeropc] #10 caught exactly this: lastDispatchPc=0x17cfa4,
        // invocationsEmpty=1, 32 eeCycles after part 37's patch let mem_fill_z_18
        // return here -- the guest's sole thread (nTh=1) went permanently
        // Dormant again.
        //
        // part 39 -- safe fallback now established, same evidentiary bar as part
        // 37 (a healthy-sibling observation in the same run, not a guess):
        // `eeref.py up 0x17cf50` shows sub_17CF50 has exactly ONE static caller
        // in the whole binary -- sub_10C480, via a jal at 0x10c4ac -- so its
        // return address is invariant across every legitimate invocation, with
        // no call-site ambiguity. This run's own probe hit #1 (eeCycle=2813992,
        // BEFORE either dormant event) recorded raAtSpPlus32=0x10c4b4 == the
        // predicted 0x10c4ac+8 exactly, confirming the value live. Hits #2
        // (eeCycle=2814288, the original part-36 freeze point) and #3
        // (eeCycle=36369000, the post-part-37 freeze point) both read 0 at the
        // same slot. Patch that one RDRAM slot with 0x10c4b4 only when it reads
        // 0 -- mirrors part 37's EeScheduler.cpp:542-599 shape exactly. See
        // [[project_session5_checkpointdue_bounce]].
        if (context.pc == 0x17cfa4u)
        {
            const uint32_t sp = GPR_U32((&context), 29);
            const uint32_t raOnEntry = GPR_U32((&context), 31);
            const uint32_t raAtSpPlus32 = Ps2FastRead32(m_rdram, sp + 32u);

            static std::atomic<uint32_t> s_cf50ResumeLogs{0u};
            const uint32_t n = s_cf50ResumeLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u)
            {
                std::cerr << "[semwatch:cf50resume] #" << n
                          << " tid=" << running->id
                          << " sp=0x" << std::hex << sp
                          << " raOnEntry=0x" << raOnEntry
                          << " raAtSpPlus32=0x" << raAtSpPlus32
                          << std::dec
                          << " eeCycle=" << m_eeCycle
                          << std::endl;
            }

            if (raAtSpPlus32 == 0u)
            {
                Ps2FastWrite32(m_rdram, sp + 32u, 0x10c4b4u);

                static std::atomic<uint32_t> s_cf50ResumeFixes{0u};
                const uint32_t fn = s_cf50ResumeFixes.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (fn <= 32u)
                {
                    std::cerr << "[workaround:cf50resume] #" << fn
                              << " tid=" << running->id
                              << " sp=0x" << std::hex << sp
                              << " patched=0x10c4b4"
                              << std::dec
                              << " eeCycle=" << m_eeCycle
                              << std::endl;
                }
            }
        }

        // 2026-08-31 session 5 part 42 -- general dispatch-level guard, per user
        // decision after part 41's watchpoint came back clean (the sub_17CF50
        // corruption it targeted didn't recur that run) and the SAME run froze
        // 900x further out at a NEW site of the identical bug class:
        // syscall_stub_z_7 (output/syscall_stub_z_7_0x174cb0.cpp) does
        // `ctx->pc = GPR_U32(ctx,31); return;` at its own resume label 0x174cb8
        // with no stack slot at all -- when that dispatch is a fresh top-level
        // lookupFunction(0x174cb8) (not a live nested call), $ra comes from
        // whatever is CURRENTLY in the live register, same "resumed at an
        // interior label outside a legitimate resume" shape as parts 37-39, just
        // read from a GPR instead of an RDRAM stack slot. syscall_stub_z_7 has
        // 20 static callers (grep over output/), so -- like part 40's
        // 0x177eb0/0x178be8 -- there is no single invariant fallback literal to
        // derive via eeref.py the way part 39 did for sub_17CF50's unique
        // caller. Rather than hand-write a 5th bespoke if-block per future
        // occurrence, this is a small reusable table: each entry names a resume
        // label and whether its dispatch-time "$ra" lives in an RDRAM stack slot
        // (offset from $sp, parts 37/39's shape) or directly in the live
        // register (this site's shape). A per-label cache AUTO-LEARNS the last
        // observed nonzero value from healthy dispatches -- no static
        // single-caller proof needed up front, at the cost of being unable to
        // heal the very first occurrence before any healthy sample was ever
        // seen. The two already-proven, single-caller-derived fixes above
        // (0x178aecu, 0x17cfa4u) are deliberately left as-is, untouched, to
        // avoid any risk of double-patching or regressing a validated fix --
        // this table only carries the NEW site for now; add a row here instead
        // of a new hardcoded block when the next one turns up.
        {
            enum class ZeroRaGuardKind : uint8_t { Slot, Live };
            struct ZeroRaGuard
            {
                uint32_t resumeLabel;
                ZeroRaGuardKind kind;
                uint32_t slotOffset; // only meaningful for Kind::Slot
            };
            static constexpr ZeroRaGuard kZeroRaGuards[] = {
                {0x174cb8u, ZeroRaGuardKind::Live, 0u}, // syscall_stub_z_7 resume
            };
            static std::unordered_map<uint32_t, uint32_t> s_lastGoodRa;
            static std::atomic<uint32_t> s_genericLogs{0u};
            static std::atomic<uint32_t> s_genericFixes{0u};

            for (const ZeroRaGuard &guard : kZeroRaGuards)
            {
                if (context.pc != guard.resumeLabel)
                {
                    continue;
                }
                const uint32_t sp = GPR_U32((&context), 29);
                const bool isSlot = guard.kind == ZeroRaGuardKind::Slot;
                const uint32_t observed = isSlot
                    ? Ps2FastRead32(m_rdram, sp + guard.slotOffset)
                    : GPR_U32((&context), 31);

                if (observed != 0u)
                {
                    s_lastGoodRa[guard.resumeLabel] = observed;
                    continue;
                }

                const auto learned = s_lastGoodRa.find(guard.resumeLabel);
                const uint32_t n = s_genericLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u)
                {
                    std::cerr << "[semwatch:genericzeroguard] #" << n
                              << " tid=" << running->id
                              << " label=0x" << std::hex << guard.resumeLabel
                              << " kind=" << (isSlot ? "slot" : "live")
                              << " sp=0x" << sp
                              << " haveLearned=" << std::dec << (learned != s_lastGoodRa.end())
                              << std::dec
                              << " eeCycle=" << m_eeCycle
                              << std::endl;
                }

                if (learned == s_lastGoodRa.end())
                {
                    // Never observed a healthy dispatch at this label yet --
                    // nothing safe to heal with. Let it derail visibly rather
                    // than guess (per [[feedback_no_guessing]]).
                    continue;
                }

                if (isSlot)
                {
                    Ps2FastWrite32(m_rdram, sp + guard.slotOffset, learned->second);
                }
                else
                {
                    SET_GPR_U32((&context), 31, learned->second);
                }

                const uint32_t fn = s_genericFixes.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (fn <= 32u)
                {
                    std::cerr << "[workaround:genericzeroguard] #" << fn
                              << " tid=" << running->id
                              << " label=0x" << std::hex << guard.resumeLabel
                              << " patchedTo=0x" << learned->second
                              << std::dec
                              << " eeCycle=" << m_eeCycle
                              << std::endl;
                }
            }
        }

        if (context.pc == 0x178a08u)
        {
            static std::atomic<uint32_t> s_fillZ18EntryLogs{0u};
            const uint32_t n = s_fillZ18EntryLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u)
            {
                std::cerr << "[semwatch:fillz18entry] #" << n
                          << " tid=" << running->id
                          << " ra=0x" << std::hex << GPR_U32((&context), 31)
                          << " sp=0x" << GPR_U32((&context), 29)
                          << " a0=0x" << GPR_U32((&context), 4)
                          << std::dec
                          << " eeCycle=" << m_eeCycle
                          << std::endl;
            }
        }

        // 2026-08-29 -- [schedwatch] testing the hypothesis that the EE
        // scheduler's dispatch loop can get stuck perpetually `continue`-ing
        // here without ever calling `function()`: m_checkpointPending only
        // clears when processPendingEvents() recomputes cycleEventDue as
        // false, which compares m_eeCycle against m_nextDeadlineCycle -- but
        // m_eeCycle only advances via accountCycles(), which (as far as static
        // reading shows) is only reachable from inside a completed guest
        // function() call. If checkpointDue() keeps returning true here,
        // function() never runs, m_eeCycle never advances, and if
        // m_nextDeadlineCycle was already <= m_eeCycle when this started, the
        // deadline looks "due" forever -- a self-sustaining skip loop that
        // would explain the observed busy%=0/frozen-pc for 196s after
        // sub_178068's dispatch chain got preempted with ctx->pc=0x175090
        // (see PS2_PROJECT_STATE.md part 22/23, [semwatch:dispexit]). Capped;
        // if this saturates the cap that itself is strong evidence of a tight
        // skip loop (host CPU would be spinning, not blocked).
        if (checkpointDue(kGuestDispatchCycles))
        {
            static uint32_t s_schedSkipDumps = 0;
            if (s_schedSkipDumps < 40u)
            {
                ++s_schedSkipDumps;
                std::cerr << "[schedwatch:skip] #" << s_schedSkipDumps
                    << " pc=0x" << std::hex << context.pc
                    << " tid=" << std::dec << (running ? running->id : -1)
                    << " eeCycle=" << m_eeCycle
                    << " nextDeadline=" << m_nextDeadlineCycle.load(std::memory_order_relaxed)
                    << " sliceEnd=" << m_sliceEndCycle
                    << " checkpointPending=" << m_checkpointPending.load(std::memory_order_relaxed)
                    << " reschedReq=" << m_rescheduleRequested
                    << std::endl;
            }
            continue;
        }

        const auto dispatchStart = std::chrono::steady_clock::now();
        try
        {
            // See hostInvocationMutex()'s comment: excludes GS.cpp's
            // dispatchGsSyncVCallback (run directly on the IRQ worker thread)
            // for the duration of this thread's own guest-code invocation.
            std::lock_guard<std::mutex> hostLock(m_hostInvocationMutex);
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            if (context.pc == 0x175090u)
            {
                std::cerr << "[schedwatch:dispatch175090] tid=" << running->id
                    << " eeCycle=" << m_eeCycle << std::endl;
            }
            m_guestExecuting.store(true, std::memory_order_release);
            function(m_rdram, &context, &m_runtime);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            g_guest_busy_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatchStart).count()), std::memory_order_relaxed);
            g_guest_resumes.fetch_add(1, std::memory_order_relaxed);
            // 2026-08-29 -- [schedwatch] follow-up #2. Neither schedwatch:skip
            // nor schedwatch:dispatch175090 fired in a full 200s run, ruling
            // out both the outer-gate skip-loop hypothesis AND a repeat outer
            // dispatch at pc=0x175090. But sub_178068 (fn_178068_0x178068.cpp)
            // calls syscall_stub_z_24 via a DIRECT in-body call
            // (fn_175090_0x175090(...), not through this loop's function()),
            // so its own internal preemption check at that call site would
            // leave THIS function() call returning early with ctx->pc left at
            // 0x175090 -- checked here, on the return side, not the dispatch
            // side. checkpointDue() has two return-true branches
            // (m_checkpointPending/m_stopRequested at line ~576, and
            // cycle-deadline-due at line ~596) that do NOT set
            // m_rescheduleRequested, vs. only the priority-preempt branch
            // (line ~608) that does. If sub_178068's call-site check hit one
            // of the first two, the post-call reschedule guard below (`if
            // (m_rescheduleRequested && ...)`) would skip enqueueReady() and
            // leave m_currentThreadId pinned to this thread WITHOUT putting
            // it in the ready queue -- a genuine orphaned-thread bug
            // candidate, distinct from blockCurrent()'s legitimate semaphore
            // path (which self-resets m_currentThreadId=0 directly and does
            // NOT go through this guard at all). Also logs running->id (guest
            // thread id) to cross-check against [semwatch:block] threadId=1
            // from the prior run -- unknown until now whether sub_178068's
            // thread and the WaitSema(4)-blocked thread are even the same
            // guest thread. Capped at 20; unconditional (not gated on the
            // outer checkpointDue skip-continue, since that path returned
            // zero records last run).
            if (context.pc == 0x175090u)
            {
                static uint32_t s_schedRetDumps = 0;
                if (s_schedRetDumps < 20u)
                {
                    ++s_schedRetDumps;
                    std::cerr << "[schedwatch:funcret175090] #" << s_schedRetDumps
                        << " tid=" << running->id
                        << " eeCycle=" << m_eeCycle
                        << " reschedReq(pre)=" << m_rescheduleRequested
                        << " currentThreadId(pre)=" << m_currentThreadId
                        << std::endl;
                }
            }
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            g_guest_busy_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatchStart).count()), std::memory_order_relaxed);
            g_guest_resumes.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if ((++tls_progress_backedge_counter & 127u) == 0u)
    {
        g_guest_progress.fetch_add(1, std::memory_order_relaxed);
    }

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    // EIE gate: see the file-scope comment above ps2x_guest_intr_disable_enter().
    // Terminate/stop (above) always wins; everything below this point is a
    // voluntary yield (interrupt delivery, reschedule) that the gate withholds
    // while the guest holds interrupts disabled.
    if (tls_intr_disabled)
    {
        if (++tls_intr_disable_samples < kIntrDisableYieldEscape)
        {
            return false;
        }
        g_intr_disable_escapes.fetch_add(1, std::memory_order_relaxed);
        tls_intr_disable_samples = 0u;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    m_eeCycle += std::max<uint64_t>(1u, cycles);
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < eeMinGuestPriority() || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

// 2026-09-06 part 87 -- read by the 1 Hz WATCH sampler in ps2_runtime.cpp.
// Host-side scheduler state has no guest address, so it cannot ride
// PS2X_TRACE_WATCH's address list; this is the seam. Order is fixed and the
// caller passes its own capacity, so adding a field later cannot corrupt a
// stale caller.
extern "C" void ps2x_sched_diag(uint64_t *out, int n)
{
    if (out == nullptr || n <= 0)
    {
        return;
    }
    const uint64_t v[] = {
        g_sleepFast,
        g_sleepBlocked,
        g_wakeAcc[6],
        g_wakeReady[6],
        static_cast<uint64_t>(g_wakeCountLast[6]),
        static_cast<uint64_t>(g_sleepCountLast[6]),
        g_wakeAcc[1],
        g_wakeReady[1],
    };
    const int have = static_cast<int>(sizeof(v) / sizeof(v[0]));
    for (int i = 0; i < n; ++i)
    {
        out[i] = (i < have) ? v[i] : 0u;
    }
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (m_currentThreadId > 0 && m_currentThreadId < kDispatchMaxId)
    {
        g_sleepCountLast[m_currentThreadId] = static_cast<uint32_t>(self->wakeupCount);
    }
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        ++g_sleepFast;
        maybeEmitDispatch();
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    ++g_sleepBlocked;
    maybeEmitDispatch();
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
        if (id > 0 && id < kDispatchMaxId)
        {
            ++g_wakeReady[id];
        }
    }
    else
    {
        ++target->wakeupCount;
        if (id > 0 && id < kDispatchMaxId)
        {
            ++g_wakeAcc[id];
            g_wakeCountLast[id] = static_cast<uint32_t>(target->wakeupCount);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < eeMinGuestPriority() || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::yieldIfHigherPriorityReady(bool interruptSafe)
{
    assertExecutor();
    const GuestThread *self = currentThread();
    if (self != nullptr && hasReadyAtOrAbovePriority(self->currentPriority))
    {
        m_rescheduleRequested = true;
    }
    transferIfRequested(interruptSafe);
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    std::cerr << "[semwatch:signal] id=" << id << " interruptSafe=" << interruptSafe
              << " found=" << (object != nullptr)
              << " waiters=" << (object ? object->waiters.size() : 0u)
              << " count=" << (object ? object->count : -1) << std::endl;
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    std::cerr << "[semwatch:wait] id=" << id << " found=" << (object != nullptr)
              << " count=" << (object ? object->count : -1) << std::endl;
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    std::cerr << "[semwatch:block] id=" << id << " threadId=" << self->id << std::endl;
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    // 2026-08-30 part 33 -- [semwatch:invokexthread]: assertExecutor() below
    // is a plain assert(), compiled out in this RelWithDebInfo build, so a
    // call to invokeCurrent() from any thread other than m_executorThread
    // would silently push onto owner->invocations (not proven thread-safe)
    // and then throw EeDispatcherTransfer{} on the WRONG thread's stack --
    // only EeScheduler::run() (on m_executorThread) catches that type, so
    // the throw would propagate past whatever called in, never resuming
    // dispatch, and the pushed invocation would sit forever unconsumed. The
    // soHandler=0x17ee80 syscall-override invocation (soBranch=4 confirmed)
    // never gets its ctx->pc==0 completion observed by run()'s dispatch
    // loop (semwatch:zeropc stayed at 0 hits, lastCall pinned at 0x174cb0
    // for a full 197s run) even though fn_17EE80_0x17ee80 correctly sets
    // ctx->pc = $ra on return -- this logs the actual calling thread
    // against the scheduler's own owning thread to settle it directly.
    // Capped; unconditional read only.
    {
        static std::atomic<uint32_t> s_invokeXThreadLogs{0u};
        const uint32_t n = s_invokeXThreadLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 32u)
        {
            const bool sameThread = (m_executorThread == std::this_thread::get_id());
            std::cerr << "[semwatch:invokexthread] #" << n
                      << " kind=" << static_cast<int>(invocation.kind)
                      << " tag=0x" << std::hex << invocation.tag << std::dec
                      << " sameThread=" << sameThread
                      << " callerTid=" << std::hash<std::thread::id>{}(std::this_thread::get_id())
                      << " execTid=" << std::hash<std::thread::id>{}(m_executorThread)
                      << std::endl;
        }
    }
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u ||
        eeAsyncNeedsOwnStack(invocation.kind))
    {
        if (getRegU32(&invocation.context, 29) != 0u) { ++g_eeOwnStackForced; }
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    {
        const uint32_t invPc = invocation.context.pc;
        const uint32_t invSp = getRegU32(&invocation.context, 29);
        const uint8_t invKind = static_cast<uint8_t>(invocation.kind);
        owner->invocations.push_back(std::move(invocation));
        eeRecordInv(EeInvKind::PushInvoke, owner->id, invPc, invSp,
                    owner->invocations.size(), m_eeCycle, invKind);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u ||
            eeAsyncNeedsOwnStack(it->kind))
        {
            if (getRegU32(&it->context, 29) != 0u) { ++g_eeOwnStackForced; }
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        {
            const uint32_t invPc = it->context.pc;
            const uint32_t invSp = getRegU32(&it->context, 29);
            const uint8_t invKind = static_cast<uint8_t>(it->kind);
            owner->invocations.push_back(std::move(*it));
            eeRecordInv(EeInvKind::PushSequence, owner->id, invPc, invSp,
                        owner->invocations.size(), m_eeCycle, invKind);
        }
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, handler.sp);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

// 2026-09-01 part 47 -- see the header. PS2X_PSEUDO_TID_HIDE=0 reverts to the
// old behaviour (leak the negative pseudo-thread id) for A/B.
int EeScheduler::guestVisibleThreadId() const noexcept
{
    static const bool hide = []()
    {
        const char *e = std::getenv("PS2X_PSEUDO_TID_HIDE");
        return (e == nullptr) || (*e != '0');
    }();
    if (!hide || m_currentThreadId >= 0)
    {
        return m_currentThreadId;
    }
    return m_lastRealThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

bool EeScheduler::isIdle() const
{
    const EeKernelSnapshot snap = snapshot();
    if (snap.runningThreadId != 0)
    {
        return false;
    }
    for (const EeThreadSnapshot &t : snap.threads)
    {
        if (t.status == EeThreadStatus::Ready)
        {
            return false;
        }
    }
    return true;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        snapshot.pc = item.activeContext().pc;
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    probeDispatch(item.id);
    if (item.id >= 0) { m_lastRealThreadId = item.id; }
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    // 2026-08-30 part 34 -- [semwatch:blockcurrent]: invokeCurrent() cross-OS-
    // thread hypothesis is FALSIFIED ([semwatch:invokexthread] showed
    // sameThread=1 for all 5 hits this run, both tag=0x83 soHandler=0x17ee80
    // x2 and tag=0x5a kernel-memcpy x3). activeContext() correctly returns
    // invocations.back().context once pushed (ee_scheduler.h:126-129), so a
    // pushed invocation should get dispatched on the very next loop
    // iteration -- yet lastCall stays pinned at 0x174cb0 for the whole run
    // and [semwatch:zeropc] never fires. The remaining path that stops all
    // future dispatch without a crash is run()'s outer loop (line ~312-320):
    // if m_currentThreadId hits 0 with selectReady()==nullptr and
    // m_pendingInvocations empty, it calls waitForEvent(), which -- when
    // m_deadlines is empty -- blocks indefinitely on m_eventCv (line ~2374),
    // woken only by postEvent()/scheduleEvent(). That matches busy%=0 (a
    // blocked host thread, not a spin loop) with zero crash and zero further
    // dispatch. blockCurrent() is the only place (besides preemption, which
    // re-enqueues Ready, not Waiting) that sends a thread into a genuine
    // wait -- logging the reason/id here for the LAST calls before the
    // freeze names exactly what the guest thread is stuck on. Capped;
    // unconditional read only.
    {
        static std::atomic<uint32_t> s_blockLogs{0u};
        const uint32_t n = s_blockLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 32u)
        {
            int waitId = -1;
            if (wait.reason == EeWaitReason::Semaphore)
                waitId = std::get<EeSemaphoreWait>(wait.payload).id;
            else if (wait.reason == EeWaitReason::EventFlag)
                waitId = std::get<EeEventFlagWait>(wait.payload).id;
            std::cerr << "[semwatch:blockcurrent] #" << n
                      << " tid=" << self->id
                      << " reason=" << static_cast<int>(wait.reason)
                      << " waitId=" << waitId
                      << " invocationsSize=" << self->invocations.size()
                      << " pc=0x" << std::hex << self->activeContext().pc << std::dec
                      << " eeCycle=" << m_eeCycle
                      << std::endl;
        }
    }
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        dispatchIrq(true, event.id);
        break;
    case EeEventType::Intc:
        // SDBZ: posted by drainPendingIntc() for causes other than vblank
        // (kIntcVblankStart/End are delivered directly via VBlankStart/End
        // above). event.id = cause.
        dispatchIrq(false, event.id);
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, alarm.sp);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    if (m_deadlines.empty())
    {
        // 2026-08-30 part 34 -- [semwatch:waitforever]: companion to
        // [semwatch:blockcurrent]. This is the only unconditional,
        // no-timeout block in the scheduler; if it's entered right before
        // the freeze and no "woke" line ever follows, the host thread is
        // parked here for good -- explaining busy%=0 with zero crash and
        // zero further dispatch. Capped; unconditional read only.
        {
            static std::atomic<uint32_t> s_waitForeverLogs{0u};
            const uint32_t n = s_waitForeverLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u)
            {
                std::cerr << "[semwatch:waitforever] #" << n << " entering eeCycle=" << m_eeCycle << std::endl;
            }
            m_eventCv.wait(lock, [this]()
                           { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
            if (n <= 32u)
            {
                std::cerr << "[semwatch:waitforever] #" << n << " woke eeCycle=" << m_eeCycle << std::endl;
            }
        }
        return;
    }

    const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                       [](const ScheduledEvent &left, const ScheduledEvent &right)
                                       {
                                           if (left.deadlineCycle != right.deadlineCycle)
                                           {
                                               return left.deadlineCycle < right.deadlineCycle;
                                           }
                                           return left.sequence < right.sequence;
                                       });
    const uint64_t deadlineCycle = next->deadlineCycle;
    const auto hostDeadline = next->hostDeadline;
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        m_eeCycle = std::max(m_eeCycle, deadlineCycle);
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}
