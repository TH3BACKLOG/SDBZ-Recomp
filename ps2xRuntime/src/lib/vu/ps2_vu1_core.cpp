#include "runtime/ps2_vu1.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_pipeline_stats.h"
#include "ps2_vu1_detail.h"
#include "Kernel/VuCap/VuCapRecorder.h"
#include "Kernel/Vu1Recomp/vu1_recomp.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>
#include <ps2_log.h>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // Earliest readyCycle among this interpreter's queued pipeline entries, so
    // commitReadyPipelines() can skip its 50-slot scan on cycles where nothing
    // is due. Kept here instead of as a member because ps2_vu1.h reaches every
    // runner TU through ps2_runtime.h. One slot per Unit; `owner` guards against
    // a second interpreter of the same unit (tests) reusing a stale hint -- a
    // mismatch just forces a full scan, which then re-claims the slot.
    // pipeReady[] holds the same bound per pipeline, so a due cycle only scans
    // the pipelines that actually have something due. A stale-low value only
    // costs a rescan; every enqueue lowers its pipeline's entry.
    enum CommitPipe : uint32_t
    {
        kPipeFlag,
        kPipeFdiv,
        kPipeEfu,
        kPipeStore,
        kPipeVf,
        kPipeVi,
        kPipeAcc,
        kPipeCount
    };

    struct CommitHint
    {
        const void *owner = nullptr;
        uint64_t nextReady = 0;
        uint64_t pipeReady[kPipeCount]{};
        // Superset of the slots holding a pending entry, per pipeline. Entries
        // invalidated elsewhere (flush/reset) may leave a stale bit; the scan
        // still checks `valid`, so a stale bit costs one probe, never a commit.
        uint32_t live[kPipeCount]{};
    };
    CommitHint g_commitHint[2];

    inline void lowerCommitHint(const void *owner, uint32_t unit, CommitPipe pipe, uint64_t readyCycle,
                                uint32_t slot)
    {
        CommitHint &hint = g_commitHint[unit & 1u];
        if (hint.owner != owner)
            return;
        hint.live[pipe] |= 1u << slot;
        if (readyCycle < hint.pipeReady[pipe])
            hint.pipeReady[pipe] = readyCycle;
        if (readyCycle < hint.nextReady)
            hint.nextReady = readyCycle;
    }

    // Slot index of an entry within its pipeline array.
    template <typename Array>
    inline uint32_t slotOf(const Array &pipeline, const typename Array::value_type &entry)
    {
        return static_cast<uint32_t>(&entry - pipeline.data());
    }

    // Recompiled VU1 program for the current micro memory, looked up again
    // only when the code generation changes (MPG upload).
    struct RecompLookup
    {
        const void *owner = nullptr;
        uint64_t generation = ~0ull;
        vu1rc::Program programs[4]{};
        uint32_t count = 0;
        uint32_t crc = 0; // image CRC, computed on first need (logs/dumps)
        bool crcValid = false;
    };
    RecompLookup g_recompLookup;

    uint32_t recompImageCrc(const uint8_t *code, uint32_t size)
    {
        if (!g_recompLookup.crcValid)
        {
            g_recompLookup.crc = vu1rc::imageCrc(code, size);
            g_recompLookup.crcValid = true;
        }
        return g_recompLookup.crc;
    }

    // A run the recompiler could not take: image not compiled, or a compiled
    // image entered at a pc it was not compiled for. Logged once per
    // (image, pc). With PS2X_VU1_DUMPDIR set, the image and entry are written
    // in the replay tool's format, as input for build_scripts/vu1_recomp.py.
    void noteUncompiledVu1Program(uint32_t crc, const uint8_t *code, uint32_t size, uint32_t pc, bool knownImage)
    {
        static std::vector<uint64_t> seen;
        const uint64_t key = (static_cast<uint64_t>(crc) << 32) | pc;
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            return;
        seen.push_back(key);
        if (seen.size() <= 64u)
            std::fprintf(stderr, "[vu1recomp] not compiled: image %08x entry pc=0x%x%s\n", crc, pc,
                         knownImage ? " (image known, entry new)" : "");
        static const char *dumpDir = std::getenv("PS2X_VU1_DUMPDIR");
        if (!dumpDir || !*dumpDir)
            return;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%08x.bin", dumpDir, crc);
        if (!knownImage && !std::filesystem::exists(path))
        {
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(code, 1, size, f);
                std::fclose(f);
            }
        }
        std::snprintf(path, sizeof(path), "%s/entries.csv", dumpDir);
        if (FILE *f = std::fopen(path, "a"))
        {
            std::fprintf(f, "%08x,%u,%d\n", crc, pc, pc != 0u ? 1 : 0);
            std::fclose(f);
        }
    }

    // PS2X_VU1_RECOMP=2: the recompiled result of this run, compared with the
    // interpreter's at the end of run().
    struct RecompVerify
    {
        bool pending = false;
        VU1State state{};
        std::vector<uint8_t> mem;
        std::vector<std::vector<uint8_t>> kicks;      // recompiled program's PATH1 packets
        std::vector<std::vector<uint8_t>> interpKicks; // interpreter's, captured in finishXgkick
        uint64_t cycles = 0;
        uint32_t workingClip = 0;
        uint32_t end = 0;
        uint32_t startPc = 0;
        uint64_t runs = 0;
        uint64_t mismatches = 0;
    };
    RecompVerify g_recompVerify;

    bool sameBits(const void *a, const void *b, size_t n) { return std::memcmp(a, b, n) == 0; }

    uint32_t vu1rcBits(float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

// Clears XGKICK state but not its 64 KB packet buffer: `m_xgkick = {}` built
// and copied a zeroed 64 KB temporary on every XGKICK and every program start
// (the memmove in the 09-27 fight profile). Bytes are always copied into the
// buffer before they are read, so stale contents are never seen.
// (Template so the private XgkickPipeline type is deduced, not named.)
template <class Xgkick>
static void resetXgkickKeepBuffer(Xgkick &x)
{
    x.sourceAddress = 0;
    x.totalBytes = 0;
    x.copiedBytes = 0;
    x.currentTagEnd = 0;
    x.cycleCredit = 0;
    x.issueCycle = 0;
    x.active = false;
    x.currentTagEop = false;
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    resetXgkickKeepBuffer(m_xgkick);
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
    CommitHint &hint = g_commitHint[static_cast<uint32_t>(m_unit) & 1u];
    if (hint.owner == this)
        hint.owner = nullptr;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return vuNormalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    return vuNormalizeOperand(value);
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

namespace
{
    // Shape of an FMAC upper op for the exact (long double) result, decoded
    // once per instruction instead of per lane. The regular (op < 0x3C) and
    // special (op >= 0x3C) tables share codes; only 0x2E differs (OPMSUB
    // subtracts from ACC, OPMULA does not).
    enum FmacExactKind : uint8_t
    {
        kFmacNone,
        kFmacAdd,   // a + b
        kFmacSub,   // a - b
        kFmacMadd,  // acc + a * b
        kFmacMsub,  // acc - a * b
        kFmacMul,   // a * b
        kFmacOpmsub, // acc - vs[l] * vt[r]
        kFmacOpmula, // vs[l] * vt[r]
    };

    // Fast classification: when the float result's exponent is far from both
    // ends of the range, the exact result is a normal non-zero number with the
    // same sign (the float result is within a few ulps of it, and cancellation
    // in MADD/MSUB either gives an exact zero or keeps the sign). Then the
    // exact path would leave the value alone and only report the sign flag, so
    // it can be skipped. The margin (2^-100 .. 2^100) is far wider than needed.
    constexpr uint32_t kSafeExpLo = 127u - 100u;
    constexpr uint32_t kSafeExpHi = 127u + 100u;

    inline bool fastNormalFlags(float value, uint8_t &flags)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exp = (bits >> 23) & 0xFFu;
        if (exp < kSafeExpLo || exp > kSafeExpHi)
            return false;
        flags = (bits >> 31) != 0u ? 0x2u : 0u;
        return true;
    }
    enum FmacExactRight : uint8_t
    {
        kRightBc,   // vt[bc]
        kRightQ,
        kRightI,
        kRightLane, // vt[component]
    };
    struct FmacExactForm
    {
        FmacExactKind kind = kFmacNone;
        FmacExactRight right = kRightLane;
        uint8_t bc = 0u;
        uint8_t fs = 0u;
        uint8_t ft = 0u;
    };

    FmacExactForm decodeFmacExactForm(uint32_t upper)
    {
        FmacExactForm form;
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const bool isSpecial = op >= 0x3Cu;
        const uint8_t code = isSpecial
                                 ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                 : op;
        form.fs = FS(upper);
        form.ft = FT(upper);
        form.bc = static_cast<uint8_t>(code & 3u);

        if (code <= 0x0Fu || (code >= 0x18u && code <= 0x1Bu))
        {
            form.right = kRightBc;
            if (code <= 0x03u)
                form.kind = kFmacAdd;
            else if (code <= 0x07u)
                form.kind = kFmacSub;
            else if (code <= 0x0Bu)
                form.kind = kFmacMadd;
            else if (code <= 0x0Fu)
                form.kind = kFmacMsub;
            else
                form.kind = kFmacMul;
            return form;
        }

        switch (code)
        {
        case 0x1Cu: form.kind = kFmacMul;  form.right = kRightQ; break;
        case 0x1Eu: form.kind = kFmacMul;  form.right = kRightI; break;
        case 0x20u: form.kind = kFmacAdd;  form.right = kRightQ; break;
        case 0x21u: form.kind = kFmacMadd; form.right = kRightQ; break;
        case 0x22u: form.kind = kFmacAdd;  form.right = kRightI; break;
        case 0x23u: form.kind = kFmacMadd; form.right = kRightI; break;
        case 0x24u: form.kind = kFmacSub;  form.right = kRightQ; break;
        case 0x25u: form.kind = kFmacMsub; form.right = kRightQ; break;
        case 0x26u: form.kind = kFmacSub;  form.right = kRightI; break;
        case 0x27u: form.kind = kFmacMsub; form.right = kRightI; break;
        case 0x28u: form.kind = kFmacAdd;  break;
        case 0x29u: form.kind = kFmacMadd; break;
        case 0x2Au: form.kind = kFmacMul;  break;
        case 0x2Cu: form.kind = kFmacSub;  break;
        case 0x2Du: form.kind = kFmacMsub; break;
        case 0x2Eu: form.kind = isSpecial ? kFmacOpmula : kFmacOpmsub; break;
        default: break;
        }
        return form;
    }

    bool evalFmacExactForm(const FmacExactForm &form, const VU1State &state,
                           uint32_t component, long double &result)
    {
        const auto operand = [](float value)
        {
            return static_cast<long double>(vuNormalizeOperand(value));
        };
        const auto vs = [&](uint32_t lane) { return operand(state.vf[form.fs][lane]); };
        const auto vt = [&](uint32_t lane) { return operand(state.vf[form.ft][lane]); };
        const auto acc = [&](uint32_t lane) { return operand(state.acc[lane]); };

        if (form.kind == kFmacOpmsub || form.kind == kFmacOpmula)
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            if (component == 3u)
                result = 0.0L;
            else if (form.kind == kFmacOpmsub)
                result = acc(component) - vs(left[component]) * vt(right[component]);
            else
                result = vs(left[component]) * vt(right[component]);
            return true;
        }

        long double b = 0.0L;
        switch (form.right)
        {
        case kRightBc: b = vt(form.bc); break;
        case kRightQ: b = operand(state.q); break;
        case kRightI: b = operand(state.i); break;
        case kRightLane: b = vt(component); break;
        }

        switch (form.kind)
        {
        case kFmacAdd: result = vs(component) + b; return true;
        case kFmacSub: result = vs(component) - b; return true;
        case kFmacMadd: result = acc(component) + vs(component) * b; return true;
        case kFmacMsub: result = acc(component) - vs(component) * b; return true;
        case kFmacMul: result = vs(component) * b; return true;
        default: return false;
        }
    }
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    const FmacExactForm form = decodeFmacExactForm(m_currentUpperInstruction);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        // OPMULA/OPMSUB w is an exact zero whatever the float lane holds.
        const bool opmW = component == 3u &&
                          (form.kind == kFmacOpmsub || form.kind == kFmacOpmula);
        if (form.kind != kFmacNone && !opmW &&
            fastNormalFlags(result[component], laneFlags[component]))
            continue;

        long double exactResult = 0.0L;
        if (evalFmacExactForm(form, m_state, component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    return evalFmacExactForm(decodeFmacExactForm(m_currentUpperInstruction), m_state,
                             component, result);
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
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

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    // Only product-sum ops (MADD/MSUB families, OPMSUB) have a product whose
    // conditions go to the sticky flags; OPMULA is a plain product.
    const FmacExactForm form = decodeFmacExactForm(m_currentUpperInstruction);
    if (form.kind != kFmacMadd && form.kind != kFmacMsub && form.kind != kFmacOpmsub)
        return 0u;

    uint32_t extraSticky = 0u;
    const float *vs = m_state.vf[form.fs];
    const float *vt = m_state.vf[form.ft];
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const bool cross = form.kind == kFmacOpmsub;
        const float left = vuNormalizeOperand(vs[cross ? crossLeft[component] : component]);
        float right = 0.0f;
        switch (form.right)
        {
        case kRightBc: right = vuNormalizeOperand(vt[form.bc]); break;
        case kRightQ: right = vuNormalizeOperand(m_state.q); break;
        case kRightI: right = vuNormalizeOperand(m_state.i); break;
        case kRightLane: right = vuNormalizeOperand(vt[cross ? crossRight[component] : component]); break;
        }

        float product = left * right;
        uint8_t fastFlags = 0u;
        if (fastNormalFlags(product, fastFlags))
        {
            extraSticky |= fastFlags;
            continue;
        }
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeFlag, entry->readyCycle, slotOf(m_flagPipeline, *entry));
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeFlag, entry.readyCycle, slotOf(m_flagPipeline, entry));
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeFlag, entry.readyCycle, slotOf(m_flagPipeline, entry));
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeFlag, entry.readyCycle, slotOf(m_flagPipeline, entry));
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeFdiv, m_fdiv.readyCycle, 0u);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeEfu, entry.readyCycle, slotOf(m_efu, entry));
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeStore, store.readyCycle, slotOf(m_storePipeline, store));
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeVf, write.readyCycle, slotOf(m_vfWritePipeline, write));
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeVi, write.readyCycle, slotOf(m_viWritePipeline, write));
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            lowerCommitHint(this, static_cast<uint32_t>(m_unit), kPipeAcc, write.readyCycle, slotOf(m_accWritePipeline, write));
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    CommitHint &hint = g_commitHint[static_cast<uint32_t>(m_unit) & 1u];
    const bool fullScan = hint.owner != this;
    if (!fullScan && m_cycle < hint.nextReady)
        return;

    // Scan only the pipelines whose earliest pending readyCycle is due (all of
    // them when the hint belongs to another interpreter), and recompute that
    // pipeline's bound. Every enqueue site lowers it via lowerCommitHint().
    constexpr uint64_t kNone = std::numeric_limits<uint64_t>::max();
    const auto due = [&](CommitPipe pipe)
    {
        return fullScan || hint.pipeReady[pipe] <= m_cycle;
    };
    uint64_t pipeNext = kNone;
    uint32_t pipeLive = 0u;
    const auto keep = [&](uint64_t readyCycle, uint32_t slot)
    {
        pipeLive |= 1u << slot;
        if (readyCycle < pipeNext)
            pipeNext = readyCycle;
    };
    const auto finish = [&](CommitPipe pipe)
    {
        hint.pipeReady[pipe] = pipeNext;
        hint.live[pipe] = pipeLive;
        pipeNext = kNone;
        pipeLive = 0u;
    };
    // Slots to visit, in ascending order (same order as the old full scan, so
    // same-cycle flag entries still apply lowest-slot first).
    const auto slots = [&](CommitPipe pipe, size_t count) -> uint32_t
    {
        return fullScan ? static_cast<uint32_t>((1ull << count) - 1ull) : hint.live[pipe];
    };

    if (due(kPipeFlag))
    {
        for (uint32_t m = slots(kPipeFlag, m_flagPipeline.size()); m != 0u; m &= m - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m));
            FlagPipelineEntry &entry = m_flagPipeline[slot];
            if (!entry.valid)
                continue;
            if (entry.readyCycle > m_cycle)
            {
                keep(entry.readyCycle, slot);
                continue;
            }

            if (entry.writesMac)
                m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writesSticky)
            {
                m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            }
            if (entry.writesClip)
                m_state.clip = entry.clip;
            entry = {};
        }
        finish(kPipeFlag);
    }

    if (due(kPipeFdiv))
    {
        if (m_fdiv.valid && m_fdiv.readyCycle > m_cycle)
            keep(m_fdiv.readyCycle, 0u);
        else if (m_fdiv.valid)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        finish(kPipeFdiv);
    }

    if (due(kPipeEfu))
    {
        for (ScalarPipelineEntry &entry : m_efu)
        {
            if (entry.valid && entry.readyCycle > m_cycle)
                keep(entry.readyCycle, slotOf(m_efu, entry));
            else if (entry.valid)
            {
                m_state.p = entry.value;
                entry = {};
            }
        }
        finish(kPipeEfu);
    }

    if (due(kPipeStore))
    {
        for (uint32_t m = slots(kPipeStore, m_storePipeline.size()); m != 0u; m &= m - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m));
            PendingStore &store = m_storePipeline[slot];
            if (!store.valid)
                continue;
            if (store.readyCycle > m_cycle)
            {
                keep(store.readyCycle, slot);
                continue;
            }
            if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
            {
                uint32_t oldWords[4]{};
                std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    if ((store.laneMask & laneForComponent(component)) != 0u)
                        oldWords[component] = store.words[component];
                }
                std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
            }
            store = {};
        }
        finish(kPipeStore);
    }

    if (due(kPipeVf))
    {
        for (uint32_t m = slots(kPipeVf, m_vfWritePipeline.size()); m != 0u; m &= m - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m));
            PendingVfWrite &write = m_vfWritePipeline[slot];
            if (!write.valid)
                continue;
            if (write.readyCycle > m_cycle)
            {
                keep(write.readyCycle, slot);
                continue;
            }
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    m_vfLatestWrite[write.reg][component] == write.sequence)
                {
                    m_state.vf[write.reg][component] = write.value[component];
                }
            }
            write = {};
        }
        finish(kPipeVf);
    }

    if (due(kPipeVi))
    {
        for (uint32_t m = slots(kPipeVi, m_viWritePipeline.size()); m != 0u; m &= m - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m));
            PendingViWrite &write = m_viWritePipeline[slot];
            if (!write.valid)
                continue;
            if (write.readyCycle > m_cycle)
            {
                keep(write.readyCycle, slot);
                continue;
            }
            if (m_viLatestWrite[write.reg] == write.sequence)
                m_state.vi[write.reg] = static_cast<int16_t>(write.value);
            write = {};
        }
        finish(kPipeVi);
    }

    if (due(kPipeAcc))
    {
        for (uint32_t m = slots(kPipeAcc, m_accWritePipeline.size()); m != 0u; m &= m - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(m));
            PendingAccWrite &write = m_accWritePipeline[slot];
            if (!write.valid)
                continue;
            if (write.readyCycle > m_cycle)
            {
                keep(write.readyCycle, slot);
                continue;
            }
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    m_accLatestWrite[component] == write.sequence)
                {
                    m_state.acc[component] = write.value[component];
                }
            }
            write = {};
        }
        finish(kPipeAcc);
    }

    uint64_t nextReady = kNone;
    for (uint64_t ready : hint.pipeReady)
        nextReady = std::min(nextReady, ready);
    hint.owner = this;
    hint.nextReady = nextReady;
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
            m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
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
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    ps2_pipeline_stats::g_xgkicks.fetch_add(1, std::memory_order_relaxed);
    ps2_pipeline_stats::g_xgkickBytes.fetch_add(m_xgkick.totalBytes, std::memory_order_relaxed);

    if (vucap::hot())
        vucap::kick(m_xgkick.sourceAddress, m_xgkick.packet.data(), m_xgkick.totalBytes);

    if (g_recompVerify.pending)
        g_recompVerify.interpKicks.emplace_back(m_xgkick.packet.data(), m_xgkick.packet.data() + m_xgkick.totalBytes);

    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    resetXgkickKeepBuffer(m_xgkick);
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        // Visit only the set bits. Lane bit b is component 3-b (laneForComponent).
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            const auto &regReady = m_vfReady[access.reg];
            for (uint32_t lanes = access.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                ready = std::max(ready, regReady[3u - static_cast<uint32_t>(std::countr_zero(lanes))]);
        }
        static_assert(std::tuple_size_v<decltype(m_viReady)> == 16u, "viRead is a 16-bit mask");
        for (uint32_t regs = usage->viRead & 0xFFFEu; regs != 0u; regs &= regs - 1u)
            ready = std::max(ready, m_viReady[static_cast<uint32_t>(std::countr_zero(regs))]);
        for (uint32_t lanes = usage->accRead & 0xFu; lanes != 0u; lanes &= lanes - 1u)
            ready = std::max(ready, m_accReady[3u - static_cast<uint32_t>(std::countr_zero(lanes))]);
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    // Visit only the set bits (VI0 excluded), same order as the old 1..15 scan.
    for (uint32_t regs = decoded.lowerUsage.viWrite & 0xFFFEu; regs != 0u; regs &= regs - 1u)
        m_viReady[static_cast<uint32_t>(std::countr_zero(regs))] =
            m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;

    // Recompiled micro program (Kernel/Vu1Recomp, build_scripts/vu1_recomp.py).
    // Only from a clean start: every pipeline empty, no branch or halt in
    // flight, VF0 as the interpreter leaves it after each pair.
    const int recompMode = vu1rc::mode();
    g_recompVerify.pending = false;
    if (recompMode != 0 && m_unit == Unit::VU1 && memory && vuCode == memory->getVU1Code() &&
        dataSize >= 16u && (dataSize & (dataSize - 1u)) == 0u && !m_stopRequested &&
        !m_state.branchPending && !m_state.ebit && !m_state.haltAfterDelaySlot &&
        m_state.vf[0][0] == 0.0f && m_state.vf[0][1] == 0.0f && m_state.vf[0][2] == 0.0f &&
        m_state.vf[0][3] == 1.0f && m_state.vi[0] == 0 && !pipelinesPending())
    {
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (g_recompLookup.owner != this || g_recompLookup.generation != generation)
        {
            g_recompLookup.owner = this;
            g_recompLookup.generation = generation;
            g_recompLookup.count = vu1rc::find(vuCode, codeSize, g_recompLookup.programs, 4u);
            g_recompLookup.crcValid = false;
        }
        if (g_recompLookup.count == 0u)
            noteUncompiledVu1Program(recompImageCrc(vuCode, codeSize), vuCode, codeSize, m_state.pc, false);
        if (g_recompLookup.count != 0u)
        {
            const bool verify = recompMode == 2;
            VU1State savedState{};
            std::vector<uint8_t> savedMem;
            if (verify)
            {
                savedState = m_state;
                savedMem.assign(vuData, vuData + dataSize);
                g_recompVerify.kicks.clear();
                g_recompVerify.interpKicks.clear();
            }

            vu1rc::Ctx ctx;
            ctx.st = &m_state;
            ctx.mem = vuData;
            ctx.memSize = dataSize;
            ctx.kickBuf = m_xgkick.packet.data();
            ctx.budget = maxCycles;
            ctx.self = this;
            if (verify)
            {
                ctx.submitKick = [](void *, uint32_t, const uint8_t *pkt, uint32_t bytes)
                {
                    g_recompVerify.kicks.emplace_back(pkt, pkt + bytes);
                };
            }
            else
            {
                ctx.submitKick = [](void *self, uint32_t srcAddr, const uint8_t *pkt, uint32_t bytes)
                {
                    VU1Interpreter &vu = *static_cast<VU1Interpreter *>(self);
                    ps2_pipeline_stats::g_xgkicks.fetch_add(1, std::memory_order_relaxed);
                    ps2_pipeline_stats::g_xgkickBytes.fetch_add(bytes, std::memory_order_relaxed);
                    if (vucap::hot())
                        vucap::kick(srcAddr, pkt, bytes);
                    if (vu.m_activeMemory)
                        vu.m_activeMemory->submitGifPacket(GifPathId::Path1, pkt, bytes);
                    else if (vu.m_activeGs)
                        vu.m_activeGs->processGIFPacket(pkt, bytes);
                };
            }
            ctx.workingClip = m_workingClip;
            ctx.bkValid = m_viBranchBackupValid;
            ctx.bkReg = m_viBranchBackupReg;
            ctx.bkVal = m_viBranchBackupValue;
            const uint32_t startPc = m_state.pc;

            bool ran = false;
            for (uint32_t k = 0; k < g_recompLookup.count && !ran; ++k)
                ran = g_recompLookup.programs[k](ctx);
            if (!ran)
                noteUncompiledVu1Program(recompImageCrc(vuCode, codeSize), vuCode, codeSize, startPc, true);
            if (ran)
            {
                if (!verify)
                {
                    m_cycle += ctx.cycles;
                    m_state.cycles = m_cycle;
                    m_workingClip = ctx.workingClip;
                    m_viBranchBackupValid = ctx.bkValid;
                    m_viBranchBackupReg = ctx.bkReg;
                    m_viBranchBackupValue = ctx.bkVal;
                    ps2_pipeline_stats::g_vu1Instrs.fetch_add(ctx.retired, std::memory_order_relaxed);
                    if (ctx.end == vu1rc::kEndEbit)
                        ps2_pipeline_stats::g_vu1EndEbit.fetch_add(1, std::memory_order_relaxed);
                    else if (ctx.end == vu1rc::kEndCycleLimit)
                        ps2_pipeline_stats::g_vu1EndCycleLimit.fetch_add(1, std::memory_order_relaxed);
                    else
                        m_stopRequested = true;
                    if (useVuRounding && previousRoundingMode != -1)
                        std::fesetround(previousRoundingMode);
                    return;
                }

                // Keep the recompiled result, restore the inputs, and let the
                // interpreter run the same program; compared at the end.
                g_recompVerify.pending = true;
                g_recompVerify.state = m_state;
                g_recompVerify.state.cycles = m_cycle + ctx.cycles;
                g_recompVerify.mem.assign(vuData, vuData + dataSize);
                g_recompVerify.cycles = ctx.cycles;
                g_recompVerify.workingClip = ctx.workingClip;
                g_recompVerify.end = ctx.end;
                g_recompVerify.startPc = startPc;
                m_state = savedState;
                std::memcpy(vuData, savedMem.data(), dataSize);
            }
        }
    }
    const uint64_t verifyStartCycle = m_cycle;

    uint64_t retired = 0;
    bool programEnded = false;
    bool endedByRange = false;
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
        {
            endedByRange = true;
            break;
        }

        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        ++retired;
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        if (const uint32_t viRegs = decoded.lowerUsage.viWrite & 0xFFFEu; viRegs != 0u)
        {
            // Lowest written VI, as the old 1..15 scan picked.
            writtenVi = static_cast<uint8_t>(std::countr_zero(viRegs));
            oldVi = m_state.vi[writtenVi];
        }

        // VF and ACC reads always stall until the register is ready
        // (calculatePairReadyCycle), so nothing can see a VF/ACC result early.
        // Writing them straight into m_state after the pair is therefore the
        // same as queueing them, and much cheaper. m_vfReady/m_accReady (set in
        // markPairWrites) still give the stall timing.
        constexpr bool kImmediateVfAcc = true;
        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = !kImmediateVfAcc && upperWrite.reg != 0u;
        const bool hasLowerWrite = !kImmediateVfAcc && lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        const bool queueAcc = !kImmediateVfAcc && decoded.upperUsage.accWrite != 0u;
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (queueAcc)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = vuNormalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (queueAcc)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        ps2_pipeline_stats::g_vu1EndEbit.fetch_add(1, std::memory_order_relaxed);
        ps2_pipeline_stats::g_vu1Instrs.fetch_add(retired, std::memory_order_relaxed);
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    else if (endedByRange)
    {
        ps2_pipeline_stats::g_vu1EndRange.fetch_add(1, std::memory_order_relaxed);
        ps2_pipeline_stats::g_vu1Instrs.fetch_add(retired, std::memory_order_relaxed);
    }
    else
    {
        // Fell out of the loop: the microprogram never signalled E-bit/D-bit/T-bit
        // and never ran off the end of code memory. A zero-filled VU1 code image
        // lands here.
        ps2_pipeline_stats::g_vu1EndCycleLimit.fetch_add(1, std::memory_order_relaxed);
        ps2_pipeline_stats::g_vu1Instrs.fetch_add(retired, std::memory_order_relaxed);
    }

    m_state.cycles = m_cycle;

    if (g_recompVerify.pending)
    {
        RecompVerify &v = g_recompVerify;
        v.pending = false;
        ++v.runs;
        char diff[1024];
        diff[0] = '\0';
        const VU1State &a = v.state;
        const VU1State &b = m_state;
        auto note = [&](const char *what)
        {
            if (diff[0] == '\0')
                std::snprintf(diff, sizeof(diff), "%s", what);
        };
        for (uint32_t reg = 0; reg < 32u && diff[0] == '\0'; ++reg)
            if (!sameBits(a.vf[reg], b.vf[reg], sizeof(a.vf[reg])))
                std::snprintf(diff, sizeof(diff), "vf%u recomp=%08x,%08x,%08x,%08x interp=%08x,%08x,%08x,%08x", reg,
                              vu1rcBits(a.vf[reg][0]), vu1rcBits(a.vf[reg][1]), vu1rcBits(a.vf[reg][2]), vu1rcBits(a.vf[reg][3]),
                              vu1rcBits(b.vf[reg][0]), vu1rcBits(b.vf[reg][1]), vu1rcBits(b.vf[reg][2]), vu1rcBits(b.vf[reg][3]));
        for (uint32_t reg = 0; reg < 16u && diff[0] == '\0'; ++reg)
            if (a.vi[reg] != b.vi[reg])
                std::snprintf(diff, sizeof(diff), "vi%u recomp=%d interp=%d", reg, a.vi[reg], b.vi[reg]);
        if (!sameBits(a.acc, b.acc, sizeof(a.acc)))
            note("acc");
        if (!sameBits(&a.q, &b.q, 4))
            note("q");
        if (!sameBits(&a.p, &b.p, 4))
            note("p");
        if (!sameBits(&a.i, &b.i, 4))
            note("i");
        if (a.r != b.r)
            note("r");
        if (a.pc != b.pc)
            std::snprintf(diff + std::strlen(diff), 64, "%spc recomp=%x interp=%x", diff[0] ? "; " : "", a.pc, b.pc);
        if (a.mac != b.mac)
            std::snprintf(diff + std::strlen(diff), 64, "%smac recomp=%x interp=%x", diff[0] ? "; " : "", a.mac, b.mac);
        if (a.status != b.status)
            std::snprintf(diff + std::strlen(diff), 64, "%sstatus recomp=%x interp=%x", diff[0] ? "; " : "", a.status, b.status);
        if (a.clip != b.clip)
            std::snprintf(diff + std::strlen(diff), 64, "%sclip recomp=%x interp=%x", diff[0] ? "; " : "", a.clip, b.clip);
        if (v.cycles != m_cycle - verifyStartCycle)
            std::snprintf(diff + std::strlen(diff), 80, "%scycles recomp=%llu interp=%llu", diff[0] ? "; " : "",
                          static_cast<unsigned long long>(v.cycles), static_cast<unsigned long long>(m_cycle - verifyStartCycle));
        if (v.workingClip != m_workingClip)
            note("workingClip");
        if (v.end != vu1rc::kEndEbit || !programEnded)
            note("end reason");
        if (!sameBits(v.mem.data(), vuData, std::min<size_t>(v.mem.size(), dataSize)))
        {
            size_t at = 0;
            while (at < v.mem.size() && v.mem[at] == vuData[at])
                ++at;
            std::snprintf(diff + std::strlen(diff), 64, "%smem @%zx", diff[0] ? "; " : "", at);
        }
        if (v.kicks != v.interpKicks)
            std::snprintf(diff + std::strlen(diff), 64, "%skicks recomp=%zu interp=%zu", diff[0] ? "; " : "",
                          v.kicks.size(), v.interpKicks.size());
        if (diff[0] != '\0')
        {
            ++v.mismatches;
            if (v.mismatches <= 40u)
                std::fprintf(stderr, "[vu1recomp] MISMATCH run %llu start pc=0x%x: %s\n",
                             static_cast<unsigned long long>(v.runs), v.startPc, diff);
        }
        if ((v.runs % 2000u) == 0u)
            std::fprintf(stderr, "[vu1recomp] verify: %llu runs, %llu mismatches\n",
                         static_cast<unsigned long long>(v.runs), static_cast<unsigned long long>(v.mismatches));
    }

    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
