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
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

// [frametrace] ring recorder, defined in ps2_runtime.cpp. Declared here rather
// than in a header on purpose: any .h edit forces a full rebuild of all 30,000+
// generated runner TUs (30+ hours). extern-between-.cpp is the sanctioned
// cross-TU pattern in this project.
void ps2FrameTraceRecord(uint32_t funcStart, uint32_t entryPc, uint32_t entrySp,
                         uint32_t exitPc, uint32_t exitSp, uint32_t exitRa) noexcept;

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

    void applySdbzKernelThunkFixes(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x0017F5D0u, &sdbzKernelStoreWordEret);
        runtime.replaceFunction(0x00104BF0u, &sdbzRegisterHandler104BF0);
        // No table slot exists for these four -- register, do not replace.
        runtime.registerFunction(0x001BFDB0u, &sdbzCtor1BFDB0);
        runtime.registerFunction(0x001BFB80u, &sdbzDtorThunk1BFB80);
        runtime.registerFunction(0x00390DF0u, &sdbzCtor390DF0);
        runtime.registerFunction(0x0038D990u, &sdbzCtor38D990);
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

        original(rdram, ctx, runtime);

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
