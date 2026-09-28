// iop_harness.cpp
// -----------------------------------------------------------------------------
// Standalone, headless driver for the embedded IOP R3000 + the real
// ARKD_DVD.IRX -- WITHOUT booting the EE game. This turns a 60-90s full-game
// launch into a sub-second run so the worker-thread / semaphore / service-func
// logic (S2.2c/d/e) can be iterated in tight loops.
//
// It does the minimum the loader needs: brings up PS2Memory (IOP RAM), points
// the CD root at the directory holding ARKD_DVD.IRX, forces the loader's
// _start + server-thread run (PS2_ARKD_IRX_RUN=1), then issues one or more
// synthetic sid-0x503 CALLs through the same ps2_iop_runArkdService bridge the
// game's SIF.cpp uses. No IOP output is faked -- everything comes from the real
// module code (honors feedback_no_iop_faking).
//
// Usage:
//   iop_harness [cdRoot] [fno] [iterations]
//     cdRoot      dir containing ARKD_DVD.IRX  (default: F:/SDBZ Recomp/ELF)
//     fno         RPC function number for the CALL (default: 0)
//     iterations  how many CALLs to issue      (default: 1)
//
// Recommended env while running:
//   PS2_IOP_TRACE=1   dump last-N executed PCs on any unexpected halt
//   PS2_ARKD_STATE=1  dump the module job-state words before/after each run
// (PS2_ARKD_IRX_RUN is set to 1 by this harness automatically.)
// -----------------------------------------------------------------------------

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

// The loader/service entry points live in ps2_iop_irx_loader.cpp. No header is
// touched (project rule): declare the externs locally, exactly as SIF.cpp does.
extern bool ps2_iop_loadArkdIrx(PS2Runtime *runtime, const std::string &modulePath);
extern bool ps2_iop_runArkdService(PS2Runtime *runtime,
                                   uint32_t sid, uint32_t rpcNum,
                                   const uint8_t *sendData, uint32_t sendSize,
                                   uint8_t *recvOut, uint32_t recvSize);

// The generated runner (register_functions.cpp) normally defines the EE
// dispatch table. This headless harness never links the runner objects and
// never dispatches an EE recompiled function -- it only drives the IOP R3000.
// ps2_runtime.lib references these symbols, so provide empty definitions.
// SlotCount==0 short-circuits every table access in ps2_runtime.cpp, so an
// empty table is safe (see generatedFunctionTableSlot / hasFunction guards).
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
const uint32_t g_ps2RecompiledFunctionTableBase      = 0u;
const uint32_t g_ps2RecompiledFunctionTableEnd       = 0u;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

static void setEnv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(int argc, char **argv)
{
    const std::string cdRoot     = (argc > 1) ? argv[1] : "F:/SDBZ Recomp/ELF";
    const uint32_t    fno        = (argc > 2) ? uint32_t(std::strtoul(argv[2], nullptr, 0)) : 0u;
    const uint32_t    iterations = (argc > 3) ? uint32_t(std::strtoul(argv[3], nullptr, 0)) : 1u;
    const std::string sendHex    = (argc > 4) ? argv[4] : ""; // real request bytes (hex)
    constexpr uint32_t kSid = 0x503u; // steady-state DVD-read service

    if (!std::filesystem::exists(std::filesystem::path(cdRoot) / "ARKD_DVD.IRX"))
    {
        std::fprintf(stderr, "[harness] ARKD_DVD.IRX not found under: %s\n", cdRoot.c_str());
        std::fprintf(stderr, "[harness] pass the containing dir as arg 1.\n");
        return 2;
    }

    // Force the loader to run _start + server threads so g_arkdServices is
    // populated (same gate the game uses). Set BEFORE loadArkdIrx.
    setEnv("PS2_ARKD_IRX_RUN", "1");

    // Headless bring-up: PS2Memory only (IOP RAM). Deliberately NOT
    // PS2Runtime::initialize() -- that opens a raylib window + audio device.
    PS2Runtime runtime;
    if (!runtime.memory().initialize())
    {
        std::fprintf(stderr, "[harness] PS2Memory::initialize() failed\n");
        return 3;
    }

    // Point the loader's path resolver at the module directory.
    PS2Runtime::IoPaths io;
    io.cdRoot       = cdRoot;
    io.elfDirectory = cdRoot;
    PS2Runtime::setIoPaths(io);

    std::fprintf(stderr, "[harness] loading ARKD_DVD.IRX from %s ...\n", cdRoot.c_str());
    if (!ps2_iop_loadArkdIrx(&runtime, "ARKD_DVD.IRX"))
    {
        std::fprintf(stderr, "[harness] ps2_iop_loadArkdIrx failed\n");
        return 4;
    }

    // DVD-read request. The steady-state CALL uses a 0x10-byte recv buffer. The
    // send buffer is the game's real request header (captured from the [ARKD:CALL]
    // send[0..48] trace); pass it as arg 4 = hex (spaces/0x optional) so the real
    // func parses real params instead of bailing on a zeroed request. Falls back
    // to a zeroed 0x10-byte send if no hex is given.
    // Tokenize on whitespace/commas; each maximal run of hex digits = one byte.
    // Matches the [ARKD:CALL] send[0..48] log format ("40 0 0 0 a 0 0 80 ...").
    std::vector<uint8_t> send;
    {
        std::string tok;
        auto flush = [&]() {
            if (!tok.empty()) { send.push_back(uint8_t(std::strtoul(tok.c_str(), nullptr, 16))); tok.clear(); }
        };
        for (char c : sendHex)
        {
            if (std::isxdigit(static_cast<unsigned char>(c))) tok.push_back(c);
            else flush();
        }
        flush();
    }
    if (send.empty()) send.assign(0x10, 0);
    std::vector<uint8_t> recv(0x10, 0);
    std::fprintf(stderr, "[harness] send buffer = %zu byte(s)\n", send.size());

    int delivered = 0;
    for (uint32_t i = 0; i < iterations; ++i)
    {
        std::fprintf(stderr, "[harness] --- CALL %u/%u sid=0x%x fno=0x%x ---\n",
                     i + 1, iterations, kSid, fno);
        const bool ok = ps2_iop_runArkdService(&runtime, kSid, fno,
                                               send.data(), uint32_t(send.size()),
                                               recv.data(), uint32_t(recv.size()));
        if (ok) ++delivered;
    }

    std::fprintf(stderr, "[harness] done: %d/%u CALL(s) delivered a reply.\n",
                 delivered, iterations);
    return delivered > 0 ? 0 : 1;
}
