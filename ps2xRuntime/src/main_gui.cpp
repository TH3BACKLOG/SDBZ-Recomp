#include <stddef.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")
#include <string>
#include <vector>
#include <fstream>
#include <stdarg.h>
#include <map>
#include <deque>
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiFileDialog.h"
#include "build_info.h"
// stb_image — implementation is already compiled in ImGuiFileDialog.cpp
#include "stb/stb_image.h"

#include "debugger_state.h"
#include "function_db.h"
#include "pcsx2_debugserver_client.h"
#include "recomp_debug_ipc.h"

// Force hybrid-GPU laptops (NVIDIA Optimus / AMD switchable graphics) to run this
// OpenGL app on the discrete GPU rather than the integrated one. When the wrong
// GPU is picked, the compositor's swap chain can mismatch the app's OpenGL
// framebuffer, producing horizontal black/pink stripe corruption across the whole
// window. These exported symbols are read by the NVIDIA/AMD drivers by name.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// Preferred, version-agnostic register source: PCSX2's DebugServer TCP plugin
// (JSON, port 21512) reports its own register layout, so it doesn't break
// when PCSX2 changes its internal cpuRegs struct between versions. The
// RPM+hardcoded-offset path below (PCSX2_cpuRegs in debugger_state.h) only
// runs as a fallback when this plugin isn't present.
static PCSX2DebugServerClient g_dbgserver;

// Thin wrappers so tab_breakpoints.cpp (which doesn't include
// pcsx2_debugserver_client.h) can arm real PCSX2-side breakpoints/
// watchpoints instead of only the local polled list.
bool PCSX2DebugServerConnected() { return g_dbgserver.isConnected(); }
bool PCSX2SetBreakpoint(uint32_t addr, const char* condition) {
    return g_dbgserver.setBreakpoint(addr, condition ? condition : "");
}
bool PCSX2RemoveBreakpoint(uint32_t addr) { return g_dbgserver.removeBreakpoint(addr); }
bool PCSX2ClearAllBreakpoints() { return g_dbgserver.clearAllBreakpoints(); }
bool PCSX2SetWatchpoint(uint32_t addr, uint32_t size, const char* type) {
    return g_dbgserver.setWatchpoint(addr, size, type ? type : "write");
}
bool PCSX2RemoveWatchpoint(uint32_t addr, uint32_t size) { return g_dbgserver.removeWatchpoint(addr, size); }
bool PCSX2DebugServerResume() { return g_dbgserver.resume(); }

// On-demand IOP register read for the "IOP Registers" panel under PCSX2 (the
// recomp backend already exposes IOP state via shared memory; PCSX2 doesn't, so
// we ask the DebugServer directly with cpu:"iop"). Blocking TCP round-trip —
// the panel throttles its own call rate.
bool PCSX2ReadIopRegisters(uint32_t& pc, uint32_t gpr[32], uint32_t& hi, uint32_t& lo) {
    if (!g_dbgserver.isConnected()) return false;
    PCSX2DebugServerClient::RawRegisters raw;
    if (!g_dbgserver.readRegisters(raw, PCSX2DebugServerClient::Cpu::IOP)) return false;
    pc = raw.pc;
    hi = (uint32_t)raw.hi;
    lo = (uint32_t)raw.lo;
    for (int i = 0; i < 32; ++i) gpr[i] = (uint32_t)raw.gpr[i];
    return true;
}
// IOP memory read via the DebugServer's read_memory (support unconfirmed — see
// PCSX2DebugServerClient::readMemory). IOP RAM has no reliable RPM base-scan
// analogue to FindEERAM (2MB region, no PC hint), so the JSON path is primary
// for IOP.
bool PCSX2ReadIopMemory(uint32_t addr, uint8_t* out, uint32_t len) {
    return g_dbgserver.readMemory(addr, out, len, PCSX2DebugServerClient::Cpu::IOP);
}

// Re-arms every breakpoint/watchpoint the user has set against a (re)connected
// DebugServer session. Used both after a fresh connect (the plugin has no
// memory of anything armed before it existed) and after step()/stepOver()
// (BUG-018: PCSX2's DebugServer leaves an internal temp breakpoint armed at
// the post-step PC to know when to stop single-stepping; clear_breakpoints
// removes that temp breakpoint too, so the user's real ones must be
// re-sent afterward or they're silently lost along with it).
static void RearmPCSX2Breakpoints() {
    int rearmed_bp = 0, rearmed_wp = 0;
    for (uint32_t addr : g_breakpoints) {
        if (PCSX2SetBreakpoint(addr)) ++rearmed_bp;
    }
    for (const auto& wp : g_watchpoints) {
        if (PCSX2SetWatchpoint(wp.addr, wp.size)) ++rearmed_wp;
    }
    if (rearmed_bp || rearmed_wp)
        AddLog("[Sync] Re-armed %d breakpoint(s), %d watchpoint(s)", rearmed_bp, rearmed_wp);
}

// Bypasses SyncFromPCSX2's 2000ms periodic reconnect throttle so a
// Pause/Resume/Frame-Advance/Step-Over click made right after a routine
// polling timeout (sendCommand() drops the socket on ANY error/timeout, see
// pcsx2_debugserver_client.cpp) doesn't just silently no-op until the next
// throttled retry. Called only from button handlers, which already gate on
// `attached` (process alive) before reaching here.
static bool EnsurePCSX2DebugServerConnected() {
    if (g_dbgserver.isConnected()) return true;
    if (g_dbgserver.connect()) {
        AddLog("[PCSX2][DebugServer] Reconnected");
        RearmPCSX2Breakpoints();
        return true;
    }
    AddLog("[PCSX2][DebugServer] Reconnect attempt failed — plugin not reachable on port 21512");
    return false;
}

// ---------------------------------------------------------------------------
// PCSX2 memory layout (v2.6.x Qt 64-bit)
//
// EE RAM:    32MB contiguous RW region found by VirtualQueryEx scan
// cpuRegs:   PCSX2-internal struct inside pcsx2-qt.exe .data — located by
//            scanning for a PS2 PC in [0x00080000, 0x01FFFFFF] and a valid
//            128-bit GPR block.
// PS2 Input: XInput / SDL GameController / EE RAM fallback chain.
// ---------------------------------------------------------------------------

static int  PineGetStatus();    // forward declaration for sync
bool PinePause();
bool PineResume();
static uintptr_t FindCpuRegs(HANDLE h); // forward declaration

static void StartCpuRegsScanAsync(HANDLE h) {
    if (g_scan_running.load()) return;
    g_scan_running = true;
    g_scan_result  = 1; // mark in-progress
    // Duplicate the handle so the thread owns its own reference
    HANDLE hDup = INVALID_HANDLE_VALUE;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &hDup,
                    PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, 0);
    std::thread([hDup]() {
        uintptr_t result = FindCpuRegs(hDup);
        CloseHandle(hDup);
        g_scan_result  = result ? result : 0;
        g_scan_running = false;
    }).detach();
}
static void PineDisconnect();

bool IsBreakpointSet(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFF;
    for (uint32_t bp : g_breakpoints)
        if ((bp & 0x1FFFFFFF) == phys) return true;
    return false;
}

// Must be called before ImGui init so g_project_root is available for IniFilename.
static void InitProjectRoot() {
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string base(exe_path);

    // Walk up until a directory containing a project marker (.git or
    // CMakeLists.txt) is found. Robust against build-tree layout changes,
    // unlike a fixed level count which silently breaks when the tree flattens.
    std::string dir = base;
    for (int i = 0; i < 8; i++) {
        auto sep = dir.find_last_of("\\/");
        if (sep == std::string::npos) break;
        dir = dir.substr(0, sep);
        if (GetFileAttributesA((dir + "\\.git").c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA((dir + "\\CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_project_root = dir;
            return;
        }
    }

    // Fallback: old fixed-depth walk (6 levels) if no marker was found.
    for (int i = 0; i < 6; i++) {
        auto sep = base.find_last_of("\\/");
        if (sep != std::string::npos) base = base.substr(0, sep);
    }
    g_project_root = base;
}

static void InitLogFilenames() {
    SYSTEMTIME t; GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d-%02d-%04d_%02d%02d",
             t.wMonth, t.wDay, t.wYear, t.wHour, t.wMinute);
    std::string log_dir     = g_project_root + "\\Logs\\";
    std::string session_dir = g_project_root + "\\Session Summary Logs\\";
    g_log_debug     = log_dir + "recomp_debug_"  + stamp + ".log";
    g_log_errors    = log_dir + "recomp_errors_" + stamp + ".log";
    g_log_crash     = log_dir + "recomp_crash_"  + stamp + ".log";
    g_log_crash_dmp = log_dir + "recomp_crash_"  + stamp + ".dmp";
    char build_tag[16];
    snprintf(build_tag, sizeof(build_tag), "b%d_", BUILD_NUMBER);
    g_session_summary = session_dir + "session_" + build_tag + stamp + ".txt";
}

static bool IsErrorLine(const char* s) {
    // Match lines that indicate a problem worth separating into the error log
    const char* keywords[] = {
        "Stale", "Rejected", "failed", "Failed", "error", "Error",
        "not found", "Not found", "frozen", "rediscover", "crash",
        "invalid", "Invalid", "disconnect", "Disconnect", "rescan"
    };
    for (auto* kw : keywords)
        if (strstr(s, kw)) return true;
    return false;
}

void AddLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    session_log.push_back(buf);
    if (session_log.size() > 500) session_log.erase(session_log.begin());
    log_scroll_to_bottom = true;
    { std::ofstream f(g_log_debug, std::ios::app); if (f) f << buf << "\n"; }
    if (IsErrorLine(buf))
        { std::ofstream f(g_log_errors, std::ios::app); if (f) f << buf << "\n"; }
}

void SaveSessionLog(const char* filename) {
    std::ofstream out(filename);
    if (!out) return;
    out << "SDBZ Recomp Debugger | " BUILD_STRING "\n";
    out << "---\n";
    for (const auto& line : session_log) out << line << "\n";
}

// ---------------------------------------------------------------------------
// Crash handler — writes recomp_crash.log + recomp_crash.dmp on any unhandled
// exception so we have a post-mortem even if the process never reaches cleanup.
// ---------------------------------------------------------------------------
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    // Write crash log
    {
        std::ofstream f(g_log_crash, std::ios::app);
        if (f) {
            SYSTEMTIME t; GetLocalTime(&t);
            f << "=== CRASH " << t.wYear << "-" << t.wMonth << "-" << t.wDay
              << " " << t.wHour << ":" << t.wMinute << ":" << t.wSecond << " ===\n";
            f << "Build:            " BUILD_STRING "\n";
            f << "ExceptionCode:    0x" << std::hex << ep->ExceptionRecord->ExceptionCode << "\n";
            f << "ExceptionAddress: 0x" << std::hex
              << (uintptr_t)ep->ExceptionRecord->ExceptionAddress << std::dec << "\n";
            // Last 50 log lines for context
            f << "--- Last log lines ---\n";
            size_t start = session_log.size() > 50 ? session_log.size() - 50 : 0;
            for (size_t i = start; i < session_log.size(); i++)
                f << session_log[i] << "\n";
            f << "=== END ===\n\n";
        }
    }
    // Write minidump
    {
        HANDLE hFile = CreateFileA(g_log_crash_dmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                              hFile, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(hFile);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH; // let Windows show the crash dialog
}

// ---------------------------------------------------------------------------
// Frame Logger
// ---------------------------------------------------------------------------
static void FrameLogger_Tick() {
    if (!fl_capturing || !regs_valid) return;
    if (frame_log.size() >= FL_MAX_FRAMES) return;

    FrameRecord r = {};
    r.frame  = fl_frame_num++;
    r.pc     = cpu_regs.PC;
    r.cycle  = cpu_regs.cycle;
    r.hi     = cpu_regs.HI.lo;
    r.lo     = cpu_regs.LO.lo;
    if (fl_log_gprs)
        for (int i = 0; i < 32; i++) r.gpr[i] = cpu_regs.GPR[i].lo;
    if (fl_log_inputs) {
        r.buttons = ~pad0.buttons;  // active-high in FrameRecord; pad0 uses active-low (PS2 SIO2)
        r.lx = pad0.lx; r.ly = pad0.ly;
        r.rx = pad0.rx; r.ry = pad0.ry;
    }
    frame_log.push_back(r);
}

// FrameLogger_DumpCSV() — moved to tab_framelogger.cpp

// ---------------------------------------------------------------------------
// Configurable paths — persisted to sdbz_debugger.ini next to the exe
// ---------------------------------------------------------------------------
static const char* ExeNameFromPath(const char* path) {
    const char* s = strrchr(path, '\\');
    const char* f = strrchr(path, '/');
    const char* sep = (s && f) ? (s > f ? s : f) : (s ? s : f);
    return sep ? sep + 1 : path;
}

// Returns the directory portion of a full file path into out_dir.
void DirFromPath(const char* path, char* out_dir, size_t out_size) {
    const char* s = strrchr(path, '\\');
    const char* f = strrchr(path, '/');
    const char* sep = (s && f) ? (s > f ? s : f) : (s ? s : f);
    if (sep && sep > path) {
        size_t len = (size_t)(sep - path);
        if (len >= out_size) len = out_size - 1;
        strncpy(out_dir, path, len);
        out_dir[len] = '\0';
    }
}

static std::string GetConfigPath() {
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char* last = strrchr(exe_path, '\\');
    if (last) *(last + 1) = '\0';
    return std::string(exe_path) + "sdbz_debugger.ini";
}
static const std::string CONFIG_FILE = GetConfigPath();

void SaveConfig() {
    std::ofstream f(CONFIG_FILE);
    if (!f.is_open()) { AddLog("[Config] Could not write %s", CONFIG_FILE.c_str()); return; }
    f << "map_path="    << cfg_map_path    << "\n";
    f << "map_dir="     << cfg_map_dir     << "\n";
    f << "pcsx2_path="  << cfg_pcsx2_path  << "\n";
    f << "pcsx2_dir="   << cfg_pcsx2_dir   << "\n";
    f << "runner_path=" << cfg_runner_path << "\n";
    f << "elf_path="    << cfg_elf_path    << "\n";
    f << "iso_path="    << cfg_iso_path    << "\n";
    f << "autostart_framelog=" << (cfg_autostart_framelog ? 1 : 0) << "\n";
    f << "default_backend=" << cfg_default_backend << "\n";
    f << "open_disasm="      << (cfg_open_disasm      ? 1 : 0) << "\n";
    f << "open_eeregs="      << (cfg_open_eeregs      ? 1 : 0) << "\n";
    f << "open_iopregs="     << (cfg_open_iopregs     ? 1 : 0) << "\n";
    f << "open_breakpoints=" << (cfg_open_breakpoints ? 1 : 0) << "\n";
    f << "open_memview="     << (cfg_open_memview     ? 1 : 0) << "\n";
    f << "open_framelog="    << (cfg_open_framelog    ? 1 : 0) << "\n";
    f << "open_runtimelog="  << (cfg_open_runtimelog  ? 1 : 0) << "\n";
    f << "open_symbols="     << (cfg_open_symbols     ? 1 : 0) << "\n";
    f << "pad_source="       << cfg_pad_source       << "\n";
    f << "pad_xinput_slot="  << cfg_pad_xinput_slot  << "\n";
    f << "pad_sdl_index="    << cfg_pad_sdl_index    << "\n";
    f << "pad_port="         << cfg_pad_port         << "\n";
    for (int port = 0; port < 2; ++port) {
        for (int i = 0; i < (int)PadInput::Count; ++i) {
            const PadBinding& b = cfg_pad_bindings[port][i];
            if (b.deviceType == PadBindDeviceType::None) continue;
            f << "pad_bind_" << port << "_" << i << "="
              << (int)b.deviceType << "," << b.deviceIndex << ","
              << b.inputCode << "," << (b.isAxis ? 1 : 0) << ","
              << (b.axisNegative ? 1 : 0) << "," << b.sensitivity << "\n";
        }
    }
    AddLog("[Config] Saved to %s", CONFIG_FILE.c_str());
}

static void LoadConfig() {
    std::ifstream f(CONFIG_FILE);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if      (line.compare(0,  9, "map_path=")   == 0) strncpy(cfg_map_path,   line.c_str() +  9, sizeof(cfg_map_path)   - 1);
        else if (line.compare(0,  8, "map_dir=")    == 0) strncpy(cfg_map_dir,    line.c_str() +  8, sizeof(cfg_map_dir)    - 1);
        else if (line.compare(0, 11, "pcsx2_path=") == 0) strncpy(cfg_pcsx2_path, line.c_str() + 11, sizeof(cfg_pcsx2_path) - 1);
        else if (line.compare(0, 10, "pcsx2_dir=")  == 0) strncpy(cfg_pcsx2_dir,  line.c_str() + 10, sizeof(cfg_pcsx2_dir)  - 1);
        else if (line.compare(0, 12, "runner_path=") == 0) strncpy(cfg_runner_path, line.c_str() + 12, sizeof(cfg_runner_path) - 1);
        else if (line.compare(0,  9, "elf_path=")   == 0) strncpy(cfg_elf_path,    line.c_str() +  9, sizeof(cfg_elf_path)    - 1);
        else if (line.compare(0, 12, "runner_args=") == 0) strncpy(cfg_elf_path,    line.c_str() + 12, sizeof(cfg_elf_path)    - 1); // legacy key
        else if (line.compare(0,  9, "iso_path=")   == 0) strncpy(cfg_iso_path,    line.c_str() +  9, sizeof(cfg_iso_path)    - 1);
        else if (line.compare(0, 19, "autostart_framelog=") == 0) cfg_autostart_framelog = (line.c_str()[19] == '1');
        else if (line.compare(0, 16, "default_backend=") == 0) cfg_default_backend = atoi(line.c_str() + 16);
        else if (line.compare(0, 12, "open_disasm=")      == 0) cfg_open_disasm      = (line.c_str()[12] == '1');
        else if (line.compare(0, 12, "open_eeregs=")      == 0) cfg_open_eeregs      = (line.c_str()[12] == '1');
        else if (line.compare(0, 13, "open_iopregs=")     == 0) cfg_open_iopregs     = (line.c_str()[13] == '1');
        else if (line.compare(0, 17, "open_breakpoints=") == 0) cfg_open_breakpoints = (line.c_str()[17] == '1');
        else if (line.compare(0, 13, "open_memview=")     == 0) cfg_open_memview     = (line.c_str()[13] == '1');
        else if (line.compare(0, 14, "open_framelog=")    == 0) cfg_open_framelog    = (line.c_str()[14] == '1');
        else if (line.compare(0, 16, "open_runtimelog=")  == 0) cfg_open_runtimelog  = (line.c_str()[16] == '1');
        else if (line.compare(0, 13, "open_symbols=")     == 0) cfg_open_symbols     = (line.c_str()[13] == '1');
        else if (line.compare(0, 11, "pad_source=")       == 0) cfg_pad_source       = atoi(line.c_str() + 11);
        else if (line.compare(0, 16, "pad_xinput_slot=")  == 0) cfg_pad_xinput_slot  = atoi(line.c_str() + 16);
        else if (line.compare(0, 14, "pad_sdl_index=")    == 0) cfg_pad_sdl_index    = atoi(line.c_str() + 14);
        else if (line.compare(0, 9,  "pad_port=")         == 0) cfg_pad_port         = atoi(line.c_str() + 9);
        else if (line.compare(0, 9,  "pad_bind_")         == 0) {
            // pad_bind_<port>_<input>=<deviceType>,<deviceIndex>,<inputCode>,<isAxis>,<axisNegative>,<sensitivity>
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                int port = -1, input = -1;
                if (sscanf(line.c_str(), "pad_bind_%d_%d=", &port, &input) == 2 &&
                    port >= 0 && port < 2 && input >= 0 && input < (int)PadInput::Count) {
                    int deviceType = 0, deviceIndex = 0, inputCode = 0, isAxis = 0, axisNegative = 0;
                    float sensitivity = 1.0f;
                    if (sscanf(line.c_str() + eq + 1, "%d,%d,%d,%d,%d,%f",
                               &deviceType, &deviceIndex, &inputCode, &isAxis, &axisNegative, &sensitivity) == 6) {
                        PadBinding& b = cfg_pad_bindings[port][input];
                        b.deviceType   = (PadBindDeviceType)deviceType;
                        b.deviceIndex  = deviceIndex;
                        b.inputCode    = inputCode;
                        b.isAxis       = isAxis != 0;
                        b.axisNegative = axisNegative != 0;
                        b.sensitivity  = sensitivity;
                    }
                }
            }
        }
    }
    g_cpu_source = (cfg_default_backend == 1) ? CPU_RECOMP : CPU_PCSX2;
    AddLog("[Config] Loaded from %s", CONFIG_FILE.c_str());
}

// ---------------------------------------------------------------------------
// Symbol table helpers
// ---------------------------------------------------------------------------
// stristr() — moved to tab_symbolsynctool.cpp

void BuildCoverage() {
    std::map<uint32_t, int> addr_count;
    for (const auto& r : frame_log) {
        uint32_t pc = r.pc & 0x1FFFFFFFu;
        if (pc == 0 || pc >= 0x2000000u) continue;
        uint32_t key = pc;
        if (!symbol_table.empty()) {
            auto it = symbol_table.upper_bound(pc);
            if (it != symbol_table.begin()) {
                --it;
                if (pc - it->first < 0x400u)
                    key = it->first;
            }
        }
        addr_count[key]++;
    }
    g_cov_entries.clear();
    for (auto& [addr, cnt] : addr_count) {
        CovEntry e;
        e.addr  = addr;
        e.count = cnt;
        auto it = symbol_table.find(addr);
        if (it != symbol_table.end()) e.name = it->second;
        g_cov_entries.push_back(e);
    }
    std::sort(g_cov_entries.begin(), g_cov_entries.end(),
        [](const CovEntry& a, const CovEntry& b) { return a.count > b.count; });
}

void LoadMapFile(const char* path) {
    std::ifstream file(path);
    if (!file.is_open()) { AddLog("[Symbols] Cannot open: %s", path); return; }
    symbol_table.clear();
    uint32_t addr; std::string name;
    while (file >> std::hex >> addr >> name)
        symbol_table[addr] = name;
    AddLog("[Symbols] Loaded %zu symbols from %s", symbol_table.size(), path);
    FnDB_RebuildFromSymbolTable();
}

// Reads the flat address list ps2EntryRunner dumps at startup
// (recomp_function_table.txt, one hex address per line, next to the exe) so
// the disassembly view can flag ELF functions that never got translated.
// Looked for next to RecompDebugger.exe first, then next to the configured
// runner exe, since the two are normally launched from the same folder.
void LoadRecompFunctionTable() {
    g_recomp_functions.clear();
    g_recomp_table_loaded = false;

    std::vector<std::string> candidates;
    candidates.push_back("recomp_function_table.txt");
    if (cfg_runner_path[0]) {
        std::string runner_dir(cfg_runner_path);
        size_t sep = runner_dir.find_last_of("\\/");
        if (sep != std::string::npos) {
            candidates.push_back(runner_dir.substr(0, sep + 1) + "recomp_function_table.txt");
        }
    }

    for (const std::string& path : candidates) {
        std::ifstream file(path);
        if (!file.is_open()) continue;
        uint32_t addr;
        while (file >> std::hex >> addr)
            g_recomp_functions.insert(addr);
        g_recomp_table_loaded = true;
        AddLog("[Recomp] Loaded %zu recompiled function addresses from %s", g_recomp_functions.size(), path.c_str());
        return;
    }
    AddLog("[Recomp] recomp_function_table.txt not found (run ps2EntryRunner at least once to generate it)");
}

bool IsFunctionInRecomp(uint32_t func_start) {
    if (!g_recomp_table_loaded) return true; // no ground truth yet — don't flag anything as missing
    for (uint32_t candidate : { func_start, func_start | 0x80000000u, func_start & 0x1FFFFFFFu })
        if (g_recomp_functions.count(candidate)) return true;
    return false;
}

void ExportGhidraCSV() {
    if (g_pending_renames.empty()) { AddLog("[Labels] Nothing to export."); return; }

    // Derive output path from cfg_map_path directory
    char out_path[MAX_PATH];
    char time_buf[32];
    time_t now = time(nullptr);
    struct tm* tm_now = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "rename_%m%d%Y_%H%M.csv", tm_now);

    strncpy(out_path, cfg_map_path, sizeof(out_path) - 1);
    out_path[sizeof(out_path) - 1] = '\0';
    char* last_sep = strrchr(out_path, '\\');
    if (!last_sep) last_sep = strrchr(out_path, '/');
    if (last_sep) {
        *(last_sep + 1) = '\0';
        strncat(out_path, time_buf, sizeof(out_path) - strlen(out_path) - 1);
    } else {
        strncpy(out_path, time_buf, sizeof(out_path) - 1);
    }

    std::ofstream f(out_path);
    if (!f) { AddLog("[Labels] Cannot write to %s", out_path); return; }
    f << "address,name\n";
    for (auto& pr : g_pending_renames)
        f << "0x" << std::hex << pr.address << "," << pr.name << "\n";
    AddLog("[Labels] Exported %d label(s) -> %s  (run BatchRename.java in Ghidra)", (int)g_pending_renames.size(), out_path);
}

void SaveSymbolToMap(const char* path, uint32_t phys_addr, const char* name) {
    if (!path || !path[0]) { AddLog("[Label] No symbols.map path set — configure it in the Symbols tab."); return; }
    // Read existing entries so we don't clobber them
    std::map<uint32_t, std::string> on_disk;
    {
        std::ifstream f(path);
        uint32_t a; std::string n;
        while (f >> std::hex >> a >> n) on_disk[a] = n;
    }
    on_disk[phys_addr] = name;
    std::ofstream f(path);
    if (!f) { AddLog("[Label] Cannot write to %s", path); return; }
    for (auto& [a, n] : on_disk)
        f << std::hex << a << " " << n << "\n";
    AddLog("[Label] Saved 0x%08X = %s  (%zu total in file)", phys_addr, name, on_disk.size());
}

const char* GetSymbolName(uint32_t addr) {
    auto it = symbol_table.find(addr);
    return (it != symbol_table.end()) ? it->second.c_str() : nullptr;
}

// ---------------------------------------------------------------------------
// PCSX2 process discovery
// ---------------------------------------------------------------------------
static DWORD FindPCSX2PID() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    const char* user_exe = (cfg_pcsx2_path[0] != '\0') ? ExeNameFromPath(cfg_pcsx2_path) : nullptr;
    if (Process32FirstW(snap, &pe)) {
        do {
            char exe_name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe_name, MAX_PATH, nullptr, nullptr);
            if (_stricmp(exe_name, "pcsx2-qt.exe") == 0 ||
                _stricmp(exe_name, "pcsx2x64.exe") == 0 ||
                _stricmp(exe_name, "pcsx2.exe")    == 0 ||
                (user_exe && _stricmp(exe_name, user_exe) == 0)) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ---------------------------------------------------------------------------
// EE RAM discovery -  scan for the 32MB committed RW region
// ---------------------------------------------------------------------------
// hint_pc: when non-zero, prefer the candidate that has non-zero bytes at that PS2 address.
static uintptr_t FindEERAM(HANDLE h, uint32_t hint_pc = 0) {
    // PCSX2 Qt (64-bit) maps EE RAM on the 64-bit heap, well above 0x20000000.
    // Anything below that threshold is PCSX2's own code/data — reject it.
    MEMORY_BASIC_INFORMATION mbi = {};
    uintptr_t addr = 0;
    std::vector<uintptr_t> candidates;
    int regions_checked = 0;

    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Type & MEM_IMAGE) &&
            (uintptr_t)mbi.BaseAddress > 0x20000000 &&  // must be on 64-bit heap
            mbi.RegionSize >= 0x2000000 &&               // at least 32MB
            mbi.RegionSize <= 0x20000000)                // at most 512MB
        {
            regions_checked++;
            uint8_t probe[16] = {}; SIZE_T br = 0;
            ReadProcessMemory(h, (LPCVOID)((uintptr_t)mbi.BaseAddress + 0x80000), probe, sizeof(probe), &br);
            bool nonzero = false;
            for (int i = 0; i < (int)br; i++) if (probe[i]) { nonzero = true; break; }
            if (nonzero) {
                AddLog("[Sync] EE RAM candidate: 0x%llX size=0x%llX (probe @+0x80000 nonzero)",
                       (uint64_t)mbi.BaseAddress, (uint64_t)mbi.RegionSize);
                candidates.push_back((uintptr_t)mbi.BaseAddress);
            }
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    if (candidates.empty()) {
        if (regions_checked == 0)
            AddLog("[Sync] FindEERAM: no RW regions >= 32MB found above 0x20000000 (game not loaded yet?)");
        return 0;
    }

    // With a PC hint, score each candidate by how many words at the PC offset decode
    // as valid R5900 opcodes. Pick the highest scorer — this distinguishes real EE RAM
    // (actual game code) from other large RW regions that happen to be non-zero.
    if (hint_pc != 0) {
        uint32_t phys = hint_pc & 0x1FFFFFFF;
        if (phys >= 0x80000 && phys < 0x2000000) {
            // Primary opcode field (bits 31-26): mark undefined opcodes false
            static const bool valid_op[64] = {
                1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, // 0x00-0x0F
                1,1,1,0,1,1,1,1, 1,1,1,1,1,0,1,1, // 0x10-0x1F (0x13, 0x1D undefined)
                1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, // 0x20-0x2F
                0,1,0,0,0,0,1,1, 0,1,0,0,0,0,1,1  // 0x30-0x3F (several undefined)
            };
            uintptr_t best = 0; int best_score = -1;
            for (uintptr_t c : candidates) {
                uint32_t words[8] = {}; SIZE_T br = 0;
                ReadProcessMemory(h, (LPCVOID)(c + phys), words, sizeof(words), &br);
                int score = 0;
                for (int i = 0; i < 8; i++) {
                    if (words[i] == 0) { score++; continue; } // NOP counts as valid
                    if (valid_op[(words[i] >> 26) & 0x3F]) score++;
                }
                AddLog("[Sync] EE RAM candidate 0x%llX MIPS score=%d/8 at PC=0x%08X",
                       (uint64_t)c, score, hint_pc);
                if (score > best_score) { best_score = score; best = c; }
            }
            if (best) return best;
        }
    }

    return candidates[0];
}

// ---------------------------------------------------------------------------
// PAD EE RAM discovery - broad scan for idle DualShock2 button word
// ---------------------------------------------------------------------------
static uintptr_t FindPadInEERAM(HANDLE h, uintptr_t ee_base) {
    // First pass: probe the most common SDBZ / PS2 known offsets quickly
    static const uint32_t known_offsets[] = {
        0x3C4A0, 0x3C4A4, 0x74A10, 0x74A14, 0x70000, 0x70004,
        0x6F000, 0x6F004, 0x3B000, 0x3B004, 0x3C000, 0x3C004,
        0x60000, 0x60004, 0x64000, 0x64004, 0x68000, 0x68004,
        0x6C000, 0x6C004, 0x40000, 0x40004, 0x44000, 0x44004
    };
    for (auto off : known_offsets) {
        uint16_t val = 0; SIZE_T br = 0;
        ReadProcessMemory(h, (LPCVOID)(ee_base + off), &val, 2, &br);
        if (br == 2 && val == 0xFFFF) {
            // Verify next 4 bytes look like centered analog sticks (50-210 range each)
            uint8_t sticks[4] = {}; SIZE_T sr = 0;
            ReadProcessMemory(h, (LPCVOID)(ee_base + off + 2), sticks, 4, &sr);
            bool sticks_ok = (sr == 4 &&
                sticks[0] >= 50 && sticks[0] <= 210 &&
                sticks[1] >= 50 && sticks[1] <= 210 &&
                sticks[2] >= 50 && sticks[2] <= 210 &&
                sticks[3] >= 50 && sticks[3] <= 210);
            if (sticks_ok) {
                AddLog("[PAD] Candidate pad offset (known list): 0x%X (sticks=%d,%d,%d,%d)",
                       off, sticks[0], sticks[1], sticks[2], sticks[3]);
                return off;
            }
        }
    }

    // Second pass: broad scan of the first 2MB of EE RAM in 4-byte steps
    // Looking for 0xFFFF button word followed by centered analog sticks
    AddLog("[PAD] Known offsets missed - starting broad EE RAM scan...");
    static const uint32_t SCAN_END = 0x200000;
    static const uint32_t CHUNK    = 0x1000; // 4KB chunks
    uint32_t candidates = 0;
    for (uint32_t base_off = 0; base_off < SCAN_END; base_off += CHUNK) {
        uint8_t chunk[CHUNK + 8] = {};
        SIZE_T br = 0;
        ReadProcessMemory(h, (LPCVOID)(ee_base + base_off), chunk, sizeof(chunk), &br);
        if (br < 6) continue;
        for (uint32_t i = 0; i + 5 < (uint32_t)br; i += 2) {
            uint16_t val = *(uint16_t*)(chunk + i);
            if (val != 0xFFFF) continue;
            uint8_t s0 = chunk[i+2], s1 = chunk[i+3], s2 = chunk[i+4], s3 = chunk[i+5];
            bool centered = (s0 >= 50 && s0 <= 210 &&
                             s1 >= 50 && s1 <= 210 &&
                             s2 >= 50 && s2 <= 210 &&
                             s3 >= 50 && s3 <= 210);
            if (centered) {
                uint32_t off = base_off + i;
                AddLog("[PAD] Broad-scan candidate: 0x%X (sticks=%d,%d,%d,%d)",
                       off, s0, s1, s2, s3);
                if (++candidates == 1) {
                    // Return first candidate; differential scan can refine later
                    return off;
                }
            }
        }
    }
    AddLog("[PAD] EE RAM scan complete - no pad candidate found (buttons held? try releasing all)");
    return 0;
}

// ---------------------------------------------------------------------------
// cpuRegs discovery -  scan pcsx2-qt.exe .data for a valid PC + GPR pattern.
// We look for a uint32 in [0x00080000, 0x01FFFFFF] (typical SDBZ EE code range)
// preceded by 512 bytes of 128-bit GPR data (each hi64 should be sign-extended
// from lo64 for integer registers, so hi64 == 0 or 0xFFFFFFFFFFFFFFFF mostly).
// ---------------------------------------------------------------------------
static bool IsValidPC(uint32_t pc) {
    if (pc & 3) return false;
    return (pc >= 0x00080000 && pc <= 0x01FFFFFF) ||  // EE RAM user
           (pc >= 0x80080000 && pc <= 0x81FFFFFF) ||  // EE RAM kseg0
           (pc >= 0x1FC00000 && pc <= 0x1FC7FFFF);   // BIOS ROM
}

static uintptr_t FindCpuRegs(HANDLE h) {
    const size_t PAGE   = 4096;
    const size_t STRIDE = 4;
    const size_t STRUCT = sizeof(PCSX2_cpuRegs);

    std::vector<uint8_t> page(PAGE + STRUCT);
    std::vector<uint8_t> prev(STRUCT, 0);

    int regions_scanned = 0;
    int valid_pcs_found = 0;
    int gpr0_failures   = 0;
    int gpr1_failures   = 0;
    int score_failures  = 0;
    int low_addr_rejects = 0;
    int sp_zero_rejects  = 0;

    uintptr_t fallback_addr      = 0;   // BIOS-range fallback
    uint32_t  fallback_pc        = 0;
    uintptr_t frozen_game_addr   = 0;   // game-range, frozen non-zero cycle (PCSX2 paused during scan)
    uint32_t  frozen_game_pc     = 0;
    uint64_t  frozen_game_sp     = 0;   // sp of best frozen candidate — non-zero preferred
    uintptr_t zero_cycle_addr    = 0;   // game-range, cycle=0 (possible stale copy — lowest priority)
    uint32_t  zero_cycle_pc      = 0;

    // Two-pass scan: MEM_IMAGE RW first (PE .data — where cpuRegs actually lives),
    // then MEM_PRIVATE as a fallback. This avoids burning time on PCSX2's huge heap/JIT
    // regions which generate hundreds of false positives.
    for (int pass = 0; pass < 2; pass++) {
    MEMORY_BASIC_INFORMATION mbi = {};
    uintptr_t addr = 0;
    memset(prev.data(), 0, STRUCT);

    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

        bool is_exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        bool is_image   = (mbi.Type == MEM_IMAGE);
        bool is_private = (mbi.Type == MEM_PRIVATE);

        // Pass 0: MEM_IMAGE RW only (PE .data sections)
        // Pass 1: MEM_PRIVATE RW only (heap/mapped — last resort)
        bool want = (pass == 0) ? (is_image && !is_exec) : (is_private && !is_exec);

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            want &&
            mbi.RegionSize >= STRUCT &&
            mbi.RegionSize <= 0x10000000)
        {
            regions_scanned++;
            memset(prev.data(), 0, STRUCT);
            uintptr_t base = (uintptr_t)mbi.BaseAddress;
            uintptr_t end  = next;

            for (uintptr_t pg = base; pg < end; pg += PAGE) {
                SIZE_T br = 0;
                memcpy(page.data(), prev.data(), STRUCT);
                ReadProcessMemory(h, (LPCVOID)pg, page.data() + STRUCT, PAGE, &br);
                if (br == 0) { memset(prev.data(), 0, STRUCT); continue; }

                size_t scan_end = STRUCT + br;
                for (size_t off = STRUCT; off + 4 <= scan_end; off += STRIDE) {
                    uint32_t pc = *reinterpret_cast<uint32_t*>(page.data() + off);
                    if (!IsValidPC(pc)) continue;
                    if (off < STRUCT + 4) continue;

                    valid_pcs_found++;
                    auto* gpr = reinterpret_cast<PCSX2_GPR*>(page.data() + off - 0x2A8);

                    if (gpr[0].lo != 0 || gpr[0].hi != 0) { gpr0_failures++; continue; }
                    if (gpr[1].hi != 0 && gpr[1].hi != 0xFFFFFFFFFFFFFFFFULL) { gpr1_failures++; continue; }
                    // Reject MSVC debug fill (0xCCCCCCCC) and other known poison patterns
                    if ((gpr[1].lo & 0xFFFFFFFF) == 0xCCCCCCCC) { score_failures++; continue; }
                    if (pc == 0xCCCCCCCC || pc == 0xF7F7F7F7 || pc == 0xA7A7A7A7) { score_failures++; continue; }

                    int valid = 0;
                    for (int g = 0; g < 32; g++) {
                        uint64_t hi = gpr[g].hi;
                        if (hi == 0 || hi == 0xFFFFFFFFFFFFFFFFULL) valid++;
                    }
                    if (valid >= 24) {
                        uintptr_t pc_addr = pg + (off - STRUCT);
                        uintptr_t candidate = pc_addr - 0x2A8;
                        // Reject anything below 512MB (PCSX2 64-bit heap is always well above this)
                        if (candidate < 0x20000000ULL) {
                            low_addr_rejects++;
                            continue;
                        }
                        // Post-lock sanity: re-read PC live and confirm still valid PS2 address
                        uint32_t live_pc = 0; SIZE_T chk = 0;
                        ReadProcessMemory(h, (LPCVOID)(candidate + 0x2A8), &live_pc, 4, &chk);
                        if (chk != 4 || !IsValidPC(live_pc)) {
                            AddLog("[Sync] Rejected stale candidate 0x%llX (live PC=0x%08X)", (uint64_t)candidate, live_pc);
                            continue;
                        }
                        // GPR[0] (zero register) must always read as 0 in real cpuRegs
                        uint64_t gpr0_live[2] = {1,1}; SIZE_T chk2 = 0;
                        ReadProcessMemory(h, (LPCVOID)candidate, gpr0_live, 16, &chk2);
                        if (chk2 != 16 || gpr0_live[0] != 0 || gpr0_live[1] != 0) {
                            AddLog("[Sync] Rejected false positive 0x%llX (GPR[0]=0x%llX:0x%llX)", (uint64_t)candidate, gpr0_live[0], gpr0_live[1]);
                            continue;
                        }
                        // sp (GPR[29]): reject only obvious host pointers (>= 4GB).
                        // JIT may not have flushed sp to cpuRegs during scan, so small or
                        // zero values are allowed — cycle check below filters remaining noise.
                        uint64_t sp_lo = 0; SIZE_T chk3 = 0;
                        ReadProcessMemory(h, (LPCVOID)(candidate + 29*16), &sp_lo, 8, &chk3);
                        if (chk3 != 8 || sp_lo >= 0x100000000ULL) {
                            sp_zero_rejects++;
                            if (live_pc < 0x01000000)
                                AddLog("[Sync] SP-rejected game-PC candidate 0x%llX (PC=0x%08X sp=0x%llX score=%d)",
                                       (uint64_t)candidate, live_pc, sp_lo, valid);
                            continue;
                        }
                        // Verify cycle is not a frozen snapshot (live cpuRegs, not a stale backup).
                        // cycle is at struct offset 0x3C0 (after sCycle[32] which ends at 0x3C0).
                        // Read twice with a 10ms gap; reject only if BOTH reads are non-zero AND identical
                        // (frozen snapshot). Do NOT reject zero-cycle candidates — the JIT may not have
                        // flushed the cycle counter yet at scan time, so zero is normal for a live struct.
                        uint32_t cycle1 = 0, cycle2 = 0; SIZE_T chk4 = 0;
                        ReadProcessMemory(h, (LPCVOID)(candidate + 0x3C0), &cycle1, 4, &chk4);
                        Sleep(10);
                        ReadProcessMemory(h, (LPCVOID)(candidate + 0x3C0), &cycle2, 4, &chk4);
                        // Game code lives at 0x00100000â€“0x00FFFFFF. Kernel/exception space
                        // is 0x00000000â€“0x000FFFFF — exclude it so bootstrap addresses
                        // like 0x000FFFF8 (PS2 reset vector area) are not accepted as game code.
                        bool game_pc = (live_pc >= 0x00100000 && live_pc < 0x01000000);
                        if (cycle1 != 0 && cycle1 == cycle2) {
                            // Frozen cycle: stale backup, OR live struct while PCSX2 is paused.
                            // Game-range PCs with non-zero cycles are better than a zero-cycle
                            // stale copy — keep as frozen fallback, prefer over BIOS-range.
                            AddLog("[Sync] Frozen-cycle candidate 0x%llX (PC=0x%08X, cycle=0x%llX, game=%s)",
                                   (uint64_t)candidate, live_pc, cycle1, game_pc ? "yes" : "no");
                            if (game_pc) {
                                // Prefer candidate with non-zero sp (stale copies typically have sp=0)
                                bool better = !frozen_game_addr || (sp_lo > 0 && frozen_game_sp == 0);
                                if (better) { frozen_game_addr = candidate; frozen_game_pc = live_pc; frozen_game_sp = sp_lo; }
                            } else if (!fallback_addr) { fallback_addr = candidate; fallback_pc = live_pc; }
                            continue;
                        }
                        if (cycle2 < cycle1 && cycle1 > 1000) {
                            // Decreasing cycle is impossible for a live counter — this is garbage data
                            AddLog("[Sync] Rejected decreasing-cycle candidate 0x%llX (cycle %u->%u)", (uint64_t)candidate, cycle1, cycle2);
                            continue;
                        }
                        // Re-read PC after the cycle delay to confirm it's still a valid PS2 address.
                        // If the JIT wrote a host pointer into this slot between reads, reject.
                        uint32_t pc_recheck = 0;
                        ReadProcessMemory(h, (LPCVOID)(candidate + 0x2A8), &pc_recheck, 4, &chk4);
                        if (!IsValidPC(pc_recheck)) {
                            AddLog("[Sync] Rejected drifted candidate 0x%llX (PC drifted to 0x%08X after cycle check)", (uint64_t)candidate, pc_recheck);
                            continue;
                        }
                        AddLog("[Sync] Pass%d %s cpuRegs at 0x%llX (PC=0x%08X, score=%d, cycle=%u->%u, regions=%d)",
                               pass, game_pc ? "Found" : "Found BIOS-range candidate",
                               (uint64_t)candidate, live_pc, valid, cycle1, cycle2, regions_scanned);
                        if (game_pc && cycle2 > 0) return candidate;  // live running struct — best possible
                        if (game_pc && !zero_cycle_addr) { zero_cycle_addr = candidate; zero_cycle_pc = live_pc; }
                        if (!game_pc && !fallback_addr) { fallback_addr = candidate; fallback_pc = live_pc; }
                    } else {
                        score_failures++;
                        if (score_failures <= 10 && valid >= 20)
                            AddLog("[Sync] Near-miss PC=0x%08X score=%d/32 gpr0=%llX:%llX",
                                   pc, valid, gpr[0].lo, gpr[0].hi);
                    }
                }
                size_t tail = (br >= STRUCT) ? STRUCT : br;
                memcpy(prev.data(), page.data() + STRUCT + br - tail, tail);
            }
        }

        if (next <= addr) break;
        addr = next;
    }
    // After pass 0 (MEM_IMAGE): priority — frozen-game > zero-cycle-game > BIOS-range
    if (pass == 0 && (frozen_game_addr || zero_cycle_addr || fallback_addr)) {
        if (frozen_game_addr) {
            AddLog("[Sync] Using frozen-cycle game cpuRegs at 0x%llX (PC=0x%08X) — PCSX2 was paused during scan",
                   (uint64_t)frozen_game_addr, frozen_game_pc);
            return frozen_game_addr;
        }
        if (zero_cycle_addr) {
            AddLog("[Sync] Using zero-cycle game cpuRegs at 0x%llX (PC=0x%08X) — may be stale",
                   (uint64_t)zero_cycle_addr, zero_cycle_pc);
            return zero_cycle_addr;
        }
        AddLog("[Sync] Using MEM_IMAGE BIOS-range fallback at 0x%llX (PC=0x%08X)", (uint64_t)fallback_addr, fallback_pc);
        return fallback_addr;
    }
    if (pass == 0)
        AddLog("[Sync] MEM_IMAGE pass complete (regions=%d) — no result, scanning heap...", regions_scanned);
    } // end two-pass loop

    if (frozen_game_addr) {
        AddLog("[Sync] Using frozen-cycle game cpuRegs at 0x%llX (PC=0x%08X, regions=%d) — PCSX2 was paused during scan",
               (uint64_t)frozen_game_addr, frozen_game_pc, regions_scanned);
        return frozen_game_addr;
    }
    if (zero_cycle_addr) {
        AddLog("[Sync] Using zero-cycle game cpuRegs at 0x%llX (PC=0x%08X, regions=%d) — may be stale",
               (uint64_t)zero_cycle_addr, zero_cycle_pc, regions_scanned);
        return zero_cycle_addr;
    }
    if (fallback_addr) {
        AddLog("[Sync] Using BIOS-range fallback cpuRegs at 0x%llX (PC=0x%08X, regions=%d)",
               (uint64_t)fallback_addr, fallback_pc, regions_scanned);
        return fallback_addr;
    }
    AddLog("[Sync] cpuRegs scan done: regions=%d validPCs=%d lowAddr=%d spZero=%d gpr0fail=%d gpr1fail=%d scorefail=%d",
           regions_scanned, valid_pcs_found, low_addr_rejects, sp_zero_rejects,
           gpr0_failures, gpr1_failures, score_failures);
    return 0;
}

// ---------------------------------------------------------------------------
// Per-frame sync
// ---------------------------------------------------------------------------
static void ReadEEWindow(uint32_t ps2_addr) {
    if (!ee_ram_base || ps2_addr >= 0x2000000) return;
    SIZE_T br = 0;
    ReadProcessMemory(pcsx2_handle,
        (LPCVOID)(ee_ram_base + ps2_addr),
        ee_ram_window, sizeof(ee_ram_window), &br);
    ee_ram_window_addr = ps2_addr;
}

// Returns true if a real host controller (XInput/SDL) drove pad0 this call --
// used by SyncFromRecomp() to know whether to arm the pad override (as
// opposed to the EE-RAM-scan fallback, which only makes sense against a real
// PCSX2 process and never applies in CPU_RECOMP mode).
static bool ReadPAD() {
    // Strategy 1: XInput (works if DS4Windows or Steam Input is running).
    // Maps XInput buttons to the PS2 DualShock2 bitmask (active-low, 0 = pressed).
    //
    // PS2 DualShock2 uint16_t layout:
    //   Bit 0  = Select   Bit 1  = L3       Bit 2  = R3       Bit 3  = Start
    //   Bit 4  = D-Up     Bit 5  = D-Right  Bit 6  = D-Down   Bit 7  = D-Left
    //   Bit 8  = L2       Bit 9  = R2       Bit 10 = L1       Bit 11 = R1
    //   Bit 12 = Triangle Bit 13 = Circle   Bit 14 = Cross    Bit 15 = Square
    if (cfg_pad_source == 0 || cfg_pad_source == 1) {
        XINPUT_STATE xs = {};
        static int  last_slot   = -1;
        static bool logged_none = false;
        // Auto (0): scan all slots, first one connected wins. XInput (1):
        // only look at the user-selected slot, so a second controller can be
        // dedicated to PS2 controller 2 without the two fighting over "first found".
        DWORD scan_begin = (cfg_pad_source == 1) ? (DWORD)cfg_pad_xinput_slot : 0;
        DWORD scan_end   = (cfg_pad_source == 1) ? (DWORD)cfg_pad_xinput_slot + 1 : XUSER_MAX_COUNT;
        for (DWORD i = scan_begin; i < scan_end; ++i) {
            if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
                if ((int)i != last_slot) {
                    AddLog("[PAD] XInput controller found on slot %u", i);
                    last_slot   = (int)i;
                    logged_none = false;
                }
                const auto& gp = xs.Gamepad;
                uint16_t ps2 = 0xFFFF;
                if (gp.wButtons & XINPUT_GAMEPAD_A)              ps2 &= ~(1 << 14); // Cross
                if (gp.wButtons & XINPUT_GAMEPAD_B)              ps2 &= ~(1 << 13); // Circle
                if (gp.wButtons & XINPUT_GAMEPAD_X)              ps2 &= ~(1 << 15); // Square
                if (gp.wButtons & XINPUT_GAMEPAD_Y)              ps2 &= ~(1 << 12); // Triangle
                if (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  ps2 &= ~(1 << 10); // L1
                if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ps2 &= ~(1 << 11); // R1
                if (gp.bLeftTrigger  > 32)                       ps2 &= ~(1 <<  8); // L2
                if (gp.bRightTrigger > 32)                       ps2 &= ~(1 <<  9); // R2
                if (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP)        ps2 &= ~(1 <<  4);
                if (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     ps2 &= ~(1 <<  5);
                if (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      ps2 &= ~(1 <<  6);
                if (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      ps2 &= ~(1 <<  7);
                if (gp.wButtons & XINPUT_GAMEPAD_START)          ps2 &= ~(1 <<  3); // Start
                if (gp.wButtons & XINPUT_GAMEPAD_BACK)           ps2 &= ~(1 <<  0); // Select
                if (gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     ps2 &= ~(1 <<  1); // L3
                if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    ps2 &= ~(1 <<  2); // R3
                pad0.buttons = ps2;
                pad0.lx = static_cast<uint8_t>((gp.sThumbLX  / 256) + 128);
                pad0.ly = static_cast<uint8_t>(-(gp.sThumbLY / 256) + 128);
                pad0.rx = static_cast<uint8_t>((gp.sThumbRX  / 256) + 128);
                pad0.ry = static_cast<uint8_t>(-(gp.sThumbRY / 256) + 128);
                return true;
            }
        }
        if (!logged_none) {
            AddLog("[PAD] No XInput controller found - falling back to EE RAM pad scan.");
            logged_none = true; last_slot = -1;
        }
    }

    // Strategy 1.5: SDL GameController - works directly with PS4 DualShock 4 over Bluetooth
    // without DS4Windows or XInput emulation. Same API PCSX2 uses internally.
    if (cfg_pad_source == 0 || cfg_pad_source == 2) {
        static SDL_GameController* sdl_gc        = nullptr;
        static int                 sdl_gc_index  = -1;
        static bool                sdl_logged_none = false;

        // Re-open if the controller was disconnected, or the user switched
        // which device index this port should use.
        if (sdl_gc && (!SDL_GameControllerGetAttached(sdl_gc) ||
                       (cfg_pad_source == 2 && sdl_gc_index != cfg_pad_sdl_index))) {
            AddLog("[PAD] SDL GameController disconnected.");
            SDL_GameControllerClose(sdl_gc);
            sdl_gc = nullptr;
            sdl_gc_index = -1;
        }

        if (!sdl_gc) {
            SDL_GameControllerUpdate();
            if (cfg_pad_source == 2) {
                // Explicit device index chosen by the user.
                if (cfg_pad_sdl_index >= 0 && cfg_pad_sdl_index < SDL_NumJoysticks() &&
                    SDL_IsGameController(cfg_pad_sdl_index)) {
                    sdl_gc = SDL_GameControllerOpen(cfg_pad_sdl_index);
                    if (sdl_gc) {
                        sdl_gc_index = cfg_pad_sdl_index;
                        AddLog("[PAD] SDL GameController opened: %s", SDL_GameControllerName(sdl_gc));
                        sdl_logged_none = false;
                    }
                }
            } else {
                for (int i = 0; i < SDL_NumJoysticks(); i++) {
                    if (SDL_IsGameController(i)) {
                        sdl_gc = SDL_GameControllerOpen(i);
                        if (sdl_gc) {
                            sdl_gc_index = i;
                            AddLog("[PAD] SDL GameController opened: %s", SDL_GameControllerName(sdl_gc));
                            sdl_logged_none = false;
                            break;
                        }
                    }
                }
            }
        }

        if (sdl_gc) {
            SDL_GameControllerUpdate();
            uint16_t ps2 = 0xFFFF;
            auto clr = [&](uint16_t bit, SDL_GameControllerButton b) {
                if (SDL_GameControllerGetButton(sdl_gc, b)) ps2 &= ~bit;
            };
            clr(1 << 14, SDL_CONTROLLER_BUTTON_A);              // Cross
            clr(1 << 13, SDL_CONTROLLER_BUTTON_B);              // Circle
            clr(1 << 15, SDL_CONTROLLER_BUTTON_X);              // Square
            clr(1 << 12, SDL_CONTROLLER_BUTTON_Y);              // Triangle
            clr(1 << 10, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);   // L1
            clr(1 << 11, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);  // R1
            clr(1 <<  4, SDL_CONTROLLER_BUTTON_DPAD_UP);
            clr(1 <<  5, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            clr(1 <<  6, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            clr(1 <<  7, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            clr(1 <<  3, SDL_CONTROLLER_BUTTON_START);          // Start / Options
            clr(1 <<  0, SDL_CONTROLLER_BUTTON_BACK);           // Select / Share
            clr(1 <<  1, SDL_CONTROLLER_BUTTON_LEFTSTICK);      // L3
            clr(1 <<  2, SDL_CONTROLLER_BUTTON_RIGHTSTICK);     // R3
            // L2/R2 are analog axes - treat as pressed past ~25% travel
            if (SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8192) ps2 &= ~(1 << 8);
            if (SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192) ps2 &= ~(1 << 9);

            pad0.buttons = ps2;

            auto axis_u8 = [](Sint16 v) -> uint8_t {
                return static_cast<uint8_t>((v / 256) + 128);
            };
            pad0.lx = axis_u8(SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_LEFTX));
            pad0.ly = axis_u8(SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_LEFTY));
            pad0.rx = axis_u8(SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_RIGHTX));
            pad0.ry = axis_u8(SDL_GameControllerGetAxis(sdl_gc, SDL_CONTROLLER_AXIS_RIGHTY));
            return true;
        }

        if (!sdl_logged_none) {
            AddLog("[PAD] No SDL GameController found - falling back to EE RAM scan.");
            sdl_logged_none = true;
        }
    }

    // Strategy 2: Read pad buttons written by the game into EE RAM after SIO2 poll.
    // Works with any PCSX2 input backend (raw HID, DualShock4, etc.). Only
    // meaningful against a real attached PCSX2 process, so it never
    // contributes a pad-override source in CPU_RECOMP mode.
    if (!ee_ram_base || !pcsx2_handle) return false;
    if (!pad_ee_offset)
        pad_ee_offset = FindPadInEERAM(pcsx2_handle, ee_ram_base);
    if (!pad_ee_offset) return false;

    uint16_t buttons = 0; SIZE_T br = 0;
    ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + pad_ee_offset), &buttons, 2, &br);
    if (br == 2) pad0.buttons = buttons;
    // Analog sticks: try offsets immediately after buttons (game-dependent)
    uint8_t sticks[4] = {128,128,128,128}; SIZE_T sr = 0;
    ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + pad_ee_offset + 2), sticks, 4, &sr);
    if (sr == 4) { pad0.rx=sticks[0]; pad0.ry=sticks[1]; pad0.lx=sticks[2]; pad0.ly=sticks[3]; }
    return false;
}

// ---------------------------------------------------------------------------
// Per-PS2-button host-input bindings (PCSX2-style). Reuses the same
// keyboard/XInput/SDL polling this file already does in ReadPAD() above, but
// resolves each PadInput slot independently instead of reading one whole
// device. This file is the debugger GUI process (SDL2 + ImGui) -- keyboard
// capture/polling uses SDL2, not raylib (raylib is only linked into the
// separate guest runtime process, ps2EntryRunner.exe).
// ---------------------------------------------------------------------------
std::string DescribeBinding(const PadBinding& b) {
    char buf[96];
    switch (b.deviceType) {
        case PadBindDeviceType::Keyboard: {
            const char* name = SDL_GetScancodeName((SDL_Scancode)b.inputCode);
            snprintf(buf, sizeof(buf), "Keyboard: %s", (name && name[0]) ? name : "?");
            return buf;
        }
        case PadBindDeviceType::XInput:
            if (b.isAxis) {
                snprintf(buf, sizeof(buf), "XInput %d: %s Trigger", b.deviceIndex, b.inputCode == 1 ? "Right" : "Left");
            } else {
                static const std::pair<uint32_t, const char*> kNames[] = {
                    {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"}, {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
                    {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
                    {XINPUT_GAMEPAD_DPAD_UP, "D-Up"}, {XINPUT_GAMEPAD_DPAD_DOWN, "D-Down"},
                    {XINPUT_GAMEPAD_DPAD_LEFT, "D-Left"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "D-Right"},
                    {XINPUT_GAMEPAD_START, "Start"}, {XINPUT_GAMEPAD_BACK, "Back"},
                    {XINPUT_GAMEPAD_LEFT_THUMB, "L3"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "R3"},
                };
                const char* name = "?";
                for (const auto& kv : kNames) if (kv.first == (uint32_t)b.inputCode) { name = kv.second; break; }
                snprintf(buf, sizeof(buf), "XInput %d: %s", b.deviceIndex, name);
            }
            return buf;
        case PadBindDeviceType::SdlGameController:
            if (b.isAxis) {
                const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)b.inputCode);
                snprintf(buf, sizeof(buf), "SDL %d: %s%s", b.deviceIndex, n ? n : "?", b.axisNegative ? "-" : "+");
            } else {
                const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b.inputCode);
                snprintf(buf, sizeof(buf), "SDL %d: %s", b.deviceIndex, n ? n : "?");
            }
            return buf;
        default:
            return "(unbound)";
    }
}

static bool PortHasAnyBinding(int port) {
    for (int i = 0; i < (int)PadInput::Count; ++i)
        if (cfg_pad_bindings[port][i].deviceType != PadBindDeviceType::None) return true;
    return false;
}

// Called once per frame from ShowInputLogger() while a capture is armed
// (g_pad_bind_capture_port >= 0). Scans keyboard, then all 4 XInput slots,
// then all connected SDL GameControllers, for the first newly-pressed
// digital input (or, for the 8 stick-direction pseudo-slots, the first axis
// past a deadzone in the wanted direction), and binds it.
void PollPadBindCapture() {
    if (g_pad_bind_capture_port < 0) return;
    const int port  = g_pad_bind_capture_port;
    const int input = g_pad_bind_capture_input;
    const bool isStickInput = input >= (int)PadInput::LeftStickXPos;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_pad_bind_capture_port = -1; return; }

    // 1. Keyboard: first scancode that transitioned to down this frame.
    static Uint8 s_prevKeys[SDL_NUM_SCANCODES] = {};
    int numKeys = 0;
    const Uint8* keys = SDL_GetKeyboardState(&numKeys);
    for (int sc = 0; sc < numKeys && sc < SDL_NUM_SCANCODES; ++sc) {
        if (sc == SDL_SCANCODE_ESCAPE) continue;
        if (keys[sc] && !s_prevKeys[sc]) {
            cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::Keyboard, 0, sc, false, false, 1.0f};
            memcpy(s_prevKeys, keys, sizeof(Uint8) * numKeys);
            g_pad_bind_capture_port = -1;
            SaveConfig();
            return;
        }
    }
    memcpy(s_prevKeys, keys, sizeof(Uint8) * numKeys);

    // 2. XInput: scan all 4 slots, first connected device that presses anything wins.
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_STATE xs = {};
        if (XInputGetState(slot, &xs) != ERROR_SUCCESS) continue;
        const auto& gp = xs.Gamepad;
        if (!isStickInput) {
            static const uint32_t kButtonMasks[] = {
                XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
                XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
                XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
            };
            for (uint32_t mask : kButtonMasks) {
                if (gp.wButtons & mask) {
                    cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::XInput, (int)slot, (int)mask, false, false, 1.0f};
                    g_pad_bind_capture_port = -1; SaveConfig(); return;
                }
            }
            if (gp.bLeftTrigger > 32) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::XInput, (int)slot, 0, true, false, 1.0f};
                g_pad_bind_capture_port = -1; SaveConfig(); return;
            }
            if (gp.bRightTrigger > 32) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::XInput, (int)slot, 1, true, false, 1.0f};
                g_pad_bind_capture_port = -1; SaveConfig(); return;
            }
        } else {
            auto pastDeadzone = [](SHORT v, bool positiveWanted) { return positiveWanted ? (v > 16000) : (v < -16000); };
            bool neg = (input == (int)PadInput::LeftStickXNeg || input == (int)PadInput::LeftStickYNeg ||
                        input == (int)PadInput::RightStickXNeg || input == (int)PadInput::RightStickYNeg);
            SHORT raw = (input <= (int)PadInput::LeftStickXNeg)  ? gp.sThumbLX
                      : (input <= (int)PadInput::LeftStickYNeg)  ? gp.sThumbLY
                      : (input <= (int)PadInput::RightStickXNeg) ? gp.sThumbRX : gp.sThumbRY;
            int axisCode = (input <= (int)PadInput::LeftStickXNeg)  ? 0
                         : (input <= (int)PadInput::LeftStickYNeg)  ? 1
                         : (input <= (int)PadInput::RightStickXNeg) ? 2 : 3;
            // XInput's Y axes are inverted relative to the PS2/ly,ry convention
            // (see ReadPAD()'s "-(gp.sThumbLY / 256)" above) -- flip here so
            // capturing "push down" binds the Down/Right(+) pseudo-slot, not Up.
            bool isYAxis = (axisCode == 1 || axisCode == 3);
            SHORT effectiveRaw = isYAxis ? (SHORT)-raw : raw;
            if (pastDeadzone(effectiveRaw, !neg)) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::XInput, (int)slot, axisCode, true, neg, 1.0f};
                g_pad_bind_capture_port = -1; SaveConfig(); return;
            }
        }
    }

    // 3. SDL GameController: scan all connected controllers.
    SDL_GameControllerUpdate();
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        bool bound = false;
        if (!isStickInput) {
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && !bound; ++b) {
                if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) {
                    cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::SdlGameController, i, b, false, false, 1.0f};
                    bound = true;
                }
            }
            if (!bound && SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::SdlGameController, i, SDL_CONTROLLER_AXIS_TRIGGERLEFT, true, false, 1.0f};
                bound = true;
            }
            if (!bound && SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::SdlGameController, i, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true, false, 1.0f};
                bound = true;
            }
        } else {
            SDL_GameControllerAxis axis =
                (input <= (int)PadInput::LeftStickXNeg)  ? SDL_CONTROLLER_AXIS_LEFTX  :
                (input <= (int)PadInput::LeftStickYNeg)  ? SDL_CONTROLLER_AXIS_LEFTY  :
                (input <= (int)PadInput::RightStickXNeg) ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_RIGHTY;
            bool wantNeg = (input == (int)PadInput::LeftStickXNeg || input == (int)PadInput::LeftStickYNeg ||
                            input == (int)PadInput::RightStickXNeg || input == (int)PadInput::RightStickYNeg);
            Sint16 v = SDL_GameControllerGetAxis(gc, axis);
            if (wantNeg ? (v < -16000) : (v > 16000)) {
                cfg_pad_bindings[port][input] = PadBinding{PadBindDeviceType::SdlGameController, i, (int)axis, true, wantNeg, 1.0f};
                bound = true;
            }
        }
        SDL_GameControllerClose(gc);
        if (bound) { g_pad_bind_capture_port = -1; SaveConfig(); return; }
    }
}

// Resolves cfg_pad_bindings[port][*] against live host input into a PS2-format
// payload (active-low buttons, sticks centered at 0x80). Returns true if the
// port has at least one bound input (mirrors ReadPAD()'s bool contract, used
// to decide WritePadOverride vs ClearPadOverride).
static bool ReadPadFromBindings(int port, uint16_t& buttons, uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry) {
    buttons = 0xFFFF;
    lx = ly = rx = ry = 0x80;
    bool any = false;

    auto resolveDigital = [](const PadBinding& b) -> bool {
        switch (b.deviceType) {
            case PadBindDeviceType::Keyboard: {
                int numKeys = 0;
                const Uint8* keys = SDL_GetKeyboardState(&numKeys);
                return b.inputCode >= 0 && b.inputCode < numKeys && keys[b.inputCode] != 0;
            }
            case PadBindDeviceType::XInput: {
                XINPUT_STATE xs = {};
                if (XInputGetState((DWORD)b.deviceIndex, &xs) != ERROR_SUCCESS) return false;
                if (b.isAxis) return b.inputCode == 0 ? (xs.Gamepad.bLeftTrigger > 32) : (xs.Gamepad.bRightTrigger > 32);
                return (xs.Gamepad.wButtons & (uint32_t)b.inputCode) != 0;
            }
            case PadBindDeviceType::SdlGameController: {
                SDL_GameController* gc = SDL_GameControllerOpen(b.deviceIndex);
                if (!gc) return false;
                bool pressed = b.isAxis ? (SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)b.inputCode) > 8192)
                                        : (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b.inputCode) != 0);
                SDL_GameControllerClose(gc);
                return pressed;
            }
            default: return false;
        }
    };

    // Returns 0..1 push amount contributed by this binding toward its bound
    // direction (already scaled by sensitivity -- PCSX2-style pressure).
    auto resolveAxisPush = [](const PadBinding& b) -> float {
        switch (b.deviceType) {
            case PadBindDeviceType::Keyboard: {
                int numKeys = 0;
                const Uint8* keys = SDL_GetKeyboardState(&numKeys);
                bool down = b.inputCode >= 0 && b.inputCode < numKeys && keys[b.inputCode] != 0;
                return down ? b.sensitivity : 0.0f;
            }
            case PadBindDeviceType::XInput: {
                XINPUT_STATE xs = {};
                if (XInputGetState((DWORD)b.deviceIndex, &xs) != ERROR_SUCCESS) return 0.0f;
                SHORT raw = b.inputCode == 0 ? xs.Gamepad.sThumbLX : b.inputCode == 1 ? xs.Gamepad.sThumbLY
                          : b.inputCode == 2 ? xs.Gamepad.sThumbRX : xs.Gamepad.sThumbRY;
                bool isYAxis = (b.inputCode == 1 || b.inputCode == 3); // see PollPadBindCapture's isYAxis comment
                float norm = (isYAxis ? -raw : raw) / 32767.0f;
                float v = b.axisNegative ? -norm : norm;
                return (v < 0.0f ? 0.0f : v) * b.sensitivity;
            }
            case PadBindDeviceType::SdlGameController: {
                SDL_GameController* gc = SDL_GameControllerOpen(b.deviceIndex);
                if (!gc) return 0.0f;
                Sint16 raw = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)b.inputCode);
                SDL_GameControllerClose(gc);
                float norm = raw / 32767.0f;
                float v = b.axisNegative ? -norm : norm;
                return (v < 0.0f ? 0.0f : v) * b.sensitivity;
            }
            default: return 0.0f;
        }
    };

    for (int i = 0; i < 16; ++i) {
        const PadBinding& b = cfg_pad_bindings[port][i];
        if (b.deviceType == PadBindDeviceType::None) continue;
        any = true;
        if (resolveDigital(b)) buttons &= ~(uint16_t)(1u << i);
    }

    auto applyStick = [&](PadInput posInput, PadInput negInput, uint8_t& axisOut) {
        const PadBinding& bp = cfg_pad_bindings[port][(int)posInput];
        const PadBinding& bn = cfg_pad_bindings[port][(int)negInput];
        float value = 0.0f;
        if (bp.deviceType != PadBindDeviceType::None) { any = true; value += resolveAxisPush(bp); }
        if (bn.deviceType != PadBindDeviceType::None) { any = true; value -= resolveAxisPush(bn); }
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        int scaled = (int)std::lround(128.0f + value * 127.0f);
        axisOut = (uint8_t)std::clamp(scaled, 0, 255);
    };
    applyStick(PadInput::LeftStickXPos,  PadInput::LeftStickXNeg,  lx);
    applyStick(PadInput::LeftStickYPos,  PadInput::LeftStickYNeg,  ly);
    applyStick(PadInput::RightStickXPos, PadInput::RightStickXNeg, rx);
    applyStick(PadInput::RightStickYPos, PadInput::RightStickYNeg, ry);

    return any;
}

static bool PineFlushJIT();
static int  PineGetStatus();

void SyncFromRecomp() {
    if (!g_recomp_backend.IsConnected()) {
        if (!g_recomp_backend.Connect()) {
            regs_valid = false;
            return;
        }
        AddLog("[Recomp] Connected to shared memory");
        if (cfg_autostart_framelog && !fl_capturing) {
            fl_capturing = true;
            AddLog("[FrameLog] Auto-started capture on attach");
        }
        if (!g_recomp_breakpoints.empty()) {
            int rearmed = 0;
            for (const auto& bp : g_recomp_breakpoints) {
                if (g_recomp_backend.AddBreakpoint(bp.addr, bp.cond_reg, bp.cond_value) >= 0)
                    ++rearmed;
            }
            AddLog("[Recomp] Re-armed %d breakpoint(s) after reconnect", rearmed);
        }
    }
    CpuRegs r;
    if (!g_recomp_backend.ReadRegs(r)) { regs_valid = false; return; }
    cpu_regs.PC      = r.pc;
    cpu_regs.HI.lo   = r.hi;
    cpu_regs.LO.lo   = r.lo;
    for (int i = 0; i < 32; ++i)
        cpu_regs.GPR[i].lo = static_cast<uint64_t>(r.gpr[i]);
    regs_valid     = true;
    g_pcsx2_paused = g_recomp_backend.IsPaused();

    if (const RecompDebugState* ext = g_recomp_backend.ReadExtended()) {
        gs_regs.PMODE    = ext->gs.pmode;
        gs_regs.SMODE2   = ext->gs.smode2;
        gs_regs.DISPFB1  = ext->gs.dispfb1;
        gs_regs.DISPLAY1 = ext->gs.display1;
        gs_regs.DISPFB2  = ext->gs.dispfb2;
        gs_regs.DISPLAY2 = ext->gs.display2;
        gs_regs.CSR      = ext->gs.csr;
        gs_pine_ok       = true; // gates the existing GS panel display, regardless of source

        // Feed the debugger's own host controller (XInput/SDL, selected via
        // cfg_pad_source/slot/index) into the running recomp, PCSX2-style,
        // then let the game's actual received input (ext->pad, below) drive
        // the Input tab display -- that way the tab shows ground truth of
        // what the guest saw, confirming the injection actually took.
        // Any per-button binding configured for this port (see the "Button
        // Bindings" table in the Input tab) switches that port from
        // whole-device passthrough to per-button resolution.
        if (PortHasAnyBinding(cfg_pad_port)) {
            uint16_t b; uint8_t blx, bly, brx, bry;
            if (ReadPadFromBindings(cfg_pad_port, b, blx, bly, brx, bry))
                g_recomp_backend.WritePadOverride(cfg_pad_port, b, blx, bly, brx, bry);
            else
                g_recomp_backend.ClearPadOverride(cfg_pad_port);
        } else if (ReadPAD()) {
            g_recomp_backend.WritePadOverride(cfg_pad_port, pad0.buttons, pad0.lx, pad0.ly, pad0.rx, pad0.ry);
        } else {
            g_recomp_backend.ClearPadOverride(cfg_pad_port);
        }

        const DbgPadSnapshot &guestPad = ext->pad[cfg_pad_port];
        pad0.buttons = guestPad.buttons;
        pad0.lx = guestPad.lx; pad0.ly = guestPad.ly;
        pad0.rx = guestPad.rx; pad0.ry = guestPad.ry;

        uint32_t log_count = ext->log_entry_count;
        if (log_count > kDbgMaxLogEntries) log_count = kDbgMaxLogEntries;
        g_recomp_log_entries.clear();
        g_recomp_log_entries.reserve(log_count);
        for (uint32_t i = 0; i < log_count; ++i) {
            g_recomp_log_entries.push_back({ext->log_entries[i].seq, std::string(ext->log_entries[i].text)});
        }
    } else {
        gs_pine_ok = false;
    }

    if (pc_history.empty() || FindFunctionStart(pc_history.back() & 0x1FFFFFFF) != FindFunctionStart(cpu_regs.PC & 0x1FFFFFFF)) {
        pc_history.push_back(cpu_regs.PC);
        if (pc_history.size() > 64) pc_history.pop_front();
    }
}

void SyncFromPCSX2() {
    if (!pcsx2_handle) return;

    // Preferred path: DebugServer plugin, version-agnostic (no struct offsets).
    // Retry the connect attempt at most once every 2s so a missing plugin
    // doesn't spam connect() every frame.
    if (!g_dbgserver.isConnected()) {
        static uint32_t s_last_dbgserver_try = 0;
        uint32_t now_ms = SDL_GetTicks();
        if (now_ms - s_last_dbgserver_try > 2000) {
            s_last_dbgserver_try = now_ms;
            if (g_dbgserver.connect()) {
                AddLog("[Sync] DebugServer plugin detected on port 21512 — using version-agnostic register reads");
                RearmPCSX2Breakpoints();
            }
        }
    }
    if (g_dbgserver.isConnected()) {
        // Throttle register reads to ~30Hz — readRegisters() is a blocking TCP
        // round-trip, and doing it every ImGui frame (up to 60Hz) stalls the
        // whole GUI on any PCSX2-side latency.
        static uint32_t s_last_regread_ms = 0;
        uint32_t now_ms = SDL_GetTicks();
        if (now_ms - s_last_regread_ms < 33) {
            return;
        }
        s_last_regread_ms = now_ms;

        PCSX2DebugServerClient::RawRegisters dr;
        if (g_dbgserver.readRegisters(dr)) {
            bool was_valid = regs_valid;
            cpu_regs.PC    = dr.pc;
            cpu_regs.HI.lo = dr.hi;
            cpu_regs.LO.lo = dr.lo;
            for (int i = 0; i < 32; ++i) cpu_regs.GPR[i].lo = dr.gpr[i];
            regs_valid   = true;
            sync_last_ok = true;
            if (!was_valid) {
                AddLog("[Sync][DebugServer] Reg snapshot: PC=0x%08X sp=0x%08X ra=0x%08X",
                       cpu_regs.PC, (uint32_t)cpu_regs.GPR[29].lo, (uint32_t)cpu_regs.GPR[31].lo);
                if (cfg_autostart_framelog && !fl_capturing) {
                    fl_capturing = true;
                    AddLog("[FrameLog] Auto-started capture on attach");
                }
            }
            if (pc_history.empty() || FindFunctionStart(pc_history.back() & 0x1FFFFFFF) != FindFunctionStart(cpu_regs.PC & 0x1FFFFFFF)) {
                pc_history.push_back(cpu_regs.PC);
                if (pc_history.size() > 64) pc_history.pop_front();
            }
            // Real breakpoints/watchpoints are armed in PCSX2 itself (via
            // set_breakpoint/set_memcheck below), so PCSX2 halts on its own —
            // this just detects that halt instead of polling PC against the
            // local list like the RPM fallback path does.
            //
            // Also keep g_pcsx2_paused in sync here (mirrors the RPM-fallback
            // path below) — this branch used to only track pause state via the
            // local `st.paused`, leaving g_pcsx2_paused stale for the Pause
            // button and the freeze/rescan detector whenever this plugin path
            // was active.
            {
                PCSX2DebugServerClient::Status st;
                if (g_dbgserver.getStatus(st)) {
                    // Explicit transition logging so it's observable from the
                    // log alone whether the server-side pause state actually
                    // flips after a Pause/Resume click, vs. the GUI simply
                    // failing to reflect a state that did change.
                    static bool s_lastLoggedPaused = false;
                    static bool s_havePaused       = false;
                    if (!s_havePaused || st.paused != s_lastLoggedPaused) {
                        AddLog("[PCSX2][DebugServer] paused: %s -> %s (pc=0x%08X)",
                               s_havePaused ? (s_lastLoggedPaused ? "true" : "false") : "?",
                               st.paused ? "true" : "false", st.pc);
                        s_lastLoggedPaused = st.paused;
                        s_havePaused       = true;
                    }
                    g_pcsx2_paused = st.paused;
                    if (st.paused && (g_bp_enabled || g_wp_enabled) && !g_bp_hit) {
                        uint32_t phys = st.pc & 0x1FFFFFFF;
                        g_bp_hit             = true;
                        g_bp_hit_addr        = phys;
                        g_disasm_follow_pc   = false;
                        g_disasm_pinned      = phys;
                        g_disasm_scroll_req  = true;
                        AddLog("[BP][DebugServer] Halted at 0x%08X", phys);
                    }
                }
            }

            // Real-time disassembly under the DebugServer plugin.
            // Register values stream over JSON above, but the disasm view reads
            // instruction *bytes* from ee_ram_window, which is filled by RPM
            // (ReadEEWindow) using ee_ram_base. That discovery + read only ran
            // on the RPM fallback below, which this branch never reaches — so
            // the disasm only populated when the socket transiently dropped.
            // The two paths are orthogonal: discover ee_ram_base (once) and
            // refresh the EE window here so disasm follows PC live, no pause.
            if (!ee_ram_base) {
                static uint32_t s_ee_scan_log = 0;
                if (s_ee_scan_log++ % 120 == 0)
                    AddLog("[Sync][DebugServer] Scanning for EE RAM (for live disasm)...");
                ee_ram_base = FindEERAM(pcsx2_handle);
                if (ee_ram_base)
                    AddLog("[Sync][DebugServer] EE RAM at 0x%llX", (uint64_t)ee_ram_base);
            }
            if (ee_ram_base) {
                uint32_t phys_pc = cpu_regs.PC & 0x1FFFFFFF;
                if (g_disasm_follow_pc) {
                    if (phys_pc >= 0x80000 && phys_pc < 0x2000000) {
                        uint32_t win_start = (phys_pc >= 1024) ? (phys_pc - 1024) & ~3u : 0u;
                        ReadEEWindow(win_start);
                    }
                } else if (g_disasm_pinned >= 0x80000 && g_disasm_pinned < 0x2000000) {
                    uint32_t win_start = (g_disasm_pinned >= 1024) ? (g_disasm_pinned - 1024) & ~3u : 0u;
                    ReadEEWindow(win_start);
                }
            }
            return;
        }
        // Plugin connected but the read failed (e.g. PCSX2 closed, or the
        // reply shape doesn't match what readRegisters anchors on). Dump the
        // raw reply head once/second so a shape mismatch is visible in THIS
        // log (the client's stderr diagnostic doesn't reach the GUI log file),
        // instead of being inferred from the reconnect storm.
        {
            static uint32_t s_last_fail_log = 0;
            uint32_t now_ms2 = SDL_GetTicks();
            if (now_ms2 - s_last_fail_log > 1000) {
                s_last_fail_log = now_ms2;
                const std::string& raw = g_dbgserver.lastRawReply();
                AddLog("[Sync][DebugServer] readRegisters FAILED; reply head: %.200s",
                       raw.empty() ? "(empty — recv timeout/socket drop)" : raw.c_str());
            }
        }
        // Drop it and fall through to the RPM fallback below.
        g_dbgserver.disconnect();
    }

    // Fallback: RPM + hardcoded cpuRegs struct offsets (breaks across PCSX2
    // versions that change the struct layout — kept only for builds without
    // the DebugServer plugin).

    // Discover EE RAM
    if (!ee_ram_base) {
        if (sync_count % 60 == 0)
            AddLog("[Sync] Scanning for EE RAM in PCSX2 (attempt %d)...", sync_count / 60 + 1);
        ee_ram_base = FindEERAM(pcsx2_handle);
        if (ee_ram_base)
            AddLog("[Sync] EE RAM at 0x%llX", (uint64_t)ee_ram_base);
        else {
            sync_last_ok = false; sync_count++; return;
        }
    }

    // Discover cpuRegs — scan runs on a background thread to avoid blocking the UI.
    if (!cpu_regs_base) {
        uintptr_t result = g_scan_result.load();
        if (result > 1) {
            // Scan finished with a result
            cpu_regs_base = result;
            g_scan_result = 0;
            AddLog("[Sync] cpuRegs scan complete: 0x%llX", (uint64_t)cpu_regs_base);
        } else if (result == 0) {
            // Not running yet — kick it off.
            // Flush JIT first so cpuRegs has accurate GPR/sp values when the scan thread reads them.
            if (sync_count % 60 == 0) {
                AddLog("[Sync] Starting background cpuRegs scan...");
                PineFlushJIT();
            }
            StartCpuRegsScanAsync(pcsx2_handle);
        }
        // result == 1 means scan is in progress — just return and wait
        sync_last_ok = false; sync_count++; return;
    }

    // Throttle the RPM-fallback tick (PineGetStatus() + ReadProcessMemory) to
    // ~30Hz for the same reason as the DebugServer path above — PineGetStatus()
    // is a blocking TCP round-trip and doing it every ImGui frame stalls the GUI.
    {
        static uint32_t s_last_rpm_tick_ms = 0;
        uint32_t now_ms = SDL_GetTicks();
        if (now_ms - s_last_rpm_tick_ms < 33) {
            return;
        }
        s_last_rpm_tick_ms = now_ms;
    }

    // Check PINE status every tick — must be the first PINE call so any failure
    // disconnects us before SyncGSRegs or watchpoints send further commands.
    // The EE JIT only flushes GPRs to cpuRegs when PCSX2 is paused; MsgRead32
    // does NOT trigger a GPR flush. We read cpuRegs on every tick but the GPR
    // values will only be accurate when g_pcsx2_paused is true.
    {
        static uint32_t s_last_status_ms = 0;
        static bool     s_was_paused     = false;
        int status = PineGetStatus();
        if (status != 0 && status != 1) {
            // status == 2 (reset/shutdown) or -1 (TCP error, PineDisconnect already called)
            if (status == 2) {
                AddLog("[Sync] PCSX2 reset detected — clearing cached addresses");
                PineDisconnect();
            }
            cpu_regs_base = 0;
            g_scan_result = 0;
            ee_ram_base   = 0;
            pad_ee_offset = 0;
            gs_pine_ok    = false;
            regs_valid    = false;
            sync_last_ok  = false;
            sync_count++;
            return;
        }
        bool paused = (status == 1);
        g_pcsx2_paused = paused;
        // Throttle pause/resume log messages to 10 Hz
        uint32_t now_ms = SDL_GetTicks();
        if (now_ms - s_last_status_ms >= 100) {
            s_last_status_ms = now_ms;
            if (paused && !s_was_paused)
                AddLog("[Sync] PCSX2 paused — GPRs will be accurate this read");
            else if (!paused && s_was_paused)
                AddLog("[Sync] PCSX2 resumed — GPRs may be stale until next pause");
            s_was_paused = paused;
        }
    }

    // Read the full cpuRegs struct from PCSX2 memory.
    SIZE_T br = 0;
    ReadProcessMemory(pcsx2_handle, (LPCVOID)cpu_regs_base,
                      &cpu_regs, sizeof(cpu_regs), &br);

    bool was_valid = regs_valid;
    static const SIZE_T MIN_READ = offsetof(PCSX2_cpuRegs, cycle) + sizeof(uint32_t);

    regs_valid = (br >= MIN_READ);

    sync_last_ok = regs_valid;

    if (!regs_valid) {
        sync_last_err = GetLastError();
        cpu_regs_base = 0; // rediscover
        g_scan_result = 0;
        ee_ram_base   = 0; // PCSX2 may have reset — force full rediscovery
        pad_ee_offset = 0;
        PineDisconnect();  // stop sending PINE commands to a resetting PCSX2
        gs_pine_ok = false;
        if (sync_count % 60 == 0)
            AddLog("[Sync] cpuRegs read failed (err %lu, got %zu/%zu bytes), rediscovering...",
                   sync_last_err, br, sizeof(cpu_regs));
    } else {
        // Log a register snapshot the first time we successfully read cpuRegs
        if (!was_valid) {
            AddLog("[Sync] Reg snapshot: PC=0x%08X sp=0x%08X ra=0x%08X",
                   cpu_regs.PC,
                   (uint32_t)cpu_regs.GPR[29].lo,
                   (uint32_t)cpu_regs.GPR[31].lo);
            AddLog("[Sync]   v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X t0=0x%08X t1=0x%08X",
                   (uint32_t)cpu_regs.GPR[2].lo,  (uint32_t)cpu_regs.GPR[3].lo,
                   (uint32_t)cpu_regs.GPR[4].lo,  (uint32_t)cpu_regs.GPR[5].lo,
                   (uint32_t)cpu_regs.GPR[8].lo,  (uint32_t)cpu_regs.GPR[9].lo);
            if (cfg_autostart_framelog && !fl_capturing) {
                fl_capturing = true;
                AddLog("[FrameLog] Auto-started capture on attach");
            }
        }

        // Track per-register changes for UI highlight
        uint32_t now_tick = SDL_GetTicks();
        for (int g = 1; g < 32; g++) {
            if (cpu_regs.GPR[g].lo != gpr_prev_lo[g]) {
                gpr_prev_lo[g]     = cpu_regs.GPR[g].lo;
                gpr_changed_tick[g] = now_tick;
            }
        }
        // Re-validate EE RAM now that we have a real PC.
        // FindEERAM may have grabbed the wrong 32MB region on first pass;
        // check whether the current candidate has live code at PC, and if not, re-pick.
        {
            uint32_t phys = cpu_regs.PC & 0x1FFFFFFF;
            if (phys >= 0x80000 && phys < 0x2000000) {
                uint8_t check[16] = {}; SIZE_T chk = 0;
                ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + phys), check, sizeof(check), &chk);
                bool all_zero = true;
                for (int i = 0; i < (int)chk; i++) if (check[i]) { all_zero = false; break; }
                if (all_zero) {
                    uintptr_t better = FindEERAM(pcsx2_handle, cpu_regs.PC);
                    if (better && better != ee_ram_base) {
                        AddLog("[Sync] EE RAM corrected: 0x%llX -> 0x%llX (PC=0x%08X)",
                               (uint64_t)ee_ram_base, (uint64_t)better, cpu_regs.PC);
                        ee_ram_base = better;
                    }
                }
            }
        }

        // Detect stale cpuRegs: either cycle never advances (frozen snapshot) or
        // cycle advances but PC is stuck in BIOS range (latched backup struct).
        //
        // s_cycle_seen_nonzero gates the frozen check: we only start counting
        // "time since last cycle move" once cycle has been non-zero at least once.
        // This prevents the BIOS fallback (cycle=0 always) from immediately
        // triggering a rescan loop every 3s.
        {
            static uint32_t s_last_cycle          = 0;
            static uint32_t s_last_pc             = 0;
            static uint32_t s_bios_stuck_ms       = 0;
            static uint32_t s_cycle_moved_ms      = 0;
            static bool     s_cycle_seen_nonzero  = false;

            bool cycle_moved = (cpu_regs.cycle != s_last_cycle);
            bool pc_moved    = (cpu_regs.PC    != s_last_pc);

            if (cpu_regs.cycle != 0) s_cycle_seen_nonzero = true;
            if (cycle_moved) s_cycle_moved_ms = SDL_GetTicks();
            s_last_cycle = cpu_regs.cycle;
            s_last_pc    = cpu_regs.PC;

            // Init baseline on first valid read
            if (s_cycle_moved_ms == 0) s_cycle_moved_ms = SDL_GetTicks();

            bool bios_pc = (cpu_regs.PC >= 0x01000000 && cpu_regs.PC <= 0x01FFFFFF);
            if (!bios_pc || pc_moved) s_bios_stuck_ms = SDL_GetTicks();

            // Reset stale timer while paused — cycle never moves when PCSX2 is paused,
            // so we must not count paused time against the 3s window.
            if (g_pcsx2_paused) s_cycle_moved_ms = SDL_GetTicks();

            // Case 1: cycle never moved in 3s — but only fire if cycle was non-zero
            // at least once (guards against BIOS fallback with cycle=0 always frozen)
            bool cycle_frozen = (s_cycle_seen_nonzero && !cycle_moved && !g_pcsx2_paused &&
                                 (SDL_GetTicks() - s_cycle_moved_ms) > 3000);
            // Case 2: cycle advancing but BIOS PC frozen for 5s
            bool bios_frozen = (bios_pc && cycle_moved && !pc_moved &&
                                (SDL_GetTicks() - s_bios_stuck_ms) > 5000);

            if (cycle_frozen || bios_frozen) {
                AddLog("[Sync] Stale cpuRegs 0x%llX (%s): cycle=%u PC=0x%08X, rescanning...",
                       (uint64_t)cpu_regs_base,
                       cycle_frozen ? "cycle frozen" : "BIOS PC stuck",
                       cpu_regs.cycle, cpu_regs.PC);
                cpu_regs_base         = 0;
                regs_valid            = false;
                g_scan_result         = 0;
                s_bios_stuck_ms       = SDL_GetTicks();
                s_cycle_moved_ms      = 0;
                s_cycle_seen_nonzero  = false;
                sync_count++; return;
            }
        }

        // Track PC history
        if (pc_history.empty() || FindFunctionStart(pc_history.back() & 0x1FFFFFFF) != FindFunctionStart(cpu_regs.PC & 0x1FFFFFFF)) {
            pc_history.push_back(cpu_regs.PC);
            if (pc_history.size() > 64) pc_history.pop_front();
        }

        // Breakpoint check — normalize to physical (strip top 3 bits) so addresses
        // set from the disasm view (physical) match cpu_regs.PC (virtual kseg0/kseg1).
        if (g_bp_enabled && !g_bp_hit && !g_breakpoints.empty()) {
            uint32_t phys_pc_bp = cpu_regs.PC & 0x1FFFFFFF;
            for (uint32_t bp : g_breakpoints) {
                if (phys_pc_bp == (bp & 0x1FFFFFFF)) {
                    g_bp_hit            = true;
                    g_bp_hit_addr       = phys_pc_bp;
                    g_disasm_follow_pc  = false;
                    g_disasm_pinned     = phys_pc_bp;
                    g_disasm_scroll_req = true;
                    AddLog("[BP] Breakpoint hit at 0x%08X", phys_pc_bp);
                    break;
                }
            }
        }
        // Watchpoint poll — read watched EE RAM addresses each sync and detect changes.
        const bool wp_recomp_ok = (g_cpu_source == CPU_RECOMP && g_recomp_backend.IsConnected());
        if (g_wp_enabled && (ee_ram_base || wp_recomp_ok) && !g_watchpoints.empty()) {
            for (auto& wp : g_watchpoints) {
                if (wp.addr < 0x2000000u) {
                    uint32_t val = 0; SIZE_T br2 = 0;
                    if (wp_recomp_ok) {
                        if (g_recomp_backend.ReadMemory(wp.addr, &val, wp.size)) br2 = wp.size;
                    } else {
                        ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + wp.addr), &val, (SIZE_T)wp.size, &br2);
                    }
                    if (br2 == (SIZE_T)wp.size) {
                        uint32_t mask = (wp.size == 1) ? 0xFFu : (wp.size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
                        val &= mask;
                        if (!wp.seeded) {
                            wp.last_val = val;
                            wp.seeded   = true;
                        } else if (val != wp.last_val) {
                            if (!wp.hit)
                                AddLog("[WP] 0x%08X [%s]: 0x%0*X -> 0x%0*X",
                                       wp.addr, wp.size==1?"Byte":wp.size==2?"Half":"Word",
                                       wp.size*2, wp.last_val, wp.size*2, val);
                            wp.hit      = true;
                            wp.hit_val  = val;
                            wp.last_val = val;
                        }
                    }
                }
            }
        }
        // Read EE RAM window: follow PC normally, or stay pinned when user navigated away
        uint32_t phys_pc = cpu_regs.PC & 0x1FFFFFFF;
        if (g_disasm_follow_pc) {
            if (phys_pc >= 0x80000 && phys_pc < 0x2000000) {
                uint32_t win_start = (phys_pc >= 1024) ? (phys_pc - 1024) & ~3u : 0u;
                ReadEEWindow(win_start);
            }
        } else if (g_disasm_pinned >= 0x80000 && g_disasm_pinned < 0x2000000) {
            uint32_t win_start = (g_disasm_pinned >= 1024) ? (g_disasm_pinned - 1024) & ~3u : 0u;
            ReadEEWindow(win_start);
        }
        ReadPAD();
    }
    sync_count++;
}

// ---------------------------------------------------------------------------
// isPCSX2Running
// ---------------------------------------------------------------------------
bool isPCSX2Running() {
    DWORD found = FindPCSX2PID();
    if (found) {
        if (!pcsx2_handle) {
            pcsx2_pid = found;
            pcsx2_handle = OpenProcess(
                PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pcsx2_pid);
            if (pcsx2_handle)
                AddLog("[System] Connected to pcsx2-qt.exe (PID %lu)", pcsx2_pid);
        }
        return true;
    } else {
        if (pcsx2_handle) {
            CloseHandle(pcsx2_handle);
            pcsx2_handle   = NULL;
            ee_ram_base    = 0;
            cpu_regs_base  = 0;
            pad_ee_offset  = 0;
            regs_valid     = false;
            sync_last_ok   = false;
            AddLog("[System] PCSX2 disconnected.");
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// PINE IPC  -  TCP socket to pcsx2-qt for GS register reads
// PCSX2 2.6.x uses TCP on the configured slot port (default 28011)
// ---------------------------------------------------------------------------
static void PineDisconnect() {
    if (pine_sock != INVALID_SOCKET) {
        closesocket(pine_sock);
        pine_sock = INVALID_SOCKET;
    }
}

static void RefreshPineConfig() {
    std::string ini_path;
    if (cfg_pcsx2_path[0] != '\0') {
        char dir[512];
        DirFromPath(cfg_pcsx2_path, dir, sizeof(dir));
        // Check for portable install first
        std::string portable = std::string(dir) + "\\portable.txt";
        if (GetFileAttributesA(portable.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ini_path = std::string(dir) + "\\inis\\PCSX2.ini";
        }
    }
    
    if (ini_path.empty()) {
        char* appdata = nullptr;
        size_t len = 0;
        if (_dupenv_s(&appdata, &len, "AppData") == 0 && appdata) {
            ini_path = std::string(appdata) + "\\PCSX2\\config\\PCSX2.ini";
            free(appdata);
        }
    }

    std::ifstream f(ini_path);
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("IpcPort =") != std::string::npos) {
                pine_port = std::stoi(line.substr(line.find('=') + 1));
            }
        }
    }
}

static bool PineConnect() {
    if (pine_sock != INVALID_SOCKET) return true;
    RefreshPineConfig();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    pine_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pine_sock == INVALID_SOCKET) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)pine_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(pine_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(pine_sock);
        pine_sock = INVALID_SOCKET;
        return false;
    }
    DWORD tv = 500;
    setsockopt(pine_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    setsockopt(pine_sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
    return true;
}

static bool TcpSendAll(const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(pine_sock, (const char*)buf + sent, len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

static bool TcpRecvAll(uint8_t* buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(pine_sock, (char*)buf + got, len - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}


// Batch-reads n 64-bit values from PS2 addresses. Returns false on error (auto-disconnects).
static bool PineBatchRead64(const uint32_t* addrs, uint64_t* out, int n) {
    if (!PineConnect()) return false;

    // Request: [uint32 total_size][n x (0x03 MsgRead64 + uint32 addr)]
    int req_size = 4 + n * 5;
    std::vector<uint8_t> req(req_size);
    *(uint32_t*)req.data() = (uint32_t)req_size;
    for (int i = 0; i < n; i++) {
        req[4 + i*5] = 0x03;
        memcpy(&req[4 + i*5 + 1], &addrs[i], 4);
    }
    if (!TcpSendAll(req.data(), req_size)) { PineDisconnect(); return false; }

    // Read size header first, then exactly that many remaining bytes
    uint8_t hdr[4];
    if (!TcpRecvAll(hdr, 4)) { PineDisconnect(); return false; }
    uint32_t resp_total = *(uint32_t*)hdr;
    int payload = (int)resp_total - 4;
    if (payload <= 0) { PineDisconnect(); return false; }
    std::vector<uint8_t> resp(payload);
    if (!TcpRecvAll(resp.data(), payload)) { PineDisconnect(); return false; }

    // Each result: uint8 status + uint64 value (9 bytes); only parse if size matches
    int entries = payload / 9;
    for (int i = 0; i < n && i < entries; i++) {
        if (resp[i*9] == 0x00)
            memcpy(&out[i], &resp[i*9 + 1], 8);
        else
            out[i] = 0;
    }
    return true;
}

// Sends a single MsgRead32 at PS2 address 0x00000000.
// PCSX2 must flush JIT-allocated host registers to cpuRegs before servicing any
// IPC memory request, so this forces the GPR writeback we need before RPM.
// Returns true if PINE is connected and the flush was accepted.
static bool PineFlushJIT() {
    if (!PineConnect()) return false;
    // Request: [uint32 size=5][0x02 MsgRead32][uint32 addr=0x00000000]
    uint8_t req[9];
    *(uint32_t*)req = 9;
    req[4] = 0x02;
    *(uint32_t*)(req + 5) = 0x00000000;
    if (!TcpSendAll(req, 9)) { PineDisconnect(); return false; }
    // Response: [uint32 size][uint8 status][uint32 value] = 9 bytes
    uint8_t resp[9];
    if (!TcpRecvAll(resp, 4)) { PineDisconnect(); return false; }
    uint32_t resp_size = *(uint32_t*)resp;
    if (resp_size < 5) { PineDisconnect(); return false; }
    uint8_t payload[16];
    int to_read = (int)resp_size - 4;
    if (to_read > (int)sizeof(payload)) to_read = sizeof(payload);
    if (!TcpRecvAll(payload, to_read)) { PineDisconnect(); return false; }
    return true;
}

// Queries PCSX2 emulator state via PINE MsgStatus (0x0F).
// Returns: 0=Running, 1=Paused, 2=Shutdown, -1=PINE error.
// When PCSX2 is paused the EE JIT has flushed all GPRs to cpuRegs,
// so a ReadProcessMemory immediately after this returns 1 gives accurate values.
static int PineGetStatus() {
    if (!PineConnect()) return -1;
    uint8_t req[5];
    *(uint32_t*)req = 5;
    req[4] = 0x0F; // MsgStatus
    if (!TcpSendAll(req, 5)) { PineDisconnect(); return -1; }
    uint8_t hdr[4];
    if (!TcpRecvAll(hdr, 4)) { PineDisconnect(); return -1; }
    uint32_t resp_size = *(uint32_t*)hdr;
    if (resp_size < 6) { PineDisconnect(); return -1; }
    uint8_t payload[16];
    int to_read = (int)resp_size - 4;
    if (to_read > (int)sizeof(payload)) to_read = sizeof(payload);
    if (!TcpRecvAll(payload, to_read)) { PineDisconnect(); return -1; }
    if (payload[0] != 0x00) return -1;
    return (int)payload[1]; // 0=Running, 1=Paused, 2=Shutdown
}

bool PinePause() {
    if (!PineConnect()) return false;
    AddLog("[PINE] Sending MsgPause on port %d", pine_port);
    uint8_t req[5];
    *(uint32_t*)req = 5;
    req[4] = 0x11; // MsgPause
    if (!TcpSendAll(req, 5)) { PineDisconnect(); return false; }
    uint8_t hdr[4];
    if (!TcpRecvAll(hdr, 4)) { PineDisconnect(); return false; }
    uint32_t resp_size = *(uint32_t*)hdr;
    bool ok = true;
    if (resp_size > 4) {
        uint8_t payload[16];
        int to_read = (int)resp_size - 4;
        if (to_read > (int)sizeof(payload)) to_read = sizeof(payload);
        if (!TcpRecvAll(payload, to_read)) { PineDisconnect(); return false; }
        ok = (payload[0] == 0x00);
    }
    if (!ok) {
        AddLog("[PINE] MsgPause rejected by PCSX2 (non-zero status)");
        return false;
    }
    int status = PineGetStatus();
    if (status != 1) {
        AddLog("[PINE] MsgPause acked but status query reports %d (expected 1=Paused)", status);
        return false;
    }
    return true;
}

bool PineResume() {
    if (!PineConnect()) return false;
    AddLog("[PINE] Sending MsgResume on port %d", pine_port);
    uint8_t req[5];
    *(uint32_t*)req = 5;
    req[4] = 0x12; // MsgResume
    if (!TcpSendAll(req, 5)) { PineDisconnect(); return false; }
    uint8_t hdr[4];
    if (!TcpRecvAll(hdr, 4)) { PineDisconnect(); return false; }
    uint32_t resp_size = *(uint32_t*)hdr;
    bool ok = true;
    if (resp_size > 4) {
        uint8_t payload[16];
        int to_read = (int)resp_size - 4;
        if (to_read > (int)sizeof(payload)) to_read = sizeof(payload);
        if (!TcpRecvAll(payload, to_read)) { PineDisconnect(); return false; }
        ok = (payload[0] == 0x00);
    }
    if (!ok) {
        AddLog("[PINE] MsgResume rejected by PCSX2 (non-zero status)");
        return false;
    }
    int status = PineGetStatus();
    if (status != 0) {
        AddLog("[PINE] MsgResume acked but status query reports %d (expected 0=Running)", status);
        return false;
    }
    return true;
}

// Naive frame-advance: PINE exposes no per-instruction step or vsync wait, so
// this approximates "one frame" by resuming for ~16ms (one NTSC/PAL-ish frame
// interval) and re-pausing. Good enough for eyeballing animation progress;
// not frame-exact. True per-instruction stepping needs the DebugServer TCP
// plugin (see mcp__pcsx2__pcsx2_step), which this in-process GUI doesn't speak.
bool PineFrameAdvance() {
    if (!PineResume()) return false;
    Sleep(16);
    return PinePause();
}

// Disconnects the shared-memory view AND kills the previously launched
// ps2EntryRunner.exe process (if this GUI launched it) — used by both the
// Disconnect button and Restart (which kills, then relaunches).
void KillRecompProcess() {
    g_recomp_backend.Disconnect();
    regs_valid = false;

    // Disconnect() only unmaps the shared-memory view — it never killed the
    // previously launched ps2EntryRunner.exe, so clicking Restart/Launch
    // again just spawned a second instance on top of the still-running one.
    if (recomp_pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, recomp_pid);
        if (h) {
            if (TerminateProcess(h, 0))
                WaitForSingleObject(h, 5000);
            else
                AddLog("[Recomp] TerminateProcess failed (GetLastError=%lu) — a duplicate instance may launch", GetLastError());
            CloseHandle(h);
        }
        recomp_pid = 0;
    }
}

// Relaunches ps2EntryRunner.exe from cfg_runner_path (NOT this debugger's own
// exe — GetModuleFileNameA(nullptr) would return the debugger itself, which
// was the bug: clicking Restart relaunched the debugger instead of the recomp).
void RestartRecomp() {
    KillRecompProcess();

    if (!cfg_runner_path[0]) {
        AddLog("[Recomp] Cannot restart — no ps2EntryRunner.exe path set in Settings");
        return;
    }
    std::string cmdLine = std::string("\"") + cfg_runner_path + "\"";
    if (cfg_elf_path[0]) cmdLine += std::string(" \"") + cfg_elf_path + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char dir[512];
    DirFromPath(cfg_runner_path, dir, sizeof(dir));

    if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, dir[0] ? dir : nullptr, &si, &pi)) {
        recomp_pid = pi.dwProcessId;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        AddLog("[Recomp] Restarted ps2EntryRunner");
    } else {
        AddLog("[Recomp] Restart failed — could not launch %s", cfg_runner_path);
    }
}

// Kills the attached PCSX2 process (if any) and relaunches it from cfg_pcsx2_path.
// Requires cfg_pcsx2_path to be set in Settings — this GUI never launched PCSX2
// itself before now, it only ever attached to an already-running instance, so
// there's no other way to know which exe to relaunch.
//
// NOTE: there is no in-emulator "reset just the running game" command in either
// the DebugServer JSON protocol or the older PINE protocol (confirmed by reading
// debug-server-client.js in full and grepping this file's PINE Msg* usage — no
// reset/reboot command exists in either). So a genuine soft-reset that leaves the
// PCSX2 process untouched is not implementable against the plugins available here.
// What IS confirmed (via `pcsx2-qt.exe -help`) is a real CLI flag, -elf <file>,
// that boots straight into the ELF instead of PCSX2's BIOS/System Menu — so this
// still kills+relaunches the process, but the user no longer has to manually
// click through the BIOS menu afterward, which was the actual pain point behind
// both #3 and #4.
// Kills the attached PCSX2 process (if any) — used by both the Disconnect
// button and Restart (which kills, then relaunches).
void KillPCSX2Process() {
    DWORD pid = pcsx2_pid ? pcsx2_pid : FindPCSX2PID();
    if (pid) {
        AddLog("[PCSX2] Found running instance PID=%lu, attempting termination", pid);
        // pcsx2_handle is opened with PROCESS_VM_READ|PROCESS_QUERY_INFORMATION only
        // (see isPCSX2Running) — it lacks PROCESS_TERMINATE, so TerminateProcess on it
        // always fails and a duplicate instance gets launched on top. Open a fresh
        // handle with the rights we actually need here.
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h) {
            if (!TerminateProcess(h, 0)) {
                AddLog("[PCSX2] TerminateProcess failed (GetLastError=%lu) — a duplicate instance may launch", GetLastError());
            } else {
                DWORD wr = WaitForSingleObject(h, 5000);
                if (wr != WAIT_OBJECT_0)
                    AddLog("[PCSX2] WaitForSingleObject did not confirm exit (result=%lu) within 5s", wr);
            }
            CloseHandle(h);
        } else {
            AddLog("[PCSX2] OpenProcess(PROCESS_TERMINATE) failed for PID=%lu (GetLastError=%lu) — a duplicate instance will launch", pid, GetLastError());
        }
    } else {
        AddLog("[PCSX2] No running instance found to terminate");
    }
    if (pcsx2_handle) { CloseHandle(pcsx2_handle); pcsx2_handle = NULL; }
    pcsx2_pid     = 0;
    ee_ram_base   = 0;
    cpu_regs_base = 0;
    regs_valid    = false;
}

// Attach-only detach: drop our socket + forget our view of PCSX2, but leave the
// process (and its running ISO session) completely untouched. This is the path
// for debugging a real ISO the user booted themselves — killing PCSX2 out from
// under a manual ISO+breakpoint session (KillPCSX2Process) is exactly what we
// must NOT do there.
void DetachPCSX2Process() {
    g_dbgserver.disconnect();
    if (pcsx2_handle) { CloseHandle(pcsx2_handle); pcsx2_handle = NULL; }
    pcsx2_pid     = 0;
    ee_ram_base   = 0;
    cpu_regs_base = 0;
    regs_valid    = false;
    AddLog("[PCSX2] Detached — PCSX2 left running");
}

void RestartPCSX2() {
    KillPCSX2Process();

    if (!cfg_pcsx2_path[0]) {
        AddLog("[PCSX2] Cannot restart — no PCSX2 executable path set in Settings");
        return;
    }
    // Boot filename (positional) mounts the ISO as the CDVD source — without
    // it CDVD has nothing to read and GS never gets a picture, even though
    // -elf successfully overrides which code actually runs. Confirmed via
    // `pcsx2-qt.exe -help`: "-elf <file>" only overrides the boot ELF, it
    // does not mount a disc; disc mounting is the positional "[boot filename]"
    // argument. -disc is for a physical host DVD drive, not applicable here.
    std::string cmdLine = std::string("\"") + cfg_pcsx2_path + "\"";
    if (cfg_iso_path[0]) cmdLine += std::string(" \"") + cfg_iso_path + "\"";
    if (cfg_elf_path[0]) cmdLine += std::string(" -elf \"") + cfg_elf_path + "\" -fastboot";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char dir[512];
    DirFromPath(cfg_pcsx2_path, dir, sizeof(dir));
    if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, dir[0] ? dir : nullptr, &si, &pi)) {
        pcsx2_pid = pi.dwProcessId;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        AddLog(cfg_elf_path[0] ? "[PCSX2] Restarted, booting %s directly"
                                : "[PCSX2] Restarted (no ELF path set — will open to BIOS menu)",
               cfg_elf_path);
    } else {
        AddLog("[PCSX2] Restart failed — could not launch %s", cfg_pcsx2_path);
    }
}

// Pauses PCSX2 via PINE to force a JIT register flush, reads cpuRegs via RPM,
// stores results in g_gpr_snapshot[], then resumes. Returns true on success.
bool TakeGPRSnapshot() {
    if (!pcsx2_handle || !cpu_regs_base) return false;
    if (!PinePause()) return false;

    PCSX2_cpuRegs snap = {};
    SIZE_T br = 0;
    ReadProcessMemory(pcsx2_handle, (LPCVOID)cpu_regs_base, &snap, sizeof(snap), &br);
    bool ok = (br >= offsetof(PCSX2_cpuRegs, cycle) + sizeof(uint32_t));

    PineResume();

    if (ok) {
        for (int i = 0; i < 32; i++) g_gpr_snapshot[i] = snap.GPR[i].lo;
        g_gpr_snapshot_hi    = snap.HI.lo;
        g_gpr_snapshot_lo    = snap.LO.lo;
        g_gpr_snapshot_pc    = snap.PC;
        g_gpr_snapshot_valid = true;
        AddLog("[Snapshot] PC=0x%08X sp=0x%08X ra=0x%08X", snap.PC,
               (uint32_t)snap.GPR[29].lo, (uint32_t)snap.GPR[31].lo);
    }
    return ok;
}

void SyncGSRegs() {
    // SyncFromPCSX2 owns the PINE connection. Don't reconnect independently —
    // if it disconnected (e.g. PCSX2 reset), wait for it to reconnect first.
    if (pine_sock == INVALID_SOCKET) return;
    // GS registers don't change while the EE is paused. Skipping PINE traffic when
    // paused prevents PineBatchRead64 from crashing PCSX2 mid-reset (the most common
    // crash scenario: user pauses â†’ inspects â†’ hits Ctrl+R).
    if (g_pcsx2_paused) return;

    static uint32_t last_ok_tick  = 0;
    static uint32_t last_try_tick = 0;
    uint32_t now = SDL_GetTicks();
    if (gs_pine_ok) {
        if (now - last_ok_tick < 100) return; // 10 Hz when connected
        last_ok_tick = now;
    } else {
        if (now - last_try_tick < 2000) return; // 2s retry when disconnected
        last_try_tick = now;
    }

    static const uint32_t addrs[7] = {
        0x12001000, // PMODE
        0x12001020, // SMODE2
        0x12001070, // DISPFB1
        0x12001080, // DISPLAY1
        0x12001090, // DISPFB2
        0x120010A0, // DISPLAY2
        0x12001100, // CSR
    };
    uint64_t vals[7] = {};
    gs_pine_ok = PineBatchRead64(addrs, vals, 7);
    if (gs_pine_ok) {
        gs_regs.PMODE    = vals[0];
        gs_regs.SMODE2   = vals[1];
        gs_regs.DISPFB1  = vals[2];
        gs_regs.DISPLAY1 = vals[3];
        gs_regs.DISPFB2  = vals[4];
        gs_regs.DISPLAY2 = vals[5];
        gs_regs.CSR      = vals[6];
    }
}

// ---------------------------------------------------------------------------
// R5900 (EE MIPS) disassembler
// ---------------------------------------------------------------------------
bool IsGPRFlushed(const PCSX2_GPR& g) {
    return g.hi == 0ULL || g.hi == 0xFFFFFFFFFFFFFFFFULL;
}

// GPR_NAMES defined in debugger_state.cpp / declared in debugger_state.h

static const char* FPR_NAMES[32] = {
    "f0","f1","f2","f3","f4","f5","f6","f7",
    "f8","f9","f10","f11","f12","f13","f14","f15",
    "f16","f17","f18","f19","f20","f21","f22","f23",
    "f24","f25","f26","f27","f28","f29","f30","f31"
};

// Returns true if the instruction is a branch or jump (has a delay slot).
bool IsBranchOrJump(uint32_t code) {
    uint32_t op = code >> 26;
    uint32_t fn = code & 0x3F;
    if (op == 0x00 && (fn == 0x08 || fn == 0x09)) return true; // JR / JALR
    if (op == 0x01) return true; // REGIMM branches
    return (op == 0x02 || op == 0x03 ||  // J, JAL
            op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 || // BEQ BNE BLEZ BGTZ
            op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17);  // BEQL BNEL BLEZL BGTZL
}

void DisassembleR5900(uint32_t code, uint32_t pc, char* buf, size_t sz) {
    uint32_t op  = (code >> 26) & 0x3F;
    uint32_t rs  = (code >> 21) & 0x1F;
    uint32_t rt  = (code >> 16) & 0x1F;
    uint32_t rd  = (code >> 11) & 0x1F;
    uint32_t sa  = (code >>  6) & 0x1F;
    uint32_t fn  = code & 0x3F;
    int16_t  imm = (int16_t)(code & 0xFFFF);
    uint32_t immu = code & 0xFFFF;
    uint32_t br_target = pc + 4 + ((int32_t)imm << 2);
    uint32_t j_target  = ((pc + 4) & 0xF0000000) | ((code & 0x3FFFFFF) << 2);
    const char** R = GPR_NAMES;
    const char** F = FPR_NAMES;

    if (code == 0) { snprintf(buf, sz, "nop"); return; }

    switch (op) {
    // ------------------------------------------------------------------
    case 0x00: // SPECIAL
        switch (fn) {
        case 0x00: snprintf(buf, sz, "sll     %s, %s, %u",    R[rd], R[rt], sa); break;
        case 0x02: snprintf(buf, sz, "srl     %s, %s, %u",    R[rd], R[rt], sa); break;
        case 0x03: snprintf(buf, sz, "sra     %s, %s, %u",    R[rd], R[rt], sa); break;
        case 0x04: snprintf(buf, sz, "sllv    %s, %s, %s",    R[rd], R[rt], R[rs]); break;
        case 0x06: snprintf(buf, sz, "srlv    %s, %s, %s",    R[rd], R[rt], R[rs]); break;
        case 0x07: snprintf(buf, sz, "srav    %s, %s, %s",    R[rd], R[rt], R[rs]); break;
        case 0x08:
            if (rd == 31) snprintf(buf, sz, "jr      %s",         R[rs]);
            else          snprintf(buf, sz, "jr      %s",         R[rs]);
            break;
        case 0x09:
            if (rd == 31) snprintf(buf, sz, "jalr    %s",         R[rs]);
            else          snprintf(buf, sz, "jalr    %s, %s",     R[rd], R[rs]);
            break;
        case 0x0A: snprintf(buf, sz, "movz    %s, %s, %s",    R[rd], R[rs], R[rt]); break;
        case 0x0B: snprintf(buf, sz, "movn    %s, %s, %s",    R[rd], R[rs], R[rt]); break;
        case 0x0C: snprintf(buf, sz, "syscall"); break;
        case 0x0D: snprintf(buf, sz, "break"); break;
        case 0x0F: snprintf(buf, sz, "sync"); break;
        case 0x10: snprintf(buf, sz, "mfhi    %s",            R[rd]); break;
        case 0x11: snprintf(buf, sz, "mthi    %s",            R[rs]); break;
        case 0x12: snprintf(buf, sz, "mflo    %s",            R[rd]); break;
        case 0x13: snprintf(buf, sz, "mtlo    %s",            R[rs]); break;
        case 0x14: snprintf(buf, sz, "dsllv   %s, %s, %s",   R[rd], R[rt], R[rs]); break;
        case 0x16: snprintf(buf, sz, "dsrlv   %s, %s, %s",   R[rd], R[rt], R[rs]); break;
        case 0x17: snprintf(buf, sz, "dsrav   %s, %s, %s",   R[rd], R[rt], R[rs]); break;
        case 0x18: snprintf(buf, sz, "mult    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x19: snprintf(buf, sz, "multu   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x1A: snprintf(buf, sz, "div     %s, %s",        R[rs], R[rt]); break;
        case 0x1B: snprintf(buf, sz, "divu    %s, %s",        R[rs], R[rt]); break;
        case 0x1C: snprintf(buf, sz, "madd    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x1D: snprintf(buf, sz, "maddu   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x1E: snprintf(buf, sz, "plzcw   %s, %s",        R[rd], R[rs]); break;
        case 0x20: snprintf(buf, sz, "add     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x21:
            if (rs == 0) snprintf(buf, sz, "move    %s, %s",  R[rd], R[rt]);
            else         snprintf(buf, sz, "addu    %s, %s, %s", R[rd], R[rs], R[rt]);
            break;
        case 0x22: snprintf(buf, sz, "sub     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x23: snprintf(buf, sz, "subu    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x24: snprintf(buf, sz, "and     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x25:
            if (rt == 0) snprintf(buf, sz, "move    %s, %s",  R[rd], R[rs]);
            else         snprintf(buf, sz, "or      %s, %s, %s", R[rd], R[rs], R[rt]);
            break;
        case 0x26: snprintf(buf, sz, "xor     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x27: snprintf(buf, sz, "nor     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x28: snprintf(buf, sz, "mfsa    %s",            R[rd]); break;
        case 0x29: snprintf(buf, sz, "mtsa    %s",            R[rs]); break;
        case 0x2A: snprintf(buf, sz, "slt     %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x2B: snprintf(buf, sz, "sltu    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x2C: snprintf(buf, sz, "dadd    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x2D: snprintf(buf, sz, "daddu   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x2E: snprintf(buf, sz, "dsub    %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x2F: snprintf(buf, sz, "dsubu   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x30: snprintf(buf, sz, "tge     %s, %s",        R[rs], R[rt]); break;
        case 0x31: snprintf(buf, sz, "tgeu    %s, %s",        R[rs], R[rt]); break;
        case 0x32: snprintf(buf, sz, "tlt     %s, %s",        R[rs], R[rt]); break;
        case 0x33: snprintf(buf, sz, "tltu    %s, %s",        R[rs], R[rt]); break;
        case 0x34: snprintf(buf, sz, "teq     %s, %s",        R[rs], R[rt]); break;
        case 0x36: snprintf(buf, sz, "tne     %s, %s",        R[rs], R[rt]); break;
        case 0x38: snprintf(buf, sz, "dsll    %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3A: snprintf(buf, sz, "dsrl    %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3B: snprintf(buf, sz, "dsra    %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3C: snprintf(buf, sz, "dsll32  %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3E: snprintf(buf, sz, "dsrl32  %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3F: snprintf(buf, sz, "dsra32  %s, %s, %u",   R[rd], R[rt], sa); break;
        default:   snprintf(buf, sz, "special_%02X", fn); break;
        }
        break;

    // ------------------------------------------------------------------
    case 0x01: // REGIMM
        switch (rt) {
        case 0x00: snprintf(buf, sz, "bltz    %s, 0x%08X", R[rs], br_target); break;
        case 0x01: snprintf(buf, sz, "bgez    %s, 0x%08X", R[rs], br_target); break;
        case 0x02: snprintf(buf, sz, "bltzl   %s, 0x%08X", R[rs], br_target); break;
        case 0x03: snprintf(buf, sz, "bgezl   %s, 0x%08X", R[rs], br_target); break;
        case 0x10: snprintf(buf, sz, "bltzal  %s, 0x%08X", R[rs], br_target); break;
        case 0x11: snprintf(buf, sz, "bgezal  %s, 0x%08X", R[rs], br_target); break;
        case 0x12: snprintf(buf, sz, "bltzall %s, 0x%08X", R[rs], br_target); break;
        case 0x13: snprintf(buf, sz, "bgezall %s, 0x%08X", R[rs], br_target); break;
        case 0x18: snprintf(buf, sz, "mtsab   %s, 0x%04X", R[rs], immu); break;
        case 0x19: snprintf(buf, sz, "mtsah   %s, 0x%04X", R[rs], immu); break;
        default:   snprintf(buf, sz, "regimm_%02X", rt); break;
        }
        break;

    // ------------------------------------------------------------------
    case 0x02: snprintf(buf, sz, "j       0x%08X", j_target); break;
    case 0x03: snprintf(buf, sz, "jal     0x%08X", j_target); break;
    case 0x04:
        if (rs == rt) snprintf(buf, sz, "b       0x%08X", br_target);
        else          snprintf(buf, sz, "beq     %s, %s, 0x%08X", R[rs], R[rt], br_target);
        break;
    case 0x05:
        if (rt == 0) snprintf(buf, sz, "bnez    %s, 0x%08X", R[rs], br_target);
        else         snprintf(buf, sz, "bne     %s, %s, 0x%08X", R[rs], R[rt], br_target);
        break;
    case 0x06: snprintf(buf, sz, "blez    %s, 0x%08X",              R[rs], br_target); break;
    case 0x07: snprintf(buf, sz, "bgtz    %s, 0x%08X",              R[rs], br_target); break;
    case 0x08: snprintf(buf, sz, "addi    %s, %s, %d",              R[rt], R[rs], (int)imm); break;
    case 0x09:
        if (rs == 0) snprintf(buf, sz, "li      %s, %d",            R[rt], (int)imm);
        else         snprintf(buf, sz, "addiu   %s, %s, %d",        R[rt], R[rs], (int)imm);
        break;
    case 0x0A: snprintf(buf, sz, "slti    %s, %s, %d",              R[rt], R[rs], (int)imm); break;
    case 0x0B: snprintf(buf, sz, "sltiu   %s, %s, %d",              R[rt], R[rs], (int)imm); break;
    case 0x0C: snprintf(buf, sz, "andi    %s, %s, 0x%04X",          R[rt], R[rs], immu); break;
    case 0x0D: snprintf(buf, sz, "ori     %s, %s, 0x%04X",          R[rt], R[rs], immu); break;
    case 0x0E: snprintf(buf, sz, "xori    %s, %s, 0x%04X",          R[rt], R[rs], immu); break;
    case 0x0F: snprintf(buf, sz, "lui     %s, 0x%04X",              R[rt], immu); break;

    // ------------------------------------------------------------------
    case 0x10: // COP0
        switch (rs) {
        case 0x00: snprintf(buf, sz, "mfc0    %s, $%u",   R[rt], rd); break;
        case 0x04: snprintf(buf, sz, "mtc0    %s, $%u",   R[rt], rd); break;
        case 0x10:
            switch (fn) {
            case 0x01: snprintf(buf, sz, "tlbr"); break;
            case 0x02: snprintf(buf, sz, "tlbwi"); break;
            case 0x06: snprintf(buf, sz, "tlbwr"); break;
            case 0x08: snprintf(buf, sz, "tlbp"); break;
            case 0x18: snprintf(buf, sz, "eret"); break;
            case 0x38: snprintf(buf, sz, "ei"); break;
            case 0x39: snprintf(buf, sz, "di"); break;
            default:   snprintf(buf, sz, "cop0_co_%02X", fn); break;
            }
            break;
        default: snprintf(buf, sz, "cop0_%02X", rs); break;
        }
        break;

    // ------------------------------------------------------------------
    case 0x11: { // COP1 (FPU)
        uint32_t fs = rd, ft_f = rt, fd = sa;
        switch (rs) {
        case 0x00: snprintf(buf, sz, "mfc1    %s, %s",        R[rt], F[rd]); break;
        case 0x02: snprintf(buf, sz, "cfc1    %s, %s",        R[rt], F[rd]); break;
        case 0x04: snprintf(buf, sz, "mtc1    %s, %s",        R[rt], F[rd]); break;
        case 0x06: snprintf(buf, sz, "ctc1    %s, %s",        R[rt], F[rd]); break;
        case 0x08:
            switch (rt) {
            case 0: snprintf(buf, sz, "bc1f    0x%08X", br_target); break;
            case 1: snprintf(buf, sz, "bc1t    0x%08X", br_target); break;
            case 2: snprintf(buf, sz, "bc1fl   0x%08X", br_target); break;
            case 3: snprintf(buf, sz, "bc1tl   0x%08X", br_target); break;
            default: snprintf(buf, sz, "bc1_%02X", rt); break;
            }
            break;
        case 0x10: // .S format
            switch (fn) {
            case 0x00: snprintf(buf, sz, "add.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x01: snprintf(buf, sz, "sub.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x02: snprintf(buf, sz, "mul.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x03: snprintf(buf, sz, "div.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x04: snprintf(buf, sz, "sqrt.s  %s, %s",     F[fd], F[fs]); break;
            case 0x05: snprintf(buf, sz, "abs.s   %s, %s",     F[fd], F[fs]); break;
            case 0x06: snprintf(buf, sz, "mov.s   %s, %s",     F[fd], F[fs]); break;
            case 0x07: snprintf(buf, sz, "neg.s   %s, %s",     F[fd], F[fs]); break;
            case 0x16: snprintf(buf, sz, "rsqrt.s %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x18: snprintf(buf, sz, "adda.s  %s, %s",     F[fs], F[ft_f]); break;
            case 0x19: snprintf(buf, sz, "suba.s  %s, %s",     F[fs], F[ft_f]); break;
            case 0x1A: snprintf(buf, sz, "mula.s  %s, %s",     F[fs], F[ft_f]); break;
            case 0x1C: snprintf(buf, sz, "madd.s  %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x1D: snprintf(buf, sz, "msub.s  %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x1E: snprintf(buf, sz, "madda.s %s, %s",     F[fs], F[ft_f]); break;
            case 0x1F: snprintf(buf, sz, "msuba.s %s, %s",     F[fs], F[ft_f]); break;
            case 0x20: snprintf(buf, sz, "cvt.w.s %s, %s",     F[fd], F[fs]); break;
            case 0x24: snprintf(buf, sz, "max.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x25: snprintf(buf, sz, "min.s   %s, %s, %s", F[fd], F[fs], F[ft_f]); break;
            case 0x30: snprintf(buf, sz, "c.f.s   %s, %s",     F[fs], F[ft_f]); break;
            case 0x32: snprintf(buf, sz, "c.eq.s  %s, %s",     F[fs], F[ft_f]); break;
            case 0x34: snprintf(buf, sz, "c.lt.s  %s, %s",     F[fs], F[ft_f]); break;
            case 0x36: snprintf(buf, sz, "c.le.s  %s, %s",     F[fs], F[ft_f]); break;
            default:   snprintf(buf, sz, "cop1s_%02X", fn); break;
            }
            break;
        case 0x14: // .W format
            if (fn == 0x20) snprintf(buf, sz, "cvt.s.w %s, %s", F[fd], F[fs]);
            else            snprintf(buf, sz, "cop1w_%02X", fn);
            break;
        default: snprintf(buf, sz, "cop1_%02X", rs); break;
        }
        break;
    }

    // ------------------------------------------------------------------
    case 0x12: { // COP2 — VU0 macro mode
        // Field layout: rs[25:21] rt[20:16] rd[15:11] sa[10:6] fn[5:0]
        // For COP2 special (rs >= 0x10): dest = rs & 0xF, fd=sa, fs=rd, ft=rt
        static const char* bc4[] = {"x","y","z","w"};
        auto dest_str = [](uint32_t d, char* b) -> const char* {
            int k = 0;
            if (d & 8) b[k++] = 'x'; if (d & 4) b[k++] = 'y';
            if (d & 2) b[k++] = 'z'; if (d & 1) b[k++] = 'w';
            b[k] = '\0'; if (!k) { b[0]='0'; b[1]='\0'; } return b;
        };
        switch (rs) {
        case 0x00: snprintf(buf, sz, "qmfc2   %s, vf%u",     R[rt], rd); break;
        case 0x01: snprintf(buf, sz, "qmfc2.i %s, vf%u",     R[rt], rd); break;
        case 0x02:
            if (fn & 1) snprintf(buf, sz, "cfc2.i  %s, $vi%u", R[rt], rd);
            else        snprintf(buf, sz, "cfc2    %s, $vi%u",  R[rt], rd);
            break;
        case 0x04: snprintf(buf, sz, "qmtc2   vf%u, %s",     rd, R[rt]); break;
        case 0x05: snprintf(buf, sz, "qmtc2.i vf%u, %s",     rd, R[rt]); break;
        case 0x06:
            if (fn & 1) snprintf(buf, sz, "ctc2.i  $vi%u, %s", rd, R[rt]);
            else        snprintf(buf, sz, "ctc2    $vi%u, %s",  rd, R[rt]);
            break;
        case 0x08: // BC2
            switch (rt) {
            case 0: snprintf(buf, sz, "bc2f    0x%08X", br_target); break;
            case 1: snprintf(buf, sz, "bc2t    0x%08X", br_target); break;
            case 2: snprintf(buf, sz, "bc2fl   0x%08X", br_target); break;
            case 3: snprintf(buf, sz, "bc2tl   0x%08X", br_target); break;
            default: snprintf(buf, sz, "bc2_%02X  0x%08X", rt, br_target); break;
            }
            break;
        default: { // COP2 Special — VU0 macro mode (rs >= 0x10)
            uint32_t dest = rs & 0xF; // xyzw mask
            uint32_t vfd  = sa, vfs = rd, vft = rt; // VF registers
            char ds[5]; dest_str(dest, ds);
            if (fn <= 0x1B) { // broadcast ops: op = fn>>2, component = fn&3
                static const char* bcops[7] = {"vadd","vsub","vmadd","vmsub","vmax","vmini","vmul"};
                const char* op = bcops[fn >> 2], *bc = bc4[fn & 3];
                snprintf(buf, sz, "%s%s.%s vf%u, vf%u, vf%u%s", op, bc, ds, vfd, vfs, vft, bc);
            } else switch (fn) {
            // Q/I register ops (fn 0x1C-0x27)
            case 0x1C: snprintf(buf, sz, "vmulq.%s  vf%u, vf%u, Q",   ds, vfd, vfs); break;
            case 0x1D: snprintf(buf, sz, "vmaxi.%s  vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x1E: snprintf(buf, sz, "vmuli.%s  vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x1F: snprintf(buf, sz, "vminii.%s vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x20: snprintf(buf, sz, "vaddq.%s  vf%u, vf%u, Q",   ds, vfd, vfs); break;
            case 0x21: snprintf(buf, sz, "vmaddq.%s vf%u, vf%u, Q",   ds, vfd, vfs); break;
            case 0x22: snprintf(buf, sz, "vaddi.%s  vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x23: snprintf(buf, sz, "vmaddi.%s vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x24: snprintf(buf, sz, "vsubq.%s  vf%u, vf%u, Q",   ds, vfd, vfs); break;
            case 0x25: snprintf(buf, sz, "vmsubq.%s vf%u, vf%u, Q",   ds, vfd, vfs); break;
            case 0x26: snprintf(buf, sz, "vsubi.%s  vf%u, vf%u, I",   ds, vfd, vfs); break;
            case 0x27: snprintf(buf, sz, "vmsubi.%s vf%u, vf%u, I",   ds, vfd, vfs); break;
            // Two-source float ops (fn 0x28-0x2F)
            case 0x28: snprintf(buf, sz, "vadd.%s   vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x29: snprintf(buf, sz, "vmadd.%s  vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x2A: snprintf(buf, sz, "vmul.%s   vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x2B: snprintf(buf, sz, "vmax.%s   vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x2C: snprintf(buf, sz, "vsub.%s   vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x2D: snprintf(buf, sz, "vmsub.%s  vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            case 0x2E: snprintf(buf, sz, "vopmsub.%s vf%u, vf%u, vf%u",  ds, vfd, vfs, vft); break;
            case 0x2F: snprintf(buf, sz, "vmini.%s  vf%u, vf%u, vf%u",   ds, vfd, vfs, vft); break;
            // Integer ops (fn 0x30-0x35)
            case 0x30: snprintf(buf, sz, "viadd    $vi%u, $vi%u, $vi%u", vfd, vfs, vft); break;
            case 0x31: snprintf(buf, sz, "visub    $vi%u, $vi%u, $vi%u", vfd, vfs, vft); break;
            case 0x32: { // viaddi vt, vs, imm5 (ft=dest, fs=src, fd=imm)
                int imm5 = (int)(vfd << 27) >> 27;
                snprintf(buf, sz, "viaddi   $vi%u, $vi%u, %d", vft, vfs, imm5);
                break;
            }
            case 0x34: snprintf(buf, sz, "viand    $vi%u, $vi%u, $vi%u", vfd, vfs, vft); break;
            case 0x35: snprintf(buf, sz, "vior     $vi%u, $vi%u, $vi%u", vfd, vfs, vft); break;
            // Call/special
            case 0x38: { // vcallms: imm = ((rt<<10)|(rd<<5)|sa) * 8
                uint32_t imm15 = ((vft << 10) | (vfs << 5) | vfd) * 8;
                snprintf(buf, sz, "vcallms  0x%X", imm15);
                break;
            }
            case 0x39: snprintf(buf, sz, "vcallmsr $vi%u", vfs); break;
            // Load/store with component select from dest
            case 0x3E: { const char* c = (dest&8)?"x":(dest&4)?"y":(dest&2)?"z":"w";
                snprintf(buf, sz, "vilwr.%s  $vi%u, ($vi%u)", c, vft, vfs); break; }
            case 0x3F: { const char* c = (dest&8)?"x":(dest&4)?"y":(dest&2)?"z":"w";
                snprintf(buf, sz, "viswr.%s  $vi%u, ($vi%u)", c, vft, vfs); break; }
            default:   snprintf(buf, sz, "vu0.%02X.%s vf%u, vf%u, vf%u", fn, ds, vfd, vfs, vft); break;
            }
            break;
        }
        }
        break;
    }

    case 0x14:
        if (rs == rt) snprintf(buf, sz, "beql    %s, 0x%08X",          R[rs], br_target);
        else          snprintf(buf, sz, "beql    %s, %s, 0x%08X",       R[rs], R[rt], br_target);
        break;
    case 0x15:
        if (rt == 0) snprintf(buf, sz, "bneql   %s, 0x%08X",            R[rs], br_target);
        else         snprintf(buf, sz, "bnel    %s, %s, 0x%08X",         R[rs], R[rt], br_target);
        break;
    case 0x16: snprintf(buf, sz, "blezl   %s, 0x%08X",                  R[rs], br_target); break;
    case 0x17: snprintf(buf, sz, "bgtzl   %s, 0x%08X",                  R[rs], br_target); break;
    case 0x18: snprintf(buf, sz, "daddi   %s, %s, %d",                  R[rt], R[rs], (int)imm); break;
    case 0x19: snprintf(buf, sz, "daddiu  %s, %s, %d",                  R[rt], R[rs], (int)imm); break;
    case 0x1A: snprintf(buf, sz, "ldl     %s, %d(%s)",                  R[rt], (int)imm, R[rs]); break;
    case 0x1B: snprintf(buf, sz, "ldr     %s, %d(%s)",                  R[rt], (int)imm, R[rs]); break;

    // ------------------------------------------------------------------
    case 0x1C: { // MMI
        switch (fn) {
        case 0x00: snprintf(buf, sz, "madd    %s, %s, %s",  R[rd], R[rs], R[rt]); break;
        case 0x01: snprintf(buf, sz, "maddu   %s, %s, %s",  R[rd], R[rs], R[rt]); break;
        case 0x04: snprintf(buf, sz, "plzcw   %s, %s",      R[rd], R[rs]); break;
        case 0x08: // MMI0
            switch (sa) {
            case 0x00: snprintf(buf, sz, "paddw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x01: snprintf(buf, sz, "psubw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x02: snprintf(buf, sz, "pcgtw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x03: snprintf(buf, sz, "pmaxw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x04: snprintf(buf, sz, "paddh   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x05: snprintf(buf, sz, "psubh   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x06: snprintf(buf, sz, "pcgth   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x07: snprintf(buf, sz, "pmaxh   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x08: snprintf(buf, sz, "paddb   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x09: snprintf(buf, sz, "psubb   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0A: snprintf(buf, sz, "pcgtb   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x10: snprintf(buf, sz, "paddsw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x11: snprintf(buf, sz, "psubsw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x12: snprintf(buf, sz, "pextlw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x13: snprintf(buf, sz, "ppacw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x14: snprintf(buf, sz, "paddsh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x15: snprintf(buf, sz, "psubsh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x16: snprintf(buf, sz, "pextlh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x17: snprintf(buf, sz, "ppach   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x18: snprintf(buf, sz, "paddsb  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x19: snprintf(buf, sz, "psubsb  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1A: snprintf(buf, sz, "pextlb  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1B: snprintf(buf, sz, "ppacb   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1E: snprintf(buf, sz, "pext5   %s, %s",     R[rd], R[rt]); break;
            case 0x1F: snprintf(buf, sz, "ppac5   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            default:   snprintf(buf, sz, "mmi0_%02X", sa); break;
            }
            break;
        case 0x09: // MMI2
            switch (sa) {
            case 0x00: snprintf(buf, sz, "pmaddw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x02: snprintf(buf, sz, "psllvw  %s, %s, %s", R[rd], R[rt], R[rs]); break;
            case 0x03: snprintf(buf, sz, "psrlvw  %s, %s, %s", R[rd], R[rt], R[rs]); break;
            case 0x04: snprintf(buf, sz, "pmsubw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x08: snprintf(buf, sz, "pmfhi   %s",         R[rd]); break;
            case 0x09: snprintf(buf, sz, "pmflo   %s",         R[rd]); break;
            case 0x0A: snprintf(buf, sz, "pinth   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0C: snprintf(buf, sz, "pmultw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0D: snprintf(buf, sz, "pdivw   %s, %s",     R[rs], R[rt]); break;
            case 0x0E: snprintf(buf, sz, "pcpyld  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x10: snprintf(buf, sz, "pmaddh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x11: snprintf(buf, sz, "phmadh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x12: snprintf(buf, sz, "pand    %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x13: snprintf(buf, sz, "pxor    %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x14: snprintf(buf, sz, "pmsubh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x15: snprintf(buf, sz, "phmsbh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1A: snprintf(buf, sz, "pexeh   %s, %s",     R[rd], R[rt]); break;
            case 0x1B: snprintf(buf, sz, "prevh   %s, %s",     R[rd], R[rt]); break;
            case 0x1C: snprintf(buf, sz, "pmulth  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1D: snprintf(buf, sz, "pdivbw  %s, %s",     R[rs], R[rt]); break;
            case 0x1E: snprintf(buf, sz, "pexew   %s, %s",     R[rd], R[rt]); break;
            case 0x1F: snprintf(buf, sz, "prot3w  %s, %s",     R[rd], R[rt]); break;
            default:   snprintf(buf, sz, "mmi2_%02X", sa); break;
            }
            break;
        case 0x10: snprintf(buf, sz, "mfhi1   %s",            R[rd]); break;
        case 0x11: snprintf(buf, sz, "mthi1   %s",            R[rs]); break;
        case 0x12: snprintf(buf, sz, "mflo1   %s",            R[rd]); break;
        case 0x13: snprintf(buf, sz, "mtlo1   %s",            R[rs]); break;
        case 0x18: snprintf(buf, sz, "mult1   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x19: snprintf(buf, sz, "multu1  %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x1A: snprintf(buf, sz, "div1    %s, %s",        R[rs], R[rt]); break;
        case 0x1B: snprintf(buf, sz, "divu1   %s, %s",        R[rs], R[rt]); break;
        case 0x1C: snprintf(buf, sz, "madd1   %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x1D: snprintf(buf, sz, "maddu1  %s, %s, %s",   R[rd], R[rs], R[rt]); break;
        case 0x28: // MMI1
            switch (sa) {
            case 0x01: snprintf(buf, sz, "pabsw   %s, %s",     R[rd], R[rt]); break;
            case 0x02: snprintf(buf, sz, "pceqw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x03: snprintf(buf, sz, "pminw   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x04: snprintf(buf, sz, "padsbh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x05: snprintf(buf, sz, "pabsh   %s, %s",     R[rd], R[rt]); break;
            case 0x06: snprintf(buf, sz, "pceqh   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x07: snprintf(buf, sz, "pminh   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0A: snprintf(buf, sz, "pceqb   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x10: snprintf(buf, sz, "padduw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x11: snprintf(buf, sz, "psubuw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x12: snprintf(buf, sz, "pextuw  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x14: snprintf(buf, sz, "padduh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x15: snprintf(buf, sz, "psubuh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x16: snprintf(buf, sz, "pextuh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x18: snprintf(buf, sz, "paddub  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x19: snprintf(buf, sz, "psubub  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1A: snprintf(buf, sz, "pextub  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1B: snprintf(buf, sz, "qfsrv   %s, %s, %s", R[rd], R[rs], R[rt]); break;
            default:   snprintf(buf, sz, "mmi1_%02X", sa); break;
            }
            break;
        case 0x29: // MMI3
            switch (sa) {
            case 0x00: snprintf(buf, sz, "pmadduw %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x03: snprintf(buf, sz, "psravw  %s, %s, %s", R[rd], R[rt], R[rs]); break;
            case 0x08: snprintf(buf, sz, "pmthi   %s",         R[rs]); break;
            case 0x09: snprintf(buf, sz, "pmtlo   %s",         R[rs]); break;
            case 0x0A: snprintf(buf, sz, "pinteh  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0C: snprintf(buf, sz, "pmultuw %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x0D: snprintf(buf, sz, "pdivuw  %s, %s",     R[rs], R[rt]); break;
            case 0x0E: snprintf(buf, sz, "pcpyud  %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x12: snprintf(buf, sz, "por     %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x13: snprintf(buf, sz, "pnor    %s, %s, %s", R[rd], R[rs], R[rt]); break;
            case 0x1A: snprintf(buf, sz, "pexch   %s, %s",     R[rd], R[rt]); break;
            case 0x1B: snprintf(buf, sz, "pcpyh   %s, %s",     R[rd], R[rt]); break;
            case 0x1E: snprintf(buf, sz, "pexcw   %s, %s",     R[rd], R[rt]); break;
            default:   snprintf(buf, sz, "mmi3_%02X", sa); break;
            }
            break;
        case 0x30: { // PMFHL
            static const char* fmt[] = {"lw","uw","slw","lh","sh"};
            if (sa < 5) snprintf(buf, sz, "pmfhl.%s %s", fmt[sa], R[rd]);
            else        snprintf(buf, sz, "pmfhl_%u %s", sa, R[rd]);
            break;
        }
        case 0x31: snprintf(buf, sz, "pmthl.lw %s",          R[rs]); break;
        case 0x34: snprintf(buf, sz, "psllh   %s, %s, %u",   R[rd], R[rt], sa & 0xF); break;
        case 0x36: snprintf(buf, sz, "psrlh   %s, %s, %u",   R[rd], R[rt], sa & 0xF); break;
        case 0x37: snprintf(buf, sz, "psrah   %s, %s, %u",   R[rd], R[rt], sa & 0xF); break;
        case 0x3C: snprintf(buf, sz, "psllw   %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3E: snprintf(buf, sz, "psrlw   %s, %s, %u",   R[rd], R[rt], sa); break;
        case 0x3F: snprintf(buf, sz, "psraw   %s, %s, %u",   R[rd], R[rt], sa); break;
        default:   snprintf(buf, sz, "mmi_%02X", fn); break;
        }
        break;
    }

    // ------------------------------------------------------------------
    case 0x1E: snprintf(buf, sz, "lq      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x1F: snprintf(buf, sz, "sq      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x20: snprintf(buf, sz, "lb      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x21: snprintf(buf, sz, "lh      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x22: snprintf(buf, sz, "lwl     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x23: snprintf(buf, sz, "lw      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x24: snprintf(buf, sz, "lbu     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x25: snprintf(buf, sz, "lhu     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x26: snprintf(buf, sz, "lwr     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x27: snprintf(buf, sz, "lwu     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x28: snprintf(buf, sz, "sb      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x29: snprintf(buf, sz, "sh      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2A: snprintf(buf, sz, "swl     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2B: snprintf(buf, sz, "sw      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2C: snprintf(buf, sz, "sdl     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2D: snprintf(buf, sz, "sdr     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2E: snprintf(buf, sz, "swr     %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x2F: snprintf(buf, sz, "cache   0x%02X, %d(%s)", rt, (int)imm, R[rs]); break;
    case 0x31: snprintf(buf, sz, "lwc1    %s, %d(%s)",  F[rt], (int)imm, R[rs]); break;
    case 0x36: snprintf(buf, sz, "lqc2    vf%u, %d(%s)", rt, (int)imm, R[rs]); break;
    case 0x37: snprintf(buf, sz, "ld      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;
    case 0x39: snprintf(buf, sz, "swc1    %s, %d(%s)",  F[rt], (int)imm, R[rs]); break;
    case 0x3E: snprintf(buf, sz, "sqc2    vf%u, %d(%s)", rt, (int)imm, R[rs]); break;
    case 0x3F: snprintf(buf, sz, "sd      %s, %d(%s)",  R[rt], (int)imm, R[rs]); break;

    default: snprintf(buf, sz, "???_%02X", op); break;
    }
}

// Resolve a PS2 physical address against the symbol table,
// trying both the raw address and the kseg0 (0x80xxxxxx) form.
const char* GetSymbolAny(uint32_t phys) {
    const char* s = GetSymbolName(phys);
    if (!s) s = GetSymbolName(phys | 0x80000000);
    return s;
}

// ---------------------------------------------------------------------------
// UI -  Code Trace tab
// ---------------------------------------------------------------------------
// Scan backwards from phys_pc to find the start of the current function.
// Step 1: nearest symbol at or before PC sets the search floor.
// Step 2: scan [floor, pc) backwards via RPM for addiu $sp,$sp,-N (real prologue).
//         The last prologue before pc is the true function start — handles unnamed
//         functions that follow a named one with no symbol entry of their own.
// Returns the detected function-start address, or phys_pc if nothing found.
uint32_t FindFunctionStart(uint32_t phys_pc) {
    uint32_t sym_floor = 0;
    bool     sym_found = false;

    // Step 1 — nearest known symbol at or before PC (floor, not final answer)
    if (!symbol_table.empty()) {
        for (uint32_t candidate : { phys_pc, phys_pc | 0x80000000u }) {
            auto it = symbol_table.upper_bound(candidate);
            if (it != symbol_table.begin()) {
                --it;
                uint32_t sym_phys = it->first & 0x1FFFFFFFu;
                if (phys_pc >= sym_phys && phys_pc - sym_phys < 0x20000u) {
                    sym_floor = sym_phys;
                    sym_found = true;
                    break;
                }
            }
        }
    }

    // Step 2 — scan [scan_lo, pc) backwards for addiu $sp,$sp,-N.
    // Use sym_floor as lower bound when available; fall back to 1KB scan.
    uint32_t scan_lo = sym_found ? sym_floor
                                 : ((phys_pc > 0x400u) ? (phys_pc - 0x400u) & ~3u : 0u);
    scan_lo &= ~3u;
    uint32_t range   = (phys_pc & ~3u) - scan_lo;

    if (range >= 4u && range <= 0x20000u) {
        static uint8_t s_ffs_buf[0x20000];
        SIZE_T br = 0;

        if (pcsx2_handle && ee_ram_base && scan_lo < 0x2000000u) {
            ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + scan_lo),
                              s_ffs_buf, range, &br);
        } else {
            // Fall back to the 2KB EE RAM window
            uint32_t win_start = ee_ram_window_addr;
            uint32_t win_bytes = (uint32_t)sizeof(ee_ram_window);
            if (scan_lo >= win_start && scan_lo + range <= win_start + win_bytes) {
                memcpy(s_ffs_buf, ee_ram_window + (scan_lo - win_start), range);
                br = range;
            }
        }

        if (br >= 4u) {
            // Scan backwards — first hit from the end is closest to PC = real function start
            uint32_t scan = (uint32_t)(br & ~3u);
            while (scan >= 4u) {
                scan -= 4u;
                uint32_t word;
                memcpy(&word, s_ffs_buf + scan, 4);
                if ((word & 0xFFFF8000u) == 0x27BD8000u ||   // addiu  $sp,$sp,-N
                    (word & 0xFFFF8000u) == 0x67BD8000u)      // daddiu $sp,$sp,-N (64-bit)
                    return scan_lo + scan;
            }
        }
    }

    return sym_found ? sym_floor : phys_pc;
}

// ShowBreakpoints() — moved to tab_breakpoints.cpp

// ShowRegWatch() — moved to tab_regwatch.cpp

// ShowCodeTrace() — moved to tab_codetrace.cpp

// ---------------------------------------------------------------------------
// UI — Call Stack tab
// ---------------------------------------------------------------------------
// ShowCallStack() — moved to tab_callstack.cpp

// ---------------------------------------------------------------------------
// UI - Memory Viewer tab
// ---------------------------------------------------------------------------
// ShowMemoryViewer() — moved to tab_memoryviewer.cpp

// ---------------------------------------------------------------------------
// UI -  Frame Logger tab
// ---------------------------------------------------------------------------
// ShowFrameLogger() — moved to tab_framelogger.cpp

// ---------------------------------------------------------------------------
// UI -  Graphics tab
// ---------------------------------------------------------------------------
// ShowGraphicsLogger() — moved to tab_graphicslogger.cpp

// ---------------------------------------------------------------------------
// UI -  Symbols tab
// ---------------------------------------------------------------------------
// ShowSymbolSyncTool() — moved to tab_symbolsynctool.cpp

// ---------------------------------------------------------------------------
// Controller texture loader — call once after GL context is ready
// ---------------------------------------------------------------------------
static GLuint LoadTexWhiteToAlpha(const char* path, int* out_w = nullptr, int* out_h = nullptr) {
    int w, h, n;
    stbi_uc* px = stbi_load(path, &w, &h, &n, STBI_rgb_alpha);
    if (!px) return 0;
    for (int i = 0; i < w * h; ++i) {
        stbi_uc* p = px + i * 4;
        if (p[0] > 230 && p[1] > 230 && p[2] > 230) p[3] = 0;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return t;
}

static GLuint LoadTexRGBA(const char* path) {
    int w, h, n;
    stbi_uc* px = stbi_load(path, &w, &h, &n, STBI_rgb_alpha);
    if (!px) return 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return t;
}

static void LoadControllerTextures() {
    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe_buf, MAX_PATH);
    wchar_t* last = wcsrchr(exe_buf, L'\\');
    if (last) *(last + 1) = L'\0';

    char exe_dir[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, exe_buf, -1, exe_dir, MAX_PATH, nullptr, nullptr);
    std::string base = std::string(exe_dir) + "assets\\controller\\";
    std::string btn  = base + "buttons\\";

    g_ctrl_tex_base    = LoadTexWhiteToAlpha((base + "Base Color.png").c_str(), &g_ctrl_img_w, &g_ctrl_img_h);
    g_ctrl_tex_outline = LoadTexWhiteToAlpha((base + "Outline Top Layer.png").c_str());
    g_ctrl_tex_analog  = LoadTexWhiteToAlpha((base + "Analog Light.png").c_str());

    if (!g_ctrl_tex_base)
        AddLog("[Inputs] Controller PNGs not found — place them in: %s", base.c_str());
    else
        AddLog("[Inputs] Controller images loaded (%dx%d)", g_ctrl_img_w, g_ctrl_img_h);

    static const struct { GLuint* pTex; const char* fname; } kBtnFiles[] = {
        { &g_btn_tex[0].tex,        "L2-Button.png" },
        { &g_btn_tex[1].tex,        "R2-Button.png" },
        { &g_btn_tex[2].tex,        "L1-Button.png" },
        { &g_btn_tex[3].tex,        "R1-Button.png" },
        { &g_btn_tex[4].tex,        "D-Pad-Up.png" },
        { &g_btn_tex[5].tex,        "D-Pad-Down.png" },
        { &g_btn_tex[6].tex,        "D-Pad-Left.png" },
        { &g_btn_tex[7].tex,        "D-Pad-Right.png" },
        { &g_btn_tex[8].tex,        "Select-Button.png" },
        { &g_btn_tex[9].tex,        "Start-Button.png" },
        { &g_btn_tex[10].tex,       "Triangle-Button.png" },
        { &g_btn_tex[11].tex,       "Circle-Button.png" },
        { &g_btn_tex[12].tex,       "X-Button.png" },
        { &g_btn_tex[13].tex,       "Square-Button.png" },
        { &g_btn_tex[14].tex,       "L3-Button-(Left-Analog-Stick).png" },
        { &g_btn_tex[15].tex,       "R3-Button-(Right-Analog-Stick).png" },
        { &g_ctrl_tex_analog_btn,   "Analog-Button.png" },
    };
    int loaded = 0;
    for (const auto& f : kBtnFiles) {
        *f.pTex = LoadTexRGBA((btn + f.fname).c_str());
        if (*f.pTex) ++loaded;
    }
    AddLog("[Inputs] Button highlight PNGs loaded: %d/17 from %s", loaded, btn.c_str());
}

// ---------------------------------------------------------------------------
// UI — Label Queue tab
// ---------------------------------------------------------------------------
// ShowLabelQueue() — moved to tab_labelqueue.cpp

// ---------------------------------------------------------------------------
// UI -  Inputs tab — DS2 controller visualizer (layered PNG)
// ---------------------------------------------------------------------------
// ShowInputLogger() — moved to tab_inputlogger.cpp

// ---------------------------------------------------------------------------
// UI -  Settings tab
// ---------------------------------------------------------------------------
// ShowSettings() — moved to tab_settings.cpp

// ShowFileDialogs() — moved to tab_filedialogs.cpp

// ---------------------------------------------------------------------------
// Main GUI loop
// ---------------------------------------------------------------------------
int run_with_gui() {
    SetUnhandledExceptionFilter(CrashHandler);
    InitProjectRoot();

    // Tell Windows this process handles its own DPI scaling - must happen before any
    // window is created (before SDL_Init). Without this, Windows bitmap-stretches the
    // window on high-DPI displays and the OpenGL framebuffer mismatches the logical
    // size, producing horizontal stripe artifacts.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Belt-and-suspenders: also tell SDL in case it recreates any surfaces internally.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING,   "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) return -1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    SDL_Window* window = SDL_CreateWindow(
        "Recomp Debugger",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    // Window / taskbar icon
    {
        int iw, ih, in_;
        stbi_uc* ipx = stbi_load(
            "assets/REComp DeBugger Logo.png",
            &iw, &ih, &in_, STBI_rgb_alpha);
        if (!ipx) {
            AddLog("[Icon] Failed to load logo PNG: %s", stbi_failure_reason());
        } else {
            AddLog("[Icon] Logo loaded: %dx%d channels=%d", iw, ih, in_);
            SDL_Surface* icon = SDL_CreateRGBSurfaceFrom(
                ipx, iw, ih, 32, iw * 4,
                0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
            if (!icon) {
                AddLog("[Icon] SDL_CreateRGBSurfaceFrom failed: %s", SDL_GetError());
            } else {
                SDL_SetWindowIcon(window, icon);
                SDL_FreeSurface(icon);
                AddLog("[Icon] SDL_SetWindowIcon called OK");
            }
            stbi_image_free(ipx);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    static std::string s_imgui_ini = g_project_root + "\\Config\\imgui.ini";
    ImGui::GetIO().IniFilename = s_imgui_ini.c_str();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        ImVec4 green      = ImVec4(0.10f, 0.70f, 0.20f, 1.00f);
        ImVec4 green_hov  = ImVec4(0.15f, 0.85f, 0.28f, 1.00f);
        ImVec4 green_act  = ImVec4(0.08f, 0.55f, 0.16f, 1.00f);
        // Active tab
        st.Colors[ImGuiCol_TabActive]           = green;
        st.Colors[ImGuiCol_TabUnfocusedActive]  = green_act;
    }
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Grab GPU name from OpenGL renderer string and trim vendor suffixes (e.g. "/PCIe/SSE2")
    static std::string g_gpu_name = "Unknown GPU";
    {
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        if (renderer) {
            std::string r(renderer);
            auto slash = r.find('/');
            if (slash != std::string::npos) r = r.substr(0, slash);
            g_gpu_name = r;
        }
        std::string title = std::string("Recomp Debugger | " BUILD_STRING " | ") + g_gpu_name;
        SDL_SetWindowTitle(window, title.c_str());
    }

    LoadControllerTextures();

    InitLogFilenames();
    // Create/truncate the debug log at startup and stamp with build info
    {
        std::ofstream f(g_log_debug, std::ios::trunc);
        if (f) f << "SDBZ Recomp Debugger | " BUILD_STRING "\n";
    }
    {
        std::ofstream f(g_log_errors, std::ios::trunc);
        if (f) f << "SDBZ Recomp Debugger | " BUILD_STRING "\n";
    }
    LoadConfig();
    if (cfg_map_path[0])    { map_path_history.push_back(cfg_map_path); }
    if (cfg_pcsx2_path[0])  { pcsx2_path_history.push_back(cfg_pcsx2_path); }
    if (cfg_runner_path[0]) { runner_path_history.push_back(cfg_runner_path); }
    if (cfg_elf_path[0])    { elf_path_history.push_back(cfg_elf_path); }
    if (cfg_iso_path[0])    { iso_path_history.push_back(cfg_iso_path); }
    LoadMapFile(cfg_map_path);
    LoadRecompFunctionTable();

    bool done = false;
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) done = true;
        }

        bool connected = (g_cpu_source == CPU_RECOMP)
                             ? g_recomp_backend.IsConnected()
                             : isPCSX2Running();
        if (g_cpu_source == CPU_RECOMP)
            SyncFromRecomp();
        else
            SyncFromPCSX2();
        SyncGSRegs();
        FrameLogger_Tick();

        // Recomp telemetry (pc/cycle_count) only advances when the guest is
        // making calls through lookupFunction — a true non-yielding spin loop
        // (e.g. a poll-only WaitSema) can leave it frozen even though the
        // process is alive and "running". Track how long it's been since
        // pc/cycle_count last changed so the status line can say so honestly
        // instead of looking like a silent hang.
        static uint32_t s_telemetry_last_pc = 0;
        static uint64_t s_telemetry_last_cycles = 0;
        static double   s_telemetry_last_change_time = 0.0;
        static bool     s_telemetry_seen = false;
        bool telemetry_stale = false;
        if (connected && g_cpu_source == CPU_RECOMP) {
            CpuRegs regs;
            if (g_recomp_backend.ReadRegs(regs)) {
                double now = ImGui::GetTime();
                if (!s_telemetry_seen || regs.pc != s_telemetry_last_pc || regs.cycles != s_telemetry_last_cycles) {
                    s_telemetry_last_pc = regs.pc;
                    s_telemetry_last_cycles = regs.cycles;
                    s_telemetry_last_change_time = now;
                    s_telemetry_seen = true;
                }
                telemetry_stale = !g_recomp_backend.IsPaused() && (now - s_telemetry_last_change_time) > 2.0;
            }
        } else {
            s_telemetry_seen = false;
        }

        // Scale text with window width so snapping the debugger to half a
        // screen (or any narrow layout) doesn't smash the text together.
        // kReferenceWidth is the width the UI was laid out at 1.0x scale.
        {
            constexpr float kReferenceWidth = 1280.0f;
            float scale = (g_win_w > 0) ? (float)g_win_w / kReferenceWidth : 1.0f;
            ImGui::GetIO().FontGlobalScale = (scale < 0.75f) ? 0.75f : (scale > 1.5f ? 1.5f : scale);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Space toggles capture when no text field is active
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) {
            if (fl_capturing) {
                fl_capturing = false;
                fl_playback_idx = (int)frame_log.size() > 0 ? (int)frame_log.size() - 1 : 0;
                AddLog("[FrameLog] Capture stopped. %zu frames recorded.", frame_log.size());
            } else {
                frame_log.clear();
                fl_frame_num = 0;
                fl_playback_idx = 0;
                fl_capturing = true;
                AddLog("[FrameLog] Capture started.");
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("RecompConsole", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar);

        // Header bar
        ImGui::Text("Recomp Debugger | " BUILD_STRING " | %s", g_gpu_name.c_str());

        char status_buf[160];
        if (!connected) {
            snprintf(status_buf, sizeof(status_buf), "%s NOT FOUND",
                     g_cpu_source == CPU_RECOMP ? "RECOMP" : "PCSX2");
        } else if (!sync_last_ok) {
            snprintf(status_buf, sizeof(status_buf), "%s LINKED | SEARCHING...",
                     g_cpu_source == CPU_RECOMP ? "RECOMP" : "PCSX2");
        } else if (telemetry_stale) {
            snprintf(status_buf, sizeof(status_buf), "LINKED | PC 0x%08X | NO TELEMETRY (target likely in a non-yielding loop)", cpu_regs.PC);
        } else {
            const char* sym = GetSymbolName(cpu_regs.PC);
            if (sym)
                snprintf(status_buf, sizeof(status_buf), "LINKED | PC 0x%08X [%s]", cpu_regs.PC, sym);
            else
                snprintf(status_buf, sizeof(status_buf), "LINKED | PC 0x%08X", cpu_regs.PC);
        }

        // Status goes on its own line, right-aligned by its actual measured
        // width — never collides with the title regardless of GPU name
        // length or window width.
        float status_w = ImGui::CalcTextSize(status_buf).x;
        ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - status_w - 12.0f));
        ImVec4 status_color = !connected ? ImVec4(1,0,0,1)
                              : !sync_last_ok ? ImVec4(1,0.6f,0,1)
                              : telemetry_stale ? ImVec4(1,0.6f,0,1)
                                              : ImVec4(0,1,0,1);
        ImGui::TextColored(status_color, "%s", status_buf);

        // Action row — backend toggle + run controls. Works identically for
        // both backends; only the label/enabled-state and underlying call
        // change based on g_cpu_source. Replaces the old scattered
        // Pause/Resume/Restart/Launch buttons that used to live only inside
        // the Settings tab.
        {
            bool use_recomp = (g_cpu_source == CPU_RECOMP);

            if (ImGui::Button(use_recomp ? "Backend: Recomp" : "Backend: PCSX2")) {
                if (use_recomp) {
                    g_cpu_source = CPU_PCSX2;
                    g_recomp_backend.Disconnect();
                    regs_valid = false;
                    AddLog("[Backend] Switched to PCSX2");
                } else {
                    g_cpu_source = CPU_RECOMP;
                    AddLog("[Backend] Switched to Recomp — waiting for ps2EntryRunner to start");
                }
                cfg_default_backend = (g_cpu_source == CPU_RECOMP) ? 1 : 0;
                SaveConfig();
            }
            ImGui::SameLine();

            bool attached = use_recomp ? g_recomp_backend.IsConnected() : isPCSX2Running();
            bool paused   = use_recomp ? g_recomp_backend.IsPaused()    : g_pcsx2_paused;

            // Run / Disconnect
            if (!attached) {
                bool can_launch = use_recomp ? (cfg_runner_path[0] != '\0') : (cfg_pcsx2_path[0] != '\0');
                ImGui::BeginDisabled(!can_launch);
                if (ImGui::Button(use_recomp ? "Launch Recomp" : "Launch PCSX2")) {
                    if (use_recomp) { AddLog("[Recomp] Launching..."); RestartRecomp(); }
                    else            { AddLog("[PCSX2] Launching...");  RestartPCSX2(); }
                }
                ImGui::EndDisabled();
            } else {
                // PCSX2: "Detach" leaves the target running (attach-only workflow —
                // debugging a real ISO the user booted themselves; killing it would
                // destroy the session). "Kill" is still available for the boot-the-
                // ELF workflow. Recomp keeps the single kill-on-disconnect button.
                if (use_recomp) {
                    if (ImGui::Button("Disconnect")) {
                        KillRecompProcess();
                        AddLog("[Recomp] Disconnected and closed ps2EntryRunner");
                    }
                } else {
                    if (ImGui::Button("Detach")) {
                        DetachPCSX2Process();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Kill PCSX2")) {
                        KillPCSX2Process();
                        AddLog("[PCSX2] Closed PCSX2");
                    }
                }
            }
            ImGui::SameLine();

            // Play / Pause
            ImGui::BeginDisabled(!attached);
            if (ImGui::Button(paused ? "Play" : "Pause")) {
                if (use_recomp) {
                    if (paused) { g_recomp_backend.Resume(); AddLog("[Recomp] Resumed"); }
                    else        { g_recomp_backend.Pause();  AddLog("[Recomp] Pause requested"); }
                } else if (EnsurePCSX2DebugServerConnected()) {
                    // DebugServer is the sole transport for these controls (BUG-018) —
                    // its paused status, step(), and pause/resume all agree with each
                    // other; PINE is a separate PCSX2 subsystem that doesn't flip
                    // DebugServer's internal paused flag, so mixing them caused the
                    // controls to desync. No PINE fallback here anymore.
                    auto reasonStr = [&]() {
                        switch (g_dbgserver.lastResult()) {
                            case PCSX2DebugServerClient::LastResult::TimedOut: return " (timed out — connection dropped)";
                            case PCSX2DebugServerClient::LastResult::Error:    return " (socket error — connection dropped)";
                            default:                                          return " (server said ok:false)";
                        }
                    };
                    if (paused) { if (g_dbgserver.resume()) AddLog("[PCSX2][DebugServer] Resumed"); else AddLog("[PCSX2][DebugServer] Resume failed%s", reasonStr()); }
                    else        { if (g_dbgserver.pause())  AddLog("[PCSX2][DebugServer] Paused");  else AddLog("[PCSX2][DebugServer] Pause failed%s", reasonStr()); }
                } else {
                    AddLog("[PCSX2] Pause/Resume requires the DebugServer plugin (port 21512) — not connected");
                }
            }
            ImGui::SameLine();

            // Frame Advance (Step on Recomp)
            if (ImGui::Button("Frame Advance")) {
                if (use_recomp) { g_recomp_backend.Step(); AddLog("[Recomp] Step"); }
                else if (EnsurePCSX2DebugServerConnected()) {
                    // Real single-step via the DebugServer plugin (PCSX2's own
                    // debug API) — steps exactly one MIPS instruction, matching
                    // Recomp's Step(). Sole transport for this control (BUG-018);
                    // no PineFrameAdvance fallback (sleep-based, not frame-exact,
                    // and desynced from DebugServer's paused state).
                    if (g_dbgserver.step()) {
                        AddLog("[PCSX2][DebugServer] Step");
                        // step() arms a temp breakpoint at the post-step PC to
                        // know when to stop single-stepping; left in place, it
                        // immediately re-halts on the next Resume/continue,
                        // which looked like "the controls just don't work"
                        // (BUG-018's actual root cause). Clear it and restore
                        // the user's real breakpoints/watchpoints.
                        g_dbgserver.clearAllBreakpoints();
                        RearmPCSX2Breakpoints();
                    }
                    else {
                        const char* reason = "server said ok:false";
                        if (g_dbgserver.lastResult() == PCSX2DebugServerClient::LastResult::TimedOut) reason = "timed out — connection dropped";
                        else if (g_dbgserver.lastResult() == PCSX2DebugServerClient::LastResult::Error) reason = "socket error — connection dropped";
                        AddLog("[PCSX2][DebugServer] Step failed (%s)", reason);
                    }
                }
                else            { AddLog("[PCSX2] Frame advance requires the DebugServer plugin (port 21512) — not connected"); }
            }
            ImGui::SameLine();

            // Step Over — recomp only, needs an active call frame (gpr[31] != 0) to target.
            if (use_recomp) {
                ImGui::BeginDisabled(!paused);
                if (ImGui::Button("Step Over")) { g_recomp_backend.StepOver(); AddLog("[Recomp] Step over"); }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }

            // Step Over (PCSX2 backend, mirrors the Recomp Step Over button above)
            if (!use_recomp) {
                ImGui::BeginDisabled(!paused);
                if (ImGui::Button("Step Over")) {
                  if (EnsurePCSX2DebugServerConnected()) {
                    if (g_dbgserver.stepOver()) {
                        AddLog("[PCSX2][DebugServer] Step over");
                        // Same temp-breakpoint cleanup as Frame Advance above.
                        g_dbgserver.clearAllBreakpoints();
                        RearmPCSX2Breakpoints();
                    }
                    else {
                        const char* reason = "server said ok:false";
                        if (g_dbgserver.lastResult() == PCSX2DebugServerClient::LastResult::TimedOut) reason = "timed out — connection dropped";
                        else if (g_dbgserver.lastResult() == PCSX2DebugServerClient::LastResult::Error) reason = "socket error — connection dropped";
                        AddLog("[PCSX2][DebugServer] Step over failed (%s)", reason);
                    }
                  } else {
                      AddLog("[PCSX2] Step over requires the DebugServer plugin (port 21512) — not connected");
                  }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }

            // Restart
            if (ImGui::Button("Restart")) {
                if (use_recomp) { AddLog("[Recomp] Restarting ps2EntryRunner..."); RestartRecomp(); }
                else            { AddLog("[PCSX2] Restarting...");                RestartPCSX2(); }
            }
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        if (ImGui::BeginTabBar("TopLevelTabs")) {
            if (ImGui::BeginTabItem("Debug")) {
                // Dockable workspace: every panel below is a real ImGui window
                // docked into a shared dockspace, so the user can drag tabs
                // between columns, resize, and close/reopen panels freely.
                // Layout (position/size) persists automatically via imgui.ini;
                // open/closed state is a separate app-level concept ImGui
                // doesn't track, so it's persisted via cfg_open_* in
                // sdbz_debugger.ini (SaveConfig/LoadConfig) instead.
                bool& s_open_disasm      = cfg_open_disasm;
                bool& s_open_eeregs      = cfg_open_eeregs;
                bool& s_open_breakpoints = cfg_open_breakpoints;
                bool& s_open_memview     = cfg_open_memview;
                bool& s_open_framelog    = cfg_open_framelog;
                bool& s_open_runtimelog  = cfg_open_runtimelog;
                bool& s_open_symbols     = cfg_open_symbols;

                ImGuiID dockspace_id = ImGui::GetID("DebugDockSpace");

                // Only build the default layout if this dockspace node doesn't
                // already exist (e.g. nothing restored from imgui.ini yet) or
                // the user explicitly asked for Reset Layout. Without this
                // check we'd clobber the restored layout every single launch,
                // which is why "Reset Layout" never appeared to stick.
                static bool s_force_rebuild = false;
                if (s_force_rebuild || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
                    s_force_rebuild = false;
                    ImGui::DockBuilderRemoveNode(dockspace_id);
                    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetContentRegionAvail());

                    ImGuiID col1 = dockspace_id;
                    ImGuiID col2 = ImGui::DockBuilderSplitNode(col1, ImGuiDir_Left, 0.30f, nullptr, &col1);
                    ImGuiID col4 = ImGui::DockBuilderSplitNode(col1, ImGuiDir_Down, 0.30f, nullptr, &col1);

                    ImGui::DockBuilderDockWindow("Disassembly", col1);
                    ImGui::DockBuilderDockWindow("Memory Viewer", col4);
                    // Registers, breakpoints/watchpoints, frame log, runtime log,
                    // and symbols all live in the same dock node so they present
                    // as one tabbed group instead of scattering across several
                    // top-level windows.
                    ImGui::DockBuilderDockWindow("Registers", col2);
                    ImGui::DockBuilderDockWindow("Markers", col2);
                    ImGui::DockBuilderDockWindow("Frame Log", col2);
                    ImGui::DockBuilderDockWindow("Runtime Log", col2);
                    ImGui::DockBuilderDockWindow("Labels", col2);

                    ImGui::DockBuilderFinish(dockspace_id);
                }

                if (ImGui::Button("Windows...")) ImGui::OpenPopup("##debug_tab_ctx");
                if (ImGui::BeginPopup("##debug_tab_ctx")) {
                    ImGui::TextDisabled("Windows");
                    ImGui::Separator();
                    ImGui::MenuItem("Disassembly", nullptr, &s_open_disasm);
                    ImGui::MenuItem("Registers", nullptr, &s_open_eeregs);
                    ImGui::MenuItem("Markers", nullptr, &s_open_breakpoints);
                    ImGui::MenuItem("Memory Viewer", nullptr, &s_open_memview);
                    ImGui::MenuItem("Frame Log", nullptr, &s_open_framelog);
                    ImGui::MenuItem("Runtime Log", nullptr, &s_open_runtimelog);
                    ImGui::MenuItem("Labels", nullptr, &s_open_symbols);
                    if (ImGui::MenuItem("Reset Layout")) s_force_rebuild = true;
                    ImGui::EndPopup();
                }

                ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);

                // NOTE: ImGui::End() must only be called when Begin() was
                // actually called. Short-circuiting on s_open_* skips Begin()
                // while still leaving End() unconditional desyncs ImGui's
                // window stack and aborts the app the moment a panel is closed.
                if (s_open_disasm) {
                    if (ImGui::Begin("Disassembly", &s_open_disasm)) ShowCodeTrace();
                    ImGui::End();
                }
                if (s_open_eeregs) {
                    if (ImGui::Begin("Registers", &s_open_eeregs)) ShowRegisters();
                    ImGui::End();
                }
                if (s_open_breakpoints) {
                    if (ImGui::Begin("Markers", &s_open_breakpoints)) ShowBreakpoints();
                    ImGui::End();
                }
                if (s_open_memview) {
                    if (ImGui::Begin("Memory Viewer", &s_open_memview)) ShowMemoryViewer();
                    ImGui::End();
                }
                if (s_open_framelog) {
                    if (ImGui::Begin("Frame Log", &s_open_framelog)) ShowFrameLogger();
                    ImGui::End();
                }
                if (s_open_runtimelog) {
                    if (ImGui::Begin("Runtime Log", &s_open_runtimelog)) ShowRuntimeLog();
                    ImGui::End();
                }
                if (s_open_symbols) {
                    if (ImGui::Begin("Labels", &s_open_symbols)) ShowLabelsTab();
                    ImGui::End();
                }

                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                ShowSettings();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();

        // Bottom-right scan progress overlay
        {
            static float s_done_timer = 0.0f;
            bool scanning = g_scan_running.load();
            if (scanning) s_done_timer = 2.0f; // show "found" banner for 2s after scan ends

            if (scanning || s_done_timer > 0.0f) {
                if (!scanning) s_done_timer -= ImGui::GetIO().DeltaTime;

                // Drawn directly on the foreground draw list (not a regular
                // ImGui window) so it always paints on top of every docked
                // panel — a real window here could end up rendered behind a
                // dockspace child and become invisible.
                const float PAD = 10.0f;
                ImVec2 disp = ImGui::GetIO().DisplaySize;
                const float W = 240.0f, H = 44.0f;
                ImVec2 p0(disp.x - W - PAD, disp.y - H - PAD);
                ImVec2 p1(disp.x - PAD, disp.y - PAD);

                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(p0, p1, IM_COL32(15, 15, 15, 255), 4.0f);
                fg->AddRect(p0, p1, IM_COL32(255, 255, 255, 90), 4.0f, 0, 1.5f);

                ImU32 col = scanning ? IM_COL32(255, 204, 51, 255) : IM_COL32(51, 255, 102, 255);
                fg->AddText(ImVec2(p0.x + 8, p0.y + 6), col, scanning ? "Scanning cpuRegs..." : "cpuRegs found");

                float barX = p0.x + 8, barY = p0.y + H - 16, barW = W - 16, barH = 8;
                fg->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(50, 50, 50, 255));
                if (scanning) {
                    float t = fmodf((float)ImGui::GetTime(), 1.2f) / 1.2f;
                    float segW = barW * 0.35f;
                    float segX = barX + t * (barW - segW);
                    fg->AddRectFilled(ImVec2(segX, barY), ImVec2(segX + segW, barY + barH), col);
                } else {
                    fg->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), col);
                }
            }
        }

        ShowFileDialogs();
        ImGui::Render();

        // Use physical pixel size (drawable size) so the viewport is correct on high-DPI displays.
        SDL_GL_GetDrawableSize(window, &g_draw_w, &g_draw_h);
        SDL_GetWindowSize(window, &g_win_w, &g_win_h);
        glViewport(0, 0, g_draw_w, g_draw_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    SaveConfig(); // persist panel open/closed state (cfg_open_*) and other settings on exit

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
