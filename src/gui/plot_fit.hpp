#pragma once
#include "implot.h"
#include "implot_internal.h"
#include <cmath>

namespace ui
{
// FitPadding is relative to half the data span: 0.2 adds 10% per side
constexpr float yFitPadding = 0.2f;
struct IndependentYFit {
        bool manual = false, wasAutomatic = false, requested = false;
        void reset()
        {
                manual    = false;
                requested = true;
        }
        /**
         * @brief Call after setup, before drawing data; a fit request leaves input unlocked
         */
        void update(bool automatic, bool areaWheelZoomsX = false)
        {
                auto      *plot        = ImPlot::GetCurrentPlot();
                auto      &axis        = plot->Axes[ImAxis_Y1];
                const bool yHovered    = ImPlot::IsAxisHovered(ImAxis_Y1);
                const bool areaHovered = ImPlot::IsPlotHovered();
                if (automatic && !wasAutomatic)
                        reset();
                wasAutomatic = automatic;
                if ((yHovered || (areaHovered && !areaWheelZoomsX)) && ImGui::GetIO().MouseWheel != 0)
                        manual = true;
                if ((axis.Held || plot->Held) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
                        manual = true;
                if (yHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        reset();
                if (requested || (automatic && !manual)) {
                        plot->FitThisFrame = true;
                        axis.FitThisFrame  = true;
                        requested          = false;
                }
        }
};

inline void
fitConstantValueAxis(ImPlotAxis &axis)
{
        if (!axis.FitThisFrame || !std::isfinite(axis.FitExtents.Min) || !std::isfinite(axis.FitExtents.Max) ||
            !ImAlmostEqual(axis.FitExtents.Min, axis.FitExtents.Max))
                return;
        const double center = axis.FitExtents.Min;
        if (center == 0.0)
                return;
        // Avoid ImPlot's fixed +/-0.5 fallback for constant nonzero signals
        const double margin = std::abs(center) * 0.1;
        axis.FitExtents.Min = center - margin;
        axis.FitExtents.Max = center + margin;
}
} // namespace ui
