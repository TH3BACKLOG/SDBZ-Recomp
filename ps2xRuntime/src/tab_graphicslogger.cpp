#include <cstdio>
#include "imgui.h"
#include "debugger_state.h"

void ShowGraphicsLogger() {
    // --- PINE status ---
    ImGui::SeparatorText("GS Privileged Registers  (via PINE IPC or Recomp shm)");
    if (gs_pine_ok) {
        ImGui::TextColored(ImVec4(0,1,0,1), "%s: connected",
                            (g_cpu_source == CPU_RECOMP) ? "Recomp" : "PINE");
    } else if (g_cpu_source == CPU_RECOMP) {
        ImGui::TextColored(ImVec4(1,0.4f,0,1), "Recomp: not connected");
    } else {
        ImGui::TextColored(ImVec4(1,0.4f,0,1), "PINE: not connected (TCP 127.0.0.1:%d)", pine_port);
        ImGui::TextDisabled("  Enable in PCSX2: Settings > Advanced > PINE Settings > Enable");
    }
    ImGui::Separator();

    if (gs_pine_ok) {
        auto HexRow = [](const char* name, uint32_t addr, uint64_t val) {
            ImGui::Text("  %-10s  [0x%08X]  =  %016llX", name, addr, (unsigned long long)val);
        };
        HexRow("PMODE",    0x12001000, gs_regs.PMODE);
        HexRow("SMODE2",   0x12001020, gs_regs.SMODE2);
        HexRow("DISPFB1",  0x12001070, gs_regs.DISPFB1);
        HexRow("DISPLAY1", 0x12001080, gs_regs.DISPLAY1);
        HexRow("DISPFB2",  0x12001090, gs_regs.DISPFB2);
        HexRow("DISPLAY2", 0x120010A0, gs_regs.DISPLAY2);
        HexRow("CSR",      0x12001100, gs_regs.CSR);

        ImGui::Separator();
        ImGui::SeparatorText("Decoded");

        ImGui::Text("  PMODE:    EN1=%d  EN2=%d  ALP=0x%02X",
            (int)(gs_regs.PMODE & 1),
            (int)((gs_regs.PMODE >> 1) & 1),
            (int)((gs_regs.PMODE >> 6) & 0xFF));

        ImGui::Text("  SMODE2:   %s  FFMD=%s",
            (gs_regs.SMODE2 & 1) ? "INTERLACED" : "PROGRESSIVE",
            (gs_regs.SMODE2 & 2) ? "FRAME" : "FIELD");

        auto DecodeDISPFB = [](const char* label, uint64_t v) {
            uint32_t fbp = (uint32_t)(v & 0x1FF);
            uint32_t fbw = (uint32_t)((v >> 9) & 0x3F);
            uint32_t psm = (uint32_t)((v >> 15) & 0x1F);
            uint32_t dbx = (uint32_t)((v >> 32) & 0x7FF);
            uint32_t dby = (uint32_t)((v >> 43) & 0x7FF);
            ImGui::Text("  %-10s  FBP=0x%03X  FBW=%u (%upx)  PSM=0x%02X  DBX=%u  DBY=%u",
                label, fbp, fbw, fbw * 64, psm, dbx, dby);
        };
        DecodeDISPFB("DISPFB1:", gs_regs.DISPFB1);
        DecodeDISPFB("DISPFB2:", gs_regs.DISPFB2);

        ImGui::Text("  CSR:      FIELD=%s  VSINT=%d  HSINT=%d  REV=0x%02X  ID=0x%02X",
            ((gs_regs.CSR >> 12) & 1) ? "ODD" : "EVEN",
            (int)((gs_regs.CSR >> 3) & 1),
            (int)((gs_regs.CSR >> 2) & 1),
            (int)((gs_regs.CSR >> 16) & 0xFF),
            (int)((gs_regs.CSR >> 24) & 0xFF));
    }

    ImGui::Separator();
    ImGui::SeparatorText("EE / Sync");
    ImGui::Text("EE RAM base:   0x%llX", (unsigned long long)ee_ram_base);
    ImGui::Text("cpuRegs base:  0x%llX", (unsigned long long)cpu_regs_base);
    ImGui::Text("Sync frames:   %llu",   (unsigned long long)sync_count);
    ImGui::Text("Last sync ok:  %s",     sync_last_ok ? "YES" : "NO");
    ImGui::Separator();
    ImGui::SeparatorText("Display");
    ImGui::Text("SDL window:    %d x %d", g_win_w,  g_win_h);
    ImGui::Text("GL drawable:   %d x %d", g_draw_w, g_draw_h);
    ImGui::Text("ImGui size:    %.0f x %.0f", ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    ImGui::Text("FramebufScale: %.2f x %.2f", ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
}
