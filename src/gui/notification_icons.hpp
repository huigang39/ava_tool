#ifndef AVA_NOTIFICATION_ICONS_HPP
#define AVA_NOTIFICATION_ICONS_HPP

#include "ImGuiNotify.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_icons.hpp"

namespace ui
{
inline ImGui::NotificationStyle
NotificationAppearance(ImGuiToastType type)
{
        const auto &style = ImGui::GetStyle();
        const auto  base  = style.Colors[ImGuiCol_WindowBg];
        ImVec4      accent;
        const char *title;
        switch (type) {
                case ImGuiToastType::Success:
                        accent = SuccessTextColor();
                        title  = tr("Success", "操作成功");
                        break;
                case ImGuiToastType::Warning:
                        accent = SemanticButtonPalette(BtnStyle::Warning, style).text;
                        title  = tr("Warning", "请注意");
                        break;
                case ImGuiToastType::Error:
                        accent = ErrorTextColor(base);
                        title  = tr("Error", "操作失败");
                        break;
                default:
                        accent = ReadableTextColor(style.Colors[ImGuiCol_CheckMark], base);
                        title  = tr("Notification", "通知");
                        break;
        }
        const auto background = MixSurface(base, accent, ColorLuminance(base) < 0.4f ? 0.06f : 0.025f);
        accent = type == ImGuiToastType::Error ? ErrorTextColor(background) : ReadableTextColor(accent, background);
        return {background,
                accent,
                (type == ImGuiToastType::Error || type == ImGuiToastType::Success)
                    ? accent
                    : ReadableTextColor(style.Colors[ImGuiCol_Text], background),
                accent,
                title};
}

inline void
DrawNotificationIcon(ImGuiToastType type, float opacity)
{
        Icon        icon;
        const auto &style = ImGui::GetStyle();
        ImVec4      color;
        switch (type) {
                case ImGuiToastType::Success:
                        icon  = Icon::Success;
                        color = SemanticButtonPalette(BtnStyle::Success, style).text;
                        break;
                case ImGuiToastType::Warning:
                        icon  = Icon::Warning;
                        color = SemanticButtonPalette(BtnStyle::Warning, style).text;
                        break;
                case ImGuiToastType::Error:
                        icon  = Icon::Error;
                        color = SemanticButtonPalette(BtnStyle::Danger, style).text;
                        break;
                case ImGuiToastType::Info:
                        icon  = Icon::Info;
                        color = ReadableTextColor(style.Colors[ImGuiCol_CheckMark], style.Colors[ImGuiCol_WindowBg]);
                        break;
                default:
                        return;
        }
        color.w              *= std::clamp(opacity, 0.0f, 1.0f);
        const float size      = ImGui::GetFontSize() * 1.1f;
        const auto  position  = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        if (ImGui::IsItemVisible())
                DrawIcon(ImGui::GetWindowDrawList(), icon, position, size, ImGui::GetColorU32(color));
}

inline bool
NotificationDismissButton(const char *id, float opacity)
{
        // Font-sized vector with the standard button hit target. Horizontal
        // padding is reduced only for this square, so the icon never clips.
        const auto &style = ImGui::GetStyle();
        const float side  = std::max(ImGui::GetFrameHeight(), ImGui::GetFontSize() * 1.1f + style.FramePadding.y * 2);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * std::clamp(opacity, 0.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.y, style.FramePadding.y));
        const bool pressed = IconButton(id, Icon::Close, BtnStyle::Neutral, ImVec2(side, side));
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", tr("Dismiss notification", "关闭通知"));
        return pressed;
}

// Register once after ImGui context initialization. The inline vendor hooks
// are shared by all translation units and read current theme colors at render
// time, including after a light/dark or platform-style change.
inline void
InstallNotificationIcons()
{
        ImGui::SetNotificationRenderers(DrawNotificationIcon, NotificationDismissButton);
        ImGui::notificationStyleProvider = NotificationAppearance;
}
} // namespace ui
#endif
