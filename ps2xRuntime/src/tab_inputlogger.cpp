#include <cstdio>
#include "imgui.h"
#include "debugger_state.h"

void SaveConfig(); // main_gui.cpp — persists cfg_pad_* alongside every other setting
std::string DescribeBinding(const PadBinding& b); // main_gui.cpp
void PollPadBindCapture();                        // main_gui.cpp — call once/frame while a bind capture is armed

static const char* kPadInputNames[(int)PadInput::Count] = {
    "Select", "L3", "R3", "Start", "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left",
    "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square",
    "Left Stick Right", "Left Stick Left", "Left Stick Down", "Left Stick Up",
    "Right Stick Right", "Right Stick Left", "Right Stick Down", "Right Stick Up",
};

static void ShowPadBindingTable() {
    ImGui::SeparatorText("Button Bindings (PCSX2-style)");
    ImGui::TextDisabled("Binding any input below switches this port from whole-device passthrough "
                         "to per-button resolution. Not yet implemented: profiles, Automatic Mapping, Macros.");

    if (g_pad_bind_capture_port == cfg_pad_port) PollPadBindCapture();

    if (ImGui::BeginTable("##pad_bindings", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("PS2 Input", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Bound To");
        ImGui::TableSetupColumn("Sensitivity", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)PadInput::Count; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPadInputNames[i]);

            bool capturingThis = (g_pad_bind_capture_port == cfg_pad_port && g_pad_bind_capture_input == i);
            PadBinding& b = cfg_pad_bindings[cfg_pad_port][i];

            ImGui::TableSetColumnIndex(1);
            if (capturingThis) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Press a key or button... (Esc to cancel)");
            } else {
                ImGui::TextUnformatted(DescribeBinding(b).c_str());
            }

            ImGui::TableSetColumnIndex(2);
            bool sensitivityApplicable = (i >= (int)PadInput::LeftStickXPos) || b.isAxis;
            if (b.deviceType != PadBindDeviceType::None && sensitivityApplicable) {
                float pct = b.sensitivity * 100.0f;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##sens", &pct, 1.0f, 1.0f, 100.0f, "%.0f%%")) {
                    b.sensitivity = pct / 100.0f;
                    SaveConfig();
                }
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button(capturingThis ? "Cancel" : "Bind")) {
                if (capturingThis) {
                    g_pad_bind_capture_port = -1;
                } else {
                    g_pad_bind_capture_port  = cfg_pad_port;
                    g_pad_bind_capture_input = i;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                b = PadBinding{};
                SaveConfig();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
}

void ShowInputLogger() {
    ImGui::SeparatorText("Host Controller Mapping");
    {
        static const char* source_names[] = { "Auto (first found)", "XInput", "SDL GameController" };
        int source = cfg_pad_source;
        if (ImGui::Combo("Source", &source, source_names, IM_ARRAYSIZE(source_names))) {
            cfg_pad_source = source;
            SaveConfig();
        }
        if (cfg_pad_source == 1) {
            int slot = cfg_pad_xinput_slot;
            if (ImGui::SliderInt("XInput slot", &slot, 0, 3)) {
                cfg_pad_xinput_slot = slot;
                SaveConfig();
            }
        } else if (cfg_pad_source == 2) {
            int idx = cfg_pad_sdl_index;
            if (ImGui::InputInt("SDL device index", &idx)) {
                if (idx < 0) idx = 0;
                cfg_pad_sdl_index = idx;
                SaveConfig();
            }
        }

        int port = cfg_pad_port;
        if (ImGui::RadioButton("Feeds Controller 1", port == 0)) { port = 0; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Feeds Controller 2", port == 1)) { port = 1; }
        if (port != cfg_pad_port) {
            cfg_pad_port = port;
            SaveConfig();
        }
        ImGui::TextDisabled("Only active while attached to the Recomp backend (CPU_RECOMP) -- "
                             "mirrors this host controller into the running game each frame, like PCSX2's own pad input.");
    }
    ImGui::Separator();

    ShowPadBindingTable();

    ImGui::SeparatorText(cfg_pad_port == 0 ? "Controller 1 (DualShock 2)" : "Controller 2 (DualShock 2)");

    float lx_f = (pad0.lx - 128) / 128.0f;
    float ly_f = (pad0.ly - 128) / 128.0f;
    float rx_f = (pad0.rx - 128) / 128.0f;
    float ry_f = (pad0.ry - 128) / 128.0f;
    auto pressed = [](uint16_t mask) { return !(pad0.buttons & mask); };

    const float IW = (g_ctrl_img_w > 0) ? (float)g_ctrl_img_w : 516.0f;
    const float IH = (g_ctrl_img_h > 0) ? (float)g_ctrl_img_h : 344.0f;
    const float DW = IW, DH = IH;

    ImGui::BeginChild("##ctrl_canvas", ImVec2(DW + 20, DH + 20), false, ImGuiWindowFlags_NoScrollbar);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(o.x + DW, o.y + DH);
    auto as_tid = [](GLuint t) { return (ImTextureID)(uintptr_t)t; };

    if (!g_ctrl_tex_base) {
        dl->AddText(o, IM_COL32(255, 100, 100, 255),
            "Controller images not found.\nPlace the 4 PNGs in assets\\controller\\ next to the exe,\nthen restart.");
        ImGui::Dummy(ImVec2(DW, DH));
        ImGui::EndChild();
        ImGui::TextDisabled("SDL GameController (DualShock 2 via DS4Windows/Steam Input or direct BT). Falls back to EE RAM scan.");
        return;
    }

    dl->AddImage(as_tid(g_ctrl_tex_base), o, p1);

    for (const auto& b : g_btn_tex)
        if (b.tex && pressed(b.mask))
            dl->AddImage(as_tid(b.tex), o, p1);
    if (g_ctrl_tex_analog_btn && g_ctrl_analog_on)
        dl->AddImage(as_tid(g_ctrl_tex_analog_btn), o, p1);

    if (g_ctrl_tex_analog) {
        ImVec2 anal_min(o.x + 0.44f * DW, o.y + 0.51f * DH);
        ImVec2 anal_max(o.x + 0.56f * DW, o.y + 0.65f * DH);
        if (ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(anal_min, anal_max))
            g_ctrl_analog_on = !g_ctrl_analog_on;
        ImU32 atint = g_ctrl_analog_on ? IM_COL32(255,255,255,255) : IM_COL32(255,255,255,60);
        dl->AddImage(as_tid(g_ctrl_tex_analog), o, p1, ImVec2(0,0), ImVec2(1,1), atint);
    }

    if (g_ctrl_tex_outline)
        dl->AddImage(as_tid(g_ctrl_tex_outline), o, p1);

    float stick_r  = DW * 0.072f;
    float dot_r    = DW * 0.018f;
    float travel   = stick_r - dot_r;
    float lsx = o.x + 0.369f * DW, lsy = o.y + 0.742f * DH;
    float rsx = o.x + 0.633f * DW, rsy = o.y + 0.742f * DH;
    ImU32 ring_col  = IM_COL32(220, 220, 220, 200);
    ImU32 cross_col = IM_COL32(140, 140, 140, 130);
    ImU32 dot_col   = IM_COL32(255, 255, 255, 245);
    dl->AddCircle(ImVec2(lsx, lsy), stick_r, ring_col, 48, 1.5f);
    dl->AddLine(ImVec2(lsx - stick_r, lsy), ImVec2(lsx + stick_r, lsy), cross_col, 1.0f);
    dl->AddLine(ImVec2(lsx, lsy - stick_r), ImVec2(lsx, lsy + stick_r), cross_col, 1.0f);
    dl->AddCircleFilled(ImVec2(lsx + lx_f*travel, lsy + ly_f*travel), dot_r, dot_col);
    dl->AddCircle(ImVec2(rsx, rsy), stick_r, ring_col, 48, 1.5f);
    dl->AddLine(ImVec2(rsx - stick_r, rsy), ImVec2(rsx + stick_r, rsy), cross_col, 1.0f);
    dl->AddLine(ImVec2(rsx, rsy - stick_r), ImVec2(rsx, rsy + stick_r), cross_col, 1.0f);
    dl->AddCircleFilled(ImVec2(rsx + rx_f*travel, rsy + ry_f*travel), dot_r, dot_col);

    ImGui::Dummy(ImVec2(DW, DH));
    ImGui::EndChild();

    ImGui::Text("Left Stick: X=%.2f  Y=%.2f    Right Stick: X=%.2f  Y=%.2f    Buttons: 0x%04X",
                lx_f, ly_f, rx_f, ry_f, pad0.buttons);
    if (g_ctrl_tex_analog) {
        ImGui::SameLine();
        ImGui::TextColored(g_ctrl_analog_on ? ImVec4(1.0f,0.2f,0.2f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f),
                           "  ANALOG %s (click to toggle)", g_ctrl_analog_on ? "ON" : "OFF");
    }
    ImGui::TextDisabled("SDL GameController (DualShock 2 via DS4Windows/Steam Input or direct BT). Falls back to EE RAM scan.");
}
