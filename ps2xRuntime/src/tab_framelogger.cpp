#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include "imgui.h"
#include "ImGuiFileDialog.h"
#include "debugger_state.h"
#include "function_db.h"

static void FrameLogger_DumpCSV(const char* path) {
    std::ofstream f(path);
    if (!f.is_open()) { AddLog("[FrameLog] Cannot write: %s", path); return; }

    f << "frame,pc,cycle,hi,lo";
    if (fl_log_gprs)
        for (int i = 0; i < 32; i++) f << ",gpr" << i;
    if (fl_log_inputs)
        f << ",buttons,lx,ly,rx,ry";
    f << "\n";

    for (const auto& r : frame_log) {
        f << r.frame << ",0x" << std::hex << r.pc << std::dec
          << "," << r.cycle
          << ",0x" << std::hex << r.hi
          << ",0x" << r.lo << std::dec;
        if (fl_log_gprs)
            for (int i = 0; i < 32; i++)
                f << ",0x" << std::hex << r.gpr[i] << std::dec;
        if (fl_log_inputs)
            f << "," << r.buttons
              << "," << (int)r.lx << "," << (int)r.ly
              << "," << (int)r.rx << "," << (int)r.ry;
        f << "\n";
    }
    AddLog("[FrameLog] Dumped %zu frames to %s", frame_log.size(), path);
}

void ShowFrameLogger() {
    if (dump_path[0] == '\0') {
        std::string dp = g_project_root + "\\Logs\\frame_log.csv";
        strncpy(dump_path, dp.c_str(), sizeof(dump_path)-1);
    }

    if (!regs_valid) {
        ImGui::TextDisabled("Waiting for PCSX2 CPU data...");
        return;
    }

    // ---- Capture controls ----
    ImGui::SeparatorText("Capture");
    ImGui::Checkbox("Log GPRs",   &fl_log_gprs);
    ImGui::SameLine();
    ImGui::Checkbox("Log Inputs", &fl_log_inputs);
    ImGui::SameLine(0, 30);

    if (!fl_capturing) {
        if (ImGui::Button("Start Capture", ImVec2(130, 0))) {
            frame_log.clear();
            fl_frame_num = 0;
            fl_playback_idx = 0;
            fl_capturing = true;
            AddLog("[FrameLog] Capture started.");
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1));
        if (ImGui::Button("Stop Capture", ImVec2(130, 0))) {
            fl_capturing = false;
            fl_playback_idx = (int)frame_log.size() > 0 ? (int)frame_log.size() - 1 : 0;
            AddLog("[FrameLog] Capture stopped. %zu frames recorded.", frame_log.size());
            if (!frame_log.empty())
                FrameLogger_DumpCSV(dump_path);
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(frame_log.empty());
    if (ImGui::Button("Clear", ImVec2(70, 0))) {
        frame_log.clear(); fl_frame_num = 0; fl_playback_idx = 0;
        AddLog("[FrameLog] Cleared.");
    }
    ImGui::EndDisabled();

    // Status
    ImGui::Spacing();
    if (fl_capturing)
        ImGui::TextColored(ImVec4(0,1,0,1), "RECORDING  %zu / %zu frames", frame_log.size(), FL_MAX_FRAMES);
    else
        ImGui::Text("Frames captured: %zu", frame_log.size());

    // ---- Export ----
    ImGui::Spacing();
    ImGui::SeparatorText("Export");

    ImGui::PushItemWidth(ImGui::GetWindowWidth() - 290);
    ImGui::InputText("##dump_path", dump_path, sizeof(dump_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##dphist", ImGuiDir_Down))
        ImGui::OpenPopup("##dphist_pop");
    if (ImGui::BeginPopup("##dphist_pop")) {
        if (dump_path_history.empty()) ImGui::TextDisabled("(no history)");
        for (auto& h : dump_path_history)
            if (ImGui::Selectable(h.c_str())) {
                strncpy(dump_path, h.c_str(), sizeof(dump_path) - 1);
                dump_path[sizeof(dump_path) - 1] = '\0';
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##dump") && !ImGuiFileDialog::Instance()->IsOpened()) {
        IGFD::FileDialogConfig cfg;
        cfg.path     = g_project_root + "\\Logs";
        cfg.fileName = "frame_log.csv";
        ImGuiFileDialog::Instance()->OpenDialog("BrowseDumpKey", "Export Frame Log CSV", ".csv", cfg);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(frame_log.empty());
    if (ImGui::Button("Dump CSV", ImVec2(80, 0))) {
        FrameLogger_DumpCSV(dump_path);
        std::string s(dump_path);
        dump_path_history.erase(std::remove(dump_path_history.begin(), dump_path_history.end(), s), dump_path_history.end());
        dump_path_history.insert(dump_path_history.begin(), s);
        if (dump_path_history.size() > 20) dump_path_history.resize(20);
    }
    ImGui::EndDisabled();

    // ---- Playback ----
    if (!frame_log.empty() && !fl_capturing) {
        ImGui::Spacing();
        ImGui::SeparatorText("Playback");

        int total = (int)frame_log.size();

        // Clamp index in case log was cleared/resized
        if (fl_playback_idx >= total) fl_playback_idx = total - 1;
        if (fl_playback_idx < 0)      fl_playback_idx = 0;

        // Step buttons
        ImGui::BeginDisabled(fl_playback_idx <= 0);
        if (ImGui::Button("|<##pb"))  fl_playback_idx = 0;
        ImGui::SameLine();
        if (ImGui::Button("<##pb"))   fl_playback_idx--;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(fl_playback_idx >= total - 1);
        if (ImGui::Button(">##pb"))   fl_playback_idx++;
        ImGui::SameLine();
        if (ImGui::Button(">|##pb"))  fl_playback_idx = total - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Frame %d / %d", fl_playback_idx, total - 1);

        // Scrub slider
        ImGui::PushItemWidth(-1);
        ImGui::SliderInt("##scrub", &fl_playback_idx, 0, total - 1, "");
        ImGui::PopItemWidth();

        // Selected frame detail
        const FrameRecord& r = frame_log[fl_playback_idx];
        const char* sym = GetSymbolName(r.pc);

        ImGui::Spacing();
        ImGui::BeginChild("PBDetail", ImVec2(0, 0), true);

        // Header row
        ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "Frame #%llu", (unsigned long long)r.frame);
        ImGui::SameLine(160);
        if (sym)
            ImGui::Text("PC  0x%08X  [%s]", r.pc, sym);
        else
            ImGui::Text("PC  0x%08X", r.pc);
        ImGui::Text("Cycle  %u       HI  0x%016llX    LO  0x%016llX",
            r.cycle, (unsigned long long)r.hi, (unsigned long long)r.lo);

        // GPR table
        bool has_gprs = false;
        for (int i = 1; i < 32; i++) if (r.gpr[i]) { has_gprs = true; break; }

        if (has_gprs) {
            ImGui::Spacing();
            ImGui::TextDisabled("Registers");
            ImGui::Separator();
            if (ImGui::BeginTable("PBGPRs", 4,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
            {
                for (int g = 0; g < 32; g++) {
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "%-4s", GPR_NAMES[g]);
                    ImGui::SameLine();
                    ImGui::Text("%016llX", (unsigned long long)r.gpr[g]);
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("(GPR logging was off for this capture)");
        }

        // Inputs
        if (fl_log_inputs) {
            ImGui::Spacing();
            ImGui::TextDisabled("Inputs");
            ImGui::Separator();
            static const struct { uint16_t bit; const char* name; } BTN_BITS[] = {
                {0x0001,"Select"},{0x0002,"L3"},{0x0004,"R3"},{0x0008,"Start"},{0x0010,"Up"},{0x0020,"Right"},
                {0x0040,"Down"},{0x0080,"Left"},{0x0100,"L2"},{0x0200,"R2"},
                {0x0400,"L1"},{0x0800,"R1"},{0x1000,"Tri"},{0x2000,"Circ"},
                {0x4000,"X"},{0x8000,"Sq"}
            };
            for (auto& b : BTN_BITS) {
                bool pressed = (r.buttons & b.bit) != 0;
                if (pressed) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f,1,0.2f,1), "[%s]", b.name);
                } else {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%s]", b.name);
                }
            }
            ImGui::Text("LStick (%3d, %3d)   RStick (%3d, %3d)",
                (int)r.lx, (int)r.ly, (int)r.rx, (int)r.ry);
        }

        ImGui::EndChild();
    } else if (!frame_log.empty() && fl_capturing) {
        // Live preview while recording — last 10 frames
        ImGui::Spacing();
        ImGui::SeparatorText("Recent Frames (last 10)");
        ImGui::BeginChild("FramePreview", ImVec2(0, 160), true);
        int start = (int)frame_log.size() - 10;
        if (start < 0) start = 0;
        for (int i = (int)frame_log.size() - 1; i >= start; i--) {
            const auto& r = frame_log[i];
            const char* sym = GetSymbolName(r.pc);
            if (sym)
                ImGui::Text("#%llu  PC=0x%08X [%s]  cycle=%u  btns=0x%04X",
                    r.frame, r.pc, sym, r.cycle, r.buttons);
            else
                ImGui::Text("#%llu  PC=0x%08X  cycle=%u  btns=0x%04X",
                    r.frame, r.pc, r.cycle, r.buttons);
        }
        ImGui::EndChild();
    }

    // ---- Function Coverage ----
    if (!frame_log.empty() && !fl_capturing) {
        ImGui::Spacing();
        ImGui::SeparatorText("Function Coverage");

        int labeled = 0;
        for (const auto& e : g_cov_entries) if (!e.name.empty()) labeled++;
        int total = (int)g_cov_entries.size();

        ImGui::Text("%d unique addresses  (%d labeled, %d unlabeled)", total, labeled, total - labeled);
        ImGui::SameLine(0, 20);
        if (ImGui::Button("Analyze") || (g_cov_entries.empty() && !frame_log.empty()))
            BuildCoverage();

        ImGui::SameLine(0, 10);
        static bool cov_hide_labeled = false;
        ImGui::Checkbox("Unlabeled only", &cov_hide_labeled);

        ImGui::Spacing();
        ImGui::BeginChild("##cov_list", ImVec2(0, 0), false);
        ImGui::TextDisabled("  Count   Address     Symbol");
        ImGui::Separator();

        static uint32_t s_cov_addr = 0;
        static char     s_cov_buf[128] = {};

        for (auto& e : g_cov_entries) {
            bool is_queued = false;
            const char* display = e.name.empty() ? nullptr : e.name.c_str();
            for (const auto& pr : g_pending_renames)
                if (pr.address == e.addr) { is_queued = true; display = pr.name; break; }

            bool labeled_now = (display != nullptr);
            if (cov_hide_labeled && labeled_now) continue;

            int cpushed = 0;
            if (!e.name.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                cpushed = 1;
            } else if (is_queued) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
                cpushed = 1;
            }

            char row[160];
            snprintf(row, sizeof(row), "[%5d]  %08X  %s##cov%08X",
                     e.count, e.addr,
                     display ? display : "???",
                     e.addr);

            bool sel = false;
            if (ImGui::Selectable(row, &sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    g_disasm_pinned     = e.addr;
                    g_disasm_follow_pc  = false;
                    g_disasm_scroll_req = true;
                    snprintf(g_disasm_goto_buf, sizeof(g_disasm_goto_buf), "%08X", e.addr);
                }
            }
            if (cpushed) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Double-click to navigate disasm  |  Right-click to label");

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::IsWindowAppearing()) {
                    s_cov_addr = e.addr;
                    auto it = symbol_table.find(e.addr);
                    const char* cur = (it != symbol_table.end()) ? it->second.c_str() : "";
                    strncpy(s_cov_buf, cur, sizeof(s_cov_buf) - 1);
                    s_cov_buf[sizeof(s_cov_buf) - 1] = '\0';
                }
                ImGui::Text("Label  0x%08X  (%d frames)", s_cov_addr, e.count);
                ImGui::Separator();
                ImGui::SetNextItemWidth(260);
                bool enter = ImGui::InputText("##cov_lbl", s_cov_buf, sizeof(s_cov_buf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("OK") || enter) && s_cov_buf[0]) {
                    FnDB_SetName(s_cov_addr, s_cov_buf);
                    bool found = false;
                    for (auto& pr : g_pending_renames)
                        if (pr.address == s_cov_addr) {
                            strncpy(pr.name, s_cov_buf, sizeof(pr.name) - 1);
                            pr.name[sizeof(pr.name) - 1] = '\0';
                            found = true; break;
                        }
                    if (!found) {
                        PendingRename pr; pr.address = s_cov_addr;
                        strncpy(pr.name, s_cov_buf, sizeof(pr.name) - 1);
                        pr.name[sizeof(pr.name) - 1] = '\0';
                        g_pending_renames.push_back(pr);
                    }
                    for (auto& ce : g_cov_entries)
                        if (ce.addr == s_cov_addr) { ce.name = s_cov_buf; break; }
                    AddLog("[Label] 0x%08X = %s  (queued for Ghidra)", s_cov_addr, s_cov_buf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
    }
}
