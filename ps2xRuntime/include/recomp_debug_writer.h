#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// RecompDbg — used by ps2EntryRunner.exe only.
// Implementation is in src/lib/recomp_debug_writer.cpp (Windows only).
// ---------------------------------------------------------------------------

struct DbgGsSnapshot;
struct DbgPadSnapshot;
struct DbgLogEntry;
struct DbgThreadInfo;

#ifdef _WIN32
namespace RecompDbg {
    bool Init();
    // tid identifies which PS2 (guest) thread this snapshot belongs to
    // (BUG-021: previously a single shared slot was written by every host
    // thread with no tid tag, producing torn/spliced pc+gpr reads once more
    // than one PS2 thread was live). Claims/updates that thread's slot in
    // RecompDebugState::thread_states[].
    void Update(int32_t tid, uint32_t pc, const uint32_t* gpr, uint32_t hi, uint32_t lo, uint64_t cycles,
                const uint8_t* rdram = nullptr, uint32_t rdram_size = 0);
    // Called once per video frame (not per-dispatch) to refresh the heavier,
    // non-CPU panels (GS regs, pad state, runtime log ring).
    void UpdateExtended(const DbgGsSnapshot& gs, const DbgPadSnapshot pad[2],
                         const DbgLogEntry* logs, uint32_t logCount, uint64_t nextSeq);
    // Called once per video frame with a snapshot of the guest thread
    // scheduler (ps2_syscalls::getThreadDebugSnapshot()), already copied out
    // from under g_thread_map_mutex by the caller.
    void UpdateThreads(const DbgThreadInfo* threads, uint32_t count);
    // Called from IopRuntime::threadProc() once per R3000 step loop iteration.
    void UpdateIop(uint32_t pc, const uint32_t* gpr, uint32_t hi, uint32_t lo, bool running);
    // Reads the debugger-armed pad override for the given PS2 port (0/1),
    // written continuously by the GUI's Input tab while a host controller is
    // mapped to it (see RecompDebugState::pad_override[]). Returns false
    // (leaving out untouched) if not connected or port is out of range.
    bool GetPadOverride(int port, bool& enabled, uint16_t& buttons,
                         uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry);
    // gpr is the current EE GPR file (32 x 32-bit low words), used to evaluate
    // any armed slot's condition (bp_slots[i].cond_reg/cond_value). tid is
    // the PS2 thread evaluating this pc, recorded into bp_hit_tid/checked
    // against step_requested_tid (BUG-021).
    // gpr is non-const: while halted (bp_hit), a debugger-armed reg_write
    // request is applied directly into this array so the caller can write it
    // back into the live CPU context (see ps2_runtime.cpp call sites).
    bool CheckBreakpoint(int32_t tid, uint32_t phys_pc, uint32_t* gpr);
    void Shutdown();
}
#else
namespace RecompDbg {
    inline bool Init()                                                                                              { return false; }
    inline void Update(int32_t, uint32_t, const uint32_t*, uint32_t, uint32_t, uint64_t, const uint8_t*, uint32_t) {}
    inline void UpdateExtended(const DbgGsSnapshot&, const DbgPadSnapshot[2], const DbgLogEntry*, uint32_t, uint64_t) {}
    inline void UpdateThreads(const DbgThreadInfo*, uint32_t)                                                        {}
    inline void UpdateIop(uint32_t, const uint32_t*, uint32_t, uint32_t, bool)                                      {}
    inline bool GetPadOverride(int, bool&, uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&)                       { return false; }
    inline bool CheckBreakpoint(int32_t, uint32_t, uint32_t*)                                                        { return false; }
    inline void Shutdown()                                                                                           {}
}
#endif
