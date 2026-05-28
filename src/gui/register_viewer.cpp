#include "gui/register_viewer.hpp"
#include "core/jlink_port.hpp"
#include "imgui.h"
#include <cstdio>

const RegisterViewer::RegDesc RegisterViewer::kRegs[] = {
    {"R0", 0},        {"R1", 1},        {"R2", 2},    {"R3", 3},   {"R4", 4},   {"R5", 5},   {"R6", 6},
    {"R7", 7},        {"R8", 8},        {"R9", 9},    {"R10", 10}, {"R11", 11}, {"R12", 12}, {"SP (R13)", 13},
    {"LR (R14)", 14}, {"PC (R15)", 15}, {"xPSR", 16}, {"MSP", 17}, {"PSP", 18},
};

void
RegisterViewer::draw()
{
        if (!show_)
                return;

        if (ImGui::Begin("Registers", &show_)) {
                bool connected = JLinkPort::instance().isConnected();
                if (!connected) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "J-Link is not connected.");
                        ImGui::End();
                        return;
                }

                bool isHalted = JLinkPort::instance().isHalted();
                ImGui::Text("Core Status: ");
                ImGui::SameLine();
                if (isHalted) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "HALTED");
                } else {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "RUNNING");
                }

                if (ImGui::Button("Halt")) {
                        JLinkPort::instance().halt();
                }
                ImGui::SameLine();
                if (ImGui::Button("Resume")) {
                        JLinkPort::instance().resume();
                }

                ImGui::Separator();

                if (!isHalted) {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                                           "Registers may not update accurately while running.");
                }

                if (ImGui::BeginTable("RegTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Register");
                        ImGui::TableSetupColumn("Value (Hex)");
                        ImGui::TableHeadersRow();

                        for (const auto &reg : kRegs) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("%s", reg.name);
                                ImGui::TableNextColumn();
                                u32 val = JLinkPort::instance().readReg(reg.jlinkIndex);
                                ImGui::Text("0x%08X", val);
                        }

                        ImGui::EndTable();
                }
        }
        ImGui::End();
}
