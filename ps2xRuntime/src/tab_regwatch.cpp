#include <SDL.h>
#include "imgui.h"
#include "debugger_state.h"

void ShowRegWatch() {
    static const char* s_all_names[] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra",
        "PC","hi","lo"
    };
    static int s_add_idx = 2;

    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##regsel", &s_add_idx, s_all_names, 35);
    ImGui::SameLine();
    if (ImGui::Button("Add##rw")) {
        bool dup = false;
        for (int idx : g_watch_gprs) if (idx == s_add_idx) { dup = true; break; }
        if (!dup) g_watch_gprs.push_back(s_add_idx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All##rw")) g_watch_gprs.clear();

    ImGui::Separator();

    if (g_watch_gprs.empty()) {
        ImGui::TextDisabled("No registers watched. Select a register above and click Add.");
        return;
    }

    if (!ImGui::BeginTable("##rwt", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("Dec", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("",   ImGuiTableColumnFlags_WidthFixed, 20);
    ImGui::TableHeadersRow();

    uint32_t now_tick = SDL_GetTicks();
    for (int i = 0; i < (int)g_watch_gprs.size(); i++) {
        int ri = g_watch_gprs[i];
        uint32_t val; const char* name;
        if      (ri < 32)  { val = (uint32_t)cpu_regs.GPR[ri].lo; name = GPR_NAMES[ri]; }
        else if (ri == 32) { val = cpu_regs.PC;                    name = "PC"; }
        else if (ri == 33) { val = (uint32_t)cpu_regs.HI.lo;       name = "hi"; }
        else               { val = (uint32_t)cpu_regs.LO.lo;       name = "lo"; }

        bool changed = regs_valid && (ri < 32) && (now_tick - gpr_changed_tick[ri] < 500);
        ImVec4 col = changed ? ImVec4(1.0f,0.9f,0.3f,1.0f) : ImVec4(1,1,1,1);

        ImGui::TableNextRow();
        ImGui::PushID(i);
        ImGui::TableSetColumnIndex(0);
        char rw_row[48];
        snprintf(rw_row, sizeof(rw_row), "%-4s  0x%08X  %u", name, val, val);
        ImGui::Selectable("##rwsel", false,
            ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0));
        if (ImGui::BeginPopupContextItem("rwctx")) {
            if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(rw_row);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(0.4f,0.8f,1.0f,1.0f), "%s", name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(col, "0x%08X", val);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(col, "%u", val);
        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton("X")) {
            g_watch_gprs.erase(g_watch_gprs.begin() + i);
            i--;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
