#include "gui/dso_scope.hpp"
#include "cJSON.h"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/ui_theme.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
constexpr auto inputFlags = ImGuiInputTextFlags_AutoSelectAll;
bool
scalar(const char *label, double &v)
{
        return ImGui::InputDouble(label, &v, 0, 0, "%.9g", inputFlags);
}
const char *
actionName(dso::Action a)
{
        using A = dso::Action;
        switch (a) {
                case A::Connect:
                        return "连接";
                case A::Disconnect:
                        return "断开";
                case A::ReadSettings:
                        return "读取设置";
                case A::Channel:
                        return "通道设置";
                case A::Timebase:
                        return "时基设置";
                case A::Trigger:
                        return "触发设置";
                case A::Run:
                        return "运行";
                case A::Stop:
                        return "停止";
                case A::Single:
                        return "单次触发";
                case A::Digitize:
                        return "采集指定通道";
                case A::Autoscale:
                        return "自动设置";
                case A::Reset:
                        return "复位";
                case A::Capture:
                        return "读取波形";
                case A::Measure:
                        return "测量";
                case A::Write:
                        return "SCPI 写入";
                case A::Query:
                        return "SCPI 查询";
                case A::Error:
                        return "仪器错误";
        }
        return "";
}
} // namespace
DsoScope::~DsoScope()
{
        shutdown();
}
void
DsoScope::shutdown()
{
        autoRead_ = false;
        controller_.cancel();
        if (pending_.valid())
                pending_.get();
        if (fft_.valid())
                fft_.get();
        controller_.close();
        connected_         = false;
        disconnectPending_ = false;
}
void
DsoScope::setOpen(bool open)
{
        if (open_ == open)
                return;
        open_     = open;
        modified_ = true;
        if (!open)
                disconnect();
}
void
DsoScope::disconnect()
{
        autoRead_ = false;
        controller_.cancel();
        disconnectPending_ = true;
}
void
DsoScope::submit(dso::Action action)
{
        if (busy() || disconnectPending_)
                return;
        config_.host      = host_;
        config_.resource  = resource_;
        config_.command   = command_;
        const auto config = config_;
        active_           = action;
        controller_.resume();
        pending_ = std::async(std::launch::async, [this, action, config] { return controller_.execute(action, config); });
}
void
DsoScope::poll()
{
        if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = pending_.get();
                connected_  = result.connected;
                identity_   = result.identity;
                error_      = result.error;
                status_     = std::string(actionName(active_)) + ": " + result.message;
                if (result.error)
                        autoRead_ = false;
                if (active_ != dso::Action::Capture || result.error) {
                        log_.push_back(status_);
                        if (log_.size() > 100)
                                log_.pop_front();
                }
                if (result.settingsValid && !disconnectPending_) {
                        config_.channels     = result.settings.channels;
                        config_.timeScale    = result.settings.timeScale;
                        config_.timePosition = result.settings.timePosition;
                        if (active_ == dso::Action::Connect) {
                                for (int i = 0; i < 4; ++i)
                                        config_.capture[i] = config_.channels[i].enabled;
                                measurementsValid_ = false;
                                std::lock_guard lock(monitorMutex_);
                                if (monitor_)
                                        monitor_->clearData();
                        }
                }
                if (result.captured && !disconnectPending_)
                        publish(result);
                if (result.measured) {
                        measurements_      = result.measurements;
                        measurementsValid_ = true;
                }
                nextRead_ = ImGui::GetTime() + std::max(100, interval_) / 1000.0;
        }
        if (disconnectPending_ && !busy()) {
                disconnectPending_ = false;
                submit(dso::Action::Disconnect);
        }
}
void
DsoScope::ensureMonitor()
{
        if (monitor_)
                return;
        monitor_                   = std::make_unique<Monitor>("DSOX2024A_monitor");
        monitor_->externalFeed_    = true;
        monitor_->triggerTimeAxis_ = true;
        monitor_->historySeconds_  = 0;
        monitor_->addScope("DSOX2024A");
        auto sc = monitor_->getScopes().at("DSOX2024A");
        sc->setAxisLabels("s", "V");
        const float colors[4][4] = {{1, .85f, .1f, 1}, {.2f, .9f, .3f, 1}, {.3f, .65f, 1, 1}, {1, .35f, .75f, 1}};
        for (int i = 0; i < 4; ++i) {
                const auto name = "CH" + std::to_string(i + 1);
                sc->addChannel(name);
                auto ch = sc->getChannels().at(name);
                ch->setType("f32");
                ch->setDevice("DSOX");
                ch->setWritable(false);
                ch->historySeconds_ = 0;
                ch->useAutoColor()  = false;
                std::copy(colors[i], colors[i] + 4, ch->getColor());
        }
}
void
DsoScope::publish(const dso::Result &r)
{
        std::lock_guard lock(monitorMutex_);
        ensureMonitor();
        if (g_monitorPaused || monitor_->isSamplingPaused())
                return;
        double earliest = 0;
        bool   first    = true;
        for (auto &[_, sc] : monitor_->getScopes()) {
                if (sc->isPaused())
                        continue;
                for (int i = 0; i < 4; ++i) {
                        auto *ch = sc->findChannel("CH" + std::to_string(i + 1));
                        if (!ch)
                                continue;
                        ch->clearData();
                        const auto &w = r.waves[i];
                        if (w.values.empty())
                                continue;
                        if (first || w.times.front() < earliest)
                                earliest = w.times.front();
                        first               = false;
                        ch->historySeconds_ = 0;
                        ch->pushBatch(w.values.data(), w.times.data(), w.values.size());
                        ch->publishSnapshot();
                        monitor_->addPoints(w.values.size());
                }
        }
        monitor_->dataStartTime_ = earliest;
        monitor_->updateHz();
}
void
DsoScope::controls()
{
        using A     = dso::Action;
        auto button = [&](const char *label, A a) {
                if (ImGui::Button(label))
                        submit(a);
        };
        ImGui::BeginDisabled(busy() || connected_);
        int transport = config_.usb ? 0 : 1;
        if (ui::Choice("连接方式", &transport, "USB / VISA\0LAN / TCP\0")) {
                config_.usb = transport == 0;
                modified_   = true;
        }
        if (config_.usb) {
                modified_ |= ImGui::InputText("VISA 资源", resource_, sizeof(resource_), inputFlags);
                ImGui::TextWrapped("留空自动查找 DSOX2024A。USB 连接需要安装 64 位 VISA（Keysight IO Libraries 或 NI-VISA）。");
        } else {
                modified_ |= ImGui::InputText("IP / 主机", host_, sizeof(host_), inputFlags);
                modified_ |= ImGui::InputInt("TCP 端口", &config_.port, 0, 0, inputFlags);
        }
        modified_ |= ImGui::InputInt("超时 (ms)", &config_.timeout, 0, 0, inputFlags);
        ImGui::EndDisabled();
        if (connected_ || busy()) {
                ImGui::BeginDisabled(disconnectPending_ || active_ == A::Disconnect);
                if (ui::Button("断开连接", ui::BtnStyle::Danger, ImVec2(150, 28)))
                        disconnect();
                ImGui::EndDisabled();
        } else if (ui::Button("连接", ui::BtnStyle::Success, ImVec2(150, 28)))
                submit(A::Connect);
        ImGui::Separator();
        ImGui::BeginDisabled(!connected_ || busy() || disconnectPending_);
        button("运行", A::Run);
        ImGui::SameLine();
        button("停止", A::Stop);
        ImGui::SameLine();
        button("单次", A::Single);
        button("自动设置", A::Autoscale);
        ImGui::SameLine();
        button("读取设置", A::ReadSettings);
        ImGui::SameLine();
        if (ImGui::Button("复位仪器"))
                ImGui::OpenPopup("确认复位示波器");
        if (ImGui::BeginPopupModal("确认复位示波器", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("将恢复示波器的默认设置。确认复位？");
                if (ui::Button("确认复位", ui::BtnStyle::Danger)) {
                        submit(A::Reset);
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消"))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
        if (ImGui::CollapsingHeader("通道设置", ImGuiTreeNodeFlags_DefaultOpen)) {
                ui::Choice("通道", &config_.channel, "CH1\0CH2\0CH3\0CH4\0");
                auto &ch = config_.channels[config_.channel];
                ImGui::Checkbox("显示通道", &ch.enabled);
                scalar("垂直量程 (V/div)", ch.scale);
                scalar("垂直偏置 (V)", ch.offset);
                scalar("探头倍率", ch.probe);
                ui::Choice("耦合", &ch.coupling, "DC\0AC\0");
                button("应用通道设置", A::Channel);
                ImGui::SameLine();
                button("采集此通道", A::Digitize);
        }
        if (ImGui::CollapsingHeader("时基与触发", ImGuiTreeNodeFlags_DefaultOpen)) {
                scalar("时基 (s/div)", config_.timeScale);
                scalar("时间偏移 (s)", config_.timePosition);
                button("应用时基", A::Timebase);
                ui::Choice("边沿触发源", &config_.triggerChannel, "CH1\0CH2\0CH3\0CH4\0");
                scalar("触发电平 (V)", config_.triggerLevel);
                ui::Choice("触发边沿", &config_.slope, "上升沿\0下降沿\0任一边沿\0");
                button("应用触发", A::Trigger);
                ImGui::TextDisabled("触发栏为待应用值；点击应用后下发。");
        }
        if (ImGui::CollapsingHeader("仪器测量", ImGuiTreeNodeFlags_DefaultOpen)) {
                button("测量所选通道", A::Measure);
                if (measurementsValid_ &&
                    ImGui::BeginTable("measurements", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        for (auto name : {"通道", "频率 (Hz)", "Vpp (V)", "RMS (V)"})
                                ImGui::TableSetupColumn(name);
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 4; ++i) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("CH%d", i + 1);
                                for (double val : measurements_[i]) {
                                        ImGui::TableNextColumn();
                                        if (std::isfinite(val))
                                                ImGui::Text("%.7g", val);
                                        else
                                                ImGui::TextUnformatted("—");
                                }
                        }
                        ImGui::EndTable();
                }
        }
        if (ImGui::CollapsingHeader("SCPI 指令")) {
                ImGui::InputText("指令", command_, sizeof(command_), inputFlags);
                button("写入", A::Write);
                ImGui::SameLine();
                button("查询文本", A::Query);
                ImGui::SameLine();
                button("读取仪器错误", A::Error);
        }
        ImGui::EndDisabled();
        if (ImGui::CollapsingHeader("操作日志")) {
                std::string log;
                for (const auto &line : log_)
                        log += line + "\n";
                ui::CopyLogButton(log);
                ui::LogView("##dso_log", log, ImVec2(0, 200));
        }
}
void
DsoScope::waveforms()
{
        using A = dso::Action;
        ImGui::BeginDisabled(busy());
        for (int i = 0; i < 4; ++i) {
                if (i)
                        ImGui::SameLine();
                ImGui::Checkbox(("CH" + std::to_string(i + 1)).c_str(), &config_.capture[i]);
        }
        ImGui::SetNextItemWidth(110);
        modified_ |= ImGui::InputInt("点/通道", &config_.points, 0, 0, inputFlags);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        modified_ |= ImGui::InputInt("间隔 (ms)", &interval_, 0, 0, inputFlags);
        interval_  = std::clamp(interval_, 100, 60000);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!connected_ || busy() || disconnectPending_);
        if (ImGui::Button("读取波形"))
                submit(A::Capture);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected_ || disconnectPending_);
        if (ImGui::Checkbox("自动刷新波形", &autoRead_))
                nextRead_ = 0;
        ImGui::EndDisabled();
        ImGui::TextWrapped("运行时各通道依次读取；比较同次采集请先停止示波器，再读取波形。横轴保留触发前的负时间。");
        std::lock_guard lock(monitorMutex_);
        ensureMonitor();
        if (monitor_->consumeClearDataRequest())
                monitor_->clearData();
        monitor_->processDiscardRequests();
        ImGui::BeginChild("scopeMonitor");
        monitor_->updateDisplay(true);
        ImGui::EndChild();
        if (!fft_.valid() || fft_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                if (fft_.valid())
                        fft_.get();
                std::vector<std::shared_ptr<MonitorScope>> scopes;
                for (auto &[_, sc] : monitor_->getScopes())
                        if (sc->isFftEnabled())
                                scopes.push_back(sc);
                if (!scopes.empty())
                        fft_ = std::async(std::launch::async, [this, scopes] {
                                for (auto &sc : scopes)
                                        sc->fftWorkerStep(monitorMutex_);
                        });
        }
}
void
DsoScope::draw()
{
        poll();
        if (!open_)
                return;
        ImGui::SetNextWindowSize(ImVec2(1200, 750), ImGuiCond_FirstUseEver);
        bool visible = dock_close::begin("DSOX2024A 示波器###DSOX2024A", &open_);
        if (!open_) {
                modified_ = true;
                disconnect();
        }
        if (visible)
                drawEmbedded();
        ImGui::End();
}
void
DsoScope::drawEmbedded()
{
        TutorialGuide::instance().markDock(TutorialGuide::Topic::DSO);
        poll();
        {
                ui::TextColored(
                    connected_ ? ImVec4(.3f, .9f, .4f, 1) : ImVec4(.9f, .5f, .3f, 1), "%s", connected_ ? "已连接" : "未连接");
                ImGui::SameLine();
                ImGui::TextUnformatted(identity_.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, error_ ? ui::ErrorTextColor() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ImGui::TextWrapped("%s%s", busy() ? "通信中… " : "", status_.c_str());
                ImGui::PopStyleColor();
                if (ImGui::BeginTable("dsoWorkspaceWaveLeft", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("waveforms", ImGuiTableColumnFlags_WidthStretch, .68f);
                        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, .32f);
                        ImGui::TableNextColumn();
                        waveforms();
                        ImGui::TableNextColumn();
                        ImGui::BeginChild("controls");
                        ImGui::PushItemWidth(150);
                        controls();
                        ImGui::PopItemWidth();
                        ImGui::EndChild();
                        ImGui::EndTable();
                }
                if (autoRead_ && connected_ && !busy() && !disconnectPending_ && ImGui::GetTime() >= nextRead_ &&
                    !g_monitorPaused && (!monitor_ || !monitor_->isSamplingPaused()))
                        submit(dso::Action::Capture);
        }
}
void
DsoScope::save(cJSON *root) const
{
        auto *o = cJSON_AddObjectToObject(root, "dsox2024a");
        cJSON_AddBoolToObject(o, "open", open_);
        cJSON_AddBoolToObject(o, "usb", config_.usb);
        cJSON_AddStringToObject(o, "host", host_);
        cJSON_AddStringToObject(o, "resource", resource_);
        cJSON_AddNumberToObject(o, "port", config_.port);
        cJSON_AddNumberToObject(o, "timeout", config_.timeout);
        cJSON_AddNumberToObject(o, "points", config_.points);
        cJSON_AddNumberToObject(o, "interval", interval_);
}
void
DsoScope::load(const cJSON *root)
{
        shutdown();
        monitor_.reset();
        config_  = {};
        host_[0] = resource_[0] = 0;
        open_                   = false;
        status_                 = "未连接";
        identity_.clear();
        log_.clear();
        measurementsValid_ = false;
        const auto *o      = root ? cJSON_GetObjectItem(root, "dsox2024a") : nullptr;
        if (!o)
                return;
        open_ = cJSON_IsTrue(cJSON_GetObjectItem(o, "open"));
        if (auto *v = cJSON_GetObjectItem(o, "usb"); cJSON_IsBool(v))
                config_.usb = cJSON_IsTrue(v);
        for (auto item : {std::pair{"host", std::pair{host_, sizeof(host_)}}, {"resource", {resource_, sizeof(resource_)}}})
                if (auto *v = cJSON_GetObjectItem(o, item.first); cJSON_IsString(v))
                        snprintf(item.second.first, item.second.second, "%s", v->valuestring);
        auto read = [&](const char *key, int &n, int lo, int hi) {
                if (auto *v = cJSON_GetObjectItem(o, key); cJSON_IsNumber(v))
                        n = std::clamp(v->valueint, lo, hi);
        };
        read("port", config_.port, 1, 65535);
        read("timeout", config_.timeout, 100, 10000);
        read("points", config_.points, 16, 4000000);
        read("interval", interval_, 100, 60000);
}
