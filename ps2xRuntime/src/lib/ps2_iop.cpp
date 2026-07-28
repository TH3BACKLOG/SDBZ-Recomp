#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/ps2_iop.h"
#include "runtime/ps2_iop_audio.h"
#include "runtime/ps2_iop_cl.h"
#include "runtime/ps2_iop_dbcman.h"
#include "runtime/ps2_iop_mcman.h"
#include "runtime/ps2_iop_sdrdrv.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "Kernel/Syscalls/RPC.h"

// S2.1: real ARKD_DVD.IRX loader lives in ps2_iop_irx_loader.cpp. Declared here
// (no header touched) so the LOADFILE path can hand it the module when the game
// asks for it. Loads + relocates the IRX into IOP RAM and builds the import map.
extern bool ps2_iop_loadArkdIrx(PS2Runtime *runtime, const std::string &modulePath);

ps2_iop::ps2_iop()
{
    reset();
}

void ps2_iop::init(uint8_t *rdram)
{
    m_rdram = rdram;
}

void ps2_iop::reset()
{
    ps2_iop_cl::reset();
    ps2_iop_dbcman::reset();
    ps2_iop_sdrdrv::reset();
}

bool ps2_iop::handleRPC(PS2Runtime *runtime,
                        uint32_t sid,
                        uint32_t rpcNum,
                        uint32_t sendBufAddr,
                        uint32_t sendSize,
                        uint32_t recvBufAddr,
                        uint32_t recvSize,
                        uint32_t &resultPtr,
                        bool &signalNowaitCompletion)
{
    resultPtr = 0u;
    signalNowaitCompletion = false;

    if (ps2_syscalls::handleSoundDriverRpcService(m_rdram,
                                                  runtime,
                                                  sid,
                                                  rpcNum,
                                                  sendBufAddr,
                                                  sendSize,
                                                  recvBufAddr,
                                                  recvSize,
                                                  resultPtr,
                                                  signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_dbcman::handleDbcManRpc(m_rdram,
                                        sid,
                                        rpcNum,
                                        sendBufAddr,
                                        sendSize,
                                        recvBufAddr,
                                        recvSize,
                                        resultPtr))
    {
        return true;
    }

    if (ps2_iop_audio::handleLibSdRpc(m_rdram,
                                      runtime,
                                      sid,
                                      rpcNum,
                                      sendBufAddr,
                                      sendSize,
                                      recvBufAddr,
                                      recvSize,
                                      resultPtr))
    {
        return true;
    }

    if (ps2_iop_cl::handleSoundRpc(m_rdram, sid,
                                   rpcNum, sendBufAddr,
                                   sendSize, recvBufAddr,
                                   recvSize, resultPtr,
                                   signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_cl::handleClFileRpc(m_rdram, sid,
                                    rpcNum, sendBufAddr,
                                    sendSize, recvBufAddr,
                                    recvSize, resultPtr))
    {
        return true;
    }

    if (ps2_iop_sdrdrv::handleSdrdrvRpc(m_rdram, sid,
                                        rpcNum, sendBufAddr,
                                        sendSize, recvBufAddr,
                                        recvSize, resultPtr))
    {
        return true;
    }

    if (ps2_iop_mcman::handleMcServRpc(m_rdram, sid,
                                       rpcNum, sendBufAddr,
                                       sendSize, recvBufAddr,
                                       recvSize, resultPtr))
    {
        return true;
    }

    if (sid == 0x80000006u) // LOADFILE (IOP loadfile module SIF RPC service)
    {
        // Phase 0 diagnostics ONLY: the game's sceSifLoadModule binds to this
        // service and calls it; we have no handler, so the EE lib returns
        // -65540 and the boot spins. Log the exact packet (rpc function number,
        // send-buffer bytes, and the embedded module path) so the IRX loader can
        // be built against the real arg layout. Still returns false -> no
        // behavioral change this phase.
        static std::atomic<uint32_t> s_loadfileLogs{0u};
        if (s_loadfileLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:LOADFILE] rpcNum=%u sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);

            const uint8_t *snd = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
            if (snd && sendSize > 0u)
            {
                const uint32_t dumpLen = sendSize < 96u ? sendSize : 96u;
                std::fprintf(stderr, "[iop:LOADFILE] send[");
                for (uint32_t i = 0; i < dumpLen; i++)
                {
                    std::fprintf(stderr, "%02X", snd[i]);
                    if ((i & 3u) == 3u)
                    {
                        std::fprintf(stderr, " ");
                    }
                }
                std::fprintf(stderr, "]\n");

                // Extract the first printable ASCII run (>=3 chars) as the path.
                std::string path;
                for (uint32_t i = 0; i < sendSize; i++)
                {
                    const char c = static_cast<char>(snd[i]);
                    if (c >= 0x20 && c < 0x7F)
                    {
                        path.push_back(c);
                        if (path.size() >= 128u)
                        {
                            break;
                        }
                    }
                    else if (path.size() >= 3u)
                    {
                        break;
                    }
                    else
                    {
                        path.clear();
                    }
                }
                std::fprintf(stderr, "[iop:LOADFILE] path=\"%s\"\n", path.c_str());
            }
        }

        // S2.1: if this LOADFILE is for ARKD_DVD.IRX, load the real module into
        // IOP RAM (once). Independent of the bounded diagnostic logging above so
        // it still fires after the 32-log cap. Extracts the module path fresh
        // from the send buffer (first printable ASCII run, same scan as above).
        static std::atomic<bool> s_arkdLoadTried{false};
        bool expected = false;
        if (s_arkdLoadTried.compare_exchange_strong(expected, true))
        {
            const uint8_t *snd2 = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
            if (snd2 && sendSize > 0u)
            {
                std::string path;
                for (uint32_t i = 0; i < sendSize; i++)
                {
                    const char c = static_cast<char>(snd2[i]);
                    if (c >= 0x20 && c < 0x7F)
                    {
                        path.push_back(c);
                        if (path.size() >= 128u) break;
                    }
                    else if (path.size() >= 3u) break;
                    else path.clear();
                }
                std::string upper = path;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (upper.find("ARKD_DVD") != std::string::npos)
                {
                    ps2_iop_loadArkdIrx(runtime, path);
                }
                else
                {
                    s_arkdLoadTried.store(false); // wasn't ARKD -- allow a later try
                }
            }
        }

        // Intentionally fall through (return false) -- Phase 0 is diagnostics only.
    }

    // Stage 5.5.1 PROBE: padman (libpad) RPC service.
    //
    // Ground truth read live from PCSX2 (SLUS-21442, paused at the memcard screen):
    //   0x187cb0  binds sid 0x80000100 with client data 0x568940
    //   0x187d04  binds sid 0x80000101 with client data 0x568968
    //   0x189110  version query -- rpcNum 1, send==recv==0x568b80 (0x80 bytes),
    //             cmd word 0x12 at send+0, module version read back from recv+0xC.
    //   0x187d34  sra $s0,$s1,8 ; beq $s0,4 -- ONLY the major nibble is checked,
    //             and the full version word is dead afterwards (0x187d78 passes
    //             only $a0 = $s2), so 0x0400 is behaviourally exact.
    //
    // Built up one command at a time, each round driven by what the previous run
    // logged as UNSERVED, so every reply below is decoded from a real call site
    // rather than guessed: 0x12 (version) -> 0x10 (init) -> 0x01 (port open).
    // Still not served: the per-frame pad state itself, which the real padman
    // pushes asynchronously via SIF CMD 0x80000019 (the guest registers its own
    // handler, callback 0x187d98, on a successful cmd 0x10).
    if (sid == 0x80000100u && rpcNum == 1u)
    {
        const uint8_t *snd = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint32_t cmd = 0u;
        if (snd && sendSize >= sizeof(uint32_t))
        {
            std::memcpy(&cmd, snd, sizeof(cmd));
        }

        // Reply convention for this service, read off three call sites:
        //   0x189110 (cmd 0x12) returns recv+0xC
        //   0x187de0 (cmd 0x10) returns recv+0xC
        //   0x187ed8 (cmd 0x0F) compares recv+0xC against 1
        // So recv+0xC is the single return slot, and 1 means success.
        uint32_t reply = 0u;
        uint32_t reply14 = 0u;      // recv+0x14, only meaningful for cmd 0x1
        bool haveReply14 = false;
        const char *replyWhat = nullptr;

        switch (cmd)
        {
        case 0x12u: // scePadGetModVersion
            // Major nibble only is inspected (0x187d34: sra $s0,$s1,8 ; beq $s0,4)
            // and the full word is dead afterwards, so 0x0400 is behaviourally exact.
            reply = 0x0400u;
            replyWhat = "modversion";
            break;

        case 0x10u: // scePadInit's open -- send+0x10 = 0, send+0x14 = global 0x464E14
            // On a non-negative rpc_call the guest itself registers the async pad
            // handler (0x177de8 with cmd 0x80000019 -> callback 0x187d98), so we
            // only have to stop returning a failure here. Pad state pushes are a
            // separate piece and are still not served.
            reply = 1u;
            replyWhat = "init";
            break;

        case 0x1u: // scePadPortOpen (0x1880e4)
        {
            // Send layout read off 0x18818c-0x1881a0:
            //   send+0x04 = port, send+0x08 = slot, send+0x10 = EE-side pad buffer.
            // Reply: recv+0xC is the return value (0x188230 lw $v0,12($s0)) and
            // recv+0x14 is an IOP-side address (0x188204 lw $t0,20($s0)) stored into
            // the pad state table at +8. scePadRead (0x187f68) later uses that word
            // as the sceSifSetDma *dest* for a 0x20-byte EE->IOP request, indexing
            // it as a double buffer (+0x20 on alternate frames). So it must be
            // non-zero and must have 0x40 bytes of room; the EE side never reads it.
            uint32_t port = 0u, slot = 0u;
            if (snd && sendSize >= 0xCu)
            {
                std::memcpy(&port, snd + 0x4, sizeof(port));
                std::memcpy(&slot, snd + 0x8, sizeof(slot));
            }
            // Distinctive base so these show up unmistakably in SIF DMA logs.
            reply14 = 0x001E0000u + ((port & 0xFu) * 0x100u) + ((slot & 0x3u) * 0x40u);
            haveReply14 = true;
            reply = 1u;
            replyWhat = "portopen";
            break;
        }

        default:
            break;
        }

        if (replyWhat)
        {
            uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;
            if (recvPtr && recvSize >= 0x10u)
            {
                // Safe to wipe even though send == recv for this service: cmd was
                // already latched above and nothing reads the send words after.
                std::memset(recvPtr, 0, recvSize);
                std::memcpy(recvPtr + 0xC, &reply, sizeof(reply));
                if (haveReply14 && recvSize >= 0x18u)
                {
                    std::memcpy(recvPtr + 0x14, &reply14, sizeof(reply14));
                }
            }

            static std::atomic<uint32_t> s_padServedLogs{0u};
            if (s_padServedLogs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            {
                std::fprintf(stderr,
                             "[iop:PADMAN] served cmd=0x%X (%s): recvBuf=0x%08X recvSize=%u reply=0x%X iopbuf=0x%08X\n",
                             cmd, replyWhat, recvBufAddr, recvSize, reply,
                             haveReply14 ? reply14 : 0u);
            }

            resultPtr = recvBufAddr;
            return true;
        }

        static std::atomic<uint32_t> s_padOtherLogs{0u};
        if (s_padOtherLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:PADMAN] UNSERVED cmd=0x%X sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         cmd, sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
        // Fall through unhandled -- the caller treats that as a failed RPC.
    }
    else if (sid == 0x80000101u)
    {
        static std::atomic<uint32_t> s_pad101Logs{0u};
        if (s_pad101Logs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:PADMAN] sid 0x80000101 rpcNum=%u sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
    }

    if (sid == IOP_SID_CDVD_SCMD)
    {
        // cdvdman S-command RPC. The EE libcdvd wrappers read the result buffer's first
        // word as a success flag (non-zero == ok) and the following words as outputs.
        // We serve disc S-commands directly via the sceCd* stubs, so here we just report
        // a benign success: word[0]=1, remaining words zeroed. For sceCdReadDvdDualInfo
        // this means "succeeded, single-layer (on_dual=0, layer1_start=0)", which is the
        // correct answer for a flat single-image disc and lets boot proceed.
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;
        if (recvPtr && recvSize > 0)
        {
            std::memset(recvPtr, 0, recvSize);
            if (recvSize >= sizeof(uint32_t))
            {
                const uint32_t okFlag = 1u;
                std::memcpy(recvPtr, &okFlag, sizeof(okFlag));
            }
        }
        resultPtr = recvBufAddr;
        return true;
    }

    return false;
}
