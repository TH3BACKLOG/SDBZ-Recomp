// VU1 micro-program recompiler: interface between VU1Interpreter::run() and the
// generated per-image code (vu1rc_<crc>.cpp, written by build_scripts/vu1_recomp.py).
//
// Not included by runner code (only by ps2_vu1_core.cpp and the generated TUs),
// so it can change without the 30-hour rebuild.
#pragma once

#include <cstdint>

struct VU1State;

namespace vu1rc
{
    // Why a recompiled program stopped.
    enum EndReason : uint32_t
    {
        kEndEbit = 0,       // E-bit + delay slot: normal end, pipelines flushed
        kEndCycleLimit = 2, // ran past the cycle budget (interpreter would stop mid-program)
        kEndError = 3,      // unknown JR target / bad GIF tag: logged, program stopped
    };

    struct Ctx
    {
        // In.
        VU1State *st = nullptr;
        uint8_t *mem = nullptr;     // VU1 data memory
        uint32_t memSize = 0;       // power of two
        uint8_t *kickBuf = nullptr; // 64 KB XGKICK packet buffer
        uint32_t budget = 0;        // cycle budget (relative)
        void *self = nullptr;
        // PATH1 packet complete. srcAddr = byte address in VU1 data memory.
        void (*submitKick)(void *self, uint32_t srcAddr, const uint8_t *pkt, uint32_t bytes) = nullptr;
        // In/out: m_workingClip and the branch VI backup (readBranchVi) left
        // by the last pair, which survives into a resumed program.
        uint32_t workingClip = 0;
        bool bkValid = false;
        uint8_t bkReg = 0;
        int32_t bkVal = 0;
        // Out.
        uint64_t cycles = 0;  // cycles used (m_cycle advances by this)
        uint64_t retired = 0; // instruction pairs executed
        uint32_t end = kEndEbit;
    };

    // Runs from ctx.st->pc until the program ends. Returns false (and does
    // nothing) when the entry pc is not one this image was compiled for.
    using Program = bool (*)(Ctx &);

    // Programs whose compiled code bytes all match this micro memory (the rest
    // of the image may differ). Writes up to maxOut; returns the count. Callers
    // cache the result per code generation and try each in turn (a program
    // declines entry pcs it was not compiled for).
    uint32_t find(const uint8_t *microMem, uint32_t size, Program *out, uint32_t maxOut);

    // A computed JR/JALR can land on a pair the recompiler never made a label
    // (the target comes from VU data, e.g. a subroutine index the EE uploads).
    // The generated code stops with kEndError and reports it; takeJrMiss()
    // returns true once for that stop and gives the target pc, and the caller
    // resumes in the interpreter there instead of dropping the packet.
    void clearJrMiss();
    bool takeJrMiss(uint32_t &target);

    // CRC-32 of the image (same as the vucap tools), for logs and dumps.
    uint32_t imageCrc(const uint8_t *microMem, uint32_t size);

    // PS2X_VU1_RECOMP: 0 = off, 1 = on (default), 2 = verify (run both, compare).
    int mode();
}
