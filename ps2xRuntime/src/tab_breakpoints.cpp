#include "imgui.h"
#include "debugger_state.h"
#include <algorithm>

// Markers tab: breakpoints, watchpoints, call stack, and registers used to be
// separate sub-tabs (Execution/Watchpoints/Call Stack), which meant only one
// was visible at a time. Collapsing sections on one scrollable page let all of
// them stay visible together. Only Watch Registers (a user-curated subset) is
// included here — the full GPR dump is redundant with the standalone "EE
// Registers" window and lives there exclusively now. IOP Registers stays a
// separate top-level window — it's IOP-CPU-specific and rarely needed at the
// same time as EE-side breakpoint work.
void ShowBreakpoints() {
    ImGui::BeginChild("##markers_scroll", ImVec2(0, 0), false);

    // ---- Watchpoints ----
    if (ImGui::CollapsingHeader("Watchpoints", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Watchpoints", &g_wp_enabled);
        bool any_hit = false;
        for (auto& wp : g_watchpoints) if (wp.hit) { any_hit = true; break; }
        if (any_hit) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
            ImGui::Text("  WRITE DETECTED");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Clear Hits"))
                for (auto& wp : g_watchpoints) wp.hit = false;
        }
        ImGui::Separator();
        static char s_wp_buf[12] = {};
        static int  s_wp_size_idx = 2;
        static const char* size_labels[] = {"Byte", "Half", "Word"};
        static const int   size_bytes[]  = {1, 2, 4};
        ImGui::SetNextItemWidth(85);
        ImGui::InputText("##wpaddr", s_wp_buf, sizeof(s_wp_buf),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::Combo("##wpsize", &s_wp_size_idx, size_labels, 3);
        ImGui::SameLine();
        if (ImGui::Button("Add##wp")) {
            uint32_t a = (uint32_t)strtoul(s_wp_buf, nullptr, 16);
            if (a < 0x2000000u) {
                bool dup = false;
                for (auto& wp : g_watchpoints)
                    if (wp.addr == a && wp.size == size_bytes[s_wp_size_idx]) { dup = true; break; }
                if (!dup) {
                    Watchpoint wp = {}; wp.addr = a; wp.size = size_bytes[s_wp_size_idx];
                    g_watchpoints.push_back(wp);
                    if (g_cpu_source == CPU_PCSX2 && PCSX2DebugServerConnected())
                        PCSX2SetWatchpoint(a, (uint32_t)wp.size);
                }
            }
            s_wp_buf[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All##wp") && !g_watchpoints.empty()) {
            if (g_cpu_source == CPU_PCSX2 && PCSX2DebugServerConnected())
                for (auto& wp : g_watchpoints) PCSX2RemoveWatchpoint(wp.addr, (uint32_t)wp.size);
            g_watchpoints.clear();
        }
        ImGui::Separator();
        if (g_watchpoints.empty()) {
            ImGui::TextDisabled("No watchpoints set. Enter an EE RAM address (physical) above.");
        } else {
            for (int i = 0; i < (int)g_watchpoints.size(); i++) {
                ImGui::PushID(i);
                Watchpoint& wp = g_watchpoints[i];
                bool is_hit = wp.hit;
                if (is_hit) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
                char wp_row[64];
                snprintf(wp_row, sizeof(wp_row), "  %c  0x%08X  [%s]  = 0x%0*X",
                            is_hit ? '!' : 'o', wp.addr,
                            wp.size == 1 ? "Byte" : wp.size == 2 ? "Half" : "Word",
                            wp.size * 2, wp.last_val);
                ImGui::TextUnformatted(wp_row);
                if (is_hit) ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("wpctx")) {
                    if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(wp_row);
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("X##wp")) {
                    if (g_cpu_source == CPU_PCSX2 && PCSX2DebugServerConnected())
                        PCSX2RemoveWatchpoint(wp.addr, (uint32_t)wp.size);
                    g_watchpoints.erase(g_watchpoints.begin() + i);
                    i--;
                }
                ImGui::PopID();
            }
        }
    }

    // ---- Call Stack ----
    if (ImGui::CollapsingHeader("Call Stack", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowCallStack();
    }

    // ---- Watch Registers (small pinned-register list; folded in here since
    // it's too small to warrant its own top-level window; the full GPR dump
    // lives only in the standalone "EE Registers" window now) ----
    if (ImGui::CollapsingHeader("Watch Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShowRegWatch();
    }

    // ---- Threads (recomp only — guest scheduler snapshot) ----
    if (g_cpu_source == CPU_RECOMP && g_recomp_backend.IsConnected()) {
        if (ImGui::CollapsingHeader("Threads")) {
            ShowThreads();
        }
    }

    // ---- Breakpoints (unified) ----
    // One panel that adapts to the active CPU source. The two backends stay
    // distinct — Recomp = shared-mem slots (with optional GPR conditions),
    // PCSX2 = DebugServer exec BPs — but they're presented under a single
    // "Breakpoints" header instead of two mutually-exclusive CPU-gated blocks.
    if (ImGui::CollapsingHeader("Breakpoints", ImGuiTreeNodeFlags_DefaultOpen)) {
      // ==== Recomp backend (shared-memory slots) ====
      if (g_cpu_source == CPU_RECOMP) {
        if (!g_recomp_backend.IsConnected()) {
            ImGui::TextDisabled("Not connected to Recomp backend.");
        } else {
            static char s_bp_buf[12]  = {};
            static char s_cond_buf[9] = {}; // hex value to compare against, blank = unconditional
            static int  s_cond_reg    = -1; // -1 = none, else GPR index

            const RecompDebugState* ext = g_recomp_backend.ReadExtended();
            bool paused = g_recomp_backend.IsPaused();
            if (paused) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.1f, 1.0f));
                ImGui::Text("  HIT: 0x%08X", g_recomp_backend.HitAddress());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Resume##bp")) g_recomp_backend.Resume();
            }
            ImGui::Separator();

            ImGui::SetNextItemWidth(85);
            ImGui::InputText("##rbpaddr", s_bp_buf, sizeof(s_bp_buf),
                             ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Cond GPR", &s_cond_reg);
            if (s_cond_reg < -1) s_cond_reg = -1;
            if (s_cond_reg > 31) s_cond_reg = 31;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(85);
            ImGui::InputText("== value", s_cond_buf, sizeof(s_cond_buf),
                             ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            if (ImGui::Button("Add##rbp") && s_bp_buf[0]) {
                uint32_t addr = (uint32_t)strtoul(s_bp_buf, nullptr, 16);
                uint8_t  cond_reg = (s_cond_reg >= 0) ? (uint8_t)s_cond_reg : kDbgNoCondReg;
                uint32_t cond_val = (uint32_t)strtoul(s_cond_buf, nullptr, 16);
                int slot = g_recomp_backend.AddBreakpoint(addr, cond_reg, cond_val);
                if (slot >= 0) {
                    AddLog("[Recomp] Breakpoint armed at 0x%08X (slot %d)%s", addr, slot,
                           cond_reg == kDbgNoCondReg ? "" : " (conditional)");
                    bool already_cached = false;
                    for (auto& bp : g_recomp_breakpoints) {
                        if (bp.addr == addr) { bp.cond_reg = cond_reg; bp.cond_value = cond_val; already_cached = true; break; }
                    }
                    if (!already_cached) g_recomp_breakpoints.push_back({addr, cond_reg, cond_val});
                } else {
                    AddLog("[Recomp] All %u breakpoint slots full", kDbgMaxBreakpoints);
                }
                s_bp_buf[0] = '\0';
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All##rbp")) {
                g_recomp_backend.ClearAllBreakpoints();
                g_recomp_breakpoints.clear();
                AddLog("[Recomp] All breakpoints cleared");
            }

            ImGui::Separator();
            if (ext) {
                for (uint32_t i = 0; i < kDbgMaxBreakpoints; ++i) {
                    const DbgBreakpoint& slot = ext->bp_slots[i];
                    if (!slot.enabled) continue;
                    ImGui::PushID((int)i);
                    bool is_hit = paused && slot.addr == g_recomp_backend.HitAddress();
                    if (is_hit) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.1f, 1.0f));
                    char bp_row[64];
                    if (slot.cond_reg == kDbgNoCondReg)
                        snprintf(bp_row, sizeof(bp_row), "  %c  0x%08X", is_hit ? '!' : 'o', slot.addr);
                    else
                        snprintf(bp_row, sizeof(bp_row), "  %c  0x%08X  [gpr%u == 0x%08X]", is_hit ? '!' : 'o',
                                    slot.addr, slot.cond_reg, slot.cond_value);
                    ImGui::TextUnformatted(bp_row);
                    if (is_hit) ImGui::PopStyleColor();
                    if (ImGui::BeginPopupContextItem("bpctx")) {
                        if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(bp_row);
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("X##rbp")) {
                        g_recomp_backend.RemoveBreakpoint(slot.addr);
                        uint32_t rm_addr = slot.addr;
                        g_recomp_breakpoints.erase(
                            std::remove_if(g_recomp_breakpoints.begin(), g_recomp_breakpoints.end(),
                                           [rm_addr](const RecompBreakpoint& bp) { return bp.addr == rm_addr; }),
                            g_recomp_breakpoints.end());
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TextDisabled("Physical address (hex). Optional GPR condition narrows the hit.");
        }
      }
      // ==== PCSX2 backend (DebugServer exec BPs) ====
      // g_breakpoints (the generic execution-BP vector) is also populated from
      // the disasm gutter right-click; list/add/remove here and mirror to the
      // PCSX2 DebugServer.
      else if (g_cpu_source == CPU_PCSX2) {
            const bool connected = PCSX2DebugServerConnected();
            if (!connected)
                ImGui::TextDisabled("Not connected to PCSX2 DebugServer — BPs are local only.");
            ImGui::Separator();

            static char s_pbp_buf[12] = {};
            ImGui::SetNextItemWidth(85);
            ImGui::InputText("##pbpaddr", s_pbp_buf, sizeof(s_pbp_buf),
                             ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            if (ImGui::Button("Add##pbp") && s_pbp_buf[0]) {
                uint32_t addr = (uint32_t)strtoul(s_pbp_buf, nullptr, 16);
                if (!IsBreakpointSet(addr)) {
                    g_breakpoints.push_back(addr);
                    if (connected && PCSX2SetBreakpoint(addr))
                        AddLog("[PCSX2] Breakpoint armed at 0x%08X", addr);
                }
                s_pbp_buf[0] = '\0';
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All##pbp") && !g_breakpoints.empty()) {
                if (connected) PCSX2ClearAllBreakpoints();
                g_breakpoints.clear();
            }

            ImGui::Separator();
            if (g_breakpoints.empty()) {
                ImGui::TextDisabled("No breakpoints set. Enter an address (hex) above or use the disasm gutter.");
            } else {
                for (int i = 0; i < (int)g_breakpoints.size(); i++) {
                    ImGui::PushID(i);
                    uint32_t addr = g_breakpoints[i];
                    char bp_row[32];
                    snprintf(bp_row, sizeof(bp_row), "  o  0x%08X", addr);
                    ImGui::TextUnformatted(bp_row);
                    if (ImGui::BeginPopupContextItem("pbpctx")) {
                        if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(bp_row);
                        if (ImGui::MenuItem("Go to in disasm")) {
                            g_disasm_follow_pc  = false;
                            g_disasm_pinned     = addr;
                            g_disasm_scroll_req = true;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("X##pbp")) {
                        if (connected) PCSX2RemoveBreakpoint(addr);
                        g_breakpoints.erase(g_breakpoints.begin() + i);
                        i--;
                    }
                    ImGui::PopID();
                }
            }
        }
    }

    ImGui::EndChild();
}

static const char* ThreadStatusName(int status) {
    switch (status) {
        case 0x01: return "RUN";
        case 0x02: return "READY";
        case 0x04: return "WAIT";
        case 0x08: return "SUSPEND";
        case 0x0c: return "WAIT+SUSP";
        case 0x10: return "DORMANT";
        default:   return "?";
    }
}

static const char* ThreadWaitTypeName(int waitType) {
    switch (waitType) {
        case 0: return "-";
        case 1: return "sleep";
        case 2: return "sema";
        case 3: return "event";
        default: return "?";
    }
}

void ShowThreads() {
    const RecompDebugState* ext = g_recomp_backend.ReadExtended();
    if (!ext) { ImGui::TextDisabled("Not connected."); return; }

    ImGui::TextDisabled("%u guest thread(s) (host-thread-per-PS2-thread scheduler)", ext->thread_count);
    ImGui::Separator();
    // Fixed height instead of (0,0) -- see ShowCallStack() for why (0,0)
    // greedily eats the rest of Markers' shared scroll region.
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float h = ImGui::GetStyle().FramePadding.y * 2 + row_h * 2 +
              row_h * (float)std::min((int)ext->thread_count + 1, 10);
    ImGui::BeginChild("##threads", ImVec2(0, h), false, 0);
    ImGui::TextDisabled(" TID   Status      Wait       PC          Entry       Stack       Prio");
    ImGui::Separator();
    for (uint32_t i = 0; i < ext->thread_count; ++i) {
        const DbgThreadInfo& t = ext->threads[i];
        ImGui::PushID((int)i);
        bool running = (t.status == 0x01);
        if (running) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::Text(" %-5d %-11s %-10s 0x%08X  0x%08X  0x%08X  %d",
                    t.tid, ThreadStatusName(t.status), ThreadWaitTypeName(t.waitType),
                    t.currentPc, t.entry, t.stack, t.currentPriority);
        if (running) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Go")) {
            g_disasm_follow_pc  = false;
            g_disasm_pinned     = t.currentPc;
            g_disasm_scroll_req = true;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}
