#include "imgui.h"
#include "debugger_state.h"

void ShowRuntimeLog() {
    static ImGuiTextFilter s_filter;
    static bool s_autoScroll = true;

    if (g_cpu_source != CPU_RECOMP) {
        ImGui::TextDisabled("Runtime log mirrors RUNTIME_LOG() calls from the live recomp process.");
        ImGui::TextDisabled("Switch CPU source to RECOMP to see it.");
        return;
    }

    ImGui::Text("Entries: %zu", g_recomp_log_entries.size());
    s_filter.Draw("Filter", 260.0f);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &s_autoScroll);

    ImGui::Separator();
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("recomp_runtime_log", 2, tableFlags, ImVec2(0, 430))) {
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const RecompLogEntry& entry : g_recomp_log_entries) {
            if (s_filter.IsActive() && !s_filter.PassFilter(entry.text.c_str())) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(entry.seq));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.text.c_str());
        }

        if (s_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
}
