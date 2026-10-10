// EE-side answers for the IOP BIOS services that SDBZ's own guest-level SIF RPC
// client binds directly. ps2xIOP loads no rom0 modules, so nothing on the IOP
// registers these SIDs. Ported from the retired ps2_iop.cpp handleRpc (1429c0d9);
// protocol decodes are recorded there.
//   0x80000003 IOPHEAP    0x80000006 LOADFILE
//   0x80000592/593 cdvdman S-command   0x80000595 cdvdfsv N-command
//   0x80000597 cdvdfsv SearchFile
#include "Common.h"
#include "RPC.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

extern bool ps2_iop_cdSearchFile(const char *name, uint32_t *lbnOut, uint32_t *sizeOut);
extern bool ps2_iop_cdReadSectors(uint32_t lbn, uint32_t sectors, uint8_t *dst, size_t byteCount);

namespace ps2_syscalls
{
    namespace
    {
        void putWord(uint8_t *recv, uint32_t recvSize, uint32_t value)
        {
            if (recv && recvSize >= sizeof(uint32_t))
                std::memcpy(recv, &value, sizeof(value));
        }
    } // namespace

    uint32_t sdbzSifIopCmdBuffer(PS2Runtime *runtime)
    {
        // IOP-side SIF cmd buffer the guest reads via SifGetReg(0x80000000/1) and
        // DMAs command packets into. Nothing consumes the bytes; it only has to
        // be real IOP RAM that the physical emulator will not reuse.
        static std::mutex s_mutex;
        static uint32_t s_addr = 0u;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_addr == 0u && runtime)
            s_addr = runtime->allocateIopMemory(0x1000u, 64u);
        return s_addr;
    }

    // SDBZ's guest libsifcmd keeps its own command-handler tables; handlers the
    // game installs never pass through the runtime's sceSifAddCmdHandler stub.
    // Look an IOP->EE command up in the guest's _sif_cmd_data (built by
    // sceSifInitCmd 0x177B00 at 0x5616D8, ps2sdk layout: +12 sys handlers,
    // +16 sys count, +20 usr handlers, +24 usr count). This SDK's entries are
    // 12 bytes {fn, arg, pad}: 0x177B00 installs SET_SREG as fn@0x56170C.
    // ARKD reports async-load completion with SET_SREG, whose guest handler
    // stores into the sreg array the game polls.
    bool sdbzGuestSifCmdHandler(uint8_t *rdram, uint32_t commandId, uint32_t &function, uint32_t &argument)
    {
        constexpr uint32_t kCmdDataAddr = 0x5616D8u;
        constexpr uint32_t kSystemCmd = 0x80000000u;
        constexpr uint32_t kEntrySize = 12u;
        const auto rd32 = [&](uint32_t addr) -> uint32_t
        {
            const uint8_t *p = getConstMemPtr(rdram, addr);
            uint32_t v = 0u;
            if (p)
                std::memcpy(&v, p, sizeof(v));
            return v;
        };
        const bool system = (commandId & kSystemCmd) != 0u;
        const uint32_t id = commandId & ~kSystemCmd;
        const uint32_t table = rd32(kCmdDataAddr + (system ? 12u : 20u));
        const uint32_t count = rd32(kCmdDataAddr + (system ? 16u : 24u));
        if (table == 0u || table >= 0x02000000u || id >= count || count > 0x100u)
            return false;
        function = rd32(table + id * kEntrySize);
        argument = rd32(table + id * kEntrySize + 4u);
        static std::atomic<uint32_t> s_logs{0u};
        if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr, "[sif:guestcmd] cid=0x%08X -> guest handler fn=0x%08X arg=0x%08X\n", commandId,
                         function, argument);
        return function != 0u;
    }

    // SET_SREG (0x80000001) is re-sent by ARKD every IOP vblank. Its guest handler
    // 0x177A88 is exactly `sregs[pkt+16] = pkt+20` with sregs = [cmd_data+28], so do
    // that store here instead of queueing a guest invocation. The invocation path
    // needs a guestMalloc'd packet, and guestMalloc shares its base (0x640380) with
    // the game's own dlmalloc heap -- 180 packets/s were landing in live game blocks.
    bool sdbzGuestSifCmdInline(uint8_t *rdram, uint32_t commandId, const void *packet, size_t packetSize)
    {
        constexpr uint32_t kSetSreg = 0x80000001u;
        constexpr uint32_t kCmdDataAddr = 0x5616D8u;
        if (commandId != kSetSreg || packet == nullptr || packetSize < 24u)
            return false;
        uint32_t idx = 0u, value = 0u, sregs = 0u;
        std::memcpy(&idx, static_cast<const uint8_t *>(packet) + 16u, sizeof(idx));
        std::memcpy(&value, static_cast<const uint8_t *>(packet) + 20u, sizeof(value));
        if (const uint8_t *p = getConstMemPtr(rdram, kCmdDataAddr + 28u))
            std::memcpy(&sregs, p, sizeof(sregs));
        if (sregs == 0u || sregs >= 0x02000000u || idx >= 32u)
            return false;
        uint8_t *dst = getMemPtr(rdram, sregs + idx * 4u);
        if (!dst)
            return false;
        std::memcpy(dst, &value, sizeof(value));
        return true;
    }

    bool isGuestHleSystemSid(uint32_t sid)
    {
        return sid == 0x80000003u || sid == 0x80000006u || (sid >= 0x80000590u && sid <= 0x8000059Fu);
    }

    bool sdbzBiosHleRpc(uint8_t *rdram, PS2Runtime *runtime, uint32_t sid, uint32_t rpcNum, uint32_t sendBuf,
                        uint32_t sendSize, uint32_t recvBuf, uint32_t recvSize)
    {
        if (!isGuestHleSystemSid(sid))
            return false;

        uint8_t *sendPtr = sendBuf ? getMemPtr(rdram, sendBuf) : nullptr;
        uint8_t *recvPtr = recvBuf ? getMemPtr(rdram, recvBuf) : nullptr;

        if (sid == 0x80000003u) // IOPHEAP
        {
            uint32_t arg = 0u;
            if (sendPtr && sendSize >= sizeof(uint32_t))
                std::memcpy(&arg, sendPtr, sizeof(arg));
            uint32_t answer = 0u;
            if (rpcNum == 1u)
                answer = arg ? runtime->allocateIopMemory(arg, 64u) : 0u;
            else if (rpcNum == 2u)
                answer = runtime->freeIopMemory(arg) ? 0u : 0xFFFFFFFFu;
            putWord(recvPtr, recvSize, answer);
            static std::atomic<uint32_t> s_logs{0u};
            if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 32u)
                std::fprintf(stderr, "[iop:iopheap] fno=%u arg=0x%08X -> 0x%08X\n", rpcNum, arg, answer);
            return true;
        }

        if (sid == 0x80000006u) // LOADFILE
        {
            if (!recvPtr || recvSize < sizeof(uint32_t))
                return false;
            if (rpcNum == 255u)
            {
                putWord(recvPtr, recvSize, 0x30303033u); // "3000"
                return true;
            }
            if (rpcNum == 0u && sendPtr && runtime)
            {
                // ps2sdk layout: path at +8; result id at recv+0, start result at recv+4.
                const std::string path = readGuestCStringBounded(rdram, sendBuf + 8u, 252u);
                const auto loaded = runtime->loadIopModule(path, nullptr, 0u);
                const int32_t id = (loaded.handled && loaded.moduleId > 0) ? loaded.moduleId : -1;
                const int32_t res = loaded.handled ? loaded.startResult : -1;
                std::memcpy(recvPtr, &id, sizeof(id));
                if (recvSize >= 8u)
                    std::memcpy(recvPtr + 4, &res, sizeof(res));
                std::fprintf(stderr, "[iop:loadfile] path='%s' id=%d start=%d\n", path.c_str(), id, res);
                return true;
            }
            std::fprintf(stderr, "[iop:loadfile] UNHANDLED fno=%u send=0x%08X/%u recv=0x%08X/%u\n", rpcNum, sendBuf,
                         sendSize, recvBuf, recvSize);
            return false;
        }

        if (sid == 0x80000597u) // cdvdfsv SearchFile
        {
            // send: +0x00 sceCdlFILE out {lsn,size,name[16],date[8]}, +0x24 char name[256]
            constexpr uint32_t kNameOff = 0x24u;
            std::string wanted;
            if (sendPtr && sendSize >= kNameOff + 2u)
            {
                const char *raw = reinterpret_cast<const char *>(sendPtr + kNameOff);
                const uint32_t maxLen = std::min<uint32_t>(sendSize - kNameOff, 256u);
                for (uint32_t i = 0; i < maxLen && raw[i] != '\0'; ++i)
                    wanted.push_back(raw[i]);
            }
            uint32_t lbn = 0u, size = 0u;
            const bool found = !wanted.empty() && ps2_iop_cdSearchFile(wanted.c_str(), &lbn, &size);
            if (found && sendPtr && sendSize >= 36u)
            {
                std::memset(sendPtr, 0, 36u);
                std::memcpy(sendPtr + 0, &lbn, sizeof(lbn));
                std::memcpy(sendPtr + 4, &size, sizeof(size));
                std::string leaf = wanted;
                const size_t cut = leaf.find_last_of("\\/");
                if (cut != std::string::npos)
                    leaf = leaf.substr(cut + 1);
                std::memcpy(sendPtr + 8, leaf.data(), std::min<size_t>(leaf.size(), 15u));
            }
            putWord(recvPtr, recvSize, found ? 1u : 0u);
            static std::atomic<uint32_t> s_logs{0u};
            if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 32u)
                std::fprintf(stderr, "[iop:cdsearch] name=\"%s\" found=%u lbn=%u size=%u\n",
                             wanted.empty() ? "<unparsed>" : wanted.c_str(), found ? 1u : 0u, lbn, size);
            return true;
        }

        if (sid == 0x80000595u) // cdvdfsv N-command
        {
            constexpr uint32_t kNcmdRead = 1u;
            constexpr uint32_t kNcmdStatus = 14u;
            constexpr uint32_t kCdStatSpin = 0x02u; // never 6 (PAUSE)
            if (rpcNum == kNcmdRead && sendPtr && sendSize >= 0x10u)
            {
                uint32_t lbn = 0u, sectors = 0u, bufAddr = 0u;
                std::memcpy(&lbn, sendPtr + 0x00, 4u);
                std::memcpy(&sectors, sendPtr + 0x04, 4u);
                std::memcpy(&bufAddr, sendPtr + 0x08, 4u);
                const uint8_t modeSize = sendPtr[0x0E];
                const uint32_t secSize = (modeSize == 1u) ? 2328u : (modeSize == 2u) ? 2340u : 2048u;
                const uint64_t bytes = static_cast<uint64_t>(sectors) * secSize;
                uint8_t *dst = (bufAddr != 0u && sectors != 0u && bytes <= 0x02000000ull) ? getMemPtr(rdram, bufAddr)
                                                                                          : nullptr;
                bool ok = false;
                if (dst)
                    ok = ps2_iop_cdReadSectors(lbn, sectors, dst, static_cast<size_t>(bytes));
                static std::atomic<uint32_t> s_logs{0u};
                if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 48u)
                    std::fprintf(stderr, "[iop:ncmd] read lbn=%u sectors=%u buf=0x%08X ok=%u\n", lbn, sectors, bufAddr,
                                 ok ? 1u : 0u);
                return true;
            }
            if (rpcNum == kNcmdStatus)
            {
                putWord(recvPtr, recvSize, kCdStatSpin);
                return true;
            }
            return false;
        }

        if (sid == 0x80000592u || sid == 0x80000593u) // cdvdman S-command
        {
            uint32_t word0 = 1u;
            switch (rpcNum)
            {
            case 4u: // GetError
                word0 = 0u;
                break;
            case 12u: // Status
                word0 = 0x02u;
                break;
            default:
                break;
            }
            if (recvPtr && recvSize > 0u)
            {
                std::memset(recvPtr, 0, recvSize);
                putWord(recvPtr, recvSize, word0);
            }
            static std::mutex s_mutex;
            static std::set<uint64_t> s_seen;
            std::lock_guard<std::mutex> lk(s_mutex);
            if (s_seen.insert((static_cast<uint64_t>(sid) << 32) | rpcNum).second)
                std::fprintf(stderr, "[iop:cdscmd] sid=0x%08X fno=%u -> word0=0x%08X recv=0x%08X/%u send=0x%08X/%u\n",
                             sid, rpcNum, word0, recvBuf, recvSize, sendBuf, sendSize);
            return true;
        }

        static std::atomic<uint32_t> s_logs{0u};
        if (s_logs.fetch_add(1u, std::memory_order_relaxed) < 32u)
            std::fprintf(stderr, "[iop:bios] UNHANDLED sid=0x%08X fno=%u send=0x%08X/%u recv=0x%08X/%u\n", sid, rpcNum,
                         sendBuf, sendSize, recvBuf, recvSize);
        return false;
    }
} // namespace ps2_syscalls
