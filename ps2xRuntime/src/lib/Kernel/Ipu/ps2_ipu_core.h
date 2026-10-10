// PS2 Image Processing Unit (IPU) -- MPEG-1/MPEG-2 macroblock decoder.
//
// WHY THIS EXISTS. Stage 5.17 spent two weeks on "the movie never shows a
// frame". The answer was that 0x10002000-0x10002030 was pure register storage:
// commands were stored and never executed, IPU_CTRL.BUSY was permanently clear
// so the guest never spun, IPU_TOP echoed the last write, and neither IPU DMA
// channel existed. The guest ships a full software MPEG-2 driver at
// 0x423xxx-0x42Axxx that drives the IPU at macroblock granularity (it does its
// own motion compensation), so nothing above the IPU could ever notice: real
// silicon cannot accept a command and produce nothing, so CRI wrote no error
// path. mwPlyGetCurFrm just returned frm=0 forever, silently.
//
// MODEL. The IPU is driven synchronously off the guest EE thread. There is no
// IPU thread and BUSY is never reported set -- instead every entry point that
// could observe IPU state calls run(), which advances the active command as far
// as the available data allows. Input is PULLED one quadword at a time straight
// out of the ch4 (IPU_TO) DMA descriptor, exactly like hardware, so MADR/QWC/
// TADR stay truthful and the guest's halt/resume bookkeeping keeps working.
// Output is PUSHED into the ch3 (IPU_FROM) transfer as it is produced; a ch3
// kick that arrives before there is data simply stays pending, which is the
// normal ordering for Sony's FMV libraries.
//
// This header is included only by the IPU translation units and by
// ps2_memory.cpp. It is NOT reachable from any src/runner TU.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2_ipu
{
    // --- lifecycle -------------------------------------------------------
    void attachMemory(uint8_t *rdram, uint32_t ramSize, uint8_t *spr, uint32_t sprSize);
    void reset();

    // --- MMIO 0x10002000 .. 0x10002034 -----------------------------------
    // Addresses are the 32-bit slots; +4 slots carry the high halves of the
    // 64-bit IPU_CMD / IPU_TOP registers (bit 63 = busy / not-enough-data).
    bool isRegister(uint32_t address);
    void writeReg(uint32_t address, uint32_t value);
    uint32_t readReg(uint32_t address);

    // --- FIFO windows 0x10007000 (out) / 0x10007010 (in) -----------------
    bool isFifo(uint32_t address);
    void writeFifo128(uint32_t address, const uint8_t *src16);
    void readFifo128(uint32_t address, uint8_t *dst16);

    // --- DMA channels 3 (IPU_FROM, IPU->RAM) and 4 (IPU_TO, RAM->IPU) ----
    // ps2_memory owns the register file; these mirror the guest's writes into
    // the engine and read the engine's live view back out.
    constexpr uint32_t kChanFrom = 0x1000B000u;
    constexpr uint32_t kChanTo = 0x1000B400u;

    bool isDmaChannel(uint32_t channelBase);
    void writeChannelReg(uint32_t address, uint32_t value);
    uint32_t readChannelReg(uint32_t address); // advances the engine first
    uint32_t peekChannelReg(uint32_t address); // pure read, for mirroring

    // Advance the engine as far as available data allows.
    void run();

    // Consume the "channel just finished" edge so the caller can raise the
    // matching D_STAT bit (3 = IPU_FROM, 4 = IPU_TO).
    bool takeFromCompletion();
    bool takeToCompletion();

    // --- observability ---------------------------------------------------
    struct Stats
    {
        uint64_t commands;
        uint64_t bdec;
        uint64_t vdec;
        uint64_t fdec;
        uint64_t csc;
        uint64_t pack;
        uint64_t idec;
        uint64_t macroblocks;
        uint64_t bytesIn;
        uint64_t bytesOut;
        uint64_t vlcErrors;
        uint64_t stalls;
    };
    const Stats &stats();
    void logSummary();

    // --- VLC tables (ps2_ipu_tables.cpp) ---------------------------------
    // A decode returns the symbol and consumes `len` bits; len == 0 means the
    // bit pattern is not in the table (-> IPU_CTRL.ECD).
    struct VlcResult
    {
        int32_t value;
        uint32_t len;
    };

    void buildTables();
    VlcResult decodeMbAddrIncrement(uint32_t bits27);
    VlcResult decodeMbType(uint32_t pictureType, uint32_t bits27);
    VlcResult decodeMotionCode(uint32_t bits27);
    VlcResult decodeDmVector(uint32_t bits27);
    VlcResult decodeCbp(uint32_t bits27);
    VlcResult decodeDcSizeLuma(uint32_t bits27);
    VlcResult decodeDcSizeChroma(uint32_t bits27);

    // DCT coefficient tables. `run` == kDctEob marks end-of-block,
    // `run` == kDctEscape marks the escape code (caller reads the fixed-length
    // run/level that follows). `level` carries the magnitude; the sign bit is
    // NOT consumed here.
    constexpr int32_t kDctEob = -1;
    constexpr int32_t kDctEscape = -2;
    struct DctResult
    {
        int32_t run;
        int32_t level;
        uint32_t len;
    };
    DctResult decodeDctCoeff(bool tableOne, bool first, uint32_t bits27);
}
