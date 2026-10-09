#pragma once

#include <cstddef>
#include <cstdint>

// SPU2 device model (E1, 2026-10-09). Behind the IOP's SPU2 register window (0x1F900000..0x1F9007FF):
// 2 MB sample RAM, 2 cores x 24 voices, ADPCM decode, ADSR, pitch, volumes, KON/KOFF, real ENDX/NAX, 48 kHz mix.
// Register names/offsets follow PCSX2 pcsx2/SPU2/regs.h (core 1 = +0x400). Reverb, noise and the AutoDMA input
// path are NOT modelled yet (the census decides; see build_scripts/spu2/spu2_census.py).
//
// Driven only from ps2xIOP/src/emulator: IopMemory (register writes, DMA) and the emulator clock (advance).
// Time is IOP cycles (36.864 MHz => 768 cycles per 48 kHz frame), never wall time, so output is deterministic.
namespace ps2x::iop::spu2
{
    constexpr uint32_t kBase = 0x1F900000u;
    constexpr uint32_t kSize = 0x800u;
    constexpr uint32_t kCyclesPerFrame = 768u;

    void reset();

    // 16-bit register access, `off` = byte offset from kBase (even).
    void writeReg(uint32_t off, uint16_t value);
    // True and *value filled when the register is live device state (ENDX, NAX, ENVX, VOLX); false = use the stored value.
    bool readLive(uint32_t off, uint16_t *value);

    // SPU DMA (IOP channel 4 = core 0, channel 7 = core 1). Copies at the core's TSA and advances it.
    void dmaToSpu(int core, const uint8_t *src, size_t bytes);
    void dmaFromSpu(int core, uint8_t *dst, size_t bytes);

    // Generate all frames due up to `iopCycles` (monotonic). Cheap when nothing is keyed on.
    void advance(uint64_t iopCycles);

    // Host side: pull interleaved L,R int16 frames. Returns frames written.
    size_t popFrames(int16_t *dst, size_t maxFrames);
    size_t framesQueued();
    uint64_t framesGenerated();
}
