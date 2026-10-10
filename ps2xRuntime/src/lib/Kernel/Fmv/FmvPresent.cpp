// FmvPresent.cpp -- raylib half of the detached host-side FMV player.
//
// NO FFMPEG IN THIS FILE. See FmvHost.h for why the two facades are kept apart.
//
// THREADING (this is the part that bites)
// ---------------------------------------
// PS2Runtime::run() spawns gameThread (ps2_runtime.cpp:3525) for ALL guest
// execution, while the present loop (:6278) stays on the thread that called
// run(). So:
//
//   * presentDraw()/presentRelease() are PRESENT THREAD ONLY. rlgl is bound to
//     the GL context InitWindow created on the main thread; UpdateTexture or
//     DrawTexturePro from the player thread is UB and a hard crash on some
//     drivers.
//   * audio* are safe from the player thread: every raylib AudioStream entry
//     point takes ma_mutex_lock(&AUDIO.System.lock). The single exception is
//     SetAudioStreamBufferSizeDefault, a bare write to AUDIO.Buffer.defaultSize,
//     which audioStart() does exactly once before LoadAudioStream while no other
//     stream exists. Nothing else in the runtime loads an AudioStream today --
//     ps2_audio.cpp only uses LoadSoundFromWave/PlaySound -- so that global is
//     uncontended.

#include "FmvHost.h"

#include "ps2_host_backend.h" // raylib.h

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>

namespace ps2x_fmv
{
    namespace
    {
        // ---------------- present thread state ----------------
        Texture2D s_tex{};
        int s_texW = 0;
        int s_texH = 0;
        std::vector<uint8_t> s_frameCpu;

        // ---------------- audio state (player thread) ----------------
        AudioStream s_stream{};
        bool s_streamLoaded = false;
        std::atomic<uint64_t> s_submittedFrames{0};

        // Must equal the stream's real sub-buffer size.
        // UpdateAudioStreamInLockedState ZERO-PADS any write shorter than the
        // sub-buffer -- that is an audible click, not a partial fill -- and warns
        // and drops on any write longer. LoadAudioStream picks
        // max(AUDIO.Buffer.defaultSize, device period), and the WASAPI shared-mode
        // period is 480-960 frames, so pinning defaultSize to 2048 makes the
        // sub-buffer deterministically 2048.
        constexpr int kChunkFrames = 2048;
        int s_channels = 2;

        void releaseTexture()
        {
            if (s_tex.id != 0)
            {
                UnloadTexture(s_tex);
                s_tex = Texture2D{};
            }
            s_texW = 0;
            s_texH = 0;
        }
    }

    // -----------------------------------------------------------------
    // Present thread
    // -----------------------------------------------------------------
    void presentDraw()
    {
        const State st = state();
        const bool active = (st == State::Requested || st == State::Playing);

        if (!active)
        {
            releaseTexture();
            return;
        }

        // Skip key. NOT KEY_ESCAPE: WindowShouldClose() (ps2_runtime.cpp:6525)
        // consumes escape to quit the whole runtime.
        if (!skipKeyDisabled())
        {
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) ||
                IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            {
                requestAbort("skip key");
            }
        }

        int w = 0;
        int h = 0;
        if (takeFrame(s_frameCpu, w, h) && w > 0 && h > 0)
        {
            if (s_texW != w || s_texH != h)
            {
                releaseTexture();
                Image img = GenImageColor(w, h, BLACK); // already R8G8B8A8
                s_tex = LoadTextureFromImage(img);
                UnloadImage(img);
                // A 256-wide source blown up ~2.7x horizontally looks awful with
                // the default point filter.
                SetTextureFilter(s_tex, TEXTURE_FILTER_BILINEAR);
                s_texW = w;
                s_texH = h;
            }
            if (s_tex.id != 0 && s_frameCpu.size() >= static_cast<size_t>(w) * h * 4u)
            {
                UpdateTexture(s_tex, s_frameCpu.data());
                // Counted HERE, not at takeFrame(): this is the instruction that
                // puts the pixels on the GPU. A frame taken but not uploaded was
                // never presented.
                notePresented();
            }
        }

        const float sw = static_cast<float>(GetScreenWidth());
        const float sh = static_cast<float>(GetScreenHeight());

        // Unconditional black fill, every frame, BEFORE the video. UploadFrame's
        // failure path fills the guest texture MAGENTA (ps2_runtime.cpp:1061),
        // which is exactly the logo-phase state -- without this the letterbox
        // bars are magenta.
        DrawRectangle(0, 0, static_cast<int>(sw), static_cast<int>(sh), BLACK);

        if (s_tex.id == 0 || s_texW <= 0 || s_texH <= 0)
        {
            return;
        }

        // Force 4:3. The stream SAR is 200:219, which is garbage (DAR 0.52), and
        // BOTH 256x448 and 512x448 are PS2 NTSC anamorphic sources meant for a
        // 4:3 raster -- so neither the pixel dimensions nor the stream SAR give
        // the right aspect.
        constexpr float kDar = 4.0f / 3.0f;
        float dw = sw;
        float dh = sw / kDar;
        if (dh > sh)
        {
            dh = sh;
            dw = sh * kDar;
        }
        const Rectangle src{0.0f, 0.0f, static_cast<float>(s_texW), static_cast<float>(s_texH)};
        const Rectangle dst{(sw - dw) * 0.5f, (sh - dh) * 0.5f, dw, dh};
        DrawTexturePro(s_tex, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    }

    void presentRelease()
    {
        releaseTexture();
        s_frameCpu.clear();
        s_frameCpu.shrink_to_fit();
    }

    // -----------------------------------------------------------------
    // Audio (player thread)
    // -----------------------------------------------------------------
    bool audioStart(int sampleRate, int channels, float vol)
    {
        if (s_streamLoaded)
        {
            return true;
        }
        if (!IsAudioDeviceReady())
        {
            std::cerr << "[fmvhost] audio device not ready -- video only" << std::endl;
            return false;
        }
        s_channels = (channels == 1) ? 1 : 2;
        SetAudioStreamBufferSizeDefault(kChunkFrames);
        s_stream = LoadAudioStream(static_cast<unsigned int>(sampleRate), 16u,
                                   static_cast<unsigned int>(s_channels));
        if (!IsAudioStreamValid(s_stream))
        {
            std::cerr << "[fmvhost] LoadAudioStream failed -- video only" << std::endl;
            return false;
        }
        SetAudioStreamVolume(s_stream, vol);
        PlayAudioStream(s_stream);
        s_streamLoaded = true;
        s_submittedFrames.store(0, std::memory_order_relaxed);
        return true;
    }

    size_t audioChunkFrames() { return static_cast<size_t>(kChunkFrames); }

    size_t audioSubmit(const int16_t *interleaved, size_t frames)
    {
        if (!s_streamLoaded || interleaved == nullptr)
        {
            return 0;
        }
        size_t consumed = 0;
        while (frames - consumed >= static_cast<size_t>(kChunkFrames) &&
               IsAudioStreamProcessed(s_stream))
        {
            UpdateAudioStream(s_stream,
                              interleaved + consumed * static_cast<size_t>(s_channels),
                              kChunkFrames);
            consumed += static_cast<size_t>(kChunkFrames);
            s_submittedFrames.fetch_add(static_cast<uint64_t>(kChunkFrames),
                                        std::memory_order_relaxed);
        }
        return consumed;
    }

    uint64_t audioSubmittedFrames()
    {
        return s_submittedFrames.load(std::memory_order_relaxed);
    }

    bool audioActive() { return s_streamLoaded; }

    void audioStop()
    {
        if (!s_streamLoaded)
        {
            return;
        }
        StopAudioStream(s_stream);
        UnloadAudioStream(s_stream);
        s_stream = AudioStream{};
        s_streamLoaded = false;
    }
}
