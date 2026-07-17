#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include "debugger_state.h"
#include "function_db.h"

static const char* stristr(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, strlen(needle)) == 0) return hay;
    }
    return nullptr;
}

void ShowSymbolSyncTool() {
    // Path/browse/history for symbols.map lives in Settings (cfg_map_path is
    // the same shared global) — no need to duplicate it here, just act on it.
    ImGui::TextDisabled("symbols.map: %s", cfg_map_path[0] ? cfg_map_path : "(not set — see Settings)");
    if (ImGui::Button("Load##map")) {
        LoadMapFile(cfg_map_path);
        std::string s(cfg_map_path);
        map_path_history.erase(std::remove(map_path_history.begin(), map_path_history.end(), s), map_path_history.end());
        map_path_history.insert(map_path_history.begin(), s);
        if (map_path_history.size() > 20) map_path_history.resize(20);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##map")) { symbol_table.clear(); FnDB_Clear(); AddLog("[Symbols] Cleared."); }
    ImGui::SameLine();
    static bool s_auto_reload = false;
    if (ImGui::Checkbox("Auto-reload##sym", &s_auto_reload)) {
        g_map_watch_inited = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-reads symbols.map whenever Ghidra updates it on disk");

    if (s_auto_reload && cfg_map_path[0]) {
        static int s_reload_tick = 0;
        if (++s_reload_tick >= 60) {
            s_reload_tick = 0;
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            if (GetFileAttributesExA(cfg_map_path, GetFileExInfoStandard, &fa)) {
                if (!g_map_watch_inited) {
                    g_map_last_write   = fa.ftLastWriteTime;
                    g_map_watch_inited = true;
                } else if (CompareFileTime(&fa.ftLastWriteTime, &g_map_last_write) != 0) {
                    g_map_last_write = fa.ftLastWriteTime;
                    LoadMapFile(cfg_map_path);
                }
            }
        }
    }

    {
        bool exists = false;
        if (cfg_map_path[0]) {
            std::ifstream probe(cfg_map_path);
            exists = probe.is_open();
        }
        if (exists)
            ImGui::TextColored(ImVec4(0,1,0,1), "  File found on disk");
        else
            ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  run ExportSymbolsMap.java in Ghidra first");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Status");
    ImGui::Text("Loaded symbols: %zu", symbol_table.size());
    ImGui::TextDisabled("Map format: <hex_addr> <name>  (one per line, no 0x prefix)");

    if (!symbol_table.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Symbol Browser");
        static char filter[128] = {};
        static int  sym_nav_idx = -1;

        ImGui::PushItemWidth(250);
        if (ImGui::InputText("##sym_filter", filter, sizeof(filter)))
            { sym_nav_idx = 0; g_sym_scroll_req = true; }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::ArrowButton("##sym_up",   ImGuiDir_Up))
            { sym_nav_idx = (sym_nav_idx > 0) ? sym_nav_idx - 1 : 0; g_sym_scroll_req = true; }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##sym_down", ImGuiDir_Down))
            { sym_nav_idx++; g_sym_scroll_req = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("Filter");
        ImGui::SameLine();

        static size_t filtered_count = 0;
        static char   filter_cache[sizeof(filter)] = {};
        if (memcmp(filter, filter_cache, sizeof(filter)) != 0) {
            memcpy(filter_cache, filter, sizeof(filter));
            filtered_count = 0;
            for (auto& [a, nm] : symbol_table)
                if (!filter[0] || stristr(nm.c_str(), filter)) filtered_count++;
        }
        if (sym_nav_idx >= (int)filtered_count) sym_nav_idx = (int)filtered_count - 1;
        if (sym_nav_idx < 0 && filtered_count > 0) sym_nav_idx = 0;
        ImGui::TextDisabled("(%zu)", filtered_count);

        // Fixed height instead of (0,0) -- this panel shares a scroll region
        // with the Ghidra panel in the merged Labels tab (ShowLabelsTab()), so
        // a greedy (0,0) child would eat all remaining space and hide the
        // Ghidra panel below it (same issue documented for ShowThreads()).
        float row_h = ImGui::GetTextLineHeightWithSpacing();
        float list_h = row_h * 12.0f;
        ImGui::BeginChild("SymList", ImVec2(0, list_h), true);
        int fi = 0;
        for (auto& [addr, name] : symbol_table) {
            if (filter[0] && stristr(name.c_str(), filter) == nullptr) continue;
            bool is_current  = (regs_valid && (cpu_regs.PC & 0x1FFFFFFF) == (addr & 0x1FFFFFFF));
            bool is_selected = (fi == sym_nav_idx);
            if (is_selected && g_sym_scroll_req) { ImGui::SetScrollHereY(0.5f); g_sym_scroll_req = false; }
            ImVec4 col = is_selected  ? ImVec4(1.0f, 0.9f, 0.2f, 1.0f) :
                         is_current   ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) :
                                        ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char sym_line[256];
            snprintf(sym_line, sizeof(sym_line), "0x%08X  %s", addr, name.c_str());
            if (ImGui::Selectable(sym_line, is_selected, ImGuiSelectableFlags_AllowDoubleClick))
                { sym_nav_idx = fi; g_sym_scroll_req = false; }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                g_disasm_pinned     = addr & 0x1FFFFFFF;
                g_disasm_follow_pc  = false;
                g_disasm_scroll_req = true;
                snprintf(g_disasm_goto_buf, sizeof(g_disasm_goto_buf), "%08X", addr);
            }
            ImGui::PopStyleColor();

            {
                static uint32_t s_sym_addr = 0;
                static char     s_sym_buf[128] = {};
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::IsWindowAppearing()) {
                        s_sym_addr = addr & 0x1FFFFFFFu;
                        strncpy(s_sym_buf, name.c_str(), sizeof(s_sym_buf) - 1);
                        s_sym_buf[sizeof(s_sym_buf) - 1] = '\0';
                    }
                    ImGui::Text("Label  0x%08X", s_sym_addr);
                    ImGui::Separator();
                    ImGui::SetNextItemWidth(260);
                    bool enter = ImGui::InputText("##sym_lbl", s_sym_buf, sizeof(s_sym_buf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("OK") || enter) && s_sym_buf[0]) {
                        FnDB_SetName(s_sym_addr, s_sym_buf);
                        bool found = false;
                        for (auto& pr : g_pending_renames)
                            if (pr.address == s_sym_addr) {
                                strncpy(pr.name, s_sym_buf, sizeof(pr.name) - 1);
                                pr.name[sizeof(pr.name) - 1] = '\0';
                                found = true; break;
                            }
                        if (!found) {
                            PendingRename pr; pr.address = s_sym_addr;
                            strncpy(pr.name, s_sym_buf, sizeof(pr.name) - 1);
                            pr.name[sizeof(pr.name) - 1] = '\0';
                            g_pending_renames.push_back(pr);
                        }
                        AddLog("[Label] 0x%08X = %s  (queued for Ghidra)", s_sym_addr, s_sym_buf);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            fi++;
        }
        ImGui::EndChild();
    }
}

// Merged "Labels" tab (formerly two separate surfaces): the "Symbols" panel
// browses/searches symbols.map and originates label edits into
// g_pending_renames; the "Ghidra" panel (formerly the Settings "Labels"
// sub-tab, ShowLabelQueue()) reviews/exports that same queue. Same
// collapsing-header-on-one-scrollable-page pattern as the Markers panel
// (tab_breakpoints.cpp).
void ShowLabelsTab() {
    ImGui::BeginChild("##labels_scroll", ImVec2(0, 0), false);

    if (ImGui::CollapsingHeader("Symbols", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowSymbolSyncTool();
    }

    if (ImGui::CollapsingHeader("Ghidra", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowLabelQueue();
    }

    ImGui::EndChild();
}
