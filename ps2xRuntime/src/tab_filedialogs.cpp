#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include "debugger_state.h"

// ImGuiFileDialog renders as a regular ImGui window; without an explicit
// focus request it can end up behind the Settings window it was opened
// from. Only steal focus when this specific dialog key is the one actually
// open — calling SetNextWindowFocus() unconditionally leaks a pending
// focus-grab onto whatever window Begin()s next when no dialog is open,
// which stomps normal window/input focus (e.g. breaks paste into fields).
static void FocusIfOpen(const char* key) {
    if (ImGuiFileDialog::Instance()->IsOpened(key))
        ImGui::SetNextWindowFocus();
}

void ShowFileDialogs() {
    ImVec2 dlg_size(700, 450);
    FocusIfOpen("BrowseMapKey");
    if (ImGuiFileDialog::Instance()->Display("BrowseMapKey", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(cfg_map_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(cfg_map_path) - 1);
            strncpy(cfg_map_dir,  ImGuiFileDialog::Instance()->GetCurrentPath().c_str(),  sizeof(cfg_map_dir)  - 1);
            SaveConfig();
            LoadMapFile(cfg_map_path);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    FocusIfOpen("BrowsePCSX2Key");
    if (ImGuiFileDialog::Instance()->Display("BrowsePCSX2Key", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(cfg_pcsx2_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(cfg_pcsx2_path) - 1);
            strncpy(cfg_pcsx2_dir,  ImGuiFileDialog::Instance()->GetCurrentPath().c_str(),  sizeof(cfg_pcsx2_dir)  - 1);
            DirFromPath(cfg_pcsx2_path, cfg_pcsx2_dir, sizeof(cfg_pcsx2_dir));
            SaveConfig();
            AddLog("[Config] PCSX2 path set to: %s", cfg_pcsx2_path);
            std::string sp(cfg_pcsx2_path);
            pcsx2_path_history.erase(std::remove(pcsx2_path_history.begin(), pcsx2_path_history.end(), sp), pcsx2_path_history.end());
            pcsx2_path_history.insert(pcsx2_path_history.begin(), sp);
            if (pcsx2_path_history.size() > 20) pcsx2_path_history.resize(20);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    FocusIfOpen("BrowseDumpKey");
    if (ImGuiFileDialog::Instance()->Display("BrowseDumpKey", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(dump_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(dump_path) - 1);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    FocusIfOpen("BrowseRunnerKey");
    if (ImGuiFileDialog::Instance()->Display("BrowseRunnerKey", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(cfg_runner_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(cfg_runner_path) - 1);
            SaveConfig();
            AddLog("[Config] ps2EntryRunner path set to: %s", cfg_runner_path);
            std::string sp(cfg_runner_path);
            runner_path_history.erase(std::remove(runner_path_history.begin(), runner_path_history.end(), sp), runner_path_history.end());
            runner_path_history.insert(runner_path_history.begin(), sp);
            if (runner_path_history.size() > 20) runner_path_history.resize(20);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    FocusIfOpen("BrowseElfKey");
    if (ImGuiFileDialog::Instance()->Display("BrowseElfKey", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(cfg_elf_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(cfg_elf_path) - 1);
            SaveConfig();
            AddLog("[Config] ELF path set to: %s", cfg_elf_path);
            std::string sp(cfg_elf_path);
            elf_path_history.erase(std::remove(elf_path_history.begin(), elf_path_history.end(), sp), elf_path_history.end());
            elf_path_history.insert(elf_path_history.begin(), sp);
            if (elf_path_history.size() > 20) elf_path_history.resize(20);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    FocusIfOpen("BrowseIsoKey");
    if (ImGuiFileDialog::Instance()->Display("BrowseIsoKey", ImGuiWindowFlags_NoCollapse, dlg_size)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            strncpy(cfg_iso_path, ImGuiFileDialog::Instance()->GetFilePathName().c_str(), sizeof(cfg_iso_path) - 1);
            SaveConfig();
            AddLog("[Config] ISO path set to: %s", cfg_iso_path);
            std::string sp(cfg_iso_path);
            iso_path_history.erase(std::remove(iso_path_history.begin(), iso_path_history.end(), sp), iso_path_history.end());
            iso_path_history.insert(iso_path_history.begin(), sp);
            if (iso_path_history.size() > 20) iso_path_history.resize(20);
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
