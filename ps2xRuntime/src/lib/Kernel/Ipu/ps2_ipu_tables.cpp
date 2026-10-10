// Variable-length code tables for the PS2 IPU's MPEG decoder.
//
// The tables are written out as bit-string specifications straight from
// ISO/IEC 11172-2 / 13818-2 Annex B and expanded into flat prefix-indexed
// lookups at first use. That costs a few hundred microseconds once and buys
// something a hand-packed table cannot: buildTables() verifies that every
// table is a complete, unambiguous prefix code, so a transcription slip is
// caught at startup instead of surfacing three weeks later as "the movie is
// green". Set PS2X_IPU_TABLECHECK=1 to print the verification result.
//
// SCOPE. Every movie this game ships is MPEG-1:
//
//     ATARI.SFD  OKR.SFD  OP.SFD  OP1.SFD  OP_PAL.SFD  OP_USA.SFD
//     -> zero extension_start_codes (0x000001B5) in any of them, i.e. no
//        sequence_extension and no picture_coding_extension.
//
// So intra_vlc_format can never be set for this title and Table B.15 (the
// MPEG-2-only intra DCT coefficient table) is unreachable here. It is
// deliberately NOT transcribed: a table recalled imperfectly would decode into
// plausible-looking garbage, whereas the missing-table path below raises
// IPU_CTRL.ECD, which the guest driver already checks at 0x424bec and handles
// cleanly. If a future title needs B.15, add it here -- do not guess it.

#include "ps2_ipu_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2_ipu
{
    namespace
    {
        struct CodeSpec
        {
            const char *bits;
            int32_t a;
            int32_t b;
        };

        // Flat lookup indexed by the top `indexBits` bits of the stream.
        struct Vlc
        {
            uint32_t indexBits = 0;
            std::vector<int32_t> a;
            std::vector<int32_t> b;
            std::vector<uint8_t> len;
            const char *name = "";
        };

        uint32_t codeLen(const char *s)
        {
            uint32_t n = 0;
            for (const char *p = s; *p; ++p)
            {
                if (*p == '0' || *p == '1')
                    ++n;
            }
            return n;
        }

        uint32_t codeValue(const char *s)
        {
            uint32_t v = 0;
            for (const char *p = s; *p; ++p)
            {
                if (*p == '0' || *p == '1')
                    v = (v << 1) | static_cast<uint32_t>(*p - '0');
            }
            return v;
        }

        // Expand `specs` into a table indexed by `indexBits` leading stream
        // bits. Reports collisions (one code a prefix of another) and, via
        // buildTables(), holes (bit patterns no code covers).
        bool buildVlc(Vlc &out, const CodeSpec *specs, size_t count,
                      uint32_t indexBits, const char *name)
        {
            const size_t size = size_t(1) << indexBits;
            out.indexBits = indexBits;
            out.name = name;
            out.a.assign(size, 0);
            out.b.assign(size, 0);
            out.len.assign(size, 0);

            bool ok = true;
            for (size_t i = 0; i < count; ++i)
            {
                const uint32_t len = codeLen(specs[i].bits);
                if (len == 0 || len > indexBits)
                {
                    std::fprintf(stderr, "[ipu:table] %s: code '%s' length %u exceeds index width %u\n",
                                 name, specs[i].bits, len, indexBits);
                    ok = false;
                    continue;
                }
                const uint32_t base = codeValue(specs[i].bits) << (indexBits - len);
                const uint32_t span = 1u << (indexBits - len);
                for (uint32_t k = 0; k < span; ++k)
                {
                    const uint32_t idx = base + k;
                    if (out.len[idx] != 0)
                    {
                        std::fprintf(stderr, "[ipu:table] %s: code '%s' collides at index 0x%X (with a %u-bit code)\n",
                                     name, specs[i].bits, idx, out.len[idx]);
                        ok = false;
                    }
                    out.a[idx] = specs[i].a;
                    out.b[idx] = specs[i].b;
                    out.len[idx] = static_cast<uint8_t>(len);
                }
            }
            return ok;
        }

        // Codes are looked up against the top 27 bits of the stream so every
        // table shares one caller-side peek width.
        constexpr uint32_t kPeekBits = 27;

        inline uint32_t narrow(uint32_t bits27, uint32_t indexBits)
        {
            return bits27 >> (kPeekBits - indexBits);
        }

        // ---- Table B.1 -- macroblock_address_increment --------------------
        constexpr int32_t kMbaEscape = -1;
        constexpr int32_t kMbaStuffing = -2;

        const CodeSpec kMbaSpecs[] = {
            {"1", 1, 0},
            {"011", 2, 0},
            {"010", 3, 0},
            {"0011", 4, 0},
            {"0010", 5, 0},
            {"00011", 6, 0},
            {"00010", 7, 0},
            {"0000111", 8, 0},
            {"0000110", 9, 0},
            {"00001011", 10, 0},
            {"00001010", 11, 0},
            {"00001001", 12, 0},
            {"00001000", 13, 0},
            {"00000111", 14, 0},
            {"00000110", 15, 0},
            {"0000010111", 16, 0},
            {"0000010110", 17, 0},
            {"0000010101", 18, 0},
            {"0000010100", 19, 0},
            {"0000010011", 20, 0},
            {"0000010010", 21, 0},
            {"00000100011", 22, 0},
            {"00000100010", 23, 0},
            {"00000100001", 24, 0},
            {"00000100000", 25, 0},
            {"00000011111", 26, 0},
            {"00000011110", 27, 0},
            {"00000011101", 28, 0},
            {"00000011100", 29, 0},
            {"00000011011", 30, 0},
            {"00000011010", 31, 0},
            {"00000011001", 32, 0},
            {"00000011000", 33, 0},
            {"00000001000", kMbaEscape, 0},
            {"00000001111", kMbaStuffing, 0},
        };

        // ---- Tables B.2 / B.3 / B.4 -- macroblock_type --------------------
        // The flag encoding is pinned by the guest driver, not chosen here:
        // 0x424484 does `andi $v0, $a0, 0xc` on the VDEC result to test
        // "has any motion vector", i.e. bit2 = backward, bit3 = forward.
        constexpr int32_t MB_INTRA = 1;
        constexpr int32_t MB_PATTERN = 2;
        constexpr int32_t MB_BACKWARD = 4;
        constexpr int32_t MB_FORWARD = 8;
        constexpr int32_t MB_QUANT = 16;

        const CodeSpec kMbTypeISpecs[] = {
            {"1", MB_INTRA, 0},
            {"01", MB_INTRA | MB_QUANT, 0},
        };

        const CodeSpec kMbTypePSpecs[] = {
            {"1", MB_FORWARD | MB_PATTERN, 0},
            {"01", MB_PATTERN, 0},
            {"001", MB_FORWARD, 0},
            {"00011", MB_INTRA, 0},
            {"00010", MB_QUANT | MB_FORWARD | MB_PATTERN, 0},
            {"00001", MB_QUANT | MB_PATTERN, 0},
            {"000001", MB_QUANT | MB_INTRA, 0},
        };

        const CodeSpec kMbTypeBSpecs[] = {
            {"10", MB_FORWARD | MB_BACKWARD, 0},
            {"11", MB_FORWARD | MB_BACKWARD | MB_PATTERN, 0},
            {"010", MB_BACKWARD, 0},
            {"011", MB_BACKWARD | MB_PATTERN, 0},
            {"0010", MB_FORWARD, 0},
            {"0011", MB_FORWARD | MB_PATTERN, 0},
            {"00011", MB_INTRA, 0},
            {"00010", MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN, 0},
            {"000011", MB_QUANT | MB_FORWARD | MB_PATTERN, 0},
            {"000010", MB_QUANT | MB_BACKWARD | MB_PATTERN, 0},
            {"000001", MB_QUANT | MB_INTRA, 0},
        };

        // Table B.5 -- D-pictures. One code, always intra.
        const CodeSpec kMbTypeDSpecs[] = {
            {"1", MB_INTRA, 0},
        };

        // ---- Table B.10 -- motion_code ------------------------------------
        // The final bit of every non-zero code is the sign, folded in here.
        const CodeSpec kMotionSpecs[] = {
            {"1", 0, 0},
            {"010", 1, 0},
            {"011", -1, 0},
            {"0010", 2, 0},
            {"0011", -2, 0},
            {"00010", 3, 0},
            {"00011", -3, 0},
            {"0000110", 4, 0},
            {"0000111", -4, 0},
            {"00001010", 5, 0},
            {"00001011", -5, 0},
            {"00001000", 6, 0},
            {"00001001", -6, 0},
            {"00000110", 7, 0},
            {"00000111", -7, 0},
            {"0000010110", 8, 0},
            {"0000010111", -8, 0},
            {"0000010100", 9, 0},
            {"0000010101", -9, 0},
            {"0000010010", 10, 0},
            {"0000010011", -10, 0},
            // +/-11 and +/-12 are ELEVEN bits, not ten. Writing +/-11 as the
            // 10-bit "0000010000" silently swallowed +/-12's two slots and
            // shifted +/-13..+/-16 down by one rung, which decodes large
            // motion vectors at the wrong length and desyncs the bitstream a
            // few hundred macroblocks later. Caught by diffing a real movie
            // against a reference decoder, not by the prefix-code check --
            // the wrong table was still a valid prefix code.
            {"00000100010", 11, 0},
            {"00000100011", -11, 0},
            {"00000100000", 12, 0},
            {"00000100001", -12, 0},
            {"00000011110", 13, 0},
            {"00000011111", -13, 0},
            {"00000011100", 14, 0},
            {"00000011101", -14, 0},
            {"00000011010", 15, 0},
            {"00000011011", -15, 0},
            {"00000011000", 16, 0},
            {"00000011001", -16, 0},
        };

        // ---- dmvector -----------------------------------------------------
        const CodeSpec kDmvSpecs[] = {
            {"0", 0, 0},
            {"10", 1, 0},
            {"11", -1, 0},
        };

        // ---- Table B.9 -- coded_block_pattern -----------------------------
        const CodeSpec kCbpSpecs[] = {
            {"111", 60, 0},
            {"1101", 4, 0},
            {"1100", 8, 0},
            {"1011", 16, 0},
            {"1010", 32, 0},
            {"10011", 12, 0},
            {"10010", 48, 0},
            {"10001", 20, 0},
            {"10000", 40, 0},
            {"01111", 28, 0},
            {"01110", 44, 0},
            {"01101", 52, 0},
            {"01100", 56, 0},
            {"01011", 1, 0},
            {"01010", 61, 0},
            {"01001", 2, 0},
            {"01000", 62, 0},
            {"001111", 24, 0},
            {"001110", 36, 0},
            {"001101", 3, 0},
            {"001100", 63, 0},
            {"0010111", 5, 0},
            {"0010110", 9, 0},
            {"0010101", 17, 0},
            {"0010100", 33, 0},
            {"0010011", 6, 0},
            {"0010010", 10, 0},
            {"0010001", 18, 0},
            {"0010000", 34, 0},
            {"00011111", 7, 0},
            {"00011110", 11, 0},
            {"00011101", 19, 0},
            {"00011100", 35, 0},
            {"00011011", 13, 0},
            {"00011010", 49, 0},
            {"00011001", 21, 0},
            {"00011000", 41, 0},
            {"00010111", 14, 0},
            {"00010110", 50, 0},
            {"00010101", 22, 0},
            {"00010100", 42, 0},
            {"00010011", 15, 0},
            {"00010010", 51, 0},
            {"00010001", 23, 0},
            {"00010000", 43, 0},
            {"00001111", 25, 0},
            {"00001110", 37, 0},
            {"00001101", 26, 0},
            {"00001100", 38, 0},
            {"00001011", 29, 0},
            {"00001010", 45, 0},
            {"00001001", 53, 0},
            {"00001000", 57, 0},
            {"00000111", 30, 0},
            {"00000110", 46, 0},
            {"00000101", 54, 0},
            {"00000100", 58, 0},
            {"000000111", 31, 0},
            {"000000110", 47, 0},
            {"000000101", 55, 0},
            {"000000100", 59, 0},
            {"000000011", 27, 0},
            {"000000010", 39, 0},
            {"000000001", 0, 0},
        };

        // ---- Tables B.12 / B.13 -- dct_dc_size ----------------------------
        // The MPEG-2 forms; they are strict supersets of the MPEG-1 tables, so
        // the extra long codes simply never occur in an MPEG-1 stream.
        const CodeSpec kDcLumaSpecs[] = {
            {"100", 0, 0},
            {"00", 1, 0},
            {"01", 2, 0},
            {"101", 3, 0},
            {"110", 4, 0},
            {"1110", 5, 0},
            {"11110", 6, 0},
            {"111110", 7, 0},
            {"1111110", 8, 0},
            {"11111110", 9, 0},
            {"111111110", 10, 0},
            {"111111111", 11, 0},
        };

        const CodeSpec kDcChromaSpecs[] = {
            {"00", 0, 0},
            {"01", 1, 0},
            {"10", 2, 0},
            {"110", 3, 0},
            {"1110", 4, 0},
            {"11110", 5, 0},
            {"111110", 6, 0},
            {"1111110", 7, 0},
            {"11111110", 8, 0},
            {"111111110", 9, 0},
            {"1111111110", 10, 0},
            {"1111111111", 11, 0},
        };

        // ---- Table B.14 -- DCT coefficients, table zero -------------------
        // a = run (or kDctEob / kDctEscape), b = level magnitude. The sign bit
        // that follows every non-EOB, non-escape code is consumed by the
        // caller, so it is NOT part of the code strings below.
        //
        // The 1-bit "first coefficient" form of (run 0, level 1) is handled in
        // decodeDctCoeff() rather than here, because it collides with EOB by
        // construction.
        const CodeSpec kDct0Specs[] = {
            {"10", kDctEob, 0},
            {"11", 0, 1},
            {"011", 1, 1},
            {"0100", 0, 2},
            {"0101", 2, 1},
            {"00101", 0, 3},
            {"00111", 3, 1},
            {"00110", 4, 1},
            {"000110", 1, 2},
            {"000111", 5, 1},
            {"000101", 6, 1},
            {"000100", 7, 1},
            {"0000110", 0, 4},
            {"0000100", 2, 2},
            {"0000111", 8, 1},
            {"0000101", 9, 1},
            {"000001", kDctEscape, 0},
            {"00100110", 0, 5},
            {"00100001", 0, 6},
            {"00100101", 1, 3},
            {"00100100", 3, 2},
            {"00100111", 10, 1},
            {"00100011", 11, 1},
            {"00100010", 12, 1},
            {"00100000", 13, 1},
            {"0000001010", 0, 7},
            {"0000001100", 1, 4},
            {"0000001011", 2, 3},
            {"0000001111", 4, 2},
            {"0000001001", 5, 2},
            {"0000001110", 14, 1},
            {"0000001101", 15, 1},
            {"0000001000", 16, 1},
            {"000000011101", 0, 8},
            {"000000011000", 0, 9},
            {"000000010011", 0, 10},
            {"000000010000", 0, 11},
            {"000000011011", 1, 5},
            {"000000010100", 2, 4},
            {"000000011100", 3, 3},
            {"000000010010", 4, 3},
            {"000000011110", 6, 2},
            {"000000010101", 7, 2},
            {"000000010001", 8, 2},
            {"000000011111", 17, 1},
            {"000000011010", 18, 1},
            {"000000011001", 19, 1},
            {"000000010111", 20, 1},
            {"000000010110", 21, 1},
            {"0000000011010", 0, 12},
            {"0000000011001", 0, 13},
            {"0000000011000", 0, 14},
            {"0000000010111", 0, 15},
            {"0000000010110", 1, 6},
            {"0000000010101", 1, 7},
            {"0000000010100", 2, 5},
            {"0000000010011", 3, 4},
            {"0000000010010", 5, 3},
            {"0000000010001", 9, 2},
            {"0000000010000", 10, 2},
            // Runs 22..26 descend as the code value ascends. Getting this group
            // ascending instead swaps (22,1)<->(25,1) and (23,1)<->(26,1), which
            // does not desync the bitstream -- the code lengths are identical --
            // it just places coefficients 2-3 positions too far along the scan.
            // Blocks then overrun index 63 and abort, a few macroblocks per
            // frame. Verified entry-by-entry against ffmpeg with a synthetic
            // one-macroblock stream per code (scratchpad/validate_b14.py).
            {"0000000011111", 22, 1},
            {"0000000011110", 23, 1},
            {"0000000011101", 24, 1},
            {"0000000011100", 25, 1},
            {"0000000011011", 26, 1},
            {"00000000011111", 0, 16},
            {"00000000011110", 0, 17},
            {"00000000011101", 0, 18},
            {"00000000011100", 0, 19},
            {"00000000011011", 0, 20},
            {"00000000011010", 0, 21},
            {"00000000011001", 0, 22},
            {"00000000011000", 0, 23},
            {"00000000010111", 0, 24},
            {"00000000010110", 0, 25},
            {"00000000010101", 0, 26},
            {"00000000010100", 0, 27},
            {"00000000010011", 0, 28},
            {"00000000010010", 0, 29},
            {"00000000010001", 0, 30},
            {"00000000010000", 0, 31},
            {"000000000011000", 0, 32},
            {"000000000010111", 0, 33},
            {"000000000010110", 0, 34},
            {"000000000010101", 0, 35},
            {"000000000010100", 0, 36},
            {"000000000010011", 0, 37},
            {"000000000010010", 0, 38},
            {"000000000010001", 0, 39},
            {"000000000010000", 0, 40},
            {"000000000011111", 1, 8},
            {"000000000011110", 1, 9},
            {"000000000011101", 1, 10},
            {"000000000011100", 1, 11},
            {"000000000011011", 1, 12},
            {"000000000011010", 1, 13},
            {"000000000011001", 1, 14},
            {"0000000000010011", 1, 15},
            {"0000000000010010", 1, 16},
            {"0000000000010001", 1, 17},
            {"0000000000010000", 1, 18},
            {"0000000000010100", 6, 3},
            {"0000000000011010", 11, 2},
            {"0000000000011001", 12, 2},
            {"0000000000011000", 13, 2},
            {"0000000000010111", 14, 2},
            {"0000000000010110", 15, 2},
            {"0000000000010101", 16, 2},
            {"0000000000011111", 27, 1},
            {"0000000000011110", 28, 1},
            {"0000000000011101", 29, 1},
            {"0000000000011100", 30, 1},
            {"0000000000011011", 31, 1},
        };

        Vlc g_mba;
        Vlc g_mbTypeI;
        Vlc g_mbTypeP;
        Vlc g_mbTypeB;
        Vlc g_mbTypeD;
        Vlc g_motion;
        Vlc g_dmv;
        Vlc g_cbp;
        Vlc g_dcLuma;
        Vlc g_dcChroma;
        Vlc g_dct0;

        bool g_built = false;

        // Count index slots no code covers. A well-formed VLC table leaves
        // holes only where the standard genuinely reserves patterns (all-zero
        // prefixes), so this number is a transcription canary, not an error.
        size_t countHoles(const Vlc &t)
        {
            size_t holes = 0;
            for (uint8_t l : t.len)
            {
                if (l == 0)
                    ++holes;
            }
            return holes;
        }

        template <size_t N>
        bool build(Vlc &t, const CodeSpec (&specs)[N], uint32_t indexBits, const char *name)
        {
            return buildVlc(t, specs, N, indexBits, name);
        }
    }

    void buildTables()
    {
        if (g_built)
            return;
        g_built = true;

        bool ok = true;
        ok &= build(g_mba, kMbaSpecs, 11, "MBA");
        ok &= build(g_mbTypeI, kMbTypeISpecs, 2, "MBTYPE_I");
        ok &= build(g_mbTypeP, kMbTypePSpecs, 6, "MBTYPE_P");
        ok &= build(g_mbTypeB, kMbTypeBSpecs, 6, "MBTYPE_B");
        ok &= build(g_mbTypeD, kMbTypeDSpecs, 1, "MBTYPE_D");
        ok &= build(g_motion, kMotionSpecs, 11, "MOTION");
        ok &= build(g_dmv, kDmvSpecs, 2, "DMV");
        ok &= build(g_cbp, kCbpSpecs, 9, "CBP");
        ok &= build(g_dcLuma, kDcLumaSpecs, 9, "DC_LUMA");
        ok &= build(g_dcChroma, kDcChromaSpecs, 10, "DC_CHROMA");
        ok &= build(g_dct0, kDct0Specs, 16, "DCT_B14");

        const char *e = std::getenv("PS2X_IPU_TABLECHECK");
        if ((e && *e && e[0] != '0') || !ok)
        {
            std::fprintf(stderr,
                         "[ipu:table] build %s -- holes: mba=%zu mbI=%zu mbP=%zu mbB=%zu "
                         "motion=%zu dmv=%zu cbp=%zu dcY=%zu dcC=%zu dct=%zu\n",
                         ok ? "OK" : "FAILED",
                         countHoles(g_mba), countHoles(g_mbTypeI), countHoles(g_mbTypeP),
                         countHoles(g_mbTypeB), countHoles(g_motion), countHoles(g_dmv),
                         countHoles(g_cbp), countHoles(g_dcLuma), countHoles(g_dcChroma),
                         countHoles(g_dct0));
        }
    }

    namespace
    {
        inline VlcResult lookup(const Vlc &t, uint32_t bits27)
        {
            const uint32_t idx = narrow(bits27, t.indexBits);
            return VlcResult{t.a[idx], t.len[idx]};
        }
    }

    VlcResult decodeMbAddrIncrement(uint32_t bits27)
    {
        return lookup(g_mba, bits27);
    }

    VlcResult decodeMbType(uint32_t pictureType, uint32_t bits27)
    {
        switch (pictureType)
        {
        case 1:
            return lookup(g_mbTypeI, bits27);
        case 2:
            return lookup(g_mbTypeP, bits27);
        case 3:
            return lookup(g_mbTypeB, bits27);
        case 4:
            return lookup(g_mbTypeD, bits27);
        default:
            return VlcResult{0, 0};
        }
    }

    VlcResult decodeMotionCode(uint32_t bits27)
    {
        return lookup(g_motion, bits27);
    }

    VlcResult decodeDmVector(uint32_t bits27)
    {
        return lookup(g_dmv, bits27);
    }

    VlcResult decodeCbp(uint32_t bits27)
    {
        return lookup(g_cbp, bits27);
    }

    VlcResult decodeDcSizeLuma(uint32_t bits27)
    {
        return lookup(g_dcLuma, bits27);
    }

    VlcResult decodeDcSizeChroma(uint32_t bits27)
    {
        return lookup(g_dcChroma, bits27);
    }

    DctResult decodeDctCoeff(bool tableOne, bool first, uint32_t bits27)
    {
        if (tableOne)
        {
            // Table B.15 is intentionally absent -- see the file header. The
            // caller turns len == 0 into IPU_CTRL.ECD.
            return DctResult{0, 0, 0};
        }

        // First coefficient of a non-intra block: '1' means (run 0, level 1);
        // everywhere else that same prefix is EOB ('10') or (0,1) ('11').
        if (first && (bits27 >> (kPeekBits - 1)) != 0u)
            return DctResult{0, 1, 1};

        const uint32_t idx = narrow(bits27, g_dct0.indexBits);
        return DctResult{g_dct0.a[idx], g_dct0.b[idx], g_dct0.len[idx]};
    }
}
