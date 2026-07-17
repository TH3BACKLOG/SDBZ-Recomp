#include "debugger_state.h"
#include <SDL_opengl.h>

// ---------------------------------------------------------------------------
// Recomp backend
// ---------------------------------------------------------------------------
CpuSource         g_cpu_source    = CPU_PCSX2;
RecompilerBackend g_recomp_backend;

// ---------------------------------------------------------------------------
// PCSX2 connection
// ---------------------------------------------------------------------------
HANDLE    pcsx2_handle  = NULL;
DWORD     pcsx2_pid     = 0;
DWORD     recomp_pid    = 0;
SOCKET    pine_sock     = INVALID_SOCKET;
int       pine_port     = 28011;
uintptr_t ee_ram_base   = 0;
uintptr_t cpu_regs_base = 0;
uintptr_t pad_ee_offset = 0;

std::atomic<uintptr_t> g_scan_result{0};
std::atomic<bool>      g_scan_running{false};

// ---------------------------------------------------------------------------
// CPU / memory state
// ---------------------------------------------------------------------------
PCSX2_cpuRegs cpu_regs  = {};
bool          regs_valid = false;
bool          g_pcsx2_paused = false;

uint64_t gpr_prev_lo[32]     = {};
uint32_t gpr_changed_tick[32] = {};

uint8_t  ee_ram_window[2048] = {};
uint32_t ee_ram_window_addr  = 0;
bool     g_disasm_follow_pc  = false;
uint32_t g_disasm_pinned     = 0;
bool     g_disasm_scroll_req = false;
bool     g_sym_scroll_req    = false;
uint32_t g_mem_view_addr     = 0x00100000;
char     g_disasm_goto_buf[12] = {};

// ---------------------------------------------------------------------------
// Breakpoints / watchpoints
// ---------------------------------------------------------------------------
std::vector<uint32_t> g_breakpoints;
bool     g_bp_enabled  = true;
bool     g_bp_hit      = false;
uint32_t g_bp_hit_addr = 0;

std::vector<Watchpoint> g_watchpoints;
bool g_wp_enabled = true;

std::vector<RecompBreakpoint> g_recomp_breakpoints;

std::vector<int> g_watch_gprs;

// ---------------------------------------------------------------------------
// GS / PAD
// ---------------------------------------------------------------------------
GS_PrivRegs gs_regs    = {};
bool        gs_pine_ok = false;
PAD_State   pad0       = {};

// ---------------------------------------------------------------------------
// Recomp-sourced runtime log (mirrors RUNTIME_LOG() ring buffer over shm)
// ---------------------------------------------------------------------------
std::vector<RecompLogEntry> g_recomp_log_entries;

// ---------------------------------------------------------------------------
// Controller textures
// ---------------------------------------------------------------------------
GLuint g_ctrl_tex_base       = 0;
GLuint g_ctrl_tex_outline    = 0;
GLuint g_ctrl_tex_analog     = 0;
GLuint g_ctrl_tex_analog_btn = 0;
int    g_ctrl_img_w          = 0;
int    g_ctrl_img_h          = 0;
bool   g_ctrl_analog_on      = true;
BtnTexEntry g_btn_tex[16] = {
    { 0, 0x0100 }, // L2
    { 0, 0x0200 }, // R2
    { 0, 0x0400 }, // L1
    { 0, 0x0800 }, // R1
    { 0, 0x0010 }, // D-Pad Up
    { 0, 0x0040 }, // D-Pad Down
    { 0, 0x0080 }, // D-Pad Left
    { 0, 0x0020 }, // D-Pad Right
    { 0, 0x0001 }, // Select
    { 0, 0x0008 }, // Start
    { 0, 0x1000 }, // Triangle
    { 0, 0x2000 }, // Circle
    { 0, 0x4000 }, // X
    { 0, 0x8000 }, // Square
    { 0, 0x0002 }, // L3
    { 0, 0x0004 }, // R3
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
std::vector<std::string> session_log;
bool log_scroll_to_bottom = false;

std::string g_project_root;
std::string g_log_debug;
std::string g_log_errors;
std::string g_log_crash;
std::string g_log_crash_dmp;
std::string g_session_summary;

// ---------------------------------------------------------------------------
// Config paths
// ---------------------------------------------------------------------------
char cfg_map_path[512]   = "";
char cfg_map_dir[512]    = "";
char cfg_pcsx2_path[512]  = "";
char cfg_pcsx2_dir[512]   = "C:\\";
char cfg_runner_path[512] = "";
char cfg_elf_path[512]    = "";
char cfg_iso_path[512]    = "";
char dump_path[512]       = "";

std::vector<std::string> goto_history;
std::vector<std::string> label_history;
std::vector<std::string> dump_path_history;
std::vector<std::string> map_path_history;
std::vector<std::string> pcsx2_path_history;
std::vector<std::string> runner_path_history;
std::vector<std::string> elf_path_history;
std::vector<std::string> iso_path_history;

int cfg_default_backend = 0; // 0 = CPU_PCSX2, 1 = CPU_RECOMP

bool cfg_open_disasm      = true;
bool cfg_open_eeregs      = true;
bool cfg_open_iopregs     = true;
bool cfg_open_breakpoints = true;
bool cfg_open_memview     = true;
bool cfg_open_framelog    = true;
bool cfg_open_runtimelog  = true;
bool cfg_open_symbols     = true;

int cfg_pad_source      = 0;  // 0 = Auto, 1 = XInput, 2 = SDL GameController
int cfg_pad_xinput_slot = 0;
int cfg_pad_sdl_index   = 0;
int cfg_pad_port        = 0;  // 0 = PS2 controller 1, 1 = PS2 controller 2

PadBinding cfg_pad_bindings[2][(int)PadInput::Count] = {};
int g_pad_bind_capture_port  = -1;
int g_pad_bind_capture_input = -1;

// ---------------------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------------------
std::map<uint32_t, std::string> symbol_table;
std::vector<PendingRename>      g_pending_renames;
std::vector<CovEntry>           g_cov_entries;
FILETIME g_map_last_write   = {};
bool     g_map_watch_inited = false;

// ---------------------------------------------------------------------------
// Recomp function coverage (ground truth dumped by ps2EntryRunner at startup)
// ---------------------------------------------------------------------------
std::unordered_set<uint32_t> g_recomp_functions;
bool                          g_recomp_table_loaded = false;

// ---------------------------------------------------------------------------
// Frame logger
// ---------------------------------------------------------------------------
std::vector<FrameRecord> frame_log;
bool     fl_capturing   = false;
bool     fl_log_gprs    = true;
bool     fl_log_inputs  = true;
uint64_t fl_frame_num   = 0;
int      fl_playback_idx = 0;
bool     cfg_autostart_framelog = false;

// ---------------------------------------------------------------------------
// Shared name tables
// ---------------------------------------------------------------------------
const char* GPR_NAMES[32] = {
    "zero","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
};

// ---------------------------------------------------------------------------
// GPR snapshot (pause→RPM→resume)
// ---------------------------------------------------------------------------
uint64_t g_gpr_snapshot[32]   = {};
uint64_t g_gpr_snapshot_hi    = 0;
uint64_t g_gpr_snapshot_lo    = 0;
uint32_t g_gpr_snapshot_pc    = 0;
bool     g_gpr_snapshot_valid = false;

// ---------------------------------------------------------------------------
// Per-frame diagnostics / PC history
// ---------------------------------------------------------------------------
uint64_t sync_count   = 0;
bool     sync_last_ok = false;
DWORD    sync_last_err = 0;
int g_draw_w = 0, g_draw_h = 0;
int g_win_w  = 0, g_win_h  = 0;
std::deque<uint32_t> pc_history;
