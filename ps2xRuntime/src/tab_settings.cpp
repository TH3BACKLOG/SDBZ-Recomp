#include <cstdio>
#include <fstream>
#include <string>
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include "debugger_state.h"

static void ShowSettingsPaths() {
    ImGui::TextDisabled("Changes take effect when you click Save. Paths are written to sdbz_debugger.ini next to the exe.");
    ImGui::Spacing();

    float browse_w = 80.0f;
    float field_w  = ImGui::GetWindowWidth() - browse_w - 205.0f;

    // --- symbols.map ---
    ImGui::Text("symbols.map");
    ImGui::PushItemWidth(field_w);
    ImGui::InputText("##map_path", cfg_map_path, sizeof(cfg_map_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##mphist2", ImGuiDir_Down))
        ImGui::OpenPopup("##mphist2_pop");
    if (ImGui::BeginPopup("##mphist2_pop")) {
        if (map_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : map_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(cfg_map_path, h.c_str(), sizeof(cfg_map_path) - 1);
                cfg_map_path[sizeof(cfg_map_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##map", ImVec2(browse_w, 0))) {
        IGFD::FileDialogConfig cfg;
        cfg.path = cfg_map_dir[0] ? std::string(cfg_map_dir) : ".";
        ImGuiFileDialog::Instance()->OpenDialog("BrowseMapKey", "Select symbols.map", ".map", cfg);
    }
    {
        std::ifstream probe(cfg_map_path);
        if (probe.is_open())
            ImGui::TextColored(ImVec4(0,1,0,1), "  File found");
        else
            ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  run ExportSymbolsMap.java in Ghidra first");
    }

    ImGui::Spacing();

    // --- PCSX2 exe ---
    ImGui::Text("PCSX2 executable (full path)");
    ImGui::PushItemWidth(field_w);
    ImGui::InputText("##pcsx2_path", cfg_pcsx2_path, sizeof(cfg_pcsx2_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##pcsx2hist", ImGuiDir_Down))
        ImGui::OpenPopup("##pcsx2hist_pop");
    if (ImGui::BeginPopup("##pcsx2hist_pop")) {
        if (pcsx2_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : pcsx2_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(cfg_pcsx2_path, h.c_str(), sizeof(cfg_pcsx2_path) - 1);
                cfg_pcsx2_path[sizeof(cfg_pcsx2_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##pcsx2", ImVec2(browse_w, 0))) {
        IGFD::FileDialogConfig cfg;
        cfg.path = cfg_pcsx2_dir[0] ? std::string(cfg_pcsx2_dir) : "C:\\";
        ImGuiFileDialog::Instance()->OpenDialog("BrowsePCSX2Key", "Select PCSX2 executable", ".exe", cfg);
    }
    ImGui::TextDisabled("  Leave blank to auto-detect pcsx2-qt.exe / pcsx2x64.exe / pcsx2.exe");
    {
        if (cfg_pcsx2_path[0]) {
            std::ifstream probe(cfg_pcsx2_path);
            if (probe.is_open())
                ImGui::TextColored(ImVec4(0,1,0,1), "  File found");
            else
                ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  check the path");
        }
    }

    ImGui::Spacing();

    // --- ps2EntryRunner exe (Recomp target restarted by the "Restart" button) ---
    ImGui::Text("ps2EntryRunner.exe (full path)");
    ImGui::PushItemWidth(field_w);
    ImGui::InputText("##runner_path", cfg_runner_path, sizeof(cfg_runner_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##runnerhist", ImGuiDir_Down))
        ImGui::OpenPopup("##runnerhist_pop");
    if (ImGui::BeginPopup("##runnerhist_pop")) {
        if (runner_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : runner_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(cfg_runner_path, h.c_str(), sizeof(cfg_runner_path) - 1);
                cfg_runner_path[sizeof(cfg_runner_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##runner", ImVec2(browse_w, 0))) {
        IGFD::FileDialogConfig cfg;
        cfg.path = "C:\\";
        ImGuiFileDialog::Instance()->OpenDialog("BrowseRunnerKey", "Select ps2EntryRunner.exe", ".exe", cfg);
    }
    if (cfg_runner_path[0]) {
        std::ifstream probe(cfg_runner_path);
        if (probe.is_open())
            ImGui::TextColored(ImVec4(0,1,0,1), "  File found");
        else
            ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  check the path");
    }

    ImGui::Spacing();

    // --- ELF Path (passed as the launch argument to ps2EntryRunner.exe) ---
    ImGui::Text("ELF Path");
    ImGui::PushItemWidth(field_w);
    ImGui::InputText("##elf_path", cfg_elf_path, sizeof(cfg_elf_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##elfhist", ImGuiDir_Down))
        ImGui::OpenPopup("##elfhist_pop");
    if (ImGui::BeginPopup("##elfhist_pop")) {
        if (elf_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : elf_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(cfg_elf_path, h.c_str(), sizeof(cfg_elf_path) - 1);
                cfg_elf_path[sizeof(cfg_elf_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##elf", ImVec2(browse_w, 0))) {
        IGFD::FileDialogConfig cfg;
        cfg.path = "C:\\";
        ImGuiFileDialog::Instance()->OpenDialog("BrowseElfKey", "Select PS2 ELF", ".*", cfg);
    }
    {
        std::ifstream probe(cfg_elf_path);
        if (probe.is_open())
            ImGui::TextColored(ImVec4(0,1,0,1), "  File found");
        else if (cfg_elf_path[0])
            ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  check the path");
    }

    ImGui::Spacing();

    // --- ISO Path (mounted as CDVD source when launching PCSX2, alongside -elf) ---
    ImGui::Text("ISO Path (PCSX2 disc source)");
    ImGui::PushItemWidth(field_w);
    ImGui::InputText("##iso_path", cfg_iso_path, sizeof(cfg_iso_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##isohist", ImGuiDir_Down))
        ImGui::OpenPopup("##isohist_pop");
    if (ImGui::BeginPopup("##isohist_pop")) {
        if (iso_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : iso_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(cfg_iso_path, h.c_str(), sizeof(cfg_iso_path) - 1);
                cfg_iso_path[sizeof(cfg_iso_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##iso", ImVec2(browse_w, 0))) {
        IGFD::FileDialogConfig cfg;
        cfg.path = "C:\\";
        ImGuiFileDialog::Instance()->OpenDialog("BrowseIsoKey", "Select PS2 ISO", ".iso,.*", cfg);
    }
    ImGui::TextDisabled("  Leave blank to launch PCSX2 with -elf only (no picture — CDVD has nothing to read)");
    {
        if (cfg_iso_path[0]) {
            std::ifstream probe(cfg_iso_path);
            if (probe.is_open())
                ImGui::TextColored(ImVec4(0,1,0,1), "  File found");
            else
                ImGui::TextColored(ImVec4(1,0.4f,0,1), "  File not found -  check the path");
        }
    }

    ImGui::Spacing();

    ImGui::Checkbox("Auto-start Frame Log capture on attach/launch", &cfg_autostart_framelog);
    ImGui::TextDisabled("  Backend is remembered automatically from the \"Backend: Recomp/PCSX2\" toggle at the top of the window.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save", ImVec2(120, 0))) {
        DirFromPath(cfg_map_path,   cfg_map_dir,   sizeof(cfg_map_dir));
        DirFromPath(cfg_pcsx2_path, cfg_pcsx2_dir, sizeof(cfg_pcsx2_dir));
        SaveConfig();
        LoadMapFile(cfg_map_path);
        if (pcsx2_handle) {
            CloseHandle(pcsx2_handle);
            pcsx2_handle  = NULL;
            ee_ram_base   = 0;
            cpu_regs_base = 0;
            regs_valid    = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Map", ImVec2(120, 0)))
        LoadMapFile(cfg_map_path);
}

static void ShowSettingsSessionLog() {
    if (ImGui::Button("Dump to File")) {
        SaveSessionLog(g_session_summary.c_str());
        AddLog("[Session] Log dumped to %s", g_session_summary.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string all;
        for (const auto& line : session_log) { all += line; all += "\n"; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("##session_log_view", ImVec2(0, 0), true);
    for (const auto& line : session_log)
        ImGui::TextUnformatted(line.c_str());
    if (log_scroll_to_bottom)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void ShowSettings() {
    ImGui::BeginChild("##settings_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (ImGui::BeginTabBar("SettingsSubTabs")) {
        if (ImGui::BeginTabItem("Paths"))       { ShowSettingsPaths();       ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Graphics"))    { ShowGraphicsLogger();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Inputs"))      { ShowInputLogger();         ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Session Log")) { ShowSettingsSessionLog();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}
