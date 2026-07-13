#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ps2_stubs::mc_internal
{
    constexpr int32_t kMcResultSucceed = 0;
    constexpr int32_t kMcResultChangedCard = -1;
    constexpr int32_t kMcResultNoFormat = -2;
    constexpr int32_t kMcResultNoEntry = -4;
    constexpr int32_t kMcResultDeniedPermit = -5;
    constexpr int32_t kMcResultNotEmpty = -6;
    constexpr int32_t kMcResultUpLimitHandle = -7;

    constexpr int32_t kMcTypePs2 = 2;
    constexpr int32_t kMcFormatted = 1;
    constexpr int32_t kMcUnformatted = 0;
    constexpr int32_t kMcFreeClusters = 0x2000;
    constexpr size_t kMcMaxPathLen = 1024;
    constexpr size_t kMcMaxOpenFiles = 32;

    constexpr uint16_t kMcAttrReadable = 0x0001;
    constexpr uint16_t kMcAttrWriteable = 0x0002;
    constexpr uint16_t kMcAttrFile = 0x0010;
    constexpr uint16_t kMcAttrSubdir = 0x0020;
    constexpr uint16_t kMcAttrClosed = 0x0080;
    constexpr uint16_t kMcAttrExists = 0x8000;

    struct SceMcStDateTime
    {
        uint8_t Resv2 = 0;
        uint8_t Sec = 0;
        uint8_t Min = 0;
        uint8_t Hour = 0;
        uint8_t Day = 0;
        uint8_t Month = 0;
        uint16_t Year = 0;
    };

    struct SceMcTblGetDir
    {
        SceMcStDateTime _Create{};
        SceMcStDateTime _Modify{};
        uint32_t FileSizeByte = 0;
        uint16_t AttrFile = 0;
        uint16_t Reserve1 = 0;
        uint32_t Reserve2 = 0;
        uint32_t PdaAplNo = 0;
        char EntryName[32]{};
    };

    static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

    struct McOpenFile
    {
        FILE *file = nullptr;
        int32_t port = 0;
        std::filesystem::path hostPath;
    };

    struct McPortState
    {
        std::string currentDir = "/";
        bool formatted = true;
    };

    // Shared memory-card state/helpers, used by both the direct syscall-style
    // sceMc* stubs (MemoryCard.cpp) and the sceMcServ RPC wrapper
    // (ps2_iop_mcman.cpp) so both entry points see the same open-file table,
    // port state, and host filesystem layout.
    extern std::mutex g_mcStateMutex;
    extern int32_t g_mcNextFd;
    extern int32_t g_mcLastCmd;
    extern int32_t g_mcLastResult;
    extern std::unordered_map<int32_t, McOpenFile> g_mcFiles;
    extern std::array<McPortState, 2> g_mcPorts;

    bool isValidMcPortSlot(int32_t port, int32_t slot);
    std::filesystem::path getMcRootPath(int32_t port);
    void ensureMcRootExists(int32_t port);
    std::string normalizeGuestMcPathLocked(int32_t port, std::string path);
    std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath);
    void fillMcDirTableEntry(SceMcTblGetDir &entry,
                             const std::string &name,
                             bool isDirectory,
                             uint32_t sizeBytes,
                             std::time_t modifiedTime);
    bool wildcardMatch(const std::string &pattern, const std::string &value);
    void setMcCommandResultLocked(int32_t cmd, int32_t result);
    int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath);
    FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags);
    std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value);
}
