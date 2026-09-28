// ---------------------------------------------------------------------------
// Shared scaffolding for scheduler-shaped tests (ps2_runtime_expansion_tests.cpp
// and ps2_scheduler_workload_regression_tests.cpp): the RAII fixture that owns the
// runtime + guest RAM for a single test, and the register-access / wait-poll
// helpers every scheduler test needs regardless of which file it lives in.
// ---------------------------------------------------------------------------
#pragma once

#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ps2x_test
{
    // Binds a MIPS-ABI argument register to a 32-bit value.
    inline void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    // Reads back a return-value register as a signed 32-bit result.
    inline int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    // Atomic 32-bit read from guest RAM. Lives here (rather than duplicated
    // per test file) because the poll helpers below need it, and any test
    // that only ever reads a control word can use it directly too.
    inline uint32_t rdramRead32(const std::vector<uint8_t> &rdram, uint32_t addr)
    {
        return std::atomic_ref<uint32_t>(
                   *reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(rdram.data()) + addr))
            .load();
    }

    // ------------------------------------------------------------------
    // 2026-09-21: resumable step functions.
    //
    // A test "step function" is a host C++ function registered with
    // PS2Runtime::registerFunction() and dispatched by EeScheduler::run() as if
    // it were recompiled guest code. THREE separate paths inside the scheduler
    // end that dispatch by THROWING EeDispatcherTransfer rather than returning:
    //
    //   blockCurrent()         EeScheduler.cpp:3971  WaitSema / SleepThread /
    //                                                WaitEventFlag blocked
    //   transferIfRequested()  EeScheduler.cpp:2897  a preemption became due --
    //                                                e.g. SignalSema woke a
    //                                                higher-priority thread
    //   exitCurrent()          EeScheduler.cpp:2161  ExitThread; [[noreturn]]
    //
    // The throw unwinds out of the step function entirely and is caught only in
    // run(). When the thread is dispatched again, run() enters at ctx->pc --
    // which for a host function is still its ENTRY address, because a host
    // function has no per-instruction pc the way recompiled code does
    // (EeScheduler.cpp:1479 looks the function up by an EXACT pc match). So the
    // step function RUNS AGAIN FROM THE TOP.
    //
    // Before this existed that meant:
    //   * a WaitSema that blocked was re-issued on wake, so the worker blocked
    //     forever -- "[semwatch:wait] id=1 found=1 count=0" immediately after
    //     the signal that had just woken it;
    //   * a WaitSema whose semaphore was deleted re-issued and returned "no such
    //     sema" instead of the KE_WAIT_DELETE makeReady() had written to $v0;
    //   * a SleepThread re-slept -- two "[semwatch:blockcurrent]" lines with the
    //     IDENTICAL pc, which is the direct fingerprint of re-entry;
    //   * anything between two syscalls (a gSeq.fetch_add, an rdram record) ran
    //     more than once whenever a syscall preempted.
    //
    // SchedStep gives a step function the resume point that recompiled code gets
    // for free. Every syscall, every side-effecting block and every host value a
    // later branch depends on is an indexed STEP. A step that already completed
    // on an earlier dispatch is skipped and its recorded result replayed, so a
    // re-dispatch replays the same path and then continues past it.
    //
    // State lives host-side rather than in the guest context: no register budget
    // to blow, no cap on the number of steps (the loop-shaped workers need
    // that), and nothing to collide with what a test puts in rdram or in $a0.
    //
    // Keyed by (guest thread id, entry pc), not thread id alone, because a step
    // function can park while NESTED inside another one on the same thread --
    // SchedulerOverrideIsolation parks stepOverrideHandler inside stepOverrideA,
    // and SchedulerRecoveryIsolation parks an invoke/DMAC handler inside its
    // caller. Those share a thread id but never an entry pc, so they get
    // independent step sequences instead of interleaving into one.
    // ------------------------------------------------------------------
    struct SchedStepState
    {
        uint32_t completed = 0u;    // steps finished on earlier dispatches
        bool pending = false;       // a step body was entered and threw out
        bool argsSaved = false;
        uint32_t args[4] = {0u, 0u, 0u, 0u};
        std::vector<int32_t> results;
    };

    inline std::unordered_map<uint64_t, SchedStepState> &schedStepStates()
    {
        static std::unordered_map<uint64_t, SchedStepState> states;
        return states;
    }

    inline uint64_t schedStepKey(int tid, uint32_t entryPc)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tid)) << 32) | entryPc;
    }

    // Guest thread ids are REUSED (see the SchedulerTidReuse suite), so any state
    // left on a tid must be dropped when a new thread is started on it. The
    // worker-start helpers call schedStepResetThread(); SchedStep::finish() also
    // clears its own key on a clean exit.
    inline void schedStepResetThread(int tid)
    {
        auto &states = schedStepStates();
        for (auto it = states.begin(); it != states.end();)
        {
            it = (static_cast<int>(it->first >> 32) == tid) ? states.erase(it) : std::next(it);
        }
    }

    inline void schedStepResetAll() { schedStepStates().clear(); }

    class SchedStep
    {
    public:
        SchedStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            : m_rdram(rdram),
              m_ctx(ctx),
              m_runtime(runtime),
              m_tid(runtime->eeScheduler().currentThreadId()),
              m_key(schedStepKey(m_tid, ctx->pc)),
              m_state(schedStepStates()[m_key])
        {
            if (!m_state.argsSaved)
            {
                // $a0-$a3 as the function was entered. call() overwrites them to
                // pass syscall arguments, so without this a step function that
                // reads its StartThread arg out of $a0 (stepU3/U4RegisterHandler-
                // ThenBlock, stepOverrideHandler) would read a stale syscall
                // argument after the first resume. Restoring them after every
                // call() keeps $a0-$a3 reading as this function's own arguments
                // on EVERY dispatch.
                for (int i = 0; i < 4; ++i)
                {
                    m_state.args[i] = ::getRegU32(m_ctx, 4 + i);
                }
                m_state.argsSaved = true;
            }
            else
            {
                restoreArgs();
            }

            if (m_state.pending)
            {
                // The previous dispatch threw out of step #completed.
                //   blockCurrent()        -> makeReady() has since written the
                //                            wake result into $v0 of THIS context
                //                            (EeScheduler.cpp:3974).
                //   transferIfRequested() -> the syscall had already finished and
                //                            written $v0 before the preemption
                //                            throw.
                // Either way $v0 is that step's result, right now.
                m_state.pending = false;
                record(getRegS32(*m_ctx, 2));
            }
        }

        // True only on the dispatch where this step function first ran. Guard
        // non-idempotent work that must happen before the first step.
        [[nodiscard]] bool firstDispatch() const { return m_state.completed == 0u; }

        [[nodiscard]] int threadId() const { return m_tid; }

        // THE primitive. Runs `body` at most once across all dispatches of this
        // step function and returns its int32_t result, replayed from the record
        // if it already ran. `body` may throw EeDispatcherTransfer and never
        // return; the next dispatch then treats the step as complete with $v0 as
        // its result, which is correct for both throw paths (see the ctor).
        template <typename Body>
        int32_t step(Body &&body)
        {
            const uint32_t index = m_index++;
            if (index < m_state.completed)
            {
                return m_state.results[index];
            }
            // Marked BEFORE the body runs: it may never return.
            m_state.pending = true;
            const int32_t result = body();
            m_state.pending = false;
            record(result);
            return result;
        }

        // One syscall, at most once; returns $v0. Argument registers are set on
        // the THREAD's own context -- that is what makes the wake value land
        // somewhere we can still read after the stack has been unwound -- and
        // restored afterwards.
        template <typename Syscall>
        int32_t call(Syscall syscall,
                     uint32_t a0 = 0u, uint32_t a1 = 0u,
                     uint32_t a2 = 0u, uint32_t a3 = 0u)
        {
            return step([&]
            {
                setRegU32(*m_ctx, 4, a0);
                setRegU32(*m_ctx, 5, a1);
                setRegU32(*m_ctx, 6, a2);
                setRegU32(*m_ctx, 7, a3);
                syscall(m_rdram, m_ctx, m_runtime);
                const int32_t ret = getRegS32(*m_ctx, 2);
                restoreArgs();
                return ret;
            });
        }

        // Host-side work with side effects (an rdram record, a counter bump), at
        // most once.
        template <typename Body>
        void once(Body &&body)
        {
            step([&] { body(); return 0; });
        }

        // A host-side value a later branch depends on -- a stop flag, a claimed
        // slot index. Recorded on the dispatch that computes it and replayed
        // afterwards, so a re-dispatch takes the SAME branch it took originally
        // instead of re-deciding against state that has moved on underneath it.
        template <typename Body>
        int32_t value(Body &&body)
        {
            return step([&] { return static_cast<int32_t>(body()); });
        }

        // Normal end of a step function: pc==0 is what run() reads as "this
        // thread's entry returned" (EeScheduler.cpp:1087).
        void finish()
        {
            m_ctx->pc = 0u;
            release();
        }

        // End of a nested handler/invocation, which returns through its own
        // sentinel instead of going dormant.
        void finishAt(uint32_t pc)
        {
            m_ctx->pc = pc;
            release();
        }

    private:
        void release() { schedStepStates().erase(m_key); }

        void restoreArgs()
        {
            for (int i = 0; i < 4; ++i)
            {
                setRegU32(*m_ctx, 4 + i, m_state.args[i]);
            }
        }

        void record(int32_t value)
        {
            if (m_state.results.size() <= m_state.completed)
            {
                m_state.results.resize(m_state.completed + 1u, 0);
            }
            m_state.results[m_state.completed] = value;
            ++m_state.completed;
        }

        uint8_t *m_rdram;
        R5900Context *m_ctx;
        PS2Runtime *m_runtime;
        int m_tid;
        uint64_t m_key;
        SchedStepState &m_state;
        uint32_t m_index = 0u;
    };

    // ------------------------------------------------------------------
    // 2026-09-21: the guest-thread pump.
    //
    // EeScheduler::run() is the ONLY code that takes a Ready guest thread and
    // runs it, and it never returns. Nothing in ps2xTest ever called it, so a
    // StartThread'd worker went Ready and stayed Ready for the whole test --
    // which is why every scheduler assertion that needed a worker to actually
    // RUN failed ("worker should block on sema within 500ms") while the ones
    // that did not, passed.
    //
    // SchedFixture publishes itself here for the duration of a test body, and
    // waitUntil() below pumps it on every poll iteration. That keeps the fix
    // in the shared poll helper, so tests written against the old fiber pool
    // (which dispatched on its own threads) need no per-test change.
    //
    // thread_local on purpose: helper HOST threads inside a test (the
    // ParkedHostWorker / std::thread probes) must never pump -- they are not
    // the executor, and EeScheduler::pumpGuestThreads() asserts that. They
    // simply see a null target and fall through to the plain sleep.
    // ------------------------------------------------------------------
    inline PS2Runtime *&currentPumpTarget()
    {
        static thread_local PS2Runtime *target = nullptr;
        return target;
    }

    // Lets the guest threads of the registered runtime run until they are all
    // blocked/dormant, or `budget` elapses. A no-op when there is no fixture
    // registered on this thread, or when this thread is not the executor.
    //
    // The executor is adopted lazily -- bindMainContextForSyscall() claims
    // whichever thread issues the FIRST syscall -- so before any syscall has
    // run, onExecutorThread() is false and this correctly does nothing.
    inline void pumpGuest(std::chrono::milliseconds budget = std::chrono::milliseconds(2))
    {
        PS2Runtime *rt = currentPumpTarget();
        if (rt != nullptr && rt->eeScheduler().onExecutorThread())
        {
            rt->eeScheduler().pumpGuestThreads(budget);
        }
    }

    // 2026-09-22 -- installs the pump target for a runtime the test OWNS
    // ITSELF, rather than one SchedFixture constructed.
    //
    // Until now SchedFixture's constructor was the only thing that ever set
    // currentPumpTarget(). A test that declares its own `PS2Runtime` -- which
    // SchedulerShutdownFiber (AA5) and SchedulerReinit (AA8) both do, because
    // AA5 needs to call requestStop() mid-body and AA8 needs TWO runtimes
    // live at once -- therefore left the target null, so pumpGuest() returned
    // immediately and no guest thread in those suites ever ran a single
    // instruction. Every assertion that needed a fiber to reach a syscall,
    // exit, or drain failed, and nothing said why: pumpGuest()'s null check is
    // a silent early return, not an assert.
    //
    // Save/restore rather than clear, matching SchedFixture, so AA8's cycle-2
    // scope leaves cycle 1's runtime pumping again when it falls out of scope.
    struct PumpTargetScope
    {
        PS2Runtime *previous;

        explicit PumpTargetScope(PS2Runtime &rt) : previous(currentPumpTarget())
        {
            currentPumpTarget() = &rt;
        }

        ~PumpTargetScope() { currentPumpTarget() = previous; }

        PumpTargetScope(const PumpTargetScope &) = delete;
        PumpTargetScope &operator=(const PumpTargetScope &) = delete;
    };

    // Polls `pred` at 1ms intervals until it is true or `timeout` elapses,
    // always giving `pred` one final check at the deadline. Each iteration
    // also pumps the guest threads (see currentPumpTarget above) -- without
    // that, a predicate waiting on anything a guest thread does can never
    // become true.
    template <typename Predicate>
    bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            pumpGuest();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        pumpGuest();
        return pred();
    }

    // Waits for the guest executor to fully quiesce (no thread running or
    // ready). This is the teardown/drain poll almost every scheduler workload
    // test ends on; a thin wrap of waitUntil so call sites collapse to one
    // line without changing the wait semantics.
    //
    // Post-EeScheduler (Phase 3d): there is no more process-global
    // g_activeThreads -- each PS2Runtime owns its own EeScheduler instance,
    // so the drain check is scoped to the runtime under test via
    // EeScheduler::isIdle() (a snapshot() read, kept fresh by publishSnapshot()
    // after every kernel-object mutation, not just inside run()'s loop).
    // 2026-09-21: this used to call EeScheduler::isIdle() directly, which in a
    // TEST can never be true: isIdle() reports false while any thread is
    // Running or Ready, and the syscall-issuing main thread is permanently one
    // or the other (bindMainContextForSyscall() makes it Running on the first
    // syscall; pumpGuestThreads() parks it Ready). So every `drainedWithin`
    // assertion was failing for a reason unrelated to draining.
    //
    // What these call sites actually mean is "the workers this test started
    // have finished", so that is what is checked: every thread except main is
    // Dormant. Negative pseudo-tids from acquireInvocationThread() are
    // included deliberately -- an unconsumed invocation is not drained.
    inline bool drainedWithin(PS2Runtime &runtime, std::chrono::milliseconds timeout)
    {
        return waitUntil([&]
        {
            // 2026-09-22 -- pump BEFORE looking, on every evaluation.
            //
            // waitUntil() checks pred() before its first pumpGuest(), so a
            // drain whose predicate is already true returns having pumped
            // ZERO times. That is not a hypothetical: dispatchIrq() only
            // QUEUES (queueInvocation -> m_pendingInvocations,
            // EeScheduler.cpp:3295), and the predicate below walks
            // snapshot().threads only -- an invocation still sitting in the
            // deque has no thread yet, so it is invisible here. The moment
            // the dispatching fiber sets its sentinel and goes dormant,
            // "every thread except main is Dormant" is true and the queued
            // handler is silently abandoned.
            //
            // That is exactly what failed M1: hasHandlerFn=1, flag=1, and
            // sent=0 at every stage including after two drains -- the handler
            // was queued four separate times and never once dispatched.
            // Fifth member of the silent-no-op family (pumpGuest()'s null
            // target, the unbound executor, the wrong-thread pump target, the
            // missing SchedStep, and now this).
            //
            // 2026-09-22 -- CORRECTION to this comment block's own earlier
            // claim, which read "One pump is sufficient by construction:
            // pumpGuestThreads() runs until there is neither a runnable thread
            // NOR a pending invocation (EeScheduler.cpp:986)."
            //
            // That is true only of an UNBUDGETED pump. pumpGuest() passes a
            // 2 ms budget, and run() tests that deadline at the TOP of its
            // loop (EeScheduler.cpp:960) and breaks BEFORE reaching the :986
            // completion test. So a pump can return with an invocation still
            // queued, and the thread walk below -- which cannot see
            // m_pendingInvocations -- then reports a drained kernel.
            //
            // Measured, not inferred: with the thread walk alone,
            // SchedulerStackIsolation failed 3 of 8 identical isolated runs
            // while passing every batch run. The handler was landing on the
            // NEXT test's worker thread instead of during the drain.
            //
            // Sixth member of the silent-no-op family, and the second one in
            // this same helper: pumpGuest()'s null target, the unbound
            // executor, the wrong-thread pump target, the missing SchedStep,
            // drainedWithin() not pumping at all, and now drainedWithin()
            // pumping but calling a budget-truncated pump "drained".
            //
            // waitUntil() loops until ITS deadline, so an honest predicate
            // simply pumps again; the budget only has to be survivable, not
            // sufficient.
            pumpGuest();

            const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
            if (snap.pendingInvocations != 0u)
            {
                return false;
            }

            for (const EeThreadSnapshot &th : snap.threads)
            {
                if (th.id == 1)   // EeScheduler::kMainThreadId
                {
                    continue;
                }
                if (th.status != EeThreadStatus::Dormant)
                {
                    return false;
                }
            }
            return true;
        }, timeout);
    }

    // Waits for a guest-RAM control word to reach an exact value, or a floor
    // (AtLeast) — the recurring "poll a flag/counter a fiber writes" shape.
    // Also thin wraps of waitUntil; no new timing behavior.
    inline bool waitForWord(const std::vector<uint8_t> &rdram, uint32_t addr,
                             uint32_t want, std::chrono::milliseconds timeout)
    {
        return waitUntil([&]
        {
            return rdramRead32(rdram, addr) == want;
        }, timeout);
    }

    inline bool waitForWordAtLeast(const std::vector<uint8_t> &rdram, uint32_t addr,
                                    uint32_t want, std::chrono::milliseconds timeout)
    {
        return waitUntil([&]
        {
            return rdramRead32(rdram, addr) >= want;
        }, timeout);
    }

    // Note: no shared rdramWrite32 is added here. ps2_runtime_expansion_tests.cpp
    // already has a file-local rdramWrite32 (same signature/behavior), and
    // ps2_scheduler_workload_regression_tests.cpp independently defines its own
    // too; both already do `using namespace ps2x_test;`, so promoting either
    // one to this header would make `rdramWrite32(...)` ambiguous at every
    // call site outside its defining anonymous namespace. Each file keeps
    // reusing its own existing local helper instead of duplicating a new one.

    // Marshals a 0-3-arg / reg4..6-in, reg2-out syscall call: binds up to
    // three MIPS-ABI argument registers, invokes the syscall, and reads back
    // its return value, collapsing the ctx/setRegU32/call/getRegS32 ritual
    // duplicated across scheduler test bodies into a single call.
// ---------------------------------------------------------------------------
// SCHED_YIELD_POINT -- the recompiler's back-edge contract, for step functions.
//
// Every generated guest loop emits this at its back edge (see any generated
// body, e.g. ps2xRuntime/src/lib/Kernel/recovered/sub_001581B8_0x1581b8.cpp:62):
//
//     ctx->pc = <loop head>;
//     if (runtime->shouldPreemptGuestExecution()) { return; }
//     goto <loop head>;
//
// The checkpoint is a POLLED bool. EeScheduler::checkpointDue() is noexcept and
// never throws -- the old yield_point()/ThreadExitException contract retired
// with the fiber layer, but several step functions here were ported across
// verbatim and still DISCARD the result, as if a throw would carry them out.
//
// A step function that discards it never returns to EeScheduler::run(), so
// run()'s while-loop never iterates, so pumpGuestThreads()' deadline -- checked
// at the top of that loop (EeScheduler.cpp:943) -- is unreachable. The whole
// process then hangs, which is what took out SchedulerJoinHost,
// SchedulerJoinStarvation, SchedulerShutdownClean and SchedulerTokenHandoff.
//
// Resuming is automatic for a step function whose ENTIRE body is the loop:
// run() re-dispatches via lookupFunction(ctx->pc) (EeScheduler.cpp:1479), and
// ctx->pc still holds this function's entry address, so leaving it alone
// re-enters at the top -- exactly where the loop starts.
//
// *** Do NOT use this inside a bounded `for (int i = 0; i < N; ++i)` loop. ***
// The counter is a host local; re-entry resets it to 0 and the loop never
// finishes. Those sites need their counter in guest RAM or in SchedStep state.
#define SCHED_YIELD_POINT(rt)                        \
    do                                               \
    {                                                \
        if ((rt)->shouldPreemptGuestExecution())     \
        {                                            \
            return;                                  \
        }                                            \
    } while (false)

    using SyscallFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    inline int32_t callSyscallRaw(PS2Runtime &rt, std::vector<uint8_t> &ram, SyscallFn fn,
                                  uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        R5900Context ctx{};
        setRegU32(ctx, 4, a0);
        setRegU32(ctx, 5, a1);
        setRegU32(ctx, 6, a2);
        fn(ram.data(), &ctx, &rt);
        return getRegS32(ctx, 2);
    }

    // callSyscall() itself is defined further down, immediately after
    // HostGuestScope -- it needs that type, and nothing between here and there
    // calls it.

    // Hands out a fresh, disjoint worker-stack base of `size` bytes on every
    // call, so scheduler tests never need to hand-pick non-overlapping
    // addresses to avoid a sibling fiber's stack in the same test. Bumped
    // monotonically for the life of the process; each returned range is
    // exactly [base, base + size), so distinct calls can never overlap
    // regardless of call order.
    inline uint32_t nextWorkerStackBase(uint32_t size)
    {
        static std::atomic<uint32_t> next{0x00510000u};
        return next.fetch_add(size, std::memory_order_relaxed);
    }

    // Owns the runtime + guest RAM a scheduler test dispatches guest threads
    // against.
    //
    // Post-EeScheduler (Phase 3d): the old ps2sched::scheduler_init()/
    // scheduler_shutdown() pair existed because the fiber scheduler was
    // process-global state shared across successive tests -- init healed
    // drift left by the previous test's teardown races, shutdown tore that
    // shared state down again. EeScheduler has no such global: `runtime` is a
    // brand-new PS2Runtime (and therefore a brand-new, already-clean
    // EeScheduler) per SchedFixture instance, so there is nothing left over
    // to heal at construction. Destruction just requests the stop, mirroring
    // PS2Runtime::requestStop()'s own production behavior (which now calls
    // straight through to EeScheduler::requestStop() -- see
    // ps2_runtime.cpp -- in place of the retired
    // ps2_syscalls::notifyRuntimeStop() this fixture used to call itself).
    //
    // A test body only needs to add its own test-specific drains (signaling
    // semas it created, joining threads it spawned) before falling out of
    // scope.
    struct SchedFixture
    {
        PS2Runtime runtime;
        std::vector<uint8_t> rdram = std::vector<uint8_t>(PS2_RAM_SIZE, 0u);

        // Saved/restored rather than just cleared so a nested fixture (a test
        // that constructs a second runtime inside the first) leaves the outer
        // one pumping again on scope exit. Declared after `runtime` so member
        // init order captures the PREVIOUS target before the body installs
        // this one.
        PS2Runtime *previousPumpTarget = currentPumpTarget();

        SchedFixture()
        {
            currentPumpTarget() = &runtime;
            // Nothing may survive into a fresh runtime: guest thread ids restart
            // from kFirstThreadId, so a previous test's step history would be
            // keyed to ids this one is about to hand out again.
            schedStepResetAll();
        }

        ~SchedFixture()
        {
            currentPumpTarget() = previousPumpTarget;
            runtime.requestStop();
            schedStepResetAll();
        }
    };

    // RAII pair of scheduler semaphores: constructs both up front (exposed
    // via .a()/.b()) and deletes both on scope exit on every path — normal
    // fall-through, an assertion failure, or an early return — with no
    // hand-written guard/delete boilerplate at the call site. `create` /
    // `destroy` are passed in (rather than named directly here) because each
    // test file owns its own thin createSchedSema/deleteSchedSema wrappers
    // around the CreateSema/DeleteSema syscalls: file-local test scaffolding,
    // not part of the shared harness.
    struct SemaPair
    {
        using CreateFn = int32_t (*)(uint8_t *, PS2Runtime *, int, int);
        using DeleteFn = void (*)(uint8_t *, PS2Runtime *, int32_t);

        uint8_t *rdram;
        PS2Runtime *runtime;
        DeleteFn destroy;
        int32_t sidA;
        int32_t sidB;

        SemaPair(uint8_t *rdram_, PS2Runtime *runtime_, CreateFn create, DeleteFn destroy_,
                 int initA, int maxA, int initB, int maxB)
            : rdram(rdram_), runtime(runtime_), destroy(destroy_),
              sidA(create(rdram_, runtime_, initA, maxA)),
              sidB(create(rdram_, runtime_, initB, maxB))
        {
        }

        int32_t a() const { return sidA; }
        int32_t b() const { return sidB; }

        ~SemaPair()
        {
            destroy(rdram, runtime, sidA);
            destroy(rdram, runtime, sidB);
        }
    };

    // RAII non-fiber host worker that briefly takes EeScheduler's
    // hostInvocationMutex() -- Phase 3d's replacement for the retired
    // ps2sched::async_guest_begin()/async_guest_end() guest-token pair
    // (see ee_scheduler.h's hostInvocationMutex() doc comment: it is the
    // same "a host thread wants to act while the executor might be
    // dispatching guest code" serialization, just a plain mutex instead of a
    // token a thread could park on). There is no more g_currentThreadId
    // token-borrowing identity to set -- EeScheduler's single-executor model
    // has no concept of a fiber's own identity for a non-guest thread to
    // impersonate.
    //
    // ⚠️ Not a verified behavioral equivalent: the OLD token had an explicit
    // starvation-avoidance gate (g_host_token_waiters) so a parked worker was
    // GUARANTEED to win within bounded time even while fibers kept the run
    // queue non-empty. A plain std::mutex only has whatever fairness the host
    // OS/STL happens to provide around EeScheduler::run()'s own per-dispatch
    // lock/unlock (see run()'s dispatch loop, EeScheduler.cpp) -- there is no
    // explicit fairness gate. Tests that assert bounded-time acquisition under
    // contention (the old token-handoff starvation regressions) need that
    // re-verified against the new scheduler before being trusted, not just
    // recompiled against this replacement. Left to whoever picks up the
    // ps2xTest reconciliation this class was flagged for.
    //
    // A caller just needs to know when the worker won the lock and when it
    // released it; the destructor joins unconditionally so callers never have
    // to remember to.
    //
    // 2026-09-21 (PS2X_SCHED_TESTS_PORTED): stand-in for the retired
    // ps2sched::host_token_waiters(). The old token kept an explicit count of
    // host threads parked on it, which guest-registered free functions polled
    // to synchronise ("spin until a worker is parked, THEN do X"). A plain
    // std::mutex cannot be asked "is anyone blocked on you?", so the worker
    // publishes its own intent here instead: incremented immediately BEFORE it
    // blocks, decremented once it holds the lock. Same observable the tests
    // actually used, without inventing a counter inside EeScheduler.
    //
    // It counts ParkedHostWorker instances ONLY -- the real interrupt worker
    // (EnsureVSyncWorkerRunning) takes hostInvocationMutex() directly and is
    // invisible here. A test that needs an observable parked worker must
    // construct a ParkedHostWorker; see G1 in the workload regression suite.
    inline std::atomic<int> &parkedHostWorkerWaiters()
    {
        static std::atomic<int> waiters{0};
        return waiters;
    }

    // ------------------------------------------------------------------
    // 2026-09-21: replacements for ps2sched::async_guest_begin()/_end().
    //
    // Same job -- a host thread announcing "I am about to run guest code, do
    // not dispatch underneath me" -- but the guest token became
    // EeScheduler::hostInvocationMutex(), a plain std::mutex (see its doc
    // comment in ee_scheduler.h). Two shapes, because the call sites have two:
    //
    //   hostGuestBegin/End  paired calls, including try/catch sites where
    //                       exactly one End runs per path. NOT reentrant and
    //                       NOT exception-safe on its own -- it is a direct
    //                       lock()/unlock(), matching what the old free
    //                       functions did.
    //   HostGuestScope      RAII, for plain nested scopes. Prefer it.
    //
    // ⚠️ Behavioural difference worth remembering: the old token had an
    // explicit starvation gate; a std::mutex has only whatever fairness the
    // platform provides. See ParkedHostWorker's note below.
    // ------------------------------------------------------------------
    // 2026-09-21: closest analogue of the retired process-global
    // g_activeThreads fiber counter. Counts guest threads that are not
    // Dormant, read from the same mutex-guarded snapshot() the debug UI uses.
    //
    // ⚠️ NOT numerically identical to the old counter: this includes the main
    // thread, which was never a fiber. Use it only for RELATIVE comparisons
    // (before/after, ">= 1"). For "everything drained" assertions use
    // drainedWithin()/isIdle() instead -- that is the sanctioned drain check
    // and it does not depend on whether main is counted.
    inline int activeGuestThreads(PS2Runtime &rt)
    {
        int live = 0;
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.status != EeThreadStatus::Dormant)
            {
                ++live;
            }
        }
        return live;
    }

    // 2026-09-21: replacements for the deleted g_threads map (which the tests
    // read under g_thread_map_mutex) and interrupt_state::g_vsync_waitList.
    // Both are now facets of EeScheduler's mutex-guarded snapshot(), so no
    // external lock is needed.
    inline bool guestThreadExists(PS2Runtime &rt, int tid)
    {
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.id == tid)
            {
                return true;
            }
        }
        return false;
    }

    // "Terminated" used to be ThreadInfo::terminated. A terminated thread now
    // either leaves the map entirely or is left Dormant.
    inline bool guestThreadTerminated(PS2Runtime &rt, int tid)
    {
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.id == tid)
            {
                return th.status == EeThreadStatus::Dormant;
            }
        }
        return true; // gone from the map
    }

    // ThreadInfo::suspendCount's replacement. NOTE: snapshot() copies the
    // whole thread table under a mutex, so a tight poll on this is heavier
    // than the old direct field read -- the T-DEF2a race window it is used to
    // catch is correspondingly harder to hit. Kept as a poll (not widened)
    // because narrowing coverage silently would be worse than a flaky-slower
    // probe; if it stops catching the race, that is a real finding to record.
    inline int guestThreadSuspendCount(PS2Runtime &rt, int tid)
    {
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.id == tid)
            {
                return th.suspendCount;
            }
        }
        return -1;
    }

    inline bool isWaitingOnVSync(PS2Runtime &rt, int tid)
    {
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.id == tid && th.waitReason == EeWaitReason::VSync)
            {
                return true;
            }
        }
        return false;
    }

    inline int vsyncWaiterCount(PS2Runtime &rt)
    {
        int n = 0;
        for (const EeThreadSnapshot &th : rt.eeScheduler().snapshot().threads)
        {
            if (th.waitReason == EeWaitReason::VSync)
            {
                ++n;
            }
        }
        return n;
    }

    // 2026-09-21: taking the mutex was only half of it. EeScheduler still
    // demanded that every syscall come from the adopted executor thread
    // (bindMainContextForSyscall -> assertExecutor), so a host thread issuing
    // one aborted the process -- four Scheduler suites died there. The mutex
    // makes the borrow SAFE (run() holds the same mutex around its guest
    // dispatch); beginBorrowedWorker() makes it LEGAL, and gives the caller a
    // negative pseudo-thread id so a self-targeting syscall is an error rather
    // than an operation on the game's main thread.
    //
    // The token is thread_local because hostGuestBegin/End are a separate
    // lock/unlock pair rather than a scope -- one borrow in flight per thread,
    // which is what the mutex already guarantees.
    inline EeScheduler::BorrowedWorkerToken &hostBorrowToken()
    {
        static thread_local EeScheduler::BorrowedWorkerToken token;
        return token;
    }

    // 2026-09-22 -- initialises the scheduler and claims the executor for THIS
    // thread, for a test whose first scheduler contact is a BORROW.
    //
    // EeScheduler is bound lazily, by bindMainContextForSyscall()'s
    // `if (m_executorThread == thread::id{}) reset(rdram, ctx)` branch. Almost
    // every test reaches that branch by accident: startSchedWorker() calls
    // ps2_syscalls::CreateThread DIRECTLY, unborrowed, on the test thread, so
    // the first syscall of the test performs the bind.
    //
    // A test that opens with hostGuestBegin() never does. beginBorrowedWorker()
    // stores this thread into m_executorThread WITHOUT resetting, so the bind
    // branch is already closed by the time the first syscall arrives; and
    // endBorrowedWorker() then restores the value captured before the borrow,
    // which is the EMPTY thread id. The scheduler is left permanently unbound
    // and unowned, so every later pumpGuest() fails its onExecutorThread()
    // test and silently does nothing -- no fiber in the test ever runs an
    // instruction. That is the same silent-no-op shape as the null pump target
    // (see PumpTargetScope), reached by a different route.
    //
    // Call it BEFORE the first hostGuestBegin(). reset() wipes threads,
    // semaphores and event flags, so it must be the first scheduler contact;
    // the snapshot test below enforces that by doing nothing once a bind has
    // happened (reset() ends in publishSnapshot(), and it is reset() that mints
    // the main thread, so an empty thread list means "never bound").
    inline void ensureSchedulerBound(PS2Runtime &rt, std::vector<uint8_t> &ram)
    {
        if (!rt.eeScheduler().snapshot().threads.empty())
        {
            return;
        }
        // Zero-initialised exactly like startSchedWorker's own createCtx: reset()
        // COPIES it into the main GuestThread (main.context = mainContext), so a
        // stack local is safe and pc/$sp/$gp of 0 are what the bind path
        // already sees everywhere else in this file.
        R5900Context bindCtx{};
        rt.eeScheduler().bindMainContextForSyscall(bindCtx, ram.data());
    }

    inline void hostGuestBegin(PS2Runtime &rt)
    {
        // Lock order is executorMutex -> hostInvocationMutex, everywhere.
        rt.eeScheduler().executorMutex().lock();
        rt.eeScheduler().hostInvocationMutex().lock();
        hostBorrowToken() = rt.eeScheduler().beginBorrowedWorker();
    }

    inline void hostGuestEnd(PS2Runtime &rt)
    {
        rt.eeScheduler().endBorrowedWorker(hostBorrowToken());
        rt.eeScheduler().hostInvocationMutex().unlock();
        rt.eeScheduler().executorMutex().unlock();
    }

    struct HostGuestScope
    {
        PS2Runtime &rt;
        EeScheduler::BorrowedWorkerToken token;
        explicit HostGuestScope(PS2Runtime &r) : rt(r)
        {
            // Lock order is executorMutex -> hostInvocationMutex, everywhere.
            rt.eeScheduler().executorMutex().lock();
            rt.eeScheduler().hostInvocationMutex().lock();
            token = rt.eeScheduler().beginBorrowedWorker();
        }
        HostGuestScope(const HostGuestScope &) = delete;
        HostGuestScope &operator=(const HostGuestScope &) = delete;
        ~HostGuestScope()
        {
            rt.eeScheduler().endBorrowedWorker(token);
            rt.eeScheduler().hostInvocationMutex().unlock();
            rt.eeScheduler().executorMutex().unlock();
        }
    };

    // 2026-09-21: tests call this from TWO kinds of host thread.
    //
    //   * The test's own thread, which EeScheduler::reset() adopted as the
    //     executor. Nothing to do -- this is the path every currently-passing
    //     scheduler test already takes, and it is left byte-identical.
    //   * A std::thread the test spawned (SchedulerWindow's signaler). Those
    //     are NOT the executor, so every syscall they issued went
    //     bindMainContextForSyscall() -> assertExecutor() -> abort, killing the
    //     whole process mid-suite.
    //
    // A foreign thread therefore borrows a worker for the duration: the mutex
    // excludes run()'s guest dispatch (it takes the same mutex around its
    // function() call), and beginBorrowedWorker() makes the call legal.
    //
    // Deliberately NOT unconditional. On the executor thread it would both
    // recursively lock a non-recursive std::mutex AND swap the caller's real
    // thread id for a negative pseudo-tid, which changes the meaning of every
    // self-targeting syscall the passing tests rely on.
    inline int32_t callSyscall(PS2Runtime &rt, std::vector<uint8_t> &ram, SyscallFn fn,
                                uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        if (rt.eeScheduler().onExecutorThread())
        {
            return callSyscallRaw(rt, ram, fn, a0, a1, a2);
        }
        HostGuestScope borrow(rt);
        return callSyscallRaw(rt, ram, fn, a0, a1, a2);
    }

    // 2026-09-22 -- for the syscalls that return their value through the
    // SCHEDULER's context rather than the one they were handed.
    //
    // Two conventions exist side by side in the runtime. Most syscalls do
    // setReturnS32(ctx, ...) on the context passed in -- PollSema, CreateEventFlag
    // and friends in Sync.cpp. The blocking ones instead write
    // setReturnS32(&self->activeContext(), ...): EeScheduler::waitSemaphore()
    // does it on both of its paths (EeScheduler.cpp, the KE_UNKNOWN_SEMID
    // early-out and the count != 0 consume).
    //
    // In the GAME those two are the same object, so the difference is
    // invisible: run() dispatches guest code as
    // `function(m_rdram, &context, ...)` where
    // `R5900Context &context = running->activeContext();`
    // (EeScheduler.cpp:1031). A syscall reached from guest code is therefore
    // always handed the very context activeContext() resolves to.
    //
    // A BORROWED HOST WORKER breaks that aliasing. callSyscallRaw() builds a
    // fresh R5900Context on the host stack, and bindMainContextForSyscall()
    // binds the passed ctx only on the first-ever syscall (the reset() branch)
    // -- for a borrow it just asserts and returns. So waitSemaphore()'s result
    // lands in the pseudo-thread's context and the caller's local ctx keeps
    // its zero-initialised $v0. That is the whole of W1 and R7: both had
    // already passed "completed", "did not throw" and "the fiber produced the
    // permit", and failed only on the returned value, which was 0.
    //
    // This reads the result back from where the runtime actually put it. It is
    // deliberately NOT folded into callSyscall(): a generic helper cannot know
    // which of the two conventions a given syscall used, and guessing wrong
    // would return a stale scheduler-side value for every syscall that
    // correctly wrote the passed ctx.
    //
    // Must be called while the borrow is still held -- currentThread() has to
    // still be the pseudo-thread -- i.e. between hostGuestBegin/hostGuestEnd.
    inline int32_t callSyscallSchedResult(PS2Runtime &rt, std::vector<uint8_t> &ram, SyscallFn fn,
                                          uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        R5900Context ctx{};
        setRegU32(ctx, 4, a0);
        setRegU32(ctx, 5, a1);
        setRegU32(ctx, 6, a2);
        fn(ram.data(), &ctx, &rt);
        if (const R5900Context *sched = rt.eeScheduler().currentContext())
        {
            return getRegS32(*sched, 2);
        }
        return getRegS32(ctx, 2);
    }

    struct ParkedHostWorker
    {
        std::atomic<bool> acquired{false}, done{false};
        std::thread th;
        explicit ParkedHostWorker(EeScheduler &scheduler) : th([this, &scheduler]
        {
            {
                parkedHostWorkerWaiters().fetch_add(1, std::memory_order_release);
                std::lock_guard<std::mutex> lock(scheduler.hostInvocationMutex());
                parkedHostWorkerWaiters().fetch_sub(1, std::memory_order_release);
                acquired.store(true, std::memory_order_release);
            }
            done.store(true, std::memory_order_release);
        })
        {
        }
        bool wonTokenWithin(std::chrono::milliseconds t)
        {
            return waitUntil([&] { return acquired.load(std::memory_order_acquire); }, t);
        }
        bool finishedWithin(std::chrono::milliseconds t)
        {
            return waitUntil([&] { return done.load(std::memory_order_acquire); }, t);
        }
        ~ParkedHostWorker()
        {
            if (th.joinable())
            {
                th.join();
            }
        }
    };
}
