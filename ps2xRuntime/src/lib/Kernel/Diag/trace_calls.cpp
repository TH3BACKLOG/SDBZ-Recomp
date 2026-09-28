// ---------------------------------------------------------------------------
// [trace] generic, env-driven guest-call tracer.
//
// WHY THIS EXISTS
// ---------------
// Until now every "who calls X, with what arguments, and what did the globals
// look like at that moment?" question cost ~200 hand-written lines in
// game_overrides.cpp plus a full rebuild. That price is why so few questions
// got asked per session, and why each answer got over-interpreted. The
// part-68 [g36set] probe is the canonical example: 40 lines of tracing wrapped
// around an exact reimplementation of a 15-instruction guest function, and its
// entire value came from two things -- the return address, and a handful of
// watched globals read at call time. Both are generic.
//
// This TU makes that shape available without a rebuild:
//
//     PS2X_TRACE_CALLS="0x1555a0,0x14e8b0:64,0x154950"   addr[:logcap]
//     PS2X_TRACE_WATCH="g36=0x45F69C,nest=0x45F670,wbusy=0x441924"
//
// HOW IT WORKS
// ------------
// Generated code routes EVERY jal/jalr through runtime->dispatchGuestBranch()
// -> lookupFunction() -> g_ps2RecompiledFunctionTable[slot], so that table is
// a universal interception point. (The old "direct C++ fn_ calls bypass the
// table" note does not hold for the current codegen -- verified 2026-09-04
// against output/CAppCRISofdec_Tick_0x3f9c10.cpp, where all seven JALs and
// both JALRs go through dispatchGuestBranch.)
//
// We snapshot the existing pointer, install a thunk, and the thunk TAIL-CALLS
// the original. Nothing is reimplemented, so the tracer cannot be wrong about
// what the function does -- only about what it reports.
//
// The recompiled signature void(*)(uint8_t*, R5900Context*, PS2Runtime*)
// carries no address, so a thunk cannot know which slot it is. Hence a fixed
// pool of template instantiations, one per slot, each with its index baked in
// at compile time.
//
// INSTALL ORDER
// -------------
// Installed from the END of ps2_game_overrides::applyMatching(), after every
// descriptor has run, so it wraps whatever the game overrides left in the
// table rather than being clobbered by them.
//
// DELIBERATE NON-FEATURES
// -----------------------
// * No un-install. A run is the unit of measurement.
// * Refuses to install on a null table slot rather than synthesising a body.
//   A tracer that invents behaviour is worse than no tracer.
// * Per-slot log cap with the mandatory [cap] marker on saturation, because a
//   silently saturated probe reads exactly like "never happened".
// ---------------------------------------------------------------------------

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

extern "C" void ps2x_probe_kv(const char *name, int n,
                              const char *const *keys, const uint64_t *vals);

namespace
{
    constexpr std::size_t kMaxTraceSlots = 64u;
    // 48, not 16: build_scripts/presets.py's 'sofdec' set alone is 34 fields,
    // and a tracer that silently drops the tail of the operator's watch list
    // would report a smaller world than was asked for.
    constexpr std::size_t kMaxWatchFields = 48u;
    constexpr uint32_t kDefaultLogCap = 256u;

    struct TraceSlot
    {
        uint32_t guestAddr = 0u;
        PS2Runtime::RecompiledFunction orig = nullptr;
        uint32_t logCap = kDefaultLogCap;
        std::atomic<uint32_t> calls{0u};
        std::atomic<bool> capAnnounced{false};
    };

    struct WatchField
    {
        char name[24] = {};
        uint32_t addr = 0u;
    };

    TraceSlot g_slots[kMaxTraceSlots];
    std::size_t g_slotCount = 0u;

    WatchField g_watch[kMaxWatchFields];
    std::size_t g_watchCount = 0u;

    // Key strings must outlive the ps2x_probe_kv call and are the same every
    // time, so build them once rather than per record.
    const char *g_watchKeys[kMaxWatchFields] = {};

    bool parseHex(const std::string &text, uint32_t &out)
    {
        if (text.empty())
        {
            return false;
        }
        char *end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
        if (end == text.c_str() || *end != '\0')
        {
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }

    // Splits on ',' and trims ASCII blanks. Empty fields are dropped rather
    // than treated as an error -- a trailing comma in an env var is not worth
    // failing a run over.
    std::vector<std::string> splitList(const char *raw)
    {
        std::vector<std::string> out;
        if (raw == nullptr)
        {
            return out;
        }
        std::string cur;
        for (const char *p = raw;; ++p)
        {
            if (*p == ',' || *p == '\0')
            {
                const std::size_t b = cur.find_first_not_of(" \t");
                const std::size_t e = cur.find_last_not_of(" \t");
                if (b != std::string::npos)
                {
                    out.push_back(cur.substr(b, e - b + 1));
                }
                cur.clear();
                if (*p == '\0')
                {
                    break;
                }
                continue;
            }
            cur.push_back(*p);
        }
        return out;
    }

    void parseWatchFields()
    {
        const char *raw = std::getenv("PS2X_TRACE_WATCH");
        for (const std::string &item : splitList(raw))
        {
            if (g_watchCount >= kMaxWatchFields)
            {
                std::cerr << "[trace] PS2X_TRACE_WATCH: more than " << kMaxWatchFields
                          << " fields, ignoring '" << item << "'" << std::endl;
                break;
            }
            const std::size_t eq = item.find('=');
            if (eq == std::string::npos || eq == 0u)
            {
                std::cerr << "[trace] PS2X_TRACE_WATCH: bad field '" << item
                          << "' (want name=0xADDR)" << std::endl;
                continue;
            }
            uint32_t addr = 0u;
            if (!parseHex(item.substr(eq + 1), addr))
            {
                std::cerr << "[trace] PS2X_TRACE_WATCH: bad address in '" << item
                          << "'" << std::endl;
                continue;
            }
            WatchField &w = g_watch[g_watchCount];
            const std::string name = item.substr(0, eq);
            std::snprintf(w.name, sizeof(w.name), "%s", name.c_str());
            w.addr = addr;
            g_watchKeys[g_watchCount] = w.name;
            ++g_watchCount;
        }
    }

    // One record per intercepted call. Parameter NAMES are load-bearing:
    // READ32 expands to a lambda that captures rdram/ctx/runtime by name.
    void traceEnter(std::size_t slotIndex, uint8_t *rdram, R5900Context *ctx,
                    PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        TraceSlot &slot = g_slots[slotIndex];
        const uint32_t n = slot.calls.fetch_add(1u, std::memory_order_relaxed) + 1u;

        if (slot.logCap != 0u && n > slot.logCap)
        {
            // Announce saturation exactly once. Without this line, a capped
            // probe and a probe that never fired produce identical output.
            if (!slot.capAnnounced.exchange(true, std::memory_order_relaxed))
            {
                std::cerr << "[cap] tag=trace addr=0x" << std::hex << slot.guestAddr
                          << std::dec << " saturated at " << slot.logCap << std::endl;
            }
            return;
        }

        const char *keys[7u + kMaxWatchFields];
        uint64_t vals[7u + kMaxWatchFields];
        int k = 0;

        keys[k] = "addr";
        vals[k++] = slot.guestAddr;
        keys[k] = "n";
        vals[k++] = n;
        keys[k] = "ra";
        vals[k++] = GPR_U32(ctx, 31);
        keys[k] = "a0";
        vals[k++] = GPR_U32(ctx, 4);
        keys[k] = "a1";
        vals[k++] = GPR_U32(ctx, 5);
        keys[k] = "a2";
        vals[k++] = GPR_U32(ctx, 6);
        keys[k] = "a3";
        vals[k++] = GPR_U32(ctx, 7);

        for (std::size_t i = 0; i < g_watchCount; ++i)
        {
            keys[k] = g_watchKeys[i];
            vals[k++] = READ32(g_watch[i].addr);
        }

        ps2x_probe_kv("TRACE", k, keys, vals);
    }

    template <std::size_t N>
    void traceThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traceEnter(N, rdram, ctx, runtime);
        g_slots[N].orig(rdram, ctx, runtime);
    }

    template <std::size_t... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)>
    makeThunkTable(std::index_sequence<Is...>)
    {
        return {{&traceThunk<Is>...}};
    }

    const auto g_thunks = makeThunkTable(std::make_index_sequence<kMaxTraceSlots>{});
}

// Declared extern (not in a header) by game_overrides.cpp -- the sanctioned
// cross-TU pattern here, because touching ps2_runtime.h forces a 30+ hour
// rebuild of every generated runner TU.
void ps2xTraceCallsInstall(PS2Runtime &runtime)
{
    const char *raw = std::getenv("PS2X_TRACE_CALLS");
    if (raw == nullptr || raw[0] == '\0')
    {
        return; // Not armed: zero cost, zero behavioural change.
    }

    parseWatchFields();

    for (const std::string &item : splitList(raw))
    {
        if (g_slotCount >= kMaxTraceSlots)
        {
            std::cerr << "[trace] more than " << kMaxTraceSlots
                      << " addresses requested, ignoring '" << item << "'" << std::endl;
            break;
        }

        std::string addrText = item;
        uint32_t cap = kDefaultLogCap;
        const std::size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            addrText = item.substr(0, colon);
            uint32_t parsedCap = 0u;
            if (!parseHex(item.substr(colon + 1), parsedCap))
            {
                std::cerr << "[trace] bad log cap in '" << item << "'" << std::endl;
                continue;
            }
            cap = parsedCap; // 0 == unlimited
        }

        uint32_t addr = 0u;
        if (!parseHex(addrText, addr))
        {
            std::cerr << "[trace] bad address '" << item << "'" << std::endl;
            continue;
        }

        // Read the table directly rather than via lookupFunction(), which has
        // the pushDispatchPc side effect and would pollute the dispatch ring
        // with addresses the guest never actually branched to.
        if ((addr & 3u) != 0u || addr < g_ps2RecompiledFunctionTableBase ||
            addr >= g_ps2RecompiledFunctionTableEnd)
        {
            std::cerr << "[trace] 0x" << std::hex << addr
                      << " is outside the generated dense table [0x"
                      << g_ps2RecompiledFunctionTableBase << ", 0x"
                      << g_ps2RecompiledFunctionTableEnd << ") -- not traced"
                      << std::dec << std::endl;
            continue;
        }

        const uint32_t tableSlot = (addr - g_ps2RecompiledFunctionTableBase) >> 2;
        if (tableSlot >= g_ps2RecompiledFunctionTableSlotCount)
        {
            std::cerr << "[trace] 0x" << std::hex << addr << std::dec
                      << " slot out of range -- not traced" << std::endl;
            continue;
        }

        PS2Runtime::RecompiledFunction orig = g_ps2RecompiledFunctionTable[tableSlot];
        if (orig == nullptr)
        {
            // Wrapping an empty slot would mean inventing a body. Say so
            // instead: "no body at this address" is itself an answer, and a
            // silent skip would read as "the function was never called".
            std::cerr << "[trace] 0x" << std::hex << addr << std::dec
                      << " has no body in the table -- not traced (dispatch hole?)"
                      << std::endl;
            continue;
        }

        const std::size_t index = g_slotCount++;
        g_slots[index].guestAddr = addr;
        g_slots[index].orig = orig;
        g_slots[index].logCap = cap;

        if (!runtime.replaceFunction(addr, g_thunks[index]))
        {
            std::cerr << "[trace] failed to install thunk at 0x" << std::hex << addr
                      << std::dec << std::endl;
            --g_slotCount;
            continue;
        }

        std::cerr << "[trace] armed 0x" << std::hex << addr
                  << " orig=0x" << reinterpret_cast<uintptr_t>(orig) << std::dec
                  << " cap=" << cap << " slot=" << index << std::endl;
    }

    if (g_slotCount != 0u)
    {
        std::cerr << "[trace] " << g_slotCount << " address(es) armed, "
                  << g_watchCount << " watch field(s); records go to the "
                     "PS2X_PROBE_FILE sink as probe=TRACE" << std::endl;
    }
}


// ===========================================================================
// [journal] Ordered address-range guest-store journal.
//
// WHY IT LIVES IN THIS FILE AND NOT ITS OWN
// -----------------------------------------
// It was written as src/lib/Kernel/Diag/store_journal.cpp on the reasonable
// assumption that CMake GLOB_RECURSE + CONFIGURE_DEPENDS would pick a new
// file up for free. It does NOT, on this build path, and the failure is
// silent until link time:
//
//   * CONFIGURE_DEPENDS is implemented by build/CMakeFiles/VerifyGlobs.cmake,
//     which is driven by the ZERO_CHECK project.
//   * ZERO_CHECK reaches ps2_runtime only as a <ProjectReference>.
//   * build.ps1 targets a single .vcxproj with no .sln, and MSBuild does not
//     walk the reference graph in that mode -- build.ps1 says so itself at
//     the rlImGui pre-build, which exists for exactly this reason.
//
// So a new .cpp under src/lib/Kernel/ is never added to the project, never
// compiled, and shows up as unresolved externals after a full build cycle.
// Adding code to an ALREADY-LISTED TU avoids the whole problem.
// Verified 2026-09-21 (Part 154).
//
// NOTE the same trap applies to ps2x_tests: it consumes ps2_runtime.lib via
// <ProjectReference> + AdditionalDependencies, so `build.ps1 Debug -Test`
// alone links a STALE ps2_runtime.lib. Build ps2_runtime.vcxproj first.
// ===========================================================================
// [journal] Ordered address-range guest-store journal.
//
// WHY THIS EXISTS
// ---------------
// Every guest store in all ~17,000 recompiled translation units already calls
// ps2TraceGuestWrite() (ps2_runtime_macros.h), which forwards to
// ps2_watch::onGuestWrite() behind one relaxed atomic bool. That hook is
// compiled in and costs nothing when disarmed. Until now it had exactly two
// consumers, and both answer a weaker question than we keep needing to ask:
//
//   * PS2X_WATCH   -- POLLS the watched words once per frame from the render
//                     loop (ps2_watch::pollWatches). A value written and
//                     overwritten inside one frame is invisible, and the
//                     reported writerPc is only the last writer before the
//                     poll. It reports STATE TRANSITIONS, not stores.
//   * PS2X_TRAPVAL -- matches a VALUE anywhere in an address range. Retired
//                     as noise (see game_overrides.cpp, "[trapval] RETIRED").
//
// Neither can answer "in what ORDER, and from which PC, was this field
// written?" -- which is the question every use-after-free / use-after-teardown
// hypothesis reduces to. This file adds that third consumer: an ordered,
// unsampled, per-store journal over explicit address ranges.
//
// PC ACCURACY -- MEASURED, NOT ASSUMED
// ------------------------------------
// ctx->pc IS assigned per instruction by the current codegen (measured: a
// generated body has MORE ctx->pc assignments than it has instructions). The
// stale comment that once claimed otherwise is corrected in ps2_runtime.cpp
// next to onGuestWrite().
//
// Branch DELAY SLOTS were checked specifically, because that is where a
// per-instruction pc would plausibly go stale: the generated code there also
// sets ctx->in_delay_slot and ctx->branch_pc, so it LOOKS like pc might be
// left naming the branch. It is not. Measured over a 300-file sample of
// output/ (1050 delay-slot stores, 3622 stores total):
//
//   * every one of the 1050 has a ctx->pc assignment within the 5 preceding
//     lines -- 0 exceptions;
//   * in every one of the 1050, the value assigned is the STORE's own address,
//     not the branch's -- 0 mismatches.
//
// Both the resume path (`if (ctx->pc == 0xADDR)`) and the fall-through path
// assign ctx->pc = <store address> before the store, with branch_pc carrying
// the branch separately.
//
// So `pc` names the storing INSTRUCTION, for every store, and a consumer may
// match on an exact store address. (An earlier draft of this comment claimed
// 29% of stores reported the branch instead; that was wrong -- it came from
// reading too small a context window around the store.)
//
// PLACEMENT
// ---------
// src/lib/Kernel/ is globbed by CMakeLists.txt with GLOB_RECURSE
// CONFIGURE_DEPENDS, so a new file here needs no CMakeLists edit. Same
// deliberate convention as HostSampler.cpp and trace_calls.cpp: a botched
// build costs 30+ hours, so the odd placement is worth it.

// 2026-09-22 -- defined in ps2_runtime.cpp, at file scope after the anonymous
// namespace that owns the dispatch ring. Declared here rather than in a header
// because ps2_dispatch_history.h feeds the runner include graph's neighbours
// and the extern-between-.cpp form is this project's sanctioned cross-TU
// pattern (game_overrides.cpp:47-50).
extern std::size_t ps2xDispatchHistoryTail(uint32_t *out, std::size_t n) noexcept;

namespace ps2_journal
{
    // One journalled store. Mirrored into a bounded ring (below) so tests and
    // an end-of-run summary can read back ORDER without parsing the JSONL.
    struct Record
    {
        uint64_t ord = 0;
        uint64_t val = 0;
        uint32_t addr = 0;
        uint32_t size = 0;
        uint32_t pc = 0;
        uint32_t range = 0;
    };

    namespace
    {
        constexpr std::size_t kMaxRanges = 8u;
        constexpr std::size_t kLabelLen = 24u;
        constexpr uint32_t kDefaultCap = 4096u;

        struct Range
        {
            uint32_t lo = 0;
            uint32_t hi = 0;
            char label[kLabelLen] = {0};
            std::atomic<uint32_t> hits{0};
            std::atomic<bool> capAnnounced{false};
        };

        Range g_ranges[kMaxRanges];
        std::size_t g_rangeCount = 0;
        uint32_t g_cap = kDefaultCap;

        // PC-keyed arming (PS2X_JOURNAL_PC). Range-arming needs the target
        // address up front, which is useless for a HEAP object whose address
        // is not known until it exists -- and the [hole] probe does not print
        // the object address, only target/source/ra. Arming by STORING PC
        // instead needs nothing known in advance: every store from a listed
        // instruction is journalled whatever object it lands in, and the
        // object base is recoverable from the logged addr minus the field
        // offset. This is how you watch "every write to field X across all
        // instances" rather than "every write to one known address".
        constexpr std::size_t kMaxPcs = 16u;
        uint32_t g_pcs[kMaxPcs] = {0};
        std::size_t g_pcCount = 0;
        std::atomic<uint32_t> g_pcHits{0};
        std::atomic<bool> g_pcCapAnnounced{false};

        // Synthetic range id for a PC-matched record, so a consumer can tell
        // the two arming modes apart in the JSONL.
        constexpr uint64_t kPcRangeId = 0xFFFFFFFFull;

        // Monotonic store ordinal across ALL ranges. This is the whole point
        // of the journal: a total order on stores that a 60Hz poll cannot
        // reconstruct.
        std::atomic<uint64_t> g_seq{0};

        uint32_t parseNum(const char *s)
        {
            if (s == nullptr || *s == '\0')
            {
                return 0u;
            }
            return static_cast<uint32_t>(std::strtoul(s, nullptr, 0));
        }

        // Hand-rolled rather than strtok_r: that is POSIX, and this TU builds
        // for MSVC (which spells it strtok_s). Splitting in-place on a local
        // buffer keeps the parser dependency-free.
        char *splitOn(char *s, char delim, char **rest)
        {
            if (s == nullptr)
            {
                *rest = nullptr;
                return nullptr;
            }
            char *p = std::strchr(s, delim);
            if (p == nullptr)
            {
                *rest = nullptr;
            }
            else
            {
                *p = '\0';
                *rest = p + 1;
            }
            return s;
        }
    } // namespace

    // Defined here, forward-declared in ps2_runtime.cpp. Set by install();
    // ps2_runtime.cpp additionally raises ps2_watch::g_writeWatchActive, which
    // is the gate ps2TraceGuestWrite() actually tests.
    std::atomic<bool> g_armed{false};

    // --- inspection surface -------------------------------------------------
    // A bounded in-memory ring of the first kRingSize records. Two uses:
    //   1. ps2xTest can assert on ORDER without parsing the JSONL sink.
    //   2. It gives a cheap end-of-run summary without re-reading the log.
    // Only written on a range MATCH, which is rare by construction, so this
    // costs nothing on the miss path.
    namespace
    {
        constexpr std::size_t kRingSize = 64u;
        Record g_ring[kRingSize];
        std::atomic<std::size_t> g_recorded{0};
    } // namespace

    std::size_t rangeCount()
    {
        return g_rangeCount;
    }

    uint32_t hits(std::size_t i)
    {
        return (i < g_rangeCount) ? g_ranges[i].hits.load(std::memory_order_relaxed) : 0u;
    }

    std::size_t recordCount()
    {
        const std::size_t n = g_recorded.load(std::memory_order_relaxed);
        return (n < kRingSize) ? n : kRingSize;
    }

    bool recordAt(std::size_t i, Record &out)
    {
        if (i >= recordCount())
        {
            return false;
        }
        out = g_ring[i];
        return true;
    }

    // Statics here are process-lifetime, so ps2xTest needs an explicit reset
    // between cases or the tests become order-dependent.
    uint32_t pcHits()
    {
        return g_pcHits.load(std::memory_order_relaxed);
    }

    void reset()
    {
        g_armed.store(false, std::memory_order_relaxed);
        g_rangeCount = 0;
        g_cap = kDefaultCap;
        g_seq.store(0u, std::memory_order_relaxed);
        g_recorded.store(0u, std::memory_order_relaxed);
        g_pcCount = 0;
        g_pcHits.store(0u, std::memory_order_relaxed);
        g_pcCapAnnounced.store(false, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kMaxPcs; ++i)
        {
            g_pcs[i] = 0u;
        }
        for (std::size_t i = 0; i < kMaxRanges; ++i)
        {
            g_ranges[i].lo = 0;
            g_ranges[i].hi = 0;
            g_ranges[i].label[0] = '\0';
            g_ranges[i].hits.store(0u, std::memory_order_relaxed);
            g_ranges[i].capAnnounced.store(false, std::memory_order_relaxed);
        }
    }

    // PS2X_JOURNAL=LO:HI[:LABEL][,LO:HI[:LABEL]...]
    // LO/HI are hex (0x-prefixed) or decimal; the range is half-open [LO,HI).
    // Returns the number of ranges armed.
    std::size_t install(const char *spec)
    {
        const char *pcSpec = std::getenv("PS2X_JOURNAL_PC");
        const bool haveRanges = (spec != nullptr && spec[0] != '\0');
        const bool havePcs = (pcSpec != nullptr && pcSpec[0] != '\0');

        // Either arming mode alone is enough; neither means stay disarmed.
        if (!haveRanges && !havePcs)
        {
            return 0u;
        }

        g_cap = kDefaultCap;
        if (const char *c = std::getenv("PS2X_JOURNAL_MAX"); c != nullptr && c[0] != '\0')
        {
            g_cap = static_cast<uint32_t>(std::strtoul(c, nullptr, 0));
        }

        char buf[512];
        buf[0] = '\0';
        if (haveRanges)
        {
            std::strncpy(buf, spec, sizeof(buf) - 1u);
            buf[sizeof(buf) - 1u] = '\0';
        }

        char *cursor = haveRanges ? buf : nullptr;
        while (cursor != nullptr && g_rangeCount < kMaxRanges)
        {
            char *nextEntry = nullptr;
            char *entry = splitOn(cursor, ',', &nextEntry);
            cursor = nextEntry;

            if (entry == nullptr || entry[0] == '\0')
            {
                continue;
            }

            char *afterLo = nullptr;
            const char *loField = splitOn(entry, ':', &afterLo);
            char *afterHi = nullptr;
            const char *hiField = splitOn(afterLo, ':', &afterHi);
            const char *labelField = afterHi;

            const uint32_t lo = parseNum(loField);
            const uint32_t hi = parseNum(hiField);

            // Reject rather than silently normalise: an inverted or empty
            // range that quietly matched nothing would be read as "no stores
            // happened", which is the exact false negative this file exists
            // to prevent.
            if (hi <= lo)
            {
                std::cerr << "[journal] REJECTED range lo=0x" << std::hex << lo
                          << " hi=0x" << hi << std::dec
                          << " (hi must be > lo; range is half-open [lo,hi))" << std::endl;
                continue;
            }

            Range &r = g_ranges[g_rangeCount];
            r.lo = lo;
            r.hi = hi;
            r.hits.store(0u, std::memory_order_relaxed);
            r.capAnnounced.store(false, std::memory_order_relaxed);
            if (labelField != nullptr && labelField[0] != '\0')
            {
                std::strncpy(r.label, labelField, kLabelLen - 1u);
                r.label[kLabelLen - 1u] = '\0';
            }
            else
            {
                std::snprintf(r.label, kLabelLen, "r%u", static_cast<unsigned>(g_rangeCount));
            }

            std::cerr << "[journal] armed #" << g_rangeCount
                      << " lo=0x" << std::hex << r.lo
                      << " hi=0x" << r.hi << std::dec
                      << " label=" << r.label
                      << " cap=" << g_cap << std::endl;
            ++g_rangeCount;
        }

        // PS2X_JOURNAL_PC=PC[,PC...] -- journal every store issued BY these
        // instructions, whatever address they land on.
        if (havePcs)
        {
            char pcBuf[512];
            std::strncpy(pcBuf, pcSpec, sizeof(pcBuf) - 1u);
            pcBuf[sizeof(pcBuf) - 1u] = '\0';

            char *pcCursor = pcBuf;
            while (pcCursor != nullptr && g_pcCount < kMaxPcs)
            {
                char *nextPc = nullptr;
                char *tok = splitOn(pcCursor, ',', &nextPc);
                pcCursor = nextPc;

                if (tok == nullptr || tok[0] == '\0')
                {
                    continue;
                }

                const uint32_t pc = parseNum(tok);
                if (pc == 0u)
                {
                    // 0 is never a real store PC, so this is a typo, not a
                    // request to watch address 0. Say so rather than arming a
                    // filter that can never match.
                    std::cerr << "[journal] REJECTED pc=" << tok
                              << " (parsed to 0; expected a hex guest PC)" << std::endl;
                    continue;
                }

                g_pcs[g_pcCount++] = pc;
                std::cerr << "[journal] armed pc=0x" << std::hex << pc << std::dec << std::endl;
            }
        }

        if (g_rangeCount != 0u || g_pcCount != 0u)
        {
            g_armed.store(true, std::memory_order_relaxed);
        }
        return g_rangeCount + g_pcCount;
    }

    namespace
    {
        // Shared emit path for both arming modes. `rangeId` is the range index,
        // or kPcRangeId when the record was matched by storing PC.
        void emitRecord(uint64_t rangeId, uint32_t addr, uint32_t size,
                        uint64_t lo, uint64_t hi, uint32_t pc) noexcept
        {
            const uint64_t ord = g_seq.fetch_add(1u, std::memory_order_relaxed);

            // Mirror into the bounded ring before emitting, so a run killed by
            // the harness mid-write still leaves the ring consistent.
            const std::size_t slot = g_recorded.fetch_add(1u, std::memory_order_relaxed);
            if (slot < kRingSize)
            {
                Record &rec = g_ring[slot];
                rec.ord = ord;
                rec.val = lo;
                rec.addr = addr;
                rec.size = size;
                rec.pc = pc;
                rec.range = static_cast<uint32_t>(rangeId);
            }

            // 2026-09-22 -- caller chain. `pc` alone is the STORING
            // instruction, which for a hunted field is the thing already known
            // from static analysis; it cannot say who drove the store. c0..c7
            // are the most recent dispatched function entry PCs on this thread,
            // c0 being the immediately-preceding dispatch.
            //
            // Emitted as kv pairs rather than a formatted trace string so the
            // record stays in the JSONL sink and a uint32 cannot be misread as
            // a uint64 (game_overrides.cpp:168). Only reached on a journal
            // match, so the hot miss path in onStore() is unaffected.
            //
            // A short chain is normal and means the ring held fewer entries --
            // it is NOT a claim that no further callers exist. See the comment
            // on ps2xDispatchHistoryTail() in ps2_runtime.cpp for the dispatch
            // blind-spot class this inherits.
            constexpr std::size_t kChain = 8u;
            uint32_t chain[kChain] = {0};
            const std::size_t chainLen = ps2xDispatchHistoryTail(chain, kChain);

            const char *keys[7 + kChain];
            uint64_t vals[7 + kChain];
            int k = 0;
            keys[k] = "ord";
            vals[k++] = ord;
            keys[k] = "range";
            vals[k++] = rangeId;
            keys[k] = "addr";
            vals[k++] = addr;
            keys[k] = "size";
            vals[k++] = size;
            keys[k] = "val";
            vals[k++] = lo;
            keys[k] = "valhi";
            vals[k++] = hi;
            keys[k] = "pc";
            vals[k++] = pc;

            static const char *const kChainKeys[kChain] = {
                "c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"};
            for (std::size_t i = 0; i < chainLen; ++i)
            {
                keys[k] = kChainKeys[i];
                vals[k++] = chain[i];
            }

            ps2x_probe_kv("JOURNAL", k, keys, vals);
        }
    } // namespace

    // Called from ps2_watch::onGuestWrite for every guest store, but only once
    // g_armed is set. Hot: keep the miss path to a bounded integer scan.
    void onStore(uint32_t addr, uint32_t size, uint64_t lo, uint64_t hi, uint32_t pc) noexcept
    {
        const uint32_t writeEnd = addr + size;
        bool matchedRange = false;

        for (std::size_t i = 0; i < g_rangeCount; ++i)
        {
            Range &r = g_ranges[i];

            // Half-open overlap test: the store [addr, addr+size) touches any
            // byte of [r.lo, r.hi). A 64- or 128-bit store that STRADDLES the
            // range boundary counts -- dropping it would hide exactly the
            // wide-store writers we are usually hunting.
            if (addr >= r.hi || r.lo >= writeEnd)
            {
                continue;
            }

            matchedRange = true;

            const uint32_t n = r.hits.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g_cap != 0u && n > g_cap)
            {
                // Announce saturation exactly once. Without this line a capped
                // journal and a journal that never fired are indistinguishable
                // in the log.
                if (!r.capAnnounced.exchange(true, std::memory_order_relaxed))
                {
                    std::cerr << "[cap] tag=journal label=" << r.label
                              << " saturated at " << std::dec << g_cap << std::endl;
                }
                continue;
            }

            emitRecord(static_cast<uint64_t>(i), addr, size, lo, hi, pc);
        }

        // PC-keyed arming. Only consulted when no range already claimed this
        // store, so arming both modes over the same store cannot double-count.
        if (matchedRange || g_pcCount == 0u)
        {
            return;
        }

        for (std::size_t i = 0; i < g_pcCount; ++i)
        {
            if (g_pcs[i] != pc)
            {
                continue;
            }

            const uint32_t n = g_pcHits.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g_cap != 0u && n > g_cap)
            {
                if (!g_pcCapAnnounced.exchange(true, std::memory_order_relaxed))
                {
                    std::cerr << "[cap] tag=journal label=pc saturated at "
                              << std::dec << g_cap << std::endl;
                }
                return;
            }

            emitRecord(kPcRangeId, addr, size, lo, hi, pc);
            return;
        }
    }
} // namespace ps2_journal
