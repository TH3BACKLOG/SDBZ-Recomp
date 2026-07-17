#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// CpuBackend — abstract interface for reading EE CPU state.
// PCSX2Backend  : attaches to PCSX2 via RPM + PINE (existing path).
// RecompilerBackend : reads from shared memory written by ps2EntryRunner.exe.
// ---------------------------------------------------------------------------

struct CpuRegs {
    uint32_t pc;
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint64_t cycles;
};

struct CpuBackend {
    virtual bool     Connect()                       = 0;
    virtual void     Disconnect()                    = 0;
    virtual bool     IsConnected()             const = 0;
    virtual bool     IsPaused()                const = 0;
    virtual bool     ReadRegs(CpuRegs& out)          = 0;
    virtual void     SetBreakpoint(uint32_t addr)    = 0;
    virtual void     ClearBreakpoint()               = 0;
    virtual void     Resume()                        = 0;
    virtual void     Pause()                         = 0; // halt on the very next dispatch iteration, regardless of PC
    virtual void     Step()                          = 0; // resume one dispatch iteration then halt again
    virtual ~CpuBackend() = default;
};
