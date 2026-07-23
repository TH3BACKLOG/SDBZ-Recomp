// ps2_iop_irx_loader.cpp
// -----------------------------------------------------------------------------
// Stage 2.1 of the ARKD DVD-read RPC fix: load the REAL ARKD_DVD.IRX into IOP
// RAM and build the import-stub -> (library, function-id) map that the IopCpu
// import hook keys on. No IOP output is faked (honors feedback_no_iop_faking) --
// this places the module's actual R3000 code + data in IOP RAM so a later stage
// can run its _start and service the 0x500-0x503 RPCs from real code.
//
// This is net-new work: SifLoadModule (RPC.cpp) is bookkeeping only and never
// gets the IRX bytes into IOP RAM. Called from the LOADFILE RPC path in
// ps2_iop.cpp (extern decl there -- NO header is touched, per project rules).
//
// IRX = ELF32 LE MIPS variant, e_type 0xFF80. Facts below were confirmed byte-
// for-byte against the shipped ARKD_DVD.IRX (irx_dump.py, 2026-07-17):
//   * 2 program headers; PT_LOAD[1]: off 0xb0, vaddr 0, filesz 0xb490,
//     memsz 0x15a50.  entry _start = 0xa6bc.
//   * Whole module linked at virtual base 0.  Every relocation has sym == 0
//     (symtab is a single null entry), so applying = "add the chosen load base".
//     Because .rodata/.data carry real non-zero vaddrs, r_offset is a full
//     link-base-0 virtual address in every .rel section -> patch site is simply
//     (loadBase + r_offset).  Uniform; no per-section base arithmetic needed.
//   * .rel.text uses only R_MIPS_26 / HI16 / LO16, with HI16 immediately
//     followed by its LO16 -> a one-slot pending list resolves each pair.
//   * The import table lives at the tail of .text (inside the PT_LOAD file
//     range), 12 libraries.  A spurious 0x41E00000 magic exists at file 0xd3aa
//     but that is inside .rel.text -- OUTSIDE the loaded segment -- so bounding
//     the scan to [p_offset, p_offset+p_filesz) rejects it with no heuristics.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ps2_iop_cpu.h"
#include "runtime/ps2_memory.h"

// Real ISO sector read, defined in CD.cpp (the TU that owns the live CD state --
// its readCdSectors lives in an anonymous namespace, so we CANNOT include it
// here or we'd bind an empty duplicate copy). CD.cpp exposes this external-
// linkage forwarder; we call it for the ARKD cdvdman read import. No header
// touched (project rule): plain extern decl.
extern bool ps2_iop_cdReadSectors(uint32_t lbn, uint32_t sectors,
                                   uint8_t *dst, size_t byteCount);
// ARKD's sceCdSearchFile import (cdvdman fid=10) gates every read; resolve the
// real ISO9660 filename to the same pseudo-LBN readCdSectors uses.
extern bool ps2_iop_cdSearchFile(const char *name, uint32_t *lbnOut,
                                 uint32_t *sizeOut);

namespace
{
    // Where we place the module image in IOP RAM. IOP RAM is 2 MB; the image is
    // ~0x15a50 bytes. Nothing else in this runtime currently occupies IOP RAM
    // beyond transient SIF DMA scratch, so a fixed mid-RAM base is safe for now.
    // Runtime allocations the module makes (sceAllocSysMemory) will be handed
    // out from ABOVE the image by the S2.2 HLE, kept clear of this range.
    constexpr uint32_t kIopLoadBase = 0x00040000u; // image: 0x40000 .. ~0x55a50
    constexpr uint32_t kIopRamMask  = PS2Memory::IOP_RAM_SIZE - 1u; // 0x1FFFFF

    // MIPS relocation types (standard values; IOP toolchain uses these).
    constexpr uint8_t R_MIPS_32   = 2;
    constexpr uint8_t R_MIPS_26   = 4;
    constexpr uint8_t R_MIPS_HI16 = 5;
    constexpr uint8_t R_MIPS_LO16 = 6;

    // IRX import-library block magic (LE 0x41E00000) and header layout.
    constexpr uint32_t kImportMagic = 0x41E00000u;
    constexpr uint32_t kImportHdrLen = 20u; // magic(4) + flags(4) + ver(2)+pad(2) + name[8]

    struct ImportRef
    {
        std::string lib;
        uint16_t    fid;
    };

    // ---- S2.3: persistent IOP CPU + the stub map, owned here (file-scope
    // static, so NO header change is needed to persist state across RPCs). ----
    std::unique_ptr<IopCpu>                      g_arkdCpu;
    std::unordered_map<uint32_t, ImportRef>      g_arkdImportMap; // stubAddr -> (lib,fid)
    uint32_t                                     g_arkdLoadBase = 0;
    uint32_t                                     g_arkdEntry    = 0;
    bool                                         g_arkdLoaded   = false;

    // ---- S2.2a: IOP-side HLE runtime state (import dispatch) ----
    // Bump allocator handed to sceAllocSysMemory. Kept strictly ABOVE the module
    // image (~0x40000..0x55a50) and well below the top of IOP RAM so it never
    // overlaps the mapped module or the SIF DMA scratch the runtime uses.
    constexpr uint32_t kIopAllocBase = 0x00060000u;
    constexpr uint32_t kIopAllocEnd  = 0x00180000u;
    uint32_t                                     g_arkdAllocCursor = kIopAllocBase;
    // Per-import-stub call counter (key = stub addr) for capped, per-key logging
    // so the full init import sequence is visible without the repeat-call flood.
    std::unordered_map<uint32_t, uint32_t>       g_arkdImportCalls;

    // ---- S2.2a-follow / S2.2b: HLE bookkeeping for the _start + server-thread
    // runs. The module's _start creates a few server threads then returns; the
    // RPC registration (sceSifRegisterRpc) happens INSIDE those thread bodies,
    // which our StartThread HLE does not schedule. So we record each created
    // thread (entry + arg) and, after _start halts, run each body once with a
    // valid stack and RpcLoop HLE'd to return immediately -- that executes the
    // real registration code and lets us capture (sid -> dispatch func).
    struct ArkdThread { uint32_t entry; uint32_t arg; };
    std::vector<ArkdThread>                      g_arkdThreads;
    struct ArkdService { uint32_t sid; uint32_t func; uint32_t buff; };
    std::unordered_map<uint32_t, ArkdService>    g_arkdServices; // sid -> service
    uint32_t                                     g_arkdNextTid  = 0x10; // fake tids
    uint32_t                                     g_arkdNextSema = 0x01; // fake sema ids

    // A valid IOP stack top. IOP RAM is 2 MB (0x200000); the module image sits at
    // 0x40000 and our bump allocator runs 0x60000..0x180000, so a stack growing
    // down from 0x1FFF00 stays clear of both. Without this, gpr[29] ($sp) is 0 and
    // every push faults at ~0xffffff** (the bug the first S2.2a run exposed).
    constexpr uint32_t kIopStackTop = 0x001FFF00u;

    // Halt sentinel return address (namespace-scope so the import hook lambda can
    // end an inline worker run by jumping PC here). Same value used by the local
    // kHaltPc constants in the run blocks below.
    constexpr uint32_t kIopHaltPc = 0x0FFFFFF0u;

    // ---- S2.2c/d: ARKD worker + real-IO wiring state ----
    // Worker-thread wait semaphore id (dword_B318), captured after InitLoadBuffers
    // runs. The WaitSema import thunk (sub_AEDC) halts the inline worker run when
    // it blocks on THIS sema with no pending job (dword_B310 == 0).
    uint32_t g_arkdWorkerSema = 0;
    // IOP RAM host base, cached at load so the cdvd-read / SIF-DMA import thunks
    // can move real bytes without re-fetching it from the runtime each hit.
    uint8_t *g_arkdIopRam = nullptr;
    // EE RDRAM host base, cached at load for the SIF-DMA (IOP->EE) import thunk.
    uint8_t *g_arkdEeRam = nullptr;

    // Copy a NUL-terminated IOP string into a host buffer, bounded by both the
    // destination size and the end of IOP RAM. Used by the sysclib string thunks
    // (strcmp/strncmp/strtol), which ARKD's argument parser leans on.
    void arkdReadIopString(uint32_t iopAddr, char *dst, size_t dstSize)
    {
        dst[0] = '\0';
        if (!g_arkdIopRam || dstSize == 0)
            return;
        const uint32_t addr = iopAddr & kIopRamMask;
        for (size_t i = 0; i < dstSize - 1u; ++i)
        {
            if (size_t(addr) + i >= PS2Memory::IOP_RAM_SIZE)
                break;
            dst[i]     = (char)g_arkdIopRam[addr + i];
            dst[i + 1] = '\0';
            if (dst[i] == '\0')
                break;
        }
    }

    // ---- IOP execution tracer (env PS2_IOP_TRACE=1) -----------------------------
    // The import hook fires once per executed instruction (ps2_iop_cpu.cpp run
    // loop), so recording the PC at the top of the hook gives a complete rolling
    // window of the last N instructions with zero extra machinery. On an
    // UNEXPECTED halt (instruction budget exhausted, or final PC != the halt
    // sentinel) we dump that window, annotating any PC that is a known import stub
    // -- turning "func retired 46 instr, dunno why" into "it blocked in thsemap
    // WaitSema at 0x...". Off by default: one bool test in the hot path when
    // disabled. Ring length is a power of two so the index reduces to a mask.
    constexpr uint32_t kIopTraceLen = 128u;
    uint32_t g_iopTrace[kIopTraceLen] = {};
    uint32_t g_iopTracePos = 0u;
    bool     g_iopTraceOn  = false; // latched from env at each run entry

    bool iopTraceEnabled()  { const char *e = std::getenv("PS2_IOP_TRACE");  return e && e[0] == '1'; }
    bool arkdStateEnabled() { const char *e = std::getenv("PS2_ARKD_STATE"); return e && e[0] == '1'; }

    void iopTraceReset() { g_iopTracePos = 0u; std::memset(g_iopTrace, 0, sizeof(g_iopTrace)); }

    void dumpIopTrace(const char *why, uint32_t finalPc)
    {
        std::fprintf(stderr, "[iop:trace] %s finalPC=0x%08x (last %u executed PCs, oldest first):\n",
                     why, finalPc, kIopTraceLen);
        for (uint32_t i = 0; i < kIopTraceLen; ++i)
        {
            const uint32_t pc = g_iopTrace[(g_iopTracePos + i) & (kIopTraceLen - 1u)];
            if (!pc) continue;
            auto it = g_arkdImportMap.find(pc);
            if (it != g_arkdImportMap.end())
                std::fprintf(stderr, "  0x%08x  <import %-8s fid=%u>\n",
                             pc, it->second.lib.c_str(), it->second.fid);
            else
                std::fprintf(stderr, "  0x%08x\n", pc);
        }
    }

    // ---- ARKD job-state dumper (env PS2_ARKD_STATE=1) ---------------------------
    // Reads the module's job-state words so a worker/service run's effect is
    // visible without hand-decoding recv bytes (e.g. confirm state B300 flips
    // 0x10000000 -> 0x30000000). Offsets are module-relative: the IRX links at
    // base 0 and loads at g_arkdLoadBase, so absolute = g_arkdLoadBase + offset.
    // Adjust the table as the layout is confirmed against the IDA dump.
    void dumpArkdState(const char *when)
    {
        if (!g_arkdCpu) return;
        static const uint32_t offs[] = { 0xB300u, 0xB310u, 0xB318u, 0xBF50u };
        std::fprintf(stderr, "[ARKD:state] %-18s base=0x%06x", when, g_arkdLoadBase);
        for (uint32_t o : offs)
            std::fprintf(stderr, "  [+%05x]=%08x", o, g_arkdCpu->busRead32(g_arkdLoadBase + o));
        std::fprintf(stderr, "\n");
    }

    // ---- little-endian readers over the raw file image ----
    uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
    uint32_t le32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    // Resolve a PS2 path ("cdrom0:\ARKD_DVD.IRX;1", "ARKD_DVD.IRX", ...) to a
    // host path under the configured CD root. Kept self-contained so this TU
    // does not depend on the heavyweight Runtime.h path helpers.
    std::filesystem::path resolveHostPath(const std::string &ps2Path)
    {
        std::string s = ps2Path;
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });

        auto strip = [&](const char *pfx)
        {
            const size_t n = std::strlen(pfx);
            if (lower.compare(0, n, pfx) == 0) { s.erase(0, n); }
        };
        // Longest-prefix first so "cdrom0:" isn't shadowed by "cdrom:".
        if (lower.rfind("cdrom0:", 0) == 0) strip("cdrom0:");
        else if (lower.rfind("cdrom:", 0) == 0) strip("cdrom:");
        else if (lower.rfind("host0:", 0) == 0) strip("host0:");
        else if (lower.rfind("host:", 0) == 0) strip("host:");

        // Drop ";1" ISO version suffix.
        const size_t semi = s.find(';');
        if (semi != std::string::npos) { s.erase(semi); }
        // Normalize separators + strip leading slashes.
        std::replace(s.begin(), s.end(), '\\', '/');
        while (!s.empty() && (s.front() == '/' )) { s.erase(s.begin()); }

        const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
        std::filesystem::path root = !io.cdRoot.empty() ? io.cdRoot
                                   : (!io.elfDirectory.empty() ? io.elfDirectory
                                                               : std::filesystem::path("."));
        return (root / std::filesystem::path(s)).lexically_normal();
    }
}

// Entry point invoked from ps2_iop.cpp's LOADFILE path when the requested
// module is ARKD_DVD.IRX. Loads + relocates the module into IOP RAM and builds
// the import-stub map. Returns true on a successful load. Idempotent: only the
// first successful call does work.
bool ps2_iop_loadArkdIrx(PS2Runtime *runtime, const std::string &modulePath)
{
    if (g_arkdLoaded) { return true; }
    if (!runtime) { return false; }

    const std::filesystem::path host = resolveHostPath(modulePath);

    // ---- read the whole module file ----
    std::vector<uint8_t> d;
    {
        std::ifstream f(host, std::ios::binary);
        if (!f.is_open())
        {
            std::fprintf(stderr, "[iop:irx] ERROR cannot open '%s' (from '%s')\n",
                         host.string().c_str(), modulePath.c_str());
            return false;
        }
        f.seekg(0, std::ios::end);
        const std::streamoff sz = f.tellg();
        f.seekg(0, std::ios::beg);
        if (sz <= 0x40)
        {
            std::fprintf(stderr, "[iop:irx] ERROR '%s' too small (%lld)\n",
                         host.string().c_str(), (long long)sz);
            return false;
        }
        d.resize(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char *>(d.data()), static_cast<std::streamsize>(sz));
    }

    // ---- ELF header ----
    if (d.size() < 0x34 || std::memcmp(d.data(), "\x7f""ELF", 4) != 0)
    {
        std::fprintf(stderr, "[iop:irx] ERROR not ELF\n");
        return false;
    }
    const uint16_t e_type      = le16(&d[16]);
    const uint32_t e_entry     = le32(&d[24]);
    const uint32_t e_phoff     = le32(&d[28]);
    const uint32_t e_shoff     = le32(&d[32]);
    const uint16_t e_phentsize = le16(&d[42]);
    const uint16_t e_phnum     = le16(&d[44]);
    const uint16_t e_shentsize = le16(&d[46]);
    const uint16_t e_shnum     = le16(&d[48]);
    if (e_type != 0xFF80)
    {
        std::fprintf(stderr, "[iop:irx] ERROR e_type=0x%x (expected 0xFF80 IRX)\n", e_type);
        return false;
    }

    // ---- locate PT_LOAD (p_type == 1) ----
    uint32_t p_offset = 0, p_vaddr = 0, p_filesz = 0, p_memsz = 0;
    bool haveLoad = false;
    for (uint16_t i = 0; i < e_phnum; ++i)
    {
        const size_t off = size_t(e_phoff) + size_t(i) * e_phentsize;
        if (off + 32 > d.size()) break;
        const uint32_t p_type = le32(&d[off]);
        if (p_type == 1u) // PT_LOAD
        {
            p_offset = le32(&d[off + 4]);
            p_vaddr  = le32(&d[off + 8]);
            p_filesz = le32(&d[off + 16]);
            p_memsz  = le32(&d[off + 20]);
            haveLoad = true;
            break;
        }
    }
    if (!haveLoad || size_t(p_offset) + p_filesz > d.size())
    {
        std::fprintf(stderr, "[iop:irx] ERROR bad/absent PT_LOAD\n");
        return false;
    }

    const uint32_t base = kIopLoadBase + p_vaddr; // p_vaddr is 0 for ARKD
    if ((uint64_t(base) + p_memsz) > PS2Memory::IOP_RAM_SIZE)
    {
        std::fprintf(stderr, "[iop:irx] ERROR image (base 0x%x + memsz 0x%x) exceeds IOP RAM\n",
                     base, p_memsz);
        return false;
    }

    // ---- copy the segment into IOP RAM + zero-fill bss ----
    uint8_t *iopram = runtime->memory().getIOPRAM();
    if (!iopram)
    {
        std::fprintf(stderr, "[iop:irx] ERROR IOP RAM not available\n");
        return false;
    }
    const uint32_t ramOff = base & kIopRamMask;
    std::memcpy(iopram + ramOff, d.data() + p_offset, p_filesz);
    if (p_memsz > p_filesz)
    {
        std::memset(iopram + ramOff + p_filesz, 0, p_memsz - p_filesz);
    }

    // ---- relocation helpers (operate directly on the IOP RAM image) ----
    auto rd32 = [&](uint32_t iopAddr) -> uint32_t
    {
        const uint32_t o = iopAddr & kIopRamMask;
        uint32_t v = 0;
        if (o + 4 <= PS2Memory::IOP_RAM_SIZE) std::memcpy(&v, iopram + o, 4);
        return v;
    };
    auto wr32 = [&](uint32_t iopAddr, uint32_t v)
    {
        const uint32_t o = iopAddr & kIopRamMask;
        if (o + 4 <= PS2Memory::IOP_RAM_SIZE) std::memcpy(iopram + o, &v, 4);
    };

    // ---- apply all SHT_REL (type 9) sections ----
    // Uniform rule (see file header): patch site = base + r_offset, and any
    // computed address is shifted by +base. HI16 defers until its LO16.
    uint32_t nR26 = 0, nHi = 0, nLo = 0, n32 = 0, nOther = 0;
    std::vector<uint32_t> pendingHi; // patch addresses of unresolved HI16s

    for (uint16_t s = 0; s < e_shnum; ++s)
    {
        const size_t so = size_t(e_shoff) + size_t(s) * e_shentsize;
        if (so + 40 > d.size()) break;
        const uint32_t sh_type    = le32(&d[so + 4]);
        const uint32_t sh_off     = le32(&d[so + 16]);
        const uint32_t sh_size    = le32(&d[so + 20]);
        const uint32_t sh_entsize = le32(&d[so + 36]);
        if (sh_type != 9u /*SHT_REL*/ || sh_entsize != 8u || sh_size == 0u) continue;
        if (size_t(sh_off) + sh_size > d.size()) continue;

        pendingHi.clear(); // pairs never straddle sections
        const uint32_t count = sh_size / 8u;
        for (uint32_t r = 0; r < count; ++r)
        {
            const uint8_t *e = &d[sh_off + r * 8u];
            const uint32_t r_offset = le32(e);
            const uint32_t r_info   = le32(e + 4);
            const uint8_t  type     = uint8_t(r_info & 0xFFu); // sym == 0 for ARKD
            const uint32_t at       = base + r_offset;

            switch (type)
            {
            case R_MIPS_32:
            {
                wr32(at, rd32(at) + base);
                ++n32;
                break;
            }
            case R_MIPS_26:
            {
                const uint32_t w = rd32(at);
                uint32_t target = (w & 0x03FFFFFFu) << 2;
                target += base;
                wr32(at, (w & 0xFC000000u) | ((target >> 2) & 0x03FFFFFFu));
                ++nR26;
                break;
            }
            case R_MIPS_HI16:
            {
                pendingHi.push_back(at);
                ++nHi;
                break;
            }
            case R_MIPS_LO16:
            {
                const uint32_t lo = rd32(at);
                const int32_t  loAdd = int16_t(lo & 0xFFFFu);
                uint32_t v = base + uint32_t(loAdd); // value if no matching HI16
                for (uint32_t hiAt : pendingHi)
                {
                    const uint32_t hi = rd32(hiAt);
                    const int32_t  hiAdd = int32_t((hi & 0xFFFFu) << 16);
                    v = uint32_t(int32_t(hiAdd + loAdd) + int32_t(base));
                    const uint32_t hiField = ((v >> 16) + ((v & 0x8000u) ? 1u : 0u)) & 0xFFFFu;
                    wr32(hiAt, (hi & 0xFFFF0000u) | hiField);
                }
                pendingHi.clear();
                wr32(at, (lo & 0xFFFF0000u) | (v & 0xFFFFu));
                ++nLo;
                break;
            }
            default:
                ++nOther;
                break;
            }
        }
    }

    // ---- build the import-stub -> (lib, fid) map (segment-bounded scan) ----
    g_arkdImportMap.clear();
    uint32_t importLibs = 0;
    const size_t segBeg = p_offset;
    const size_t segEnd = size_t(p_offset) + p_filesz;
    for (size_t j = segBeg; j + kImportHdrLen <= segEnd; )
    {
        if (le32(&d[j]) != kImportMagic) { ++j; continue; }

        // Library name (up to 8 chars). Reject non-printable => not a real block.
        char name[9] = {0};
        bool printable = false;
        for (int c = 0; c < 8; ++c)
        {
            const uint8_t ch = d[j + 12 + c];
            if (ch == 0) break;
            if (ch < 0x20 || ch >= 0x7F) { printable = false; break; }
            name[c] = char(ch);
            printable = true;
        }
        if (!printable) { ++j; continue; }

        // 8-byte stubs follow the header until a double-zero terminator.
        size_t k = j + kImportHdrLen;
        uint32_t nstub = 0;
        while (k + 8 <= segEnd)
        {
            const uint32_t w0 = le32(&d[k]);
            const uint32_t w1 = le32(&d[k + 4]);
            if (w0 == 0 && w1 == 0) break;
            const uint16_t fid = uint16_t(w1 & 0xFFFFu);
            const uint32_t stubVaddr = uint32_t(k - p_offset) + p_vaddr;
            const uint32_t stubAddr  = base + stubVaddr;
            g_arkdImportMap[stubAddr] = ImportRef{std::string(name), fid};
            ++nstub;
            k += 8;
        }
        ++importLibs;
        std::fprintf(stderr, "[iop:irx] import lib %-9s stubs=%u\n", name, nstub);
        j = k; // continue past this block
    }

    // ---- S2.3: construct the persistent IOP CPU (reusable across RPCs) ----
    g_arkdCpu = std::make_unique<IopCpu>(&runtime->memory());
    g_arkdLoadBase = base;
    g_arkdEntry    = base + e_entry;
    g_arkdIopRam   = iopram;
    g_arkdEeRam    = runtime->memory().getRDRAM();
    g_arkdLoaded   = true;

    std::fprintf(stderr,
                 "[iop:irx] LOADED ARKD_DVD.IRX @base 0x%06x entry 0x%06x  "
                 "copied 0x%x zero 0x%x  reloc[26=%u hi=%u lo=%u 32=%u other=%u]  "
                 "libs=%u stubs=%zu\n",
                 base, g_arkdEntry, p_filesz, (p_memsz - p_filesz),
                 nR26, nHi, nLo, n32, nOther, importLibs, g_arkdImportMap.size());

    // ---- S2.2a: real import-HLE dispatch run of _start (gated: PS2_ARKD_IRX_RUN=1) ----
    // Promotes the old "every import returns 0" stub into a structured dispatch
    // keyed on g_arkdImportMap[pc] -> (lib, fid). It richly logs the FIRST few
    // calls per import stub (lib, fid, a0-a3, ra) so the real init sequence and
    // argument shapes become visible data -- that data drives which fids get real
    // handlers in the S2.2a follow-up / S2.2b-d. AllocSysMemory is serviced for
    // real (a bump allocator above the image) because returning NULL there would
    // stall _start; every other import is logged and returns 0 until the log
    // confirms its export ordinal. Bounded + gated, so the default boot path is
    // byte-for-byte unchanged (this only runs under PS2_ARKD_IRX_RUN=1).
    if (const char *runFlag = std::getenv("PS2_ARKD_IRX_RUN"); runFlag && runFlag[0] == '1')
    {
        constexpr uint32_t kHaltPc = 0x0FFFFFF0u; // sentinel return address
        g_arkdAllocCursor = kIopAllocBase;        // fresh allocator per run
        g_arkdImportCalls.clear();
        g_arkdThreads.clear();
        g_arkdServices.clear();
        g_arkdNextTid  = 0x10;
        g_arkdNextSema = 0x01;
        g_iopTraceOn   = iopTraceEnabled();

        // Import HLE dispatch, keyed on g_arkdImportMap[pc] -> (lib, fid). The
        // export ordinals below were confirmed by the first S2.2a run's log
        // (thbase 4/6/33, thsemap 4, loadcore 5, sifcmd 14, stdio 4). Anything
        // not special-cased returns 0 and returns to $ra -- correct for the
        // init/void calls (InitRpc, SetRpcQueue, RpcLoop, DelayThread,
        // RegisterLibraryEntries) and loud-logged so any real gap surfaces.
        g_arkdCpu->setImportHook([](IopCpu &cpu, uint32_t pc) -> bool
        {
            // Tracer: record every executed PC (hook fires per instruction).
            if (g_iopTraceOn) { g_iopTrace[g_iopTracePos & (kIopTraceLen - 1u)] = pc; ++g_iopTracePos; }

            auto it = g_arkdImportMap.find(pc);
            if (it == g_arkdImportMap.end()) return false; // not an import stub
            const std::string &lib = it->second.lib;
            const uint16_t     fid = it->second.fid;

            const uint32_t a0 = cpu.gpr(4), a1 = cpu.gpr(5), a2 = cpu.gpr(6),
                           a3 = cpu.gpr(7), ra = cpu.gpr(31);

            // Per-stub capped logging: show the first few hits of each distinct
            // import so the whole init sequence is captured without flooding once
            // a stub is hit in a loop.
            const uint32_t seen = g_arkdImportCalls[pc]++;
            if (seen < 6u)
            {
                std::fprintf(stderr,
                    "[iop:import] %-8s fid=%-3u @0x%06x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x\n",
                    lib.c_str(), fid, pc, a0, a1, a2, a3, ra);
            }

            // ---- S2.2c/d: ARKD internal cdvd / SIF-DMA / worker-sema thunks,
            // dispatched by module offset (these are import stubs, so `it` above
            // is valid; the offset just names which one). They move REAL bytes --
            // the read pulls real ISO sectors, the DMA copies them to the EE dest
            // the game chose -- so nothing here is faked (feedback_no_iop_faking).
            const uint32_t off = pc - g_arkdLoadBase;

            // WaitSema(sema) thunk (sub_AEDC): when the worker (sub_21B0) blocks on
            // its own wait-sema with no pending job (dword_B310 == 0), end the
            // inline worker run by halting; every other wait "acquires" (returns 0).
            if (off == 0xAEDCu)
            {
                if (a0 == g_arkdWorkerSema &&
                    cpu.busRead32(g_arkdLoadBase + 0xB310u) == 0u)
                {
                    // Diagnostic: if the worker blocks on its idle wait-sema while
                    // B300 is still in the 0x20000000 in-progress band, the transfer
                    // body cleared B310 and looped back to wait BEFORE finishing the
                    // read+DMA -- so halting here freezes B300 mid-transfer. That is
                    // the exact premature cut-off Phase C must avoid.
                    const uint32_t wB300 = cpu.busRead32(g_arkdLoadBase + 0xB300u);
                    static uint32_t s_idleLogs = 0u;
                    if (s_idleLogs < 16u &&
                        (wB300 & 0xF0000000u) == 0x20000000u)
                    {
                        ++s_idleLogs;
                        std::fprintf(stderr,
                            "[ARKD:idle] worker WaitSema(idle) @off=+0x%06x with "
                            "B300=%08x (mid-transfer) -- halting freezes transfer\n",
                            (pc - g_arkdLoadBase) & 0x00FFFFFFu, wB300);
                    }
                    cpu.setPC(kIopHaltPc); // idle worker -> clean end of inline run
                    return true;
                }
                cpu.setGpr(2, 0);
                cpu.setPC(ra);
                return true;
            }

            // sceCdSearchFile thunk (sub_ACA8 = cdvdman fid 10): a0 = sceCdlFILE*
            // {u32 lsn, u32 size, char name[16], u8 date[8]}, a1 = "\NAME;1".
            // ARKD gates EVERY read behind `while(!sceCdSearchFile(fp,name)) delay()`,
            // so this must resolve the real ISO file or the driver spins forever.
            if (off == 0xACA8u)
            {
                char name[64] = {0};
                if (g_arkdIopRam)
                {
                    const uint32_t nameAddr = a1 & kIopRamMask;
                    for (uint32_t i = 0; i < sizeof(name) - 1u; ++i)
                    {
                        if (size_t(nameAddr) + i >= PS2Memory::IOP_RAM_SIZE)
                            break;
                        const char c = (char)g_arkdIopRam[nameAddr + i];
                        name[i] = c;
                        if (c == '\0')
                            break;
                    }
                }
                uint32_t lbn = 0, size = 0;
                const bool ok = ps2_iop_cdSearchFile(name, &lbn, &size);
                const uint32_t fp = a0 & kIopRamMask;
                if (ok && g_arkdIopRam &&
                    size_t(fp) + 8u <= PS2Memory::IOP_RAM_SIZE)
                {
                    std::memcpy(g_arkdIopRam + fp + 0u, &lbn, 4u);
                    std::memcpy(g_arkdIopRam + fp + 4u, &size, 4u);
                }
                if (seen < 16u)
                    std::fprintf(stderr,
                        "[ARKD:search] \"%s\" -> %s lbn=%u size=0x%x\n",
                        name, ok ? "found" : "MISS", lbn, size);
                cpu.setGpr(2, ok ? 1u : 0u); // nonzero breaks the search poll
                cpu.setPC(ra);
                return true;
            }

            // cdvd sector read thunk (sub_AC98): (lbn, sectors, iopDest, &status).
            // Pull real ISO sectors into the IOP buffer via CD.cpp's live reader.
            if (off == 0xAC98u)
            {
                const uint32_t lbn     = a0;
                const uint32_t sectors = a1;
                const uint32_t iopDest = a2 & kIopRamMask;
                bool ok = false;
                if (g_arkdIopRam && sectors &&
                    size_t(iopDest) + size_t(sectors) * 2048u <= PS2Memory::IOP_RAM_SIZE)
                {
                    ok = ps2_iop_cdReadSectors(lbn, sectors,
                                               g_arkdIopRam + iopDest,
                                               size_t(sectors) * 2048u);
                }
                if (seen < 8u)
                    std::fprintf(stderr,
                        "[ARKD:cdread] lbn=%u sectors=%u iopDest=0x%06x -> %s\n",
                        lbn, sectors, iopDest, ok ? "ok" : "FAIL");
                cpu.setGpr(2, ok ? 1u : 0u); // 1 = read complete (breaks caller poll)
                cpu.setPC(ra);
                return true;
            }

            // sceCdDiskReady thunk (sub_ACB8 = cdvdman fid 13 / export 0Dh).
            // sub_91C is `while (sceCdDiskReady(1) != 2) delay();` and it gates
            // InitLoadBuffers plus every read-retry path, so an unhooked default
            // of 0 spins the driver forever. 2 = SCECdComplete ("ready"), which
            // is what our EE-side stub (Kernel/Stubs/CD.cpp sceCdDiskReady) already
            // returns -- the disc image is a host file and is never not-ready.
            if (off == 0xACB8u)
            {
                cpu.setGpr(2, 2u);
                cpu.setPC(ra);
                return true;
            }

            // ---- sysclib string thunks (strcmp / strncmp / strtol) --------------
            // ARKD's module-arg parser (+0x3020) is four sequential `if`s, each
            // consuming one argv entry only when its strncmp matches:
            //     if (!strncmp(p,"disk=",5))  { ...strcmp(p+5,"CD")... ++i; }
            //     if (!strncmp(p,"file=",5))  { B49C = strtol(p+5,0,0); ++i; }
            //     if (!strncmp(p,"pitch=",6)) { B4A8 = strtol(p+6,0,0); ++i; }
            //     if (!strncmp(p,"sndeff=",7)){ ...strcmp(p+7,"OFF")... ++i; }
            // Unhandled imports default to $v0 = 0, which reads as "equal" for both
            // compares and as the value 0 for strtol. That made every branch fire
            // (harmless only because our argv order happens to match the if-chain)
            // and, fatally, set B49C = 0 -- so the TOC alloc asked for 48*0 bytes,
            // returned NULL, and the read went out as sectors=0 dest=0 -> FAIL.
            // strcmp is also used at +0x286 for TOC filename lookup, so a constant
            // 0 there would make every filename match the first entry.
            if (off == 0xAE34u || off == 0xAE4Cu) // strcmp / strncmp
            {
                char lhs[128], rhs[128];
                arkdReadIopString(a0, lhs, sizeof(lhs));
                arkdReadIopString(a1, rhs, sizeof(rhs));
                const int r = (off == 0xAE34u)
                                  ? std::strcmp(lhs, rhs)
                                  : std::strncmp(lhs, rhs, size_t(a2));
                cpu.setGpr(2, uint32_t(int32_t(r < 0 ? -1 : (r > 0 ? 1 : 0))));
                cpu.setPC(ra);
                return true;
            }

            // strtol(str, endptr, base): parses the `file=` / `pitch=` values.
            if (off == 0xAE54u)
            {
                char buf[64];
                arkdReadIopString(a0, buf, sizeof(buf));
                char *end = buf;
                const long v = std::strtol(buf, &end, int(a2));
                if (a1)
                    cpu.busWrite32(a1, a0 + uint32_t(end - buf)); // endptr, in IOP space
                if (seen < 6u)
                    std::fprintf(stderr,
                        "[ARKD:strtol] \"%s\" base=%u -> %ld\n", buf, a2, v);
                cpu.setGpr(2, uint32_t(int32_t(v)));
                cpu.setPC(ra);
                return true;
            }

            // sysclib memcpy(dest, src, n) thunk (import stub at off 0xAE24).
            // ARKD's sub_2410/sub_250C latch the RPC request's EE-dest word into
            // dword_BF50 by *calling this resident memcpy* (a0=&BF50, a1=&request,
            // a2=0x24). memcpy is an import, so the unhooked default silently
            // no-ops the copy -- BF50 stayed 0, the SIF-DMA descriptor's dest field
            // (sub_ADD4 reads descriptor[+4] straight out of BF50) read 0, and the
            // TOC transfer landed at EE 0x0 -- the eeDest=0 derail. Measured
            // 2026-07-22: request word[0]=0x01652400 present in both IOP buffers,
            // post-func BF50=0 -> cause-table Row 3 (latch never ran). Perform the
            // real IOP->IOP copy the driver asked for (no faking) and return dest.
            if (off == 0xAE24u)
            {
                const uint32_t d = a0 & kIopRamMask, s = a1 & kIopRamMask;
                if (g_arkdIopRam && a2 &&
                    size_t(d) + a2 <= PS2Memory::IOP_RAM_SIZE &&
                    size_t(s) + a2 <= PS2Memory::IOP_RAM_SIZE)
                {
                    std::memmove(g_arkdIopRam + d, g_arkdIopRam + s, a2);
                }
                cpu.setGpr(2, a0); // memcpy returns dest
                cpu.setPC(ra);
                return true;
            }

            // SIF DMA IOP->EE thunk (sub_ADD4): a0 = &descriptor{src, dest, size,
            // attr}, a1 = count. Deliver the just-read bytes to the EE dest the
            // game named. Must return a non-zero queue id -- 0 spins the caller's
            // `while(!v5) v5 = sub_ADD4(...)` forever.
            if (off == 0xADD4u)
            {
                const uint32_t src  = cpu.busRead32(a0 + 0u) & kIopRamMask;
                const uint32_t dest = cpu.busRead32(a0 + 4u) & PS2_RAM_MASK;
                const uint32_t size = cpu.busRead32(a0 + 8u);
                bool ok = false;
                if (g_arkdIopRam && g_arkdEeRam && size &&
                    size_t(src)  + size <= PS2Memory::IOP_RAM_SIZE &&
                    size_t(dest) + size <= PS2_RAM_SIZE)
                {
                    std::memcpy(g_arkdEeRam + dest, g_arkdIopRam + src, size);
                    ok = true;
                }
                if (seen < 8u)
                {
                    // dest comes straight out of dword_BF50, which sub_2410/sub_250C
                    // fill by copying 36 bytes from the RPC request buffer. Log the
                    // caller ($ra, module-relative so it maps onto the decompile's
                    // irx_arkddvd_sub_XXXX names) -- the driver globals alone cannot
                    // say which function built this descriptor.
                    std::fprintf(stderr,
                        "[ARKD:sifdma] from=+0x%06x desc=0x%06x"
                        " iopSrc=0x%06x eeDest=0x%08x size=0x%x -> %s"
                        " | BF50=%08x B300=%08x B310=%08x B49C=%08x\n",
                        (ra - g_arkdLoadBase) & 0x00FFFFFFu, a0 & kIopRamMask,
                        src, dest, size, ok ? "copied" : "SKIP",
                        cpu.busRead32(g_arkdLoadBase + 0xBF50u),
                        cpu.busRead32(g_arkdLoadBase + 0xB300u),
                        cpu.busRead32(g_arkdLoadBase + 0xB310u),
                        cpu.busRead32(g_arkdLoadBase + 0xB49Cu));
                }
                cpu.setGpr(2, 1u); // non-zero queue id -> breaks the submit poll
                cpu.setPC(ra);
                return true;
            }

            // sceSifDmaStat(id) (sub_ADDC): the worker polls this in a
            // `do { stat = sceSifDmaStat(id); DelayThread(); } while (stat >= 0)`
            // loop after submitting via 0xADD4. IOP convention is 0 = still
            // queued, 1 = in flight, -1 = complete. Falling through to the
            // rv=0 default meant "still queued" forever, which is what pinned
            // the worker at +0xaddc and left dword_B300 at 0x20000000. Our
            // 0xADD4 handler copies synchronously, so by the time this is
            // reached the transfer really is done -- report it.
            if (off == 0xADDCu)
            {
                cpu.setGpr(2, 0xFFFFFFFFu); // -1 == completed
                cpu.setPC(ra);
                return true;
            }

            uint32_t rv = 0;

            // sysmem AllocSysMemory(mode, size, addr) -> IOP address from the
            // bump allocator (ordinal 4, stable across the IOP SDK).
            if (lib == "sysmem" && fid == 4u)
            {
                const uint32_t size = (a1 + 0xFFu) & ~0xFFu; // 256-byte align
                if (g_arkdAllocCursor + size <= kIopAllocEnd && size != 0u)
                {
                    rv = g_arkdAllocCursor;
                    g_arkdAllocCursor += size;
                }
                else
                {
                    std::fprintf(stderr, "[iop:import]   AllocSysMemory FAIL size=0x%x cursor=0x%06x\n",
                                 size, g_arkdAllocCursor);
                    rv = 0;
                }
                if (seen < 6u)
                    std::fprintf(stderr, "[iop:import]   -> alloc 0x%06x (size 0x%x)\n", rv, size);
            }
            // thsemap CreateSema (ordinal 4) -> non-zero sema id (0 reads as fail).
            else if (lib == "thsemap" && fid == 4u)
            {
                rv = g_arkdNextSema++;
            }
            // thbase CreateThread (ordinal 4): param struct at a0; entry at +8,
            // stacksize +12, priority +16 (matches the logged writes). Record the
            // entry so we can run the body after _start; return a non-zero tid.
            else if (lib == "thbase" && fid == 4u)
            {
                const uint32_t entry = cpu.busRead32(a0 + 8u);
                g_arkdThreads.push_back(ArkdThread{entry, 0u});
                rv = g_arkdNextTid++;
                if (seen < 6u)
                    std::fprintf(stderr, "[iop:import]   -> CreateThread entry=0x%06x tid=0x%x\n",
                                 entry, rv);
            }
            // thbase StartThread (ordinal 6): a0=tid, a1=arg. Do not schedule --
            // record the arg against the matching thread (tid = 0x10 + index).
            else if (lib == "thbase" && fid == 6u)
            {
                const uint32_t idx = a0 - 0x10u;
                if (idx < g_arkdThreads.size()) g_arkdThreads[idx].arg = a1;
                rv = 0;
            }
            // sifcmd sceSifRegisterRpc (ordinal 17):
            //   (srv, sid, func, buff, cfunc, cbuff, qd) -> a1=sid, a2=func, a3=buff
            // Capture the real (sid -> dispatch func) straight from the module.
            else if (lib == "sifcmd" && fid == 17u)
            {
                g_arkdServices[a1] = ArkdService{a1, a2, a3};
                std::fprintf(stderr,
                    "[ARKD:svc] registered sid=0x%x func=0x%06x buff=0x%06x\n", a1, a2, a3);
                rv = 0;
            }
            // stdio printf (ordinal 4): a0 = IOP format-string pointer. Log it.
            else if (lib == "stdio" && fid == 4u)
            {
                if (seen < 6u)
                {
                    char buf[80]; int n = 0;
                    for (; n < 79; ++n)
                    {
                        const uint8_t ch = cpu.busRead8(a0 + uint32_t(n));
                        if (ch == 0) break;
                        buf[n] = (ch >= 0x20 && ch < 0x7F) ? char(ch) : '.';
                    }
                    buf[n] = 0;
                    std::fprintf(stderr, "[iop:printf] \"%s\"\n", buf);
                }
                rv = 0;
            }
            // Everything else (InitRpc, SetRpcQueue, RpcLoop, DelayThread,
            // RegisterLibraryEntries, CpuEnable/DisableIntr, ...) -> return 0.

            cpu.setGpr(2, rv);        // $v0
            cpu.setPC(ra);            // return to caller ($ra)
            return true;
        });

        // ---- stage the module arguments _start expects ----
        // ARKD_DVD parses argv in its entry: "disk=", "file=", "pitch=",
        // "sndeff=". With argc=0 it keeps its defaults, and the default
        // file-table cap is 1279 entries. This disc's INFO.DAT declares 1930
        // files, so TocDecodeAndIndex (+0x2C0) hits
        //     if (toc[44] >= maxFiles) { printf("TOC File len is Over..."); for(;;) delay(); }
        // and wedges the driver forever.
        //
        // The EE builds the real arg blob at 0x422338..0x4223b0: it sprintf()s
        // "disk=DVD file=%d pitch=4096 sndeff=OFF" with $a2 = 0x7FF (2047),
        // then rewrites every space to NUL to form a NUL-separated argv before
        // calling SifLoadStartModule. We reproduce that blob verbatim.
        // 2047 also sizes the TOC buffer to 48*2047 = 0x17FD0, which is the
        // whole 98304-byte INFO.DAT rather than the truncated 29 sectors.
        constexpr uint32_t kIopArgvBase = 0x0005F000u; // image end .. kIopAllocBase gap
        static const char *const kArkdArgv[] = {
            "cdrom0:\\ARKD_DVD.IRX;1",
            "disk=DVD",
            "file=2047",
            "pitch=4096",
            "sndeff=OFF",
        };
        constexpr uint32_t kArkdArgc = uint32_t(sizeof(kArkdArgv) / sizeof(kArkdArgv[0]));

        // ---- run _start (now with a valid stack) ----
        g_arkdCpu->reset();
        {
            uint32_t strCursor = kIopArgvBase + kArkdArgc * 4u;
            for (uint32_t i = 0; i < kArkdArgc; ++i)
            {
                g_arkdCpu->busWrite32(kIopArgvBase + i * 4u, strCursor);
                for (const char *p = kArkdArgv[i]; *p; ++p)
                    g_arkdCpu->busWrite8(strCursor++, uint8_t(*p));
                g_arkdCpu->busWrite8(strCursor++, 0u);
            }
        }
        g_arkdCpu->setGpr(4, kArkdArgc);     // $a0 = argc
        g_arkdCpu->setGpr(5, kIopArgvBase);  // $a1 = argv
        g_arkdCpu->setGpr(29, kIopStackTop); // $sp -- THE FIX (see kIopStackTop)
        g_arkdCpu->setGpr(31, kHaltPc);      // _start returns here -> halt
        g_arkdCpu->setHaltPc(kHaltPc);
        g_arkdCpu->setPC(g_arkdEntry);
        if (g_iopTraceOn) iopTraceReset();
        const uint32_t retired = g_arkdCpu->run(4u * 1000u * 1000u);
        std::fprintf(stderr,
                     "[iop:irx] _start run: retired=%u finalPC=0x%08x halted=%d "
                     "allocTop=0x%06x distinctImports=%zu threads=%zu\n",
                     retired, g_arkdCpu->pc(), g_arkdCpu->halted() ? 1 : 0,
                     g_arkdAllocCursor, g_arkdImportCalls.size(), g_arkdThreads.size());
        if (g_iopTraceOn && (!g_arkdCpu->halted() || g_arkdCpu->pc() != kHaltPc))
            dumpIopTrace("_start did not reach halt sentinel", g_arkdCpu->pc());
        if (arkdStateEnabled()) dumpArkdState("after _start");

        // ---- S2.2b: run each recorded server-thread body once to reach its
        // sceSifRegisterRpc. RpcLoop/WaitSema return immediately (default hook),
        // so the body runs its InitRpc/SetRpcQueue/RegisterRpc setup then falls
        // through to the halt sentinel. Each gets its own stack slice below the
        // _start stack; state (sema/libs) set up by _start persists in IOP RAM.
        for (size_t i = 0; i < g_arkdThreads.size(); ++i)
        {
            g_arkdCpu->setGpr(29, kIopStackTop - 0x4000u * uint32_t(i + 1)); // $sp
            g_arkdCpu->setGpr(31, kHaltPc);
            g_arkdCpu->setGpr(4, g_arkdThreads[i].arg); // a0 = thread arg
            g_arkdCpu->setHaltPc(kHaltPc);
            g_arkdCpu->setPC(g_arkdThreads[i].entry);
            if (g_iopTraceOn) iopTraceReset();
            const uint32_t tret = g_arkdCpu->run(2u * 1000u * 1000u);
            std::fprintf(stderr,
                         "[iop:irx] thread[%zu] entry=0x%06x run: retired=%u finalPC=0x%08x halted=%d\n",
                         i, g_arkdThreads[i].entry, tret, g_arkdCpu->pc(),
                         g_arkdCpu->halted() ? 1 : 0);
            if (g_iopTraceOn && (!g_arkdCpu->halted() || g_arkdCpu->pc() != kHaltPc))
                dumpIopTrace("thread did not reach halt sentinel", g_arkdCpu->pc());
        }
        if (arkdStateEnabled()) dumpArkdState("after threads");

        // ---- S2.2c: run InitLoadBuffers (sub_28AC @ +0x28AC) once so the module
        // creates its worker wait-sema (dword_B318), allocates its load buffers,
        // and reads the REAL disc TOC (through the sub_AC98 cdvd hook) -- leaving
        // dword_B300 == 0x30000000 (ready), which the read dispatchers require.
        // Its CreateThread(sub_21B0) is only recorded (not scheduled) -- the worker
        // body is run inline per-CALL in ps2_iop_runArkdService.
        {
            g_arkdCpu->setGpr(29, kIopStackTop - 0x9000u); // $sp clear of thread stacks
            g_arkdCpu->setGpr(31, kHaltPc);
            g_arkdCpu->setHaltPc(kHaltPc);
            g_arkdCpu->setPC(g_arkdLoadBase + 0x28ACu);
            if (g_iopTraceOn) iopTraceReset();
            // Budget note: once `file=2047` parses correctly, the TOC descrambler
            // (sub_2C0) walks 48 * 2047 == 98256 bytes doing three read-modify-write
            // ops each -- roughly 3M IOP instructions on its own, which overran the
            // 4M cap used elsewhere and left us halted mid-loop at +0x34c. This is a
            // bounded one-shot init, not a poll loop, so give it room to finish.
            const uint32_t iret = g_arkdCpu->run(64u * 1000u * 1000u);
            g_arkdWorkerSema = g_arkdCpu->busRead32(g_arkdLoadBase + 0xB318u);
            std::fprintf(stderr,
                "[iop:irx] InitLoadBuffers run: retired=%u finalPC=0x%08x halted=%d "
                "workerSema=0x%x B300=%08x\n",
                iret, g_arkdCpu->pc(), g_arkdCpu->halted() ? 1 : 0,
                g_arkdWorkerSema, g_arkdCpu->busRead32(g_arkdLoadBase + 0xB300u));
            if (g_iopTraceOn && (!g_arkdCpu->halted() || g_arkdCpu->pc() != kHaltPc))
                dumpIopTrace("InitLoadBuffers did not reach halt sentinel", g_arkdCpu->pc());
            if (arkdStateEnabled()) dumpArkdState("after InitLoadBuffers");
        }

        std::fprintf(stderr, "[ARKD:svc] captured %zu service(s)\n", g_arkdServices.size());
    }

    return true;
}

// -----------------------------------------------------------------------------
// S2.2d-1: run one registered ARKD RPC service function on demand, from real
// module code in the persistent IopCpu. Called by SIF.cpp's CALL bridge (gated
// PS2_ARKD_SERVICE=1) when the game issues a 0x503 DVD-read CALL. No IOP output
// is faked -- the reply bytes come from whatever the real func writes into IOP
// RAM (honors feedback_no_iop_faking).
//
// SIF-RPC server convention: void *func(int fno, void *buff, int size). On real
// HW the SIF DMA copies the client send buffer into the server's registered
// receive buffer (`buff`, captured at sceSifRegisterRpc time = g_arkdServices
// [sid].buff) before the dispatcher calls func; func returns a pointer to its
// reply data in IOP RAM, `rsize` bytes of which DMA back to the client recv buf.
// We reproduce that here: send -> IOP buff, run func, copy recvSize bytes from
// IOP RAM at $v0 -> recvOut.
//
// Requires the IRX already loaded AND _start/server-threads already run (i.e.
// PS2_ARKD_IRX_RUN=1) so g_arkdServices is populated, the import hook installed,
// and IOP RAM/sema state set up. Returns false (bridge stays observe-only) if
// that precondition is not met or the sid isn't registered.
bool ps2_iop_runArkdService(PS2Runtime *runtime,
                            uint32_t sid, uint32_t rpcNum,
                            const uint8_t *sendData, uint32_t sendSize,
                            uint8_t *recvOut, uint32_t recvSize)
{
    if (!runtime || !g_arkdLoaded || !g_arkdCpu) { return false; }

    auto it = g_arkdServices.find(sid);
    if (it == g_arkdServices.end()) { return false; }
    const ArkdService &svc = it->second;

    uint8_t *iopram = runtime->memory().getIOPRAM();
    if (!iopram) { return false; }

    // Bounded IOP-RAM copy helper (clamps at the 2 MB edge).
    auto iopCopyIn = [&](uint32_t iopAddr, const uint8_t *src, uint32_t len)
    {
        const uint32_t o = iopAddr & kIopRamMask;
        uint32_t n = len;
        if (o >= PS2Memory::IOP_RAM_SIZE) { n = 0; }
        else if (o + n > PS2Memory::IOP_RAM_SIZE) { n = PS2Memory::IOP_RAM_SIZE - o; }
        if (n) { std::memcpy(iopram + o, src, n); }
    };
    auto iopCopyOut = [&](uint8_t *dst, uint32_t iopAddr, uint32_t len)
    {
        const uint32_t o = iopAddr & kIopRamMask;
        uint32_t n = len;
        if (o >= PS2Memory::IOP_RAM_SIZE) { n = 0; }
        else if (o + n > PS2Memory::IOP_RAM_SIZE) { n = PS2Memory::IOP_RAM_SIZE - o; }
        if (n) { std::memcpy(dst, iopram + o, n); }
    };

    // Copy the guest's send buffer into the service's registered IOP buffer,
    // exactly as the SIF DMA would before the dispatcher calls func.
    const uint32_t buff = svc.buff;
    if (sendData && sendSize) { iopCopyIn(buff, sendData, sendSize); }

    // ---- S2 diag: the eeDest=0 hunt. Log, for the first few CALLs, whether the
    // send payload actually reached the IOP buffer the dispatcher reads. hostBuff
    // is iopram[] (what iopCopyIn wrote); cpuBuff is the R3000 bus view (what
    // sub_2410 reads). A mismatch = aliasing; a null sendData = copy skipped;
    // both zero = the EE payload's word[0] really was 0 at CALL time. Removed once
    // the defect is pinned. Instrumentation only -- no IOP output is faked.
    static uint32_t s_diag = 0u;
    const bool diag = (s_diag < 8u);
    if (diag)
    {
        ++s_diag;
        const uint32_t bo = buff & kIopRamMask;
        const uint32_t hostW0 = (bo + 4u <= PS2Memory::IOP_RAM_SIZE)
            ? *reinterpret_cast<const uint32_t *>(iopram + bo) : 0xDEADBEEFu;
        std::fprintf(stderr,
            "[ARKD:diag] pre-func sid=0x%x fno=0x%x sendData=%s ssz=0x%x buff=0x%06x "
            "hostBuff[0]=0x%08x cpuBuff[0]=0x%08x\n",
            sid, rpcNum, sendData ? "nonnull" : "NULL", sendSize, buff,
            hostW0, g_arkdCpu->busRead32(buff));
    }

    // Run func(fno, buff, size) on a fresh stack slice clear of the image
    // (0x40000..) and the bump allocator (0x60000..0x180000). Do NOT reset() --
    // the sema/lib state _start established in IOP RAM must persist.
    constexpr uint32_t kHaltPc = 0x0FFFFFF0u;
    g_arkdCpu->setGpr(29, kIopStackTop - 0x8000u); // $sp
    g_arkdCpu->setGpr(31, kHaltPc);                // $ra -> halt sentinel
    g_arkdCpu->setGpr(4, rpcNum);                  // $a0 = fno
    g_arkdCpu->setGpr(5, buff);                    // $a1 = buff
    g_arkdCpu->setGpr(6, sendSize);                // $a2 = size
    g_arkdCpu->setHaltPc(kHaltPc);
    g_arkdCpu->setPC(svc.func);
    g_iopTraceOn = iopTraceEnabled();
    if (g_iopTraceOn) iopTraceReset();
    if (arkdStateEnabled()) dumpArkdState("before service");
    const uint32_t retired = g_arkdCpu->run(4u * 1000u * 1000u);

    const bool     halted = g_arkdCpu->halted();
    const uint32_t rv     = g_arkdCpu->gpr(2); // $v0 -> IOP ptr to reply data

    if (g_iopTraceOn && (!halted || g_arkdCpu->pc() != kHaltPc))
        dumpIopTrace("service func did not reach halt sentinel", g_arkdCpu->pc());
    if (arkdStateEnabled()) dumpArkdState("after service");

    if (diag)
    {
        std::fprintf(stderr,
            "[ARKD:diag] post-func BF50=0x%08x B300=0x%08x B310=0x%08x\n",
            g_arkdCpu->busRead32(g_arkdLoadBase + 0xBF50u),
            g_arkdCpu->busRead32(g_arkdLoadBase + 0xB300u),
            g_arkdCpu->busRead32(g_arkdLoadBase + 0xB310u));
    }

    // ---- S2.2c: if the dispatcher queued a worker job (dword_B310 != 0), run the
    // real worker body (sub_21B0 @ +0x21B0) inline now. On real HW SignalSema wakes
    // the worker thread; we have no scheduler, so we run it directly. It reads real
    // ISO sectors (sub_AC98 hook), DMAs them to the EE dest (sub_ADD4 hook), sets
    // dword_B300 to the completion status, clears dword_B310, then blocks on its
    // idle wait-sema -- where the WaitSema hook halts it (clean end). No faking.
    if (halted && g_arkdWorkerSema &&
        g_arkdCpu->busRead32(g_arkdLoadBase + 0xB310u) != 0u)
    {
        g_arkdCpu->setGpr(29, kIopStackTop - 0x9000u); // worker stack slice
        g_arkdCpu->setGpr(31, kHaltPc);
        g_arkdCpu->setHaltPc(kHaltPc);
        g_arkdCpu->setPC(g_arkdLoadBase + 0x21B0u);
        if (g_iopTraceOn) iopTraceReset();
        const uint32_t wret = g_arkdCpu->run(4u * 1000u * 1000u);
        static uint32_t s_wlogs = 0u;
        if (s_wlogs < 16u)
        {
            ++s_wlogs;
            const uint32_t wB300 = g_arkdCpu->busRead32(g_arkdLoadBase + 0xB300u);
            const uint32_t finalPc = g_arkdCpu->pc();
            // finalPC as a module-relative offset makes it directly cross-refable
            // against the ARKD decompile (sub_1A58/1DA4/1EB0/CdReadRetryLoop).
            // INCOMPLETE = worker halted with B300 still in the 0x20000000
            // in-progress band -> the transfer body was cut off before it ran
            // the sector read (0xAC98) / SIF DMA (0xADD4). That is the stall.
            const uint32_t band = wB300 & 0xF0000000u;
            std::fprintf(stderr,
                "[ARKD:worker] run: retired=%u finalPC=0x%08x off=+0x%06x halted=%d "
                "B300=%08x B310=%08x%s\n",
                wret, finalPc,
                (finalPc - g_arkdLoadBase) & 0x00FFFFFFu,
                g_arkdCpu->halted() ? 1 : 0, wB300,
                g_arkdCpu->busRead32(g_arkdLoadBase + 0xB310u),
                (band == 0x20000000u) ? "  <-- INCOMPLETE (mid-transfer)" : "");
        }
        if (g_iopTraceOn && (!g_arkdCpu->halted() || g_arkdCpu->pc() != kHaltPc))
            dumpIopTrace("worker did not reach halt sentinel", g_arkdCpu->pc());
        if (arkdStateEnabled()) dumpArkdState("after worker");
    }

    // Copy the func's reply into the guest recv buffer. Only on a clean halt with
    // a non-null reply pointer -- otherwise leave recvOut untouched and the
    // caller keeps its observe-only behavior (no faked data).
    bool delivered = false;
    if (halted && rv && recvOut && recvSize)
    {
        iopCopyOut(recvOut, rv, recvSize);
        delivered = true;
    }

    static uint32_t s_logs = 0u;
    if (s_logs < 32u)
    {
        ++s_logs;
        std::fprintf(stderr,
            "[ARKD:run] sid=0x%x func=0x%06x fno=0x%x buff=0x%06x ssz=0x%x -> "
            "retired=%u halted=%d $v0=0x%08x rsz=0x%x delivered=%d\n",
            sid, svc.func, rpcNum, buff, sendSize, retired, halted ? 1 : 0,
            rv, recvSize, delivered ? 1 : 0);
        if (delivered)
        {
            std::fprintf(stderr, "[ARKD:run] recv=");
            for (uint32_t b = 0; b < recvSize && b < 32u; ++b)
            {
                std::fprintf(stderr, "%02x ", recvOut[b]);
            }
            std::fprintf(stderr, "\n");
        }
    }

    return delivered;
}
