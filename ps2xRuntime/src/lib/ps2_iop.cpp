#include <atomic>
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

        // Intentionally fall through (return false) -- Phase 0 is diagnostics only.
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
