#pragma once
#include <stddef.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <atomic>
#include <unordered_set>
#include "cpu_backend.h"
#include "recomp_backend.h"

// ---------------------------------------------------------------------------
// PCSX2 structs
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct PCSX2_GPR {
    uint64_t lo;
    uint64_t hi;
};
// Matches PCSX2 v2.6.3 pcsx2/R5900.h cpuRegisters exactly
struct PCSX2_cpuRegs {
    PCSX2_GPR GPR[32];       // 0x000
    PCSX2_GPR HI, LO;        // 0x200
    uint32_t  CP0[32];       // 0x220
    uint32_t  sa;            // 0x2A0
    uint32_t  IsDelaySlot;   // 0x2A4
    uint32_t  PC;            // 0x2A8
    uint32_t  code;          // 0x2AC
    uint32_t  PERF[4];       // 0x2B0
    uint32_t  eCycle[32];    // 0x2C0
    uint32_t  sCycle[32];    // 0x340
    uint32_t  cycle;         // 0x3C0
    uint32_t  interrupt;     // 0x3C4
    int32_t   branch;        // 0x3C8
    int32_t   opmode;        // 0x3CC
    uint32_t  tempcycles;    // 0x3D0
    uint32_t  dmastall;      // 0x3D4
    uint32_t  pcWriteback;   // 0x3D8
    uint32_t  nextEventCycle;// 0x3DC
    uint32_t  lastEventCycle;// 0x3E0
    uint32_t  lastCOP0Cycle; // 0x3E4
    uint32_t  lastPERFCycle[2]; // 0x3E8
};                           // total: 0x3F0 (1008 bytes)
#pragma pack(pop)

struct GS_PrivRegs {
    uint64_t PMODE;    // 0x12001000
    uint64_t SMODE2;   // 0x12001020
    uint64_t DISPFB1;  // 0x12001070
    uint64_t DISPLAY1; // 0x12001080
    uint64_t DISPFB2;  // 0x12001090
    uint64_t DISPLAY2; // 0x120010A0
    uint64_t CSR;      // 0x12001100
};

struct PAD_State {
    uint16_t buttons; // active-low (0 = pressed)
    uint8_t  lx, ly, rx, ry;
};

struct Watchpoint {
    uint32_t addr;
    int      size;     // 1, 2, or 4 bytes
    uint32_t last_val;
    bool     hit;
    uint32_t hit_val;
    bool     seeded;   // avoids false positive on add
};

struct FrameRecord {
    uint64_t frame;
    uint32_t pc;
    uint32_t cycle;
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint16_t buttons;
    uint8_t  lx, ly, rx, ry;
};

struct CovEntry {
    uint32_t    addr;
    std::string name;
    int         count;
};

struct PendingRename {
    uint32_t address;
    char     name[128];
};

struct CallFrame {
    uint32_t pc;    // physical address of the call site (jal instruction) for this frame
    uint32_t func;  // physical start of the containing function
    uint32_t ra;    // return address this frame will jump to when it returns
};

struct BtnTexEntry {
    GLuint   tex = 0;
    uint16_t mask;
};

// ---------------------------------------------------------------------------
// Recomp backend
// ---------------------------------------------------------------------------
enum CpuSource { CPU_PCSX2, CPU_RECOMP };
extern CpuSource          g_cpu_source;
extern RecompilerBackend  g_recomp_backend;

// ---------------------------------------------------------------------------
// PCSX2 connection
// ---------------------------------------------------------------------------
extern HANDLE    pcsx2_handle;
extern DWORD     pcsx2_pid;
extern DWORD     recomp_pid;
extern SOCKET    pine_sock;
extern int       pine_port;
extern uintptr_t ee_ram_base;
extern uintptr_t cpu_regs_base;
extern uintptr_t pad_ee_offset;

extern std::atomic<uintptr_t> g_scan_result;
extern std::atomic<bool>      g_scan_running;

// ---------------------------------------------------------------------------
// CPU / memory state
// ---------------------------------------------------------------------------
extern PCSX2_cpuRegs cpu_regs;
extern bool          regs_valid;
extern bool          g_pcsx2_paused;

extern uint64_t gpr_prev_lo[32];
extern uint32_t gpr_changed_tick[32];

extern uint8_t  ee_ram_window[2048];
extern uint32_t ee_ram_window_addr;
extern bool     g_disasm_follow_pc;
extern uint32_t g_disasm_pinned;
extern bool     g_disasm_scroll_req;
extern bool     g_sym_scroll_req;
extern uint32_t g_mem_view_addr;
extern char     g_disasm_goto_buf[12];

// ---------------------------------------------------------------------------
// Breakpoints / watchpoints
// ---------------------------------------------------------------------------
extern std::vector<uint32_t>  g_breakpoints;
extern bool     g_bp_enabled;
extern bool     g_bp_hit;
extern uint32_t g_bp_hit_addr;

extern std::vector<Watchpoint> g_watchpoints;
extern bool     g_wp_enabled;

// Client-side cache of Recomp-backend breakpoints, mirrored into the
// target's shared memory on every reconnect (fresh process = fresh, empty
// shared memory, so nothing survives Restart without this cache).
struct RecompBreakpoint {
    uint32_t addr;
    uint8_t  cond_reg;
    uint32_t cond_value;
};
extern std::vector<RecompBreakpoint> g_recomp_breakpoints;

// Register Watch — 0-31 = GPR[i].lo, 32 = PC, 33 = HI.lo, 34 = LO.lo
extern std::vector<int> g_watch_gprs;

// ---------------------------------------------------------------------------
// GS / PAD
// ---------------------------------------------------------------------------
extern GS_PrivRegs gs_regs;
extern bool        gs_pine_ok;
extern PAD_State   pad0;

// ---------------------------------------------------------------------------
// Recomp-sourced runtime log (mirrors RUNTIME_LOG() ring buffer over shm)
// ---------------------------------------------------------------------------
struct RecompLogEntry {
    uint64_t    seq;
    std::string text;
};
extern std::vector<RecompLogEntry> g_recomp_log_entries;

// ---------------------------------------------------------------------------
// Controller textures
// ---------------------------------------------------------------------------
extern GLuint g_ctrl_tex_base;
extern GLuint g_ctrl_tex_outline;
extern GLuint g_ctrl_tex_analog;
extern GLuint g_ctrl_tex_analog_btn;
extern int    g_ctrl_img_w;
extern int    g_ctrl_img_h;
extern bool   g_ctrl_analog_on;
extern BtnTexEntry g_btn_tex[16];

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
extern std::vector<std::string> session_log;
extern bool log_scroll_to_bottom;

extern std::string g_project_root;
extern std::string g_log_debug;
extern std::string g_log_errors;
extern std::string g_log_crash;
extern std::string g_log_crash_dmp;
extern std::string g_session_summary;

// ---------------------------------------------------------------------------
// Config paths
// ---------------------------------------------------------------------------
extern char cfg_map_path[512];
extern char cfg_map_dir[512];
extern char cfg_pcsx2_path[512];
extern char cfg_pcsx2_dir[512];
extern char cfg_runner_path[512];
extern char cfg_elf_path[512];
extern char cfg_iso_path[512];
extern char dump_path[512];

extern std::vector<std::string> goto_history;
extern std::vector<std::string> label_history;
extern std::vector<std::string> dump_path_history;
extern std::vector<std::string> map_path_history;
extern std::vector<std::string> pcsx2_path_history;
extern std::vector<std::string> runner_path_history;
extern std::vector<std::string> elf_path_history;
extern std::vector<std::string> iso_path_history;

// Last-used debugger backend, restored at startup instead of always
// defaulting to CPU_PCSX2 (see g_cpu_source above).
extern int cfg_default_backend; // 0 = CPU_PCSX2, 1 = CPU_RECOMP

// Panel open/closed state, restored at startup instead of always defaulting
// all panels to visible.
extern bool cfg_open_disasm;
extern bool cfg_open_eeregs;
extern bool cfg_open_iopregs;
extern bool cfg_open_breakpoints;
extern bool cfg_open_memview;
extern bool cfg_open_framelog;
extern bool cfg_open_runtimelog;
extern bool cfg_open_symbols;

// ---------------------------------------------------------------------------
// Input tab: host controller selection, restored at startup.
// ---------------------------------------------------------------------------
// 0 = Auto (first available, old behavior), 1 = XInput, 2 = SDL GameController.
extern int cfg_pad_source;
// XInput user index (0-3) to use when cfg_pad_source == 1.
extern int cfg_pad_xinput_slot;
// SDL joystick device index to use when cfg_pad_source == 2.
extern int cfg_pad_sdl_index;
// Which PS2 controller port this host controller feeds: 0 = controller 1, 1 = controller 2.
extern int cfg_pad_port;

// ---------------------------------------------------------------------------
// Input tab: per-PS2-button host-input bindings (PCSX2-style), one binding
// set per PS2 port. When a port has at least one non-None binding, it drives
// the pad override by resolving each button/stick-direction independently
// instead of reading one whole device via cfg_pad_source (see
// ReadPadFromBindings() / SyncFromRecomp() in main_gui.cpp).
// ---------------------------------------------------------------------------
enum class PadBindDeviceType : int { None = 0, Keyboard = 1, XInput = 2, SdlGameController = 3 };

// Identifies which PS2 input a binding drives. The first 16 values map
// directly onto the active-low PS2 button bitmask bit-for-bit (bit 0 =
// Select ... bit 15 = Square, see ReadPAD()'s layout comment in main_gui.cpp).
// The remaining 8 are pseudo-slots, one per half-axis direction, matching how
// PCSX2 exposes bindable stick rows.
enum class PadInput : int {
    Select = 0, L3, R3, Start, Up, Right, Down, Left,
    L2, R2, L1, R1, Triangle, Circle, Cross, Square,
    LeftStickXPos, LeftStickXNeg, LeftStickYPos, LeftStickYNeg,
    RightStickXPos, RightStickXNeg, RightStickYPos, RightStickYNeg,
    Count
};

struct PadBinding {
    PadBindDeviceType deviceType = PadBindDeviceType::None;
    int   deviceIndex   = 0;     // XInput user index, or SDL joystick device index; unused for Keyboard
    int   inputCode     = 0;     // raylib KeyboardKey, XINPUT_GAMEPAD_* bit / trigger id, or SDL button/axis id
    bool  isAxis        = false; // true if inputCode identifies a physical stick axis rather than a digital button
    bool  axisNegative  = false; // for axis bindings: which half of the physical axis this logical direction reads
    float sensitivity   = 1.0f;  // 0..1 push amount (digital->analog push, and axis scaling), PCSX2-style pressure
};

extern PadBinding cfg_pad_bindings[2][(int)PadInput::Count]; // [port][input]

// Capture-next-input state for the Bind button in tab_inputlogger.cpp.
// -1 = not currently capturing.
extern int g_pad_bind_capture_port;
extern int g_pad_bind_capture_input;

// ---------------------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------------------
extern std::map<uint32_t, std::string> symbol_table;
extern std::vector<PendingRename>      g_pending_renames;
extern std::vector<CovEntry>           g_cov_entries;
extern FILETIME g_map_last_write;
extern bool     g_map_watch_inited;

// ---------------------------------------------------------------------------
// Recomp function coverage (ground truth dumped by ps2EntryRunner at startup)
// ---------------------------------------------------------------------------
extern std::unordered_set<uint32_t> g_recomp_functions;
extern bool                          g_recomp_table_loaded;

// ---------------------------------------------------------------------------
// Frame logger
// ---------------------------------------------------------------------------
static constexpr size_t FL_MAX_FRAMES = 10000;
extern std::vector<FrameRecord> frame_log;
extern bool     fl_capturing;
extern bool     fl_log_gprs;
extern bool     fl_log_inputs;
extern uint64_t fl_frame_num;
extern int      fl_playback_idx;
extern bool     cfg_autostart_framelog;

// ---------------------------------------------------------------------------
// GPR snapshot (pause→RPM→resume)
// ---------------------------------------------------------------------------
extern uint64_t g_gpr_snapshot[32];
extern uint64_t g_gpr_snapshot_hi;
extern uint64_t g_gpr_snapshot_lo;
extern uint32_t g_gpr_snapshot_pc;
extern bool     g_gpr_snapshot_valid;

// ---------------------------------------------------------------------------
// Per-frame diagnostics / PC history
// ---------------------------------------------------------------------------
extern uint64_t sync_count;
extern bool     sync_last_ok;
extern DWORD    sync_last_err;
extern int g_draw_w, g_draw_h;
extern int g_win_w,  g_win_h;
extern std::deque<uint32_t> pc_history;

// ---------------------------------------------------------------------------
// Shared name tables
// ---------------------------------------------------------------------------
extern const char* GPR_NAMES[32];

// ---------------------------------------------------------------------------
// Functions defined in main_gui.cpp that tabs need
// ---------------------------------------------------------------------------
void    AddLog(const char* fmt, ...);
void    SaveSessionLog(const char* filename);
bool    TakeGPRSnapshot();
void    LoadMapFile(const char* path);
void    LoadRecompFunctionTable();
bool    IsFunctionInRecomp(uint32_t func_start);
const char* GetSymbolName(uint32_t addr);
void    SyncFromPCSX2();
bool    isPCSX2Running();
bool    IsBreakpointSet(uint32_t addr);
void    BuildCoverage();
void    SaveSymbolToMap(const char* path, uint32_t phys_addr, const char* name);
bool    IsGPRFlushed(const PCSX2_GPR& g);
bool    IsBranchOrJump(uint32_t code);
void    DisassembleR5900(uint32_t code, uint32_t pc, char* buf, size_t sz);
const char* GetSymbolAny(uint32_t phys);
uint32_t    FindFunctionStart(uint32_t phys_pc);
void    DirFromPath(const char* path, char* out_dir, size_t out_size);
void    SaveConfig();
void    ExportGhidraCSV();
bool    PinePause();
bool    PineResume();
bool    PineFrameAdvance();
void    RestartRecomp();
void    RestartPCSX2();

// Real hardware breakpoints/watchpoints via the PCSX2 DebugServer plugin
// (distinct from PinePause/PineResume, which use the older PINE protocol).
// Wrappers around the file-scope g_dbgserver client in main_gui.cpp so
// tab_breakpoints.cpp doesn't need PCSX2DebugServerClient visibility.
bool    PCSX2DebugServerConnected();
bool    PCSX2SetBreakpoint(uint32_t addr, const char* condition = "");
bool    PCSX2RemoveBreakpoint(uint32_t addr);
bool    PCSX2ClearAllBreakpoints();
bool    PCSX2SetWatchpoint(uint32_t addr, uint32_t size, const char* type = "write");
bool    PCSX2RemoveWatchpoint(uint32_t addr, uint32_t size);
bool    PCSX2DebugServerResume();
bool    PCSX2ReadIopRegisters(uint32_t& pc, uint32_t gpr[32], uint32_t& hi, uint32_t& lo);
bool    PCSX2ReadIopMemory(uint32_t addr, uint8_t* out, uint32_t len);

// ---------------------------------------------------------------------------
// Tab functions (defined in tab_*.cpp)
// ---------------------------------------------------------------------------
void ShowBreakpoints();
void ShowCallStack();
void ShowThreads();
void ShowCodeTrace();
void ShowEERegisters();
void ShowIopRegisters();
void ShowRegisters();
void ShowFileDialogs();
void ShowFrameLogger();
void ShowGraphicsLogger();
void ShowInputLogger();
void ShowLabelQueue();
void ShowMemoryViewer();
void ShowRegWatch();
void ShowRuntimeLog();
void ShowSettings();
void ShowSymbolSyncTool();
void ShowLabelsTab();
