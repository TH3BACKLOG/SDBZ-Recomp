#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "debugger_state.h"

void ShowMemoryViewer() {
    static char    s_addr_buf[12] = "00100000";
    static uint8_t  s_mem_buf[256] = {};
    static int      s_mem_cpu = 0; // 0 = EE, 1 = IOP (PCSX2 only)

    // IOP mode reads guest RAM through the DebugServer JSON path (no reliable
    // RPM base-scan for IOP); only offered under PCSX2.
    const bool iop_mode  = (s_mem_cpu == 1 && g_cpu_source == CPU_PCSX2);
    const uint32_t addr_limit = iop_mode ? 0x200000u : 0x2000000u; // IOP 2MB vs EE 32MB

    const bool pcsx2_ok  = iop_mode
                             ? PCSX2DebugServerConnected()
                             : (ee_ram_base != 0 && pcsx2_handle != nullptr);
    const bool recomp_ok = (!iop_mode && g_cpu_source == CPU_RECOMP && g_recomp_backend.IsConnected());
    const bool connected = pcsx2_ok || recomp_ok;

    // EE/IOP selector (PCSX2 only — recomp backend Memory Viewer stays EE).
    if (g_cpu_source == CPU_PCSX2) {
        ImGui::TextUnformatted("CPU:");
        ImGui::SameLine();
        ImGui::RadioButton("EE", &s_mem_cpu, 0);
        ImGui::SameLine();
        ImGui::RadioButton("IOP", &s_mem_cpu, 1);
    }

    // --- toolbar ---
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputText("##memaddr", s_addr_buf, sizeof(s_addr_buf),
                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Go")) {
        uint32_t parsed = 0;
        if (sscanf(s_addr_buf, "%X", &parsed) == 1)
            g_mem_view_addr = parsed & ~15u;  // align to 16-byte row
    }
    ImGui::SameLine();
    if (ImGui::Button("PC")) {
        uint32_t phys = cpu_regs.PC & 0x1FFFFFFFu;
        g_mem_view_addr = phys & ~15u;
        snprintf(s_addr_buf, sizeof(s_addr_buf), "%08X", g_mem_view_addr);
    }
    ImGui::SameLine();
    if (ImGui::Button("< Prev")) {
        g_mem_view_addr = (g_mem_view_addr >= 256) ? g_mem_view_addr - 256 : 0;
        snprintf(s_addr_buf, sizeof(s_addr_buf), "%08X", g_mem_view_addr);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next >")) {
        uint32_t next = g_mem_view_addr + 256;
        if (next < addr_limit) { g_mem_view_addr = next; snprintf(s_addr_buf, sizeof(s_addr_buf), "%08X", g_mem_view_addr); }
    }

    if (!connected) {
        ImGui::TextDisabled(iop_mode  ? "Not connected to PCSX2 DebugServer (IOP)."
                          : recomp_ok ? "Not connected to recomp backend."
                                      : "Not connected to PCSX2.");
        return;
    }

    // --- write toolbar (recomp backend only — PCSX2 side has no write-memory
    // transport wired up yet, unlike WriteMemory() already present on
    // RecompilerBackend) ---
    {
        static char s_wr_addr[12] = {};
        static char s_wr_val[20]  = {};
        static int  s_wr_size_idx = 2;
        static const char* size_labels[] = {"Byte", "Half", "Word", "Qword"};
        static const int   size_bytes[]  = {1, 2, 4, 8};

        ImGui::SetNextItemWidth(85);
        ImGui::InputTextWithHint("##wraddr", "addr", s_wr_addr, sizeof(s_wr_addr),
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##wrsize", &s_wr_size_idx, size_labels, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##wrval", "value", s_wr_val, sizeof(s_wr_val),
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        ImGui::BeginDisabled(!recomp_ok);
        if (ImGui::Button("Write")) {
            uint32_t addr = (uint32_t)strtoul(s_wr_addr, nullptr, 16);
            uint64_t val  = strtoull(s_wr_val, nullptr, 16);
            int size = size_bytes[s_wr_size_idx];
            if (addr < 0x2000000u) {
                if (g_recomp_backend.WriteMemory(addr, &val, (uint32_t)size))
                    AddLog("[Memory] Wrote %d byte(s) = 0x%llX @ 0x%08X", size, (unsigned long long)val, addr);
                else
                    AddLog("[Memory] Write to 0x%08X failed", addr);
            }
        }
        ImGui::EndDisabled();
        if (!recomp_ok) {
            ImGui::SameLine();
            ImGui::TextDisabled("(recomp backend only)");
        }
    }

    // Read 256 bytes fresh every frame
    memset(s_mem_buf, 0, sizeof(s_mem_buf));
    if (iop_mode) {
        if (g_mem_view_addr < addr_limit)
            PCSX2ReadIopMemory(g_mem_view_addr, s_mem_buf, sizeof(s_mem_buf));
    } else if (recomp_ok) {
        if (g_mem_view_addr < 0x2000000u) {
            // Prefer the on-demand request/response path so any address is
            // readable, not just the 2KB window centred on the current PC.
            if (!g_recomp_backend.ReadMemory(g_mem_view_addr, s_mem_buf, sizeof(s_mem_buf))) {
                uint32_t win_end = ee_ram_window_addr + (uint32_t)sizeof(ee_ram_window);
                for (uint32_t i = 0; i < 256; i++) {
                    uint32_t a = g_mem_view_addr + i;
                    if (a >= ee_ram_window_addr && a < win_end)
                        s_mem_buf[i] = ee_ram_window[a - ee_ram_window_addr];
                }
            }
        }
    } else if (pcsx2_ok && g_mem_view_addr < 0x2000000u) {
        SIZE_T br = 0;
        ReadProcessMemory(pcsx2_handle, (LPCVOID)(ee_ram_base + g_mem_view_addr),
                          s_mem_buf, sizeof(s_mem_buf), &br);
    }

    ImGui::Separator();

    // Hex dump: 16 bytes per row
    ImGui::BeginChild("##memview", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const uint32_t rows = 256 / 16;
    for (uint32_t r = 0; r < rows; r++) {
        const uint32_t row_addr = g_mem_view_addr + r * 16;
        const uint8_t* p        = s_mem_buf + r * 16;

        // Address column
        ImGui::TextDisabled("%08X  ", row_addr);
        ImGui::SameLine(0, 0);

        // Hex bytes — two groups of 8 separated by an extra space
        char hex_buf[64];
        int  pos = 0;
        for (int i = 0; i < 16; i++) {
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", p[i]);
            if (i == 7) hex_buf[pos++] = ' ';
        }
        hex_buf[pos] = '\0';
        ImGui::TextUnformatted(hex_buf);
        ImGui::SameLine(0, 0);

        // ASCII column
        char ascii[20] = " |";
        for (int i = 0; i < 16; i++)
            ascii[2 + i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
        ascii[18] = '|'; ascii[19] = '\0';
        ImGui::TextDisabled("%s", ascii);
    }

    ImGui::EndChild();

    // Mouse wheel scrolls by one row (16 bytes) per tick
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            int64_t new_addr = (int64_t)g_mem_view_addr + (int32_t)(-wheel) * 16;
            if (new_addr < 0) new_addr = 0;
            if (new_addr + 256 > (int64_t)addr_limit) new_addr = (int64_t)addr_limit - 256;
            g_mem_view_addr = (uint32_t)new_addr & ~15u;
            snprintf(s_addr_buf, sizeof(s_addr_buf), "%08X", g_mem_view_addr);
        }
    }
}
