// FmvHost.cpp -- the anchor TU of the detached host-side FMV player.
//
// WHAT THIS IS
// ------------
// The opening Atari / Okratron logo movies do not play. The guest's own SofDec
// path is reachable (PS2X_SKIPFMV=0) but hangs on the parked g36 bug: g36
// (0x0045F69C) latches 0->1 and never clears, sub_165300 -- the sole drainer of
// the stream handle -- is never called again, class 6 starves, thread 6 spins at
// priority 1 forever. Nothing here reopens that.
//
// Instead this decodes the real .SFD on the HOST with the FFmpeg already linked
// into ps2_runtime, presents it over raylib, and only then tells the guest's logo
// phase machine "complete". The guest never runs SofDec, so the parked hang is
// irrelevant to seeing the movie.
//
// WHY IT LIVES UNDER Kernel/ AND WHY IT IS INSTALLED FROM run()
// -------------------------------------------------------------
// CMakeLists.txt:506-512 globs src/lib/Kernel/*.cpp with GLOB_RECURSE +
// CONFIGURE_DEPENDS into ps2_runtime, so a new subdirectory here needs ZERO
// CMakeLists edits. Same trick, same reasoning as HostSampler.cpp:19-22.
//
// It deliberately does NOT use PS2_REGISTER_GAME_OVERRIDE, because that macro
// walks into two hazards at once:
//
//   1. DEAD-STRIP. ps2_runtime is a STATIC lib with no /WHOLEARCHIVE. A TU
//      reachable only through a static-initializer self-registration is silently
//      dropped at link time.
//   2. CROSS-TU ORDER. applyMatching applies descriptors in registry order;
//      within a TU that is declaration order, but ACROSS TUs static-init order is
//      unspecified. game_overrides.cpp registers applySdbzSkipFmv LAST (:8315)
//      *precisely* so it beats applySdbzSregProbe, which also replaces 0x420E70
//      (:8184). A new TU using the macro is a coin flip against that.
//
// Installing from PS2Runtime::run() kills both. applyMatching runs inside
// loadELF (ps2_runtime.cpp:1606), which main.cpp:222 calls BEFORE run(), so a
// replaceFunction issued from run() deterministically wins -- and the extern "C"
// reference from ps2_runtime.cpp is itself the link anchor. game_overrides.cpp is
// never edited, which is the whole point of the "detached" requirement: upstream
// ran-j/PS2Recomp has 183 lines in that file, we have 8,398.
//
// THREADING
// ---------
// The guest hooks run on gameThread (ps2_runtime.cpp:3525). presentDraw() runs on
// the main thread (:6278). The decode loop gets its own thread. See FmvHost.h.
//
// FAILURE POLICY
// --------------
// Every failure -- missing file, no FFmpeg, decode error, watchdog -- degrades to
// exactly skipFmvPhaseDone's behaviour ([obj+48]=0, $v0=1) so boot proceeds. The
// parked hang is never re-introduced as a failure mode.

#include "FmvHost.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace ps2x_fmv
{
    namespace
    {
        // -------------------------------------------------------------
        // Movie table
        // -------------------------------------------------------------
        // The mapping is NOT filename resemblance. game_overrides.cpp:4961-4963
        // records a PCSX2 DebugServer session on real hardware: 0x130ef0 is
        // called exactly TWICE, "movie/atari.sfd" then "movie/okr.sfd", ~7.6 s
        // apart, and :5073 records the literal disc path \MOVIE\ATARI.SFD;1.
        // Two logo apps, two opens, in that order.
        //
        // OP.SFD (named by MOVIE/0FLIST.DIR) is the later attract-mode opening
        // played by a DIFFERENT app. The table is extensible on purpose; do not
        // wire OP here without re-deriving its phase addresses.
        enum MovieId : int
        {
            kMovieAtari = 0,
            kMovieOkrtron = 1,
            kMovieCount
        };

        struct MovieDef
        {
            const char *label;
            const char *file;
            const char *envOverride;
            uint32_t openFn;
            uint32_t closeFn;
        };

        constexpr MovieDef kMovies[kMovieCount] = {
            {"ATARI", "ATARI.SFD", "PS2X_FMV_ATARI", 0x00420E70u, 0x00420FC0u},
            {"OKR", "OKR.SFD", "PS2X_FMV_OKR", 0x004216E0u, 0x00421830u},
        };

        // -------------------------------------------------------------
        // Env gating -- file-local static const + IIFE, matching the idiom in
        // game_overrides.cpp:8250-8259.
        // -------------------------------------------------------------
        bool envTruthy(const char *name)
        {
            const char *e = std::getenv(name);
            return e != nullptr && e[0] != 0 && e[0] != '0';
        }

        bool parseHostEnabled()
        {
            // PS2X_FMV=host is the documented switch; PS2X_FMVHOST=1 is accepted
            // as a shorthand.
            const char *m = std::getenv("PS2X_FMV");
            if (m != nullptr)
            {
                std::string v(m);
                for (char &c : v)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (v == "host")
                {
                    return true;
                }
            }
            return envTruthy("PS2X_FMVHOST");
        }

        double envDouble(const char *name, double fallback)
        {
            const char *e = std::getenv(name);
            if (e == nullptr || e[0] == 0)
            {
                return fallback;
            }
            try
            {
                return std::stod(e);
            }
            catch (...)
            {
                return fallback;
            }
        }

        unsigned envUnsigned(const char *name, unsigned fallback)
        {
            const char *e = std::getenv(name);
            if (e == nullptr || e[0] == 0)
            {
                return fallback;
            }
            try
            {
                return static_cast<unsigned>(std::stoul(e));
            }
            catch (...)
            {
                return fallback;
            }
        }

        // -------------------------------------------------------------
        // Shared state
        // -------------------------------------------------------------
        std::atomic<int> s_state{static_cast<int>(State::Idle)};
        std::atomic<bool> s_abort{false};
        std::atomic<int> s_activeMovie{-1};
        bool s_completed[kMovieCount] = {false, false};

        std::thread s_player;
        std::mutex s_frameMutex;
        std::vector<uint8_t> s_frameBuf;
        int s_frameW = 0;
        int s_frameH = 0;
        bool s_frameFresh = false;

        std::chrono::steady_clock::time_point s_requestedAt;
        double s_watchdogCap = 0.0;
        std::atomic<bool> s_firstTickLogged{false};

        // Reset per movie by resetFrameCounters(); read once on the Finished edge.
        std::atomic<uint64_t> s_published{0};
        std::atomic<uint64_t> s_presented{0};

        void setState(State s) { s_state.store(static_cast<int>(s), std::memory_order_release); }

        // -------------------------------------------------------------
        // Path resolution
        // -------------------------------------------------------------
        std::filesystem::path findCaseInsensitive(const std::filesystem::path &dir,
                                                  const std::string &leaf)
        {
            std::error_code ec;
            if (std::filesystem::exists(dir / leaf, ec))
            {
                return dir / leaf;
            }
            if (!std::filesystem::is_directory(dir, ec))
            {
                return {};
            }
            std::string want = leaf;
            for (char &c : want)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::string have = entry.path().filename().string();
                for (char &c : have)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (have == want)
                {
                    return entry.path();
                }
            }
            return {};
        }

        std::filesystem::path resolveMoviePath(const MovieDef &def)
        {
            const char *envPath = std::getenv(def.envOverride);
            if (envPath != nullptr && envPath[0] != 0)
            {
                return std::filesystem::path(envPath);
            }

            // getIoPaths().cdRoot is set from PS2_CD_ROOT, which
            // launch_recomp.ps1:168 fills from -CdRoot.
            const std::filesystem::path root = PS2Runtime::getIoPaths().cdRoot;
            if (root.empty())
            {
                return {};
            }
            std::filesystem::path movieDir = findCaseInsensitive(root, "MOVIE");
            if (movieDir.empty())
            {
                movieDir = root / "MOVIE";
            }
            return findCaseInsensitive(movieDir, def.file);
        }

        // -------------------------------------------------------------
        // Player thread
        // -------------------------------------------------------------
        void playerMain(int movie)
        {
            const MovieDef &def = kMovies[movie];
            const std::filesystem::path path = resolveMoviePath(def);

            if (path.empty())
            {
                std::cerr << "[fmvhost] " << def.label
                          << ": could not resolve MOVIE/" << def.file
                          << " under cdRoot='" << PS2Runtime::getIoPaths().cdRoot.string()
                          << "' -- degrading to skip" << std::endl;
                setState(State::Failed);
                return;
            }

            std::error_code ec;
            const uintmax_t size = std::filesystem::file_size(path, ec);

            Source src;
            std::string err;
            if (!src.open(path.string(), err))
            {
                std::cerr << "[fmvhost] " << def.label << ": open failed: " << err
                          << " (" << path.string() << ") -- degrading to skip" << std::endl;
                setState(State::Failed);
                return;
            }

            const StreamInfo &info = src.info();
            std::cerr << "[fmvhost] " << def.label << " " << info.width << "x" << info.height
                      << " " << info.videoCodec
                      << " " << info.fps << "fps " << info.durationSec << "s"
                      << (info.hasAudio
                              ? (" audio=" + info.audioCodec + " " +
                                 std::to_string(info.audioRate) + "x" +
                                 std::to_string(info.audioChannels))
                              : std::string(" audio=none"))
                      << " size=" << (ec ? 0u : size)
                      << " -> " << path.string() << std::endl;

            s_watchdogCap = envDouble("PS2X_FMV_TIMEOUT",
                                      (info.durationSec > 0.0 ? info.durationSec : 60.0) + 5.0);

            const bool wantAudio = info.hasAudio && !audioDisabled();
            const bool haveAudio = wantAudio && audioStart(info.audioRate, info.audioChannels, volume());
            const double latency = haveAudio
                                       ? (2.0 * static_cast<double>(audioChunkFrames()) /
                                          static_cast<double>(info.audioRate))
                                       : 0.0;

            std::deque<VideoFrame> vq;
            std::vector<int16_t> pcm;
            size_t pcmConsumed = 0; // in FRAMES, not samples
            const size_t kQueueCap = 8;
            const int channels = haveAudio ? info.audioChannels : 2;

            resetFrameCounters();

            // Prebuffer so the first frames and the first audio chunks are ready
            // before the clock starts.
            for (int i = 0; i < 64 && vq.size() < 4 && !src.eof(); ++i)
            {
                src.pump(vq, pcm, kQueueCap);
            }

            setState(State::Playing);
            const auto t0 = std::chrono::steady_clock::now();

            for (;;)
            {
                if (s_abort.load(std::memory_order_acquire))
                {
                    break;
                }

                if (vq.size() < kQueueCap && !src.eof())
                {
                    src.pump(vq, pcm, kQueueCap);
                }

                if (haveAudio)
                {
                    const size_t available = (pcm.size() / static_cast<size_t>(channels)) - pcmConsumed;
                    if (available > 0)
                    {
                        const size_t took = audioSubmit(
                            pcm.data() + pcmConsumed * static_cast<size_t>(channels), available);
                        pcmConsumed += took;
                    }
                    // Compact rather than erase-from-front every iteration.
                    if (pcmConsumed > 48000u)
                    {
                        pcm.erase(pcm.begin(),
                                  pcm.begin() + static_cast<ptrdiff_t>(pcmConsumed *
                                                                       static_cast<size_t>(channels)));
                        pcmConsumed = 0;
                    }
                }

                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() -
                    latency;

                bool published = false;
                while (!vq.empty() && (vq.front().pts < 0.0 || vq.front().pts <= elapsed))
                {
                    VideoFrame f = std::move(vq.front());
                    vq.pop_front();
                    publishFrame(f.rgba.data(), f.width, f.height);
                    published = true;
                }
                (void)published;

                if (src.eof() && vq.empty())
                {
                    // Let the audio tail drain rather than cutting the last ~85 ms.
                    const bool audioDrained =
                        !haveAudio ||
                        (pcm.size() / static_cast<size_t>(channels)) - pcmConsumed < audioChunkFrames();
                    if (audioDrained && elapsed >= info.durationSec)
                    {
                        break;
                    }
                }

                if (elapsed > s_watchdogCap)
                {
                    std::cerr << "[fmvhost] " << def.label << ": watchdog at " << elapsed
                              << "s (cap " << s_watchdogCap << "s) -- ending playback" << std::endl;
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (haveAudio)
            {
                audioStop();
            }
            src.close();

            if (!src.error().empty())
            {
                std::cerr << "[fmvhost] " << def.label << ": decode error: " << src.error()
                          << std::endl;
            }
            const double playedFor =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double sinceFirstTick =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - s_requestedAt)
                    .count();
            std::cerr << "[fmvhost] " << def.label << ": finished after " << playedFor
                      << "s (" << presentedFrames() << " frames presented, "
                      << publishedFrames() << " decoded)"
                      << "  movie=" << info.durationSec << "s"
                      << "  sinceFirstTick=" << sinceFirstTick << "s"
                      << (s_abort.load(std::memory_order_acquire) ? "  ABORTED" : "")
                      << std::endl;
            setState(State::Finished);
        }

        void joinPlayer()
        {
            if (s_player.joinable())
            {
                s_player.join();
            }
        }

        // -------------------------------------------------------------
        // Guest-side phase handlers
        // -------------------------------------------------------------

        // Byte-identical to game_overrides.cpp's skipFmvPhaseDone (:8260-8267) so
        // the host-player path and the already-proven skip path leave the guest in
        // exactly the same state.
        inline void phaseComplete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)runtime;
            const uint32_t obj = GPR_U32(ctx, 4); // $a0 = the CApp object
            WRITE32(obj + 48u, 0u);
            SET_GPR_U64(ctx, 2, 1u); // $v0 = 1 -> "phase complete"
            ctx->pc = GPR_U32(ctx, 31);
        }

        // Returning 0 WITHOUT touching [obj+48] parks the app: 0x420E70 is a
        // per-frame step machine driven from GameUpdate 0x421EA0 vtable slot 5
        // (game_overrides.cpp:7735-7746), and each arm advances only on a nonzero
        // return. The guest keeps its normal VSync-paced frame loop; it does not
        // hot-spin, and it does not starve the EeScheduler.
        inline void phaseWorking(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)rdram;
            (void)runtime;
            SET_GPR_U64(ctx, 2, 0u); // $v0 = 0 -> "still working"
            ctx->pc = GPR_U32(ctx, 31);
        }

        void openPhase(int movie, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const MovieDef &def = kMovies[movie];

            if (!s_firstTickLogged.exchange(true, std::memory_order_relaxed))
            {
                // A quiet run must be distinguishable from a hook that is never
                // DISPATCHED -- replaceFunction only intercepts dispatch-loop
                // calls, and a direct recompiled C++ call bypasses it entirely.
                std::cerr << "[fmvhost] first tick: " << def.label << " open 0x"
                          << std::hex << def.openFn << std::dec << std::endl;
            }

            if (s_completed[movie])
            {
                phaseComplete(rdram, ctx, runtime);
                return;
            }

            // Polling-contract probe. PS2X_FMV_PROBE=N returns "still working" N
            // times and then completes, with no decoding at all. This is the one
            // measurement the whole design rests on: if the guest does not
            // re-poll, the lifecycle is wrong and no decoder work is worth doing.
            const unsigned probe = probeTicks();
            if (probe != 0u)
            {
                static unsigned s_probeCount[kMovieCount] = {0u, 0u};
                if (s_probeCount[movie] < probe)
                {
                    ++s_probeCount[movie];
                    if (s_probeCount[movie] == probe)
                    {
                        std::cerr << "[fmvhost] probe: " << def.label << " re-polled "
                                  << probe << " times -- polling contract HOLDS"
                                  << std::endl;
                    }
                    phaseWorking(rdram, ctx, runtime);
                    return;
                }
                s_completed[movie] = true;
                phaseComplete(rdram, ctx, runtime);
                return;
            }

            const State st = state();

            if (st == State::Idle)
            {
                joinPlayer(); // a previous movie's thread, already Finished
                s_abort.store(false, std::memory_order_release);
                s_activeMovie.store(movie, std::memory_order_release);
                s_requestedAt = std::chrono::steady_clock::now();
                setState(State::Requested);
                s_player = std::thread(&playerMain, movie);
                phaseWorking(rdram, ctx, runtime);
                return;
            }

            if (st == State::Requested || st == State::Playing)
            {
                if (s_activeMovie.load(std::memory_order_acquire) != movie)
                {
                    // Two logo apps are ~7.6 s apart on hardware so this should be
                    // unreachable; hold rather than double-open if it is not.
                    phaseWorking(rdram, ctx, runtime);
                    return;
                }
                // Coarse outer watchdog in case the player thread wedges inside
                // avformat: the inner one lives on the player thread and cannot
                // fire if that thread is stuck.
                const double held =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - s_requestedAt)
                        .count();
                if (held > 120.0)
                {
                    std::cerr << "[fmvhost] outer watchdog: " << def.label
                              << " held " << held << "s -- forcing completion" << std::endl;
                    requestAbort("outer watchdog");
                }
                phaseWorking(rdram, ctx, runtime);
                return;
            }

            // Finished or Failed -> tear down and answer "complete".
            // This is the tick on which the guest finally accepts the answer, so
            // the delta against the completion line above is guest-side latency,
            // not playback time. Runs once per movie (s_completed gates it).
            std::cerr << "[fmvhost] " << def.label << ": retired after "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       s_requestedAt)
                             .count()
                      << "s since first tick" << std::endl;
            joinPlayer();
            audioStop();
            s_activeMovie.store(-1, std::memory_order_release);
            s_completed[movie] = true;
            setState(State::Idle);
            phaseComplete(rdram, ctx, runtime);
        }

        void openAtari(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            openPhase(kMovieAtari, rdram, ctx, runtime);
        }

        void openOkrtron(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            openPhase(kMovieOkrtron, rdram, ctx, runtime);
        }

        void closePhase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            phaseComplete(rdram, ctx, runtime);
        }

        bool isSdbz()
        {
            std::string name = PS2Runtime::getIoPaths().elfPath.filename().string();
            for (char &c : name)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return name.find("SLUS_214.42") != std::string::npos;
        }
    }

    // -----------------------------------------------------------------
    // Public-to-the-module accessors
    // -----------------------------------------------------------------
    State state() { return static_cast<State>(s_state.load(std::memory_order_acquire)); }
    bool aborted() { return s_abort.load(std::memory_order_acquire); }

    void requestAbort(const char *why)
    {
        if (!s_abort.exchange(true, std::memory_order_acq_rel))
        {
            std::cerr << "[fmvhost] abort requested: " << (why ? why : "?") << std::endl;
        }
    }

    void publishFrame(const uint8_t *rgba, int width, int height)
    {
        if (rgba == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        std::lock_guard<std::mutex> lock(s_frameMutex);
        s_frameBuf.resize(bytes);
        std::memcpy(s_frameBuf.data(), rgba, bytes);
        s_frameW = width;
        s_frameH = height;
        s_frameFresh = true;
        s_published.fetch_add(1u, std::memory_order_relaxed);
    }

    void notePresented() { s_presented.fetch_add(1u, std::memory_order_relaxed); }

    uint64_t publishedFrames() { return s_published.load(std::memory_order_relaxed); }

    uint64_t presentedFrames() { return s_presented.load(std::memory_order_relaxed); }

    void resetFrameCounters()
    {
        s_published.store(0u, std::memory_order_relaxed);
        s_presented.store(0u, std::memory_order_relaxed);
    }

    bool takeFrame(std::vector<uint8_t> &dst, int &width, int &height)
    {
        std::lock_guard<std::mutex> lock(s_frameMutex);
        if (!s_frameFresh)
        {
            return false;
        }
        dst = s_frameBuf;
        width = s_frameW;
        height = s_frameH;
        s_frameFresh = false;
        return true;
    }

    bool hostEnabled()
    {
        static const bool on = parseHostEnabled();
        return on;
    }

    bool audioDisabled()
    {
        static const bool off = envTruthy("PS2X_FMV_NOAUDIO");
        return off;
    }

    bool skipKeyDisabled()
    {
        static const bool off = envTruthy("PS2X_FMV_NOSKIP");
        return off;
    }

    float volume()
    {
        static const float v = static_cast<float>(envDouble("PS2X_FMV_VOL", 1.0));
        return v;
    }

    unsigned probeTicks()
    {
        static const unsigned n = envUnsigned("PS2X_FMV_PROBE", 0u);
        return n;
    }
}

// ---------------------------------------------------------------------
// The three entry points ps2_runtime.cpp calls. Declared there with extern "C"
// and no header, so this feature does not touch a .h and therefore does not
// force a ~30h rebuild of the ~30,000 runner TUs.
// ---------------------------------------------------------------------

extern "C" void ps2x_fmv_host_install(PS2Runtime *runtime)
{
    using namespace ps2x_fmv;

    if (!hostEnabled())
    {
        return; // fully inert; [skipfmv]'s stubs stay in charge
    }
    if (runtime == nullptr || !isSdbz())
    {
        std::cerr << "[fmvhost] requested but this is not SLUS_214.42 -- inert" << std::endl;
        return;
    }
    if (!decoderAvailable() && probeTicks() == 0u)
    {
        std::cerr << "[fmvhost] built without FFmpeg (PS2X_ENABLE_FFMPEG=OFF)"
                     " -- degrading to skip" << std::endl;
        return;
    }

    unsigned installed = 0u;
    for (int i = 0; i < kMovieCount; ++i)
    {
        const MovieDef &def = kMovies[i];
        PS2Runtime::RecompiledFunction openFn =
            (i == kMovieAtari) ? &openAtari : &openOkrtron;
        if (runtime->lookupFunction(def.openFn) != nullptr &&
            runtime->replaceFunction(def.openFn, openFn))
        {
            ++installed;
        }
        if (runtime->lookupFunction(def.closeFn) != nullptr &&
            runtime->replaceFunction(def.closeFn, &closePhase))
        {
            ++installed;
        }
    }

    // Loud on purpose, and it says it overrides [skipfmv] because that banner has
    // already printed by now (the skip defaults ON) and would otherwise mislead.
    // A partial install must not read as success --
    // see feedback_stubbed_hardware_has_no_error_path.
    std::cerr << "[fmvhost] ACTIVE -- host FFmpeg player owns"
                 " 0x420e70/0x420fc0/0x4216e0/0x421830, OVERRIDING [skipfmv]'s stubs."
                 " installed=" << installed << "/4"
              << (installed == 4u ? "" : "  <-- INCOMPLETE, expected 4")
              << (probeTicks() != 0u ? "  [PROBE MODE: no decoding]" : "")
              << std::endl;
}

extern "C" void ps2x_fmv_host_draw(void)
{
    if (!ps2x_fmv::hostEnabled())
    {
        return;
    }
    ps2x_fmv::presentDraw();
}

extern "C" void ps2x_fmv_host_shutdown(void)
{
    if (!ps2x_fmv::hostEnabled())
    {
        return;
    }
    // Ordering matters: run() does UnloadTexture/CloseWindow right after this
    // (ps2_runtime.cpp:6557-6558) and ~PS2Runtime does CloseAudioDevice (:1213).
    // A live player thread touching the stream after that is a use-after-free.
    // WindowShouldClose() (:6525) can break the present loop mid-movie, so this
    // must be safe from the Playing state, not only from Finished.
    using namespace ps2x_fmv;
    requestAbort("shutdown");
    joinPlayer();      // the decode thread must be gone before the stream/texture die
    audioStop();       // idempotent; the player thread normally does this itself
    presentRelease();  // main thread == present thread here, so the GL call is legal
}
