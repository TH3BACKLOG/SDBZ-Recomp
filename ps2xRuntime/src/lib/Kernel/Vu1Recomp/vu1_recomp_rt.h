// Support code for the generated VU1 programs (vu1rc_<crc>.cpp). Every helper
// mirrors a piece of VU1Interpreter (ps2xRuntime/src/lib/vu/ps2_vu1_*.cpp) and
// must stay bit-exact and cycle-exact with it; PS2X_VU1_RECOMP=2 checks that
// run by run.
//
// Timing model. A program always starts with every interpreter pipeline empty
// (execute() resets them, resume() follows an E-bit end which flushed them), so
// the generated code keeps its own pipeline state, relative to cycle 0:
//  - Stalls: per-register ready cycles, as calculatePairReadyCycle.
//  - VF/ACC/VI results are written at issue. Every reader stalls until the
//    register is ready, so no reader can see the difference (the interpreter
//    already does this for VF/ACC). Branches read the pre-write VI of the
//    previous pair through the same backup rule as readBranchVi.
//  - MAC/status/clip, Q and P are committed lazily: only when read, or at the
//    end, and always in the interpreter's commit order.
//  - Stores are written at issue after XGKICK has copied every qword due up to
//    that cycle (the interpreter commits the store at the next cycle boundary,
//    before PATH1 takes its next qword).
#pragma once

#include "Kernel/Vu1Recomp/vu1_recomp.h"
#include "runtime/ps2_vu1.h"
#include "vu/ps2_vu1_detail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#define VU1RC_INLINE __forceinline

namespace vu1rc
{
    void reportError(const char *what, uint32_t pc, uint32_t value);

    constexpr uint32_t kFmacLatency = 4u;

    // --- FMAC flags (normalizeFmacResult / calculateFmacProductSticky) -------

    enum Kind : uint8_t
    {
        kAdd,
        kSub,
        kMadd,
        kMsub,
        kMul,
        kOpmsub,
        kOpmula,
    };

    // vuNormalizeOperand, spelled out: these TUs build at /Ob1 (no CMake
    // edit), where the plain `inline` original was left as a call.
    VU1RC_INLINE float N(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    VU1RC_INLINE float bitsF(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    VU1RC_INLINE uint32_t fBits(float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    VU1RC_INLINE bool fastNormalFlags(float value, uint8_t &flags)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exp = (bits >> 23) & 0xFFu;
        if (exp < 127u - 100u || exp > 127u + 100u)
            return false;
        flags = (bits >> 31) != 0u ? 0x2u : 0u;
        return true;
    }

    // normalizeFmacExactResult. Not inlined: rare path.
    uint8_t normalizeExact(float &value, long double exact);

    // One FMAC lane. l/r/a are the normalized left, right and ACC operands of
    // this lane (for OPM* forms, l/r are the crossed lanes). Returns the float
    // result exactly as execUpper computes it and fills the lane flags.
    template <Kind K>
    VU1RC_INLINE float lane(float l, float r, float a, bool opmW, uint8_t &flags)
    {
        float res;
        if constexpr (K == kAdd)
            res = l + r;
        else if constexpr (K == kSub)
            res = l - r;
        else if constexpr (K == kMadd)
            res = a + l * r;
        else if constexpr (K == kMsub || K == kOpmsub)
            res = a - l * r;
        else
            res = l * r;
        if (opmW)
            res = 0.0f;
        if (!opmW && fastNormalFlags(res, flags))
            return res;
        long double exact = 0.0L;
        if (!opmW)
        {
            const long double L = l, R = r, A = a;
            if constexpr (K == kAdd)
                exact = L + R;
            else if constexpr (K == kSub)
                exact = L - R;
            else if constexpr (K == kMadd)
                exact = A + L * R;
            else if constexpr (K == kMsub || K == kOpmsub)
                exact = A - L * R;
            else
                exact = L * R;
        }
        flags = normalizeExact(res, exact);
        return res;
    }

    // Product sticky bits of one MADD/MSUB/OPMSUB lane.
    VU1RC_INLINE uint32_t productSticky(float l, float r)
    {
        float product = l * r;
        uint8_t f = 0u;
        if (fastNormalFlags(product, f))
            return f;
        const long double exact = static_cast<long double>(l) * static_cast<long double>(r);
        return normalizeExact(product, exact) & 0xFu;
    }

    // --- Pipeline state -------------------------------------------------------

    struct FlagEntry
    {
        uint32_t ready;
        uint32_t mac;
        uint32_t status;
        uint32_t extra;
        uint32_t clip;
        uint32_t what; // 1 mac, 2 status, 4 sticky (FSSET), 8 clip
    };

    struct R
    {
        Ctx *ctx;
        VU1State *st;
        uint8_t *mem;
        uint32_t memMask;
        uint32_t cyc = 0;

        uint32_t vfR[32][4];
        uint32_t viR[16];
        uint32_t accR[4];

        // Flag pipeline, FIFO (every entry is ready = issue + 4).
        FlagEntry fl[16];
        uint32_t flHead = 0, flTail = 0, flMaxReady = 0;

        bool fdivValid = false;
        uint32_t fdivReady = 0;
        float fdivValue = 0.0f;
        uint32_t fdivDi = 0;

        struct Efu
        {
            bool valid;
            uint32_t ready;
            float value;
        } efu[2]{};
        uint32_t efuResourceReady = 0;

        // XGKICK (PATH1): qword k of the packet is copied at cycle boundary
        // issue + 1 + 2k.
        bool kickActive = false;
        uint32_t kickIssue = 0, kickSrc = 0, kickCopied = 0, kickTagEnd = 0, kickTotal = 0;
        bool kickEop = false;
        uint32_t kickLastBoundary = 0;

        uint32_t workingClip = 0;
        bool error = false;

        VU1RC_INLINE void init(Ctx &c)
        {
            ctx = &c;
            st = c.st;
            mem = c.mem;
            memMask = c.memSize - 1u;
            std::memset(vfR, 0, sizeof(vfR));
            std::memset(viR, 0, sizeof(viR));
            std::memset(accR, 0, sizeof(accR));
            workingClip = c.workingClip;
        }

        // --- flags / Q (status-affecting commits, interpreter order) ---------

        VU1RC_INLINE void applyFlag(const FlagEntry &e)
        {
            if (e.what & 1u)
                st->mac = e.mac;
            if (e.what & 2u)
            {
                const uint32_t current = e.status & 0xFu;
                st->status = (st->status & 0xFF0u) | current | ((current | e.extra) << 6);
            }
            if (e.what & 4u)
                st->status = (st->status & 0x03Fu) | (e.status & 0xFC0u);
            if (e.what & 8u)
                st->clip = e.clip;
        }

        VU1RC_INLINE void applyFdiv()
        {
            st->q = fdivValue;
            const uint32_t currentDi = fdivDi & 0x30u;
            st->status = (st->status & 0xFCFu) | currentDi | (currentDi << 6);
            fdivValid = false;
        }

        // Everything in the flag FIFO and FDIV that is due by cycle t. At one
        // cycle boundary the interpreter commits flag entries before FDIV.
        VU1RC_INLINE void commitTo(uint32_t t)
        {
            for (;;)
            {
                const bool f = flHead != flTail && fl[flHead & 15u].ready <= t;
                const bool d = fdivValid && fdivReady <= t;
                if (f && (!d || fl[flHead & 15u].ready <= fdivReady))
                {
                    applyFlag(fl[flHead & 15u]);
                    ++flHead;
                }
                else if (d)
                    applyFdiv();
                else
                    return;
            }
        }

        VU1RC_INLINE FlagEntry &pushFlag()
        {
            if (flHead != flTail && fl[flHead & 15u].ready <= cyc)
                commitTo(cyc);
            if (flTail - flHead >= 8u)
            {
                // The interpreter has 8 slots and reports a reserved
                // instruction when they are all busy.
                reportError("flag pipeline full", st->pc, 0u);
                error = true;
            }
            FlagEntry &e = fl[flTail & 15u];
            ++flTail;
            e.ready = cyc + kFmacLatency;
            e.what = 0u;
            flMaxReady = e.ready;
            return e;
        }

        VU1RC_INLINE void fmacFlags(const uint8_t lf[4], uint8_t dest, uint32_t extraSticky)
        {
            uint32_t mac = 0u, status = 0u;
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const uint32_t ln = 1u << (3u - c);
                if ((dest & ln) == 0u)
                    continue;
                const uint32_t f = lf[c];
                if (f & 1u)
                    mac |= ln;
                if (f & 2u)
                    mac |= ln << 4;
                if (f & 4u)
                    mac |= ln << 8;
                if (f & 8u)
                    mac |= ln << 12;
                status |= f;
            }
            FlagEntry &e = pushFlag();
            e.mac = mac;
            e.status = status;
            e.extra = extraSticky;
            e.what = 3u;
        }

        // FSSET / FCSET cancel the same-cycle entry's status / clip write.
        VU1RC_INLINE void cancelSameCycle(uint32_t whatBit)
        {
            for (uint32_t k = flHead; k != flTail; ++k)
                if (fl[k & 15u].ready == cyc + kFmacLatency)
                    fl[k & 15u].what &= ~whatBit;
        }

        VU1RC_INLINE void fsset(uint32_t imm12)
        {
            cancelSameCycle(2u);
            FlagEntry &e = pushFlag();
            e.status = imm12 & 0xFC0u;
            e.what = 4u;
        }

        VU1RC_INLINE void clip(uint32_t flags)
        {
            workingClip = ((workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
            FlagEntry &e = pushFlag();
            e.clip = workingClip;
            e.what = 8u;
        }

        VU1RC_INLINE void fcset(uint32_t value)
        {
            workingClip = value & 0xFFFFFFu;
            cancelSameCycle(8u);
            FlagEntry &e = pushFlag();
            e.clip = workingClip;
            e.what = 8u;
        }

        VU1RC_INLINE uint32_t mac() { commitTo(cyc); return st->mac; }
        VU1RC_INLINE uint32_t status() { commitTo(cyc); return st->status; }
        VU1RC_INLINE uint32_t clipFlag() { commitTo(cyc); return st->clip; }
        VU1RC_INLINE float q() { commitTo(cyc); return st->q; }

        VU1RC_INLINE void queueQ(float value, uint32_t latency, uint32_t di)
        {
            commitTo(cyc);
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            const uint32_t sign = bits & 0x80000000u;
            const uint32_t exponent = (bits >> 23) & 0xFFu;
            if ((bits & 0x7FFFFFFFu) != 0u && exponent == 0u)
                bits = sign;
            else if (exponent == 0xFFu)
                bits = sign | 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
            fdivValid = true;
            fdivReady = cyc + latency;
            fdivValue = value;
            fdivDi = di & 0x30u;
        }

        // --- P ---------------------------------------------------------------

        VU1RC_INLINE void commitP(uint32_t t)
        {
            for (;;)
            {
                int pick = -1;
                for (int k = 0; k < 2; ++k)
                    if (efu[k].valid && efu[k].ready <= t && (pick < 0 || efu[k].ready < efu[pick].ready))
                        pick = k;
                if (pick < 0)
                    return;
                st->p = efu[pick].value;
                efu[pick].valid = false;
            }
        }

        VU1RC_INLINE float p() { commitP(cyc); return st->p; }

        VU1RC_INLINE void queueP(float value, uint32_t latency)
        {
            commitP(cyc);
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            const uint32_t sign = bits & 0x80000000u;
            const uint32_t exponent = (bits >> 23) & 0xFFu;
            if ((bits & 0x7FFFFFFFu) != 0u && exponent == 0u)
                bits = sign;
            else if (exponent == 0xFFu)
                bits = sign | 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
            for (auto &e : efu)
            {
                if (!e.valid)
                {
                    e.valid = true;
                    e.ready = cyc + latency;
                    e.value = value;
                    efuResourceReady = cyc + (latency > 0u ? latency - 1u : 0u);
                    return;
                }
            }
            reportError("EFU pipeline full", st->pc, 0u);
            error = true;
        }

        VU1RC_INLINE uint32_t efuWaitReady() const
        {
            uint32_t r = 0u;
            for (const auto &e : efu)
                if (e.valid)
                    r = std::max(r, e.ready);
            return r;
        }

        // --- XGKICK ----------------------------------------------------------

        // Copies the next qword (progressXgkick body). Returns false when the
        // packet is finished or failed.
        bool kickStep();

        // Copies every qword due at cycle boundaries <= t.
        VU1RC_INLINE void kickCatchUp(uint32_t t)
        {
            if (!kickActive || t < kickIssue + 1u)
                return;
            const uint32_t due = (t - kickIssue - 1u) / 2u + 1u;
            while (kickActive && kickCopied / 16u < due)
                kickStep();
        }

        // Runs the packet to its end. Returns the boundary it finished at.
        VU1RC_INLINE uint32_t kickFinishAll()
        {
            while (kickActive)
                kickStep();
            return kickLastBoundary;
        }

        VU1RC_INLINE void kickStart(uint32_t qwordAddress)
        {
            kickActive = true;
            kickSrc = (qwordAddress * 16u) & memMask;
            kickIssue = cyc;
            kickCopied = 0u;
            kickTagEnd = 0u;
            kickTotal = 0u;
            kickEop = false;
        }

        // --- memory ----------------------------------------------------------

        VU1RC_INLINE uint32_t addr(int32_t qword) const
        {
            return (static_cast<uint32_t>(qword) * 16u) & memMask;
        }

        VU1RC_INLINE void load(float *dst, uint32_t a, uint8_t dest)
        {
            const float *s = reinterpret_cast<const float *>(mem + a);
            if (dest & 8u) dst[0] = s[0];
            if (dest & 4u) dst[1] = s[1];
            if (dest & 2u) dst[2] = s[2];
            if (dest & 1u) dst[3] = s[3];
        }

        VU1RC_INLINE void storeWords(uint32_t a, const uint32_t *w, uint8_t dest)
        {
            kickCatchUp(cyc);
            uint32_t *d = reinterpret_cast<uint32_t *>(mem + a);
            if (dest & 8u) d[0] = w[0];
            if (dest & 4u) d[1] = w[1];
            if (dest & 2u) d[2] = w[2];
            if (dest & 1u) d[3] = w[3];
        }

        VU1RC_INLINE void storeVf(uint32_t a, const float *v, uint8_t dest)
        {
            uint32_t w[4];
            std::memcpy(w, v, sizeof(w));
            storeWords(a, w, dest);
        }

        VU1RC_INLINE void storeVi(uint32_t a, int32_t vi, uint8_t dest)
        {
            const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(vi & 0xFFFF));
            const uint32_t w[4] = {val, val, val, val};
            storeWords(a, w, dest);
        }

        VU1RC_INLINE int32_t ilw(uint32_t a, uint8_t dest) const
        {
            const uint32_t comp = (dest & 8u) ? 0u : (dest & 4u) ? 1u : (dest & 2u) ? 2u : 3u;
            uint32_t v;
            std::memcpy(&v, mem + a + comp * 4u, 4);
            return static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
        }

        // --- end -------------------------------------------------------------

        // E-bit end: cyc is already one past the last pair (its
        // advanceOneCycle); the interpreter then steps until every pipeline is
        // empty. Returns the end cycle.
        VU1RC_INLINE uint32_t finish(uint32_t viMaxReady)
        {
            uint32_t end = cyc;
            if (flHead != flTail)
                end = std::max(end, flMaxReady);
            if (fdivValid)
                end = std::max(end, fdivReady);
            for (const auto &e : efu)
                if (e.valid)
                    end = std::max(end, e.ready);
            end = std::max(end, viMaxReady);
            if (kickActive)
                end = std::max(end, kickFinishAll());
            commitTo(0xFFFFFFFFu);
            commitP(0xFFFFFFFFu);
            return end;
        }

        VU1RC_INLINE uint32_t viMax() const
        {
            uint32_t m = 0u;
            for (uint32_t r : viR)
                m = std::max(m, r);
            return m;
        }
    };

    // Upper-op helpers ---------------------------------------------------------

    VU1RC_INLINE int32_t floatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    VU1RC_INLINE bool clipExceeds(float value, uint32_t signMask, int32_t limit)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        bits ^= signMask;
        int32_t ordered;
        std::memcpy(&ordered, &bits, sizeof(ordered));
        return ordered > limit;
    }

    VU1RC_INLINE uint32_t clipFlags(const float *vs, const float *vt)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &vt[3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        uint32_t f = 0u;
        if (clipExceeds(vs[0], 0u, limit)) f |= 0x01u;
        if (clipExceeds(vs[0], 0x80000000u, limit)) f |= 0x02u;
        if (clipExceeds(vs[1], 0u, limit)) f |= 0x04u;
        if (clipExceeds(vs[1], 0x80000000u, limit)) f |= 0x08u;
        if (clipExceeds(vs[2], 0u, limit)) f |= 0x10u;
        if (clipExceeds(vs[2], 0x80000000u, limit)) f |= 0x20u;
        return f;
    }

    // Lower-op helpers (EFU polynomials, same as ps2_vu1_lower.cpp).
    float eatan(float value);
    float esin(float value);
    float eexp(float value);
}
