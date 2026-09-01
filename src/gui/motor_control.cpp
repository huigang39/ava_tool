#include "gui/motor_control.hpp"
#include "cJSON.h"
#include "core/motor_faults.hpp"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/fault_decoder.hpp"
#include "gui/gui.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "implot.h"
#include "platform/elevation.hpp"
#include "platform/native_dlg.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

bool
MotorControl::npcapAvailable()
{
#ifdef _WIN32
        wchar_t system[MAX_PATH];
        auto    n = GetSystemDirectoryW(system, MAX_PATH);
        if (!n || n >= MAX_PATH)
                return false;
        auto path   = std::wstring(system) + L"\\Npcap\\wpcap.dll";
        auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
                return false;
        bool ok = GetProcAddress(module, "pcap_open") && GetProcAddress(module, "pcap_findalldevs");
        FreeLibrary(module);
        return ok;
#else
        return false;
#endif
}

void
MotorControl::npcapDialog()
{
        if (npcapPrompt_) {
                ImGui::OpenPopup("Npcap installation###npcapInstall");
                npcapPrompt_ = false;
        }
        if (ImGui::BeginPopupModal(tr("Npcap installation###npcapInstall", "安装 Npcap###npcapInstall"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    tr("Npcap is not installed or cannot be loaded.", "未安装 Npcap，或当前 Npcap 运行库无法加载。"));
                ImGui::TextUnformatted(tr("Install Npcap from the official website, then scan adapters again.",
                                          "请前往官网下载并安装 Npcap，安装后重新扫描网卡。"));
                if (ImGui::Button(tr("Open official download page", "打开官网下载页"))) {
#ifdef _WIN32
                        ShellExecuteW(nullptr, L"open", L"https://npcap.com/#download", nullptr, nullptr, SW_SHOWNORMAL);
#endif
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Installed, check again", "已安装，重新检测")) && npcapAvailable())
                        ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button(tr("Close", "关闭")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
}

namespace
{
const ImVec4  accent(0.26f, 0.72f, 0.95f, 1), green(0.32f, 0.85f, 0.66f, 1), muted(0.57f, 0.65f, 0.74f, 1);
constexpr int modes[] = {1, -1, 3, -3, 4, 10, 6, 8, -8, 9, -9, -10, -128, -126, -125, -124};
const char *
specialLabel(unsigned val)
{
        const char *labels[] = {tr("0  None", "0  无（恢复控制字）"),
                                tr("1  Disable", "1  失能"),
                                tr("2  Enable", "2  使能"),
                                tr("3  Calibrate", "3  校准"),
                                tr("4  Sensorless", "4  无感"),
                                tr("5  Output position zero", "5  出轴角度传感器标零"),
                                tr("6  Clear faults", "6  清错"),
                                tr("7  Torque sensor zero", "7  扭矩传感器标零")};
        return val < 8 ? labels[val] : tr("Unknown", "未知");
}
bool
specialAllowed(const MotorController::State &s, unsigned axis, unsigned val)
{
        if (!s.connected || axis >= s.snapshot.count || val > 7 || !(s.info.capabilities & AVA_ETH_CAP_SPECIAL_CMD))
                return false;
        const auto &f = s.snapshot.axes[axis];
        if ((f.flags & (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) != 3)
                return false;
        if (val <= 1)
                return true;
        if ((f.flags & AVA_ETH_ENABLED) || f.special_cmd == AVA_ETH_SPECIAL_CALI)
                return false;
        return val == AVA_ETH_SPECIAL_CLEAR_FAULT || !(f.flags & AVA_ETH_FAULT);
}
const char *modeNames[] = {
    "PP", "PPC", "PV", "PVC", "PT", "CST", "HM", "CSP", "CSPC", "CSV", "CSVC", "CSC", "PD", "I/F", "V/F", "ASC"};
const char *
modeLabel(int index)
{
        const char              *descriptions[] = {tr("Profile position / torque loop", "轮廓位置 / 力矩环"),
                                                   tr("Profile position / current loop", "轮廓位置 / 电流环"),
                                                   tr("Profile velocity / torque loop", "轮廓速度 / 力矩环"),
                                                   tr("Profile velocity / current loop", "轮廓速度 / 电流环"),
                                                   tr("Profile torque", "轮廓力矩"),
                                                   tr("Synchronous torque", "同步力矩"),
                                                   tr("Homing", "回零"),
                                                   tr("Synchronous position / torque loop", "同步位置 / 力矩环"),
                                                   tr("Synchronous position / current loop", "同步位置 / 电流环"),
                                                   tr("Synchronous velocity / torque loop", "同步速度 / 力矩环"),
                                                   tr("Synchronous velocity / current loop", "同步速度 / 电流环"),
                                                   tr("Current", "电流"),
                                                   tr("Position / velocity PD", "位置 / 速度 PD"),
                                                   "I/F",
                                                   "V/F",
                                                   "ASC"};
        static thread_local char text[128];
        snprintf(text, sizeof(text), "%d  %s  %s", modes[index], modeNames[index], descriptions[index]);
        return text;
}
bool
valid(const AvaEthFeedback &f)
{
        return (f.flags & (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) == 3u;
}
} // namespace

void
MotorControl::findLibrary()
{
        if (useBuiltin_ || initialized_)
                return;
        initialized_ = true;
        if (library_[0])
                return;
        const auto appDir = Gui::getAppDir();
        const auto app    = std::filesystem::path(std::u8string(appDir.begin(), appDir.end()));
#ifdef _WIN32
        const char *name = "ava_eth_sdk.dll", *platform = "win";
#else
        const char *name = "libava_eth_sdk.so", *platform = "linux";
#endif
        const std::filesystem::path candidates[] = {app / name,
                                                    app / "sdk" / "ava_eth" / name,
                                                    app / ".." / ".." / "sdk" / "ava_eth" / "bin" / platform / name,
                                                    app / ".." / ".." / ".." / "ava_eth" / "build" / platform / name};
        for (const auto &path : candidates) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) {
                        const auto        utf8 = std::filesystem::absolute(path).lexically_normal().u8string();
                        const std::string text(utf8.begin(), utf8.end());
                        snprintf(library_, sizeof(library_), "%s", text.c_str());
                        break;
                }
        }
}

void
MotorControl::disconnect(const char *reason)
{
        dialDragging_ = dialPending_ = false;
        const auto state             = controller_.state();
        if (!state.connected && !state.busy)
                return;
        MotorController::Command request;
        request.action = MotorController::Action::Disconnect;
        request.reason = reason;
        controller_.post(std::move(request));
}

void
MotorControl::setOpen(bool open)
{
        if (open && !open_ && backend_ == 3 && !npcapAvailable())
                npcapPrompt_ = true;
        if (!open)
                cancelConfirmation();
        if (open_ && !open)
                disconnect("Page closed / 关闭页面");
        if (open_ != open)
                modified_ = true;
        open_ = open;
}

MotorController::Config
MotorControl::config() const
{
        return {useBuiltin_ ? std::string{} : std::string(library_),
                device_,
                static_cast<uint32_t>(backend_ + 1),
                1u,
                static_cast<uint32_t>(period_),
                static_cast<uint32_t>(watchdog_),
                relaxed_,
                jlinkConfig_};
}

void
MotorControl::command(MotorController::Action action)
{
        if (backend_ == 3 && (action == MotorController::Action::Connect || action == MotorController::Action::Scan) &&
            !npcapAvailable()) {
                npcapPrompt_ = true;
                return;
        }
        if (backend_ == 4 && action == MotorController::Action::Connect) {
                refreshJlinkSymbols(jlinkConfig_.fields[motor_jlink::Mode].name.empty());
                if (!resolveJlinkConfig())
                        return;
        }
        MotorController::Command c;
        c.action = action;
        if (action == MotorController::Action::Stop || action == MotorController::Action::Disable ||
            action == MotorController::Action::Disconnect)
                dialDragging_ = dialPending_ = false;
        if (action == MotorController::Action::Disconnect)
                c.reason = "Disconnect button / 点击断开按钮";
        c.config           = config();
        c.axis             = static_cast<uint32_t>(axis_);
        c.target           = {c.axis, modes[mode_], position_, velocity_, current_, torque_};
        c.interpolate      = interpolation_ && (c.target.mode == 8 || c.target.mode == -8);
        c.max_velocity     = maxPositionVelocity_;
        c.max_acceleration = maxPositionAcceleration_;
        controller_.post(std::move(c));
}

void
MotorControl::connection(const MotorController::State &s)
{
        const auto &f      = s.snapshot.axes[axis_];
        const char *status = !s.connected                ? tr("DISCONNECTED", "未连接")
                             : f.flags & AVA_ETH_FAULT   ? tr("FAULT", "故障")
                             : !valid(f) && s.error      ? tr("COMMUNICATION ERROR", "通信异常")
                             : !valid(f)                 ? tr("WAITING", "等待反馈")
                             : f.flags & AVA_ETH_ENABLED ? tr("ENABLED", "已使能")
                                                         : tr("DISABLED", "未使能");
        ImGui::TextColored((f.flags & AVA_ETH_FAULT) || (!valid(f) && s.error) ? ui::ErrorTextColor()
                           : s.connected && valid(f)                           ? green
                                                                               : muted,
                           "● %s",
                           status);
#ifdef _WIN32
        ImGui::SameLine();
        static const bool admin = elevation::isAdministrator();
        ImGui::TextColored(admin ? green : ImVec4(1, 0.65f, 0.3f, 1),
                           "%s",
                           admin ? tr("Administrator", "管理员") : tr("Not elevated", "非管理员"));
#endif
        ImGui::BeginDisabled(s.connected || s.busy);
        if (ImGui::BeginTable("connectionSettings", 1, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("device", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableNextColumn();
                const char *backends[] = {
                    tr("EtherCAT  kernel PDO", "EtherCAT  内核 PDO"), tr("EtherCAT  Npcap", "EtherCAT  Npcap"), "J-Link"};
                int selection = std::clamp(backend_ - 2, 0, 2);
                ImGui::TextDisabled("%s", tr("Backend", "通信后端"));
                ImGui::SetNextItemWidth(-1);
                if (ui::Choice("##backend", &selection, backends, 3)) {
                        backend_   = selection + 2;
                        modified_  = true;
                        device_[0] = 0;
                        deviceName_.clear();
                        if (backend_ == 4)
                                refreshJlinkSymbols(jlinkConfig_.fields[motor_jlink::Mode].name.empty());
                        if (backend_ == 3 && !npcapAvailable())
                                npcapPrompt_ = true;
                }
                if (backend_ == 4) {
                        jlinkSettings();
                } else {
                        if (ImGui::TreeNode(tr("Advanced runtime settings", "高级运行库设置"))) {
                                if (ImGui::Checkbox(tr("Use bundled runtime", "使用内置运行库"), &useBuiltin_)) {
                                        modified_ = true;
                                        findLibrary();
                                }
                                if (!useBuiltin_) {
                                        ImGui::TextDisabled("%s", tr("SDK library", "SDK 动态库"));
                                        ImGui::SetNextItemWidth(-52);
                                        if (ImGui::InputText("##library", library_, sizeof(library_)))
                                                modified_ = true;
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", library_);
                                        ImGui::SameLine();
                                        if (ImGui::Button("...##dll", ImVec2(40, 0))) {
                                                auto path = nativeDlgOpen("ava_eth SDK", {{"SDK", {"dll", "so", "dylib"}}});
                                                if (!path.empty()) {
                                                        snprintf(library_, sizeof(library_), "%s", path.c_str());
                                                        modified_ = true;
                                                }
                                        }
                                }
                                ImGui::TreePop();
                        }
                        {
                                ImGui::TextDisabled(
                                    "%s",
                                    backend_ == 3 ? tr("Network adapter (select explicitly)", "网卡（请选择连接电机的网卡）")
                                                  : tr("Control device (blank = first)", "控制设备（留空选择首个）"));
                                for (const auto &item : s.devices)
                                        if (item.path == device_ && deviceName_ != item.name) {
                                                deviceName_ = item.name;
                                                modified_   = true;
                                        }
                                ImGui::SetNextItemWidth(-1);
                                const char *preview = deviceName_.empty()
                                                          ? (*device_ ? device_ : tr("Select device", "选择设备"))
                                                          : deviceName_.c_str();
                                if (ImGui::BeginCombo("##devices", preview)) {
                                        for (const auto &item : s.devices) {
                                                if ((item.path.find("NPF_") != std::string::npos) != (backend_ == 3))
                                                        continue;
                                                ImGui::PushID(item.path.c_str());
                                                if (ImGui::Selectable(item.name.c_str(), item.path == device_)) {
                                                        snprintf(device_, sizeof(device_), "%s", item.path.c_str());
                                                        deviceName_ = item.name;
                                                        modified_   = true;
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip(
                                                            "%s\n%s", item.description.c_str(), item.path.c_str());
                                                ImGui::PopID();
                                        }
                                        ImGui::EndCombo();
                                }
                                if (ImGui::IsItemHovered() && *device_)
                                        ImGui::SetTooltip("%s", device_);
                                if (ui::SmallButton(backend_ == 3 ? tr("Scan adapters", "扫描网卡")
                                                                  : tr("Scan devices", "扫描设备"),
                                                    ui::BtnStyle::Primary))
                                        command(MotorController::Action::Scan);
                                if (ImGui::TreeNode(tr("Device path", "设备路径"))) {
                                        ImGui::SetNextItemWidth(-1);
                                        if (ImGui::InputText("##device", device_, sizeof(device_))) {
                                                deviceName_.clear();
                                                modified_ = true;
                                        }
                                        ImGui::TreePop();
                                }
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", tr("Timing", "通信参数"));
                        ImGui::SetNextItemWidth(105);
                        if (ImGui::InputInt(tr("Period (us)", "周期 (us)"), &period_, 100, 1000)) {
                                period_   = std::clamp(period_, 1000, 10000);
                                modified_ = true;
                        }
                        ImGui::SetNextItemWidth(105);
                        if (ImGui::InputInt(tr("Watchdog (ms)", "看门狗 (ms)"), &watchdog_, 50, 100)) {
                                watchdog_ = std::clamp(watchdog_, 250, 1000);
                                modified_ = true;
                        }
                        if (ImGui::Checkbox(tr("20 ms receive budget", "20 ms 接收预算"), &relaxed_))
                                modified_ = true;
                        ImGui::TextDisabled("%s", tr("Hardware SDK: one actuator", "实机 SDK：当前支持一个执行器"));
                }
                ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(s.busy || (backend_ != 4 && !useBuiltin_ && !library_[0]) ||
                             (!s.connected && backend_ == 3 && !device_[0]));
        if (!s.connected && ui::Button(tr("Connect (disabled)", "连接（不使能）"), ui::BtnStyle::Success, ImVec2(170, 30)))
                command(MotorController::Action::Connect);
        if (s.connected && ui::Button(tr("Disconnect", "断开连接"), ui::BtnStyle::Danger, ImVec2(170, 30)))
                command(MotorController::Action::Disconnect);
        ImGui::EndDisabled();
        // Keep changing message lengths from moving the interactive gauges below
        if (ImGui::BeginChild("connectionMessage", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 3))) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(s.error ? ui::ErrorTextColor() : muted, "%s%s", s.busy ? "… " : "", s.message.c_str());
                ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::PushTextWrapPos(0);
#ifdef _WIN32
        if (backend_ != 4 && s.elevation_required && !elevation::isAdministrator() &&
            ImGui::Button(tr("Restart as administrator...", "以管理员身份重启…")))
                elevationRequested_ = true;
#endif
        ImGui::TextDisabled(
            "%s",
            backend_ == 4 ? "J-Link  FocDriver CiA402"
            : backend_ == 3
                ? tr("Npcap user-space  select the wired EtherCAT adapter", "Npcap 用户态  选择连接 EtherCAT 的有线网卡")
                : tr("Requires the ava_eth controller and network drivers", "需已安装 ava_eth 控制设备及网络驱动"));
        ImGui::PopTextWrapPos();
}

void
MotorControl::controls(const MotorController::State &s)
{
        const auto f       = s.snapshot.axes[axis_];
        const bool enabled = valid(f) && ((f.flags & AVA_ETH_ENABLED) || f.special_cmd == AVA_ETH_SPECIAL_CALI);
        const bool ready = s.connected && axis_ < static_cast<int>(s.snapshot.count) && valid(f) && !(f.flags & AVA_ETH_FAULT);
        ImGui::SetNextItemWidth(-1);
        const auto axisText = std::string(tr("Actuator ", "执行器 ")) + std::to_string(axis_);
        if (ImGui::BeginCombo("##axis", axisText.c_str())) {
                const int count = s.connected ? s.info.axes : 1;
                for (int i = 0; i < count; ++i) {
                        const auto label = std::string(tr("Actuator ", "执行器 ")) + std::to_string(i);
                        if (ImGui::Selectable(label.c_str(), axis_ == i)) {
                                dialDragging_ = dialPending_ = false;
                                axis_                        = i;
                                controller_.pulse(i);
                                const auto &t  = s.targets[i];
                                const auto  it = std::find(std::begin(modes), std::end(modes), t.mode);
                                mode_          = it == std::end(modes) ? 7 : static_cast<int>(it - modes);
                                setTargetEditor(t);
                        }
                }
                ImGui::EndCombo();
        }
        ImGui::TextUnformatted(tr("Control mode", "控制模式"));
        if (valid(f)) {
                const auto it = std::find(std::begin(modes), std::end(modes), f.mode);
                if (it != std::end(modes))
                        ImGui::TextWrapped("%s: %s", tr("Feedback mode", "实际反馈模式"), modeLabel(int(it - modes)));
                else
                        ImGui::Text("%s: %d", tr("Feedback mode", "实际反馈模式"), f.mode);
        }
        ImGui::BeginDisabled(s.busy || !ready || f.special_cmd != 0);
        ImGui::SetNextItemWidth(-1);
        std::array<std::string, 16> controlModeLabels;
        const char                 *controlModes[16];
        for (int i = 0; i < 16; ++i) {
                // modeLabel formats into one scratch buffer; keep each label alive.
                controlModeLabels[i] = modeLabel(i);
                controlModes[i]      = controlModeLabels[i].c_str();
        }
        int selectedMode = mode_;
        // Reselecting the current mode still requests confirmation, as before.
        if (ui::EnumCombo("##mode", &selectedMode, controlModes, 16, nullptr, true))
                requestConfirmation(MotorController::Action::SetMode, selectedMode);
        ImGui::EndDisabled();
        ImGui::TextUnformatted(tr("Special command  special_cmd", "特殊指令  special_cmd"));
        ImGui::BeginDisabled(s.busy || !specialAllowed(s, axis_, 0));
        ImGui::SetNextItemWidth(-1);
        const char *specialCommands[8];
        bool        allowedCommands[8];
        for (unsigned i = 0; i < 8; ++i) {
                specialCommands[i] = specialLabel(i);
                allowedCommands[i] = specialAllowed(s, axis_, i);
        }
        int selectedCommand = static_cast<int>(f.special_cmd);
        if (ui::EnumCombo("##special_cmd", &selectedCommand, specialCommands, 8, allowedCommands))
                requestConfirmation(MotorController::Action::Special, selectedCommand);
        ImGui::EndDisabled();
        if (s.connected && !(s.info.capabilities & AVA_ETH_CAP_SPECIAL_CMD))
                ImGui::TextWrapped("%s",
                                   tr("This backend/driver does not support special_cmd; use the bundled Npcap backend.",
                                      "当前后端／驱动不支持特殊指令，可使用内置 Npcap 后端。"));
        else if (f.special_cmd)
                ImGui::TextWrapped("%s",
                                   tr("Select None before repeating a one-shot command or returning to normal control.",
                                      "重复执行一次性指令或恢复普通控制前，请先选择“无”。"));
        const int mode = modes[mode_];
        ImGui::TextDisabled("%s", tr("Press Enter to confirm and send a target", "输入指令后按回车确认发送"));
        if (ImGui::BeginTable("commands", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (int i = 0; i < 4; ++i) {
                        ImGui::TableNextColumn();
                        ImGui::PushID(i);
                        targetField(s, i);
                        ImGui::PopID();
                }
                ImGui::EndTable();
        }
        const bool finite =
            std::isfinite(position_) && std::isfinite(velocity_) && std::isfinite(current_) && std::isfinite(torque_);
        ImGui::BeginDisabled(s.busy || (enabled ? !s.connected : (!ready || !finite || f.special_cmd != 0)));
        if (ui::Button(enabled ? tr("Disable", "失能") : tr("Enable", "使能"),
                       enabled ? ui::BtnStyle::Danger : ui::BtnStyle::Success,
                       ImVec2(170, 30))) {
                if (enabled)
                        command(MotorController::Action::Disable);
                else
                        requestConfirmation(MotorController::Action::Enable);
        }
        ImGui::EndDisabled();
        if (mode == 6 || mode <= -124)
                ImGui::TextWrapped("%s",
                                   tr("Homing trigger and special-mode parameters are not exposed by this SDK.",
                                      "当前 SDK 尚未提供回零触发及特殊模式的专用参数。"));
}

void
MotorControl::faultDetails(const MotorController::State &s)
{
        const auto &feedback = s.snapshot.axes[axis_];
        // Diagnostics currently describe the single hardware axis, never a simulator axis
        const bool available = s.diagnostics_valid && axis_ == 0;
        const auto entries   = available ? motor_faults::decode(s.diagnostics.error_pages) : std::vector<motor_faults::Entry>{};
        const bool active    = feedback.error_code || (feedback.flags & AVA_ETH_FAULT) || !entries.empty();
        if (active && !faultDetailsActive_)
                ImGui::SetNextItemOpen(true);
        faultDetailsActive_ = active;
        if (!ImGui::CollapsingHeader(tr("Fault details###motorFaults", "故障与警告详情###motorFaults"),
                                     active ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))
                return;
        if (feedback.error_code)
                ImGui::TextWrapped("%s: 0x%08X — %s",
                                   tr("Controller", "控制器"),
                                   feedback.error_code,
                                   motor_faults::controllerReason(feedback.error_code));
        if (!available) {
                ImGui::TextWrapped("%s",
                                   tr("Firmware pages unavailable; the controller code cannot identify a FocDriver fault bit.",
                                      "暂无固件错误页；不能仅凭控制器错误码判断 FocDriver 的具体故障位。"));
                return;
        }
        if (!valid(feedback))
                ImGui::TextWrapped("%s",
                                   tr("Feedback is invalid; these are the last received firmware pages.",
                                      "反馈已失效，以下为最后一次接收到的固件错误页。"));
        ImGui::TextDisabled("%s: %s", tr("Fault catalog", "故障目录"), motor_faults::catalogId);
        ImGui::TextWrapped("%s",
                           tr("Click a description to inspect a snapshot and troubleshooting details.",
                              "点击故障条目可打开错误码快照，查看位图、排查建议与清除条件。"));
        if (entries.empty()) {
                ImGui::TextWrapped("%s", tr("All firmware error pages are zero.", "固件错误页全部为 0。"));
                return;
        }
        if (ImGui::BeginTable(
                "firmwareFaults", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn(tr("Type", "类别"), ImGuiTableColumnFlags_WidthFixed, 62);
                ImGui::TableSetupColumn(tr("Source", "来源"), ImGuiTableColumnFlags_WidthFixed, 165);
                ImGui::TableSetupColumn(tr("Description", "中文说明"));
                ImGui::TableHeadersRow();
                for (const auto &entry : entries) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(entry.kind == motor_faults::Kind::Warning       ? tr("Warning", "警告")
                                               : entry.kind == motor_faults::Kind::Calibration ? tr("Calibration", "校准")
                                               : entry.kind == motor_faults::Kind::Unknown     ? tr("Unknown", "未定义")
                                                                                               : tr("Fault", "故障"));
                        ImGui::TableNextColumn();
                        ImGui::Text("Error%u  bit%u", entry.page + 1, entry.bit);
                        ImGui::Text("0x%04X: 0x%08X", 0x2030 + entry.page * 4, s.diagnostics.error_pages[entry.page]);
                        ImGui::TableNextColumn();
                        ImGui::PushID(int(entry.page * 32 + entry.bit));
                        if (ImGui::Selectable(entry.zh))
                                faultDecoder().inspect(entry.page, s.diagnostics.error_pages[entry.page], entry.bit);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s\n%s", entry.symbol, entry.detail ? entry.detail->cause : "");
                        ImGui::PopID();
                }
                ImGui::EndTable();
        }
}

void
MotorControl::diagnostics(const MotorController::State &s)
{
        if (ImGui::BeginTabBar("details")) {
                if (ImGui::BeginTabItem(tr("Events", "操作记录"))) {
                        std::string events;
                        for (auto it = s.events.rbegin(); it != s.events.rend(); ++it)
                                events += *it + "\n";
                        ui::CopyLogButton(events);
                        ui::LogView("events", events, ImVec2(0, 130));
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Communication", "通信诊断"))) {
                        if (!s.diagnostics_valid)
                                ImGui::TextDisabled("%s", tr("Hardware diagnostics unavailable", "暂无实机通信诊断"));
                        else {
                                const auto &d = s.diagnostics;
                                ImGui::Text("AL=0x%04X   AL code=0x%04X   Status=0x%04X   WKC=%u/%u",
                                            d.al_status,
                                            d.al_code,
                                            d.status_word,
                                            d.wkc,
                                            d.expected_wkc);
                                ImGui::Text("%s %llu / %llu   |   %s %llu   |   %s %llu",
                                            tr("TX / RX", "发送 / 接收"),
                                            (unsigned long long)d.sent,
                                            (unsigned long long)d.received,
                                            tr("Timeouts", "超时"),
                                            (unsigned long long)d.timeouts,
                                            tr("Missed periods", "错过周期"),
                                            (unsigned long long)d.missed_periods);
                                ImGui::Text("%s %.1f us   |   %s %u us   |   %s %llu",
                                            tr("Max RTT", "最大回包耗时"),
                                            d.max_rtt_ns / 1000.0,
                                            tr("Receive budget", "接收预算"),
                                            d.receive_timeout_us,
                                            tr("Probes", "辅助请求"),
                                            (unsigned long long)d.receive_probes);
                                ImGui::Text("late=%llu   unmatched=%llu   bad_wkc=%llu   bad_al=%llu",
                                            (unsigned long long)d.late,
                                            (unsigned long long)d.unmatched,
                                            (unsigned long long)d.bad_wkc,
                                            (unsigned long long)d.bad_al);
                                if (ImGui::TreeNode(tr("Raw firmware error pages", "固件原始错误页"))) {
                                        for (int i = 0; i < 8; ++i)
                                                ImGui::Text("0x%04X: 0x%08X", 0x2030 + i * 4, d.error_pages[i]);
                                        ImGui::TreePop();
                                }
                        }
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
}

void
MotorControl::draw()
{
        if (!open_)
                return;
        if (backend_ != 4)
                findLibrary();
        controller_.pulse(axis_);
        auto s = controller_.state();
#ifdef _WIN32
        if (seenElevationRevision_ != s.elevation_revision) {
                seenElevationRevision_ = s.elevation_revision;
                if (backend_ != 4 && s.elevation_required && !elevation::isAdministrator())
                        elevationRequested_ = true;
        }
#endif
        if (s.connected && axis_ >= static_cast<int>(s.info.axes))
                axis_ = 0;
        if (seenModeRevisions_[axis_] != s.target_sync_revisions[axis_]) {
                if (s.backend == motor_jlink::backend) {
                        const auto it = std::find(std::begin(modes), std::end(modes), s.targets[axis_].mode);
                        if (it != std::end(modes))
                                mode_ = int(it - modes);
                }
                seenModeRevisions_[axis_] = s.target_sync_revisions[axis_];
                setTargetEditor(s.targets[axis_]);
        }
        ImGui::SetNextWindowSize(ImVec2(1580, 960), ImGuiCond_FirstUseEver);
        const bool visible = dock_close::begin(tr("Motor Control###MotorControl", "电机控制###MotorControl"), &open_);
        if (!open_) {
                cancelConfirmation();
                disconnect("Page closed / 关闭页面");
                modified_ = true;
        }
        if (visible) {
                if (ImGui::BeginTable("motorWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("monitor", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                        ImGui::TableSetupColumn("dashboard", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                        ImGui::TableNextColumn();
                        ImGui::BeginChild("variableMonitor", ImVec2(0, -30));
                        variables(s);
                        ImGui::EndChild();
                        ImGui::TableNextColumn();
                        ImGui::BeginChild("motorRight", ImVec2(0, -30));
                        if (ImGui::BeginTabBar("motorControlPages")) {
                                auto pageFlags = [&](int page) {
                                        return rightPageRequest_ == page ? ImGuiTabItemFlags_SetSelected
                                                                         : ImGuiTabItemFlags_None;
                                };
                                if (ImGui::BeginTabItem(tr("Control", "控制"), nullptr, pageFlags(0))) {
                                        controls(s);
                                        if (backend_ != 4 && ImGui::TreeNode(tr("Position interpolation", "位置插补设置"))) {
                                                interpolationSettings(s);
                                                ImGui::TreePop();
                                        }
                                        ImGui::EndTabItem();
                                }
                                if (ImGui::BeginTabItem(tr("Connection", "连接"), nullptr, pageFlags(1))) {
                                        connection(s);
                                        ImGui::EndTabItem();
                                }
                                if (ImGui::BeginTabItem(tr("Instruments", "仪表"), nullptr, pageFlags(2))) {
                                        dashboard(s);
                                        ImGui::EndTabItem();
                                }
                                if (ImGui::BeginTabItem(tr("Diagnostics", "诊断"), nullptr, pageFlags(3))) {
                                        faultDetails(s);
                                        pidParameters(s);
                                        if (ImGui::CollapsingHeader(tr("Log###motor_log", "日志###motor_log")))
                                                diagnostics(s);
                                        ImGui::EndTabItem();
                                }
                                ImGui::EndTabBar();
                        }
                        rightPageRequest_ = -1;
                        ImGui::EndChild();
                        ImGui::EndTable();
                }
                ImGui::Separator();
                ImGui::TextWrapped("%s",
                                   tr("Stop is a software request. A broken link cannot confirm mechanical standstill.",
                                      "停止为软件请求，链路断开时无法确认机械停机。"));
        }
        confirmation(s);
        npcapDialog();
        ImGui::End();
}

void
MotorControl::pidParameters(const MotorController::State &s)
{
        static const char *names[] = {
            "PD Kp", "PD Kd", "位置→转矩 Kp", "速度→转矩 Kp", "速度→转矩 Ki", "位置→电流 Kp", "速度→电流 Kp", "速度→电流 Ki"};
        static const unsigned indices[] = {0x2400, 0x2402, 0x2404, 0x2408, 0x240C, 0x2410, 0x2414, 0x2418};
        const auto            cfg       = config();
        const auto            matches   = [&](const MotorController::Config &other, uint32_t axis) {
                return cfg.backend == other.backend && cfg.device == other.device && cfg.library == other.library &&
                       axis == static_cast<uint32_t>(axis_);
        };
        const auto context = cfg.device + "\n" + cfg.library + "\n" + std::to_string(cfg.backend) + ":" + std::to_string(axis_);
        if (pidDraftContext_ != context) {
                pidDraftContext_ = context;
                pidSelection_    = 0;
                pidDraft_        = {};
                seenPidRevision_ = 0;
                pendingPid_.reset();
        }
        const bool available = s.connected && !s.busy && !s.pidPending && cfg.backend == AVA_ETH_NPCAP && axis_ == 0 &&
                               s.snapshot.count > 0 && (s.snapshot.axes[0].flags & AVA_ETH_FEEDBACK_VALID) &&
                               !(s.snapshot.axes[0].flags & AVA_ETH_FAULT);
        const bool sentHere = s.pidSentMask && matches(s.pidConfig, s.pidAxis);
        if (sentHere && s.pidRevision != seenPidRevision_) {
                for (int i = 0; i < 8; ++i)
                        if (s.pidSentMask & (1u << i))
                                pidDraft_.values[i] = s.pid.values[i];
                pidSelection_    = 0; // Explicit selection for every write; never replay on connection
                seenPidRevision_ = s.pidRevision;
        }
        if (ImGui::CollapsingHeader(tr("PD / PID parameters###motorPid", "PD / PID 参数###motorPid"))) {
                ImGui::TextWrapped(
                    "%s",
                    tr("While connected through Npcap, select gains to update live. One actuator per network; runtime only, "
                       "not "
                       "saved to Flash.",
                       "Npcap 连接后可实时修改，保持当前使能状态及运动指令。仅支持单执行器网段；不保存到 Flash。"));
                if (s.pidPending)
                        ImGui::TextUnformatted(tr("Updating PID parameters...", "正在在线更新 PID 参数…"));
                ImGui::TextWrapped("%s",
                                   tr("The current firmware supports PID RxPDO writes, but no gain readback. Last sent values "
                                      "are not device measurements.",
                                      "现有固件支持 PID 的 RxPDO 写入，不支持参数回读。“上次发送”不代表设备当前值。"));
                bool validValues = true;
                ImGui::BeginDisabled(!available);
                if (ImGui::BeginTable("pidValues", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn(tr("Write", "写入"), ImGuiTableColumnFlags_WidthFixed, 42);
                        ImGui::TableSetupColumn(tr("Parameter", "参数"));
                        ImGui::TableSetupColumn("OD", ImGuiTableColumnFlags_WidthFixed, 85);
                        ImGui::TableSetupColumn(tr("Set value", "设置值"));
                        ImGui::TableSetupColumn(tr("Last sent", "上次发送"));
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 8; ++i) {
                                ImGui::PushID(i);
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                bool selected = (pidSelection_ & (1u << i)) != 0;
                                if (ImGui::Checkbox("##selectGain", &selected)) {
                                        if (selected)
                                                pidSelection_ |= 1u << i;
                                        else
                                                pidSelection_ &= ~(1u << i);
                                }
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(names[i]);
                                ImGui::TableNextColumn();
                                ImGui::Text("%04X:00", indices[i]);
                                ImGui::TableNextColumn();
                                ImGui::SetNextItemWidth(-1);
                                ImGui::BeginDisabled(!selected);
                                ImGui::InputFloat("##gain", &pidDraft_.values[i], 0, 0, "%.7g");
                                ImGui::EndDisabled();
                                const float val = pidDraft_.values[i];
                                if (selected)
                                        validValues &= std::isfinite(val) && val >= 0 && (i >= 2 || val <= 65504);
                                ImGui::TableNextColumn();
                                if (sentHere && (s.pidSentMask & (1u << i)))
                                        ImGui::Text("%.7g", s.pid.values[i]);
                                else
                                        ImGui::TextUnformatted("—");
                                ImGui::PopID();
                        }
                        ImGui::EndTable();
                }
                ImGui::TextDisabled("%s",
                                    tr("PD: float16 (max 65504); other gains: float32. Unselected gains are unchanged.",
                                       "PD：float16（最大 65504）；其余：float32。未勾选项不写入。"));
                ImGui::BeginDisabled(!validValues || !pidSelection_);
                if (ImGui::Button(tr("Write selected parameters###writePid", "写入所选参数###writePid"))) {
                        if (!npcapAvailable())
                                npcapPrompt_ = true;
                        else {
                                MotorController::Command c;
                                c.action    = MotorController::Action::WritePid;
                                c.config    = cfg;
                                c.axis      = axis_;
                                c.pid       = pidDraft_;
                                c.pidMask   = pidSelection_;
                                pendingPid_ = c;
                                ImGui::OpenPopup(tr("Confirm PID parameters###pidConfirm", "确认 PID 参数###pidConfirm"));
                        }
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!validValues)
                        ImGui::TextColored(
                            ImVec4(1, .45f, .4f, 1),
                            "%s",
                            tr("Use finite, nonnegative values. PD maximum: 65504.", "请输入有限非负数，PD 最大为 65504。"));
        }
        if (ImGui::BeginPopupModal(tr("Confirm PID parameters###pidConfirm", "确认 PID 参数###pidConfirm"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                if (pendingPid_) {
                        const auto &c = *pendingPid_;
                        ImGui::Text("%s %u", tr("Actuator", "执行器"), c.axis);
                        for (int i = 0; i < 8; ++i)
                                if (c.pidMask & (1u << i))
                                        ImGui::Text("%s = %.7g", names[i], c.pid.values[i]);
                        ImGui::TextUnformatted(tr("Update selected gains live; preserve enable state and motion commands.",
                                                  "在线更新所选参数，保持当前使能状态及运动指令。"));
                        ImGui::TextUnformatted(
                            tr("No device readback is available. Partial writes are possible on communication failure.",
                               "固件不支持参数回读；通信失败时可能已有部分参数写入。"));
                        ImGui::BeginDisabled(!available || !matches(c.config, c.axis));
                        if (ImGui::Button(tr("Confirm write", "确认写入"))) {
                                controller_.post(c);
                                pendingPid_.reset();
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                }
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        pendingPid_.reset();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
MotorControl::save(cJSON *root) const
{
        auto *o = cJSON_AddObjectToObject(root, "motorControl");
        cJSON_AddBoolToObject(o, "open", open_);
        cJSON_AddBoolToObject(o, "use_builtin", useBuiltin_);
        cJSON_AddStringToObject(o, "library", library_);
        cJSON_AddStringToObject(o, "device", device_);
        cJSON_AddStringToObject(o, "device_name", deviceName_.c_str());
        cJSON_AddNumberToObject(o, "position_unit", positionUnit_);
        cJSON_AddNumberToObject(o, "velocity_unit", velocityUnit_);
        cJSON_AddNumberToObject(o, "gauge_range_version", 2);
        auto *ranges = cJSON_AddArrayToObject(o, "gauge_ranges");
        for (const auto &range : gaugeRanges_) {
                auto *entry = cJSON_CreateObject();
                cJSON_AddNumberToObject(entry, "minimum", range.minimum);
                cJSON_AddNumberToObject(entry, "maximum", range.maximum);
                cJSON_AddItemToArray(ranges, entry);
        }
        cJSON_AddStringToObject(o, "jlink_source", jlinkSource_.c_str());
        auto *mapping = cJSON_AddObjectToObject(o, "jlink_mapping");
        for (int i = 0; i < motor_jlink::Count; ++i)
                cJSON_AddStringToObject(
                    mapping, motor_jlink::preset(motor_jlink::Field(i)), jlinkConfig_.fields[i].name.c_str());
        cJSON_AddNumberToObject(o, "backend", backend_);
        cJSON_AddNumberToObject(o, "period_us", period_);
        cJSON_AddNumberToObject(o, "watchdog_ms", watchdog_);
        cJSON_AddBoolToObject(o, "relaxed", relaxed_);
        cJSON_AddNumberToObject(o, "receive_budget_version", 1);
        // Targets, enable and connection state are deliberately never restored
}

void
MotorControl::load(const cJSON *root)
{
        cancelConfirmation();
        dialDragging_ = dialPending_ = false;
        disconnect("Session loaded / 载入会话");
        const auto *o = cJSON_GetObjectItem(root, "motorControl");
        open_         = false;
        backend_      = 3;
        jlinkConfig_  = {};
        jlinkSource_.clear();
        jlinkSymbols_.clear();
        jlinkMappingError_.clear();
        npcapPrompt_             = false;
        period_                  = 1000;
        watchdog_                = 300;
        relaxed_                 = true;
        axis_                    = 0;
        mode_                    = 7;
        interpolation_           = true;
        maxPositionVelocity_     = 1;
        maxPositionAcceleration_ = 2;
        position_ = velocity_ = current_ = torque_ = 0;
        device_[0] = library_[0] = 0;
        deviceName_.clear();
        gaugeRanges_  = defaultMotorGaugeRanges;
        positionUnit_ = velocityUnit_ = 0;
        targetDrafts_                 = {};
        initialized_                  = false;
        useBuiltin_                   = true;
        if (!cJSON_IsObject(o))
                return;
        if (const auto *ranges = cJSON_GetObjectItem(o, "gauge_ranges"); cJSON_IsArray(ranges))
                for (int i = 0; i < 9; ++i) {
                        const auto *entry   = cJSON_GetArrayItem(ranges, i);
                        const auto *minimum = cJSON_GetObjectItem(entry, "minimum"),
                                   *maximum = cJSON_GetObjectItem(entry, "maximum");
                        if (cJSON_IsNumber(minimum) && cJSON_IsNumber(maximum)) {
                                MotorGaugeRange range{float(minimum->valuedouble), float(maximum->valuedouble)};
                                if (range.valid())
                                        gaugeRanges_[i] = range;
                        }
                }
        auto read = [&](const char *key, int &val, int low, int high) {
                const auto *v = cJSON_GetObjectItem(o, key);
                if (cJSON_IsNumber(v))
                        val = std::clamp(v->valueint, low, high);
        };
        read("position_unit", positionUnit_, 0, 1);
        read("velocity_unit", velocityUnit_, 0, 2);
        if (!cJSON_GetObjectItem(o, "gauge_range_version") && std::abs(gaugeRanges_[0].minimum) < 1e-6f &&
            std::abs(gaugeRanges_[0].maximum - 6.28318530718f) < 1e-5f)
                gaugeRanges_[0] = defaultMotorGaugeRanges[0];
        open_ = cJSON_IsTrue(cJSON_GetObjectItem(o, "open"));
        if (const auto *v = cJSON_GetObjectItem(o, "use_builtin"); cJSON_IsBool(v))
                useBuiltin_ = cJSON_IsTrue(v);
        // Migrate the previous 5 ms default; subsequently saved strict choices remain explicit
        if (const auto *revision = cJSON_GetObjectItem(o, "receive_budget_version");
            cJSON_IsNumber(revision) && revision->valueint >= 1) {
                if (const auto *v = cJSON_GetObjectItem(o, "relaxed"); cJSON_IsBool(v))
                        relaxed_ = cJSON_IsTrue(v);
        }
        read("backend", backend_, 0, 4);
        if (const auto *v = cJSON_GetObjectItem(o, "jlink_source"); cJSON_IsString(v))
                jlinkSource_ = v->valuestring;
        if (const auto *mapping = cJSON_GetObjectItem(o, "jlink_mapping"); cJSON_IsObject(mapping))
                for (int i = 0; i < motor_jlink::Count; ++i)
                        if (const auto *v = cJSON_GetObjectItem(mapping, motor_jlink::preset(motor_jlink::Field(i)));
                            cJSON_IsString(v))
                                jlinkConfig_.fields[i].name = v->valuestring;
        const bool legacySimulator = backend_ < 2;
        if (legacySimulator)
                backend_ = 3;
        if (backend_ == 3 && !npcapAvailable())
                npcapPrompt_ = true;
        read("period_us", period_, 1000, 10000);
        read("watchdog_ms", watchdog_, 250, 1000);
        for (auto pair : {std::pair<const char *, char *>{"library", library_}, {"device", device_}}) {
                const auto *v = cJSON_GetObjectItem(o, pair.first);
                if (cJSON_IsString(v))
                        snprintf(pair.second, 1024, "%s", v->valuestring);
        }
        if (const auto *name = cJSON_GetObjectItem(o, "device_name"); cJSON_IsString(name) && *device_)
                deviceName_ = name->valuestring;
        if (legacySimulator) {
                device_[0] = 0;
                deviceName_.clear();
        }
}

int
MotorControl::selectedMode() const
{
        return modes[mode_];
}
void
MotorControl::setTargetEditor(const AvaEthTarget &target)
{
        position_     = target.position;
        velocity_     = target.velocity;
        current_      = target.current;
        torque_       = target.torque;
        targetDrafts_ = {};
        // InputText retains its own edit buffer while focused. A new identity
        // prevents a pre-switch draft from overwriting the accepted target
        ++targetInputRevision_;
        dialDragging_ = dialPending_ = false;
}

void
MotorControl::targetField(const MotorController::State &s, int channel)
{
        const int   mode     = selectedMode();
        const bool  active[] = {mode == 1 || mode == -1 || mode == 8 || mode == -8 || mode == -128,
                                mode == 3 || mode == -3 || mode == 9 || mode == -9 || mode == -128,
                                mode == -10,
                                mode == 4 || mode == 10};
        float      *values[] = {&position_, &velocity_, &current_, &torque_};
        const char *labels[] = {tr("Position target  rad", "目标位置  rad"),
                                tr("Velocity target  rad/s", "目标速度  rad/s"),
                                tr("Current target  A", "目标电流  A"),
                                tr("Torque target  Nm", "目标转矩  Nm")};
        if (channel < 2)
                ImGui::TextDisabled("%s  %s",
                                    channel == 0 ? tr("Position target", "目标位置") : tr("Velocity target", "目标速度"),
                                    channel == 0 ? motor_units::positionLabel(positionUnit_)
                                                 : motor_units::velocityLabel(velocityUnit_));
        else
                ImGui::TextDisabled("%s", labels[channel]);
        ImGui::BeginDisabled(!active[channel] || s.busy || s.snapshot.axes[axis_].special_cmd != 0);
        ImGui::SetNextItemWidth(-1);
        const double factor    = channel == 0   ? motor_units::positionScale(positionUnit_)
                                 : channel == 1 ? motor_units::velocityScale(velocityUnit_)
                                                : 1;
        float        displayed = float(*values[channel] * factor);
        ImGui::PushID(static_cast<int>(targetInputRevision_));
        if (inputWaveFloatOnEnter("##target", displayed, targetDrafts_[channel], "%.4f", true)) {
                const float raw = float(displayed / factor);
                if (std::isfinite(raw)) {
                        *values[channel] = raw;
                        requestConfirmation(MotorController::Action::Apply);
                }
        }
        ImGui::PopID();
        ImGui::EndDisabled();
}

void
MotorControl::requestConfirmation(MotorController::Action action, int newMode)
{
        if (pendingCommand_)
                return;
        previousMode_    = mode_;
        previousTargets_ = {position_, velocity_, current_, torque_};
        if (action == MotorController::Action::SetMode && newMode >= 0) {
                const auto state     = controller_.state();
                mode_                = newMode;
                const float position = valid(state.snapshot.axes[axis_]) ? state.snapshot.axes[axis_].position : 0;
                setTargetEditor({static_cast<uint32_t>(axis_), selectedMode(), position, 0, 0, 0});
        }
        MotorController::Command c;
        c.action           = action;
        c.special_cmd      = action == MotorController::Action::Special ? static_cast<uint32_t>(newMode) : 0;
        c.config           = config();
        c.axis             = axis_;
        c.target           = {c.axis, selectedMode(), position_, velocity_, current_, torque_};
        c.interpolate      = interpolation_ && (c.target.mode == 8 || c.target.mode == -8);
        c.max_velocity     = maxPositionVelocity_;
        c.max_acceleration = maxPositionAcceleration_;
        pendingCommand_    = c;
        openConfirmation_  = true;
        dialDragging_ = dialPending_ = false;
}

void
MotorControl::cancelConfirmation()
{
        if (pendingCommand_ && pendingCommand_->action == MotorController::Action::SetMode) {
                mode_ = previousMode_;
                setTargetEditor({static_cast<uint32_t>(axis_),
                                 selectedMode(),
                                 previousTargets_[0],
                                 previousTargets_[1],
                                 previousTargets_[2],
                                 previousTargets_[3]});
        }
        pendingCommand_.reset();
        pendingPid_.reset();
        openConfirmation_ = false;
}

void
MotorControl::confirmation(const MotorController::State &s)
{
        const char *title = tr("Confirm motor command###motorConfirm", "确认电机指令###motorConfirm");
        if (openConfirmation_) {
                ImGui::OpenPopup(title);
                openConfirmation_ = false;
        }
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (pendingCommand_) {
                        auto &c = *pendingCommand_;
                        // This is a preview only. The SDK samples the latest raw,
                        // multi-turn feedback again when it executes the confirmed switch
                        const bool positionMode = c.target.mode == 1 || c.target.mode == -1 || c.target.mode == 8 ||
                                                  c.target.mode == -8 || c.target.mode == -128;
                        const bool firstPositionEnable =
                            positionMode && s.targets[c.axis].mode != c.target.mode &&
                            (c.action == MotorController::Action::Enable ||
                             (c.action == MotorController::Action::Special && c.special_cmd == AVA_ETH_SPECIAL_ENABLE));
                        if ((c.action == MotorController::Action::SetMode || firstPositionEnable) &&
                            c.axis < s.snapshot.count && valid(s.snapshot.axes[c.axis]) &&
                            std::isfinite(s.snapshot.axes[c.axis].position)) {
                                c.target.position = s.snapshot.axes[c.axis].position;
                                c.target.velocity = c.target.current = c.target.torque = 0;
                                if (position_ != c.target.position || velocity_ != 0 || current_ != 0 || torque_ != 0)
                                        setTargetEditor(c.target);
                        }
                        ImGui::Text("%s %u  |  %s", tr("Actuator", "执行器"), c.axis, modeLabel(mode_));
                        ImGui::Text("position=%.4f %s, velocity=%.4f %s",
                                    c.target.position * motor_units::positionScale(positionUnit_),
                                    motor_units::positionLabel(positionUnit_),
                                    c.target.velocity * motor_units::velocityScale(velocityUnit_),
                                    motor_units::velocityLabel(velocityUnit_));
                        ImGui::Text("current=%.4f A, torque=%.4f Nm", c.target.current, c.target.torque);
                        if (c.action == MotorController::Action::SetMode)
                                ImGui::TextUnformatted(
                                    tr("Keep the enable state. Position modes start at the latest feedback position.",
                                       "保持原有使能状态；位置模式以执行切换时的最新反馈位置作为初始目标。"));
                        if (c.action == MotorController::Action::Enable)
                                ImGui::TextUnformatted(
                                    firstPositionEnable
                                        ? tr("Initialize this position mode at the latest feedback position, then enable.",
                                             "首次使能此位置模式：以执行时最新反馈位置初始化目标，再使能。")
                                        : tr("Send these targets and enable the selected actuator.",
                                             "发送以上目标并使能选定执行器。"));
                        if (c.action == MotorController::Action::Special) {
                                ImGui::Text("special_cmd = %s  (PDO 0x2080)", specialLabel(c.special_cmd));
                                ImGui::TextUnformatted(
                                    tr("The value remains active until changed; acknowledgement is not completion.",
                                       "该值保持生效直到修改；PDO 确认不代表固件动作完成。"));
                                if (c.special_cmd == AVA_ETH_SPECIAL_CALI)
                                        ImGui::TextUnformatted(
                                            tr("Calibration may move the actuator using its configured calibration tasks.",
                                               "校准会执行固件配置的校准任务，执行器可能运动。"));
                                if (c.special_cmd == AVA_ETH_SPECIAL_ENABLE)
                                        ImGui::TextUnformatted(tr("Send the displayed targets and enable the actuator.",
                                                                  "将发送以上目标并使能执行器。"));
                        }
                        const auto &f     = s.snapshot.axes[c.axis];
                        bool        ready = s.connected && !s.busy && c.axis < s.snapshot.count && valid(f) &&
                                     !(f.flags & AVA_ETH_FAULT) && std::isfinite(c.target.position) &&
                                     std::isfinite(c.target.velocity) && std::isfinite(c.target.current) &&
                                     std::isfinite(c.target.torque);
                        if (c.action == MotorController::Action::Special)
                                ready = !s.busy && specialAllowed(s, c.axis, c.special_cmd) &&
                                        (c.special_cmd != AVA_ETH_SPECIAL_ENABLE || ready);
                        ImGui::BeginDisabled(!ready);
                        if (ImGui::Button(tr("Confirm", "确认"))) {
                                controller_.post(c);
                                pendingCommand_.reset();
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                }
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        cancelConfirmation();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}
