// ps2x_conformance -- Stage B opcode conformance test, OUR side.
//
// build_scripts/conformance/gen_conformance.py writes conformance.elf (one test function per
// real SDBZ game word + a job table) and recompiles it with ps2_recomp into
// build_scripts/conformance/out/gen/. This exe links those generated functions, loads the
// ELF into a bare RDRAM, and walks the same job table PCSX2's guest main walks:
// k0 = input image, k1 = output record, call the test function. The output region is then
// written to a file for conformance_diff.py.
//
//   ps2x_conformance <conformance.elf> <ours.bin>
//
// No PS2Runtime: a test function only does RAM loads/stores (never a special address) and
// register ops, so runtime stays null. A crash in one test is caught and flagged in the
// record header (rec[12] = 0xC0DEDEAD) so the other tests still run.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define CONF_FN(name, addr, idx) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
#include "conformance_fns.inc"
#undef CONF_FN

namespace
{
    using ConfFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    struct ConfEntry
    {
        uint32_t addr;
        ConfFn fn;
    };

    const ConfEntry kConfFns[] = {
#define CONF_FN(name, addr, idx) {addr, &name},
#include "conformance_fns.inc"
#undef CONF_FN
    };

    // Same values as gen_conformance.py.
    constexpr uint32_t kHdr = 0x00400000u;
    constexpr uint32_t kMagic = 0x464E4F43u;
    constexpr uint32_t kCrashFlag = 0xC0DEDEADu;

    uint32_t Rd32(const uint8_t *ram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, ram + addr, sizeof(v));
        return v;
    }

    bool LoadElf(const char *path, uint8_t *ram)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f"
                                                       "ELF",
                                           4) != 0)
            return false;
        const uint32_t phoff = Rd32(elf.data(), 0x1C);
        uint16_t phentsize, phnum;
        std::memcpy(&phentsize, elf.data() + 0x2A, 2);
        std::memcpy(&phnum, elf.data() + 0x2C, 2);
        for (uint16_t i = 0; i < phnum; ++i)
        {
            const uint8_t *ph = elf.data() + phoff + i * phentsize;
            const uint32_t type = Rd32(ph, 0), off = Rd32(ph, 4), vaddr = Rd32(ph, 8), filesz = Rd32(ph, 16);
            if (type != 1 || filesz == 0)
                continue;
            if (off + filesz > elf.size() || (vaddr & PS2_RAM_MASK) + filesz > PS2_RAM_SIZE)
                return false;
            std::memcpy(ram + (vaddr & PS2_RAM_MASK), elf.data() + off, filesz);
        }
        return true;
    }

    // Same reset values as PS2Runtime's boot (ps2_runtime.cpp: vf0 = (0,0,0,1), Q = 1, R = 1.0).
    // A bare R5900Context is all zeros, which would make vf0.w = 0 and break every VU test.
    void ResetContext(R5900Context &ctx)
    {
        ctx = R5900Context{};
        ctx.r[0] = _mm_setzero_si128();
        ctx.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx.vu0_q = 1.0f;
        ctx.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
    }

    // No C++ objects in here: __try cannot unwind them.
    int RunOne(ConfFn fn, uint8_t *ram, R5900Context *ctx)
    {
#ifdef _MSC_VER
        __try
        {
            fn(ram, ctx, nullptr);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 1;
        }
#else
        fn(ram, ctx, nullptr);
        return 0;
#endif
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: ps2x_conformance <conformance.elf> <ours.bin>\n");
        return 2;
    }
    std::vector<uint8_t> ramBuf(PS2_RAM_SIZE + 64, 0);
    uint8_t *ram = ramBuf.data();
    if (!LoadElf(argv[1], ram))
    {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    if (Rd32(ram, kHdr) != kMagic)
    {
        std::fprintf(stderr, "%s: no conformance header at 0x%08X\n", argv[1], kHdr);
        return 2;
    }
    const uint32_t jobs = Rd32(ram, kHdr + 8), njobs = Rd32(ram, kHdr + 12);
    const uint32_t outBase = Rd32(ram, kHdr + 16), outSize = Rd32(ram, kHdr + 20);

    std::unordered_map<uint32_t, ConfFn> byAddr;
    for (const ConfEntry &e : kConfFns)
        byAddr[e.addr] = e.fn;

    // The game thread runs all guest code in EE float mode (round toward zero, FTZ, DAZ; ps2_runtime.cpp
    // gameThread). Without this every float op here would round to nearest unlike the game (10-08 run 2).
    unsigned int csr = 0;
    const char *fpEnv = std::getenv("PS2X_GUEST_FP");
    if (!(fpEnv && fpEnv[0] == '0'))
        csr = Ps2ApplyGuestFpMode();
    std::printf("[conformance] guest FP mode %s (MXCSR=0x%X)\n", csr ? "RZ+FTZ+DAZ" : "OFF (PS2X_GUEST_FP=0)", _mm_getcsr());

    static R5900Context ctx; // state carries from job to job, as on PCSX2
    ResetContext(ctx);
    uint32_t crashes = 0, unknown = 0;
    for (uint32_t j = 0; j < njobs; ++j)
    {
        const uint32_t fnAddr = Rd32(ram, jobs + 16 * j), in = Rd32(ram, jobs + 16 * j + 4);
        const uint32_t out = Rd32(ram, jobs + 16 * j + 8);
        auto it = byAddr.find(fnAddr);
        if (it == byAddr.end())
        {
            ++unknown;
            continue;
        }
        ctx.r[26] = _mm_cvtsi32_si128(static_cast<int>(in));
        ctx.r[27] = _mm_cvtsi32_si128(static_cast<int>(out));
        ctx.r[31] = _mm_setzero_si128();
        ctx.pc = fnAddr;
        if (RunOne(it->second, ram, &ctx))
        {
            ++crashes;
            std::memcpy(ram + out + 12, &kCrashFlag, 4);
            ResetContext(ctx);
        }
    }
    std::FILE *f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(ram + outBase, 1, outSize, f) != outSize)
    {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    std::fclose(f);
    std::printf("[conformance] %u jobs, %zu functions linked, %u crashed, %u unknown fn -> %s (%u bytes)\n",
                njobs, sizeof(kConfFns) / sizeof(kConfFns[0]), crashes, unknown, argv[2], outSize);
    return unknown ? 1 : 0;
}
