// Offline VU1 capture replay harness.
//
// Why this exists
// ---------------
// 3D in the game is warped. Every vertex goes VIF1 UNPACK -> VU1 micro program ->
// XGKICK, and a wrong byte anywhere in that chain looks the same on screen. A
// modified PCSX2 (F:\PCSX2-src, pcsx2/DebugTools/VuCapture.cpp) records the VIF1
// words it consumed, VU1 state and data-memory CRC at every run start and end,
// and every XGKICK byte. This test feeds the same VIF1 words through our
// PS2Memory::processVIF1Data + VU1Interpreter and compares, run by run:
//
//   before a run : our data/micro memory CRC32 vs PCSX2's   -> S1 (VIF/UNPACK) if different
//                  our MSCAL pc / TOP / ITOP vs PCSX2's
//   after a run  : VF1..31 / VI1..15 vs PCSX2's              -> S2 (VU1 interpreter)
//                  every XGKICK packet, byte for byte        -> S2
//
//   1. PCSX2 (VU1 rec OFF, MTVU OFF): python build_scripts/pcsx2_ee.py --vucap cap.vucap --frames 10 --wait
//   2. python build_scripts/vucap.py cap.vucap        (loss check)
//   3. set PS2X_VUCAP=cap.vucap ; ps2x_tests.exe VU1CaptureReplay
//
// Environment:
//   PS2X_VUCAP               capture path (unset = skipped)
//   PS2X_VUCAP_OUT           report path (default: <capture>.replay.txt)
//   PS2X_VUCAP_MODE          "resync" (default): before every run, load PCSX2's VU
//                            state (and data memory when the capture has it), so
//                            each run is judged on its own. "free": seed once from
//                            the snapshot and let errors carry forward.
//   PS2X_VUCAP_MAXRUNS       stop after this many runs (0 = all)
//   PS2X_VUCAP_DETAIL        detail lines per category (default 20)
//   PS2X_VUCAP_CORRUPT_VIF   negative control: flip a bit in the last word of VIF record N
//   PS2X_VUCAP_CORRUPT_MICRO negative control: flip a bit in snapshot micro memory at byte N

#include "MiniTest.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kVu1Size = 0x4000u;
    constexpr uint32_t kStateSize = 760u;

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

    constexpr uint8_t kVifT[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

    std::string envOr(const char *name, const char *fallback)
    {
        const char *v = std::getenv(name);
        return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
    }

    long long envInt(const char *name, long long fallback)
    {
        const char *v = std::getenv(name);
        return (v != nullptr && v[0] != '\0') ? std::strtoll(v, nullptr, 0) : fallback;
    }

    uint32_t crc32(const uint8_t *p, size_t n)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            ready = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
            c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // PCSX2 VURegs as written by VuCapture::PutState.
    struct PcsxState
    {
        float vf[32][4];
        uint32_t vi[32];
        float acc[4];
        uint32_t q, p, mac, status, clip;
        uint32_t microMac[4], microClip[4], microStatus[4];
        uint32_t branch, branchpc, delaybranchpc, takedelaybranch, ebit;
        uint64_t cycle;
        uint32_t vpuStat, fbrst;
    };

    PcsxState parseState(const uint8_t *p)
    {
        PcsxState s{};
        size_t o = 0;
        auto take = [&](void *dst, size_t n)
        {
            std::memcpy(dst, p + o, n);
            o += n;
        };
        take(s.vf, sizeof(s.vf));
        take(s.vi, sizeof(s.vi));
        take(s.acc, sizeof(s.acc));
        take(&s.q, 4);
        take(&s.p, 4);
        take(&s.mac, 4);
        take(&s.status, 4);
        take(&s.clip, 4);
        take(s.microMac, sizeof(s.microMac));
        take(s.microClip, sizeof(s.microClip));
        take(s.microStatus, sizeof(s.microStatus));
        take(&s.branch, 4);
        take(&s.branchpc, 4);
        take(&s.delaybranchpc, 4);
        take(&s.takedelaybranch, 4);
        take(&s.ebit, 4);
        take(&s.cycle, 8);
        take(&s.vpuStat, 4);
        take(&s.fbrst, 4);
        return s;
    }

    void seedVuState(VU1State &dst, const PcsxState &s)
    {
        std::memcpy(dst.vf, s.vf, sizeof(dst.vf));
        dst.vf[0][0] = 0.0f;
        dst.vf[0][1] = 0.0f;
        dst.vf[0][2] = 0.0f;
        dst.vf[0][3] = 1.0f;
        for (int i = 0; i < 16; ++i)
            dst.vi[i] = static_cast<int16_t>(s.vi[i] & 0xFFFFu);
        dst.vi[0] = 0;
        std::memcpy(dst.acc, s.acc, sizeof(dst.acc));
        std::memcpy(&dst.q, &s.q, 4);
        std::memcpy(&dst.i, &s.vi[21], 4); // REG_I
        dst.r = s.vi[20];                  // REG_R
        dst.mac = s.mac;
        dst.status = s.status;
        dst.clip = s.clip;
    }

    // PCSX2 VIFregisters: one u32 every 16 bytes.
    void seedVifRegs(VIFRegisters &r, const uint8_t *raw, uint32_t size)
    {
        auto at = [&](uint32_t idx) -> uint32_t
        {
            uint32_t v = 0;
            if ((idx + 1u) * 16u <= size + 12u)
                std::memcpy(&v, raw + idx * 16u, 4);
            return v;
        };
        r.stat = at(0);
        r.fbrst = at(1);
        r.err = at(2);
        r.mark = at(3);
        r.cycle = at(4);
        r.mode = at(5);
        r.num = at(6);
        r.mask = at(7);
        r.code = at(8);
        r.itops = at(9);
        r.base = at(10);
        r.ofst = at(11);
        r.tops = at(12);
        r.itop = at(13);
        r.top = at(14);
        for (uint32_t i = 0; i < 4; ++i)
        {
            r.row[i] = at(16 + i);
            r.col[i] = at(20 + i);
        }
    }

    // Full VIF command length in words (PCSX2's rules, so a split record is
    // reassembled exactly as PCSX2 consumed it -- not by our interpreter's rules).
    uint32_t vifCommandWords(uint32_t cmd, uint32_t cycle)
    {
        const uint32_t op = (cmd >> 24) & 0x7Fu;
        const uint32_t num = (cmd >> 16) & 0xFFu;
        const uint32_t imm = cmd & 0xFFFFu;
        if (op == 0x20u)
            return 2u;
        if (op == 0x30u || op == 0x31u)
            return 5u;
        if (op == 0x4Au)
            return 1u + (num ? num : 256u) * 2u;
        if (op == 0x50u || op == 0x51u)
            return 1u + (imm ? imm : 65536u) * 4u;
        if ((op & 0x60u) == 0x60u)
        {
            const uint32_t vnum = num ? num : 256u;
            const uint32_t gsize = kVifT[op & 0xFu];
            const uint32_t cl = cycle & 0xFFu;
            const uint32_t wlRaw = (cycle >> 8) & 0xFFu;
            const uint32_t wl = wlRaw ? wlRaw : 256u;
            if (wl <= cl)
                return 1u + (vnum * gsize + 3u) / 4u;
            const uint32_t n = cl * (vnum / wl) + std::min(vnum % wl, cl);
            return 1u + ((n * gsize + 3u) >> 2);
        }
        return 1u;
    }

    struct PendingRun
    {
        bool cont = false;
        uint32_t startPC = 0;
        uint32_t top = 0;
        uint32_t itop = 0;
    };

    struct Report
    {
        std::FILE *fp = nullptr;
        void line(const char *fmt, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            if (fp)
                std::fprintf(fp, "%s\n", buf);
        }
        void both(const char *fmt, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            std::printf("[vucap] %s\n", buf);
            if (fp)
                std::fprintf(fp, "%s\n", buf);
        }
    };

    struct Category
    {
        const char *name;
        uint64_t count = 0;
        uint32_t firstRun = 0;
    };

    struct Replay
    {
        PS2Memory mem;
        GS gs;
        VU1Interpreter vu1;
        Report rep;

        bool resync = true;
        uint32_t maxRuns = 0;
        uint32_t detail = 20;
        long long corruptVif = -1;
        long long corruptMicro = -1;

        // VIF reassembly.
        std::vector<uint8_t> vifPending;
        uint32_t vifOwed = 0;
        uint32_t vifCycle = 0;
        uint64_t vifRecordIndex = 0;

        std::deque<PendingRun> pendingRuns;
        bool inRun = false;
        std::vector<std::vector<uint8_t>> ourKicks;
        std::vector<std::vector<uint8_t>> pcsxKicks;
        uint32_t kickRun = 0;
        uint32_t curRun = 0;
        uint32_t runsDone = 0;
        bool haveKickRun = false;

        Category unmatched{"run with no MSCAL/MSCNT from our VIF1 (S1)"};
        Category extraKicks{"our VIF1 produced extra MSCAL/MSCNT (S1)"};
        Category kickArgs{"MSCAL pc / TOP / ITOP differ (S1)"};
        Category contPc{"MSCNT resume pc differs (info)"};
        Category memCrc{"data memory differs before run (S1)"};
        Category microCrc{"micro memory differs before run (S1: MPG)"};
        Category vfFar{"VF differs after run (S2)"};
        Category vfNear{"VF within 1e-3 only (info)"};
        Category viDiff{"VI differs after run (S2)"};
        Category kickCount{"XGKICK packet count differs (S2)"};
        Category kickBytes{"XGKICK bytes differ (S2)"};

        std::vector<Category *> categories() { return {&unmatched, &extraKicks, &kickArgs, &memCrc, &microCrc, &vfFar, &viDiff, &kickCount, &kickBytes, &contPc, &vfNear}; }

        bool note(Category &c, uint32_t run)
        {
            if (c.count++ == 0)
                c.firstRun = run;
            return c.count <= detail;
        }

        bool init()
        {
            if (!mem.initialize())
                return false;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            mem.setGifPacketCallback([this](const uint8_t *data, uint32_t size)
                                     {
                                         if (inRun)
                                             ourKicks.emplace_back(data, data + size);
                                     });
            mem.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                    { pendingRuns.push_back({false, startPC, top, itop}); });
            mem.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                    { pendingRuns.push_back({true, 0u, top, itop}); });
            return true;
        }

        void feedCommand(const uint8_t *bytes, uint32_t size) { mem.processVIF1Data(bytes, size); }

        void feedVif(std::vector<uint8_t> &payload)
        {
            uint32_t n = 0;
            std::memcpy(&n, payload.data(), 4);
            if (payload.size() < 4u + n * 4u || n == 0)
                return;
            uint32_t *words = reinterpret_cast<uint32_t *>(payload.data() + 4);
            if (corruptVif >= 0 && static_cast<long long>(vifRecordIndex) == corruptVif)
            {
                words[n - 1] ^= 0x00010000u;
                rep.both("negative control: flipped bit 16 of the last word of VIF record %llu", static_cast<unsigned long long>(vifRecordIndex));
            }
            ++vifRecordIndex;

            uint32_t i = 0;
            if (vifOwed)
            {
                const uint32_t take = std::min(vifOwed, n);
                vifPending.insert(vifPending.end(), payload.data() + 4, payload.data() + 4 + take * 4u);
                vifOwed -= take;
                i = take;
                if (vifOwed == 0)
                {
                    feedCommand(vifPending.data(), static_cast<uint32_t>(vifPending.size()));
                    vifPending.clear();
                }
            }
            while (i < n)
            {
                const uint32_t cmd = words[i];
                if (((cmd >> 24) & 0x7Fu) == 0x01u)
                    vifCycle = cmd & 0xFFFFu;
                const uint32_t len = vifCommandWords(cmd, vifCycle);
                if (i + len <= n)
                {
                    feedCommand(payload.data() + 4 + i * 4u, len * 4u);
                    i += len;
                }
                else
                {
                    vifPending.assign(payload.data() + 4 + i * 4u, payload.data() + 4 + n * 4u);
                    vifOwed = i + len - n;
                    i = n;
                }
            }
        }

        static std::string hexQword(const uint8_t *p)
        {
            char buf[64];
            uint32_t lo = 0, hi = 0;
            std::memcpy(&lo, p, 4);
            std::memcpy(&hi, p + 4, 4);
            std::snprintf(buf, sizeof(buf), "%08x_%08x", hi, lo);
            return buf;
        }

        void compareKicks()
        {
            if (!haveKickRun)
                return;
            const uint32_t run = kickRun;
            if (ourKicks.size() != pcsxKicks.size())
            {
                if (note(kickCount, run))
                    rep.line("run %u: XGKICK packets ours=%zu pcsx2=%zu", run, ourKicks.size(), pcsxKicks.size());
            }
            const size_t pairs = std::min(ourKicks.size(), pcsxKicks.size());
            bool reported = false;
            for (size_t k = 0; k < pairs && !reported; ++k)
            {
                const auto &a = ourKicks[k];
                const auto &b = pcsxKicks[k];
                const size_t common = std::min(a.size(), b.size()) & ~size_t(15);
                size_t diffAt = SIZE_MAX;
                for (size_t o = 0; o < common; o += 16)
                {
                    if (std::memcmp(a.data() + o, b.data() + o, 16) != 0)
                    {
                        diffAt = o;
                        break;
                    }
                }
                if (diffAt == SIZE_MAX && a.size() == b.size())
                    continue;
                reported = true;
                if (note(kickBytes, run))
                {
                    size_t diffs = 0;
                    for (size_t o = 0; o < common; o += 16)
                        diffs += std::memcmp(a.data() + o, b.data() + o, 16) != 0;
                    rep.line("run %u: XGKICK packet %zu size ours=%zu pcsx2=%zu, %zu/%zu qwords differ",
                             run, k, a.size(), b.size(), diffs, common / 16);
                    if (diffAt != SIZE_MAX)
                    {
                        rep.line("    first diff at qword %zu: ours=%s pcsx2=%s", diffAt / 16,
                                 hexQword(a.data() + diffAt).c_str(), hexQword(b.data() + diffAt).c_str());
                        int shown = 0;
                        for (size_t o = diffAt + 16; o < common && shown < 3; o += 16)
                        {
                            if (std::memcmp(a.data() + o, b.data() + o, 16) != 0)
                            {
                                rep.line("    qword %zu: ours=%s pcsx2=%s", o / 16,
                                         hexQword(a.data() + o).c_str(), hexQword(b.data() + o).c_str());
                                ++shown;
                            }
                        }
                    }
                }
            }
            ourKicks.clear();
            pcsxKicks.clear();
            haveKickRun = false;
        }

        void diffMemory(uint32_t run, const uint8_t *theirs)
        {
            const uint8_t *ours = mem.getVU1Data();
            uint32_t diffs = 0;
            uint32_t lo = UINT32_MAX, hi = 0;
            for (uint32_t o = 0; o < kVu1Size; o += 16)
            {
                if (std::memcmp(ours + o, theirs + o, 16) != 0)
                {
                    ++diffs;
                    lo = std::min(lo, o / 16);
                    hi = std::max(hi, o / 16);
                }
            }
            rep.line("    %u/1024 data qwords differ, range qword %u..%u", diffs, lo, hi);
            int shown = 0;
            for (uint32_t o = 0; o < kVu1Size && shown < 6; o += 16)
            {
                if (std::memcmp(ours + o, theirs + o, 16) != 0)
                {
                    float fa[4], fb[4];
                    std::memcpy(fa, ours + o, 16);
                    std::memcpy(fb, theirs + o, 16);
                    rep.line("    qword %3u: ours=(%g %g %g %g) pcsx2=(%g %g %g %g)", o / 16,
                             fa[0], fa[1], fa[2], fa[3], fb[0], fb[1], fb[2], fb[3]);
                    ++shown;
                }
            }
        }

        void onRunStart(const std::vector<uint8_t> &p)
        {
            compareKicks();
            uint32_t f[7];
            std::memcpy(f, p.data(), sizeof(f));
            const uint32_t run = f[0], tpc = f[1], top = f[2], itop = f[3], memCrcV = f[4], microCrcV = f[5], hasMem = f[6];
            const PcsxState st = parseState(p.data() + 28);
            const uint8_t *fullMem = (hasMem && p.size() >= 28u + kStateSize + kVu1Size) ? p.data() + 28 + kStateSize : nullptr;
            curRun = run;

            PendingRun pr;
            bool matched = true;
            if (pendingRuns.empty())
            {
                matched = false;
                if (note(unmatched, run))
                    rep.line("run %u: PCSX2 ran tpc=0x%03x but our VIF1 queued no MSCAL/MSCNT", run, tpc);
                pr.startPC = tpc * 8u;
                pr.top = top;
                pr.itop = itop;
            }
            else
            {
                pr = pendingRuns.front();
                pendingRuns.pop_front();
                if (!pendingRuns.empty())
                {
                    if (note(extraKicks, run))
                        rep.line("run %u: %zu more MSCAL/MSCNT queued by our VIF1 than PCSX2 ran", run, pendingRuns.size());
                    pendingRuns.clear();
                }
            }

            if (matched)
            {
                const bool pcBad = !pr.cont && (pr.startPC / 8u) != (tpc & 0x7FFu);
                if (pcBad || (pr.top & 0x3FFu) != (top & 0x3FFu) || (pr.itop & 0x3FFu) != (itop & 0x3FFu))
                {
                    if (note(kickArgs, run))
                        rep.line("run %u: %s ours pc=0x%03x top=0x%x itop=0x%x | pcsx2 tpc=0x%03x top=0x%x itop=0x%x",
                                 run, pr.cont ? "MSCNT" : "MSCAL", pr.startPC / 8u, pr.top, pr.itop, tpc, top, itop);
                }
                if (pr.cont && (vu1.state().pc / 8u) != (tpc & 0x7FFu))
                {
                    if (note(contPc, run))
                        rep.line("run %u: MSCNT resumes ours pc=0x%03x pcsx2 tpc=0x%03x", run, vu1.state().pc / 8u, tpc);
                }
            }

            // Negative control. Applied before every run, because the game
            // re-uploads its micro programs by MPG each frame: a one-time flip in
            // the snapshot is overwritten before the first run and proves nothing.
            if (corruptMicro >= 0 && corruptMicro < static_cast<long long>(kVu1Size))
            {
                mem.getVU1Code()[corruptMicro] ^= 0x01u;
                mem.write8(PS2_VU1_CODE_BASE + static_cast<uint32_t>(corruptMicro), mem.getVU1Code()[corruptMicro]);
            }

            const uint32_t ourMicro = crc32(mem.getVU1Code(), kVu1Size);
            if (ourMicro != microCrcV)
            {
                if (note(microCrc, run))
                    rep.line("run %u: micro crc ours=%08x pcsx2=%08x", run, ourMicro, microCrcV);
            }
            const uint32_t ourMem = crc32(mem.getVU1Data(), kVu1Size);
            if (ourMem != memCrcV)
            {
                if (note(memCrc, run))
                {
                    rep.line("run %u (tpc=0x%03x): data crc ours=%08x pcsx2=%08x%s", run, tpc, ourMem, memCrcV,
                             fullMem ? "" : " (no full memory in capture: re-capture with --fullmem)");
                    if (fullMem)
                        diffMemory(run, fullMem);
                }
            }

            if (resync)
            {
                if (fullMem)
                    std::memcpy(mem.getVU1Data(), fullMem, kVu1Size);
                seedVuState(vu1.state(), st);
                if (pr.cont)
                    vu1.state().pc = (tpc & 0x7FFu) * 8u;
            }

            vu1.state().dBitEnabled = (st.fbrst & (1u << 10)) != 0u;
            vu1.state().tBitEnabled = (st.fbrst & (1u << 11)) != 0u;

            inRun = true;
            if (pr.cont)
                vu1.resume(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE,
                           gs, &mem, top & 0x3FFu, itop & 0x3FFu, 65536u);
            else
                vu1.execute(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE,
                            gs, &mem, (tpc & 0x7FFu) * 8u, top & 0x3FFu, itop & 0x3FFu, 65536u);
            inRun = false;
            kickRun = run;
            haveKickRun = true;
        }

        void onRunEnd(const std::vector<uint8_t> &p)
        {
            uint32_t run = 0, reason = 0;
            std::memcpy(&run, p.data(), 4);
            std::memcpy(&reason, p.data() + 4, 4);
            const PcsxState st = parseState(p.data() + 8);
            const VU1State &ours = vu1.state();

            int far = 0, near = 0, firstReg = -1, firstLane = -1;
            for (int r = 1; r < 32; ++r)
            {
                for (int l = 0; l < 4; ++l)
                {
                    const float a = ours.vf[r][l];
                    const float b = st.vf[r][l];
                    if (std::memcmp(&a, &b, 4) == 0)
                        continue;
                    const bool bothNan = std::isnan(a) && std::isnan(b);
                    if (bothNan)
                        continue;
                    const double tol = 1e-3 * std::max(1.0, std::fabs(static_cast<double>(b)));
                    if (std::isfinite(a) && std::isfinite(b) && std::fabs(static_cast<double>(a) - b) <= tol)
                        ++near;
                    else
                    {
                        if (far++ == 0)
                        {
                            firstReg = r;
                            firstLane = l;
                        }
                    }
                }
            }
            if (far)
            {
                if (note(vfFar, run))
                {
                    rep.line("run %u: %d VF lanes differ (end reason %u), first vf%d.%c", run, far, reason, firstReg, "xyzw"[firstLane]);
                    int shown = 0;
                    for (int r = 1; r < 32 && shown < 4; ++r)
                    {
                        if (std::memcmp(ours.vf[r], st.vf[r], 16) != 0)
                        {
                            rep.line("    vf%-2d ours=(%g %g %g %g) pcsx2=(%g %g %g %g)", r,
                                     ours.vf[r][0], ours.vf[r][1], ours.vf[r][2], ours.vf[r][3],
                                     st.vf[r][0], st.vf[r][1], st.vf[r][2], st.vf[r][3]);
                            ++shown;
                        }
                    }
                }
            }
            else if (near)
            {
                note(vfNear, run);
            }

            int viBad = 0;
            std::string viText;
            for (int r = 1; r < 16; ++r)
            {
                const uint16_t a = static_cast<uint16_t>(ours.vi[r]);
                const uint16_t b = static_cast<uint16_t>(st.vi[r]);
                if (a != b)
                {
                    ++viBad;
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), " vi%d ours=%u pcsx2=%u", r, a, b);
                    viText += buf;
                }
            }
            if (viBad && note(viDiff, run))
                rep.line("run %u:%s", run, viText.c_str());

            ++runsDone;
        }

        void onKickStart(const std::vector<uint8_t> &p)
        {
            uint32_t run = 0;
            std::memcpy(&run, p.data(), 4);
            if (haveKickRun && run == kickRun)
                pcsxKicks.emplace_back();
        }

        void onKickData(const std::vector<uint8_t> &p)
        {
            uint32_t f[4];
            std::memcpy(f, p.data(), sizeof(f));
            if (!haveKickRun || f[0] != kickRun || p.size() < 16u + f[2])
                return;
            if (pcsxKicks.empty())
                pcsxKicks.emplace_back();
            pcsxKicks.back().insert(pcsxKicks.back().end(), p.data() + 16, p.data() + 16 + f[2]);
        }
    };

    bool readRecord(std::FILE *f, uint8_t &type, std::vector<uint8_t> &payload, bool &torn)
    {
        uint8_t hdr[8];
        const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
        if (got == 0)
            return false;
        if (got != sizeof(hdr))
        {
            torn = true;
            return false;
        }
        type = hdr[0];
        uint32_t len = 0;
        std::memcpy(&len, hdr + 4, 4);
        payload.resize(len);
        if (len && std::fread(payload.data(), 1, len, f) != len)
        {
            torn = true;
            return false;
        }
        return true;
    }
}

void register_ps2_vu1_capture_replay_tests()
{
    MiniTest::Case("VU1CaptureReplay", [](TestCase &tc)
    {
        tc.Run("replay a PCSX2 VU1 capture through our VIF1 + VU1", [](TestCase &t)
        {
            const std::string path = envOr("PS2X_VUCAP", "");
            if (path.empty())
            {
                std::printf("[vucap] skipped: set PS2X_VUCAP to a .vucap from build_scripts/pcsx2_ee.py --vucap\n");
                return;
            }
            std::FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
            {
                t.Fail("cannot open " + path);
                return;
            }
            char header[16]{};
            if (std::fread(header, 1, sizeof(header), f) != sizeof(header) || std::memcmp(header, "PS2VUCAP", 8) != 0)
            {
                std::fclose(f);
                t.Fail("bad .vucap magic: " + path);
                return;
            }

            auto replay = std::make_unique<Replay>();
            Replay &rp = *replay;
            t.IsTrue(rp.init(), "PS2Memory should initialize");

            const std::string outPath = envOr("PS2X_VUCAP_OUT", (path + ".replay.txt").c_str());
            rp.rep.fp = std::fopen(outPath.c_str(), "w");
            rp.resync = envOr("PS2X_VUCAP_MODE", "resync") != "free";
            rp.maxRuns = static_cast<uint32_t>(envInt("PS2X_VUCAP_MAXRUNS", 0));
            rp.detail = static_cast<uint32_t>(envInt("PS2X_VUCAP_DETAIL", 20));
            rp.corruptVif = envInt("PS2X_VUCAP_CORRUPT_VIF", -1);
            rp.corruptMicro = envInt("PS2X_VUCAP_CORRUPT_MICRO", -1);
            rp.rep.both("capture %s, mode %s, report %s", path.c_str(), rp.resync ? "resync" : "free", outPath.c_str());

            uint8_t type = 0;
            std::vector<uint8_t> payload;
            bool torn = false;
            bool sawSnap = false, sawEnd = false;
            uint64_t vsyncs = 0;
            uint64_t lateKicks = 0;

            // Records read ahead of time, replayed before reading the file again.
            std::deque<std::pair<uint8_t, std::vector<uint8_t>>> lookahead;
            auto nextRecord = [&](uint8_t &ty, std::vector<uint8_t> &pl) -> bool
            {
                if (!lookahead.empty())
                {
                    ty = lookahead.front().first;
                    pl = std::move(lookahead.front().second);
                    lookahead.pop_front();
                    return true;
                }
                return readRecord(f, ty, pl, torn);
            };

            while (nextRecord(type, payload))
            {
                // PCSX2 logs a VIF command's words after its handler returns, but
                // MSCAL/MSCNT followed by UNPACK (and MSCALF) run the program inside
                // the handler. So the kick's VIF record lands after the run's own
                // records. Pull the next VIF record forward so our kick is queued
                // before the run it starts.
                if (sawSnap && type == REC_RUNSTART && rp.pendingRuns.empty())
                {
                    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> held;
                    uint8_t t2 = 0;
                    std::vector<uint8_t> p2;
                    bool found = false;
                    while (held.size() < 200000u && nextRecord(t2, p2))
                    {
                        if (t2 == REC_VIF)
                        {
                            rp.feedVif(p2);
                            found = true;
                            break;
                        }
                        if (t2 == REC_RUNSTART)
                        {
                            held.emplace_back(t2, std::move(p2));
                            break;
                        }
                        held.emplace_back(t2, std::move(p2));
                    }
                    if (found && !rp.pendingRuns.empty())
                        ++lateKicks;
                    for (auto it = held.rbegin(); it != held.rend(); ++it)
                        lookahead.emplace_front(std::move(*it));
                }

                if (!sawSnap)
                {
                    if (type != REC_SNAP || payload.size() < 16u + kStateSize)
                        break;
                    uint32_t regSize = 0;
                    std::memcpy(&regSize, payload.data() + 12 + kStateSize, 4);
                    const size_t regsAt = 16u + kStateSize;
                    if (payload.size() < regsAt + regSize + 2u * kVu1Size)
                        break;
                    const uint8_t *regs = payload.data() + regsAt;
                    std::memcpy(rp.mem.getVU1Code(), regs + regSize, kVu1Size);
                    std::memcpy(rp.mem.getVU1Data(), regs + regSize + kVu1Size, kVu1Size);
                    if (rp.corruptMicro >= 0 && rp.corruptMicro < static_cast<long long>(kVu1Size))
                        rp.rep.both("negative control: bit 0 of micro byte 0x%llx flipped before every run", static_cast<unsigned long long>(rp.corruptMicro));
                    seedVifRegs(rp.mem.vif1_regs, regs, regSize);
                    rp.vifCycle = rp.mem.vif1_regs.cycle;
                    seedVuState(rp.vu1.state(), parseState(payload.data() + 12));
                    uint32_t frame = 0, flags = 0;
                    std::memcpy(&frame, payload.data() + 4, 4);
                    std::memcpy(&flags, payload.data() + 8, 4);
                    rp.rep.both("snapshot frame %u flags 0x%x", frame, flags);
                    sawSnap = true;
                    continue;
                }

                switch (type)
                {
                case REC_VIF:
                    rp.feedVif(payload);
                    break;
                case REC_RUNSTART:
                    rp.onRunStart(payload);
                    break;
                case REC_RUNEND:
                    rp.onRunEnd(payload);
                    break;
                case REC_KICKSTART:
                    rp.onKickStart(payload);
                    break;
                case REC_KICKDATA:
                    rp.onKickData(payload);
                    break;
                case REC_VSYNC:
                    ++vsyncs;
                    break;
                case REC_MEMSYNC:
                    if (payload.size() >= 2u * kVu1Size)
                    {
                        std::memcpy(rp.mem.getVU1Code(), payload.data(), kVu1Size);
                        std::memcpy(rp.mem.getVU1Data(), payload.data() + kVu1Size, kVu1Size);
                    }
                    break;
                case REC_END:
                    sawEnd = true;
                    break;
                default:
                    break;
                }
                if (rp.maxRuns && rp.runsDone >= rp.maxRuns)
                    break;
            }
            std::fclose(f);
            rp.compareKicks();

            rp.rep.both("kicks logged after their run by PCSX2 (pulled forward): %llu", static_cast<unsigned long long>(lateKicks));
            rp.rep.both("frames %llu, runs replayed %u%s%s", static_cast<unsigned long long>(vsyncs), rp.runsDone,
                        torn ? ", TORN TAIL" : "", (sawEnd || rp.maxRuns) ? "" : ", no END record");
            uint64_t failing = 0;
            for (Category *c : rp.categories())
            {
                const bool info = std::strstr(c->name, "(info)") != nullptr;
                if (!info)
                    failing += c->count;
                if (c->count)
                    rp.rep.both("  %-48s %8llu runs, first run %u", c->name, static_cast<unsigned long long>(c->count), c->firstRun);
            }
            if (failing == 0)
                rp.rep.both("  0 mismatches");
            if (rp.rep.fp)
                std::fclose(rp.rep.fp);

            t.IsTrue(sawSnap, "capture must start with a SNAP record");
            t.IsTrue(!torn, "capture must not have a torn tail");
            t.IsTrue(rp.runsDone > 0, "capture should contain VU1 runs");
            t.Equals(failing, uint64_t{0}, "every VU1 run should match PCSX2 (see report)");
        });
    });
}
