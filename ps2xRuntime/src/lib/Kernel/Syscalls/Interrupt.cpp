#include "Common.h"
#include "Interrupt.h"
#include "ps2_log.h"
#include "Stubs/GS.h"
#include "ps2_fiber.h"

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

namespace ps2_syscalls
{
    namespace interrupt_state
    {
        constexpr uint32_t kIntcVblankStart = 2u;
        constexpr uint32_t kIntcVblankEnd = 3u;
        constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
        constexpr int kMaxCatchupTicks = 4;

        std::mutex g_irq_handler_mutex;
        std::mutex g_irq_worker_mutex;
        std::condition_variable g_irq_worker_cv;
        std::mutex g_vsync_flag_mutex;
        std::vector<std::pair<int, ps2sched::FiberToken>> g_vsync_waitList;
        std::atomic<bool> g_irq_worker_stop{false};
        std::atomic<bool> g_irq_worker_running{false};
        std::thread g_irq_worker_thread; // joinable worker handle so stopInterruptWorker() can join it
        // See Interrupt.h: read from the IRQ worker thread without holding
        // g_irq_handler_mutex (the dispatch*HandlersForCause call sites read this
        // while evaluating a function argument), so it must be atomic rather than
        // mutex-protected like the rest of the handler bookkeeping.
        std::atomic<uint32_t> g_enabled_intc_mask{0xFFFFFFFFu};
        std::atomic<uint32_t> g_enabled_dmac_mask{0xFFFFFFFFu};
        constexpr uint32_t kAsyncHandlerStackSize = 0x4000u; // 16 KB, one pool slot
        uint64_t g_vsync_tick_counter = 0u;
        VSyncFlagRegistration g_vsync_registration{};

        std::atomic<uint32_t> g_pending_intc_causes{0u};              // bitmask, one pending bit per cause
        std::atomic<uint32_t> g_pending_intc_age[32] = {};            // drain ticks since raise, per cause
        // The age entries are atomic because raisePendingIntc (any thread) resets
        // an age while the interrupt worker thread increments it in drainPendingIntc.
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

    static uint32_t getAsyncHandlerStackTop(PS2Runtime *runtime)
    {
        // Only reachable if the callback stack pool is exhausted or runtime is
        // null; see kAsyncCallbackFallbackSp for the fallback.
        thread_local PS2Runtime *s_cachedRuntime = nullptr;
        thread_local uint32_t s_cachedStackTop = 0u;

        if (runtime == nullptr)
        {
            return kAsyncCallbackFallbackSp;
        }

        if (s_cachedRuntime != runtime || s_cachedStackTop == 0u)
        {
            s_cachedRuntime = runtime;
            s_cachedStackTop = runtime->reserveAsyncCallbackStack(kAsyncHandlerStackSize, 16u);
        }

        return (s_cachedStackTop != 0u) ? s_cachedStackTop : kAsyncCallbackFallbackSp;
    }

    // Unified INTC/DMAC dispatch: collect handlers from `handlerMap` that are
    // enabled for `cause`, then run them under AsyncGuestScope. `enabledMask`
    // is the per-cause enable bitmask (g_enabled_intc_mask or g_enabled_dmac_mask).
    // `tag` is used for exception logging ("INTC" or "DMAC").
    //
    // NOTE on guest-memory concurrency: the handler bodies invoked below run on
    // the IRQ worker thread (a real, separate host thread — see AsyncGuestScope),
    // genuinely in parallel with the main guest fiber executing on the scheduler's
    // single guest-executor thread. If a handler and the main guest code both
    // touch the same rdram address without the guest itself arranging a lock/
    // semaphore, that is an intentional characteristic of this "IRQ handlers are
    // real concurrent workers" design (it mirrors how an interrupt handler
    // touching a shared variable requires the GUEST to synchronize, exactly as
    // on real hardware) rather than a synchronization bug in the runtime. Do not
    // add locking around guest rdram accesses here to silence such reports.
    static void dispatchHandlersForCause(
        uint8_t *rdram, PS2Runtime *runtime, uint32_t cause,
        const std::unordered_map<int, IrqHandlerInfo> &handlerMap,
        uint32_t enabledMask, const char *tag)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        // 2026-07-26 -- guest interrupt-mask visibility (see the CRITSEC block in
        // game_overrides.cpp and the gate in ps2_scheduler.cpp). The host-worker
        // path below is already covered: while the guest holds DisableIntr, the
        // fiber refuses to surrender the guest token, so AsyncGuestScope cannot
        // acquire it and no handler runs. The INLINE path (ps2sched::is_guest_thread
        // branch further down) is NOT covered -- it is a direct nested call on the
        // calling fiber, not a preemption, so a DMAC dispatch reached from
        // sceSifSetDma / sceDmaSend / drainCompletedDmacHandlers can still run
        // inside a guest critical section.
        //
        // No evidence yet that this happens (0x178428's section makes no syscall),
        // so this only reports rather than defers: deferring would need a queue and
        // would change delivery ordering on a path that has not been shown broken.
        // If DEFERINL ever appears in run_probe.jsonl, that assumption is wrong and
        // the inline path needs the same treatment as the token path.
        if (ps2x_guest_intr_disable_depth() != 0u)
        {
            static std::atomic<uint32_t> s_inlineInSection{0};
            const uint32_t n = s_inlineInSection.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
            {
                static const char *const k[] = {"n", "cause", "depth"};
                const uint64_t v[] = {n, cause, ps2x_guest_intr_disable_depth()};
                ps2x_probe_kv("DEFERINL", 3, k, v);
            }
        }

        std::vector<IrqHandlerInfo> handlers;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (enabledMask & (1u << cause)) == 0u)
            {
                return;
            }

            handlers.reserve(handlerMap.size());
            for (const auto &kv : handlerMap)
            {
                const IrqHandlerInfo &info = kv.second;
                if (!info.enabled || info.cause != cause || info.handler == 0u)
                {
                    continue;
                }
                handlers.push_back(info);
            }
            std::sort(handlers.begin(), handlers.end(), [](const IrqHandlerInfo &a, const IrqHandlerInfo &b)
                      { return a.order < b.order; });
        }

        auto runHandlers = [&](uint32_t stackTop)
        {
            for (const IrqHandlerInfo &info : handlers)
            {
                if (!runtime->hasFunction(info.handler))
                {
                    continue;
                }

                try
                {
                    R5900Context irqCtx{};
                    SET_GPR_U32(&irqCtx, 28, info.gp);
                    SET_GPR_U32(&irqCtx, 29, stackTop);
                    SET_GPR_U32(&irqCtx, 31, 0u);
                    SET_GPR_U32(&irqCtx, 4, cause);
                    SET_GPR_U32(&irqCtx, 5, info.arg);
                    SET_GPR_U32(&irqCtx, 6, 0u);
                    SET_GPR_U32(&irqCtx, 7, 0u);
                    irqCtx.pc = info.handler;

                    while (irqCtx.pc != 0u && runtime && !runtime->isStopRequested())
                    {
                        PS2Runtime::RecompiledFunction step = runtime->lookupFunction(irqCtx.pc);
                        if (!step)
                        {
                            break;
                        }
                        step(rdram, &irqCtx, runtime);
                    }
                }
                catch (const ThreadExitException &)
                {
                }
                catch (const std::exception &e)
                {
                    static uint32_t warnCount = 0;
                    if (warnCount < 8u)
                    {
                        std::cerr << "[" << tag << "] handler 0x" << std::hex << info.handler
                                  << " threw exception: " << e.what() << std::dec << std::endl;
                        ++warnCount;
                    }
                }
            }
        };

        // Nothing to run: skip the token borrow and the scratch reservation.
        if (handlers.empty())
        {
            return;
        }

        // The INTC path only ever runs on the IRQ worker host thread, but the
        // DMAC path is ALSO reachable synchronously from guest code:
        // sceSifSetDma (Stubs/SIF.cpp), sceDmaSend (Stubs/Helpers/Support.h),
        // and drainCompletedDmacHandlers (ps2_runtime.cpp) all call
        // dispatchDmacHandlersForCause inline while servicing a guest syscall,
        // i.e. while the calling fiber IS the guest execution slot.
        // AsyncGuestScope's async_guest_begin() aborts by design if invoked
        // from the guest executor thread (that guard exists to catch host
        // workers mistakenly running there) - so only borrow the token when
        // this call is NOT already running on the guest thread.
        if (ps2sched::is_guest_thread())
        {
            // Inline on the calling fiber: a handler body can yield at a
            // back-edge, so a shared stack would be clobbered by another fiber
            // dispatching inline (or by a nested inline DMAC on this same
            // fiber). Reserve a fresh per-invocation scratch stack — NOT the
            // async pool: a per-fiber pool reservation would exhaust the pool's
            // 32 slots and fall back onto a shared stack, reintroducing the bug.
            // Handlers in this loop run sequentially (never nested), so one
            // reservation for the whole dispatch is correct; guestFree fires
            // here when the dispatch (and any yield inside it) completes.
            GuestScratchStack handlerStack(runtime, kAsyncHandlerStackSize);
            runHandlers(handlerStack.valid() ? handlerStack.top()
                                             : getAsyncHandlerStackTop(runtime));
        }
        else
        {
            // Host worker (INTC) under AsyncGuestScope: cannot yield, one
            // callback runs to completion, so the per-OS-thread pool cache is
            // safe, including its failure fallback (kAsyncCallbackFallbackSp
            // when the pool is exhausted or runtime is null).
            AsyncGuestScope guestScope; // token released on any exit path
            runHandlers(getAsyncHandlerStackTop(runtime));
        }
    }

    static int dispatchAndCountIntcHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        if (!rdram || !runtime)
        {
            return 0;
        }

        int eligible = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (g_enabled_intc_mask.load(std::memory_order_acquire) & (1u << cause)) == 0u)
            {
                eligible = 0;
            }
            else
            {
                for (const auto &kv : g_intcHandlers)
                {
                    const IrqHandlerInfo &info = kv.second;
                    if (info.enabled && info.cause == cause && info.handler != 0u)
                    {
                        ++eligible;
                    }
                }
            }
        }

        if (eligible > 0)
        {
            dispatchHandlersForCause(rdram, runtime, cause, g_intcHandlers,
                                      g_enabled_intc_mask.load(std::memory_order_acquire), "INTC");
        }
        return eligible;
    }


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
        uint32_t pending = g_pending_intc_causes.load(std::memory_order_acquire);
        while (pending != 0u)
        {
            const uint32_t cause = static_cast<uint32_t>(std::countr_zero(pending));
            const uint32_t bit = 1u << cause;
            pending &= ~bit;

            const int ran = dispatchAndCountIntcHandlersForCause(rdram, runtime, cause);
            if (ran > 0)
            {
                // Level-triggered by design: delivery clears the single pending
                // bit. If another raise of this same cause lands mid-drain
                // (between the dispatch above and this fetch_and), the two raises
                // collapse into one delivery. Accepted under the
                // level-triggered design -- a set bit means "at least one pending",
                // not a count -- a deliberate tradeoff, not a lost-wakeup bug.
                g_pending_intc_causes.fetch_and(~bit, std::memory_order_acq_rel);
                g_pending_intc_age[cause].store(0u, std::memory_order_relaxed);
                PS2_IF_AGRESSIVE_LOGS({
                    static std::atomic<uint32_t> s_deliverLogCount{0u};
                    const uint32_t logIndex = s_deliverLogCount.fetch_add(1u, std::memory_order_relaxed);
                    if (logIndex < 16u || (logIndex % 256u) == 0u)
                    {
                        RUNTIME_LOG("[INTC:deliver] cause=" << cause << " handlers=" << ran);
                    }
                });
            }
            else if (g_pending_intc_age[cause].fetch_add(1u, std::memory_order_relaxed) + 1u > kPendingIntcMaxAgeTicks)
            {
                g_pending_intc_causes.fetch_and(~bit, std::memory_order_acq_rel);
                g_pending_intc_age[cause].store(0u, std::memory_order_relaxed);
                PS2_IF_AGRESSIVE_LOGS({
                    static std::atomic<uint32_t> s_dropLogCount{0u};
                    const uint32_t logIndex = s_dropLogCount.fetch_add(1u, std::memory_order_relaxed);
                    if (logIndex < 16u || (logIndex % 256u) == 0u)
                    {
                        RUNTIME_LOG("[INTC:drop] cause=" << cause << " aged out with no registered/enabled handler");
                    }
                });
            }
        }
    }

    void resetInterruptHandlerState()
    {
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_intcHandlers.clear();
            g_dmacHandlers.clear();
            g_nextIntcHandlerId = 1;
            g_nextDmacHandlerId = 1;
            g_intc_head_order = 0;
            g_intc_tail_order = 1000;
            g_dmac_head_order = 0;
            g_dmac_tail_order = 1000;
            g_enabled_intc_mask = 0xFFFFFFFFu;
            g_enabled_dmac_mask = 0xFFFFFFFFu;
        }
        g_pending_intc_causes.store(0u, std::memory_order_release);
        for (auto &age : g_pending_intc_age)
        {
            age.store(0u, std::memory_order_relaxed);
        }
    }

    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        dispatchHandlersForCause(rdram, runtime, cause, g_dmacHandlers,
                                  g_enabled_dmac_mask.load(std::memory_order_acquire), "DMAC");
    }

    static void updateGsCsrFieldForVSync(PS2Runtime *runtime, uint64_t tickValue)
    {
        if (!runtime)
        {
            return;
        }

        constexpr uint64_t kGsCsrFieldMask = 0x2000ull;
        std::atomic<uint64_t> &csr = runtime->memory().gs().csr;
        if (tickValue & 1ull)
        {
            csr.fetch_or(kGsCsrFieldMask);
        }
        else
        {
            csr.fetch_and(~kGsCsrFieldMask);
        }
    }

    static uint64_t signalVSyncFlag(uint8_t *rdram, PS2Runtime *runtime)
    {
        VSyncFlagRegistration reg{};
        uint64_t tickValue = 0u;
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            reg = g_vsync_registration;
            tickValue = ++g_vsync_tick_counter;

            // Consume the registration: syscall 73h is ONE-SHOT. The SDK's
            // GsSyncV (guest 0x1751c0) registers its OWN stack frame --
            // flag = sp+0, tick = sp+8 -- spins until the flag flips, then
            // returns and pops the frame WITHOUT ever calling
            // SetVSyncFlag(0, 0). No deregister path exists in that function.
            // So the idiom is only sound if the kernel stops writing after it
            // fires once; a persistent registration turns every later vblank
            // into an async store into an abandoned frame. Measured 2026-07-27:
            // flag = 0x1ffbeb0 with GsSyncV's saved $ra at 0x1ffbec0, and a
            // hardware watchpoint caught this very store planting 0x1 in
            // rpc_call's (0x178be8) saved-$ra slot once that stack depth was
            // reused -- the Stage 5.4.2 boot derail.
            g_vsync_registration.flagAddr = 0u;
            g_vsync_registration.tickAddr = 0u;
        }

        // Wake all guest threads waiting for the next vsync tick.
        // Called from the IRQ worker (a non-guest host thread). Use the identity-
        // validated wakeup so a recycled tid cannot deliver this tick to the wrong
        // fiber: each entry carries the parking fiber's generation token.
        wakeWaiters(g_vsync_flag_mutex, g_vsync_waitList);
        updateGsCsrFieldForVSync(runtime, tickValue);

        // These two writes race, by design, with guest/test code polling the
        // same rdram words on another thread (real PS2 hardware exposes the
        // vsync flag/tick exactly this way: a plain memory-mapped word the
        // application polls, with no interlock). TSan reports this as a data
        // race because it is one under the C++ memory model, but adding a
        // mutex here would not match real hardware semantics and would still
        // require the poller to take the same lock (it can't: it's guest code
        // reading raw rdram, or test code via readGuestU32/readGuestU64,
        // neither of which we can — or should — change). Left unsynchronized
        // intentionally; do not wrap in a lock.
        if (reg.flagAddr != 0u)
        {
            writeGuestU32NoThrow(rdram, reg.flagAddr, 1u);
        }
        if (reg.tickAddr != 0u)
        {
            writeGuestU64NoThrow(rdram, reg.tickAddr, tickValue);
        }
        return tickValue;
    }

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

    // Delivered vblank ticks (stage 5.6.2). gif/s ~3 against an expected ~60
    // has two readings -- the guest renders one frame per ~20 vblanks, or the
    // vblank tick itself is not arriving at 60Hz -- and nothing measured so far
    // separates them. Counted at the point of delivery, after the pacing wait,
    // so it reflects ticks the guest actually saw. Read by the watchdog via a
    // local extern "C" (no header change; §3 prohibition).
    static std::atomic<uint64_t> g_vblankTicks{0};

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
                }
                else if (stalledPolls >= kDetStallPolls)
                {
                    ticksToProcess = 1;
                    stalledPolls = 0;
                    nextProgressTick = progress + quantum;
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
                g_vblankTicks.fetch_add(1, std::memory_order_relaxed);
                const uint64_t tickValue = signalVSyncFlag(rdram, runtime);
                ps2_stubs::dispatchGsSyncVCallback(rdram, runtime, tickValue);

                // Raise INTC_STAT VBLANK-start (bit2). The game's VBLANK wait at
                // 0x175210 spins on `lw 0x1000F000; andi 4` until this asserts;
                // it W1C-acks it via `sw 4,(0x1000F000)` and loops for the next
                // frame. Without this the raw MMIO poll never advances (magenta
                // screen). Set before dispatching the VBon handler list.
                runtime->memory().orIORegister(0x1000F000u, 1u << 2);
                dispatchAndCountIntcHandlersForCause(rdram, runtime, kIntcVblankStart);
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

                // Raise INTC_STAT VBLANK-end (bit3) for the VBoff handler list.
                runtime->memory().orIORegister(0x1000F000u, 1u << 3);
                dispatchAndCountIntcHandlersForCause(rdram, runtime, kIntcVblankEnd);
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

    uint64_t GetCurrentVSyncTick()
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        return g_vsync_tick_counter;
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

        // Wake any guest threads waiting on vsync during shutdown.
        wakeWaiters(g_vsync_flag_mutex, g_vsync_waitList);
    }

    uint64_t WaitForNextVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
        ensureInterruptWorkerRunning(rdram, runtime);

        // Opaque identity of the fiber that is about to park. Non-fiber host
        // workers get token FiberToken{} and never publish to the wait-list.
        const ps2sched::FiberToken selfToken = ps2sched::current_fiber_token();
        const bool onFiber = (selfToken != ps2sched::FiberToken{});

        // Snapshot the tick we are waiting to advance past. A non-fiber worker
        // never publishes to g_vsync_waitList (it cannot park), so it cannot
        // rely on a single wake meaning "signalVSyncFlag ran"; it must instead
        // poll this counter directly until it changes.
        uint64_t entryTick;
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            entryTick = g_vsync_tick_counter;
        }

        if (!onFiber)
        {
            // Non-fiber path (IRQ/alarm worker calling back into vsync wait, or
            // a borrowed host worker): loop with bounded exponential backoff
            // (mirrors WaitSema/WaitEventFlag's non-fiber Mesa loop) until
            // g_vsync_tick_counter actually advances past entryTick or runtime
            // stop is requested. A single block_current()+backoff step could
            // return before the IRQ worker's next tick fired, handing back the
            // SAME tick the caller already observed instead of truly waiting
            // for the next one.
            NonFiberBackoff nfBackoff;
            for (;;)
            {
                const ps2sched::BlockResult br = nfBackoff.wait(false);

                std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
                if (g_vsync_tick_counter != entryTick)
                {
                    return g_vsync_tick_counter;
                }
                if (runtime == nullptr || runtime->isStopRequested())
                {
                    return g_vsync_tick_counter;
                }
            }
        }

        // Publish under g_vsync_flag_mutex; arm_park after the lock is
        // released so g_sched_mutex is never nested under it.
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            g_vsync_waitList.emplace_back(g_currentThreadId, selfToken);
        }
        // Block the current fiber; signalVSyncFlag calls the validated wakeup
        // from the IRQ worker thread to wake us. onFiber is always true here
        // (the !onFiber path above already returned), so wait() never runs a
        // backoff step for this park.
        NonFiberBackoff nfBackoff;
        const ps2sched::BlockResult br = nfBackoff.wait(true);

        // A fiber woken from a real park (Parked) may have been woken by
        // scheduler_shutdown / TerminateThread rather than a vsync tick. If so,
        // unwind instead of returning a tick value. Mirrors WaitSema's terminate
        // check after wake.
        if (br == ps2sched::BlockResult::Parked)
        {
            std::shared_ptr<ThreadInfo> info = lookupThreadInfo(g_currentThreadId);
            if (info && info->terminated.load())
            {
                // Drop our wait-list entry before unwinding so a recycled tid
                // cannot inherit a stale token.
                {
                    std::lock_guard<std::mutex> clLock(g_vsync_flag_mutex);
                    auto &wl = g_vsync_waitList;
                    auto it = std::find_if(wl.begin(), wl.end(),
                                           [selfToken](const std::pair<int, ps2sched::FiberToken> &e)
                                           { return e.second == selfToken; });
                    if (it != wl.end()) wl.erase(it);
                }
                throw ThreadExitException();
            }
        }

        // If we were woken by something other than a vsync tick (shutdown,
        // TerminateThread, or a wakeup during the parking window), signalVSyncFlag
        // never drained us, so our entry is still queued. Remove it by fiber-token
        // identity (NOT by tid, which can recycle). A real vsync wake already
        // swapped us out, so this erase is a harmless no-op on that path.
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        auto &wl = g_vsync_waitList;
        auto it = std::find_if(wl.begin(), wl.end(),
                               [selfToken](const std::pair<int, ps2sched::FiberToken> &e)
                               { return e.second == selfToken; });
        if (it != wl.end())
        {
            wl.erase(it);
        }
        return g_vsync_tick_counter;
    }

    void WaitVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
        (void)WaitForNextVSyncTick(rdram, runtime);
    }

    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flagAddr = getRegU32(ctx, 4);
        const uint32_t tickAddr = getRegU32(ctx, 5);

        // 2026-07-27: the hardware-watchpoint run named signalVSyncFlag as the
        // writer of the 0x1 that lands in rpc_call's saved-$ra slot (0x1ffbeb0).
        // The write itself is correct per ps2tek 73h -- so the registered
        // flagAddr must be a dead stack address. Log every registration with the
        // caller's $ra so we can see (a) who registers it and (b) whether the
        // guest ever deregisters with (0,0) as the wait-then-release idiom
        // requires. Guest stack lives below 0x02000000 and above the heap; the
        // `stack` field flags the addresses that can be reused under us.
        {
            static uint64_t s_seq = 0;
            const char *k[5] = {"seq", "flag", "tick", "ra", "stack"};
            const uint64_t v[5] = {
                ++s_seq,
                flagAddr,
                tickAddr,
                getRegU32(ctx, 31),
                (flagAddr >= 0x01000000u && flagAddr < 0x02000000u) ? 1ull : 0ull};
            ps2x_probe_kv("VSYNCREG", 5, k, v);
        }

        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            g_vsync_registration.flagAddr = flagAddr;
            g_vsync_registration.tickAddr = tickAddr;
        }

        writeGuestU32NoThrow(rdram, flagAddr, 0u);
        writeGuestU64NoThrow(rdram, tickAddr, 0u);
        ensureInterruptWorkerRunning(rdram, runtime);
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            // Atomic RMW: g_enabled_intc_mask is read lock-free from the IRQ
            // worker thread (see declaration comment), so it must also be
            // written lock-free rather than under g_irq_handler_mutex.
            g_enabled_intc_mask.fetch_or(1u << cause, std::memory_order_acq_rel);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableIntc(rdram, ctx, runtime);
    }

    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            g_enabled_intc_mask.fetch_and(~(1u << cause), std::memory_order_acq_rel);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableIntc(rdram, ctx, runtime);
    }

    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_intc_head_order : ++g_intc_tail_order;
            handlerId = g_nextIntcHandlerId++;
            info.id = handlerId;
            g_intcHandlers[handlerId] = info;
        }

        ensureInterruptWorkerRunning(rdram, runtime);
        setReturnS32(ctx, handlerId);
    }

    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddIntcHandler(rdram, ctx, runtime);
    }

    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_intcHandlers.find(handlerId);
            if (it != g_intcHandlers.end() && it->second.cause == cause)
            {
                g_intcHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_dmac_head_order : ++g_dmac_tail_order;
            handlerId = g_nextDmacHandlerId++;
            info.id = handlerId;
            g_dmacHandlers[handlerId] = info;
        }
        setReturnS32(ctx, handlerId);
    }

    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddDmacHandler(rdram, ctx, runtime);
    }

    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_dmacHandlers.find(handlerId);
            if (it != g_dmacHandlers.end() && it->second.cause == cause)
            {
                g_dmacHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            // See EnableIntc: g_enabled_dmac_mask is atomic for the same reason.
            g_enabled_dmac_mask.fetch_or(1u << cause, std::memory_order_acq_rel);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableDmac(rdram, ctx, runtime);
    }

    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            g_enabled_dmac_mask.fetch_and(~(1u << cause), std::memory_order_acq_rel);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableDmac(rdram, ctx, runtime);
    }
}
