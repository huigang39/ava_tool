#include "gui/i18n.hpp"
#include "gui/motor_control.hpp"
#include "gui/motor_gauge_average.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

void
MotorControl::positionDial(const MotorController::State &s, int mode)
{
        const auto &f            = s.snapshot.axes[axis_];
        const auto  display      = motorGaugeAverage(s, unsigned(axis_));
        const bool  positionMode = mode == 1 || mode == -1 || mode == 8 || mode == -8 || mode == -128;
        const bool  available    = s.connected && (f.flags & (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) == 3;
        const bool  live = available && !(f.flags & AVA_ETH_FAULT) && !s.error && (f.flags & AVA_ETH_ENABLED) && positionMode &&
                          f.mode == mode && f.special_cmd == 0;
        const ImVec4 blue(0.26f, 0.72f, 0.95f, 1), green(0.32f, 0.85f, 0.66f, 1), muted(0.57f, 0.65f, 0.74f, 1);
        const float  size = std::min(170.0f, ImGui::GetContentRegionAvail().x);
        if (size < 80)
                return;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - size) / 2));
        const auto   origin = ImGui::GetCursorScreenPos();
        const ImVec2 center(origin.x + size / 2, origin.y + size / 2);
        const float  radius       = size * 0.36f;
        dialCenterX_              = center.x;
        dialCenterY_              = center.y;
        dialRadius_               = radius;
        const auto  &range        = gaugeRanges_[0];
        const double scale        = 2 * PositionDialDrag::pi / range.span();
        auto         displayAngle = [&](double val) { return (val - range.minimum) * scale; };
        auto         point        = [&](double angle, float length) {
                return ImVec2(center.x + static_cast<float>(std::cos(angle)) * length,
                              center.y - static_cast<float>(std::sin(angle)) * length);
        };
        ImGui::BeginDisabled(!positionMode || !std::isfinite(position_) || (s.busy && !dialDragging_) || f.special_cmd != 0);
        ImGui::InvisibleButton("##positionDial", ImVec2(size, size));
        const auto   mouse = ImGui::GetIO().MousePos;
        const float  dx = mouse.x - center.x, dy = mouse.y - center.y;
        const double mouseAngle = std::atan2(-dy, dx);
        if (ImGui::IsItemActivated()) {
                const auto  tip = point(displayAngle(position_), radius * 0.84f);
                const float tx = tip.x - center.x, ty = tip.y - center.y;
                const float along    = std::clamp((dx * tx + dy * ty) / (tx * tx + ty * ty), 0.0f, 1.0f);
                const float distance = std::hypot(dx - along * tx, dy - along * ty);
                dialDragging_        = distance <= std::max(12.0f, size * 0.065f) && std::hypot(dx, dy) > radius * 0.2f;
                if (dialDragging_) {
                        // Start from the accepted goal instead of an unrelated unapplied edit
                        const double initial = live ? s.targets[axis_].position : position_;
                        position_            = static_cast<float>(initial);
                        dialDrag_.begin(displayAngle(initial), mouseAngle);
                }
        }
        if (dialDragging_ && ImGui::IsItemActive() && std::hypot(dx, dy) > radius * 0.2f) {
                const float target = static_cast<float>(range.minimum + dialDrag_.update(mouseAngle) / scale);
                if (std::isfinite(target) && target != position_) {
                        position_    = target;
                        dialPending_ = live;
                }
        }
        if (!ImGui::IsItemActive())
                dialDragging_ = false;
        if (!live)
                dialPending_ = false;
        if (dialPending_ && !s.busy && (ImGui::GetTime() - dialLastSend_ >= 0.05 || !dialDragging_)) {
                command(MotorController::Action::Apply);
                dialLastSend_ = ImGui::GetTime();
                dialPending_  = false;
        }
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::EndDisabled();
        auto      *draw = ImGui::GetWindowDrawList();
        const auto grey = ImGui::GetColorU32(muted), targetColor = ImGui::GetColorU32(positionMode ? blue : muted);
        draw->AddCircle(center, radius, grey, 96, 1.5f);
        for (int i = 0; i < 48; ++i) {
                const double angle = i * PositionDialDrag::pi / 24;
                draw->AddLine(point(angle, radius - (i % 4 ? 4 : 10)), point(angle, radius - 1), grey);
        }
        if (available && std::isfinite(display.position))
                draw->AddLine(center, point(displayAngle(display.position), radius * 0.7f), ImGui::GetColorU32(green), 2);
        const auto tip = point(displayAngle(std::isfinite(position_) ? position_ : 0), radius * 0.84f);
        draw->AddLine(center, tip, targetColor, 3);
        draw->AddCircleFilled(tip, hovered || dialDragging_ ? 8 : 6, targetColor);
        draw->AddCircleFilled(center, 5, grey);
        for (int i = 0; i < 4; ++i) {
                char label[32];
                snprintf(label,
                         sizeof(label),
                         "%.3g",
                         (range.minimum + range.span() * i / 4) * motor_units::positionScale(positionUnit_));
                const auto p    = point(i * PositionDialDrag::pi / 2, radius + 18);
                const auto text = ImGui::CalcTextSize(label);
                draw->AddText(ImVec2(p.x - text.x / 2, p.y - text.y / 2), grey, label);
        }
        const auto unit   = motor_units::positionLabel(positionUnit_);
        const auto factor = motor_units::positionScale(positionUnit_);
        if (s.snapshot.count > unsigned(axis_) && std::isfinite(display.position)) {
                const auto turns = motor_units::splitTurns(display.position);
                ImGui::Text("%s: %.0f %s + %.3f %s",
                            available ? tr("Position", "位置") : tr("Last", "最后位置"),
                            turns.count,
                            tr("turns", "圈"),
                            turns.angle * factor,
                            unit);
                ImGui::TextDisabled("%s %.3f %s", tr("Total", "多圈"), display.position * factor, unit);
        } else
                ImGui::Text("— %s", unit);
        if (std::isfinite(position_)) {
                const auto target = motor_units::splitTurns(position_);
                ImGui::TextDisabled(
                    "%s %.0f %s + %.3f %s", tr("Target", "目标"), target.count, tr("turns", "圈"), target.angle * factor, unit);
        }

        if (hovered)
                ImGui::SetTooltip("%s",
                                  !positionMode ? tr("Choose a position control mode", "请选择位置控制模式")
                                  : live        ? tr("Blue: target; green: feedback. Drag to send position targets.",
                                              "蓝色为目标，绿色为反馈；拖拽指针实时发送位置目标。")
                                                : tr("Drag to edit; confirm with Enter in the target input / enable",
                                              "拖拽修改目标后，在目标输入框回车确认／使能。"));
}

void
MotorControl::interpolationSettings(const MotorController::State &s)
{
        const auto &f = s.snapshot.axes[axis_];
        ImGui::BeginDisabled(s.busy || (f.flags & AVA_ETH_ENABLED));
        if (ImGui::Checkbox(tr("Position interpolation", "位置插补"), &interpolation_))
                modified_ = true;
        if (interpolation_) {
                ImGui::SetNextItemWidth(100);
                ImGui::InputFloat(tr("Max speed  rad/s", "最大速度  rad/s"), &maxPositionVelocity_, 0, 0, "%.3f");
                ImGui::SetNextItemWidth(100);
                ImGui::InputFloat(
                    tr("Max acceleration  rad/s²", "最大加速度  rad/s²"), &maxPositionAcceleration_, 0, 0, "%.3f");
                if (!std::isfinite(maxPositionVelocity_) || !std::isfinite(maxPositionAcceleration_) ||
                    maxPositionVelocity_ <= 0 || maxPositionVelocity_ > 65504 || maxPositionAcceleration_ <= 0 ||
                    maxPositionAcceleration_ > 1000000)
                        ImGui::TextColored(
                            ImVec4(1, .4f, .3f, 1), "%s", tr("Enter positive finite limits", "请输入有效的正数限值"));
                ImGui::TextDisabled("%s", tr("CSP / CSPC only; change limits while disabled", "仅 CSP / CSPC；失能后修改限值"));
        }
        ImGui::EndDisabled();
}
