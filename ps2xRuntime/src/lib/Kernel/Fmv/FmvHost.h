// FmvHost.h -- module-internal header for the detached host-side FMV player.
//
// NOT a public runtime header. Nothing outside src/lib/Kernel/Fmv/ includes this,
// and it deliberately leaks neither FFmpeg nor raylib types, so the two facades
// (FmvDecoder.cpp / FmvPresent.cpp) never see each other's headers. That
// separation is why MPEG.cpp compiles today next to a raylib TU: raylib's
// CloseWindow/ShowCursor collide with <windows.h>, and FFmpeg pulls in enough
// that keeping the surfaces apart is cheaper than fighting it.
//
// The three exported entry points live in FmvHost.cpp and are declared in
// ps2_runtime.cpp directly (extern "C", no header) so that adding this feature
// does not touch a header and therefore does not trigger a 30h full rebuild.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ps2x_fmv
{
    // ---------------------------------------------------------------------
    // Shared state machine
    // ---------------------------------------------------------------------
    enum class State : int
    {
        Idle = 0,   // nothing requested
        Requested,  // guest asked; player thread not yet spun up
        Playing,    // decoding + presenting
        Finished,   // end of stream reached cleanly
        Failed      // could not play -- MUST degrade to skip semantics, never hang
    };

    // ---------------------------------------------------------------------
    // Decoder facade -- implemented in FmvDecoder.cpp (FFmpeg only, no raylib)
    // ---------------------------------------------------------------------
    struct StreamInfo
    {
        int width = 0;
        int height = 0;
        double durationSec = 0.0;
        double fps = 0.0;
        bool hasAudio = false;
        int audioRate = 0;
        int audioChannels = 0;
        std::string videoCodec;
        std::string audioCodec;
    };

    struct VideoFrame
    {
        double pts = 0.0;   // seconds, stream time base applied
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    class Source
    {
    public:
        Source();
        ~Source();
        Source(const Source &) = delete;
        Source &operator=(const Source &) = delete;

        bool open(const std::string &path, std::string &err);
        void close();

        const StreamInfo &info() const;

        // Decodes until at least one video frame lands in `vq`, or the stream
        // ends. Any audio decoded on the way is appended to `pcm` as interleaved
        // signed 16-bit at info().audioRate / info().audioChannels.
        //
        // Returns false on EOF or fatal error -- distinguish with eof().
        bool pump(std::deque<VideoFrame> &vq, std::vector<int16_t> &pcm, size_t maxQueued);

        bool eof() const;
        const std::string &error() const;

    private:
        struct Impl;
        Impl *m_impl = nullptr;
    };

    // False in a PS2X_ENABLE_FFMPEG=OFF build. The three extern "C" symbols must
    // still exist in that case or ps2_runtime.cpp will not link, so FmvDecoder.cpp
    // ships an #else stub rather than compiling out.
    bool decoderAvailable();

    // ---------------------------------------------------------------------
    // Present facade -- implemented in FmvPresent.cpp (raylib only, no FFmpeg)
    // ---------------------------------------------------------------------

    // Present thread ONLY. rlgl is bound to the GL context InitWindow created on
    // the main thread; calling these from the player thread is undefined
    // behaviour and a hard crash on some drivers.
    void presentDraw();
    void presentRelease();

    // Player thread. Safe off-thread: every raylib AudioStream entry point takes
    // ma_mutex_lock(&AUDIO.System.lock). The one exception is
    // SetAudioStreamBufferSizeDefault, which audioStart() calls exactly once
    // before LoadAudioStream while no other stream exists.
    bool audioStart(int sampleRate, int channels, float volume);
    size_t audioChunkFrames();
    // Submits whole chunks only. UpdateAudioStreamInLockedState zero-pads a short
    // write to the full sub-buffer, which is an audible click rather than a
    // partial fill -- so a short tail is never submitted here.
    size_t audioSubmit(const int16_t *interleaved, size_t frames);
    uint64_t audioSubmittedFrames();
    void audioStop();
    bool audioActive();

    // ---------------------------------------------------------------------
    // Cross-thread plumbing -- implemented in FmvHost.cpp
    // ---------------------------------------------------------------------
    State state();
    bool aborted();
    void requestAbort(const char *why);

    // Player thread -> present thread. Mutex + swap; at 30 fps and <=917 KB a
    // frame the lock is free, and it is far easier to argue about than a
    // lock-free triple buffer.
    void publishFrame(const uint8_t *rgba, int width, int height);
    bool takeFrame(std::vector<uint8_t> &dst, int &width, int &height);

    // Frame accounting for the completion line. publishFrame() counts frames the
    // decoder handed over; notePresented() is called by the PRESENT thread only
    // after a frame actually reached the GPU. The two differ whenever the host
    // frame loop turns over slower than the movie -- under det=1 it usually does
    // (4-7 vbl/s measured), so "decoded" is NOT evidence that a frame was drawn.
    void notePresented();
    uint64_t publishedFrames();
    uint64_t presentedFrames();
    void resetFrameCounters();

    // Env knobs, parsed once.
    bool hostEnabled();
    bool audioDisabled();
    bool skipKeyDisabled();
    float volume();
    unsigned probeTicks();
}
