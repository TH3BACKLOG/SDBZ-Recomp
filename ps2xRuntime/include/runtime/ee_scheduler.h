#pragma once

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;

    // 2026-09-22 -- invocations QUEUED but not yet dispatched.
    //
    // Without this the snapshot cannot express "drained". dispatchIrq() only
    // queues (queueInvocation -> m_pendingInvocations), and a queued
    // invocation has no thread yet, so every observer that walked `threads`
    // alone reported a fully-dormant kernel while a handler was still owed a
    // dispatch. SchedTestSupport's drainedWithin() did exactly that.
    //
    // It is NOT enough to say "the pump drains everything anyway":
    // pumpGuestThreads() takes a BUDGET, and run() breaks on that deadline at
    // the top of its loop (EeScheduler.cpp:960) BEFORE it ever reaches the
    // `!next && m_pendingInvocations.empty()` completion test at :986. A 2 ms
    // budget -- pumpGuest()'s default -- therefore returns with work still
    // queued, intermittently. Measured, not reasoned: SchedulerStackIsolation
    // failed 3 of 8 identical isolated runs on this exact window.
    std::size_t pendingInvocations = 0;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
    // SDBZ: arbitrary INTC cause dispatch, posted from interruptWorkerMain's
    // drainPendingIntc() (a separate OS thread) via postEvent() -- dispatchIrq()
    // itself asserts it runs on the executor thread. id = cause. See
    // [[project_upstream_full_catchup_plan]] Phase 3c-2.
    Intc,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    void accountCycles(uint32_t cycles) noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    // Ported off ps2sched::force_reschedule() (Phase 3d): if a ready thread at
    // or above the current thread's priority exists, request and immediately
    // act on a reschedule (synchronous -- unlike checkpointDue()'s polled
    // model, this is for call sites like ReferThreadStatus's cross-thread
    // handshake fix that need the handoff to happen right now, not at the
    // next checkpoint).
    void yieldIfHigherPriorityReady(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    // 2026-09-01 part 47 -- the id GetThreadId (syscall 2Fh) may hand the
    // guest. acquireInvocationThread() mints pseudo-threads with NEGATIVE
    // ids; those are an internal device and no real PS2 thread id is ever
    // negative. Leaking one lets the guest store it and later ask about it
    // (iReferThreadStatus), which is the t=129s stall. Returns the last
    // REAL current thread instead. Internal callers keep currentThreadId().
    [[nodiscard]] int guestVisibleThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // True when no guest thread is running or ready (Phase 3d watchdog probe,
    // ported from ps2sched::ps2x_guest_idle()). Reads the same mutex-guarded
    // snapshot() the debug UI uses, so it is safe to call from another thread,
    // but can lag the live state by up to kDebugPublishDispatchInterval
    // dispatches -- fine for a 1Hz diagnostic, not for scheduling decisions.
    [[nodiscard]] bool isIdle() const;

    // Serializes a host thread's direct guest-code invocation (e.g. GS.cpp's
    // dispatchGsSyncVCallback, called from the IRQ worker thread to run a
    // recompiled sceGsSyncVCallback body) against run()'s own function-
    // dispatch bracket on the game thread. Ported off ps2sched's
    // AsyncGuestScope (Phase 3d): EeScheduler assumes single-threaded guest
    // execution, so any OTHER thread invoking a recompiled function directly
    // (bypassing the normal thread/invocation queue) must hold this lock for
    // the duration, e.g. `std::lock_guard lock(scheduler.hostInvocationMutex());`.
    [[nodiscard]] std::mutex &hostInvocationMutex() noexcept { return m_hostInvocationMutex; }

    // Held for the whole span in which a thread acts as the executor, so that
    // ownership cannot change under a thread that is mid-pump.
    //
    // LOCK ORDER, everywhere, no exceptions: executorMutex() first, then
    // hostInvocationMutex(). pumpGuestThreads() takes only the first and its
    // inner run() takes only the second, so that order has no cycle.
    [[nodiscard]] std::recursive_mutex &executorMutex() noexcept { return m_executorMutex; }

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    // True when the caller is the thread that owns guest execution -- the one
    // that called reset(), i.e. the thread run() dispatches on.
    //
    // 2026-09-21: this is the public replacement for the retired
    // ps2fiber_on_executor_thread(). The comparison already existed, but only
    // inside the private assertExecutor(), so a caller that wanted to ASK
    // rather than assert had no way to.
    [[nodiscard]] bool onExecutorThread() const noexcept;

    // Bounded cooperative pump: runs the normal run() dispatch loop until
    // every guest thread EXCEPT the syscall-issuing main thread is blocked,
    // dormant or finished, or `budget` elapses.
    //
    // 2026-09-21: exists because run() is the ONLY code that dispatches a
    // Ready guest thread, and it never returns. A host caller that drives the
    // scheduler through direct syscall calls (ps2xTest) therefore had no way
    // to let the threads it started actually execute -- a StartThread'd thread
    // went Ready and stayed Ready forever, which is why every scheduler test
    // that needed a worker to RUN failed while the ones that did not, passed.
    //
    // Must be called on the executor thread. Returns with main Ready and
    // m_currentThreadId == 0, which is exactly the state the next
    // bindMainContextForSyscall() expects.
    void pumpGuestThreads(std::chrono::milliseconds budget);

    // ---- borrowed host workers -------------------------------------------
    // A host thread that is NOT the executor but needs to issue a guest
    // syscall: the IRQ worker running a GS callback, or a test's std::thread.
    //
    // 2026-09-21: before this existed every such call went
    // bindMainContextForSyscall() -> assertExecutor() -> abort. FOUR whole
    // Scheduler suites (BorrowedGuard, BorrowedWorker, Stress, Window) died on
    // that single assert.
    //
    // beginBorrowedWorker() makes the CALLING thread the executor and gives it
    // a NEGATIVE pseudo-thread id from acquireInvocationThread(), so it has no
    // real guest thread. endBorrowedWorker() restores both.
    //
    // WARNING: the caller MUST hold executorMutex() AND THEN
    // hostInvocationMutex(), both for the whole span, in that order.
    //
    // 2026-09-22 -- hostInvocationMutex() alone was not enough, and the old
    // comment here said why without noticing it was describing a bug: it
    // brackets run()'s function() dispatch only, "does NOT exclude run()'s
    // between-dispatch bookkeeping, so this is only sound while the real
    // executor is parked". In SchedulerJoinHost/AA6 the real executor is NOT
    // parked -- it is spinning in waitUntil() -> pumpGuest(), which reads
    // m_executorThread outside that mutex. The borrow stole the executor
    // between that read and pumpGuestThreads()' own assertExecutor(), and the
    // process aborted. executorMutex() closes exactly that window.
    struct BorrowedWorkerToken
    {
        std::thread::id executor;
        int currentThreadId = 0;
        int workerId = 0;
    };
    [[nodiscard]] BorrowedWorkerToken beginBorrowedWorker();
    void endBorrowedWorker(const BorrowedWorkerToken &token);

    // True when the caller has no real guest thread -- its current id is a
    // negative pseudo-tid. A thread syscall issued in that state cannot mean
    // "self", so the Thread.cpp entry points return KE_ILLEGAL_THID instead of
    // acting on whatever record happens to be current.
    //
    // This also hardens a defect Part 47 caught with the PSEUDOREFER probe: a
    // pseudo-tid leaked out of GetThreadId and came back in as
    // iReferThreadStatus(a0=-403) just before the t=129s stall. Under this rule
    // that is a clean error instead of a silent lookup.
    [[nodiscard]] bool callerHasNoGuestThread() const noexcept;

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    void publishSnapshot();

private:
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void unwindForStop();
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();

    PS2Runtime &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    int m_lastRealThreadId = 0;
    // pumpGuestThreads() state. Zero/false in production, where the pump-mode
    // branches in run() and selectReady() are therefore never taken. Thread id
    // 0 is never a real thread (kMainThreadId is 1), so it is a safe
    // "exclude nothing" sentinel.
    bool m_pumpMode = false;
    int m_pumpExcludedThreadId = 0;
    std::chrono::steady_clock::time_point m_pumpDeadline{};
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint64_t m_eeCycle = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    // 2026-09-22 -- atomic because this is genuinely touched cross-thread:
    // beginBorrowedWorker() WRITES it from a borrowing host thread while the
    // executor READS it through onExecutorThread()/assertExecutor(). As a
    // plain std::thread::id that was an unsynchronised access -- a data race,
    // i.e. UB, not merely a stale read. Atomic removes the UB; m_executorMutex
    // below removes the check-then-act window that remains.
    std::atomic<std::thread::id> m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::mutex m_hostInvocationMutex;
    // Serialises EXECUTOR OWNERSHIP, which m_hostInvocationMutex does not:
    // that one brackets a single guest function() call, this one brackets a
    // whole span of ACTING as the executor. Recursive because
    // pumpGuestThreads() takes it and then re-enters run(), and because a
    // borrowing thread may legitimately pump itself.
    std::recursive_mutex m_executorMutex;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;
};
