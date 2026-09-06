#include "game_overrides.h"
#include "Kernel/recovered/recovered_functions.h"
#include "fn_forward_decls.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"
#include "ps2_runtime_calls.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

// 2026-07-27 HWWATCH support (see the HWWATCH block below). Kept to kernel32 +
// dbghelp so no CMake link edit is needed; the pragma is the same mechanism
// main_gui.cpp already uses. Guarded defines because ps2_runtime.h may have
// already pulled windows.h in transitively.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// [frametrace] ring recorder, defined in ps2_runtime.cpp. Declared here rather
// than in a header on purpose: any .h edit forces a full rebuild of all 30,000+
// generated runner TUs (30+ hours). extern-between-.cpp is the sanctioned
// cross-TU pattern in this project.
void ps2FrameTraceRecord(uint32_t funcStart, uint32_t entryPc, uint32_t entrySp,
                         uint32_t exitPc, uint32_t exitSp, uint32_t exitRa) noexcept;

// Guest-progress counter (ps2_scheduler.cpp): one tick per 128 guest back-edges.
// Same extern-between-.cpp rule as above.
extern "C" uint64_t ps2x_guest_progress();

// Delivered-vblank counter (Kernel/Syscalls/Interrupt.cpp). A plain relaxed
// atomic load -- no CRT, no lock -- which is why the HWWATCH VEH below may call
// it. It is the only frame index available to a probe, and HWWATCH needs one:
// "in what order were these stores made" is only answerable once the hits are
// segmented into frames.
extern "C" uint64_t ps2x_vblank_ticks();

// Phase C stack-bounds guard (Kernel/Syscalls/Thread.cpp), which owns the
// per-tid stack ranges recorded at StartThread. Returns 1 when sp is outside
// the running fiber's own stack and emits one bounded STACKOOB record. site is
// 0 at slot entry, 1 at slot exit. Self-disabling for threads that never went
// through StartThread, so it is safe to leave armed in every run.
extern "C" int ps2x_stack_check(uint32_t pc, uint32_t sp, uint32_t site);

// Guest interrupt-disable preemption gate (2026-07-26; ported to
// Kernel/EeScheduler.cpp in Phase 3d, retiring ps2_scheduler.cpp).
// SDBZ brackets its shared-structure critical sections with the EE kernel's
// DisableIntr (0x17ed60, `cop0 0x39`) / EnableIntr (0x17edb0, `cop0 0x38`).
// The runtime has no COP0 Status EIE bit, so those calls used to be invisible
// to the scheduler and yield_point could hand the guest token to the IRQ
// worker mid-section; under EeScheduler the equivalent gate lives in
// checkpointDue(). The wrappers for those two addresses call these to tell
// the scheduler when the guest considers interrupts masked; the depth/escape
// accessors are for the CRITSEC probe below. Same extern-between-.cpp rule.
extern "C" void ps2x_guest_intr_disable_enter();
extern "C" void ps2x_guest_intr_disable_leave();
extern "C" uint32_t ps2x_guest_intr_disable_depth();
extern "C" uint64_t ps2x_guest_intr_disable_escapes();
extern "C" uint64_t ps2x_guest_intr_disable_sections();
extern "C" uint64_t ps2x_guest_intr_disable_redundant();
extern "C" uint64_t ps2x_guest_intr_disable_stray();

// Stage 5.17 sreg probe: how many times the IOP actually issued SIF_CMD
// SET_SREG. Defined in Kernel/Stubs/SIF.cpp -- declared here rather than in a
// header, per the .cpp-pair rule (a header touch rebuilds all ~4,520 runner
// TUs).
extern "C" uint64_t ps2x_iop_setsreg_calls();

// ---------------------------------------------------------------------------
// Probe sink (Phase B, 2026-07-26)
// ---------------------------------------------------------------------------
// One structured output channel for every diagnostic probe, replacing the 14
// ad-hoc "[frametrace:NAME] k=0x.. k=0x.." std::cerr families.
//
// WHY THIS EXISTS. The old probes wrote to stderr, which the launcher tees
// through a PowerShell pipe. That pipe has twice produced wrong ANSWERS, not
// just ugly output:
//   * Tee-Object writes UTF-16 under PS 5.1 and UTF-8 under pwsh 7, so a
//     signature harness silently scored five EMPTY signatures as a PASS.
//   * The console wraps long lines, so one probe record becomes two physical
//     lines and a line-oriented parser glues unrelated records together.
// A dedicated file descriptor, opened once and never routed through a shell,
// removes both permanently. ASCII-only JSONL, one complete record per line.
//
// Every record carries four automatic fields before the caller's own:
//   seq      global emission order (the only totally-ordered clock we have)
//   tid      host thread hash -- probes from different fibers interleave
//   progress guest-progress ticks, i.e. WHEN in guest execution this happened
//   probe    the family name
//
// Values are emitted as hex STRINGS ("0x1c0728") without exception, including
// counters. Uniformity is deliberate: the parser does int(v, 16) on every
// value and never has to know which key is an address and which is a count.
//
// Flushed per record on purpose. The harness kills the run with a hard 25s
// timeout, so anything sitting in a buffer at that moment is lost -- and the
// interesting records are the ones just before the kill.
// Defined in ps2_runtime.cpp so every target that links ps2_runtime resolves it
// without depending on this TU. Written by sdbzRunCallbackList13C4F8 below,
// read by the watchdog line.
extern std::atomic<uint32_t> g_sdbzCb13C4F8InFlight;

namespace
{
    std::mutex g_probeMutex;
    std::FILE *g_probeFile = nullptr;
    bool g_probeOpenTried = false;
    std::atomic<uint64_t> g_probeSeq{0};

    // Returns nullptr if the sink is disabled or could not be opened; callers
    // must treat that as "probing off" rather than an error. Caller holds
    // g_probeMutex.
    std::FILE *probeFile()
    {
        if (!g_probeOpenTried)
        {
            g_probeOpenTried = true;
            const char *path = std::getenv("PS2X_PROBE_FILE");
            if (path == nullptr || path[0] == '\0')
                path = "run_probe.jsonl";
            // "w" not "a": each run owns its file. Appending across runs is how
            // stale records from a previous build get mistaken for current ones.
            g_probeFile = std::fopen(path, "w");
            if (g_probeFile == nullptr)
            {
                std::cerr << "[probe] could not open sink '" << path
                          << "' -- probes disabled for this run\n";
            }
            else
            {
                std::cerr << "[probe] sink open: " << path << "\n";
            }
        }
        return g_probeFile;
    }
} // namespace

// C-ABI so any other .cpp can use it with a single extern line and no header.
// Keys/values are parallel arrays rather than varargs: varargs would silently
// accept a uint32_t where a uint64_t is read and corrupt the record.
extern "C" void ps2x_probe_kv(const char *name, int n,
                              const char *const *keys, const uint64_t *vals)
{
    std::lock_guard<std::mutex> lock(g_probeMutex);
    std::FILE *f = probeFile();
    if (f == nullptr)
        return;

    const uint64_t seq = g_probeSeq.fetch_add(1, std::memory_order_relaxed);
    const uint32_t tid = static_cast<uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);

    std::fprintf(f,
                 "{\"seq\":\"0x%llx\",\"tid\":\"0x%x\",\"progress\":\"0x%llx\",\"probe\":\"%s\"",
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned>(tid),
                 static_cast<unsigned long long>(ps2x_guest_progress()),
                 name != nullptr ? name : "?");
    for (int i = 0; i < n; ++i)
    {
        std::fprintf(f, ",\"%s\":\"0x%llx\"",
                     keys[i] != nullptr ? keys[i] : "?",
                     static_cast<unsigned long long>(vals[i]));
    }
    std::fputs("}\n", f);
    std::fflush(f);
}

// Recompiler boundary-detection gap: neither the recompiler nor an
// independent IDA analysis of the EE ELF places a function boundary at
// these addresses; both are unconditional-jump ("j") targets that land
// between two recognized functions. A direct MIPS jump cannot cross into
// IOP address space, so these are EE-side, not IOP. Defined here as
// diagnostic stubs (log once, return) until the real function bodies are
// recovered. If/when the correct owning function is identified, prefer
// ps2_game_overrides::bindAddressHandler / PS2Runtime::replaceFunction to
// redirect the dispatch table instead of editing runner output.
void fn_151830_0x151830(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    RUNTIME_LOG("[game_overrides] fn_151830_0x151830 stub hit at pc=0x" << std::hex << ctx->pc << std::dec);
}

void fn_170268_0x170268(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    RUNTIME_LOG("[game_overrides] fn_170268_0x170268 stub hit at pc=0x" << std::hex << ctx->pc << std::dec);
}

void fn_11ABA0_0x11aba0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    RUNTIME_LOG("[game_overrides] fn_11ABA0_0x11aba0 stub hit at pc=0x" << std::hex << ctx->pc << std::dec);
}

// Missing-body dispatch hole, same class as above but with a known root
// cause and a real body instead of a stub. 0x17ee80-0x17eebc sits in the
// func-map gap between syscall_stub_z_34 (ends 0x17ee48) and
// syscall_stub_z_35 (starts 0x17eec0); IDA's boundary scan never carved it
// out because nothing reaches it via a static jal/j -- the guest only
// reaches it indirectly, by registering it as its own syscall 0x83 handler
// via SetSyscall (see PS2_PROJECT_STATE.md HANDOFF 2026-08-28 part 10/11).
// Disassembled by hand (mips_r5900_disassembler.py "ELF/SLUS_214.42"
// 0x17ee38 40): a compiler-unrolled word-scan loop, semantically
// `for (a0 = start; a0 < end; a0 += 4) if (*a0 == target) return a0; return 0;`
// with a0=$a0 (start), a1=$a1 (end, exclusive), a2=$a2 (target word). The
// two `movz $a0,$zero,$v0` merge points look asymmetric in the disassembly
// but both only fire when the preceding sltu found the scan already out of
// range, so they never clobber a genuine match -- a plain loop is a faithful
// translation, not just an approximation.
void fn_17EE80_0x17ee80(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;

    uint32_t addr = GPR_U32(ctx, 4);
    const uint32_t end = GPR_U32(ctx, 5);
    const uint32_t target = GPR_U32(ctx, 6);

    uint32_t result = 0u;
    while (addr < end)
    {
        if (READ32(addr) == target)
        {
            result = addr;
            break;
        }
        addr += 4u;
    }

    SET_GPR_U32(ctx, 2, result);
    ctx->pc = GPR_U32(ctx, 31);
}

// Stage 5.10 [poolbase] probe: singleton_get_camera_0x199db0 is a generic
// singleton-pool accessor (name is a stale Ghidra auto-label, not literally
// "camera"). GameInit calls it once and zero-fills 0xC000 bytes starting at
// its return pointer. 0x8a6440 -- the guest address confirmed by HWWATCH to
// be zero-filled once at boot and never written again -- may fall inside
// this pool. Pass-through wrapper: calls the real function, then logs the
// returned base pointer and its offset from 0x8a6440 so we can tell whether
// this pool owns that address.
void sdbzPoolBaseProbe199DB0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    singleton_get_camera_0x199db0(rdram, ctx, runtime);
    const uint32_t base = GPR_U32(ctx, 2);
    const int64_t offset = static_cast<int64_t>(0x8a6440u) - static_cast<int64_t>(base);
    RUNTIME_LOG("[poolbase] base=0x" << std::hex << base
        << " target=0x8a6440 offset=0x" << offset
        << " inRange=" << std::dec << ((offset >= 0 && offset < 0xC000) ? 1 : 0));
}

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t size)
    {
        static std::array<uint32_t, 256> table = []()
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
                }
                values[i] = c;
            }
            return values;
        }();

        uint32_t out = crc;
        for (size_t i = 0; i < size; ++i)
        {
            out = table[(out ^ data[i]) & 0xFFu] ^ (out >> 8u);
        }
        return out;
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        std::array<uint8_t, 4096> chunk{};
        uint32_t crc = 0xFFFFFFFFu;

        while (file.good())
        {
            file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = file.gcount();
            if (got <= 0)
            {
                break;
            }
            crc = crc32Update(crc, chunk.data(), static_cast<size_t>(got));
        }

        crcOut = ~crc;
        return true;
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}

// Defined in Kernel/Diag/trace_calls.cpp. Declared here rather than in a
// header on purpose: ps2_runtime.h is included by every generated runner TU,
// so touching it costs a 30+ hour rebuild. Same cross-TU pattern as the
// [frametrace] symbols above.
void ps2xTraceCallsInstall(PS2Runtime &runtime);

namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime, const std::string &elfPath, uint32_t entry)
    {
        ps2_syscalls::clearSoundDriverCompatLayout();
        ps2_syscalls::clearDtxCompatLayout();

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        // No early return on an empty registry: the [trace] install at the end
        // of this function must run whether or not any override matched, and a
        // loop over an empty vector already does nothing.

        const std::string elfName = basenameFromPath(elfPath);
        uint32_t fileCrc32 = 0u;
        bool fileCrcComputed = false;
        bool fileCrcValid = false;

        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcComputed)
                {
                    fileCrcComputed = true;
                    fileCrcValid = computeFileCrc32(elfPath, fileCrc32);
                    if (!fileCrcValid)
                    {
                        std::cerr << "[game_overrides] failed to compute CRC32 for '" << elfPath << "'" << std::endl;
                    }
                }

                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }

        // LAST, deliberately. The [trace] tracer snapshots whatever pointer is
        // in the dispatch table and tail-calls it, so it must see the table in
        // its final, post-override state -- otherwise it would either wrap a
        // generated body that an override later replaced (tracing a function
        // the guest no longer runs) or be clobbered by that override outright.
        // No-ops unless PS2X_TRACE_CALLS is set. Defined in
        // Kernel/Diag/trace_calls.cpp.
        ps2xTraceCallsInstall(runtime);
    }
}

namespace
{
    void applyRecvxSoundDriverCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain a bit of Resident Evil Code: Veronica X sound-driver guest globals.
        // Update these guest addresses/callback PCs when porting the override to another build:
        // - checksum tables back the SE/MIDI status values mirrored through the snddrv RPC stubs
        // - busyFlagAddr is the guest-side "work in progress" word cleared on completion
        // - completion/clearBusy callbacks are guest PCs reached when async snddrv work finishes
        PS2SoundDriverCompatLayout layout{};
        layout.primarySeCheckAddr = 0x01E0EF10u;
        layout.primaryMidiCheckAddr = 0x01E0EF20u;
        layout.fallbackSeCheckAddr = 0x01E1EF10u;
        layout.fallbackMidiCheckAddr = 0x01E1EF20u;
        layout.busyFlagAddr = 0x01E212C8u;
        layout.completionCallbacks = {0x002EAC20u, 0x002EAC30u, 0x002FAC20u, 0x002FAC30u};
        layout.clearBusyCallbacks = {0x002EAC30u, 0x002FAC30u};

        // SID + subcommand (fno) numbers RE:CVX's sound driver speaks; carried per-game
        // so its getStatus RPC provisions the status/addr-table pool the
        // sceSifGetOtherData checksum backfill depends on. The submit path (SID 0 /
        // fno 0, never a live service) is intentionally left unconfigured.
        layout.stateSid = 1u;
        layout.getStatusFno = 0x12u;
        layout.getAddrTableFno = 0x13u;
        ps2_syscalls::setSoundDriverCompatLayout(layout);
    }

    void applyRecvxDtxCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain abit of Resident Evil Code: Veronica X DTX guest layout.
        // Update these guest values when porting the middleware override to another build:
        // - rpcSid identifies the DTX RPC service the guest binds/registers
        // - urpc object/table addresses back the SJX/PS2RNA/SJRMT command tables
        // - dispatcherFuncAddr is the guest-side DTX RPC handler used for URPC dispatch
        PS2DtxCompatLayout layout{};
        layout.rpcSid = 0x7D000000u;
        layout.urpcObjBase = 0x01F18000u;
        layout.urpcObjLimit = 0x01F1FF00u;
        layout.urpcObjStride = 0x20u;
        layout.urpcFnTableBase = 0x0034FED0u;
        layout.urpcObjTableBase = 0x0034FFD0u;
        layout.dispatcherFuncAddr = 0x002FABC0u;
        ps2_syscalls::setDtxCompatLayout(layout);
    }

    void applyLotrSoundRpcCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        PS2SoundDriverCompatLayout layout{};
        layout.completionCallbacks = {0x001FFD70u, 0u, 0u, 0u};
        ps2_syscalls::setSoundDriverCompatLayout(layout);
    }

    // Kernel store-word/eret thunk at 0x17F5D0 — recompiler truncated it to one
    // instruction (mfc0) with no pc advance, livelocking the dispatch loop.
    // Real body (recovered from raw ELF bytes + sibling wrapper thunks at
    // 0x17F640/17F650/17F660): di-guarded Status dance, sw $a1,0($a0),
    // mtc0 $ra,ErrorEPC, eret. Net semantics: *(u32*)a0 = a1; return to ra.
    void sdbzKernelStoreWordEret(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        WRITE32(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // EE kernel syscall thunk table (0x174880..0x174FF0, 120 stubs, 16 bytes
    // each): "addiu $v1,$zero,N; syscall; jr $ra; nop". The recompiler folded
    // most of them into the preceding function body with no dense-table entry
    // of their own, so an indirect call (jalr) into one dies with "No exact
    // recompiled function for guest PC 0x1748a0" (first hit: trace
    // 0x171fd8 -> 0x172168 -> 0x174fe0 -> 0x1748a0, ra=0x1057fc). Register the
    // whole table so any thunk reached through the dispatch loop works.
    // Negative entries are the from-interrupt-context variants; addiu
    // sign-extends, so $v1 must carry the sign-extended value — the numeric
    // dispatcher already has static_cast<uint32_t>(-N) cases for them.
    // Semantics mirror the generated code for a bare syscall stub exactly:
    // set $v1, handleSyscall (2-arg form == encoded id 0 == read $v1),
    // then jr $ra.
    constexpr uint32_t kSdbzSyscallThunkBase = 0x00174880u;
    constexpr int16_t kSdbzSyscallThunkNums[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 252, 253, -26, -27, -28, -29,
        -254, -255, 32, 33, 34, 35, 36, 37, -38, 39, 40, 41, -42, 43, -44, 45,
        -46, 47, 48, -49, 50, 51, -52, 53, -54, 55, -56, 57, -58, 59, 60, 61,
        62, 63, 64, 65, 66, -67, 68, 69, -70, 71, -72, 73, 74, 75, 76, 77,
        78, 79, 80, 81, 82, -83, 84, -85, 86, 87, -88, 89, -90, 91, 92, -92,
        93, -93, 94, -94, 95, -95, 96, 97, 98, 99, 100, 102, -103, -104, -106, 107,
        108, 109, 110, 111, 112, -112, 113, -113};
    constexpr size_t kSdbzSyscallThunkCount =
        sizeof(kSdbzSyscallThunkNums) / sizeof(kSdbzSyscallThunkNums[0]);

    template <size_t I>
    void sdbzSyscallThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_S64(ctx, 3, static_cast<int64_t>(kSdbzSyscallThunkNums[I]));
        runtime->handleSyscall(rdram, ctx);
        ctx->pc = GPR_U32(ctx, 31);
    }

    template <size_t... Is>
    void registerSdbzSyscallThunks(PS2Runtime &runtime, std::index_sequence<Is...>)
    {
        (runtime.replaceFunction(kSdbzSyscallThunkBase + static_cast<uint32_t>(Is) * 16u,
                                 &sdbzSyscallThunk<Is>),
         ...);
    }

    // Handler-registration leaf at 0x104BF0 — second confirmed instance of the
    // same truncation class as sdbzKernelStoreWordEret above. The recompiler
    // emitted 1 of 4 instructions (header says "0x104bf0 - 0x104bf4", body is
    // just the `lui $v0, 0x50`), dropping the store, the `jr $ra` and the
    // return value. The body then falls off its own end with ctx->pc still
    // 0x104bf0, so the dispatch loop re-dispatches it forever — this is the
    // 2026-07-19 boot hang (watchdog: pc=0x104bf0 ra=0x421c84 stuckSecs=30).
    //
    // Real function, straight from the ELF:
    //   0x104bf0  lui   $v0, 0x50
    //   0x104bf4  sw    $a0, 12848($v0)   ; -> 0x503230 (gp+0x1c0, gp=0x503070)
    //   0x104bf8  jr    $ra
    //   0x104bfc  addiu $v0, $zero, 0x1   ; return 1
    //
    // Reached by tail-call `j 0x104bf0` from sub_1BFB60 (itself called at
    // 0x421c7c), which is why $ra reads 0x421c84 — $ra passes through a tail
    // call unchanged. fn_1BFB60's generated body carries a hasFunction() guard
    // before the tail call, so it routes through lookupFunction and this
    // replaceFunction override is honoured.
    void sdbzRegisterHandler104BF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        WRITE32(0x00503230u, GPR_U32(ctx, 4));
        SET_GPR_S32(ctx, 2, 1);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // --- C++ global ctor/dtor thunks missed by function discovery (2026-07-20) --
    // Measured, not inferred. Both addresses faulted the dispatcher
    // ("No exact recompiled function for guest PC 0x1bfdb0 / 0x1bfb80") and
    // neither has a table slot, because the recompiler discovers functions by
    // following `jal` targets. These two are never the target of a `jal`: their
    // addresses are materialised as `lui`+`addiu` register pairs and passed as
    // *function pointers*. A 32-bit scan of the ELF finds no word equal to
    // either address precisely because the value is split across two
    // instructions and never exists as data.
    //
    // Construction site, 0x1bffe4 - 0x1c0004:
    //   lui/addiu $a0 = 0x5ad760   (array base)
    //   lui/addiu $a1 = 0x1bfdb0   (ctor)      <- this function
    //   lui/addiu $a2 = 0x1bfd00   (dtor)
    //   addiu     $a3 = 0x30       (element size)
    //   jal       0x171c30         (array-construct helper; jalr's $a1 per elem)
    //   addiu     $t0 = 0x10       (element count)
    //
    // The fault's [gpr] dump read a0=0x5ad760 a2=0x1bfd00 a3=0x30 t0=0x10 --
    // an exact match for those arguments -- and 0x1bfdb0 faulted 16 times,
    // exactly $t0. That pins the call site beyond doubt.
    //
    // Body straight from the ELF (0x1bfdb0 - 0x1bfdd0):
    //   lui   $v0, 0x4e
    //   sw    $zero, 8($a0)
    //   addiu $v0, $v0, 0x7930     ; vtable 0x4E7930
    //   sw    $zero, 4($a0)
    //   sw    $v0, 0($a0)
    //   daddu $v0, $a0, $zero      ; return this
    //   sb    $zero, 12($a0)
    //   jr    $ra
    //   sw    $zero, 44($a0)       ; delay slot -- last word of the 0x30 element
    //
    // Vtable 0x4E7930 is already fully registered in every populated slot, so
    // only the constructor itself was missing.
    void sdbzCtor1BFDB0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4);
        WRITE32(self + 0u, 0x004E7930u);
        WRITE32(self + 4u, 0u);
        WRITE32(self + 8u, 0u);
        WRITE8(self + 12u, 0u);
        WRITE32(self + 44u, 0u);
        SET_GPR_S32(ctx, 2, (int32_t)self);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Companion global-destructor thunk, 0x1bfb80:
    //   j     0x1bfa50
    //   daddu $a0, $zero, $zero    ; delay slot -- $a0 = 0 (destroy pass)
    //
    // This is the GCC `_GLOBAL__D_*` shape: $a0=0 destroys, $a0=1 constructs.
    // It is registered for later execution at 0x1bfb64 - 0x1bfb70, which loads
    // $a0 = 0x1bfb80 and tail-jumps into the atexit-style registrar at
    // 0x104bf0 -- the same leaf already overridden above.
    //
    // Tail call, so $ra passes through untouched: hand control to 0x1bfa50 via
    // the table rather than re-entering the dispatch loop at a dead PC.
    void sdbzDtorThunk1BFB80(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_S32(ctx, 4, 0); // delay slot: $a0 = 0
        if (PS2Runtime::RecompiledFunction target = runtime->lookupFunction(0x001BFA50u))
        {
            ctx->pc = 0x001BFA50u;
            target(rdram, ctx, runtime);
            return;
        }
        ctx->pc = 0x001BFA50u;
    }

    // --- Dropped-function class, instance 2 (2026-07-21) ----------------------
    // Same bug shape as sdbzCtor1BFDB0 above, and now understood as a CLASS:
    // the recompiler's function discovery walks a function to its `jr $ra`, hits
    // the trailing `nop` alignment padding, treats that as end-of-function, and
    // resumes scanning at the next *statically discovered* start. Any function
    // that begins immediately after the padding and is reached only indirectly
    // (`jalr $reg`) is skipped entirely and lands in no dispatch slot.
    //
    // Here fn_390DE0 is two instructions (0x390de0 - 0x390de8), followed by two
    // nops at 0x390de8/0x390dec, and the next registered function is 0x390e10.
    // 0x390df0 falls in that hole. grep of register_functions.cpp: zero hits.
    // Runtime fault: "No exact recompiled function for guest PC 0x390df0".
    //
    // Sole caller is the indirect `jalr $s5` at 0x171d04 inside fn_171C30, which
    // is why nothing static ever pointed at it.
    //
    // Body straight from the ELF (0x390df0 - 0x390e0c):
    //   lui   $v0, 0x4f
    //   sw    $zero, 8($a0)
    //   addiu $v0, $v0, 0x67e0     ; vtable 0x4F67E0
    //   sw    $zero, 4($a0)
    //   sw    $v0, 0($a0)
    //   jr    $ra
    //   daddu $v0, $a0, $zero      ; delay slot -- return this
    //
    // An offline scan (padding-gap heuristic vs the full 375,485 registered
    // dispatch addresses) puts ~150 more candidates in this class binary-wide.
    // They are NOT fixed here: none is known to be on the boot path, and hand
    // translating them speculatively would cost the same single build while
    // adding ~150 chances to introduce a transcription error. The fault is loud
    // and names its own address, so the next one that matters will announce
    // itself. Do not batch-fix without evidence of reachability.
    void sdbzCtor390DF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4);
        WRITE32(self + 0u, 0x004F67E0u);
        WRITE32(self + 4u, 0u);
        WRITE32(self + 8u, 0u);
        SET_GPR_S32(ctx, 2, (int32_t)self);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // --- Dropped-function class, instance 3 (2026-07-21) ----------------------
    // Same padding-gap bug as sdbzCtor390DF0 above. Found in the SAME run log:
    // 254 faults vs 0x390df0's 110, so this one is the hotter of the two.
    //
    // Body from the ELF (0x38d990 - 0x38d9a0):
    //   lui   $v0, 0x4f
    //   addiu $v0, $v0, 0x67f0     ; vtable 0x4F67F0
    //   sw    $v0, 64($a0)         ; secondary vtable slot, offset 0x40
    //   jr    $ra
    //   daddu $v0, $a0, $zero      ; delay slot -- return this
    //
    // Writes ONLY at +64 and calls no base constructor: this is the secondary
    // vtable pointer of a multiply-inherited class whose primary ctor runs
    // separately. Do not zero any other field -- the primary ctor owns them, and
    // clearing them here would undo work already done.
    //
    // Vtable 0x4F67F0 sits 16 bytes after 0x4F67E0 (sdbzCtor390DF0's), i.e. the
    // two are sibling subobjects of the same class.
    void sdbzCtor38D990(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4);
        WRITE32(self + 64u, 0x004F67F0u);
        SET_GPR_S32(ctx, 2, (int32_t)self);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Leaf getter at 0x1C0170 -- not a missing table slot, a genuine gap in the
    // recompiler's function-boundary discovery. It sits nop-padded between
    // pool_entry_pop_i (ends 0x1c0168) and sub_1C0180 (starts 0x1c0180); no
    // runner/*.cpp, no forward decl, and no register_functions.cpp entry exist
    // for it at all -- the func-map generator never saw it as a jal target.
    // Straight from the ELF:
    //   0x1c0170  jr    $ra
    //   0x1c0174  lw    $v0, 0($a0)   ; delay slot -- return *(uint32_t*)a0
    void sdbzLeaf1C0170(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_S32(ctx, 2, (int32_t)READ32(GPR_U32(ctx, 4)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Draw-order comparator at 0x2FF580 -- same boundary-discovery gap class as
    // sdbzLeaf1C0170, but this one silently corrupted every frame of rendering.
    //
    // It is nop-padded between an unnamed body and sub_2FF5C0, and is NEVER a jal
    // target: its address only ever appears as the 4th argument of the qsort call
    // at 0x2FF800, so the func-map generator never saw it. No runner/*.cpp exists.
    //
    // The scene renderer sub_2FF5C0 sorts its display objects before emitting:
    //   0x2ff688  a1 = [gp-0x1ED4]            ; N = live object count
    //   0x2ff690  if (N < 2) skip the sort
    //   0x2ff7f8  a0 = 0x5D6EC0               ; array of object pointers
    //   0x2ff7fc  a2 = 4                      ; element size
    //   0x2ff800  jal  sub_18FCE8             ; qsort
    //   0x2ff804  a3 = 0x2FF580               ; delay slot -- the comparator
    // then walks the sorted array calling each object's vtable slot +0x2C, which
    // is what appends that object's primitives to the command buffer. So the
    // comparator alone fixes the guest's draw order.
    //
    // With no body registered, the JALR inside qsort hits dispatchGuestBranch's
    // missing-target path under MissingFunctionPolicy::ContinueToTarget: it logs
    // once, does NOT unwind, and resumes after the call with $v0 untouched --
    // i.e. the comparator "returns" whatever qsort happened to leave in $v0
    // (observed: 1). A comparator that answers "greater" for every pair yields a
    // deterministic but meaningless permutation. Live proof, run_log.txt:
    //   [guest-branch:missing-target] kind=IndirectCall op=JALR
    //     source=0x19002c target=0x2ff580 a0=0x5d6ec0 a1=0x5d6ed4 a2=0x4
    //     a3=0x2ff580 v0=0x1 policy=1
    // (source 0x19002c is inside sub_18FCE8, 0x18fce8-0x1906d4; a0/a1 are two
    // slots of the 0x5D6EC0 array, 0x14 apart.) It is the only distinct missing
    // target in the whole log.
    //
    // Straight from the ELF (0x2ff580 - 0x2ff5bc), delay slots resolved:
    //   lw    $v1, 0($a1)          ; objB = *b
    //   lw    $v0, 0($a0)          ; objA = *a
    //   lwc1  $f1, 0x18($v1)       ; keyB
    //   lwc1  $f0, 0x18($v0)       ; keyA
    //   c.le.s $f0, $f1
    //   bc1f  0x2ff5b4             ; !(keyA <= keyB) -> return +1
    //   li    $v0, 1               ; delay slot
    //   c.lt.s $f0, $f1
    //   bc1t  0x2ff5ac             ; keyA < keyB  -> v0 stays 1
    //   li    $v0, 1               ; delay slot
    //   dmove $v0, $zero           ; keyA == keyB -> v0 = 0
    //   andi  $v0, 0xFF
    //   subu  $v0, $zero, $v0      ; negate: 1 -> -1, 0 -> 0
    //   jr    $ra
    // Ascending sort on the float at object offset +0x18 (a depth/priority key).
    // The !(a <= b) form is kept deliberately rather than (a > b): both MIPS
    // compares are ordered, so a NaN key must fall out as +1, which (a > b)
    // would report as 0 (2026-08-09).
    void sdbzDrawOrderCompare2FF580(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t objA = READ32(GPR_U32(ctx, 4));
        const uint32_t objB = READ32(GPR_U32(ctx, 5));

        const uint32_t bitsA = READ32(objA + 0x18u);
        const uint32_t bitsB = READ32(objB + 0x18u);

        float keyA, keyB;
        std::memcpy(&keyA, &bitsA, sizeof(keyA));
        std::memcpy(&keyB, &bitsB, sizeof(keyB));

        int32_t result;
        if (!(keyA <= keyB))
        {
            result = 1;
        }
        else if (keyA < keyB)
        {
            result = -1;
        }
        else
        {
            result = 0;
        }

        SET_GPR_S32(ctx, 2, result);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // SetHostLock at 0x12F708 -- boundary-discovery gap again, but reached by a
    // DIRECT `j`, which is a shape we had not seen before and which fails in a
    // way the previous two did not.
    //
    // 0x132430 is a five-instruction thunk whose last real instruction is
    //   0x13243c  j 0x12f708      (tail call, delay slot 0x132440)
    // IDA folded the callee's body into the thunk and named the pair
    // noop_wrapper___270, so the func map lists only 0x132430-0x132444 and has
    // NOTHING between 0x12f704 and 0x12f760. No runner/*.cpp exists for
    // 0x12f708, so hasFunction() says no and dispatchGuestBranch takes the
    // ContinueToTarget path: it logs once, sets pc = 0x12f708, and returns TRUE.
    // The generated thunk then runs its own last line, `ctx->pc = 0x132444u`.
    //
    // 0x132444 is the nop that pads the thunk out to the next function, so it is
    // not an entry in any table -- the dispatch loop misses on it, the miss does
    // not advance pc, and it re-misses forever. That is the boot-screen spin:
    //   [guest-branch:missing-target] kind=DirectJump op=J source=0x13243c
    //     target=0x12f708 pc=0x12f708 ra=0x113f50 policy=1
    // followed by 1.35 BILLION reports of "guest PC 0x132444" (2026-08-10).
    //
    // Note the terminus address is NOT the missing function -- the one and only
    // [guest-branch:missing-target] line is what names 0x12f708. A dispatch-miss
    // pc that sits one instruction past a func-map end is the fall-through, not
    // the cause; read the missing-target record before chasing the spin address.
    //
    // Straight from the ELF (0x12f708 - 0x12f758), all 21 words field-decoded,
    // delay slots resolved:
    //   lui   $v1, 0x45 ; addiu $v1, -11660      ; v1 = 0x44D274
    //   sll   $v0, $a0, 24 ; sra $v0, $v0, 24    ; v0 = (int8_t)a0
    //   addiu $sp, $sp, -16 ; sd $ra, 0($sp)
    //   lui   $a2, 0x45                          ; a2 = 0x450000
    //   li    $a1, 1
    //   sw    $v0, 0($v1)                        ; *0x44D274 = (int8_t)a0
    //   lw    $v0, 0($v1)                        ; reload
    //   bne   $a0, $a1, 0x12f750
    //   sw    $v0, -11612($a2)                   ; delay slot -- *0x44D2A4 = v0
    //   lui   $a0, 0x4c ; addiu $a0, -23720      ; a0 = 0x4BA358
    //   ld    $ra, 0($sp)
    //   j     0x177988                           ; tail call, varargs printf
    //   addiu $sp, $sp, 16                       ; delay slot
    // 0x12f750: ld $ra, 0($sp) ; jr $ra ; addiu $sp, $sp, 16
    //
    // 0x4BA358 is the string "SRD: Enable HostLock\r\n" and 0x177988 is the
    // va_start/disable-interrupts print helper, so this is a debug-trace setter:
    // it stores the flag and announces only the enable transition.
    //
    // The sd/ld $ra pair and the -16/+16 on $sp are both net-neutral; they are
    // reproduced anyway so the red-zone bytes below $sp match the real function.
    void sdbzSetHostLock12F708(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t flagArg = GPR_U64(ctx, 4);
        const int32_t flagByte = (int32_t)(int8_t)(uint8_t)flagArg;

        const uint32_t frame = ADD32(GPR_U32(ctx, 29), 0xFFFFFFF0u);
        WRITE64(frame, GPR_U64(ctx, 31));
        SET_GPR_U64(ctx, 31, READ64(frame));

        WRITE32(0x0044D274u, (uint32_t)flagByte);
        const uint32_t stored = READ32(0x0044D274u);
        WRITE32(0x0044D2A4u, stored);

        SET_GPR_S32(ctx, 2, (int32_t)stored);

        // BNE compares the full 64-bit registers.
        if (flagArg != 1u)
        {
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }

        SET_GPR_S32(ctx, 4, (int32_t)0x004BA358u);
        ctx->pc = 0x00177988u;
        runtime->dispatchGuestBranch(rdram, ctx, 0x00177988u, 0x0012F744u, 0u,
                                     PS2Runtime::GuestBranchKind::DirectJump, "J");
    }

    // --- Boundary-discovery gap, instance 4 (2026-08-11, Stage 5.13 run 45) ---
    // Same shape as sdbzSetHostLock12F708 above, one screen further on. With
    // 0x12f708 registered the Atari-loading spin moved to guest PC 0x13c718 --
    // again the *fall-through*, not the fault: 0x13c718 is one word past the
    // func-map END of the thunk at 0x13c700. The one-shot record named the hole:
    //
    //   [guest-branch:missing-target] kind=DirectJump op=J
    //     source=0x13c668 target=0x13c4f8 policy=1
    //
    // The func map jumps 0x13c4f4 -> 0x13c5c0, so 0x13c4f8 has no body. It is
    // the shared tail of EIGHT sibling thunks at 0x13c658..0x13c710, each of
    // which only loads its own index into $a0 and jumps here:
    //
    //   0x13c658: $a0 = 0 ... 0x13c710: $a0 = 7
    //
    // 0x13c4f8 itself is a callback-list runner. Two parallel arrays:
    //   0x0054E960 + idx*72 -- six {fn, arg, pad} records of 12 bytes
    //   0x0045EFE8 + idx*4  -- "a callback is executing" flag (1 during, 0 after)
    //   0x0045EFC8 + idx*4  -- per-index invocation counter, bumped on the way out
    // It ORs every callback's return value together and returns that.
    //
    // All 21 words of the ELF body were field-decoded before translating.
    // Two details that a casual read gets wrong:
    //   - `addiu $s0, $s0, 0xc` sits in the BEQ's delay slot, so the record
    //     pointer advances even for a null callback slot.
    //   - the loop-back is `bgezl` (branch LIKELY), so its `lw $v0, 0($s0)`
    //     delay slot is skipped on the final, not-taken test.
    // $s0-$s5 and $ra are saved and restored by the original, so they are held
    // in C locals here and the frame stores are replayed for byte fidelity.
    void sdbzRunCallbackList13C4F8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t idx = GPR_U32(ctx, 4);
        const uint32_t frame = ADD32(GPR_U32(ctx, 29), 0xFFFFFFC0u); // addiu $sp, -0x40

        WRITE64(frame + 0u, GPR_U64(ctx, 16));  // sd $s0, 0($sp)
        WRITE64(frame + 8u, GPR_U64(ctx, 17));  // sd $s1, 8($sp)
        WRITE64(frame + 16u, GPR_U64(ctx, 18)); // sd $s2, 16($sp)
        WRITE64(frame + 24u, GPR_U64(ctx, 19)); // sd $s3, 24($sp)
        WRITE64(frame + 32u, GPR_U64(ctx, 20)); // sd $s4, 32($sp)
        WRITE64(frame + 40u, GPR_U64(ctx, 21)); // sd $s5, 40($sp)
        WRITE64(frame + 48u, GPR_U64(ctx, 31)); // sd $ra, 48($sp)
        SET_GPR_U32(ctx, 29, frame);

        const uint32_t byIndex = idx * 4u;                  // sll $s4, $a0, 2
        const uint32_t busyFlag = 0x0045EFE8u + byIndex;
        uint32_t record = 0x0054E960u + idx * 72u;          // ((idx<<3)+idx)<<3

        uint64_t accumulated = 0;
        for (int slot = 5; slot >= 0; --slot)
        {
            const uint32_t callback = READ32(record + 0u);
            const uint32_t argument = READ32(record + 4u);
            record = ADD32(record, 12u); // delay slot: unconditional

            if (callback == 0u)
            {
                continue;
            }

            WRITE32(busyFlag, 1u);
            // Host-side mirror of busyFlag that also records *which* callback.
            // The watchdog prints it as cb=; a nonzero value on a parked
            // watchdog names the callback that entered and never returned,
            // zero rules this loop out. One store per callback, so unlike a
            // per-call log line it cannot flood and cannot be capped.
            g_sdbzCb13C4F8InFlight.store(callback, std::memory_order_relaxed);
            SET_GPR_S32(ctx, 4, (int32_t)argument);
            SET_GPR_U64(ctx, 31, 0x0013C568u); // jalr $ra, $v0 -- return to the OR
            ctx->pc = callback;
            if (PS2Runtime::RecompiledFunction target = runtime->lookupFunction(callback))
            {
                target(rdram, ctx, runtime);
            }
            else
            {
                runtime->dispatchGuestBranch(rdram, ctx, callback, 0x0013C560u, 0x0013C568u,
                                             PS2Runtime::GuestBranchKind::IndirectCall, "JALR");
            }
            g_sdbzCb13C4F8InFlight.store(0u, std::memory_order_relaxed);
            WRITE32(busyFlag, 0u);
            accumulated |= GPR_U64(ctx, 2); // or $s3, $s3, $v0 -- 64-bit
        }

        const uint32_t counter = ADD32(0x0045EFC8u, byIndex);
        const uint32_t bumped = ADD32(READ32(counter), 1u);
        WRITE32(counter, bumped);

        SET_GPR_U64(ctx, 16, READ64(frame + 0u));
        SET_GPR_U64(ctx, 17, READ64(frame + 8u));
        SET_GPR_U64(ctx, 18, READ64(frame + 16u));
        SET_GPR_U64(ctx, 19, READ64(frame + 24u));
        SET_GPR_U64(ctx, 20, READ64(frame + 32u));
        SET_GPR_U64(ctx, 21, READ64(frame + 40u));
        SET_GPR_U64(ctx, 31, READ64(frame + 48u));
        SET_GPR_U32(ctx, 29, ADD32(frame, 0x40u));

        SET_GPR_S32(ctx, 4, (int32_t)counter); // $a0 and $v1 are epilogue scratch
        SET_GPR_S32(ctx, 3, (int32_t)bumped);
        SET_GPR_U64(ctx, 2, accumulated);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Boundary-discovery hole, instance 5: 0x116CD0, reached by a direct `j` from
    // two tail-call thunks (0x116d24 and 0x120a1c) that both restore $ra and drop
    // their frame first. Run 46 spun on 0x116d2c, which is one word past the END of
    // wrap_wrap_mem_set_h (0x116cf0..0x116d2c) -- the fall-through again, not the
    // fault; the one-shot [guest-branch:missing-target] named the real target.
    // The func map has a gap from 0x116cb0 to 0x116ce0 and 0x116cd0 sits inside it.
    //
    // The whole function is three instructions:
    //   0x116cd0  lui $v0, 0x44                  ; $v0 = 0x00440000
    //   0x116cd4  jr  $ra
    //   0x116cd8  sw  $a0, -13428($v0)           ; DELAY SLOT -- runs unconditionally,
    //                                           ; base is the lui result, so the
    //                                           ; address is 0x440000-0x3474 = 0x43CB8C
    //
    // It is the setter half of a pair: get_global_var_2 at 0x116ce0 is
    // `lui $v1,0x44 / jr $ra / lw $v0,-13428($v1)` -- the same word. $v0 is left
    // holding the lui result, which no caller uses but is cheap to reproduce.
    // ---- part 68 (09-04): who latches g36, and is the close ever reached? ---
    //
    // RETRACTION FIRST.  Part 67's d6n-vs-w6tick ratio test was INVALID.  It
    // assumed the nested run_class(6) reached through 0x154950 would bump the
    // per-class counter at 0x45EFC8+cls*4.  It does not.  0x154950 tail-jumps
    // to 0x13c448, a bare single-slot dispatcher (table 0x54EBA0; load fnp,
    // jalr, return) that bumps NOTHING; the counter is bumped at 0x13c5b0,
    // inside the other dispatcher 0x13c4f8.  So d6n ~= w6tick holds whether or
    // not the bracket is ever entered, and the observed 1:1 tested nothing.
    //
    // What IS established, from the part-63 HWWATCH run: the last g36=1 write
    // has no matching clear for the remainder of the run.  g36 is latched.
    // Unresolved: which bracket left it open, and why the close never ran.
    //
    // 0x1555a0 is the ONLY writer and is 15 instructions:
    //     s0=a0; s1=a1; v0=f_14e4d0(); if (s0) [s0+92]=s1; [v0+36]=s1
    // and 0x14e4d0 is a pure constant -- lui/jr/addiu, v0 = 0x45F678, no loads
    // and no side effects -- so inlining it below is exact, not an estimate.
    //
    // ra names the call site uniquely.  Static read finds exactly four:
    //     0x14e934 = bracket A open   (sub_14E8B0, a0=0,   a1=1)
    //     0x14e948 = bracket A close  (sub_14E8B0, a0=0,   a1=0)
    //     0x155650 = bracket C open   (sub_155630, a0=obj, a1=1)
    //     0x155664 = bracket C close  (sub_155630, a0=obj, a1=0)
    // Any other ra means a fifth caller the static sweep missed, which would
    // itself be the answer.
    //
    // nest = [0x45F670] (= ctx-8), the counter sub_14E8B0 decrements at
    // 0x14e8dc; unless it lands exactly on 0 at 0x14e8e4 the entire body --
    // bracket A included -- is skipped to the early-out at 0x14e9d0.  Printed
    // signed, because the Stage 5.17 movie gate was blocked by exactly this
    // shape going negative ([0x45EFC0] = -2).
    //
    // prev = g36 as it reads BEFORE this store, so open/close pairs and any
    // double-open show up as a transition instead of being inferred.
    void sdbzTraceSetG36_1555A0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t val = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);

        static uint32_t s_g36Calls = 0;
        static const uint32_t kG36LogMax = 512u;
        ++s_g36Calls;
        if (s_g36Calls <= kG36LogMax)
        {
            std::cerr << "[g36set] #" << std::dec << s_g36Calls
                      << " ra=0x" << std::hex << ra
                      << " obj=0x" << obj << std::dec
                      << " val=" << val
                      << " prev=" << READ32(0x0045F69Cu)
                      << " nest=" << (int32_t)READ32(0x0045F670u)
                      << " wbusy=" << READ32(0x00441924u)
                      << std::endl;
            if (s_g36Calls == kG36LogMax)
            {
                // [capped_probes_false_negatives]: once this fires, a missing
                // close in the log stops being evidence that none happened.
                std::cerr << "[cap] tag=g36set saturated at " << kG36LogMax
                          << std::endl;
            }
        }

        if (obj != 0u)
        {
            WRITE32(obj + 92u, val);
        }
        WRITE32(0x0045F678u + 36u, val);

        ctx->pc = ra;
    }


    void sdbzSetGlobalVar116CD0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U64(ctx, 2, 0x00440000u);
        WRITE32(0x0043CB8Cu, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // ---- The 0x460F10 flag block: one CLUSTER of holes, not one hole ---------
    //
    // Stage 5.14 run 50. With the SJX creates served the game finally left
    // ps2rna_init_psj, and parked instead at guest PC 0x14facc with
    // "dispatch-miss x406300000". That x-number is a COUNT (406 million), not an
    // address -- ps2_runtime.cpp prints a running total every 100000 misses once
    // the first 8 reports are suppressed. So this is the dispatch-hole shape,
    // not another deliberate error loop.
    //
    // 0x14facc is the fall-through, as always. The func map reads:
    //   noop_wrapper___, 0x0014fab8, 0x0014facc, 0x14   <- a 5-word thunk
    // and that thunk is
    //   0x14fab8  addiu $sp, $sp, -0x10
    //   0x14fabc  sd    $ra, 0($sp)
    //   0x14fac0  ld    $ra, 0($sp)
    //   0x14fac4  j     0x14fc88
    //   0x14fac8  addiu $sp, $sp, 0x10      ; delay slot
    // reached by `jal 0x14fab8` at 0x14e590 -- which is exactly the observed
    // ra=0x14e598. Its target 0x14fc88 has no body: the map jumps
    // 0x14fc48..0x14fc88 straight to 0x14fcb0, leaving 0x40 bytes unclaimed.
    //
    // Rather than fix that one hole and spend a build cycle discovering the next,
    // the whole enclosing module was swept. 0x14fa58..0x150008 is a small flag
    // module over four words at 0x00460F10..0x00460F1C, and it has FIVE holes,
    // every one of them a two-to-five instruction accessor. They are all
    // translated below. Only 0x14fc88 is proven reachable; the other four cost
    // nothing and are the obvious next parks if left alone.
    //
    // Note each hole's map start is a padding `nop` belonging to the previous
    // function, with the real entry one word later. Callers could name either,
    // so both addresses are registered to the same body -- the leading nop is a
    // no-op in the translation regardless.

    // 0x14fc88 (hole 0x14fc88..0x14fcb0) -- the run-50 blocker. Resets all four
    // flags, arming the first:
    //   lui $a2,0x46 / addiu $v0,$zero,1 / lui $v1,0x46 / lui $a0,0x46
    //   lui $a1,0x46 / sw $v0,3856($a2) / sw $zero,3860($v1)
    //   sw $zero,3864($a0) / jr $ra / sw $zero,3868($a1)   <- DELAY SLOT store
    // Four separate `lui`s into a0/a1/a2/v1 for what is one base address; the
    // original leaves all of them holding 0x00460000 and v0 holding 1.
    void sdbzResetFlags14FC88(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U64(ctx, 4, 0x00460000u);  // $a0
        SET_GPR_U64(ctx, 5, 0x00460000u);  // $a1
        SET_GPR_U64(ctx, 6, 0x00460000u);  // $a2
        SET_GPR_U64(ctx, 3, 0x00460000u);  // $v1
        SET_GPR_S32(ctx, 2, 1);            // $v0

        WRITE32(0x00460F10u, 1u);
        WRITE32(0x00460F14u, 0u);
        WRITE32(0x00460F18u, 0u);
        WRITE32(0x00460F1Cu, 0u); // delay slot -- runs unconditionally
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x14fc00 (hole 0x14fbfc..0x14fc28) -- normalises $a0 to a 0/1 flag:
    //   0x14fc00  bne   $a0, $zero, 0x14fc18
    //   0x14fc04  lui   $v1, 0x46          ; DELAY SLOT -- runs either way
    //   0x14fc08  lui   $v0, 0x46
    //   0x14fc0c  jr    $ra
    //   0x14fc10  sw    $zero, 3856($v0)   ; DELAY SLOT
    //   0x14fc18  addiu $v0, $zero, 1
    //   0x14fc1c  jr    $ra
    //   0x14fc20  sw    $v0, 3856($v1)     ; DELAY SLOT
    // BNE compares the full 64-bit register, so the test is on the whole $a0.
    // On the zero path $v0 is left holding the lui result rather than 0 -- no
    // caller is known to read it, but it is one assignment to stay faithful.
    void sdbzSetArmedFlag14FC00(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U64(ctx, 3, 0x00460000u); // $v1, from the delay slot

        if (GPR_U64(ctx, 4) != 0u)
        {
            SET_GPR_S32(ctx, 2, 1);
            WRITE32(0x00460F10u, 1u);
        }
        else
        {
            SET_GPR_U64(ctx, 2, 0x00460000u);
            WRITE32(0x00460F10u, 0u);
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x14fc38 (hole 0x14fc34..0x14fc48) -- setter for the second word:
    //   lui $v0, 0x46 / jr $ra / sw $a0, 3860($v0)
    void sdbzSetFlag14FC38(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U64(ctx, 2, 0x00460000u);
        WRITE32(0x00460F14u, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x14fe88 (hole 0x14fe84..0x14fe98) -- setter for the fourth word. Its
    // getter half, get_global_var_z_3 at 0x14fe98, already has a body.
    //   lui $v0, 0x46 / jr $ra / sw $a0, 3868($v0)
    void sdbzSetFlag14FE88(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U64(ctx, 2, 0x00460000u);
        WRITE32(0x00460F1Cu, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x14ff78 (hole 0x14ff78..0x14ff80) -- `jr $ra / nop`, wedged between the
    // two mapped no-ops noop_u (0x14ff70) and noop_v (0x14ff80). Genuinely does
    // nothing; it touches no register, so nothing is reproduced but the return.
    void sdbzNoop14FF78(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
    }

    // ---- The 0x144100 mem-routine module: memcpy + reverse memset -------------
    //
    // Stage 5.14 run 51. The 0x460F10 flag-block sweep held -- the 0x14facc spin
    // and the dispatch-miss flood are both gone -- and the game ran far enough to
    // push real work at the GS (gif/s=46 dma/s=59 at t=1s) before dying here.
    //
    // Unlike run 50 this park left exactly ONE structured record, which names the
    // fault directly instead of the fall-through:
    //   [guest-branch:missing-target] kind=DirectJump op=J
    //     source=0x1641a8 target=0x1443e0 ra=0x15df30 a0=0x560730 a1=0x0
    // and the caller sets the arguments in the clear:
    //   0x164194  lui   $a0, 0x56
    //   0x16419c  addiu $a0, $a0, 0x730     ; a0 = 0x560730   (matches the record)
    //   0x1641a0  daddu $a1, $zero, $zero   ; a1 = 0          (matches)
    //   0x1641a4  addiu $a2, $zero, 0x48    ; a2 = 0x48
    //   0x1641a8  j     0x1443e0            ; TAIL call -- $ra already reloaded
    // i.e. memset(0x560730, 0, 0x48). Because it is a `j` after `ld $ra`, the
    // callee returns straight past 0x1641a8 to 0x15df30, which is the observed ra.
    //
    // Map gap: mem_fill_s_0 ends 0x1443dc, next entry sub_1444B0 at 0x1444b0.
    // Both holes below are translated literally -- loop for loop, store for store
    // -- rather than as a call to host memcpy/memset. These are the two functions
    // every other routine in the game leans on, so an approximation that is right
    // about the bytes but wrong about a pointer would corrupt silently and be far
    // more expensive to find than the clean park it replaced.

    // 0x144100 (hole 0x1440fc..0x1442d0) -- forward byte memcpy(a0=dst, a1=src,
    // a2=n):
    //   0x144100  andi $v1, $a2, 0x1f      ; v1 = n & 31
    //   0x144104  beql $v1, $zero, 0x144130
    //   0x144108  srl  $a2, $a2, 5         ; DELAY SLOT, branch-LIKELY: taken only
    //   ...byte loop, v1 iterations, lbu/sb...
    //   0x14412c  srl  $a2, $a2, 5         ; the not-taken path's copy
    //   0x144130  beq  $a2, $zero, 0x1442c8
    //   ...block loop: 32 lb/sb pairs, two `addiu $a0,$a0,0x10`, one a2 decrement
    //
    // The 32-bytes-per-iteration figure is measured, not assumed: the loop body
    // 0x144138..0x1442c4 holds exactly 32 `sb` and exactly two `addiu $a0,0x10`,
    // which is what makes the `n >> 5` block count consistent.
    //
    // NOTE the `beql` at 0x144104 is branch-likely, so its delay slot runs ONLY
    // when the branch is taken. Both paths still end up doing `a2 >>= 5` exactly
    // once (0x144108 when taken, 0x14412c when not), so the shift is unconditional
    // in effect -- but it is unconditional for that reason, not by accident.
    void sdbzMemCopy144100(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t dst = GPR_U32(ctx, 4);  // $a0
        uint32_t src = GPR_U32(ctx, 5);  // $a1
        uint32_t n = GPR_U32(ctx, 6);    // $a2

        uint32_t tail = n & 0x1Fu; // $v1
        for (; tail != 0u; --tail)
        {
            WRITE8(dst, READ8(src));
            ++src;
            ++dst;
        }

        uint32_t blocks = n >> 5; // $a2
        for (; blocks != 0u; --blocks)
        {
            for (uint32_t i = 0u; i < 32u; ++i)
            {
                WRITE8(dst + i, READ8(src + i));
            }
            src += 32u;
            dst += 32u;
        }

        // End state, as the original leaves it: a0/a1 walked to the end of their
        // buffers, a2 and v1 drained to zero by their loop counters.
        SET_GPR_U64(ctx, 4, dst);
        SET_GPR_U64(ctx, 5, src);
        SET_GPR_S32(ctx, 6, 0);
        SET_GPR_S32(ctx, 3, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x1443e0 (hole 0x1443dc..0x1444b0) -- the run-51 blocker. A REVERSE byte
    // memset(a0=dst, a1=byte, a2=n): it seeks to dst+n first and fills downward.
    //   0x1443e0  sll  $a1, $a1, 24
    //   0x1443e4  andi $v0, $a2, 0xf       ; v0 = n & 15
    //   0x1443e8  sra  $a1, $a1, 24        ; a1 = sign-extended low byte
    //   0x1443ec  beq  $v0, $zero, 0x144414
    //   0x1443f0  addu $a0, $a0, $a2       ; DELAY SLOT -- a0 = dst + n, always
    //   ...tail loop, v0 iterations: pre-decrement a0, then sb...
    //   0x144414  srl  $a2, $a2, 4         ; blocks of 16
    //   0x144418  beq  $a2, $zero, 0x1444a8
    //   ...block loop: 16 pre-decrement/sb pairs, one a2 decrement...
    //   0x1444a8  jr   $ra
    //
    // Direction is invisible in the result for a fill, but it is reproduced anyway
    // so the end-state pointer is right: every decrement is accounted for, so a0
    // lands back exactly on dst (dst+n, minus (n&15), minus 16*(n>>4)).
    void sdbzMemFill1443E0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t dst = GPR_U32(ctx, 4);          // $a0
        int32_t byteVal = (int32_t)GPR_U32(ctx, 5);
        byteVal = (int32_t)(int8_t)(byteVal & 0xFF); // sll 24 / sra 24
        uint32_t n = GPR_U32(ctx, 6);            // $a2

        uint8_t b = (uint8_t)(byteVal & 0xFF);
        uint32_t p = dst + n; // the delay slot runs on both paths

        for (uint32_t t = n & 0xFu; t != 0u; --t)
        {
            --p;
            WRITE8(p, b);
        }

        for (uint32_t blocks = n >> 4; blocks != 0u; --blocks)
        {
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                --p;
                WRITE8(p, b);
            }
        }

        // a0 has walked all the way back down to dst; v0 and a2 are drained.
        SET_GPR_U64(ctx, 4, p);
        SET_GPR_S32(ctx, 5, byteVal);
        SET_GPR_S32(ctx, 6, 0);
        SET_GPR_S32(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // --- Hole on the libcdvd RPC completion path (2026-08-17, Stage 5.15 run 61) ---
    //
    // Run 61 opened MOVIE/ATARI.SFD for the first time (handle 0x54c060 latched
    // at 0x125898) and then parked: the ADX stream slot sat at act=1 st=2 pau=1
    // bsy=1 cmd=0 unchanged from t=129s to t=238s. The one-shot record named the
    // reason:
    //
    //   [guest-branch:missing-target] kind=IndirectCall op=JALR
    //     source=0x1785cc target=0x186310 pc=0x186310 ra=0x1785d4 v1=0x8000000a
    //
    // 0x1785cc is inside sub_178560, the EE SIF-RPC completion dispatcher. It
    // reads the packet's command word (v1=0x8000000a == RPC_CALL_END), takes the
    // client-data pointer from packet[7], and JALRs client[7] -- the caller's
    // `end_function` -- with client[8] as its argument. libcdvd registers 0x186310
    // there, so with no body at 0x186310 the completion callback silently never
    // ran and every async libcdvd request stayed busy forever.
    //
    // 0x186310 is a func-map GAP, not a truncated body: sub_186270 ends at
    // 0x18630c and the next mapped row is sub_1863C8, so 0xb8 bytes have no
    // dispatch slot at all. Disassembled from the ELF:
    //
    //   0x186310  lui   $a1, 0x2000          ; a1 = 0x20000000
    //   0x186314  or    $a3, $a0, $a1        ; a3 = packet | uncached mirror
    //   0x186318  lw    $v0, 0($a3)          ; count1
    //   0x18631c  blez  $v0, 0x186370
    //   0x186320  daddu $a2, $zero, $zero    ; DELAY SLOT: i = 0, both paths
    //   0x186324  lw    $t1, 8($a3)          ; dst1
    //   0x186330  addiu $t0, $a3, 0x10       ; src1 = packet + 0x10
    //   ...loop: lbu from t0+i, sb to BOTH (dst1|0x20000000)+i and dst1+i,
    //      reloading the count from 0($a3) every iteration...
    //   0x186378  blez  $v1, 0x1863c0        ; v1 = count2 from 4($a3)
    //   0x186380  lw    $t1, 12($a3)         ; dst2
    //   0x186390  addiu $t0, $a3, 0x50       ; src2 = packet + 0x50
    //   ...same loop, count reloaded from 4($a3)...
    //   0x1863c0  j     0x186100
    //   0x1863c4  addiu $a0, $t3, 0x32a0     ; DELAY SLOT: a0 = 0x4632a0
    //
    // So: two byte-wise copy-outs from an inline payload in the RPC packet to two
    // caller-supplied destinations, then a TAIL CALL (j, not jal) to sub_186100
    // with &dword_4632A0. sub_186100 is mapped and recompiled, so it is reached
    // through dispatchGuestBranch here rather than reimplemented.
    //
    // The doubled store (uncached mirror and cached alias) is one physical byte in
    // this runtime, but both are issued so the register end-state matches.
    // `$t3` is `lui 0x46` on BOTH paths (0x186334 and 0x186370), which is what
    // makes the delay-slot `addiu $a0, $t3, 0x32a0` land on 0x4632a0 even when
    // neither copy loop runs.
    void sdbzCdvdRpcEnd186310(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t packet = GPR_U32(ctx, 4);          // $a0
        const uint32_t uncached = packet | 0x20000000u;   // $a3

        // Copy 1: count at +0x00, destination pointer at +0x08, payload at +0x10.
        uint32_t i = 0u;                                  // $a2
        int32_t count1 = (int32_t)READ32(uncached + 0x00u);
        uint32_t dst1 = 0u;                               // $t1
        if (count1 > 0)
        {
            dst1 = READ32(uncached + 0x08u);
            while ((int32_t)i < count1)
            {
                const uint8_t v = READ8(uncached + 0x10u + i);
                WRITE8((dst1 | 0x20000000u) + i, v);
                WRITE8(dst1 + i, v);
                ++i;
                count1 = (int32_t)READ32(uncached + 0x00u); // reloaded each pass
            }
        }

        // Copy 2: count at +0x04, destination pointer at +0x0c, payload at +0x50.
        uint32_t j = 0u;
        int32_t count2 = (int32_t)READ32(uncached + 0x04u);
        uint32_t dst2 = 0u;
        if (count2 > 0)
        {
            dst2 = READ32(uncached + 0x0Cu);
            while ((int32_t)j < count2)
            {
                const uint8_t v = READ8(uncached + 0x50u + j);
                WRITE8((dst2 | 0x20000000u) + j, v);
                WRITE8(dst2 + j, v);
                ++j;
                count2 = (int32_t)READ32(uncached + 0x04u);
            }
        }

        // Firing probe. A hole fix that changes nothing is indistinguishable from
        // a hole fix that never ran, so the first few completions are named
        // outright. Capped, and the cap announces itself.
        {
            static std::atomic<uint32_t> s_rpcEndSeq{0u};
            constexpr uint32_t kRpcEndMax = 24u;
            const uint32_t seq = s_rpcEndSeq.fetch_add(1u);
            if (seq == kRpcEndMax)
            {
                std::cerr << "[cap] tag=rpcend saturated at " << kRpcEndMax
                          << " -- absence of a [rpcend] line past this point is"
                             " NOT evidence."
                          << std::endl;
            }
            if (seq < kRpcEndMax)
            {
                std::ostringstream oss;
                oss << "[rpcend] fn=186310 seq=" << std::dec << seq
                    << std::hex
                    << " packet=0x" << packet
                    << " ra=0x" << GPR_U32(ctx, 31)
                    << " n1=" << std::dec << (int32_t)READ32(uncached + 0x00u)
                    << " dst1=0x" << std::hex << dst1
                    << " n2=" << std::dec << (int32_t)READ32(uncached + 0x04u)
                    << " dst2=0x" << std::hex << dst2
                    << " copied1=" << std::dec << i
                    << " copied2=" << j;
                std::cerr << oss.str() << std::endl;
            }
        }

        // Register end-state at the jump, then the tail call itself.
        SET_GPR_U64(ctx, 5, 0x20000000u);  // $a1
        SET_GPR_U64(ctx, 7, uncached);     // $a3
        SET_GPR_U64(ctx, 11, 0x00460000u); // $t3
        SET_GPR_U64(ctx, 4, 0x004632A0u);  // $a0, from the delay slot
        ctx->pc = 0x00186100u;
        runtime->dispatchGuestBranch(rdram, ctx, 0x00186100u, 0x001863C0u, 0u,
                                     PS2Runtime::GuestBranchKind::DirectJump, "J");
    }

    // ---- Holes on the live PCSX2 title-screen path (traced 2026-08-11) --------
    //
    // With PCSX2 paused on the title screen, the EE backtrace gave nine frame
    // entry points. Walking the direct call graph (j/jal) to closure from those
    // roots reaches 772 functions, of which only FOUR have no dispatch slot --
    // one of them being 0x13C4F8, already fixed above. So the whole remaining gap
    // between where we are and a drawn title screen is the three below plus one
    // unregistered interior entry point. See PS2_PROJECT_STATE.md, run 46.

    // 0x1748A0 is one cell of the EE syscall-stub table: sixteen bytes per entry,
    //   addiu $v1, $zero, N / syscall / jr $ra / nop
    // starting at N=2 and counting up. Three cells are holes; they differ only in
    // the immediate, so one helper covers all three.
    //
    // This matches what the recompiler emits for SPECIAL_SYSCALL
    // (special_translator.cpp:44): a bare `handleSyscall(rdram, ctx, code)` with
    // code==0 for a plain `syscall`, which makes the runtime read the id from $v1.
    // Generated code leaves pc to the following branch logic; here that is the
    // `jr $ra`. Some syscalls redirect pc themselves (thread switch, Exit), so pc
    // is parked on the syscall instruction first and only forced to $ra if the
    // handler left it untouched.
    static void sdbzSyscallStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                uint32_t syscallId, uint32_t stubPc)
    {
        const uint32_t syscallPc = stubPc + 4u;
        SET_GPR_U64(ctx, 3, syscallId);
        ctx->pc = syscallPc;
        runtime->handleSyscall(rdram, ctx, 0u);
        if (ctx->pc == syscallPc)
        {
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    // 0x1748a0 -- syscall 2 (SetGsCrt). Reached by `j` from 0x1720d0 and 0x172148,
    // both inside 0x1721e0, which is frame #1 of the live title-screen stack.
    void sdbzSyscallStub1748A0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sdbzSyscallStub(rdram, ctx, runtime, 2u, 0x001748A0u);
    }

    // 0x1748c0 -- syscall 4, reached by `j` from 0x17f438.
    void sdbzSyscallStub1748C0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sdbzSyscallStub(rdram, ctx, runtime, 4u, 0x001748C0u);
    }

    // 0x1748e0 -- syscall 6, reached by `j` from 0x17f410.
    void sdbzSyscallStub1748E0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sdbzSyscallStub(rdram, ctx, runtime, 6u, 0x001748E0u);
    }

    // 0x100218: a two-word trampoline in the low jump table,
    //   0x100218  j   0x17f418
    //   0x10021c  nop
    // Nothing else -- no frame, no register change. 0x17f418 does have a slot, so
    // handing pc straight to it is the whole translation. Reached from 0x18c5bc.
    void sdbzTrampoline100218(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0x0017F418u;
    }

    // Struct-zero leaf at 0x22CBE0 -- third confirmed instance of the truncation
    // class (sdbzKernelStoreWordEret, sdbzRegisterHandler104BF0). Generated
    // fn_22CBE0_0x22cbe0 emitted only the first instruction (header even says
    // "0x22cbe0 - 0x22cbe4", one sw), dropping the whole 16-iteration zero-fill
    // loop and the return -- ctx->pc stays 0x22cbe0 forever, re-dispatch loop.
    // Real function, straight from the ELF (0x22cbe0 - 0x22cc44):
    //   sw    $zero, 0($a0)
    //   daddu $a1, $zero, $zero          ; a1 = 0
    //   sw    $zero, 516($a0)            ; *(a0+0x204) = 0
    //   daddu $v1, $a0, $zero            ; v1 = a0
    //   loop (do-while, 16 iterations, v1 += 0x20 each pass):
    //     sh $zero, 4(v1) .. sh $zero, 34(v1)   ; 16 halfwords, offsets 4..34
    //     a1 += 8; loop while a1 < 0x80
    //   jr $ra
    //   daddu $v0, $a0, $zero            ; delay slot -- return this
    void sdbzLeaf22CBE0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = GPR_U32(ctx, 4);
        WRITE32(a0 + 0u, 0u);
        WRITE32(a0 + 516u, 0u);
        uint32_t v1 = a0;
        for (uint32_t a1 = 0u; a1 < 0x80u; a1 += 8u)
        {
            WRITE16(v1 + 4u, 0u);
            WRITE16(v1 + 6u, 0u);
            WRITE16(v1 + 8u, 0u);
            WRITE16(v1 + 10u, 0u);
            WRITE16(v1 + 12u, 0u);
            WRITE16(v1 + 14u, 0u);
            WRITE16(v1 + 16u, 0u);
            WRITE16(v1 + 18u, 0u);
            WRITE16(v1 + 20u, 0u);
            WRITE16(v1 + 22u, 0u);
            WRITE16(v1 + 24u, 0u);
            WRITE16(v1 + 26u, 0u);
            WRITE16(v1 + 28u, 0u);
            WRITE16(v1 + 30u, 0u);
            WRITE16(v1 + 32u, 0u);
            WRITE16(v1 + 34u, 0u);
            v1 += 0x20u;
        }
        SET_GPR_U64(ctx, 2, (uint64_t)GPR_U64(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // --- BUG-009 diagnostic: rpc_handle_valid (0x178de8) (2026-07-30) --------
    // sub_327810's boot state machine is frozen at state 1, gated by
    // wrap_rpc_handle_valid() over 4 SIF RPC client handles (0x5A9330 /
    // 0x5A9358 / 0x5A9380 / 0x5A93A8). Source (per rpc_handle_valid_0x178de8.cpp):
    //   v1 = *a0;                                   // client->hdr.pkt_addr
    //   return v1 && a0[1] == *(u32*)(v1+24)          // == client->hdr.rpc_id
    //             && (*(u32*)(v1+16) & 1);
    // Re-instates the June rpcValid= diagnostic (never landed in this build --
    // zero hits in git history for "rpc_handle_valid" additions to this file)
    // by replacing the generated leaf with an identical-behavior version that
    // logs, per call, which of the three sub-conditions failed: null pkt_addr,
    // rpc_id mismatch, or ready-bit clear. Bounded so a spin doesn't flood the
    // log. Logic must exactly match rpc_handle_valid_0x178de8.cpp -- do not
    // "fix" the condition here, this is observation only.
    // Forward decl: real definition (with the HWWATCH VEH/armer machinery) is
    // further down this same anonymous namespace, from the 2026-07-27 $ra hunt.
    void hwWatchArm(const uint8_t *rdram, uint32_t guestAddr);

    void sdbzDiagRpcHandleValid178DE8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t clientPtr = GPR_U32(ctx, 4);
        const uint32_t pktAddr = READ32(clientPtr + 0u);
        bool result = false;
        const char *reason = "null-pkt-addr";

        // 2026-07-30 BUG-009: SifBindRpc has zero call sites anywhere in
        // src/runner (confirmed by full-directory grep) -- the SDK bind path
        // is not reachable from RPC.cpp, so both prior fix attempts targeted
        // dead code. Point the existing HWWATCH infra (see the 07-27 rpc_call
        // $ra hunt above) at client->hdr.pkt_addr (offset 0) itself instead,
        // to catch the real writer -- or prove there isn't one. Single fixed
        // target so the watchpoint doesn't thrash across all 5 clients;
        // override with PS2X_HWWATCH_CLIENT=0x... Requires PS2X_HWWATCH=1 and
        // PS2X_HWWATCH_VAL=0xFFFFFFFF (record every store, not just 0x1) to
        // actually see anything -- see hwWatchArmerMain/hwWatchArm above.
        static const uint32_t s_hwWatchClientTarget = [] {
            if (const char *env = std::getenv("PS2X_HWWATCH_CLIENT"))
                if (env[0] != '\0')
                    return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
            return 0x00464dc0u;
        }();
        if (clientPtr == s_hwWatchClientTarget)
        {
            hwWatchArm(rdram, clientPtr);
        }

        if (pktAddr != 0u)
        {
            const uint32_t clientRpcId = READ32(clientPtr + 4u);
            const uint32_t pktRpcId = READ32(pktAddr + 24u);
            if (clientRpcId != pktRpcId)
            {
                reason = "rpc-id-mismatch";
            }
            else
            {
                const uint32_t readyWord = READ32(pktAddr + 16u);
                if (readyWord & 1u)
                {
                    result = true;
                    reason = "ready";
                }
                else
                {
                    reason = "ready-bit-clear";
                }
            }
        }

        static std::atomic<uint32_t> s_rpcValidLogs{0u};
        if (s_rpcValidLogs.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            std::cerr << "[rpcValid] client=0x" << std::hex << clientPtr
                       << " pktAddr=0x" << pktAddr << std::dec
                       << " result=" << (result ? 1 : 0)
                       << " reason=" << reason << std::endl;
        }

        SET_GPR_S32(ctx, 2, result ? 1 : 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void applySdbzKernelThunkFixes(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x0017F5D0u, &sdbzKernelStoreWordEret);
        runtime.replaceFunction(0x00104BF0u, &sdbzRegisterHandler104BF0);
        runtime.replaceFunction(0x00178DE8u, &sdbzDiagRpcHandleValid178DE8);
        // No table slot exists for these four -- register, do not replace.
        runtime.registerFunction(0x001BFDB0u, &sdbzCtor1BFDB0);
        runtime.registerFunction(0x001BFB80u, &sdbzDtorThunk1BFB80);
        runtime.registerFunction(0x00390DF0u, &sdbzCtor390DF0);
        runtime.registerFunction(0x0038D990u, &sdbzCtor38D990);
        // 0x180d30: the dispatch-table gap this used to paper over is CLOSED. The
        // func-map rebuild gave it a real slot -- register_functions.cpp:28832 maps
        // 0x180d30 -> sub_00180D30_0x180d30 -- so the old registration of the
        // generated fn_ body for this address was overwriting a current body with
        // a stale 2026-07-24 one. Removed 2026-08-31; do not re-add without first
        // checking register_functions.cpp for a slot.
        // 0x1a4500: same dispatch-table-gap class as 0x180d30 above. The generated
        // body (runner/fn_1A4500_0x1a4500.cpp) is a complete, correctly-terminated
        // translation (ends with ctx->pc = GPR_U32(ctx, 31)) -- an older memory note
        // describing this as a truncated 1-instruction stub is stale post-func-map-
        // rebuild; it just has no g_ps2RecompiledFunctionTable slot (2026-07-25).
        runtime.registerFunction(0x001A4500u, &fn_1A4500_0x1a4500);
        // 0x1c0170: no generated body exists at all (see sdbzLeaf1C0170 above) --
        // this is a native hand-translation, not a table-registration of
        // recompiler output (2026-07-25).
        runtime.registerFunction(0x001C0170u, &sdbzLeaf1C0170);
        // 0x2ff580: same "never a jal target" gap as 0x1c0170 -- reached only as
        // the comparator argument of the qsort at 0x2ff800, so no generated body
        // exists. Registration is what makes it reachable: the call site is an
        // indirect JALR, which goes through dispatchGuestBranch -> lookupFunction
        // and therefore does consult the registry (unlike a direct C++ fn_ call).
        // This is the Stage 5.11 draw-order root cause (2026-08-09).
        runtime.registerFunction(0x002FF580u, &sdbzDrawOrderCompare2FF580);
        // 0x12f708: same boundary-discovery gap, reached by the direct `j` at
        // 0x13243c. A DirectJump also goes through dispatchGuestBranch, so
        // registering the body is what stops the ContinueToTarget fall-through
        // that left the guest re-dispatching the pad nop at 0x132444 forever.
        // This is the Stage 5.13 boot-screen spin (2026-08-10).
        runtime.registerFunction(0x0012F708u, &sdbzSetHostLock12F708);

        // 0x13c4f8: the next hole on the same boot path, shared tail of the eight
        // index thunks at 0x13c658..0x13c710. Registering it also retires the
        // 0x13c718 spin, which was only the last thunk's fall-through.
        runtime.registerFunction(0x0013C4F8u, &sdbzRunCallbackList13C4F8);

        // 0x116cd0: hole instance 5, the setter for the global at 0x0043CB8C.
        // Retires the 0x116d2c spin, which was only its caller's fall-through.
        runtime.registerFunction(0x00116CD0u, &sdbzSetGlobalVar116CD0);
        // part 68: trace the only g36 writer (see sdbzTraceSetG36_1555A0).
        runtime.registerFunction(0x001555A0u, &sdbzTraceSetG36_1555A0);

        // The 0x460F10 flag-block cluster (Stage 5.14 run 50). 0x14fc88 is the
        // one proven reachable -- it is the target of the thunk at 0x14fab8 that
        // 0x14e590 calls. The other four are the same module's remaining holes,
        // registered now so they cannot each cost their own build-and-run.
        // Each pair is (map start, real entry); the map start is a padding nop.
        runtime.registerFunction(0x0014FC88u, &sdbzResetFlags14FC88);
        runtime.registerFunction(0x0014FBFCu, &sdbzSetArmedFlag14FC00);
        runtime.registerFunction(0x0014FC00u, &sdbzSetArmedFlag14FC00);
        runtime.registerFunction(0x0014FC34u, &sdbzSetFlag14FC38);
        runtime.registerFunction(0x0014FC38u, &sdbzSetFlag14FC38);
        runtime.registerFunction(0x0014FE84u, &sdbzSetFlag14FE88);
        runtime.registerFunction(0x0014FE88u, &sdbzSetFlag14FE88);
        runtime.registerFunction(0x0014FF78u, &sdbzNoop14FF78);

        // The 0x144100 mem-routine module (Stage 5.14 run 51). 0x1443e0 is the
        // proven blocker -- it is the named target of the one missing-target
        // record in the run. 0x144100 is the module's other hole; no caller is
        // proven for it, but a byte-copy primitive sitting one map entry away
        // from a reached byte-fill primitive is not worth a separate build.
        // Map-start padding nops registered alongside the real entries, as usual.
        runtime.registerFunction(0x001443DCu, &sdbzMemFill1443E0);
        runtime.registerFunction(0x001443E0u, &sdbzMemFill1443E0);
        runtime.registerFunction(0x001440FCu, &sdbzMemCopy144100);
        runtime.registerFunction(0x00144100u, &sdbzMemCopy144100);

        // The libcdvd SIF-RPC end_function, hit by run 61 the moment the .SFD
        // finally opened. Its absence is why the ADX stream sat bsy=1 for the
        // last 109 seconds of that run. Map-start padding registered alongside.
        runtime.registerFunction(0x0018630Cu, &sdbzCdvdRpcEnd186310);
        runtime.registerFunction(0x00186310u, &sdbzCdvdRpcEnd186310);

        // The four holes a PCSX2 backtrace at the paused title screen says are
        // still between us and a drawn title screen (run 46 trace, 772 functions
        // walked from nine live frame entry points).
        runtime.registerFunction(0x001748A0u, &sdbzSyscallStub1748A0);
        runtime.registerFunction(0x001748C0u, &sdbzSyscallStub1748C0);
        runtime.registerFunction(0x001748E0u, &sdbzSyscallStub1748E0);
        runtime.registerFunction(0x00100218u, &sdbzTrampoline100218);
        // (0x180d30 is on that list too, but is already registered above with its
        // real generated body -- find_dispatch_holes.py reads register_functions.cpp
        // only, so overrides registered from here still show up as holes.)

        // 0x1bb460 is the fourth kind: a body DOES cover it (sub_001BB450,
        // 0x1bb450..0x1bb578) but the generator never gave that entry point a
        // slot, while it did give one to 0x1bb478, 0x1bb494, 0x1bb520 and
        // 0x1bb550 -- all aliasing the same function, whose entry switch dispatches
        // on ctx->pc. 0x1bb450 is `j 0x1bb460`, so the very first thing the body
        // does is dispatch to an address it has no slot for. Alias it the same way
        // the generator aliased its siblings; looked up rather than named so this
        // does not depend on fn_forward_decls.h.
        if (PS2Runtime::RecompiledFunction body1BB450 = runtime.lookupFunction(0x001BB450u))
        {
            runtime.registerFunction(0x001BB460u, body1BB450);
        }

        // REMOVED 2026-08-31. Six registerFunction calls lived here, added when the
        // generator "simply never emitted a table slot" for these addresses. That
        // premise is dead post-func-map-rebuild: every one of them now has a real
        // slot, and because registerFunction() is an unconditional overwrite
        // (ps2_runtime.cpp:1699-1702) these lines were actively CLOBBERING current
        // generated bodies with stale 2026-07-02 ones -- fn_1137B0_0x1137b0.cpp
        // alone carried 8 `__entryPc` sites, a code shape the generator abandoned.
        //
        // Current coverage, verified in register_functions.cpp:
        //   0x1137b0 -> sub_1137A4_0x1137a4    (interior alias, +0xC)  :1127
        //   0x1c9980 -> sub_1C997C_0x1c997c    (interior alias, +0x4)  :46117
        //   0x1c9ab0 -> sub_001C9AB0_0x1c9ab0                          :46125
        //   0x256960 -> sub_00256960_0x256960                          :140702
        //   0x341920 -> sub_00341920_0x341920                          :270368
        //   0x356cd0 -> sub_00356CD0_0x356cd0                          :279460
        //
        // The two interior-alias entries are a real behaviour change: those
        // addresses now enter their owner mid-body rather than running a
        // standalone stale body. That is what the func map says is correct.

        // 0x1bf2e0: same dispatch-table-gap class as 0x180d30/0x1a4500 above. The
        // generated body (runner/fn_1BF2E0_0x1bf2e0.cpp) is a complete leaf
        // function (struct-init returning $a0), correctly terminated -- just
        // missing its g_ps2RecompiledFunctionTable slot in register_functions.cpp,
        // between sub_1BF290's 0x1bf2c0 entry and pool_entry_push's 0x1bf300 entry
        // (2026-07-25).
        runtime.registerFunction(0x001BF2E0u, &fn_1BF2E0_0x1bf2e0);
        // 0x22c8f0: same dispatch-table-gap class as 0x180d30/0x1a4500/0x1bf2e0.
        // Generated fn_22C8F0_0x22c8f0.cpp is a complete, correctly-terminated
        // struct-init leaf -- just missing its table slot, in the gap between
        // sub_22C890 (ends 0x22c8e4) and sub_22C930 (starts 0x22c930) (2026-07-25).
        runtime.registerFunction(0x0022C8F0u, &fn_22C8F0_0x22c8f0);
        // 0x22cbe0: truncation class (see sdbzLeaf22CBE0 above), NOT a table gap --
        // the generated body is real but incomplete (2026-07-25).
        runtime.registerFunction(0x0022CBE0u, &sdbzLeaf22CBE0);
        // Stage 5.10 [poolbase] probe (temporary, diagnostic only): pass-through
        // wrapper around singleton_get_camera_0x199db0, see sdbzPoolBaseProbe199DB0.
        runtime.replaceFunction(0x00199DB0u, &sdbzPoolBaseProbe199DB0);

        // --- Recovered tail-chunk bodies (Stage 5.14, 2026-08-11) -------------
        // The missing-body hole class was never a translation failure. IDA models
        // a function as a set of CHUNKS; export_func_map.py wrote only
        // f.start_ea/f.end_ea, which describe the ENTRY chunk, so every tail chunk
        // was dropped from sdbz_func_map_merged.csv. No map row means no generated
        // body, and a direct `j` into one of those ranges dispatched to nothing.
        //
        // The map now carries them and the production recompiler emitted the
        // bodies; they live in src/lib/Kernel/recovered/ rather than runner/ so
        // fn_forward_decls.h stays byte-identical and no runner TU rebuilds.
        //
        // These only need registering, not rebuilding, because dispatchGuestBranch
        // already resolves through the table -- the existing compiled callers start
        // working the moment their target has an entry.
        for (const RecoveredFn &rf : kRecoveredFns)
        {
            runtime.registerFunction(rf.addr, rf.fn);
        }

        registerSdbzSyscallThunks(runtime, std::make_index_sequence<kSdbzSyscallThunkCount>{});

        // 0x17ee80: same dispatch-table-gap class as 0x180d30/0x1a4500/0x1bf2e0
        // above, except reached only through the guest's own SetSyscall(0x83)
        // registration, never a direct jal/j -- see fn_17EE80_0x17ee80's own
        // comment for the disassembly/semantics. Forward-declaring it in
        // fn_forward_decls.h (auto-generated) is NOT enough to make hasFunction()
        // see it -- that only adds the C++ declaration, not a table slot. Without
        // this line dispatchSyscallOverride() keeps taking the
        // !runtime->hasFunction(handler) branch forever (soBranch=3 confirmed for
        // an entire 200s run at ~9M calls/sec, HANDOFF 2026-08-28 part 11).
        runtime.registerFunction(0x0017EE80u, &fn_17EE80_0x17ee80);
    }

    // --- Loadfile signature-gate seed (2026-07-17e) ---------------------------
    // noop_sub_d4a0 @0x17D4A0 (the game's loadfile-init) opens with
    //   if (dword_461C30 >= 0) return 0;
    // so with 461C30 at its BSS-0 default it returns immediately, skipping the
    // real path: sceSifBindRpc(sid 0x80000006) then a func-255 signature fetch
    // (callRpc) that populates dword_564D68. The only writer of 461C30 = -1
    // (wrap_mem_set_u @0x17D630, SDK loadfile-init glue) has NO caller in our
    // decompile, so 461C30 stays 0 and 564D68 stays 0. The gate
    // wrap_mem_compare_n_c_0 @0x17D5A0 then rejects (564D68 != "3000") and
    // sceSifLoadModule returns -65540, spinning forever on SIO2MAN.IRX.
    //
    // The 2026-07-17d wrapper (replaceFunction on d4a0) never fired: d4a0 is
    // reached by a direct jal from its callers (generated fn_0017D4A0 call), so
    // it bypasses the dense function table entirely — replaceFunction only
    // intercepts jalr/dispatch-loop calls. Confirmed by the run: no
    // [loadfile-seed] line, 461C30 still 0, yet d4a0 present in the trace.
    //
    // Fix: seed the flag directly at override-apply time. applyMatching() runs
    // at the end of loadELF, after BSS is zeroed and before the entry point
    // executes; nothing writes 461C30 before d4a0's first call, so this -1
    // persists into that call and makes d4a0 run its own real bind + func-255
    // RPC through our SIF handleRPC path. No hand-faked values — d4a0 itself
    // sets 461C30 = 0 after one pass, so the real init runs exactly once.
    void applySdbzLoadfileSeed(PS2Runtime &runtime)
    {
        runtime.memory().write32(0x00461C30u, 0xFFFFFFFFu); // dword_461C30 = -1
        // RUNTIME_LOG is a compiled-out no-op in this build (PS2_RUNTIME_LOGS
        // undefined), so report through the always-on std::cerr channel and read
        // the value straight back to prove the write landed at apply time.
        const uint32_t readback = runtime.memory().read32(0x00461C30u);
        const uint32_t fetched = runtime.memory().read32(0x00564D68u);
        std::cerr << std::hex
                  << "[loadfile-seed] 0x461C30 <- -1 at ELF-load; readback=0x" << readback
                  << " 564D68=0x" << fetched << std::dec << std::endl;
    }

    // --- [frametrace] stale-frame $ra measurement (2026-07-20) ---------------
    // MEASUREMENT ONLY. Installs pass-through wrappers; changes no behaviour.
    //
    // The boot derails with pc == ra == target == 0x20561900 (the SIF
    // packet-pool base). Every write-side explanation is dead: PS2X_TRAPVAL
    // logged 155 hits, all legitimate SIF pool-pointer stores, and none at the
    // suspect slot sp+0x20 = 0x01FFBC60. Nothing WROTE that $ra -- which is
    // exactly what a stale-frame LOAD would look like.
    //
    // Mechanism under test: the recompiler registers interior "resume"
    // addresses of a function as their own dispatch-table slots aliasing back
    // to the same function (register_functions.cpp:149822-149838). The
    // generated entry `switch (ctx->pc)` goto's past the prologue, but the
    // epilogue runs unconditionally -- so a dispatch that reaches one of those
    // slots outside a legitimate resume executes `ld $ra, N($sp)` against the
    // CALLER's frame.
    //
    // Both functions on the derail trace are wrapped at EVERY slot: a wrapper on
    // the entry address alone would never see a mid-body dispatch, which is the
    // whole mechanism.
    //
    // The originals are captured via lookupFunction() rather than by symbol, to
    // avoid depending on fn_forward_decls.h (auto-generated every build).
    struct SdbzFrameTraceSlot
    {
        uint32_t addr;
        uint32_t funcStart;
    };

    // 2026-07-20d retarget -- BACK to the SIF-RPC set, with the evidence that was
    // missing the first two times.
    //
    // The 07-20c run wrapped the CRI ADX tick chain (0x11fa60 -> 0x11e3b0 ->
    // 0x11e560 -> 0x11f240) because the watchdog's derail trace named it. That
    // measured 36 of 36 calls perfectly clean: every entryPc == funcStart, every
    // exitSp == entrySp, every exit sane (0x11e578 / 0x11e3c8 / 0x11fb44 /
    // 0x104bcc). Nine full unwind cycles, no MIDBODY, no SPDELTA, no ALARM.
    //
    // The reason it measured clean is that it was the WRONG STACK. Those frames
    // all sit at sp ~= 0x000fffe0; the fault reports sp = 0x01ffbc40. The
    // watchdog "trace=" line is a global dispatch history, not the faulting call
    // chain -- it interleaves threads. It has now misdirected this investigation
    // twice, so do not retarget off it again.
    //
    // The faulting stack is the SIF-RPC dispatcher. The [stack] window at fault
    // time holds these saved return addresses, which resolve to:
    //     0x177fc4 -> fn_177EB0   (register_functions.cpp:122863)
    //     0x178188 -> fn_178068   (:122976)  <- the RPC dispatcher
    //     0x178018 -> fn_177FE8   (:122884)
    //     0x178d1c -> fn_178BE8   (:123717)
    // plus the RPC_END cid 0x80000008 at sp-0x28 and client 0x5a9380 at sp-0x4.
    //
    // And the offset match that closes it: the fault dump shows
    //     [stack] 0x1ffbc60 (sp+0x20) 0x20561900
    // while fn_178428's epilogue is `ld $ra, 0x20($sp)` (fn_178428_0x178428.cpp:242).
    // A mid-body dispatch into one of its four alias slots at this sp loads
    // exactly 0x20561900 as $ra. That is the original hypothesis, on the original
    // address. The earlier "disproof" ran 19 wrappers in a boot where the derail
    // never fired at all, so it measured a clean path and the verdict was
    // over-generalised -- see [[project_stale_frame_ra_read]].
    //
    // 0x178068 has 82 slots (a dispatcher jump table) -- entry only, or it drowns
    // the 64-entry ring. The other five are wrapped at EVERY slot, since a wrapper
    // on the entry address alone can never observe a mid-body dispatch, which is
    // the whole mechanism.
    constexpr SdbzFrameTraceSlot kSdbzFrameTraceSlots[] = {
        // sub_1BAF90 is the direct, measured caller of rpc_call on the failing
        // path: jal 0x178BE8 at 0x1BB0A8, with return/resume at 0x1BB0B0.
        // PCSX2 and recomp disagree by 0x2D60 at rpc_call entry, so this is
        // the first parent boundary that can distinguish "already wrong on
        // entry" from "lost inside the RPC subtree".  Wrap every registered
        // resume slot as well as the true entry; a resume must not be mistaken
        // for a fresh prologue execution when reading the fault ring.
        {0x001BAF90u, 0x001BAF90u},
        {0x001BB000u, 0x001BAF90u},
        {0x001BB00Cu, 0x001BAF90u},
        {0x001BB020u, 0x001BAF90u},
        {0x001BB02Cu, 0x001BAF90u},
        {0x001BB040u, 0x001BAF90u},
        {0x001BB04Cu, 0x001BAF90u},
        {0x001BB060u, 0x001BAF90u},
        {0x001BB06Cu, 0x001BAF90u},
        {0x001BB0B0u, 0x001BAF90u},
        // fn_178428 -- prime suspect: `ld $ra, 0x20($sp)`, and sp+0x20 == 0x20561900.
        {0x00178428u, 0x00178428u},
        {0x00178440u, 0x00178428u},
        {0x00178458u, 0x00178428u},
        {0x0017849Cu, 0x00178428u},
        {0x001784B8u, 0x00178428u},
        // fn_178260 -- 14 slots; also reloads $ra inside branch delay slots.
        {0x00178260u, 0x00178260u},
        {0x00178278u, 0x00178260u},
        {0x001782A8u, 0x00178260u},
        {0x001782B0u, 0x00178260u},
        {0x001782B8u, 0x00178260u},
        {0x0017832Cu, 0x00178260u},
        {0x00178344u, 0x00178260u},
        {0x0017835Cu, 0x00178260u},
        {0x00178374u, 0x00178260u},
        {0x0017837Cu, 0x00178260u},
        {0x00178388u, 0x00178260u},
        {0x001783B4u, 0x00178260u},
        {0x001783B8u, 0x00178260u},
        {0x001783C0u, 0x00178260u},
        // 2026-07-23 gap-fill: 17ed60/17edb0/17edc8 sit between 178428 and 177fe8
        // in the observed chain (178be8 -> 178428 -> 17ed60 -> 17edb0 -> 177fe8 ->
        // 177eb0 -> ...) but were never wrapped, so SLOTWATCH could only bracket
        // the stomp to "somewhere inside 178428's subtree" instead of a specific
        // callee. Entry-only wrap; each is a leaf/near-leaf, no alias slots.
        {0x0017ED60u, 0x0017ED60u},
        {0x0017EDB0u, 0x0017EDB0u},
        {0x0017EDC8u, 0x0017EDC8u},
        // fn_177EB0 -- live on the faulting stack (saved $ra 0x177fc4).
        {0x00177EB0u, 0x00177EB0u},
        {0x00177F38u, 0x00177EB0u},
        {0x00177FA0u, 0x00177EB0u},
        {0x00177FB4u, 0x00177EB0u},
        {0x00177FC4u, 0x00177EB0u},
        // fn_177FE8 -- live on the faulting stack (saved $ra 0x178018).
        {0x00177FE8u, 0x00177FE8u},
        {0x00178018u, 0x00177FE8u},
        // fn_178BE8 -- live on the faulting stack (saved $ra 0x178d1c).
        {0x00178BE8u, 0x00178BE8u},
        {0x00178C40u, 0x00178BE8u},
        {0x00178CACu, 0x00178BE8u},
        {0x00178CC4u, 0x00178BE8u},
        {0x00178CD4u, 0x00178BE8u},
        {0x00178D1Cu, 0x00178BE8u},
        {0x00178D48u, 0x00178BE8u},
        {0x00178D58u, 0x00178BE8u},
        {0x00178D84u, 0x00178BE8u},
        {0x00178D94u, 0x00178BE8u},
        {0x00178D9Cu, 0x00178BE8u},
        {0x00178DACu, 0x00178BE8u},
        {0x00178DB4u, 0x00178BE8u},
        // RPC dispatcher, entry slot only (82 aliases = jump table).
        {0x00178068u, 0x00178068u},
        // 2026-07-25g -- new suspect, not yet measured. sub_178560 is the SIF
        // dispatcher's _request_end callback handler (see the kSifCmdRpcCall
        // comment block in SIF.cpp): it loads a completion-callback function
        // pointer out of the client block (`lw $v0, 28($s1)` at 0x1785c8, where
        // $s1 = *(client_block+28)) and calls it directly (`jalr $v0` at
        // 0x1785cc, disasm-confirmed). The dispatch trace immediately preceding
        // every pc=1/ra=1 derail this run ends "...0x178560 -> 0x1784d0 -> 0x1",
        // and 0x1784d0 (bit-clear leaf, disasm-confirmed) only `jr $ra` -- it
        // can't originate pc=1 itself, so whatever set $ra=1 did so before
        // 0x1784d0 ran, i.e. inside or before 0x178560. Entry-only wrap to
        // measure entryRa vs exitRa here directly, instead of inferring it from
        // the callers further up the chain.
        {0x00178560u, 0x00178560u},
        // 2026-07-25i follow-up -- bracket narrowed to between 0x177eb0's entry
        // (clean) and 0x178560's entry (dirty). These three are the unwrapped
        // intermediate calls in the known chain (177eb0 -> 1781b0 -> 175060 ->
        // 178068(x2) -> 175090 -> 178560) that bypass the dispatch-table wrapper
        // (direct C++ calls, per the "registerFunction Bypass" pattern) and so
        // never logged a SLOTENTRY line despite running per the trace. Entry-only
        // wrap to see which one is first to observe the slot already dirty.
        {0x001781B0u, 0x001781B0u},
        {0x00175060u, 0x00175060u},
        {0x00175090u, 0x00175090u},
        // 2026-07-25j -- RETRACTED 2026-07-25k. This block previously claimed
        // "WRITER IDENTIFIED: GS_DispatchPending's `sq $s0, 0($sp)` at 0x102894
        // stomps rpc_call's saved-$ra slot, proving a frame overlap". Every leg
        // of that failed on measurement:
        //   * "exactly one PC writes 0x1ffbeb0" rested on a 2-hit PS2X_TRAPVAL
        //     run; a later 151-hit run showed six distinct writers.
        //   * GSENTRY measurement exonerates GS_DispatchPending outright --
        //     entryPc is always 0x102870 (real entry, no resume-slot aliasing),
        //     entrySp is stable at 0x1ffbf00, entryRa stable at 0x104c9c.
        //   * The 0x1ffbe80..0x1ffbf00 band is ordinary downward stack growth,
        //     not an overlap. PS2X_TRAPVAL is RETIRED; it produced only noise.
        // The 0x102870 wraps below are kept purely as trace coverage -- they are
        // NOT evidence of a suspect. Do not resurrect the overlap theory.
        {0x00102870u, 0x00102870u},
        {0x001028CCu, 0x00102870u},
        {0x00102904u, 0x00102870u},
        {0x00102984u, 0x00102870u},
        {0x00102994u, 0x00102870u},
        {0x001029DCu, 0x00102870u},
        {0x00102A24u, 0x00102870u},
        {0x00102A30u, 0x00102870u},
        // 2026-07-25k -- new bracket, from the FIRST derail of the latest run
        // (run_log.txt:2738) rather than a late one. Two facts reframed 5.4.1:
        //   1. $ra is provably clean across every syscall (see the retraction
        //      note at the top of Kernel/Syscalls/Dispatcher.cpp), so neither
        //      the fiber save/restore path nor the syscall thunk loses it.
        //   2. Both derails measured so far exit a CALL-FREE LEAF: 0x1784d0
        //      (bit-clear, per the 07-25g note above) and now 0x191780, a SIMD
        //      strcpy/stpcpy (lq/sq -> ld/sd -> lbu/sb scan, ending `jr $ra;
        //      daddu $v0,$a3,$zero` at 0x19188c, disasm-confirmed). A leaf never
        //      touches $ra, so $ra was ALREADY 0x1 when the leaf was entered --
        //      leaves are just the first place it becomes visible.
        // The derail's terminal function varies run to run (0x1784d0 / 0x191780
        // / 0x174cc0), matching the known nondeterminism, and six distinct tids
        // were live this run (0x10..0x14, 0x6c50) -- the "single thread" claim
        // from 07-25j is retracted with the rest of it.
        // 0x191780's caller in the observed loop is 0x2b93e0, reached via
        // 0x191780 -> 0x2b9300 -> 0x18e408 -> 0x2b93e0 -> 0x191780 (which spins
        // successfully many times before the derail). Entry-only wraps on the
        // two unwrapped links so entryRa says which frame first sees $ra dirty.
        {0x002B93E0u, 0x002B93E0u},
        {0x0018E408u, 0x0018E408u},
        {0x002B9300u, 0x002B9300u},
        {0x00191780u, 0x00191780u},
        // 2026-07-25L -- the leaf's caller set is RUN-DEPENDENT. The 07-25k run's
        // loop (0x2b9300/0x2b93e0) does not appear at all in the next run, whose
        // loop is 0x32bbb0 -> 0x32bca0 -> 0x191898 -> 0x32bca0 -> 0x191780
        // (repeating, 323 hits on 0x191780 alone). register_functions.cpp names
        // 0x191780 `strcpy_0x191780` and 0x191898 `strlen_0x191898`, independently
        // confirming the disasm read that both are call-free libc leaves. Wrap the
        // new run's callers too so whichever loop a given run takes is covered.
        {0x0032BBB0u, 0x0032BBB0u},
        {0x0032BCA0u, 0x0032BCA0u},
        {0x00191898u, 0x00191898u},
        // 2026-07-25o -- VECCTOR. 5.4.1's premise ("a bad ctor fnptr 0x30 reaches
        // the unguarded `jalr $s5` at 0x171d04") is FALSIFIED. All 172 `jal
        // 0x171c30` sites in the ELF were enumerated and their argument blocks
        // resolved by simulating the lui/addiu pairs (incl. delay slots): 92
        // distinct $a1 values, NONE equal to 0x30, none unresolved, none below
        // 0x100000. The single site carrying 0x30 anywhere is 0x2b8bfc, and it
        // is $t0 -- the ELEMENT COUNT (48) -- not a function pointer:
        //     0x2b8bfc: a0=0x2c7c40 a1=0x2b9300 a2=0x2b93f0 a3=0x180 t0=0x30
        // Corroborated by the 07-25n LEAFEXIT lines (callee=0x2b9300
        // entryRa=0x171d0c, i.e. the return slot of that very `jalr $s5`), which
        // prove the ctor ran with a VALID $s5. So the derail PC 0x30 equals the
        // loop TRIP COUNT, which lands in $s3 at 0x171c60.
        //
        // array_call_ctor_dtor's frame is 0xb0, and its loop counter lives in
        // MEMORY at sp+0xa0 -- in the frame's top scratch area, ABOVE the saved
        // register block ($ra at sp+0x80). The ctor 0x2b9300 is a deep non-leaf
        // (jal 0x18e408, a vtable slot `lw $t9,0($s0)`/`lw $t9,12($t9)`/`jalr
        // $t9` at 0x2b93b0, jal 0x191780) invoked 48 times into that frame top.
        // Truncation is ruled out: none of 0x171c30 / 0x2b9300 / 0x2b93f0 appear
        // in build_scripts/truncated_functions.txt.
        // Two live candidates remain, and this probe separates them:
        //   (a) $s3/$s5 confusion or clobber in the recompiled body -> $s5 or
        //       $s3 changes across a ctor return while the memory counter is sane.
        //   (b) frame-top clobber of the in-memory counter by the nested ctor
        //       -> counter at sp+0xa0 jumps/rewinds while $s3/$s5 stay intact.
        // Wrap the entry AND the two in-loop slots (the ctor-return resume at
        // 0x171d0c and the loop-back target 0x171d00) so each iteration is
        // sampled, not just the call.
        {0x00171C30u, 0x00171C30u},
        {0x00171D00u, 0x00171C30u},
        {0x00171D0Cu, 0x00171C30u},
        // 2026-08-31 session 5 part 41 -- sub_17CF50's own $ra slot corruption
        // (parts 38-40: [semwatch:cf50resume] raAtSpPlus32 reads 0 at the
        // 0x17cfa4 resume, after 32+ healthy hits earlier in the same run).
        // Reading fn_17CF50_0x17cf50.cpp directly ruled out "prologue
        // skipped" -- the real prologue (0x17cf50) does write a valid $ra to
        // sp+0x20 -- so this wraps every registered slot (true entry plus the
        // three resume labels) the same way the rpc_call hunt above did, to
        // arm a live HWWATCH write-trap on that exact RDRAM address instead
        // of guessing the writer. See the funcStart==0x0017CF50u block in
        // sdbzFrameTraceWrapper below.
        {0x0017CF50u, 0x0017CF50u},
        {0x0017CF68u, 0x0017CF50u},
        {0x0017CF70u, 0x0017CF50u},
        {0x0017CFA4u, 0x0017CF50u},
    };

    constexpr size_t kSdbzFrameTraceSlotCount =
        sizeof(kSdbzFrameTraceSlots) / sizeof(kSdbzFrameTraceSlots[0]);

    // Captured before replaceFunction() overwrites the slot, so the wrapper can
    // still reach the real body.
    PS2Runtime::RecompiledFunction g_sdbzFrameTraceOriginals[kSdbzFrameTraceSlotCount] = {};

    // 2026-07-22 SLOTWATCH -- fork B is confirmed: rpc_call (0x178be8) is handed a
    // valid $ra (0x1bb0b0), saves it at newsp+0xB0 (== entrySp-0x10 == 0x1ffbeb0),
    // yet the epilogue reloads 0x1. So a dispatched callee stomps that slot
    // mid-body. Arm a watch on the slot at rpc_call true-entry; every nested
    // wrapped callee samples it on the way out. The first sample that reads a
    // changed value brackets the stomp to (previous callee .. this callee].
    std::atomic<uint32_t> g_rpcSlotAddr{0};      // guest addr of watched slot, 0 = disarmed
    std::atomic<uint32_t> g_rpcSlotExpected{0};  // value rpc_call wrote at its prologue
    std::atomic<uint32_t> g_slotWatchDumps{0};

    // -----------------------------------------------------------------------
    // 2026-07-27 HWWATCH -- in-process x64 hardware data breakpoint on the
    // rpc_call saved-$ra slot.
    //
    // Why this and not the SLOTWATCH bracketing already above: SLOTWATCH can
    // only sample the slot at wrapped-frame boundaries, so it brackets the
    // writer to a span of code, never names it. Instrumenting guest stores is
    // not an option either -- stores go through macros in
    // ps2_runtime_macros.h, a HEADER, and touching it costs a 30h rebuild.
    // The CPU's debug registers catch the store itself, with the real host
    // RIP, and cost one .cpp.
    //
    // Structure:
    //   - guest side (hwWatchArm) only publishes the target address. It never
    //     suspends a thread, so it cannot deadlock against the CRT heap lock.
    //   - a single background armer thread does the SuspendThread/
    //     SetThreadContext sweep, re-sweeping when the target moves and
    //     periodically so threads created later also get armed.
    //   - the VEH does NO allocation and takes NO lock: it copies RIP + a raw
    //     backtrace into a fixed ring. The armer thread symbolizes and writes.
    //     (Symbolizing inside the handler could deadlock if the store happened
    //     while the faulting thread held the heap lock.)
    //
    // Output: HWWATCH records in the probe JSONL (numeric), plus a symbolized
    // text dump next to it at <PS2X_PROBE_FILE>.hwwatch.txt.
    // -----------------------------------------------------------------------
    // 2026-08-09 run 35 -- 64 -> 2048. This is a hard cap, not a ring: once it
    // fills, every later hit is dropped on the floor. That was tolerable when
    // the target was a stack slot written a handful of times, but the run 35
    // hunt watches a display-list slot that the builder rewrites EVERY FRAME,
    // and the frames that matter (the dialog, ~199-299) are nowhere near the
    // first 64. At 64 the probe would have reported the boot-time writer with
    // total confidence and never seen the dialog-frame one.
    //
    // The cap is still finite, so hwWatchArmerMain's heartbeat now publishes
    // seen/dropped alongside hits -- a saturated probe has to convict itself
    // rather than look identical to "it never happened".
    // 2026-08-09 run 37 -- 2048 was sized for the SetVSyncFlag hunt, where the
    // value filter kept all but a handful of stores out of the ring. The
    // draw-order question needs PS2X_HWWATCH_VAL=0xFFFFFFFF on a buffer address
    // that is written many times per frame, so 2048 fills during boot and every
    // dialog-frame store lands in the invisible tail (drop>0). 16384 slots x
    // 128 B = ~2 MB of host BSS, which buys ~8x the window; the vblank gate
    // below is what actually puts those slots on the frames we care about.
    constexpr uint32_t kHwWatchMaxHits = 16384;
    constexpr uint32_t kHwWatchMaxFrames = 12;

    struct HwWatchHit
    {
        uint64_t rip;
        uint64_t frames[kHwWatchMaxFrames];
        uint64_t vbl; // delivered-vblank count at capture == frame index
        uint32_t nframes;
        uint32_t val;
        uint32_t tid;
        uint32_t guestAddr;
    };

    HwWatchHit g_hwWatchHits[kHwWatchMaxHits] = {};
    std::atomic<uint32_t> g_hwWatchWrite{0};   // slots reserved by the VEH
    std::atomic<uint32_t> g_hwWatchReady{0};   // slots fully filled in
    std::atomic<uint32_t> g_hwWatchDrained{0}; // slots already written out
    std::atomic<uint64_t> g_hwWatchHost{0};    // host address under watch
    std::atomic<uint32_t> g_hwWatchGuest{0};   // guest address under watch
    std::atomic<bool> g_hwWatchArmerUp{false};
    std::atomic<bool> g_hwWatchVehUp{false};
    // Times hwWatchArmSelf actually set DR0 on the calling thread. Published
    // in HWSTAT as "selfarm" -- see the 2026-08-31 race note on hwWatchArm.
    std::atomic<uint32_t> g_hwWatchSelfArmed{0};
    // Only stores of this value are recorded. 0xFFFFFFFF = record all.
    std::atomic<uint32_t> g_hwWatchWantVal{1};
    std::atomic<uint64_t> g_hwWatchSkipped{0}; // filtered-out stores, for sanity

    // 2026-08-09 run 37 -- vblank window gate.
    //
    // PS2X_HWWATCH_VBL_LO / _HI bracket the frames whose stores get recorded
    // (half-open: LO <= vbl < HI). Default is the whole run, so an unset pair
    // behaves exactly as before.
    //
    // This exists because the ring is finite and the interesting frames are
    // late. Without it a run that records everything spends all 16384 slots on
    // boot traffic and then reports NOTHING about the dialog frames -- which
    // looks identical to "the dialog frames make no stores". That is the
    // capped-probe false negative this project has already hit three times, so
    // the gate is paired with two counters below: stores rejected for being
    // outside the window are counted separately from value-filtered ones, and
    // both are published in HWSTAT. An empty result is only readable when you
    // can see WHICH filter ate the traffic.
    std::atomic<uint64_t> g_hwWatchVblLo{0};
    std::atomic<uint64_t> g_hwWatchVblHi{~0ull};
    std::atomic<uint64_t> g_hwWatchOutOfWin{0}; // rejected by the vblank gate

    // Dr7: L0 (bit0) + LE (bit8) + RW0=01 write-only (bits16-17) +
    // LEN0=11 four bytes (bits18-19). Bits 16-19 therefore read 0b1101.
    constexpr uint64_t kHwWatchDr7Mask = (0xFull << 16) | 0x3ull;
    constexpr uint64_t kHwWatchDr7Bits = 0x1ull | (1ull << 8) | (0xDull << 16);

    LONG CALLBACK hwWatchVeh(EXCEPTION_POINTERS *info)
    {
        if (info == nullptr || info->ExceptionRecord == nullptr ||
            info->ContextRecord == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
        if (info->ExceptionRecord->ExceptionCode !=
            static_cast<DWORD>(STATUS_SINGLE_STEP))
            return EXCEPTION_CONTINUE_SEARCH;

        CONTEXT *cr = info->ContextRecord;
        if ((cr->Dr6 & 0x1ull) == 0)
            return EXCEPTION_CONTINUE_SEARCH; // single-step, but not our DR0

        cr->Dr6 = 0; // ack, else it re-reports

        const uint64_t host = g_hwWatchHost.load(std::memory_order_relaxed);
        if (host == 0)
            return EXCEPTION_CONTINUE_EXECUTION;

        const uint32_t newVal =
            *reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(host));

        // 2026-07-27 run 1 lesson: without this filter the 64-slot ring fills
        // with ordinary stack traffic (return addresses, sizes, GS words) long
        // before the clobber lands, so the interesting store is never captured.
        // Record only the value we are hunting. PS2X_HWWATCH_VAL overrides it;
        // 0xFFFFFFFF means "record everything" (the old behaviour).
        const uint32_t want = g_hwWatchWantVal.load(std::memory_order_relaxed);
        if (want != 0xFFFFFFFFu && newVal != want)
        {
            g_hwWatchSkipped.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Frame stamp. Read once and reused for both the gate and the record so
        // a vblank landing between the two cannot put a hit in the ring with a
        // frame index the gate never approved.
        const uint64_t vbl = ps2x_vblank_ticks();
        if (vbl < g_hwWatchVblLo.load(std::memory_order_relaxed) ||
            vbl >= g_hwWatchVblHi.load(std::memory_order_relaxed))
        {
            g_hwWatchOutOfWin.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // idx doubles as the ORDERING record: fetch_add serializes concurrent
        // hits, so slot order is store order. It is surfaced as HWWATCH's "n",
        // which is therefore a capture ordinal, not just an array index.
        const uint32_t idx = g_hwWatchWrite.fetch_add(1, std::memory_order_relaxed);
        if (idx < kHwWatchMaxHits)
        {
            HwWatchHit &h = g_hwWatchHits[idx];
            h.rip = cr->Rip;
            h.val = newVal;
            h.vbl = vbl;
            h.tid = static_cast<uint32_t>(GetCurrentThreadId());
            h.guestAddr = g_hwWatchGuest.load(std::memory_order_relaxed);
            void *raw[kHwWatchMaxFrames] = {};
            const USHORT n = RtlCaptureStackBackTrace(
                0, static_cast<DWORD>(kHwWatchMaxFrames), raw, nullptr);
            h.nframes = n;
            for (USHORT i = 0; i < n; ++i)
                h.frames[i] = reinterpret_cast<uint64_t>(raw[i]);
        }
        // Publish AFTER the record is filled -- the drainer keys off this
        // counter, not off the reservation counter. With concurrent hits the
        // ready count can lag a specific index by one; acceptable for a
        // diagnostic, and hits here are effectively serialized in practice.
        g_hwWatchReady.fetch_add(1, std::memory_order_release);

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Threads whose DR0 the LAST sweep actually set. Published in HWSTAT.
    //
    // 2026-08-09: run 35's backtraces were read with the caveat "DR0 may not
    // have stayed pinned all run", which made a negative result unusable -- an
    // unseen writer could always be blamed on an unarmed thread. With
    // PS2X_HWWATCH_ADDR set the pin is structural (hwWatchArm rewrites every
    // caller's address to the fixed one, so no arm site can steal DR0), but
    // structural is not measured. This counter measures it. The residual hole
    // is timing, not contention: a thread created just after a sweep runs
    // unarmed for up to the 250 ms until the next one.
    std::atomic<uint32_t> g_hwWatchThreadsArmed{0};

    void hwWatchSweep(uint64_t addr)
    {
        uint32_t armedCount = 0;
        const DWORD myTid = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;

        THREADENTRY32 te = {};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                if (te.th32ThreadID == myTid)
                    continue; // the armer never touches guest memory

                HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                          THREAD_SUSPEND_RESUME,
                                      FALSE, te.th32ThreadID);
                if (h == nullptr)
                    continue;

                // No allocation between Suspend and Resume: a suspended thread
                // may hold the CRT heap lock.
                SuspendThread(h);
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c))
                {
                    c.Dr0 = addr;
                    c.Dr6 = 0;
                    c.Dr7 = (c.Dr7 & ~kHwWatchDr7Mask) | kHwWatchDr7Bits;
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(h, &c))
                        ++armedCount;
                }
                ResumeThread(h);
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        g_hwWatchThreadsArmed.store(armedCount, std::memory_order_relaxed);
    }

    // 2026-08-31 session 5 part 32 -- closes the arm-to-first-sweep race found
    // after part 31's hwwatch run came back armed=1/hits=0/seen=0 for the
    // whole run: the run_probe.jsonl sequence numbers show the FIRST HWSTAT
    // heartbeat (which only prints after hwWatchArmerMain's SymInitialize +
    // AddVectoredExceptionHandler complete and the loop reaches its first
    // hwWatchSweep) already reported the guest's final frozen progress value
    // -- i.e. by the time DR0 was ever set on the guest thread via the
    // background armer, the freeze had already happened. Any write to the
    // watched address during the actual suspension window (the thing part 31
    // was trying to catch) was invisible by construction, which makes the
    // "never written" HWSTAT reading unusable for this run
    // (see feedback_run_window_false_negative / feedback_degenerate_result_convicts_the_probe).
    //
    // AddVectoredExceptionHandler is a cheap, synchronous, allocation-free
    // registration -- it is SymInitialize (symbol/module enumeration) that is
    // slow and was gating it. Split them: register the VEH here, eagerly, on
    // the calling (guest) thread, the first time any code asks to arm the
    // watch. SymInitialize stays in hwWatchArmerMain (only needed later, for
    // hwWatchSymbolize's backtraces on the eventual dump).
    void hwWatchEnsureVeh()
    {
        bool expected = false;
        if (g_hwWatchVehUp.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel))
        {
            AddVectoredExceptionHandler(1, hwWatchVeh);
        }
    }

    // Arms DR0 on the CALLING thread directly, without suspending it -- you
    // cannot SuspendThread yourself, but SetThreadContext on your own pseudo
    // handle (GetCurrentThread()) is well-defined for debug registers and
    // takes effect on return. This must run AFTER hwWatchEnsureVeh(): arming
    // DR0 before the VEH is live would turn the very write we want to catch
    // into an unhandled STATUS_SINGLE_STEP.
    //
    // This does not replace the background armer thread's periodic
    // hwWatchSweep -- that still covers every OTHER thread in the process (in
    // this project nTh=1 so there normally isn't one, but it's not assumed
    // here) and re-arms threads created after the fact. This just guarantees
    // the ONE thread that is actually calling into guest code is armed
    // synchronously, inline, with no thread-handoff latency at all.
    void hwWatchArmSelf(uint64_t host)
    {
        static thread_local uint64_t s_lastArmed = 0;
        if (s_lastArmed == host)
            return;
        HANDLE self = GetCurrentThread();
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(self, &c))
        {
            c.Dr0 = host;
            c.Dr6 = 0;
            c.Dr7 = (c.Dr7 & ~kHwWatchDr7Mask) | kHwWatchDr7Bits;
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (SetThreadContext(self, &c))
            {
                s_lastArmed = host;
                g_hwWatchSelfArmed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::string hwWatchDumpPath()
    {
        const char *probe = std::getenv("PS2X_PROBE_FILE");
        if (probe != nullptr && probe[0] != '\0')
            return std::string(probe) + ".hwwatch.txt";
        return std::string("run_hwwatch.txt");
    }

    void hwWatchSymbolize(std::FILE *f, uint64_t addr)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 500;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym))
            std::fprintf(f, "0x%016llx  %s + 0x%llx",
                         static_cast<unsigned long long>(addr), sym->Name,
                         static_cast<unsigned long long>(disp));
        else
            std::fprintf(f, "0x%016llx  <no symbol>",
                         static_cast<unsigned long long>(addr));

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line))
            std::fprintf(f, "   [%s:%lu]", line.FileName, line.LineNumber);
        std::fputc('\n', f);
    }

    void hwWatchDrain()
    {
        uint32_t done = g_hwWatchDrained.load(std::memory_order_relaxed);
        const uint32_t have = g_hwWatchReady.load(std::memory_order_acquire);
        if (done >= have)
            return;

        // 2026-08-09 -- first open of the run truncates. This file was opened
        // "a" from the start, so it accumulated across every run that ever
        // armed HWWATCH; backtraces from a three-build-old binary sat directly
        // above the current ones with nothing marking the boundary. The probe
        // JSONL has always been "w" per run (see probeFile) -- this just makes
        // the text sidecar agree with it.
        static bool s_truncated = false;
        std::FILE *f = std::fopen(hwWatchDumpPath().c_str(),
                                  s_truncated ? "a" : "w");
        s_truncated = true;
        for (; done < have && done < kHwWatchMaxHits; ++done)
        {
            const HwWatchHit &h = g_hwWatchHits[done];

            const char *keys[6] = {"n", "vbl", "val", "rip", "tid", "guest"};
            const uint64_t vals[6] = {done, h.vbl, h.val,
                                      h.rip, h.tid, h.guestAddr};
            ps2x_probe_kv("HWWATCH", 6, keys, vals);

            if (f == nullptr)
                continue;
            std::fprintf(f,
                         "\n=== HWWATCH hit #%u  vbl=%llu  guest=0x%08x  "
                         "newval=0x%08x  tid=0x%x ===\n",
                         done, static_cast<unsigned long long>(h.vbl),
                         h.guestAddr, h.val, h.tid);
            std::fputs("  store site (RIP after the write):\n    ", f);
            hwWatchSymbolize(f, h.rip);
            std::fputs("  host stack:\n", f);
            for (uint32_t i = 0; i < h.nframes; ++i)
            {
                std::fputs("    ", f);
                hwWatchSymbolize(f, h.frames[i]);
            }
            std::fflush(f);
        }
        if (f != nullptr)
            std::fclose(f);
        g_hwWatchDrained.store(done, std::memory_order_relaxed);
    }

    void hwWatchArmerMain()
    {
        // Resolve the filter BEFORE the handler goes live -- getenv touches the
        // CRT and must not run inside the VEH.
        if (const char *wantEnv = std::getenv("PS2X_HWWATCH_VAL"))
        {
            if (wantEnv[0] != '\0')
                g_hwWatchWantVal.store(
                    static_cast<uint32_t>(std::strtoul(wantEnv, nullptr, 0)),
                    std::memory_order_relaxed);
        }
        // Same rule for the vblank window: resolved here, before the handler is
        // registered, because getenv is not VEH-safe.
        if (const char *loEnv = std::getenv("PS2X_HWWATCH_VBL_LO"))
        {
            if (loEnv[0] != '\0')
                g_hwWatchVblLo.store(std::strtoull(loEnv, nullptr, 0),
                                     std::memory_order_relaxed);
        }
        if (const char *hiEnv = std::getenv("PS2X_HWWATCH_VBL_HI"))
        {
            if (hiEnv[0] != '\0')
                g_hwWatchVblHi.store(std::strtoull(hiEnv, nullptr, 0),
                                     std::memory_order_relaxed);
        }

        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        // hwWatchArm() already registers this eagerly on the calling thread
        // (see hwWatchEnsureVeh) before this thread even starts; guarded so
        // calling it again here is a harmless no-op, not a double-registration.
        hwWatchEnsureVeh();

        uint64_t armed = 0;
        uint32_t tick = 0;
        for (;;)
        {
            const uint64_t want = g_hwWatchHost.load(std::memory_order_relaxed);
            // Re-sweep on target change, and periodically so threads created
            // after the first sweep are armed too.
            if (want != 0)
                hwWatchSweep(want);
            armed = want;
            (void)armed;
            hwWatchDrain();

            // Heartbeat: with the value filter on, a run can legitimately end
            // with zero HWWATCH records. HWSTAT proves the watchpoint was armed
            // and firing, so "no hits" can be read as "the 0x1 store never
            // happened here" rather than "the probe was dead".
            if ((tick++ % 40u) == 0u)
            {
                // seen  = every store the VEH saw and wanted to record
                // hits  = how many actually fit in the fixed array
                // drop  = seen - hits, i.e. how much of the run is INVISIBLE.
                // drop>0 means the backtraces below describe the FIRST 2048
                // stores only, so the absence of a writer in them proves
                // nothing about the rest of the run.
                const uint64_t seen =
                    g_hwWatchWrite.load(std::memory_order_relaxed);
                const uint64_t hits =
                    g_hwWatchReady.load(std::memory_order_relaxed);
                const uint64_t drop = (seen > hits) ? (seen - hits) : 0ull;
                // outwin/vbl/winlo/winhi added 2026-08-09. Zero hits now has
                // four distinguishable causes instead of one ambiguous one:
                //   armed=0                  -> no arm site ever ran; the probe
                //                               never existed. Nothing else in
                //                               this record means anything.
                //   outwin>0, skipped=0      -> the stores happen, but never in
                //                               the requested frames. Move the
                //                               window; vbl says where the run
                //                               actually got to.
                //   skipped>0, outwin=0      -> right frames, wrong value
                //                               filter. Re-run with
                //                               PS2X_HWWATCH_VAL=0xFFFFFFFF.
                //   all four zero            -> the address is genuinely never
                //                               written.
                // drop>0 still poisons every absence argument: it means the
                // records describe the first kHwWatchMaxHits stores only.
                // selfarm (2026-08-31, part 32): count of hwWatchArmSelf
                // successes, i.e. inline calling-thread arms independent of
                // this background thread's own setup latency. selfarm>0 on
                // the FIRST heartbeat directly proves DR0 was live before this
                // thread even finished starting up -- the specific guarantee
                // part 31's run was missing.
                const char *keys[12] = {"guest", "armed", "skipped", "outwin",
                                        "hits",  "seen",  "drop",    "vbl",
                                        "winlo", "winhi", "thr",     "selfarm"};
                const uint64_t vals[12] = {
                    g_hwWatchGuest.load(std::memory_order_relaxed),
                    g_hwWatchHost.load(std::memory_order_relaxed) != 0ull ? 1ull
                                                                         : 0ull,
                    g_hwWatchSkipped.load(std::memory_order_relaxed),
                    g_hwWatchOutOfWin.load(std::memory_order_relaxed), hits,
                    seen, drop,
                    // Sampled live, not from the VEH: if the handler never
                    // fires, a VEH-written value would freeze at 0 and read as
                    // "the run never advanced past frame 0" -- the probe
                    // fabricating a finding out of its own silence.
                    ps2x_vblank_ticks(),
                    g_hwWatchVblLo.load(std::memory_order_relaxed),
                    g_hwWatchVblHi.load(std::memory_order_relaxed),
                    g_hwWatchThreadsArmed.load(std::memory_order_relaxed),
                    g_hwWatchSelfArmed.load(std::memory_order_relaxed)};
                ps2x_probe_kv("HWSTAT", 12, keys, vals);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    // 2026-07-28 -- HWWATCH is now OPT-IN, off by default.
    //
    // It found the SetVSyncFlag writer (see the 07-27 entry) and is worth
    // keeping, but it is NOT free: the armer thread suspends every thread in
    // the process 4x/second to re-sweep the debug registers, and [hostprof]
    // measured it at 19.06s of CPU in a 96s run -- ~20% of the wall clock.
    // Leaving it always-on puts that cost inside every perf measurement,
    // including the Debug-vs-RelWithDebInfo A/B it would otherwise skew.
    //
    // Arm it explicitly with PS2X_HWWATCH=1 when hunting a writer; leave it
    // unset for any run whose timing matters.
    bool hwWatchEnabled()
    {
        static const bool enabled = [] {
            const char *env = std::getenv("PS2X_HWWATCH");
            return env != nullptr && env[0] != '\0' && env[0] != '0';
        }();
        return enabled;
    }

    // 2026-08-04 -- Stage 5.9 static-target mode.
    //
    // [gifsrc] proved EE RAM 0x88d400..0x8fd400 (~384 KB) is entirely zero and
    // is nonetheless uploaded to the GS as texture data every frame. The
    // question is no longer "who clobbered it" but "does ANYTHING ever write
    // it". Every existing arm site is tied to a guest hook (rpc_call's stack
    // slot, the BUG-009 client pointer); none of them can point DR0 at a fixed
    // address.
    //
    // PS2X_HWWATCH_ADDR=0x89d400 overrides whatever address the arm sites
    // publish, so any of them can act as the trigger to stand the armer up and
    // DR0 then stays parked on the fixed target for the whole run. Idempotent:
    // re-arming republishes the same address, and the armer's sweep is a no-op
    // when the target has not moved.
    //
    // Read the result this way:
    //   HWSTAT hits>0                  -> someone writes it; the .hwwatch.txt
    //                                     backtrace names the producer.
    //   HWSTAT hits=0, skipped>0       -> writes DO land, but never with the
    //                                     filtered value. Re-run with
    //                                     PS2X_HWWATCH_VAL=0xFFFFFFFF.
    //   HWSTAT hits=0 AND skipped=0    -> the region is NEVER written. The
    //                                     asset load never targets it; the
    //                                     fault is upstream in CD/IOP/decomp.
    // The armed-and-silent case is only meaningful because HWSTAT proves the
    // watchpoint was live -- see the heartbeat note in hwWatchArmerMain.
    uint32_t hwWatchStaticAddr()
    {
        static const uint32_t addr = [] {
            const char *env = std::getenv("PS2X_HWWATCH_ADDR");
            if (env == nullptr || env[0] == '\0')
                return 0u;
            return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
        }();
        return addr;
    }

    // ---------------------------------------------------------------------
    // [ramcensus] -- nonzero-page map of guest EE RAM.
    //
    // Stage 5.10 sits on a contradiction: ARKD read 1.13 MB of asset data, the
    // GS issued 434/440 image transfers, and the PSMT4 source buffer at
    // 0x8a6440 is all-zero. The existing censuses cannot arbitrate that --
    // [vramcensus] scans GS VRAM, [dmakick] scans channel starts, the VIF1
    // census scans one transfer's buffer. None of them answers the question
    // that actually decides where to look next: DID THE LOADED DATA LAND
    // ANYWHERE IN EE RAM AT ALL, AND WHERE?
    //
    // Three outcomes, all decisive:
    //   data present elsewhere  -> a copy/decompress step never ran (the bug
    //                              is a missing call, and its destination is
    //                              now a known address).
    //   data nowhere in EE RAM  -> the IOP->EE SIF delivery never happened;
    //                              back to the SIF layer, not the GS layer.
    //   data at 0x8a6440+delta  -> our source-address arithmetic is off.
    //
    // Deliberately zero-blind only, not swizzle-aware: "is there ANY nonzero
    // byte in this page" needs no format knowledge and cannot be fooled by a
    // layout assumption, which is the failure mode that has cost this project
    // retractions before.
    //
    // Cost: 32 MB of sequential reads (~10 ms), N times per run, off the hot
    // path. Output is run-length compressed to ranges, so a fully-populated
    // 32 MB prints a few dozen lines rather than 8192.
    constexpr uint32_t kRamCensusPage = 0x1000u;      // 4 KB
    constexpr uint32_t kRamCensusSize = 0x02000000u;  // 32 MB RDRAM
    constexpr uint32_t kRamCensusPages = kRamCensusSize / kRamCensusPage;

    // Interval, in traced-slot entries, between dumps. 0/unset = disabled.
    uint32_t ramCensusInterval()
    {
        static const uint32_t v = []() -> uint32_t {
            const char *env = std::getenv("PS2X_RAMCENSUS");
            if (env == nullptr || env[0] == '\0')
                return 0u;
            return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
        }();
        return v;
    }

    // How many dumps to take. Several dumps spaced apart turn a static picture
    // into a differential one: a page that is zero at dump 1 and nonzero at
    // dump 3 identifies WHEN the fill happened, which a single snapshot never
    // can. Default 3.
    uint32_t ramCensusCount()
    {
        static const uint32_t v = []() -> uint32_t {
            const char *env = std::getenv("PS2X_RAMCENSUS_N");
            if (env == nullptr || env[0] == '\0')
                return 3u;
            return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
        }();
        return v;
    }

    void ramCensusDump(const uint8_t *rdram, uint32_t dumpIndex)
    {
        // Page classification, one pass. nonzero counts BYTES, not words, so a
        // buffer holding a single stray 0x01 is still reported as live rather
        // than rounding away.
        static std::vector<uint8_t> live;   // 1 = page has a nonzero byte
        live.assign(kRamCensusPages, 0u);

        uint64_t livePages = 0u;
        uint64_t liveBytes = 0u;
        for (uint32_t p = 0; p < kRamCensusPages; ++p)
        {
            const uint8_t *page = rdram + static_cast<size_t>(p) * kRamCensusPage;
            uint32_t hits = 0u;
            for (uint32_t i = 0; i < kRamCensusPage; ++i)
            {
                if (page[i] != 0u)
                    ++hits;
            }
            if (hits != 0u)
            {
                live[p] = 1u;
                ++livePages;
                liveBytes += hits;
            }
        }

        RUNTIME_LOG("[ramcensus] dump=" << dumpIndex
                    << " livepages=" << livePages << "/" << kRamCensusPages
                    << " livebytes=" << liveBytes
                    << " progress=" << ps2x_guest_progress());

        // Run-length compress to ranges. Without this a populated 32 MB is
        // 8192 log lines and the answer drowns in its own output.
        uint32_t start = 0u;
        bool inRun = false;
        uint32_t runs = 0u;
        for (uint32_t p = 0; p <= kRamCensusPages; ++p)
        {
            const bool isLive = (p < kRamCensusPages) && live[p] != 0u;
            if (isLive && !inRun)
            {
                start = p;
                inRun = true;
            }
            else if (!isLive && inRun)
            {
                inRun = false;
                ++runs;
                // Cap the range list, not the totals above: the summary line
                // stays truthful even when the detail is elided, so this can
                // never become a silent false negative.
                if (runs <= 64u)
                {
                    RUNTIME_LOG("[ramcensus] dump=" << dumpIndex << " range=0x"
                                << std::hex << (start * kRamCensusPage) << "-0x"
                                << (p * kRamCensusPage - 1u) << std::dec
                                << " pages=" << (p - start));
                }
                else if (runs == 65u)
                {
                    RUNTIME_LOG("[cap] tag=ramcensus.range limit=64");
                }
            }
        }
        RUNTIME_LOG("[ramcensus] dump=" << dumpIndex << " ranges=" << runs);

        // The Stage 5.10 region of interest, always reported explicitly so its
        // state is never inferred from the presence or absence of a range line.
        for (uint32_t base = 0x008a0000u; base < 0x008b0000u; base += 0x4000u)
        {
            uint32_t hits = 0u;
            for (uint32_t i = 0; i < 0x4000u; ++i)
            {
                if (rdram[base + i] != 0u)
                    ++hits;
            }
            RUNTIME_LOG("[ramcensus] dump=" << dumpIndex << " roi=0x"
                        << std::hex << base << std::dec
                        << " nonzero=" << hits << "/16384");
        }
    }

    // Called from the traced-slot wrapper. Counter is relaxed-atomic because
    // several EE fibers reach this; an occasional interleaved count only shifts
    // WHEN a dump fires, never whether it is correct.
    void ramCensusTick(const uint8_t *rdram)
    {
        const uint32_t interval = ramCensusInterval();
        if (interval == 0u || rdram == nullptr)
            return;

        static std::atomic<uint32_t> s_entries{0u};
        static std::atomic<uint32_t> s_dumps{0u};
        if (s_dumps.load(std::memory_order_relaxed) >= ramCensusCount())
            return;

        const uint32_t n = s_entries.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n % interval != 0u)
            return;

        // One dumper at a time; a second fiber arriving mid-scan would
        // interleave its lines into the first dump's range list.
        static std::mutex s_mu;
        std::lock_guard<std::mutex> lock(s_mu);
        const uint32_t idx = s_dumps.fetch_add(1u, std::memory_order_relaxed);
        if (idx >= ramCensusCount())
            return;
        ramCensusDump(rdram, idx);
    }

    // ------------------------------------------------------------------
    // 2026-08-06 Stage 5.10 -- [fontgate]
    //
    // PCSX2 A/B on real hardware identified the code that fills the PSMT4
    // glyph payload at 0x008a6440. It is NOT a disc asset and NOT a DMA/SIF
    // delivery: it is a plain CPU byte blit, and the whole chain is
    //
    //   0x00422630 -> 0x00421ea0 -> 0x003280c0 -> 0x00199fb0
    //              -> 0x00112750 -> 0x00113200        (8x-unrolled lbu/sb)
    //
    // caught mid-copy at 0x0011342c with src a1=0x0054ba80, dst t0=0x008a644d,
    // length v1=0x102.
    //
    // All three leaf functions already exist as generated runner bodies, so
    // this is not missing code. sub_00112750 is guarded by two gates, and
    // sub_00199fb0 by a third:
    //
    //   A. lw v0,-0x2950(gp) with gp=0x00503070  => global word 0x00500720.
    //      Zero => sub_00112750 returns immediately and no glyph is ever
    //      written. On real hardware this reads 1.
    //   B. a0 (the font slot index) must be 1 or 2; sub_00112750 then indexes
    //      a 0x70-stride control block array at 0x0054bc80 and requires
    //      obj[+0x6C] != 0. On real hardware index 1's +0x6C reads 1.
    //   C. sub_00199fb0 early-outs when its object's [+0xC] == -1, i.e. no
    //      font slot assigned; it passes that same field as a0 to gate B.
    //
    // So exactly one of: gate A closed, gate B closed, or the upstream chain
    // never runs. This probe reads the gate state directly rather than
    // hooking the functions -- these are direct fn_ calls from recompiled
    // code, which bypass registerFunction interception entirely
    // ([[feedback_registerfunction_bypass]]), so an override wrapper would
    // see nothing.
    //
    // Emits only when the observed tuple CHANGES (plus the first sample), so
    // a gate that opens late is visible as a transition rather than buried in
    // identical repeats. Hard-capped so it can never flood the log.
    constexpr uint32_t kFontGateGlobal = 0x00500720u; // gate A
    constexpr uint32_t kFontGateObjBase = 0x0054bc80u; // gate B array
    constexpr uint32_t kFontGateObjStride = 0x70u;
    constexpr uint32_t kFontGateSrc = 0x0054ba80u;    // blit source
    constexpr uint32_t kFontGateClut = 0x008a6400u;   // 16x u32 CLUT
    constexpr uint32_t kFontGatePayload = 0x008a6440u; // 4bpp glyph bitmap
    constexpr uint32_t kFontGateMaxLines = 24u;
    // Hardware A/B (PCSX2, main menu): the glyph producer sub_00111530 reads a
    // font header whose byte at +0x1E selects the decoder (1/2/4 bpp). Anything
    // else falls through to a loop that zero-fills 0x200 bytes into the blit
    // source -- which is exactly the srcNz=0/512 we measure. On hardware that
    // header lives at 0x00659400 and reads "FFON" w=24 h=24 fmt=4. So the real
    // question is whether the FFON asset is resident at all; its load address is
    // not guaranteed stable, hence the scan as well as the fixed-address peek.
    constexpr uint32_t kFontGateHwHdr = 0x00659400u; // header addr observed on HW
    constexpr uint32_t kFontGateTable = 0x00503670u; // font descriptor table (BSS)
    constexpr uint32_t kFontMagicFFON = 0x4E4F4646u; // 'F','F','O','N' little-endian

    uint32_t fontGateInterval()
    {
        static const uint32_t v = []() -> uint32_t {
            const char *env = std::getenv("PS2X_FONTGATE");
            if (env == nullptr || env[0] == '\0')
                return 2000u; // on by default; PS2X_FONTGATE=0 disables
            return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
        }();
        return v;
    }

    uint32_t fontGateRead32(const uint8_t *rdram, uint32_t guestAddr)
    {
        const uint32_t a = guestAddr & 0x01FFFFFCu;
        uint32_t v = 0u;
        std::memcpy(&v, rdram + a, sizeof(v));
        return v;
    }

    uint32_t fontGateNonzero(const uint8_t *rdram, uint32_t guestAddr, uint32_t len)
    {
        const uint32_t a = guestAddr & 0x01FFFFFFu;
        uint32_t hits = 0u;
        for (uint32_t i = 0; i < len; ++i)
        {
            if (rdram[a + i] != 0u)
                ++hits;
        }
        return hits;
    }

    // 2026-08-07 Stage 5.10 (run 7) -- rival-reading field for the missing memset.
    // module_obj_init(glyphMap, 255, 0x904) should leave a 2308-byte run of 0xFF
    // somewhere in the text-object arena. Find where it actually landed instead of
    // assuming resolve(obj[9]) is where the game wrote. Returns the guest address
    // of the first run of >= minRun consecutive 0xFF bytes, or 0 if there is none.
    // [[feedback_degenerate_result_convicts_the_probe]]
    uint32_t fontGateFindFFRun(const uint8_t *rdram, uint32_t guestAddr, uint32_t len,
                               uint32_t minRun, uint32_t *outLen)
    {
        const uint32_t base = guestAddr & 0x01FFFFFFu;
        uint32_t run = 0u;
        for (uint32_t i = 0; i < len; ++i)
        {
            if (rdram[base + i] == 0xFFu)
            {
                ++run;
                continue;
            }
            if (run >= minRun)
            {
                if (outLen)
                    *outLen = run;
                return guestAddr + i - run;
            }
            run = 0u;
        }
        if (run >= minRun)
        {
            if (outLen)
                *outLen = run;
            return guestAddr + len - run;
        }
        if (outLen)
            *outLen = 0u;
        return 0u;
    }

    // 2026-08-07 Stage 5.10 -- handle resolution.
    //
    // Every pointer field in the text-layer object is a *handle*, not an
    // address: the small values 7/9/0xa/0xb/0xc/0xd/0xe seen in the obj dump
    // are indices into the resource manager. sub_00103E10 wraps sub_00110260,
    // which is just
    //
    //   addr = (h == 0) ? 0 : *(u32*)( *(u32*)( *(u32*)0x005030D0 ) + 16*(h-1) )
    //
    // Replicating it here is read-only and lets the probe follow the same
    // pointers the guest does, instead of guessing at fixed addresses.
    constexpr uint32_t kFontGateResMgr = 0x005030D0u;

    uint32_t fontGateResolve(const uint8_t *rdram, uint32_t handle)
    {
        if (handle == 0u)
            return 0u;
        // 2026-08-07 -- CORRECTED. This previously took one deref too many.
        // The IDA text at 0x103E10 reads `vtable_table_lookup(dword_5030D0, h)`
        // with NO cast, while its neighbours at 0x103E20/0x103E30 both cast to
        // `(unsigned int)`. That cast-free form is IDA printing the ADDRESS of
        // the global, so sub_00110260's `*a1` dereferences 0x005030D0 itself --
        // it does not dereference the value stored there a second time.
        //
        // Confirmed empirically, not just read off the listing: with this
        // formula, resolve(obj[13]=4) lands on 0x659400, whose first word is
        // 'FFON'. The old formula returned 0x771400 (magic 0x20000013).
        const uint32_t arr = fontGateRead32(rdram, kFontGateResMgr);
        if (arr == 0u)
            return 0u;
        return fontGateRead32(rdram, arr + 16u * (handle - 1u));
    }

    // Whole-RDRAM sweep for the FFON magic. 4-byte aligned; the header starts a
    // file image so alignment is safe. Returns the first hit in *firstAddr and
    // the total count (saturating at 255). Once a hit is found the answer is
    // latched, so the 32 MB sweep runs at most once per tick until it succeeds.
    uint32_t fontMagicScan(const uint8_t *rdram, uint32_t *firstAddr)
    {
        static uint32_t s_first = 0u;
        static uint32_t s_count = 0u;
        if (s_count != 0u)
        {
            *firstAddr = s_first;
            return s_count;
        }

        constexpr uint32_t kRdramBytes = 0x02000000u;
        uint32_t count = 0u;
        uint32_t first = 0u;
        for (uint32_t a = 0u; a + 4u <= kRdramBytes; a += 4u)
        {
            uint32_t w = 0u;
            std::memcpy(&w, rdram + a, sizeof(w));
            if (w != kFontMagicFFON)
                continue;
            if (count == 0u)
                first = a;
            if (count < 255u)
                ++count;
        }
        s_first = first;
        s_count = count;
        *firstAddr = first;
        return count;
    }

    void fontGateTick(const uint8_t *rdram)
    {
        const uint32_t interval = fontGateInterval();
        if (interval == 0u || rdram == nullptr)
            return;

        static std::atomic<uint32_t> s_entries{0u};
        static std::atomic<uint32_t> s_lines{0u};
        if (s_lines.load(std::memory_order_relaxed) > kFontGateMaxLines)
            return;

        const uint32_t n = s_entries.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n % interval != 0u)
            return;

        static std::mutex s_mu;
        std::lock_guard<std::mutex> lock(s_mu);

        const uint32_t gateA = fontGateRead32(rdram, kFontGateGlobal);
        // Slots 0..2. Real hardware has only index 1 live, but reading three
        // costs nothing and proves the array is where we think it is rather
        // than assuming it ([[feedback_probe_the_final_value]]).
        uint32_t ready[3] = {0u, 0u, 0u};
        uint32_t objHead[3] = {0u, 0u, 0u};
        for (uint32_t i = 0; i < 3u; ++i)
        {
            const uint32_t obj = kFontGateObjBase + i * kFontGateObjStride;
            ready[i] = fontGateRead32(rdram, obj + 0x6Cu);
            objHead[i] = fontGateRead32(rdram, obj + 0x00u);
        }
        const uint32_t srcNz = fontGateNonzero(rdram, kFontGateSrc, 0x200u);
        const uint32_t clutNz = fontGateNonzero(rdram, kFontGateClut, 0x40u);
        const uint32_t payNz = fontGateNonzero(rdram, kFontGatePayload, 0x1000u);

        // Font asset residency. hwMagic is the fixed address hardware uses;
        // ffonAddr/ffonCount answer the same question without assuming the
        // load address survived our allocator.
        const uint32_t hwMagic = fontGateRead32(rdram, kFontGateHwHdr);
        uint32_t ffonAddr = 0u;
        const uint32_t ffonCount = fontMagicScan(rdram, &ffonAddr);
        // Decoder selector byte lives at header+0x1E; 1/2/4 are real formats,
        // anything else takes the zero-fill path.
        uint32_t ffonFmt = 0xFFu;
        uint32_t ffonWH = 0u;
        if (ffonCount != 0u)
        {
            const uint32_t dims = fontGateRead32(rdram, ffonAddr + 0x1Cu);
            ffonWH = dims & 0xFFFFu;          // w = byte 0x1C, h = byte 0x1D
            ffonFmt = (dims >> 16) & 0xFFu;   // fmt = byte 0x1E
        }

        // Font-object (s5) control fields, slot 0 -- a 28-dword text layer,
        // decompiled at sub_00112C00 as dword_54BC80[28*(id-1)].
        //
        //   +0x04 char-buffer capacity (HW 0x800)   +0x08 chars used
        //   +0x10 max lines (HW 0x100)              +0x14 lines queued
        //   +0x18 lines to render                   +0x1C glyphs built
        //   +0x28 secondary glyph count             +0x6C gate B (ready)
        //
        // sub_00112940 is the enqueue API: it bumps +0x14 per line and appends
        // to +0x08. sub_00112750 runs per frame, zeroes +0x1C/+0x28, then
        // *bails to LABEL_12 when +0x14 == 0* -- skipping the glyph build, so
        // sub_00113200 later sees +0x1C == 0 and early-outs at 0x001132D0
        // without ever calling the decoder sub_00111530.
        //
        // That makes +0x14 the field that separates "nothing was ever printed"
        // from "text was printed but the glyph build failed". +0x1C alone
        // cannot: it reads 0 in both cases.
        //
        // HW (main menu, mid-frame): o08=0x22f o14=0x12 o18=0x12 o1C=0x32 o28=0x32
        uint32_t obj[24] = {0u};
        for (uint32_t i = 0; i < 24u; ++i)
            obj[i] = fontGateRead32(rdram, kFontGateObjBase + i * 4u);
        const uint32_t o08 = obj[2];
        const uint32_t o14 = obj[5];
        const uint32_t o18 = obj[6];
        const uint32_t o1C = obj[7];
        const uint32_t o28 = obj[10];

        // ------------------------------------------------------------------
        // 2026-08-07 -- why +0x1C stays 0 even though +0x14 is now nonzero.
        //
        // sub_00112750 zeroes +0x1C/+0x28, and only skips the glyph build when
        // +0x14 == 0. We measure +0x14 = 1..4, so it does NOT skip: it calls
        // sub_00111DA0 (the glyph builder) and, only if that returns nonzero,
        // sub_00113200 (the blit). +0x1C is incremented *inside* sub_00111DA0.
        //
        // sub_00111DA0 opens by resolving obj[13] (+0x34) and validating a font
        // header:
        //     *(u32*)hdr == 'FFON'  &&  (u16)*(u32*)(hdr+0x0C) == 0x100
        //     && bytes hdr+0x0E and hdr+0x0F are zero
        // On success it fills a 4-entry stack descriptor v47[]. On failure it
        // FALLS THROUGH leaving v47[] as uninitialised stack, then still passes
        // it to the per-character glyph lookup -- so every lookup misses, and
        // +0x1C (a1[7]) and +0x28 (a1[10]) are never incremented. That is
        // exactly the signature we measure (o1C=0, o28=0, srcNz=0, payloadNz=0)
        // while HW reads o1C=0x32, o28=0x32.
        //
        // The other way to get the same signature is an empty line string: the
        // builder loops over +0x18 line records (stride 28) and walks the text
        // at charBuf + *(u32*)(line+24), stopping instantly on a NUL. So dump
        // both the header validation inputs and the first line's bytes -- that
        // separates "font descriptor rejected" from "nothing to draw".
        const uint32_t hdrA = fontGateResolve(rdram, obj[13]);
        const uint32_t hdrM = hdrA ? fontGateRead32(rdram, hdrA) : 0u;
        const uint32_t hdr0C = hdrA ? fontGateRead32(rdram, hdrA + 0x0Cu) : 0u;
        const uint32_t bufA = fontGateResolve(rdram, obj[0]);
        const uint32_t lineA = fontGateResolve(rdram, obj[3]);
        const uint32_t lineOff = lineA ? fontGateRead32(rdram, lineA + 24u) : 0u;
        const uint32_t strA = (bufA != 0u && lineA != 0u) ? (bufA + lineOff) : 0u;
        uint32_t str0 = 0u;
        std::ostringstream strDump;
        strDump << std::hex;
        if (strA != 0u)
        {
            const uint32_t sb = strA & 0x01FFFFFFu;
            str0 = rdram[sb];
            for (uint32_t i = 0; i < 16u; ++i)
                strDump << (i ? "," : "") << static_cast<uint32_t>(rdram[sb + i]);
        }
        // Resolve the remaining handles too, so a resource table that is simply
        // not populated yet is visible as a row of zeros rather than being
        // mistaken for a font-format problem.
        std::ostringstream resDump;
        resDump << std::hex;
        {
            const uint32_t handles[6] = {obj[0], obj[3], obj[8], obj[9], obj[12], obj[14]};
            for (uint32_t i = 0; i < 6u; ++i)
                resDump << (i ? "," : "") << fontGateResolve(rdram, handles[i]);
        }

        // 2026-08-07 (run 3) -- ANSWERED, and it overturned the run-2 reading:
        // altB proved the resolver had one deref too many, so the resource
        // table was never broken and the FFON header check never failed. The
        // fault is downstream, inside sub_00111DA0's character loop:
        //
        //   v15 = resolve(a1[0]);           // char buffer base
        //   v14 = resolve(a1[3]);           // line records, stride 28
        //   v21 = v15 + *(u32*)(v14 + 24);  // this line's text
        //   while (*v21) { ... a1[10]++ ... }
        //
        // a1[10] (o28) is incremented on BOTH character paths -- the newly
        // allocated glyph branch and the already-cached branch -- so o28 == 0
        // proves the loop body never ran for a single character. Only two
        // things produce that: the text pointer lands on a NUL, or the
        // char->glyph decode returns negative every time.
        //
        // str0 (above) already separates those. Everything here is the
        // supporting state needed to act on whichever way it falls.
        const uint32_t hdr10 = hdrA ? fontGateRead32(rdram, hdrA + 0x10u) : 0u;
        // The exact three-part validation from the decompile, so a partial pass
        // is distinguishable from a total one.
        const uint32_t hdrOK = (hdrM == 0x4E4F4646u &&
                                (hdr0C & 0xFFFFu) == 0x100u &&
                                (hdr0C & 0x00FF0000u) == 0u &&
                                (hdr0C & 0xFF000000u) == 0u)
                                   ? 1u
                                   : 0u;
        // The four handles the loop resolves besides the font and the text:
        //   glyphMap  = a1[9]  -- u16 per glyph slot, 0x8000 = free
        //   charTab   = a1[8]  -- u16 written per built glyph, indexed by a1[7]
        //   freeList  = a1[12] -- LIFO of free slots, popped via a1[11]
        //   cacheMap  = a1[14] -- 4 bytes per slot, +2 == 2 means resident
        const uint32_t glyphMap = fontGateResolve(rdram, obj[9]);
        const uint32_t charTab = fontGateResolve(rdram, obj[8]);
        const uint32_t freeList = fontGateResolve(rdram, obj[12]);
        const uint32_t cacheMap = fontGateResolve(rdram, obj[14]);
        // Raw first line record (28 bytes, 7 words). +24 is the text offset the
        // loop adds to the char buffer; the rest identifies the line.
        std::ostringstream lineDump;
        lineDump << std::hex;
        for (uint32_t i = 0; i < 7u; ++i)
            lineDump << (i ? "," : "")
                     << (lineA ? fontGateRead32(rdram, lineA + 4u * i) : 0u);
        // How much of the char buffer is actually populated? A live buffer with
        // str0 == 0 would mean the line offset is wrong rather than the text
        // being absent -- a different bug with a different owner.
        const uint32_t bufNz = bufA ? fontGateNonzero(rdram, bufA, 256u) : 0u;

        // 2026-08-07 (run 4) -- run 3 answered str: the text IS there.
        //   str=<4e,6f,20,6d,65,6d,6f,72,79,...> == "No memory card ("
        //   bufNz=180/256, lineOff=0, line0=<19,132,3f800000,12,12,ffffffff,0>
        // so v21 = v15 + *(u32*)(v14+24) points at 'N', not a NUL. That kills
        // the "nothing to draw" branch and leaves the char->glyph decode.
        //
        // CORRECTION to run 3's note: o18 (a1[6]) cycling 4->3->2->1 does NOT
        // prove sub_00111DA0's outer loop ran -- a1[6] is written by the line
        // queueing code, not by the builder. So two causes still stand, and
        // this probe separates them by replicating the decode exactly.
        //
        // reg_save_stub_z_32 @ 0x111A00, for an ASCII byte c < 0x80:
        //     page = *(u8 *)(v47[1] + 0);            v47[1] = hdr + 32
        //     if (!page) return -1;
        //     idx  = *(u16 *)(v47[2] + 2*(c - 32 + 96*(page - 1)));
        //     if (!idx)  return -1;                  v47[2] = hdr + 160
        //     return idx - 1;
        // Both tables live inside the FFON asset, so if the asset body arrived
        // zero-filled (which is also what payloadNz=0/4096 would look like
        // downstream) every character decodes to -1 and a1[10] never moves.
        //
        // Branches:
        //   dcRes >= 0  -> decode works, so sub_00111DA0 is never being CALLED;
        //                  move the probe up to its caller sub_00112750.
        //   dcRes <  0  -> decode fails; pgNz/gmNz say whether the FFON tables
        //                  are empty (asset body never loaded) or merely wrong.
        const uint32_t descPage = hdrA ? hdrA + 32u : 0u;
        const uint32_t descIdx = hdrA ? hdrA + 160u : 0u;
        const uint32_t dcCh = str0;
        uint32_t dcPage = 0u;
        uint32_t dcSlot = 0u;
        int32_t dcRes = -1;
        if (hdrA != 0u && dcCh != 0u && dcCh < 0x80u)
        {
            dcPage = rdram[descPage & 0x01FFFFFFu];
            if (dcPage != 0u)
            {
                const uint32_t off = 2u * (dcCh - 32u + 96u * (dcPage - 1u));
                const uint32_t w = descIdx
                                       ? fontGateRead32(rdram, (descIdx + off) & ~3u)
                                       : 0u;
                dcSlot = ((descIdx + off) & 2u) ? (w >> 16) : (w & 0xFFFFu);
                if (dcSlot != 0u)
                    dcRes = static_cast<int32_t>(dcSlot) - 1;
            }
        }
        // Is the FFON asset body actually present, or just its header?
        const uint32_t pgNz = hdrA ? fontGateNonzero(rdram, hdrA + 32u, 128u) : 0u;
        const uint32_t gmNz = hdrA ? fontGateNonzero(rdram, hdrA + 160u, 512u) : 0u;
        const uint32_t ffonNz = hdrA ? fontGateNonzero(rdram, hdrA, 0x1000u) : 0u;

        // 2026-08-07 (run 5) -- run 4 answered dcRes: the decode WORKS.
        //   dcCh=0x4e dcPage=0x1 dcSlot=0x2f dcRes=46, FFON body populated
        //   (pgNz=40/128 gmNz=252/512 ffonNz=1431/4096). So both run-4 causes
        //   are dead: the tables are there and they resolve.
        //
        // New witness, read straight out of run 4's own obj dump: obj[11] --
        // the glyph free-list stack pointer at +0x2C -- reads 0x200 in ALL 13
        // records, n=2000..36000. sub_00111DA0 decrements it on every glyph it
        // allocates (a1[11] = --v26), and the caller sub_00112750 never clears
        // it (it clears only +8/+14/+18/+1C/+28). So it is a persistent,
        // monotone counter and it has NEVER MOVED: zero glyphs allocated, ever.
        //
        // Why the allocation is skipped, from the decompile:
        //     v23 = glyphMap + 2*glyphIdx;   v24 = *(u16*)v23;
        //     if (v24 & 0x8000) { ...allocate from free list... }
        //     else if (*(u8*)(cacheMap + 4*v24 + 2) != 2) ++a1[10];   // no-op
        // 0x8000 is the "slot empty, go allocate" marker. A ZERO-FILLED
        // glyphMap has the bit clear, so every character takes the else branch,
        // silently pretends slot 0 is a cache hit, allocates nothing, and
        // builds nothing -- which is exactly payloadNz=0/4096.
        //
        // glyphMap is seeded with 0x8000 by the aging loop at the tail of
        // sub_00112750 (*(u16*)(v11 + 2*...) = 0x8000, v11 = resolve(a1[9])),
        // and that loop is gated on  *(u32*)(a1+44) < *(u32*)(a1+92)  i.e.
        // obj[11] < obj[23]  ==  0x200 < 0x200  ==  FALSE. It never runs.
        // So: if nothing else ever seeds glyphMap, the cache can never fill.
        //
        // Read: gmap8k counts how many of the 512 u16 slots carry 0x8000.
        //   gmap8k == 0            -> confirmed, glyphMap was never seeded
        //   gmap8k  > 0, o2C==0x200-> seeded but allocation still never fires
        //   o2C < 0x200            -> glyphs ARE allocated; fault moves to
        //                             mem_fill_n_0 @ 0x112030 (the GS emit)
        const uint32_t o2C = fontGateRead32(rdram, kFontGateObjBase + 0x2Cu);
        const uint32_t o5C = fontGateRead32(rdram, kFontGateObjBase + 0x5Cu);
        const uint32_t o6C = fontGateRead32(rdram, kFontGateObjBase + 0x6Cu);
        uint32_t gmapW = 0u;
        uint32_t gmap8k = 0u;
        uint32_t gmapNz = 0u;
        if (glyphMap != 0u)
        {
            gmapNz = fontGateNonzero(rdram, glyphMap, 1024u);
            for (uint32_t i = 0; i < 512u; ++i)
            {
                const uint32_t w = fontGateRead32(rdram, (glyphMap + 2u * i) & ~3u);
                const uint32_t h = ((glyphMap + 2u * i) & 2u) ? (w >> 16) : (w & 0xFFFFu);
                if (h & 0x8000u)
                    ++gmap8k;
                if (dcRes >= 0 && i == static_cast<uint32_t>(dcRes))
                    gmapW = h;
            }
        }

        // 2026-08-07 (run 6) -- run 5 confirmed the seed branch: gmap8k=0/512 and
        //   gmapNz=0/1024, i.e. glyphMap @0x8a1400 is entirely zero, while
        //   o2C stayed 0x200 in all 13 records. Never seeded, never allocated.
        //
        // Found the seeder. The object-create routine (line 14490 of the
        // decompile, the one that ends `*(u32*)(v18+44) = *(u32*)(v18+92)`,
        // which is exactly our obj[11]=obj[23]=0x200) does:
        //
        //     v22 = resolve(*(int*)(v18+36));            // obj[9]  = glyphMap
        //     v24 = resolve(*(int*)(v18+48));            // obj[12] = freeList
        //     v23 = resolve(*(int*)(v18+52));            // obj[13] = font
        //     if (*(u32*)v23 == 'FFON' && hdr check ok) v13 = v23;
        //     module_obj_init(v22, 255, 2 * *(u32*)(v13 + 24));   // <- memset 0xFF
        //     do { *(u16*)v24 = --count; v24 += 2; } while (...); // free list
        //     *(u32*)(v18+28) = 0;
        //     *(u32*)(v18+44) = *(u32*)(v18+92);                  // obj[11] = 0x200
        //
        // module_obj_init @0x18E408 is a memset (pcpyh/pcpyld + sq stores), so
        // glyphMap is supposed to come out 0xFFFF-filled -- and 0xFFFF has the
        // 0x8000 "slot empty" bit set. That is the seed. It did not happen.
        //
        // Two candidates, and freeList tells them apart, because the free-list
        // fill loop sits a few lines AFTER the memset in the same block:
        //   flNz > 0  -> the init routine DID run; the memset length
        //                2 * *(u32*)(hdr+0x18) must have been 0 -> read hdr18
        //   flNz == 0 -> the init routine never ran for this object at all,
        //                yet obj[11]=0x200 -- meaning 0x200 is static data,
        //                not the init's write, and the create path is skipped
        // ctNz/cmNz are carried as cheap corroboration: if every one of the six
        // buffers is zero, nothing about this object was ever constructed.
        const uint32_t hdr18 = hdrA ? fontGateRead32(rdram, hdrA + 0x18u) : 0u;
        const uint32_t flNz = freeList ? fontGateNonzero(rdram, freeList, 1024u) : 0u;
        const uint32_t fl0 = freeList ? fontGateRead32(rdram, freeList) : 0u;
        const uint32_t ctNz = charTab ? fontGateNonzero(rdram, charTab, 1024u) : 0u;
        const uint32_t cmNz = cacheMap ? fontGateNonzero(rdram, cacheMap, 2048u) : 0u;

        // 2026-08-07 (run 7). ANSWERED, and it is branch 3, not branch 1 or 2:
        //   hdr18=0x482  fl0=0x1fe01ff  flNz=766/1024  gmapNz=0/1024
        // The free-list is a byte-perfect match for the descending fill
        // 0x1ff,0x1fe,...,0x000 (510 nonzero low bytes + 256 nonzero high bytes
        // = 766, and fl0 is exactly 0x01fe,0x01ff little-endian). So:
        //   * the create routine RAN,
        //   * fontGateResolve() is correct -- handle 0xe -> 0x8a4400 provably
        //     holds the game's own data,
        //   * the memset length was NOT zero: 2 * hdr18 = 0x904 = 2308 bytes.
        // The memset sits immediately BEFORE that fill in the same basic block.
        // The fill worked; the memset did not land where we are looking.
        //
        // Ruled out already: translatePCPYH and PS2_PCPYLD are both correct
        // (_mm_set_epi16(h,h,h,h,l,l,l,l) and _mm_unpacklo_epi64(rt,rs)), and a
        // globally broken `sq` would take the whole game down, not one buffer.
        //
        // So before blaming the guest, find where 2308 bytes of 0xFF actually
        // went. Scan the whole text-object arena 0x00890000..0x008b0000:
        //   ffAt == glyphMap(0x8a1400)      -> impossible, gmapNz would be > 0
        //   ffAt == some other buffer addr  -> resolve(obj[9]) picks the wrong
        //                                      slot; the handle->slot mapping is
        //                                      off and the game memset elsewhere
        //   ffAt == 0                       -> the memset genuinely stored
        //                                      nothing; chase sub_18E408 itself
        // ffLen distinguishes a real 0x904 fill from incidental 0xFF padding.
        uint32_t ffLen = 0u;
        const uint32_t ffAt =
            fontGateFindFFRun(rdram, 0x00890000u, 0x00020000u, 256u, &ffLen);

        // Font descriptor table, 0x30-byte records, indexed at 0x00113350 and
        // consumed by sub_0010DDF0 to build the descriptor sub_00111530 reads.
        // It sits past PT_LOAD filesz (0x500680) so it is BSS -- runtime-built.
        // HW record 0: 0x2b600001 0x00082b08 0x00030008 0x00140001
        const uint32_t tbl0 = fontGateRead32(rdram, kFontGateTable);
        const uint32_t tbl1 = fontGateRead32(rdram, kFontGateTable + 4u);
        const uint32_t tblNz = fontGateNonzero(rdram, kFontGateTable, 0x300u);

        // Change detection over the whole tuple.
        static bool s_seen = false;
        static uint32_t s_prev[47] = {0u};
        const uint32_t cur[47] = {gateA, ready[0], ready[1], ready[2],
                                  objHead[1], srcNz, clutNz, payNz,
                                  hwMagic, ffonAddr, ffonCount, ffonFmt,
                                  o08, o14, o18, o1C, o28, tbl0, tblNz,
                                  hdrA, hdrM, hdr0C, str0,
                                  hdr10, hdrOK, glyphMap, charTab, cacheMap,
                                  bufNz,
                                  dcCh, dcPage, dcSlot, pgNz, gmNz,
                                  o2C, o5C, o6C, gmapW, gmap8k, gmapNz,
                                  hdr18, flNz, fl0, ctNz, cmNz,
                                  ffAt, ffLen};
        bool changed = !s_seen;
        for (uint32_t i = 0; i < 47u && !changed; ++i)
        {
            if (cur[i] != s_prev[i])
                changed = true;
        }
        if (!changed)
            return;
        s_seen = true;
        std::memcpy(s_prev, cur, sizeof(s_prev));

        const uint32_t line = s_lines.fetch_add(1u, std::memory_order_relaxed);
        if (line >= kFontGateMaxLines)
        {
            RUNTIME_LOG("[cap] tag=fontgate limit=" << kFontGateMaxLines);
            return;
        }

        // Full 28-dword layer dump, so a future divergence in a field we have
        // not named yet is visible without another build cycle.
        std::ostringstream objDump;
        objDump << std::hex;
        for (uint32_t i = 0; i < 24u; ++i)
            objDump << (i ? "," : "") << obj[i];

        RUNTIME_LOG("[fontgate] n=" << n
                    << " progress=" << ps2x_guest_progress()
                    << std::hex
                    << " gateA@0x500720=0x" << gateA
                    << " ready0=0x" << ready[0]
                    << " ready1=0x" << ready[1]
                    << " ready2=0x" << ready[2]
                    << " obj1head=0x" << objHead[1]
                    << " hwHdr@0x659400=0x" << hwMagic
                    << " ffonAddr=0x" << ffonAddr
                    << " ffonWH=0x" << ffonWH
                    << " ffonFmt=0x" << ffonFmt
                    << " o08=0x" << o08
                    << " o14=0x" << o14
                    << " o18=0x" << o18
                    << " o1C=0x" << o1C
                    << " o28=0x" << o28
                    // Angle brackets, not square: analyze_run.py splits records
                    // on '[' and silently truncated every field after obj= on
                    // the 2026-08-06 run.
                    << " obj=<" << objDump.str() << ">"
                    << " hdrA=0x" << hdrA
                    << " hdrM=0x" << hdrM
                    << " hdr0C=0x" << hdr0C
                    << " strA=0x" << strA
                    << " str=<" << strDump.str() << ">"
                    << " res=<" << resDump.str() << ">"
                    << " bufA=0x" << bufA
                    << " lineA=0x" << lineA
                    << " lineOff=0x" << lineOff
                    << " line0=<" << lineDump.str() << ">"
                    << std::dec << " bufNz=" << bufNz << "/256 hdrOK=" << hdrOK << std::hex
                    << " hdr10=0x" << hdr10
                    << " glyphMap=0x" << glyphMap
                    << " charTab=0x" << charTab
                    << " freeList=0x" << freeList
                    << " cacheMap=0x" << cacheMap
                    << " dcCh=0x" << dcCh
                    << " dcPage=0x" << dcPage
                    << " dcSlot=0x" << dcSlot
                    << std::dec << " dcRes=" << dcRes
                    << " pgNz=" << pgNz << "/128"
                    << " gmNz=" << gmNz << "/512"
                    << " ffonNz=" << ffonNz << "/4096" << std::hex
                    << " o2C=0x" << o2C
                    << " o5C=0x" << o5C
                    << " o6C=0x" << o6C
                    << " gmapW=0x" << gmapW
                    << std::dec << " gmap8k=" << gmap8k << "/512"
                    << " gmapNz=" << gmapNz << "/1024" << std::hex
                    << " hdr18=0x" << hdr18
                    << " fl0=0x" << fl0
                    << std::dec << " flNz=" << flNz << "/1024"
                    << " ctNz=" << ctNz << "/1024"
                    << " cmNz=" << cmNz << "/2048" << std::hex
                    << " ffAt=0x" << ffAt
                    << std::dec << " ffLen=" << ffLen << std::hex
                    << " tbl0=0x" << tbl0
                    << " tbl1=0x" << tbl1
                    << std::dec
                    << " tblNz=" << tblNz << "/768"
                    << " ffonCount=" << ffonCount
                    << " srcNz=" << srcNz << "/512"
                    << " clutNz=" << clutNz << "/64"
                    << " payloadNz=" << payNz << "/4096");
    }

    // Guest-side entry point. Cheap and lock-free; safe to call on every
    // rpc_call true entry even though the slot address moves between calls.
    void hwWatchArm(const uint8_t *rdram, uint32_t guestAddr)
    {
        if (!hwWatchEnabled())
            return;
        if (rdram == nullptr)
            return;
        if (const uint32_t fixed = hwWatchStaticAddr())
            guestAddr = fixed;
        const uint32_t masked = guestAddr & 0x1FFFFFFCu; // 4-byte aligned
        const uint64_t host =
            reinterpret_cast<uint64_t>(rdram) + static_cast<uint64_t>(masked);

        g_hwWatchGuest.store(masked, std::memory_order_relaxed);
        g_hwWatchHost.store(host, std::memory_order_relaxed);

        // 2026-08-31 part 32: arm synchronously, inline, before handing off to
        // the background thread -- see the note above hwWatchEnsureVeh/
        // hwWatchArmSelf for why the old background-only path could lose the
        // race against a freeze that lands inside the first real second.
        hwWatchEnsureVeh();
        hwWatchArmSelf(host);

        bool expected = false;
        if (g_hwWatchArmerUp.compare_exchange_strong(expected, true,
                                                     std::memory_order_relaxed))
        {
            std::thread(hwWatchArmerMain).detach();
        }
    }

    // One distinct wrapper per slot (template instantiation => distinct function
    // address), so each knows its own slot without searching by pc.
    template <size_t I>
    void sdbzFrameTraceWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_sdbzFrameTraceOriginals[I];
        if (original == nullptr)
        {
            return;
        }

        const uint32_t entryPc = ctx->pc;
        const uint32_t entrySp = GPR_U32(ctx, 29);
        // Phase C stack-bounds guard (site 0 = slot entry). See the declaration
        // near the top of this file.
        ps2x_stack_check(entryPc, entrySp, 0u);

        // 2026-08-04 Stage 5.9 -- PS2X_HWWATCH_ADDR arm. The other two arm
        // sites are both behind narrow guest conditions (rpc_call true entry,
        // one specific RPC client pointer); neither is guaranteed to run in a
        // MainMenu-only session. This one runs on every traced slot, so the
        // armer thread comes up early and DR0 lands on the fixed target
        // regardless of which guest paths execute. hwWatchArm is a no-op unless
        // PS2X_HWWATCH=1, and it overrides guestAddr with the env value anyway.
        if (hwWatchStaticAddr() != 0u)
        {
            hwWatchArm(rdram, 0u);
        }
        // 2026-08-06 Stage 5.10 -- [ramcensus]. Same reasoning as the arm above:
        // this wrapper is the only site guaranteed to run in a MainMenu-only
        // session. No-op unless PS2X_RAMCENSUS is set.
        ramCensusTick(rdram);
        // 2026-08-06 Stage 5.10 -- [fontgate]. Same site, same reasoning: this
        // is the one wrapper guaranteed to run in a MainMenu-only session.
        // Rate-limited and change-gated internally; PS2X_FONTGATE=0 disables.
        fontGateTick(rdram);
        // 2026-07-22 -- entry-$ra probe. The frametrace records the RESTORED
        // $ra (exitRa) but never the value the caller handed in. For the
        // rpc_call ($0x178be8) $ra=0x1 derail that is the whole question:
        //   entryRa == exitRa == 0x1  => the CALLER passed 0x1 (fork A: bad
        //     caller / uninitialised RPC client chain upstream). The function
        //     faithfully saved and restored a garbage return address.
        //   entryRa valid, exitRa 0x1 => the saved-$ra slot was clobbered
        //     mid-body (fork B: a stomper inside this function or a callee).
        // exitRa already equals the reloaded stack slot (epilogue `ld $ra`),
        // so entryRa is the only missing half of the comparison.
        const uint32_t entryRa = GPR_U32(ctx, 31);

        // 2026-07-22 SLOTWATCH arm. Must run BEFORE original() executes the body,
        // so the nested wrapped callees dispatched inside it can observe the slot.
        const uint32_t funcStart = kSdbzFrameTraceSlots[I].funcStart;
        const bool isRpcTrueEntry =
            funcStart == 0x00178BE8u &&
            kSdbzFrameTraceSlots[I].addr == funcStart &&
            entryPc == funcStart;
        if (isRpcTrueEntry)
        {
            g_rpcSlotExpected.store(entryRa, std::memory_order_relaxed);
            g_rpcSlotAddr.store(entrySp - 0x10u, std::memory_order_relaxed); // newsp(-0xC0)+0xB0
            // 2026-07-27: point the hardware watchpoint at the same slot. The
            // slot address is NOT constant across rpc_call invocations (seen
            // 0x1ffbec0 and 0x1ffbe30), so re-publish it every true entry
            // rather than latching the first one.
            //
            // 2026-07-30: this re-arms on EVERY rpc_call entry, which starves
            // the BUG-009 client->hdr.pkt_addr hunt (sdbzDiagRpcHandleValid178DE8
            // arms the same single DR0 slot, conditionally, far less often) --
            // this stack-slot arm wins the race almost every time and the
            // pkt_addr watch never gets a chance to fire. Skip this arm when
            // PS2X_HWWATCH_CLIENT selects the client-target hunt instead.
            static const bool s_clientHuntActive =
                std::getenv("PS2X_HWWATCH_CLIENT") != nullptr;
            if (!s_clientHuntActive)
            {
                hwWatchArm(rdram, entrySp - 0x10u);
            }
        }

        // 2026-08-31 session 5 part 41c -- sub_17CF50's $ra slot. First two
        // attempts (41/41b) armed entrySp+0x20 and got a clean armed=1/
        // selfarm=0x987/hits=0/seen=0 result -- a genuine "never written"
        // negative on the WRONG address. `entrySp` here is captured at the
        // wrapper's start, BEFORE the real prologue's `addiu $sp,$sp,-0x30`
        // (fn_17CF50_0x17cf50.cpp:29-37) runs, so `0x20($sp)` in that
        // prologue's own frame of reference is entrySp-0x30+0x20 = entrySp-
        // 0x10, not entrySp+0x20. Confirmed by cross-referencing THIS run's
        // own data, not by re-deriving on paper: [frametrace:cf50wrap] #1
        // showed entrySp=0x1ffbee0 for the same invocation whose resume-time
        // sp (post-decrement, read by [semwatch:cf50resume] #1 in
        // EeScheduler.cpp) was 0x1ffbeb0 -- exactly 0x30 less, confirming the
        // arithmetic. entryPc==funcStart restricts this to the TRUE entry
        // slot (kSdbzFrameTraceSlots[I].addr==0x17CF50), never one of the
        // three resume slots also wrapped above. Re-published every true
        // entry since sp is not constant across invocations. Deliberately
        // UNCONDITIONAL on PS2X_HWWATCH_CLIENT -- that var exists to silence
        // the COMPETING rpc_call arm above, not this one.
        const bool isCf50TrueEntry =
            funcStart == 0x0017CF50u &&
            kSdbzFrameTraceSlots[I].addr == funcStart &&
            entryPc == funcStart;
        // 2026-08-31 part 41b -- two prior runs with this arm in place showed
        // ZERO [HWSTAT]/[HWWATCH] records despite [frametrace] confirming all
        // 79 slots (this one included) installed cleanly. Rather than guess
        // why, print unconditionally (no PS2X_HWWATCH dependency) whenever
        // funcStart==0x17CF50u fires AT ALL, showing entryPc/addr/I and the
        // isCf50TrueEntry verdict -- settles whether the wrapper never runs
        // for this slot, or runs but the guard never evaluates true. Capped;
        // unconditional.
        if (funcStart == 0x0017CF50u)
        {
            static std::atomic<uint32_t> s_cf50WrapperLogs{0u};
            const uint32_t n = s_cf50WrapperLogs.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
            {
                std::cerr << "[frametrace:cf50wrap] #" << n
                          << " I=" << I
                          << " slotAddr=0x" << std::hex << kSdbzFrameTraceSlots[I].addr
                          << " entryPc=0x" << entryPc
                          << " entrySp=0x" << entrySp
                          << std::dec
                          << " isTrueEntry=" << isCf50TrueEntry
                          << std::endl;
            }
        }
        if (isCf50TrueEntry)
        {
            hwWatchArm(rdram, entrySp - 0x10u);
        }

        // 2026-07-22 SLOTWATCH2 -- fn_178068 is SLOTWATCH's #1 (deepest wrapped
        // callee) and the first frame to see slot=0x1. Arithmetic: its prologue
        // `addiu $sp,-0xA0` + `sd $ra,0x90($sp)` (0x178074) writes to sp-0x10.
        // That equals the watched slot 0x1ffbeb0 IFF its entry sp == rpc_call's
        // entry sp (0x1ffbec0) -- i.e. the 8-deep chain produced ZERO net stack
        // descent, so 178068's frame [sp-0xA0,sp) overlaps rpc_call's. Measure it:
        // read the slot BEFORE 178068's body and log its entry sp/ra.
        //   slotBefore==expected & slotAfter==0x1 => stomp is INSIDE 178068 (own
        //     store, or callees 0x175090 / the 0x178180 jalr).
        //   entrySp==0x1ffbec0 & storeTarget(sp-0x10)==slotAddr => pins it to the
        //     0x178074 `sd $ra` store, and entryRa is then the source of the 0x1.
        //   slotBefore already 0x1 => stomp happened up-chain, before 178068 ran.
        const bool is178068 = (funcStart == 0x00178068u &&
                               g_rpcSlotAddr.load(std::memory_order_relaxed) != 0u);
        uint32_t slot178068Before = 0u;
        if (is178068)
        {
            slot178068Before = READ32(g_rpcSlotAddr.load(std::memory_order_relaxed));
        }

        // 2026-08-29 -- [semwatch] follow-up #3. dispidx (gated on funcStart==
        // 0x175090) never fired -- the fresh decompile of sub_178068 shows why:
        // `dword_5616D8` is NOT itself the ready-gate byte. It is a pointer
        // VARIABLE (at address 0x5616D8) that sub_177B00 initializes ONCE to the
        // constant 0x20561600. The gate/size byte sub_178068 actually tests is
        // `*(BYTE*)dword_5616D8`, i.e. the byte AT 0x20561600 -- a different
        // address entirely. The IMBAL/watchdog data from the dispidx run already
        // shows sub_178068 exiting at 0x1780c0 (the early `if (!byte) return 0;`
        // path, per decompiles_SLUS_214_42.txt:94822-94874) -- syscall_stub_z_24
        // is never reached because the gate reads 0 every time. Log the REAL
        // gate byte on every entry, unconditional, bounded, to see whether it's
        // ever nonzero anywhere in a run (race: arrives late) or stays 0 the
        // whole time (missing producer).
        if (funcStart == 0x00178068u)
        {
            static std::atomic<uint32_t> s_gateByteDumps{0};
            const uint32_t n = s_gateByteDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                std::cerr << "[semwatch:gatebyte] #" << std::dec << n
                    << " ptr=0x" << std::hex << READ32(0x005616D8u)
                    << " gate=0x" << (uint32_t)READ8(0x20561600u)
                    << " entrySp=0x" << entrySp
                    << std::endl;
            }
        }

        // 2026-07-23 SLOTWATCH BEFORE-read. Capture the watched slot on the way
        // IN to every armed callee (per-frame local). Paired with the after-read
        // in the SLOTWATCH block below, the DEEPEST wrapped frame whose
        // before==expected & after==0x1 brackets the writer to its own body /
        // unwrapped leaves. Frames entered already-dirty (before==0x1) are below
        // the writer. This is the datum SLOTWATCH2's gate accidentally filtered.
        const uint32_t slotWatchAddrPre = g_rpcSlotAddr.load(std::memory_order_relaxed);
        const uint32_t slotBeforeCall =
            (slotWatchAddrPre != 0u && funcStart != 0x00178BE8u)
                ? READ32(slotWatchAddrPre)
                : 0xFFFFFFFFu;

        // 2026-07-25h -- SLOTWATCH ENTRY probe. Every 07-25g AFTER-sample (idle
        // 178068, real 178560, real 178068, 177eb0, 177fe8) already read before==0x1,
        // i.e. dirty at THEIR OWN entry -- the stomp is earlier than any of them.
        // 178428/17ed60/17edb0 sit between rpc_call's arm and 177fe8 in the chain
        // but never logged (crash cut the unwind short before their own AFTER-check
        // ran). Log BEFORE unconditionally here (bounded) so the entry sequence
        // itself -- not just post-return comparisons -- shows which of these three
        // is first to see the slot already dirty at its own entry.
        if (slotWatchAddrPre != 0u && funcStart != 0x00178BE8u)
        {
            static std::atomic<uint32_t> s_slotEntryDumps{0};
            const uint32_t n = s_slotEntryDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 40u)
            {
                const uint32_t expected = g_rpcSlotExpected.load(std::memory_order_relaxed);
                // 2026-07-25j -- 17edb0 (clean) -> 177fe8 (dirty) bracket has ZERO
                // memory stores in either intervening callee (cpu_enable_interrupts_
                // 0x17edb0 and cache_writeback_range_0x1781b0 are both pure-register/
                // no-op-CACHE bodies, verified by reading the generated .cpp). That
                // rules out a same-thread sequential stomp in this chain and points
                // at a DIFFERENT host thread writing the same guest address
                // concurrently (178068 already showed two very different entrySp
                // ranges -- ~0x1f00000 idle vs ~0x1ffbcXX worker -- in the same
                // "expected" bucket, proving concurrent threads share g_rpcSlotExpected
                // and can interleave in this log). Tag each line with a short thread
                // hash so the sequence can be filtered to one thread's own timeline.
                const uint32_t tid = static_cast<uint32_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                std::ostringstream oss;
                oss << "[frametrace:SLOTENTRY] #" << std::dec << n
                    << " tid=0x" << std::hex << tid
                    << " callee=0x" << funcStart
                    << " entryPc=0x" << entryPc
                    << " entrySp=0x" << entrySp
                    << " expected=0x" << expected
                    << " before=0x" << slotBeforeCall
                    << "\n";
                std::cerr << oss.str();
                {
                    static const char *const k[] = {
                        "n", "callee", "entryPc", "entrySp", "expected", "before"};
                    const uint64_t v[] = {n, funcStart, entryPc, entrySp, expected, slotBeforeCall};
                    ps2x_probe_kv("SLOTENTRY", 6, k, v);
                }
            }
        }

        // 2026-07-25k -- GSENTRY. The SLOTENTRY probe above is gated on
        // slotWatchAddrPre != 0, i.e. it only logs while rpc_call (0x178be8) has
        // a watch armed. GS_DispatchPending runs OUTSIDE that window, so the 8
        // GS slots added in 07-25j could never emit a line -- the 07-25j run's
        // empty grep was a probe-design artifact, not evidence that GS never ran
        // (the watchdog trace at that run's line 2899 shows 0x102870 executing).
        // Log GS entries unconditionally instead, bounded. entryPc distinguishes
        // real-entry-with-stale-sp from resume-slot aliasing; entrySp is the
        // number that matters (0x1ffbf00 == the overlapping frame).
        // 2026-08-29 -- [semwatch] follow-up. semwatch proved iSignalSema(4)
        // never fires this run. 0x178560 is the SIF dispatcher's _request_end
        // completion-callback handler (see the kSdbzFrameTraceSlots comment
        // above) -- the only known place a completion callback could trigger
        // that signal. It's already wrapped (entry-only) but the frametrace
        // ring only logs on anomaly/VECCTOR/LEAFEXIT conditions, not on plain
        // entry, so an unreached function and a silently-clean one look
        // identical in that ring. Log entry unconditionally, bounded, same as
        // the GSENTRY probe below, to settle it directly instead of inferring
        // from the (thread-interleaved, ring-limited) trace= history.
        if (kSdbzFrameTraceSlots[I].funcStart == 0x00178560u)
        {
            static std::atomic<uint32_t> s_requestEndEntryDumps{0};
            const uint32_t n = s_requestEndEntryDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                std::cerr << "[semwatch:reqend] #" << std::dec << n
                    << " entryPc=0x" << std::hex << entryPc
                    << " entrySp=0x" << entrySp
                    << " entryRa=0x" << entryRa
                    << std::endl;
            }
        }
        if (kSdbzFrameTraceSlots[I].funcStart == 0x00102870u)
        {
            static std::atomic<uint32_t> s_gsEntryDumps{0};
            const uint32_t n = s_gsEntryDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                const uint32_t tid = static_cast<uint32_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                std::ostringstream oss;
                oss << "[frametrace:GSENTRY] #" << std::dec << n
                    << " tid=0x" << std::hex << tid
                    << " entryPc=0x" << entryPc
                    << " entrySp=0x" << entrySp
                    << " entryRa=0x" << entryRa
                    << "\n";
                std::cerr << oss.str();
                {
                    static const char *const k[] = {"n", "entryPc", "entrySp", "entryRa"};
                    const uint64_t v[] = {n, entryPc, entrySp, entryRa};
                    ps2x_probe_kv("GSENTRY", 4, k, v);
                }
            }
        }

        // 2026-07-25L -- LEAFENTRY. Same gating trap as GSENTRY: the SLOTENTRY
        // probe only logs while rpc_call has a watch armed, so the 07-25k leaf
        // slots emitted nothing despite the trace showing 0x191780 running 323
        // times. Log these unconditionally -- but only when $ra is ALREADY out of
        // guest range on entry, or changed across the body. Healthy calls stay
        // silent, so the budget is spent on the derail instead of the 300+ good
        // iterations. The first frame to report entry-dirty is where $ra died.
        //
        // 2026-07-25m -- WIDENED TO EVERY SLOT. Three consecutive runs took three
        // completely different loops (0x2b9300/0x2b93e0, then 0x32bbb0/0x32bca0/
        // 0x191898, then 0x2f4e30/0x2f49c0), so naming leaf addresses is pure
        // whack-a-mole -- each run's named set simply never executes. The filter
        // that matters is not WHICH function, it is "$ra was already out of guest
        // range when this frame was entered", which is run-independent. Healthy
        // frames still stay silent, so the 48-line budget is unaffected.
        //
        // 2026-07-25n -- exclude entryRa == 0. The widened filter spent its entire
        // 48-line budget on identical boot-path frames
        // (callee=0x178068 entryPc=0x178068 entrySp=0x1f00000 entryRa=0x0), which are
        // deliverSifRpcReply re-entering the guest dispatcher with $ra never set --
        // a real defect, but a KNOWN one, now fixed in SIF.cpp. Zero is the
        // "never initialised" case, not the "was live and got clobbered" case we
        // are hunting, so it must not consume the budget.
        const bool isLeafProbe = true;
        const bool leafEntryDirty = isLeafProbe && entryRa != 0u &&
                                    (entryRa < 0x00100000u || entryRa >= 0x02000000u);
        if (leafEntryDirty)
        {
            static std::atomic<uint32_t> s_leafDumps{0};
            const uint32_t n = s_leafDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 48u)
            {
                const uint32_t tid = static_cast<uint32_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                std::ostringstream oss;
                oss << "[frametrace:LEAFENTRY] #" << std::dec << n
                    << " tid=0x" << std::hex << tid
                    << " callee=0x" << funcStart
                    << " entryPc=0x" << entryPc
                    << " entrySp=0x" << entrySp
                    << " entryRa=0x" << entryRa
                    << "\n";
                std::cerr << oss.str();
                {
                    static const char *const k[] = {"n", "callee", "entryPc", "entrySp", "entryRa"};
                    const uint64_t v[] = {n, funcStart, entryPc, entrySp, entryRa};
                    ps2x_probe_kv("LEAFENTRY", 5, k, v);
                }
            }
        }

        // 2026-07-25o -- VECCTOR probe. See the slot-table note above for why
        // 5.4.1 was re-framed from "bad ctor fnptr" to "the trip count reaches
        // the PC". Sample the three quantities that distinguish the two
        // remaining mechanisms, on BOTH sides of the body:
        //   $s5 (r21) = ctor fnptr, loaded ONCE at 0x171c50 and never reloaded
        //               in the loop -> if it drifts, the `jalr $s5` at 0x171d04
        //               is dispatching to whatever overwrote it.
        //   $s3 (r19) = trip count (0x30 at the 0x2b8bfc site) -> the value that
        //               matches the derail PC.
        //   [sp+0xa0] = the in-memory loop counter, in the frame-top scratch
        //               area the nested ctor could clobber.
        // Unconditional but tightly bounded; the site only runs 48 iterations.
        const bool isVecCtor = (funcStart == 0x00171C30u);
        uint32_t vecS5Before = 0u, vecS3Before = 0u, vecCntBefore = 0u;
        if (isVecCtor)
        {
            vecS5Before = GPR_U32(ctx, 21);
            vecS3Before = GPR_U32(ctx, 19);
            // Counter address is sp+0xa0 relative to the callee's OWN frame. At
            // true entry sp is still the caller's, so the frame is not yet
            // established; at the two in-loop resume slots sp is already the
            // callee's. Read it only where it is meaningful.
            vecCntBefore = (entryPc == 0x00171C30u) ? 0xFFFFFFFFu
                                                    : READ32(entrySp + 0xA0u);
        }

        // 2026-07-25s -- CTORTGT. Supersedes A1SITE, which is now ALSO a dead
        // thread. A1SITE sampled $a1 at true entry (before original(), so the
        // args were genuinely live) across 96 call sites and every single one
        // came back a valid code pointer -- 0x1a4500, 0x2fe7b0, 0x38a370, ...
        // Not one small value. So:
        //   * "one bad caller passes a count in $a1" -- DEAD.
        //   * "argument-register shift / second entry ABI" -- DEAD.
        //   * and it confirms the earlier VECCTOR "a1 always 0x2-0x1f" data was
        //     exit garbage; those small values were $t0-shaped COUNTS read
        //     after original() clobbered the caller-saved regs.
        //
        // No caller ever supplies 0x30. Therefore $s5 must be going bad AFTER
        // 0x171c50 latches it -- and the place that can happen without any
        // caller involvement is the resume slots. `jalr $s5` at 0x171d04 splits
        // the function; the recompiler re-enters at 0x171d00/0x171d0c with
        // whatever ctx holds. A1SITE could never see this: it gated on
        // entryPc == 0x171c30 and the resume slots have a different entryPc.
        //
        // Two design changes, both aimed at the same failure of the last run:
        //   1. Watch EVERY entry slot, not just true entry, and read the value
        //      the jalr will actually use -- $a1 at true entry (pre-0x171c50),
        //      $s5 at the resume slots (post-0x171c50).
        //   2. FILTER. A1SITE burned all 96 records on healthy calls in a single
        //      thread and was exhausted long before the derail. Log only when
        //      the target is NOT a plausible code pointer, so the budget is
        //      reserved for the anomaly. Silence here is itself a result: it
        //      means 0x30 is not reaching $s5 through this function at all.
        // 2026-07-25t -- RABAD. CTORTGT answered its question with silence and is
        // retired below; 0x171c30 is no longer the subject at all.
        //
        // What the CTORTGT run actually showed: the derail PC is NOT 0x30 any
        // more, it is 0x1 (301 hits) plus 0x11 (2). "PC = 0x30" was never a
        // stable fact about this bug -- the value varies run to run, so every
        // hypothesis that treated 0x30 as a specific meaningful constant (a
        // count, an element size) was chasing noise.
        //
        // The fault's own dump is the giveaway:
        //   [guest-branch:missing-target] target=0x1 pc=0x1 ra=0x1
        // ra == pc == 1. This is not a bad CALL target -- it is a `jr $ra` with
        // a corrupt $ra. The dispatcher is the victim, not the crime scene.
        //
        // The dispatch trace is a tight cycle
        //   0x32bca0 -> 0x191780 -> 0x32bbb0 -> 0x32bca0 -> 0x191898
        // = getter / strcpy / ctor / getter / strlen. All four were read from
        // the ELF and all four are innocent:
        //   * 0x32bbb0's two `jalr $t9` sites both emit
        //     SET_GPR_U32(ctx, 31, <return slot>) correctly.
        //   * vtable 0x4F3250[+8] == 0x32bca0, so the indirect target is right.
        //   * gp_field_get_z_396_0x32bca0 correctly does ctx->pc = $ra.
        // Nothing here CREATES ra=1. Some function is being ENTERED with $ra
        // already corrupt, and the damage only surfaces later when that
        // function reaches its own `jr $ra`.
        //
        // So watch the entry, not the exit, and watch it globally -- the
        // corrupting caller need not be one of the four above. Log any callee
        // entered with an $ra that cannot be a return address. Filtered, so the
        // budget survives to the event; `entryRa == 0` is excluded because the
        // dispatcher legitimately enters top-level slots that way (that thread
        // is already closed -- see the entryRa=0 note in PS2_PROJECT_STATE.md).
        {
            const uint32_t ra = entryRa;
            const bool sane = (ra == 0u) || (ra >= 0x00100000u && ra < 0x00600000u);
            if (!sane)
            {
                static std::atomic<uint32_t> s_raBadDumps{0};
                const uint32_t n = s_raBadDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 48u)
                {
                    const uint32_t tid = static_cast<uint32_t>(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                    std::ostringstream oss;
                    oss << "[frametrace:RABAD] #" << std::dec << n
                        << " tid=0x" << std::hex << tid
                        << " callee=0x" << funcStart
                        << " entryPc=0x" << entryPc
                        << " ra=0x" << ra
                        << " sp=0x" << entrySp
                        << " gp=0x" << GPR_U32(ctx, 28)
                        << " v0=0x" << GPR_U32(ctx, 2)
                        << " a0=0x" << GPR_U32(ctx, 4)
                        << " t9=0x" << GPR_U32(ctx, 25)
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        // 2026-07-25s -- CTORTGT. RETIRED, do not reinstate. It ran with the
        // probe demonstrably alive (VECCTOR emitted 73 records in the same run,
        // A1SITE emitted 0, confirming the new binary) and produced ZERO
        // records. Every jalr target flowing through 0x171c30 was a valid code
        // pointer. Combined with A1SITE's 96 clean caller records, 0x171c30 is
        // fully exonerated: it neither receives nor dispatches a bad pointer.
        // Kept live only because it is free when silent.
        if (isVecCtor)
        {
            const bool trueEntry = (entryPc == 0x00171C30u);
            // Pre-0x171c50 the fnptr still lives in $a1; after it, in $s5.
            const uint32_t target = trueEntry ? GPR_U32(ctx, 5) : vecS5Before;
            const bool codePtr = (target >= 0x00100000u && target < 0x00600000u);
            if (!codePtr)
            {
                static std::atomic<uint32_t> s_ctorTgtDumps{0};
                const uint32_t n = s_ctorTgtDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 64u)
                {
                    const uint32_t tid = static_cast<uint32_t>(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                    std::ostringstream oss;
                    oss << "[frametrace:CTORTGT] #" << std::dec << n
                        << " tid=0x" << std::hex << tid
                        << " entryPc=0x" << entryPc
                        << " slot=" << (trueEntry ? "ENTRY" : "RESUME")
                        << " caller=0x" << entryRa
                        << " sp=0x" << entrySp
                        << " target=0x" << target
                        << " a1=0x" << GPR_U32(ctx, 5)
                        << " s5=0x" << vecS5Before
                        << " s3=0x" << vecS3Before
                        << " cnt=0x" << vecCntBefore
                        << " ra=0x" << GPR_U32(ctx, 31)
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        // 2026-07-26 CRITSEC -- honour the guest's own interrupt mask.
        //
        // 0x17ed60 is DisableIntr (mfc0 $12 / cop0 0x39 / spin until EIE clear)
        // and 0x17edb0 is EnableIntr (cop0 0x38). SDBZ wraps its pool free-list
        // walk at 0x178428 in that pair, and the measured $ra corruption lands
        // strictly inside it: RASLOT saw the saved-$ra word at 0x1ffbeb0 flip
        // 0x1bb0b0 -> 0x1, and SLOTENTRY's before-samples bracket the flip
        // between entry to 0x17ed60 and entry to 0x17edb0. 0x17ed60 itself
        // contains no store instruction, so the writer is a concurrent one: the
        // IRQ worker re-entering the SIF dispatcher 0x178068 on its own context
        // (frametrace #840: entrySp=0x1f00000 = kAsyncCallbackFallbackSp,
        // exitRa=0x0 = Interrupt.cpp's SET_GPR_U32(&irqCtx, 31, 0)).
        //
        // The span is deliberately maximal -- raised before DisableIntr's body
        // (so its EIE spin loop is covered too) and lowered after EnableIntr's
        // body -- because a partial section is exactly the failure being fixed.
        // Guarded by funcStart, not entryPc: both addresses are single-slot
        // (addr == funcStart, see kSdbzFrameTraceSlots), so every wrapper
        // invocation is one whole guest call.
        //
        // These calls do NOT have to balance, and the 2026-07-27 run proved
        // they do not: DisableIntr returns the old EIE state and the guest
        // idiom `old = DisableIntr(); ...; if (old) EnableIntr();` skips the
        // enable on a nested section. enter/leave therefore set and clear a
        // single bit rather than pushing and popping a counter -- see the long
        // comment on tls_intr_disabled in Kernel/EeScheduler.cpp.
        const bool isIntrDisable = (funcStart == 0x0017ED60u);
        const bool isIntrEnable  = (funcStart == 0x0017EDB0u);
        if (isIntrDisable)
        {
            ps2x_guest_intr_disable_enter();
        }

        original(rdram, ctx, runtime);

        // 2026-08-29 -- [semwatch] follow-up #2. reqend (0x178560) never fires --
        // the SIF dispatcher (0x178068) never reaches it. Per the fresh decompile
        // of fn_178068_0x178068.cpp, dispatch routing is NOT literally "word[2]"
        // of the packet as earlier hypothesised -- it is the return value of
        // syscall_stub_z_24 (0x175090, already an entry-only wrapped slot in this
        // table), used as a signed index into one of two function-pointer tables
        // ([0x5616E4]/[0x5616E8] for negative, [0x5616EC]/[0x5616F0] for
        // non-negative), stride 12. A null table slot or an out-of-bound index
        // both fall straight to LABEL_15 (sync/ei, no handler call) without ever
        // invoking anything -- that is the exact shape that would leave 0x178560
        // unreached while 0x178068 itself still runs once. $v0 on return from
        // 0x175090 IS that index (decompile's local v12). Log it unconditionally,
        // bounded, to see the real value instead of inferring it.
        if (funcStart == 0x00175090u)
        {
            static std::atomic<uint32_t> s_dispIdxDumps{0};
            const uint32_t n = s_dispIdxDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                std::cerr << "[semwatch:dispidx] #" << std::dec << n
                    << " v0=0x" << std::hex << GPR_U32(ctx, 2)
                    << " v1=0x" << GPR_U32(ctx, 3)
                    << " entrySp=0x" << entrySp
                    << " negTbl=0x" << READ32(0x005616E4u)
                    << " negBound=0x" << READ32(0x005616E8u)
                    << " posTbl=0x" << READ32(0x005616ECu)
                    << " posBound=0x" << READ32(0x005616F0u)
                    << std::endl;
            }
        }

        // 2026-08-29 -- [semwatch] follow-up #4. dispidx STILL never fires even
        // though [semwatch:gatebyte] now shows the gate byte CAN be nonzero
        // (0x40 seen once). Re-read of the actual runner file
        // fn_178068_0x178068.cpp (not the IDA decompile) shows exitPc=0x1780c0
        // is NOT a return at all -- it is the copy-loop body address
        // (`lq $v0,0x0($a2)`); the only way to leave the function with pc
        // parked there is the cooperative-preemption check on the loop's
        // backward branch (`if (runtime->shouldPreemptGuestExecution())
        // return;`). So sub_178068 DOES take the true branch and DOES start
        // the packet copy -- it got yielded out mid-loop, confirmed by
        // matching gatebyte's entrySp=0x1ff3db0/gate=0x40 sample against the
        // IMBAL exitPc=0x1780c0 line at that same entrySp. My prior part-21
        // "early return, gate reads zero" conclusion was WRONG -- correcting
        // it here. Separately: per [[feedback_registerfunction_bypass]],
        // sub_178068 reaches syscall_stub_z_24 via
        // `runtime->lookupFunction(0x175090)` called DIRECTLY from inside its
        // own translated body (fn_178068_0x178068.cpp:277-288) -- a direct
        // C++ call, not a dispatch-loop call -- so it never passes through
        // whatever wraps funcStart==0x175090 at the top level. dispidx's
        // silence is an instrumentation gap, not proof the dispatch never
        // runs. Watch THIS wrapper's own funcStart==0x178068 invocation
        // instead: after original() returns, log where ctx->pc actually
        // landed (0x1780c0 again == preempted again; 0x1781b0/other == the
        // call actually finished) plus the real dispatch input at [sp+8]
        // (decompile's v12 / the asm's post-call $v1 reload at 0x1780e4).
        if (funcStart == 0x00178068u)
        {
            static std::atomic<uint32_t> s_dispExitDumps{0};
            const uint32_t n = s_dispExitDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                std::cerr << "[semwatch:dispexit] #" << std::dec << n
                    << " exitPc=0x" << std::hex << ctx->pc
                    << " exitSp=0x" << GPR_U32(ctx, 29)
                    << " v1@sp8=0x" << READ32(ADD32(GPR_U32(ctx, 29), 8))
                    << " negTbl=0x" << READ32(0x005616E4u)
                    << " negBound=0x" << READ32(0x005616E8u)
                    << " posTbl=0x" << READ32(0x005616ECu)
                    << " posBound=0x" << READ32(0x005616F0u)
                    << std::endl;
            }
        }

        if (isIntrEnable)
        {
            ps2x_guest_intr_disable_leave();

            // Health criteria for the next run, in priority order:
            //   depth    -- must read 0 here. The first cut used a nesting
            //               counter and this field sat at 2 forever, which is
            //               how the leak was caught; with the single-bit model
            //               a non-zero value means EnableIntr did not clear it.
            //   sections -- must CLIMB. Stuck at 1 is the leak signature: the
            //               bit never returned to the open state, so no later
            //               section was ever entered fresh.
            //   escapes  -- must stay 0. Non-zero means a section outran the
            //               scheduler's safety valve, the gate stopped
            //               protecting it, and the run proves nothing.
            //   stray    -- enables without a matching disable. Expected 0.
            //   redundant-- nested disables. Expected NON-zero and harmless;
            //               it is the idiom that broke the counter model.
            // Sample the first few sections as a liveness baseline, then only
            // when a counter that is supposed to stay still actually moves.
            static std::atomic<uint64_t> s_lastEscapes{0};
            static std::atomic<uint64_t> s_lastStray{0};
            const uint64_t escapes  = ps2x_guest_intr_disable_escapes();
            const uint64_t sections = ps2x_guest_intr_disable_sections();
            const uint64_t stray    = ps2x_guest_intr_disable_stray();
            const uint64_t prevEsc  = s_lastEscapes.exchange(escapes, std::memory_order_relaxed);
            const uint64_t prevStr  = s_lastStray.exchange(stray, std::memory_order_relaxed);
            if (sections <= 3u || escapes != prevEsc || stray != prevStr)
            {
                static const char *const k[] = {"sections", "escapes", "depth",
                                                "redundant", "stray"};
                const uint64_t v[] = {sections, escapes,
                                      ps2x_guest_intr_disable_depth(),
                                      ps2x_guest_intr_disable_redundant(), stray};
                ps2x_probe_kv("CRITSEC", 5, k, v);
            }
        }

        {
            const uint32_t raOut = GPR_U32(ctx, 31);
            const bool saneOut = (raOut == 0u) || (raOut >= 0x00100000u && raOut < 0x00600000u);
            if (!saneOut)
            {
                static std::atomic<uint32_t> s_raOutDumps{0};
                const uint32_t n = s_raOutDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 48u)
                {
                    const uint32_t tid = static_cast<uint32_t>(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                    std::ostringstream oss;
                    oss << "[frametrace:RAOUT] #" << std::dec << n
                        << " tid=0x" << std::hex << tid
                        << " callee=0x" << funcStart
                        << " entryPc=0x" << entryPc
                        << " entryRa=0x" << entryRa
                        << " raOut=0x" << raOut
                        << " exitPc=0x" << ctx->pc
                        << " entrySp=0x" << entrySp
                        << " exitSp=0x" << GPR_U32(ctx, 29)
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        // 2026-07-26 -- POOL. RAOUT named the corrupting body: 0x178be8 enters
        // with a sane entryRa (0x1bb0b0, from `jal 0x178be8` at 0x1bb0a8) and a
        // BALANCED stack (entrySp == exitSp == 0x1ffbec0), yet leaves with
        // $ra == pc == 1. So the damage is internal to 0x178be8, and the parked
        // 0xa0 stale-frame theory does not apply to this site.
        //
        // 0x178be8 saves $ra at newsp+176 == entrySp-0x10 == 0x1ffbeb0 -- the
        // slot an earlier session already had under watch. It then calls the
        // pool allocator 0x178428 with $a0 = 0x00563100 (lui 0x56 / addiu
        // 0x3100), takes the result into $s0, null-checks it ONLY
        // (`beq $s0,$zero,0x178db8`), and proceeds to store through it at
        // +20/+28/+32/+36/+40/+44/+48/+52. At 0x178cf0 that includes
        // `sw $v0,48($s0)` with $v0 == 1.
        //
        // $s0 == 0x1ffbe80 would put that store exactly on 0x1ffbeb0 and write
        // a literal 1 over the saved $ra -- reproducing the measurement with a
        // balanced sp. 0x178428 itself derives its return purely from guest
        // memory: base = [0x563104], count = [0x563108], entries 0x40 apart.
        // An uninitialised header therefore yields a non-null garbage pointer
        // that the null check waves through.
        //
        // MEASURED 2026-07-26 (POOL probe, now replaced): the allocator is
        // HEALTHY and the inference above was WRONG. The header at 0x563100 is
        // well-formed -- base = 0x20561900, count = 0x20 -- and 0x20561900 is
        // simply the uncached-mirror (EE 0x20000000 segment) alias of
        // 0x00561900, a legitimate pointer. 36 consecutive calls returned it
        // unchanged, consistent with matched alloc/free of a short-lived
        // object, and its stores land at 0x561914..0x561934 -- nowhere near
        // 0x1ffbeb0. 0x178428 is therefore EXONERATED as the $ra writer.
        //
        // The writer is some other store inside 0x178be8. That body is wrapped
        // as 14 slots, which turns "which instruction" into a bisect rather
        // than another guess: the wrapper runs after each slot's body, so
        // sampling the saved-$ra word at entrySp-0x10 on every slot shows
        // exactly which slot boundary it flips from 0x1bb0b0 to 0x1.
        //
        // 2026-07-26 RESOLVED -- the bisect above ran and did NOT find a slot
        // boundary, because there is no single instruction to find. RASLOT
        // caught the flip (saved=0x1 while entryRa=0x1bb0b0), and SLOTENTRY's
        // before-samples bracket it strictly between entry to 0x17ed60 and
        // entry to 0x17edb0 -- i.e. inside 0x178428's DisableIntr/EnableIntr
        // critical section, on a frame 0xE0 bytes ABOVE 0x17ed60's own sp.
        // 0x17ed60 has no store instruction at all, so the writer is the IRQ
        // worker re-entering 0x178068 concurrently while the runtime ignored
        // the guest's `di`. 0x178428 stays exonerated as the pool-header
        // corruptor -- it is the victim's caller, not the writer. See the
        // CRITSEC block near original() for the fix; this whole POOL/RASLOT
        // apparatus is kept only as the record of how it was narrowed.
        //
        // Log only the invocation that actually goes bad -- a full history is
        // useless here, and A1SITE already burned its budget that way. Records
        // are emitted once the watched word stops matching entryRa, plus a
        // short healthy baseline so a silent probe stays distinguishable from
        // a probe that never saw the event.
        if (funcStart == 0x00178BE8u)
        {
            // Only the entry slot sees the caller's sp; the resume slots run on
            // the already-shifted frame. Latch the address on entry and reuse
            // it, so every slot samples the SAME word.
            thread_local uint32_t t_raSlot = 0u;
            if (entryPc == 0x00178BE8u)
            {
                t_raSlot = (entrySp - 0x10u) & 0x1FFFFFFFu;
            }
            const uint32_t raSlot = t_raSlot;
            const uint32_t saved = raSlot ? *reinterpret_cast<const uint32_t *>(rdram + raSlot) : 0u;
            const bool bad = (saved != entryRa);
            static std::atomic<uint32_t> s_raSlotDumps{0};
            const uint32_t n = s_raSlotDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 6u || (bad && n <= 200u))
            {
                const uint32_t tid = static_cast<uint32_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                std::ostringstream oss;
                oss << "[frametrace:RASLOT] #" << std::dec << n
                    << (bad ? " BAD" : " ok")
                    << std::hex
                    << " tid=0x" << tid
                    << " slot=0x" << entryPc
                    << " at=0x" << raSlot
                    << " saved=0x" << saved
                    << " entryRa=0x" << entryRa
                    << " entrySp=0x" << entrySp
                    << " liveRa=0x" << GPR_U32(ctx, 31)
                    << "\n";
                std::cerr << oss.str();

                // Phase B: same record, same gating, into the structured sink.
                // Gating is deliberately IDENTICAL to the stderr line above so
                // the two streams can be compared 1:1 -- that equality is the
                // Phase B exit test. Once the sink is trusted the stderr line
                // goes away and the budget here can be widened independently.
                {
                    static const char *const k[] = {
                        "n", "bad", "slot", "at", "saved", "entryRa", "entrySp", "liveRa"};
                    const uint64_t v[] = {
                        n, bad ? 1u : 0u, entryPc, raSlot, saved, entryRa, entrySp,
                        GPR_U32(ctx, 31)};
                    ps2x_probe_kv("RASLOT", 8, k, v);
                }
            }
        }

        if (isVecCtor)
        {
            static std::atomic<uint32_t> s_vecDumps{0};
            const uint32_t n = s_vecDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 64u)
            {
                const uint32_t s5After = GPR_U32(ctx, 21);
                const uint32_t s3After = GPR_U32(ctx, 19);
                const uint32_t cntAfter = (entryPc == 0x00171C30u)
                                              ? 0xFFFFFFFFu
                                              : READ32(entrySp + 0xA0u);
                const uint32_t tid = static_cast<uint32_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                std::ostringstream oss;
                oss << "[frametrace:VECCTOR] #" << std::dec << n
                    << " tid=0x" << std::hex << tid
                    << " entryPc=0x" << entryPc
                    << " entrySp=0x" << entrySp
                    << " entryRa=0x" << entryRa
                    << " exitRa=0x" << GPR_U32(ctx, 31)
                    << " exitSp=0x" << GPR_U32(ctx, 29)
                    << " a1=0x" << GPR_U32(ctx, 5)
                    << " t0=0x" << GPR_U32(ctx, 8)
                    << " s5=0x" << vecS5Before << "->0x" << s5After
                    << " s3=0x" << vecS3Before << "->0x" << s3After
                    << " cnt=0x" << vecCntBefore << "->0x" << cntAfter
                    << "\n";
                std::cerr << oss.str();
            }
        }

        // 2026-07-25r -- the CTOREXIT (funcStart==0x2b9300) and S5ZERO
        // (entryPc==0x171d00/0x171d0c) probes that lived here are REMOVED.
        // Both were proven non-informative: CTOREXIT never fired because the
        // ctor is never dispatched, and every S5ZERO record turned out to be a
        // healthy epilogue restore of the caller's $s5. See the A1SITE comment
        // above for the surviving lead. Do not reinstate either probe.
        if (isLeafProbe && !leafEntryDirty)
        {
            const uint32_t exitRa = GPR_U32(ctx, 31);
            if (exitRa != entryRa)
            {
                static std::atomic<uint32_t> s_leafExitDumps{0};
                const uint32_t n = s_leafExitDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 48u)
                {
                    const uint32_t tid = static_cast<uint32_t>(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
                    std::ostringstream oss;
                    oss << "[frametrace:LEAFEXIT] #" << std::dec << n
                        << " tid=0x" << std::hex << tid
                        << " callee=0x" << funcStart
                        << " entrySp=0x" << entrySp
                        << " entryRa=0x" << entryRa
                        << " exitRa=0x" << exitRa
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        if (is178068)
        {
            // 2026-07-22b GATING FIX. The first probe spent its whole budget on
            // healthy/idle 178068 calls (entrySp~0x1f00000 idle thread, ~0x1ffbcXX
            // worker) that NEVER touch the watched slot 0x1ffbeb0 -- storeTarget
            // (entrySp-0x10) was 0x1effff0/0x1ffbcd0/... nowhere near it -- and it
            // exhausted long before the real derail (log line ~3201). So the frame-
            // overlap hypothesis is NEITHER confirmed nor refuted for the DERAIL call.
            // Gate strictly on the two derail signatures so the budget is spent on it:
            //   storeTarget == slotAddr  => 178068's own `sd $ra,0x90($sp)` (0x178074)
            //     lands ON the watched slot -> frame overlap CONFIRMED, 178068 is writer.
            //   slotAfter != slotBefore  => 178068's execution (own store OR its leaf
            //     callees 0x175090 / the 0x178180 jalr) mutated the slot -> stomp is
            //     INSIDE this frame regardless of address arithmetic.
            // Self-referential (before vs after) so it never depends on a stale
            // g_rpcSlotExpected. Healthy far-from-slot calls change nothing -> skipped.
            const uint32_t wa = g_rpcSlotAddr.load(std::memory_order_relaxed);
            const uint32_t storeTarget = entrySp - 0x10u;
            const uint32_t slotAfter = wa ? READ32(wa) : 0u;
            const bool interesting = (storeTarget == wa) ||
                                     (slotAfter != slot178068Before);
            if (interesting)
            {
                static std::atomic<uint32_t> s_slot2Dumps{0};
                const uint32_t n = s_slot2Dumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 24u)
                {
                    const uint32_t expected = g_rpcSlotExpected.load(std::memory_order_relaxed);
                    // Build the whole record then emit with ONE write, so the
                    // concurrent "guest PC 0x1" watchdog can't split it mid-line.
                    std::ostringstream oss;
                    oss << "[frametrace:SLOTWATCH2] #" << std::dec << n
                        << " 178068 entrySp=0x" << std::hex << entrySp
                        << " entryRa=0x" << entryRa
                        << " storeTarget=0x" << storeTarget
                        << " slotAddr=0x" << wa
                        << " expected=0x" << expected
                        << " slotBefore=0x" << slot178068Before
                        << " slotAfter=0x" << slotAfter
                        << " exitPc=0x" << ctx->pc
                        << " exitSp=0x" << GPR_U32(ctx, 29)
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        ps2FrameTraceRecord(kSdbzFrameTraceSlots[I].funcStart, entryPc, entrySp,
                            ctx->pc, GPR_U32(ctx, 29), GPR_U32(ctx, 31));

        // 2026-07-22 SLOTWATCH sample. While a watch is armed, any wrapped callee
        // (funcStart != rpc itself) that returns having changed the slot reports
        // it. The FIRST such line names the stomper -- or, if the stomper is an
        // unwrapped callee, the first wrapped callee AFTER it (bracket with the
        // known chain 178428->17ed60->17edb0->177fe8->177eb0->1781b0->175060->178068).
        {
            const uint32_t watchAddr = g_rpcSlotAddr.load(std::memory_order_relaxed);
            if (watchAddr != 0u && funcStart != 0x00178BE8u)
            {
                const uint32_t slotNow = READ32(watchAddr);
                const uint32_t expected = g_rpcSlotExpected.load(std::memory_order_relaxed);
                if (slotNow != expected)
                {
                    const uint32_t n = g_slotWatchDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                    if (n <= 24u)
                    {
                        std::ostringstream oss;
                        oss << "[frametrace:SLOTWATCH] #" << std::dec << n
                            << " callee=0x" << std::hex << funcStart
                            << " entryPc=0x" << entryPc
                            << " entrySp=0x" << entrySp
                            << " slot=0x" << watchAddr
                            << " expected=0x" << expected
                            << " before=0x" << slotBeforeCall
                            << " now=0x" << slotNow
                            << "\n";
                        std::cerr << oss.str();
                    }
                }
            }
        }

        // Fires only on a genuine top-of-function entry (addr == funcStart, not
        // an interior resume alias) that derails on the way out. exitRa/pc of
        // 0x1 (or the SIF packet-pool base) is the derail signature. Bounded so
        // it cannot flood even if the fault repeats.
        {
            const uint32_t exitRa = GPR_U32(ctx, 31);
            const bool trueEntry =
                kSdbzFrameTraceSlots[I].addr == kSdbzFrameTraceSlots[I].funcStart &&
                entryPc == kSdbzFrameTraceSlots[I].funcStart;
            const bool badExit =
                exitRa == 0x1u || ctx->pc == 0x1u ||
                exitRa == 0x20561900u || ctx->pc == 0x20561900u;
            if (trueEntry && badExit)
            {
                static std::atomic<uint32_t> s_raForkDumps{0};
                const uint32_t n = s_raForkDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 8u)
                {
                    const char *fork =
                        (entryRa == exitRa) ? "A(caller-passed-garbage)"
                                            : "B(mid-body-clobber)";
                    std::ostringstream oss;
                    oss << "[frametrace:RAFORK] #" << std::dec << n
                        << " func=0x" << std::hex << kSdbzFrameTraceSlots[I].funcStart
                        << " entryPc=0x" << entryPc
                        << " entryRa=0x" << entryRa
                        << " entrySp=0x" << entrySp
                        << " exitPc=0x" << ctx->pc
                        << " exitRa=0x" << exitRa
                        << " exitSp=0x" << GPR_U32(ctx, 29)
                        << " -> fork=" << fork
                        << "\n";
                    std::cerr << oss.str();
                }
            }
        }

        // 2026-07-20e -- the one unmeasured link.
        //
        // The derail is `jalr $a2` at guest 0x178180 (fn_178068_0x178068.cpp:519),
        // and $a2 is NOT corrupted en route -- it is LOADED from a table:
        //     0x178150  lw   $v1, 0x14($s1)     ; table base
        //     0x178154  mult $a1, $a1, $v0      ; index * stride
        //     0x178158  addu $v0, $a1, $v1
        //     0x17815c  lw   $a2, 0x0($v0)      ; -> function pointer
        //     0x178180  jalr $a2
        //
        // Everything about WHICH table and WHICH index has so far been inferred,
        // never measured -- and the last two inferences drawn that way (the bind
        // sid word-offset, the "empty" ARKD CALL) both proved wrong on contact
        // with the actual struct layout. So read the registers instead of
        // reasoning about them. They are still live in ctx at wrapper exit.
        if (ctx->pc == 0x20561900u || GPR_U32(ctx, 31) == 0x20561900u)
        {
            static std::atomic<uint32_t> s_tableDumps{0};
            const uint32_t n = s_tableDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 8u)
            {
                const uint32_t s1 = GPR_U32(ctx, 17);
                const uint32_t tableBase = READ32(ADD32(s1, 20)); // [$s1+0x14]
                std::ostringstream oss;
                oss << "[frametrace:TABLE] #" << std::dec << n
                    << " slot=0x" << std::hex << kSdbzFrameTraceSlots[I].addr
                    << " s1=0x" << s1
                    << " [s1+0x14]=0x" << tableBase
                    << " a1=0x" << GPR_U32(ctx, 5)
                    << " v0=0x" << GPR_U32(ctx, 2)
                    << " v1=0x" << GPR_U32(ctx, 3)
                    << " a2=0x" << GPR_U32(ctx, 6)
                    << "\n";
                std::cerr << oss.str();
            }
        }

        // 2026-07-20f -- the copy-loop overrun candidate, measured not assumed.
        //
        // Ring entry #133 is the first corruption: func=0x178068 exits at
        // 0x1780c0 (inside the lq/sq copy loop) with sp == entrySp - 0xa0, i.e.
        // it left mid-body without ever running its epilogue. Every healthy
        // entry for the same function exits at 0x177fc4 with sp == entrySp.
        //
        // The loop's trip count comes from memory, not from a constant:
        //     0x178080  lw   $a3, 0x16d8($v1)   ; $v1 = 0x560000 -> [0x5616d8]
        //               lbu  ...                ; byte at [$a3]
        //     0x1780b4  blez $a1, 0x1780dc
        //     0x1780c0  lq   $v0, 0($a2)        ; src
        //     0x1780cc  sq   $v0, 0($v1)        ; dst = sp, 16 bytes per iter
        // The frame is only 0xa0 bytes and holds $ra at 0x90($sp), so an
        // oversized count would walk the saved $ra. That is a CANDIDATE, not a
        // finding -- so dump the actual registers and the actual count source
        // on any frame imbalance and let the numbers decide.
        const uint32_t exitSp = GPR_U32(ctx, 29);
        ps2x_stack_check(ctx->pc, exitSp, 1u);
        if (exitSp != entrySp)
        {
            static std::atomic<uint32_t> s_imbalanceDumps{0};
            const uint32_t n = s_imbalanceDumps.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 8u)
            {
                const uint32_t countPtr = READ32(0x005616D8u);
                std::ostringstream oss;
                oss << "[frametrace:IMBAL] #" << std::dec << n
                    << " func=0x" << std::hex << kSdbzFrameTraceSlots[I].funcStart
                    << " entryPc=0x" << entryPc
                    << " entrySp=0x" << entrySp
                    << " exitPc=0x" << ctx->pc
                    << " exitSp=0x" << exitSp
                    << " delta=-0x" << (entrySp - exitSp)
                    << " [0x5616d8]=0x" << countPtr
                    << " countByte=0x"
                    << (countPtr < 0x02000000u ? (uint32_t)READ8(countPtr) : 0xFFFFFFFFu)
                    << " a0=0x" << GPR_U32(ctx, 4)
                    << " a1=0x" << GPR_U32(ctx, 5)
                    << " a2=0x" << GPR_U32(ctx, 6)
                    << " a3=0x" << GPR_U32(ctx, 7)
                    << " v1=0x" << GPR_U32(ctx, 3)
                    << " ra=0x" << GPR_U32(ctx, 31)
                    << "\n";
                std::cerr << oss.str();
            }
        }

        // 2026-07-22 SLOTWATCH disarm. rpc_call's body has fully run; clear the
        // watch so it re-arms cleanly on the next loop iteration. (Loop is
        // sequential, so nested rpc_call re-entry is not a concern here.)
        if (isRpcTrueEntry)
        {
            g_rpcSlotAddr.store(0u, std::memory_order_relaxed);
        }
    }

    template <size_t... I>
    void installSdbzFrameTraceWrappers(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        // Capture every original FIRST -- replaceFunction on an alias slot would
        // otherwise hand a later capture the wrapper instead of the real body.
        ((g_sdbzFrameTraceOriginals[I] = runtime.lookupFunction(kSdbzFrameTraceSlots[I].addr)), ...);
        ((g_sdbzFrameTraceOriginals[I] != nullptr
              ? (void)runtime.replaceFunction(kSdbzFrameTraceSlots[I].addr, &sdbzFrameTraceWrapper<I>)
              : (void)0),
         ...);
    }

    // ------------------------------------------------------------------
    // 2026-08-09 Stage 5.11 -- [pktord] display-list build-order probe
    //
    // Run 34 settled that the draw-order inversion is built on the EE side:
    // our runtime transports the display list faithfully, but the recompiled
    // guest code hands it over in a different order than PCSX2 does. By rect,
    // live emits [3,9,7,8,4,10,5,6,2,1] where the PCSX2 dump emits [1..10] --
    // the box body lands before the background instead of after it.
    //
    // Run 35 (hardware data breakpoint on the display-list slot 0x7d5fb0)
    // named the writer: CopyQW_0x1002b0, called from sub_00103F50. The IDA
    // decompile of 0x103f50 confirms what it is -- the per-sprite packet
    // allocator:
    //
    //     v5 = (*(_DWORD *)a1 & 0x7FFF) + 1;          // qword count
    //     result = heap_alloc_aligned(16 * v5, 16);
    //     *(_DWORD *)(a1 + 8)  = 0x13000000;
    //     *(_DWORD *)(a1 + 12) = (v5 - 1) | 0x51000000;
    //     *(_DWORD *)a1        = *(_DWORD *)a1 | 0x80000000;
    //     CopyQW(a1, result, v5);
    //     return result & 0xFFFFFFF | 0x40000000;      // a DMAtag link
    //
    // v5 == 7 gives 16*7 == 0x70 bytes, which is exactly the source-chain
    // stride measured in the PCSX2 EE ground truth (0x771470 clear ->
    // 0x7717d0 background -> 0x771a00 box body -> 0x771a70 borders...). So
    // every link in the dialog chain passes through this one function, once,
    // in build order. It is a funnel: wrapping it cannot miss a sprite, and
    // there is no address or shape to gate on and get wrong
    // ([[feedback_probe_gate_on_shape_not_address]]).
    //
    // What this probe records, per call, is therefore the ORDER itself plus
    // enough identity to say which sprite each call built:
    //   seq  -- global call index; the answer to "what order"
    //   ra   -- guest return address; NAMES the caller that ordered it
    //   tmpl -- the a0 template object the packet is built from
    //   dst  -- the returned DMAtag (link address spliced into the chain)
    //   n    -- qword count; 7 == the dialog stride
    //   hdr  -- the 0x13000000 / (n-1)|0x51000000 header words
    //   q1..q3 -- the first three data qwords, RAW.
    //
    // The qwords are logged raw rather than decoded on purpose. The exact
    // GIFtag/DMAtag field split at a1+0..+15 is not established from the
    // decompile alone, and a decoder built on a guessed layout would report
    // confident nonsense about the wrong field
    // ([[feedback_degenerate_result_convicts_the_probe]]). Decode offline
    // against dump.jsonl, where the true layout is already known.
    //
    // No shape gate, no address gate: every call is emitted until the cap.
    // [PKTSTAT] publishes calls/emitted/dropped so a capped run can never be
    // mistaken for a quiet one ([[feedback_capped_probes_false_negatives]]).
    //
    // Off unless PS2X_PKTORD is set to something other than 0.
    //
    // ---- RUN 36 RESULT: THE FUNNEL PREMISE ABOVE IS FALSE ----------------
    //
    // 3846 calls, and every identity field held exactly ONE distinct value:
    // ra=0x10dc64, tmpl=0x43c370, n=8, dst=0x0, hdr0=0xf0000007. One caller,
    // one static template, invariant payload -- and n=8 (0x80 bytes), not the
    // n=7/0x70 dialog stride the comment above predicted. The background, box,
    // border and glyph packets do not reach this wrapper at all.
    //
    // The cause is instrumentation, not the guest. replaceFunction only
    // intercepts calls routed through the dispatch loop; a recompiled call site
    // that calls fn_00103F50 directly in C++ bypasses the wrapper entirely
    // ([[feedback_registerfunction_bypass]]). 0x10dbe0 reaches the allocator
    // indirectly so we see it; the rest call direct so we never do. THE ABSENCE
    // OF OTHER CALLERS IN PKTORD OUTPUT IS THEREFORE NOT EVIDENCE. Neither is
    // dst=0x0 readable: the decompiled success path can never return 0
    // (| 0x40000000 is unconditional), so either allocation fails on this path
    // or v0 is not where the value lands after the reg_save_stub_i tail call --
    // and the coverage hole makes the question moot.
    //
    // hdr0/hdr1 are non-evidence for the same reason plus one of their own:
    // tmpl is a fixed static, so bit31 there could have been stamped by any
    // earlier invocation, including the direct-call ones this probe cannot see.
    //
    // Kept, disarmed, as the record of a falsified hypothesis. Any successor
    // built on replaceFunction inherits the identical blind spot -- which is
    // why run 37 moved to the hardware data breakpoint (HWWATCH above), whose
    // capture happens on the host memory write and cannot be bypassed by the
    // call mechanism.
    // ------------------------------------------------------------------
    constexpr uint32_t kPktOrdSlot = 0x00103F50u;
    constexpr uint64_t kPktOrdMaxEmit = 200000u;
    // Run 36 emitted 3846 records and never once printed PKTSTAT, because the
    // whole run was shorter than one heartbeat period. A cadence coarser than
    // the total call count is a heartbeat that cannot beat.
    constexpr uint64_t kPktOrdStatEvery = 256u;

    PS2Runtime::RecompiledFunction g_pktOrdOriginal = nullptr;
    std::atomic<uint64_t> g_pktOrdCalls{0u};
    std::atomic<uint64_t> g_pktOrdEmitted{0u};
    std::atomic<uint64_t> g_pktOrdDropped{0u};

    bool pktOrdEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_PKTORD");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
        }();
        return on;
    }

    uint64_t pktOrdRead64(const uint8_t *rdram, uint32_t guestAddr)
    {
        const uint64_t lo = fontGateRead32(rdram, guestAddr);
        const uint64_t hi = fontGateRead32(rdram, guestAddr + 4u);
        return lo | (hi << 32);
    }

    void pktOrdWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_pktOrdOriginal;
        if (original == nullptr)
        {
            return;
        }

        if (!pktOrdEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        // a0/$ra must be captured BEFORE the body runs -- the body is free to
        // clobber both, and $ra is the whole point of the record.
        const uint32_t tmpl = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);

        original(rdram, ctx, runtime);

        const uint32_t dst = GPR_U32(ctx, 2); // v0 == the returned DMAtag
        const uint64_t seq = g_pktOrdCalls.fetch_add(1u, std::memory_order_relaxed);

        if (seq >= kPktOrdMaxEmit)
        {
            g_pktOrdDropped.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            // The header word is read back AFTER the body, so bit 31 tells us
            // the allocation actually succeeded and the packet was stamped.
            const uint64_t hdr0 = pktOrdRead64(rdram, tmpl + 0x00u);
            const uint64_t hdr1 = pktOrdRead64(rdram, tmpl + 0x08u);
            const uint32_t n = (static_cast<uint32_t>(hdr0) & 0x7FFFu) + 1u;

            const char *k[] = {"seq", "ra", "tmpl", "dst", "n",
                               "hdr0", "hdr1",
                               "q1lo", "q1hi", "q2lo", "q2hi", "q3lo", "q3hi"};
            const uint64_t v[] = {
                seq, ra, tmpl, dst, n,
                hdr0, hdr1,
                pktOrdRead64(rdram, tmpl + 0x10u), pktOrdRead64(rdram, tmpl + 0x18u),
                pktOrdRead64(rdram, tmpl + 0x20u), pktOrdRead64(rdram, tmpl + 0x28u),
                pktOrdRead64(rdram, tmpl + 0x30u), pktOrdRead64(rdram, tmpl + 0x38u)};
            ps2x_probe_kv("PKTORD", 13, k, v);
            g_pktOrdEmitted.fetch_add(1u, std::memory_order_relaxed);
        }

        // Heartbeat. Runs on the same cadence whether or not anything was
        // emitted, so "no PKTORD records" and "PKTORD never ran" stay
        // distinguishable, and a saturated run says so out loud.
        if (((seq + 1u) % kPktOrdStatEvery) == 0u)
        {
            const char *sk[] = {"calls", "emitted", "dropped", "cap"};
            const uint64_t sv[] = {
                g_pktOrdCalls.load(std::memory_order_relaxed),
                g_pktOrdEmitted.load(std::memory_order_relaxed),
                g_pktOrdDropped.load(std::memory_order_relaxed),
                kPktOrdMaxEmit};
            ps2x_probe_kv("PKTSTAT", 4, sk, sv);
        }
    }

    void applySdbzPacketOrder(PS2Runtime &runtime)
    {
        g_pktOrdOriginal = runtime.lookupFunction(kPktOrdSlot);
        if (g_pktOrdOriginal != nullptr)
        {
            runtime.replaceFunction(kPktOrdSlot, &pktOrdWrapper);
        }

        // Always report. "installed 0" would mean 0x103f50 is not in the dense
        // table at all, which invalidates the measurement before the run --
        // exactly the failure the frametrace banner below was added to catch.
        std::cerr << "[pktord] wrapper on 0x" << std::hex << kPktOrdSlot << std::dec
                  << " installed " << (g_pktOrdOriginal != nullptr ? 1 : 0) << " of 1"
                  << " (set PS2X_PKTORD=1 to arm recording)" << std::endl;
    }

    void applySdbzFrameTrace(PS2Runtime &runtime)
    {
        installSdbzFrameTraceWrappers(runtime, std::make_index_sequence<kSdbzFrameTraceSlotCount>{});

        size_t installed = 0;
        for (size_t i = 0; i < kSdbzFrameTraceSlotCount; ++i)
        {
            if (g_sdbzFrameTraceOriginals[i] != nullptr)
            {
                ++installed;
            }
        }

        // Always report: "0 of 18" would mean these addresses are not in the
        // dense table at all, which invalidates the measurement before the run.
        std::cerr << "[frametrace] wrappers installed " << std::dec << installed
                  << " of " << kSdbzFrameTraceSlotCount
                  << " (set PS2X_FRAMETRACE=1 to arm recording)" << std::endl;
    }

    // ------------------------------------------------------------------
    // 2026-08-17 Stage 5.15 -- [moviegate] movie-open decision probe
    //
    // WHAT THIS ANSWERS. Across 21 archived runs the \MOVIE\OKR.SFD;1 open
    // fired 3 times. Those 3 are also the only runs carrying a DVCI record.
    // ATARI is 0 in ALL 21 -- including the 3 golden ones -- so our runtime
    // never opens the Atari logo movie at all, while hardware always opens
    // it first. The divergence therefore starts strictly UPSTREAM of the
    // DVCI layer, and probing 0x130ef0 alone cannot see it.
    //
    // HARDWARE GROUND TRUTH (PCSX2 DebugServer, 2026-08-17). The chain is:
    //
    //     sub_125898  (movie state machine, $s0 = $a0 = state object)
    //        -- jal 0x12cc20  @ 0x125958  (DirectCall, dispatch-routed)
    //     sub_12CC20  (CRI open wrapper)
    //        -- jalr $v0      @ 0x12ccd8  (IndirectCall, dispatch-routed)
    //     sub_130EF0  (DVCI file open)
    //
    // Hardware calls 0x130ef0 exactly TWICE -- "movie/atari.sfd" then
    // "movie/okr.sfd", ~7.6 s apart -- and BOTH take the cache-MISS path
    // ($a1 == *(h+0x24) == 0 at 0x130f78). So the "DVCI: File cache was not
    // hit" print is the NORMAL path, not an anomaly, and the hypothesis that
    // our silent runs take a quiet cache-HIT branch is dead. 0x130ef0 is
    // simply not being entered.
    //
    // THE GATE. sub_125898's guard chain, disassembled live:
    //
    //     0x125924  bne   $a0, 1     -> 0x125ac8      (state must be 1)
    //     0x125930  bnez  0x49($s0)  -> 0x125994      ONE-SHOT LATCH
    //     0x125938  sb    $a0, 0x49($s0)              latch set to 1
    //     0x125948  bnezl 0x08($s0)  -> 0x125994      HANDLE ALREADY SET
    //     0x125950  lw    $a0, 0x50($s0)              filename pointer
    //     0x125958  jal   0x12cc20                    THE OPEN
    //     0x125964  sw    $v0, 0x08($s0)              store handle
    //
    // plus three earlier byte gates at the prologue: 0x02($s0) must be 0,
    // 0x48($s0) must be 1, and 0x47($s0) is read at 0x1258cc.
    //
    // +0x49 is a one-shot: once nonzero the open never runs again for the
    // rest of the run. +0x08 is a second permanent skip. Either one left
    // dirty produces exactly the observed 3-in-21 behaviour. That is a
    // HYPOTHESIS -- this probe exists to measure it, not to confirm it.
    //
    // WHY WRAPPERS CAN FIRE HERE. replaceFunction only intercepts calls that
    // go through the dispatch loop; a recompiled call site that calls fn_ in
    // C++ directly bypasses it entirely, which is what silently hollowed out
    // the [pktord] measurement in run 36 ([[feedback_registerfunction_bypass]]).
    // All three edges were checked in the generated output before writing
    // this: 0x126410 -> 0x125898, 0x125958 -> 0x12cc20 and the 0x12ccd8 JALR
    // all emit dispatchGuestBranch(). None is a direct C++ call.
    //
    // THE DECISION TABLE this produces:
    //   125898 fires, 12CC20 does not -> a +0x49 / +0x08 / prologue gate is
    //                                    the blocker, and the record names
    //                                    which byte was wrong.
    //   neither fires                 -> the state machine never runs; the
    //                                    fault is further back still.
    //   12CC20 fires, 130EF0 does not -> the fault is inside 0x12cc20's own
    //                                    argument checks.
    //
    // VOLUME. 0x125898 is a per-frame state machine (~5k calls in a 90 s
    // run), so its record is CHANGE-GATED: emitted on the first few calls
    // and thereafter only when the watched fields actually move. That keeps
    // every state TRANSITION -- which is the whole question -- without
    // flooding. Snapshots are taken both before and after the body runs, so
    // a latch that flips inside a single call is still visible.
    // 0x12cc20 / 0x130ef0 fire twice per run on hardware, so they are
    // emitted unconditionally up to a generous cap.
    //
    // Every counter is reported by [moviegate:stat], and saturation prints
    // an explicit [cap] line, so a quiet run and a capped run can never be
    // confused ([[feedback_capped_probes_false_negatives]]).
    //
    // On by default. Set PS2X_MOVIEGATE=0 to disable. Default-on because the
    // event is intermittent: a run lost to a forgotten env var costs a whole
    // cycle, and the change-gate makes the idle cost ~nil.
    // ------------------------------------------------------------------

    constexpr uint32_t kMovieGateStateFn = 0x00125898u; // movie state machine
    constexpr uint32_t kMovieGateOpenFn = 0x0012CC20u;  // CRI open wrapper
    constexpr uint32_t kMovieGateDvciFn = 0x00130EF0u;  // DVCI file open

    // The two functions that ARM the object, plus their public thunks.
    //
    // 0x124e38 writes the request into the object and arms it:
    //     [+0x0c]=a4  [+0x14]=a5  [+0x50]=a2  [+0x54]=a3
    //     [+0x45]=1   [+0x10]=a5<<11
    // 0x1263b8 is the synchronous pump, and it is gated:
    //     if ([+0x50] != 0 && [+0x08] == 0) { [+0x45]=1; spin { 0x125898 } }
    //
    // The 08-17 run measured fptr50=0 on all ~2560 calls to 0x125898, which
    // means [+0x50] was never written -- i.e. 0x124e38 never ran. So the
    // question is no longer "what blocks the open", it is "who was supposed
    // to request one". Each of these has exactly one direct caller (a thunk
    // that brackets it with the interrupt-disable pair), so BOTH levels are
    // wrapped: the inner one proves the write happened, the OUTER one's $ra
    // names the real originator -- 0x1274e8, 0x134f78 or 0x1559b0 for the
    // request, 0x11cc28 for the pump. Wrapping only the inner address would
    // report a $ra inside its own thunk and answer nothing.
    constexpr uint32_t kMovieGateReqOuterFn = 0x00124DC8u;  // thunk -> 0x124e38
    constexpr uint32_t kMovieGateReqFn = 0x00124E38u;       // arms [+0x45]/[+0x50]
    constexpr uint32_t kMovieGatePumpOuterFn = 0x00126388u; // thunk -> 0x1263b8
    constexpr uint32_t kMovieGatePumpFn = 0x001263B8u;      // gated spin pump

    // The pump's spin waits on this global; 0x125898 runs only when it reads
    // back 1. It sits 0xc below the ADX stream table, so a wild write to the
    // table's tail would land on it.
    constexpr uint32_t kMovieGatePumpFlag = 0x0044BEACu;

    constexpr uint64_t kMovieGateMaxOpen = 256u;  // per-wrapper emit cap
    constexpr uint64_t kMovieGateMinState = 4u;   // always emit first N states
    constexpr uint64_t kMovieGateMaxState = 4096u;
    constexpr uint64_t kMovieGateStatEvery = 512u;

    // The request is the smoking gun and should be rare, so it is emitted
    // unconditionally. The pump may be per-frame, so it is emitted for the
    // first few calls and thereafter only when its gate inputs move.
    constexpr uint64_t kMovieGateMaxReq = 64u;
    constexpr uint64_t kMovieGateMinPump = 8u;
    constexpr uint64_t kMovieGateMaxPump = 256u;

    // 2026-08-17 -- [cdsearch] the disc lookup underneath the failing open.
    //
    // WHAT THE 08-17 RUN ESTABLISHED. The movie request is fine. 0x124e38
    // armed the object correctly (arm45=1, fptr50=0x1b173a8), 0x125898 saw
    // it, 0x12cc20 ran, 0x130ef0 ran -- and 0x130ef0 returned 0. The guest
    // printed its own "DVCI: File cache was not hit." on the way, which is
    // past 0x130ef0's null-fname, rw and allocation checks, so exactly one
    // of its four early returns is left:
    //
    //     if ( !sub_130AD8(h + 32, "\MOVIE\ATARI.SFD;1") ) {
    //         CRI_error("E0092911 sceCdSearchFile ...");
    //         free(h); return 0;                       <-- the observed ret
    //     }
    //
    // WHAT 0x1866A0 ACTUALLY IS. Decompiling it settles a question this
    // project has re-opened three times: it is the EE-side sceCdSearchFile.
    // It copies the filename into a 256-byte buffer at 0x568724, prints
    // "ee call cmd search %s", writes the caller's sceCdlFILE pointer to
    // 0x568828, cache-flushes 300 bytes at *0x568700, and issues
    //
    //     sceSifCallRpc(client=*0x568880, func=0, send=*0x568700/300,
    //                   recv=*0x568840/4)
    //
    // then RETURNS *0x568840 -- the four-byte RPC recv word, verbatim.
    // The client it binds is sid 0x80000597 (the literal -2147482217 in the
    // bind call), which is the sid this project retired as "a symptom" and
    // which handleRPC still has no case for.
    //
    // So an unanswered call leaves recv untouched and the return is
    // whatever was already there -- 0. Nothing prints, because every printf
    // in the function is gated on the libcdvd verbosity word at 0x463250.
    // This is the identical shape to the Stage 5.12 memory-card finding:
    // SILENCE MUST BE WRITTEN, NOT OMITTED ([[project_stage512_memory_card]]).
    //
    // WHY BOTH LEVELS ARE WRAPPED. 0x130AD8 is another interrupt-disable
    // thunk -- wrap_irq_handler_push(0x8000) / worker / (0x8001) -- so a
    // wrapper on 0x1866A0 alone reports a $ra inside that thunk and names
    // nobody ([[project_stage58_vu1_microcode_missing]] thunk doctrine).
    // The generated body was checked before writing this: the jal at
    // 0x130B04 emits dispatchGuestBranch(..., DirectCall), so replaceFunction
    // really does intercept it ([[feedback_registerfunction_bypass]]).
    //
    // THE DECISION TABLE. Read cdsearch1866a0.calls first, then nameBuf:
    //
    //   calls=0                  -> 0x130ef0 never reached the lookup; the
    //                               blocker is above it after all, and the
    //                               elimination argument above is wrong.
    //   calls>0, nameBuf stale   -> bailed at the 0x463268 guard or at
    //                               0x186bb0 before the filename copy; the
    //                               RPC was never even attempted.
    //   calls>0, nameBuf=ATARI,
    //           ret=0, recv=0    -> reached the RPC, nobody answered.
    //                               CONFIRMS unserved sid 0x80000597.
    //   calls>0, ret!=0          -> the lookup SUCCEEDS and 0x130ef0 fails
    //                               for a different reason entirely.
    //
    // nameBuf is the load-bearing field: it is the only one that separates
    // "the RPC went unanswered" from "we never got as far as asking", and
    // those two have opposite fixes.
    constexpr uint32_t kCdSearchThunkFn = 0x00130AD8u; // irq-disable thunk
    constexpr uint32_t kCdSearchFn = 0x001866A0u;      // EE sceCdSearchFile

    // The 300-byte send buffer, decoded. 36 + 256 + 4 + 4 == 300 == 0x12c,
    // which is exactly the ssz the 08-17 log reports, so this layout is
    // measured rather than assumed:
    //     +0x000  sceCdlFILE result  (lsn, size, name[16], date[8]) - 36B
    //     +0x024  char name[256]     the path the guest is searching for
    //     +0x124  u32  pResult       = 0x568700, where the IOP writes back
    //     +0x128  u32  mode          = *0x44E70C
    constexpr uint32_t kCdSearchNameBuf = 0x00568724u; // 256B filename copy
    constexpr uint32_t kCdSearchRecvWord = 0x00568840u; // recv/4 == the return
    constexpr uint32_t kCdSearchClientPtr = 0x00568880u; // RPC client (0x...597)
    constexpr uint32_t kCdSearchSendPtr = 0x00568700u;  // send/300
    constexpr uint32_t kCdSearchModeWord = 0x00568828u; // send +0x128
    constexpr uint32_t kCdSearchGuard = 0x00463268u;    // compared at entry
    constexpr uint32_t kCdSearchBindLatch = 0x0046328Cu; // <0 => bind not done
    constexpr uint32_t kCdSearchBindDone = 0x005688A4u;  // async bind-complete
    constexpr uint32_t kCdSearchVerbose = 0x00463250u;   // >0 => guest printfs

    constexpr uint64_t kCdSearchMax = 64u;

    PS2Runtime::RecompiledFunction g_movieGateStateOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGateOpenOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGateDvciOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGateReqOuterOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGateReqOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGatePumpOuterOrig = nullptr;
    PS2Runtime::RecompiledFunction g_movieGatePumpOrig = nullptr;
    PS2Runtime::RecompiledFunction g_cdSearchThunkOrig = nullptr;
    PS2Runtime::RecompiledFunction g_cdSearchOrig = nullptr;

    std::atomic<uint64_t> g_movieGateStateCalls{0u};
    std::atomic<uint64_t> g_movieGateStateEmit{0u};
    std::atomic<uint64_t> g_movieGateOpenCalls{0u};
    std::atomic<uint64_t> g_movieGateDvciCalls{0u};
    std::atomic<uint64_t> g_movieGateReqOuterCalls{0u};
    std::atomic<uint64_t> g_movieGateReqCalls{0u};
    std::atomic<uint64_t> g_movieGatePumpOuterCalls{0u};
    std::atomic<uint64_t> g_movieGatePumpCalls{0u};
    std::atomic<uint64_t> g_movieGatePumpEmit{0u};
    std::atomic<uint64_t> g_cdSearchThunkCalls{0u};
    std::atomic<uint64_t> g_cdSearchCalls{0u};
    std::atomic<uint64_t> g_cdSearchNonZeroRet{0u};

    bool movieGateEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_MOVIEGATE");
            return e == nullptr || (e[0] != '\0' && e[0] != '0');
        }();
        return on;
    }

    // Rides the PS2X_MOVIEGATE master switch so one variable still turns the
    // whole Stage 5.15 instrumentation off for a clean perf baseline, but has
    // its own opt-out for isolating this probe's cost on its own.
    bool cdSearchEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_CDSEARCH");
            return e == nullptr || (e[0] != '\0' && e[0] != '0');
        }();
        return on && movieGateEnabled();
    }

    uint32_t movieGateRead8(const uint8_t *rdram, uint32_t guestAddr)
    {
        return rdram[guestAddr & 0x01FFFFFFu];
    }

    // Guest C string -> a bounded, printable std::string. Non-printables are
    // rendered as '.' rather than dropped: a filename that is present but
    // corrupt must not read as a filename that is absent.
    std::string movieGateStr(const uint8_t *rdram, uint32_t guestAddr, uint32_t maxLen = 64u)
    {
        if (rdram == nullptr || guestAddr == 0u || guestAddr >= 0x02000000u)
        {
            return std::string("<null>");
        }
        std::string s;
        s.reserve(maxLen);
        for (uint32_t i = 0; i < maxLen; ++i)
        {
            const uint8_t c = rdram[(guestAddr + i) & 0x01FFFFFFu];
            if (c == 0u)
                break;
            s.push_back((c >= 0x20u && c < 0x7Fu) ? static_cast<char>(c) : '.');
        }
        return s.empty() ? std::string("<empty>") : s;
    }

    // The watched fields of the movie state object, in one comparable blob.
    struct MovieGateState
    {
        uint32_t base = 0u;
        uint32_t handle = 0u; // +0x08 -- nonzero permanently skips the open
        uint32_t fname = 0u;  // +0x50 -- filename pointer
        uint32_t arg1 = 0u;   // +0x54 -- second arg to 0x12cc20
        uint8_t b02 = 0u;     // +0x02 -- prologue gate, must be 0
        uint8_t b46 = 0u;     // +0x46
        uint8_t b47 = 0u;     // +0x47
        uint8_t b45 = 0u;     // +0x45 -- THE ARM BYTE, set by 0x124e38 and by
                              //          0x1263b8, cleared by the state machine
        uint8_t b48 = 0u;     // +0x48 -- prologue gate, must be 1
        uint8_t b49 = 0u;     // +0x49 -- suspected one-shot latch (measured 0)

        bool sameAs(const MovieGateState &o) const
        {
            return base == o.base && handle == o.handle && fname == o.fname &&
                   arg1 == o.arg1 && b02 == o.b02 && b45 == o.b45 &&
                   b46 == o.b46 && b47 == o.b47 && b48 == o.b48 &&
                   b49 == o.b49;
        }
    };

    MovieGateState movieGateSnap(const uint8_t *rdram, uint32_t base)
    {
        MovieGateState s;
        s.base = base;
        if (rdram == nullptr || base == 0u || base >= 0x02000000u)
        {
            return s;
        }
        s.handle = fontGateRead32(rdram, base + 0x08u);
        s.fname = fontGateRead32(rdram, base + 0x50u);
        s.arg1 = fontGateRead32(rdram, base + 0x54u);
        s.b02 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x02u));
        s.b45 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x45u));
        s.b46 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x46u));
        s.b47 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x47u));
        s.b48 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x48u));
        s.b49 = static_cast<uint8_t>(movieGateRead8(rdram, base + 0x49u));
        return s;
    }

    // Last emitted state, per distinct object base. Small and linear on
    // purpose -- hardware only ever showed one live movie object, but keying
    // on base means a second one cannot masquerade as a transition of the
    // first ([[feedback_degenerate_result_convicts_the_probe]]).
    constexpr size_t kMovieGateSlots = 8;
    std::mutex g_movieGateMutex;
    MovieGateState g_movieGateLast[kMovieGateSlots];
    bool g_movieGateLastValid[kMovieGateSlots] = {};
    size_t g_movieGateNextSlot = 0;

    bool movieGateChanged(const MovieGateState &s)
    {
        std::lock_guard<std::mutex> lock(g_movieGateMutex);
        for (size_t i = 0; i < kMovieGateSlots; ++i)
        {
            if (g_movieGateLastValid[i] && g_movieGateLast[i].base == s.base)
            {
                if (g_movieGateLast[i].sameAs(s))
                    return false;
                g_movieGateLast[i] = s;
                return true;
            }
        }
        const size_t slot = g_movieGateNextSlot % kMovieGateSlots;
        g_movieGateNextSlot = slot + 1;
        g_movieGateLast[slot] = s;
        g_movieGateLastValid[slot] = true;
        return true;
    }

    void movieGateEmitState(const char *when, uint64_t seq, const MovieGateState &s,
                            uint32_t a0, uint32_t ra)
    {
        std::ostringstream oss;
        oss << "[moviegate] fn=125898 when=" << when
            << " seq=" << std::dec << seq
            << std::hex
            << " base=0x" << s.base
            << " a0=0x" << a0
            << " ra=0x" << ra
            << " b02=0x" << (uint32_t)s.b02
            << " arm45=0x" << (uint32_t)s.b45
            << " b46=0x" << (uint32_t)s.b46
            << " b47=0x" << (uint32_t)s.b47
            << " b48=0x" << (uint32_t)s.b48
            << " latch49=0x" << (uint32_t)s.b49
            << " hnd08=0x" << s.handle
            << " fptr50=0x" << s.fname
            << " arg54=0x" << s.arg1
            << std::dec << "\n";
        std::cerr << oss.str();
        g_movieGateStateEmit.fetch_add(1u, std::memory_order_relaxed);
    }

    void movieGateStat(const char *why)
    {
        std::ostringstream oss;
        oss << "[moviegate:stat] why=" << why << std::dec
            << " state.calls=" << g_movieGateStateCalls.load(std::memory_order_relaxed)
            << " state.emit=" << g_movieGateStateEmit.load(std::memory_order_relaxed)
            << " open12cc20.calls=" << g_movieGateOpenCalls.load(std::memory_order_relaxed)
            << " dvci130ef0.calls=" << g_movieGateDvciCalls.load(std::memory_order_relaxed)
            // The request/pump counters answer the standing question even when
            // every one of them is zero -- "nobody ever asked for a movie" is
            // a result, not a missing measurement.
            << " req124dc8.calls=" << g_movieGateReqOuterCalls.load(std::memory_order_relaxed)
            << " req124e38.calls=" << g_movieGateReqCalls.load(std::memory_order_relaxed)
            << " pump126388.calls=" << g_movieGatePumpOuterCalls.load(std::memory_order_relaxed)
            << " pump1263b8.calls=" << g_movieGatePumpCalls.load(std::memory_order_relaxed)
            << " pump.emit=" << g_movieGatePumpEmit.load(std::memory_order_relaxed)
            // A zero here is the single most informative number in the line:
            // it says 0x130ef0 never reached the disc lookup at all, which
            // contradicts the elimination argument that predicted it would.
            << " cdsearch130ad8.calls=" << g_cdSearchThunkCalls.load(std::memory_order_relaxed)
            << " cdsearch1866a0.calls=" << g_cdSearchCalls.load(std::memory_order_relaxed)
            << " cdsearch.okret=" << g_cdSearchNonZeroRet.load(std::memory_order_relaxed)
            << " capState=" << kMovieGateMaxState
            << " capOpen=" << kMovieGateMaxOpen
            << " capReq=" << kMovieGateMaxReq
            << " capPump=" << kMovieGateMaxPump
            << "\n";
        std::cerr << oss.str();
    }

    // --- wrapper 1: sub_125898, the movie state machine -----------------
    void movieGateStateWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGateStateOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        // $s0 = $a0 at 0x1258a0, so the state object is $a0 on entry. Capture
        // before the body runs; the body owns both registers afterwards.
        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGateStateCalls.fetch_add(1u, std::memory_order_relaxed);

        const bool emitting = seq < kMovieGateMaxState;
        MovieGateState pre;
        if (emitting)
        {
            pre = movieGateSnap(rdram, base);
            if (seq < kMovieGateMinState || movieGateChanged(pre))
            {
                movieGateEmitState("pre", seq, pre, base, ra);
            }
        }

        original(rdram, ctx, runtime);

        if (emitting)
        {
            // The interesting transition -- +0x49 latching, +0x08 gaining a
            // handle -- happens INSIDE the body, so a pre-only snapshot would
            // attribute it to the next call, or miss it entirely on the last.
            const MovieGateState post = movieGateSnap(rdram, base);
            if (!post.sameAs(pre) && movieGateChanged(post))
            {
                movieGateEmitState("post", seq, post, base, ra);
            }
        }

        if (seq == kMovieGateMaxState)
        {
            std::cerr << "[cap] tag=moviegate limit=" << std::dec << kMovieGateMaxState
                      << " -- LATER 125898 STATES ARE INVISIBLE\n";
        }
        if (((seq + 1u) % kMovieGateStatEvery) == 0u)
        {
            movieGateStat("heartbeat");
        }
    }

    // --- wrapper 2: sub_12CC20, the CRI open wrapper --------------------
    void movieGateOpenWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGateOpenOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGateOpenCalls.fetch_add(1u, std::memory_order_relaxed);
        const std::string name = movieGateStr(rdram, a0);

        original(rdram, ctx, runtime);

        if (seq < kMovieGateMaxOpen)
        {
            std::ostringstream oss;
            oss << "[moviegate] fn=12cc20 seq=" << std::dec << seq
                << std::hex
                << " a0=0x" << a0
                << " a1=0x" << a1
                << " a2=0x" << a2
                << " ra=0x" << ra
                << " ret=0x" << GPR_U32(ctx, 2)
                << std::dec << " name=\"" << name << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kMovieGateMaxOpen)
        {
            std::cerr << "[cap] tag=moviegate.12cc20 limit=" << std::dec
                      << kMovieGateMaxOpen << "\n";
        }
    }

    // --- wrapper 3: sub_130EF0, the DVCI file open ----------------------
    void movieGateDvciWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGateDvciOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint32_t s1in = GPR_U32(ctx, 17); // hardware showed 0x01b173a8 here,
                                                // matching [adx:stream] f80
        const uint64_t seq = g_movieGateDvciCalls.fetch_add(1u, std::memory_order_relaxed);
        const std::string name = movieGateStr(rdram, a0);

        original(rdram, ctx, runtime);

        if (seq < kMovieGateMaxOpen)
        {
            // v0 is the returned CRI handle h, or 0 on failure. h+0x24 is the
            // field the cache-hit branch at 0x130f78 tests, and h+0x04 is the
            // size it stores; hardware showed +0x24 == 0 (cache MISS) on both
            // of its two opens, so a nonzero here is the NEW case, not the
            // expected one.
            const uint32_t h = GPR_U32(ctx, 2);
            const bool hOk = (h != 0u && h < 0x02000000u);
            std::ostringstream oss;
            oss << "[moviegate] fn=130ef0 seq=" << std::dec << seq
                << std::hex
                << " a0=0x" << a0
                << " a1=0x" << a1
                << " a2=0x" << a2
                << " ra=0x" << ra
                << " s1=0x" << s1in
                << " ret=0x" << h
                << " h24=0x" << (hOk ? fontGateRead32(rdram, h + 0x24u) : 0u)
                << " h04=0x" << (hOk ? fontGateRead32(rdram, h + 0x04u) : 0u)
                << " cache=" << (hOk && fontGateRead32(rdram, h + 0x24u) != 0u ? "HIT" : "miss")
                << std::dec << " name=\"" << name << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kMovieGateMaxOpen)
        {
            std::cerr << "[cap] tag=moviegate.130ef0 limit=" << std::dec
                      << kMovieGateMaxOpen << "\n";
        }
    }

    // --- wrappers 4/5: 0x124DC8 -> 0x124E38, the movie REQUEST -----------
    //
    // Emitted before AND after the body: "before" proves the call reached us
    // with the arguments the caller intended, "after" proves the five stores
    // actually landed. A request that arrives with a null filename is a
    // different bug from a request whose stores get lost, and a pre-only
    // record cannot tell them apart.
    void movieGateReqWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGateReqOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t a1 = GPR_U32(ctx, 5); // -> [+0x50], the filename pointer
        const uint32_t a2 = GPR_U32(ctx, 6); // -> [+0x54]
        const uint32_t a3 = GPR_U32(ctx, 7); // -> [+0x0c]
        const uint32_t a4 = GPR_U32(ctx, 8); // -> [+0x14], and [+0x10] = a4<<11
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGateReqCalls.fetch_add(1u, std::memory_order_relaxed);
        const std::string name = movieGateStr(rdram, a1);

        original(rdram, ctx, runtime);

        if (seq < kMovieGateMaxReq)
        {
            const MovieGateState post = movieGateSnap(rdram, base);
            std::ostringstream oss;
            oss << "[moviegate] fn=124e38 seq=" << std::dec << seq
                << std::hex
                << " base=0x" << base
                << " a1=0x" << a1
                << " a2=0x" << a2
                << " a3=0x" << a3
                << " a4=0x" << a4
                << " ra=0x" << ra
                << " post.arm45=0x" << (uint32_t)post.b45
                << " post.fptr50=0x" << post.fname
                << " post.arg54=0x" << post.arg1
                << " post.hnd08=0x" << post.handle
                << std::dec << " name=\"" << name << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kMovieGateMaxReq)
        {
            std::cerr << "[cap] tag=moviegate.124e38 limit=" << std::dec
                      << kMovieGateMaxReq << " -- later requests are invisible\n";
        }
    }

    void movieGateReqOuterWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGateReqOuterOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        // This is the level whose $ra is worth anything: 0x124e38's own caller
        // is always this thunk.
        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t a1 = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGateReqOuterCalls.fetch_add(1u, std::memory_order_relaxed);

        if (seq < kMovieGateMaxReq)
        {
            std::ostringstream oss;
            oss << "[moviegate] fn=124dc8 seq=" << std::dec << seq
                << std::hex
                << " base=0x" << base
                << " a1=0x" << a1
                << " ra=0x" << ra
                << std::dec << " name=\"" << movieGateStr(rdram, a1) << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kMovieGateMaxReq)
        {
            std::cerr << "[cap] tag=moviegate.124dc8 limit=" << std::dec
                      << kMovieGateMaxReq << " -- later requests are invisible\n";
        }

        original(rdram, ctx, runtime);
    }

    // --- wrappers 6/7: 0x126388 -> 0x1263B8, the gated pump --------------
    //
    // The pump refuses to do anything unless [+0x50] != 0 and [+0x08] == 0,
    // so its record carries both gate inputs and the verdict they imply. A
    // pump that runs often while gateFptr=0 says the play command IS being
    // issued and only the request is missing -- the opposite conclusion from
    // a pump that never runs at all, and the two are indistinguishable
    // without this counter.
    void movieGatePumpWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGatePumpOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGatePumpCalls.fetch_add(1u, std::memory_order_relaxed);

        const MovieGateState pre = movieGateSnap(rdram, base);
        const bool gatePass = (pre.fname != 0u && pre.handle == 0u);

        // Deliberately NOT movieGateChanged(): that tracker is keyed on base
        // and shared with the 0x125898 probe, which watches these same two
        // objects. Reusing it would let a pump snapshot swallow a state
        // transition, and vice versa -- each probe would then report the
        // other's silence as its own.
        static std::mutex pumpMx;
        static uint32_t lastBase = 0xFFFFFFFFu;
        static uint32_t lastFname = 0xFFFFFFFFu;
        static uint32_t lastHandle = 0xFFFFFFFFu;
        static uint32_t lastArm = 0xFFFFFFFFu;
        bool moved = false;
        {
            std::lock_guard<std::mutex> lock(pumpMx);
            moved = (base != lastBase || pre.fname != lastFname ||
                     pre.handle != lastHandle || pre.b45 != lastArm);
            lastBase = base;
            lastFname = pre.fname;
            lastHandle = pre.handle;
            lastArm = pre.b45;
        }

        if (seq < kMovieGateMaxPump && (seq < kMovieGateMinPump || moved))
        {
            std::ostringstream oss;
            oss << "[moviegate] fn=1263b8 seq=" << std::dec << seq
                << std::hex
                << " base=0x" << base
                << " ra=0x" << ra
                << " fptr50=0x" << pre.fname
                << " hnd08=0x" << pre.handle
                << " arm45=0x" << (uint32_t)pre.b45
                << " flag44beac=0x" << fontGateRead32(rdram, kMovieGatePumpFlag)
                << std::dec << " gate=" << (gatePass ? "PASS" : "blocked")
                << " why=" << (pre.fname == 0u ? "no-request"
                                               : (pre.handle != 0u ? "already-open" : "-"))
                << " name=\"" << movieGateStr(rdram, pre.fname) << "\"\n";
            std::cerr << oss.str();
            g_movieGatePumpEmit.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (seq == kMovieGateMaxPump)
        {
            std::cerr << "[cap] tag=moviegate.1263b8 limit=" << std::dec
                      << kMovieGateMaxPump << " -- later pumps are invisible\n";
        }

        original(rdram, ctx, runtime);
    }

    void movieGatePumpOuterWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_movieGatePumpOuterOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!movieGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_movieGatePumpOuterCalls.fetch_add(1u, std::memory_order_relaxed);

        if (seq < kMovieGateMinPump)
        {
            std::ostringstream oss;
            oss << "[moviegate] fn=126388 seq=" << std::dec << seq
                << std::hex << " base=0x" << base << " ra=0x" << ra
                << std::dec << "\n";
            std::cerr << oss.str();
        }

        original(rdram, ctx, runtime);
    }

    // --- wrapper 8: sub_130AD8, the irq-disable thunk over the lookup -----
    //
    // Cheap and deliberately dumb: it exists so that "the lookup returned 0"
    // and "0x130ef0 never called the lookup" cannot be confused. Its $ra is
    // the only one on this chain that names a real caller.
    void cdSearchThunkWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_cdSearchThunkOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!cdSearchEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4); // -> sceCdlFILE out, = handle+0x20
        const uint32_t a1 = GPR_U32(ctx, 5); // -> the "\MOVIE\....SFD;1" path
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_cdSearchThunkCalls.fetch_add(1u, std::memory_order_relaxed);
        const std::string name = movieGateStr(rdram, a1);

        original(rdram, ctx, runtime);

        if (seq < kCdSearchMax)
        {
            std::ostringstream oss;
            oss << "[cdsearch] fn=130ad8 seq=" << std::dec << seq
                << std::hex
                << " a0=0x" << a0
                << " ra=0x" << ra
                << " ret=0x" << GPR_U32(ctx, 2)
                << std::dec << " name=\"" << name << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kCdSearchMax)
        {
            std::cerr << "[cap] tag=cdsearch.130ad8 limit=" << std::dec << kCdSearchMax
                      << " -- later lookups are invisible; absence past this"
                         " point is NOT evidence\n";
        }
    }

    // --- wrapper 9: sub_1866A0, the EE sceCdSearchFile itself -------------
    //
    // Every field here exists to split one specific pair of rival readings;
    // none is decoration. In particular nameBuf is sampled AFTER the body so
    // it reports what the guest actually copied, not what we assume it would
    // have ([[feedback_probe_the_final_value]]), and the bind words are
    // sampled both before and after so a bind that completes inside this very
    // call is still visible rather than reading as "was always ready".
    void cdSearchWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_cdSearchOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!cdSearchEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4); // sceCdlFILE* out
        const uint32_t a1 = GPR_U32(ctx, 5); // const char* name
        const uint32_t a2 = GPR_U32(ctx, 6); // *0x44E70C, the search mode
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_cdSearchCalls.fetch_add(1u, std::memory_order_relaxed);

        const std::string name = movieGateStr(rdram, a1);
        const uint32_t preLatch = fontGateRead32(rdram, kCdSearchBindLatch);
        const uint32_t preDone = fontGateRead32(rdram, kCdSearchBindDone);
        const uint32_t preRecv = fontGateRead32(rdram, kCdSearchRecvWord);

        original(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        if (ret != 0u)
        {
            g_cdSearchNonZeroRet.fetch_add(1u, std::memory_order_relaxed);
        }

        if (seq < kCdSearchMax)
        {
            // The out struct is only meaningful if the guest gave us a sane
            // pointer; a garbage a0 must not be reported as lbn=0 size=0,
            // which is indistinguishable from a real empty result.
            const bool outOk = (a0 != 0u && a0 < 0x02000000u);
            std::ostringstream oss;
            oss << "[cdsearch] fn=1866a0 seq=" << std::dec << seq
                << std::hex
                << " a0=0x" << a0
                << " a2=0x" << a2
                << " ra=0x" << ra
                << " ret=0x" << ret
                // recv/4 IS the return value; showing both proves the return
                // came from the RPC rather than from an early-out path.
                << " recv840=0x" << fontGateRead32(rdram, kCdSearchRecvWord)
                << " preRecv=0x" << preRecv
                << " guard68=0x" << fontGateRead32(rdram, kCdSearchGuard)
                << " bindLatch=0x" << preLatch
                << "->0x" << fontGateRead32(rdram, kCdSearchBindLatch)
                << " bindDone=0x" << preDone
                << "->0x" << fontGateRead32(rdram, kCdSearchBindDone)
                // First word of the RPC client struct and of the 300-byte
                // send buffer. NOT the pointers -- those are the addresses
                // themselves, confirmed against [SIF:BIND] client=0x568880
                // and send=0x568700/0x12c in the 08-17 log.
                << " clientW0=0x" << fontGateRead32(rdram, kCdSearchClientPtr)
                << " sendLbn=0x" << fontGateRead32(rdram, kCdSearchSendPtr)
                << " sendSize=0x" << fontGateRead32(rdram, kCdSearchSendPtr + 4u)
                << " sendMode=0x" << fontGateRead32(rdram, kCdSearchModeWord)
                // >0 would mean the guest's own "ee call cmd search" /
                // "libcdvd bind error" printfs are enabled. It reads 0, which
                // is why this whole path has been silent in every run so far.
                << " verbose=0x" << fontGateRead32(rdram, kCdSearchVerbose)
                << " outLbn=0x" << (outOk ? fontGateRead32(rdram, a0) : 0u)
                << " outSize=0x" << (outOk ? fontGateRead32(rdram, a0 + 4u) : 0u)
                << std::dec
                << " name=\"" << name << "\""
                // THE discriminator: what the guest actually copied into its
                // own 256-byte search buffer. Matching `name` proves execution
                // reached the copy and therefore the RPC.
                << " nameBuf=\"" << movieGateStr(rdram, kCdSearchNameBuf, 48u) << "\""
                << " outName=\"" << (outOk ? movieGateStr(rdram, a0 + 8u, 24u)
                                           : std::string("<badptr>"))
                << "\"\n";
            std::cerr << oss.str();
        }
        else if (seq == kCdSearchMax)
        {
            std::cerr << "[cap] tag=cdsearch.1866a0 limit=" << std::dec << kCdSearchMax
                      << " -- later lookups are invisible; absence past this"
                         " point is NOT evidence\n";
        }
    }

    // --- wrappers 10/11: 0x11CBF0 -> 0x11CC28, the ADX status/pump GATE ---
    //
    // Stage 5.16 traced 0x1263b8 (the movie-open pump) to zero calls in run
    // 64 despite its own wrapper reporting "installed=1" -- the pump itself
    // is wired correctly, it is simply never reached. sub_11CC28 is its only
    // static caller (grep for the symbol name, not the address: the callee
    // is invoked as `noop_wrapper___152(*(int*)(a1 + 4))`), and it is gated
    // on a completely different field:
    //
    //     if ( (unsigned)*(int*)(a1 + 16) > 0x7FFFF9FFu ) {   // "pending"
    //         noop_wrapper___152(*(int*)(a1 + 4));            // -> pump
    //         ...
    //     } else {
    //         return *(int*)(a1 + 16);                        // cached, no pump
    //     }
    //
    // *(a1+16) is an ADX file-status word (this call chain's own error
    // strings say "...ADXF..."). The threshold 0x7FFFF9FF means the pump
    // only runs while the status reads as a small negative number
    // (-1600..-1 signed) -- i.e. "still pending". This splits two very
    // different next fixes:
    //
    //   calls=0                 -> sub_11CC28 itself is never invoked; the
    //                              blocker is one level further back still.
    //   calls>0, gate=blocked   -> the status word is never in the pending
    //                              range, so the pump is skipped by design
    //                              every time -- plausibly because Stage
    //                              5.16's retry loop never lets it get there.
    //   calls>0, gate=PASS      -> the pump SHOULD fire; if 1263b8's own
    //                              counter is still 0 here, `pumpBase`
    //                              (a1+4) is not the address we think it is.
    constexpr uint32_t kAdxGateOuterFn = 0x0011CBF0u; // thunk -> 0x11cc28
    constexpr uint32_t kAdxGateFn = 0x0011CC28u;      // status check + pump trigger
    constexpr uint64_t kAdxGateMax = 64u;

    PS2Runtime::RecompiledFunction g_adxGateOuterOrig = nullptr;
    PS2Runtime::RecompiledFunction g_adxGateOrig = nullptr;
    std::atomic<uint64_t> g_adxGateOuterCalls{0u};
    std::atomic<uint64_t> g_adxGateCalls{0u};

    bool adxGateEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_ADXGATE");
            return e == nullptr || (e[0] != '\0' && e[0] != '0');
        }();
        return on && movieGateEnabled();
    }

    void adxGateWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_adxGateOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!adxGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a1 = GPR_U32(ctx, 4); // ADX file object
        const uint32_t a2 = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_adxGateCalls.fetch_add(1u, std::memory_order_relaxed);

        const bool objOk = (a1 != 0u && a1 < 0x02000000u);
        const uint32_t preStatus = objOk ? fontGateRead32(rdram, a1 + 0x10u) : 0u;
        const uint32_t pumpBase = objOk ? fontGateRead32(rdram, a1 + 0x04u) : 0u;
        const bool gatePass = objOk && (preStatus > 0x7FFFF9FFu);

        original(rdram, ctx, runtime);

        if (seq < kAdxGateMax)
        {
            const uint32_t ret = GPR_U32(ctx, 2);
            const uint32_t postStatus = objOk ? fontGateRead32(rdram, a1 + 0x10u) : 0u;
            std::ostringstream oss;
            oss << "[adxgate] fn=11cc28 seq=" << std::dec << seq
                << std::hex
                << " a1=0x" << a1
                << " a2=0x" << a2
                << " ra=0x" << ra
                << " preStatus=0x" << preStatus
                << " pumpBase=0x" << pumpBase
                << " ret=0x" << ret
                << " postStatus=0x" << postStatus
                << std::dec << " gate=" << (gatePass ? "PASS" : "blocked") << "\n";
            std::cerr << oss.str();
        }
        else if (seq == kAdxGateMax)
        {
            std::cerr << "[cap] tag=adxgate.11cc28 limit=" << std::dec << kAdxGateMax
                      << " -- later status checks are invisible\n";
        }
    }

    void adxGateOuterWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_adxGateOuterOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!adxGateEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a1 = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_adxGateOuterCalls.fetch_add(1u, std::memory_order_relaxed);

        if (seq < kAdxGateMax)
        {
            std::ostringstream oss;
            oss << "[adxgate] fn=11cbf0 seq=" << std::dec << seq
                << std::hex << " a1=0x" << a1 << " ra=0x" << ra
                << std::dec << "\n";
            std::cerr << oss.str();
        }

        original(rdram, ctx, runtime);
    }

    // --- wrappers 12-14: neighbors of the 11CBF0/11CC28 block ---
    //
    // Run 65: adxgate records=0 -- 0x11CBF0 (the thunk INTO 0x11CC28) never
    // fires either, not just 0x11CC28 itself. Static search is exhausted for
    // both addresses (no symbol-name xref, no literal 4-byte match anywhere
    // in ELF/SLUS_214.42), so the real entry point is further back still.
    // 0x11C9F8/0x11CBB0/0x11CCF0 are the physically-adjacent real functions
    // in this same block (IDA fills the gaps between them with
    // noop_wrapper___ stubs). None of the three has a static xref either --
    // this probe exists purely to catch a first hit and read $ra off it,
    // which names the caller even though nothing in the binary names this
    // callee.
    constexpr uint32_t kAdxNeighborFn9F8 = 0x0011C9F8u;
    constexpr uint32_t kAdxNeighborFnBB0 = 0x0011CBB0u;
    constexpr uint32_t kAdxNeighborFnCF0 = 0x0011CCF0u;
    constexpr uint64_t kAdxNeighborMax = 16u;

    PS2Runtime::RecompiledFunction g_adxNeighbor9F8Orig = nullptr;
    PS2Runtime::RecompiledFunction g_adxNeighborBB0Orig = nullptr;
    PS2Runtime::RecompiledFunction g_adxNeighborCF0Orig = nullptr;
    std::atomic<uint64_t> g_adxNeighbor9F8Calls{0u};
    std::atomic<uint64_t> g_adxNeighborBB0Calls{0u};
    std::atomic<uint64_t> g_adxNeighborCF0Calls{0u};

    void adxNeighborLog(const char *tag, uint32_t addr, std::atomic<uint64_t> &counter,
                         R5900Context *ctx)
    {
        const uint64_t seq = counter.fetch_add(1u, std::memory_order_relaxed);
        if (seq >= kAdxNeighborMax)
        {
            if (seq == kAdxNeighborMax)
            {
                std::cerr << "[cap] tag=adxneighbor." << std::hex << addr << std::dec
                          << " limit=" << kAdxNeighborMax << " -- later calls invisible\n";
            }
            return;
        }
        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t a1 = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);
        std::ostringstream oss;
        oss << "[adxneighbor] fn=" << std::hex << addr << " seq=" << std::dec << seq
            << std::hex << " a0=0x" << a0 << " a1=0x" << a1 << " ra=0x" << ra
            << std::dec << " tag=" << tag << "\n";
        std::cerr << oss.str();
    }

    void adxNeighbor9F8Wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_adxNeighbor9F8Orig;
        if (original == nullptr)
        {
            return;
        }
        if (adxGateEnabled() && rdram != nullptr)
        {
            adxNeighborLog("11c9f8", kAdxNeighborFn9F8, g_adxNeighbor9F8Calls, ctx);
        }
        original(rdram, ctx, runtime);
    }

    void adxNeighborBB0Wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_adxNeighborBB0Orig;
        if (original == nullptr)
        {
            return;
        }
        if (adxGateEnabled() && rdram != nullptr)
        {
            adxNeighborLog("11cbb0", kAdxNeighborFnBB0, g_adxNeighborBB0Calls, ctx);
        }
        original(rdram, ctx, runtime);
    }

    void adxNeighborCF0Wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_adxNeighborCF0Orig;
        if (original == nullptr)
        {
            return;
        }
        if (adxGateEnabled() && rdram != nullptr)
        {
            adxNeighborLog("11ccf0", kAdxNeighborFnCF0, g_adxNeighborCF0Calls, ctx);
        }
        original(rdram, ctx, runtime);
    }

    // ================= [sofdec] the REAL per-frame movie driver ============
    //
    // 2026-08-19. Everything above this line instruments the ADXF chain
    // (0x11cbf0 -> 0x11cc28 -> 0x126388 -> 0x1263b8). Run 64 measured that
    // chain's pump at calls=0 with installed=1, and a live PCSX2 trace of the
    // retail disc then reproduced exactly the same result on real hardware:
    //
    //   * execution breakpoints on 0x126388 and 0x1263b8 took ZERO hits
    //     across ~1.9e9 EE cycles, from a cold boot through the load screen;
    //   * find_pattern for the little-endian pointer bytes of both addresses
    //     over 0x00100000-0x02000000 returned no matches, so they are not
    //     reachable through a flat function-pointer table either.
    //
    // So the recomp is FAITHFUL here. The ADXF pump is dead code on the path
    // we care about, and "the pump never runs" was never the bug -- it was a
    // correct measurement of the wrong function.
    //
    // The same PCSX2 session found what does drive movies. With breakpoints
    // on the whole CAppCRISofdec_* family:
    //
    //   cycle   59,765,856  CAppCRISofdec_Tick_clone_01 (0x3fc250)
    //                       caller frame entry=0x3e1280 = loadscreen_reset
    //   cycle 1,393,119,062 CAppCRISofdec_Tick_clone_03 (0x4212b0)
    //                       caller frame entry=0x3e0e60 = loadscreen_tick
    //   re-hit at 1,398,251,378 (d=5,132,316) and 1,403,130,179 (d=4,878,801)
    //
    // NTSC is 4,919,595 EE cycles/frame, and both deltas bracket it, so
    // clone_03 is a genuine per-frame tick rather than a one-off. loadscreen_*
    // owns a Sofdec instance and pumps it every frame; that is the live movie
    // path, and it shares no code with the ADXF chain.
    //
    // WHAT THIS PROBE DECIDES. loadscreen_tick reaches clone_03 through a
    // JALR, not a JAL -- its recompiled body at 0x3e0e60 contains no direct
    // call to 0x4212b0, only four indirect dispatches (0x3e0f34, 0x3e0fd8,
    // 0x3e101c, 0x3e1060). Every one of them goes through dispatchGuestBranch,
    // so replaceFunction wrappers do see them ([[feedback_registerfunction_bypass]]
    // does not bite here; that was checked in the generated body first, not
    // assumed). All nine bodies exist in src/runner/, so this is not a Stage
    // 5.13 dispatch hole. That leaves three readings, and the counters below
    // separate them without a second run:
    //
    //   loadscreen_tick=0            -> the blocker is upstream of the movie
    //                                   code entirely; Sofdec is irrelevant.
    //   loadscreen_tick>0, ticks=0   -> we reach the caller but the JALR
    //                                   target is wrong, i.e. the instance's
    //                                   function pointer was never populated.
    //   ticks>0                      -> the recomp does drive Sofdec, and the
    //                                   divergence is inside a tick, not in
    //                                   whether one happens.
    //
    // Counters are reported as tag=<calls>/<installed> so that a zero can
    // never be read as "the wrapper was there and saw nothing" when in fact
    // the address was missing from the dense table
    // ([[feedback_probe_gate_on_shape_not_address]]), and a detached reporter
    // prints them on a timer rather than from any guest callback -- an
    // all-zero result is the single most informative outcome here, so it must
    // not depend on guest code running to be seen.
    struct SofdecSite
    {
        const char *tag;
        uint32_t addr;
    };

    constexpr SofdecSite kSofdecSites[] = {
        {"loadscreen_reset.3e1280", 0x003E1280u},
        {"loadscreen_tick.3e0e60", 0x003E0E60u},
        {"Begin.3f9bd0", 0x003F9BD0u},
        {"Tick.3f9c10", 0x003F9C10u},
        {"End.3f9d50", 0x003F9D50u},
        {"Update.3f9d80", 0x003F9D80u},
        {"Tick_c1.3fc250", 0x003FC250u},
        {"Tick_c2.420780", 0x00420780u},
        {"Tick_c3.4212b0", 0x004212B0u},
        {"nullcb.115318", 0x00115318u},
        {"st4cb.420260", 0x00420260u},
        {"st4b.3e2ff0", 0x003e2ff0u},
        {"crisrv.11d510", 0x0011D510u},
        {"cvfsrd.131190", 0x00131190u},
    };
    constexpr size_t kSofdecSiteCount = sizeof(kSofdecSites) / sizeof(kSofdecSites[0]);

    // Per site, not global: clone_03 is per-frame and would otherwise burn the
    // whole budget before loadscreen_reset's one-shot ever printed.
    constexpr uint64_t kSofdecMaxEmit = 24u;
    constexpr unsigned kSofdecStatPeriodSec = 5u;

    PS2Runtime::RecompiledFunction g_sofdecOrig[kSofdecSiteCount] = {};
    std::atomic<uint64_t> g_sofdecCalls[kSofdecSiteCount];
    std::atomic<uint32_t> g_sofdecFirstRa[kSofdecSiteCount];
    std::atomic<uint32_t> g_sofdecLastA0[kSofdecSiteCount];

    // ---- [lstick] why loadscreen_tick does 1870 calls and almost no work ---
    //
    // Run 65 measured: loadscreen_tick=1870, Tick_c3=4, Begin/Tick/End/Update=0.
    // The disassembly of 0x3e0e60 says byte[$s0+9] is a STATE SELECTOR:
    //
    //   0x3e0e6c  lbu  $v0, -3514($gp)   ; global enable byte
    //   0x3e0e70  beqz $v0, 0x3e0fb8     ; <-- whole tick is a no-op if 0
    //   0x3e0ec8  state==2 -> 0x3e0fd0 -> jalr vtable[+0x38]   (= Tick_c3)
    //   0x3e0ed8  state==3 -> 0x3e1014 -> jalr vtable[+0x44]
    //   0x3e0ee8  state==4 -> 0x3e1058 -> jalr
    //
    // So run 65's zeros do NOT mean "the call site is broken" -- they mean the
    // object sat in some state whose arm we never probed. Probing callees was
    // the wrong measurement; the state byte and the resolved $t9 are the right
    // one. The gate address is derived from the live $gp, not hardcoded, so a
    // wrong $gp shows up as a wrong gateAddr instead of a confident wrong byte.
    std::atomic<uint32_t> g_lstickGateAddr{0u};
    std::atomic<uint32_t> g_lstickGate{0xFFFFFFFFu};
    std::atomic<uint64_t> g_lstickState[16];
    std::atomic<uint64_t> g_lstickStateOther{0u};
    std::atomic<uint32_t> g_lstickF8{0xFFFFFFFFu};
    std::atomic<uint32_t> g_lstickB18{0xFFFFFFFFu};
    std::atomic<uint32_t> g_lstickB19{0xFFFFFFFFu};
    std::atomic<uint32_t> g_lstickW12{0xFFFFFFFFu};
    std::atomic<uint32_t> g_lstickVt{0u};
    std::atomic<uint32_t> g_lstickVt38{0u};
    std::atomic<uint32_t> g_lstickVt40{0u};
    std::atomic<uint32_t> g_lstickVt44{0u};
    std::atomic<uint32_t> g_nullcbSlotAddr{0u};
    std::atomic<uint32_t> g_nullcbSlot{0xFFFFFFFFu};
    std::atomic<uint32_t> g_nullcbA0C0{0xFFFFFFFFu};

    // ---- [st4] the one measurement run 70 was missing -----------------------
    //
    // Run 70: st=1:6,2:27,4:3668 -- the loadscreen object is parked in state 4
    // for 3668 of 3701 ticks with f8=0x0 and w12=0x0, i.e. nothing is
    // error-latched, it simply never advances. loadscreen_tick's state-4 arm is
    //
    //     if (byte[a1+9] == 4 && ((fn)vtable[+0x40])(a1) && ...) byte[a1+9] = 5;
    //
    // and vtable[+0x40] was read STATICALLY out of the ELF (PT_LOAD-mapped, not
    // guessed) as 0x00420260 -- the same table whose +0x38/+0x44 entries the
    // live run already reported byte-for-byte, so the vtable is intact and the
    // callee identification is not an inference.
    //
    // 0x420260 is an inner state machine on *(int *)(a1+164) that returns 1
    // ONLY from case 10, and only when camera_fade_is_active(a1) is false;
    // every other path falls through to result = 0. Cases 0..5 form a loop:
    // case 5 requires the accumulator *(float *)(a1+172) to reach 0.083333
    // (5/60 s), fed each call by *(float *)(*(uint32_t *)(a1+40)+4) -- a frame
    // delta -- and then resets a1+164 back to 0. The only escape toward case 10
    // is case 3 with byte[a1+161] == 2.
    //
    // Four readings fit "returns 0 forever", and these counters separate them
    // in ONE run rather than four:
    //
    //   sub stuck at 1/4        -> the fade never clears
    //   sub cycles 0..5, acc    -> the frame delta is ZERO, i.e. our timing
    //     not growing              source, not the game
    //   sub cycles 0..5, acc    -> the loop is by design; nobody ever writes 2
    //     growing                  into a1+161, so the blocker is the CALLER
    //   sub stuck at 10         -> the fade itself never completes
    //
    // accUp is the load-bearing field: an accumulator sampled once cannot tell
    // "not advancing" from "advancing between samples"
    // ([[feedback_probe_the_final_value]]), so growth is counted per call
    // instead of inferred from first-vs-last. ret1 carries the rival reading --
    // if it is ever nonzero then state 4 DID get its green light and the stall
    // is downstream of here, which would convict this whole probe
    // ([[feedback_degenerate_result_convicts_the_probe]]).
    std::atomic<uint64_t> g_st4Sub[16];
    std::atomic<uint64_t> g_st4SubOther{0u};
    std::atomic<uint64_t> g_st4Ret1{0u};
    std::atomic<uint64_t> g_st4AccUp{0u};
    std::atomic<uint64_t> g_st4SubChanged{0u};
    std::atomic<uint32_t> g_st4A0{0u};
    std::atomic<uint32_t> g_st4Clk{0u};
    std::atomic<uint32_t> g_st4DtBits{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4AccFirst{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4AccLast{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4AccMax{0u};
    std::atomic<uint32_t> g_st4SubLast{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4SubAfter{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4B160{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4B161{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4B162{0xFFFFFFFFu};

    // ---- [st4b] the gate that actually governs the stuck object ------------
    //
    // Run 71 CONVICTED the [st4] probe, not the game. [st4:stat] reported a
    // fully healthy machine -- sub cycled 0..5, reached case 10, and ret1
    // finally went to 1 -- while [lstick:stat] sat at st=4:6111 and rising.
    // Both cannot describe the same object, and they do not:
    //
    //   [sofdec:stat] loadscreen_tick a0=0x632b40   <- the STUCK object
    //                 st4cb.420260   a0=0x632df0   <- a DIFFERENT object
    //   [lstick:stat] vt=0x4f9a70  vt38=0x3e2e80  vt44=0x3e31f0
    //
    // and vtable 0x4f9a70 read straight out of the ELF gives +0x40 = 0x3e2ff0,
    // NOT the 0x420260 that vtable 0x4fadf0 gives. 0x4fadf0 was the vtable the
    // object carried in run 70; the class changed under us, so a probe gated on
    // a statically resolved callee measured a sibling class that works fine
    // ([[feedback_probe_gate_on_shape_not_address]]). vt40 is now printed in
    // [lstick:stat] so this exact mis-aim is visible without an ELF read.
    //
    // sub_3E2FF0(float *a1) is a far simpler machine than 0x420260:
    //
    //   *(int *)(a1+48)   sub-state: 0 -> 1 -> 2 -> 3 -> 4, and 4 returns 1
    //   *(float *)(a1+44) accumulator; state 0 does acc += dt until acc > 83.0
    //   *(int *)(a1+40)   clock object, dt = *(float *)(clk+4)
    //   state 1 waits for camera_fade_is_active(1) to go false
    //   states 2 and 3 fall through immediately
    //
    // The threshold is the whole question. This class also writes 230.0 and
    // 1000.0 into that same accumulator as skip values, which reads as
    // MILLISECONDS (83 ms = 5 frames, 1000 ms = 1 s). But 0x420260's equivalent
    // threshold was 0.083333 -- SECONDS -- against what looked like the same
    // clk+4 field, and [st4:stat] measured dtF=0.0166667 there. If that same
    // 1/60 lands in this accumulator, 83.0 needs 4980 ticks instead of 5, which
    // at the observed ~25 ticks/s is 200 s of dead loadscreen.
    //
    // That is a hypothesis with a clean rival, and clkB is captured separately
    // from dt precisely so the two can be told apart: if clkB differs from the
    // 0x5ad580 that [st4:stat] reported, the two classes read different clocks
    // and the unit mismatch is imaginary. accMaxB adjudicates -- it says how
    // close to 83.0 the accumulator actually got inside the run window, so a
    // "never arrives" verdict carries its own number instead of resting on
    // absence ([[feedback_run_window_false_negative]]).
    std::atomic<uint64_t> g_st4bSub[16];
    std::atomic<uint64_t> g_st4bSubOther{0u};
    std::atomic<uint64_t> g_st4bRet1{0u};
    std::atomic<uint64_t> g_st4bAccUp{0u};
    std::atomic<uint64_t> g_st4bOver83{0u};
    std::atomic<uint32_t> g_st4bA0{0u};
    std::atomic<uint32_t> g_st4bClk{0u};
    std::atomic<uint32_t> g_st4bDtBits{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4bAccFirst{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4bAccLast{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4bAccMax{0u};
    std::atomic<uint32_t> g_st4bSubAfter{0xFFFFFFFFu};
    std::atomic<uint32_t> g_st4bW52{0xFFFFFFFFu};

    // ---- [crisrv] why the movie never finishes ------------------------------
    //
    // Runs 72/73 closed the loadscreen question and opened this one. The 83.0
    // accumulator in sub_3E2FF0 is NOT a hang -- it is the TIMEOUT ARM of a
    // wait-for-the-movie-to-end. The skip arm is
    //
    //     if (MovieUpdate() == 1) a1[11] = 230.0;     // 230 > 83 -> advance
    //
    // where MovieUpdate is 0x113920, identified positively rather than by
    // name: it shares its globals (dword_54BD90, byte_54BE28) with 0x113AA0,
    // and 0x113AA0 is the function that prints "MovieCreate: Use Work Size =
    // 0x%x". That string appears 6x in the run 73 log, so the movie handle
    // EXISTS. MovieUpdate returns (status - 1) >= 2, i.e. 1 only once status
    // leaves {1,2}, and [movie] reported stat=1 maxstat=1 across every one of
    // 298 records in run 73 and all 284 in run 72. The movie is pinned in
    // "still playing" forever, so the loadscreen is correctly falling through
    // to its 83-second safety net. Nothing on the loadscreen side is broken.
    //
    // The data supply is where it dies, and the counts are lopsided in a way
    // that names the layer:
    //
    //     12ee20  = 6      the SRD pump: state 1 issues sceCdRead, 2 waits
    //     130b80  = 1955   the completion poller
    //     cvfsStat= 6      ... but it found an outstanding request only 6x
    //
    // 130b80 returns immediately unless obj+28 holds a pending request, so
    // 1949 of those 1955 polls were no-ops. The poller is alive; the ISSUER is
    // not. And 12ee20 has exactly ONE caller in the whole ELF -- 0x12F1E8 --
    // which in turn is reached only from 0x11D510, the CRI server tick:
    //
    //     0x11D4E8 -> 0x11D510 -> { SRD tick, ..., SRD tick }
    //
    // 0x11D510 opens with a reentrancy guard that is not a boolean but a
    // PROGRAM COUNTER: dword_4407D8 is set to 1,2,3...8 between successive
    // sub-ticks and cleared to 0 at the end, and the function bails at the top
    // whenever it is nonzero. So a guest that dies, is killed, or is preempted
    // and never resumed inside any sub-tick strands the guard at the value of
    // the step it was on -- and every later call returns instantly, silently,
    // for the rest of the run. That is exactly the shape of "6 reads and then
    // nothing", and it is not idle speculation: this same run reports
    // DEFERINL >= 4096, i.e. the scheduler dispatched an interrupt INLINE,
    // inside a guest critical section, at least four thousand times. (That
    // 4096 is itself only a lower bound, and it was reported as "64" until the
    // silent cap on that probe was fixed for this very run.)
    //
    // Three readings, separated in one run:
    //   calls=0            -> nobody drives the server at all; the guard is
    //                         innocent and the bug is a missing caller
    //   calls>0, bail~=0   -> the server runs fine and the stall is BELOW it,
    //                         in byte_44D231 / the CVFS layer -- which would
    //                         convict this probe's whole premise
    //   calls>0, bail>>0   -> the guard is stranded; ent= names the sub-tick
    //
    // The globals are ALSO sampled on the stat thread, not only on entry, so
    // the calls=0 reading still arrives with the SRD state attached instead of
    // a row of zeros that cannot be told apart from "never sampled"
    // ([[feedback_probe_gate_on_shape_not_address]]).
    constexpr uint32_t kCriGuard    = 0x004407D8u; // 0=idle, 1..8=inside step N
    constexpr uint32_t kCriSrdLock  = 0x0044D26Cu; // 0x12F1E8 reentrancy lock
    // srd_obj: +1 devtype (DVD=2/HST=1), +2 stat -- both names taken from
    // sub_12F598, the "SRD Info" debug printer inside the game itself.
    constexpr uint32_t kCriSrdObj   = 0x0044D230u; // arg to 0x12EE20
    constexpr uint32_t kCriIssued   = 0x0044D288u; // ++ on sceCdRead accepted
    constexpr uint32_t kCriDone     = 0x0044D28Cu; // ++ on completion/abort
    constexpr uint32_t kCriErr      = 0x0044D284u; // printed by the SRD error
    constexpr uint32_t kCriA8       = 0x0044D2A8u;

    std::atomic<const uint8_t *> g_criRdram{nullptr};
    std::atomic<uint64_t> g_criEnt[16];
    std::atomic<uint64_t> g_criEntOther{0u};
    std::atomic<uint64_t> g_criBail{0u};

    // ---- [cvfs] the layer that stopped asking --------------------------------
    //
    // Run 74 CONVICTED the [crisrv] premise, which is exactly what it was built
    // to be able to do. The CRI server tick ran 1121 times and the step-numbered
    // reentrancy guard was 0 on EVERY ONE of them (bail=0, ent=0:1121). Nothing
    // is stranded. So the stall is below the server, and run 74's own numbers
    // say how far below:
    //
    //   crisrv.11d510 = 1121   the server tick -- healthy
    //   12ee20        = 4      the SRD pump
    //   issued/done   = 3/3    every read that was issued also COMPLETED
    //   dvci130b80    = 1382   the completion poller, finding work only 4x
    //   srd_obj       = all zero: devtype=0 stat=0 pend=0 lsn=0 secs=0 buf=0
    //
    // and the three reads that did happen are consecutive and clean:
    //
    //   lbn=0x157484 nsec=0x168 -> postStat=3
    //   lbn=0x1575ed nsec=0x16d -> postStat=3      (0x157484+0x168 = 0x1575ec)
    //   lbn=0x157a8e nsec=0x16d -> postStat=3
    //
    // The whole disc path works. It is simply never asked again.
    //
    // The field names are not guesses: sub_12F598 is the game's own "SRD Info"
    // printer, and it labels dword_44D26C srd_enter_fg, dword_44D284
    // srd_debug_geterror, byte_44D232 srd_obj.stat, and byte_44D231 devtype
    // (DVD=2 / HST=1). Our probe read devtype=0 -- NEITHER -- and sub_12F1E8
    // dispatches the pump only on devtype 1 or 2, so with 0 in there both arms
    // are skipped forever and the pump has nothing to do. devtype is written in
    // exactly one place, sub_12E7B0 (srd_read), which also fills in lbn, nsct
    // and buf. Nobody called it.
    //
    // sub_12E7B0's only caller is sub_131190, the CVFS read entry -- and 131190
    // has NO jal callers anywhere in the ELF, it is reached purely through a
    // device-driver function pointer. That is the same shape as the Stage 5.11
    // comparator, so the first thing to establish is whether it is being reached
    // at all. Its body IS present in the runner (three variants) and run 74 had
    // dispatch-miss=0, so this is not a dispatch hole -- but "present" and
    // "called" are different claims and only one of them has been measured.
    //
    // 131190 is probed here and 12E7B0 is NOT, deliberately: the generated
    // sub_131190_0x131190.cpp contains zero dispatchGuestBranch calls and three
    // direct references to 0x12E7B0, so a replaceFunction on 12E7B0 would be
    // bypassed and would report 0/1 forever
    // ([[feedback_registerfunction_bypass]]). 131190 itself is a pointer target,
    // so it goes through the dispatch loop and the wrapper does fire. Acceptance
    // is therefore measured the only way that is actually observable: by reading
    // the SRD object's devtype AFTER the call returns.
    //
    // sub_131190(obj, nsct, buf) has four rejection paths and they are NOT
    // equivalent, so each gets its own counter rather than being collapsed into
    // one "returned 0":
    //
    //   obj == 0        -> prints E0092912 Handle Invalid
    //   nsct < 0        -> prints E0092913 nsct<0
    //   buf == 0        -> prints E0092914 Buf is NULL
    //   obj[2] == 2     -> SILENT. async read already outstanding
    //   nsct == 0       -> SILENT. end of file, or a zero-length request
    //
    // The two silent ones are the interesting ones precisely because they leave
    // nothing in the log, and obj[2]==2 is self-sustaining: 131190 sets it to 2
    // on every accepted async read and only 130B80 clears it back to 1. A single
    // lost completion parks the whole stream permanently.
    //
    // Field offsets come from 130B80's own switch, not from inference:
    //   +2 stat  +8 total  +12 pos  +16 nsct  +20 lastLen  +24 buf  +28 req
    // The object address is LATCHED from the live a0 rather than hardcoded, and
    // printed, so a wrong object convicts itself instead of reporting a
    // confident row of zeros ([[feedback_probe_gate_on_shape_not_address]]).
    constexpr uint32_t kCvfsObjFallback = 0x0044F278u;

    std::atomic<uint32_t> g_cvfsObj{0u};
    std::atomic<uint64_t> g_cvfsCalls{0u};
    std::atomic<uint64_t> g_cvfsBadObj{0u};
    std::atomic<uint64_t> g_cvfsNsctNeg{0u};
    std::atomic<uint64_t> g_cvfsBufNull{0u};
    std::atomic<uint64_t> g_cvfsStat2{0u};
    std::atomic<uint64_t> g_cvfsNsct0{0u};
    std::atomic<uint64_t> g_cvfsPassable{0u};
    std::atomic<uint64_t> g_cvfsRetNz{0u};
    std::atomic<uint64_t> g_cvfsArmed{0u};
    std::atomic<uint64_t> g_cvfsStatIn[8];
    std::atomic<uint64_t> g_cvfsStatInOther{0u};
    std::atomic<uint32_t> g_cvfsLastNsct{0xFFFFFFFFu};
    std::atomic<uint32_t> g_cvfsLastBuf{0xFFFFFFFFu};
    std::atomic<uint32_t> g_cvfsRet{0xFFFFFFFFu};
    std::atomic<uint32_t> g_cvfsStatOut{0xFFFFFFFFu};
    std::atomic<uint32_t> g_cvfsDevAfter{0xFFFFFFFFu};

    bool sofdecAddrOk(uint32_t a)
    {
        return a != 0u && a < 0x02000000u;
    }

    uint32_t sofdecRead32(const uint8_t *rdram, uint32_t guestAddr)
    {
        if (rdram == nullptr || !sofdecAddrOk(guestAddr))
        {
            return 0xFFFFFFFFu;
        }
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (guestAddr & 0x01FFFFFCu), sizeof(v));
        return v;
    }

    uint32_t sofdecRead8(const uint8_t *rdram, uint32_t guestAddr)
    {
        if (rdram == nullptr || !sofdecAddrOk(guestAddr))
        {
            return 0xFFFFFFFFu;
        }
        return rdram[guestAddr & 0x01FFFFFFu];
    }

    // Raw bits are carried alongside every decoded float: 0xFFFFFFFF is the
    // read-failed sentinel and decodes to a NaN that would otherwise be
    // indistinguishable from a genuine NaN in the guest's accumulator.
    float sofdecF32(uint32_t bits)
    {
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // Return value and post-call sub-state. Sampling a1+164 only on entry
    // cannot see a transition the call itself performs, and $v0 is the entire
    // question for state 4 -- a probe that never reads it would be measuring
    // the input to the decision instead of the decision.
    void sofdecCapturePost(size_t site, const uint8_t *rdram, R5900Context *ctx);

    void sofdecCapture(size_t site, const uint8_t *rdram, R5900Context *ctx)
    {
        if (rdram == nullptr || ctx == nullptr)
        {
            return;
        }
        const uint32_t a0 = GPR_U32(ctx, 4);

        // loadscreen_tick: the state machine and its two vtable arms.
        if (kSofdecSites[site].addr == 0x003E0E60u)
        {
            const uint32_t gateAddr = GPR_U32(ctx, 28) - 3514u;
            g_lstickGateAddr.store(gateAddr, std::memory_order_relaxed);
            g_lstickGate.store(sofdecRead8(rdram, gateAddr), std::memory_order_relaxed);

            const uint32_t st = sofdecRead8(rdram, a0 + 9u);
            if (st < 16u)
            {
                g_lstickState[st].fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                g_lstickStateOther.fetch_add(1u, std::memory_order_relaxed);
            }
            g_lstickF8.store(sofdecRead8(rdram, a0 + 8u), std::memory_order_relaxed);
            g_lstickB18.store(sofdecRead8(rdram, a0 + 18u), std::memory_order_relaxed);
            g_lstickB19.store(sofdecRead8(rdram, a0 + 19u), std::memory_order_relaxed);
            g_lstickW12.store(sofdecRead32(rdram, a0 + 12u), std::memory_order_relaxed);

            const uint32_t vt = sofdecRead32(rdram, a0 + 0u);
            g_lstickVt.store(vt, std::memory_order_relaxed);
            g_lstickVt38.store(sofdecRead32(rdram, vt + 0x38u), std::memory_order_relaxed);
            g_lstickVt40.store(sofdecRead32(rdram, vt + 0x40u), std::memory_order_relaxed);
            g_lstickVt44.store(sofdecRead32(rdram, vt + 0x44u), std::memory_order_relaxed);
            return;
        }

        // 0x420260: state 4's callee. Everything here is read off a0, which is
        // the same object loadscreen_tick reports, so a mismatched a0 in the
        // stat line convicts the site identification before any field is
        // believed ([[feedback_probe_gate_on_shape_not_address]]).
        if (kSofdecSites[site].addr == 0x00131190u)
        {
            const uint32_t nsct = GPR_U32(ctx, 5);
            const uint32_t buf  = GPR_U32(ctx, 6);
            g_cvfsCalls.fetch_add(1u, std::memory_order_relaxed);
            g_cvfsLastNsct.store(nsct, std::memory_order_relaxed);
            g_cvfsLastBuf.store(buf, std::memory_order_relaxed);

            if (a0 != 0u) { g_cvfsObj.store(a0, std::memory_order_relaxed); }

            // Same order as the guest's own guards, so the counters partition
            // the calls instead of overlapping: the first one that fires is the
            // one that actually rejected this call.
            if (a0 == 0u)
            {
                g_cvfsBadObj.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            if (static_cast<int32_t>(nsct) < 0)
            {
                g_cvfsNsctNeg.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            if (buf == 0u)
            {
                g_cvfsBufNull.fetch_add(1u, std::memory_order_relaxed);
                return;
            }

            const uint32_t st = sofdecRead8(rdram, a0 + 2u);
            if (st < 8u) { g_cvfsStatIn[st].fetch_add(1u, std::memory_order_relaxed); }
            else         { g_cvfsStatInOther.fetch_add(1u, std::memory_order_relaxed); }

            if (st == 2u)        { g_cvfsStat2.fetch_add(1u, std::memory_order_relaxed); }
            else if (nsct == 0u) { g_cvfsNsct0.fetch_add(1u, std::memory_order_relaxed); }
            else                 { g_cvfsPassable.fetch_add(1u, std::memory_order_relaxed); }
            return;
        }

        if (kSofdecSites[site].addr == 0x0011D510u)
        {
            // Read the guard BEFORE the original runs: afterwards it is 0
            // again on every healthy call, which would make the histogram say
            // "always idle" no matter how badly it had been stranded.
            const uint32_t g = sofdecRead32(rdram, kCriGuard);
            if (g < 16u) { g_criEnt[g].fetch_add(1u, std::memory_order_relaxed); }
            else         { g_criEntOther.fetch_add(1u, std::memory_order_relaxed); }
            if (g != 0u) { g_criBail.fetch_add(1u, std::memory_order_relaxed); }
            return;
        }

        if (kSofdecSites[site].addr == 0x003e2ff0u)
        {
            g_st4bA0.store(a0, std::memory_order_relaxed);

            const uint32_t sub = sofdecRead32(rdram, a0 + 48u);
            if (sub < 16u) { g_st4bSub[sub].fetch_add(1u, std::memory_order_relaxed); }
            else           { g_st4bSubOther.fetch_add(1u, std::memory_order_relaxed); }

            const uint32_t accBits = sofdecRead32(rdram, a0 + 44u);
            const uint32_t prevBits =
                g_st4bAccLast.exchange(accBits, std::memory_order_relaxed);
            if (g_st4bAccFirst.load(std::memory_order_relaxed) == 0xFFFFFFFFu)
            {
                g_st4bAccFirst.store(accBits, std::memory_order_relaxed);
            }
            const float acc = sofdecF32(accBits);
            // Growth is counted per call, not inferred from first-vs-last: an
            // accumulator that is reset every cycle looks identical to a frozen
            // one when it is only sampled twice
            // ([[feedback_probe_the_final_value]]).
            if (prevBits != 0xFFFFFFFFu && acc > sofdecF32(prevBits))
            {
                g_st4bAccUp.fetch_add(1u, std::memory_order_relaxed);
            }
            if (acc > sofdecF32(g_st4bAccMax.load(std::memory_order_relaxed)))
            {
                g_st4bAccMax.store(accBits, std::memory_order_relaxed);
            }
            if (acc > 83.0f)
            {
                g_st4bOver83.fetch_add(1u, std::memory_order_relaxed);
            }

            // Pointer first, then the deref, so a null or wild clock reports as
            // clkB=0 rather than as a confident dt of zero.
            const uint32_t clk = sofdecRead32(rdram, a0 + 40u);
            g_st4bClk.store(clk, std::memory_order_relaxed);
            g_st4bDtBits.store(sofdecAddrOk(clk) ? sofdecRead32(rdram, clk + 4u)
                                                 : 0xFFFFFFFFu,
                               std::memory_order_relaxed);

            g_st4bW52.store(sofdecRead32(rdram, a0 + 52u), std::memory_order_relaxed);
            return;
        }

        if (kSofdecSites[site].addr == 0x00420260u)
        {
            g_st4A0.store(a0, std::memory_order_relaxed);

            const uint32_t sub = sofdecRead32(rdram, a0 + 164u);
            if (sub < 16u)
            {
                g_st4Sub[sub].fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                g_st4SubOther.fetch_add(1u, std::memory_order_relaxed);
            }
            if (g_st4SubLast.exchange(sub, std::memory_order_relaxed) != sub)
            {
                g_st4SubChanged.fetch_add(1u, std::memory_order_relaxed);
            }

            const uint32_t accBits = sofdecRead32(rdram, a0 + 172u);
            const uint32_t prevBits =
                g_st4AccLast.exchange(accBits, std::memory_order_relaxed);
            if (g_st4AccFirst.load(std::memory_order_relaxed) == 0xFFFFFFFFu)
            {
                g_st4AccFirst.store(accBits, std::memory_order_relaxed);
            }
            const float acc = sofdecF32(accBits);
            if (prevBits != 0xFFFFFFFFu && acc > sofdecF32(prevBits))
            {
                g_st4AccUp.fetch_add(1u, std::memory_order_relaxed);
            }
            if (acc > sofdecF32(g_st4AccMax.load(std::memory_order_relaxed)))
            {
                g_st4AccMax.store(accBits, std::memory_order_relaxed);
            }

            // The frame delta is one indirection out, through a0+40. Read the
            // pointer first and report it, so a null clock shows up as clk=0
            // rather than as a confident dt of zero.
            const uint32_t clk = sofdecRead32(rdram, a0 + 40u);
            g_st4Clk.store(clk, std::memory_order_relaxed);
            g_st4DtBits.store(sofdecAddrOk(clk) ? sofdecRead32(rdram, clk + 4u)
                                                : 0xFFFFFFFFu,
                              std::memory_order_relaxed);

            g_st4B160.store(sofdecRead8(rdram, a0 + 160u), std::memory_order_relaxed);
            g_st4B161.store(sofdecRead8(rdram, a0 + 161u), std::memory_order_relaxed);
            g_st4B162.store(sofdecRead8(rdram, a0 + 162u), std::memory_order_relaxed);
            return;
        }

        // 0x115318: lw $v0,192($a0) / lui $v0,0x44 / lw $v1,-14008($v0) / jalr $v1
        // Run 65 emitted exactly one [guest-branch:missing-target] here with
        // target=0x0, so that global slot held a null pointer. Recompute the
        // address the same way the guest does rather than pasting 0x43c948.
        if (kSofdecSites[site].addr == 0x00115318u)
        {
            const uint32_t slot = (0x44u << 16) - 14008u;
            g_nullcbSlotAddr.store(slot, std::memory_order_relaxed);
            g_nullcbSlot.store(sofdecRead32(rdram, slot), std::memory_order_relaxed);
            g_nullcbA0C0.store(sofdecRead32(rdram, a0 + 192u), std::memory_order_relaxed);
        }
    }

    void sofdecCapturePost(size_t site, const uint8_t *rdram, R5900Context *ctx)
    {
        if (rdram == nullptr || ctx == nullptr)
        {
            return;
        }
        const bool ret1 = (GPR_U32(ctx, 2) != 0u);
        if (kSofdecSites[site].addr == 0x00420260u)
        {
            if (ret1) { g_st4Ret1.fetch_add(1u, std::memory_order_relaxed); }
            g_st4SubAfter.store(
                sofdecRead32(rdram, g_st4A0.load(std::memory_order_relaxed) + 164u),
                std::memory_order_relaxed);
        }
        else if (kSofdecSites[site].addr == 0x00131190u)
        {
            if (ret1) { g_cvfsRetNz.fetch_add(1u, std::memory_order_relaxed); }
            g_cvfsRet.store(GPR_U32(ctx, 2), std::memory_order_relaxed);
            g_cvfsStatOut.store(
                sofdecRead8(rdram, g_cvfsObj.load(std::memory_order_relaxed) + 2u),
                std::memory_order_relaxed);
            // devtype AFTER the call is the ONLY observable proof that
            // sub_12E7B0 accepted the request, because 12E7B0 is called
            // directly rather than dispatched and cannot be wrapped.
            const uint32_t dev = sofdecRead8(rdram, kCriSrdObj + 1u);
            g_cvfsDevAfter.store(dev, std::memory_order_relaxed);
            if (dev != 0u) { g_cvfsArmed.fetch_add(1u, std::memory_order_relaxed); }
        }
        else if (kSofdecSites[site].addr == 0x003e2ff0u)
        {
            if (ret1) { g_st4bRet1.fetch_add(1u, std::memory_order_relaxed); }
            g_st4bSubAfter.store(
                sofdecRead32(rdram, g_st4bA0.load(std::memory_order_relaxed) + 48u),
                std::memory_order_relaxed);
        }
    }

    bool sofdecEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SOFDEC");
            return e == nullptr || (e[0] != '\0' && e[0] != '0');
        }();
        return on;
    }

    void sofdecStatLine(const char *why)
    {
        std::ostringstream oss;
        oss << "[sofdec:stat] why=" << why;
        for (size_t i = 0; i < kSofdecSiteCount; ++i)
        {
            oss << " " << kSofdecSites[i].tag << "="
                << std::dec << g_sofdecCalls[i].load(std::memory_order_relaxed)
                << "/" << (g_sofdecOrig[i] != nullptr ? 1 : 0);
        }
        oss << std::hex;
        for (size_t i = 0; i < kSofdecSiteCount; ++i)
        {
            const uint64_t n = g_sofdecCalls[i].load(std::memory_order_relaxed);
            if (n == 0u)
            {
                continue;
            }
            oss << " " << kSofdecSites[i].tag
                << ":ra0=0x" << g_sofdecFirstRa[i].load(std::memory_order_relaxed)
                << ":a0=0x" << g_sofdecLastA0[i].load(std::memory_order_relaxed);
        }
        oss << std::dec << "\n";

        // Second line, separate tag: [sofdec:stat] answers "was the function
        // entered", [lstick:stat] answers "and did it then do anything".
        oss << "[lstick:stat] why=" << why
            << std::hex
            << " gateAddr=0x" << g_lstickGateAddr.load(std::memory_order_relaxed)
            << " gate=0x" << g_lstickGate.load(std::memory_order_relaxed)
            << " f8=0x" << g_lstickF8.load(std::memory_order_relaxed)
            << " b18=0x" << g_lstickB18.load(std::memory_order_relaxed)
            << " b19=0x" << g_lstickB19.load(std::memory_order_relaxed)
            << " w12=0x" << g_lstickW12.load(std::memory_order_relaxed)
            << " vt=0x" << g_lstickVt.load(std::memory_order_relaxed)
            << " vt38=0x" << g_lstickVt38.load(std::memory_order_relaxed)
            << " vt40=0x" << g_lstickVt40.load(std::memory_order_relaxed)
            << " vt44=0x" << g_lstickVt44.load(std::memory_order_relaxed)
            << " cbSlotAddr=0x" << g_nullcbSlotAddr.load(std::memory_order_relaxed)
            << " cbSlot=0x" << g_nullcbSlot.load(std::memory_order_relaxed)
            << " cbA0C0=0x" << g_nullcbA0C0.load(std::memory_order_relaxed)
            << std::dec << " st=";
        for (size_t i = 0; i < 16u; ++i)
        {
            const uint64_t n = g_lstickState[i].load(std::memory_order_relaxed);
            if (n != 0u)
            {
                oss << i << ":" << n << ",";
            }
        }
        oss << "other:" << g_lstickStateOther.load(std::memory_order_relaxed)
            << "\n";

        // Third line, separate tag again: [st4:stat] answers "and why does the
        // state-4 arm keep saying no".
        const uint32_t dtBits = g_st4DtBits.load(std::memory_order_relaxed);
        const uint32_t accFirst = g_st4AccFirst.load(std::memory_order_relaxed);
        const uint32_t accLast = g_st4AccLast.load(std::memory_order_relaxed);
        const uint32_t accMax = g_st4AccMax.load(std::memory_order_relaxed);
        oss << "[st4:stat] why=" << why
            << std::dec
            << " ret1=" << g_st4Ret1.load(std::memory_order_relaxed)
            << " accUp=" << g_st4AccUp.load(std::memory_order_relaxed)
            << " subChanged=" << g_st4SubChanged.load(std::memory_order_relaxed)
            << std::hex
            << " a0=0x" << g_st4A0.load(std::memory_order_relaxed)
            << " clk=0x" << g_st4Clk.load(std::memory_order_relaxed)
            << " b160=0x" << g_st4B160.load(std::memory_order_relaxed)
            << " b161=0x" << g_st4B161.load(std::memory_order_relaxed)
            << " b162=0x" << g_st4B162.load(std::memory_order_relaxed)
            << " subAfter=0x" << g_st4SubAfter.load(std::memory_order_relaxed)
            << " dt=0x" << dtBits
            << " accFirst=0x" << accFirst
            << " accLast=0x" << accLast
            << " accMax=0x" << accMax
            << std::dec
            << " dtF=" << sofdecF32(dtBits)
            << " accFirstF=" << sofdecF32(accFirst)
            << " accLastF=" << sofdecF32(accLast)
            << " accMaxF=" << sofdecF32(accMax)
            << " sub=";
        for (size_t i = 0; i < 16u; ++i)
        {
            const uint64_t n = g_st4Sub[i].load(std::memory_order_relaxed);
            if (n != 0u)
            {
                oss << i << ":" << n << ",";
            }
        }
        oss << "other:" << g_st4SubOther.load(std::memory_order_relaxed)
            << "\n";

        // Fourth line: [st4b:stat] is the one aimed at the object that is
        // actually stuck. clkB and dt are the rival-reading pair -- a clkB that
        // differs from [st4:stat]'s clk means the two classes read different
        // clocks and the seconds-vs-milliseconds theory is dead on arrival.
        const uint32_t dtB = g_st4bDtBits.load(std::memory_order_relaxed);
        const uint32_t accBF = g_st4bAccFirst.load(std::memory_order_relaxed);
        const uint32_t accBL = g_st4bAccLast.load(std::memory_order_relaxed);
        const uint32_t accBM = g_st4bAccMax.load(std::memory_order_relaxed);
        oss << "[st4b:stat] why=" << why
            << std::dec
            << " ret1=" << g_st4bRet1.load(std::memory_order_relaxed)
            << " accUp=" << g_st4bAccUp.load(std::memory_order_relaxed)
            << " over83=" << g_st4bOver83.load(std::memory_order_relaxed)
            << std::hex
            << " a0=0x" << g_st4bA0.load(std::memory_order_relaxed)
            << " clkB=0x" << g_st4bClk.load(std::memory_order_relaxed)
            << " w52=0x" << g_st4bW52.load(std::memory_order_relaxed)
            << " subAfter=0x" << g_st4bSubAfter.load(std::memory_order_relaxed)
            << " dt=0x" << dtB
            << " accFirst=0x" << accBF
            << " accLast=0x" << accBL
            << " accMax=0x" << accBM
            << std::dec
            << " dtF=" << sofdecF32(dtB)
            << " accFirstF=" << sofdecF32(accBF)
            << " accLastF=" << sofdecF32(accBL)
            << " accMaxF=" << sofdecF32(accBM)
            << " needF=83 sub=";
        for (size_t i = 0; i < 16u; ++i)
        {
            const uint64_t n = g_st4bSub[i].load(std::memory_order_relaxed);
            if (n != 0u)
            {
                oss << i << ":" << n << ",";
            }
        }
        oss << "other:" << g_st4bSubOther.load(std::memory_order_relaxed)
            << "\n";

        // Fifth line: the streaming supply. Sampled from the cached rdram
        // rather than from a site hit, so calls=0 still prints real state.
        const uint8_t *cr = g_criRdram.load(std::memory_order_relaxed);
        oss << "[crisrv:stat] why=" << why
            << std::dec << " bail=" << g_criBail.load(std::memory_order_relaxed)
            << " rdram=" << (cr != nullptr ? 1 : 0)
            << std::hex
            << " guard=0x" << sofdecRead32(cr, kCriGuard)
            << " srdLock=0x" << sofdecRead32(cr, kCriSrdLock)
            << " devtype=0x" << sofdecRead8(cr, kCriSrdObj + 1u)
            << " st=0x" << sofdecRead8(cr, kCriSrdObj + 2u)
            << " pend=0x" << sofdecRead32(cr, kCriSrdObj + 4u)
            << " lsn=0x" << sofdecRead32(cr, kCriSrdObj + 8u)
            << " secs=0x" << sofdecRead32(cr, kCriSrdObj + 12u)
            << " buf=0x" << sofdecRead32(cr, kCriSrdObj + 16u)
            << " issued=0x" << sofdecRead32(cr, kCriIssued)
            << " done=0x" << sofdecRead32(cr, kCriDone)
            << " err=0x" << sofdecRead32(cr, kCriErr)
            << " a8=0x" << sofdecRead32(cr, kCriA8)
            << std::dec << " ent=";
        for (size_t i = 0; i < 16u; ++i)
        {
            const uint64_t n = g_criEnt[i].load(std::memory_order_relaxed);
            if (n != 0u)
            {
                oss << i << ":" << n << ",";
            }
        }
        oss << "other:" << g_criEntOther.load(std::memory_order_relaxed)
            << "\n";

        // Sixth line: the CVFS layer. Sampled from the cached rdram, and from a
        // latched object address that falls back to the one run 74 observed, so
        // calls=0 still prints real state instead of a row that cannot be told
        // apart from "never sampled".
        uint32_t cvo = g_cvfsObj.load(std::memory_order_relaxed);
        const bool cvoLatched = (cvo != 0u);
        if (!cvoLatched) { cvo = kCvfsObjFallback; }
        oss << "[cvfs:stat] why=" << why
            << std::dec
            << " calls=" << g_cvfsCalls.load(std::memory_order_relaxed)
            << " passable=" << g_cvfsPassable.load(std::memory_order_relaxed)
            << " retNz=" << g_cvfsRetNz.load(std::memory_order_relaxed)
            << " armed=" << g_cvfsArmed.load(std::memory_order_relaxed)
            << " badObj=" << g_cvfsBadObj.load(std::memory_order_relaxed)
            << " nsctNeg=" << g_cvfsNsctNeg.load(std::memory_order_relaxed)
            << " bufNull=" << g_cvfsBufNull.load(std::memory_order_relaxed)
            << " stat2=" << g_cvfsStat2.load(std::memory_order_relaxed)
            << " nsct0=" << g_cvfsNsct0.load(std::memory_order_relaxed)
            << " latched=" << (cvoLatched ? 1 : 0)
            << std::hex
            << " obj=0x" << cvo
            << " nsct=0x" << g_cvfsLastNsct.load(std::memory_order_relaxed)
            << " buf=0x" << g_cvfsLastBuf.load(std::memory_order_relaxed)
            << " ret=0x" << g_cvfsRet.load(std::memory_order_relaxed)
            << " statOut=0x" << g_cvfsStatOut.load(std::memory_order_relaxed)
            << " devAfter=0x" << g_cvfsDevAfter.load(std::memory_order_relaxed)
            << " o2=0x" << sofdecRead8(cr, cvo + 2u)
            << " oTotal=0x" << sofdecRead32(cr, cvo + 8u)
            << " oPos=0x" << sofdecRead32(cr, cvo + 12u)
            << " oNsct=0x" << sofdecRead32(cr, cvo + 16u)
            << " oLast=0x" << sofdecRead32(cr, cvo + 20u)
            << " oBuf=0x" << sofdecRead32(cr, cvo + 24u)
            << " oReq=0x" << sofdecRead32(cr, cvo + 28u)
            << " oBase=0x" << sofdecRead32(cr, cvo + 32u)
            << std::dec << " statIn=";
        for (size_t i = 0; i < 8u; ++i)
        {
            const uint64_t n = g_cvfsStatIn[i].load(std::memory_order_relaxed);
            if (n != 0u)
            {
                oss << i << ":" << n << ",";
            }
        }
        oss << "other:" << g_cvfsStatInOther.load(std::memory_order_relaxed)
            << "\n";

        std::cerr << oss.str();
    }

    template <size_t I>
    void sofdecWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_sofdecOrig[I];
        if (original == nullptr)
        {
            return;
        }
        if (sofdecEnabled() && ctx != nullptr)
        {
            const uint64_t seq = g_sofdecCalls[I].fetch_add(1u, std::memory_order_relaxed);
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t ra = GPR_U32(ctx, 31);
            g_sofdecLastA0[I].store(a0, std::memory_order_relaxed);
            g_criRdram.store(rdram, std::memory_order_relaxed);
            sofdecCapture(I, rdram, ctx);
            if (seq == 0u)
            {
                g_sofdecFirstRa[I].store(ra, std::memory_order_relaxed);
            }
            if (seq < kSofdecMaxEmit)
            {
                std::ostringstream oss;
                oss << "[sofdec] fn=" << kSofdecSites[I].tag
                    << " seq=" << std::dec << seq
                    << std::hex << " a0=0x" << a0 << " ra=0x" << ra
                    << std::dec << "\n";
                std::cerr << oss.str();
            }
            else if (seq == kSofdecMaxEmit)
            {
                std::cerr << "[cap] tag=sofdec." << kSofdecSites[I].tag
                          << " limit=" << std::dec << kSofdecMaxEmit
                          << " -- later calls are still COUNTED; read the"
                             " [sofdec:stat] totals, not these lines\n";
            }
        }
        original(rdram, ctx, runtime);
        if (sofdecEnabled() && ctx != nullptr)
        {
            sofdecCapturePost(I, rdram, ctx);
        }
    }

    template <size_t... Is>
    void sofdecInstall(PS2Runtime &runtime, std::index_sequence<Is...>)
    {
        ((g_sofdecOrig[Is] != nullptr
              ? (void)runtime.replaceFunction(kSofdecSites[Is].addr, &sofdecWrapper<Is>)
              : (void)0),
         ...);
    }

    void applySdbzSofdecProbe(PS2Runtime &runtime)
    {
        for (size_t i = 0; i < kSofdecSiteCount; ++i)
        {
            g_sofdecOrig[i] = runtime.lookupFunction(kSofdecSites[i].addr);
        }
        sofdecInstall(runtime, std::make_index_sequence<kSofdecSiteCount>{});

        // Printed before any guest code runs, so the /installed half of every
        // counter is on record even if the process dies early.
        sofdecStatLine("install");
        std::cerr << "[sofdec] enabled=" << (sofdecEnabled() ? 1 : 0)
                  << " statEvery=" << kSofdecStatPeriodSec << "s"
                  << " (PS2X_SOFDEC=0 disables)" << std::endl;

        if (!sofdecEnabled())
        {
            return;
        }
        static std::once_flag reporterOnce;
        std::call_once(reporterOnce, [] {
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(
                        std::chrono::seconds(kSofdecStatPeriodSec));
                    sofdecStatLine("periodic");
                }
            }).detach();
        });
    }

    // =====================================================================
    // STAGE 5.16 -- SRD COMPLETION PROBE  (run 67 follow-up, 2026-08-19)
    // =====================================================================
    // Run 67's coverage census killed the sleeping-CRI-thread hypothesis:
    // 0x130c48 ticked 727x, 0x1875e8 (sceCdRead) fired 354x, 0x186310 (the
    // SIF-RPC end_function) returned 354x, 0x187898 (sceCdGetError) ran 354x.
    // The data path is fully alive.  What never happens is the CVFS status
    // byte reaching 3, so the movie layer re-reads the identical LBN forever.
    //
    // sub_12EE20 (SRD state handler, object at 0x44D230) ends like this:
    //
    //     sync = sceCdSync(1);                 // 0x186bb0
    //     chk  = sub_12ECC0(obj);              // 0x12ecc0, the error check
    //     if (sync == 0) obj[2] = (chk == 1) ? 9 : 3;
    //
    // THREE rival outcomes fit every number we have, and they have three
    // completely different fixes:
    //
    //   A. sync != 0        -> obj[2] STAYS 2. Nothing completes, ever.
    //                          Fix lives in sceCdSync / dword_463274.
    //   B. sync == 0, chk 1 -> obj[2] = 9 (ERROR) -> DVCI obj[2] = 3 ->
    //                          movie layer retries the same LBN.
    //                          Fix lives in sceCdGetError / cdvdfsv status.
    //   C. sync == 0, chk 0 -> obj[2] = 3 (DONE). Reads genuinely succeed and
    //                          the bug is ABOVE CVFS, in the movie source
    //                          binding ([movie] objSrc=0x0 for all 294 ticks).
    //
    // Note B has a silent sub-case: sub_12ECC0's whole body is gated on
    // dword_44D27C, so if that flag is 0 the check returns 0 without ever
    // consulting sceCdGetError -- which would look identical to C from the
    // outside.  dword_44D27C is therefore logged explicitly; a probe that
    // reported "chk=0" without it would confidently describe the wrong thing
    // ([[feedback_probe_gate_on_shape_not_address]]).
    //
    // Every field that separates A/B/C is captured, plus a rival-reading
    // dump of the raw 8 bytes at the object base -- IDA calls byte_44D231 a
    // handler selector and byte_44D232 the status, and a one-byte error in
    // that reading would otherwise be invisible
    // ([[feedback_degenerate_result_convicts_the_probe]]).
    constexpr uint32_t kSrdStateFn = 0x0012EE20u;   // SRD state handler
    constexpr uint32_t kSrdErrChkFn = 0x0012ECC0u;  // SRD error check
    constexpr uint32_t kSrdGetErrFn = 0x00187898u;  // EE sceCdGetError
    constexpr uint32_t kSrdSyncFn = 0x00186BB0u;    // EE sceCdSync
    constexpr uint32_t kSrdDvciDoneFn = 0x00130B80u; // DVCI async completion

    constexpr uint32_t kSrdObjBase = 0x0044D230u;   // the one SRD object
    constexpr uint32_t kSrdGateWord = 0x0044D26Cu;  // outer tick gate
    constexpr uint32_t kSrdErrGate = 0x0044D27Cu;   // gates ALL of 12ecc0
    constexpr uint32_t kSrdIssued = 0x0044D288u;    // ++ per sceCdRead accepted
    constexpr uint32_t kSrdDone = 0x0044D28Cu;      // ++ per completion
    constexpr uint32_t kSrdSavedErr = 0x0044D2B4u;  // saved sceCdGetError

    constexpr uint64_t kSrdMaxDetail = 96u;         // per-site detail lines
    constexpr uint64_t kSrdStatEvery = 128u;        // 12ee20 heartbeat period

    PS2Runtime::RecompiledFunction g_srdStateOrig = nullptr;
    PS2Runtime::RecompiledFunction g_srdErrChkOrig = nullptr;
    PS2Runtime::RecompiledFunction g_srdGetErrOrig = nullptr;
    PS2Runtime::RecompiledFunction g_srdSyncOrig = nullptr;
    PS2Runtime::RecompiledFunction g_srdDvciDoneOrig = nullptr;

    std::atomic<uint64_t> g_srdStateCalls{0u};
    std::atomic<uint64_t> g_srdErrChkCalls{0u};
    std::atomic<uint64_t> g_srdGetErrCalls{0u};
    std::atomic<uint64_t> g_srdSyncCalls{0u};
    std::atomic<uint64_t> g_srdDvciDoneCalls{0u};

    // Nested-return relay: sceCdSync and 12ecc0 both run INSIDE 12ee20, so
    // their wrappers stash the value the outer wrapper needs to see.
    std::atomic<uint32_t> g_srdLastSync{0xFFFFFFFFu};
    std::atomic<uint32_t> g_srdLastChk{0xFFFFFFFFu};
    std::atomic<uint32_t> g_srdLastGetErr{0xFFFFFFFFu};

    // Histograms. All five sites are low-rate (354-1424 calls per run), so a
    // mutex here costs nothing and keeps the accounting exact.
    std::mutex g_srdHistMutex;
    uint64_t g_srdExitStat[16] = {};   // 12ee20 exit obj[2]
    uint64_t g_srdEntryStat[16] = {};  // 12ee20 entry obj[2]
    uint64_t g_srdChkHist[4] = {};     // 12ecc0 return (0/1/other)
    uint64_t g_srdSyncHist[4] = {};    // sceCdSync(1) return (0/1/other)
    uint64_t g_srdCvfsStat[16] = {};   // 130b80 switch selector
    // sceCdGetError value histogram -- open-valued, so (value,count) slots.
    uint32_t g_srdErrVal[16] = {};
    uint64_t g_srdErrCnt[16] = {};
    uint32_t g_srdErrSlots = 0u;
    uint64_t g_srdErrOverflow = 0u;

    bool srdProbeEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SRD");
            return e == nullptr || (e[0] != '\0' && e[0] != '0');
        }();
        return on;
    }

    inline uint32_t srdRd8(const uint8_t *rdram, uint32_t a)
    {
        return rdram[a & 0x01FFFFFFu];
    }

    inline uint32_t srdRd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (a & 0x01FFFFFCu), sizeof(v));
        return v;
    }

    void srdBump(uint64_t *table, size_t n, uint32_t v)
    {
        std::lock_guard<std::mutex> lock(g_srdHistMutex);
        table[(v < n) ? v : (n - 1u)] += 1u;
    }

    void srdBumpErrVal(uint32_t v)
    {
        std::lock_guard<std::mutex> lock(g_srdHistMutex);
        for (uint32_t i = 0u; i < g_srdErrSlots; ++i)
        {
            if (g_srdErrVal[i] == v)
            {
                g_srdErrCnt[i] += 1u;
                return;
            }
        }
        if (g_srdErrSlots < 16u)
        {
            g_srdErrVal[g_srdErrSlots] = v;
            g_srdErrCnt[g_srdErrSlots] = 1u;
            ++g_srdErrSlots;
            return;
        }
        ++g_srdErrOverflow;
    }

    void srdStatLine(const char *why)
    {
        std::ostringstream oss;
        oss << "[srd] stat why=" << why << std::dec
            << " 12ee20=" << g_srdStateCalls.load(std::memory_order_relaxed)
            << " 12ecc0=" << g_srdErrChkCalls.load(std::memory_order_relaxed)
            << " sync186bb0=" << g_srdSyncCalls.load(std::memory_order_relaxed)
            << " geterr187898=" << g_srdGetErrCalls.load(std::memory_order_relaxed)
            << " dvci130b80=" << g_srdDvciDoneCalls.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_srdHistMutex);
            oss << " entryStat=";
            for (uint32_t i = 0u; i < 16u; ++i)
                if (g_srdEntryStat[i]) oss << i << ":" << g_srdEntryStat[i] << ",";
            oss << " exitStat=";
            for (uint32_t i = 0u; i < 16u; ++i)
                if (g_srdExitStat[i]) oss << i << ":" << g_srdExitStat[i] << ",";
            oss << " chkRet=";
            for (uint32_t i = 0u; i < 4u; ++i)
                if (g_srdChkHist[i]) oss << i << ":" << g_srdChkHist[i] << ",";
            oss << " syncRet=";
            for (uint32_t i = 0u; i < 4u; ++i)
                if (g_srdSyncHist[i]) oss << i << ":" << g_srdSyncHist[i] << ",";
            oss << " cvfsStat=";
            for (uint32_t i = 0u; i < 16u; ++i)
                if (g_srdCvfsStat[i]) oss << i << ":" << g_srdCvfsStat[i] << ",";
            oss << std::hex << " cdErr=";
            for (uint32_t i = 0u; i < g_srdErrSlots; ++i)
                oss << "0x" << g_srdErrVal[i] << ":" << std::dec << g_srdErrCnt[i]
                    << std::hex << ",";
            oss << std::dec << " errOverflow=" << g_srdErrOverflow;
        }
        oss << "\n";
        std::cerr << oss.str();
    }

    // ---------------------------------------------------------------------
    // Time-driven histogram dump (stage 5.17).
    //
    // srdStatLine() previously reached the log by exactly two routes: a
    // heartbeat every 128 calls of 0x12EE20, and an std::atexit handler. Run 69
    // hit neither -- only 2 reads happened in 300 s (nowhere near 128), and the
    // launcher ends the run with TerminateProcess, which does not run atexit.
    // So the run that most needed the histograms is precisely the run that
    // printed none of them, and their absence read as "the probe never fired".
    // Driving the dump off the watchdog 1 s tick makes the numbers
    // unconditional: a killed run still leaves the last snapshot behind.
    //
    // Exported at file scope as ps2x_srd_stat_tick (end of this file), not from
    // here: MSVC gives a C-linkage function declared inside an unnamed namespace
    // internal linkage, so ps2_runtime.cpp's local declaration would not link.
    // ---------------------------------------------------------------------
    std::atomic<uint32_t> g_srdDumpTicks{0u};

    void srdStatTick()
    {
        if (!srdProbeEnabled())
        {
            return;
        }
        // Every 30 watchdog seconds: frequent enough that a kill loses at most
        // half a minute of resolution, rare enough not to bloat the log.
        const uint32_t n = g_srdDumpTicks.fetch_add(1u, std::memory_order_relaxed);
        if ((n % 30u) == 0u)
        {
            srdStatLine("periodic");
        }
    }

    // --- sceCdGetError (0x187898) --------------------------------------
    // THE decisive number. Its return is what sub_12ECC0 turns into the
    // ERROR-vs-DONE verdict, and it has never been measured.
    void srdGetErrWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_srdGetErrOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!srdProbeEnabled())
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_srdGetErrCalls.fetch_add(1u, std::memory_order_relaxed);

        original(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        g_srdLastGetErr.store(ret, std::memory_order_relaxed);
        srdBumpErrVal(ret);

        if (seq < kSrdMaxDetail)
        {
            std::ostringstream oss;
            oss << "[srd] fn=187898 sceCdGetError seq=" << std::dec << seq
                << std::hex << " ra=0x" << ra << " ret=0x" << ret
                << " (0 or 0xffffffff => no error; 0x20 => tolerated;"
                   " anything else => SRD calls it a DRIVE ERROR)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSrdMaxDetail)
        {
            std::cerr << "[cap] tag=srd.187898 limit=" << std::dec << kSrdMaxDetail
                      << " -- detail lines stop, histogram keeps counting\n";
        }
    }

    // --- sceCdSync (0x186BB0) ------------------------------------------
    // Only the blocking-poll form (a0 != 0) matters: sub_12EE20 gates the
    // whole completion on `if (!sync)`. A nonzero return here is rival
    // outcome A -- the read never completes and neither 9 nor 3 is written.
    void srdSyncWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_srdSyncOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!srdProbeEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_srdSyncCalls.fetch_add(1u, std::memory_order_relaxed);

        original(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        if (a0 != 0u)
        {
            g_srdLastSync.store(ret, std::memory_order_relaxed);
            srdBump(g_srdSyncHist, 4u, ret);
        }

        if (seq < kSrdMaxDetail)
        {
            std::ostringstream oss;
            oss << "[srd] fn=186bb0 sceCdSync seq=" << std::dec << seq
                << std::hex << " a0=0x" << a0 << " ra=0x" << ra
                << " ret=0x" << ret
                << " busyFlag463274=0x" << srdRd32(rdram, 0x00463274u) << "\n";
            std::cerr << oss.str();
        }
        else if (seq == kSrdMaxDetail)
        {
            std::cerr << "[cap] tag=srd.186bb0 limit=" << std::dec << kSrdMaxDetail
                      << " -- detail lines stop, histogram keeps counting\n";
        }
    }

    // --- sub_12ECC0, the SRD error check --------------------------------
    // Returns 1 => "drive error" => status 9. Returns 0 => status 3.
    // dword_44D27C is captured because a 0 there short-circuits the whole
    // body: chk=0 with gate=0 means "never asked", not "asked and it was fine".
    void srdErrChkWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_srdErrChkOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!srdProbeEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint32_t gate = srdRd32(rdram, kSrdErrGate);
        const uint32_t preStat = srdRd8(rdram, a0 + 2u);
        const uint64_t seq = g_srdErrChkCalls.fetch_add(1u, std::memory_order_relaxed);

        original(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        g_srdLastChk.store(ret, std::memory_order_relaxed);
        srdBump(g_srdChkHist, 4u, ret);

        if (seq < kSrdMaxDetail)
        {
            std::ostringstream oss;
            oss << "[srd] fn=12ecc0 errchk seq=" << std::dec << seq
                << std::hex << " a0=0x" << a0 << " ra=0x" << ra
                << " gate44d27c=0x" << gate
                << " preStat=0x" << preStat
                << " ret=0x" << ret
                << " saved44d2b4=0x" << srdRd32(rdram, kSrdSavedErr)
                << " objErr=0x" << srdRd32(rdram, a0 + 52u)
                << " (ret=1 => status 9 ERROR; ret=0 => status 3 DONE;"
                   " gate=0 => body skipped entirely)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSrdMaxDetail)
        {
            std::cerr << "[cap] tag=srd.12ecc0 limit=" << std::dec << kSrdMaxDetail
                      << " -- detail lines stop, histogram keeps counting\n";
        }
    }

    // --- sub_12EE20, the SRD state handler ------------------------------
    // The adjudicator. Everything the A/B/C decision needs lands on one line.
    void srdStateWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_srdStateOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!srdProbeEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_srdStateCalls.fetch_add(1u, std::memory_order_relaxed);

        const uint32_t preStat = srdRd8(rdram, obj + 2u);
        const uint32_t preReq = srdRd32(rdram, obj + 4u);
        const uint32_t lbn = srdRd32(rdram, obj + 8u);
        const uint32_t nsec = srdRd32(rdram, obj + 12u);
        const uint32_t buf = srdRd32(rdram, obj + 16u);
        const uint32_t issuedPre = srdRd32(rdram, kSrdIssued);
        const uint32_t donePre = srdRd32(rdram, kSrdDone);

        // Clear the relays so a stale value from a previous tick can never be
        // reported as this tick's ([[feedback_probe_the_final_value]]).
        g_srdLastSync.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_srdLastChk.store(0xFFFFFFFFu, std::memory_order_relaxed);

        original(rdram, ctx, runtime);

        const uint32_t postStat = srdRd8(rdram, obj + 2u);
        srdBump(g_srdEntryStat, 16u, preStat);
        srdBump(g_srdExitStat, 16u, postStat);

        const bool interesting = (preStat != postStat) || (preStat == 1u) || (preStat == 2u);
        if (seq < kSrdMaxDetail && interesting)
        {
            const uint32_t sync = g_srdLastSync.load(std::memory_order_relaxed);
            const uint32_t chk = g_srdLastChk.load(std::memory_order_relaxed);
            std::ostringstream oss;
            oss << "[srd] fn=12ee20 seq=" << std::dec << seq
                << std::hex << " obj=0x" << obj << " ra=0x" << ra
                << " preStat=0x" << preStat << " postStat=0x" << postStat
                << " req=0x" << preReq
                << " lbn=0x" << lbn << " nsec=0x" << nsec << " buf=0x" << buf
                << " sync=0x" << sync << " chk=0x" << chk
                << " cdErr=0x" << g_srdLastGetErr.load(std::memory_order_relaxed)
                << " objErr=0x" << srdRd32(rdram, obj + 52u)
                << std::dec
                << " issued=" << issuedPre << "->" << srdRd32(rdram, kSrdIssued)
                << " done=" << donePre << "->" << srdRd32(rdram, kSrdDone)
                << std::hex
                << " gate44d27c=0x" << srdRd32(rdram, kSrdErrGate)
                << " tickGate44d26c=0x" << srdRd32(rdram, kSrdGateWord);
            // Rival reading: IDA calls +1 the handler selector and +2 the
            // status. Dump the raw base bytes so a one-off in that reading is
            // visible instead of silently reframing every field above.
            oss << " raw44d230=";
            for (uint32_t i = 0u; i < 8u; ++i)
                oss << (i ? "." : "") << srdRd8(rdram, kSrdObjBase + i);
            oss << "\n";
            std::cerr << oss.str();
        }
        else if (seq == kSrdMaxDetail)
        {
            std::cerr << "[cap] tag=srd.12ee20 limit=" << std::dec << kSrdMaxDetail
                      << " -- detail lines stop, histograms keep counting\n";
        }

        if (((seq + 1u) % kSrdStatEvery) == 0u)
        {
            srdStatLine("heartbeat");
        }
    }

    // --- sub_130B80, the DVCI async completion --------------------------
    // Consumes the CVFS status the SRD produced: 3 => DONE, 9 => ERROR,
    // 0 => still running. Wrapping it turns the SRD verdict into the value
    // the movie layer actually sees, closing the chain in one run instead of
    // two.
    void srdDvciDoneWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_srdDvciDoneOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!srdProbeEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint64_t seq = g_srdDvciDoneCalls.fetch_add(1u, std::memory_order_relaxed);

        const uint32_t handle = srdRd32(rdram, obj + 28u);
        const uint32_t hStat = (handle != 0u && handle < 0x02000000u)
                                   ? srdRd8(rdram, handle + 2u)
                                   : 0xFFu;
        const uint32_t preObj = srdRd8(rdram, obj + 2u);

        original(rdram, ctx, runtime);

        const uint32_t postObj = srdRd8(rdram, obj + 2u);
        if (handle != 0u)
        {
            srdBump(g_srdCvfsStat, 16u, hStat);
        }

        if (seq < kSrdMaxDetail && handle != 0u)
        {
            std::ostringstream oss;
            oss << "[srd] fn=130b80 dvcidone seq=" << std::dec << seq
                << std::hex << " obj=0x" << obj << " ra=0x" << ra
                << " handle=0x" << handle << " cvfsStat=0x" << hStat
                << " preObj2=0x" << preObj << " postObj2=0x" << postObj
                << " (cvfs 3 => obj 1 DONE; cvfs 9 => obj 3 ERROR ->"
                   " movie layer retries the same LBN)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSrdMaxDetail)
        {
            std::cerr << "[cap] tag=srd.130b80 limit=" << std::dec << kSrdMaxDetail
                      << " -- detail lines stop, histogram keeps counting\n";
        }
    }

    void applySdbzSrdProbe(PS2Runtime &runtime)
    {
        // Capture every original BEFORE installing anything: 12ee20 calls
        // both 186bb0 and 12ecc0, so a replaceFunction done between two
        // lookups would hand a later capture the wrapper instead of the body.
        g_srdStateOrig = runtime.lookupFunction(kSrdStateFn);
        g_srdErrChkOrig = runtime.lookupFunction(kSrdErrChkFn);
        g_srdGetErrOrig = runtime.lookupFunction(kSrdGetErrFn);
        g_srdSyncOrig = runtime.lookupFunction(kSrdSyncFn);
        g_srdDvciDoneOrig = runtime.lookupFunction(kSrdDvciDoneFn);

        if (g_srdStateOrig != nullptr)
            runtime.replaceFunction(kSrdStateFn, &srdStateWrapper);
        if (g_srdErrChkOrig != nullptr)
            runtime.replaceFunction(kSrdErrChkFn, &srdErrChkWrapper);
        if (g_srdGetErrOrig != nullptr)
            runtime.replaceFunction(kSrdGetErrFn, &srdGetErrWrapper);
        if (g_srdSyncOrig != nullptr)
            runtime.replaceFunction(kSrdSyncFn, &srdSyncWrapper);
        if (g_srdDvciDoneOrig != nullptr)
            runtime.replaceFunction(kSrdDvciDoneFn, &srdDvciDoneWrapper);

        // Report per address, always. Run 67's coverage measured every one of
        // these as dispatched (12ee20=707, 12ecc0=354, 187898=354, 186bb0=1424,
        // 130b80=708), so a 0 here is a broken install, not a quiet guest --
        // and it must be visible BEFORE the run, not inferred from silence
        // afterwards ([[feedback_degenerate_result_convicts_the_probe]]).
        std::cerr << "[srd] wrappers"
                  << " 12ee20=" << (g_srdStateOrig != nullptr ? 1 : 0)
                  << " 12ecc0=" << (g_srdErrChkOrig != nullptr ? 1 : 0)
                  << " 187898=" << (g_srdGetErrOrig != nullptr ? 1 : 0)
                  << " 186bb0=" << (g_srdSyncOrig != nullptr ? 1 : 0)
                  << " 130b80=" << (g_srdDvciDoneOrig != nullptr ? 1 : 0)
                  << " enabled=" << (srdProbeEnabled() ? 1 : 0)
                  << " (PS2X_SRD=0 disables)" << std::endl;

        // Final word regardless of caps: the histograms are the verdict, the
        // detail lines are only the narrative.
        static std::once_flag srdAtexitOnce;
        std::call_once(srdAtexitOnce,
                       [] { std::atexit([] { srdStatLine("shutdown"); }); });
    }

    void applySdbzMovieGate(PS2Runtime &runtime)
    {
        // Capture every original BEFORE installing any wrapper: these three
        // are on one call chain, so a replaceFunction done between two
        // lookups would hand a later capture the wrapper instead of the body.
        g_movieGateStateOrig = runtime.lookupFunction(kMovieGateStateFn);
        g_movieGateOpenOrig = runtime.lookupFunction(kMovieGateOpenFn);
        g_movieGateDvciOrig = runtime.lookupFunction(kMovieGateDvciFn);
        g_movieGateReqOuterOrig = runtime.lookupFunction(kMovieGateReqOuterFn);
        g_movieGateReqOrig = runtime.lookupFunction(kMovieGateReqFn);
        g_movieGatePumpOuterOrig = runtime.lookupFunction(kMovieGatePumpOuterFn);
        g_movieGatePumpOrig = runtime.lookupFunction(kMovieGatePumpFn);
        g_cdSearchThunkOrig = runtime.lookupFunction(kCdSearchThunkFn);
        g_cdSearchOrig = runtime.lookupFunction(kCdSearchFn);
        g_adxGateOuterOrig = runtime.lookupFunction(kAdxGateOuterFn);
        g_adxGateOrig = runtime.lookupFunction(kAdxGateFn);
        g_adxNeighbor9F8Orig = runtime.lookupFunction(kAdxNeighborFn9F8);
        g_adxNeighborBB0Orig = runtime.lookupFunction(kAdxNeighborFnBB0);
        g_adxNeighborCF0Orig = runtime.lookupFunction(kAdxNeighborFnCF0);

        if (g_movieGateReqOuterOrig != nullptr)
            runtime.replaceFunction(kMovieGateReqOuterFn, &movieGateReqOuterWrapper);
        if (g_movieGateReqOrig != nullptr)
            runtime.replaceFunction(kMovieGateReqFn, &movieGateReqWrapper);
        if (g_movieGatePumpOuterOrig != nullptr)
            runtime.replaceFunction(kMovieGatePumpOuterFn, &movieGatePumpOuterWrapper);
        if (g_movieGatePumpOrig != nullptr)
            runtime.replaceFunction(kMovieGatePumpFn, &movieGatePumpWrapper);

        if (g_movieGateStateOrig != nullptr)
            runtime.replaceFunction(kMovieGateStateFn, &movieGateStateWrapper);
        if (g_movieGateOpenOrig != nullptr)
            runtime.replaceFunction(kMovieGateOpenFn, &movieGateOpenWrapper);
        if (g_movieGateDvciOrig != nullptr)
            runtime.replaceFunction(kMovieGateDvciFn, &movieGateDvciWrapper);

        if (g_cdSearchThunkOrig != nullptr)
            runtime.replaceFunction(kCdSearchThunkFn, &cdSearchThunkWrapper);
        if (g_cdSearchOrig != nullptr)
            runtime.replaceFunction(kCdSearchFn, &cdSearchWrapper);
        if (g_adxGateOuterOrig != nullptr)
            runtime.replaceFunction(kAdxGateOuterFn, &adxGateOuterWrapper);
        if (g_adxGateOrig != nullptr)
            runtime.replaceFunction(kAdxGateFn, &adxGateWrapper);
        if (g_adxNeighbor9F8Orig != nullptr)
            runtime.replaceFunction(kAdxNeighborFn9F8, &adxNeighbor9F8Wrapper);
        if (g_adxNeighborBB0Orig != nullptr)
            runtime.replaceFunction(kAdxNeighborFnBB0, &adxNeighborBB0Wrapper);
        if (g_adxNeighborCF0Orig != nullptr)
            runtime.replaceFunction(kAdxNeighborFnCF0, &adxNeighborCF0Wrapper);

        // Always report, per address. "installed 0" for any one of these
        // means that address is not in the dense table, which invalidates
        // the whole decision table above BEFORE the run rather than after --
        // absence of records would otherwise read as "the call never happened"
        // ([[feedback_probe_gate_on_shape_not_address]]).
        std::cerr << "[moviegate] wrappers"
                  << " 125898=" << (g_movieGateStateOrig != nullptr ? 1 : 0)
                  << " 12cc20=" << (g_movieGateOpenOrig != nullptr ? 1 : 0)
                  << " 130ef0=" << (g_movieGateDvciOrig != nullptr ? 1 : 0)
                  << " 124dc8=" << (g_movieGateReqOuterOrig != nullptr ? 1 : 0)
                  << " 124e38=" << (g_movieGateReqOrig != nullptr ? 1 : 0)
                  << " 126388=" << (g_movieGatePumpOuterOrig != nullptr ? 1 : 0)
                  << " 1263b8=" << (g_movieGatePumpOrig != nullptr ? 1 : 0)
                  << " 130ad8=" << (g_cdSearchThunkOrig != nullptr ? 1 : 0)
                  << " 1866a0=" << (g_cdSearchOrig != nullptr ? 1 : 0)
                  << " 11cbf0=" << (g_adxGateOuterOrig != nullptr ? 1 : 0)
                  << " 11cc28=" << (g_adxGateOrig != nullptr ? 1 : 0)
                  << " 11c9f8=" << (g_adxNeighbor9F8Orig != nullptr ? 1 : 0)
                  << " 11cbb0=" << (g_adxNeighborBB0Orig != nullptr ? 1 : 0)
                  << " 11ccf0=" << (g_adxNeighborCF0Orig != nullptr ? 1 : 0)
                  << " enabled=" << (movieGateEnabled() ? 1 : 0)
                  << " cdsearch=" << (cdSearchEnabled() ? 1 : 0)
                  << " adxgate=" << (adxGateEnabled() ? 1 : 0)
                  << " (PS2X_MOVIEGATE=0 disables all, PS2X_CDSEARCH=0 just"
                     " the lookup probe, PS2X_ADXGATE=0 just the status-gate"
                     " probe)" << std::endl;
    }

    // =================================================================
    // Stage 5.17 -- SIF software-register (sreg) probe. MEASUREMENT ONLY.
    //
    // The static lane (2026-08-22) resolved the logo/movie stall to one
    // instruction pair. GameUpdate 0x421EA0 calls the app manager's vtable
    // slot 5; the three clone logo states (CAppLogoAtari 0x420E70,
    // CAppLogoOkrtron 0x4216E0, unnamed 0x3E2E80) each run a per-frame step
    // machine on this->step at offset 48:
    //
    //   step 0   : if (sceSifGetReg(13) & 0x201) return;   <- only blocking step
    //   step 1   : [0x500724].b = 1 ; -> 0xb
    //   step 0xb : -> 0xc      step 0xc : -> 0xd
    //   step 0xd : jal 0x113D30  ->  [0x500728] = 1        <- ONLY path to CRI
    //   step 0xe : -> 0xf      step 0xf : step = 0, state complete
    //
    // Steps 1..0xe advance unconditionally, so a state that lingers can only
    // be sitting on step 0. Run 87 shows both states executing (t=119..133,
    // t=168..187) yet [0x500728] stays 0 for 41 samples afterwards.
    //
    // Two rival readings survive the static evidence and cannot be separated
    // without a measurement:
    //   A. sregs[13] & 0x201 is nonzero => the gate blocks, step 0 never
    //      advances, and the fix is the sreg mirror (ps2_iop_sifSetEeSreg
    //      writes a host-side std::unordered_map the guest cannot read).
    //   B. sregs[13] is 0 (it is BSS, and no EE code writes index 13), the
    //      gate PASSES, and the stall is somewhere else entirely.
    // This probe measures the value instead of arguing about it.
    //
    // Design notes that are load-bearing, not decoration:
    //  * The sregs base is READ FROM THE DESCRIPTOR ([0x5616F4], written by
    //    sceSifInitCmd 0x177B00), never assumed. The hardcoded 0x561880 is
    //    reported alongside it, and the descriptor's own signature words are
    //    printed, so a wrong base assumption is visible in the output rather
    //    than silently producing confident garbage
    //    ([[feedback_probe_gate_on_shape_not_address]]).
    //  * this->step is sampled at the state's ENTRY, not by a 1 Hz sampler --
    //    a sampler cannot tell "never reached step 0xd" from "reached it
    //    between two ticks" ([[project_ps2x_order_trace]]). The step
    //    histogram is the verdict; the transition lines are the narrative.
    //  * Every detail stream is capped and PRINTS ITS CAP, because a
    //    saturated probe reads exactly like "it never happened"
    //    ([[feedback_capped_probes_false_negatives]]).
    //  * The periodic line is driven off the watchdog tick (via
    //    ps2x_srd_stat_tick at the end of this file), so a run killed by
    //    -RunSeconds still leaves the last snapshot on disk.
    //
    // NOTHING HERE CHANGES GUEST BEHAVIOUR. Every wrapper calls the original.
    // =================================================================
    constexpr uint32_t kSregGetFn = 0x177AB8u;   // sceSifGetReg
    constexpr uint32_t kSregSetFn = 0x177AD0u;   // sceSifSetReg
    constexpr uint32_t kSregStateAtariFn = 0x420E70u;    // CAppLogoAtari slot 0
    constexpr uint32_t kSregStateOkrtronFn = 0x4216E0u;  // CAppLogoOkrtron slot 0
    constexpr uint32_t kSregStateCloneFn = 0x3E2E80u;    // unnamed clone slot 0

    // sceSifInitCmd (0x177B00) builds this descriptor. +28 holds the sregs
    // base; +12 the 32-entry handler table; +16 the handler count (0x20).
    constexpr uint32_t kSregDescAddr = 0x5616D8u;
    constexpr uint32_t kSregDescBaseField = kSregDescAddr + 28u;  // 0x5616F4
    constexpr uint32_t kSregFallbackBase = 0x561880u;             // what 0x177B00 writes
    constexpr uint32_t kSregGateIdx = 13u;
    constexpr uint32_t kSregGateMask = 0x201u;
    constexpr uint32_t kCriPrevAddr = 0x500724u;  // step 1 sets this byte
    constexpr uint32_t kCriGateAddr = 0x500728u;  // step 0xd sets this word
    constexpr uint64_t kSregMaxDetail = 64u;

    PS2Runtime::RecompiledFunction g_sregGetOrig = nullptr;
    PS2Runtime::RecompiledFunction g_sregSetOrig = nullptr;
    PS2Runtime::RecompiledFunction g_sregStateOrig[3] = {nullptr, nullptr, nullptr};
    PS2Runtime *g_sregRuntime = nullptr;

    std::mutex g_sregMutex;
    uint64_t g_sregGetIdxHist[32] = {};
    uint64_t g_sregSetIdxHist[32] = {};
    // Open-valued (value,count) slots for what index 13 actually returns.
    uint32_t g_sregGate13Val[8] = {};
    uint64_t g_sregGate13Cnt[8] = {};
    uint32_t g_sregGate13Slots = 0u;
    uint64_t g_sregGate13Overflow = 0u;
    std::atomic<uint64_t> g_sregGate13Blocking{0u};  // (v & 0x201) != 0 -> state blocks
    std::atomic<uint64_t> g_sregGate13Passing{0u};   // (v & 0x201) == 0 -> state advances
    std::atomic<uint64_t> g_sregGetCalls{0u};
    std::atomic<uint64_t> g_sregSetCalls{0u};
    std::atomic<uint64_t> g_sregSetDetail{0u};
    std::atomic<uint64_t> g_sregGateDetail{0u};
    std::atomic<uint64_t> g_sregStepDetail{0u};
    std::atomic<uint32_t> g_sregGateLastVerdict{0xFFFFFFFFu};

    // Plain scalars, all guarded by g_sregMutex. Every writer already takes
    // that lock for the step histogram, so there is nothing to gain from
    // atomics here and one less way for the build to fail.
    struct SregStateStat
    {
        const char *name;
        uint32_t addr;
        uint64_t calls;
        uint32_t lastObj;
        uint32_t lastStep;
        uint32_t maxStep;
        uint64_t stepHist[32];
    };
    SregStateStat g_sregStates[3] = {
        {"AtariLogo", kSregStateAtariFn, 0u, 0u, 0xFFFFFFFFu, 0u, {}},
        {"OkrtronLogo", kSregStateOkrtronFn, 0u, 0u, 0xFFFFFFFFu, 0u, {}},
        {"CloneLogo", kSregStateCloneFn, 0u, 0u, 0xFFFFFFFFu, 0u, {}},
    };

    bool sregProbeEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_SREG");
            return e == nullptr || (e[0] != 0 && e[0] != '0');
        }();
        return on;
    }

    inline uint32_t sregRd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (a & 0x01FFFFFCu), sizeof(v));
        return v;
    }

    inline uint32_t sregRd8(const uint8_t *rdram, uint32_t a)
    {
        return rdram[a & 0x01FFFFFFu];
    }

    void sregBumpGate13(uint32_t v)
    {
        std::lock_guard<std::mutex> lock(g_sregMutex);
        for (uint32_t i = 0u; i < g_sregGate13Slots; ++i)
        {
            if (g_sregGate13Val[i] == v)
            {
                g_sregGate13Cnt[i] += 1u;
                return;
            }
        }
        if (g_sregGate13Slots < 8u)
        {
            g_sregGate13Val[g_sregGate13Slots] = v;
            g_sregGate13Cnt[g_sregGate13Slots] = 1u;
            ++g_sregGate13Slots;
            return;
        }
        ++g_sregGate13Overflow;
    }

    // --- sceSifGetReg (0x177AB8) ---------------------------------------
    // return *(u32*)(base + idx*4). Index 13 IS the gate the logo step machine
    // spins on; index 12 is the only one EE code ever writes.
    void sregGetWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_sregGetOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!sregProbeEnabled())
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t idx = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);

        original(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        g_sregGetCalls.fetch_add(1u, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_sregMutex);
            g_sregGetIdxHist[idx & 31u] += 1u;
        }
        if (idx != kSregGateIdx)
        {
            return;
        }

        sregBumpGate13(ret);
        const bool blocking = (ret & kSregGateMask) != 0u;
        if (blocking)
            g_sregGate13Blocking.fetch_add(1u, std::memory_order_relaxed);
        else
            g_sregGate13Passing.fetch_add(1u, std::memory_order_relaxed);

        // Detail only on a CHANGE of the masked verdict: this runs ~3x per
        // frame from 0x1BB450, so an unconditional line would saturate the cap
        // in the first two seconds and bury the one transition that matters.
        const uint32_t verdict = blocking ? 1u : 0u;
        if (g_sregGateLastVerdict.exchange(verdict, std::memory_order_relaxed) == verdict)
        {
            return;
        }

        const uint64_t seq = g_sregGateDetail.fetch_add(1u, std::memory_order_relaxed);
        if (seq < kSregMaxDetail)
        {
            std::ostringstream oss;
            oss << "[sreg] gate idx=13 seq=" << std::dec << seq << std::hex
                << " ra=0x" << ra << " val=0x" << ret
                << " masked=0x" << (ret & kSregGateMask) << std::dec
                << " verdict=" << (blocking ? "BLOCK" : "PASS")
                << " (BLOCK => logo step 0 returns without advancing;"
                   " PASS => step 0 advances and the stall is NOT this gate)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSregMaxDetail)
        {
            std::cerr << "[cap] tag=sreg.gate limit=" << std::dec << kSregMaxDetail
                      << " -- transition lines stop, counters keep counting\n";
        }
    }

    // --- sceSifSetReg (0x177AD0) ---------------------------------------
    // Static analysis found exactly one EE caller (0x1BC5F0) and it only ever
    // passes index 12. If index 13 shows up here, that static claim was wrong
    // and the IOP-mirror theory is moot -- which is precisely why this exists.
    void sregSetWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_sregSetOrig;
        if (original == nullptr)
        {
            return;
        }
        if (!sregProbeEnabled())
        {
            original(rdram, ctx, runtime);
            return;
        }

        const uint32_t idx = GPR_U32(ctx, 4);
        const uint32_t val = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);

        original(rdram, ctx, runtime);

        g_sregSetCalls.fetch_add(1u, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_sregMutex);
            g_sregSetIdxHist[idx & 31u] += 1u;
        }

        const uint64_t seq = g_sregSetDetail.fetch_add(1u, std::memory_order_relaxed);
        if (seq < kSregMaxDetail)
        {
            std::ostringstream oss;
            oss << "[sreg] set seq=" << std::dec << seq
                << " idx=" << idx << std::hex
                << " val=0x" << val << " ra=0x" << ra << std::dec
                << " (static says EE only ever writes idx 12)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSregMaxDetail)
        {
            std::cerr << "[cap] tag=sreg.set limit=" << std::dec << kSregMaxDetail
                      << " -- detail lines stop, setIdx keeps counting\n";
        }
    }

    // --- the three logo step machines ----------------------------------
    // Sampled at entry, before the original runs, so the value is the step the
    // machine is ABOUT to execute. The histogram is the verdict: if step 0 is
    // the only bucket that ever fills for a state that ran 14 s, the gate is
    // confirmed; if 0xb/0xc appear but 0xd never does, something between them
    // is wrong instead and the gate theory dies with it.
    void sregStateWrapperN(uint32_t slot, uint8_t *rdram, R5900Context *ctx,
                           PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction original = g_sregStateOrig[slot];
        if (original == nullptr)
        {
            return;
        }
        if (!sregProbeEnabled() || rdram == nullptr)
        {
            original(rdram, ctx, runtime);
            return;
        }

        SregStateStat &st = g_sregStates[slot];
        const uint32_t obj = GPR_U32(ctx, 4);
        const bool objSane = (obj != 0u && obj < 0x02000000u);
        const uint32_t step = objSane ? sregRd32(rdram, obj + 48u) : 0xFFFFFFFFu;

        uint32_t prev = 0xFFFFFFFFu;
        {
            std::lock_guard<std::mutex> lock(g_sregMutex);
            st.calls += 1u;
            st.lastObj = obj;
            st.stepHist[step & 31u] += 1u;
            if (step != 0xFFFFFFFFu && step > st.maxStep)
            {
                st.maxStep = step;
            }
            prev = st.lastStep;
            st.lastStep = step;
        }

        original(rdram, ctx, runtime);

        if (prev == step)
        {
            return;  // same step as last frame -- nothing new to say
        }

        const uint32_t after = objSane ? sregRd32(rdram, obj + 48u) : 0xFFFFFFFFu;
        const uint64_t seq = g_sregStepDetail.fetch_add(1u, std::memory_order_relaxed);
        if (seq < kSregMaxDetail)
        {
            std::ostringstream oss;
            oss << "[sreg] step " << st.name << " seq=" << std::dec << seq
                << std::hex << " fn=0x" << st.addr << " obj=0x" << obj
                << " step=0x" << prev << "->0x" << step << " post=0x" << after
                << " cri@0x500728=0x" << sregRd32(rdram, kCriGateAddr)
                << " prev@0x500724=0x" << sregRd8(rdram, kCriPrevAddr) << std::dec
                << " (0xd is the ONLY step that sets 0x500728)\n";
            std::cerr << oss.str();
        }
        else if (seq == kSregMaxDetail)
        {
            std::cerr << "[cap] tag=sreg.step limit=" << std::dec << kSregMaxDetail
                      << " -- transition lines stop, stepHist keeps counting\n";
        }
    }

    void sregStateWrapper0(uint8_t *r, R5900Context *c, PS2Runtime *rt) { sregStateWrapperN(0u, r, c, rt); }
    void sregStateWrapper1(uint8_t *r, R5900Context *c, PS2Runtime *rt) { sregStateWrapperN(1u, r, c, rt); }
    void sregStateWrapper2(uint8_t *r, R5900Context *c, PS2Runtime *rt) { sregStateWrapperN(2u, r, c, rt); }

    // --- the periodic snapshot -----------------------------------------
    void sregStatLine(const char *why)
    {
        if (!sregProbeEnabled() || g_sregRuntime == nullptr)
        {
            return;
        }
        const uint8_t *rdram = g_sregRuntime->memory().getRDRAM();
        if (rdram == nullptr)
        {
            std::cerr << "[sreg] why=" << why << " rdram=null (snapshot skipped)\n";
            return;
        }

        // Read the base the game itself uses. If sceSifInitCmd never ran, this
        // is 0 and every sregs[] value below is meaningless -- which the line
        // says out loud instead of printing a plausible-looking dump.
        const uint32_t descBase = sregRd32(rdram, kSregDescBaseField);
        const uint32_t handlers = sregRd32(rdram, kSregDescAddr + 12u);
        const uint32_t nHandlers = sregRd32(rdram, kSregDescAddr + 16u);
        const bool descSane = (descBase != 0u && descBase < 0x02000000u);
        const uint32_t base = descSane ? descBase : kSregFallbackBase;

        std::ostringstream oss;
        oss << "[sreg] why=" << why << std::hex
            << " descBase@0x5616F4=0x" << descBase
            << " hTable=0x" << handlers << " nH=0x" << nHandlers
            << " fallback=0x" << kSregFallbackBase << std::dec
            << " baseMatch=" << (descBase == kSregFallbackBase ? 1 : 0)
            << " initCmdRan=" << (descBase != 0u ? 1 : 0)
            << std::hex << " usedBase=0x" << base << " sregsNZ=";
        for (uint32_t i = 0u; i < 32u; ++i)
        {
            const uint32_t v = sregRd32(rdram, base + i * 4u);
            if (v != 0u)
                oss << std::dec << i << ":0x" << std::hex << v << ",";
        }
        oss << " sreg13=0x" << sregRd32(rdram, base + kSregGateIdx * 4u);
        // Second opinion: if the two bases disagree, print index 13 from both
        // so the reader can see which one the game is really using.
        if (descSane && descBase != kSregFallbackBase)
        {
            oss << " sreg13@fallback=0x"
                << sregRd32(rdram, kSregFallbackBase + kSregGateIdx * 4u);
        }

        oss << " cri@0x500728=0x" << sregRd32(rdram, kCriGateAddr)
            << " prev@0x500724=0x" << sregRd8(rdram, kCriPrevAddr) << std::dec
            << " getCalls=" << g_sregGetCalls.load(std::memory_order_relaxed)
            << " setCalls=" << g_sregSetCalls.load(std::memory_order_relaxed)
            << " gate13block=" << g_sregGate13Blocking.load(std::memory_order_relaxed)
            << " gate13pass=" << g_sregGate13Passing.load(std::memory_order_relaxed)
            << " iopSetSreg=" << ps2x_iop_setsreg_calls();

        {
            std::lock_guard<std::mutex> lock(g_sregMutex);
            oss << " getIdx=";
            for (uint32_t i = 0u; i < 32u; ++i)
                if (g_sregGetIdxHist[i]) oss << i << ":" << g_sregGetIdxHist[i] << ",";
            oss << " setIdx=";
            for (uint32_t i = 0u; i < 32u; ++i)
                if (g_sregSetIdxHist[i]) oss << i << ":" << g_sregSetIdxHist[i] << ",";
            oss << std::hex << " gate13vals=";
            for (uint32_t i = 0u; i < g_sregGate13Slots; ++i)
                oss << "0x" << g_sregGate13Val[i] << ":" << std::dec
                    << g_sregGate13Cnt[i] << std::hex << ",";
            oss << std::dec << " gate13overflow=" << g_sregGate13Overflow;

            for (uint32_t s = 0u; s < 3u; ++s)
            {
                SregStateStat &st = g_sregStates[s];
                oss << " " << st.name << "{calls=" << st.calls << std::hex
                    << " obj=0x" << st.lastObj
                    << " step=0x" << st.lastStep
                    << " maxStep=0x" << st.maxStep
                    << " hist=";
                for (uint32_t i = 0u; i < 32u; ++i)
                    if (st.stepHist[i])
                        oss << "0x" << i << ":" << std::dec << st.stepHist[i]
                            << std::hex << ",";
                oss << std::dec << "}";
            }
        }
        oss << "\n";
        std::cerr << oss.str();
    }

    std::atomic<uint32_t> g_sregDumpTicks{0u};

    void sregStatTick()
    {
        if (!sregProbeEnabled())
        {
            return;
        }
        // Every 5 watchdog seconds. Denser than the SRD dump because the thing
        // being watched is a per-frame step machine, and a 30 s grid could sit
        // entirely inside one 14 s logo window and report nothing about it.
        const uint32_t n = g_sregDumpTicks.fetch_add(1u, std::memory_order_relaxed);
        if ((n % 5u) == 0u)
        {
            sregStatLine("periodic");
        }
    }

    void applySdbzSregProbe(PS2Runtime &runtime)
    {
        g_sregRuntime = &runtime;

        // Capture every original BEFORE installing anything.
        g_sregGetOrig = runtime.lookupFunction(kSregGetFn);
        g_sregSetOrig = runtime.lookupFunction(kSregSetFn);
        g_sregStateOrig[0] = runtime.lookupFunction(kSregStateAtariFn);
        g_sregStateOrig[1] = runtime.lookupFunction(kSregStateOkrtronFn);
        g_sregStateOrig[2] = runtime.lookupFunction(kSregStateCloneFn);

        if (g_sregGetOrig != nullptr)
            runtime.replaceFunction(kSregGetFn, &sregGetWrapper);
        if (g_sregSetOrig != nullptr)
            runtime.replaceFunction(kSregSetFn, &sregSetWrapper);
        if (g_sregStateOrig[0] != nullptr)
            runtime.replaceFunction(kSregStateAtariFn, &sregStateWrapper0);
        if (g_sregStateOrig[1] != nullptr)
            runtime.replaceFunction(kSregStateOkrtronFn, &sregStateWrapper1);
        if (g_sregStateOrig[2] != nullptr)
            runtime.replaceFunction(kSregStateCloneFn, &sregStateWrapper2);

        // Per-address install report, printed BEFORE the run. Run 87's watchdog
        // trace measured 0x177AB8 (755 hits), 0x420E70 and 0x4216E0 as
        // dispatched, so a 0 here is a broken install and not a quiet guest --
        // and that has to be visible up front rather than inferred from silence
        // afterwards ([[feedback_degenerate_result_convicts_the_probe]]).
        std::cerr << "[sreg] wrappers"
                  << " 177ab8=" << (g_sregGetOrig != nullptr ? 1 : 0)
                  << " 177ad0=" << (g_sregSetOrig != nullptr ? 1 : 0)
                  << " 420e70=" << (g_sregStateOrig[0] != nullptr ? 1 : 0)
                  << " 4216e0=" << (g_sregStateOrig[1] != nullptr ? 1 : 0)
                  << " 3e2e80=" << (g_sregStateOrig[2] != nullptr ? 1 : 0)
                  << " enabled=" << (sregProbeEnabled() ? 1 : 0)
                  << " (PS2X_SREG=0 disables)" << std::endl;

        static std::once_flag sregAtexitOnce;
        std::call_once(sregAtexitOnce,
                       [] { sregStatLine("install"); std::atexit([] { sregStatLine("shutdown"); }); });
    }

    PS2_REGISTER_GAME_OVERRIDE("RECVX sound-driver compat", "slus_201.84", 0u, 0u, &applyRecvxSoundDriverCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX DTX compat", "slus_201.84", 0u, 0u, &applyRecvxDtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("LotR sound RPC compat", "SLUS_205.78", 0u, 0u, &applyLotrSoundRpcCompat);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ kernel thunk fixes", "SLUS_214.42", 0u, 0u, &applySdbzKernelThunkFixes);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ loadfile signature-gate seed", "SLUS_214.42", 0u, 0u, &applySdbzLoadfileSeed);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ frame-trace measurement", "SLUS_214.42", 0u, 0u, &applySdbzFrameTrace);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ packet build-order probe", "SLUS_214.42", 0u, 0u, &applySdbzPacketOrder);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ movie-open gate probe", "SLUS_214.42", 0u, 0u, &applySdbzMovieGate);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ Sofdec per-frame driver probe", "SLUS_214.42", 0u, 0u, &applySdbzSofdecProbe);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ SRD completion probe", "SLUS_214.42", 0u, 0u, &applySdbzSrdProbe);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ SIF sreg + logo-step probe", "SLUS_214.42", 0u, 0u, &applySdbzSregProbe);
}

// File scope on purpose -- see srdStatTick() above. Mirrors the placement of
// ps2x_probe_kv near the top of this file.
extern "C" void ps2x_srd_stat_tick()
{
    srdStatTick();
    // Stage 5.17: the sreg snapshot rides the same watchdog second. The
    // watchdog is the only thing still ticking when the guest stalls, and the
    // launcher ends runs with TerminateProcess, so anything driven from the
    // guest side would print nothing on exactly the run that matters.
    sregStatTick();
}

// ---------------------------------------------------------------------------
// Exposed for EeScheduler's eeResolveOwnerEntry().
//
// The frametrace wrappers give every slot -- including interior resume labels --
// its own distinct sdbzFrameTraceWrapper<I> instantiation, so two addresses in
// the SAME guest function end up holding DIFFERENT function pointers in the
// dispatch table. That defeats a pointer-identity walk back through the table:
// the walk sees fn != self at the first wrapped probe and stops, reporting the
// resume label as its own entry.
//
// Measured 2026-08-31: 0x17CFA4 (a known resume label inside sub_17CF50, parts
// 38/39) was reported as owner==itself for exactly this reason, while the
// unwrapped 0x178AC4 resolved correctly to 0x178A08. This table is the
// authoritative address -> owner map for the wrapped addresses.
// ---------------------------------------------------------------------------
bool sdbzLookupFrameTraceOwner(uint32_t address, uint32_t &funcStart)
{
    for (size_t i = 0; i < kSdbzFrameTraceSlotCount; ++i)
    {
        if (kSdbzFrameTraceSlots[i].addr == address)
        {
            funcStart = kSdbzFrameTraceSlots[i].funcStart;
            return true;
        }
    }
    return false;
}
