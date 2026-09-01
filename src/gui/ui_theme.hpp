/**
 * @file  ui_theme.hpp
 * @brief Semantic button palette so colours convey meaning consistently.
 *
 * Instead of scattering raw PushStyleColor() calls, draw buttons through
 * ui::Button() / ui::SmallButton() with a semantic style:
 *
 *   Neutral  – generic action, no special meaning (Add, OK, Cancel, Close…).
 *              Uses the theme's default button colour.
 *   Primary  – the main action in a dialog or section (platform accent).
 *   Success  – constructive / go / confirm: Connect, Play, Start, Resume,
 *              Export, Save (green).
 *   Warning  – caution / reversible interruption: Pause, Clear Data, Reset,
 *              Restart, elevate (amber).
 *   Danger   – destructive / teardown: Delete, Remove, Disconnect, Stop (red).
 *   Muted    – de-emphasised / inactive / busy state (grey).
 *
 * Only primary actions use a filled accent. Other semantic actions use quiet
 * tinted surfaces and readable foregrounds, leaving plots/data visually dominant.
 */
#ifndef UI_THEME_HPP
#define UI_THEME_HPP

#include "gui/design_tokens.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace ui
{
enum class BtnStyle { Neutral, Primary, Success, Warning, Danger, Muted };

inline float
ColorLuminance(const ImVec4 &color)
{
        const auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * linear(color.x) + 0.7152f * linear(color.y) + 0.0722f * linear(color.z);
}

inline float
ColorContrast(const ImVec4 &a, const ImVec4 &b)
{
        const float x = ColorLuminance(a), y = ColorLuminance(b);
        return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f);
}

inline ImVec4
MixSurface(const ImVec4 &surface, const ImVec4 &tint, float amount)
{
        return ImVec4(surface.x + (tint.x - surface.x) * amount,
                      surface.y + (tint.y - surface.y) * amount,
                      surface.z + (tint.z - surface.z) * amount,
                      1.0f);
}

// For labels that inherit a user-selected series color. Preserve its hue as
// much as possible, darkening/lightening only enough for readable text.
inline ImVec4
ReadableTextColor(ImVec4 color, const ImVec4 &background, float minimumContrast = 4.5f)
{
        const float alpha = color.w;
        if (ColorContrast(color, background) >= minimumContrast)
                return color;
        const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1);
        const ImVec4 target = ColorContrast(black, background) > ColorContrast(white, background) ? black : white;
        float        low = 0, high = 1;
        for (int i = 0; i < 14; ++i) {
                const float mid = (low + high) * 0.5f;
                if (ColorContrast(MixSurface(color, target, mid), background) >= minimumContrast)
                        high = mid;
                else
                        low = mid;
        }
        ImVec4 readable = MixSurface(color, target, high);
        readable.w      = alpha;
        return readable;
}

// Match ImGui's formatted status-text API while adapting the foreground to the
// active theme. Preserve opacity so callers' fade animations continue to work.
inline void TextColored(const ImVec4 &color, const char *format, ...) IM_FMTARGS(2);
inline void
TextColored(const ImVec4 &color, const char *format, ...)
{
        va_list arguments;
        va_start(arguments, format);
        ImGui::TextColoredV(ReadableTextColor(color, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]), format, arguments);
        va_end(arguments);
}

// Errors retain a recognizable red in every palette, including light themes.
inline ImVec4
ErrorTextColor(const ImVec4 &background)
{
        const bool dark = ColorLuminance(background) < 0.4f;
        return ReadableTextColor(dark ? ImVec4(1.0f, 0.38f, 0.38f, 1) : ImVec4(0.72f, 0.08f, 0.12f, 1), background);
}

inline ImVec4
ErrorTextColor()
{
        return ErrorTextColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
}

inline void ErrorText(const char *format, ...) IM_FMTARGS(1);
inline void
ErrorText(const char *format, ...)
{
        va_list arguments;
        va_start(arguments, format);
        ImGui::TextColoredV(ErrorTextColor(), format, arguments);
        va_end(arguments);
}

inline void ErrorTextWrapped(const char *format, ...) IM_FMTARGS(1);
inline void
ErrorTextWrapped(const char *format, ...)
{
        ImGui::PushStyleColor(ImGuiCol_Text, ErrorTextColor());
        va_list arguments;
        va_start(arguments, format);
        ImGui::TextWrappedV(format, arguments);
        va_end(arguments);
        ImGui::PopStyleColor();
}

inline ImVec4
SuccessTextColor()
{
        const auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        return ReadableTextColor(ColorLuminance(background) < .4f ? ImVec4(.35f, .90f, .53f, 1) : ImVec4(.06f, .40f, .20f, 1),
                                 background);
}

inline void SuccessText(const char *format, ...) IM_FMTARGS(1);
inline void
SuccessText(const char *format, ...)
{
        va_list arguments;
        va_start(arguments, format);
        ImGui::TextColoredV(SuccessTextColor(), format, arguments);
        va_end(arguments);
}

inline void SuccessTextWrapped(const char *format, ...) IM_FMTARGS(1);
inline void
SuccessTextWrapped(const char *format, ...)
{
        ImGui::PushStyleColor(ImGuiCol_Text, SuccessTextColor());
        va_list arguments;
        va_start(arguments, format);
        ImGui::TextWrappedV(format, arguments);
        va_end(arguments);
        ImGui::PopStyleColor();
}

struct ButtonPalette {
        ImVec4 idle, hovered, active, text;
};

inline ButtonPalette
SemanticButtonPalette(BtnStyle semantic, const ImGuiStyle &style)
{
        const ImVec4 base = style.Colors[ImGuiCol_Button];
        const bool   dark = ColorLuminance(style.Colors[ImGuiCol_WindowBg]) < 0.4f;
        ImVec4       tint;
        ImVec4       text;
        switch (semantic) {
                case BtnStyle::Primary: {
                        const ImVec4 accent = style.Colors[ImGuiCol_CheckMark];
                        // Darken the platform accent enough for a white label.
                        // Even a bright Windows/Breeze accent must remain readable.
                        const float  factor = dark ? 0.52f : 0.88f;
                        const ImVec4 fill(accent.x * factor, accent.y * factor, accent.z * factor, 1);
                        return {fill,
                                MixSurface(fill, ImVec4(1, 1, 1, 1), 0.06f),
                                MixSurface(fill, ImVec4(0, 0, 0, 1), 0.12f),
                                ImVec4(0.98f, 0.98f, 0.99f, 1)};
                }
                case BtnStyle::Success:
                        tint = ImVec4(0.24f, 0.66f, 0.42f, 1);
                        text = dark ? ImVec4(0.65f, 0.91f, 0.73f, 1) : ImVec4(0.08f, 0.34f, 0.19f, 1);
                        break;
                case BtnStyle::Warning:
                        tint = ImVec4(0.77f, 0.54f, 0.16f, 1);
                        text = dark ? ImVec4(0.98f, 0.80f, 0.49f, 1) : ImVec4(0.43f, 0.25f, 0.025f, 1);
                        break;
                case BtnStyle::Danger:
                        tint = ImVec4(0.76f, 0.27f, 0.26f, 1);
                        text = ErrorTextColor(style.Colors[ImGuiCol_WindowBg]);
                        break;
                case BtnStyle::Muted:
                        return {base,
                                style.Colors[ImGuiCol_ButtonHovered],
                                style.Colors[ImGuiCol_ButtonActive],
                                dark ? ImVec4(0.74f, 0.77f, 0.80f, 1) : ImVec4(0.35f, 0.37f, 0.40f, 1)};
                default:
                        return {base,
                                style.Colors[ImGuiCol_ButtonHovered],
                                style.Colors[ImGuiCol_ButtonActive],
                                style.Colors[ImGuiCol_Text]};
        }
        const auto idle    = MixSurface(base, tint, .045f);
        const auto hovered = MixSurface(base, tint, .12f);
        const auto active  = MixSurface(base, tint, .20f);
        for (const auto &surface : {idle, hovered, active})
                text = ReadableTextColor(text, surface);
        return {idle, hovered, active, text};
}

/**
 * @brief Begin() records these colours per window, including inactive dock tabs
 */
inline int
PushPausedDockTabStyle(bool paused, bool displayFrozen = false)
{
        if (!paused)
                return 0;
        const auto surface  = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const auto tint     = displayFrozen ? ImVec4(.32f, .64f, .90f, 1) : ImVec4(.88f, .62f, .26f, 1);
        const auto idle     = MixSurface(surface, tint, .09f);
        const auto selected = MixSurface(surface, tint, .18f);
        const auto hovered  = MixSurface(selected, tint, .08f);
        auto       text     = tint;
        for (const auto &background : {idle, selected, hovered})
                text = ReadableTextColor(text, background);
        ImGui::PushStyleColor(ImGuiCol_Tab, idle);
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, idle);
        ImGui::PushStyleColor(ImGuiCol_TabSelected, selected);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, selected);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, hovered);
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, text);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, text);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, idle);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, selected);
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, idle);
        ImGui::PushStyleColor(ImGuiCol_Text, text);
        return 11;
}

/**
 * @brief Pushes Button/ButtonHovered/ButtonActive/Text and returns how many
 *        colours were pushed (0 for Neutral, which keeps the theme default)
 */
inline int
PushButtonStyle(BtnStyle s)
{
        if (s == BtnStyle::Neutral)
                return 0;
        const ButtonPalette p = SemanticButtonPalette(s, ImGui::GetStyle());
        ImGui::PushStyleColor(ImGuiCol_Button, p.idle);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.active);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text);
        return 4;
}

inline bool
Button(const char *label, BtnStyle s, const ImVec2 &size = ImVec2(0, 0))
{
        const int  n = PushButtonStyle(s);
        const bool r = ImGui::Button(label, size);
        if (n)
                ImGui::PopStyleColor(n);
        return r;
}

inline bool
SmallButton(const char *label, BtnStyle s)
{
        const int  n = PushButtonStyle(s);
        const bool r = ImGui::SmallButton(label);
        if (n)
                ImGui::PopStyleColor(n);
        return r;
}
} // namespace ui

#endif // !UI_THEME_HPP
