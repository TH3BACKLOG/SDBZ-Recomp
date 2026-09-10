"""
MCP server exposing the live ps2EntryRunner.exe RecompDebugState shared-memory
channel (see PS2Recomp/ps2xRuntime/include/recomp_debug_ipc.h) directly as
tools, so Claude can inspect/control a running recomp boot without going
through the ps2xStudio GUI.

Mirrors the same protocol ps2xStudio's RecompBackend already implements
(PS2Recomp/ps2xStudio/src/subsystems/RecompBackend.cpp) — this is a second
reader of the same shared memory block, not a new IPC mechanism.

BUG-021 (2026-07-03): the shared-memory layout moved from a single shared
pc/gpr slot to a per-thread table (thread_states[kDbgMaxTrackedThreads]),
version 6 -> 7. This script's struct format was updated to match; see
DbgThreadState in recomp_debug_ipc.h for the authoritative layout.

Protocol facts re-verified against the C++ writer on 2026-09-09 by reading
recomp_debug_writer.cpp and recomp_debug_ipc.h directly. Older comments in
this file were stale; trust these:
  - There IS a general-purpose memory bus. mem_req/mem_resp and mem_write_*
    reach any RDRAM address, 4096 bytes per request, and are serviced both
    while running AND while halted (CheckBreakpoint()'s spin calls
    ServiceMemRequests() every 1ms). recomp_read_memory() is the legacy
    ram_window-only path — prefer recomp_read_memory_general().
  - Breakpoints: 8 slots, each optionally CONDITIONAL on
    gpr[cond_reg] == cond_value, honoured by CheckBreakpoint().
  - Stepping is per-tid via step_requested_tid (BUG-021).
  - There is still no true async pause — recomp_pause() arms a breakpoint at
    the current PC and waits for the recompiler to reach it again.

*** BREAKPOINT GRANULARITY — READ BEFORE BELIEVING A NEGATIVE ***
CheckBreakpoint() is called from EeScheduler.cpp:947 once per scheduler
DISPATCH ITERATION, with context.pc = the PC the thread is about to dispatch
from. It is NOT a per-instruction hook. An address interior to a recompiled
function body never appears as a dispatch PC, so a breakpoint armed there
never fires even though the code provably runs. Arm on function-map ENTRY
addresses / resume labels only. Same failure mode as the T1 tracer.

NOT WIRED — present in the struct but dead, so no tool exposes them:
  - pad_override[]: RecompDbg::GetPadOverride() has no caller. ps2_runtime.cpp
    deliberately dropped the consumer because setPadOverrideState() is
    single-port. Writing pad_override[] does nothing.
  - iop_pc / iop_gpr[]: RecompDbg::UpdateIop() has no caller, so they stay 0.
Both would need a C++ change (rebuild) before a tool would mean anything.
"""
import mmap
import struct
import time

from mcp.server.fastmcp import FastMCP

SHM_NAME = "RecompDebugState"  # mmap tagname; the "Local\\" prefix is implicit on Windows
NUM_GPR = 32
RAM_WINDOW_SIZE = 2048
NUM_BP_SLOTS = 8
NUM_TRACKED_THREADS = 8  # kDbgMaxTrackedThreads
NO_COND_REG = 0xFF  # kDbgNoCondReg: unconditional breakpoint
PAUSE_SLOT = 0  # kPauseSlot in RecompBackend.cpp — reserved bp_slots[] index for pause()

# Layout must match recomp_debug_ipc.h exactly (pack(1), version 7):
#   uint32 version
#   DbgThreadState thread_states[8]:
#     int32  tid
#     uint32 pc
#     uint32 gpr[32]
#     uint32 hi
#     uint32 lo
#     uint64 cycle_count
#     uint32 update_seq
#     uint64 last_update_cycle
#   uint32 active_thread_count
#   uint8  recomp_running
#   DbgBreakpoint bp_slots[8]   -- each: uint32 addr, uint8 enabled, uint8 cond_reg, uint8 _pad[2], uint32 cond_value
#   uint32 bp_hit_addr
#   int32  bp_hit_tid
#   uint8  bp_hit
#   int32  step_requested_tid
#   uint32 ram_window_base
#   uint8  ram_window[2048]
# (extended per-frame / IOP / mem-req / thread fields follow but are not
#  needed by any tool in this file, so the format string stops here.)
# DbgThreadState is declared *before* `#pragma pack(push, 1)` in
# recomp_debug_ipc.h, so it keeps natural alignment: the compiler inserts
# 4 padding bytes before the uint64 last_update_cycle (which would
# otherwise sit at a non-8-aligned offset), making the struct 168 bytes,
# not 164. "4x" reproduces that pad without consuming an unpack() value.
_THREAD_STATE_FMT = "i" + "I" + ("I" * NUM_GPR) + "I" + "I" + "Q" + "I" + "4x" + "Q"
_BP_SLOT_FMT = "IBB2xI"  # addr, enabled, cond_reg, pad(2, skipped), cond_value
HEADER_FMT = (
    "<I"
    + (_THREAD_STATE_FMT * NUM_TRACKED_THREADS)
    + "IB"
    + (_BP_SLOT_FMT * NUM_BP_SLOTS)
    + "IiBiI"
)
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SHM_SIZE = HEADER_SIZE + RAM_WINDOW_SIZE

THREAD_STATE_SIZE = struct.calcsize("<" + _THREAD_STATE_FMT)
_FIELDS_PER_THREAD_STATE = 2 + NUM_GPR + 5  # tid, pc, gpr[32], hi, lo, cycle_count, update_seq, last_update_cycle
THREAD_STATES_OFFSET = struct.calcsize("<I")
BP_SLOTS_OFFSET = (
    THREAD_STATES_OFFSET
    + THREAD_STATE_SIZE * NUM_TRACKED_THREADS
    + struct.calcsize("<IB")  # active_thread_count, recomp_running
)
BP_SLOT_SIZE = struct.calcsize("<" + _BP_SLOT_FMT)
_TAIL_OFFSET = BP_SLOTS_OFFSET + BP_SLOT_SIZE * NUM_BP_SLOTS  # bp_hit_addr
BP_HIT_TID_OFFSET = _TAIL_OFFSET + struct.calcsize("<I")
BP_HIT_OFFSET = BP_HIT_TID_OFFSET + struct.calcsize("<i")
STEP_REQUESTED_TID_OFFSET = BP_HIT_OFFSET + struct.calcsize("<B")

# --- mem_write_* handshake offsets (post-header fields, not covered by HEADER_FMT) ---
# Layout order in recomp_debug_ipc.h after ram_window[2048]:
#   DbgGsSnapshot gs (7x uint64 = 56B), DbgPadSnapshot pad[2] (2x (uint16 + 4x uint8) = 12B),
#   uint32 log_entry_count, uint64 log_next_seq, DbgLogEntry log_entries[64] (each: uint64 seq + char[160] = 168B),
#   uint32 iop_pc, uint32 iop_gpr[32], uint32 iop_hi, uint32 iop_lo, uint8 iop_running, uint8 iop_valid,
#   uint32 mem_req_addr, uint32 mem_req_size, uint32 mem_req_seq, uint32 mem_resp_seq, uint8 mem_resp[4096],
#   uint32 mem_write_addr, uint32 mem_write_size, uint32 mem_write_seq, uint32 mem_write_done_seq, uint8 mem_write_data[4096]
_GS_SIZE = 7 * 8
_PAD_SIZE = (2 + 4) * 2
_LOG_ENTRY_SIZE = 8 + 160
_LOG_BLOCK_SIZE = 4 + 8 + _LOG_ENTRY_SIZE * 64
_IOP_BLOCK_SIZE = 4 + 4 * 32 + 4 + 4 + 1 + 1
_MEM_REQ_MAX_SIZE = 4096
_MEM_REQ_BLOCK_SIZE = 4 + 4 + 4 + 4 + _MEM_REQ_MAX_SIZE

_POST_RAM_WINDOW_OFFSET = HEADER_SIZE + RAM_WINDOW_SIZE
_MEM_REQ_BLOCK_OFFSET = (
    _POST_RAM_WINDOW_OFFSET + _GS_SIZE + _PAD_SIZE + _LOG_BLOCK_SIZE + _IOP_BLOCK_SIZE
)
MEM_REQ_ADDR_OFFSET = _MEM_REQ_BLOCK_OFFSET
MEM_REQ_SIZE_OFFSET = MEM_REQ_ADDR_OFFSET + 4
MEM_REQ_SEQ_OFFSET = MEM_REQ_SIZE_OFFSET + 4
MEM_RESP_SEQ_OFFSET = MEM_REQ_SEQ_OFFSET + 4
MEM_RESP_DATA_OFFSET = MEM_RESP_SEQ_OFFSET + 4

_MEM_WRITE_BLOCK_OFFSET = (
    _POST_RAM_WINDOW_OFFSET + _GS_SIZE + _PAD_SIZE + _LOG_BLOCK_SIZE + _IOP_BLOCK_SIZE + _MEM_REQ_BLOCK_SIZE
)
MEM_WRITE_ADDR_OFFSET = _MEM_WRITE_BLOCK_OFFSET
MEM_WRITE_SIZE_OFFSET = MEM_WRITE_ADDR_OFFSET + 4
MEM_WRITE_SEQ_OFFSET = MEM_WRITE_SIZE_OFFSET + 4
MEM_WRITE_DONE_SEQ_OFFSET = MEM_WRITE_SEQ_OFFSET + 4
MEM_WRITE_DATA_OFFSET = MEM_WRITE_DONE_SEQ_OFFSET + 4

# --- reg_write_* handshake offsets (version 8, added for register write-back) ---
REG_WRITE_IDX_OFFSET = MEM_WRITE_DATA_OFFSET + _MEM_REQ_MAX_SIZE
REG_WRITE_VALUE_OFFSET = REG_WRITE_IDX_OFFSET + 4
REG_WRITE_SEQ_OFFSET = REG_WRITE_VALUE_OFFSET + 4
REG_WRITE_DONE_SEQ_OFFSET = REG_WRITE_SEQ_OFFSET + 4
# --- guest scheduler snapshot: threads[64] (version 9) -------------------
# Populated by RecompDbg::UpdateThreads(), called once per video frame from
# ps2_runtime.cpp:6394. DbgThreadInfo is declared *before* the pack(1) block
# but is 8x int32, so natural alignment adds no padding: 32 bytes.
THREAD_COUNT_OFFSET = REG_WRITE_DONE_SEQ_OFFSET + 4
THREADS_OFFSET = THREAD_COUNT_OFFSET + 4
_THREAD_INFO_FMT = "<iIIIiiii"  # tid, entry, currentPc, stack, status, waitType, waitId, priority
THREAD_INFO_SIZE = struct.calcsize(_THREAD_INFO_FMT)  # 32
NUM_SCHED_THREADS = 64  # kDbgMaxThreads

# pad_override[2] (declared inside pack(1), so 1+3+2+4 = 10 bytes each) plus
# uint8 _reserved[4] close the struct.
PAD_OVERRIDE_OFFSET = THREADS_OFFSET + THREAD_INFO_SIZE * NUM_SCHED_THREADS
FULL_SHM_SIZE = PAD_OVERRIDE_OFFSET + 10 * 2 + 4  # == sizeof(RecompDebugState)

# THS_* / TSW_* decode tables for DbgThreadInfo.status / .waitType
_THS = {0x01: "RUN", 0x02: "READY", 0x04: "WAIT", 0x08: "SUSPEND",
        0x0C: "WAITSUSPEND", 0x10: "DORMANT"}
_TSW = {0: "NONE", 1: "SLEEP", 2: "SEMA", 3: "EVENT"}

mcp = FastMCP("recomp")


def _open():
    try:
        return mmap.mmap(-1, FULL_SHM_SIZE, tagname=SHM_NAME, access=mmap.ACCESS_WRITE)
    except OSError as e:
        raise RuntimeError(
            f"Could not open shared memory '{SHM_NAME}' — is ps2EntryRunner.exe "
            f"running with the debug writer active? ({e})"
        )


def _read_thread_slot(shm: mmap.mmap, index: int):
    """Seqlock-protected read of one thread_states[] slot (mirrors
    RecompBackend.cpp's ReadThreadSlot). Retries if a write is caught mid-flight."""
    offset = THREAD_STATES_OFFSET + THREAD_STATE_SIZE * index
    for _ in range(8):
        raw = shm[offset:offset + THREAD_STATE_SIZE]
        fields = struct.unpack("<" + _THREAD_STATE_FMT, raw)
        tid = fields[0]
        pc = fields[1]
        gpr = fields[2:2 + NUM_GPR]
        idx = 2 + NUM_GPR
        hi, lo, cycle_count, seq1, last_update_cycle = fields[idx:idx + 5]
        if seq1 & 1:
            continue  # write in progress
        raw2 = shm[offset:offset + THREAD_STATE_SIZE]
        seq2 = struct.unpack("<" + _THREAD_STATE_FMT, raw2)[2 + NUM_GPR + 3]
        if seq1 != seq2:
            continue
        return {
            "tid": tid,
            "pc": pc,
            "gpr": list(gpr),
            "hi": hi,
            "lo": lo,
            "cycle_count": cycle_count,
            "last_update_cycle": last_update_cycle,
        }
    return None


def _read_header(shm: mmap.mmap):
    raw = shm[:HEADER_SIZE]
    fields = struct.unpack(HEADER_FMT, raw)
    version = fields[0]
    idx = 1 + NUM_TRACKED_THREADS * _FIELDS_PER_THREAD_STATE
    active_thread_count, recomp_running = fields[idx:idx + 2]
    idx += 2
    bp_slots = []
    for _ in range(NUM_BP_SLOTS):
        addr, enabled, cond_reg, cond_value = fields[idx:idx + 4]
        bp_slots.append({
            "addr": addr,
            "enabled": bool(enabled),
            "cond_reg": cond_reg,
            "cond_value": cond_value,
        })
        idx += 4
    bp_hit_addr, bp_hit_tid, bp_hit, step_requested_tid, ram_window_base = fields[idx:idx + 5]
    ram_window = shm[HEADER_SIZE:HEADER_SIZE + RAM_WINDOW_SIZE]
    return {
        "version": version,
        "active_thread_count": active_thread_count,
        "recomp_running": bool(recomp_running),
        "bp_slots": bp_slots,
        "bp_hit_addr": bp_hit_addr,
        "bp_hit_tid": bp_hit_tid,
        "bp_hit": bool(bp_hit),
        "step_requested_tid": step_requested_tid,
        "ram_window_base": ram_window_base,
        "ram_window": ram_window,
    }


def _list_tracked_tids(shm: mmap.mmap):
    tids = []
    for i in range(NUM_TRACKED_THREADS):
        offset = THREAD_STATES_OFFSET + THREAD_STATE_SIZE * i
        tid = struct.unpack("<i", shm[offset:offset + 4])[0]
        if tid != -1:
            tids.append((i, tid))
    return tids


def _select_thread_index(shm: mmap.mmap, header: dict, tid: int | None = None):
    """Mirrors RecompBackend.cpp's default-selection logic: explicit tid if
    given, else bp_hit_tid, else the first populated slot."""
    tracked = _list_tracked_tids(shm)
    if tid is not None:
        for idx, t in tracked:
            if t == tid:
                return idx
        return None
    if header["bp_hit_tid"] >= 0:
        for idx, t in tracked:
            if t == header["bp_hit_tid"]:
                return idx
    if tracked:
        return tracked[0][0]
    return None


def _write_bp_slot(shm: mmap.mmap, slot: int, addr: int, enabled: int,
                   cond_reg: int = NO_COND_REG, cond_value: int = 0):
    """Write one DbgBreakpoint slot. cond_reg == NO_COND_REG (0xFF) is
    unconditional; otherwise CheckBreakpoint() only fires the slot when
    gpr[cond_reg] == cond_value."""
    offset = BP_SLOTS_OFFSET + BP_SLOT_SIZE * slot
    chunk = struct.pack("<IBBxxI", addr & 0x1FFFFFFF, enabled,
                        cond_reg & 0xFF, cond_value & 0xFFFFFFFF)
    shm[offset:offset + len(chunk)] = chunk


def _bad_version(h: dict):
    """Returns an error dict if the layout version is not the one this file
    was written against, else None. Every tool that trusts an offset past the
    header must call this first."""
    if h["version"] != 10:
        return {"error": f"unexpected RecompDebugState version {h['version']} "
                         f"(expected 10) — struct layout mismatch, trust no field"}
    return None


def _read_thread_table(shm: mmap.mmap):
    count = struct.unpack("<I", shm[THREAD_COUNT_OFFSET:THREAD_COUNT_OFFSET + 4])[0]
    count = min(count, NUM_SCHED_THREADS)
    rows = []
    for i in range(count):
        off = THREADS_OFFSET + THREAD_INFO_SIZE * i
        tid, entry, pc, stack, status, wt, wid, prio = struct.unpack(
            _THREAD_INFO_FMT, shm[off:off + THREAD_INFO_SIZE])
        rows.append({
            "tid": tid,
            "entry": hex(entry),
            "currentPc": hex(pc),
            "stack": hex(stack),
            "status": _THS.get(status, hex(status)),
            "waitType": _TSW.get(wt, str(wt)),
            "waitId": wid,
            "priority": prio,
        })
    return count, rows


def _clear_bp_hit(shm: mmap.mmap):
    shm[BP_HIT_OFFSET:BP_HIT_OFFSET + 1] = b"\x00"
    shm[BP_HIT_TID_OFFSET:BP_HIT_TID_OFFSET + 4] = struct.pack("<i", -1)


@mcp.tool()
def recomp_status(tid: int = -1) -> dict:
    """Read EE register state, PC, cycle count, and breakpoint state from the
    live ps2EntryRunner.exe process via shared memory. With no `tid` (or -1),
    selects whichever thread most recently hit a breakpoint, falling back to
    the first tracked thread. Pass an explicit tid to read a specific thread's
    slot (see recomp_list_threads)."""
    shm = _open()
    try:
        h = _read_header(shm)
        if h["version"] != 10:
            return {"error": f"unexpected RecompDebugState version {h['version']} (expected 10) — struct layout mismatch"}
        idx = _select_thread_index(shm, h, tid if tid != -1 else None)
        slot = _read_thread_slot(shm, idx) if idx is not None else None
        return {
            "version": h["version"],
            "recomp_running": h["recomp_running"],
            "active_thread_count": h["active_thread_count"],
            "selected_tid": slot["tid"] if slot else None,
            "pc": hex(slot["pc"]) if slot else None,
            "hi": hex(slot["hi"]) if slot else None,
            "lo": hex(slot["lo"]) if slot else None,
            "cycle_count": slot["cycle_count"] if slot else None,
            "bp_slot0": {
                "addr": hex(h["bp_slots"][0]["addr"]),
                "enabled": h["bp_slots"][0]["enabled"],
            },
            "bp_hit_addr": hex(h["bp_hit_addr"]),
            "bp_hit_tid": h["bp_hit_tid"],
            "bp_hit": h["bp_hit"],
            "ram_window_base": hex(h["ram_window_base"]),
            "gpr": {f"r{i}": hex(v) for i, v in enumerate(slot["gpr"])} if slot else None,
        }
    finally:
        shm.close()


@mcp.tool()
def recomp_list_threads() -> dict:
    """List all currently-tracked PS2 threads (tid, pc, cycle_count) from the
    per-thread debug slot table. Use a returned tid with recomp_status(tid=...)
    to inspect a specific thread."""
    shm = _open()
    try:
        h = _read_header(shm)
        if h["version"] != 10:
            return {"error": f"unexpected RecompDebugState version {h['version']} (expected 10) — struct layout mismatch"}
        threads = []
        for idx, tid in _list_tracked_tids(shm):
            slot = _read_thread_slot(shm, idx)
            if slot:
                threads.append({"tid": tid, "pc": hex(slot["pc"]), "cycle_count": slot["cycle_count"]})
        return {"active_thread_count": h["active_thread_count"], "threads": threads, "bp_hit_tid": h["bp_hit_tid"]}
    finally:
        shm.close()


@mcp.tool()
def recomp_read_memory(address: str, size: int) -> dict:
    """LEGACY / narrow. Read up to `size` bytes at physical EE address
    `address` (hex string, e.g. "0x4418D0"), but only if it falls inside the
    live ~2KB ram_window centred on the current PC.

    ⚠ Prefer recomp_read_memory_general() — it reaches ANY RDRAM address via
    the mem_req/mem_resp bus. This tool exists only because it needs no
    handshake round-trip, so it still works when the dispatch loop is not
    servicing requests. A failure here does NOT mean the address is
    unreachable."""
    addr = int(address, 16)
    shm = _open()
    try:
        h = _read_header(shm)
        base = h["ram_window_base"]
        offset = addr - base
        if offset < 0 or offset + size > RAM_WINDOW_SIZE:
            return {
                "ok": False,
                "error": f"address {hex(addr)} outside live ram_window "
                         f"[{hex(base)}, {hex(base + RAM_WINDOW_SIZE)})",
            }
        data = h["ram_window"][offset:offset + size]
        return {"ok": True, "address": hex(addr), "bytes": data.hex(), "size": size}
    finally:
        shm.close()


@mcp.tool()
def recomp_read_memory_general(address: str, size: int, timeout_seconds: float = 1.0) -> dict:
    """Read up to 4096 bytes at any physical EE address (hex string, e.g.
    "0x4418D0") via the mem_req/mem_resp on-demand handshake serviced by
    RecompDbg::ServiceMemRequests() in recomp_debug_writer.cpp — the same
    general-purpose mechanism recomp_write_memory already uses, just the read
    direction. Unlike recomp_read_memory (limited to the ~2KB ram_window
    around the current PC), this reaches arbitrary heap/data addresses, but
    only completes while the recomp dispatch loop is actively servicing
    requests (running, or paused-but-spinning in CheckBreakpoint)."""
    addr = int(address, 16)
    if size <= 0 or size > _MEM_REQ_MAX_SIZE:
        return {"ok": False, "error": f"size {size} out of range (1..{_MEM_REQ_MAX_SIZE})"}
    shm = _open()
    try:
        shm[MEM_REQ_ADDR_OFFSET:MEM_REQ_ADDR_OFFSET + 4] = struct.pack("<I", addr & 0x1FFFFFFF)
        shm[MEM_REQ_SIZE_OFFSET:MEM_REQ_SIZE_OFFSET + 4] = struct.pack("<I", size)
        prev_seq = struct.unpack("<I", shm[MEM_REQ_SEQ_OFFSET:MEM_REQ_SEQ_OFFSET + 4])[0]
        new_seq = (prev_seq + 1) & 0xFFFFFFFF
        shm[MEM_REQ_SEQ_OFFSET:MEM_REQ_SEQ_OFFSET + 4] = struct.pack("<I", new_seq)

        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            resp_seq = struct.unpack("<I", shm[MEM_RESP_SEQ_OFFSET:MEM_RESP_SEQ_OFFSET + 4])[0]
            if resp_seq == new_seq:
                data = shm[MEM_RESP_DATA_OFFSET:MEM_RESP_DATA_OFFSET + size]
                return {"ok": True, "address": hex(addr), "bytes": bytes(data).hex(), "size": size}
            time.sleep(0.01)
        return {"ok": False, "error": "timeout waiting for mem_resp_seq (recomp not servicing requests?)"}
    finally:
        shm.close()


@mcp.tool()
def recomp_write_memory(address: str, data_hex: str, timeout_seconds: float = 1.0) -> dict:
    """Write bytes (given as a hex string, e.g. "aabbcc") to physical EE
    address `address` (hex string). Uses the mem_write_* handshake already
    serviced by RecompDbg::ServiceMemRequests() in recomp_debug_writer.cpp —
    the same mechanism recomp_read_memory's mem_req side uses, just the write
    direction, which was previously unexposed as a tool. Only completes while
    the recomp dispatch loop is actively servicing requests (running or
    paused-but-spinning in CheckBreakpoint); size is capped at 4096 bytes."""
    addr = int(address, 16)
    data = bytes.fromhex(data_hex)
    size = len(data)
    if size == 0 or size > _MEM_REQ_MAX_SIZE:
        return {"ok": False, "error": f"size {size} out of range (1..{_MEM_REQ_MAX_SIZE})"}
    shm = _open()
    try:
        shm[MEM_WRITE_ADDR_OFFSET:MEM_WRITE_ADDR_OFFSET + 4] = struct.pack("<I", addr & 0x1FFFFFFF)
        shm[MEM_WRITE_SIZE_OFFSET:MEM_WRITE_SIZE_OFFSET + 4] = struct.pack("<I", size)
        shm[MEM_WRITE_DATA_OFFSET:MEM_WRITE_DATA_OFFSET + size] = data
        prev_seq = struct.unpack("<I", shm[MEM_WRITE_SEQ_OFFSET:MEM_WRITE_SEQ_OFFSET + 4])[0]
        new_seq = (prev_seq + 1) & 0xFFFFFFFF
        shm[MEM_WRITE_SEQ_OFFSET:MEM_WRITE_SEQ_OFFSET + 4] = struct.pack("<I", new_seq)

        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            done_seq = struct.unpack("<I", shm[MEM_WRITE_DONE_SEQ_OFFSET:MEM_WRITE_DONE_SEQ_OFFSET + 4])[0]
            if done_seq == new_seq:
                return {"ok": True, "address": hex(addr), "size": size}
            time.sleep(0.01)
        return {"ok": False, "error": "timeout waiting for mem_write_done_seq (recomp not servicing requests?)"}
    finally:
        shm.close()


@mcp.tool()
def recomp_set_breakpoint(address: str, slot: int = PAUSE_SLOT,
                          cond_reg: int = -1, cond_value: str = "0") -> dict:
    """Arm a breakpoint at physical EE address `address` (hex string).

    `slot` picks one of the 8 bp_slots (0..7). Slot 0 is shared with
    recomp_pause()/recomp_resume(), so use 1..7 for breakpoints that should
    survive a resume.

    `cond_reg` (0..31) makes it CONDITIONAL: CheckBreakpoint() halts only
    when gpr[cond_reg] == cond_value. -1 (default) = unconditional.
    `cond_value` is a hex string ("0x1234") or a decimal string.

    The condition is one 32-bit compare inside the scheduler's existing
    per-dispatch loop — NOT a per-instruction interpreter hook. So unlike
    PCSX2, a conditional breakpoint on a hot address does not throttle the
    runtime.

    ⚠ Only addresses that appear as a dispatch PC (function-map entries and
    resume labels) can ever fire — see the module docstring."""
    addr = int(address, 16)
    if slot < 0 or slot >= NUM_BP_SLOTS:
        return {"ok": False, "error": f"slot must be 0..{NUM_BP_SLOTS - 1}"}
    if cond_reg != -1 and not (0 <= cond_reg < 32):
        return {"ok": False, "error": "cond_reg must be 0..31, or -1 for unconditional"}
    cv = str(cond_value)
    val = int(cv, 16) if cv.lower().startswith("0x") else int(cv)
    creg = NO_COND_REG if cond_reg == -1 else cond_reg
    shm = _open()
    try:
        h = _read_header(shm)
        bad = _bad_version(h)
        if bad:
            return bad
        _write_bp_slot(shm, slot, addr, 1, creg, val)
        _clear_bp_hit(shm)
        return {
            "ok": True,
            "slot": slot,
            "armed_at": hex(addr & 0x1FFFFFFF),
            "condition": None if creg == NO_COND_REG else f"r{creg} == {hex(val)}",
        }
    finally:
        shm.close()


@mcp.tool()
def recomp_list_breakpoints() -> dict:
    """List all 8 bp_slots (address, enabled, condition) plus the current
    bp_hit / step_requested_tid state."""
    shm = _open()
    try:
        h = _read_header(shm)
        bad = _bad_version(h)
        if bad:
            return bad
        slots = [{
            "slot": i,
            "addr": hex(b["addr"]),
            "enabled": b["enabled"],
            "condition": None if b["cond_reg"] == NO_COND_REG
                         else f"r{b['cond_reg']} == {hex(b['cond_value'])}",
            "reserved_for_pause": i == PAUSE_SLOT,
        } for i, b in enumerate(h["bp_slots"])]
        return {
            "slots": slots,
            "bp_hit": h["bp_hit"],
            "bp_hit_addr": hex(h["bp_hit_addr"]),
            "bp_hit_tid": h["bp_hit_tid"],
            "step_requested_tid": h["step_requested_tid"],
        }
    finally:
        shm.close()


@mcp.tool()
def recomp_clear_breakpoint(slot: int) -> dict:
    """Disable one bp_slot (0..7) without touching bp_hit or the other slots."""
    if slot < 0 or slot >= NUM_BP_SLOTS:
        return {"ok": False, "error": f"slot must be 0..{NUM_BP_SLOTS - 1}"}
    shm = _open()
    try:
        _write_bp_slot(shm, slot, 0, 0)
        return {"ok": True, "slot": slot}
    finally:
        shm.close()


@mcp.tool()
def recomp_clear_all_breakpoints() -> dict:
    """Disable all 8 bp_slots and clear bp_hit + step_requested_tid, so a
    halted thread resumes and nothing re-arms behind it."""
    shm = _open()
    try:
        for i in range(NUM_BP_SLOTS):
            _write_bp_slot(shm, i, 0, 0)
        shm[STEP_REQUESTED_TID_OFFSET:STEP_REQUESTED_TID_OFFSET + 4] = struct.pack("<i", -1)
        _clear_bp_hit(shm)
        return {"ok": True, "cleared_slots": NUM_BP_SLOTS}
    finally:
        shm.close()


@mcp.tool()
def recomp_step(tid: int = -1, timeout_seconds: float = 5.0) -> dict:
    """Single-step one PS2 thread via step_requested_tid, then wait for it to
    halt. tid=-1 steps whichever thread last hit a breakpoint, falling back to
    the first tracked thread.

    Per BUG-021 this is per-tid — other threads keep running unaffected.
    Granularity is one SCHEDULER DISPATCH of that thread, not one MIPS
    instruction (CheckBreakpoint runs once per dispatch iteration).

    If the target is already halted, bp_hit is cleared first so it can
    advance. Returns with the thread halted again; release it with
    recomp_continue()."""
    shm = _open()
    try:
        h = _read_header(shm)
        bad = _bad_version(h)
        if bad:
            return bad
        target = tid
        if target == -1:
            idx = _select_thread_index(shm, h)
            if idx is None:
                return {"ok": False, "error": "no tracked threads"}
            st = _read_thread_slot(shm, idx)
            if not st:
                return {"ok": False, "error": "seqlock read failed"}
            target = st["tid"]

        shm[STEP_REQUESTED_TID_OFFSET:STEP_REQUESTED_TID_OFFSET + 4] = struct.pack("<i", target)
        # If the thread is parked in CheckBreakpoint()'s spin it must be
        # released before it can reach the step check on its next dispatch.
        if h["bp_hit"]:
            _clear_bp_hit(shm)

        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            h2 = _read_header(shm)
            if h2["bp_hit"] and h2["bp_hit_tid"] == target:
                idx = _select_thread_index(shm, h2, target)
                st = _read_thread_slot(shm, idx) if idx is not None else None
                return {
                    "ok": True,
                    "tid": target,
                    "pc": hex(st["pc"]) if st else hex(h2["bp_hit_addr"]),
                    "cycle_count": st["cycle_count"] if st else None,
                    "gpr": {f"r{i}": hex(v) for i, v in enumerate(st["gpr"])} if st else None,
                }
            if not h2["recomp_running"]:
                return {"ok": False, "error": "recomp_running went to 0 (process exited)"}
            time.sleep(0.01)
        return {"ok": False, "tid": target,
                "error": "timeout waiting for the step to land — is that tid still being "
                         "dispatched? A blocked/dormant thread never reaches CheckBreakpoint."}
    finally:
        shm.close()


@mcp.tool()
def recomp_continue() -> dict:
    """Release a halted thread by clearing bp_hit ONLY, leaving every armed
    bp_slot in place. Use this instead of recomp_resume() when the breakpoint
    should be able to fire again, and after recomp_step()."""
    shm = _open()
    try:
        h = _read_header(shm)
        was_hit = h["bp_hit"]
        _clear_bp_hit(shm)
        return {"ok": True, "was_halted": was_hit}
    finally:
        shm.close()


@mcp.tool()
def recomp_thread_table() -> dict:
    """Read the full guest scheduler snapshot: up to 64 PS2 threads with tid,
    entry point, current PC, stack, THS_* status, TSW_* wait type, waitId and
    priority. Written once per video frame by RecompDbg::UpdateThreads().

    This is the scheduler's own view — much broader than
    recomp_list_threads(), which only shows the 8 hot per-dispatch debug
    slots. Use it to see DORMANT/WAIT threads that never appear there."""
    shm = _open()
    try:
        h = _read_header(shm)
        bad = _bad_version(h)
        if bad:
            return bad
        count, rows = _read_thread_table(shm)
        return {"thread_count": count, "threads": rows}
    finally:
        shm.close()


@mcp.tool()
def recomp_wait_for_break(timeout_seconds: float = 30.0, poll_interval: float = 0.05) -> dict:
    """Poll bp_hit until the armed breakpoint fires or timeout elapses.
    Returns the full register snapshot of whichever thread fired it
    (identified by bp_hit_tid)."""
    deadline = time.monotonic() + timeout_seconds
    shm = _open()
    try:
        while time.monotonic() < deadline:
            h = _read_header(shm)
            if h["bp_hit"]:
                idx = _select_thread_index(shm, h, h["bp_hit_tid"] if h["bp_hit_tid"] >= 0 else None)
                slot = _read_thread_slot(shm, idx) if idx is not None else None
                return {
                    "hit": True,
                    "bp_hit_tid": h["bp_hit_tid"],
                    "pc": hex(slot["pc"]) if slot else hex(h["bp_hit_addr"]),
                    "cycle_count": slot["cycle_count"] if slot else None,
                    "gpr": {f"r{i}": hex(v) for i, v in enumerate(slot["gpr"])} if slot else None,
                }
            if not h["recomp_running"]:
                return {"hit": False, "error": "recomp_running went to 0 (process exited)"}
            time.sleep(poll_interval)
        return {"hit": False, "error": "timeout"}
    finally:
        shm.close()


@mcp.tool()
def recomp_resume() -> dict:
    """Clear the pause/breakpoint slot (bp_slots[0]) AND bp_hit, so the
    recompiler's Sleep(1) wait loop exits and execution continues.

    ⚠ This disarms slot 0. If you armed a breakpoint you want to fire again,
    use recomp_continue() instead — it clears only bp_hit."""
    shm = _open()
    try:
        _write_bp_slot(shm, PAUSE_SLOT, 0, 0)
        _clear_bp_hit(shm)
        return {"ok": True}
    finally:
        shm.close()


@mcp.tool()
def recomp_pause() -> dict:
    """Pseudo-pause: arms a breakpoint at the current PC of whichever thread
    most recently hit a breakpoint, or the first tracked thread. Since the
    recomp has no true async pause, this only halts once that thread's
    dispatch loop next reaches this exact PC again (e.g. inside a hot
    spin/poll loop it will halt almost immediately)."""
    shm = _open()
    try:
        h = _read_header(shm)
        idx = _select_thread_index(shm, h)
        if idx is None:
            return {"ok": False, "error": "no tracked threads"}
        slot = _read_thread_slot(shm, idx)
        if not slot:
            return {"ok": False, "error": "seqlock read failed"}
        _write_bp_slot(shm, PAUSE_SLOT, slot["pc"], 1)
        _clear_bp_hit(shm)
        return {"ok": True, "armed_at": hex(slot["pc"]), "tid": slot["tid"]}
    finally:
        shm.close()


@mcp.tool()
def recomp_write_register(index: int, value: str, timeout_seconds: float = 1.0) -> dict:
    """Write a GPR (index 1..31; 0/$zero is not writable) on the currently
    halted thread via the reg_write_* handshake serviced inside
    CheckBreakpoint()'s pause spin. Only takes effect while bp_hit is set
    (i.e. execution is actually halted at a breakpoint/step) — otherwise
    there is no live ctx pointer for the recompiler to write into."""
    val = int(value, 16) if value.lower().startswith("0x") else int(value)
    if index <= 0 or index >= 32:
        return {"ok": False, "error": "index must be 1..31 (0 is $zero, not writable)"}
    shm = _open()
    try:
        h = _read_header(shm)
        if h["version"] != 10:
            return {"error": f"unexpected RecompDebugState version {h['version']} (expected 10) — struct layout mismatch"}
        if not h["bp_hit"]:
            return {"ok": False, "error": "target is not currently halted (bp_hit == 0)"}
        prev_seq = struct.unpack("<I", shm[REG_WRITE_SEQ_OFFSET:REG_WRITE_SEQ_OFFSET + 4])[0]
        new_seq = (prev_seq + 1) & 0xFFFFFFFF
        shm[REG_WRITE_IDX_OFFSET:REG_WRITE_IDX_OFFSET + 4] = struct.pack("<I", index)
        shm[REG_WRITE_VALUE_OFFSET:REG_WRITE_VALUE_OFFSET + 4] = struct.pack("<I", val & 0xFFFFFFFF)
        shm[REG_WRITE_SEQ_OFFSET:REG_WRITE_SEQ_OFFSET + 4] = struct.pack("<I", new_seq)

        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            done_seq = struct.unpack("<I", shm[REG_WRITE_DONE_SEQ_OFFSET:REG_WRITE_DONE_SEQ_OFFSET + 4])[0]
            if done_seq == new_seq:
                return {"ok": True, "index": index, "value": hex(val)}
            time.sleep(0.01)
        return {"ok": False, "error": "timeout waiting for reg_write_done_seq (target not halted/servicing?)"}
    finally:
        shm.close()


if __name__ == "__main__":
    mcp.run()
