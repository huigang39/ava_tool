/**
 * @file  ui_theme.hpp
 * @brief Semantic button palette so colours convey meaning consistently.
 *
 * Instead of scattering raw PushStyleColor() calls, draw buttons through
 * ui::Button() / ui::SmallButton() with a semantic style:
 *
 *   Neutral  – generic action, no special meaning (Add, OK, Cancel, Close…).
 *              Uses the theme's default button colour.
 *   Primary  – informational / active-state toggle (blue).
 *   Success  – constructive / go / confirm: Connect, Play, Start, Resume,
 *              Export, Save (green).
 *   Warning  – caution / reversible interruption: Pause, Clear Data, Reset,
 *              Restart, elevate (amber).
 *   Danger   – destructive / teardown: Delete, Remove, Disconnect, Stop (red).
 *   Muted    – de-emphasised / inactive / busy state (grey).
 *
 * The whole app shares one mapping: green = go, amber = caution, red =
 * destructive, blue = active/info, grey = inactive.
 */
#ifndef UI_THEME_HPP
#define UI_THEME_HPP

#include "imgui.h"

namespace ui
{
enum class BtnStyle { Neutral, Primary, Success, Warning, Danger, Muted };

// Pushes Button/ButtonHovered/ButtonActive for the style and returns how many
// colours were pushed (0 for Neutral, which keeps the theme default).
inline int
PushButtonStyle(BtnStyle s)
{
        ImVec4 b, h, a;
        switch (s) {
                case BtnStyle::Primary:
                        b = ImVec4(0.20f, 0.45f, 0.85f, 1.0f);
                        h = ImVec4(0.28f, 0.55f, 0.95f, 1.0f);
                        a = ImVec4(0.16f, 0.38f, 0.72f, 1.0f);
                        break;
                case BtnStyle::Success:
                        b = ImVec4(0.20f, 0.60f, 0.25f, 1.0f);
                        h = ImVec4(0.27f, 0.72f, 0.32f, 1.0f);
                        a = ImVec4(0.16f, 0.52f, 0.22f, 1.0f);
                        break;
                case BtnStyle::Warning:
                        b = ImVec4(0.85f, 0.60f, 0.13f, 1.0f);
                        h = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
                        a = ImVec4(0.75f, 0.50f, 0.10f, 1.0f);
                        break;
                case BtnStyle::Danger:
                        b = ImVec4(0.75f, 0.22f, 0.20f, 1.0f);
                        h = ImVec4(0.86f, 0.30f, 0.28f, 1.0f);
                        a = ImVec4(0.62f, 0.16f, 0.15f, 1.0f);
                        break;
                case BtnStyle::Muted:
                        b = ImVec4(0.42f, 0.42f, 0.42f, 0.70f);
                        h = ImVec4(0.52f, 0.52f, 0.52f, 0.85f);
                        a = ImVec4(0.48f, 0.48f, 0.48f, 1.00f);
                        break;
                case BtnStyle::Neutral:
                default:
                        return 0;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, b);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, h);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, a);
        return 3;
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
