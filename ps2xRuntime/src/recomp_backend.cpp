#include "recomp_backend.h"
#include "../include/recomp_debug_ipc.h"
#include "debugger_state.h"
#include <cstring>

bool RecompilerBackend::Connect() {
    if (m_shm) return true;
    m_hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, RECOMP_DEBUG_SHM_NAME);
    if (!m_hMap) return false;
    m_shm = static_cast<RecompDebugState*>(
        MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, RECOMP_DEBUG_SHM_SIZE));
    if (!m_shm) { CloseHandle(m_hMap); m_hMap = nullptr; return false; }
    return true;
}

void RecompilerBackend::Disconnect() {
    if (m_shm) { UnmapViewOfFile(m_shm); m_shm = nullptr; }
    if (m_hMap) { CloseHandle(m_hMap); m_hMap = nullptr; }
}

int32_t RecompilerBackend::CurrentTid() const {
    if (!m_shm) return -1;
    if (m_shm->bp_hit) return m_shm->bp_hit_tid;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (m_shm->thread_states[i].tid != -1) return m_shm->thread_states[i].tid;
    }
    return -1;
}

bool RecompilerBackend::ReadRegs(CpuRegs& out) {
    if (!m_shm || !m_shm->recomp_running) return false;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return false; // stale debugger/runner build mismatch

    PollStepOver(); // called every frame so an in-flight StepOver() keeps chasing ra

    int32_t tid = CurrentTid();
    if (tid == -1) return false;
    uint32_t slotIdx = kDbgMaxTrackedThreads;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (m_shm->thread_states[i].tid == tid) { slotIdx = i; break; }
    }
    if (slotIdx == kDbgMaxTrackedThreads) return false;

    // Seqlock read: retry if a write was in progress (odd seq) or the slot
    // changed mid-copy (BUG-021 layout).
    const DbgThreadState& slot = m_shm->thread_states[slotIdx];
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t seqBefore = slot.update_seq;
        if (seqBefore & 1) continue;
        out.pc     = slot.pc;
        out.hi     = slot.hi;
        out.lo     = slot.lo;
        out.cycles = slot.cycle_count;
        memcpy(out.gpr, slot.gpr, sizeof(out.gpr));
        if (slot.update_seq == seqBefore) break;
    }

    // Populate disassembly window from shared memory RAM snapshot
    memcpy(ee_ram_window, m_shm->ram_window, sizeof(ee_ram_window));
    ee_ram_window_addr = m_shm->ram_window_base;
    return true;
}

void RecompilerBackend::SetBreakpoint(uint32_t addr) {
    if (!m_shm) return;
    m_shm->bp_slots[0].addr      = addr & 0x1FFFFFFFu;
    m_shm->bp_slots[0].enabled   = 1;
    m_shm->bp_slots[0].cond_reg  = kDbgNoCondReg;
    m_shm->bp_slots[0].cond_value = 0;
}

void RecompilerBackend::ClearBreakpoint() {
    if (!m_shm) return;
    m_shm->bp_slots[0].enabled = 0;
    m_shm->bp_slots[0].addr    = 0;
}

int RecompilerBackend::AddBreakpoint(uint32_t addr, uint8_t cond_reg, uint32_t cond_value) {
    if (!m_shm) return -1;
    addr &= 0x1FFFFFFFu;
    for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
        if (m_shm->bp_slots[i].enabled && m_shm->bp_slots[i].addr == addr) return (int)i; // already armed
    }
    for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
        if (!m_shm->bp_slots[i].enabled) {
            m_shm->bp_slots[i].addr       = addr;
            m_shm->bp_slots[i].enabled    = 1;
            m_shm->bp_slots[i].cond_reg   = cond_reg;
            m_shm->bp_slots[i].cond_value = cond_value;
            return (int)i;
        }
    }
    return -1;
}

void RecompilerBackend::RemoveBreakpoint(uint32_t addr) {
    if (!m_shm) return;
    addr &= 0x1FFFFFFFu;
    for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
        if (m_shm->bp_slots[i].enabled && m_shm->bp_slots[i].addr == addr) {
            m_shm->bp_slots[i].enabled = 0;
            m_shm->bp_slots[i].addr    = 0;
        }
    }
    // The target thread parks in CheckBreakpoint()'s `while (bp_hit) Sleep(1)`
    // spin, gated only on bp_hit — not on the slot that triggered it. Deleting
    // the currently-hit breakpoint would otherwise strand the target forever
    // with no slot left in the list to explain why it's still paused.
    if (m_shm->bp_hit && m_shm->bp_hit_addr == addr) {
        m_shm->bp_hit = 0;
    }
}

void RecompilerBackend::ClearAllBreakpoints() {
    if (!m_shm) return;
    for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
        m_shm->bp_slots[i].enabled = 0;
        m_shm->bp_slots[i].addr    = 0;
    }
    // Same rationale as RemoveBreakpoint(): none of the breakpoints that could
    // have caused the current pause still exist, so don't leave it stranded.
    m_shm->bp_hit = 0;
}

uint32_t RecompilerBackend::HitAddress() const {
    return m_shm ? m_shm->bp_hit_addr : 0;
}

void RecompilerBackend::Resume() {
    if (!m_shm) return;
    m_stepOverActive = false;
    m_shm->bp_hit = 0;
}

void RecompilerBackend::StepOver() {
    if (!m_shm) return;
    int32_t tid = CurrentTid();
    uint32_t ra = 0;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (m_shm->thread_states[i].tid == tid) { ra = m_shm->thread_states[i].gpr[31] & 0x1FFFFFFFu; break; }
    }
    if (!ra) { Step(); return; }

    m_stepOverActive   = true;
    m_stepOverTargetRa = ra;
    m_stepOverTid      = tid;
    if (tid != -1) m_shm->step_requested_tid = tid;
    m_shm->bp_hit = 0; // release; PollStepOver() re-arms on each intermediate halt until pc reaches ra
}

void RecompilerBackend::PollStepOver() {
    if (!m_stepOverActive || !m_shm) return;
    if (!m_shm->bp_hit) return; // still running toward the next step boundary

    // bp_hit is a single shared flag across all PS2 threads (each running on
    // its own host thread). If some other thread hit a real breakpoint (or
    // its own step) while we're chasing m_stepOverTid, that halt is genuine
    // and must be surfaced to the user -- not silently cleared as if it were
    // our own intermediate step. Only keep chasing if the halt belongs to the
    // thread we're actually stepping.
    if (m_shm->bp_hit_tid != m_stepOverTid) {
        m_stepOverActive = false;
        return;
    }

    uint32_t pc = 0;
    bool found = false;
    for (uint32_t i = 0; i < kDbgMaxTrackedThreads; ++i) {
        if (m_shm->thread_states[i].tid == m_stepOverTid) { pc = m_shm->thread_states[i].pc & 0x1FFFFFFFu; found = true; break; }
    }
    // Lost track of the thread (exited?) -- stop chasing and just present
    // whatever halted it as the result.
    if (!found || pc == m_stepOverTargetRa) {
        m_stepOverActive = false;
        return;
    }
    m_shm->step_requested_tid = m_stepOverTid;
    m_shm->bp_hit = 0;
}

void RecompilerBackend::Pause() {
    if (!m_shm) return;
    // step_requested_tid is per-tid (BUG-021): only CurrentTid()'s
    // CheckBreakpoint() call honors it, halting on its very next dispatch
    // iteration regardless of address.
    int32_t tid = CurrentTid();
    if (tid != -1) m_shm->step_requested_tid = tid;
}

const RecompDebugState* RecompilerBackend::ReadExtended() const {
    if (!m_shm || !m_shm->recomp_running) return nullptr;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return nullptr;
    return m_shm;
}

bool RecompilerBackend::ReadMemory(uint32_t addr, void* out, uint32_t size) {
    if (!m_shm || !m_shm->recomp_running) return false;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return false;
    if (size == 0 || size > kDbgMemReqMaxSize) return false;

    m_shm->mem_req_addr = addr & 0x1FFFFFFFu;
    m_shm->mem_req_size = size;
    uint32_t seq = ++m_shm->mem_req_seq;

    // Bounded poll: the request is serviced once per dispatch iteration, so
    // this only completes while the recomp is actively running (not paused).
    // Kept short (<=50ms) since this runs on the GUI thread once per frame.
    for (int i = 0; i < 50; ++i) {
        if (m_shm->mem_resp_seq == seq) {
            memcpy(out, m_shm->mem_resp, size);
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool RecompilerBackend::WriteMemory(uint32_t addr, const void* data, uint32_t size) {
    if (!m_shm || !m_shm->recomp_running) return false;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return false;
    if (size == 0 || size > kDbgMemReqMaxSize) return false;

    m_shm->mem_write_addr = addr & 0x1FFFFFFFu;
    m_shm->mem_write_size = size;
    memcpy(m_shm->mem_write_data, data, size);
    uint32_t seq = ++m_shm->mem_write_seq;

    for (int i = 0; i < 50; ++i) {
        if (m_shm->mem_write_done_seq == seq) return true;
        Sleep(1);
    }
    return false;
}

bool RecompilerBackend::WriteRegister(int index, uint32_t value) {
    if (!m_shm || !m_shm->recomp_running) return false;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return false;
    if (index <= 0 || index >= 32) return false;
    if (!m_shm->bp_hit) return false; // only serviced from within CheckBreakpoint's halt spin

    m_shm->reg_write_idx   = (uint32_t)index;
    m_shm->reg_write_value = value;
    uint32_t seq = ++m_shm->reg_write_seq;

    for (int i = 0; i < 50; ++i) {
        if (m_shm->reg_write_done_seq == seq) return true;
        Sleep(1);
    }
    return false;
}

bool RecompilerBackend::WritePadOverride(int port, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (!m_shm || !m_shm->recomp_running) return false;
    if (m_shm->version != RECOMP_DEBUG_STATE_VERSION) return false;
    if (port < 0 || port >= (int)RecompDebugState::kDbgPadOverridePorts) return false;

    RecompDebugState::DbgPadOverride& ov = m_shm->pad_override[port];
    ov.enabled = 1;
    ov.buttons = buttons;
    ov.lx = lx; ov.ly = ly; ov.rx = rx; ov.ry = ry;
    return true;
}

bool RecompilerBackend::ClearPadOverride(int port) {
    if (!m_shm) return false;
    if (port < 0 || port >= (int)RecompDebugState::kDbgPadOverridePorts) return false;
    m_shm->pad_override[port] = RecompDebugState::DbgPadOverride{};
    return true;
}

void RecompilerBackend::Step() {
    if (!m_shm) return;
    // Re-arm before releasing the current pause so the dispatch loop halts
    // again after exactly one more iteration instead of running free.
    int32_t tid = CurrentTid();
    if (tid != -1) m_shm->step_requested_tid = tid;
    m_shm->bp_hit = 0;
}
