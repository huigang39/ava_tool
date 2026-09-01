#ifndef AVA_PLATFORM_STYLE_HPP
#define AVA_PLATFORM_STYLE_HPP

#include "imgui.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ui
{
enum class PlatformStyle { Auto, Windows, MacOS, Adwaita, Breeze };

// XDG_CURRENT_DESKTOP can contain a colon-separated list, e.g. ubuntu:GNOME.
inline PlatformStyle
LinuxStyle(const char *desktop, const char *session)
{
        std::string name = desktop && *desktop ? desktop : (session ? session : "");
        for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name.find("kde") != std::string::npos || name.find("plasma") != std::string::npos ? PlatformStyle::Breeze
                                                                                                 : PlatformStyle::Adwaita;
}

inline PlatformStyle
HostStyle()
{
#if defined(_WIN32)
        return PlatformStyle::Windows;
#elif defined(__APPLE__)
        return PlatformStyle::MacOS;
#else
        return LinuxStyle(std::getenv("XDG_CURRENT_DESKTOP"), std::getenv("DESKTOP_SESSION"));
#endif
}

inline const char *
ShortcutLabel(const char *ctrl, const char *cmd)
{
        return ImGui::GetIO().ConfigMacOSXBehaviors ? cmd : ctrl;
}

inline const char *
PlatformStyleName(PlatformStyle platform)
{
        switch (platform == PlatformStyle::Auto ? HostStyle() : platform) {
                case PlatformStyle::Windows:
                        return "Windows / Fluent";
                case PlatformStyle::MacOS:
                        return "macOS";
                case PlatformStyle::Breeze:
                        return "Linux / KDE Breeze";
                default:
                        return "Linux / GNOME Adwaita";
        }
}

// Shared ImGui controls with platform-inspired metrics and surfaces. The OS
// continues to own native window decorations and file dialogs. Reset before
// scaling so switching styles never compounds DPI scaling.
inline void
ApplyPlatformStyle(PlatformStyle platform, float scale, bool viewports, bool dark = true)
{
        if (platform == PlatformStyle::Auto)
                platform = HostStyle();
        ImGuiStyle &style = ImGui::GetStyle();
        style             = ImGuiStyle();
        if (dark)
                ImGui::StyleColorsDark(&style);
        else
                ImGui::StyleColorsLight(&style);
        ImVec4 accent, surface, panel, control;
        style.WindowPadding                    = ImVec2(12, 10);
        style.FramePadding                     = ImVec2(9, 5);
        style.ItemSpacing                      = ImVec2(8, 6);
        style.ItemInnerSpacing                 = ImVec2(5, 4);
        style.CellPadding                      = ImVec2(6, 3);
        style.IndentSpacing                    = 20;
        style.GrabMinSize                      = 12;
        style.ScrollbarSize                    = 14;
        style.ScrollbarRounding                = 8;
        style.ScrollbarPadding                 = 3;
        style.FrameBorderSize                  = 1;
        style.WindowBorderSize                 = 1;
        style.PopupBorderSize                  = 1;
        style.TabBorderSize                    = 0;
        style.TabMinWidthBase                  = 64;
        style.TabMinWidthShrink                = 48;
        style.TabCloseButtonMinWidthSelected   = -1;
        style.TabCloseButtonMinWidthUnselected = 0;
        style.TabBarBorderSize                 = 1;
        style.TabBarOverlineSize               = 2;
        style.SeparatorTextBorderSize          = 1;
        style.SeparatorTextPadding             = ImVec2(0, 8);
        style.DisabledAlpha                    = 0.48f;
        style.HoverDelayNormal                 = 0.45f;
        style.HoverDelayShort                  = 0.15f;
        style.WindowMenuButtonPosition         = ImGuiDir_None;
        switch (platform) {
                case PlatformStyle::MacOS:
                        accent               = ImVec4(0.27f, 0.61f, 1.0f, 1);
                        surface              = ImVec4(0.12f, 0.12f, 0.13f, 1);
                        panel                = ImVec4(0.17f, 0.17f, 0.18f, 1);
                        control              = ImVec4(0.25f, 0.25f, 0.27f, 1);
                        style.WindowRounding = style.PopupRounding = 12;
                        style.ChildRounding                        = 10;
                        style.FrameRounding = style.GrabRounding = style.TabRounding = 8;
                        style.FrameBorderSize                                        = 0;
                        style.ItemSpacing                                            = ImVec2(8, 6);
                        style.FramePadding                                           = ImVec2(9, 4);
                        style.ScrollbarSize                                          = 12;
                        break;
                case PlatformStyle::Breeze:
                        accent               = ImVec4(0.24f, 0.68f, 0.91f, 1);
                        surface              = ImVec4(0.12f, 0.14f, 0.16f, 1);
                        panel                = ImVec4(0.16f, 0.18f, 0.20f, 1);
                        control              = ImVec4(0.20f, 0.22f, 0.25f, 1);
                        style.WindowRounding = 10;
                        style.PopupRounding  = 12;
                        style.ChildRounding  = 8;
                        style.FrameRounding = style.GrabRounding = style.TabRounding = 6;
                        style.FramePadding                                           = ImVec2(9, 5);
                        break;
                case PlatformStyle::Adwaita:
                        accent               = ImVec4(0.38f, 0.65f, 0.98f, 1);
                        surface              = ImVec4(0.12f, 0.12f, 0.12f, 1);
                        panel                = ImVec4(0.16f, 0.16f, 0.16f, 1);
                        control              = ImVec4(0.23f, 0.23f, 0.23f, 1);
                        style.WindowRounding = style.PopupRounding = 12;
                        style.ChildRounding                        = 10;
                        style.FrameRounding = style.GrabRounding = style.TabRounding = 8;
                        style.FramePadding                                           = ImVec2(10, 6);
                        style.FrameBorderSize                                        = 0;
                        break;
                default:
                        accent               = ImVec4(0.38f, 0.80f, 0.98f, 1);
                        surface              = ImVec4(0.125f, 0.125f, 0.125f, 1);
                        panel                = ImVec4(0.17f, 0.17f, 0.17f, 1);
                        control              = ImVec4(0.21f, 0.21f, 0.21f, 1);
                        style.WindowRounding = style.PopupRounding = 12;
                        style.ChildRounding                        = 8;
                        style.FrameRounding = style.GrabRounding = style.TabRounding = 6;
                        break;
        }
        if (!dark) {
                switch (platform) {
                        case PlatformStyle::MacOS:
                                accent  = ImVec4(0.0f, 0.40f, 0.80f, 1);
                                surface = ImVec4(0.965f, 0.965f, 0.975f, 1);
                                panel   = ImVec4(0.93f, 0.93f, 0.945f, 1);
                                control = ImVec4(1, 1, 1, 1);
                                break;
                        case PlatformStyle::Breeze:
                                accent  = ImVec4(0.0f, 0.435f, 0.675f, 1);
                                surface = ImVec4(0.945f, 0.953f, 0.965f, 1);
                                panel   = ImVec4(0.92f, 0.935f, 0.95f, 1);
                                control = ImVec4(0.985f, 0.99f, 1.0f, 1);
                                break;
                        case PlatformStyle::Adwaita:
                                accent  = ImVec4(0.11f, 0.44f, 0.845f, 1);
                                surface = ImVec4(0.98f, 0.98f, 0.98f, 1);
                                panel   = ImVec4(0.94f, 0.94f, 0.94f, 1);
                                control = ImVec4(1, 1, 1, 1);
                                break;
                        default:
                                accent  = ImVec4(0.0f, 0.405f, 0.755f, 1);
                                surface = ImVec4(0.953f, 0.953f, 0.953f, 1);
                                panel   = ImVec4(0.975f, 0.975f, 0.975f, 1);
                                control = ImVec4(1, 1, 1, 1);
                                break;
                }
        }
        auto &c                  = style.Colors;
        c[ImGuiCol_Text]         = dark ? ImVec4(0.94f, 0.94f, 0.95f, 1) : ImVec4(0.10f, 0.12f, 0.15f, 1);
        c[ImGuiCol_TextDisabled] = dark ? ImVec4(0.61f, 0.63f, 0.66f, 1) : ImVec4(0.38f, 0.40f, 0.43f, 1);
        c[ImGuiCol_WindowBg]     = surface;
        c[ImGuiCol_ChildBg]      = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = c[ImGuiCol_MenuBarBg] = panel;
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = panel;
        c[ImGuiCol_Border]       = dark ? ImVec4(0.68f, 0.70f, 0.74f, 0.18f) : ImVec4(0.16f, 0.18f, 0.20f, 0.22f);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = control;
        const float hoverOffset                  = dark ? 0.045f : -0.035f;
        c[ImGuiCol_FrameBgHovered]               = c[ImGuiCol_ButtonHovered] =
            ImVec4(control.x + hoverOffset, control.y + hoverOffset, control.z + hoverOffset, 1);
        const float activeOffset  = dark ? -0.025f : -0.065f;
        c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] =
            ImVec4(control.x + activeOffset, control.y + activeOffset, control.z + activeOffset, 1);
        c[ImGuiCol_Header]        = ImVec4(accent.x, accent.y, accent.z, 0.16f);
        c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.23f);
        c[ImGuiCol_HeaderActive]  = ImVec4(accent.x, accent.y, accent.z, 0.29f);
        c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = accent;
        c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_Separator]                                      = c[ImGuiCol_Border];
        c[ImGuiCol_ScrollbarBg]                                    = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = dark ? ImVec4(0.44f, 0.46f, 0.49f, 0.65f) : ImVec4(0.44f, 0.46f, 0.49f, 0.70f);
        c[ImGuiCol_ScrollbarGrabHovered] = dark ? ImVec4(0.54f, 0.56f, 0.59f, 0.85f) : ImVec4(0.30f, 0.32f, 0.35f, 0.85f);
        c[ImGuiCol_ScrollbarGrabActive]  = dark ? ImVec4(0.62f, 0.64f, 0.67f, 1) : ImVec4(0.22f, 0.24f, 0.27f, 1);
        c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_ResizeGripActive]     = accent;
        c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = surface;
        c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = control;
        c[ImGuiCol_TabHovered]                                  = c[ImGuiCol_HeaderHovered];
        c[ImGuiCol_TabSelectedOverline]                         = accent;
        c[ImGuiCol_TabDimmedSelectedOverline]                   = ImVec4(accent.x, accent.y, accent.z, 0.45f);
        c[ImGuiCol_TextSelectedBg] = c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_NavCursor]                                   = accent;
        c[ImGuiCol_TableHeaderBg]                               = panel;
        c[ImGuiCol_TableBorderStrong] = dark ? ImVec4(0.58f, 0.60f, 0.64f, 0.22f) : ImVec4(0.30f, 0.32f, 0.36f, 0.22f);
        c[ImGuiCol_TableBorderLight]  = dark ? ImVec4(0.58f, 0.60f, 0.64f, 0.10f) : ImVec4(0.30f, 0.32f, 0.36f, 0.10f);
        c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]     = dark ? ImVec4(1, 1, 1, 0.025f) : ImVec4(0, 0, 0, 0.025f);
        c[ImGuiCol_DockingEmptyBg]    = surface;
        c[ImGuiCol_ModalWindowDimBg]  = ImVec4(0, 0, 0, dark ? 0.52f : 0.30f);
        style.ScaleAllSizes(std::isfinite(scale) && scale > 0 ? scale : 1.0f);
        // ImGui already makes OS-owned viewports rectangular; preserve rounding
        // for floating windows rendered inside the main viewport.
        (void)viewports;
}
} // namespace ui
#endif
