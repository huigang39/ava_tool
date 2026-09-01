#include "gui/choice_control.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/motor_control.hpp"
#include "gui/motor_gauge_average.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace
{
bool
available(const MotorController::State &s, int axis)
{
        return s.connected && axis < int(s.snapshot.count) &&
               (s.snapshot.axes[axis].flags & (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) == 3;
}
void
gauge(const char *label, float val, const char *unit, bool valid, float minimum, float maximum, const char *note = nullptr)
{
        ImGui::TextUnformatted(label);
        const float  width  = ImGui::GetContentRegionAvail().x;
        const float  radius = std::min(78.0f, width * .40f);
        const auto   origin = ImGui::GetCursorScreenPos();
        const ImVec2 center(origin.x + width / 2, origin.y + radius + 8);
        auto        *draw = ImGui::GetWindowDrawList();
        const auto   grey = ImGui::GetColorU32(ImGuiCol_TextDisabled), blue = ImGui::GetColorU32(ImGuiCol_Text);
        const float  start = 2.35f, sweep = 4.72f;
        auto         point = [&](float angle, float r) {
                return ImVec2(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
        };
        draw->PathArcTo(center, radius, start, start + sweep, 64);
        draw->PathStroke(grey, 0, 1);
        for (int i = 0; i <= 20; ++i) {
                const float a = start + sweep * i / 20;
                draw->AddLine(point(a, radius - (i % 5 ? 5 : 11)), point(a, radius + 1), grey);
        }
        if (valid && std::isfinite(val)) {
                const float a =
                    start + sweep * float(std::clamp((double(val) - minimum) / (double(maximum) - minimum), 0.0, 1.0));
                draw->AddLine(center, point(a, radius - 15), blue, 1.5f);
        }
        draw->AddCircleFilled(center, 5, grey);
        char text[80];
        if (valid && std::isfinite(val))
                snprintf(text, sizeof(text), "%.4g %s", val, unit);
        else
                snprintf(text, sizeof(text), "— %s", unit);
        auto size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(center.x - size.x / 2, center.y + radius * .45f), valid ? blue : grey, text);
        ImGui::Dummy(ImVec2(width, radius * 2 + 22));
        if (note)
                ImGui::TextWrapped("%s", note);
}
} // namespace

void
MotorControl::gaugeRangeEditor(int index)
{
        auto        &range   = gaugeRanges_[index];
        const char  *units[] = {motor_units::positionLabel(positionUnit_),
                                motor_units::velocityLabel(velocityUnit_),
                                "A",
                                "Nm",
                                "Nm",
                                "V",
                                "°C",
                                "°C",
                                "°C"};
        const double factor  = index == 0   ? motor_units::positionScale(positionUnit_)
                               : index == 1 ? motor_units::velocityScale(velocityUnit_)
                                            : 1;
        ImGui::TextDisabled("%g … %g %s", range.minimum * factor, range.maximum * factor, units[index]);
        if (ImGui::Button(tr("Range...", "量程…"))) {
                rangeDraft_ = {float(range.minimum * factor), float(range.maximum * factor)};
                ImGui::OpenPopup("##gaugeRange");
        }
        if (ImGui::BeginPopup("##gaugeRange")) {
                ImGui::Text("%s (%s)", tr("Display range", "显示量程"), units[index]);
                ImGui::SetNextItemWidth(150);
                ImGui::InputFloat(tr("Minimum", "下限"), &rangeDraft_.minimum, 0, 0, "%.6g");
                ImGui::SetNextItemWidth(150);
                ImGui::InputFloat(tr("Maximum", "上限"), &rangeDraft_.maximum, 0, 0, "%.6g");
                if (!rangeDraft_.valid())
                        ImGui::TextUnformatted(
                            tr("Use finite values with minimum < maximum.", "请输入有效数值，且下限必须小于上限。"));
                ImGui::TextUnformatted(tr("Display scale only; does not change motor commands or limits.",
                                          "仅调整显示刻度，不修改电机指令或限值。"));
                const MotorGaugeRange converted{float(rangeDraft_.minimum / factor), float(rangeDraft_.maximum / factor)};
                ImGui::BeginDisabled(!rangeDraft_.valid() || !converted.valid());
                if (ImGui::Button(tr("Apply", "应用"))) {
                        range     = converted;
                        modified_ = true;
                        if (index == 0)
                                dialDragging_ = dialPending_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(tr("Default", "恢复默认"))) {
                        range     = defaultMotorGaugeRanges[index];
                        modified_ = true;
                        if (index == 0)
                                dialDragging_ = dialPending_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
}

void
MotorControl::dashboard(const MotorController::State &s)
{
        const auto f  = motorGaugeAverage(s, unsigned(axis_));
        const bool ok = available(s, axis_), thermal = ok && (f.flags & AVA_ETH_TEMPERATURE_VALID);
        ImGui::TextDisabled("%s", tr("Gauge feedback: 200 ms moving average", "仪表反馈：最近 200 ms 平均值"));
        const int columns = ImGui::GetContentRegionAvail().x >= 570 ? 3 : ImGui::GetContentRegionAvail().x >= 350 ? 2 : 1;
        if (ImGui::BeginTable("instrumentGrid", columns, ImGuiTableFlags_SizingStretchSame)) {
                for (int i = 0; i < 9; ++i) {
                        ImGui::TableNextColumn();
                        ImGui::PushID(i);
                        ImGui::BeginChild("gauge", ImVec2(0, 370), ImGuiChildFlags_Borders);
                        if (i < 2) {
                                ImGui::SetNextItemWidth(100);
                                int &unit = i == 0 ? positionUnit_ : velocityUnit_;
                                if (ui::Choice("##unit", &unit, i == 0 ? "rad\0deg\0" : "rad/s\0deg/s\0rpm\0")) {
                                        targetDrafts_[i] = {};
                                        modified_        = true;
                                }
                        }
                        const auto &range = gaugeRanges_[i];
                        switch (i) {
                                case 0:
                                        ImGui::TextUnformatted(tr("Position", "位置"));
                                        positionDial(s, selectedMode());
                                        break;
                                case 1:
                                        gauge(tr("Velocity", "速度"),
                                              float(f.velocity * motor_units::velocityScale(velocityUnit_)),
                                              motor_units::velocityLabel(velocityUnit_),
                                              ok,
                                              float(range.minimum * motor_units::velocityScale(velocityUnit_)),
                                              float(range.maximum * motor_units::velocityScale(velocityUnit_)));
                                        break;
                                case 2:
                                        gauge(tr("Current", "电流"), f.current, "A", ok, range.minimum, range.maximum);
                                        break;
                                case 3:
                                        gauge(tr("Electromagnetic torque", "电磁转矩"),
                                              f.electromagnetic_torque,
                                              "Nm",
                                              ok && (f.flags & AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID),
                                              range.minimum,
                                              range.maximum,
                                              (f.flags & AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID)
                                                  ? nullptr
                                                  : tr("No valid electromagnetic torque feedback", "无有效电磁转矩反馈"));
                                        break;
                                case 4:
                                        gauge(tr("Load torque", "负载转矩"), f.torque, "Nm", ok, range.minimum, range.maximum);
                                        break;
                                case 5:
                                        gauge(tr("Bus voltage", "母线电压"), f.vbus, "V", ok, range.minimum, range.maximum);
                                        break;
                                case 6:
                                        gauge(tr("Inverter temperature", "逆变器温度"),
                                              f.drive_temperature,
                                              "°C",
                                              thermal,
                                              range.minimum,
                                              range.maximum);
                                        break;
                                case 7:
                                        gauge(tr("Winding temperature", "绕组温度"),
                                              f.motor_temperature,
                                              "°C",
                                              thermal,
                                              range.minimum,
                                              range.maximum);
                                        break;
                                case 8:
                                        gauge(tr("MCU temperature", "MCU 温度"),
                                              0,
                                              "°C",
                                              false,
                                              range.minimum,
                                              range.maximum,
                                              tr("Not provided by SDK", "SDK 未提供"));
                                        break;
                        }
                        gaugeRangeEditor(i);
                        ImGui::EndChild();
                        ImGui::PopID();
                }
                ImGui::EndTable();
        }
}

MotorControl::MotorControl() = default;
MotorControl::~MotorControl()
{
        controller_.shutdown();
        if (monitorFft_.valid())
                monitorFft_.wait();
}

void
MotorControl::variables(const MotorController::State &s)
{
        std::unique_lock lock(monitorMutex_);
        if (!monitor_) {
                monitor_                  = std::make_unique<Monitor>("motorVariableMonitor");
                monitor_->externalFeed_   = true;
                monitor_->hideAxisLabels_ = true;
                monitor_->historySeconds_ = 30;
                const char *names[]       = {"Position", "Velocity", "Current", "Torque", "Temperature", "Voltage"};
                const char *labels[]      = {tr("Position", "位置"),
                                             tr("Velocity", "速度"),
                                             tr("Current", "电流"),
                                             tr("Torque", "转矩"),
                                             tr("Temperature", "温度"),
                                             tr("Bus voltage", "母线电压")};
                const char *units[]       = {"rad", "rad/s", "A", "Nm", "°C", "V"};
                for (int i = 0; i < 6; ++i) {
                        monitor_->addScope(names[i]);
                        auto sc = monitor_->getScopes().at(names[i]);
                        sc->setLabel(labels[i]);
                        sc->setAxisLabels("s", units[i]);
                        auto channel = [&](const char *key, const char *label, int index) {
                                sc->addChannel(key);
                                auto ch = sc->getChannels().at(key);
                                ch->setLabel(label);
                                ch->setType("f32");
                                ch->setDevice("Motor");
                                ch->setWritable(false);
                                ch->historySeconds_     = 30;
                                monitorChannels_[index] = ch;
                        };
                        if (i < 4) {
                                channel("feedback", i == 3 ? tr("Load torque", "负载转矩") : tr("Feedback", "反馈"), i);
                                channel("target", tr("Target", "目标"), i + 7);
                                if (i == 3)
                                        channel("electromagnetic_torque", tr("Electromagnetic torque", "电磁转矩"), 11);
                        } else if (i == 4) {
                                channel("motor_temperature", tr("Winding", "绕组"), 4);
                                channel("drive_temperature", tr("Inverter", "逆变器"), 5);
                                sc->setHidden(true);
                        } else {
                                channel("vbus", tr("Bus voltage", "母线电压"), 6);
                                sc->setHidden(true);
                        }
                }
        }
        if (monitorPositionUnit_ != positionUnit_ || monitorVelocityUnit_ != velocityUnit_) {
                monitorPositionUnit_ = positionUnit_;
                monitorVelocityUnit_ = velocityUnit_;
                // Rebuild from raw SI history, never mix units in the same trace
                monitor_->clearData();
                lastMonitorSample_ = -1;
                for (auto &[_, sc] : monitor_->getScopes())
                        for (auto &[__, ch] : sc->getChannels()) {
                                if (ch == monitorChannels_[0])
                                        sc->setAxisLabels("s", motor_units::positionLabel(positionUnit_));
                                if (ch == monitorChannels_[1])
                                        sc->setAxisLabels("s", motor_units::velocityLabel(velocityUnit_));
                        }
        }
        if (monitorAxis_ != axis_ || (s.connected && !monitorConnected_)) {
                monitor_->clearData();
                monitor_->showFullData();
                lastMonitorSample_ = -1;
                monitorAxis_       = axis_;
        }
        monitorConnected_ = s.connected;
        if (monitor_->consumeClearDataRequest()) {
                monitor_->clearData();
                if (!s.history.empty())
                        lastMonitorSample_ = s.history.back().seconds;
        }
        monitor_->processDiscardRequests();
        std::unordered_set<MonitorChannel *> activeChannels;
        for (auto &[_, sc] : monitor_->getScopes()) {
                sc->purgeDeleted();
                if (!sc->isPaused() && !sc->isPendingDelete())
                        for (auto &[__, ch] : sc->getChannels())
                                activeChannels.insert(ch.get());
        }
        for (const auto &sample : s.history) {
                if (sample.seconds <= lastMonitorSample_)
                        continue;
                lastMonitorSample_ = sample.seconds;
                if (monitor_->isSamplingPaused())
                        continue;
                const auto &f        = sample.feedback;
                const auto &t        = sample.target;
                const bool  ok       = (f.flags & (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) == 3;
                const bool  temp     = ok && (f.flags & AVA_ETH_TEMPERATURE_VALID);
                const float nan      = std::numeric_limits<float>::quiet_NaN();
                const float values[] = {ok ? float(f.position * motor_units::positionScale(positionUnit_)) : nan,
                                        ok ? float(f.velocity * motor_units::velocityScale(velocityUnit_)) : nan,
                                        ok ? f.current : nan,
                                        ok ? f.torque : nan,
                                        temp ? f.motor_temperature : nan,
                                        temp ? f.drive_temperature : nan,
                                        ok ? f.vbus : nan,
                                        float(t.position * motor_units::positionScale(positionUnit_)),
                                        float(t.velocity * motor_units::velocityScale(velocityUnit_)),
                                        t.current,
                                        t.torque,
                                        ok && (f.flags & AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID) ? f.electromagnetic_torque
                                                                                               : nan};
                for (int i = 0; i < 12; ++i)
                        if (monitorChannels_[i] && activeChannels.count(monitorChannels_[i].get()))
                                monitorChannels_[i]->setRVal(values[i], sample.seconds);
                monitor_->addPoints(1);
        }
        for (auto &[_, sc] : monitor_->getScopes()) {
                sc->purgeDeleted();
                if (!g_monitorPaused && !sc->isPaused())
                        for (auto &[__, ch] : sc->getChannels())
                                ch->publishSnapshot();
        }
        monitor_->updateHz();
        monitor_->updateDisplay(true);
        if (!monitorFft_.valid() || monitorFft_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                if (monitorFft_.valid())
                        monitorFft_.get();
                std::vector<std::shared_ptr<MonitorScope>> scopes;
                for (auto &[_, sc] : monitor_->getScopes())
                        if (sc->isFftEnabled())
                                scopes.push_back(sc);
                if (!scopes.empty())
                        monitorFft_ = std::async(std::launch::async, [this, scopes] {
                                for (const auto &sc : scopes)
                                        sc->fftWorkerStep(monitorMutex_);
                        });
        }
}
