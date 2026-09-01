#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/unit_conversion_ui.hpp"
#include "implot_internal.h"
#include <stdexcept>

static void
check(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

struct ScopeInteractionTestAccess {
        static void fft(MonitorScope &scope) { scope.showFft_ = true; }
};

int
main(int, char **)
{
        module_init();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = ImVec2(1100, 800);
        io.DeltaTime                    = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels;
        int            width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try {
                Monitor monitor("Unit hover regression");
                monitor.externalFeed_ = true;
                monitor.addScope("Signals");
                auto scope = monitor.getScopes().at("Signals");
                scope->addChannel("angle_input");
                scope->addChannel("angle_output");
                auto *input  = scope->findChannel("angle_input");
                auto *output = scope->findChannel("angle_output");
                for (auto *channel : {input, output}) {
                        channel->setType("f32");
                        channel->setDevice("LOCAL");
                        channel->setLabel("Angle"); // Deliberately non-unique aliases.
                }
                input->getGain() = 2;
                input->getBias() = 10;
                input->setRVal(1, 0);
                input->setRVal(3, 1);
                input->publishSnapshot();
                output->setRVal(40, 0);
                output->setRVal(60, 1);
                output->publishSnapshot();
                output->unitConversion_ = unit_conversion::DefaultSettings(unit_conversion::Quantity::AngularSpeed);
                monitor.clearModified();
                auto frame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(1100, 800));
                        monitor.updateDisplay();
                        ui::UnitConversionEndFrame();
                        ImGui::Render();
                };
                auto hover = [&](ImVec2 point) {
                        for (int i = 0; i < 32; ++i)
                                frame(point);
                };
                for (int i = 0; i < 4; ++i)
                        frame();
                auto *plot = ImPlot::GetCurrentContext()->Plots.GetByIndex(0);
                check(plot, "real monitor plot is rendered");
                auto pixel = [&](double x, double y) {
                        return ImVec2(plot->Axes[ImAxis_X1].PlotToPixels(x), plot->Axes[ImAxis_Y1].PlotToPixels(y));
                };
                auto legend = [&](const char *name) {
                        const std::string label = std::string("Angle###") + name;
                        auto             *item  = plot->Items.GetItem(ImHashStr(label.c_str(), 0, plot->Items.ID));
                        check(item, "canonical channel has a real legend entry");
                        const auto &style  = ImPlot::GetStyle();
                        const float height = ImGui::GetTextLineHeight();
                        ImVec2      point(plot->Items.Legend.Rect.Min.x + style.LegendPadding.x + height * .5f,
                                     plot->Items.Legend.Rect.Min.y + style.LegendPadding.y + height * .5f);
                        // ImPlot records LegendHoverRect only after an entry is hovered.
                        // Locate its first hover from the actual legend order and geometry.
                        for (int i = 0; i < plot->Items.GetLegendCount(); ++i) {
                                if (plot->Items.GetLegendItem(i) == item)
                                        return point;
                                if (plot->Items.Legend.Flags & ImPlotLegendFlags_Horizontal)
                                        point.x += height + style.LegendSpacing.x +
                                                   ImGui::CalcTextSize(plot->Items.GetLegendLabel(i), nullptr, true).x;
                                else
                                        point.y += height + style.LegendSpacing.y;
                        }
                        throw std::runtime_error("channel is missing from the visible legend");
                };
                hover(pixel(0.25, 13));
                check(!ui::UnitConversionVisible(), "hovering a sparse curve segment never opens unit conversion");
                hover(pixel(1, 16));
                check(!ui::UnitConversionVisible(), "hovering a raw sample never opens unit conversion");
                hover(legend("angle_input"));
                check(!ImGui::FindWindowByName(ui::unit_hover_detail::windowName),
                      "hovering a legend entry never opens a unit-conversion card");
                check(!input->unitConversion_.enabled, "an unconfigured variable remains unconfigured on hover");
                const auto outputSuffix = ui::UnitConversionSuffix(60, output->unitConversion_);
                check(outputSuffix.find("rpm") != std::string::npos,
                      "configured scope conversion supplies an alternate-unit readout");
                auto inputSettings   = unit_conversion::DefaultSettings(unit_conversion::Quantity::AngularPosition);
                inputSettings.source = unit_conversion::Unit::Count;
                inputSettings.target = unit_conversion::Unit::Degree;
                inputSettings.countsPerRevolution = 4096;
                Monitor::updateUnitConversion("angle_input", std::nullopt, inputSettings);
                check(input->unitConversion_.enabled && input->unitConversion_.source == unit_conversion::Unit::Count &&
                          output->unitConversion_.source != unit_conversion::Unit::Count,
                      "saved variable settings propagate to matching scope channels without affecting others");
                hover(pixel(1, 16));
                check(!ImGui::FindWindowByName(ui::unit_hover_detail::windowName),
                      "hovering a waveform does not open unit settings");

                scope->setDraw(MonitorScope::DrawEnum::TABLE);
                for (int i = 0; i < 4; ++i)
                        frame();
                ImGuiTable *table = nullptr;
                for (int i = 0; i < GImGui->Tables.GetBufSize(); ++i) {
                        auto *candidate = GImGui->Tables.GetByIndex(i);
                        if (candidate->LastFrameActive == ImGui::GetFrameCount() && candidate->ColumnsCount == 6)
                                table = candidate;
                }
                check(table, "real monitor value table exists");
                const auto channelId = ImHashStr("angle_input", 0, table->ID);
                auto      *window    = table->InnerWindow;
                ImGui::SetFocusID(ImHashStr("##val", 0, channelId), window);
                window->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                frame();
                const auto valueRect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
                check(valueRect.Min.x >= 0, "numeric value input is available for real hover");
                hover(valueRect.GetCenter());
                check(!ImGui::FindWindowByName(ui::unit_hover_detail::windowName),
                      "hovering a table value does not open unit settings");
                check(input->getDispVal() == 16 && input->getGain() == 2 && input->getBias() == 10 &&
                          input->read_.rawSize() == 2 && input->read_.rawVal(0) == 1 && input->read_.rawVal(1) == 3 &&
                          input->getWVal() == 0,
                      "hover conversion leaves display values, history, transforms and writes untouched");
                input->unitConversion_        = unit_conversion::DefaultSettings(unit_conversion::Quantity::AngularPosition);
                input->unitConversion_.source = unit_conversion::Unit::Count;
                input->unitConversion_.countsPerRevolution = 4096;
                auto clone                                 = input->cloneWithHistory();
                check(clone->unitConversion_.enabled && clone->unitConversion_.source == unit_conversion::Unit::Count &&
                          clone->unitConversion_.countsPerRevolution == 4096 && clone->read_.rawVal(1) == 3,
                      "Shift-drag clone copies hover settings and preserves raw history");
                clone->unitConversion_.countsPerRevolution = 8192;
                check(input->unitConversion_.countsPerRevolution == 4096, "clone settings are independently editable");
                scope->setDraw(MonitorScope::DrawEnum::PLOT);
                ScopeInteractionTestAccess::fft(*scope);
                for (int i = 0; i < 4; ++i)
                        frame();
                hover(legend("angle_input"));
                check(!ImGui::FindWindowByName(ui::unit_hover_detail::windowName),
                      "FFT frequency/amplitude never reuse time-series unit conversion");
                std::puts("PASS: unit conversion is read-only while hovering, alternate-unit scope readout, no legend popup, "
                          "raw snapshots and clone settings");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                result = 1;
        }
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        log_deinit(&g_log_ops);
        log_deinit(&g_log_perf);
        return result;
}
