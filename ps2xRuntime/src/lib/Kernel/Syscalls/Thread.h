#pragma once

#include "ps2_syscalls.h"
#include <vector>

namespace ps2_syscalls
{
    // Plain-data snapshot of one PS2 thread's scheduler state, safe to copy
    // out from under g_thread_map_mutex without exposing ThreadInfo (which
    // holds a std::mutex/condition_variable) to callers outside Kernel/.
    // Restored 2026-07-14 for the RecompDebugger revival (was dropped along
    // with the rest of the debug IPC layer when the repo was flattened).
    struct ThreadDebugSnapshot
    {
        int tid = 0;
        uint32_t entry = 0;
        uint32_t currentPc = 0;
        uint32_t stack = 0;
        int status = 0;
        int waitType = 0;
        int waitId = 0;
        int currentPriority = 0;
    };

    // Snapshots g_threads under g_thread_map_mutex. Safe to call from any
    // thread; used by the debug IPC layer once per video frame.
    std::vector<ThreadDebugSnapshot> getThreadDebugSnapshot();

    void FlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ResetEE(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SetMemoryMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void InitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
