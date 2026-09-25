// ===========================================================================
// PORTED to the per-runtime EeScheduler API on 2026-09-21 (was disabled behind
// PS2X_SCHED_TESTS_PORTED since the Phase 3d/3e refactor deleted ps2sched).
//
// Mapping used throughout this file:
//   ps2sched::scheduler_init()      -> deleted; no process-global scheduler.
//   ps2sched::scheduler_shutdown()  -> runtime.requestStop() (SchedFixture's
//                                      destructor already does this).
//   g_activeThreads.load() == 0     -> drainedWithin(runtime, timeout), i.e.
//                                      EeScheduler::isIdle().
//   ps2sched::host_token_waiters()  -> ps2x_test::parkedHostWorkerWaiters();
//                                      the guest token became a plain
//                                      std::mutex, so the waiter publishes its
//                                      own intent (see SchedTestSupport.h).
//   ParkedHostWorker w;             -> ParkedHostWorker w(runtime.eeScheduler()).
//   g_thread_map_mutex/g_nextThreadId
//                                   -> createDormantWorkerWithId(), which walks
//                                      the private allocator from outside.
//
// Two semantic notes, both flagged at their sites:
//   * G1's original failure mode (requestStop joining a parked worker) is now
//     STRUCTURALLY IMPOSSIBLE -- PS2Runtime::requestStop() is signal-only. The
//     test is kept to pin the surviving invariant, not the old bug.
//   * ParkedHostWorker's bounded-acquisition fairness is NOT a verified
//     equivalent of the old token's starvation gate (SchedTestSupport.h
//     documents this). H1-H3 assert bounded acquisition and are the tests that
//     would expose it if the plain mutex turns out to be unfair in practice.
// ===========================================================================

// ---------------------------------------------------------------------------
// Scheduler / runtime regression tests grounded in how real recompiled PS2
// guests exercise the N=1 fiber scheduler. Each suite here targets a bug that
// the synthetic scheduler suites cannot see because their fibers
// are well-behaved (they yield cooperatively, park promptly, and shut down
// from the main thread). Real recompiled guests spin across function
// dispatches, monopolize the executor, and call requestStop() from guest
// context — these tests reproduce those shapes with bounded waits (every wait
// has a timeout and an escape hatch: a regression FAILS, it does not hang).
// ---------------------------------------------------------------------------

#include "MiniTest.h"
#include "SchedTestSupport.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2_runtime_macros.h"
#include "Kernel/Syscalls/Interrupt.h"
#include "Kernel/Syscalls/System.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Kernel/Syscalls/Helpers/State.h" // g_threads / g_thread_map_mutex / g_activeThreads

using namespace ps2_syscalls;
using namespace ps2x_test;

namespace
{
    // These helpers move test control/result words between the test main
    // thread and guest fibers (which may run on other OS threads under the
    // pthread fiber backend). Use atomic accesses so that cross-thread
    // traffic is well-defined and the suite runs clean under TSan; the
    // addresses used are all 4-byte aligned, as std::atomic_ref requires.
    void rdramWrite32Raw(uint8_t *rdram, uint32_t addr, uint32_t val)
    {
        std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(rdram + addr)).store(val);
    }

    uint32_t rdramRead32Raw(const uint8_t *rdram, uint32_t addr)
    {
        return std::atomic_ref<uint32_t>(
                   *reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(rdram) + addr))
            .load();
    }

    void rdramWrite32(std::vector<uint8_t> &rdram, uint32_t addr, uint32_t val)
    {
        rdramWrite32Raw(rdram.data(), addr, val);
    }

    // rdramRead32 (vector-based) now lives in SchedTestSupport.h, shared with
    // the poll helpers there (waitForWord/waitForWordAtLeast/drainedWithin).

    // Create a semaphore via CreateSema (EE layout: count, max_count, init_count).
    int32_t createSchedSema(uint8_t *rdram, PS2Runtime *runtime, int initCount, int maxCount)
    {
        constexpr uint32_t kSemaParamAddr = 0x2F40u;
        const uint32_t params[6] = {
            0u,
            static_cast<uint32_t>(maxCount),
            static_cast<uint32_t>(initCount),
            0u,
            0u,
            0u,
        };
        std::memcpy(rdram + kSemaParamAddr, params, sizeof(params));

        R5900Context cCtx{};
        setRegU32(cCtx, 4, kSemaParamAddr);
        ps2_syscalls::CreateSema(rdram, &cCtx, runtime);
        return getRegS32(cCtx, 2);
    }

    void deleteSchedSema(uint8_t *rdram, PS2Runtime *runtime, int32_t sid)
    {
        if (sid <= 0)
        {
            return;
        }
        R5900Context dCtx{};
        setRegU32(dCtx, 4, static_cast<uint32_t>(sid));
        ps2_syscalls::DeleteSema(rdram, &dCtx, runtime);
    }

    void signalSchedSema(uint8_t *rdram, PS2Runtime *runtime, int32_t sid)
    {
        if (sid <= 0)
        {
            return;
        }
        R5900Context sCtx{};
        setRegU32(sCtx, 4, static_cast<uint32_t>(sid));
        ps2_syscalls::SignalSema(rdram, &sCtx, runtime);
    }

    // Create + start a fiber worker thread; returns tid or -1.
    int32_t startSchedWorker(uint8_t *rdram, PS2Runtime *runtime,
                             uint32_t entryAddr, int priority,
                             uint32_t stackAddr, uint32_t stackSize)
    {
        constexpr uint32_t kThreadParamAddr = 0x2E40u;
        const uint32_t threadParam[7] = {
            0u,
            entryAddr,
            stackAddr,
            stackSize,
            0u,
            static_cast<uint32_t>(priority),
            0u,
        };
        std::memcpy(rdram + kThreadParamAddr, threadParam, sizeof(threadParam));

        R5900Context createCtx{};
        setRegU32(createCtx, 4, kThreadParamAddr);
        ps2_syscalls::CreateThread(rdram, &createCtx, runtime);
        const int32_t tid = getRegS32(createCtx, 2);
        if (tid <= 0)
        {
            return -1;
        }

        R5900Context startCtx{};
        setRegU32(startCtx, 4, static_cast<uint32_t>(tid));
        setRegU32(startCtx, 5, 0u);
        ps2_syscalls::StartThread(rdram, &startCtx, runtime);
        if (getRegS32(startCtx, 2) != 0)
        {
            return -1;
        }
        return tid;
    }

    // ------------------------------------------------------------------
    // 2026-09-21: the two halves of startSchedWorker, split so R2 can force
    // GENUINE thread-id reuse.
    //
    // The old test seeded the allocator directly (`g_nextThreadId = T1` under
    // g_thread_map_mutex). EeScheduler::allocateThreadId() is private and
    // m_nextThreadId has no public seam, so instead we drive the allocator the
    // way a guest would: it marches monotonically over [2, 255] with wrap,
    // skipping live ids (EeScheduler.cpp, allocateThreadId), so creating
    // dormant scratch threads and KEEPING them live walks it forward until it
    // hands back the freed id we want. Bounded by the 254-id range.
    // ------------------------------------------------------------------
    int32_t createDormantWorker(uint8_t *rdram, PS2Runtime *runtime,
                                uint32_t entryAddr, int priority,
                                uint32_t stackAddr, uint32_t stackSize)
    {
        constexpr uint32_t kThreadParamAddr = 0x2E40u;
        const uint32_t threadParam[7] = {
            0u, entryAddr, stackAddr, stackSize, 0u,
            static_cast<uint32_t>(priority), 0u,
        };
        std::memcpy(rdram + kThreadParamAddr, threadParam, sizeof(threadParam));

        R5900Context createCtx{};
        setRegU32(createCtx, 4, kThreadParamAddr);
        ps2_syscalls::CreateThread(rdram, &createCtx, runtime);
        return getRegS32(createCtx, 2);
    }

    bool startExistingWorker(uint8_t *rdram, PS2Runtime *runtime, int32_t tid)
    {
        // See startSchedWorker: thread ids are reused, so drop any step history a
        // terminated predecessor left on this tid. R2 forces this case on purpose.
        schedStepResetThread(tid);
        R5900Context startCtx{};
        setRegU32(startCtx, 4, static_cast<uint32_t>(tid));
        setRegU32(startCtx, 5, 0u);
        ps2_syscalls::StartThread(rdram, &startCtx, runtime);
        return getRegS32(startCtx, 2) == 0;
    }

    void deleteWorkerById(uint8_t *rdram, PS2Runtime *runtime, int32_t tid)
    {
        R5900Context d{};
        setRegU32(d, 4, static_cast<uint32_t>(tid));
        ps2_syscalls::DeleteThread(rdram, &d, runtime);
    }

    // Walks the id allocator until it hands back `wanted` (which must already
    // be free). Returns the dormant thread now occupying `wanted`, created
    // with the given entry/stack so the caller can just StartThread it; or -1.
    // Scratch ids consumed along the way are released before returning.
    int32_t createDormantWorkerWithId(uint8_t *rdram, PS2Runtime *runtime, int32_t wanted,
                                      uint32_t entryAddr, int priority,
                                      uint32_t stackAddr, uint32_t stackSize)
    {
        std::vector<int32_t> scratch;
        int32_t got = -1;
        for (int attempts = 0; attempts < 300; ++attempts)
        {
            const int32_t id = createDormantWorker(rdram, runtime, entryAddr, priority,
                                                   stackAddr, stackSize);
            if (id <= 0)
            {
                break;
            }
            if (id == wanted)
            {
                got = id;
                break;
            }
            scratch.push_back(id);
        }
        for (const int32_t id : scratch)
        {
            deleteWorkerById(rdram, runtime, id);
        }
        return got;
    }

    // ------------------------------------------------------------------
    // Control words (kernel-area scratch, disjoint from other suites)
    // ------------------------------------------------------------------
    constexpr uint32_t kThStop  = 0x00060000u; // 1 => guest loops exit
    constexpr uint32_t kThSidX  = 0x00060010u; // ping-pong sema X
    constexpr uint32_t kThSidY  = 0x00060014u; // ping-pong sema Y
    constexpr uint32_t kThFlag  = 0x00060020u; // starvation probe flag
    constexpr uint32_t kThCount = 0x00060024u; // ping-pong loop counter
    constexpr uint32_t kThSent  = 0x00060028u; // DMAC handler sentinel

    // ------------------------------------------------------------------
    // Guest step functions (registered as recompiled functions)
    // ------------------------------------------------------------------

    // Ping-pong pair: one permit circulates between semas X and Y, so the run
    // queue is (almost) never empty and the executor never runs out of ready
    // fibers — the shape that starved parked host workers before the
    // g_host_token_waiters executor gate.
    static void stepPingA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        // The stop word is read through step.value() so the replayed prefix of
        // this loop takes the SAME branch it took originally -- a raw read would
        // see a flag the test has since set and break out mid-replay.
        while (step.value([&] { return rdramRead32Raw(rdram, kThStop) == 0u ? 1 : 0; }) != 0)
        {
            const int32_t sidX = static_cast<int32_t>(rdramRead32Raw(rdram, kThSidX));
            const int32_t sidY = static_cast<int32_t>(rdramRead32Raw(rdram, kThSidY));
            step.call(ps2_syscalls::WaitSema, static_cast<uint32_t>(sidX));
            step.once([&] { rdramWrite32Raw(rdram, kThCount, rdramRead32Raw(rdram, kThCount) + 1u); });
            step.call(ps2_syscalls::SignalSema, static_cast<uint32_t>(sidY));
        }
        step.finish();
    }

    // A guest loop that never blocks and never returns to the dispatch loop:
    // its only scheduling hook is the recompiler-emitted back-edge call to
    // shouldPreemptGuestExecution() (yield_point). Without yield_point step 4
    // this fiber holds the executor inside ps2fiber_resume forever and a
    // parked host worker starves no matter what the executor predicate does.
    static void stepSpinShouldPreempt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        while (rdramRead32Raw(rdram, kThStop) == 0u)
        {
            rdramWrite32Raw(rdram, kThCount, rdramRead32Raw(rdram, kThCount) + 1u);
            SCHED_YIELD_POINT(runtime); // recompiled back-edge hook
        }
        ctx->pc = 0u;
    }

    // Cross-dispatch spin pair: each function returns to the dispatch loop
    // with ctx->pc aimed at the other. Neither body ever calls the emitted
    // back-edge hook (mimicking generated code whose loop back-edge is a
    // call/return across function boundaries), so the ONLY place a yield can
    // happen is the dispatch loop itself.
    static void stepCrossDispatchA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rdramWrite32Raw(rdram, kThCount, rdramRead32Raw(rdram, kThCount) + 1u);
        ctx->pc = (rdramRead32Raw(rdram, kThStop) == 0u) ? 0x00700400u : 0u;
    }

    static void stepCrossDispatchB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = (rdramRead32Raw(rdram, kThStop) == 0u) ? 0x00700300u : 0u;
    }

    // RPC-server shape (sceSifRpcInit + sceSifSetRpcQueue + sceSifRpcLoop):
    // the generated caller leaves ctx->pc at the loop entry, so if the stub
    // returns without blocking the dispatch loop re-enters it forever.
    static void stepRpcServerLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_stubs::sceSifRpcLoop(rdram, ctx, runtime);
        // ctx->pc intentionally left at this function's address: the real
        // guest loop re-enters sceSifRpcLoop after any (stray) wakeup.
    }

    static void stepWriteFlagAndExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rdramWrite32Raw(rdram, kThFlag, 1u);
        ctx->pc = 0u;
    }

    // Guest-context stop shape: spin (no yields) until a host worker is parked
    // for the guest token, then call requestStop() from inside the guest
    // thread - exactly what the unimplemented-function default handler does
    // when a guest thread faults. kThStop is a spin escape hatch.
    //
    // 2026-09-21: was `ps2sched::host_token_waiters() == 0`, waiting on the
    // REAL interrupt worker. EeScheduler serialises host invocations with a
    // plain std::mutex, which cannot report its blocked waiters, so G1 now
    // constructs an explicit ParkedHostWorker and we poll its published
    // intent. Same shape; the thing being waited for is now observable.
    static void stepGuestContextStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        while (ps2x_test::parkedHostWorkerWaiters().load(std::memory_order_acquire) == 0 &&
               rdramRead32Raw(rdram, kThStop) == 0u)
        {
            // busy spin: no yield points, so the parked worker stays parked
        }
        runtime->requestStop(); // notifyRuntimeStop() from GUEST context
        rdramWrite32Raw(rdram, kThFlag, 1u);
        ctx->pc = 0u;
    }

    // DMAC handler body: bump the sentinel and finish.
    static void stepDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rdramWrite32Raw(rdram, kThSent, rdramRead32Raw(rdram, kThSent) + 1u);
        ctx->pc = 0u;
    }

    // sceSifSetDma shape: a guest syscall path that dispatches DMAC handlers
    // SYNCHRONOUSLY from the fiber servicing the syscall.
    static void stepDmacFromGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);
        rdramWrite32Raw(rdram, kThFlag, 1u);
        ctx->pc = 0u;
    }

    static void stepPingB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        while (step.value([&] { return rdramRead32Raw(rdram, kThStop) == 0u ? 1 : 0; }) != 0)
        {
            const int32_t sidX = static_cast<int32_t>(rdramRead32Raw(rdram, kThSidX));
            const int32_t sidY = static_cast<int32_t>(rdramRead32Raw(rdram, kThSidY));
            step.call(ps2_syscalls::WaitSema, static_cast<uint32_t>(sidY));
            step.call(ps2_syscalls::SignalSema, static_cast<uint32_t>(sidX));
        }
        step.finish();
    }

    // ------------------------------------------------------------------
    // SchedulerRecoveryIsolation helpers
    //
    // The diagnostic dispatch-trace ring (formerly a single process-wide
    // `thread_local DispatchHistory`) is now owned per-fiber by FiberContext.
    // These step functions call PS2Runtime::debugCurrentDispatchTrace() -
    // which resolves to whichever fiber (or per-OS-thread fallback) is
    // running on the calling thread - so they MUST run inside a fiber's step
    // function. Calling it from the test/main thread instead would silently
    // observe the host fallback ring, not any fiber's history, and the tests
    // would pass vacuously without ever exercising the fix. All assertions
    // are therefore made on rdram words written by the fibers themselves;
    // the main thread only reads rdram after the fibers finish.
    // ------------------------------------------------------------------

    // No-op registrant for marker PCs: never dispatched via ctx->pc, only
    // looked up directly (lookupFunction) to push a PC into the calling
    // fiber's dispatch-trace ring. Registered purely to avoid the noisy
    // "no exact recompiled function" cerr log that an unregistered PC would
    // trigger (harmless to test state, but kept clean per the design's risk
    // list).
    static void stepNoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram; (void)ctx; (void)runtime;
    }

    // Fiber A: push its own marker, hand off to B via sema, then park until B
    // hands back. On resume A inspects its OWN trace. A fiber resuming out of
    // WaitSema continues at the point of the call - it does NOT re-enter
    // dispatchLoop - so the only writer into A's ring between A's push and
    // A's post-resume read is B, and (under the fix) B writes into its OWN
    // ring, never A's.
    static void stepIsoA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        // The marker push is a step: re-entering after the park below would
        // otherwise push it a second time and change the ring this test reads.
        step.once([&] { runtime->lookupFunction(0x00700A80u); }); // push A's marker into A's ring

        step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, 0x00061004u)); // sidB -- let B run
        step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, 0x00061000u));   // sidA -- park; B runs meanwhile

        step.once([&]
        {
            const std::string trace = runtime->debugCurrentDispatchTrace(); // A resumes: inspect A's own ring
            rdramWrite32Raw(rdram, 0x00061010u,
                (trace.find("700b80") != std::string::npos ||
                 trace.find("700b00") != std::string::npos) ? 1u : 0u); // kRiAForeign
            rdramWrite32Raw(rdram, 0x00061018u,
                (trace.find("700a80") != std::string::npos) ? 1u : 0u); // kRiAOwn
        });
        step.finish();
    }

    // Fiber B: wait for A's handoff, push its own marker, inspect its OWN
    // trace, then signal A back.
    static void stepIsoB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, 0x00061004u)); // sidB

        step.once([&]
        {
            runtime->lookupFunction(0x00700B80u); // push B's marker into B's ring
            const std::string trace = runtime->debugCurrentDispatchTrace();
            rdramWrite32Raw(rdram, 0x00061014u,
                (trace.find("700a80") != std::string::npos ||
                 trace.find("700a00") != std::string::npos) ? 1u : 0u); // kRiBForeign
            rdramWrite32Raw(rdram, 0x0006101Cu,
                (trace.find("700b80") != std::string::npos) ? 1u : 0u); // kRiBOwn
        });

        step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, 0x00061000u)); // sidA -- wake A
        step.finish();
    }

    // R2 probe: same entry PC for both the original fiber (run==0) and the
    // fiber that reuses its tid after teardown (run==1). Both fibers get
    // 0x00700C00 pushed into THEIR OWN ring automatically by dispatchLoop
    // before this body runs, so "the ring started empty" cannot be tested as
    // trace=="(empty)" (it never is, even under the fix). Instead we check
    // whether the ring, at entry, already contains the OTHER fiber's distinct
    // marker (0x00700C80, pushed only by run==0 after this check) - that is
    // the signal that would only appear via cross-fiber leakage of the old
    // shared thread_local.
    static void stepReuseProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kReuseMarker = 0x00700C80u;
        const uint32_t run = rdramRead32Raw(rdram, 0x00062000u); // kReuseRun
        const bool inheritedPrevMarker =
            (runtime->debugCurrentDispatchTrace().find("700c80") != std::string::npos);

        if (run == 0u)
        {
            rdramWrite32Raw(rdram, 0x00062004u, inheritedPrevMarker ? 0u : 1u); // kReuseNotInherited0
            runtime->lookupFunction(kReuseMarker);
            rdramWrite32Raw(rdram, 0x00062008u,
                (runtime->debugCurrentDispatchTrace().find("700c80") != std::string::npos) ? 1u : 0u); // kReuseNonEmpty0
        }
        else
        {
            rdramWrite32Raw(rdram, 0x0006200Cu, inheritedPrevMarker ? 0u : 1u); // kReuseNotInherited1
        }
        ctx->pc = 0u;
    }

    // ------------------------------------------------------------------
    // SchedulerStackIsolation control words and addresses (disjoint from
    // other suites, which use <= 0x00062xxx)
    // ------------------------------------------------------------------
    constexpr uint32_t kSiSemA     = 0x00063000u; // ping-pong sema A
    constexpr uint32_t kSiSemB     = 0x00063004u; // ping-pong sema B
    constexpr uint32_t kSiSysA     = 0x00063008u; // override syscall number, fiber A
    constexpr uint32_t kSiSysB     = 0x0006300Cu; // override syscall number, fiber B
    constexpr uint32_t kSiTopA     = 0x00063010u; // B1: invoke $sp seen by A
    constexpr uint32_t kSiTopB     = 0x00063014u; // B1: invoke $sp seen by B
    constexpr uint32_t kSiDmacTopA = 0x00063018u; // B3: handler $sp seen by A
    constexpr uint32_t kSiNestTopOuter = 0x00063020u; // I2: $sp of the depth-0 invocation
    constexpr uint32_t kSiNestTopInner = 0x00063024u; // I2: $sp of the depth-1 invocation

    // ---- I1 (B1): RPC/override invoke stack isolation ----------------------
    // Invoked BY rpcInvokeFunction; ctx is its internal `tmp`, so $29 is the
    // scratch-stack top and $31 is rpcInvokeFunction's return sentinel.
    static void stepInvokeRecordA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        const uint32_t retSentinel = getRegU32(ctx, 31);
        step.once([&] { rdramWrite32Raw(rdram, kSiTopA, getRegU32(ctx, 29)); }); // record A's scratch top
        step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, kSiSemB));     // let B proceed
        step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, kSiSemA));       // PARK inside invoke; A's stack stays live
        step.finishAt(retSentinel);                              // resume: return to sentinel -> invoke loop ends
    }
    static void stepInvokeRecordB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        const uint32_t retSentinel = getRegU32(ctx, 31);
        step.once([&] { rdramWrite32Raw(rdram, kSiTopB, getRegU32(ctx, 29)); }); // A's scratch STILL live here
        step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, kSiSemA));     // wake A
        step.finishAt(retSentinel);
    }
    static void stepInvokeEntryA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // 2026-09-22 -- this was the ONLY step function in the suite with no
        // SchedStep, and that is what failed I1's drain.
        //
        // The invoke below parks inside stepInvokeRecordA (WaitSema on semA),
        // which throws out of this frame. When B wakes A the invocation resumes,
        // finishes and POPS -- and the thread's own pc is still this entry, so
        // run() re-dispatches stepInvokeEntryA FROM THE TOP. With no step record
        // it issued a SECOND dispatchNumericSyscall: a fresh invoke that signals
        // semB (B has already exited, nobody is waiting) and then parks on semA
        // with nobody left to signal it. The fiber never exits, so drainedWithin()
        // times out.
        //
        // stepInvokeEntryB survived the identical re-dispatch only because it
        // already had a SchedStep to replay. Re-dispatching at the entry is
        // CORRECT scheduler behaviour -- a host step function has no mid-function
        // pc to resume at, which is the whole reason SchedStep exists -- so this
        // is a test defect, not a runtime one.
        SchedStep step(rdram, ctx, runtime);
        step.once([&]
        {
            ps2_syscalls::dispatchNumericSyscall(rdramRead32Raw(rdram, kSiSysA), rdram, ctx, runtime);
        });
        step.finish();
    }
    static void stepInvokeEntryB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, kSiSemB)); // wait until A recorded + parked
        step.once([&]
        {
            ps2_syscalls::dispatchNumericSyscall(rdramRead32Raw(rdram, kSiSysB), rdram, ctx, runtime);
        });
        step.finish();
    }

    // ---- I2: invocation stacks are reserved per {thread, depth} -----------
    //
    // 2026-09-22 -- restructured. The previous version had two fibers ping-pong
    // through INLINE DMAC dispatch: handler A recorded its $sp, signalled semB
    // and then parked on semA *inside interrupt context*, so that handler B
    // would record its own $sp while A's handler stack was still live.
    //
    // That shape is not reachable under EeScheduler, and the trace says so
    // rather than the code review: dispatchIrq() only QUEUES (queueInvocation
    // -> m_pendingInvocations), so entry A (tid 2) went dormant at eeCycle=8
    // with its handler not yet run, and the queued invocation was then pushed
    // onto whichever thread ran next -- tid 3, entry B's thread
    // (EeScheduler.cpp:1528). Handler A parked there
    // ([semwatch:blockcurrent] #2 tid=3 reason=2 waitId=1 invocationsSize=1
    // pc=0x701600), starving entry B's own body, so cause 6 was never
    // dispatched, handler B never ran and nobody ever signalled semA. Third
    // stale-design test found this week, after U2 and U3/U4.
    //
    // The CONTRACT underneath is live, so this is a restructure and not a
    // retirement. EeScheduler::invocationStackTop() (EeScheduler.cpp:3399)
    // reserves a stack keyed by
    //
    //     {owner->id, owner->invocations.size()}
    //
    // i.e. by thread AND by nesting depth, with the key taken BEFORE the
    // push_back. Two invocations that can be live simultaneously therefore
    // differ in at least one component and get different stacks; two at the
    // same {tid, depth} deliberately SHARE, because they cannot overlap.
    //
    // I1 already covers the thread component (two fibers, one depth each).
    // Nothing covered the DEPTH component, which is the half a future refactor
    // is most likely to break -- collapsing the key to just the tid would keep
    // I1 green. So the two halves below are:
    //
    //   1. an interrupt handler runs on a RESERVED stack, not on the stack of
    //      the thread it interrupted (the DMAC subject I2 was written for,
    //      minus the part that required blocking in interrupt context);
    //   2. an invocation nested inside a LIVE one gets a disjoint stack.
    //
    // Neither half blocks, so both are deterministic -- no race to lose and no
    // flake to chase later. The nesting uses the INVOKE path
    // (dispatchSyscallOverride -> invokeCurrent, System.cpp:573) because that
    // is the only path that nests synchronously: it throws EeDispatcherTransfer
    // and the inner invocation runs while the outer is still on the stack. The
    // DMAC path cannot nest, which is exactly what the old test assumed it
    // could.

    // Runs AS a DMAC handler; ctx is the invocation context, so $29 is the
    // stack EeScheduler reserved for it.
    static void stepDmacRecordA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.once([&] { rdramWrite32Raw(rdram, kSiDmacTopA, getRegU32(ctx, 29)); });
        step.finish();
    }

    // Guest thread body: queue the cause-5 handler, then return.
    static void stepDmacDispatchA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.once([&] { ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u); });
        step.finish();
    }

    // Depth-0 invocation: record our own reserved $sp, then invoke a SECOND,
    // DISTINCT override from inside it. Distinct numbers are mandatory, not
    // stylistic -- dispatchSyscallOverride bails on
    // hasInvocation(SyscallOverride, syscallNumber) (System.cpp:544), so
    // re-entering the SAME number would silently not nest at all and the test
    // would pass by measuring nothing.
    static void stepNestOuter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.once([&] { rdramWrite32Raw(rdram, kSiNestTopOuter, getRegU32(ctx, 29)); });
        step.once([&]
        {
            ps2_syscalls::dispatchNumericSyscall(rdramRead32Raw(rdram, kSiSysB), rdram, ctx, runtime);
        });
        step.finish();
    }

    // Depth-1 invocation: record our own reserved $sp. Nothing blocks.
    static void stepNestInner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.once([&] { rdramWrite32Raw(rdram, kSiNestTopInner, getRegU32(ctx, 29)); });
        step.finish();
    }

    // Guest thread body for the nesting half: invoke override A (depth 0).
    static void stepNestEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.once([&]
        {
            ps2_syscalls::dispatchNumericSyscall(rdramRead32Raw(rdram, kSiSysA), rdram, ctx, runtime);
        });
        step.finish();
    }

    // ------------------------------------------------------------------
    // SchedulerOverrideIsolation helpers
    //
    // dispatchSyscallOverride's reentrancy guard (System.cpp) used to be one
    // process-wide `thread_local std::vector`. Under the N=1 fiber scheduler
    // that vector is keyed to the ONE guest executor OS thread, shared by every
    // fiber. So while fiber A is parked INSIDE its override for syscall N (N
    // still on the shared vector because A hasn't returned to pop it), fiber B
    // issuing the same N sees N "active", is treated as reentrant, and its
    // override is SILENTLY SKIPPED in favor of the builtin. The fix moves the
    // stack into FiberContext (per-fiber), so B has its own empty stack and its
    // override runs. Wrong-dispatch, not memory-safety.
    //
    // The handler PARKS via WaitSema from inside rpcInvokeFunction (inside the
    // handler invoke). On ucontext the fiber's whole C++ stack is frozen at the
    // swap, so nesting depth is irrelevant (same mechanism stepIsoA relies on).
    // RED ONLY ON build-ucontext: on the pthread backend each fiber is its own
    // OS thread, the old thread_local is accidentally per-fiber, and this test
    // is vacuously green even unfixed.
    // ------------------------------------------------------------------
    constexpr uint32_t kOvSyscall     = 0x79u;       // free syscall #, benign TODO fallback
    constexpr uint32_t kOvHandler     = 0x00700D00u; // registered override handler
    constexpr uint32_t kOvEntryA      = 0x00700D80u;
    constexpr uint32_t kOvEntryB      = 0x00700E00u;
    constexpr uint32_t kOvSidStartB   = 0x00063020u; // A -> B: "A has parked inside its override"
    constexpr uint32_t kOvSidResumeA  = 0x00063024u; // B -> A: "you may resume"
    constexpr uint32_t kOvAEntered    = 0x00063028u; // A's handler was entered (liveness)
    constexpr uint32_t kOvARan        = 0x0006302Cu; // A's handler resumed & completed (liveness)
    constexpr uint32_t kOvBRan        = 0x00063030u; // B's handler ran => override NOT skipped (THE signal)

    // One handler for both fibers; $a0 selects mode. mode 1 = A (park), 2 = B (mark).
    static void stepOverrideHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // SchedStep first: it restores $a0-$a3 to the values this handler was
        // entered with, so the mode read below survives the park in the mode-1
        // branch. It also keys on (tid, entry pc), so this handler parking while
        // NESTED inside stepOverrideA gets its own step sequence rather than
        // interleaving with its caller's.
        SchedStep step(rdram, ctx, runtime);
        const uint32_t mode = getRegU32(ctx, 4); // $a0, forwarded by rpcInvokeFunction
        const uint32_t retSentinel = getRegU32(ctx, 31);
        if (mode == 1u)
        {
            step.once([&] { rdramWrite32Raw(rdram, kOvAEntered, 1u); });
            step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, kOvSidStartB));  // release B
            step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, kOvSidResumeA));   // PARK: N is on A's override stack
            step.once([&] { rdramWrite32Raw(rdram, kOvARan, 1u); });                   // resumed
        }
        else
        {
            step.once([&] { rdramWrite32Raw(rdram, kOvBRan, 1u); });                   // B's override actually ran
        }
        setReturnU32(ctx, 0u);
        step.finishAt(retSentinel);                          // return through rpcInvoke sentinel
    }

    static void stepOverrideA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // handleSyscall runs the override INLINE, so when the handler parks the
        // throw unwinds through here too -- hence a step, not a bare call.
        SchedStep step(rdram, ctx, runtime);
        step.once([&]
        {
            setRegU32(*ctx, 4, 1u);                           // mode A (park)
            runtime->handleSyscall(rdram, ctx, kOvSyscall);   // -> dispatchSyscallOverride -> handler parks
        });
        step.finish();
    }

    static void stepOverrideB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        step.call(ps2_syscalls::WaitSema, rdramRead32Raw(rdram, kOvSidStartB)); // proceed only after A has parked
        step.once([&]
        {
            setRegU32(*ctx, 4, 2u);                           // mode B (mark)
            runtime->handleSyscall(rdram, ctx, kOvSyscall);   // fix: B's override runs; bug: skipped -> builtin
        });
        step.call(ps2_syscalls::SignalSema, rdramRead32Raw(rdram, kOvSidResumeA)); // wake A regardless -> clean drain
        step.finish();
    }

    // ------------------------------------------------------------------
    // SchedulerJoinStarvation control words + step functions.
    //
    // Reproduces the TerminateThread hang caused by join_fiber flooring the
    // joiner one level BELOW the target (t->priority + 1) instead of AT the
    // target's level. With the +1 floor, an equal-priority sibling that stays
    // runnable after the target exits keeps the joiner (now one level lower)
    // permanently off the run-queue head, so join_fiber never re-observes the
    // target as finished and TerminateThread never returns.
    // ------------------------------------------------------------------
    constexpr uint32_t kJsTargetTid    = 0x00062000u; // int32: target (B) tid, read by joiner A
    constexpr uint32_t kJsJoinReturned = 0x00062004u; // 1 => A's TerminateThread(B) returned
    constexpr uint32_t kJsBStarted     = 0x00062008u; // 1 => target B is running

    // Target B (prio 10): announce running, then spin on the recompiler
    // back-edge hook until terminated. B never self-exits — it is killed by
    // A's TerminateThread(B), which guarantees B is still joinable when A
    // enters join_fiber (so the priority floor is actually exercised). The
    // 2026-09-22: the comment here used to say shouldPreemptGuestExecution()
    // THROWS ThreadExitException once terminateRequested is set. It does not --
    // that was the retired yield_point() contract. checkpointDue() is noexcept
    // and returns a bool, so B must return on it (SCHED_YIELD_POINT) to hand
    // run() back its loop. kThStop is a belt-and-suspenders escape.
    static void stepJoinStarveTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        rdramWrite32Raw(rdram, kJsBStarted, 1u);
        while (rdramRead32Raw(rdram, kThStop) == 0u)
        {
            SCHED_YIELD_POINT(runtime); // back-edge hook: returns on terminate/stop
        }
        ctx->pc = 0u;
    }

    // Joiner A (prio 5, strictly higher than B and the prio-10 pingers):
    // TerminateThread(B) routes through request_terminate + join_fiber. The
    // floor demotes A to the target's level (10, fixed) or one below (11,
    // buggy). Only the fixed floor lets A rotate back to the run-queue head
    // (FIFO among the prio-10 group) to observe B finished and return; the
    // buggy floor strands A below the ever-runnable pingers forever. A sets
    // kJsJoinReturned only AFTER TerminateThread returns.
    static void stepJoinStarveJoiner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SchedStep step(rdram, ctx, runtime);
        const int32_t targetTid = static_cast<int32_t>(rdramRead32Raw(rdram, kJsTargetTid));
        step.call(ps2_syscalls::TerminateThread, static_cast<uint32_t>(targetTid)); // -> join_fiber(targetTid)
        step.once([&] { rdramWrite32Raw(rdram, kJsJoinReturned, 1u); });
        step.finish();
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Token handoff: a host worker parked in async_guest_begin() must win the
// guest token within bounded time even while guest fibers keep the run queue
// non-empty. Regression test for the executor resume predicate: without the
// g_host_token_waiters gate the executor re-resumes fibers without ever
// releasing g_sched_mutex into a cv wait, so the parked worker never runs
// (the VBlank-starvation shape: a guest polling for vsync ticks in a
// semaphore/flag main loop keeps the run queue non-empty, so the starved
// interrupt worker cannot deliver the ticks the guest is waiting on).
// ---------------------------------------------------------------------------
void register_scheduler_token_handoff_tests()
{
    MiniTest::Case("SchedulerTokenHandoff", [](TestCase &tc)
    {
        tc.Run("H1: parked host worker wins the token while fibers ping-pong", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700000u, &stepPingA);
            runtime.registerFunction(0x00700100u, &stepPingB);

            SemaPair semas(rdram.data(), &runtime, createSchedSema, deleteSchedSema,
                           1, 1, 0, 1); // sidX seeded permit, sidY empty
            t.IsTrue(semas.a() > 0 && semas.b() > 0, "H1: semas created");
            if (semas.a() <= 0 || semas.b() <= 0)
            {
                return;
            }
            const int32_t sidX = semas.a();
            const int32_t sidY = semas.b();
            rdramWrite32(rdram, kThStop, 0u);
            rdramWrite32(rdram, kThSidX, static_cast<uint32_t>(sidX));
            rdramWrite32(rdram, kThSidY, static_cast<uint32_t>(sidY));
            rdramWrite32(rdram, kThCount, 0u);

            const int32_t tidA = startSchedWorker(rdram.data(), &runtime,
                                                  0x00700000u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            const int32_t tidB = startSchedWorker(rdram.data(), &runtime,
                                                  0x00700100u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidA > 0 && tidB > 0, "H1: ping-pong fibers started");
            if (tidA <= 0 || tidB <= 0)
            {
                rdramWrite32(rdram, kThStop, 1u);
                signalSchedSema(rdram.data(), &runtime, sidX);
                signalSchedSema(rdram.data(), &runtime, sidY);
                return;
            }

            // Wait until the ping-pong is demonstrably live.
            const bool spinning = waitForWordAtLeast(rdram, kThCount, 3u, std::chrono::milliseconds(2000));
            t.IsTrue(spinning, "H1: ping-pong fibers are running");

            // Host worker (interrupt-worker shape): parks for the guest token.
            ParkedHostWorker worker(runtime.eeScheduler());

            // THE regression assertion: the worker must win the token while the
            // fibers are still ping-ponging (bounded wait — a starved worker
            // fails here instead of hanging the binary).
            const bool acquiredWhileBusy = worker.wonTokenWithin(std::chrono::milliseconds(3000));

            // Escape hatch + orderly teardown (runs regardless of the verdict:
            // once the fibers exit, the executor idles and the worker's
            // async_guest_begin proceeds, so the destructor's join cannot hang).
            rdramWrite32(rdram, kThStop, 1u);
            signalSchedSema(rdram.data(), &runtime, sidX);
            signalSchedSema(rdram.data(), &runtime, sidY);

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            const bool workerFinished = worker.finishedWithin(std::chrono::milliseconds(3000));

            t.IsTrue(acquiredWhileBusy,
                     "H1: parked host worker acquired the guest token while fibers were busy "
                     "(executor must sleep while g_host_token_waiters > 0)");
            t.IsTrue(drained, "H1: ping-pong fibers drained after stop");
            t.IsTrue(workerFinished, "H1: host worker completed");

            const uint32_t loops = rdramRead32(rdram, kThCount);
            t.IsTrue(loops >= 3u, "H1: guest made progress during the test");
        });
        tc.Run("H2: spinning fiber yields at yield_point when a host worker is parked", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700200u, &stepSpinShouldPreempt);
            rdramWrite32(rdram, kThStop, 0u);
            rdramWrite32(rdram, kThCount, 0u);

            const int32_t tid = startSchedWorker(rdram.data(), &runtime,
                                                 0x00700200u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tid > 0, "H2: spin fiber started");
            if (tid <= 0)
            {
                return;
            }

            const bool spinning = waitForWordAtLeast(rdram, kThCount, 1000u, std::chrono::milliseconds(2000));
            t.IsTrue(spinning, "H2: fiber is spinning through yield_point samples");

            ParkedHostWorker worker(runtime.eeScheduler());

            // THE regression assertion: without yield_point step 4 the fiber
            // never leaves ps2fiber_resume, so the worker cannot win the token
            // while the spin is live (bounded wait; escape hatch below).
            const bool acquiredWhileSpinning = worker.wonTokenWithin(std::chrono::milliseconds(3000));

            // Escape hatch + teardown: end the spin; the fiber exits, the
            // executor idles, and the parked worker (if still parked) proceeds.
            rdramWrite32(rdram, kThStop, 1u);

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            const bool workerFinished = worker.finishedWithin(std::chrono::milliseconds(3000));

            t.IsTrue(acquiredWhileSpinning,
                     "H2: parked host worker acquired the token while the fiber was spinning "
                     "(yield_point must yield when host_token_waiters > 0)");
            t.IsTrue(drained, "H2: spin fiber drained after stop");
            t.IsTrue(workerFinished, "H2: host worker completed");
        });
        tc.Run("H3: cross-dispatch spin yields via the dispatch loop preempt hook", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700300u, &stepCrossDispatchA);
            runtime.registerFunction(0x00700400u, &stepCrossDispatchB);
            rdramWrite32(rdram, kThStop, 0u);
            rdramWrite32(rdram, kThCount, 0u);

            const int32_t tid = startSchedWorker(rdram.data(), &runtime,
                                                 0x00700300u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tid > 0, "H3: cross-dispatch fiber started");
            if (tid <= 0)
            {
                return;
            }

            const bool spinning = waitForWordAtLeast(rdram, kThCount, 1000u, std::chrono::milliseconds(2000));
            t.IsTrue(spinning, "H3: fiber is spinning across function dispatches");

            ParkedHostWorker worker(runtime.eeScheduler());

            // THE regression assertion: the function bodies never call the
            // back-edge hook, so only the dispatch-loop preempt can yield.
            const bool acquiredWhileSpinning = worker.wonTokenWithin(std::chrono::milliseconds(3000));

            // Escape hatch + teardown.
            rdramWrite32(rdram, kThStop, 1u);

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            const bool workerFinished = worker.finishedWithin(std::chrono::milliseconds(3000));

            t.IsTrue(acquiredWhileSpinning,
                     "H3: parked host worker acquired the token during a cross-dispatch spin "
                     "(dispatchLoop must call shouldPreemptGuestExecution)");
            t.IsTrue(drained, "H3: cross-dispatch fiber drained after stop");
            t.IsTrue(workerFinished, "H3: host worker completed");
        });
    }); // MiniTest::Case("SchedulerTokenHandoff")
}

// ---------------------------------------------------------------------------
// sceSifRpcLoop must PARK its thread, not return. The real kernel loop is
// `while (1) { SleepThread(); serve; }`; with all SIF RPC HLE'd host-side no
// wakeup ever arrives, so a stub that returns turns the RPC server thread
// into a hot spin that monopolizes the N=1 executor (any SIF-RPC server
// thread — pad, memcard, audio, filesystem — then starves the main thread
// forever).
// ---------------------------------------------------------------------------
void register_scheduler_rpc_loop_park_tests()
{
    MiniTest::Case("SchedulerRpcLoopPark", [](TestCase &tc)
    {
        tc.Run("R1: sceSifRpcLoop parks its fiber and does not monopolize the executor", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700500u, &stepRpcServerLoop);
            runtime.registerFunction(0x00700600u, &stepWriteFlagAndExit);
            rdramWrite32(rdram, kThFlag, 0u);

            // RPC server first (it runs first and, unfixed, never lets go).
            const int32_t serverTid = startSchedWorker(rdram.data(), &runtime,
                                                       0x00700500u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            // Probe fiber at the SAME priority: only a genuine park (not a
            // priority preempt) can let it run.
            const int32_t probeTid = startSchedWorker(rdram.data(), &runtime,
                                                      0x00700600u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(serverTid > 0 && probeTid > 0, "R1: both fibers started");
            if (serverTid <= 0 || probeTid <= 0)
            {
                return;
            }

            // THE regression assertion: the probe fiber runs within bounded
            // time, i.e. the RPC server fiber parked in SleepThread instead of
            // re-entering the stub forever.
            const bool probeRan = waitForWord(rdram, kThFlag, 1u, std::chrono::milliseconds(3000));

            t.IsTrue(probeRan,
                     "R1: equal-priority fiber ran while sceSifRpcLoop was active "
                     "(the RPC server fiber must park via SleepThread)");

            // Teardown: requestStop() terminates the parked server thread
            // (SleepThread observes the stop and unwinds). Bounded even on
            // regression: the stop also unwinds a hot-spinning server at its
            // next dispatch-loop yield point.
            //
            // 2026-09-21: was scheduler_shutdown() + g_activeThreads==0. The
            // process-global fiber count is gone; isIdle() (via drainedWithin)
            // is the drain check, and it asserts the same thing -- no guest
            // thread left running or ready.
            runtime.requestStop();
            t.IsTrue(drainedWithin(runtime, std::chrono::milliseconds(3000)),
                     "R1: all guest threads terminated after stop");
        });
    }); // MiniTest::Case("SchedulerRpcLoopPark")
}

// ---------------------------------------------------------------------------
// requestStop() from GUEST context must not block on host workers.
//
// ORIGINAL BUG (pre-Phase-3d): a worker parked in ps2sched::async_guest_begin()
// waited for g_running_fiber == nullptr, which could never happen while the
// joining fiber WAS the running fiber -- a deadlock, reached whenever the
// unimplemented-function default fault handler called requestStop from a fiber
// while an interrupt worker was parked for the guest token.
//
// 2026-09-21 -- ⚠️ THE ORIGINAL FAILURE MODE IS NOW STRUCTURALLY IMPOSSIBLE.
// PS2Runtime::requestStop() (ps2_runtime.cpp) is signal-only: it sets
// m_stopRequested and calls EeScheduler::requestStop(), and JOINS NOTHING.
// There is no longer any code path by which requestStop could wait on a host
// worker, so this test can no longer fail the way it was written to fail.
//
// It is kept, not deleted, because the INVARIANT is still worth pinning: a
// stop issued from guest context must return to the guest. If someone later
// reintroduces a join (or a blocking drain) inside requestStop, this goes red
// again. What changed is the contention it is exercised under -- see below.
// ---------------------------------------------------------------------------
void register_scheduler_guest_context_stop_tests()
{
    MiniTest::Case("SchedulerGuestContextStop", [](TestCase &tc)
    {
        tc.Run("G1: requestStop from guest context returns while a host worker is parked", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700700u, &stepGuestContextStop);
            rdramWrite32(rdram, kThStop, 0u);
            rdramWrite32(rdram, kThFlag, 0u);

            const int32_t tid = startSchedWorker(rdram.data(), &runtime,
                                                 0x00700700u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tid > 0, "G1: guest thread started");
            if (tid <= 0)
            {
                rdramWrite32(rdram, kThStop, 1u);
                return;
            }

            // ORDER IS LOAD-BEARING. The worker must be constructed AFTER the
            // guest thread is dispatching, because EeScheduler::run() holds
            // hostInvocationMutex() for the duration of each guest dispatch
            // (EeScheduler.cpp, see hostInvocationMutex()'s doc comment). With
            // the guest spinning inside its dispatch, the worker genuinely
            // blocks and its published intent stays visible for the whole
            // spin. Constructed BEFORE the thread started, it would win the
            // uncontended mutex instantly and the guest would spin forever on
            // a counter that had already returned to zero.
            //
            // Replaces the old EnsureVSyncWorkerRunning(): the real interrupt
            // worker takes the mutex directly and publishes nothing, so the
            // guest has no way to observe that it is parked.
            ParkedHostWorker worker(runtime.eeScheduler());

            // THE assertion: the guest observes the parked worker, calls
            // requestStop() from guest context, and RETURNS (writing the flag).
            const bool stopReturned = waitForWord(rdram, kThFlag, 1u, std::chrono::milliseconds(5000));

            // Escape hatch first, so a red assertion never leaves the guest
            // spinning and the worker blocked behind it (the ParkedHostWorker
            // destructor joins unconditionally).
            rdramWrite32(rdram, kThStop, 1u);

            t.IsTrue(stopReturned,
                     "G1: requestStop() invoked from guest context returned "
                     "(stop must be signal-only, never a join)");

            const bool workerFinished = worker.finishedWithin(std::chrono::milliseconds(3000));
            t.IsTrue(workerFinished,
                     "G1: parked host worker acquired the host-invocation mutex and finished");

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained, "G1: guest thread drained after guest-context stop");
        });
    }); // MiniTest::Case("SchedulerGuestContextStop")
}

// ---------------------------------------------------------------------------
// Async callback stacks must live in kernel-reserved memory, never at
// top-of-RAM. Games place their main stack at top-of-RAM via SetupThread
// (SDK crt0 places the main stack at the top of user RAM, and the boot $sp
// defaults to PS2_RAM_SIZE - 0x10); the old pool
// carved down from PS2_RAM_SIZE, so host-dispatched guest callbacks (GS
// vsync / INTC / alarms) ran ON the guest's live main stack and both sides'
// register spills corrupted each other (garbage callback pointers, $ra
// clobbers near the stack top).
// ---------------------------------------------------------------------------
void register_runtime_async_stack_pool_tests()
{
    MiniTest::Case("RuntimeAsyncStackPool", [](TestCase &tc)
    {
        tc.Run("S1: callback stack pool is kernel-area and disjoint from top-of-RAM", [](TestCase &t)
        {
            constexpr uint32_t kPoolFloor = 0x00080000u;
            constexpr uint32_t kPoolTop = 0x00100000u;
            constexpr uint32_t kStackSize = 0x4000u;

            PS2Runtime runtime;

            // First reservation: this is the stack the GS vsync-callback path
            // grabs at boot. Under the old top-of-RAM pool it was 0x1FFFFF0 -
            // inside the guest's main stack.
            const uint32_t first = runtime.reserveAsyncCallbackStack(kStackSize, 16u);
            t.Equals(first, kPoolTop - 0x10u, "S1: first reservation tops the kernel pool");
            t.IsTrue(first < kPoolTop, "S1: first reservation is below the ELF load base");
            t.IsTrue(first >= kPoolFloor, "S1: first reservation is above the pool floor");

            // Exhaust the pool: every reservation must stay inside
            // [kPoolFloor, kPoolTop), and the pool must hold exactly
            // (kPoolTop - kPoolFloor) / kStackSize stacks before returning 0 (bounded loop).
            uint32_t count = 1u;
            bool allInPool = (first >= kPoolFloor && first < kPoolTop);
            for (uint32_t i = 0u; i < 64u; ++i)
            {
                const uint32_t top = runtime.reserveAsyncCallbackStack(kStackSize, 16u);
                if (top == 0u)
                {
                    break;
                }
                ++count;
                allInPool = allInPool && (top >= kPoolFloor && top < kPoolTop);
            }
            t.IsTrue(allInPool, "S1: every reservation stays inside the kernel pool");
            t.Equals(count, (kPoolTop - kPoolFloor) / kStackSize,
                     "S1: pool capacity matches [0x80000, 0x100000) / 16KB");

            runtime.requestStop();
        });
    }); // MiniTest::Case("RuntimeAsyncStackPool")
}

// ---------------------------------------------------------------------------
// dispatchDmacHandlersForCause is reachable from BOTH sides of the guest
// token: from host workers (IRQ worker, DMA completion paths) AND
// synchronously from guest syscall context (sceSifSetDma calls it inline
// while a fiber services the syscall). It borrowed the token via
// AsyncGuestScope unconditionally, and async_guest_begin() aborts by design
// on the guest executor thread - so a guest program arming a DMAC handler
// and then calling sceSifSetDma killed the process deterministically
// ("FATAL [ps2sched]: async_guest_begin from guest executor thread"; any
// guest that arms a DMAC handler and then issues a SIF transfer — e.g. a
// sound-driver init path — hits this).
// ---------------------------------------------------------------------------
void register_scheduler_dmac_guest_dispatch_tests()
{
    MiniTest::Case("SchedulerDmacGuestDispatch", [](TestCase &tc)
    {
        tc.Run("M1: DMAC dispatch from guest syscall context runs handlers without aborting", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(0x00700800u, &stepDmacHandler);
            runtime.registerFunction(0x00700900u, &stepDmacFromGuest);
            rdramWrite32(rdram, kThFlag, 0u);
            rdramWrite32(rdram, kThSent, 0u);

            // Arm a DMAC handler on cause 5 (SIF0), like sceSifSetDma clients do.
            R5900Context ac{};
            setRegU32(ac, 4, 5u);           // cause
            setRegU32(ac, 5, 0x00700800u);  // handler
            setRegU32(ac, 6, 0u);           // next
            setRegU32(ac, 7, 0u);           // arg
            ps2_syscalls::AddDmacHandler(rdram.data(), &ac, &runtime);
            t.IsTrue(getRegS32(ac, 2) > 0, "M1: DMAC handler registered");

            // Guest-context dispatch: the fiber calls
            // dispatchDmacHandlersForCause synchronously. Against the
            // unconditional AsyncGuestScope this std::terminate()s the
            // process - a loud bounded failure, never a hang.
            const int32_t tid = startSchedWorker(rdram.data(), &runtime,
                                                 0x00700900u, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tid > 0, "M1: dispatching fiber started");
            if (tid <= 0)
            {
                return;
            }

            const bool done = waitForWord(rdram, kThFlag, 1u, std::chrono::milliseconds(3000));
            t.IsTrue(done, "M1: guest-context dispatch returned");

            // 2026-09-22 -- the counts are read AFTER a drain, never at the
            // sentinel.
            //
            // This test was written against the ps2sched dispatcher, which ran
            // DMAC handlers INLINE on the fiber servicing the syscall -- hence
            // the "SYNCHRONOUSLY" wording above and the original
            // t.Equals(rdramRead32(rdram, kThSent), 1u) sitting immediately
            // after the sentinel. EeScheduler::dispatchIrq() only QUEUES
            // (queueInvocation -> m_pendingInvocations, EeScheduler.cpp:3295)
            // and the handler runs when the scheduler next dispatches, so a
            // read taken at the sentinel is 0 no matter how healthy the runtime
            // is. It was measuring before the work it measures could happen.
            //
            // [m1probe] established that by measurement rather than inference:
            //   hasHandlerFn=1 hasEntryFn=1 flag=1 sentAfterGuest=0
            //   sentAfterDrain=1 sentAfterHost=1 sentAfterHostPump=2
            // The handler fires exactly once per dispatch; only the read points
            // were wrong. Before the drainedWithin() pump fix in
            // SchedTestSupport.h every one of those numbers was 0 -- that
            // defect is what hid this one, so both had to be measured to
            // separate them.
            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained, "M1: fiber drained");

            t.Equals(rdramRead32(rdram, kThSent), 1u,
                     "M1: handler ran exactly once from guest context");

            // Host-thread path must still borrow the token and work: dispatch
            // the same cause from this (non-executor) thread. Same rule --
            // queue, drain, then read.
            ps2_syscalls::dispatchDmacHandlersForCause(rdram.data(), &runtime, 5u);
            t.IsTrue(drainedWithin(runtime, std::chrono::milliseconds(3000)),
                     "M1: host-path dispatch drained");

            t.Equals(rdramRead32(rdram, kThSent), 2u,
                     "M1: handler also runs via the borrowed-token host path");
        });
    }); // MiniTest::Case("SchedulerDmacGuestDispatch")
}

// ---------------------------------------------------------------------------
// The diagnostic dispatch-trace ring (the "trace=..." string logged by
// lookupFunction/reportMissingFunction/dispatchLoop) used to be a single
// process-wide `thread_local DispatchHistory` in ps2_runtime.cpp. Under the
// N=1 fiber scheduler that thread_local is keyed to the ONE guest executor
// OS thread, not to any individual fiber - so EVERY fiber that ever ran on
// that thread shared and overwrote the same ring. Two concrete failures fall
// out of that:
//   R1 (isolation): while fiber A is legitimately still alive (parked in
//      WaitSema, about to resume), a second live fiber B pushes into the
//      "same" ring, so A's post-resume trace is contaminated with B's PCs -
//      a diagnostic reading a completely unrelated fiber's history.
//   R2 (freshness on tid reuse): after a fiber exits and its tid is
//      genuinely recycled by CreateThread's tid allocator, the new fiber at
//      that tid inherits the old fiber's leftover ring contents, because the
//      thread_local lives on the OS thread, not the (now-destroyed) fiber.
// The fix embeds the ring directly in FiberContext (fresh at construction,
// destroyed with the fiber; see ps2_scheduler_internal.h), so both failure
// modes are structurally impossible: there is no shared, outlives-the-fiber
// storage left to leak through.
// ---------------------------------------------------------------------------
void register_scheduler_recovery_isolation_tests()
{
    MiniTest::Case("SchedulerRecoveryIsolation", [](TestCase &tc)
    {
        tc.Run("R1: dispatch-trace ring is isolated across two live interleaved fibers", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            constexpr uint32_t kEntryA = 0x00700A00u;
            constexpr uint32_t kMarkerA = 0x00700A80u;
            constexpr uint32_t kEntryB = 0x00700B00u;
            constexpr uint32_t kMarkerB = 0x00700B80u;
            constexpr uint32_t kRiSidA = 0x00061000u;
            constexpr uint32_t kRiSidB = 0x00061004u;
            constexpr uint32_t kRiAForeign = 0x00061010u; // A saw B's marker? (bug => 1)
            constexpr uint32_t kRiBForeign = 0x00061014u; // B saw A's marker? (bug => 1)
            constexpr uint32_t kRiAOwn = 0x00061018u;      // A saw its own marker? (must be 1)
            constexpr uint32_t kRiBOwn = 0x0006101Cu;      // B saw its own marker? (must be 1)

            runtime.registerFunction(kEntryA, &stepIsoA);
            runtime.registerFunction(kEntryB, &stepIsoB);
            runtime.registerFunction(kMarkerA, &stepNoop);
            runtime.registerFunction(kMarkerB, &stepNoop);

            // Both semas start at 0: A signals B then waits on A; B waits on B
            // then signals A - a one-shot ping-pong handoff, not a loop.
            SemaPair semas(rdram.data(), &runtime, createSchedSema, deleteSchedSema,
                           0, 1, 0, 1);
            t.IsTrue(semas.a() > 0 && semas.b() > 0, "R1: semas created");
            if (semas.a() <= 0 || semas.b() <= 0)
            {
                return;
            }
            const int32_t sidA = semas.a();
            const int32_t sidB = semas.b();
            rdramWrite32(rdram, kRiSidA, static_cast<uint32_t>(sidA));
            rdramWrite32(rdram, kRiSidB, static_cast<uint32_t>(sidB));
            rdramWrite32(rdram, kRiAForeign, 0u);
            rdramWrite32(rdram, kRiBForeign, 0u);
            rdramWrite32(rdram, kRiAOwn, 0u);
            rdramWrite32(rdram, kRiBOwn, 0u);

            const int32_t tidA = startSchedWorker(rdram.data(), &runtime,
                                                  kEntryA, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            const int32_t tidB = startSchedWorker(rdram.data(), &runtime,
                                                  kEntryB, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidA > 0 && tidB > 0, "R1: both fibers started");
            if (tidA <= 0 || tidB <= 0)
            {
                return;
            }

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained, "R1: both fibers completed the handoff and exited");

            // THE regression assertions: under the shared thread_local, A and
            // B push into the SAME ring, so each fiber's post-handoff trace
            // is contaminated with the other's markers.
            t.Equals(rdramRead32(rdram, kRiAForeign), 0u,
                     "R1: fiber A's trace must not contain fiber B's markers");
            t.Equals(rdramRead32(rdram, kRiBForeign), 0u,
                     "R1: fiber B's trace must not contain fiber A's markers");
            t.Equals(rdramRead32(rdram, kRiAOwn), 1u,
                     "R1: fiber A's trace must contain fiber A's own marker");
            t.Equals(rdramRead32(rdram, kRiBOwn), 1u,
                     "R1: fiber B's trace must contain fiber B's own marker");
        });

        tc.Run("R2: dispatch-trace ring is fresh after genuine tid reuse", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            constexpr uint32_t kEntryReuse = 0x00700C00u;
            constexpr uint32_t kReuseMarker = 0x00700C80u;
            constexpr uint32_t kReuseRun = 0x00062000u;             // 0 => first fiber, 1 => reused
            constexpr uint32_t kReuseNotInherited0 = 0x00062004u;   // fiber1 did not inherit a prior marker (expect 1)
            constexpr uint32_t kReuseNonEmpty0 = 0x00062008u;       // fiber1 pushed its own marker (expect 1)
            constexpr uint32_t kReuseNotInherited1 = 0x0006200Cu;   // fiber2 (reused tid) did not inherit fiber1's marker (expect 1)

            runtime.registerFunction(kEntryReuse, &stepReuseProbe);
            runtime.registerFunction(kReuseMarker, &stepNoop);
            rdramWrite32(rdram, kReuseRun, 0u);
            rdramWrite32(rdram, kReuseNotInherited0, 0u);
            rdramWrite32(rdram, kReuseNonEmpty0, 0u);
            rdramWrite32(rdram, kReuseNotInherited1, 0u);

            const int32_t T1 = startSchedWorker(rdram.data(), &runtime,
                                                kEntryReuse, 10, 0x00538000u, 0x2000u);
            t.IsTrue(T1 > 0, "R2: fiber 1 started");
            if (T1 <= 0)
            {
                return;
            }

            const bool drained1 = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained1, "R2: fiber 1 fully torn down (FiberContext destroyed)");

            // Force GENUINE tid reuse: erase T1 from the kernel thread map
            // (DeleteThread requires THS_DORMANT, which thread exit already
            // set), then walk the allocator around until it hands out exactly
            // T1 again.
            //
            // 2026-09-21: was a direct `g_nextThreadId = T1` seed under
            // g_thread_map_mutex. EeScheduler owns the allocator privately now
            // (allocateThreadId(), m_nextThreadId) with no public seam, so we
            // drive it from outside instead -- see createDormantWorkerWithId.
            deleteWorkerById(rdram.data(), &runtime, T1);

            rdramWrite32(rdram, kReuseRun, 1u);
            const int32_t T2 = createDormantWorkerWithId(rdram.data(), &runtime, T1,
                                                         kEntryReuse, 10, 0x0053C000u, 0x2000u);
            t.Equals(T2, T1, "R2: tid genuinely recycled (not merely a fresh, different tid)");
            if (T2 != T1)
            {
                return;
            }
            t.IsTrue(startExistingWorker(rdram.data(), &runtime, T2),
                     "R2: recycled-tid thread started");

            const bool drained2 = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained2, "R2: fiber 2 (reused tid) completed");

            // THE regression assertion: under the shared thread_local, fiber 2
            // (running on the same executor OS thread) sees fiber 1's leftover
            // 0x00700c80 marker still sitting in the one global ring.
            t.Equals(rdramRead32(rdram, kReuseNotInherited0), 1u,
                     "R2: fiber 1 (first ever on this tid) starts with no prior marker");
            t.Equals(rdramRead32(rdram, kReuseNonEmpty0), 1u,
                     "R2: fiber 1 successfully pushed its own marker");
            t.Equals(rdramRead32(rdram, kReuseNotInherited1), 1u,
                     "R2: fiber 2 (reused tid) must NOT inherit fiber 1's marker");
        });
    }); // MiniTest::Case("SchedulerRecoveryIsolation")
}

// ---------------------------------------------------------------------------
// Shared-guest-stack bug class (B1/B3/B4): rpcInvokeFunction (RPC/override
// invoke), inline DMAC handler dispatch, and MPEG stream callbacks each ran
// recompiled guest code on a per-OS-thread `thread_local` scratch stack. Under
// the N=1 fiber scheduler every fiber runs on the ONE guest-executor OS
// thread, so that thread_local is a SINGLE stack shared by every fiber (and
// by one fiber re-entering the same path). A fiber can yield mid-invoke (the
// invoked guest body hits a back-edge / blocking syscall), so a second fiber
// entering the same path sets $sp to the SAME top and overwrites the parked
// fiber's live frames. Fixed by GuestScratchStack: a fresh, RAII-released
// guest-heap reservation per invocation (see ps2_runtime.h), so interleaved
// or re-entrant invocations always run on disjoint stacks.
//
// I1/I2 are RED on build-ucontext against the old shared thread_local
// (topA == topB) and GREEN with the fix (disjoint reservations). On the
// pthread backend each fiber is its own OS thread, so the old thread_local is
// already per-fiber and both tests pass even unfixed — mirrors the existing
// SchedulerRecoveryIsolation banner note. I3 exercises the GuestScratchStack
// contract directly (stands in for B4 — dispatchGuestStreamCallback is
// anon-namespace-internal and reachable only through the full MPEG decode
// path, disproportionate to stand up for a unit test; B4's site uses the same
// helper proven here and at I1/I2).
//
// LOAD-BEARING CHOREOGRAPHY: a sequential (non-overlapping) two-fiber test
// FALSE-PASSES even on the fix, because guestMalloc recycles a just-freed
// address — if fiber A frees before B allocates, topA == topB regardless of
// the fix. Both reservations must be LIVE SIMULTANEOUSLY: fiber A parks
// INSIDE its invoke/dispatch (its GuestScratchStack not yet destructed) via a
// semaphore wait executed from within the invoked guest body / handler body,
// while fiber B allocates and records. Do not "simplify" this overlap away.
// ---------------------------------------------------------------------------
void register_scheduler_stack_isolation_tests()
{
    MiniTest::Case("SchedulerStackIsolation", [](TestCase &tc)
    {
        // I1 --- B1: two fibers interleaving through rpcInvokeFunction must run
        // on disjoint scratch stacks. rpcInvokeFunction gets its stack from a
        // fresh, per-invocation GuestScratchStack reservation (see
        // ps2_runtime.h), so two interleaved invocations never share $sp
        // (topA != topB).
        tc.Run("I1: rpc/override invoke stack is isolated across interleaved fibers", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            constexpr uint32_t kSysA = 0x00005A01u, kSysB = 0x00005A02u;
            constexpr uint32_t kEntryA = 0x00701000u, kEntryB = 0x00701100u;
            constexpr uint32_t kInvA = 0x00701200u, kInvB = 0x00701300u;

            runtime.registerFunction(kEntryA, &stepInvokeEntryA);
            runtime.registerFunction(kEntryB, &stepInvokeEntryB);
            runtime.registerFunction(kInvA,  &stepInvokeRecordA);
            runtime.registerFunction(kInvB,  &stepInvokeRecordB);

            // Register two DISTINCT syscall overrides (distinct numbers dodge
            // B2's shared s_activeSyscallOverrides reentrancy guard; that bug
            // is tracked separately by SchedulerOverrideIsolation).
            { R5900Context s{}; setRegU32(s,4,kSysA); setRegU32(s,5,kInvA);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
            { R5900Context s{}; setRegU32(s,4,kSysB); setRegU32(s,5,kInvB);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }

            SemaPair semas(rdram.data(), &runtime, createSchedSema, deleteSchedSema,
                           0, 1, 0, 1);
            t.IsTrue(semas.a() > 0 && semas.b() > 0, "I1: semas created");
            rdramWrite32(rdram, kSiSemA, static_cast<uint32_t>(semas.a()));
            rdramWrite32(rdram, kSiSemB, static_cast<uint32_t>(semas.b()));
            rdramWrite32(rdram, kSiSysA, kSysA);
            rdramWrite32(rdram, kSiSysB, kSysB);
            rdramWrite32(rdram, kSiTopA, 0u);
            rdramWrite32(rdram, kSiTopB, 0u);

            const int32_t tidA = startSchedWorker(rdram.data(), &runtime, kEntryA, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            const int32_t tidB = startSchedWorker(rdram.data(), &runtime, kEntryB, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidA > 0 && tidB > 0, "I1: both fibers started");

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained, "I1: both fibers completed the handoff and exited");

            const uint32_t topA = rdramRead32(rdram, kSiTopA);
            const uint32_t topB = rdramRead32(rdram, kSiTopB);
            t.IsTrue(topA != 0u && topB != 0u, "I1: both invokes reserved a scratch stack");
            t.IsTrue(topA != topB,
                     "I1: interleaved invokes must run on DISJOINT scratch stacks");

            // Cleanup: erase the global overrides so later suites are unaffected.
            { R5900Context s{}; setRegU32(s,4,kSysA); setRegU32(s,5,0u);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
            { R5900Context s{}; setRegU32(s,4,kSysB); setRegU32(s,5,0u);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
        });

        // I2 --- the invocation-stack reservation contract. See the step
        // functions above for why this no longer ping-pongs two fibers through
        // inline DMAC dispatch, and for the trace that settled it.
        tc.Run("I2: invocation stacks are reserved per {thread, depth}", [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            constexpr uint32_t kEntryA = 0x00701400u, kHndA = 0x00701600u;
            constexpr uint32_t kNestEntry = 0x00701500u;
            constexpr uint32_t kNestOuter = 0x00701700u, kNestInner = 0x00701800u;
            constexpr uint32_t kSysNestA = 0x00005A03u, kSysNestB = 0x00005A04u;
            constexpr uint32_t kWorkerSize = 0x2000u;

            runtime.registerFunction(kEntryA,    &stepDmacDispatchA);
            runtime.registerFunction(kHndA,      &stepDmacRecordA);
            runtime.registerFunction(kNestEntry, &stepNestEntry);
            runtime.registerFunction(kNestOuter, &stepNestOuter);
            runtime.registerFunction(kNestInner, &stepNestInner);

            // ---------- half 1: an interrupt handler gets its OWN stack -----
            int32_t hidA = -1;
            { R5900Context a{}; setRegU32(a,4,5u); setRegU32(a,5,kHndA); setRegU32(a,6,0u); setRegU32(a,7,0u);
              ps2_syscalls::AddDmacHandler(rdram.data(), &a, &runtime); hidA = getRegS32(a, 2); }
            t.IsTrue(hidA > 0, "I2: DMAC handler registered");

            rdramWrite32(rdram, kSiDmacTopA, 0u);

            // The base is bound to a local so the handler's $sp can be checked
            // against the interrupted thread's actual stack range rather than
            // against a re-derived guess.
            const uint32_t workerBase = nextWorkerStackBase(kWorkerSize);
            const int32_t tidA = startSchedWorker(rdram.data(), &runtime, kEntryA, 10,
                                                  workerBase, kWorkerSize);
            t.IsTrue(tidA > 0, "I2: dispatch fiber started");

            t.IsTrue(drainedWithin(runtime, std::chrono::milliseconds(3000)),
                     "I2: dispatch fiber and its queued handler both completed");

            const uint32_t topA = rdramRead32(rdram, kSiDmacTopA);
            t.IsTrue(topA != 0u, "I2: the DMAC handler ran on a reserved stack");
            t.IsTrue(topA < workerBase || topA >= workerBase + kWorkerSize,
                     "I2: an interrupt handler must NOT run on the interrupted thread's stack");

            // ---------- half 2: nesting depth gets its OWN stack ------------
            { R5900Context s{}; setRegU32(s,4,kSysNestA); setRegU32(s,5,kNestOuter);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
            { R5900Context s{}; setRegU32(s,4,kSysNestB); setRegU32(s,5,kNestInner);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
            rdramWrite32(rdram, kSiSysA, kSysNestA);
            rdramWrite32(rdram, kSiSysB, kSysNestB);
            rdramWrite32(rdram, kSiNestTopOuter, 0u);
            rdramWrite32(rdram, kSiNestTopInner, 0u);

            const int32_t tidN = startSchedWorker(rdram.data(), &runtime, kNestEntry, 10,
                                                  nextWorkerStackBase(kWorkerSize), kWorkerSize);
            t.IsTrue(tidN > 0, "I2: nesting fiber started");
            t.IsTrue(drainedWithin(runtime, std::chrono::milliseconds(3000)),
                     "I2: nested invocations completed and the fiber exited");

            const uint32_t outer = rdramRead32(rdram, kSiNestTopOuter);
            const uint32_t inner = rdramRead32(rdram, kSiNestTopInner);
            t.IsTrue(outer != 0u && inner != 0u,
                     "I2: both nested invocations reserved a stack");
            t.IsTrue(outer != inner,
                     "I2: an invocation nested inside a LIVE one must get a DISJOINT stack");

            // Cleanup: RemoveDmacHandler reads cause=$a0, handlerId=$a1, and the
            // syscall overrides are process-global, so later suites see them
            // unless they are erased here.
            { R5900Context r{}; setRegU32(r,4,5u); setRegU32(r,5,static_cast<uint32_t>(hidA));
              ps2_syscalls::RemoveDmacHandler(rdram.data(), &r, &runtime); }
            { R5900Context s{}; setRegU32(s,4,kSysNestA); setRegU32(s,5,0u);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
            { R5900Context s{}; setRegU32(s,4,kSysNestB); setRegU32(s,5,0u);
              ps2_syscalls::SetSyscall(rdram.data(), &s, &runtime); }
        });

        // I3 --- shared-mechanism contract (stands in for B4). dispatchGuest-
        // StreamCallback is anon-namespace-internal and standing up MPEG demux
        // state to trigger it from a fiber is disproportionate; instead assert
        // the GuestScratchStack contract directly: two LIVE reservations are
        // disjoint. B4's site uses this exact helper, and the fiber-level proof
        // that it isolates under interleaving is I1/I2.
        tc.Run("I3: two live GuestScratchStack reservations are disjoint", [](TestCase &t)
        {
            PS2Runtime runtime;
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            constexpr uint32_t kSize = 0x4000u;

            GuestScratchStack a(&runtime, kSize);
            GuestScratchStack b(&runtime, kSize); // b alive WHILE a is alive
            t.IsTrue(a.valid() && b.valid(), "I3: both reservations succeeded");
            t.IsTrue(a.top() != b.top(), "I3: live reservations have distinct tops");
            const uint32_t hi = (a.top() > b.top()) ? a.top() : b.top();
            const uint32_t lo = (a.top() > b.top()) ? b.top() : a.top();
            t.IsTrue(hi - lo >= kSize, "I3: reserved ranges do not overlap");
            // After scope exit both free; a subsequent reservation may recycle
            // an address (expected) — which is exactly why I1/I2 keep both live.

            runtime.requestStop();
        });
    }); // MiniTest::Case("SchedulerStackIsolation")
}

// ---------------------------------------------------------------------------
// B2: dispatchSyscallOverride reentrancy-guard bug -- see the helper
// banner above for the shared-thread_local mechanism and pthread-backend
// caveat. This case proves fiber B's override actually runs while A is
// parked inside its own override.
// ---------------------------------------------------------------------------
void register_scheduler_override_isolation_tests()
{
    MiniTest::Case("SchedulerOverrideIsolation", [](TestCase &tc)
    {
        tc.Run("O1: a fiber parked inside a syscall override must not suppress another fiber's override for the same syscall",
               [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            runtime.registerFunction(kOvEntryA, &stepOverrideA);
            runtime.registerFunction(kOvEntryB, &stepOverrideB);
            runtime.registerFunction(kOvHandler, &stepOverrideHandler);

            // Register the override directly. Passing rdram = nullptr skips the
            // guest-memory table mirroring, which is the SetSyscall side effect
            // this test is avoiding; the map is per-runtime and self-locking.
            runtime.setEeSyscallOverride(nullptr, kOvSyscall, kOvHandler);

            SemaPair semas(rdram.data(), &runtime, createSchedSema, deleteSchedSema,
                           0, 1, 0, 1);
            t.IsTrue(semas.a() > 0 && semas.b() > 0, "O1: semas created");
            if (semas.a() <= 0 || semas.b() <= 0)
            {
                runtime.setEeSyscallOverride(nullptr, kOvSyscall, 0u);
                return;
            }
            const int32_t sidStartB  = semas.a();
            const int32_t sidResumeA = semas.b();
            rdramWrite32(rdram, kOvSidStartB,  static_cast<uint32_t>(sidStartB));
            rdramWrite32(rdram, kOvSidResumeA, static_cast<uint32_t>(sidResumeA));
            rdramWrite32(rdram, kOvAEntered, 0u);
            rdramWrite32(rdram, kOvARan,     0u);
            rdramWrite32(rdram, kOvBRan,     0u);

            const int32_t tidA = startSchedWorker(rdram.data(), &runtime,
                                                  kOvEntryA, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            const int32_t tidB = startSchedWorker(rdram.data(), &runtime,
                                                  kOvEntryB, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidA > 0 && tidB > 0, "O1: both fibers started");
            if (tidA <= 0 || tidB <= 0)
            {
                runtime.setEeSyscallOverride(nullptr, kOvSyscall, 0u);
                return;
            }

            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            // A timeout here means the PARK mechanism failed (WaitSema nested in
            // the override invoke) — NOT the B2 guard. Debug the park, not the fix.
            t.IsTrue(drained, "O1: both fibers completed the handoff and exited");

            // Liveness: prove A really parked inside its override and resumed —
            // otherwise kOvBRan could be satisfied vacuously.
            t.Equals(rdramRead32(rdram, kOvAEntered), 1u,
                     "O1: fiber A entered its override handler");
            t.Equals(rdramRead32(rdram, kOvARan), 1u,
                     "O1: fiber A resumed from its park inside the override");

            // THE regression assertion. Bug: B's override is skipped as
            // 'reentrant' (A's N still on the shared stack) => kOvBRan == 0.
            t.Equals(rdramRead32(rdram, kOvBRan), 1u,
                     "O1: fiber B's own override must run while fiber A is parked inside the same syscall's override");

            runtime.setEeSyscallOverride(nullptr, kOvSyscall, 0u);
        });
    }); // MiniTest::Case("SchedulerOverrideIsolation")
}

// ---------------------------------------------------------------------------
// TerminateThread must not hang when the target has an equal-priority sibling
// that stays runnable after the target exits.
//
// join_fiber() applies a temporary priority floor to the joiner so it runs
// strictly AFTER the target. The floor must be the target's OWN level, not
// one below it: PS2 EE priority is "lower number = higher priority", and the
// run queue is FIFO within a level. Flooring the joiner one level below the
// target (t->priority + 1) drops it beneath every equal-priority sibling of
// the target. If any such sibling stays runnable after the target finishes,
// the joiner never regains the run-queue head, never re-observes the target
// as finished, and join_fiber() — hence TerminateThread — never returns.
//
// Shape (all guest fibers, N=1 cooperative executor):
//   S1,S2 (prio 10): a one-permit semaphore ping-pong. Exactly one of the two
//                    is runnable at any instant, so the prio-10 level is
//                    continuously "hot" for the whole test, including after
//                    the target exits.
//   B     (prio 10): the terminate target. Spins until killed by A.
//   A     (prio  5): calls TerminateThread(B). join_fiber floors A to B's
//                    level. Fixed (floor = 10): A ties the pingers and, by
//                    FIFO rotation, cycles back to the head to see B finished
//                    and returns. Buggy (floor = 11): A sits below the ever-
//                    runnable pingers forever and TerminateThread hangs.
//
// The assertion is a bounded-time poll on A's "join returned" flag, so the
// buggy build FAILS via timeout instead of hanging the binary. Teardown stops
// the pingers and drains to zero active threads in BOTH outcomes (and the
// SchedFixture destructor's scheduler_shutdown is the final backstop), so no
// runnable fiber is ever leaked into the rest of the suite.
// ---------------------------------------------------------------------------
void register_scheduler_join_starvation_tests()
{
    MiniTest::Case("SchedulerJoinStarvation", [](TestCase &tc)
    {
        tc.Run("J1: TerminateThread returns when the target has an equal-priority sibling that outlives it",
               [](TestCase &t)
        {
            SchedFixture fx;
            PS2Runtime &runtime = fx.runtime;
            std::vector<uint8_t> &rdram = fx.rdram;

            constexpr uint32_t kEntryPingA  = 0x00760000u;
            constexpr uint32_t kEntryPingB  = 0x00760100u;
            constexpr uint32_t kEntryTarget = 0x00760200u;
            constexpr uint32_t kEntryJoiner = 0x00760300u;

            runtime.registerFunction(kEntryPingA,  &stepPingA);
            runtime.registerFunction(kEntryPingB,  &stepPingB);
            runtime.registerFunction(kEntryTarget, &stepJoinStarveTarget);
            runtime.registerFunction(kEntryJoiner, &stepJoinStarveJoiner);

            // One permit circulates X -> Y -> X, keeping the prio-10 level hot.
            SemaPair semas(rdram.data(), &runtime, createSchedSema, deleteSchedSema,
                           1, 1, 0, 1); // sidX seeded permit, sidY empty
            t.IsTrue(semas.a() > 0 && semas.b() > 0, "J1: ping-pong semas created");
            if (semas.a() <= 0 || semas.b() <= 0)
            {
                return;
            }
            const int32_t sidX = semas.a();
            const int32_t sidY = semas.b();
            rdramWrite32(rdram, kThSidX, static_cast<uint32_t>(sidX));
            rdramWrite32(rdram, kThSidY, static_cast<uint32_t>(sidY));
            rdramWrite32(rdram, kThStop, 0u);
            rdramWrite32(rdram, kThCount, 0u);
            rdramWrite32(rdram, kJsBStarted, 0u);
            rdramWrite32(rdram, kJsJoinReturned, 0u);
            rdramWrite32(rdram, kJsTargetTid, 0u);

            // Pingers first (prio 10), then confirm the level is circulating.
            const int32_t tidS1 = startSchedWorker(rdram.data(), &runtime, kEntryPingA, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            const int32_t tidS2 = startSchedWorker(rdram.data(), &runtime, kEntryPingB, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidS1 > 0 && tidS2 > 0, "J1: ping-pong pair started");

            const bool pingLive = waitForWordAtLeast(rdram, kThCount, 3u, std::chrono::milliseconds(1000));
            t.IsTrue(pingLive, "J1: prio-10 ping-pong is circulating");

            // Target B (prio 10) — never self-exits; killed by A's Terminate.
            const int32_t tidB = startSchedWorker(rdram.data(), &runtime, kEntryTarget, 10, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidB > 0, "J1: target fiber started");
            const bool bRunning = waitForWord(rdram, kJsBStarted, 1u, std::chrono::milliseconds(1000));
            t.IsTrue(bRunning, "J1: target fiber is running");

            // Joiner A (prio 5) terminates B; join_fiber floors A to B's level.
            rdramWrite32(rdram, kJsTargetTid, static_cast<uint32_t>(tidB));
            const int32_t tidA = startSchedWorker(rdram.data(), &runtime, kEntryJoiner, 5, nextWorkerStackBase(0x2000u), 0x2000u);
            t.IsTrue(tidA > 0, "J1: joiner fiber started");

            // THE regression assertion: TerminateThread(B) returns within a
            // bounded number of scheduler rounds. Buggy (+1 floor): A is
            // stranded below the pingers and this never flips -> timeout ->
            // FAIL (binary still exits; teardown below quiesces everything).
            const bool joinReturned = waitForWord(rdram, kJsJoinReturned, 1u, std::chrono::milliseconds(3000));
            t.IsTrue(joinReturned,
                     "J1: TerminateThread(target) returned — join floor must be the "
                     "target's own priority level, not target+1");

            // ---- Teardown (runs in BOTH pass and fail outcomes) ----
            // Stop the pingers and release any WaitSema-parked pinger so it
            // re-checks kThStop and exits. Once the prio-10 level drains, a
            // stranded (buggy-case) joiner also regains the head, observes B
            // finished, and returns — so g_activeThreads reaches zero here,
            // proving clean quiescence without leaking a runnable fiber.
            rdramWrite32(rdram, kThStop, 1u);
            for (int i = 0; i < 4; ++i)
            {
                signalSchedSema(rdram.data(), &runtime, sidX);
                signalSchedSema(rdram.data(), &runtime, sidY);
            }
            const bool drained = drainedWithin(runtime, std::chrono::milliseconds(3000));
            t.IsTrue(drained, "J1: all fibers quiesced after teardown");
        });
    }); // MiniTest::Case("SchedulerJoinStarvation")
}

