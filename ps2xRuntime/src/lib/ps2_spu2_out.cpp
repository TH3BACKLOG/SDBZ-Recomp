// Host output for the SPU2 device model: one raylib AudioStream (48 kHz stereo s16) fed from the
// device's frame ring by the audio thread. PS2X_SPU2_HOST=0 turns it off.
//
// The device clock is IOP cycles, not wall time, so the ring can fill faster or slower than real time
// (the guest runs 0.3x-3x of real speed depending on the scene). Keep latency bounded: when more than
// ~300 ms is queued drop down to ~100 ms (skip ahead); on underrun play silence.
#include "ps2x/iop/spu2_output.h"

#include "raylib.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr size_t kMaxQueued = 14400u;
    constexpr size_t kKeepQueued = 4800u;
    AudioStream s_stream{};
    bool s_on = false;

    void audioCallback(void *buffer, unsigned int frames)
    {
        auto *out = static_cast<int16_t *>(buffer);
        namespace spu2 = ps2x::iop::spu2;
        const size_t queued = spu2::framesQueued();
        if (queued > kMaxQueued)
        {
            static int16_t scratch[4096 * 2];
            size_t drop = queued - kKeepQueued;
            while (drop)
            {
                const size_t got = spu2::popFrames(scratch, drop < 4096u ? drop : 4096u);
                if (!got)
                    break;
                drop -= got;
            }
        }
        const size_t got = spu2::popFrames(out, frames);
        if (got < frames)
            std::memset(out + got * 2u, 0, (frames - got) * 2u * sizeof(int16_t));
    }
}

void ps2xSpu2OutputStart()
{
    const char *off = std::getenv("PS2X_SPU2_HOST");
    if (s_on || (off && off[0] == '0'))
        return;
    SetAudioStreamBufferSizeDefault(1024);
    s_stream = LoadAudioStream(48000u, 16u, 2u);
    if (!IsAudioStreamValid(s_stream))
    {
        std::cerr << "[spu2] host audio stream failed to load" << std::endl;
        return;
    }
    SetAudioStreamCallback(s_stream, audioCallback);
    PlayAudioStream(s_stream);
    s_on = true;
    std::cerr << "[spu2] host audio output on (48 kHz stereo)" << std::endl;
}

void ps2xSpu2OutputStop()
{
    if (!s_on)
        return;
    StopAudioStream(s_stream);
    UnloadAudioStream(s_stream);
    s_stream = AudioStream{};
    s_on = false;
}
