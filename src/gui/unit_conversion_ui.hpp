#ifndef AVA_UNIT_CONVERSION_UI_HPP
#define AVA_UNIT_CONVERSION_UI_HPP

#include "core/unit_conversion.hpp"
#include "gui/choice_control.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace ui
{
namespace unit_hover_detail
{
inline constexpr const char *windowName = "###UnitConversionHover";
struct State {
        ImGuiContext             *context{};
        ImGuiID                   id{};
        int                       seenFrame{-1};
        double                    started{}, keepUntil{};
        bool                      visible{}, editing{};
        ImVec2                    anchor{};
        ImRect                    rect{};
        std::optional<double>     capturedValue;
        unit_conversion::Settings draft;
};
inline State state;

inline const char *
quantityLabel(unit_conversion::Quantity quantity)
{
        using unit_conversion::Quantity;
        switch (quantity) {
                case Quantity::AngularPosition:
                        return tr("Angular position", "角位置");
                case Quantity::AngularSpeed:
                        return tr("Rotational speed", "转速");
                case Quantity::LinearPosition:
                        return tr("Linear position", "直线位置");
                case Quantity::LinearSpeed:
                        return tr("Linear speed", "直线速度");
        }
        return "?";
}

inline const char *
errorLabel(unit_conversion::Error error)
{
        using unit_conversion::Error;
        switch (error) {
                case Error::InvalidCountsPerRevolution:
                        return tr("Enter a finite, positive encoder count per revolution.", "请填写大于零的每圈编码器计数。");
                case Error::NonFinite:
                        return tr("The value or result is outside the numeric range.", "数值或换算结果超出有效范围。");
                case Error::IncompatibleUnits:
                        return tr("Choose compatible source and target units.", "请选择同一物理量的原始单位和目标单位。");
                default:
                        return "";
        }
}

inline bool
hasPopup()
{
        auto *window = ImGui::FindWindowByName(windowName);
        if (!window)
                return false;
        for (const auto &popup : GImGui->OpenPopupStack)
                if (popup.Window && ImGui::IsWindowChildOf(popup.Window, window, true, false))
                        return true;
        return false;
}

inline void
closePopups()
{
        auto *window = ImGui::FindWindowByName(windowName);
        if (!window)
                return;
        for (int i = 0; i < GImGui->OpenPopupStack.Size; ++i)
                if (GImGui->OpenPopupStack[i].Window &&
                    ImGui::IsWindowChildOf(GImGui->OpenPopupStack[i].Window, window, true, false)) {
                        ImGui::ClosePopupToLevel(i, true);
                        break;
                }
}

inline void
unitCombo(const char *label, unit_conversion::Quantity quantity, unit_conversion::Unit &unit)
{
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        ImGui::PushID(label);
        std::vector<const char *>          labels;
        std::vector<unit_conversion::Unit> values;
        int                                selected = -1;
        for (int i = 0; i <= static_cast<int>(unit_conversion::Unit::MeterPerMinute); ++i) {
                const auto candidate = static_cast<unit_conversion::Unit>(i);
                if (unit_conversion::QuantityOf(candidate) != quantity)
                        continue;
                if (candidate == unit)
                        selected = static_cast<int>(values.size());
                labels.push_back(unit_conversion::UnitLabel(candidate));
                values.push_back(candidate);
        }
        if (ui::Choice("##unit", &selected, labels.data(), static_cast<int>(labels.size())))
                unit = values[selected];
        ImGui::PopID();
}
} // namespace unit_hover_detail

inline bool
UnitConversionFields(unit_conversion::Settings &draft)
{
        using namespace unit_hover_detail;
        const char *quantities[4];
        for (int i = 0; i < 4; ++i)
                quantities[i] = quantityLabel(static_cast<unit_conversion::Quantity>(i));
        int quantity = static_cast<int>(draft.quantity);
        if (ui::Choice("##quantity", &quantity, quantities, 4)) {
                const double counts       = draft.countsPerRevolution;
                draft                     = unit_conversion::DefaultSettings(static_cast<unit_conversion::Quantity>(quantity));
                draft.countsPerRevolution = counts;
        }
        unitCombo(tr("Source unit", "原始单位"), draft.quantity, draft.source);
        unitCombo(tr("Display unit", "目标单位"), draft.quantity, draft.target);
        if (unit_conversion::UsesCounts(draft.source) || unit_conversion::UsesCounts(draft.target)) {
                ImGui::TextWrapped("%s", tr("Encoder counts per revolution", "编码器每圈计数"));
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##countsPerRevolution", &draft.countsPerRevolution, 0, 0, "%.10g");
                ImGui::TextWrapped("%s",
                                   tr("Use the effective count after quadrature. No gearing is inferred.",
                                      "填写倍频后的有效计数；不会自动推断减速比。"));
        }
        const auto valid = unit_conversion::Convert(1, draft);
        if (!valid)
                ui::ErrorTextWrapped("%s", errorLabel(valid.error));
        return bool(valid);
}

inline std::string
UnitConversionSuffix(double rawValue, const unit_conversion::Settings &settings)
{
        if (!settings.enabled || settings.source == settings.target || !std::isfinite(rawValue))
                return {};
        const auto result = unit_conversion::Convert(rawValue, settings);
        if (!result)
                return {};
        char text[96];
        std::snprintf(text, sizeof(text), " (= %.7g %s)", result.value, unit_conversion::UnitLabel(settings.target));
        return text;
}

inline bool
UnitConversionVisible()
{
        const auto &state = unit_hover_detail::state;
        return state.context == ImGui::GetCurrentContext() && state.visible && state.seenFrame == ImGui::GetFrameCount();
}

inline bool
UnitConversionActive(const char *id)
{
        const auto &state = unit_hover_detail::state;
        return state.context == ImGui::GetCurrentContext() && state.visible && state.id == ImGui::GetID(id);
}

// Call after submitting the workspace. Dropping/deleting/collapsing a source
// dismisses its card and any open combo without retaining row/channel pointers.
inline void
UnitConversionEndFrame()
{
        auto &state = unit_hover_detail::state;
        if (state.context == ImGui::GetCurrentContext() && state.id && state.seenFrame != ImGui::GetFrameCount()) {
                unit_hover_detail::closePopups();
                state = unit_hover_detail::State{ImGui::GetCurrentContext()};
        }
}

// Submit after the source item's editing/drag interactions. No pointers to rows
// or channels survive this call. A removed/hidden source cannot leave an editor
// referring to destroyed storage. Only Apply/Disable changes persisted settings.
inline bool
UnitConversionHover(
    const char *id, const char *title, std::optional<double> value, unit_conversion::Settings &settings, bool itemHovered)
{
        using namespace unit_hover_detail;
        auto        &g      = *ImGui::GetCurrentContext();
        const auto   item   = g.LastItemData;
        const auto   source = ImGui::GetID(id);
        const double now    = g.Time;
        const auto   mouse  = ImGui::GetMousePos();
        if (state.context != &g || state.seenFrame > g.FrameCount || state.seenFrame < g.FrameCount - 1)
                state = State{&g};
        // Editing/panning the source keeps its normal interaction uninterrupted.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || (item.ID && g.ActiveId == item.ID))
                itemHovered = false;
        const bool cardHovered = state.visible && state.rect.Contains(mouse);
        if (itemHovered && !state.editing && !cardHovered && !hasPopup()) {
                if (state.id != source) {
                        state         = State{&g};
                        state.id      = source;
                        state.started = now;
                }
                state.keepUntil     = now + 0.45;
                state.capturedValue = value;
                if (!state.visible && now - state.started >= 0.4) {
                        state.visible = true;
                        state.anchor  = ImVec2(mouse.x + 12, mouse.y + 16);
                }
        }
        if (state.id != source)
                return false;
        state.seenFrame = g.FrameCount;
        if (!state.visible) {
                if (!itemHovered)
                        state = State{&g};
                return false;
        }
        const bool popupOpen = hasPopup();
        if (cardHovered || popupOpen || state.editing)
                state.keepUntil = now + 0.45;
        const bool outsideClick =
            state.editing && !cardHovered && !popupOpen && !itemHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (now > state.keepUntil || outsideClick || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                closePopups();
                state = State{&g};
                return false;
        }

        const auto *viewport        = ImGui::GetWindowViewport();
        const float font            = ImGui::GetFontSize();
        const float width           = std::max(1.0f, std::min(font * 24, viewport->WorkSize.x - 12));
        const float estimatedHeight = font * (state.editing ? 25.0f : 12.0f);
        ImVec2      position        = state.anchor;
        position.x                  = std::clamp(position.x,
                                viewport->WorkPos.x + 6,
                                std::max(viewport->WorkPos.x + 6, viewport->WorkPos.x + viewport->WorkSize.x - width - 6));
        position.y =
            std::clamp(position.y,
                       viewport->WorkPos.y + 6,
                       std::max(viewport->WorkPos.y + 6, viewport->WorkPos.y + viewport->WorkSize.y - estimatedHeight - 6));
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, std::max(1.0f, viewport->WorkSize.y - 12)));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().PopupRounding);
        // Unlike BeginTooltip(), this hover surface accepts its own buttons and
        // selectors, without making the rest of the application modal.
        const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
        bool changed = false;
        if (ImGui::Begin(windowName, nullptr, flags)) {
                ImGui::TextUnformatted(tr("Unit conversion", "单位换算"));
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
                ImGui::TextUnformatted(title);
                ImGui::PopTextWrapPos();
                if (state.capturedValue && std::isfinite(*state.capturedValue)) {
                        ImGui::Text(tr("Source: %.12g %s", "原值：%.12g %s"),
                                    *state.capturedValue,
                                    settings.enabled ? unit_conversion::UnitLabel(settings.source) : "");
                        if (settings.enabled) {
                                const auto result = unit_conversion::Convert(*state.capturedValue, settings);
                                if (result)
                                        ImGui::Text(tr("Converted: %.12g %s", "换算：%.12g %s"),
                                                    result.value,
                                                    unit_conversion::UnitLabel(settings.target));
                                else
                                        ui::ErrorTextWrapped("%s", errorLabel(result.error));
                        }
                } else {
                        ImGui::TextDisabled("%s", tr("No valid numeric sample", "暂无有效数值"));
                }
                ImGui::TextWrapped("%s",
                                   tr("Hover snapshot. Recorded values and device writes stay unchanged.",
                                      "显示悬停时的取值，不改变采集数据和设备写入。"));
                ImGui::Spacing();
                if (!state.editing) {
                        if (ImGui::Button(settings.enabled ? tr("Edit units...", "设置单位...")
                                                           : tr("Set up conversion...", "设置单位换算..."))) {
                                state.draft =
                                    settings.enabled
                                        ? settings
                                        : unit_conversion::DefaultSettings(unit_conversion::Quantity::AngularPosition);
                                state.draft.enabled = true;
                                state.editing       = true;
                        }
                        if (settings.enabled) {
                                ImGui::SameLine();
                                if (ImGui::Button(tr("Disable", "关闭换算"))) {
                                        settings.enabled = false;
                                        changed          = true;
                                }
                        }
                } else {
                        ImGui::Separator();
                        ImGui::TextUnformatted(tr("Quantity", "物理量"));
                        ImGui::SetNextItemWidth(-1);
                        const char *quantities[4];
                        for (int i = 0; i < 4; ++i)
                                quantities[i] = quantityLabel(static_cast<unit_conversion::Quantity>(i));
                        int quantity = static_cast<int>(state.draft.quantity);
                        if (ui::Choice("##quantity", &quantity, quantities, 4)) {
                                const double counts = state.draft.countsPerRevolution;
                                state.draft =
                                    unit_conversion::DefaultSettings(static_cast<unit_conversion::Quantity>(quantity));
                                state.draft.countsPerRevolution = counts;
                        }
                        unitCombo(tr("Source unit", "原始单位"), state.draft.quantity, state.draft.source);
                        unitCombo(tr("Display unit", "目标单位"), state.draft.quantity, state.draft.target);
                        if (unit_conversion::UsesCounts(state.draft.source) ||
                            unit_conversion::UsesCounts(state.draft.target)) {
                                ImGui::TextWrapped("%s", tr("Encoder counts per revolution", "编码器每圈计数"));
                                ImGui::SetNextItemWidth(-1);
                                ImGui::InputDouble("##countsPerRevolution", &state.draft.countsPerRevolution, 0, 0, "%.10g");
                                ImGui::TextWrapped("%s",
                                                   tr("Use the effective count after quadrature. No gearing is inferred.",
                                                      "填写倍频后的有效计数；不会自动推断减速比。"));
                        }
                        const auto valid = unit_conversion::Convert(1, state.draft);
                        if (!valid)
                                ui::ErrorTextWrapped("%s", errorLabel(valid.error));
                        ImGui::BeginDisabled(!valid);
                        if (ui::Button(tr("Apply", "应用"), ui::BtnStyle::Primary)) {
                                settings         = state.draft;
                                settings.enabled = true;
                                state.editing    = false;
                                changed          = true;
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Cancel", "取消")))
                                state.editing = false;
                }
                auto *window = ImGui::GetCurrentWindow();
                state.rect   = ImRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y));
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsAnyItemActive() || hasPopup())
                        state.keepUntil = now + 0.45;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        g.LastItemData = item;
        return changed;
}
} // namespace ui
#endif
