#include <cstdio>
#include "imgui.h"
#include "debugger_state.h"
#include "function_db.h"

void ShowLabelQueue() {
    ImGui::SeparatorText("Pending Labels for Ghidra");
    ImGui::TextDisabled("Right-click any function in Call Stack or Symbols to add. Export -> run BatchRename.java in Ghidra.");
    ImGui::Spacing();

    if (ImGui::Button("Export Ghidra CSV"))
        ExportGhidraCSV();
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        g_pending_renames.clear();
        AddLog("[Labels] Queue cleared.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d pending)", (int)g_pending_renames.size());

    if (g_pending_renames.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Queue is empty.");
        return;
    }

    ImGui::Spacing();
    ImGui::Separator();

    float avail_h = ImGui::GetContentRegionAvail().y - 4;
    if (ImGui::BeginTable("##lq", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg   | ImGuiTableFlags_SizingFixedFit,
        ImVec2(0, avail_h))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address",  ImGuiTableColumnFlags_WidthFixed,   100.0f);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##del",    ImGuiTableColumnFlags_WidthFixed,    36.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)g_pending_renames.size(); i++) {
            auto& pr = g_pending_renames[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%08X", pr.address);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##nm", pr.name, sizeof(pr.name))) {
                if (pr.name[0])
                    FnDB_SetName(pr.address, pr.name);
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("X")) {
                g_pending_renames.erase(g_pending_renames.begin() + i);
                i--;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
