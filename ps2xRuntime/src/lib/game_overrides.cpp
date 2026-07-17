#include "game_overrides.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"
#include "ps2_runtime_calls.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

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

    void applySdbzKernelThunkFixes(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x0017F5D0u, &sdbzKernelStoreWordEret);
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

    PS2_REGISTER_GAME_OVERRIDE("RECVX sound-driver compat", "slus_201.84", 0u, 0u, &applyRecvxSoundDriverCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX DTX compat", "slus_201.84", 0u, 0u, &applyRecvxDtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("LotR sound RPC compat", "SLUS_205.78", 0u, 0u, &applyLotrSoundRpcCompat);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ kernel thunk fixes", "SLUS_214.42", 0u, 0u, &applySdbzKernelThunkFixes);
    PS2_REGISTER_GAME_OVERRIDE("SDBZ loadfile signature-gate seed", "SLUS_214.42", 0u, 0u, &applySdbzLoadfileSeed);
}
