#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <utility>
#include <vector>
#include "ps2_syscalls.h"

namespace ps2_syscalls
{
    namespace interrupt_state
    {
        struct VSyncFlagRegistration
        {
            uint32_t flagAddr;
            uint32_t tickAddr;
        };

        extern std::mutex g_irq_handler_mutex;
        extern std::mutex g_vsync_flag_mutex;
        // Written by Enable/DisableIntc(Dmac) and read by dispatchIntcHandlersForCause /
        // dispatchDmacHandlersForCause from the IRQ worker thread. Atomic (rather
        // than g_irq_handler_mutex) because the dispatch call sites read the mask
        // while evaluating a function argument, i.e. before any lock is taken.
        extern std::atomic<uint32_t> g_enabled_intc_mask;
        extern std::atomic<uint32_t> g_enabled_dmac_mask;
        extern uint64_t g_vsync_tick_counter;
        extern VSyncFlagRegistration g_vsync_registration;
        constexpr uint32_t kPendingIntcMaxAgeTicks = 120u;
    }

    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause);
    void raisePendingIntc(uint32_t cause);
    void drainPendingIntc(uint8_t *rdram, PS2Runtime *runtime);
    // Test-support: reset all INTC/DMAC handler bookkeeping to process-start
    // defaults so regression tests are order-independent.
    void resetInterruptHandlerState();
    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime);
    void stopInterruptWorker();
    // Signal-only variant: sets the stop flag and wakes the worker but does
    // NOT join. For callers on the guest executor thread (a fiber calling
    // requestStop): joining there can deadlock against a worker blocked in
    // async_guest_begin(), whose wait predicate (g_running_fiber == nullptr)
    // cannot become true while the joining fiber is itself the running fiber.
    // The join happens later in scheduler_shutdown() on the main thread
    // (stopInterruptWorker is idempotent).
    void signalInterruptWorkerStop();
    uint64_t GetCurrentVSyncTick(PS2Runtime *runtime);
    void WaitVSyncTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int fixedResult);
    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
