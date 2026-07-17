#ifdef _WIN32
// Include <windows.h> first, isolated from raylib/ThreadNaming headers.
#include <windows.h>
#include "recomp_debug_ipc.h"
#include "recomp_debug_writer.h"
#include <cstring>
#include <mutex>

namespace RecompDbg {

static HANDLE            s_hMapFile   = nullptr;
static RecompDebugState* s_shm        = nullptr;
static const uint8_t*    s_rdram      = nullptr;
static uint32_t          s_rdram_size = 0;

// Guards claim/eviction of thread_states[] slots only (BUG-021). Slot
// contents themselves are never touched under this lock -- each slot is
// written exclusively by the one host thread that owns it, protected from
// the GUI reader via the per-slot update_seq seqlock instead. This mutex is
// process-local (not in shared memory) and only taken on a thread's first
// Update() call or when all slots are full and one must be evicted -- rare,
// not on the per-dispatch hot path.
static std::mutex s_slotClaimMutex;

// Finds (or claims/evicts) the DbgThreadState slot for tid. Must be called
// with s_slotClaimMutex held only for the claim/evict path; returns the
// slot index, always valid if s_shm is non-null.
static uint32_t FindOrClaimSlot(int32_t tid) {
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (s_shm->thread_states[i].tid == tid) return i;
    }

    std::lock_guard<std::mutex> lock(s_slotClaimMutex);
    // Re-check under the lock: another thread may have claimed a slot for
    // this tid, or an unclaimed slot may no longer be free.
    uint32_t freeSlot = kDbgMaxTrackedThreads;
    uint32_t lruSlot  = 0;
    uint64_t lruCycle = UINT64_MAX;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (s_shm->thread_states[i].tid == tid) return i;
        if (s_shm->thread_states[i].tid == -1 && freeSlot == kDbgMaxTrackedThreads) freeSlot = i;
        if (s_shm->thread_states[i].last_update_cycle < lruCycle) {
            lruCycle = s_shm->thread_states[i].last_update_cycle;
            lruSlot  = i;
        }
    }
    uint32_t slot = (freeSlot != kDbgMaxTrackedThreads) ? freeSlot : lruSlot;
    s_shm->thread_states[slot].tid = tid;
    uint32_t claimed = 0;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i)
        if (s_shm->thread_states[i].tid != -1) ++claimed;
    s_shm->active_thread_count = claimed;
    return slot;
}

// Services one pending mem_req/mem_write handshake against the RDRAM pointer
// cached from the last Update() call. Shared by Update()'s per-dispatch check
// and CheckBreakpoint()'s pause spin, so reads/writes keep working while the
// target is halted (paused/stepped) instead of only while running.
static void ServiceMemRequests() {
    if (!s_shm || !s_rdram) return;
    if (s_shm->mem_req_seq != s_shm->mem_resp_seq) {
        uint32_t size = s_shm->mem_req_size;
        if (size > kDbgMemReqMaxSize) size = kDbgMemReqMaxSize;
        uint32_t addr = s_shm->mem_req_addr & 0x1FFFFFFFu;
        if (addr < s_rdram_size) {
            uint32_t avail = s_rdram_size - addr;
            uint32_t copy  = (avail < size) ? avail : size;
            memcpy(s_shm->mem_resp, s_rdram + addr, copy);
            if (copy < size) memset(s_shm->mem_resp + copy, 0, size - copy);
        } else {
            memset(s_shm->mem_resp, 0, size);
        }
        s_shm->mem_resp_seq = s_shm->mem_req_seq;
    }
    if (s_shm->mem_write_seq != s_shm->mem_write_done_seq) {
        uint32_t size = s_shm->mem_write_size;
        if (size > kDbgMemReqMaxSize) size = kDbgMemReqMaxSize;
        uint32_t addr = s_shm->mem_write_addr & 0x1FFFFFFFu;
        if (addr < s_rdram_size) {
            uint32_t avail = s_rdram_size - addr;
            uint32_t copy  = (avail < size) ? avail : size;
            memcpy(const_cast<uint8_t*>(s_rdram) + addr, s_shm->mem_write_data, copy);
        }
        s_shm->mem_write_done_seq = s_shm->mem_write_seq;
    }
}

bool Init() {
    s_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(RecompDebugState)),
        RECOMP_DEBUG_SHM_NAME);
    if (!s_hMapFile) return false;

    s_shm = static_cast<RecompDebugState*>(
        MapViewOfFile(s_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RecompDebugState)));
    if (!s_shm) { CloseHandle(s_hMapFile); s_hMapFile = nullptr; return false; }

    ZeroMemory(s_shm, sizeof(RecompDebugState));
    s_shm->version        = RECOMP_DEBUG_STATE_VERSION;
    s_shm->recomp_running = 1;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i)
        s_shm->thread_states[i].tid = -1;
    s_shm->bp_hit_tid        = -1;
    s_shm->step_requested_tid = -1;
    return true;
}

void UpdateExtended(const DbgGsSnapshot& gs, const DbgPadSnapshot pad[2],
                     const DbgLogEntry* logs, uint32_t logCount, uint64_t nextSeq) {
    if (!s_shm) return;
    s_shm->gs      = gs;
    s_shm->pad[0]  = pad[0];
    s_shm->pad[1]  = pad[1];

    logCount = (logCount > kDbgMaxLogEntries) ? kDbgMaxLogEntries : logCount;
    if (logCount > 0) {
        memcpy(s_shm->log_entries, logs, logCount * sizeof(DbgLogEntry));
    }
    s_shm->log_entry_count = logCount;
    s_shm->log_next_seq    = nextSeq;
}

void UpdateThreads(const DbgThreadInfo* threads, uint32_t count) {
    if (!s_shm) return;
    count = (count > kDbgMaxThreads) ? kDbgMaxThreads : count;
    if (count > 0) {
        memcpy(s_shm->threads, threads, count * sizeof(DbgThreadInfo));
    }
    s_shm->thread_count = count;
}

void Update(int32_t tid, uint32_t pc, const uint32_t* gpr, uint32_t hi, uint32_t lo, uint64_t cycles,
            const uint8_t* rdram, uint32_t rdram_size) {
    if (!s_shm) return;
    uint32_t phys = pc & 0x1FFFFFFFu;

    uint32_t slotIdx = FindOrClaimSlot(tid);
    DbgThreadState& slot = s_shm->thread_states[slotIdx];
    // Seqlock: odd = write in progress. Only this tid's owning thread ever
    // writes this slot, so no CAS/lock needed here -- just the two fences
    // implied by the plain stores bracketing the write (BUG-021).
    slot.update_seq++;
    slot.pc                 = phys;
    slot.hi                 = hi;
    slot.lo                 = lo;
    slot.cycle_count        = cycles;
    memcpy(slot.gpr, gpr, 32 * sizeof(uint32_t));
    slot.last_update_cycle  = cycles;
    slot.update_seq++;

    // Fill RAM window centred on PC (512 instructions = 2048 bytes)
    constexpr uint32_t kWin = sizeof(s_shm->ram_window);
    uint32_t base = (phys >= kWin / 2) ? (phys - kWin / 2) : 0;
    base &= ~3u; // align to 4 bytes
    if (rdram && rdram_size > 0 && base < rdram_size) {
        uint32_t avail = rdram_size - base;
        uint32_t copy  = (avail < kWin) ? avail : kWin;
        memcpy(s_shm->ram_window, rdram + base, copy);
        if (copy < kWin) memset(s_shm->ram_window + copy, 0, kWin - copy);
    } else {
        memset(s_shm->ram_window, 0, kWin);
        base = phys;
    }
    s_shm->ram_window_base = base;

    // Cache RDRAM pointer/size so CheckBreakpoint()'s pause spin can keep
    // servicing memory requests via ServiceMemRequests() while halted.
    s_rdram      = rdram;
    s_rdram_size = rdram_size;

    // Service on-demand memory read/write requests (mirrors bp_requested/bp_hit).
    ServiceMemRequests();
}

void UpdateIop(uint32_t pc, const uint32_t* gpr, uint32_t hi, uint32_t lo, bool running) {
    if (!s_shm) return;
    s_shm->iop_pc      = pc;
    s_shm->iop_hi      = hi;
    s_shm->iop_lo      = lo;
    s_shm->iop_running = running ? 1 : 0;
    s_shm->iop_valid   = 1;
    memcpy(s_shm->iop_gpr, gpr, 32 * sizeof(uint32_t));
}

// Applies a pending debugger-armed GPR write directly into the live gpr
// array of the halted thread. Only meaningful while s_shm->bp_hit is set --
// there's no other point where a live ctx pointer is reachable from here.
static void ServiceRegWrite(uint32_t* gpr) {
    if (!s_shm || !gpr) return;
    if (s_shm->reg_write_seq != s_shm->reg_write_done_seq) {
        uint32_t idx = s_shm->reg_write_idx;
        if (idx > 0 && idx < 32) gpr[idx] = s_shm->reg_write_value; // idx 0 ($zero) never writable
        s_shm->reg_write_done_seq = s_shm->reg_write_seq;
    }
}

bool CheckBreakpoint(int32_t tid, uint32_t phys_pc, uint32_t* gpr) {
    if (!s_shm) return false;

    // A step request only halts the specific tid that requested it -- other
    // threads' CheckBreakpoint() calls for non-matching tids fall through
    // and keep running unaffected (BUG-021: previously a single global flag
    // could be silently consumed by whichever thread happened to observe it
    // first, regardless of which thread the debugger meant to step).
    if (s_shm->step_requested_tid == tid) {
        s_shm->step_requested_tid = -1;
        s_shm->bp_hit_addr        = phys_pc;
        s_shm->bp_hit_tid         = tid;
        s_shm->bp_hit             = 1;
        while (s_shm->bp_hit) { ServiceMemRequests(); ServiceRegWrite(gpr); Sleep(1); }
        return true;
    }

    for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
        DbgBreakpoint& slot = s_shm->bp_slots[i];
        if (!slot.enabled || slot.addr != phys_pc) continue;
        if (slot.cond_reg != kDbgNoCondReg) {
            if (slot.cond_reg >= 32 || !gpr || gpr[slot.cond_reg] != slot.cond_value) continue;
        }
        s_shm->bp_hit_addr = phys_pc;
        s_shm->bp_hit_tid  = tid;
        s_shm->bp_hit       = 1;
        while (s_shm->bp_hit) { ServiceMemRequests(); ServiceRegWrite(gpr); Sleep(1); }
        return true;
    }
    return false;
}

bool GetPadOverride(int port, bool& enabled, uint16_t& buttons,
                     uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry) {
    if (!s_shm || port < 0 || port >= (int)RecompDebugState::kDbgPadOverridePorts) return false;
    const RecompDebugState::DbgPadOverride& ov = s_shm->pad_override[port];
    enabled = ov.enabled != 0;
    buttons = ov.buttons;
    lx = ov.lx; ly = ov.ly; rx = ov.rx; ry = ov.ry;
    return true;
}

void Shutdown() {
    if (s_shm) { s_shm->recomp_running = 0; UnmapViewOfFile(s_shm); s_shm = nullptr; }
    if (s_hMapFile) { CloseHandle(s_hMapFile); s_hMapFile = nullptr; }
}

} // namespace RecompDbg
#endif // _WIN32
