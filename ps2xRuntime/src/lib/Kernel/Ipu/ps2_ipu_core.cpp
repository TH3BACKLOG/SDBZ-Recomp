// PS2 Image Processing Unit -- register file, FIFOs, DMA channels 3/4, and the
// MPEG macroblock decoder behind BDEC/VDEC/FDEC/CSC/PACK/SETIQ/SETVQ/SETTH.
//
// See ps2_ipu_core.h for why this exists and how it is scheduled. Two facts
// from the guest driver shape the whole design and are worth restating here,
// because both were checked against the disassembly rather than assumed:
//
//  * The guest's "wait for command" routine at 0x424b98 spins on IPU_CMD bit 63
//    and, crucially, REFILLS the IPU_TO DMA from inside that loop (0x424c70
//    reads IPU_TO's QWC, 0x424c7c calls the refill helper at 0x428448) with a
//    501-iteration bail-out. So reporting BUSY while a command is starved is
//    both correct and safe: the guest reacts by feeding more data, and a
//    genuine stall terminates instead of hanging.
//
//  * 0x424484 tests the VDEC macroblock-type result with `andi $v0, $a0, 0xc`,
//    which pins the flag encoding to bit2 = backward, bit3 = forward, and
//    0x424440 reads the result as a sign-extended 16-bit value out of
//    IPU_CMD[15:0].
//
// Bitstream order is plain memory order, most-significant-bit first: the guest
// DMAs the MPEG elementary stream as bytes and the quantiser matrices arrive
// through the same FIFO in ascending zigzag order.

#include "ps2_ipu_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2_ipu
{
    namespace
    {
        // ------------------------------------------------------------------
        // Host memory
        // ------------------------------------------------------------------
        uint8_t *g_rdram = nullptr;
        uint32_t g_ramSize = 0;
        uint8_t *g_spr = nullptr;
        uint32_t g_sprSize = 0;

        uint8_t *dmaPtr(uint32_t addr, uint32_t bytes)
        {
            if ((addr & 0x80000000u) != 0u)
            {
                if (!g_spr || g_sprSize == 0)
                    return nullptr;
                const uint32_t off = addr & (g_sprSize - 1u);
                if (off + bytes > g_sprSize)
                    return nullptr;
                return g_spr + off;
            }
            const uint32_t phys = addr & 0x1FFFFFFFu;
            if (!g_rdram || phys + bytes > g_ramSize)
                return nullptr;
            return g_rdram + phys;
        }

        // ------------------------------------------------------------------
        // Diagnostics
        // ------------------------------------------------------------------
        Stats g_stats{};

        // Context captured where a VLC lookup failed, so the ECD log names the
        // offending bit pattern instead of just "something went wrong".
        uint32_t g_failBits = 0;
        int g_failBlock = -1;
        int g_failIndex = -1;
        bool g_failIntra = false;

        // Histogram of DCT run values / code lengths seen in blocks that
        // decoded cleanly. Proves which table entries real streams exercise.
        bool g_hist = std::getenv("PS2X_IPU_HIST") != nullptr;
        unsigned long long g_histRun[64] = {};
        unsigned long long g_histLen[24] = {};
        unsigned long long g_histPair[64][24] = {};

        void histNote(int run, int len)
        {
            if (run >= 0 && run < 64)
                ++g_histRun[run];
            if (len >= 0 && len < 24)
                ++g_histLen[len];
            if (run >= 0 && run < 64 && len >= 0 && len < 24)
                ++g_histPair[run][len];
        }

        uint32_t g_trace = 0;     // PS2X_IPUTRACE: first N command traces
        uint32_t g_traced = 0;
        bool g_traceInit = false;
        uint64_t g_nextSummary = 0;

        void traceInit()
        {
            if (g_traceInit)
                return;
            g_traceInit = true;
            const char *e = std::getenv("PS2X_IPUTRACE");
            g_trace = (e && *e) ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
        }

        // ------------------------------------------------------------------
        // Register state
        // ------------------------------------------------------------------
        // Guest-writable IPU_CTRL configuration bits: IDP(16-17), AS(20),
        // IVF(21), QST(22), MP1(23), PCT(24-26).
        constexpr uint32_t kCtrlCfgMask = 0x07F30000u;

        uint32_t g_ctrlCfg = 0;
        uint32_t g_cbp = 0;
        bool g_ecd = false;
        bool g_scd = false;
        uint32_t g_cmdResult = 0;
        bool g_haveCmdResult = false;

        uint32_t g_th0 = 0;
        uint32_t g_th1 = 0;

        uint8_t g_iqIntra[64];
        uint8_t g_iqNonIntra[64];
        uint16_t g_vq[16];

        int32_t g_dcPred[3] = {128, 128, 128};

        inline uint32_t ctrlIdp() { return (g_ctrlCfg >> 16) & 3u; }
        inline bool ctrlAltScan() { return (g_ctrlCfg & (1u << 20)) != 0u; }
        inline bool ctrlIvf() { return (g_ctrlCfg & (1u << 21)) != 0u; }
        inline bool ctrlQst() { return (g_ctrlCfg & (1u << 22)) != 0u; }
        inline bool ctrlMp1() { return (g_ctrlCfg & (1u << 23)) != 0u; }
        inline uint32_t ctrlPictureType() { return (g_ctrlCfg >> 24) & 7u; }

        // ------------------------------------------------------------------
        // DMA channels
        // ------------------------------------------------------------------
        struct Channel
        {
            uint32_t chcr = 0;
            uint32_t madr = 0;
            uint32_t qwc = 0;
            uint32_t tadr = 0;
            uint32_t asr0 = 0;
            uint32_t asr1 = 0;
            bool running = false;
            bool chainDone = false;
        };

        Channel g_to;   // channel 4, RAM -> IPU
        Channel g_from; // channel 3, IPU -> RAM
        bool g_toCompleted = false;
        bool g_fromCompleted = false;

        // ------------------------------------------------------------------
        // FIFOs
        // ------------------------------------------------------------------
        // Input is append-only for the duration of one command so a starved
        // command can roll its bit cursor back and retry from the exact same
        // position once more data arrives. Consumed quadwords are dropped only
        // at a successful commit.
        std::vector<uint8_t> g_in;
        uint32_t g_bp = 0; // bit cursor into g_in

        std::vector<uint8_t> g_out;
        size_t g_outRead = 0;

        inline size_t outAvail() { return g_out.size() - g_outRead; }

        void compactOut()
        {
            if (g_outRead >= 4096 && g_outRead == g_out.size())
            {
                g_out.clear();
                g_outRead = 0;
            }
            else if (g_outRead >= 65536)
            {
                g_out.erase(g_out.begin(), g_out.begin() + static_cast<ptrdiff_t>(g_outRead));
                g_outRead = 0;
            }
        }

        void serviceFrom();

        void pushOut(const uint8_t *src, size_t n)
        {
            g_out.insert(g_out.end(), src, src + n);
            g_stats.bytesOut += n;
            serviceFrom();
        }

        // Bit position a starved command rewinds to. Advanced explicitly at
        // each point where progress is durable (one whole macroblock), so a
        // multi-macroblock CSC never re-converts work it already emitted.
        uint32_t g_rollbackBp = 0;

        // Drop fully consumed quadwords from the front of the input FIFO.
        void commitInput()
        {
            const uint32_t whole = g_bp / 128u;
            if (whole == 0)
                return;
            const uint32_t shift = whole * 128u;
            const size_t bytes = size_t(whole) * 16u;
            if (bytes >= g_in.size())
                g_in.clear();
            else
                g_in.erase(g_in.begin(), g_in.begin() + static_cast<ptrdiff_t>(bytes));
            g_bp -= shift;
            g_rollbackBp = (g_rollbackBp >= shift) ? (g_rollbackBp - shift) : 0u;
        }

        // Mark everything decoded so far as durable, then reclaim the input.
        void commitProgress()
        {
            g_rollbackBp = g_bp;
            commitInput();
        }

        inline int64_t availBits()
        {
            return static_cast<int64_t>(g_in.size()) * 8 - static_cast<int64_t>(g_bp);
        }

        // ------------------------------------------------------------------
        // Pulling the next quadword out of the IPU_TO descriptor
        // ------------------------------------------------------------------
        bool advanceToChain();

        bool pullQword()
        {
            for (int guard = 0; guard < 4096; ++guard)
            {
                if (!g_to.running)
                    return false;

                if (g_to.qwc > 0)
                {
                    const uint8_t *src = dmaPtr(g_to.madr, 16);
                    if (!src)
                    {
                        // Unmapped source: stop rather than fabricate data.
                        g_to.running = false;
                        g_to.chcr &= ~0x100u;
                        g_toCompleted = true;
                        return false;
                    }
                    g_in.insert(g_in.end(), src, src + 16);
                    g_stats.bytesIn += 16;
                    g_to.madr += 16;
                    --g_to.qwc;
                    if (g_to.qwc == 0 && ((g_to.chcr >> 2) & 3u) == 0u)
                    {
                        // Normal mode: the descriptor is drained.
                        g_to.running = false;
                        g_to.chcr &= ~0x100u;
                        g_toCompleted = true;
                    }
                    return true;
                }

                // qwc exhausted; chain mode may have another tag.
                if (((g_to.chcr >> 2) & 3u) != 1u || !advanceToChain())
                {
                    g_to.running = false;
                    g_to.chcr &= ~0x100u;
                    g_toCompleted = true;
                    return false;
                }
            }
            return false;
        }

        // Walk one source-chain DMAtag. Returns false at the end of the chain.
        bool advanceToChain()
        {
            if (g_to.chainDone)
                return false;

            const uint8_t *tagPtr = dmaPtr(g_to.tadr, 16);
            if (!tagPtr)
                return false;

            uint64_t tag = 0;
            std::memcpy(&tag, tagPtr, sizeof(tag));
            const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
            const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFu);
            const bool irq = ((tag >> 31) & 1u) != 0u;
            const bool tie = (g_to.chcr & (1u << 7)) != 0u;
            uint32_t asp = (g_to.chcr >> 4) & 3u;

            uint32_t dataAddr = 0;
            bool endChain = false;

            switch (id)
            {
            case 0: // refe
                dataAddr = addr;
                g_to.tadr += 16;
                endChain = true;
                break;
            case 1: // cnt
                dataAddr = g_to.tadr + 16;
                g_to.tadr = dataAddr + qwc * 16u;
                break;
            case 2: // next
                dataAddr = g_to.tadr + 16;
                g_to.tadr = addr;
                break;
            case 3: // ref
            case 4: // refs
                dataAddr = addr;
                g_to.tadr += 16;
                break;
            case 5: // call
                dataAddr = g_to.tadr + 16;
                if (asp == 0)
                {
                    g_to.asr0 = dataAddr + qwc * 16u;
                    asp = 1;
                }
                else if (asp == 1)
                {
                    g_to.asr1 = dataAddr + qwc * 16u;
                    asp = 2;
                }
                g_to.tadr = addr;
                break;
            case 6: // ret
                dataAddr = g_to.tadr + 16;
                if (asp == 2)
                {
                    g_to.tadr = g_to.asr1;
                    asp = 1;
                }
                else if (asp == 1)
                {
                    g_to.tadr = g_to.asr0;
                    asp = 0;
                }
                else
                {
                    endChain = true;
                }
                break;
            case 7: // end
            default:
                dataAddr = g_to.tadr + 16;
                endChain = true;
                break;
            }

            g_to.chcr = (g_to.chcr & ~(3u << 4)) | (asp << 4);
            g_to.madr = dataAddr;
            g_to.qwc = qwc;
            if (irq && tie)
                endChain = true;
            if (endChain)
                g_to.chainDone = true;

            return qwc > 0 || !endChain;
        }

        // ------------------------------------------------------------------
        // Bit reader
        // ------------------------------------------------------------------
        bool ensureBits(uint32_t n)
        {
            while (availBits() < static_cast<int64_t>(n))
            {
                if (!pullQword())
                    return false;
            }
            return true;
        }

        // Top the input FIFO back up to its hardware depth of 8 quadwords.
        void fillInputFifo()
        {
            constexpr int64_t kFifoBits = 128 * 8;
            while (availBits() < kFifoBits && g_to.running)
            {
                if (!pullQword())
                    break;
            }
        }

        // Peek n (<= 32) bits without consuming; missing bits read as zero.
        uint32_t peekBits(uint32_t n)
        {
            if (n == 0)
                return 0;
            const size_t byte = g_bp >> 3;
            const uint32_t bit = g_bp & 7u;
            uint64_t acc = 0;
            for (int i = 0; i < 5; ++i)
            {
                const size_t idx = byte + static_cast<size_t>(i);
                acc = (acc << 8) | (idx < g_in.size() ? static_cast<uint64_t>(g_in[idx]) : 0ull);
            }
            acc >>= (40u - bit - n);
            if (n >= 32)
                return static_cast<uint32_t>(acc);
            return static_cast<uint32_t>(acc & ((1ull << n) - 1ull));
        }

        inline void skipBits(uint32_t n) { g_bp += n; }

        inline uint32_t getBits(uint32_t n)
        {
            const uint32_t v = peekBits(n);
            g_bp += n;
            return v;
        }

        // 27 bits is the widest window any VLC table needs. If the FIFO cannot
        // supply them the command stalls rather than decoding zero-padding:
        // that is what the hardware does (BUSY stays set until the DMA catches
        // up) and it is what the guest's wait loop at 0x424b98 is built to
        // handle -- it refills IPU_TO from inside the loop and bails after 501
        // tries. Padding here would silently manufacture wrong coefficients.
        inline bool peek27(uint32_t &out)
        {
            if (!ensureBits(27))
                return false;
            out = peekBits(27);
            return true;
        }

        // ------------------------------------------------------------------
        // Scan orders and quantiser defaults
        // ------------------------------------------------------------------
        const uint8_t kZigzag[64] = {
            0, 1, 8, 16, 9, 2, 3, 10,
            17, 24, 32, 25, 18, 11, 4, 5,
            12, 19, 26, 33, 40, 48, 41, 34,
            27, 20, 13, 6, 7, 14, 21, 28,
            35, 42, 49, 56, 57, 50, 43, 36,
            29, 22, 15, 23, 30, 37, 44, 51,
            58, 59, 52, 45, 38, 31, 39, 46,
            53, 60, 61, 54, 47, 55, 62, 63};

        const uint8_t kAltScan[64] = {
            0, 8, 16, 24, 1, 9, 2, 10,
            17, 25, 32, 40, 48, 56, 57, 49,
            41, 33, 26, 18, 11, 3, 4, 12,
            19, 27, 34, 42, 50, 58, 35, 43,
            51, 59, 20, 28, 5, 13, 6, 14,
            21, 29, 36, 44, 52, 60, 37, 45,
            53, 61, 22, 30, 7, 15, 23, 31,
            38, 46, 54, 62, 39, 47, 55, 63};

        const uint8_t kDefaultIntra[64] = {
            8, 16, 19, 22, 26, 27, 29, 34,
            16, 16, 22, 24, 27, 29, 34, 37,
            19, 22, 26, 27, 29, 34, 34, 38,
            22, 22, 26, 27, 29, 34, 37, 40,
            22, 26, 27, 29, 32, 35, 40, 48,
            26, 27, 29, 32, 35, 40, 48, 58,
            26, 27, 29, 34, 38, 46, 56, 69,
            27, 29, 35, 38, 46, 56, 69, 83};

        const uint8_t kNonLinearQ[32] = {
            0, 1, 2, 3, 4, 5, 6, 7,
            8, 10, 12, 14, 16, 18, 20, 22,
            24, 28, 32, 36, 40, 44, 48, 52,
            56, 64, 72, 80, 88, 96, 104, 112};

        // ------------------------------------------------------------------
        // IDCT
        // ------------------------------------------------------------------
        float g_idctCos[8][8];
        bool g_idctReady = false;

        void idctInit()
        {
            if (g_idctReady)
                return;
            g_idctReady = true;
            for (int x = 0; x < 8; ++x)
            {
                for (int u = 0; u < 8; ++u)
                {
                    const double c = (u == 0) ? std::sqrt(0.125) : 0.5;
                    g_idctCos[x][u] = static_cast<float>(
                        c * std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0));
                }
            }
        }

        void idct8x8(const int32_t in[64], int32_t out[64])
        {
            float tmp[64];
            for (int y = 0; y < 8; ++y)
            {
                const int32_t *row = in + y * 8;
                float *dst = tmp + y * 8;
                for (int x = 0; x < 8; ++x)
                {
                    float s = 0.0f;
                    for (int u = 0; u < 8; ++u)
                        s += g_idctCos[x][u] * static_cast<float>(row[u]);
                    dst[x] = s;
                }
            }
            for (int x = 0; x < 8; ++x)
            {
                for (int y = 0; y < 8; ++y)
                {
                    float s = 0.0f;
                    for (int v = 0; v < 8; ++v)
                        s += g_idctCos[y][v] * tmp[v * 8 + x];
                    out[y * 8 + x] = static_cast<int32_t>(std::lrint(s));
                }
            }
        }

        // ------------------------------------------------------------------
        // Block decode
        // ------------------------------------------------------------------
        inline int32_t sgn(int32_t v) { return (v > 0) ? 1 : ((v < 0) ? -1 : 0); }

        inline int32_t clampi(int32_t v, int32_t lo, int32_t hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // Decode one 8x8 block into `coeff` (raster order, dequantised).
        // Returns false if the input ran dry (retryable) -- `fatal` separates
        // that from a malformed stream, which raises ECD instead.
        bool decodeBlock(int blockIndex, bool intra, uint32_t qsc, int32_t coeff[64], bool &fatal)
        {
            fatal = false;
            std::memset(coeff, 0, sizeof(int32_t) * 64);

            const bool mp1 = ctrlMp1();
            const uint8_t *scan = ctrlAltScan() ? kAltScan : kZigzag;
            const uint32_t idp = mp1 ? 0u : ctrlIdp();
            const int32_t qscale = mp1 ? static_cast<int32_t>(qsc)
                                       : (ctrlQst() ? static_cast<int32_t>(kNonLinearQ[qsc & 31u])
                                                    : static_cast<int32_t>(qsc) * 2);
            const int32_t divisor = mp1 ? 16 : 32;
            const int cc = (blockIndex < 4) ? 0 : (blockIndex == 4 ? 1 : 2);

            int index = 0;

            if (intra)
            {
                uint32_t bits = 0;
                if (!peek27(bits))
                    return false;
                const VlcResult dc = (cc == 0) ? decodeDcSizeLuma(bits) : decodeDcSizeChroma(bits);
                if (dc.len == 0)
                {
                    fatal = true;
                    return false;
                }
                skipBits(dc.len);

                int32_t diff = 0;
                if (dc.value > 0)
                {
                    const uint32_t size = static_cast<uint32_t>(dc.value);
                    if (!ensureBits(size))
                        return false;
                    const uint32_t raw = getBits(size);
                    diff = static_cast<int32_t>(raw);
                    if (diff < (1 << (size - 1)))
                        diff -= (1 << size) - 1;
                }
                g_dcPred[cc] += diff;
                coeff[0] = g_dcPred[cc] * static_cast<int32_t>(8u >> idp);
                index = 1;
            }

            bool first = !intra;
            int32_t sum = 0;

            // Ring of symbols decoded in this block, dumped if the block fails.
            struct Sym { int32_t run, level; uint32_t len; uint32_t bp; };
            Sym trail[72];
            int trailN = 0;

            while (true)
            {
                uint32_t bits = 0;
                if (!peek27(bits))
                    return false;

                const DctResult d = decodeDctCoeff(ctrlIvf() && intra, first, bits);
                if (d.len == 0)
                {
                    g_failBits = bits;
                    g_failBlock = blockIndex;
                    g_failIndex = index;
                    g_failIntra = intra;
                    fatal = true;
                    return false;
                }
                skipBits(d.len);
                first = false;

                if (d.run == kDctEob)
                    break;

                int32_t run = 0;
                int32_t level = 0;

                if (d.run == kDctEscape)
                {
                    if (!ensureBits(mp1 ? 30u : 18u))
                        return false;
                    run = static_cast<int32_t>(getBits(6));
                    if (mp1)
                    {
                        const uint32_t l8 = getBits(8);
                        if (l8 == 0x00u)
                            level = static_cast<int32_t>(getBits(8));
                        else if (l8 == 0x80u)
                            level = static_cast<int32_t>(getBits(8)) - 256;
                        else
                            level = (l8 & 0x80u) ? static_cast<int32_t>(l8) - 256
                                                 : static_cast<int32_t>(l8);
                    }
                    else
                    {
                        const uint32_t l12 = getBits(12);
                        level = (l12 & 0x800u) ? static_cast<int32_t>(l12) - 4096
                                               : static_cast<int32_t>(l12);
                    }
                }
                else
                {
                    if (!ensureBits(1))
                        return false;
                    run = d.run;
                    level = d.level;
                    if (getBits(1))
                        level = -level;
                }

                if (trailN < 72)
                    trail[trailN++] = Sym{run, level, d.len, g_bp};

                index += run;
                if (index > 63)
                {
                    g_failBits = bits;
                    g_failBlock = blockIndex;
                    g_failIndex = index;
                    g_failIntra = intra;
                    if (std::getenv("PS2X_IPU_TRAIL"))
                    {
                        std::fprintf(stderr, "[ipu:trail] blk=%d intra=%d n=%d:", blockIndex,
                                     intra ? 1 : 0, trailN);
                        for (int t = 0; t < trailN; ++t)
                            std::fprintf(stderr, " (%d,%d,l%u@%u)", trail[t].run, trail[t].level,
                                         trail[t].len, trail[t].bp);
                        std::fprintf(stderr, "\n");
                    }
                    fatal = true;
                    return false;
                }
                const uint32_t pos = scan[index];
                ++index;

                const int32_t w = intra ? static_cast<int32_t>(g_iqIntra[pos])
                                        : static_cast<int32_t>(g_iqNonIntra[pos]);
                int32_t v;
                if (intra)
                    v = (2 * level * qscale * w) / divisor;
                else
                    v = ((2 * level + sgn(level)) * qscale * w) / divisor;

                if (mp1)
                {
                    if ((v & 1) == 0)
                        v -= sgn(v);
                }
                v = clampi(v, -2048, 2047);
                coeff[pos] = v;
            }

            if (!mp1)
            {
                // MPEG-2 mismatch control: force the sum of all coefficients odd.
                for (int i = 0; i < 64; ++i)
                    sum += coeff[i];
                if ((sum & 1) == 0)
                    coeff[63] ^= 1;
            }

            if (g_hist)
            {
                for (int t = 0; t < trailN; ++t)
                    histNote(trail[t].run, int(trail[t].len));
            }

            return true;
        }

        // ------------------------------------------------------------------
        // Command state
        // ------------------------------------------------------------------
        struct Command
        {
            bool active = false;
            uint32_t code = 0;
            uint32_t opt = 0;
            bool fbDone = false;
            uint32_t mbcLeft = 0;
            bool started = false;
        };

        Command g_cmd;

        void raiseEcd(const char *what)
        {
            g_ecd = true;
            ++g_stats.vlcErrors;
            if (g_stats.vlcErrors <= 24)
            {
                char bits[28];
                for (int i = 0; i < 27; ++i)
                    bits[i] = ((g_failBits >> (26 - i)) & 1u) ? '1' : '0';
                bits[27] = '\0';
                std::fprintf(stderr,
                             "[ipu:ecd] %s cmd=%u opt=0x%07X bp=%u avail=%lld mb=%llu "
                             "blk=%d idx=%d intra=%d bits=%s\n",
                             what, g_cmd.code, g_cmd.opt, g_bp,
                             static_cast<long long>(availBits()),
                             static_cast<unsigned long long>(g_stats.macroblocks),
                             g_failBlock, g_failIndex, g_failIntra ? 1 : 0, bits);
            }
            g_cmd.active = false;
        }

        // ---- SETIQ / SETVQ -------------------------------------------------
        bool doSetIq()
        {
            if (!ensureBits(512))
                return false;
            uint8_t raw[64];
            for (int i = 0; i < 64; ++i)
                raw[i] = static_cast<uint8_t>(getBits(8));
            // Matrices arrive in zigzag scan order; store them raster-indexed.
            uint8_t *dst = ((g_cmd.opt & (1u << 27)) != 0u) ? g_iqNonIntra : g_iqIntra;
            for (int i = 0; i < 64; ++i)
                dst[kZigzag[i]] = raw[i];
            return true;
        }

        bool doSetVq()
        {
            if (!ensureBits(256))
                return false;
            for (int i = 0; i < 16; ++i)
                g_vq[i] = static_cast<uint16_t>(getBits(16));
            return true;
        }

        // ---- VDEC ----------------------------------------------------------
        bool doVdec()
        {
            const uint32_t tbl = (g_cmd.opt >> 26) & 3u;
            int32_t result = 0;

            if (tbl == 0)
            {
                // macroblock_address_increment, with MPEG-1 escape/stuffing.
                int32_t total = 0;
                for (int guard = 0; guard < 64; ++guard)
                {
                    uint32_t bits = 0;
                    if (!peek27(bits))
                        return false;
                    const VlcResult r = decodeMbAddrIncrement(bits);
                    if (r.len == 0)
                    {
                        raiseEcd("vdec/mba");
                        return true;
                    }
                    skipBits(r.len);
                    if (r.value == -1) // macroblock_escape
                    {
                        total += 33;
                        continue;
                    }
                    if (r.value == -2) // macroblock_stuffing
                        continue;
                    total += r.value;
                    break;
                }
                result = total;
            }
            else
            {
                uint32_t bits = 0;
                if (!peek27(bits))
                    return false;
                VlcResult r{0, 0};
                if (tbl == 1)
                    r = decodeMbType(ctrlPictureType(), bits);
                else if (tbl == 2)
                    r = decodeMotionCode(bits);
                else
                    r = decodeDmVector(bits);
                if (r.len == 0)
                {
                    raiseEcd(tbl == 1 ? "vdec/mbtype" : (tbl == 2 ? "vdec/motion" : "vdec/dmv"));
                    return true;
                }
                skipBits(r.len);
                result = r.value;
            }

            // The guest reads this back as a sign-extended 16-bit field.
            g_cmdResult = static_cast<uint32_t>(result) & 0xFFFFu;
            g_haveCmdResult = true;
            ++g_stats.vdec;
            return true;
        }

        // ---- FDEC ----------------------------------------------------------
        bool doFdec()
        {
            if (!ensureBits(32))
                return false;
            g_cmdResult = getBits(32);
            g_haveCmdResult = true;
            ++g_stats.fdec;
            return true;
        }

        // ---- BDEC ----------------------------------------------------------
        bool doBdec()
        {
            const uint32_t qsc = (g_cmd.opt >> 16) & 0x1Fu;
            const bool dt = (g_cmd.opt & (1u << 25)) != 0u;
            const bool dcr = (g_cmd.opt & (1u << 26)) != 0u;
            const bool intra = (g_cmd.opt & (1u << 27)) != 0u;

            if (!g_cmd.started)
            {
                if (dcr)
                {
                    const int32_t resetVal = 128 << (ctrlMp1() ? 0u : ctrlIdp());
                    g_dcPred[0] = g_dcPred[1] = g_dcPred[2] = resetVal;
                }
                g_cmd.started = true;
            }

            // A macroblock is all-or-nothing: if the FIFO runs dry partway
            // through, the retry re-decodes from the same bit position, so the
            // intra DC predictors must not carry over the abandoned attempt.
            const int32_t dcSave[3] = {g_dcPred[0], g_dcPred[1], g_dcPred[2]};
            struct DcRestore
            {
                const int32_t *save;
                bool armed = true;
                ~DcRestore()
                {
                    if (armed)
                    {
                        g_dcPred[0] = save[0];
                        g_dcPred[1] = save[1];
                        g_dcPred[2] = save[2];
                    }
                }
            } dcRestore{dcSave};

            uint32_t cbp = 0x3Fu;
            if (!intra)
            {
                uint32_t bits = 0;
                if (!peek27(bits))
                    return false;
                const VlcResult r = decodeCbp(bits);
                if (r.len == 0)
                {
                    raiseEcd("bdec/cbp");
                    dcRestore.armed = false;
                    return true;
                }
                skipBits(r.len);
                cbp = static_cast<uint32_t>(r.value) & 0x3Fu;
            }

            int32_t blocks[6][64];
            for (int b = 0; b < 6; ++b)
            {
                if ((cbp & (1u << (5 - b))) == 0u)
                {
                    std::memset(blocks[b], 0, sizeof(blocks[b]));
                    continue;
                }
                int32_t coeff[64];
                bool fatal = false;
                if (!decodeBlock(b, intra, qsc, coeff, fatal))
                {
                    if (fatal)
                    {
                        raiseEcd("bdec/block");
                        dcRestore.armed = false;
                        return true;
                    }
                    return false; // starved -- retry the whole macroblock
                }
                idct8x8(coeff, blocks[b]);
            }

            g_cbp = cbp;

            // Output layout: int16 Y[16][16], Cb[8][8], Cr[8][8] == 768 bytes,
            // which is exactly the size the guest programs into IPU_FROM
            // (0x424710: `addiu $a1, $zero, 0x300`).
            int16_t mb[384];
            const int32_t lo = intra ? 0 : -256;
            const int32_t hi = intra ? 255 : 255;

            for (int b = 0; b < 4; ++b)
            {
                const int colBase = (b & 1) ? 8 : 0;
                for (int y = 0; y < 8; ++y)
                {
                    int row;
                    if (dt)
                        row = (y * 2) + ((b >> 1) & 1); // field DCT: even/odd lines
                    else
                        row = ((b >> 1) & 1) * 8 + y;
                    for (int x = 0; x < 8; ++x)
                        mb[row * 16 + colBase + x] =
                            static_cast<int16_t>(clampi(blocks[b][y * 8 + x], lo, hi));
                }
            }
            for (int i = 0; i < 64; ++i)
            {
                mb[256 + i] = static_cast<int16_t>(clampi(blocks[4][i], lo, hi));
                mb[320 + i] = static_cast<int16_t>(clampi(blocks[5][i], lo, hi));
            }

            pushOut(reinterpret_cast<const uint8_t *>(mb), sizeof(mb));
            ++g_stats.bdec;
            ++g_stats.macroblocks;
            dcRestore.armed = false;
            return true;
        }

        // ---- CSC / PACK ----------------------------------------------------
        // The IPU's colour-space matrix in 6-bit fixed point: every coefficient
        // below divided by 64 gives the value in the comment, so the combine
        // step shifts by kCscShift, NOT by 8 or 7. Getting that wrong scales
        // the whole picture (a full-white macroblock came out mid-grey).
        constexpr int32_t kCscShift = 6;
        constexpr int32_t kCscRound = 1 << (kCscShift - 1);
        constexpr int32_t kYCoeff = 76;   //  1.1875
        constexpr int32_t kRCr = 90;      //  1.40625
        constexpr int32_t kGCr = -102;    // -1.59375
        constexpr int32_t kGCb = -25;     // -0.390625
        constexpr int32_t kBCb = 113;     //  1.765625
        constexpr int32_t kYBias = 16;

        inline uint8_t clamp8(int32_t v)
        {
            return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }

        void yuvMacroblockToRgb32(const uint8_t *mb8, uint8_t *rgb32, bool sgnBias)
        {
            const uint8_t *Y = mb8;
            const uint8_t *Cb = mb8 + 256;
            const uint8_t *Cr = mb8 + 320;

            for (int y = 0; y < 16; ++y)
            {
                for (int x = 0; x < 16; ++x)
                {
                    const int ci = (y >> 1) * 8 + (x >> 1);
                    const int32_t cb = static_cast<int32_t>(Cb[ci]) - 128;
                    const int32_t cr = static_cast<int32_t>(Cr[ci]) - 128;
                    const int32_t lum = kYCoeff * std::max(0, static_cast<int32_t>(Y[y * 16 + x]) - kYBias);

                    int32_t r = (lum + kRCr * cr + kCscRound) >> kCscShift;
                    int32_t g = (lum + kGCr * cr + kGCb * cb + kCscRound) >> kCscShift;
                    int32_t b = (lum + kBCb * cb + kCscRound) >> kCscShift;

                    uint8_t r8 = clamp8(r);
                    uint8_t g8 = clamp8(g);
                    uint8_t b8 = clamp8(b);
                    uint8_t a8 = 0x80;

                    if (g_th0 > 0 && r8 < g_th0 && g8 < g_th0 && b8 < g_th0)
                    {
                        r8 = g8 = b8 = 0;
                        a8 = 0;
                    }
                    else if (g_th1 > 0 && r8 < g_th1 && g8 < g_th1 && b8 < g_th1)
                    {
                        a8 = 0x40;
                    }

                    if (sgnBias)
                    {
                        r8 = static_cast<uint8_t>(r8 - 128);
                        g8 = static_cast<uint8_t>(g8 - 128);
                        b8 = static_cast<uint8_t>(b8 - 128);
                    }

                    uint8_t *px = rgb32 + (y * 16 + x) * 4;
                    px[0] = r8;
                    px[1] = g8;
                    px[2] = b8;
                    px[3] = a8;
                }
            }
        }

        void rgb32ToRgb16(const uint8_t *rgb32, uint8_t *rgb16, bool dither)
        {
            for (int i = 0; i < 256; ++i)
            {
                const uint8_t *px = rgb32 + i * 4;
                int32_t r = px[0];
                int32_t g = px[1];
                int32_t b = px[2];
                if (dither)
                {
                    // 2x2 ordered dither, matching the IPU's documented use of
                    // DTE for RGB16 output.
                    static const int32_t kDither[4] = {-4, 0, 3, -1};
                    const int32_t d = kDither[((i >> 4) & 1) * 2 + (i & 1)];
                    r = clampi(r + d, 0, 255);
                    g = clampi(g + d, 0, 255);
                    b = clampi(b + d, 0, 255);
                }
                const uint16_t v = static_cast<uint16_t>(((r >> 3) & 0x1F) |
                                                         (((g >> 3) & 0x1F) << 5) |
                                                         (((b >> 3) & 0x1F) << 10) |
                                                         ((px[3] >= 0x40 ? 1u : 0u) << 15));
                std::memcpy(rgb16 + i * 2, &v, sizeof(v));
            }
        }

        bool doCsc()
        {
            const bool ofm = (g_cmd.opt & (1u << 27)) != 0u;
            const bool dte = (g_cmd.opt & (1u << 26)) != 0u;

            while (g_cmd.mbcLeft > 0)
            {
                if (!ensureBits(384u * 8u))
                    return false;

                uint8_t mb8[384];
                for (int i = 0; i < 384; ++i)
                    mb8[i] = static_cast<uint8_t>(getBits(8));

                uint8_t rgb32[1024];
                yuvMacroblockToRgb32(mb8, rgb32, false);

                if (ofm)
                {
                    uint8_t rgb16[512];
                    rgb32ToRgb16(rgb32, rgb16, dte);
                    pushOut(rgb16, sizeof(rgb16));
                }
                else
                {
                    pushOut(rgb32, sizeof(rgb32));
                }

                --g_cmd.mbcLeft;
                ++g_stats.macroblocks;
                commitProgress();
            }
            ++g_stats.csc;
            return true;
        }

        bool doPack()
        {
            const bool ofm = (g_cmd.opt & (1u << 27)) != 0u;
            const bool dte = (g_cmd.opt & (1u << 26)) != 0u;

            while (g_cmd.mbcLeft > 0)
            {
                if (!ensureBits(1024u * 8u))
                    return false;

                uint8_t rgb32[1024];
                for (int i = 0; i < 1024; ++i)
                    rgb32[i] = static_cast<uint8_t>(getBits(8));

                if (ofm)
                {
                    uint8_t rgb16[512];
                    rgb32ToRgb16(rgb32, rgb16, dte);
                    pushOut(rgb16, sizeof(rgb16));
                }
                else
                {
                    // INDX4: nearest entry in the 16-colour VQ CLUT, two
                    // pixels per byte.
                    uint8_t idx[128];
                    std::memset(idx, 0, sizeof(idx));
                    for (int i = 0; i < 256; ++i)
                    {
                        const uint8_t *px = rgb32 + i * 4;
                        int best = 0;
                        int32_t bestD = 0x7FFFFFFF;
                        for (int c = 0; c < 16; ++c)
                        {
                            const uint16_t e = g_vq[c];
                            const int32_t dr = static_cast<int32_t>(px[0] >> 3) - (e & 0x1F);
                            const int32_t dg = static_cast<int32_t>(px[1] >> 3) - ((e >> 5) & 0x1F);
                            const int32_t db = static_cast<int32_t>(px[2] >> 3) - ((e >> 10) & 0x1F);
                            const int32_t d = dr * dr + dg * dg + db * db;
                            if (d < bestD)
                            {
                                bestD = d;
                                best = c;
                            }
                        }
                        idx[i >> 1] |= static_cast<uint8_t>(best << ((i & 1) ? 4 : 0));
                    }
                    pushOut(idx, sizeof(idx));
                }

                --g_cmd.mbcLeft;
                commitProgress();
            }
            ++g_stats.pack;
            return true;
        }

        // ------------------------------------------------------------------
        // Command dispatch
        // ------------------------------------------------------------------
        // Returns true when the active command finished (or aborted).
        bool stepCommand()
        {
            if (!g_cmd.active)
                return true;

            const uint32_t entryBp = g_bp;

            if (!g_cmd.fbDone)
            {
                // IDEC/BDEC/VDEC/FDEC/SETIQ begin with an FB bitstream skip in
                // option bits 0-5. If even that cannot be satisfied, rewind to
                // the entry position so the retry still performs the skip.
                const uint32_t code = g_cmd.code;
                if (code == 1u || code == 2u || code == 3u || code == 4u || code == 5u)
                {
                    const uint32_t fb = g_cmd.opt & 0x3Fu;
                    if (fb != 0)
                    {
                        if (!ensureBits(fb))
                        {
                            g_bp = entryBp;
                            ++g_stats.stalls;
                            return false;
                        }
                        skipBits(fb);
                    }
                }
                g_cmd.fbDone = true;
            }

            // Checkpoint sits AFTER the FB skip, which is consumed exactly once.
            g_rollbackBp = g_bp;

            bool done = false;
            switch (g_cmd.code)
            {
            case 0x0: // BCLR
                g_in.clear();
                g_bp = g_cmd.opt & 0x7Fu;
                g_haveCmdResult = false;
                done = true;
                break;

            case 0x1: // IDEC -- slice decode
                // Not implemented. No SDBZ code path issues it (a static scan
                // of every IPU_CMD store in the ELF finds only BCLR, BDEC,
                // VDEC, FDEC, SETIQ, SETVQ, SETTH and CSC), and a half-built
                // slice decoder would corrupt silently rather than fail.
                if (g_stats.idec == 0)
                    std::fprintf(stderr, "[ipu:idec] IDEC issued (opt=0x%07X) but not implemented\n",
                                 g_cmd.opt);
                ++g_stats.idec;
                g_ecd = true;
                done = true;
                break;

            case 0x2: // BDEC
                done = doBdec();
                break;

            case 0x3: // VDEC
                done = doVdec();
                break;

            case 0x4: // FDEC
                done = doFdec();
                break;

            case 0x5: // SETIQ
                done = doSetIq();
                break;

            case 0x6: // SETVQ
                done = doSetVq();
                break;

            case 0x7: // CSC
                done = doCsc();
                break;

            case 0x8: // PACK
                done = doPack();
                break;

            case 0x9: // SETTH
                g_th0 = g_cmd.opt & 0x1FFu;
                g_th1 = (g_cmd.opt >> 16) & 0x1FFu;
                done = true;
                break;

            default:
                g_ecd = true;
                done = true;
                break;
            }

            if (!done && g_cmd.active)
            {
                // Starved: rewind to the last durable checkpoint so the retry
                // sees an identical bitstream once more data lands.
                g_bp = g_rollbackBp;
                ++g_stats.stalls;
                return false;
            }

            g_cmd.active = false;
            commitProgress();
            return true;
        }

        void serviceFrom()
        {
            if (!g_from.running)
                return;

            while (g_from.qwc > 0 && outAvail() >= 16)
            {
                uint8_t *dst = dmaPtr(g_from.madr, 16);
                if (!dst)
                {
                    g_from.running = false;
                    g_from.chcr &= ~0x100u;
                    g_fromCompleted = true;
                    return;
                }
                std::memcpy(dst, g_out.data() + g_outRead, 16);
                g_outRead += 16;
                g_from.madr += 16;
                --g_from.qwc;
            }
            compactOut();

            if (g_from.qwc == 0)
            {
                g_from.running = false;
                g_from.chcr &= ~0x100u;
                g_fromCompleted = true;
            }
        }

        // IPU_CTRL.RST. Clears the FIFOs, the bit pointer, the in-flight
        // command and the CTRL configuration -- but deliberately PRESERVES the
        // quantiser matrices, the VQ CLUT and the alpha thresholds.
        //
        // The guest resets mid-playback (0x427928, 0x429c54, 0x429ca0) as an
        // "abort the current command" idiom and only reloads SETIQ/SETVQ/SETTH
        // at init (0x42a2a0..0x42a364). Wiping the tables on every reset would
        // corrupt the next frame; keeping them costs nothing at init because
        // the guest overwrites them there anyway.
        void softReset()
        {
            g_ctrlCfg = 0;
            g_cbp = 0;
            g_ecd = false;
            g_scd = false;
            g_cmdResult = 0;
            g_haveCmdResult = false;
            g_dcPred[0] = g_dcPred[1] = g_dcPred[2] = 128;
            g_in.clear();
            g_bp = 0;
            g_rollbackBp = 0;
            g_out.clear();
            g_outRead = 0;
            g_cmd = Command{};
        }

        void resetState()
        {
            softReset();
            g_th0 = 0;
            g_th1 = 0;
            std::memcpy(g_iqIntra, kDefaultIntra, sizeof(g_iqIntra));
            std::memset(g_iqNonIntra, 16, sizeof(g_iqNonIntra));
            std::memset(g_vq, 0, sizeof(g_vq));
        }
    }

    // ----------------------------------------------------------------------
    // Public interface
    // ----------------------------------------------------------------------
    void attachMemory(uint8_t *rdram, uint32_t ramSize, uint8_t *spr, uint32_t sprSize)
    {
        g_rdram = rdram;
        g_ramSize = ramSize;
        g_spr = spr;
        g_sprSize = sprSize;
        traceInit();
        buildTables();
        idctInit();
        resetState();
    }

    void reset()
    {
        resetState();
        g_to = Channel{};
        g_from = Channel{};
        g_toCompleted = false;
        g_fromCompleted = false;
    }

    const Stats &stats() { return g_stats; }

    void logSummary()
    {
        std::fprintf(stderr,
                     "[ipu] cmds=%llu bdec=%llu vdec=%llu fdec=%llu csc=%llu pack=%llu "
                     "mb=%llu in=%llu out=%llu ecd=%llu stalls=%llu\n",
                     (unsigned long long)g_stats.commands, (unsigned long long)g_stats.bdec,
                     (unsigned long long)g_stats.vdec, (unsigned long long)g_stats.fdec,
                     (unsigned long long)g_stats.csc, (unsigned long long)g_stats.pack,
                     (unsigned long long)g_stats.macroblocks, (unsigned long long)g_stats.bytesIn,
                     (unsigned long long)g_stats.bytesOut, (unsigned long long)g_stats.vlcErrors,
                     (unsigned long long)g_stats.stalls);
        if (g_hist)
        {
            std::fprintf(stderr, "[ipu:hist] runs:");
            for (int i = 0; i < 64; ++i)
                if (g_histRun[i])
                    std::fprintf(stderr, " %d:%llu", i, g_histRun[i]);
            std::fprintf(stderr, "\n[ipu:hist] lens:");
            for (int i = 0; i < 24; ++i)
                if (g_histLen[i])
                    std::fprintf(stderr, " %d:%llu", i, g_histLen[i]);
            std::fprintf(stderr, "\n");
        }
    }

    bool isRegister(uint32_t address)
    {
        return address >= 0x10002000u && address <= 0x10002034u;
    }

    bool isFifo(uint32_t address)
    {
        return address == 0x10007000u || address == 0x10007010u;
    }

    bool isDmaChannel(uint32_t channelBase)
    {
        return channelBase == kChanFrom || channelBase == kChanTo;
    }

    void run()
    {
        // The IPU_TO descriptor fills the input FIFO on its own, not only when
        // a command asks for bits. IPU_TOP, IPU_BP and the IPU_CMD "first 32
        // bits of the bitstream" quirk are all read BETWEEN commands -- the
        // guest's driver peeks IPU_TOP to parse fixed-length fields and folds
        // what it used into the next command's FB field -- so the FIFO has to
        // be primed even with no command in flight.
        fillInputFifo();
        serviceFrom();
        for (int guard = 0; guard < 4096; ++guard)
        {
            if (!g_cmd.active)
                break;
            if (!stepCommand())
                break;
        }
        serviceFrom();

        if (g_trace != 0 && g_stats.macroblocks >= g_nextSummary)
        {
            g_nextSummary = g_stats.macroblocks + 4096;
            logSummary();
        }
    }

    void writeReg(uint32_t address, uint32_t value)
    {
        traceInit();
        switch (address)
        {
        case 0x10002000u: // IPU_CMD
        {
            g_ecd = false;
            g_scd = false;
            g_cmd = Command{};
            g_cmd.active = true;
            g_cmd.code = (value >> 28) & 0xFu;
            g_cmd.opt = value & 0x0FFFFFFFu;
            if (g_cmd.code == 0x7u || g_cmd.code == 0x8u)
                g_cmd.mbcLeft = g_cmd.opt & 0x7FFu;
            ++g_stats.commands;
            if (g_traced < g_trace)
            {
                ++g_traced;
                std::fprintf(stderr, "[ipu:cmd] #%u code=%u opt=0x%07X ctrl=0x%08X bp=%u avail=%lld\n",
                             g_traced, g_cmd.code, g_cmd.opt, g_ctrlCfg, g_bp,
                             (long long)availBits());
            }
            run();
            break;
        }
        case 0x10002010u: // IPU_CTRL
            if ((value & (1u << 30)) != 0u)
            {
                softReset();
                break;
            }
            g_ctrlCfg = (g_ctrlCfg & ~kCtrlCfgMask) | (value & kCtrlCfgMask);
            break;

        case 0x10002020u: // IPU_BP is read-only
        case 0x10002030u: // IPU_TOP is read-only
        case 0x10002004u:
        case 0x10002014u:
        case 0x10002024u:
        case 0x10002034u:
        default:
            break;
        }
    }

    uint32_t readReg(uint32_t address)
    {
        traceInit();
        run();

        const bool busy = g_cmd.active;

        switch (address)
        {
        case 0x10002000u: // IPU_CMD result
            if (!g_haveCmdResult && availBits() >= 32)
            {
                // Documented quirk: with data in the FIFO but no command issued
                // yet, IPU_CMD reads back the first 32 bits of the bitstream.
                return peekBits(32);
            }
            return g_cmdResult;

        case 0x10002004u: // IPU_CMD high word -- bit 63 is busy
            return busy ? 0x80000000u : 0u;

        case 0x10002010u: // IPU_CTRL
        {
            const uint32_t ifc = static_cast<uint32_t>(std::min<size_t>(g_in.size() / 16u, 15u));
            const uint32_t ofc = static_cast<uint32_t>(std::min<size_t>(outAvail() / 16u, 15u));
            uint32_t v = (ifc & 0xFu) | ((ofc & 0xFu) << 4) | ((g_cbp & 0x3Fu) << 8);
            if (g_ecd)
                v |= (1u << 14);
            if (g_scd)
                v |= (1u << 15);
            v |= (g_ctrlCfg & kCtrlCfgMask);
            if (busy)
                v |= (1u << 31);
            return v;
        }

        case 0x10002020u: // IPU_BP
        {
            const uint32_t bp = g_bp & 0x7Fu;
            const uint32_t ifc = static_cast<uint32_t>(std::min<size_t>(g_in.size() / 16u, 15u));
            return bp | (ifc << 8);
        }

        case 0x10002030u: // IPU_TOP -- next 32 bits of the bitstream
        {
            const int64_t avail = availBits();
            if (avail >= 32)
                return peekBits(32);
            if (avail <= 0)
                return 0;
            return peekBits(static_cast<uint32_t>(avail)) << (32u - static_cast<uint32_t>(avail));
        }

        case 0x10002034u: // IPU_TOP high word -- bit 63 = fewer than 32 bits
            return (availBits() < 32) ? 0x80000000u : 0u;

        default:
            return 0;
        }
    }

    void writeFifo128(uint32_t address, const uint8_t *src16)
    {
        if (address != 0x10007010u)
            return;
        g_in.insert(g_in.end(), src16, src16 + 16);
        g_stats.bytesIn += 16;
        run();
    }

    void readFifo128(uint32_t address, uint8_t *dst16)
    {
        std::memset(dst16, 0, 16);
        if (address != 0x10007000u)
            return;
        run();
        if (outAvail() >= 16)
        {
            std::memcpy(dst16, g_out.data() + g_outRead, 16);
            g_outRead += 16;
            compactOut();
        }
    }

    void writeChannelReg(uint32_t address, uint32_t value)
    {
        const uint32_t base = address & 0xFFFFFF00u;
        Channel &ch = (base == kChanFrom) ? g_from : g_to;

        switch (address & 0xFFu)
        {
        case 0x10:
            ch.madr = value;
            return;
        case 0x20:
            ch.qwc = value & 0xFFFFu;
            return;
        case 0x30:
            ch.tadr = value;
            return;
        case 0x40:
            ch.asr0 = value;
            return;
        case 0x50:
            ch.asr1 = value;
            return;
        case 0x00:
            break;
        default:
            return;
        }

        const bool start = (value & 0x100u) != 0u;
        ch.chcr = value;

        if (!start)
        {
            // The guest aborts a streaming transfer by clearing STR; MADR/QWC
            // are left where the engine got to so its resume maths stay honest.
            ch.running = false;
            return;
        }

        ch.running = true;
        ch.chainDone = false;
        if (base == kChanTo && ((value >> 2) & 3u) == 1u)
        {
            // Chain mode: the first tag has not been read yet.
            ch.qwc = 0;
        }
        run();
    }

    uint32_t peekChannelReg(uint32_t address)
    {
        const uint32_t base = address & 0xFFFFFF00u;
        const Channel &ch = (base == kChanFrom) ? g_from : g_to;

        switch (address & 0xFFu)
        {
        case 0x00:
            return ch.running ? (ch.chcr | 0x100u) : (ch.chcr & ~0x100u);
        case 0x10:
            return ch.madr;
        case 0x20:
            return ch.qwc;
        case 0x30:
            return ch.tadr;
        case 0x40:
            return ch.asr0;
        case 0x50:
            return ch.asr1;
        default:
            return 0;
        }
    }

    uint32_t readChannelReg(uint32_t address)
    {
        run();
        const uint32_t base = address & 0xFFFFFF00u;
        const Channel &ch = (base == kChanFrom) ? g_from : g_to;

        switch (address & 0xFFu)
        {
        case 0x00:
            return ch.running ? (ch.chcr | 0x100u) : (ch.chcr & ~0x100u);
        case 0x10:
            return ch.madr;
        case 0x20:
            return ch.qwc;
        case 0x30:
            return ch.tadr;
        case 0x40:
            return ch.asr0;
        case 0x50:
            return ch.asr1;
        default:
            return 0;
        }
    }

    bool takeFromCompletion()
    {
        const bool v = g_fromCompleted;
        g_fromCompleted = false;
        return v;
    }

    bool takeToCompletion()
    {
        const bool v = g_toCompleted;
        g_toCompleted = false;
        return v;
    }
}

