#include "game_overrides.h"
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

// Phase C stack-bounds guard (Kernel/Syscalls/Thread.cpp), which owns the
// per-tid stack ranges recorded at StartThread. Returns 1 when sp is outside
// the running fiber's own stack and emits one bounded STACKOOB record. site is
// 0 at slot entry, 1 at slot exit. Self-disabling for threads that never went
// through StartThread, so it is safe to leave armed in every run.
extern "C" int ps2x_stack_check(uint32_t pc, uint32_t sp, uint32_t site);

// Guest interrupt-disable preemption gate (ps2_scheduler.cpp, 2026-07-26).
// SDBZ brackets its shared-structure critical sections with the EE kernel's
// DisableIntr (0x17ed60, `cop0 0x39`) / EnableIntr (0x17edb0, `cop0 0x38`).
// The runtime has no COP0 Status EIE bit, so those calls used to be invisible
// to the scheduler and yield_point could hand the guest token to the IRQ
// worker mid-section. The wrappers for those two addresses call these to tell
// the scheduler when the guest considers interrupts masked; the depth/escape
// accessors are for the CRITSEC probe below. Same extern-between-.cpp rule.
extern "C" void ps2x_guest_intr_disable_enter();
extern "C" void ps2x_guest_intr_disable_leave();
extern "C" uint32_t ps2x_guest_intr_disable_depth();
extern "C" uint64_t ps2x_guest_intr_disable_escapes();
extern "C" uint64_t ps2x_guest_intr_disable_sections();
extern "C" uint64_t ps2x_guest_intr_disable_redundant();
extern "C" uint64_t ps2x_guest_intr_disable_stray();

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

        if (descriptors.empty())
        {
            return;
        }

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
        // 0x180d30: generated fn_180D30_0x180d30 exists (runner/fn_180D30_0x180d30.cpp)
        // but has no g_ps2RecompiledFunctionTable slot in register_functions.cpp --
        // dispatch-table gap, not a missing translation. Register the generated
        // body directly instead of regenerating (2026-07-24).
        runtime.registerFunction(0x00180D30u, &fn_180D30_0x180d30);
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
        registerSdbzSyscallThunks(runtime, std::make_index_sequence<kSdbzSyscallThunkCount>{});
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
    constexpr uint32_t kHwWatchMaxHits = 64;
    constexpr uint32_t kHwWatchMaxFrames = 12;

    struct HwWatchHit
    {
        uint64_t rip;
        uint64_t frames[kHwWatchMaxFrames];
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
    // Only stores of this value are recorded. 0xFFFFFFFF = record all.
    std::atomic<uint32_t> g_hwWatchWantVal{1};
    std::atomic<uint64_t> g_hwWatchSkipped{0}; // filtered-out stores, for sanity

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

        const uint32_t idx = g_hwWatchWrite.fetch_add(1, std::memory_order_relaxed);
        if (idx < kHwWatchMaxHits)
        {
            HwWatchHit &h = g_hwWatchHits[idx];
            h.rip = cr->Rip;
            h.val = newVal;
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

    void hwWatchSweep(uint64_t addr)
    {
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
                    SetThreadContext(h, &c);
                }
                ResumeThread(h);
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
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

        std::FILE *f = std::fopen(hwWatchDumpPath().c_str(), "a");
        for (; done < have && done < kHwWatchMaxHits; ++done)
        {
            const HwWatchHit &h = g_hwWatchHits[done];

            const char *keys[5] = {"n", "val", "rip", "tid", "guest"};
            const uint64_t vals[5] = {done, h.val, h.rip, h.tid, h.guestAddr};
            ps2x_probe_kv("HWWATCH", 5, keys, vals);

            if (f == nullptr)
                continue;
            std::fprintf(f,
                         "\n=== HWWATCH hit #%u  guest=0x%08x  newval=0x%08x  "
                         "tid=0x%x ===\n",
                         done, h.guestAddr, h.val, h.tid);
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

        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        AddVectoredExceptionHandler(1, hwWatchVeh);

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
                const char *keys[3] = {"guest", "skipped", "hits"};
                const uint64_t vals[3] = {
                    g_hwWatchGuest.load(std::memory_order_relaxed),
                    g_hwWatchSkipped.load(std::memory_order_relaxed),
                    g_hwWatchReady.load(std::memory_order_relaxed)};
                ps2x_probe_kv("HWSTAT", 3, keys, vals);
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

    // Guest-side entry point. Cheap and lock-free; safe to call on every
    // rpc_call true entry even though the slot address moves between calls.
    void hwWatchArm(const uint8_t *rdram, uint32_t guestAddr)
    {
        if (!hwWatchEnabled())
            return;
        if (rdram == nullptr)
            return;
        const uint32_t masked = guestAddr & 0x1FFFFFFCu; // 4-byte aligned
        const uint64_t host =
            reinterpret_cast<uint64_t>(rdram) + static_cast<uint64_t>(masked);

        g_hwWatchGuest.store(masked, std::memory_order_relaxed);
        g_hwWatchHost.store(host, std::memory_order_relaxed);

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
        // comment on tls_intr_disabled in ps2_scheduler.cpp.
        const bool isIntrDisable = (funcStart == 0x0017ED60u);
        const bool isIntrEnable  = (funcStart == 0x0017EDB0u);
        if (isIntrDisable)
        {
            ps2x_guest_intr_disable_enter();
        }

        original(rdram, ctx, runtime);

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

    PS2_REGISTER_GAME_OVERRIDE("RECVX sound-driver compat", "slus_201.84", 0u, 0u, &applyRecvxSoundDriverCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX DTX compat", "slus_201.84", 0u, 0u, &applyRecvxDtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("LotR sound RPC compat", "SLUS_205.78", 0u, 0u, &applyLotrSoundRpcCompat);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ kernel thunk fixes", "SLUS_214.42", 0u, 0u, &applySdbzKernelThunkFixes);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ loadfile signature-gate seed", "SLUS_214.42", 0u, 0u, &applySdbzLoadfileSeed);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ frame-trace measurement", "SLUS_214.42", 0u, 0u, &applySdbzFrameTrace);
}
