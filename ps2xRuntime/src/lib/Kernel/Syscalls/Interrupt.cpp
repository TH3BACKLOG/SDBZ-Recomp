#include "Common.h"
#include "Interrupt.h"
#include "ps2_log.h"
#include "Stubs/GS.h"
#include "ps2_fiber.h"
#include "runtime/ee_scheduler.h"

#include <bit>
#include <cstdlib> // std::getenv / std::strtoull for the determinism knobs below

// Cross-.cpp externs (no header edit: a .h change forces a full rebuild of all
// 30,000+ generated runner TUs). Defined in ps2_scheduler.cpp and
// game_overrides.cpp respectively. The depth counter is thread_local, so on the
// IRQ worker thread it always reads 0 -- only the inline, on-guest-fiber dispatch
// path can ever observe a non-zero value, which is exactly the case being probed.
extern "C" uint32_t ps2x_guest_intr_disable_depth();
extern "C" void ps2x_probe_kv(const char *name, int n,
                              const char *const *keys, const uint64_t *vals);

// Set while this fiber is executing an IRQ handler body on a borrowed stack
// (GuestScratchStack out of the guest heap, or the async callback pool) rather
// than on its own thread stack. Read by ps2x_stack_check() in Thread.cpp, which
// otherwise reports every such handler as a stack-bounds violation for whichever
// EE thread happened to own the fiber -- run 53 produced four STACKOOB records
// that way (sp=0x1f00000, i.e. the guest-heap limit, blamed on thread 0x4) and
// they were carried into the handoff as a suspected context bleed. Thread-local
// and nesting-safe: handlers in one dispatch run sequentially, but a handler can
// yield at a back-edge and another fiber can dispatch inline on top of it.
static thread_local uint32_t tls_on_irq_handler_stack = 0u;

extern "C" uint32_t ps2x_on_irq_handler_stack()
{
    return tls_on_irq_handler_stack;
}

namespace
{
    struct IrqHandlerStackScope
    {
        IrqHandlerStackScope() { ++tls_on_irq_handler_stack; }
        ~IrqHandlerStackScope() { --tls_on_irq_handler_stack; }
        IrqHandlerStackScope(const IrqHandlerStackScope &)            = delete;
        IrqHandlerStackScope &operator=(const IrqHandlerStackScope &) = delete;
    };
}

namespace ps2_syscalls
{
    namespace
    {
        constexpr uint32_t kIntcVblankStart = 2u;
        constexpr uint32_t kIntcVblankEnd = 3u;
        constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
        constexpr int kMaxCatchupTicks = 4;

        // SDBZ: worker-thread lifecycle for interruptWorkerMain (determinism-
        // quantum vblank pacing, Stage 5.17). Handler REGISTRATION and the
        // vsync flag/tick/waitlist state now live inside EeScheduler
        // (m_intcHandlers/m_dmacHandlers/m_vsyncTick/EeVSyncWait) - only the
        // pending-INTC bookkeeping and the worker thread itself stay here.
        std::mutex g_irq_worker_mutex;
        std::condition_variable g_irq_worker_cv;
        std::atomic<bool> g_irq_worker_stop{false};
        std::atomic<bool> g_irq_worker_running{false};
        std::thread g_irq_worker_thread; // joinable worker handle so stopInterruptWorker() can join it

        std::atomic<uint32_t> g_pending_intc_causes{0u};              // bitmask, one pending bit per cause
        std::atomic<uint32_t> g_pending_intc_age[32] = {};            // drain ticks since raise, per cause
        // The age entries are atomic because raisePendingIntc (any thread) resets
        // an age while the interrupt worker thread increments it in drainPendingIntc.

        EeScheduler &scheduler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            EeScheduler &result = runtime->eeScheduler();
            result.bindMainContextForSyscall(*ctx, rdram);
            return result;
        }
    }

    using namespace interrupt_state;

    static void writeGuestU32NoThrow(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static void writeGuestU64NoThrow(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static uint32_t readGuestU32NoThrow(uint8_t *rdram, uint32_t addr)
    {
        if (addr == 0u)
        {
            return 0u;
        }

        uint8_t *src = getMemPtr(rdram, addr);
        if (!src)
        {
            return 0u;
        }

        uint32_t value = 0u;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }

        void setCauseEnabled(uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime,
                             bool dmac,
                             bool enabled)
        {
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .setIrqCauseEnabled(dmac, getRegU32(ctx, 4), enabled));
        }

        void addHandler(uint8_t *rdram,
                        R5900Context *ctx,
                        PS2Runtime *runtime,
                        bool dmac)
        {
            const int id = scheduler(rdram, ctx, runtime)
                               .addIrqHandler(dmac,
                                              getRegU32(ctx, 4),
                                              getRegU32(ctx, 5),
                                              getRegU32(ctx, 6) != 0u,
                                              getRegU32(ctx, 7),
                                              getRegU32(ctx, 28),
                                              getRegU32(ctx, 29));
            setReturnS32(ctx, id);
        }

        // 2026-07-26 -- guest interrupt-mask visibility (see the CRITSEC block in
        // game_overrides.cpp and the gate in ps2_scheduler.cpp).
        //
        // 2026-08-12 -- run 53 fired this four times (causes 0x1 and 0x5): a DMAC
        // dispatch reached from sceSifSetDma / sceDmaSend / drainCompletedDmacHandlers
        // runs INLINE on the calling fiber while the guest holds DisableIntr, since
        // that path is a direct nested call, not a preemption. It was probe-only in
        // the old real-thread dispatcher (DEFERINL/ps2x_probe_kv) and never actually
        // gated; that probe is retired here since the dispatch behavior itself
        // (run inline regardless) is unchanged under EeScheduler::dispatchIrq -- see
        // [[project_upstream_full_catchup_plan]] Phase 3c-2 if this scenario needs
        // fresh diagnostic visibility again.

    void raisePendingIntc(uint32_t cause)
    {
        if (cause >= 32u || cause == kIntcVblankStart || cause == kIntcVblankEnd)
        {
            return;
        }

        const uint32_t bit = 1u << cause;
        const uint32_t prev = g_pending_intc_causes.fetch_or(bit, std::memory_order_acq_rel);
        if ((prev & bit) == 0u)
        {
            g_pending_intc_age[cause].store(0u, std::memory_order_relaxed); // fresh raise: age restarts
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_raiseLogCount{0u};
                const uint32_t logIndex = s_raiseLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 16u || (logIndex % 256u) == 0u)
                {
                    RUNTIME_LOG("[INTC:raise] cause=" << cause);
                }
            });
        }
    }

    void drainPendingIntc(uint8_t *rdram, PS2Runtime *runtime)
    {
        // dispatchIrq() (called from EeScheduler::processEvent, on the executor
        // thread) asserts executor-thread-only and already no-ops gracefully for
        // a cause with zero enabled handlers, so the old "count eligible
        // handlers first, age out otherwise" split is no longer needed for
        // correctness -- every pending cause is posted, thread-safely, and its
        // bit cleared immediately. See [[project_upstream_full_catchup_plan]]
        // Phase 3c-2: this trades the old "[INTC:drop] aged out with no
        // registered/enabled handler" diagnostic for simplicity, since EeScheduler
        // now silently absorbs that case identically either way.
        (void)rdram;
        uint32_t pending = g_pending_intc_causes.exchange(0u, std::memory_order_acq_rel);
        while (pending != 0u)
        {
            const uint32_t cause = static_cast<uint32_t>(std::countr_zero(pending));
            pending &= ~(1u << cause);
            g_pending_intc_age[cause].store(0u, std::memory_order_relaxed);
            runtime->eeScheduler().postEvent(EeEvent{EeEventType::Intc, cause, 0});
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_deliverLogCount{0u};
                const uint32_t logIndex = s_deliverLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 16u || (logIndex % 256u) == 0u)
                {
                    RUNTIME_LOG("[INTC:deliver] cause=" << cause);
                }
            });
        }
    }

    void resetInterruptHandlerState()
    {
        // Handler registration/mask state now lives inside EeScheduler
        // (m_intcHandlers/m_dmacHandlers/m_enabledIntcMask/m_enabledDmacMask) -
        // only the SDBZ pending-INTC bookkeeping is reset here.
        g_pending_intc_causes.store(0u, std::memory_order_release);
        for (auto &age : g_pending_intc_age)
        {
            age.store(0u, std::memory_order_relaxed);
        }
    }

        void removeHandler(uint8_t *rdram,
                           R5900Context *ctx,
                           PS2Runtime *runtime,
                           bool dmac)
        {
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .removeIrqHandler(dmac,
                                               getRegU32(ctx, 4),
                                               static_cast<int>(getRegU32(ctx, 5))));
        }

        void setHandlerEnabled(uint8_t *rdram,
                               R5900Context *ctx,
                               PS2Runtime *runtime,
                               bool dmac,
                               bool enabled)
        {
            setReturnS32(ctx,
                         scheduler(rdram, ctx, runtime)
                             .setIrqHandlerEnabled(dmac,
                                                   static_cast<int>(getRegU32(ctx, 5)),
                                                   enabled));
        }

    void dispatchDmacHandlersForCause(uint8_t *, PS2Runtime *runtime, uint32_t cause)
    {
        runtime->eeScheduler().dispatchIrq(true, cause);
    }

    uint64_t GetCurrentVSyncTick(PS2Runtime *runtime)
    {
        return runtime->eeScheduler().currentVSyncTick();
    }

    // signalVSyncFlag (g_vsync_registration flag/tick write + wakeWaiters) is
    // retired: EeScheduler::processEvent(VBlankStart) now does this internally
    // (writeGuestU32/64 to m_vsyncFlagAddress/m_vsyncTickAddress, set via
    // SetVSyncFlag -> scheduler().setVSyncFlag(), plus completeVSync() to wake
    // EeVSyncWait parkers) when interruptWorkerMain posts the tick below.
    // WaitVSyncTick itself is defined further down, alongside SetVSyncFlag.
    // See [[project_upstream_full_catchup_plan]] Phase 3c-2.

    // --- Deterministic vblank pacing (Phase A) -----------------------------
    // Defined in ps2_scheduler.cpp. Declared here rather than in
    // ps2_scheduler.h because touching that header forces a full rebuild of the
    // ~30,000 generated runner translation units (§3 prohibition).
    //
    // ps2x_guest_progress() advances once per 128 guest back-edges, so it is a
    // clock measured in guest execution rather than wall time. Pacing vblank off
    // it makes interrupt delivery land at a fixed point in the guest instruction
    // stream, which is what makes a run reproducible.
    extern "C" uint64_t ps2x_guest_progress();
    extern "C" int ps2x_determinism_enabled();

    // ps2x_guest_idle() -> 1 when the scheduler has no runnable guest thread
    // and none running. This is the condition the deterministic vblank pacer
    // actually needs: a vsync-gated guest stops retiring back-edges BECAUSE it
    // is parked waiting for the tick, so pacing purely off progress starves the
    // very interrupt that would unpark it (stage 5.17). Same no-header rule.
    extern "C" int ps2x_guest_idle();

    // Delivered vblank ticks (stage 5.6.2). gif/s ~3 against an expected ~60
    // has two readings -- the guest renders one frame per ~20 vblanks, or the
    // vblank tick itself is not arriving at 60Hz -- and nothing measured so far
    // separates them. Counted at the point of delivery, after the pacing wait,
    // so it reflects ticks the guest actually saw. Read by the watchdog via a
    // local extern "C" (no header change; §3 prohibition).
    static std::atomic<uint64_t> g_vblankTicks{0};

    // Which pacing rule produced each tick (stage 5.17). Without this split,
    // "vbl/s=3" is the same number whether the quantum path is firing slowly or
    // not firing at all and the stall fallback is carrying the whole run -- two
    // states that call for opposite fixes. Read by the watchdog via a local
    // extern "C" (no header change).
    static std::atomic<uint64_t> g_vblankFromQuantum{0};
    static std::atomic<uint64_t> g_vblankFromIdle{0};
    static std::atomic<uint64_t> g_vblankFromStall{0};

    extern "C" void ps2x_vblank_tick_sources(uint64_t *quantum, uint64_t *idle,
                                             uint64_t *stall)
    {
        if (quantum) *quantum = g_vblankFromQuantum.load(std::memory_order_relaxed);
        if (idle) *idle = g_vblankFromIdle.load(std::memory_order_relaxed);
        if (stall) *stall = g_vblankFromStall.load(std::memory_order_relaxed);
    }

    // Defined inside ps2_syscalls deliberately: extern "C" gives the symbol C
    // language linkage, so the enclosing namespace does not decorate its name
    // and ps2_runtime.cpp can declare it locally without a header change.
    extern "C" uint64_t ps2x_vblank_ticks()
    {
        return g_vblankTicks.load(std::memory_order_relaxed);
    }

    // Guest-progress ticks per vblank under PS2X_DETERMINISM. There is no
    // principled value here -- the real ratio depends on how many back-edges the
    // guest retires per frame, which varies by workload -- so it is tunable via
    // PS2X_DET_VBLANK_QUANTUM and the effective value is logged once at startup.
    static uint64_t deterministicVblankQuantum()
    {
        static const uint64_t q = []() -> uint64_t
        {
            const char *e = std::getenv("PS2X_DET_VBLANK_QUANTUM");
            const uint64_t parsed = (e && *e) ? std::strtoull(e, nullptr, 0) : 0ull;
            return (parsed != 0ull) ? parsed : 20000ull;
        }();
        return q;
    }

    static void interruptWorkerMain(uint8_t *rdram, PS2Runtime *runtime)
    {
        g_currentThreadId = -1;

        using clock = std::chrono::steady_clock;
        auto nextTick = clock::now() + kVblankPeriod;

        const bool deterministic = (ps2x_determinism_enabled() != 0);
        const uint64_t quantum = deterministicVblankQuantum();
        uint64_t nextProgressTick = ps2x_guest_progress() + quantum;
        uint64_t lastProgressSeen = ps2x_guest_progress();
        int stalledPolls = 0;
        // Poll cadence and stall threshold for the deterministic path. The stall
        // fallback exists because the guest can legitimately stop retiring
        // back-edges precisely BECAUSE it is waiting for the vblank we are about
        // to deliver (the 0x175210 INTC_STAT spin parks its fiber). Without a
        // fallback that is a deadlock: vblank waits on progress, progress waits
        // on vblank. Delivering on stall does not reintroduce interleaving
        // non-determinism, because a stalled guest has no instruction stream for
        // the interrupt to interleave with.
        constexpr auto kDetPollPeriod = std::chrono::microseconds(250);
        constexpr int kDetStallPolls = 200; // ~50 ms of no guest progress

        // Idle-tick arming (stage 5.17). An idle guest would otherwise take a
        // tick every poll (4 kHz), so a delivered idle tick disarms itself and
        // only re-arms once the guest has demonstrably acted on it -- either it
        // retired more back-edges, or a fiber became runnable. Vblank rate then
        // equals the rate the guest can consume frames, which is the intended
        // semantics, and it stays a pure function of guest state (no wall
        // clock), so the determinism argument is the same one the stall
        // fallback already relies on.
        bool idleTickArmed = true;
        uint64_t progressAtIdleTick = 0;

        if (deterministic)
        {
            // std::cerr, not RUNTIME_LOG: RUNTIME_LOG compiles to nothing unless
            // PS2_RUNTIME_LOGS is on, and this line must always be present so a
            // run's log states unambiguously whether determinism was active.
            std::cerr << "[determinism] vblank paced by guest progress, quantum="
                      << quantum << " ticks" << std::endl;
        }

        while (runtime != nullptr && !runtime->isStopRequested())
        {
            int ticksToProcess = 0;

            if (deterministic)
            {
                {
                    std::unique_lock<std::mutex> lock(g_irq_worker_mutex);
                    if (g_irq_worker_cv.wait_for(lock, kDetPollPeriod, []()
                                                 { return g_irq_worker_stop.load(std::memory_order_acquire); }))
                    {
                        break;
                    }
                }

                const uint64_t progress = ps2x_guest_progress();
                if (progress != lastProgressSeen)
                {
                    lastProgressSeen = progress;
                    stalledPolls = 0;
                }
                else
                {
                    ++stalledPolls;
                }

                if (progress >= nextProgressTick)
                {
                    // Catch up in whole quanta, capped like the wall-clock path.
                    while (progress >= nextProgressTick && ticksToProcess < kMaxCatchupTicks)
                    {
                        ++ticksToProcess;
                        nextProgressTick += quantum;
                    }
                    // If the guest outran the catch-up cap, resynchronise rather
                    // than accumulating a debt we would burn down over later
                    // frames (that debt would itself be a source of drift).
                    if (progress >= nextProgressTick)
                    {
                        nextProgressTick = progress + quantum;
                    }
                    g_vblankFromQuantum.fetch_add(static_cast<uint64_t>(ticksToProcess),
                                                  std::memory_order_relaxed);
                }
                else if (stalledPolls >= kDetStallPolls)
                {
                    // Last-resort escape: guest frozen outright, nobody to wake.
                    ticksToProcess = 1;
                    stalledPolls = 0;
                    nextProgressTick = progress + quantum;
                    g_vblankFromStall.fetch_add(1, std::memory_order_relaxed);
                }

                // Idle delivery. Checked after the quantum path so a compute-
                // bound guest is unaffected: it is never idle for long enough to
                // matter, and when it is, this only fills gaps the quantum path
                // left. Re-arm on either signal that the guest reacted.
                const int idle = ps2x_guest_idle();
                if (!idleTickArmed && (idle == 0 || progress != progressAtIdleTick))
                {
                    idleTickArmed = true;
                }
                if (ticksToProcess == 0 && idle != 0 && idleTickArmed)
                {
                    ticksToProcess = 1;
                    idleTickArmed = false;
                    progressAtIdleTick = progress;
                    stalledPolls = 0;
                    // Resync the quantum deadline: the guest was parked, so the
                    // progress it did not make is not a debt to burn down later.
                    nextProgressTick = progress + quantum;
                    g_vblankFromIdle.fetch_add(1, std::memory_order_relaxed);
                }

                if (ticksToProcess == 0)
                {
                    continue;
                }
            }
            else
            {
                {
                    std::unique_lock<std::mutex> lock(g_irq_worker_mutex);
                    if (g_irq_worker_cv.wait_until(lock, nextTick, []()
                                                   { return g_irq_worker_stop.load(std::memory_order_acquire); }))
                    {
                        break;
                    }
                }

                const auto now = clock::now();
                while (now >= nextTick && ticksToProcess < kMaxCatchupTicks)
                {
                    ++ticksToProcess;
                    nextTick += kVblankPeriod;
                }
                if (ticksToProcess == 0)
                {
                    continue;
                }
            }

            for (int i = 0; i < ticksToProcess; ++i)
            {
                // g_vblankTicks: already-atomic, already-monotonic - reused
                // below as the tick value for dispatchGsSyncVCallback instead
                // of EeScheduler's own m_vsyncTick, which is a plain (non-
                // atomic) uint64_t only safe to touch on the executor thread;
                // reading it here (a separate OS thread) while
                // processEvent(VBlankStart) concurrently increments it would
                // be a real data race.
                const uint64_t tickValue = g_vblankTicks.fetch_add(1, std::memory_order_relaxed) + 1u;

                // Post (thread-safe: interruptWorkerMain is a separate OS
                // thread, dispatchIrq()/completeVSync() assert executor-thread-
                // only) instead of delivering inline. EeScheduler's own
                // processEvent(VBlankStart/End) does everything the old inline
                // delivery did here: increments its own vsync tick, updates
                // GS csr/vsyncTick, writes the guest vsync flag/tick memory,
                // wakes EeVSyncWait parkers, and calls dispatchIrq(false,2)/
                // dispatchIrq(false,3) for the VBLANK-start/end INTC causes
                // (0x1000F000 bit2/bit3) that the game's 0x175210 spin polls.
                // ps2_stubs::dispatchGsSyncVCallback (SDBZ's own GS-sync-V
                // callback registration, kept in Phase 3b - separate from and
                // in addition to EeScheduler's own unused setGsVSyncCallback
                // path) still needs a tick value, so keep calling it directly
                // rather than through the queued event.
                runtime->eeScheduler().postEvent(EeEvent{EeEventType::VBlankStart, 0, 0});
                ps2_stubs::dispatchGsSyncVCallback(rdram, runtime, tickValue);

                // Gap between VBon and VBoff so the guest gets a chance to run
                // its VBon handler before VBoff is asserted. Off the wall clock
                // under determinism: wait on guest progress instead, with a
                // bounded poll count so a quiescent guest cannot wedge the tick.
                if (deterministic)
                {
                    const uint64_t gapTarget = ps2x_guest_progress() + 1ull;
                    for (int i = 0; i < 8 && ps2x_guest_progress() < gapTarget; ++i)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(250));
                    }
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }

                runtime->eeScheduler().postEvent(EeEvent{EeEventType::VBlankEnd, 0, 0});
                drainPendingIntc(rdram, runtime);
            }
        }

        g_irq_worker_running.store(false, std::memory_order_release);
        g_irq_worker_cv.notify_all();
    }

    static void ensureInterruptWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(g_irq_worker_mutex);
        if (g_irq_worker_running.load(std::memory_order_acquire))
        {
            return;
        }

        if (g_irq_worker_thread.joinable())
        {
            g_irq_worker_thread.join();
        }

        g_irq_worker_stop.store(false, std::memory_order_release);
        g_irq_worker_running.store(true, std::memory_order_release);
        try
        {
            g_irq_worker_thread = std::thread(interruptWorkerMain, rdram, runtime);
        }
        catch (...)
        {
            g_irq_worker_running.store(false, std::memory_order_release);
        }
    }

    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
        ensureInterruptWorkerRunning(rdram, runtime);
    }

    void signalInterruptWorkerStop()
    {
        // Signal-only: no join (see Interrupt.h). The worker observes the stop
        // flag on its next CV wait / loop check and exits; scheduler_shutdown()
        // performs the join on the main thread via stopInterruptWorker().
        g_irq_worker_stop.store(true, std::memory_order_release);
        g_irq_worker_cv.notify_all();
    }

    void stopInterruptWorker()
    {
        g_irq_worker_stop.store(true, std::memory_order_release);
        g_irq_worker_cv.notify_all();

        // Join the worker to a clean stop; it checks g_irq_worker_stop on both
        // its CV wait and its while-condition, so it exits promptly. We must
        // NOT hold g_irq_worker_mutex while joining (the worker takes that
        // mutex on its CV wait — joining under it would deadlock).
        std::thread workerToJoin;
        {
            std::lock_guard<std::mutex> lock(g_irq_worker_mutex);
            if (g_irq_worker_thread.joinable())
            {
                workerToJoin = std::move(g_irq_worker_thread);
            }
        }
        if (workerToJoin.joinable())
        {
            workerToJoin.join();
        }
        // Waking vsync waiters during shutdown is now EeScheduler's own
        // responsibility (PS2Runtime::requestStop() -> m_eeScheduler->
        // requestStop()); the old g_vsync_waitList this used to wake no
        // longer exists (WaitForNextVSyncTick's fiber-park mechanism is
        // superseded by EeScheduler's EeVSyncWait).
    }

    void WaitVSyncTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int fixedResult)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        ee.waitVSync(ee.currentVSyncTick(), fixedResult);
    }

    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // 2026-07-27: hardware-watchpoint diagnostic kept from the old
        // signalVSyncFlag path - logs every registration with the caller's
        // $ra so a dead-stack-address registration (the Stage 5.4.2 boot
        // derail class of bug) stays visible under the new EeScheduler-owned
        // delivery path too.
        {
            static uint64_t s_seq = 0;
            const uint32_t flagAddrForLog = getRegU32(ctx, 4);
            const char *k[5] = {"seq", "flag", "tick", "ra", "stack"};
            const uint64_t v[5] = {
                ++s_seq,
                flagAddrForLog,
                getRegU32(ctx, 5),
                getRegU32(ctx, 31),
                (flagAddrForLog >= 0x01000000u && flagAddrForLog < 0x02000000u) ? 1ull : 0ull};
            ps2x_probe_kv("VSYNCREG", 5, k, v);
        }

        const uint32_t flagAddress = getRegU32(ctx, 4);
        const uint32_t tickAddress = getRegU32(ctx, 5);
        if ((flagAddress != 0u && !getEeGuestStruct<uint32_t>(rdram, flagAddress)) ||
            (tickAddress != 0u && !getEeGuestStruct<uint64_t>(rdram, tickAddress)))
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        scheduler(rdram, ctx, runtime).setVSyncFlag(flagAddress, tickAddress);
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, false, true);
    }

    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableIntc(rdram, ctx, runtime);
    }

    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, false, false);
    }

    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableIntc(rdram, ctx, runtime);
    }

    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        addHandler(rdram, ctx, runtime, false);
    }

    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddIntcHandler(rdram, ctx, runtime);
    }

    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        removeHandler(rdram, ctx, runtime, false);
    }

    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        addHandler(rdram, ctx, runtime, true);
    }

    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddDmacHandler(rdram, ctx, runtime);
    }

    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        removeHandler(rdram, ctx, runtime, true);
    }

    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, false, true);
    }

    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, false, false);
    }

    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, true, true);
    }

    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setHandlerEnabled(rdram, ctx, runtime, true, false);
    }

    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, true, true);
    }

    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableDmac(rdram, ctx, runtime);
    }

    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setCauseEnabled(rdram, ctx, runtime, true, false);
    }

    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableDmac(rdram, ctx, runtime);
    }
}
