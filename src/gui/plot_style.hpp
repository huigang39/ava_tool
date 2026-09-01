#pragma once

#include "gui/design_tokens.hpp"
#include "implot.h"

namespace ui
{
inline void
ApplyPlotStyle(bool dark)
{
        if (dark)
                ImPlot::StyleColorsDark();
        else
                ImPlot::StyleColorsLight();
        const auto &ui                      = ImGui::GetStyle();
        auto       &plot                    = ImPlot::GetStyle();
        plot.Colors[ImPlotCol_FrameBg]      = ImVec4(0, 0, 0, 0);
        plot.Colors[ImPlotCol_PlotBg]       = ui.Colors[ImGuiCol_FrameBg];
        plot.Colors[ImPlotCol_PlotBorder]   = ui.Colors[ImGuiCol_Border];
        plot.Colors[ImPlotCol_LegendBg]     = ui.Colors[ImGuiCol_PopupBg];
        plot.Colors[ImPlotCol_LegendBorder] = ui.Colors[ImGuiCol_Border];
        plot.Colors[ImPlotCol_AxisText]     = ui.Colors[ImGuiCol_TextDisabled];
        auto grid                           = ui.Colors[ImGuiCol_Text];
        grid.w                              = dark ? .10f : .09f;
        plot.Colors[ImPlotCol_AxisGrid]     = grid;
        plot.PlotBorderSize                 = design::Stroke();
        plot.MajorGridSize = plot.MinorGridSize = ImVec2(design::hairline, design::hairline);
        plot.MajorTickSize = plot.MinorTickSize = ImVec2(design::hairline, design::hairline);
        plot.PlotPadding                        = ui.WindowPadding;
        plot.LegendInnerPadding                 = ui.FramePadding;
        plot.LegendSpacing                      = ImVec2(ui.ItemInnerSpacing.x, 2);
}
} // namespace ui
