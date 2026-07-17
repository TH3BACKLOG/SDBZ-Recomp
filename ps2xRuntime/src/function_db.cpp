#include "function_db.h"
#include "debugger_state.h"
#include <cstring>

static std::unordered_map<uint32_t, FunctionRecord> s_db;

// ---------------------------------------------------------------------------

FunctionRecord& FnDB_GetOrCreate(uint32_t addr) {
    auto it = s_db.find(addr);
    if (it != s_db.end()) return it->second;
    FunctionRecord& r = s_db[addr];
    r.addr = addr;
    // Seed name from symbol_table if already known
    auto sym = symbol_table.find(addr);
    if (sym != symbol_table.end() && !sym->second.empty()) {
        r.name  = sym->second;
        r.flags |= FN_NAMED;
    }
    return r;
}

FunctionRecord* FnDB_Get(uint32_t addr) {
    auto it = s_db.find(addr);
    return (it != s_db.end()) ? &it->second : nullptr;
}

void FnDB_RecordCall(uint32_t caller_site, uint32_t callee_addr) {
    FunctionRecord& callee = FnDB_GetOrCreate(callee_addr);
    callee.callers.insert(caller_site);

    // Determine which function owns caller_site and add callee to its callees
    uint32_t caller_fn = FindFunctionStart(caller_site);
    if (caller_fn != 0) {
        FunctionRecord& caller = FnDB_GetOrCreate(caller_fn);
        caller.callees.insert(callee_addr);
        caller.flags &= ~FN_LEAF;
    }
}

void FnDB_RecordExec(uint32_t addr) {
    FnDB_GetOrCreate(addr).exec_count++;
}

void FnDB_SetName(uint32_t addr, const char* name) {
    FunctionRecord& r = FnDB_GetOrCreate(addr);
    r.name  = name;
    r.flags |= FN_NAMED;

    // Keep symbol_table in sync (both physical and virtual variants)
    symbol_table[addr]                = name;
    symbol_table[addr | 0x80000000u] = name;
}

void FnDB_RebuildFromSymbolTable() {
    for (auto& [addr, name] : symbol_table) {
        if (addr & 0x80000000u) continue; // skip virtual duplicates
        if (name.empty())       continue;
        FunctionRecord& r = FnDB_GetOrCreate(addr);
        r.name  = name;
        r.flags |= FN_NAMED;
    }
}

size_t FnDB_Size() {
    return s_db.size();
}

void FnDB_Clear() {
    s_db.clear();
}

const std::unordered_map<uint32_t, FunctionRecord>& FnDB_All() {
    return s_db;
}
