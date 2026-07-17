#pragma once
#include "cpu_backend.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "recomp_debug_ipc.h"

// ---------------------------------------------------------------------------
// RecompilerBackend — reads EE state from the shared memory block written
// by ps2EntryRunner.exe (RecompDbg::Update).
// ---------------------------------------------------------------------------

class RecompilerBackend : public CpuBackend {
public:
    bool Connect()    override;
    void Disconnect() override;
    bool IsConnected() const override { return m_shm != nullptr && m_shm->recomp_running; }
    bool IsPaused()    const override { return m_shm && m_shm->bp_hit; }
    bool ReadRegs(CpuRegs& out) override;
    // CpuBackend's single-breakpoint interface — operates on slot 0 for
    // callers that don't need the multi-slot API below.
    void SetBreakpoint(uint32_t addr) override;
    void ClearBreakpoint()            override;
    void Resume()                     override;
    void Pause()                      override;
    void Step()                       override;
    // Resumes toward the current return address (gpr[31]). Resolved
    // JAL/JALR calls compile to direct native C++ calls that return via
    // ordinary C++ unwinding, never re-entering the dispatch loop mid-call --
    // so an address-match breakpoint armed at a nested `ra` is never actually
    // reached (confirmed empirically: it free-runs past it). Instead this
    // repeatedly re-arms step_requested_tid (the mechanism Step() uses, which
    // *does* fire reliably on every dispatch/call-boundary) and polls via
    // PollStepOver() until the observed pc lands on `ra`. Falls back to
    // Step() if gpr[31] is zero.
    void StepOver();
    // Must be called once per frame (e.g. alongside ReadRegs()) while a
    // StepOver() is in flight; drives the repeated-step loop described above.
    // No-op if no StepOver() is in progress.
    void PollStepOver();

    // Multi-slot breakpoints (up to kDbgMaxBreakpoints), optionally
    // conditional on a GPR value. Returns the slot index used, or -1 if all
    // slots are full.
    int  AddBreakpoint(uint32_t addr, uint8_t cond_reg = 0xFF, uint32_t cond_value = 0);
    void RemoveBreakpoint(uint32_t addr);
    void ClearAllBreakpoints();
    // Physical address of the slot that most recently fired (valid while paused).
    uint32_t HitAddress() const;

    // Raw access to the extended (GS/pad/log) snapshot fields that have no
    // generic CpuBackend equivalent — only meaningful for the RECOMP source.
    // Returns nullptr if not connected or the shm layout version doesn't match.
    const struct RecompDebugState* ReadExtended() const;

    // On-demand memory read/write at an arbitrary EE physical address.
    // Blocks (bounded poll) until the recomp dispatch loop services the
    // request. Returns false if not connected, size exceeds
    // kDbgMemReqMaxSize, or the poll times out.
    bool ReadMemory(uint32_t addr, void* out, uint32_t size);
    bool WriteMemory(uint32_t addr, const void* data, uint32_t size);

    // Writes a single GPR's low 32 bits back into the live context. Only
    // possible while halted (IsPaused()) -- there's no live ctx pointer to
    // write into otherwise. index must be 1..31 ($zero is never writable).
    bool WriteRegister(int index, uint32_t value);

    // Streams the debugger's own host-controller state into the runner's pad
    // override for the given PS2 port (0/1 = controller 1/2), so it drives
    // the running game every frame. Unlike WriteRegister, this is fire-and-
    // forget (no seq/ack) and works while the target is running, not only
    // while halted -- ps2_runtime.cpp applies it once per video frame.
    bool WritePadOverride(int port, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    // Releases the override for a port, letting the guest's normal
    // scePadRead() input (or another mapped source) take over again.
    bool ClearPadOverride(int port);

private:
    // Resolves which tracked PS2 thread the GUI is currently looking at:
    // the halted thread's tid while paused (bp_hit_tid), otherwise the
    // first claimed thread_states[] slot. Returns -1 if none is tracked yet.
    int32_t CurrentTid() const;

    HANDLE            m_hMap = nullptr;
    struct RecompDebugState* m_shm = nullptr;
    bool              m_stepOverActive   = false;
    uint32_t          m_stepOverTargetRa = 0;
    int32_t           m_stepOverTid      = -1;
};
