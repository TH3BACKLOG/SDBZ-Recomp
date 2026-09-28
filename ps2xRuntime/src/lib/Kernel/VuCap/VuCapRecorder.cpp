// VU1 capture recorder: the runtime side of build_scripts/vucap.py.
//
// Writes the .vucap v1 file that PCSX2's pcsx2/DebugTools/VuCapture.cpp writes
// (F:\PCSX2-src; the record layout is in that file's header comment), so
// vucap.py, vucap_memscan.py and ps2x_vucap_replay read either one. Opt-in:
//
//     PS2X_VUCAP=<path>             enable; output file
//     PS2X_VUCAP_AT=<s>[,<s>...]    start a capture this many seconds after the
//                                   first vsync (default 0). Several values give
//                                   several files, "_t<s>" added before the extension.
//     PS2X_VUCAP_FRAMES=<n>         vsyncs per capture (default 20)
//     PS2X_VUCAP_FULLMEM=0|1        VU1 data memory in every RUNSTART (default 1)
//
// How the file differs from a PCSX2 capture:
//   - one VIF record per processVIF1Data call (the whole buffer), not per command;
//   - one KICKSTART + one KICKDATA per XGKICK (the whole packet, can exceed 0x4000);
//   - MEMSYNC is written at a processVIF1Data call when VU1 code or data memory
//     changed since the previous call returned, i.e. the EE wrote it;
//   - SNAP flags are 0; VIF registers sit at PCSX2's 16-byte spacing (seedVifRegs);
//   - STATE has VI[16..31], the micro flag instances and takedelaybranch as 0,
//     and SNAP's VPU_STAT/FBRST as 0.
//
// Threads: the vsync worker writes VSYNC; everything else runs on the EE
// executor. One mutex guards the file. A capture begins and ends only at a
// processVIF1Data call, where no VU1 run is in progress. If the process exits
// mid-capture the file has no END record (vucap.py reports LOSS).

#include "Kernel/VuCap/VuCapRecorder.h"

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace vucap
{
    std::atomic<bool> g_hot{false};

    namespace
    {
        enum : uint8_t
        {
            REC_SNAP = 1,
            REC_VIF = 2,
            REC_RUNSTART = 3,
            REC_RUNEND = 4,
            REC_KICKSTART = 5,
            REC_KICKDATA = 6,
            REC_VSYNC = 7,
            REC_MEMSYNC = 8,
            REC_END = 9,
        };

        enum class State
        {
            Off,
            Idle,
            Armed,
            Capturing,
            Finishing,
        };

        constexpr uint32_t kFileVersion = 1u;
        constexpr uint32_t kVu1Size = 0x4000u;
        constexpr size_t kStateSize = 760u;
        constexpr uint32_t kPcsxVifRegsSize = 392u; // sizeof(PCSX2 VIFregisters)
        constexpr size_t kFlushAt = 4u << 20;

        std::mutex s_mutex;
        std::atomic<const VU1State *> s_stateSource{nullptr};

        // Guarded by s_mutex.
        bool s_configured = false;
        State s_state = State::Off;
        std::string s_basePath;
        std::vector<double> s_windows;
        size_t s_nextWindow = 0;
        uint32_t s_framesWanted = 20;
        bool s_fullMem = true;
        std::chrono::steady_clock::time_point s_t0;
        uint64_t s_lastTick = 0;

        std::FILE *s_fp = nullptr;
        std::string s_path;
        std::vector<uint8_t> s_buf;
        uint64_t s_bytes = 0;
        uint32_t s_frames = 0;
        uint32_t s_runs = 0;
        uint32_t s_kickChunks = 0;
        uint32_t s_vifRecords = 0;
        uint32_t s_memSyncs = 0;
        uint32_t s_runIdx = 0;
        bool s_inRun = false;
        bool s_stateSizeWarned = false;
        uint32_t s_microCrc = 0;
        uint32_t s_memCrc = 0;
        uint32_t s_crcTable[256];

        void crcInit()
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                s_crcTable[i] = c;
            }
        }

        // Same as zlib crc32(). A missing block hashes as 0.
        uint32_t crc32(const uint8_t *p, size_t n)
        {
            if (!p)
                return 0u;
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i)
                c = s_crcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        void put(const void *p, size_t n)
        {
            const uint8_t *b = static_cast<const uint8_t *>(p);
            s_buf.insert(s_buf.end(), b, b + n);
        }

        void putU32(uint32_t v) { put(&v, sizeof(v)); }
        void putU64(uint64_t v) { put(&v, sizeof(v)); }
        void putF32(float v) { put(&v, sizeof(v)); }
        void putZeros(size_t n) { s_buf.insert(s_buf.end(), n, uint8_t{0}); }

        void putVu1Block(const uint8_t *p)
        {
            if (p)
                put(p, kVu1Size);
            else
                putZeros(kVu1Size);
        }

        size_t beginRecord(uint8_t type)
        {
            const size_t at = s_buf.size();
            const uint8_t hdr[8] = {type, 0, 0, 0, 0, 0, 0, 0};
            put(hdr, sizeof(hdr));
            return at;
        }

        void endRecord(size_t at)
        {
            const uint32_t len = static_cast<uint32_t>(s_buf.size() - at - 8u);
            std::memcpy(&s_buf[at + 4u], &len, sizeof(len));
        }

        void flush()
        {
            if (!s_fp || s_buf.empty())
                return;
            const size_t wrote = std::fwrite(s_buf.data(), 1, s_buf.size(), s_fp);
            s_bytes += wrote;
            if (wrote != s_buf.size())
                std::cerr << "[vucap] short write (disk full?) path=" << s_path << std::endl;
            s_buf.clear();
        }

        void maybeFlush()
        {
            if (s_buf.size() >= kFlushAt)
                flush();
        }

        void putState(const VU1State *st, uint32_t vpuStat, uint32_t fbrst)
        {
            VU1State blank{};
            blank.vf[0][3] = 1.0f;
            if (!st)
                st = &blank;

            const size_t before = s_buf.size();
            for (int r = 0; r < 32; ++r)
                put(st->vf[r], sizeof(st->vf[r]));
            for (int i = 0; i < 32; ++i)
                putU32(i < 16 ? static_cast<uint32_t>(st->vi[i]) : 0u);
            put(st->acc, sizeof(st->acc));
            putF32(st->q);
            putF32(st->p);
            putU32(st->mac);
            putU32(st->status);
            putU32(st->clip);
            putZeros(12u * 4u); // micro mac / clip / status flag instances
            putU32(st->branchPending ? 1u : 0u);
            putU32(st->branchTarget);
            putU32(st->branchDelay);
            putU32(0u); // takedelaybranch
            putU32(st->ebit ? 1u : 0u);
            putU64(st->cycles);
            putU32(vpuStat);
            putU32(fbrst);

            if (s_buf.size() - before != kStateSize && !s_stateSizeWarned)
            {
                s_stateSizeWarned = true;
                std::cerr << "[vucap] STATE is " << (s_buf.size() - before) << " bytes, expected " << kStateSize
                          << " -- file will not parse" << std::endl;
            }
        }

        std::string windowPath(size_t k)
        {
            if (s_windows.size() <= 1u)
                return s_basePath;
            const size_t slash = s_basePath.find_last_of("/\\");
            size_t dot = s_basePath.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                dot = s_basePath.size();
            char tag[32];
            std::snprintf(tag, sizeof(tag), "_t%.0f", s_windows[k]);
            return s_basePath.substr(0, dot) + tag + s_basePath.substr(dot);
        }

        void configureLocked()
        {
            s_configured = true;
            const char *path = std::getenv("PS2X_VUCAP");
            if (!path || !*path)
                return;

            s_basePath = path;
            const char *at = std::getenv("PS2X_VUCAP_AT");
            const std::string list = (at && *at) ? at : "0";
            size_t i = 0;
            while (i <= list.size())
            {
                size_t comma = list.find(',', i);
                if (comma == std::string::npos)
                    comma = list.size();
                const std::string item = list.substr(i, comma - i);
                if (!item.empty())
                    s_windows.push_back(std::strtod(item.c_str(), nullptr));
                i = comma + 1u;
            }
            if (s_windows.empty())
                s_windows.push_back(0.0);
            std::sort(s_windows.begin(), s_windows.end());

            if (const char *frames = std::getenv("PS2X_VUCAP_FRAMES"); frames && *frames)
            {
                const unsigned long n = std::strtoul(frames, nullptr, 0);
                if (n > 0ul)
                    s_framesWanted = static_cast<uint32_t>(n);
            }
            if (const char *full = std::getenv("PS2X_VUCAP_FULLMEM"); full && *full)
                s_fullMem = std::strtoul(full, nullptr, 0) != 0ul;

            crcInit();
            s_t0 = std::chrono::steady_clock::now();
            s_state = State::Idle;
            std::cerr << "[vucap] ACTIVE path=" << s_basePath << " windows=" << s_windows.size()
                      << " first_at=" << s_windows.front() << "s frames=" << s_framesWanted
                      << " fullmem=" << (s_fullMem ? 1 : 0) << std::endl;
        }

        void beginLocked(const VIFRegisters &regs, const uint8_t *micro, const uint8_t *mem)
        {
            s_path = windowPath(s_nextWindow);
            s_fp = std::fopen(s_path.c_str(), "wb");
            if (!s_fp)
            {
                std::cerr << "[vucap] FAILED to open path=" << s_path << std::endl;
                ++s_nextWindow;
                s_state = State::Idle;
                g_hot.store(false, std::memory_order_relaxed);
                return;
            }

            s_buf.clear();
            s_buf.reserve(kFlushAt + (1u << 20));
            s_bytes = 0;
            s_frames = 0;
            s_runs = 0;
            s_kickChunks = 0;
            s_vifRecords = 0;
            s_memSyncs = 0;
            s_runIdx = 0;
            s_inRun = false;

            const char magic[8] = {'P', 'S', '2', 'V', 'U', 'C', 'A', 'P'};
            put(magic, sizeof(magic));
            putU32(kFileVersion);
            putU32(0u);

            const size_t at = beginRecord(REC_SNAP);
            putU32(kFileVersion);
            putU32(static_cast<uint32_t>(s_lastTick));
            putU32(0u);
            putState(s_stateSource.load(std::memory_order_relaxed), 0u, 0u);
            putU32(kPcsxVifRegsSize);
            const size_t regsAt = s_buf.size();
            putZeros(kPcsxVifRegsSize);
            const uint32_t v[23] = {regs.stat, regs.fbrst, regs.err, regs.mark, regs.cycle, regs.mode,
                                    regs.num, regs.mask, regs.code, regs.itops, regs.base, regs.ofst,
                                    regs.tops, regs.itop, regs.top, regs.row[0], regs.row[1], regs.row[2],
                                    regs.row[3], regs.col[0], regs.col[1], regs.col[2], regs.col[3]};
            for (size_t i = 0; i < 23u; ++i)
                std::memcpy(&s_buf[regsAt + i * 16u], &v[i], sizeof(v[i]));
            putVu1Block(micro);
            putVu1Block(mem);
            endRecord(at);

            s_microCrc = crc32(micro, kVu1Size);
            s_memCrc = crc32(mem, kVu1Size);
            s_state = State::Capturing;
            std::cerr << "[vucap] capturing window " << (s_nextWindow + 1u) << "/" << s_windows.size() << " at t="
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count()
                      << "s tick=" << s_lastTick << " frames=" << s_framesWanted << " -> " << s_path << std::endl;
        }

        void finishLocked()
        {
            const size_t at = beginRecord(REC_END);
            putU32(s_runs);
            putU32(s_kickChunks);
            putU32(s_vifRecords);
            putU32(s_memSyncs);
            putU32(s_frames);
            putU32(s_inRun ? 1u : 0u);
            endRecord(at);
            flush();
            std::fclose(s_fp);
            s_fp = nullptr;
            std::cerr << "[vucap] done: " << s_frames << " frames, " << s_runs << " runs, " << s_kickChunks
                      << " kicks, " << s_vifRecords << " vif, " << s_memSyncs << " memsync, " << s_bytes
                      << " bytes -> " << s_path << std::endl;
            s_state = State::Idle;
            ++s_nextWindow;
            g_hot.store(false, std::memory_order_relaxed);
        }
    }

    void setStateSource(const VU1State *state)
    {
        s_stateSource.store(state, std::memory_order_relaxed);
    }

    void onVSync(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_configured)
            configureLocked();
        if (s_state == State::Off)
            return;
        s_lastTick = tick;

        if (s_state == State::Capturing)
        {
            const size_t at = beginRecord(REC_VSYNC);
            putU32(static_cast<uint32_t>(tick));
            endRecord(at);
            if (++s_frames >= s_framesWanted)
                s_state = State::Finishing;
            maybeFlush();
            return;
        }

        if (s_state == State::Idle && s_nextWindow < s_windows.size())
        {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
            if (t >= s_windows[s_nextWindow])
            {
                s_state = State::Armed;
                g_hot.store(true, std::memory_order_relaxed);
                std::cerr << "[vucap] armed window " << (s_nextWindow + 1u) << " at t=" << t << "s tick=" << tick
                          << std::endl;
            }
        }
    }

    void vifCallBegin(const uint8_t *data, uint32_t sizeBytes, const uint8_t *micro, const uint8_t *mem,
                      const VIFRegisters &regs)
    {
        if (!hot())
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_state == State::Finishing)
            finishLocked();
        if (s_state == State::Armed)
            beginLocked(regs, micro, mem);
        if (s_state != State::Capturing)
            return;

        const uint32_t microCrc = crc32(micro, kVu1Size);
        const uint32_t memCrc = crc32(mem, kVu1Size);
        if (microCrc != s_microCrc || memCrc != s_memCrc)
        {
            const size_t sync = beginRecord(REC_MEMSYNC);
            putVu1Block(micro);
            putVu1Block(mem);
            endRecord(sync);
            ++s_memSyncs;
        }

        const uint32_t words = sizeBytes / 4u;
        const size_t at = beginRecord(REC_VIF);
        putU32(words);
        put(data, static_cast<size_t>(words) * 4u);
        endRecord(at);
        ++s_vifRecords;
        maybeFlush();
    }

    void vifCallEnd(const uint8_t *micro, const uint8_t *mem)
    {
        if (!hot())
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_state != State::Capturing)
            return;
        s_microCrc = crc32(micro, kVu1Size);
        s_memCrc = crc32(mem, kVu1Size);
    }

    void runStart(uint32_t tpc, uint32_t top, uint32_t itop, const uint8_t *micro, const uint8_t *mem,
                  const VU1State &state, uint32_t vpuStat, uint32_t fbrst)
    {
        if (!hot())
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_state != State::Capturing)
            return;

        ++s_runIdx;
        ++s_runs;
        s_inRun = true;
        const bool fullMem = s_fullMem && mem != nullptr;
        const size_t at = beginRecord(REC_RUNSTART);
        putU32(s_runIdx);
        putU32(tpc);
        putU32(top);
        putU32(itop);
        putU32(crc32(mem, kVu1Size));
        putU32(crc32(micro, kVu1Size));
        putU32(fullMem ? 1u : 0u);
        putState(&state, vpuStat, fbrst);
        if (fullMem)
            put(mem, kVu1Size);
        endRecord(at);
        maybeFlush();
    }

    void runEnd(const VU1State &state, bool forced, uint32_t vpuStat, uint32_t fbrst)
    {
        if (!hot())
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_state != State::Capturing || !s_inRun)
            return;

        const size_t at = beginRecord(REC_RUNEND);
        putU32(s_runIdx);
        putU32(forced ? 1u : 0u);
        putState(&state, vpuStat, fbrst);
        endRecord(at);
        s_inRun = false;
        maybeFlush();
    }

    void kick(uint32_t sourceAddress, const uint8_t *bytes, uint32_t sizeBytes)
    {
        if (!hot() || !bytes || sizeBytes == 0u)
            return;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_state != State::Capturing)
            return;

        size_t at = beginRecord(REC_KICKSTART);
        putU32(s_runIdx);
        putU32(sourceAddress / 16u);
        endRecord(at);

        at = beginRecord(REC_KICKDATA);
        putU32(s_runIdx);
        putU32(sourceAddress);
        putU32(sizeBytes);
        putU32(1u);
        put(bytes, sizeBytes);
        endRecord(at);
        ++s_kickChunks;
        maybeFlush();
    }
}
