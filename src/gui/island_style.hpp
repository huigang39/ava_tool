#pragma once

#include "gui/design_tokens.hpp"
#include "gui/platform_style.hpp"
#include "gui/ui_theme.hpp"
#include <cstdio>

namespace ui
{
inline ImGuiID
IslandDockSpace(ImGuiViewport *viewport)
{
        // Keep the existing host name / dock ID so saved layouts still restore.
        char name[48];
        std::snprintf(name, sizeof(name), "WindowOverViewport_%08X", viewport->ID);
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        const float gap = ImGui::GetStyle().DockingSeparatorSize;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_DockingEmptyBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gap, gap));
        ImGui::Begin(name,
                     nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
        const ImGuiID id = ImGui::GetID("DockSpace");
        // Docking uses Border and ResizeGrip colors for its full-width splitter.
        // Keep the gutter as plain canvas; the resize cursor still works.
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
        ImGui::DockSpace(id);
        ImGui::PopStyleColor(3);
        ImGui::End();
        ImGui::PopStyleColor();
        return id;
}

enum class ColorScheme { Ocean, Graphite, Violet, Mint, Rose, Amber, Count };

inline ImVec4
SchemeAccent(ColorScheme scheme, bool dark)
{
        switch (scheme) {
                case ColorScheme::Graphite:
                        return dark ? ImVec4(.65f, .69f, .75f, 1) : ImVec4(.32f, .36f, .43f, 1);
                case ColorScheme::Violet:
                        return dark ? ImVec4(.69f, .54f, .98f, 1) : ImVec4(.48f, .30f, .78f, 1);
                case ColorScheme::Mint:
                        return dark ? ImVec4(.30f, .78f, .66f, 1) : ImVec4(.08f, .48f, .39f, 1);
                case ColorScheme::Rose:
                        return dark ? ImVec4(.96f, .52f, .66f, 1) : ImVec4(.72f, .25f, .40f, 1);
                case ColorScheme::Amber:
                        return dark ? ImVec4(.95f, .70f, .34f, 1) : ImVec4(.63f, .39f, .10f, 1);
                default:
                        return dark ? ImVec4(.32f, .64f, 1, 1) : ImVec4(.05f, .40f, .80f, 1);
        }
}

// One desktop design on every platform; native dialogs and keyboard shortcuts
// still follow the host OS. Rebuild at 1x before scaling to avoid DPI drift.
inline void
ApplyAppStyle(ColorScheme scheme, float scale, bool viewports, bool dark)
{
        ApplyPlatformStyle(PlatformStyle::MacOS, 1, viewports, dark);
        auto      &s               = ImGui::GetStyle();
        auto      &c               = s.Colors;
        const auto accent          = SchemeAccent(scheme, dark);
        const auto canvas          = dark ? ImVec4(.070f, .078f, .090f, 1) : ImVec4(.925f, .935f, .950f, 1);
        const auto card            = dark ? ImVec4(.115f, .125f, .143f, 1) : ImVec4(.985f, .987f, .992f, 1);
        const auto raised          = dark ? ImVec4(.155f, .166f, .185f, 1) : ImVec4(1, 1, 1, 1);
        const auto control         = MixSurface(card, dark ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1), dark ? .055f : .035f);
        const auto input           = dark ? ImVec4(.086f, .095f, .112f, 1) : ImVec4(1, 1, 1, 1);
        c[ImGuiCol_Text]           = dark ? ImVec4(.91f, .93f, .96f, 1) : ImVec4(.12f, .15f, .20f, 1);
        c[ImGuiCol_TextDisabled]   = dark ? ImVec4(.63f, .67f, .73f, 1) : ImVec4(.39f, .43f, .49f, 1);
        c[ImGuiCol_WindowBg]       = card;
        c[ImGuiCol_DockingEmptyBg] = canvas;
        c[ImGuiCol_ChildBg]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = card;
        c[ImGuiCol_PopupBg]                                                            = raised;
        c[ImGuiCol_MenuBarBg]                                                          = card;
        c[ImGuiCol_FrameBg]                                                            = input;
        c[ImGuiCol_FrameBgHovered]                                                     = MixSurface(input, accent, .07f);
        c[ImGuiCol_FrameBgActive]                                                      = MixSurface(input, accent, .11f);
        c[ImGuiCol_Button]                                                             = control;
        c[ImGuiCol_ButtonHovered]                                                      = MixSurface(control, accent, .10f);
        c[ImGuiCol_ButtonActive]                                                       = MixSurface(control, accent, .18f);
        c[ImGuiCol_Header]                                                             = MixSurface(card, accent, .10f);
        c[ImGuiCol_HeaderHovered]                                                      = MixSurface(card, accent, .14f);
        c[ImGuiCol_HeaderActive]                                                       = MixSurface(card, accent, .21f);
        c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = card;
        c[ImGuiCol_TabSelected]                 = MixSurface(card, accent, .16f);
        c[ImGuiCol_TabDimmedSelected]           = control;
        c[ImGuiCol_TabHovered]                  = c[ImGuiCol_HeaderHovered];
        c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_SliderGrabActive]                                           = MixSurface(accent, c[ImGuiCol_Text], .18f);
        c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_TabSelectedOverline]                            = accent;
        c[ImGuiCol_TabDimmedSelectedOverline]                      = c[ImGuiCol_Border];
        c[ImGuiCol_TextSelectedBg] = c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, .25f);
        c[ImGuiCol_TableHeaderBg]                               = control;
        c[ImGuiCol_TableBorderStrong]                           = MixSurface(card, c[ImGuiCol_Text], .12f);
        c[ImGuiCol_TableBorderLight]                            = MixSurface(card, c[ImGuiCol_Text], .05f);
        c[ImGuiCol_TableRowBg]                                  = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]                               = MixSurface(card, c[ImGuiCol_Text], .015f);
        c[ImGuiCol_Border]               = dark ? ImVec4(.58f, .63f, .71f, .32f) : ImVec4(.20f, .26f, .35f, .24f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_Separator]            = c[ImGuiCol_Border];
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = MixSurface(card, c[ImGuiCol_Text], .24f);
        c[ImGuiCol_ScrollbarGrabHovered] = MixSurface(card, c[ImGuiCol_Text], .36f);
        c[ImGuiCol_ScrollbarGrabActive]  = MixSurface(card, accent, .55f);
        c[ImGuiCol_ResizeGrip]           = ImVec4(accent.x, accent.y, accent.z, .18f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(accent.x, accent.y, accent.z, .60f);
        c[ImGuiCol_ResizeGripActive]     = accent;
        c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, dark ? .42f : .20f);
        for (auto background : {ImGuiCol_FrameBg,
                                ImGuiCol_FrameBgHovered,
                                ImGuiCol_FrameBgActive,
                                ImGuiCol_Button,
                                ImGuiCol_TabSelected,
                                ImGuiCol_TableHeaderBg,
                                ImGuiCol_PopupBg})
                c[ImGuiCol_TextDisabled] = ReadableTextColor(c[ImGuiCol_TextDisabled], c[background]);
        s.WindowRounding = design::windowRadius;
        s.PopupRounding  = design::panelRadius;
        s.ChildRounding  = design::InnerRadius(s.WindowRounding, design::space);
        s.FrameRounding = s.GrabRounding = s.TabRounding = design::controlRadius;
        s.WindowPadding                                  = ImVec2(12, 10);
        s.FramePadding                                   = ImVec2(10, 5);
        s.ItemSpacing                                    = ImVec2(8, 6);
        s.ItemInnerSpacing                               = ImVec2(6, 4);
        s.CellPadding                                    = ImVec2(10, 4);
        s.DockingSeparatorSize                           = 8;
        s.ScrollbarSize                                  = 12;
        s.ScrollbarPadding                               = 3;
        s.ScrollbarRounding                              = 6;
        s.WindowBorderSize = s.ChildBorderSize = s.PopupBorderSize = s.FrameBorderSize = design::hairline;
        s.TabBorderSize                                                                = 0;
        s.TabBarBorderSize                                                             = 0;
        s.TabBarOverlineSize                                                           = design::hairline;
        s.SeparatorTextBorderSize                                                      = design::hairline;
        s.DisabledAlpha                                                                = .65f;
        s.ScaleAllSizes(design::Scale(scale));
}
} // namespace ui
