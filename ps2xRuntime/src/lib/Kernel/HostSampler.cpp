// HostSampler.cpp -- in-process CPU sampling profiler for the host runtime.
//
// WHY THIS EXISTS
// ---------------
// [cputime] measurements (2026-07-27/28) put the runner at a steady ~118% of one
// core across three separate 60-90s runs, split over three threads (~47s, ~43s,
// ~16s), while the guest advances only ~13,400 back-edges/s. That works out to
// tens of microseconds of host CPU burned per guest back-edge -- five orders of
// magnitude off. Process-level CPU time proves the cost is real but says nothing
// about WHERE it is. Every attempt to name the culprit by reading code has been
// a guess, and two of those guesses (the DR0 watchpoint probe, the RecompDbg
// shared-memory writer) were disproved only after a build+run round trip.
//
// This samples the actual instruction pointer, so the next round trip ends the
// guessing instead of shifting it.
//
// WHY IT LIVES IN Kernel/
// ----------------------
// Not because it is kernel code -- it is not. ps2xRuntime/CMakeLists.txt:469
// globs this directory with GLOB_RECURSE + CONFIGURE_DEPENDS into the same
// ps2_runtime target, so a new file here is picked up with no CMakeLists edit.
// Given that a botched build costs 30+ hours, that is worth the odd placement.
// It also cannot live in ps2_runtime.cpp: that TU includes raylib, whose
// CloseWindow/ShowCursor collide with <windows.h>.
//
// HOW IT SAMPLES
// --------------
// A background thread wakes every PS2X_PROFILE_MS (default 5) and, for each
// other thread in this process: SuspendThread -> GetThreadContext (RIP) ->
// GetThreadTimes -> ResumeThread. Nothing is allocated while a thread is
// suspended.
//
// A raw RIP histogram would count sleeping threads as busy, so samples are
// weighted by the thread's CPU-time delta since the previous tick and dropped
// entirely when that delta is zero. The result is a true CPU-time profile whose
// per-thread totals should reconcile with the [cputime] table -- that agreement
// is the sanity check on the whole measurement. GetThreadTimes has ~15.6ms
// granularity, so individual attributions are coarse; only the aggregate means
// anything, which is fine when we are hunting something that eats whole cores.
//
// The report prints itself from the sampler thread after PS2X_PROFILE_SECS
// rather than waiting for shutdown, because launch_recomp.ps1 auto-stop calls
// CloseMainWindow() and then Kill()s shortly after -- a shutdown-only report
// could be killed mid-symbol-resolution and lose the entire run.
//
// Enable with PS2X_PROFILE=1. Off by default and costs nothing when off.

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{

// One sampled thread. `weight` accumulates 100ns CPU units, so the per-address
// histogram is denominated in real CPU time rather than tick counts.
struct ThreadSamples
{
    std::string name;
    uint64_t totalWeight = 0;
    std::unordered_map<uint64_t, uint64_t> byRip;
};

struct TrackedThread
{
    HANDLE handle = nullptr;
    uint64_t lastCpu100ns = 0;
    bool primed = false; // first tick only establishes a baseline
};

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reported{false};
std::thread g_thread;

std::unordered_map<DWORD, ThreadSamples> g_samples;
uint64_t g_ticks = 0;

uint64_t threadCpu100ns(HANDLE h)
{
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(h, &creation, &exit, &kernel, &user))
        return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

// Thread names are set all over this runtime (ThreadNaming.h), and they are the
// difference between "thread 23656 burns 47s" and "the guest executor burns 47s".
std::string threadName(HANDLE h)
{
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static GetThreadDescriptionFn fn = []() -> GetThreadDescriptionFn {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 ? reinterpret_cast<GetThreadDescriptionFn>(
                         GetProcAddress(k32, "GetThreadDescription"))
                   : nullptr;
    }();
    if (!fn)
        return {};

    PWSTR desc = nullptr;
    if (FAILED(fn(h, &desc)) || !desc)
        return {};
    std::string out;
    for (PWSTR p = desc; *p; ++p)
        out.push_back(static_cast<char>(*p < 128 ? *p : '?'));
    LocalFree(desc);
    return out;
}

// Rebuilt periodically rather than every tick: enumeration is far more expensive
// than the sampling itself, and this process does not churn threads.
void refreshThreads(std::unordered_map<DWORD, TrackedThread> &tracked)
{
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    std::vector<DWORD> live;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            live.push_back(te.th32ThreadID);
            if (tracked.find(te.th32ThreadID) != tracked.end())
                continue;

            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h)
                continue;
            TrackedThread t;
            t.handle = h;
            tracked[te.th32ThreadID] = t;
            g_samples[te.th32ThreadID].name = threadName(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    for (auto it = tracked.begin(); it != tracked.end();)
    {
        if (std::find(live.begin(), live.end(), it->first) == live.end())
        {
            CloseHandle(it->second.handle);
            it = tracked.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::string moduleOf(HANDLE proc, uint64_t addr)
{
    IMAGEHLP_MODULE64 mi{};
    mi.SizeOfStruct = sizeof(mi);
    if (SymGetModuleInfo64(proc, addr, &mi))
        return mi.ModuleName;
    return "?";
}

std::string symbolOf(HANDLE proc, uint64_t addr)
{
    alignas(8) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym))
    {
        char out[MAX_SYM_NAME + 64];
        std::snprintf(out, sizeof(out), "%s+0x%llx", sym->Name,
                      static_cast<unsigned long long>(disp));
        return out;
    }
    return {};
}

void report(double windowSec)
{
    if (g_reported.exchange(true))
        return;

    // Cheap, order-independent facts first, flushed as they go: if a kill lands
    // during the (slow, PDB-loading) symbol pass, the per-thread totals and the
    // module breakdown still make it out.
    std::printf("[hostprof] ==== CPU sample report: %.1fs window, %llu ticks ====\n",
                windowSec, static_cast<unsigned long long>(g_ticks));

    std::vector<const std::pair<const DWORD, ThreadSamples> *> ordered;
    for (const auto &kv : g_samples)
        if (kv.second.totalWeight > 0)
            ordered.push_back(&kv);
    std::sort(ordered.begin(), ordered.end(), [](auto *a, auto *b) {
        return a->second.totalWeight > b->second.totalWeight;
    });

    for (const auto *e : ordered)
        std::printf("[hostprof]   tid %-6lu %8.2fs  %s\n",
                    static_cast<unsigned long>(e->first),
                    static_cast<double>(e->second.totalWeight) / 1e7,
                    e->second.name.empty() ? "(unnamed)" : e->second.name.c_str());
    std::fflush(stdout);

    // GitHub Copilot (Claude Sonnet 5), 2026-07-28: dbghelp allows exactly one
    // live SymInitialize() per process handle. game_overrides.cpp's hardware
    // watchpoint armer (hwWatchArmerMain, wired unconditionally onto every
    // rpc_call/0x178BE8 entry -- see hwWatchArm callers) already calls
    // SymInitialize(GetCurrentProcess(), ..., TRUE) with no matching
    // SymCleanup, so by the time this report runs dbghelp is already
    // initialized process-wide. Calling SymInitialize again then fails with
    // ERROR_INVALID_PARAMETER -- this used to be read as "no symbols
    // available" and silently degraded every [hostprof] report to
    // module="?" with the entire per-symbol breakdown skipped (observed in
    // the 2026-07-28 t=82s/t=96s runs: 100% "module ?", zero named symbols
    // on every thread). The pre-existing initialization is still fully
    // usable -- fInvadeProcess=TRUE already loaded every module for the
    // whole process -- so treat "already initialized elsewhere" as success
    // rather than failure.
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS);
    const BOOL initOk = SymInitialize(proc, nullptr, TRUE);
    const bool haveSyms = initOk != FALSE || GetLastError() == ERROR_INVALID_PARAMETER;

    const size_t kMaxThreads = 4;   // deeper is noise; the top few hold the cost
    const size_t kMaxResolve = 300; // bounds the PDB work against the kill timer
    const size_t kMaxPrint = 12;

    for (size_t ti = 0; ti < ordered.size() && ti < kMaxThreads; ++ti)
    {
        const DWORD tid = ordered[ti]->first;
        const ThreadSamples &ts = ordered[ti]->second;

        std::printf("[hostprof]\n[hostprof] -- tid %lu (%s) %.2fs CPU, %zu distinct addrs\n",
                    static_cast<unsigned long>(tid),
                    ts.name.empty() ? "(unnamed)" : ts.name.c_str(),
                    static_cast<double>(ts.totalWeight) / 1e7, ts.byRip.size());

        std::vector<std::pair<uint64_t, uint64_t>> hot(ts.byRip.begin(), ts.byRip.end());
        std::sort(hot.begin(), hot.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });

        // Module attribution covers every sample -- it is a range lookup, not a
        // PDB query, so it is safe to run over the whole histogram.
        std::unordered_map<std::string, uint64_t> byModule;
        for (const auto &h : hot)
            byModule[haveSyms ? moduleOf(proc, h.first) : "?"] += h.second;

        std::vector<std::pair<std::string, uint64_t>> mods(byModule.begin(), byModule.end());
        std::sort(mods.begin(), mods.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < mods.size() && i < 6; ++i)
            std::printf("[hostprof]      module %-28s %5.1f%%\n", mods[i].first.c_str(),
                        100.0 * static_cast<double>(mods[i].second) /
                            static_cast<double>(ts.totalWeight));
        std::fflush(stdout);

        if (!haveSyms)
            continue;

        // Merge by symbol: one hot function spread across its own instructions
        // would otherwise look like dozens of unrelated addresses.
        std::unordered_map<std::string, uint64_t> bySym;
        for (size_t i = 0; i < hot.size() && i < kMaxResolve; ++i)
        {
            std::string name = symbolOf(proc, hot[i].first);
            if (name.empty())
            {
                char raw[64];
                std::snprintf(raw, sizeof(raw), "0x%llx",
                              static_cast<unsigned long long>(hot[i].first));
                name = moduleOf(proc, hot[i].first) + "!" + raw;
            }
            else
            {
                // Collapse the +0xNN so all of a function's instructions merge.
                const size_t plus = name.rfind("+0x");
                if (plus != std::string::npos)
                    name.resize(plus);
                name = moduleOf(proc, hot[i].first) + "!" + name;
            }
            bySym[name] += hot[i].second;
        }

        std::vector<std::pair<std::string, uint64_t>> syms(bySym.begin(), bySym.end());
        std::sort(syms.begin(), syms.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < syms.size() && i < kMaxPrint; ++i)
            std::printf("[hostprof]      %5.1f%%  %8.2fs  %s\n",
                        100.0 * static_cast<double>(syms[i].second) /
                            static_cast<double>(ts.totalWeight),
                        static_cast<double>(syms[i].second) / 1e7, syms[i].first.c_str());
        std::fflush(stdout);
    }

    std::printf("[hostprof] ==== end report ====\n");
    std::fflush(stdout);
}

void samplerMain(int intervalMs, double reportAfterSec)
{
    std::unordered_map<DWORD, TrackedThread> tracked;
    const auto started = std::chrono::steady_clock::now();
    auto lastRefresh = started - std::chrono::seconds(10);

    while (!g_stop.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRefresh > std::chrono::seconds(1))
        {
            refreshThreads(tracked);
            lastRefresh = now;
        }

        ++g_ticks;
        for (auto &kv : tracked)
        {
            TrackedThread &t = kv.second;

            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;

            if (SuspendThread(t.handle) == static_cast<DWORD>(-1))
                continue;
            const bool gotCtx = GetThreadContext(t.handle, &ctx) != FALSE;
            const uint64_t cpu = threadCpu100ns(t.handle);
            ResumeThread(t.handle);

            // Everything below runs with the target already resumed; allocating
            // (the histogram insert) while it is suspended could deadlock on the
            // CRT heap lock.
            if (!gotCtx)
                continue;

            if (!t.primed)
            {
                t.primed = true;
                t.lastCpu100ns = cpu;
                continue;
            }
            const uint64_t delta = cpu > t.lastCpu100ns ? cpu - t.lastCpu100ns : 0;
            t.lastCpu100ns = cpu;
            if (delta == 0)
                continue; // thread was parked this interval -- not a CPU sample

            ThreadSamples &s = g_samples[kv.first];
            s.totalWeight += delta;
            s.byRip[ctx.Rip] += delta;
        }

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (reportAfterSec > 0.0 && elapsed >= reportAfterSec && !g_reported.load())
            report(elapsed);

        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    for (auto &kv : tracked)
        CloseHandle(kv.second.handle);
}

} // namespace

extern "C" void ps2x_host_sampler_start(void)
{
    const char *en = std::getenv("PS2X_PROFILE");
    if (!en || !*en || *en == '0')
        return;

    int intervalMs = 5;
    if (const char *ms = std::getenv("PS2X_PROFILE_MS"))
    {
        const int v = std::atoi(ms);
        if (v > 0)
            intervalMs = v;
    }

    double reportAfter = 60.0;
    if (const char *sec = std::getenv("PS2X_PROFILE_SECS"))
    {
        const double v = std::atof(sec);
        if (v > 0.0)
            reportAfter = v;
    }

    std::printf("[hostprof] sampling every %dms, auto-report at %.0fs "
                "(PS2X_PROFILE_MS / PS2X_PROFILE_SECS)\n",
                intervalMs, reportAfter);
    std::fflush(stdout);

    g_thread = std::thread(samplerMain, intervalMs, reportAfter);
}

extern "C" void ps2x_host_sampler_stop(void)
{
    if (!g_thread.joinable())
        return;
    g_stop.store(true, std::memory_order_relaxed);
    g_thread.join();
    if (!g_reported.load())
        report(0.0);
}

#else // !_WIN32

extern "C" void ps2x_host_sampler_start(void) {}
extern "C" void ps2x_host_sampler_stop(void) {}

#endif
