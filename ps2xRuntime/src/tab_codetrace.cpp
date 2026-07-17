#include <SDL.h>
#include <algorithm>
#include <string>
#include "imgui.h"
#include "debugger_state.h"

void ShowCodeTrace() {
    // Only show the placeholder before the very first valid snapshot. After
    // that, keep rendering the last-good cpu_regs/ee_ram_window while a
    // reconnect/rescan is briefly in flight — blanking the whole view on every
    // transient regs_valid dip is what caused the "populate on pause" flicker.
    static bool s_had_first_snapshot = false;
    if (regs_valid) s_had_first_snapshot = true;
    if (!s_had_first_snapshot) {
        ImGui::TextDisabled("Waiting for PCSX2 CPU data...");
        return;
    }

    // Header bar: PC + symbol + cycle
    uint32_t phys_pc = cpu_regs.PC & 0x1FFFFFFF;
    const char* pc_sym = GetSymbolAny(phys_pc);
    if (pc_sym) ImGui::Text("PC: 0x%08X  [ %s ]", cpu_regs.PC, pc_sym);
    else        ImGui::Text("PC: 0x%08X", cpu_regs.PC);
    ImGui::SameLine(ImGui::GetWindowWidth() - 160);
    ImGui::Text("Cycle: %u", cpu_regs.cycle);

    // Func/label panel tracks the viewed address: pinned Go addr or live PC
    uint32_t view_addr = g_disasm_follow_pc ? phys_pc : (g_disasm_pinned & 0x1FFFFFFF);

    // Current function detector + quick-label
    {
        uint32_t func_start = FindFunctionStart(view_addr);
        const char* func_sym = GetSymbolAny(func_start);
        bool found = (func_start != phys_pc) || func_sym;

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
        if (func_sym)
            ImGui::Text("Func: 0x%08X  < %s >", func_start, func_sym);
        else if (found)
            ImGui::Text("Func: 0x%08X  (unlabeled)", func_start);
        else
            ImGui::TextDisabled("Func: (prologue not in window)");
        ImGui::PopStyleColor();

        // Pre-fill the label box when the detected function changes
        static char     label_buf[128]     = {};
        static uint32_t label_func_addr    = 0xFFFFFFFF;
        if (func_start != label_func_addr) {
            label_func_addr = func_start;
            const char* existing = GetSymbolAny(func_start);
            if (existing) strncpy(label_buf, existing, sizeof(label_buf) - 1);
            else           label_buf[0] = '\0';
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        bool enter = ImGui::InputText("##fnlabel", label_buf, sizeof(label_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##lhist", ImGuiDir_Down))
            ImGui::OpenPopup("##lhist_pop");
        if (ImGui::BeginPopup("##lhist_pop")) {
            if (label_history.empty()) ImGui::TextDisabled("(no history)");
            for (auto& h : label_history)
                if (ImGui::Selectable(h.c_str())) {
                    strncpy(label_buf, h.c_str(), sizeof(label_buf) - 1);
                    label_buf[sizeof(label_buf) - 1] = '\0';
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if ((ImGui::Button("Label##fn") || enter) && label_buf[0]) {
            symbol_table[func_start]                = label_buf;
            symbol_table[func_start | 0x80000000u] = label_buf;
            SaveSymbolToMap(cfg_map_path, func_start, label_buf);
            std::string s(label_buf);
            label_history.erase(std::remove(label_history.begin(), label_history.end(), s), label_history.end());
            label_history.insert(label_history.begin(), s);
            if (label_history.size() > 20) label_history.resize(20);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(name this function)");
    }
    ImGui::Separator();

    // Disassembly now fills the whole panel — the embedded register pane
    // that used to live alongside it moved into its own dockable
    // "EE Registers" tab (ShowEERegisters() below) since the GPR table /
    // HI-LO / PC history don't need to share space with the disasm view.
    ImGui::BeginChild("DisasmPane", ImVec2(0, 0), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SeparatorText("Disassembly");

        // "Go to address" toolbar
        char* goto_buf = g_disasm_goto_buf;
        bool&     follow_pc   = g_disasm_follow_pc;
        uint32_t& pinned_addr = g_disasm_pinned;

        // One-shot jump to address — clears Follow so the EE window re-centers on the typed address
        auto goto_nav = [&](uint32_t a) {
            if (!a) return;
            pinned_addr = a & 0x1FFFFFFF;
            follow_pc   = false;
            g_disasm_scroll_req = true;
            std::string s(goto_buf);
            goto_history.erase(std::remove(goto_history.begin(), goto_history.end(), s), goto_history.end());
            goto_history.insert(goto_history.begin(), s);
            if (goto_history.size() > 20) goto_history.resize(20);
        };

        ImGui::SetNextItemWidth(100);
        if (ImGui::InputText("##goto", goto_buf, sizeof(g_disasm_goto_buf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal))
            goto_nav((uint32_t)strtoul(goto_buf, nullptr, 16));
        ImGui::SameLine();
        if (ImGui::Button("Go"))
            goto_nav((uint32_t)strtoul(goto_buf, nullptr, 16));
        ImGui::SameLine();
        if (ImGui::Button("PC")) {
            snprintf(goto_buf, sizeof(g_disasm_goto_buf), "%08X", phys_pc);
            pinned_addr = phys_pc & 0x1FFFFFFF;
            g_disasm_scroll_req = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Follow", &follow_pc);
        ImGui::SameLine();
        if (ImGui::Button("Copy All")) {
            std::string dump;
            uint32_t win_start2 = ee_ram_window_addr;
            uint32_t num_insns2 = (uint32_t)(sizeof(ee_ram_window) / 4);
            char line[96];
            for (uint32_t i = 0; i < num_insns2; i++) {
                uint32_t addr2 = win_start2 + i * 4;
                uint32_t code2;
                memcpy(&code2, ee_ram_window + i * 4, 4);
                char disasm2[72];
                DisassembleR5900(code2, addr2, disasm2, sizeof(disasm2));
                const char* sym2 = GetSymbolAny(addr2);
                if (sym2) { snprintf(line, sizeof(line), "< %s >\n", sym2); dump += line; }
                snprintf(line, sizeof(line), "%08X  %08X  %s\n", addr2, code2, disasm2);
                dump += line;
            }
            ImGui::SetClipboardText(dump.c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the visible disassembly window to clipboard");
        ImGui::SameLine();
        ImGui::TextDisabled("(hex addr, Enter or Go)");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##ghist", ImGuiDir_Down))
            ImGui::OpenPopup("##ghist_pop");
        if (ImGui::BeginPopup("##ghist_pop")) {
            if (goto_history.empty()) ImGui::TextDisabled("(no history)");
            for (auto& h : goto_history)
                if (ImGui::Selectable(h.c_str())) {
                    strncpy(goto_buf, h.c_str(), sizeof(g_disasm_goto_buf) - 1);
                    goto_buf[sizeof(g_disasm_goto_buf) - 1] = '\0';
                    goto_nav((uint32_t)strtoul(goto_buf, nullptr, 16));
                }
            ImGui::EndPopup();
        }

        // Detect PC change — trigger scroll when Follow is on
        static uint32_t prev_pc = 0xFFFFFFFF;
        bool pc_changed = (phys_pc != prev_pc);
        if (pc_changed && follow_pc) {
            pinned_addr = phys_pc & 0x1FFFFFFF;
            g_disasm_scroll_req = true;
        }

        ImGui::Spacing();
        ImVec2 tbl_outer(0, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("DisasmTbl", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollY, tbl_outer))
        {
            ImGui::TableSetupColumn(" ",           ImGuiTableColumnFlags_WidthFixed,   14);
            ImGui::TableSetupColumn("Address",     ImGuiTableColumnFlags_WidthFixed,   75);
            ImGui::TableSetupColumn("Code",        ImGuiTableColumnFlags_WidthFixed,   75);
            ImGui::TableSetupColumn("Disassembly", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            uint32_t win_start = ee_ram_window_addr;
            uint32_t num_insns = (uint32_t)(sizeof(ee_ram_window) / 4);

            bool win_empty = true;
            for (int i = 0; i < (int)sizeof(ee_ram_window) && win_empty; i++)
                if (ee_ram_window[i]) win_empty = false;

            if (win_empty) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("EE RAM unavailable at this address.");
            }

            for (uint32_t i = 0; i < num_insns; i++) {
                uint32_t addr = win_start + i * 4;
                uint32_t code;
                memcpy(&code, ee_ram_window + i * 4, 4);

                uint32_t prev_code = 0;
                if (i > 0) memcpy(&prev_code, ee_ram_window + (i - 1) * 4, 4);
                bool is_delay = (i > 0) && IsBranchOrJump(prev_code);

                // Function label row — centered across all columns
                const char* row_sym = GetSymbolAny(addr);
                if (row_sym) {
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.18f, 0.16f, 0.04f, 1.0f)));
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "< %s >", row_sym);
                    float text_w = ImGui::CalcTextSize(lbl).x;
                    float win_w  = ImGui::GetWindowWidth();
                    ImVec2 sp    = ImGui::GetCursorScreenPos();
                    sp.x = ImGui::GetWindowPos().x + (win_w - text_w) * 0.5f;
                    ImGui::SetCursorScreenPos(sp);
                    ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns);
                    ImGui::PopStyleColor();
                }

                bool is_highlighted = (addr == (pinned_addr & ~3u));

                // Auto-scroll on navigation events only (one-shot, clears after first match)
                if (is_highlighted && g_disasm_scroll_req) {
                    ImGui::SetScrollHereY(0.4f);
                    g_disasm_scroll_req = false;
                }

                char disasm[72];
                DisassembleR5900(code, addr, disasm, sizeof(disasm));

                ImVec4 col = is_highlighted      ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) :
                             is_delay            ? ImVec4(0.4f, 0.85f, 1.0f, 1.0f) :
                                                   ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

                bool is_bp     = IsBreakpointSet(addr);
                bool is_bp_hit = g_bp_hit && (addr == g_bp_hit_addr);

                // Recomp coverage: whichever function this instruction belongs
                // to, is it in the ground-truth table ps2EntryRunner dumped?
                uint32_t cov_func_start = FindFunctionStart(addr);
                bool     in_recomp      = IsFunctionInRecomp(cov_func_start);

                ImGui::TableNextRow();
                if (is_bp_hit)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.35f, 0.07f, 0.04f, 1.0f)));
                else if (is_bp)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.22f, 0.04f, 0.04f, 1.0f)));
                else if (is_highlighted)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.08f, 0.28f, 0.10f, 1.0f)));
                else if (g_recomp_table_loaded && !in_recomp)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.28f, 0.05f, 0.05f, 1.0f)));
                else if (g_recomp_table_loaded)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.05f, 0.08f, 0.22f, 1.0f)));

                ImGui::PushID((int)i);

                // Col 0: marker — spanning selectable for row interaction
                ImGui::TableSetColumnIndex(0);
                ImGui::Selectable("##r", false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, 0));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    snprintf(goto_buf, sizeof(g_disasm_goto_buf), "%08X", addr);
                    goto_nav(addr);
                }
                // Right-click context menu
                if (ImGui::BeginPopupContextItem("dctx")) {
                    char tmp[20]; snprintf(tmp, sizeof(tmp), "0x%08X", addr);
                    if (ImGui::MenuItem("Copy Address"))  ImGui::SetClipboardText(tmp);
                    char full[96]; snprintf(full, sizeof(full), "%08X  %08X  %s", addr, code, disasm);
                    if (ImGui::MenuItem("Copy Line"))     ImGui::SetClipboardText(full);
                    if (ImGui::MenuItem("Go to Address")) {
                        snprintf(goto_buf, sizeof(g_disasm_goto_buf), "%08X", addr);
                        goto_nav((uint32_t)strtoul(goto_buf, nullptr, 16));
                    }
                    ImGui::Separator();
                    {
                        bool has_bp = IsBreakpointSet(addr);
                        if (ImGui::MenuItem(has_bp ? "Remove Breakpoint" : "Set Breakpoint")) {
                            uint32_t phys = addr & 0x1FFFFFFF;
                            auto it = std::find_if(g_breakpoints.begin(), g_breakpoints.end(),
                                [phys](uint32_t bp){ return (bp & 0x1FFFFFFF) == phys; });
                            bool arm_pcsx2 = (g_cpu_source == CPU_PCSX2 && PCSX2DebugServerConnected());
                            if (has_bp) {
                                g_breakpoints.erase(it);
                                if (arm_pcsx2) PCSX2RemoveBreakpoint(phys);
                            } else {
                                g_breakpoints.push_back(phys);
                                if (arm_pcsx2) PCSX2SetBreakpoint(phys);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                // Hover tooltip
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("0x%08X  %08X  %s", addr, code, disasm);
                    uint32_t op2 = code >> 26, fn2 = code & 0x3F;
                    bool is_j  = (op2 == 0x02 || op2 == 0x03);
                    bool is_br = (!is_j && IsBranchOrJump(code) &&
                                  !(op2 == 0x00 && (fn2 == 0x08 || fn2 == 0x09)));
                    if (is_j) {
                        uint32_t t = ((addr + 4) & 0xF0000000) | ((code & 0x3FFFFFF) << 2);
                        const char* ts = GetSymbolAny(t & 0x1FFFFFFF);
                        if (ts) ImGui::Text("Target: 0x%08X  [%s]", t, ts);
                        else    ImGui::Text("Target: 0x%08X", t);
                    } else if (is_br) {
                        int16_t off2 = (int16_t)(code & 0xFFFF);
                        uint32_t t   = addr + 4 + ((int32_t)off2 << 2);
                        const char* ts = GetSymbolAny(t & 0x1FFFFFFF);
                        if (ts) ImGui::Text("Target: 0x%08X  [%s]", t, ts);
                        else    ImGui::Text("Target: 0x%08X", t);
                    }
                    ImGui::EndTooltip();
                }

                // Draw cell content over the spanning selectable
                ImGui::TableSetColumnIndex(0);
                if (is_highlighted)
                    ImGui::TextColored(col, ">");
                else if (is_bp_hit)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.1f, 1.0f), "!");
                else if (is_bp)
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "o");
                else
                    ImGui::TextUnformatted(" ");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(col, "%08X", addr);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(col, "%08X", code);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(col, "%s", disasm);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (pc_changed) prev_pc = phys_pc;
    }
    ImGui::EndChild();
}

// Full GPR table + HI/LO, split out of ShowCodeTrace() into its own dockable
// tab so Disassembly isn't forced to share width with it. PC History (the
// old function-change log under this table) was dropped as redundant with
// the Disassembly tab's own PC tracking + the Call Stack/Breakpoints tabs.
void ShowEERegisters() {
    static bool s_had_first_snapshot = false;
    if (regs_valid) s_had_first_snapshot = true;
    if (!s_had_first_snapshot) {
        ImGui::TextDisabled("Waiting for PCSX2 CPU data...");
        return;
    }

    // Snapshot controls
    static bool s_show_snapshot = false;
    {
        if (ImGui::Button("Snapshot GPRs")) {
            if (TakeGPRSnapshot()) s_show_snapshot = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pauses PCSX2, reads flushed GPRs, then resumes.\nUse this to get accurate register values while the game runs.");
        if (g_gpr_snapshot_valid) {
            ImGui::SameLine();
            ImGui::Checkbox("Show Snapshot", &s_show_snapshot);
            if (s_show_snapshot) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f,1,0.8f,1), "PC 0x%08X", g_gpr_snapshot_pc);
            }
        }
    }

    // Quick health indicator
    if (!s_show_snapshot || !g_gpr_snapshot_valid) {
        int nonzero = 0;
        for (int g = 1; g < 32; g++) if (cpu_regs.GPR[g].lo) nonzero++;
        if (nonzero == 0) {
            if (g_pcsx2_paused)
                ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "All zero (paused) - game may be at startup or cpuRegs struct is stale");
            else
                ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "All zero - GPRs held in JIT host regs. Use Snapshot GPRs above.");
        } else {
            if (g_pcsx2_paused)
                ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "%d / 31 regs non-zero  [PAUSED - accurate]", nonzero);
            else
                ImGui::TextColored(ImVec4(0.8f,0.8f,0.4f,1), "%d / 31 regs non-zero  [running - may be stale]", nonzero);
        }
    } else {
        ImGui::TextColored(ImVec4(0.4f,1,0.8f,1), "Snapshot values (accurate at capture time)");
    }

    if (ImGui::BeginTable("GPRTable", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 148);
        ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 148);
        ImGui::TableHeadersRow();

        bool using_snap = s_show_snapshot && g_gpr_snapshot_valid;
        uint32_t ui_tick = SDL_GetTicks();
        for (int i = 0; i < 32; i += 2) {
            ImGui::TableNextRow();
            for (int col = 0; col < 2; col++) {
                int g = i + col;
                uint64_t val = using_snap ? g_gpr_snapshot[g] : cpu_regs.GPR[g].lo;
                bool flushed = using_snap ? true : IsGPRFlushed(cpu_regs.GPR[g]);

                uint32_t age_ms = ui_tick - gpr_changed_tick[g];
                ImVec4 valCol;
                if (g == 0) {
                    valCol = ImVec4(1,1,1,1);
                } else if (!using_snap && age_ms < 800) {
                    float t = age_ms / 800.0f;
                    float gc = 0.85f + t * 0.15f;
                    float rb = 0.2f  + t * 0.8f;
                    valCol = flushed ? ImVec4(rb, gc, rb, 1) : ImVec4(rb*0.6f, gc*0.6f, rb*0.6f, 1);
                } else {
                    valCol = using_snap ? ImVec4(0.4f,1,0.8f,1)
                           : flushed    ? ImVec4(1,1,1,1)
                                        : ImVec4(0.55f,0.55f,0.55f,1);
                }

                ImGui::TableSetColumnIndex(col * 2);
                ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "%s", GPR_NAMES[g]);

                ImGui::TableSetColumnIndex(col * 2 + 1);
                char val_str[24];
                snprintf(val_str, sizeof(val_str), "0x%016llX", val);
                bool can_edit = (g != 0) && !using_snap && g_cpu_source == CPU_RECOMP && g_recomp_backend.IsPaused();

                ImGui::PushID(g);
                ImGui::PushStyleColor(ImGuiCol_Text, valCol);
                ImGui::Selectable(val_str, false, ImGuiSelectableFlags_None);
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("copy_gpr")) {
                    if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(val_str);
                    if (can_edit && ImGui::MenuItem("Edit")) {
                        ImGui::OpenPopup("edit_gpr");
                    }
                    ImGui::EndPopup();
                }
                if (can_edit && ImGui::BeginPopup("edit_gpr")) {
                    static char s_edit_buf[16] = {};
                    static int  s_edit_reg = -1;
                    if (s_edit_reg != g) {
                        s_edit_reg = g;
                        snprintf(s_edit_buf, sizeof(s_edit_buf), "%08X", (uint32_t)val);
                    }
                    ImGui::Text("%s (halted)", GPR_NAMES[g]);
                    ImGui::SetNextItemWidth(140);
                    bool enter = ImGui::InputText("##editval", s_edit_buf, sizeof(s_edit_buf),
                                                  ImGuiInputTextFlags_CharsHexadecimal |
                                                  ImGuiInputTextFlags_CharsUppercase |
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                    if ((ImGui::Button("Write") || enter)) {
                        uint32_t newval = (uint32_t)strtoul(s_edit_buf, nullptr, 16);
                        if (g_recomp_backend.WriteRegister(g, newval))
                            AddLog("[Registers] %s = 0x%08X", GPR_NAMES[g], newval);
                        else
                            AddLog("[Registers] Write to %s failed", GPR_NAMES[g]);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (using_snap)
                        ImGui::TextColored(ImVec4(0.4f,1,0.8f,1), "Snapshot value (JIT-flushed)");
                    else if (!flushed)
                        ImGui::Text("JIT stale  hi=0x%016llX", cpu_regs.GPR[g].hi);
                    if (!using_snap && age_ms < 800)
                        ImGui::Text("Changed %u ms ago", age_ms);
                    else if (!using_snap && gpr_changed_tick[g] == 0)
                        ImGui::TextDisabled("Not yet observed changing");
                    else if (!using_snap)
                        ImGui::TextDisabled("Last changed %u ms ago", age_ms);
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // HI / LO
    ImGui::Spacing();
    if (s_show_snapshot && g_gpr_snapshot_valid) {
        ImGui::Text("HI: 0x%016llX", g_gpr_snapshot_hi);
        ImGui::Text("LO: 0x%016llX", g_gpr_snapshot_lo);
    } else {
        ImGui::Text("HI: 0x%016llX", cpu_regs.HI.lo);
        ImGui::Text("LO: 0x%016llX", cpu_regs.LO.lo);
    }
}

// IOP (R3000A) register panel. Only meaningful when connected to the Recomp
// backend — ps2EntryRunner's embedded IopRuntime is the only source of this
// data (PCSX2 backend has no IOP equivalent wired up).
void ShowIopRegisters() {
    // PCSX2 path: the recomp backend publishes IOP state over shared memory, but
    // PCSX2 does not — pull IOP GPR/pc from the DebugServer (cpu:"iop") on demand.
    if (g_cpu_source == CPU_PCSX2) {
        static uint32_t s_pc = 0, s_gpr[32] = {}, s_hi = 0, s_lo = 0;
        static bool     s_valid = false;
        static uint32_t s_last_ms = 0;

        if (!PCSX2DebugServerConnected()) {
            ImGui::TextDisabled("Not connected to PCSX2 DebugServer.");
            return;
        }

        // Throttle the blocking TCP read to ~10Hz.
        uint32_t now = SDL_GetTicks();
        if (now - s_last_ms >= 100) {
            s_last_ms = now;
            s_valid = PCSX2ReadIopRegisters(s_pc, s_gpr, s_hi, s_lo);
        }
        if (!s_valid) {
            ImGui::TextDisabled("Waiting for IOP data (DebugServer may not expose IOP)...");
            return;
        }

        ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "IOP  PC 0x%08X", s_pc);
        if (ImGui::BeginTable("IOPGPRTablePcsx2", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();
            for (int i = 0; i < 32; i += 2) {
                ImGui::TableNextRow();
                for (int col = 0; col < 2; col++) {
                    int g = i + col;
                    ImGui::TableSetColumnIndex(col * 2);
                    ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "%s", GPR_NAMES[g]);
                    ImGui::TableSetColumnIndex(col * 2 + 1);
                    char val_str[16];
                    snprintf(val_str, sizeof(val_str), "0x%08X", s_gpr[g]);
                    ImGui::PushID(2000 + g);
                    ImGui::Selectable(val_str, false, ImGuiSelectableFlags_None);
                    if (ImGui::BeginPopupContextItem("copy_iop_gpr_pcsx2")) {
                        if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(val_str);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::Text("HI: 0x%08X", s_hi);
        ImGui::Text("LO: 0x%08X", s_lo);
        return;
    }

    if (g_cpu_source != CPU_RECOMP) {
        ImGui::TextDisabled("IOP state is only available on the Recomp backend.");
        return;
    }

    const RecompDebugState* ext = g_recomp_backend.ReadExtended();
    if (!ext || !ext->iop_valid) {
        ImGui::TextDisabled("Waiting for IOP data...");
        return;
    }

    if (ext->iop_running)
        ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "IOP running  PC 0x%08X", ext->iop_pc);
    else
        ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "IOP halted  PC 0x%08X", ext->iop_pc);

    if (ImGui::BeginTable("IOPGPRTable", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 32; i += 2) {
            ImGui::TableNextRow();
            for (int col = 0; col < 2; col++) {
                int g = i + col;
                ImGui::TableSetColumnIndex(col * 2);
                ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "%s", GPR_NAMES[g]);

                ImGui::TableSetColumnIndex(col * 2 + 1);
                char val_str[16];
                snprintf(val_str, sizeof(val_str), "0x%08X", ext->iop_gpr[g]);
                ImGui::PushID(1000 + g);
                ImGui::Selectable(val_str, false, ImGuiSelectableFlags_None);
                if (ImGui::BeginPopupContextItem("copy_iop_gpr")) {
                    if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(val_str);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("HI: 0x%08X", ext->iop_hi);
    ImGui::Text("LO: 0x%08X", ext->iop_lo);
}

// Combined "Registers" window: EE Registers and IOP Registers as two
// CollapsingHeader panels sharing one dockable tab, mirroring the
// Markers window's Breakpoints/Watchpoints/etc. layout.
void ShowRegisters() {
    if (ImGui::CollapsingHeader("EE Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowEERegisters();
    }
    if (ImGui::CollapsingHeader("IOP Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowIopRegisters();
    }
}
