#include "Common.h"
#include <cstdlib>
#include "Thread.h"
#include "runtime/ee_scheduler.h"

// Phase C -- EE thread visibility.
//
// Defined in game_overrides.cpp. Declared extern here rather than in a header:
// touching a .h forces a full 30h+ rebuild (skill SS3 prohibition 3).
extern "C" void ps2x_probe_kv(const char *name, int n,
                              const char *const *keys, const uint64_t *vals);

// Defined in Kernel/Syscalls/Interrupt.cpp. Non-zero while this fiber is running
// an IRQ handler body on a BORROWED stack -- a GuestScratchStack carved out of
// the guest heap for the inline DMAC path, or the async callback pool for the
// host-worker INTC path. In both cases $sp legitimately sits outside the stack
// StartThread handed the fiber, so the guard below must not fire. Same extern-in-
// .cpp rule as ps2x_probe_kv: a header edit costs a full rebuild.
extern "C" uint32_t ps2x_on_irq_handler_stack();

// ---------------------------------------------------------------------------
// Phase C -- stack-bounds guard.
//
// Every probe we have fires where corruption is OBSERVED, which is always
// downstream of where the invariant BROKE. RASLOT is the extreme case: the
// det=0 run put it 0 progress ticks and 1 record ahead of the dispatch miss,
// i.e. it reports the damage at the same instant the damage is consumed.
//
// This guard fires on the violation instead: the first moment a thread's sp
// leaves the stack range its own StartThread handed it. Kept in a file-scope
// map keyed by tid rather than as a scheduler-owned field, because the
// scheduler state lives in a header and headers are off-limits.
namespace
{
    struct GuestStackRange
    {
        uint32_t lo = 0;
        uint32_t hi = 0;   // exclusive; hi==0 means "unknown, do not check"
    };

    std::mutex g_stackRangeMutex;
    std::unordered_map<int, GuestStackRange> g_stackRanges;

    // Bounded. An out-of-bounds sp usually stays out of bounds for every
    // subsequent call, so an unbounded guard would reproduce exactly the
    // 89,000-line flood the dispatch-miss path already produces.
    // Run 71 produced EXACTLY 16 STACKOOB records -- which is exactly the cap.
    // The old code stopped emitting at 16 and said nothing about it, so "16
    // violations" and "sixteen million violations" were indistinguishable in
    // the log, and the run-71 sweep read the number as a total. A silent cap is
    // the specific failure [[feedback_capped_probes_false_negatives]] exists to
    // prevent: a saturated probe looks exactly like a bounded one. Three fixes,
    // all cheap:
    //   * the limit is tunable (PS2X_STACKOOB_MAX, 0 = unlimited)
    //   * saturation emits one [cap] line, so absence past it is not evidence
    //   * the running total is re-announced at each power of two past the cap,
    //     which gives the ORDER OF MAGNITUDE without reopening the flood the
    //     bound was added to stop
    std::atomic<uint64_t> g_stackOobTotal{0};

    int stackOobMax()
    {
        static const int kMax = []() -> int {
            if (const char *e = std::getenv("PS2X_STACKOOB_MAX"))
            {
                const long parsed = std::strtol(e, nullptr, 0);
                if (parsed >= 0) { return static_cast<int>(parsed); }
            }
            return 16;
        }();
        return kMax;
    }
}

extern "C" void ps2x_stack_register(int tid, uint32_t lo, uint32_t hi)
{
    std::lock_guard<std::mutex> lock(g_stackRangeMutex);
    g_stackRanges[tid] = GuestStackRange{lo, hi};
}

// Returns 1 if sp is outside the calling fiber's registered stack, else 0.
// site is a caller-chosen tag (0 = slot entry, 1 = slot exit) so the record
// says WHERE the check ran without needing a second probe family.
extern "C" int ps2x_stack_check(uint32_t pc, uint32_t sp, uint32_t site)
{
    // An IRQ handler running on a borrowed stack is not a violation. Run 53
    // reported four (sp=0x1f00000 = the guest-heap limit, i.e. the top of a
    // 16 KB GuestScratchStack) against thread 0x4's range and they read as a
    // context bleed. Checked first: the borrowed stack is never in range, so
    // every one of those records would otherwise be a guaranteed false positive.
    if (ps2x_on_irq_handler_stack() != 0u)
        return 0;

    const int tid = g_currentThreadId;
    GuestStackRange r;
    {
        std::lock_guard<std::mutex> lock(g_stackRangeMutex);
        auto it = g_stackRanges.find(tid);
        if (it == g_stackRanges.end())
            return 0;   // never started through StartThread; nothing to compare
        r = it->second;
    }
    if (r.hi == 0 || (sp >= r.lo && sp < r.hi))
        return 0;

    // One counter, not two: a separate int report-counter would keep climbing
    // past INT_MAX in a long run, and the ordinal is already implied by total.
    const uint64_t total = g_stackOobTotal.fetch_add(1, std::memory_order_relaxed) + 1u;
    const uint64_t max = static_cast<uint64_t>(stackOobMax());
    if (max == 0u || total <= max)
    {
        static const char *const k[] = {"pc", "sp", "lo", "hi", "site", "thid"};
        const uint64_t v[] = {pc, sp, r.lo, r.hi, site,
                              static_cast<uint64_t>(static_cast<uint32_t>(tid))};
        ps2x_probe_kv("STACKOOB", 6, k, v);
    }
    else if (total == max + 1u)
    {
        RUNTIME_LOG("[cap] tag=STACKOOB saturated at " << max
                    << " -- LATER VIOLATIONS ARE INVISIBLE. Absence of a record"
                       " past this point is NOT evidence. Raise with"
                       " PS2X_STACKOOB_MAX (0 = unlimited).");
    }
    else if ((total & (total - 1u)) == 0u)
    {
        // Power-of-two only: 16 lines to reach a million, so the magnitude is
        // always on the record and the flood never comes back.
        RUNTIME_LOG("[cap] tag=STACKOOB total=" << total
                    << " (still capped at " << max << ")");
    }
    return 1;
}

// NOTE (Phase 3c-3b, EE scheduler merge): force_reschedule()/g_currentThreadId
// still resolve to the pre-EeScheduler ps2sched globals (ps2_scheduler.h/.cpp),
// which are already broken (dispatchLoop removed in sub-phase 3a) and are
// slated for wholesale retirement in Phase 3d. refstatYieldEnabled()'s
// force_reschedule() call below and ps2x_stack_check()'s g_currentThreadId read
// are cross-cutting dependencies that must be re-pointed at EeScheduler
// (m_rescheduleRequested/transferIfRequested and EeScheduler::currentThreadId(),
// respectively) when ps2_scheduler.cpp/.h are retired in Phase 3d.
namespace ps2sched { void force_reschedule(); }

// Stage 5.17 -- equal-priority yield inside ReferThreadStatus, env-gated OFF.
//
// The guest's cross-thread handshake sub_11E690 sets [0x441924]=1, boosts the
// worker to its OWN priority via 29h, then spins on 0x30 ReferThreadStatus until
// the worker ACKs. Our 29h already calls force_reschedule() (below, ~line 1131),
// so the FIRST handoff works. But once the worker blocks inside the pump and
// later becomes Ready again, nothing can hand it the slot back: maybe_yield()
// and yield_point() step 3 both select a STRICTLY higher-priority head, and 0x30
// -- the only syscall inside the spin -- has no yield at all. Measured cost in
// run 20260824-101518: one handshake takes 8 wall seconds with the worker
// reading NOT-RUNNING for 3 consecutive seconds, and vbl/s collapses to 1 while
// it holds.
//
// Gated rather than unconditional for two reasons: 0x30 does NOT reschedule on
// real hardware (the release there comes from the interrupt-exit reschedule we
// do not model), and an env gate lets one binary serve both arms of the A/B
// instead of costing a second ~48-minute build.
static bool refstatYieldEnabled()
{
    static const bool on = []() -> bool
    {
        const char *e = std::getenv("PS2X_REFSTAT_YIELD");
        return e && *e && *e != '0';
    }();
    return on;
}

namespace ps2_syscalls
{
    namespace
    {
        EeScheduler &scheduler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            EeScheduler &result = runtime->eeScheduler();
            result.bindMainContextForSyscall(*ctx, rdram);
            return result;
        }

        int rawThreadStatus(EeThreadStatus status)
        {
            switch (status)
            {
            case EeThreadStatus::Running:
                return THS_RUN;
            case EeThreadStatus::Ready:
                return THS_READY;
            case EeThreadStatus::Waiting:
                return THS_WAIT;
            case EeThreadStatus::WaitingSuspended:
                return THS_WAITSUSPEND;
            case EeThreadStatus::Suspended:
                return THS_SUSPEND;
            case EeThreadStatus::Dormant:
                return THS_DORMANT;
            }
            return THS_DORMANT;
        }

        int rawWaitType(EeWaitReason reason)
        {
            switch (reason)
            {
            case EeWaitReason::Sleep:
                return TSW_SLEEP;
            case EeWaitReason::Semaphore:
                return TSW_SEMA;
            case EeWaitReason::EventFlag:
                return TSW_EVENT;
            case EeWaitReason::VSync:
                return 4;
            case EeWaitReason::External:
            case EeWaitReason::Mpeg:
                return 5;
            case EeWaitReason::None:
                return TSW_NONE;
            }
            return TSW_NONE;
        }

        int waitId(const GuestThread &thread)
        {
            if (thread.wait.reason == EeWaitReason::Semaphore)
            {
                return std::get<EeSemaphoreWait>(thread.wait.payload).id;
            }
            if (thread.wait.reason == EeWaitReason::EventFlag)
            {
                return std::get<EeEventFlagWait>(thread.wait.payload).id;
            }
            return 0;
        }

        [[noreturn]] void exitThreadWithHandlers(int tid,
                                                 R5900Context *ctx,
                                                 PS2Runtime *runtime,
                                                 bool deleteThread)
        {
            EeScheduler &ee = runtime->eeScheduler();
            const auto handlers = runtime->takeEeExitHandlers(tid);
            std::vector<GuestInvocation> invocations;
            invocations.reserve(handlers.size());
            for (const PS2Runtime::EeExitHandlerRegistration &handler : handlers)
            {
                if (handler.function == 0u || !runtime->hasFunction(handler.function))
                {
                    continue;
                }
                GuestInvocation invocation{};
                invocation.kind = GuestInvocationKind::ExitHandler;
                invocation.context = *ctx;
                invocation.context.pc = handler.function;
                SET_GPR_U32(&invocation.context, 4, handler.argument);
                SET_GPR_U32(&invocation.context, 29, ee.invocationStackTop());
                SET_GPR_U32(&invocation.context, 31, 0u);
                invocations.push_back(std::move(invocation));
            }
            if (invocations.empty())
            {
                ee.exitCurrent(deleteThread);
            }
            invocations.back().onComplete = [runtime, deleteThread](const R5900Context &, R5900Context &)
            {
                runtime->eeScheduler().exitCurrent(deleteThread);
            };
            ee.invokeCurrentSequence(std::move(invocations));
        }

        void changePriorityImpl(uint8_t *rdram,
                                R5900Context *ctx,
                                PS2Runtime *runtime,
                                bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int id = static_cast<int>(getRegU32(ctx, 4));
            const int priority = static_cast<int>(getRegU32(ctx, 5));
            int oldPriority = 0;
            const int result = ee.changePriority(id, priority, interruptSafe, oldPriority);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void rotateReadyQueueImpl(uint8_t *rdram,
                                  R5900Context *ctx,
                                  PS2Runtime *runtime,
                                  bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.rotateReadyQueue(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void wakeupThreadImpl(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.wakeupThread(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }

        void releaseWaitImpl(uint8_t *rdram,
                             R5900Context *ctx,
                             PS2Runtime *runtime,
                             bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.releaseWait(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            setReturnS32(ctx, result);
            ee.transferIfRequested(interruptSafe);
        }
    }

    // Snapshots the EeScheduler's published thread table. Safe to call from
    // any thread (EeScheduler::snapshot() is mutex-guarded); used by the debug
    // IPC layer once per video frame. Restored 2026-07-14 for the
    // RecompDebugger revival, re-pointed at EeScheduler in Phase 3c-3b.
    std::vector<ThreadDebugSnapshot> getThreadDebugSnapshot(PS2Runtime *runtime)
    {
        std::vector<ThreadDebugSnapshot> out;
        if (!runtime)
        {
            return out;
        }
        const EeKernelSnapshot snap = runtime->eeScheduler().snapshot();
        out.reserve(snap.threads.size());
        for (const EeThreadSnapshot &t : snap.threads)
        {
            ThreadDebugSnapshot debugSnap;
            debugSnap.tid             = t.id;
            debugSnap.entry           = t.entry;
            debugSnap.currentPc       = t.pc;
            debugSnap.stack           = t.stack;
            debugSnap.status          = rawThreadStatus(t.status);
            debugSnap.waitType        = rawWaitType(t.waitReason);
            debugSnap.waitId          = t.waitId;
            debugSnap.currentPriority = t.currentPriority;
            out.push_back(debugSnap);
        }
        return out;
    }

    void FlushCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        FlushCache(rdram, ctx, runtime);
    }

    void EnableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void DisableCache(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void ResetEE(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void SetMemoryMode(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, KE_OK);
    }

    void InitThread(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, EeScheduler::kMainThreadId);
    }

    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t address = getRegU32(ctx, 4);
        if (address == 0u)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        const auto *param = getEeGuestStruct<ee_thread_t>(rdram, address);
        if (!param)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        if (param->stack_size < 0)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (param->stack != 0u)
        {
            uint32_t stackOffset = 0u;
            bool scratch = false;
            if (!resolveEeGuestRange(param->stack,
                                     static_cast<size_t>(param->stack_size),
                                     stackOffset,
                                     scratch))
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }
        }

        // PS2SDK EE t_ee_thread: status, func, stack, stack_size, gp_reg,
        // initial_priority, current_priority, attr, option.
        const EeThreadCreateParams decoded{
            param->attr,
            param->func,
            param->stack,
            static_cast<uint32_t>(param->stack_size),
            param->gp_reg,
            param->initial_priority,
            param->option,
        };
        setReturnS32(ctx, scheduler(rdram, ctx, runtime).createThread(decoded));
    }

    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        uint32_t ownedStack = 0;
        const int result = ee.deleteThread(id, ownedStack);
        if (result == KE_OK)
        {
            runtime->removeEeExitHandlers(id);
        }
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int id = static_cast<int>(getRegU32(ctx, 4));
        const uint32_t arg = getRegU32(ctx, 5);
        GuestThread *target = ee.thread(id);
        if (!target)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        if (target->status != EeThreadStatus::Dormant)
        {
            setReturnS32(ctx, KE_NOT_DORMANT);
            return;
        }
        if (!runtime->hasFunction(target->entry))
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (target->stack == 0u && target->stackSize != 0u)
        {
            target->stack = runtime->guestMalloc(target->stackSize, 16u);
            if (target->stack == 0u)
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }
            target->ownsStack = true;
        }

        // Arm the STACKOOB bounds guard for this thread. When no stack is
        // registered (target->stack == 0) hi=0 disables the check rather than
        // inventing a range (see ps2x_stack_check).
        if (target->stack != 0u)
        {
            const uint32_t stackSize = (target->stackSize != 0u) ? target->stackSize : 0x800u;
            ps2x_stack_register(id, target->stack, target->stack + stackSize);
        }
        else
        {
            ps2x_stack_register(id, 0, 0);
        }

        const int result = ee.startThread(id, arg, *ctx, false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, false);
    }

    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        exitThreadWithHandlers(ee.currentThreadId(), ctx, runtime, true);
    }

    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        uint32_t ownedStack = 0;
        const int result = ee.terminateThread(static_cast<int>(getRegU32(ctx, 4)), ownedStack, false);
        if (ownedStack != 0u)
        {
            runtime->guestFree(ownedStack);
        }
        setReturnS32(ctx, result);
    }

    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.suspendThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int result = ee.resumeThread(static_cast<int>(getRegU32(ctx, 4)), false);
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, scheduler(rdram, ctx, runtime).currentThreadId());
    }

    // Shared body. Takes no scheduler action of any kind, so both 0x30 and the
    // interrupt-context 0x31 can use it. The lock_guard is confined to its own
    // scope: g_sched_mutex must never nest under a ThreadInfo::m (see
    // SleepThread's "Drop info->m before ANY scheduler operation"), so any yield
    // has to happen after this returns, not inside it.
    static void referThreadStatusImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        int id = static_cast<int>(getRegU32(ctx, 4));
        if (id == 0)
        {
            id = ee.currentThreadId();
        }
        const GuestThread *thread = ee.thread(id);
        if (!thread)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }
        auto *status = getEeGuestStruct<ee_thread_status_t>(rdram, getRegU32(ctx, 5));
        if (!status)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        *status = {};
        status->status = rawThreadStatus(thread->status);
        status->func = thread->entry;
        status->stack = thread->stack;
        status->stack_size = static_cast<int>(thread->stackSize);
        status->gp_reg = thread->gp;
        status->initial_priority = thread->initialPriority;
        status->current_priority = thread->currentPriority;
        status->attr = thread->attr;
        status->option = thread->option;
        status->waitType = rawWaitType(thread->wait.reason);
        status->waitId = waitId(*thread);
        status->wakeupCount = thread->wakeupCount;
        setReturnS32(ctx, KE_OK);
    }

    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        referThreadStatusImpl(rdram, ctx, runtime);

        // Outside referThreadStatusImpl, so any scheduler-internal locks are
        // already released. See refstatYieldEnabled() above for why this is
        // gated and what it fixes.
        if (refstatYieldEnabled())
            ps2sched::force_reschedule();
    }

    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // ps2tek 31h: the i-prefixed form runs in interrupt context and must NOT
        // reschedule. Calls the impl directly rather than ReferThreadStatus, so
        // the gate above can never leak into it.
        referThreadStatusImpl(rdram, ctx, runtime);
    }

    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        ee.sleepCurrent();
        setReturnS32(ctx, KE_OK);
    }

    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, false);
    }

    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        wakeupThreadImpl(rdram, ctx, runtime, true);
    }

    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx,
                     scheduler(rdram, ctx, runtime).cancelWakeup(static_cast<int>(getRegU32(ctx, 4))));
    }

    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == 0u)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }
        CancelWakeupThread(rdram, ctx, runtime);
    }

    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Note (Phase 3c-3b): the SDBZ-only "chgpri" self-boost diagnostic that
        // used to live here confirmed every ChangeThreadPriority call in the
        // sub_11E690 handshake is a self-boost (tid==callerTid, status==Running).
        // EeScheduler::changePriority() already covers that case natively: when
        // status==Running it scans for a strictly-higher-priority ready thread
        // and sets m_rescheduleRequested, which changePriorityImpl's
        // ee.transferIfRequested() below then acts on -- the equivalent of the
        // old ps2sched::force_reschedule() call, without needing an env gate.
        changePriorityImpl(rdram, ctx, runtime, false);
    }

    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        changePriorityImpl(rdram, ctx, runtime, true);
    }

    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, false);
    }

    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rotateReadyQueueImpl(rdram, ctx, runtime, true);
    }

    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, false);
    }

    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        releaseWaitImpl(rdram, ctx, runtime, true);
    }
}
