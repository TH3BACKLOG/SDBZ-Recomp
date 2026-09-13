#include "Common.h"
#include <cstdlib>
#include <chrono>
#include <cstdint>
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

// Defined in Kernel/EeScheduler.cpp (Phase 3d, replacing ps2sched's
// thread_local g_currentThreadId). Same extern-in-.cpp rule as above.
extern "C" int ps2x_guest_current_thread_id();

// ---------------------------------------------------------------------------
// 2026-09-03 part 56 -- probe budgets that cannot go blind on the stall.
//
// The 09-03 00:44 run raised CHGPRI/THLIFE from 400/600 to 6000 and BOTH still
// saturated at guest progress 0xe80793 (~15.2M), while the stall did not begin
// until roughly t=336s at progress ~36M. A first-N cap spends its whole budget
// on the healthy phase, so raising it only moves the blind spot -- it never
// reaches the window we need (feedback_capped_probes_false_negatives). That is
// why the 6000-record CHGPRI census shows thread 6 exactly ONCE (its creation
// priority 0x19) even though the watchdog caught ChangeThreadPriority(6, 1)
// live at t=394s: the probe was already blind by then.
//
// Replaced with two overlapping budgets:
//   * "early"  -- the first `warm` records, unconditional, for the boot history.
//   * "steady" -- after that, at most `perSec` records per wall second, forever.
//
// A run of any length therefore carries records from every second of it at a
// bounded cost, and `n` (the TRUE call count, incremented even when the record
// is thinned) is emitted so a thinned stream can never be misread as a low call
// rate -- the failure mode a plain cap has.
class ProbeBudget
{
public:
    ProbeBudget(unsigned warm, unsigned perSec) : m_warm(warm), m_perSec(perSec) {}

    bool allow()
    {
        ++m_calls;
        if (m_calls <= m_warm)
        {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - m_windowStart >= std::chrono::seconds(1))
        {
            m_windowStart = now;
            m_inWindow = 0;
        }
        if (m_inWindow >= m_perSec)
        {
            return false;
        }
        ++m_inWindow;
        return true;
    }

    uint64_t calls() const { return m_calls; }

private:
    unsigned m_warm;
    unsigned m_perSec;
    uint64_t m_calls = 0;
    unsigned m_inWindow = 0;
    std::chrono::steady_clock::time_point m_windowStart{};
};


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

    // ...except the guard above is DEAD CODE and has been since it was written.
    // `tls_on_irq_handler_stack` is only ever incremented by Interrupt.cpp's
    // `IrqHandlerStackScope`, and that type is declared and never instantiated
    // -- grep it: five hits, all inside its own definition. So the counter is
    // permanently 0 and the early-out never fires.
    //
    // Measured cost, run of 2026-09-10 (first run with tid 1 registered):
    // 32,768+ STACKOOB violations, cap [cap] line at 16, and all 16 surviving
    // records identical -- pc=0x178068, sp=0xffff0, blamed on thid=0x1.
    // 0xffff0 is the async callback pool's stackTop, exactly as printed by the
    // one `[async-stack] reserved [0xfc000, 0x100000) stackTop=0xffff0` line of
    // that boot. Every one of those 32,768 is the false positive the dead guard
    // was supposed to suppress, and they drowned the probe before it could say
    // anything about the frame this instrument was added to watch.
    //
    // Fixed by address band rather than by arming the scope. Arming it means
    // touching all four invocation dispatch sites in EeScheduler.cpp, and the
    // band test is exact: ps2_runtime.cpp:318-336 documents the pool invariant
    // as "guest thread stacks are game-chosen addresses >= 0x00100000
    // [and the pool] is disjoint from ALL guest memory by construction". A $sp
    // below that ceiling is kernel-reserved by definition and can never be a
    // guest thread's own stack, so this cannot mask a real violation.
    //
    // Not a header include: ps2_runtime.h is pulled in by ~4,520 generated TUs
    // and Thread.cpp does not currently include it. Mirrored constant, with the
    // definition site named so the two stay findable together.
    constexpr uint32_t kKernelReservedStackCeiling = 0x00100000u; // == kAsyncCallbackStackTop, ps2xRuntime/include/ps2_runtime.h:432
    if (sp < kKernelReservedStackCeiling)
        return 0;

    const int tid = ps2x_guest_current_thread_id();
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

// Stage 5.17 -- equal-priority yield inside ReferThreadStatus, env-gated OFF.
//
// The guest's cross-thread handshake sub_11E690 sets [0x441924]=1, boosts the
// worker to its OWN priority via 29h, then spins on 0x30 ReferThreadStatus until
// the worker ACKs. Our 29h (ChangeThreadPriority, below) already reschedules
// natively on a self-boost -- see ChangeThreadPriority's own comment -- so the
// FIRST handoff works. But once the worker blocks inside the pump and later
// becomes Ready again, nothing can hand it the slot back: EeScheduler's own
// checkpoint/reschedule paths only select a STRICTLY higher-priority head, and
// 0x30 -- the only syscall inside the spin -- has no yield at all. Measured cost in
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

// 2026-09-02 part 50. ps2tek 29h/2Ah say ChangeThreadPriority / iChangeThreadPriority
// return the thread's OLD priority on success. We returned KE_OK (0). Set
// PS2X_CHGPRI_RET=0 to restore the old status-code return for A/B testing;
// unset (the default) is the ps2tek-conformant behaviour.
static bool chgPriReturnsOldPriority()
{
    static const bool on = []() -> bool
    {
        const char *e = std::getenv("PS2X_CHGPRI_RET");
        return !(e && *e == '0' && e[1] == '\0');
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
            // 2026-09-01 part 48 -- the second candidate producer of -403.
            if (result == KE_ILLEGAL_PRIORITY)
            {
                static const char *const k[] = {"thid", "prio", "isafe", "pc", "ra"};
                const uint64_t v[] = {static_cast<uint64_t>(static_cast<uint32_t>(id)),
                                      static_cast<uint64_t>(static_cast<uint32_t>(priority)),
                                      interruptSafe ? 1u : 0u,
                                      ctx->pc,
                                      getRegU32(ctx, 31)};
                ps2x_probe_kv("PRIOREJECT", 5, k, v);
            }
            // 2026-09-02 part 50 -- ps2tek 29h/2Ah: ChangeThreadPriority returns
            // the thread's OLD priority on success, not a status code. We were
            // returning KE_OK (0) and discarding oldPriority, which breaks the
            // guest's own save/restore pair around its critical section:
            //
            //   0x11e5d4  jal 0x174b30         ; ChangeThreadPriority(self, boost=1)
            //   0x11e5e0  sw  $v0, -28144($v1) ; saved = $v0   <-- got 0
            //   ...
            //   0x11e668  jal 0x174b30         ; ChangeThreadPriority(self, saved)
            //
            // Every critical-section EXIT therefore pinned the caller at
            // priority 0 -- the top of the ready queue. In the 2026-09-02 04:24
            // run that left main RUNNING at pri 0 forever while the sub_11EAC8
            // acker sat READY at the guest's boost level of 1 and could never
            // preempt it ([thsync] VERDICT=WORKER-NOT-RUNNING, nTh=6).
            //
            // Failure codes are left alone: our KE_* values are the house
            // convention and PRIOREJECT above keys off KE_ILLEGAL_PRIORITY.
            // Gated so one binary serves both arms of the A/B.
            const int returned =
                (result == KE_OK && chgPriReturnsOldPriority()) ? oldPriority : result;

            // Every call, not just the rejects: the open question after the
            // priority-0 fix is whether the guest ever raises main above the
            // boost level at all, or whether main stays at 0 from ExecPS2
            // onward (ps2tek 07h creates the main thread at priority 0). Only a
            // full census of thid/prio/old answers that; a reject-only probe
            // cannot. Capped, and the cap is reported in the record so a
            // saturated probe cannot be read as an absence.
            {
                // Thread 6 gets its own budget: it appears ONCE in the whole
                // 6000-record 09-03 census (creation, prio 0x19), so its warm
                // budget is guaranteed still intact when the stall starts, and
                // the separate n6 counter gives its true call rate even after
                // the steady-state thinning kicks in.
                static ProbeBudget budget(1500u, 24u);
                static ProbeBudget t6Budget(400u, 24u);
                const bool anyBudget = budget.allow();
                // Budget on the RESOLVED thread, not the argument: a caller
                // passing id == 0 ("self") would otherwise log thid=0x0 and
                // miss the thread-6 budget entirely. The three known callers
                // (enter_critsec ra=0x11e5dc, leave_critsec ra=0x11e670,
                // spinner sub_11E690) all pass an explicit tid via GetThreadId,
                // but the caller that pins thread 6 is exactly what is not yet
                // known, so do not assume it follows the same shape.
                const int callerId = ps2x_guest_current_thread_id();
                const int resolvedId = (id == 0) ? callerId : id;
                const bool t6Budgeted = (resolvedId == 6) && t6Budget.allow();
                if (anyBudget || t6Budgeted)
                {
                    // "cur" = the thread that ISSUED this call, as opposed to
                    // "rid" (the thread being changed). Part 59 took the static
                    // graph as far as it goes: the spinner sub_11E690 is reached
                    // only through callback slot 6, dispatched from cblist_run(6)
                    // (0x13c6e8), which has FOUR call sites (0x11e390, 0x11e480,
                    // 0x11e53c, 0x11eb3c) and is not confined to one thread -- so
                    // no static read can name the spinner's host. "cur" on the
                    // ra=0x11e6e8 records answers it directly, and "cur" on the
                    // ra=0x11e5dc records confirms whether the poisoned save is
                    // thread 6 saving its own already-boosted priority.
                    static const char *const k[] = {"thid", "prio", "old", "ret", "isafe", "ra", "n", "n6", "rid", "cur"};
                    const uint64_t v[] = {static_cast<uint64_t>(static_cast<uint32_t>(id)),
                                          static_cast<uint64_t>(static_cast<uint32_t>(priority)),
                                          static_cast<uint64_t>(static_cast<uint32_t>(oldPriority)),
                                          static_cast<uint64_t>(static_cast<uint32_t>(returned)),
                                          interruptSafe ? 1u : 0u,
                                          getRegU32(ctx, 31),
                                          budget.calls(),
                                          t6Budget.calls(),
                                          static_cast<uint64_t>(static_cast<uint32_t>(resolvedId)),
                                          static_cast<uint64_t>(static_cast<uint32_t>(callerId))};
                    ps2x_probe_kv("CHGPRI", 10, k, v);
                }
            }

            setReturnS32(ctx, returned);
            ee.transferIfRequested(interruptSafe);
        }

        void rotateReadyQueueImpl(uint8_t *rdram,
                                  R5900Context *ctx,
                                  PS2Runtime *runtime,
                                  bool interruptSafe)
        {
            EeScheduler &ee = scheduler(rdram, ctx, runtime);
            const int result = ee.rotateReadyQueue(static_cast<int>(getRegU32(ctx, 4)), interruptSafe);
            // 2026-09-01 part 48 -- the third and last producer of -403.
            if (result == KE_ILLEGAL_PRIORITY)
            {
                static const char *const k[] = {"prio", "isafe", "pc", "ra"};
                const uint64_t v[] = {static_cast<uint64_t>(getRegU32(ctx, 4)),
                                      interruptSafe ? 1u : 0u,
                                      ctx->pc,
                                      getRegU32(ctx, 31)};
                ps2x_probe_kv("ROTREJECT", 4, k, v);
            }
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
        // 2026-09-01 part 48 -- THCREATE. The t=140s stall polls
        // ReferThreadStatus(a0 = -403). -403 is KE_ILLEGAL_PRIORITY
        // (EeScheduler.cpp:19), NOT a pseudo-thread id -- part 47's PSEUDOTID
        // probe fired 0 times, so GetThreadId never leaked one and that
        // hypothesis is dead. The remaining producers of -403 are
        // createThread / changePriority / rotateReadyQueue. Of those only a
        // createThread return is plausibly stored by the guest as a thread id
        // and later handed back to ReferThreadStatus -- but that is still
        // INFERENCE, so all three are probed rather than assumed. CreateThread
        // is low volume (the game makes tens of threads, not thousands), so
        // this logs every call, not just the failures: knowing which thread
        // was refused matters as much as knowing that one was.
        const int createResult = scheduler(rdram, ctx, runtime).createThread(decoded);
        {
            static const char *const k[] = {"res",  "prio", "func", "attr",
                                            "stksz", "stk",  "ra"};
            const uint64_t v[] = {static_cast<uint64_t>(static_cast<uint32_t>(createResult)),
                                  static_cast<uint64_t>(static_cast<uint32_t>(param->initial_priority)),
                                  param->func,
                                  param->attr,
                                  static_cast<uint64_t>(static_cast<uint32_t>(param->stack_size)),
                                  param->stack,
                                  getRegU32(ctx, 31)};
            ps2x_probe_kv("THCREATE", 7, k, v);
        }
        setReturnS32(ctx, createResult);
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

    // 2026-09-02 part 52 -- THLIFE probe.
    //
    // Thread 3 (entry 0x11e7e0) has sat at status SUSPEND on its own entry PC
    // for three consecutive 200s runs and has never executed one instruction of
    // its body. The acker sub_11EAC8 ends every run polling it through
    // thread_resume_if_suspended (0x11ed90 -> ReferThreadStatus 0x174ba0, ra
    // 0x11edb4). Whether the guest suspends it, our resumeThread fails to clear
    // the flag, or a resume succeeds and is immediately undone is not decidable
    // from a 1 Hz status snapshot -- all three look identical. Record every
    // Start/Suspend/Resume with the status on both sides of the call.
    static int lifecycleStatusOf(EeScheduler &ee, int id)
    {
        GuestThread *t = ee.thread(id);
        return t ? rawThreadStatus(t->status) : -1;
    }

    static int lifecyclePriorityOf(EeScheduler &ee, int id)
    {
        GuestThread *t = ee.thread(id);
        return t ? t->currentPriority : -1;
    }

    // 2026-09-02 part 53 -- THLIFE now also answers WHY no preemption happens.
    //
    // Part 52 measured the ping-pong: resume_if_suspended (0x11ed90) puts
    // thread 3 SUSPEND->READY, then suspend_if_running (0x11edf8) puts it
    // READY->SUSPEND, 283 times, with no guest progress in between. Our
    // resumeThread is correct -- st1 is READY every time. What never happens
    // is the context switch: EeScheduler::resumeThread calls
    // requestPreemptionIfHigher(), which bails when
    //     readyThread.currentPriority >= running->currentPriority
    // On real hardware, resuming a priority-8 thread from a lower-priority
    // caller switches immediately, so thread 3 runs before the suspend lands.
    //
    // That comparison is the whole question and it is not visible from any
    // existing probe: THCREATE gives creation priority, CHGPRI gives requested
    // changes, neither gives the RUNNING thread at the moment of the resume.
    // thread_resume_if_suspended has 11 static callers, so the caller cannot be
    // attributed by counting. Record both sides of the comparison instead:
    // me/mypri = the thread that issued the syscall, tpri = the target's
    // priority. A record with tpri >= mypri explains the missing switch; a
    // record with tpri < mypri means the bail is elsewhere and this hypothesis
    // is dead.
    static void probeThreadLifecycle(
        EeScheduler &ee, char op, int id, int st0, int st1, int res, uint32_t ra)
    {
        static ProbeBudget budget(1500u, 24u);
        if (!budget.allow())
        {
            return;
        }
        const int me = ee.currentThreadId();
        static const char *const k[] = {"op",  "thid",  "st0",  "st1", "res",
                                       "ra",  "me",    "mypri", "tpri", "n"};
        const uint64_t v[] = {static_cast<uint64_t>(static_cast<unsigned char>(op)),
                              static_cast<uint64_t>(static_cast<uint32_t>(id)),
                              static_cast<uint64_t>(static_cast<uint32_t>(st0)),
                              static_cast<uint64_t>(static_cast<uint32_t>(st1)),
                              static_cast<uint64_t>(static_cast<uint32_t>(res)),
                              static_cast<uint64_t>(ra),
                              static_cast<uint64_t>(static_cast<uint32_t>(me)),
                              static_cast<uint64_t>(static_cast<uint32_t>(
                                  lifecyclePriorityOf(ee, me))),
                              static_cast<uint64_t>(static_cast<uint32_t>(
                                  lifecyclePriorityOf(ee, id))),
                              budget.calls()};
        ps2x_probe_kv("THLIFE", 10, k, v);
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

        const int before = rawThreadStatus(target->status);
        const int result = ee.startThread(id, arg, *ctx, false);
        probeThreadLifecycle(ee, 'S', id, before, lifecycleStatusOf(ee, id), result, getRegU32(ctx, 31));
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
        const int susId = static_cast<int>(getRegU32(ctx, 4));
        const int susBefore = lifecycleStatusOf(ee, susId);
        const int result = ee.suspendThread(susId, false);
        probeThreadLifecycle(ee, 'U', susId, susBefore, lifecycleStatusOf(ee, susId), result, getRegU32(ctx, 31));
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int resId = static_cast<int>(getRegU32(ctx, 4));
        const int resBefore = lifecycleStatusOf(ee, resId);
        const int result = ee.resumeThread(resId, false);
        probeThreadLifecycle(ee, 'R', resId, resBefore, lifecycleStatusOf(ee, resId), result, getRegU32(ctx, 31));
        setReturnS32(ctx, result);
        ee.transferIfRequested(false);
    }

    // 2026-09-01 part 47 -- PSEUDOTID probe.
    //
    // The t=129s stall ends with iReferThreadStatus(a0=0xfffffe6d = -403).
    // Negative ids are minted ONLY by EeScheduler::acquireInvocationThread()
    // (EeScheduler.cpp:2940, m_nextInvocationThreadId--), and no real PS2
    // thread id is ever negative. The suspected path is that the guest read
    // one out of GetThreadId while an async invocation was standing in for a
    // real thread, stored it, and asked about it later.
    //
    // That last link is INFERRED, so it gets measured rather than assumed:
    // "raw" is what currentThreadId() would have returned, "given" is what the
    // guest actually receives. raw != given proves the leak existed and that
    // PS2X_PSEUDO_TID_HIDE suppressed it. Uncapped on purpose -- if the fix
    // works this fires a handful of times, and a cap here would turn the
    // interesting case (it kept happening) into a false negative.
    std::atomic<uint64_t> g_pseudoTidSeen{0};

    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = scheduler(rdram, ctx, runtime);
        const int raw = ee.currentThreadId();
        const int given = ee.guestVisibleThreadId();
        if (raw < 0)
        {
            const uint64_t n =
                g_pseudoTidSeen.fetch_add(1, std::memory_order_relaxed) + 1u;
            static const char *const k[] = {"raw", "given", "pc", "ra", "n"};
            const uint64_t v[] = {static_cast<uint64_t>(static_cast<uint32_t>(raw)),
                                  static_cast<uint64_t>(static_cast<uint32_t>(given)),
                                  ctx->pc, getRegU32(ctx, 31), n};
            ps2x_probe_kv("PSEUDOTID", 5, k, v);
        }
        setReturnS32(ctx, given);
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
        // 2026-09-01 part 47 -- the consumer half of PSEUDOTID. A negative id
        // arriving here means one leaked out of GetThreadId earlier in the run,
        // and "found" says whether the pseudo-thread record still exists (it is
        // erased on exit, EeScheduler.cpp:1804) or the guest gets KE_UNKNOWN_THID
        // for a thread it believes it owns. This is the syscall the t=129s
        // watchdog line names, so it is the exact point to watch.
        if (id < 0)
        {
            static const char *const k[] = {"thid", "found", "pc", "ra"};
            const uint64_t v[] = {static_cast<uint64_t>(static_cast<uint32_t>(id)),
                                  thread != nullptr ? 1u : 0u,
                                  ctx->pc, getRegU32(ctx, 31)};
            ps2x_probe_kv("PSEUDOREFER", 4, k, v);
        }
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
            scheduler(rdram, ctx, runtime).yieldIfHigherPriorityReady(false);
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
