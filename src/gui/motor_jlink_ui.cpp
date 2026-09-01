#include "core/jlink_port.hpp"
#include "gui/i18n.hpp"
#include "gui/motor_control.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <set>

void
MotorControl::refreshJlinkSymbols(bool applyPreset)
{
        jlinkSymbols_ = onJlinkSymbols ? onJlinkSymbols() : std::vector<motor_jlink::Symbol>{};
        if (jlinkSource_.empty()) {
                const auto it = std::find_if(jlinkSymbols_.begin(), jlinkSymbols_.end(), [](const auto &s) {
                        return s.name == motor_jlink::preset(motor_jlink::Mode);
                });
                if (it != jlinkSymbols_.end())
                        jlinkSource_ = it->source;
        }
        if (applyPreset) {
                for (int i = 0; i < motor_jlink::Count; ++i) {
                        auto &s  = jlinkConfig_.fields[i];
                        s        = {};
                        s.source = jlinkSource_;
                        s.name   = motor_jlink::preset(motor_jlink::Field(i));
                }
                modified_ = true;
        }
        resolveJlinkConfig();
}

bool
MotorControl::resolveJlinkConfig()
{
        // Resolve names again for every connection. Saved addresses are never trusted
        jlinkMappingError_.clear();
        for (int i = 0; i < motor_jlink::Count; ++i) {
                auto &s   = jlinkConfig_.fields[i];
                s.address = 0;
                s.size    = 0;
                s.source  = jlinkSource_;
                if (s.name.empty() && motor_jlink::optional(motor_jlink::Field(i)))
                        continue;
                const motor_jlink::Symbol *found = nullptr;
                for (const auto &candidate : jlinkSymbols_)
                        if (candidate.source == jlinkSource_ && candidate.name == s.name) {
                                if (found && (found->address != candidate.address || found->size != candidate.size)) {
                                        jlinkMappingError_ = "同名变量地址不唯一：" + s.name;
                                        return false;
                                }
                                found = &candidate;
                        }
                if (found)
                        s = *found;
                else if (!s.name.empty() && !motor_jlink::optional(motor_jlink::Field(i))) {
                        jlinkMappingError_ = "ELF 中找不到变量：" + s.name;
                        return false;
                }
        }
        try {
                motor_jlink::validate(jlinkConfig_);
        } catch (const std::exception &e) {
                jlinkMappingError_ = e.what();
                return false;
        }
        return true;
}

void
MotorControl::jlinkSettings()
{
        ImGui::TextWrapped("%s",
                           tr("Uses the top-bar J-Link connection and the CM7 ELF from Variable Manager.",
                              "使用顶部 J-Link 连接及变量管理器中加载的 CM7 ELF/AXF。"));
        ImGui::TextDisabled("%s",
                            JLinkPort::instance().isConnected() ? tr("Probe connected", "探针已连接")
                                                                : tr("Connect J-Link first", "请先连接 J-Link"));
        if (ImGui::Button(tr("Refresh variables", "刷新变量")))
                refreshJlinkSymbols(jlinkConfig_.fields[motor_jlink::Mode].name.empty());
        ImGui::SameLine();
        if (ImGui::Button(tr("FocDriver preset", "FocDriver 预设")))
                refreshJlinkSymbols(true);
        ImGui::TextUnformatted(tr("ELF source", "ELF 来源"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##jlinkSource",
                              jlinkSource_.empty() ? tr("Load CM7 ELF in Variable Manager", "在变量管理器加载 CM7 ELF")
                                                   : jlinkSource_.c_str())) {
                std::set<std::string> sources;
                for (const auto &s : jlinkSymbols_)
                        sources.insert(s.source);
                for (const auto &source : sources)
                        if (ImGui::Selectable(source.c_str(), source == jlinkSource_)) {
                                jlinkSource_ = source;
                                resolveJlinkConfig();
                                modified_ = true;
                        }
                ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", jlinkSource_.c_str());
        auto field = [&](int i) {
                const auto key     = motor_jlink::Field(i);
                auto      &binding = jlinkConfig_.fields[i];
                ImGui::PushID(i);
                ImGui::TextUnformatted(motor_jlink::label(key));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##symbol", binding.name.empty() ? tr("Not mapped", "未映射") : binding.name.c_str())) {
                        if (ImGui::IsWindowAppearing())
                                jlinkFilter_[0] = 0;
                        ImGui::InputText(
                            tr("Filter", "筛选"), jlinkFilter_, sizeof(jlinkFilter_), ImGuiInputTextFlags_AutoSelectAll);
                        if (motor_jlink::optional(key) && ImGui::Selectable(tr("Not mapped", "未映射"), binding.name.empty())) {
                                binding   = {};
                                modified_ = true;
                                resolveJlinkConfig();
                        }
                        for (const auto &symbol : jlinkSymbols_) {
                                if (symbol.source != jlinkSource_ ||
                                    (*jlinkFilter_ && symbol.name.find(jlinkFilter_) == std::string::npos))
                                        continue;
                                if (ImGui::Selectable(symbol.name.c_str(), symbol.name == binding.name)) {
                                        binding   = symbol;
                                        modified_ = true;
                                        resolveJlinkConfig();
                                }
                        }
                        ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n0x%08X  %u bytes", binding.name.c_str(), binding.address, binding.size);
                ImGui::PopID();
        };
        for (int i = 0; i <= motor_jlink::Special; ++i)
                field(i);
        if (ImGui::TreeNode(tr("Targets, feedback and ownership", "目标、反馈与控制权映射"))) {
                for (int i = motor_jlink::TargetPosition; i < motor_jlink::Count; ++i)
                        field(i);
                ImGui::TreePop();
        }
        if (!jlinkMappingError_.empty())
                ui::ErrorTextWrapped("%s", jlinkMappingError_.c_str());
        ImGui::TextWrapped(
            "%s",
            tr("FocDriver CiA402 protocol: modes use signed CiA402 codes, state uses the 16-bit control/status words.",
               "采用 FocDriver CiA402 协议：模式为有符号 CiA402 编码，状态为控制字/状态字。不要选择 g_foc.lo.mode 或 "
               "state。"));
        ImGui::TextWrapped("%s",
                           tr("Connect only while disabled. SHM input is turned off and remains off after disconnect; reset "
                              "firmware to restore it.",
                              "请先失能。连接将关闭共享内存指令输入，断开后保持关闭；复位固件可恢复。"));
        if (jlinkConfig_.fields[motor_jlink::DriveHeartbeat].address)
                ImGui::TextWrapped(
                    "%s",
                    tr("Target heartbeat is mapped; feedback polling advances it. Timeout shutdown is enforced by firmware.",
                       "已映射驱动保活计数，反馈采样会更新计数；超时关断由固件执行。"));
        else
                ImGui::TextWrapped(
                    "%s",
                    tr("20 ms host sampling; no hard real-time interpolation or target-side watchdog. USB loss can "
                       "prevent a disable command.",
                       "主机约 20 ms 采样，未映射目标端保活；USB 中断时可能无法发送失能。"));
}
