#include "ImGuiNotify.hpp"
#include "core/jlink_port.hpp"
#include "core/mmap_vector.hpp"
#include "core/sampler.hpp"
#include "core/storage_maintenance.hpp"
#include "gui/choice_control.hpp"
#include "gui/fault_decoder.hpp"
#include "gui/flow_toolbar.hpp"
#include "gui/gui.hpp"
#include "gui/hover_panel.hpp"
#include "gui/interaction_history.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_icons.hpp"
#include "gui/ui_theme.hpp"
#include "imgui_internal.h"
#include "platform/native_dlg.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace
{
std::string
fileLabel(const std::string &path)
{
        const auto slash = path.find_last_of("/\\");
        return path.substr(slash == std::string::npos ? 0 : slash + 1);
}
std::string
searchKey(std::string text)
{
        for (auto &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
}
std::string
compactPointCount(uint64_t count)
{
        if (count < 1000)
                return std::to_string(count);
        const char *units[] = {"", "K", "M", "G", "T", "P", "E"};
        double      value   = static_cast<double>(count);
        int         unit    = 0;
        while (value >= 999.5 && unit < 6) {
                value /= 1000;
                ++unit;
        }
        char text[32];
        snprintf(text, sizeof(text), "%.*f", value < 10 ? 2 : value < 100 ? 1 : 0, value);
        std::string number(text);
        if (number.find('.') != std::string::npos) {
                while (number.back() == '0')
                        number.pop_back();
                if (number.back() == '.')
                        number.pop_back();
        }
        return number + units[unit];
}
void
hint(const char *text)
{
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", text);
}
int
localHour()
{
        const auto now = std::time(nullptr);
        std::tm    local{};
#ifdef _WIN32
        return localtime_s(&local, &now) == 0 ? local.tm_hour : 12;
#else
        return localtime_r(&now, &local) ? local.tm_hour : 12;
#endif
}
} // namespace

void
Gui::loadAppearance()
{
        std::ifstream file(getAppDir() + "/appearance.txt");
        int           mode = 0, light = 7, dark = 19;
        if (file >> mode >> light >> dark && mode >= 0 && mode <= 3 && light >= 0 && light < 24 && dark >= 0 && dark < 24 &&
            light != dark) {
                appearanceMode_ = static_cast<appearance::Mode>(mode);
                lightStartHour_ = light;
                darkStartHour_  = dark;
        }
#ifdef _WIN32
        systemAppearanceDark_ = appearance::QuerySystemDark(true);
#endif
        appearanceDark_ =
            appearance::ResolveDark(appearanceMode_, systemAppearanceDark_, localHour(), lightStartHour_, darkStartHour_);
}

void
Gui::saveAppearance()
{
        std::ofstream file(getAppDir() + "/appearance.txt", std::ios::trunc);
        file << static_cast<int>(appearanceMode_) << ' ' << lightStartHour_ << ' ' << darkStartHour_ << '\n';
        nextAppearanceQuery_ = 0;
}

void
Gui::updateAppearance()
{
        if (appearanceQuery_.valid() && appearanceQuery_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                        systemAppearanceDark_ = appearanceQuery_.get();
                } catch (...) { /* retain last known theme */
                }
        }
        if (appearanceMode_ == appearance::Mode::System && !appearanceQuery_.valid() &&
            ImGui::GetTime() >= nextAppearanceQuery_) {
                nextAppearanceQuery_ = ImGui::GetTime() + 5.0;
                const bool fallback  = systemAppearanceDark_;
                appearanceQuery_ = std::async(std::launch::async, [fallback] { return appearance::QuerySystemDark(fallback); });
        }
        const bool dark =
            appearance::ResolveDark(appearanceMode_, systemAppearanceDark_, localHour(), lightStartHour_, darkStartHour_);
        if (dark != appearanceDark_) {
                appearanceDark_     = dark;
                platformStyleDirty_ = true;
        }
}

void
Gui::drawAppearanceMenu()
{
        if (!ImGui::BeginMenu(tr("Appearance", "外观")))
                return;
        const char *labels[] = {
            tr("Follow system", "跟随系统"), tr("Follow schedule", "按时间切换"), tr("Light", "浅色"), tr("Dark", "深色")};
        int appearance = static_cast<int>(appearanceMode_);
        if (ui::Choice("##appearance", &appearance, labels, 4)) {
                appearanceMode_ = static_cast<appearance::Mode>(appearance);
                saveAppearance();
        }
        if (appearanceMode_ == appearance::Mode::Schedule) {
                ImGui::Separator();
                auto hour = [&](const char *label, int &value, int other) {
                        char current[8];
                        std::snprintf(current, sizeof(current), "%02d:00", value);
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
                        if (ImGui::BeginCombo(label, current)) {
                                for (int i = 0; i < 24; ++i) {
                                        char item[8];
                                        std::snprintf(item, sizeof(item), "%02d:00", i);
                                        ImGui::BeginDisabled(i == other);
                                        if (ImGui::Selectable(item, value == i)) {
                                                value = i;
                                                saveAppearance();
                                        }
                                        ImGui::EndDisabled();
                                }
                                ImGui::EndCombo();
                        }
                };
                hour(tr("Light starts", "浅色开始"), lightStartHour_, darkStartHour_);
                hour(tr("Dark starts", "深色开始"), darkStartHour_, lightStartHour_);
                ImGui::TextDisabled("%s", tr("Uses this device's local time.", "使用此设备的本地时间。"));
        }
        ImGui::SeparatorText(tr("Color scheme", "配色"));
        const char *schemeLabels[] = {tr("Ocean", "海蓝"),
                                      tr("Graphite", "石墨"),
                                      tr("Violet", "鸢紫"),
                                      tr("Mint", "薄荷"),
                                      tr("Rose", "玫瑰"),
                                      tr("Amber", "琥珀")};
        int         scheme         = static_cast<int>(colorScheme_);
        if (ui::Choice("##color_scheme", &scheme, schemeLabels, static_cast<int>(ui::ColorScheme::Count))) {
                colorScheme_        = static_cast<ui::ColorScheme>(scheme);
                platformStyleDirty_ = true;
                std::ofstream file(getAppDir() + "/color_scheme.txt", std::ios::trunc);
                file << scheme << '\n';
                file.flush();
                if (!file)
                        ImGui::InsertNotification(
                            {ImGuiToastType::Error, 4000, "%s", tr("Cannot save color scheme.", "无法保存配色。")});
        }
        ImGui::EndMenu();
}

std::string
Gui::sessionUiState() const
{
        auto *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "home", showHome_);
        cJSON_AddBoolToObject(root, "calculator", showCalculator_);
        cJSON_AddBoolToObject(root, "assembly", asmViewer_.show_);
        cJSON_AddBoolToObject(root, "bode", bode_.show_);
        cJSON_AddBoolToObject(root, "inverter", inverterSimulator_.show_);
        cJSON_AddBoolToObject(root, "sequence", seqEditor_.isOpen());
        cJSON_AddBoolToObject(root, "devices", deviceManager_.isOpen());
        auto *jlink = cJSON_AddObjectToObject(root, "jlink");
        JLinkPort::instance().saveSession(jlink);
        flashTool_.saveSession(root);
        faultDecoder().save(root);
        midiTool_.save(root);
        motorControl_.save(root);
        char             *text   = cJSON_PrintUnformatted(root);
        const std::string result = text ? text : "";
        cJSON_free(text);
        cJSON_Delete(root);
        return result;
}

void
Gui::captureSessionBaseline(bool settleLayout)
{
        sessionUiBaseline_     = sessionUiState();
        sessionLayoutBaseline_ = ImGui::GetCurrentContext() ? ImGui::SaveIniSettingsToMemory() : "";
        sessionLayoutSettle_   = settleLayout ? 8 : 0;
}

bool
Gui::hasSessionChanges() const
{
        if (isModified_ || seqEditor_.isModified() || deviceManager_.isModified() || bode_.isModified() ||
            inverterSimulator_.isModified() || bkpSramDebugger_.isModified() || JLinkPort::instance().isConfigModified())
                return true;
        if (!sessionUiBaseline_.empty() && sessionUiBaseline_ != sessionUiState())
                return true;
        if (!sessionLayoutSettle_ && !sessionLayoutBaseline_.empty() && ImGui::GetCurrentContext() &&
            sessionLayoutBaseline_ != ImGui::SaveIniSettingsToMemory())
                return true;
        for (const auto &[_, variable] : vars_)
                if (variable->isModified())
                        return true;
        for (const auto &panel : sdkPanels_)
                if (panel->isModified())
                        return true;
        std::lock_guard lock(mtxMonitors_);
        for (const auto &[_, monitor] : monitors_)
                if (monitor->isModified())
                        return true;
        return false;
}

void
Gui::requestSessionChange(SessionChange action, const std::string &path, const std::string &source)
{
        if (sessionChange_ != SessionChange::None)
                return;
        sessionChange_       = action;
        sessionChangePath_   = path;
        sessionChangeSource_ = source;
        sessionChangeReady_  = !hasSessionChanges();
}

void
Gui::applySessionChange()
{
        const auto action   = sessionChange_;
        const auto path     = std::move(sessionChangePath_);
        const auto source   = std::move(sessionChangeSource_);
        sessionChange_      = SessionChange::None;
        sessionChangeReady_ = false;
        if (action == SessionChange::New)
                newSession();
        if (action == SessionChange::Open)
                loadSession(path, source);
}

void
Gui::drawSessionChangePrompt()
{
        if (showQuitModal_ || showCloseChoice_)
                return;
        if (sessionChange_ != SessionChange::None && !sessionChangeReady_)
                ImGui::OpenPopup("###SessionChange");
        if (ImGui::BeginPopupModal(tr("Unsaved changes###SessionChange", "尚未保存的更改###SessionChange"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(tr("Save this workspace before continuing?", "继续前保存当前工作区吗？"));
                ImGui::TextDisabled(
                    "%s", isFirstSave_ ? tr("Untitled session", "未命名会话") : fileLabel(currentSessionPath_).c_str());
                ImGui::Spacing();
                if (ui::Button(tr("Save and continue", "保存并继续"), ui::BtnStyle::Primary)) {
                        if (saveSession()) {
                                ImGui::CloseCurrentPopup();
                                sessionChangeReady_ = true;
                        }
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ui::Button(tr("Discard changes", "放弃更改"), ui::BtnStyle::Danger)) {
                        ImGui::CloseCurrentPopup();
                        sessionChangeReady_ = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        sessionChange_ = SessionChange::None;
                        sessionChangePath_.clear();
                        sessionChangeSource_.clear();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        if (sessionChange_ != SessionChange::None)
                return;
        if (showClearData_)
                ImGui::OpenPopup("###ClearWorkspaceData");
        if (ImGui::BeginPopupModal(tr("Clear recorded data?###ClearWorkspaceData", "清空已采集数据？###ClearWorkspaceData"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    tr("This clears recorded samples in all monitors.", "这会清空所有监视器中的已采集数据。"));
                ImGui::TextUnformatted(tr("Channels and settings stay in place. This cannot be undone.",
                                          "通道和设置会保留；清空的数据无法撤销恢复。"));
                ImGui::Spacing();
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        showClearData_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ui::Button(tr("Clear data", "清空数据"), ui::BtnStyle::Danger)) {
                        Monitor::clearAll();
                        bkpSramDebugger_.requestClearData();
                        showClearData_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Gui::addVariableWindow()
{
        size_t      index = vars_.size();
        std::string name;
        do {
                name = "变量管理器_" + std::to_string(index++);
        } while (vars_.count(name));
        auto variable = std::make_shared<Variable>(name);
        variable->setTitle(std::string(tr("Variables", "变量")) + " " + std::to_string(index));
        vars_[name] = std::move(variable);
        isModified_ = true;
}

void
Gui::addMonitorWindow()
{
        std::lock_guard lock(mtxMonitors_);
        size_t          index = monitors_.size();
        std::string     name;
        do {
                name = "变量记录器_" + std::to_string(index++);
        } while (monitors_.count(name));
        auto monitor = std::make_shared<Monitor>(name);
        monitor->setTitle(std::string(tr("Variable Recorder", "变量记录器")) + " " + std::to_string(index));
        monitors_[name] = std::move(monitor);
        isModified_     = true;
}

void
Gui::executeCommand()
{
        const auto action = pendingCommand_;
        pendingCommand_   = AppCommand::None;
        switch (action) {
                case AppCommand::Home:
                        showHome_ = true;
                        ImGui::SetWindowFocus("###WorkspaceHome");
                        break;
                case AppCommand::Commands:
                        showCommands_     = true;
                        commandFocus_     = true;
                        commandQuery_[0]  = 0;
                        commandSelection_ = 0;
                        break;
                case AppCommand::Open: {
                        auto path = nativeDlgOpen(tr("Open Session", "打开会话"), {{"Session Files", {"ava", "json"}}});
                        if (!path.empty())
                                requestSessionChange(SessionChange::Open, path);
                        break;
                }
                case AppCommand::New:
                        requestSessionChange(SessionChange::New);
                        break;
                case AppCommand::Save:
                        saveSession();
                        break;
                case AppCommand::SaveAs:
                        saveSessionAs();
                        break;
                case AppCommand::Variables:
                        addVariableWindow();
                        break;
                case AppCommand::Monitor:
                        addMonitorWindow();
                        break;
                case AppCommand::Devices:
                        deviceManager_.setOpen(true);
                        break;
                case AppCommand::Flash:
                        flashTool_.open();
                        break;
                case AppCommand::Parameters:
                        if (vars_.empty())
                                addVariableWindow();
                        vars_.begin()->second->openParameterEditor();
                        break;
                case AppCommand::Bode:
                        bode_.show_ = true;
                        break;
                case AppCommand::Motor:
                        motorControl_.setOpen(true);
                        break;
                case AppCommand::Calculator:
                        showCalculator_ = true;
                        break;
                case AppCommand::Scope:
                        newSdkPanel();
                        break;
                case AppCommand::Sdk:
                        newSdkPanel();
                        break;
                case AppCommand::Trace:
                        JLinkPort::instance().openTraceWindow();
                        break;
                case AppCommand::Sequence:
                        seqEditor_.setOpen(true);
                        break;
                case AppCommand::Safety:
                        faultDecoder().open = true;
                        break;
                case AppCommand::Connect:
                        if (!JLinkPort::instance().isBusy() && !JLinkPort::instance().isConnected())
                                JLinkPort::instance().connectAsync();
                        break;
                case AppCommand::Disconnect:
                        if (!JLinkPort::instance().isBusy() && JLinkPort::instance().isConnected())
                                JLinkPort::instance().disconnectAsync();
                        break;
                case AppCommand::FreezeDisplay:
                        g_monitorPaused.store(!g_monitorPaused.load());
                        break;
                case AppCommand::PauseAcquisition: {
                        const bool      pause = !g_jlinkSamplingPaused.load();
                        std::lock_guard lock(mtxMonitors_);
                        for (auto &[_, monitor] : monitors_)
                                monitor->setSamplingPaused(pause);
                        g_jlinkSamplingPaused.store(pause);
                        break;
                }
                case AppCommand::ClearData:
                        showClearData_ = true;
                        break;
                default:
                        break;
        }
}

void
Gui::drawWorkspaceChrome()
{
        auto       *viewport = ImGui::GetMainViewport();
        const auto &style    = ImGui::GetStyle();
        const bool  narrow   = viewport->WorkSize.x < ImGui::GetFontSize() * 41;
        const float rowHeight =
            ImGui::GetFrameHeight() + style.WindowPadding.y * 2 + (narrow ? ImGui::GetFrameHeightWithSpacing() : 0);
        const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_MenuBarBg]);
        if (ImGui::BeginViewportSideBar("##WorkspaceCommands", viewport, ImGuiDir_Up, rowHeight, flags)) {
                const bool      compact = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 69;
                ui::FlowToolbar commands;
                const auto      iconWidth = [&](const char *label) {
                        const float text = ImGui::CalcTextSize(label, nullptr, true).x;
                        return ImGui::GetFontSize() * 1.1f + style.FramePadding.x * 2 + text +
                               (text > 0 ? style.ItemInnerSpacing.x + 1 : 0);
                };
                auto button =
                    [&](const char *label, ui::Icon icon, AppCommand command, ui::BtnStyle tone = ui::BtnStyle::Neutral) {
                            const std::string id =
                                std::string(compact ? "" : label) + "###Command" + std::to_string(static_cast<int>(command));
                            commands.next(iconWidth(id.c_str()));
                            if (ui::IconButton(id.c_str(), icon, tone))
                                    pendingCommand_ = command;
                            if (compact)
                                    hint(label);
                    };
                ImGui::BeginDisabled(!ui::interaction::canBack());
                commands.next(ImGui::GetFrameHeight());
                if (ImGui::ArrowButton("##Back", ImGuiDir_Left))
                        ui::interaction::requested = ui::interaction::Action::Back;
                ImGui::EndDisabled();
                hint(tr("Back (Alt+Left / mouse back)", "后退（Alt+←／鼠标侧键）"));
                ImGui::BeginDisabled(!ui::interaction::canForward());
                commands.next(ImGui::GetFrameHeight());
                if (ImGui::ArrowButton("##Forward", ImGuiDir_Right))
                        ui::interaction::requested = ui::interaction::Action::Forward;
                ImGui::EndDisabled();
                hint(tr("Forward (Alt+Right)", "前进（Alt+→）"));
                ImGui::BeginDisabled(!ui::interaction::parentAvailable);
                commands.next(ImGui::GetFrameHeight());
                if (ImGui::ArrowButton("##Parent", ImGuiDir_Up))
                        ui::interaction::requested = ui::interaction::Action::Parent;
                ImGui::EndDisabled();
                hint(tr("Up one level (Alt+Up)", "返回上一级（Alt+↑）"));
                button(tr("Home", "首页"), ui::Icon::Home, AppCommand::Home);
                button(tr("Open", "打开"), ui::Icon::Open, AppCommand::Open);
                hint(ui::ShortcutLabel("Ctrl+O", "Cmd+O"));
                button(tr("Save", "保存"), ui::Icon::Save, AppCommand::Save);
                hint(ui::ShortcutLabel("Ctrl+S", "Cmd+S"));
                commands.next(iconWidth(compact ? "" : tr("Add panel...", "添加面板...")));
                if (ui::IconButton(compact ? "###AddPanelButton"
                                           : tr("Add panel...###AddPanelButton", "添加面板...###AddPanelButton"),
                                   ui::Icon::Add))
                        ImGui::OpenPopup("AddPanel");
                if (compact)
                        hint(tr("Add panel...", "添加面板..."));
                if (ImGui::BeginPopup("AddPanel")) {
                        if (ImGui::MenuItem(tr("Variables", "变量管理器")))
                                pendingCommand_ = AppCommand::Variables;
                        if (ImGui::MenuItem(tr("Variable Recorder", "变量记录器")))
                                pendingCommand_ = AppCommand::Monitor;
                        if (ImGui::MenuItem(tr("Device Manager", "设备管理器")))
                                pendingCommand_ = AppCommand::Devices;
                        if (ImGui::MenuItem(tr("Dynamic Library Caller", "动态库调用器")))
                                pendingCommand_ = AppCommand::Sdk;
                        ImGui::EndPopup();
                }
                button(tr("Find command...", "查找功能..."), ui::Icon::Search, AppCommand::Commands);
                hint(ui::ShortcutLabel("Ctrl+K", "Cmd+K"));
                const bool  connected = JLinkPort::instance().isConnected();
                const bool  busy      = JLinkPort::instance().isBusy();
                const char *label     = busy        ? tr("J-Link: working", "J-Link  处理中")
                                        : connected ? tr("J-Link: connected", "J-Link  已连接")
                                                    : tr("J-Link: offline", "J-Link  未连接");
                commands.group();
                commands.next(iconWidth(compact ? "J-Link" : label));
                const bool activateJLink =
                    ui::IconButton(compact ? "J-Link###JLinkPanel" : (std::string(label) + "###JLinkPanel").c_str(),
                                   ui::Icon::Connect,
                                   connected ? ui::BtnStyle::Success : ui::BtnStyle::Neutral);
                TutorialGuide::instance().markJLinkEntry();
                hint("J-Link");
                if (ui::BeginHoverPanel(
                        "##JLinkControls", "J-Link", jlinkReveal_, activateJLink, ImGui::GetFontSize() * 20.0f, viewport)) {
                        TutorialGuide::instance().markDock(TutorialGuide::Topic::JLink);
                        JLinkPort::instance().drawUI(true);
                        ImGui::Spacing();
                        ImGui::SeparatorText(tr("Acquisition", "采集"));
                        int         hssHz  = g_maxHssHz.load(std::memory_order_relaxed);
                        const float fieldX = ImGui::GetCursorPosX() + ImGui::GetFontSize() * 5.5f;
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(tr("HSS rate", "采集频率"));
                        ImGui::SameLine(fieldX);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::SliderInt("##HssRate", &hssHz, 1, 50000, "%d Hz", ImGuiSliderFlags_Logarithmic)) {
                                g_maxHssHz.store(hssHz, std::memory_order_relaxed);
                                isModified_ = true;
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit())
                                JLinkPort::instance().reqRestart();
                        hint(tr("HSS sampling limit. Ctrl+click to type a value.", "HSS 采集频率上限，Ctrl+点击可直接输入。"));
                        const bool paused = g_jlinkSamplingPaused.load();
                        ImGui::Spacing();
                        if (ui::Button(paused ? tr("Resume acquisition", "恢复采集") : tr("Pause acquisition", "暂停采集"),
                                       paused ? ui::BtnStyle::Success : ui::BtnStyle::Warning,
                                       ImVec2(-FLT_MIN, 0)))
                                pendingCommand_ = AppCommand::PauseAcquisition;
                        TutorialGuide::instance().mark("pause_btn");
                        ImGui::End();
                }
                commands.next(iconWidth(""));
                const bool activateActions = ui::IconButton("###WorkspaceActions", ui::Icon::Properties);
                hint(tr("Display", "画面操作"));
                if (ui::BeginHoverPanel("##WorkspaceActions",
                                        tr("Display", "画面操作"),
                                        workspaceActionsReveal_,
                                        activateActions,
                                        ImGui::GetFontSize() * 17.0f,
                                        viewport)) {
                        if (ImGui::MenuItem(g_monitorPaused.load() ? tr("Resume display", "恢复画面")
                                                                   : tr("Freeze display", "冻结画面")))
                                pendingCommand_ = AppCommand::FreezeDisplay;
                        hint(tr("Freezes the display only; acquisition continues.", "仅冻结显示，后台采集仍会继续。"));
                        if (ImGui::MenuItem(tr("Clear recorded data...", "清空已采集数据...")))
                                pendingCommand_ = AppCommand::ClearData;
                        ImGui::End();
                }
                commands.group();
                commands.next(iconWidth("") * 2 + style.ItemSpacing.x);
                drawCaptureControls(compact);
        }
        ImGui::End();
        const float statusHeight = ImGui::GetTextLineHeight() + 12.0f * uiScale_;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * uiScale_, 6 * uiScale_));
        if (ImGui::BeginViewportSideBar("##WorkspaceStatus", viewport, ImGuiDir_Down, statusHeight, flags)) {
                const bool connected = JLinkPort::instance().isConnected();
                if (connected) {
                        ImGui::TextUnformatted(tr("J-Link connected", "J-Link 已连接"));
                        ImGui::SameLine(0, 16 * uiScale_);
                }
                static u64 points = 0;
                {
                        std::unique_lock lock(mtxMonitors_, std::try_to_lock);
                        if (lock.owns_lock())
                                points = Monitor::getGlobalPointCount();
                }
                ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);
                ImGui::SameLine(0, 16 * uiScale_);
                ImGui::Text(tr("Points: %s", "数据点: %s"), compactPointCount(points).c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(tr("Total points: %llu", "数据点总数：%llu"),
                                          static_cast<unsigned long long>(points));

                if (ImGui::GetWindowWidth() - (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) > 540 * uiScale_) {
                        if (g_monitorPaused.load()) {
                                ImGui::SameLine(0, 20 * uiScale_);
                                ImGui::TextDisabled("%s", tr("Display frozen", "画面已冻结"));
                        }
                        if (connected) {
                                ImGui::SameLine(0, 20 * uiScale_);
                                ImGui::TextDisabled("%s",
                                                    g_jlinkSamplingPaused.load() ? tr("Acquisition paused", "采集已暂停")
                                                                                 : tr("Acquisition enabled", "采集已启用"));
                        }
                }
                const std::string session =
                    (hasSessionChanges() ? "* " : "") +
                    std::string(isFirstSave_ ? tr("Untitled session", "未命名会话") : fileLabel(currentSessionPath_));
                const float width = ImGui::CalcTextSize(session.c_str()).x;
                const float right = ImGui::GetWindowWidth() - width - style.WindowPadding.x;
                if (right > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 24 * uiScale_) {
                        ImGui::SameLine(right);
                        ImGui::TextDisabled("%s", session.c_str());
                        hint(currentSessionPath_.c_str());
                }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
}

void
Gui::drawHome()
{
        if (!showHome_)
                return;
        if (workspaceDock_)
                ImGui::SetNextWindowDockID(workspaceDock_, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900 * uiScale_, 600 * uiScale_), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Workspace###WorkspaceHome", "工作区###WorkspaceHome"), &showHome_)) {
                ImGui::End();
                return;
        }
        ImGui::Spacing();
        ImGui::SetWindowFontScale(ui::design::heading / ui::design::body);
        ImGui::TextUnformatted(tr("Your workspace", "工作区"));
        ImGui::SetWindowFontScale(1);
        ui::FlowToolbar help;
        help.next(tr("Getting started", "新手引导"));
        if (ImGui::Button(tr("Getting started", "新手引导")))
                TutorialGuide::instance().start();
        help.next(tr("Panel guides", "面板指南"));
        if (ImGui::Button(tr("Panel guides", "面板指南")))
                TutorialGuide::instance().openCatalog();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const bool wide = ImGui::GetContentRegionAvail().x > 720 * uiScale_;
        if (ImGui::BeginTable("HomeColumns", wide ? 2 : 1, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::SeparatorText(tr("Get started", "开始使用"));
                auto action = [&](const char *label, const char *description, AppCommand command, bool primary = false) {
                        ImGui::PushID(static_cast<int>(command));
                        const auto icon = command == AppCommand::Open        ? ui::Icon::Open
                                          : command == AppCommand::Variables ? ui::Icon::Variables
                                          : command == AppCommand::Monitor   ? ui::Icon::Chart
                                          : command == AppCommand::Motor     ? ui::Icon::Motor
                                          : command == AppCommand::Commands  ? ui::Icon::Search
                                                                             : ui::Icon::Device;
                        if (ui::IconButton(label,
                                           icon,
                                           primary ? ui::BtnStyle::Primary : ui::BtnStyle::Neutral,
                                           ImVec2(-1, ImGui::GetFrameHeight() + 8 * uiScale_)))
                                pendingCommand_ = command;
                        (void)description;
                        ImGui::Spacing();
                        ImGui::PopID();
                };
                action(tr("Open session...", "打开会话..."),
                       tr("Restore panels, channels and device settings from an .ava file.",
                          "从 .ava 文件恢复面板、通道和设备设置。"),
                       AppCommand::Open,
                       true);
                action(tr("Inspect variables", "查看变量"),
                       tr("Import ELF/AXF symbols or add addresses and local variables.",
                          "导入 ELF/AXF 符号，或添加地址变量和本地变量。"),
                       AppCommand::Variables);
                action(tr("Create a monitor", "创建变量记录器"),
                       tr("Compare signals in a grid, stack plots or focus one scope.",
                          "使用网格、纵向排列或聚焦视图比较数据曲线。"),
                       AppCommand::Monitor);
                ImGui::SeparatorText(tr("Recent sessions", "最近会话"));
                if (recentSessions_.empty())
                        ImGui::TextDisabled("%s", tr("No recent sessions", "暂无最近会话"));
                std::string selected;
                for (size_t i = 0; i < std::min<size_t>(recentSessions_.size(), 6); ++i) {
                        const auto &path = recentSessions_[i];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(fileLabel(path).c_str(), false, 0, ImVec2(0, ImGui::GetFrameHeight())))
                                selected = path;
                        hint(path.c_str());
                        ImGui::PopID();
                }
                ImGui::TableNextColumn();
                ImGui::SeparatorText(tr("Tools", "工具"));
                action(tr("Devices & connections", "设备与连接"),
                       tr("Manage devices and their properties in one place.", "管理设备及其属性。"),
                       AppCommand::Devices);
                action(tr("Motor control", "电机控制"),
                       tr("Open the motor dashboard and control panels.", "打开电机仪表盘与控制面板。"),
                       AppCommand::Motor);
                action(tr("Flash & firmware", "Flash 与固件"),
                       tr("Inspect memory and prepare firmware operations.", "查看内存并配置固件操作。"),
                       AppCommand::Flash);
                action(tr("Browse all commands...", "查找全部功能..."),
                       tr("Search tools and actions by name. Ctrl/Cmd+K.", "按名称查找工具与操作，快捷键 Ctrl/Cmd+K。"),
                       AppCommand::Commands);
                ImGui::EndTable();
                if (!selected.empty())
                        requestSessionChange(SessionChange::Open, selected);
        }
        ImGui::End();
}

void
Gui::drawCommands()
{
        if (showQuitModal_ || showCloseChoice_ || sessionChange_ != SessionChange::None || showClearData_)
                return;
        struct Entry {
                AppCommand  command;
                const char *en;
                const char *zh;
                const char *detailEn;
                const char *detailZh;
        };
        const Entry entries[] = {
            {AppCommand::Open, "Open session", "打开会话", "File", "文件"},
            {AppCommand::Save, "Save session", "保存会话", "File", "文件"},
            {AppCommand::SaveAs, "Save session as", "会话另存为", "File", "文件"},
            {AppCommand::New, "New session", "新建会话", "File", "文件"},
            {AppCommand::Home, "Workspace home", "工作区首页", "Navigation", "导航"},
            {AppCommand::Variables, "Add variable manager", "添加变量管理器", "Panels", "面板"},
            {AppCommand::Monitor, "Add variable recorder", "添加变量记录器", "Panels", "面板"},
            {AppCommand::Devices, "Device Manager", "设备管理器", "Devices", "设备"},
            {AppCommand::Parameters, "Parameter configuration", "参数配置", "Tools", "工具"},
            {AppCommand::Motor, "Motor control", "电机控制", "Tools", "工具"},
            {AppCommand::Flash, "Flash & firmware", "Flash 与固件", "Tools", "工具"},
            {AppCommand::Bode, "Bode plot", "伯德图", "Analysis", "分析"},
            {AppCommand::Calculator, "Motor calculator", "电机参数计算器", "Tools", "工具"},
            {AppCommand::Sdk, "Dynamic Library Caller", "动态库调用器", "Tools", "工具"},
            {AppCommand::Trace, "SWO / ITM trace", "SWO / ITM 跟踪", "Analysis", "分析"},
            {AppCommand::Sequence, "Sequence editor", "序列编辑器", "Tools", "工具"},
            {AppCommand::Safety, "Safety diagnostics", "功能安全诊断", "Analysis", "分析"},
            {AppCommand::Connect, "Connect J-Link", "连接 J-Link", "Devices", "设备"},
            {AppCommand::Disconnect, "Disconnect J-Link", "断开 J-Link", "Devices", "设备"},
            {AppCommand::FreezeDisplay, "Freeze / resume display", "冻结 / 恢复画面", "Acquisition continues", "采集继续运行"},
            {AppCommand::PauseAcquisition, "Pause / resume acquisition", "暂停 / 恢复采集", "All monitors", "所有监视器"},
            {AppCommand::ClearData, "Clear recorded data...", "清空已采集数据...", "Requires confirmation", "需要确认"}};
        if (showCommands_)
                ImGui::OpenPopup("###CommandPalette");
        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * .5f, viewport->Pos.y + viewport->Size.y * .18f),
                                ImGuiCond_Appearing,
                                ImVec2(.5f, 0));
        ImGui::SetNextWindowSize(ImVec2(std::min(640.0f * uiScale_, viewport->Size.x - 32 * uiScale_), 0));
        if (ImGui::BeginPopupModal(tr("Find a command###CommandPalette", "查找功能###CommandPalette"),
                                   &showCommands_,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                if (commandFocus_) {
                        ImGui::SetKeyboardFocusHere();
                        commandFocus_ = false;
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##commandQuery",
                                             tr("Type a tool or action...", "输入工具或操作名称..."),
                                             commandQuery_,
                                             sizeof(commandQuery_)))
                        commandSelection_ = 0;
                std::vector<const Entry *> matches;
                const auto                 query = searchKey(commandQuery_);
                for (const auto &entry : entries)
                        if (searchKey(std::string(entry.en) + entry.zh + entry.detailEn + entry.detailZh).find(query) !=
                            std::string::npos)
                                matches.push_back(&entry);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                        ++commandSelection_;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                        --commandSelection_;
                commandSelection_ = std::clamp(commandSelection_, 0, std::max(0, static_cast<int>(matches.size()) - 1));
                AppCommand chosen = AppCommand::None;
                if (ImGui::BeginChild("CommandResults",
                                      ImVec2(0, std::min(360.0f * uiScale_, viewport->Size.y * .5f)),
                                      ImGuiChildFlags_Borders)) {
                        if (matches.empty())
                                ImGui::TextWrapped("%s",
                                                   tr("No matching command. Try a tool name such as Monitor or Variables.",
                                                      "没有匹配的功能，可尝试“监视器”或“变量”等名称。"));
                        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
                                const auto &entry = *matches[i];
                                ImGui::PushID(static_cast<int>(entry.command));
                                const bool enabled =
                                    entry.command == AppCommand::Connect
                                        ? !JLinkPort::instance().isConnected() && !JLinkPort::instance().isBusy()
                                    : entry.command == AppCommand::Disconnect
                                        ? JLinkPort::instance().isConnected() && !JLinkPort::instance().isBusy()
                                        : true;
                                ImGui::BeginDisabled(!enabled);
                                if (ImGui::Selectable(
                                        tr(entry.en, entry.zh), i == commandSelection_, 0, ImVec2(0, ImGui::GetFrameHeight())))
                                        chosen = entry.command;
                                if (i == commandSelection_ &&
                                    (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                                        ImGui::SetScrollHereY();
                                hint(tr(entry.detailEn, entry.detailZh));
                                if (enabled && i == commandSelection_ && ImGui::IsKeyPressed(ImGuiKey_Enter))
                                        chosen = entry.command;
                                ImGui::EndDisabled();
                                ImGui::PopID();
                        }
                }
                ImGui::EndChild();
                if (chosen != AppCommand::None) {
                        pendingCommand_ = chosen;
                        showCommands_   = false;
                        ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        showCommands_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Gui::drawStorageSettings()
{
        namespace sm = storage_maintenance;
        struct Snapshot {
                sm::Usage logs, cache, cleanup;
                bool      cleaned{};
        };
        static Snapshot              stats;
        static std::future<Snapshot> pending;
        static std::string           directoryKey;
        static double                nextRefresh = 0;
        const std::string            custom      = mmapVectorCacheDir();
        const auto                   app         = std::filesystem::u8path(getAppDir());
        std::error_code              ec;
        const auto                   temp  = std::filesystem::temp_directory_path(ec);
        const auto                   cache = (custom.empty() ? temp : std::filesystem::u8path(custom)) / "ava_tool_mmap_cache";
        std::vector<sm::Root>        caches{{cache, sm::Kind::Sampling}, {temp / "ava_tool_remote", sm::Kind::Downloads}};
        if (!custom.empty())
                caches.push_back({temp / "ava_tool_mmap_cache", sm::Kind::Sampling});
        const std::vector<sm::Root> logs{{app / "log", sm::Kind::Logs}};
        auto                        launch = [&](int cleanup) {
                pending     = std::async(std::launch::async, [logs, caches, cleanup] {
                        Snapshot result;
                        if (cleanup) {
                                result.cleanup = sm::process(cleanup == 1 ? logs : caches, true);
                                result.cleaned = true;
                        }
                        result.logs  = sm::process(logs);
                        result.cache = sm::process(caches);
                        return result;
                });
                nextRefresh = ImGui::GetTime() + 5;
        };
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                        auto next = pending.get();
                        if (!next.cleaned && stats.cleaned) {
                                next.cleanup = stats.cleanup;
                                next.cleaned = true;
                        }
                        stats = next;
                } catch (...) {
                        ++stats.cache.errors;
                }
        }
        if (!pending.valid() && (directoryKey != custom || ImGui::GetTime() >= nextRefresh)) {
                directoryKey = custom;
                launch(0);
        }
        auto bytes = [](uint64_t size) {
                const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
                double      value   = static_cast<double>(size);
                int         unit    = 0;
                while (value >= 1024 && unit < 4) {
                        value /= 1024;
                        ++unit;
                }
                char text[64];
                snprintf(text, sizeof(text), "%.2f %s", value, units[unit]);
                return std::string(text);
        };
        ImGui::Text("%s: %s  (%llu %s)",
                    tr("Logs", "日志"),
                    bytes(stats.logs.bytes).c_str(),
                    static_cast<unsigned long long>(stats.logs.files),
                    tr("files", "个文件"));
        ImGui::Text("%s: %s  (%llu %s)",
                    tr("Cache", "缓存"),
                    bytes(stats.cache.bytes).c_str(),
                    static_cast<unsigned long long>(stats.cache.files),
                    tr("files", "个文件"));
        if (pending.valid())
                ImGui::TextDisabled("%s", tr("Calculating...", "正在统计／清理…"));
        ImGui::BeginDisabled(pending.valid());
        if (ImGui::Button(tr("Clear logs", "清空日志")))
                launch(1);
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear cache", "清空缓存")))
                launch(2);
        ImGui::SameLine();
        if (ImGui::Button(tr("Refresh usage", "刷新占用")))
                launch(0);
        ImGui::EndDisabled();
        if (stats.cleaned)
                ImGui::TextWrapped(tr("Released %s; removed %llu files, skipped %llu busy files.",
                                      "已释放 %s；清理 %llu 个文件，跳过 %llu 个占用文件。"),
                                   bytes(stats.cleanup.released).c_str(),
                                   static_cast<unsigned long long>(stats.cleanup.removed),
                                   static_cast<unsigned long long>(stats.cleanup.skipped));
        if (stats.logs.errors || stats.cache.errors || stats.cleanup.errors)
                ImGui::TextWrapped(
                    "%s", tr("Some paths could not be read; totals may be incomplete.", "部分路径无法读取，统计可能不完整。"));
        ImGui::Separator();
        const auto utf = cache.u8string();
        ImGui::TextWrapped("%s: %s", tr("Sampling cache", "采样缓存"), std::string(utf.begin(), utf.end()).c_str());
        const auto logUtf = (app / "log").u8string();
        ImGui::TextWrapped("%s: %s", tr("Logs", "日志"), std::string(logUtf.begin(), logUtf.end()).c_str());
        if (ImGui::Button(tr("Change cache folder...", "更改缓存目录…"))) {
                const auto selected = nativeDlgPickDir(tr("Cache folder", "缓存目录"));
                if (!selected.empty()) {
                        setCacheDir(selected);
                        nextRefresh = 0;
                }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(custom.empty());
        if (ImGui::Button(tr("Use system temp", "使用系统临时目录"))) {
                setCacheDir("");
                nextRefresh = 0;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s",
                            tr("Active logs and sampling files are kept. New buffers use the selected folder.",
                               "正在写入的日志和采样文件会保留；新缓冲区使用所选目录。"));
}
