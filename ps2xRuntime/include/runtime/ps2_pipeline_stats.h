#pragma once

// Aggregate counters for the VIF1 -> VU1 -> XGKICK -> GIF geometry path.
//
// Why this exists: [gs:frame] proved the game submits >170k primitives per
// report interval but every one has tme=0 and primmask=0x41 (POINT+SPRITE
// only, never a triangle). Those are the shapes the EE builds directly into
// PATH3 GIF packets. Real 3D geometry on the PS2 arrives via
// DMA1 -> VIF1 -> MSCAL -> VU1 -> XGKICK -> GIF PATH1, and that entire chain
// currently has *no* probe on it -- so its silence in the log is a false
// negative, not evidence (see the "capped probes are false negatives" rule:
// absence only means something when the emit site is known to be live).
//
// These counters are unconditional and lock-free so they cannot themselves be
// throttled into a false negative. They are reported once per present, next to
// [gs:frame], as [vu:frame].
//
// Header-only inline variables so this needs no new TU and no CMakeLists edit.

#include <atomic>
#include <cstdint>

namespace ps2_pipeline_stats
{
    // DMA1 payload actually handed to the VIF1 interpreter.
    inline std::atomic<uint64_t> g_vif1Calls{0};
    inline std::atomic<uint64_t> g_vif1Bytes{0};

    // VIF1 microprogram triggers.
    inline std::atomic<uint64_t> g_mscal{0};
    inline std::atomic<uint64_t> g_mscnt{0};

    // VU1 microprogram runs that actually entered the interpreter.
    inline std::atomic<uint64_t> g_vu1Runs{0};

    // XGKICK: the only way VU1-built geometry can reach the GS.
    inline std::atomic<uint64_t> g_xgkicks{0};
    inline std::atomic<uint64_t> g_xgkickBytes{0};

    // Round 2. vu1runs>0 with xgkicks==0 means the microprogram is invoked but
    // never kicks. The two candidate explanations are distinguished here:
    //  - mpgUploads==0  -> there is no microprogram; VU1 is running zero-filled
    //                      code memory, which decodes as NOPs with no E-bit and
    //                      therefore burns the full cycle budget every MSCAL.
    //  - mpgUploads>0 and instrs small with endEbit>0 -> a real program runs to
    //                      completion and simply never reaches an XGKICK.
    inline std::atomic<uint64_t> g_mpgUploads{0};
    inline std::atomic<uint64_t> g_mpgBytes{0};

    // Instruction pairs retired, and how each run terminated.
    inline std::atomic<uint64_t> g_vu1Instrs{0};
    inline std::atomic<uint64_t> g_vu1EndEbit{0};
    inline std::atomic<uint64_t> g_vu1EndCycleLimit{0};
    inline std::atomic<uint64_t> g_vu1EndRange{0};

    // Round 3. mpgUploads==0 for the entire run -- boot included -- while MSCAL
    // fires 30x/frame. So either the EE uploads VU1 micro memory directly through
    // its mapped 0x11008000 window (in which case code memory is NOT empty and the
    // fault is in execution), or nothing uploads it at all. These separate the two:
    //
    //  - codeNonzero==0 -> VU1 code memory is genuinely empty. No upload path is
    //                     reaching it, and the MSCALs are running zeros as NOPs.
    //  - codeNonzero>0  -> a microprogram IS resident. The bug is downstream, in
    //                     decode/branch/XGKICK, not in the upload.
    //
    // dataNonzero tells us separately whether VIF1 UNPACK is landing geometry in
    // VU1 data memory, which is what the ~8 MB/interval should be doing.
    // Both are running maxima, not sums.
    inline std::atomic<uint64_t> g_vu1CodeNonzero{0};
    inline std::atomic<uint64_t> g_vu1DataNonzero{0};

    // Last MSCAL entry point, to confirm the game is not just calling address 0.
    inline std::atomic<uint64_t> g_lastMscalPC{0};

    // Census of VIF1 opcodes actually seen (bit N = opcode N). Opcodes are masked
    // to 7 bits, so two words cover the space. If MPG (0x4A) is absent here the
    // upload truly never enters the VIF1 stream.
    inline std::atomic<uint64_t> g_vif1OpLo{0}; // opcodes 0x00-0x3F
    inline std::atomic<uint64_t> g_vif1OpHi{0}; // opcodes 0x40-0x7F

    inline void noteVif1Opcode(uint8_t op) noexcept
    {
        if (op < 64)
            g_vif1OpLo.fetch_or(1ull << op, std::memory_order_relaxed);
        else
            g_vif1OpHi.fetch_or(1ull << (op - 64), std::memory_order_relaxed);
    }

    // Round 4. Counters are exhausted and were, in the end, misleading: the
    // round-3 census showed ~57 distinct opcodes including 11 that do not exist
    // in the VIF ISA, and mscalpc=0x1b9d0 implies an MSCAL immediate of 14138
    // against a hard maximum of 2047. The stream is being mis-parsed, so every
    // aggregate drawn from it -- including the MPG bit being set -- is noise.
    // The remaining question is positional, so this captures one real buffer
    // per report interval to be walked by hand.
    //
    // Note the parser silently swallows unrecognized opcodes, advancing only 4
    // bytes (ps2_vif1_interpreter.cpp, the trailing else). Once desynced it can
    // neither recover nor complain, which is why firstBadPos matters: it is the
    // first byte offset where the parse is provably wrong, and the command that
    // precedes it is the one that advanced pos by the wrong amount.

    // Opcodes the VIF1 ISA actually defines. Deliberately independent of the
    // parser's if-chain so the probe cannot inherit the parser's own mistake.
    inline bool isValidVif1Opcode(uint8_t op) noexcept
    {
        if (op <= 0x07u)
            return true; // NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK
        if (op == 0x10u || op == 0x11u || op == 0x13u)
            return true; // FLUSHE, FLUSH, FLUSHA
        if (op == 0x14u || op == 0x15u || op == 0x17u)
            return true; // MSCAL, MSCALF, MSCNT
        if (op == 0x20u)
            return true; // STMASK
        if (op == 0x30u || op == 0x31u)
            return true; // STROW, STCOL
        if (op == 0x4Au)
            return true; // MPG
        if (op == 0x50u || op == 0x51u)
            return true; // DIRECT, DIRECTHL
        if ((op & 0x60u) == 0x60u)
            return true; // UNPACK 0x60-0x7F
        return false;
    }

    constexpr uint32_t kVif1SnapHeadBytes = 128u; // 32 VIF words -- enough to see packet structure

    struct Vif1Snap
    {
        uint32_t sizeBytes;    // buffer handed to processVIF1Data
        uint32_t endPos;       // pos when the parse loop exited; < sizeBytes means it bailed
        uint32_t firstBadPos;  // byte offset of the first invalid opcode, 0xFFFFFFFF if none
        uint32_t firstBadCmd;  // the raw 32-bit VIFcode word there
        uint32_t badCount;     // invalid opcodes in this buffer
        uint32_t cmdCount;     // command words decoded
        uint32_t headBytes;    // valid bytes in head[]
        uint8_t head[kVif1SnapHeadBytes];
    };

    // 0 = free, 1 = a writer owns it, 2 = filled and waiting to be printed.
    inline std::atomic<uint32_t> g_vif1SnapState{0};
    inline Vif1Snap g_vif1Snap{};

    // True if this caller now owns the slot and must fill it and publish.
    inline bool claimVif1Snap() noexcept
    {
        uint32_t expected = 0u;
        return g_vif1SnapState.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel);
    }

    inline void publishVif1Snap() noexcept
    {
        g_vif1SnapState.store(2u, std::memory_order_release);
    }

    // Copies out a filled snapshot and re-arms the slot for the next interval.
    inline bool takeVif1Snap(Vif1Snap &out) noexcept
    {
        if (g_vif1SnapState.load(std::memory_order_acquire) != 2u)
            return false;
        out = g_vif1Snap;
        g_vif1SnapState.store(0u, std::memory_order_release);
        return true;
    }

    // Records a running maximum without clobbering a larger concurrent value.
    inline void noteMax(std::atomic<uint64_t> &slot, uint64_t value) noexcept
    {
        uint64_t prev = slot.load(std::memory_order_relaxed);
        while (value > prev && !slot.compare_exchange_weak(prev, value, std::memory_order_relaxed))
        {
        }
    }

    struct Snapshot
    {
        uint64_t vif1Calls;
        uint64_t vif1Bytes;
        uint64_t mscal;
        uint64_t mscnt;
        uint64_t vu1Runs;
        uint64_t xgkicks;
        uint64_t xgkickBytes;
        uint64_t mpgUploads;
        uint64_t mpgBytes;
        uint64_t vu1Instrs;
        uint64_t endEbit;
        uint64_t endCycleLimit;
        uint64_t endRange;
        uint64_t codeNonzero;
        uint64_t dataNonzero;
        uint64_t lastMscalPC;
        uint64_t opLo;
        uint64_t opHi;
    };

    // Drains the interval counters, matching [gs:frame]'s exchange() style so
    // the numbers describe the interval since the previous report.
    inline Snapshot drain()
    {
        Snapshot s;
        s.vif1Calls = g_vif1Calls.exchange(0, std::memory_order_relaxed);
        s.vif1Bytes = g_vif1Bytes.exchange(0, std::memory_order_relaxed);
        s.mscal = g_mscal.exchange(0, std::memory_order_relaxed);
        s.mscnt = g_mscnt.exchange(0, std::memory_order_relaxed);
        s.vu1Runs = g_vu1Runs.exchange(0, std::memory_order_relaxed);
        s.xgkicks = g_xgkicks.exchange(0, std::memory_order_relaxed);
        s.xgkickBytes = g_xgkickBytes.exchange(0, std::memory_order_relaxed);
        s.mpgUploads = g_mpgUploads.exchange(0, std::memory_order_relaxed);
        s.mpgBytes = g_mpgBytes.exchange(0, std::memory_order_relaxed);
        s.vu1Instrs = g_vu1Instrs.exchange(0, std::memory_order_relaxed);
        s.endEbit = g_vu1EndEbit.exchange(0, std::memory_order_relaxed);
        s.endCycleLimit = g_vu1EndCycleLimit.exchange(0, std::memory_order_relaxed);
        s.endRange = g_vu1EndRange.exchange(0, std::memory_order_relaxed);
        s.codeNonzero = g_vu1CodeNonzero.exchange(0, std::memory_order_relaxed);
        s.dataNonzero = g_vu1DataNonzero.exchange(0, std::memory_order_relaxed);
        s.lastMscalPC = g_lastMscalPC.load(std::memory_order_relaxed);
        // The opcode census is cumulative on purpose: a single MPG anywhere in the
        // run is the answer, and draining it per interval could hide it.
        s.opLo = g_vif1OpLo.load(std::memory_order_relaxed);
        s.opHi = g_vif1OpHi.load(std::memory_order_relaxed);
        return s;
    }
}
