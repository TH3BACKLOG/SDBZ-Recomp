#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// FunctionRecord — one entry per known function address
// ---------------------------------------------------------------------------
enum FnFlags : uint32_t {
    FN_NAMED        = 1 << 0,  // has a user/map-assigned name
    FN_ANALYZED     = 1 << 1,  // prologue scan completed
    FN_LEAF         = 1 << 2,  // makes no outgoing calls
    FN_STARTUP_ONLY = 1 << 3,  // never seen after first 300 frames
};

struct FunctionRecord {
    uint32_t addr         = 0;
    std::string name;
    std::unordered_set<uint32_t> callers;   // JAL sites that called this fn
    std::unordered_set<uint32_t> callees;   // functions this fn calls
    uint32_t exec_count   = 0;
    int      stack_frame  = -1;             // bytes, -1 = unknown
    uint8_t  confidence   = 0;             // 0-100
    uint32_t flags        = 0;
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Returns existing record or creates an empty one. Never returns null.
FunctionRecord& FnDB_GetOrCreate(uint32_t addr);

// Returns pointer to existing record, or nullptr if absent.
FunctionRecord* FnDB_Get(uint32_t addr);

// Record a JAL: caller_site is the address of the jal instruction,
// callee is the function being called.
void FnDB_RecordCall(uint32_t caller_site, uint32_t callee_addr);

// Increment exec count for the function at addr.
void FnDB_RecordExec(uint32_t addr);

// Set / update the name; also updates the global symbol_table.
void FnDB_SetName(uint32_t addr, const char* name);

// Import all entries from symbol_table into the DB (run once after map load).
void FnDB_RebuildFromSymbolTable();

// Number of records currently in the DB.
size_t FnDB_Size();

// Wipe all records.
void FnDB_Clear();

// Read-only access to the full table (for UI iteration).
const std::unordered_map<uint32_t, FunctionRecord>& FnDB_All();
