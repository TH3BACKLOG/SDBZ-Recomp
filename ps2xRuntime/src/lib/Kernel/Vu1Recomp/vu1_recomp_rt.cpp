// Out-of-line parts of the VU1 recompiler runtime (see vu1_recomp_rt.h).
#include "Kernel/Vu1Recomp/vu1_recomp_rt.h"

#include <cstdio>
#include <cstdlib>
#include <ps2_log.h>

namespace vu1rc
{
    namespace
    {
        thread_local bool t_jrMiss = false;
        thread_local uint32_t t_jrTarget = 0;
    }

    void clearJrMiss()
    {
        t_jrMiss = false;
    }

    bool takeJrMiss(uint32_t &target)
    {
        if (!t_jrMiss)
            return false;
        t_jrMiss = false;
        target = t_jrTarget;
        return true;
    }

    void reportError(const char *what, uint32_t pc, uint32_t value)
    {
        if (std::strcmp(what, "JR to an address outside the compiled program") == 0)
        {
            t_jrMiss = true;
            t_jrTarget = value;
            // The interpreter finishes the packet. Say so once per distinct (entry, target).
            static thread_local uint64_t seen[16];
            static thread_local uint32_t nSeen = 0;
            const uint64_t key = (static_cast<uint64_t>(pc) << 32) | value;
            for (uint32_t i = 0; i < nSeen; ++i)
                if (seen[i] == key)
                    return;
            if (nSeen < 16u)
                seen[nSeen++] = key;
            RUNTIME_ERROR("[VU1 recomp] " << what << " pc=0x" << std::hex << pc << " value=0x" << value << std::dec
                                           << " (interpreter continues from the target)\n");
            return;
        }
        RUNTIME_ERROR("[VU1 recomp] " << what << " pc=0x" << std::hex << pc << " value=0x" << value << std::dec << '\n');
    }

    int mode()
    {
        static const int m = []
        {
            const char *e = std::getenv("PS2X_VU1_RECOMP");
            return e && *e ? std::atoi(e) : 1;
        }();
        return m;
    }

    uint8_t normalizeExact(float &value, long double exactResult)
    {
        const bool negative = std::signbit(exactResult);
        const long double magnitude = std::fabs(exactResult);
        const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
        const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
        uint8_t flags = negative ? 0x2u : 0u;

        uint32_t bits = negative ? 0x80000000u : 0u;
        if (magnitude == 0.0L)
        {
            flags |= 0x1u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude > maximum)
        {
            flags |= 0x8u;
            bits |= 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude < minimum)
        {
            flags |= 0x5u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        return flags;
    }

    bool R::kickStep()
    {
        constexpr uint32_t kBufferSize = 0x10000u;
        uint8_t *packet = ctx->kickBuf;
        if (kickCopied > kBufferSize - 16u)
        {
            reportError("XGKICK packet overflow", st->pc, kickCopied);
            kickActive = false;
            error = true;
            return false;
        }

        const uint32_t qwordOffset = kickCopied;
        // kickSrc and kickCopied are qword multiples and the memory size is a
        // power of two, so a qword never wraps: one 16-byte copy.
        std::memcpy(packet + kickCopied, mem + ((kickSrc + kickCopied) & memMask), 16u);
        kickCopied += 16u;
        kickLastBoundary = kickIssue + 1u + 2u * (kickCopied / 16u - 1u);

        if (kickTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, packet + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportError("XGKICK bad GIF tag format", st->pc, format);
                kickActive = false;
                error = true;
                return false;
            }
            if (tagBytes > kBufferSize - qwordOffset)
            {
                reportError("XGKICK tag overflow", st->pc, static_cast<uint32_t>(tagBytes));
                kickActive = false;
                error = true;
                return false;
            }
            kickTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            kickEop = ((tagLo >> 15) & 1u) != 0u;
            if (kickEop)
                kickTotal = kickTagEnd;
        }

        if (kickCopied >= kickTagEnd)
        {
            if (kickEop)
            {
                kickActive = false;
                ctx->submitKick(ctx->self, kickSrc, packet, kickTotal);
                return false;
            }
            kickTagEnd = 0u;
            kickEop = false;
        }
        return true;
    }

    float eatan(float value)
    {
        constexpr float coefficients[] = {
            0.999999344348907f,
            -0.333298563957214f,
            0.199465364217758f,
            -0.13085337519646f,
            0.096420042216778f,
            -0.055909886956215f,
            0.021861229091883f,
            -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;
        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return quarterPi + value * polynomial;
    }

    float esin(float value)
    {
        constexpr float coefficients[] = {
            1.0f,
            -0.166666567325592f,
            0.008333025500178f,
            -0.000198074136279f,
            0.000002601886990f};
        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }

    float eexp(float value)
    {
        constexpr float coefficients[] = {
            0.249998688697815f,
            0.031257584691048f,
            0.002591371303424f,
            0.000171562001924f,
            0.000005430199963f,
            0.000000690600018f};
        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = coefficients[index] + value * polynomial;
        polynomial = 1.0f + value * polynomial;
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }
}

// Program lookup table, written by build_scripts/vu1_recomp.py. The generated
// programs are the game's own microcode and are not in git; without them the
// recompiler finds nothing and every run stays on the interpreter. (After the
// first generate, rebuild this file: a header that did not exist before is not
// a tracked dependency.)
#if __has_include("Kernel/Vu1Recomp/generated/vu1rc_table.inc")
#include "Kernel/Vu1Recomp/generated/vu1rc_table.inc"
#else
uint32_t vu1rc::find(const uint8_t *, uint32_t, Program *, uint32_t)
{
    return 0u;
}

uint32_t vu1rc::imageCrc(const uint8_t *, uint32_t)
{
    return 0u;
}
#endif
