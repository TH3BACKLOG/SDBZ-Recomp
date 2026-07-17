#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Shared memory IPC between ps2EntryRunner.exe (writer) and the debugger GUI
// (reader).  Both sides include this header.
//
// The recompiler opens/creates the mapping on startup and writes after every
// translated basic block.  The debugger opens it read/write so it can arm
// breakpoints via bp_requested/bp_address.
// ---------------------------------------------------------------------------

#define RECOMP_DEBUG_SHM_NAME  "Local\\RecompDebugState"
#define RECOMP_DEBUG_SHM_SIZE  sizeof(RecompDebugState)
#define RECOMP_DEBUG_STATE_VERSION 10u

inline constexpr uint32_t kDbgMemReqMaxSize   = 4096;
inline constexpr uint32_t kDbgMaxBreakpoints  = 8;
inline constexpr uint8_t  kDbgNoCondReg       = 0xFFu; // bp_slots[i].cond_reg: unconditional breakpoint

// Per-thread pc/gpr snapshot slots (BUG-021 fix). Distinct from, and much
// smaller than, kDbgMaxThreads below: this table only needs to cover threads
// actively being inspected/stepped via the hot per-dispatch Update() path,
// not every possible PS2 thread. Each slot is claimed by tid on that host
// thread's first Update() call and is only ever written by the thread that
// owns it (one host std::thread per PS2 thread), so the only cross-thread
// race is a GUI reader observing a slot mid-write -- guarded by the
// update_seq seqlock (even = stable, odd = write in progress; reader must
// retry if seq is odd or changed across the read).
inline constexpr uint32_t kDbgMaxTrackedThreads = 8;

struct DbgThreadState {
    int32_t  tid;              // -1 = unclaimed slot
    uint32_t pc;
    uint32_t gpr[32];
    uint32_t hi;
    uint32_t lo;
    uint64_t cycle_count;
    uint32_t update_seq;       // seqlock
    uint64_t last_update_cycle; // for LRU eviction when all slots are claimed
};

struct DbgBreakpoint {
    uint32_t addr;
    uint8_t  enabled;
    uint8_t  cond_reg;   // GPR index 0-31 to compare, or kDbgNoCondReg for unconditional
    uint8_t  _pad[2];
    uint32_t cond_value; // fires only when gpr[cond_reg] == cond_value (ignored if cond_reg == kDbgNoCondReg)
};

// --- Extended (once-per-frame, not per-dispatch) snapshot types ---------
// These mirror data the in-game F1 debug overlay used to read directly off
// live runtime objects. They're populated by RecompDbg::UpdateExtended(),
// called once per video frame from ps2_runtime.cpp, NOT from the hot
// per-dispatch RecompDbg::Update() path.

struct DbgGsSnapshot {
    uint64_t pmode;
    uint64_t smode2;
    uint64_t dispfb1;
    uint64_t display1;
    uint64_t dispfb2;
    uint64_t display2;
    uint64_t csr;
};

struct DbgPadSnapshot {
    uint16_t buttons; // active-low (0 = pressed)
    uint8_t  lx, ly, rx, ry;
};

inline constexpr uint32_t kDbgMaxLogEntries = 64;
inline constexpr uint32_t kDbgLogTextSize   = 160;

struct DbgLogEntry {
    uint64_t seq;
    char     text[kDbgLogTextSize];
};

// PS2 (guest) thread scheduler snapshot — mirrors ps2_syscalls::ThreadDebugSnapshot.
// The recomp runtime spawns one real host std::thread per PS2 thread
// (g_hostThreads in Kernel/Syscalls/Helpers/State.h), so this is captured
// under g_thread_map_mutex once per video frame, not per-dispatch.
inline constexpr uint32_t kDbgMaxThreads = 64;

struct DbgThreadInfo {
    int32_t  tid;
    uint32_t entry;
    uint32_t currentPc;
    uint32_t stack;
    int32_t  status;         // THS_RUN / THS_READY / THS_WAIT / THS_SUSPEND / THS_WAITSUSPEND / THS_DORMANT
    int32_t  waitType;       // TSW_NONE / TSW_SLEEP / TSW_SEMA / TSW_EVENT
    int32_t  waitId;
    int32_t  currentPriority;
};

#pragma pack(push, 1)
struct RecompDebugState {
    uint32_t version; // RECOMP_DEBUG_STATE_VERSION; reader must verify before trusting layout

    // --- written by recompiler, read by debugger ---
    // Per-thread pc/gpr/hi/lo/cycle_count (BUG-021: replaces a single shared
    // slot that was torn/spliced across concurrent PS2 threads). See
    // DbgThreadState above for the seqlock/ownership contract.
    DbgThreadState thread_states[kDbgMaxTrackedThreads];
    uint32_t       active_thread_count; // number of currently-claimed slots
    uint8_t  recomp_running; // 1 while ps2EntryRunner is alive, 0 on exit

    // --- written by debugger, polled by recompiler ---
    DbgBreakpoint bp_slots[kDbgMaxBreakpoints]; // debugger fills addr/enabled/cond_*; recompiler only reads
    uint32_t bp_hit_addr;  // physical address of the slot that fired
    int32_t  bp_hit_tid;   // tid of the thread that hit the breakpoint (BUG-021)
    uint8_t  bp_hit;       // recompiler sets 1 when a slot fires; debugger clears to resume

    int32_t  step_requested_tid; // debugger sets to a tid to request that thread single-step; -1 = none. Recompiler resets to -1 after honoring it (BUG-021: was a global flag, ambiguous which thread it applied to)

    // --- RAM window around PC: written by recompiler, read by debugger ---
    uint32_t ram_window_base;   // physical address of ram_window[0]
    uint8_t  ram_window[2048];  // 512 instructions centred on PC

    // --- extended snapshot: written by recompiler once per video frame ---
    DbgGsSnapshot  gs;
    DbgPadSnapshot pad[2]; // index 0/1 = PS2 controller 1/2 (was a single shared-port snapshot)
    uint32_t       log_entry_count;          // valid entries in log_entries[0..count)
    uint64_t       log_next_seq;              // seq of the next entry that will be written
    DbgLogEntry    log_entries[kDbgMaxLogEntries];

    // --- IOP (R3000A) snapshot: written by iop_runtime once per step loop iteration ---
    uint32_t iop_pc;
    uint32_t iop_gpr[32];
    uint32_t iop_hi;
    uint32_t iop_lo;
    uint8_t  iop_running;     // mirrors R3000State::running
    uint8_t  iop_valid;       // 1 once IopRuntime has started and written at least one update

    // --- on-demand memory read/write at arbitrary address --------------------
    // Debugger sets mem_req_addr/mem_req_size then increments mem_req_seq to
    // arm a request. Recomp services it once per dispatch iteration (mirrors
    // the bp_requested/bp_hit handshake) into mem_resp[], then sets
    // mem_resp_seq = mem_req_seq so the debugger knows the response is fresh.
    uint32_t mem_req_addr;
    uint32_t mem_req_size;      // clamped to kDbgMemReqMaxSize by both sides
    uint32_t mem_req_seq;       // debugger increments to arm a new read
    uint32_t mem_resp_seq;      // recomp mirrors mem_req_seq once mem_resp is valid
    uint8_t  mem_resp[kDbgMemReqMaxSize];

    // Debugger sets mem_write_addr/mem_write_size/mem_write_data then
    // increments mem_write_seq to arm a write. Recomp performs it once per
    // dispatch iteration and mirrors mem_write_seq into mem_write_done_seq.
    uint32_t mem_write_addr;
    uint32_t mem_write_size;    // clamped to kDbgMemReqMaxSize by both sides
    uint32_t mem_write_seq;
    uint32_t mem_write_done_seq;
    uint8_t  mem_write_data[kDbgMemReqMaxSize];

    // Debugger sets reg_write_idx/reg_write_value then increments
    // reg_write_seq to arm a GPR write. Only serviced while a breakpoint/step
    // is actively halting the target thread (CheckBreakpoint's spin loop) --
    // there is no live ctx pointer to write back into otherwise. Recomp
    // mirrors reg_write_seq into reg_write_done_seq once applied.
    uint32_t reg_write_idx;    // 0..31
    uint32_t reg_write_value;
    uint32_t reg_write_seq;
    uint32_t reg_write_done_seq;

    // --- guest thread scheduler snapshot: written by recompiler once per video frame ---
    uint32_t       thread_count;              // valid entries in threads[0..count)
    DbgThreadInfo  threads[kDbgMaxThreads];

    // --- debugger -> runner pad injection (host controller mapped through the
    // Input tab, similar to how PCSX2 feeds its own host input into the guest
    // pad). Unlike reg_write_*, this is continuous streaming rather than a
    // one-shot handshake: the debugger overwrites pad_override[port] every
    // frame a controller is mapped to that port, and the runtime applies (or
    // clears) it unconditionally once per video frame in ps2_runtime.cpp,
    // regardless of run/halt state -- no seq/ack needed. port 0/1 map to PS2
    // controller 1/2 (DbgPadOverridePorts).
    struct DbgPadOverride {
        uint8_t  enabled;   // 0 = not mapped, runtime falls back to normal scePadRead() input
        uint8_t  _pad[3];
        uint16_t buttons;   // active-low PS2 bitmask (0 = pressed)
        uint8_t  lx, ly, rx, ry;
    };
    static constexpr uint32_t kDbgPadOverridePorts = 2;
    DbgPadOverride pad_override[kDbgPadOverridePorts];

    uint8_t        _reserved[4]; // pad struct to a 4-byte multiple (pack(1) defeats natural alignment)
};
#pragma pack(pop)

static_assert(sizeof(RecompDebugState) % 4 == 0, "RecompDebugState must be 4-byte aligned");
