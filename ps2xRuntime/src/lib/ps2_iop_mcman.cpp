#include "runtime/ps2_iop_mcman.h"
#include "runtime/ps2_iop.h"
#include "runtime/ps2_memory.h"
#include "Kernel/Stubs/MemoryCard_Internal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>

using namespace ps2_stubs::mc_internal;

namespace
{
    // sceMcServ RPC command numbers (PS2 SDK libmc wire format). These match
    // the kMcCmd* values in MemoryCard.cpp -- the RPC path and the direct
    // syscall-style sceMc* stubs both drive the same commands, just with
    // packed sendBuf/recvBuf args here instead of ctx registers.
    constexpr uint32_t kRpcGetInfo = 0x01u;
    constexpr uint32_t kRpcOpen = 0x02u;
    constexpr uint32_t kRpcClose = 0x03u;
    constexpr uint32_t kRpcSeek = 0x04u;
    constexpr uint32_t kRpcRead = 0x05u;
    constexpr uint32_t kRpcWrite = 0x06u;
    constexpr uint32_t kRpcFlush = 0x0Au;
    constexpr uint32_t kRpcMkdir = 0x0Bu;
    constexpr uint32_t kRpcChdir = 0x0Cu;
    constexpr uint32_t kRpcGetDir = 0x0Du;
    constexpr uint32_t kRpcSetFileInfo = 0x0Eu;
    constexpr uint32_t kRpcDelete = 0x0Fu;
    constexpr uint32_t kRpcFormat = 0x10u;
    constexpr uint32_t kRpcUnformat = 0x11u;
    constexpr uint32_t kRpcGetEntSpace = 0x12u;
    constexpr uint32_t kRpcRename = 0x13u;
    // libmc sceMcInit handshake (game EE 0x189318 calls fnum 0xFE with a 12-byte
    // recv buffer): recv+0 = result, recv+4 = mcserv version (must be >= 0x20A),
    // recv+8 = mcman version (must be >= 0x20E) or libmc prints
    // "too old release of mcserv.irx"/"mcman.irx" and disables the memory card.
    constexpr uint32_t kRpcInit = 0xFEu;
    constexpr int32_t kMcServVersion = 0x20A;
    constexpr int32_t kMcManVersion = 0x20E;

    bool isMcInitBypassEnabled()
    {
        const char *env = std::getenv("PS2X_MC_BYPASS_INIT_CHECK");
        return env && env[0] != '\0' && env[0] != '0';
    }

    bool mcservTraceEnabled()
    {
        const char *env = std::getenv("PS2X_MCSERV_TRACE");
        return env && env[0] != '\0' && env[0] != '0';
    }

    bool readGuestU32(const uint8_t *rdram, uint32_t addr, uint32_t &out)
    {
        const uint8_t *ptr = getConstMemPtr(rdram, addr);
        if (!ptr)
        {
            return false;
        }
        std::memcpy(&out, ptr, sizeof(out));
        return true;
    }

    void writeResultI32(uint8_t *rdram, uint32_t recvBufAddr, int32_t result)
    {
        if (recvBufAddr == 0u)
        {
            return;
        }
        if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
        {
            std::memcpy(dst, &result, sizeof(result));
        }
    }

    std::string readGuestPath(const uint8_t *rdram, uint32_t addr)
    {
        if (addr == 0u)
        {
            return {};
        }
        const char *ptr = reinterpret_cast<const char *>(getConstMemPtr(rdram, addr));
        if (!ptr)
        {
            return {};
        }
        return std::string(ptr, strnlen(ptr, kMcMaxPathLen));
    }
}

namespace ps2_iop_mcman
{
    bool handleMcServRpc(uint8_t *rdram,
                         uint32_t sid,
                         uint32_t rpcNum,
                         uint32_t sendBufAddr,
                         uint32_t sendSize,
                         uint32_t recvBufAddr,
                         uint32_t recvSize,
                         uint32_t &resultPtr)
    {
        const bool traceMcserv = mcservTraceEnabled();
        if (sid != IOP_SID_MCSERV && sid != IOP_SID_MCSERV_LEGACY)
        {
            if (traceMcserv && sid == 0x80000400u)
            {
                static std::atomic<uint32_t> s_sidRejectLogs{0u};
                if (s_sidRejectLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
                {
                    std::fprintf(stderr,
                                 "[iop:mcserv] sid reject sid=0x%08X rpc=0x%X expected=0x%08X/0x%08X\n",
                                 sid, rpcNum, IOP_SID_MCSERV, IOP_SID_MCSERV_LEGACY);
                }
            }
            return false;
        }

        resultPtr = recvBufAddr;

        // sendBuf layout for the port/slot-taking commands: word0=port, word1=slot,
        // followed by command-specific args (path string, flags, etc.) -- matches
        // the PS2 SDK libmc RPC client's argument packing.
        uint32_t port = 0u;
        uint32_t slot = 0u;
        readGuestU32(rdram, sendBufAddr + 0x00u, port);
        readGuestU32(rdram, sendBufAddr + 0x04u, slot);

        std::lock_guard<std::mutex> lock(g_mcStateMutex);

        switch (rpcNum)
        {
        case kRpcInit:
        {
            if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
            {
                const int32_t reply[3] = {kMcResultSucceed, kMcServVersion, kMcManVersion};
                std::memcpy(dst, reply, sizeof(reply));
            }
            std::fprintf(stderr,
                         "[iop:mcserv] Init (fnum 0xFE) -> result=0 mcservVer=0x%X mcmanVer=0x%X recv=0x%08X\n",
                         kMcServVersion, kMcManVersion, recvBufAddr);
            setMcCommandResultLocked(static_cast<int32_t>(kRpcInit), kMcResultSucceed);
            resultPtr = recvBufAddr;
            return true;
        }

        case kRpcOpen:
        {
            uint32_t flags = 0u;
            readGuestU32(rdram, sendBufAddr + 0x08u, flags);
            const std::string guestPath = readGuestPath(rdram, sendBufAddr + 0x0Cu);

            int32_t result = kMcResultNoEntry;
            if (isValidMcPortSlot(static_cast<int32_t>(port), static_cast<int32_t>(slot)))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string normalized =
                        normalizeGuestMcPathLocked(static_cast<int32_t>(port), guestPath);
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(static_cast<int32_t>(port), normalized);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

                    if (normalized == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, static_cast<int32_t>(port), hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcOpen), result);
            writeResultI32(rdram, recvBufAddr, result);
            return true;
        }

        case kRpcClose:
        {
            uint32_t fd = 0u;
            readGuestU32(rdram, sendBufAddr + 0x00u, fd);

            int32_t result = kMcResultNoEntry;
            auto it = g_mcFiles.find(static_cast<int32_t>(fd));
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcClose), result);
            writeResultI32(rdram, recvBufAddr, result);
            return true;
        }

        case kRpcRead:
        {
            uint32_t fd = 0u;
            uint32_t size = 0u;
            readGuestU32(rdram, sendBufAddr + 0x00u, fd);
            readGuestU32(rdram, sendBufAddr + 0x04u, size);

            int32_t result = kMcResultNoEntry;
            auto it = g_mcFiles.find(static_cast<int32_t>(fd));
            if (size == 0u)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
            {
                const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                if (std::ferror(it->second.file))
                {
                    std::clearerr(it->second.file);
                }
            }
            else
            {
                result = kMcResultDeniedPermit;
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcRead), result);
            // Read returns byte count via IOP RPC result field, not the recv buffer
            // (recv buffer IS the destination data). Result is reported through resultPtr.
            resultPtr = recvBufAddr;
            (void)result;
            return true;
        }

        case kRpcWrite:
        {
            uint32_t fd = 0u;
            uint32_t size = 0u;
            readGuestU32(rdram, sendBufAddr + 0x00u, fd);
            readGuestU32(rdram, sendBufAddr + 0x04u, size);
            const uint8_t *src = (size > 0u) ? getConstMemPtr(rdram, sendBufAddr + 0x08u) : nullptr;

            int32_t result = kMcResultNoEntry;
            auto it = g_mcFiles.find(static_cast<int32_t>(fd));
            if (size == 0u)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                if (!std::ferror(it->second.file))
                {
                    std::fflush(it->second.file);
                }
                else
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcWrite), result);
            writeResultI32(rdram, recvBufAddr, result);
            return true;
        }

        case kRpcGetInfo:
        {
            int32_t cardType = 0;
            int32_t freeBlocks = 0;
            int32_t format = kMcUnformatted;
            int32_t result = kMcResultNoEntry;

            if (isValidMcPortSlot(static_cast<int32_t>(port), static_cast<int32_t>(slot)))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                cardType = kMcTypePs2;
                freeBlocks = state.formatted ? kMcFreeClusters : 0;
                format = state.formatted ? kMcFormatted : kMcUnformatted;
                result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcGetInfo), result);
            if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
            {
                if (recvSize >= 12u)
                {
                    std::memcpy(dst + 0u, &cardType, sizeof(cardType));
                    std::memcpy(dst + 4u, &freeBlocks, sizeof(freeBlocks));
                    std::memcpy(dst + 8u, &format, sizeof(format));
                }
            }
            resultPtr = recvBufAddr;
            return true;
        }

        case kRpcGetDir:
        {
            const std::string rawPath = readGuestPath(rdram, sendBufAddr + 0x08u);
            uint32_t maxEntries = 0u;
            readGuestU32(rdram, sendBufAddr + 0x08u + static_cast<uint32_t>(kMcMaxPathLen), maxEntries);

            std::vector<SceMcTblGetDir> entries;
            int32_t result = kMcResultNoEntry;

            if (isValidMcPortSlot(static_cast<int32_t>(port), static_cast<int32_t>(slot)))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(static_cast<int32_t>(port));
                    const std::string guestQuery = normalizeGuestMcPathLocked(
                        static_cast<int32_t>(port), rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    const std::filesystem::path queryRel =
                        (guestQuery.size() > 1u) ? std::filesystem::path(guestQuery.substr(1)) : std::filesystem::path{};

                    std::filesystem::path parentRel;
                    std::string pattern;
                    if (hasWildcard)
                    {
                        parentRel = queryRel.parent_path();
                        pattern = queryRel.filename().string();
                    }
                    else
                    {
                        const std::filesystem::path queryHostPath =
                            guestMcPathToHostPath(static_cast<int32_t>(port), guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentRel = queryRel;
                            pattern = "*";
                        }
                        else
                        {
                            parentRel = queryRel.parent_path();
                            pattern = queryRel.filename().string();
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    std::filesystem::path hostDir = getMcRootPath(static_cast<int32_t>(port));
                    if (!parentRel.empty())
                    {
                        hostDir /= parentRel;
                    }
                    hostDir = hostDir.lexically_normal();

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = std::time(nullptr);
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        std::vector<std::filesystem::directory_entry> dirEntries;
                        for (const auto &entry : std::filesystem::directory_iterator(
                                 hostDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (ec)
                            {
                                break;
                            }
                            dirEntries.push_back(entry);
                        }

                        std::sort(dirEntries.begin(), dirEntries.end(),
                                  [](const std::filesystem::directory_entry &lhs,
                                     const std::filesystem::directory_entry &rhs)
                                  {
                                      return lhs.path().filename().string() <
                                             rhs.path().filename().string();
                                  });

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = entry.path().filename().string();
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t modifiedTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry, name, isDirectory, sizeBytes,
                                                entryEc ? now : modifiedTime);
                            entries.push_back(tableEntry);
                        }

                        const size_t entryCount =
                            std::min(entries.size(), maxEntries > 0u ? static_cast<size_t>(maxEntries) : 0u);
                        if (entryCount == 0u || recvBufAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
                        {
                            const size_t maxBytes = std::min<size_t>(entryCount * sizeof(SceMcTblGetDir), recvSize);
                            const size_t copyBytes = (maxBytes / sizeof(SceMcTblGetDir)) * sizeof(SceMcTblGetDir);
                            std::memcpy(dst, entries.data(), copyBytes);
                            result = static_cast<int32_t>(copyBytes / sizeof(SceMcTblGetDir));
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(static_cast<int32_t>(kRpcGetDir), result);
            resultPtr = recvBufAddr;
            return true;
        }

        default:
            if (traceMcserv)
            {
                static std::atomic<uint32_t> s_unhandledLogs{0u};
                if (s_unhandledLogs.fetch_add(1u, std::memory_order_relaxed) < 96u)
                {
                    std::fprintf(stderr,
                                 "[iop:mcserv] unhandled rpc=0x%X send=0x%08X/%u recv=0x%08X/%u\n",
                                 rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);
                }
            }
            if (isMcInitBypassEnabled() && recvBufAddr != 0u && recvSize >= 12u)
            {
                if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
                {
                    const int32_t reply[3] = {kMcResultSucceed, kMcServVersion, kMcManVersion};
                    std::memcpy(dst, reply, sizeof(reply));
                }
                std::fprintf(stderr,
                             "[iop:mcserv] Debug bypass active (env PS2X_MC_BYPASS_INIT_CHECK) rpc=0x%X recv=0x%08X\n",
                             rpcNum, recvBufAddr);
                setMcCommandResultLocked(static_cast<int32_t>(rpcNum), kMcResultSucceed);
                resultPtr = recvBufAddr;
                return true;
            }

            // Unhandled MCSERV command: still claim the RPC so the caller gets a
            // definite (empty/zero) reply instead of parking forever waiting for
            // one that will never arrive.
            if (recvBufAddr != 0u && recvSize > 0u)
            {
                if (uint8_t *dst = getMemPtr(rdram, recvBufAddr))
                {
                    std::memset(dst, 0, recvSize);
                }
            }
            setMcCommandResultLocked(static_cast<int32_t>(rpcNum), kMcResultNoEntry);
            resultPtr = recvBufAddr;
            return true;
        }
    }
}
