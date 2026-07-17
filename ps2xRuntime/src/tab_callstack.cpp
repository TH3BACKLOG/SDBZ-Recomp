#include <algorithm>
#include <climits>
#include <cstring>
#include "imgui.h"
#include "debugger_state.h"
#include "function_db.h"

static std::vector<CallFrame> g_call_stack;

// Read bytes from EE physical memory using whichever source is active.
// Returns number of bytes actually read.
static size_t ReadEEMem(uint32_t phys, void* dst, size_t len) {
    if (g_cpu_source == CPU_RECOMP) {
        uint32_t win_base = ee_ram_window_addr;
        uint32_t win_end  = win_base + sizeof(ee_ram_window);
        if (phys >= win_base && phys < win_end) {
            size_t avail = win_end - phys;
            size_t copy  = (len < avail) ? len : avail;
            memcpy(dst, ee_ram_window + (phys - win_base), copy);
            return copy;
        }
        // Outside the PC-centred window: fall back to an on-demand request
        // (recomp_backend.cpp), which unlocks full backtraces instead of
        // being limited to the current dispatch iteration's neighborhood.
        if (len <= kDbgMemReqMaxSize && g_recomp_backend.ReadMemory(phys, dst, (uint32_t)len))
            return len;
        return 0;
    }
    if (!pcsx2_handle || !ee_ram_base) return 0;
    SIZE_T br = 0;
    ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + phys), dst, len, &br);
    return (size_t)br;
}

static void RebuildCallStack() {
    g_call_stack.clear();
    bool recomp_ok = (g_cpu_source == CPU_RECOMP && g_recomp_backend.IsConnected());
    if (!recomp_ok && (!ee_ram_base || !pcsx2_handle)) return;

    uint32_t pc = cpu_regs.PC  & 0x1FFFFFFFu;
    uint32_t sp = (uint32_t)cpu_regs.GPR[29].lo & 0x1FFFFFFFu;
    uint32_t ra = (uint32_t)cpu_regs.GPR[31].lo;

    const int MAX_FRAMES = 24;
    for (int f = 0; f < MAX_FRAMES; f++) {
        if (pc >= 0x2000000u) break;

        uint32_t func_start = FindFunctionStart(pc);
        g_call_stack.push_back({ pc, func_start, ra });

        uint32_t scan_start, scan_bytes;
        if (func_start < pc) {
            scan_start = func_start;
            scan_bytes = pc - func_start;
            if (scan_bytes > 2044u) scan_bytes = 2044u;
        } else {
            scan_start = (pc >= 2048u) ? (pc - 2048u) & ~3u : 0u;
            scan_bytes = pc - scan_start;
        }
        if (scan_bytes < 4) break;

        uint8_t  prologue[2048] = {};
        size_t   br = ReadEEMem(scan_start, prologue, scan_bytes);
        if (br < 4) break;

        int frame_size = 0;
        int ra_offset  = INT_MIN;
        for (uint32_t i = 0; i + 4 <= (uint32_t)br; i += 4) {
            uint32_t word;
            memcpy(&word, prologue + i, 4);
            if      ((word & 0xFFFF8000u) == 0x27BD8000u ||   // addiu  $sp,$sp,-N
                     (word & 0xFFFF8000u) == 0x67BD8000u)      // daddiu $sp,$sp,-N (64-bit)
                frame_size = -(int16_t)(word & 0xFFFF);
            else if ((word & 0xFFFF0000u) == 0xAFBF0000u ||   // sw  $ra, off($sp)
                     (word & 0xFFFF0000u) == 0xFFBF0000u)      // sd  $ra, off($sp) (64-bit)
                ra_offset  =  (int16_t)(word & 0xFFFF);
        }

        if (frame_size <= 0 || ra_offset == INT_MIN) {
            if (ra == 0 || (ra & 3u)) break;
            pc = (ra - 8) & 0x1FFFFFFFu;
            ra = 0;
            continue;
        }

        uint32_t saved_ra_phys = (sp + (uint32_t)ra_offset) & 0x1FFFFFFFu;
        if (saved_ra_phys + 4 > 0x2000000u) break;
        uint32_t saved_ra = 0;
        if (ReadEEMem(saved_ra_phys, &saved_ra, 4) < 4) break;
        if (saved_ra == 0 || (saved_ra & 3u)) break;

        sp  = (sp + (uint32_t)frame_size) & 0x1FFFFFFFu;
        pc  = (saved_ra - 8) & 0x1FFFFFFFu;
        ra  = saved_ra;
    }
}

void ShowCallStack() {
    const bool connected = (ee_ram_base != 0 && pcsx2_handle != nullptr)
                        || (g_cpu_source == CPU_RECOMP && g_recomp_backend.IsConnected());

    static bool s_was_paused = false;
    bool just_paused = g_pcsx2_paused && !s_was_paused;
    s_was_paused = g_pcsx2_paused;

    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    if (ImGui::Button("Refresh")) {
        if (connected && !any_popup) RebuildCallStack();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d frame(s)", (int)g_call_stack.size());
    if (g_pcsx2_paused && connected) {
        ImGui::SameLine();
        ImGui::TextDisabled("(auto)");
        if (just_paused && !any_popup)
            RebuildCallStack();
    }

    if (!connected) { ImGui::TextDisabled("Not connected."); return; }

    ImGui::Separator();
    // Fixed height instead of (0,0): inside Markers' shared scroll region a
    // (0,0)-sized child greedily claims all remaining space, shoving every
    // section below it (Registers, Threads, Recomp) far down the page.
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float h = ImGui::GetStyle().FramePadding.y * 2 + row_h * 2 +
              row_h * (float)std::min((int)g_call_stack.size() + 1, 12);
    ImGui::BeginChild("##callstack", ImVec2(0, h), false, 0);

    ImGui::TextDisabled(" #   PC          Func        Symbol");
    ImGui::Separator();

    for (int i = 0; i < (int)g_call_stack.size(); i++) {
        const CallFrame& cf = g_call_stack[i];

        const char* sym = GetSymbolAny(cf.func);
        if (!sym && !symbol_table.empty()) {
            auto it = symbol_table.upper_bound(cf.func);
            if (it != symbol_table.begin()) {
                --it;
                if (cf.func - it->first < 0x200u)
                    sym = it->second.c_str();
            }
        }

        char sub_buf[20];
        if (!sym) { snprintf(sub_buf, sizeof(sub_buf), "sub_%08X", cf.func); sym = sub_buf; }

        bool has_symbol = (symbol_table.count(cf.func) > 0);
        bool is_queued  = false;
        for (const auto& pr : g_pending_renames)
            if (pr.address == cf.func) { is_queued = true; break; }

        if (has_symbol)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        else if (is_queued)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));

        char label[160];
        snprintf(label, sizeof(label), "[%2d]  %08X  %08X  %s",
                 i, cf.pc, cf.func, sym);

        bool sel = false;
        if (ImGui::Selectable(label, &sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                g_disasm_pinned     = cf.func;
                g_disasm_follow_pc  = false;
                g_disasm_scroll_req = true;
                snprintf(g_disasm_goto_buf, sizeof(g_disasm_goto_buf), "%08X", cf.func);
            }
        }

        if (has_symbol || is_queued)
            ImGui::PopStyleColor();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Double-click to navigate disasm  |  Right-click to label");

        {
            static uint32_t s_cs_addr = 0;
            static char     s_cs_buf[128] = {};
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::IsWindowAppearing()) {
                    s_cs_addr = cf.func;
                    auto it = symbol_table.find(cf.func);
                    const char* cur = (it != symbol_table.end()) ? it->second.c_str() : "";
                    strncpy(s_cs_buf, cur, sizeof(s_cs_buf) - 1);
                    s_cs_buf[sizeof(s_cs_buf) - 1] = '\0';
                }
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(label);
                ImGui::Separator();
                ImGui::Text("Label  0x%08X", s_cs_addr);
                ImGui::Separator();
                ImGui::SetNextItemWidth(260);
                bool enter = ImGui::InputText("##cs_lbl", s_cs_buf, sizeof(s_cs_buf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("OK") || enter) && s_cs_buf[0]) {
                    FnDB_SetName(s_cs_addr, s_cs_buf);
                    bool found = false;
                    for (auto& pr : g_pending_renames)
                        if (pr.address == s_cs_addr) {
                            strncpy(pr.name, s_cs_buf, sizeof(pr.name) - 1);
                            pr.name[sizeof(pr.name) - 1] = '\0';
                            found = true; break;
                        }
                    if (!found) {
                        PendingRename pr; pr.address = s_cs_addr;
                        strncpy(pr.name, s_cs_buf, sizeof(pr.name) - 1);
                        pr.name[sizeof(pr.name) - 1] = '\0';
                        g_pending_renames.push_back(pr);
                    }
                    AddLog("[Label] 0x%08X = %s  (queued for Ghidra)", s_cs_addr, s_cs_buf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }

    ImGui::EndChild();
}
