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

    // Polls `pred` at 1ms intervals until it is true or `timeout` elapses,
    // always giving `pred` one final check at the deadline.
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
    inline bool drainedWithin(PS2Runtime &runtime, std::chrono::milliseconds timeout)
    {
        return waitUntil([&]
        {
            return runtime.eeScheduler().isIdle();
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
    using SyscallFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    inline int32_t callSyscall(PS2Runtime &rt, std::vector<uint8_t> &ram, SyscallFn fn,
                                uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        R5900Context ctx{};
        setRegU32(ctx, 4, a0);
        setRegU32(ctx, 5, a1);
        setRegU32(ctx, 6, a2);
        fn(ram.data(), &ctx, &rt);
        return getRegS32(ctx, 2);
    }

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

        ~SchedFixture()
        {
            runtime.requestStop();
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
    struct ParkedHostWorker
    {
        std::atomic<bool> acquired{false}, done{false};
        std::thread th;
        explicit ParkedHostWorker(EeScheduler &scheduler) : th([this, &scheduler]
        {
            {
                std::lock_guard<std::mutex> lock(scheduler.hostInvocationMutex());
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
