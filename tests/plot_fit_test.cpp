#include "gui/plot_fit.hpp"
#include <cassert>
#include <iostream>

static ImPlotRange fit(double val, bool corrected)
{
        double x[]{0, 1, 2}, y[]{val, val, val};
        auto &io = ImGui::GetIO();
        ImPlotRange result;
        for (int frame = 0; frame < 3; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(800, 500));
                ImGui::Begin("test");
                if (ImPlot::BeginPlot("signal", ImVec2(700, 400))) {
                        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                        ImPlot::PlotLine("value", x, y, 3);
                        auto &axis = ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1];
                        if (corrected)
                                ui::fitConstantValueAxis(axis);
                        ImPlot::EndPlot();
                        result = axis.Range;
                }
                ImGui::End();
                ImGui::Render();
        }
        return result;
}

int main()
{
        ImGui::CreateContext(); ImPlot::CreateContext();
        auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 700); io.DeltaTime = 1.0f / 60;
        unsigned char *pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        const auto original = fit(0.005449, false);
        assert(original.Size() > 0.9);
        for (double val : {0.005449, 0.000443, -0.005449, 1000.0, 1e-20}) {
                const auto range = fit(val, true);
                assert(range.Min < val && val < range.Max);
                assert(range.Size() < std::abs(val) * 0.3);
                assert(std::abs((range.Min + range.Max) * 0.5 - val) < std::abs(val) * 1e-10);
        }
        const auto zero = fit(0.0, true);
        assert(zero.Min < 0 && zero.Max > 0 && zero.Size() >= 1.0);
        ImPlotAxis unchanged;
        unchanged.FitThisFrame = true; unchanged.FitExtents = ImPlotRange(-2, 3);
        ui::fitConstantValueAxis(unchanged);
        assert(unchanged.FitExtents.Min == -2 && unchanged.FitExtents.Max == 3);
        unchanged.FitThisFrame = false; unchanged.FitExtents = ImPlotRange(0.005449, 0.005449);
        ui::fitConstantValueAxis(unchanged);
        assert(unchanged.FitExtents.Min == unchanged.FitExtents.Max);
        ImPlot::DestroyContext(); ImGui::DestroyContext();
        std::cout << "PASS: real ImPlot fit for positive/negative/tiny constants, zero, varying data and manual ranges\n";
}
