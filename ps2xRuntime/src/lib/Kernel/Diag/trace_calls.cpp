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
