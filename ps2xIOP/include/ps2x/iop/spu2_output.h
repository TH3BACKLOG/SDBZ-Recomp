#pragma once

#include <cstddef>
#include <cstdint>

// Host-facing side of the SPU2 device model (ps2xIOP/src/emulator/core/spu2.cpp). Pull-only: the host
// audio callback reads frames the device already generated; nothing here feeds back into emulation.
namespace ps2x::iop::spu2
{
    // Interleaved L,R int16 at 48 kHz. Returns frames written (<= maxFrames).
    size_t popFrames(int16_t *dst, size_t maxFrames);
    size_t framesQueued();
}
