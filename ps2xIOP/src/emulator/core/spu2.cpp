#include "spu2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// See spu2.h. Register layout per PCSX2 regs.h; behaviour written from the hardware description
// (ADPCM block format, gaussian interpolation, ADSR counter model) and checked against PCSX2 SW in E3.
namespace ps2x::iop::spu2
{
    namespace
    {
        constexpr uint32_t kRamWords = 0x100000u; // 2 MB
        constexpr uint32_t kAddrMask = 0xFFFFFu;
        constexpr int kVoices = 24;

        enum Phase : int
        {
            PhaseStopped = 0,
            PhaseAttack,
            PhaseDecay,
            PhaseSustain,
            PhaseRelease,
        };

        struct Slide
        {
            // Volume register: bit 15 clear => fixed (value << 1, signed). Slide mode (bit 15 set) is modelled with
            // the same counter equations as the envelope.
            uint16_t reg = 0;
            int32_t value = 0;
            uint32_t counter = 0;
        };

        struct Voice
        {
            uint16_t pitch = 0;
            uint16_t adsr1 = 0;
            uint16_t adsr2 = 0;
            uint32_t ssa = 0;
            uint32_t lsax = 0;
            bool loopMode = false;
            Slide volL, volR;

            // runtime
            int phase = PhaseStopped;
            int32_t env = 0;
            uint32_t envCounter = 0;
            uint32_t nextA = 0;
            uint32_t loopStart = 0;
            uint8_t flags = 0;
            bool haveBlock = false;
            std::array<int16_t, 28> block{};
            int32_t prev1 = 0;
            int32_t prev2 = 0;
            std::array<int16_t, 32> fifo{};
            uint32_t wr = 0;
            uint32_t rd = 0;
            uint32_t sp = 0;
            int32_t outx = 0;
        };

        struct Core
        {
            std::array<Voice, kVoices> v{};
            uint32_t endx = 0xFFFFFFu; // PS2 reset value
            uint32_t tsa = 0;
            uint32_t pmon = 0;
            uint32_t vmixL = 0;
            uint32_t vmixR = 0;
            uint16_t mmix = 0;
            Slide masterL, masterR;
            int32_t extVolL = 0x7FFF, extVolR = 0x7FFF; // core 1 only (AVOL)
        };

        struct State
        {
            std::vector<uint16_t> ram = std::vector<uint16_t>(kRamWords);
            std::array<uint16_t, 0x400> reg{}; // raw 16-bit register file, index = off / 2
            std::array<Core, 2> core{};
            uint64_t nextFrameCycle = 0;
            uint64_t frames = 0;
            std::array<std::array<int16_t, 4>, 256> gauss{};
            bool gaussReady = false;
            std::FILE *wav = nullptr;
            uint32_t wavBytes = 0;
            bool wavChecked = false;
        };

        State g;

        std::mutex g_ringLock;
        std::vector<int16_t> g_ring; // interleaved L,R
        constexpr size_t kRingMaxFrames = 48000u * 4u;

        // ---- helpers ----------------------------------------------------------------------------------------

        int16_t clamp16(int32_t x)
        {
            return static_cast<int16_t>(std::clamp(x, -0x8000, 0x7FFF));
        }

        // Gaussian interpolation table (Near / nocash formulation: 512-point windowed sinc, normalised, split into
        // 256 four-tap phases).
        void buildGauss()
        {
            double table[512];
            const double pi = 3.14159265358979323846;
            for (uint32_t n = 0; n < 512; ++n)
            {
                const double k = 0.5 + n;
                const double s = std::sin(pi * k * 2.048 / 1024.0);
                const double t = (std::cos(pi * k * 2.000 / 1023.0) - 1.0) * 0.50;
                const double u = (std::cos(pi * k * 4.000 / 1023.0) - 1.0) * 0.08;
                table[511 - n] = s * (t + u + 1.0) / k;
            }
            double sum = 0.0;
            for (double n : table)
                sum += n;
            const double scale = 0x7F80 * 128 / sum;
            for (double &n : table)
                n *= scale;
            for (uint32_t phase = 0; phase < 256; ++phase)
            {
                const double phaseSum = table[phase] + table[phase + 256] + table[511 - phase] + table[255 - phase];
                const double diff = (phaseSum - 0x7F80) / 4.0;
                g.gauss[255 - phase][0] = static_cast<int16_t>(std::lround(table[phase] - diff));
                g.gauss[255 - phase][1] = static_cast<int16_t>(std::lround(table[phase + 256] - diff));
                g.gauss[255 - phase][2] = static_cast<int16_t>(std::lround(table[511 - phase] - diff));
                g.gauss[255 - phase][3] = static_cast<int16_t>(std::lround(table[255 - phase] - diff));
            }
            g.gaussReady = true;
        }

        uint16_t &rawReg(uint32_t off)
        {
            return g.reg[(off >> 1) & 0x3FFu];
        }

        // Volume register -> fixed value / slide setup.
        void slideSet(Slide &s, uint16_t value)
        {
            s.reg = value;
            if ((value & 0x8000u) == 0u)
                s.value = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
        }

        // Volume slide (bit 15 set): bit 14 exponential, bit 13 decrease, bit 12 phase (negative), bits 2-6 shift, 0-1 step.
        void slideUpdate(Slide &s)
        {
            if ((s.reg & 0x8000u) == 0u)
                return;
            const bool exp = (s.reg & 0x4000u) != 0u;
            const bool decr = (s.reg & 0x2000u) != 0u;
            const bool neg = (s.reg & 0x1000u) != 0u;
            const int shift = (s.reg >> 2u) & 0x1Fu;
            const int step = s.reg & 3u;
            int32_t stepSize = 7 - step;
            if (decr)
                stepSize = ~stepSize;
            uint32_t counterInc = 0x8000u >> std::max(0, shift - 11);
            int32_t levelInc = stepSize << std::max(0, 11 - shift);
            if (exp)
            {
                if (!decr && s.value > 0x6000)
                    counterInc >>= 2;
                if (decr)
                    levelInc = static_cast<int16_t>((levelInc * s.value) >> 15);
            }
            if (step != 3 && shift != 0x1F)
                counterInc = std::max<uint32_t>(1u, counterInc);
            s.counter += counterInc;
            if (!(exp && decr))
                levelInc = neg ? -levelInc : levelInc;
            if (s.counter >= 0x8000u)
            {
                s.counter = 0;
                if (!decr)
                    s.value = std::clamp(s.value + levelInc, -0x8000, 0x7FFF);
                else
                {
                    int32_t lo = neg ? -0x8000 : 0;
                    int32_t hi = neg ? 0 : 0x7FFF;
                    if (exp)
                    {
                        lo = 0;
                        hi = 0x7FFF;
                    }
                    s.value = std::clamp(s.value + levelInc, lo, hi);
                }
            }
        }

        // ---- ADSR -------------------------------------------------------------------------------------------

        struct PhaseParams
        {
            bool decr;
            bool exp;
            int shift;
            int step;
            int32_t target;
        };

        PhaseParams phaseParams(const Voice &v, int phase)
        {
            switch (phase)
            {
            case PhaseAttack:
                return {false, (v.adsr1 & 0x8000u) != 0u, (v.adsr1 >> 10) & 0x1F, 7 - ((v.adsr1 >> 8) & 3), 0x7FFF};
            case PhaseDecay:
                return {true, true, (v.adsr1 >> 4) & 0xF, -8, static_cast<int32_t>(((v.adsr1 & 0xFu) + 1u) << 11)};
            case PhaseSustain:
            {
                const bool decr = (v.adsr2 & 0x4000u) != 0u;
                int step = 7 - ((v.adsr2 >> 6) & 3);
                if (decr)
                    step = ~step;
                return {decr, (v.adsr2 & 0x8000u) != 0u, (v.adsr2 >> 8) & 0x1F, step, 0};
            }
            default:
                return {true, (v.adsr2 & 0x20u) != 0u, v.adsr2 & 0x1F, -8, 0};
            }
        }

        // false => voice finished (all phases done / sustain reached silence)
        bool adsrTick(Voice &v)
        {
            const PhaseParams p = phaseParams(v, v.phase);
            uint32_t counterInc = 0x8000u >> std::max(0, p.shift - 11);
            int32_t levelInc = p.step << std::max(0, 11 - p.shift);
            if (p.exp)
            {
                if (!p.decr && v.env > 0x6000)
                    counterInc >>= 2;
                if (p.decr)
                    levelInc = static_cast<int16_t>((levelInc * v.env) >> 15);
            }
            counterInc = std::max<uint32_t>(1u, counterInc);
            v.envCounter += counterInc;
            if (v.envCounter >= 0x8000u)
            {
                v.envCounter = 0;
                v.env = std::clamp(v.env + levelInc, 0, 0x7FFF);
            }
            if (v.phase == PhaseSustain)
                return v.env != 0;
            if ((!p.decr && v.env >= p.target) || (p.decr && v.env <= p.target))
                ++v.phase;
            return v.phase <= PhaseRelease;
        }

        // ---- ADPCM ------------------------------------------------------------------------------------------

        constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

        void decodeBlock(Voice &v, uint32_t blockAddr)
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(g.ram.data() + (blockAddr & kAddrMask));
            const int shift = (bytes[0] & 0xF) + 16;
            const int id = std::min<int>(bytes[0] >> 4, 4);
            const int p1 = kFilter[id][0];
            const int p2 = kFilter[id][1];
            int out = 0;
            for (int i = 0; i < 14; ++i)
            {
                const int8_t b = static_cast<int8_t>(bytes[2 + i]);
                for (int half = 0; half < 2; ++half)
                {
                    const int32_t data = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint8_t>(b)) << (half ? 24 : 28)) & static_cast<int32_t>(0xF0000000u);
                    int32_t pcm = (data >> shift) + (((p1 * v.prev1) + (p2 * v.prev2) + 32) >> 6);
                    pcm = std::clamp(pcm, -0x8000, 0x7FFF);
                    v.block[out++] = static_cast<int16_t>(pcm);
                    v.prev2 = v.prev1;
                    v.prev1 = pcm;
                }
            }
        }

        // Append one data word (4 samples) to the voice's decode FIFO; handles block headers and loop/end flags.
        void fetchWord(Core &c, int voiceIdx)
        {
            Voice &v = c.v[voiceIdx];
            const uint32_t blockAddr = v.nextA & 0xFFFF8u;
            if (!v.haveBlock)
            {
                const uint16_t header = g.ram[blockAddr];
                v.flags = static_cast<uint8_t>(header >> 8);
                if ((v.flags & 4u) != 0u && !v.loopMode)
                    v.loopStart = blockAddr;
                decodeBlock(v, blockAddr);
                v.haveBlock = true;
            }
            const int sampleIdx = (static_cast<int>(v.nextA & 7u) - 1) * 4;
            for (int i = 0; i < 4; ++i)
                v.fifo[(v.wr + i) & 31u] = v.block[sampleIdx + i];
            v.wr += 4;
            v.nextA = (v.nextA + 1u) & kAddrMask;
            if ((v.nextA & 7u) == 0u)
            {
                if ((v.flags & 1u) != 0u) // loop end
                {
                    c.endx |= 1u << voiceIdx;
                    v.nextA = v.loopStart;
                    if ((v.flags & 2u) == 0u) // not looping => voice off
                    {
                        v.env = 0;
                        v.phase = PhaseStopped;
                    }
                }
                v.nextA = (v.nextA + 1u) & kAddrMask; // skip the next block's header word
                v.haveBlock = false;
            }
        }

        void startVoice(Core &c, int idx)
        {
            Voice &v = c.v[idx];
            v.phase = PhaseAttack;
            v.env = 0;
            v.envCounter = 0;
            v.sp = 0;
            v.flags = 0;
            v.haveBlock = false;
            v.nextA = ((v.ssa & 0xFFFF8u) | 1u) & kAddrMask;
            // PCSX2 V_Voice::Start (spu2sys.cpp:216): KON clears LoopMode, so a loop-start flag in the new
            // sample's blocks sets LSA again; an LSAX write earlier must not pin the loop point forever.
            v.loopMode = false;
            v.loopStart = v.ssa & 0xFFFF8u;
            v.prev1 = v.prev2 = 0;
            v.wr = v.rd = 0;
            v.outx = 0;
            c.endx &= ~(1u << idx);
        }

        void stopVoice(Voice &v)
        {
            if (v.phase != PhaseStopped)
            {
                v.phase = PhaseRelease;
                v.envCounter = 0;
            }
        }

        // One 48 kHz frame of one voice. Returns the post-ADSR sample (before voice volume).
        int32_t voiceSample(Core &c, int idx)
        {
            Voice &v = c.v[idx];
            slideUpdate(v.volL);
            slideUpdate(v.volR);
            if (v.phase == PhaseStopped)
            {
                v.outx = 0;
                return 0;
            }
            // keep the 4-tap window fed
            int guard = 0;
            while (static_cast<int32_t>(v.wr - v.rd) < 4 && v.phase != PhaseStopped && guard++ < 8)
                fetchWord(c, idx);
            int32_t out = 0;
            if (static_cast<int32_t>(v.wr - v.rd) >= 4)
            {
                const auto &taps = g.gauss[(v.sp >> 4) & 0xFFu];
                for (int i = 0; i < 4; ++i)
                    out += (taps[i] * v.fifo[(v.rd + i) & 31u]) >> 15;
            }
            if (!adsrTick(v))
            {
                v.env = 0;
                v.phase = PhaseStopped;
            }
            out = (out * v.env) >> 15;
            v.outx = out;

            int32_t pitch = v.pitch;
            if (((c.pmon >> idx) & 1u) != 0u && idx > 0)
                pitch = std::clamp((pitch * (32768 + c.v[idx - 1].outx)) >> 15, 0, 0x3FFF);
            pitch = std::min(pitch, 0x3FFF);
            v.sp += static_cast<uint32_t>(pitch);
            v.rd += v.sp >> 12;
            v.sp &= 0xFFFu;
            return out;
        }

        int32_t applyVol(int32_t data, int32_t vol)
        {
            return (vol * data) >> 15;
        }

        void wavOpen()
        {
            g.wavChecked = true;
            const char *path = std::getenv("PS2X_SPU2_WAV");
            if (!path || !*path)
                return;
            g.wav = std::fopen(path, "wb");
            if (!g.wav)
                return;
            uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                               0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
            std::fwrite(hdr, 1, sizeof(hdr), g.wav);
        }

        void wavPatch()
        {
            if (!g.wav)
                return;
            const uint32_t riff = 36u + g.wavBytes;
            std::fseek(g.wav, 4, SEEK_SET);
            std::fwrite(&riff, 4, 1, g.wav);
            std::fseek(g.wav, 40, SEEK_SET);
            std::fwrite(&g.wavBytes, 4, 1, g.wav);
            std::fseek(g.wav, 0, SEEK_END);
            std::fflush(g.wav);
        }

        void emit(int16_t l, int16_t r)
        {
            if (!g.wavChecked)
                wavOpen();
            if (g.wav)
            {
                const int16_t s[2] = {l, r};
                std::fwrite(s, sizeof(s), 1, g.wav);
                g.wavBytes += 4u;
                if ((g.frames % 48000u) == 0u)
                    wavPatch();
            }
            std::lock_guard<std::mutex> lock(g_ringLock);
            if (g_ring.size() / 2u < kRingMaxFrames)
            {
                g_ring.push_back(l);
                g_ring.push_back(r);
            }
        }

        // One 48 kHz output frame.
        void mixFrame()
        {
            int32_t coreL[2] = {0, 0};
            int32_t coreR[2] = {0, 0};
            for (int ci = 0; ci < 2; ++ci)
            {
                Core &c = g.core[ci];
                int32_t dryL = 0, dryR = 0;
                for (int vi = 0; vi < kVoices; ++vi)
                {
                    Voice &v = c.v[vi];
                    const int32_t s = voiceSample(c, vi);
                    if (s == 0)
                        continue;
                    const int32_t l = applyVol(s, v.volL.value);
                    const int32_t r = applyVol(s, v.volR.value);
                    if ((c.vmixL >> vi) & 1u)
                        dryL += l;
                    if ((c.vmixR >> vi) & 1u)
                        dryR += r;
                }
                coreL[ci] = (c.mmix & 0x800u) ? clamp16(dryL) : 0;
                coreR[ci] = (c.mmix & 0x400u) ? clamp16(dryR) : 0;
                slideUpdate(c.masterL);
                slideUpdate(c.masterR);
            }
            // core 0 -> master volume -> core 1 external input
            const int32_t extL = applyVol(coreL[0], g.core[0].masterL.value);
            const int32_t extR = applyVol(coreR[0], g.core[0].masterR.value);
            Core &c1 = g.core[1];
            int32_t outL = coreL[1];
            int32_t outR = coreR[1];
            if (c1.mmix & 0x8u)
                outL += applyVol(extL, c1.extVolL);
            if (c1.mmix & 0x4u)
                outR += applyVol(extR, c1.extVolR);
            outL = applyVol(clamp16(outL), c1.masterL.value);
            outR = applyVol(clamp16(outR), c1.masterR.value);
            emit(clamp16(outL), clamp16(outR));
            ++g.frames;
        }

        // 20-bit SPU word address from the hi/lo register pair (hi = bits 16-19, lo = bits 3-15; low 3 bits dropped).
        uint32_t makeAddr(uint16_t hi, uint16_t lo)
        {
            return ((static_cast<uint32_t>(hi & 0xFu) << 16) | (lo & 0xFFF8u)) & kAddrMask;
        }
    }

    void reset()
    {
        if (g.wav)
        {
            wavPatch();
            std::fclose(g.wav);
            g.wav = nullptr;
        }
        std::fill(g.ram.begin(), g.ram.end(), uint16_t{0});
        g.reg.fill(0);
        g.core = {};
        g.nextFrameCycle = 0;
        g.frames = 0;
        g.wavBytes = 0;
        g.wavChecked = false;
        if (!g.gaussReady)
            buildGauss();
        for (Core &c : g.core)
            for (Voice &v : c.v)
            {
                v.nextA = 0x2801u;
                v.ssa = v.lsax = 0x2800u;
            }
        {
            std::lock_guard<std::mutex> lock(g_ringLock);
            g_ring.clear();
        }
    }

    void writeReg(uint32_t off, uint16_t value)
    {
        if (off >= kSize) // IopMemory forwards the whole 64 KB window; do not mirror it onto live registers
            return;
        off &= 0x7FEu;
        if (!g.gaussReady)
            buildGauss();
        rawReg(off) = value;
        const int ci = (off < 0x760u && (off & 0x400u)) ? 1 : 0;
        if (off >= 0x760u)
        {
            // master/effects volume block: core 0 = 0x760.., core 1 = 0x788..
            if (off >= 0x7C0u)
                return; // SPDIF
            const int core = off >= 0x788u ? 1 : 0;
            const uint32_t r = off - (core ? 0x788u : 0x760u);
            Core &c = g.core[core];
            switch (r)
            {
            case 0x00: slideSet(c.masterL, value); break;
            case 0x02: slideSet(c.masterR, value); break;
            case 0x08: c.extVolL = static_cast<int16_t>(value); break;
            case 0x0A: c.extVolR = static_cast<int16_t>(value); break;
            default: break;
            }
            return;
        }
        Core &c = g.core[ci];
        const uint32_t r = off & 0x3FFu;
        if (r < 0x180u)
        {
            Voice &v = c.v[r / 16u];
            switch ((r % 16u) / 2u)
            {
            case 0: slideSet(v.volL, value); break;
            case 1: slideSet(v.volR, value); break;
            case 2: v.pitch = value; break;
            case 3: v.adsr1 = value; break;
            case 4: v.adsr2 = value; break;
            default: break;
            }
            return;
        }
        if (r >= 0x1C0u && r < 0x2E0u)
        {
            const uint32_t k = r - 0x1C0u;
            Voice &v = c.v[k / 12u];
            const uint32_t f = k % 12u;
            const uint32_t base = (0x1C0u + (k / 12u) * 12u) | (static_cast<uint32_t>(ci) << 10);
            const auto pair = [&](uint32_t first)
            { return makeAddr(rawReg(base + first), rawReg(base + first + 2u)); };
            if (f < 4u)
                v.ssa = pair(0u);
            else if (f < 8u)
            {
                v.loopMode = true;
                v.lsax = pair(4u);
                v.loopStart = v.lsax;
            }
            else
            {
                v.nextA = pair(8u) | 1u;
                v.haveBlock = false;
            }
            return;
        }
        switch (r)
        {
        case 0x180: case 0x182:
            c.pmon = rawReg(off & ~2u & 0x7FFu) | (static_cast<uint32_t>(rawReg((off & ~2u & 0x7FFu) + 2u)) << 16);
            break;
        case 0x188: case 0x18A:
            c.vmixL = rawReg(ci * 0x400u + 0x188u) | (static_cast<uint32_t>(rawReg(ci * 0x400u + 0x18Au)) << 16);
            break;
        case 0x190: case 0x192:
            c.vmixR = rawReg(ci * 0x400u + 0x190u) | (static_cast<uint32_t>(rawReg(ci * 0x400u + 0x192u)) << 16);
            break;
        case 0x198:
            c.mmix = value;
            break;
        case 0x1A0: // KON lo (voices 0-15)
        case 0x1A2: // KON hi (16-23)
        {
            const uint32_t mask = (r == 0x1A0u ? static_cast<uint32_t>(value) : (static_cast<uint32_t>(value & 0xFFu) << 16));
            for (int i = 0; i < kVoices; ++i)
                if ((mask >> i) & 1u)
                    startVoice(c, i);
            break;
        }
        case 0x1A4:
        case 0x1A6:
        {
            const uint32_t mask = (r == 0x1A4u ? static_cast<uint32_t>(value) : (static_cast<uint32_t>(value & 0xFFu) << 16));
            for (int i = 0; i < kVoices; ++i)
                if ((mask >> i) & 1u)
                    stopVoice(c.v[i]);
            break;
        }
        case 0x1A8: // TSA hi
        case 0x1AA: // TSA lo
            c.tsa = ((static_cast<uint32_t>(rawReg(ci * 0x400u + 0x1A8u) & 0xFu) << 16) | rawReg(ci * 0x400u + 0x1AAu)) & kAddrMask;
            break;
        case 0x1AC: // DATA port (PIO)
            g.ram[c.tsa & kAddrMask] = value;
            c.tsa = (c.tsa + 1u) & kAddrMask;
            break;
        default:
            break;
        }
    }

    bool readLive(uint32_t off, uint16_t *value)
    {
        if (off >= kSize)
            return false;
        off &= 0x7FEu;
        if (off >= 0x760u)
            return false;
        const int ci = (off & 0x400u) ? 1 : 0;
        const Core &c = g.core[ci];
        const uint32_t r = off & 0x3FFu;
        if (r < 0x180u)
        {
            const Voice &v = c.v[r / 16u];
            switch ((r % 16u) / 2u)
            {
            case 5: *value = static_cast<uint16_t>(v.env); return true;
            case 6: *value = static_cast<uint16_t>(v.volL.value); return true;
            case 7: *value = static_cast<uint16_t>(v.volR.value); return true;
            default: return false;
            }
        }
        if (r >= 0x1C0u && r < 0x2E0u)
        {
            const uint32_t k = r - 0x1C0u;
            if ((k % 12u) >= 8u)
            {
                const Voice &v = c.v[k / 12u];
                *value = (k % 12u) == 8u ? static_cast<uint16_t>((v.nextA >> 16) & 0xFu) : static_cast<uint16_t>(v.nextA & 0xFFFFu);
                return true;
            }
            return false;
        }
        if (r == 0x340u)
        {
            *value = static_cast<uint16_t>(c.endx & 0xFFFFu);
            return true;
        }
        if (r == 0x342u)
        {
            *value = static_cast<uint16_t>(c.endx >> 16);
            return true;
        }
        return false;
    }

    void dmaToSpu(int core, const uint8_t *src, size_t bytes)
    {
        Core &c = g.core[core & 1];
        for (size_t i = 0; i + 1 < bytes; i += 2)
        {
            g.ram[c.tsa & kAddrMask] = static_cast<uint16_t>(src[i] | (src[i + 1] << 8));
            c.tsa = (c.tsa + 1u) & kAddrMask;
        }
    }

    void dmaFromSpu(int core, uint8_t *dst, size_t bytes)
    {
        Core &c = g.core[core & 1];
        for (size_t i = 0; i + 1 < bytes; i += 2)
        {
            const uint16_t w = g.ram[c.tsa & kAddrMask];
            dst[i] = static_cast<uint8_t>(w);
            dst[i + 1] = static_cast<uint8_t>(w >> 8);
            c.tsa = (c.tsa + 1u) & kAddrMask;
        }
    }

    void advance(uint64_t iopCycles)
    {
        if (!g.gaussReady)
            buildGauss();
        // After a long idle jump do not mix an unbounded backlog of silence frame by frame.
        if (iopCycles > g.nextFrameCycle + 48000ull * kCyclesPerFrame)
            g.nextFrameCycle = iopCycles - 4800ull * kCyclesPerFrame;
        while (g.nextFrameCycle + kCyclesPerFrame <= iopCycles)
        {
            mixFrame();
            g.nextFrameCycle += kCyclesPerFrame;
        }
    }

    size_t popFrames(int16_t *dst, size_t maxFrames)
    {
        std::lock_guard<std::mutex> lock(g_ringLock);
        const size_t have = g_ring.size() / 2u;
        const size_t n = std::min(have, maxFrames);
        if (n)
        {
            std::memcpy(dst, g_ring.data(), n * 2u * sizeof(int16_t));
            g_ring.erase(g_ring.begin(), g_ring.begin() + static_cast<std::ptrdiff_t>(n * 2u));
        }
        return n;
    }

    size_t framesQueued()
    {
        std::lock_guard<std::mutex> lock(g_ringLock);
        return g_ring.size() / 2u;
    }

    uint64_t framesGenerated()
    {
        return g.frames;
    }
}
