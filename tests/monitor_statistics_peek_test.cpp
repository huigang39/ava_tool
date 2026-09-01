#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include "implot_internal.h"
#include <stdexcept>

struct ScopeInteractionTestAccess {
        static void statistics(MonitorScope &scope, float height) { scope.drawStatisticsPanel(height); }
        static bool peeking(const MonitorScope &scope) { return scope.statisticsPeek_.isOpen(); }
};
static void check(bool ok, const char *message)
{
        if (!ok) throw std::runtime_error(message);
}

int main()
{
        module_init();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 700);
        io.DeltaTime = .025f;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        std::string error;
        ImGui::GetCurrentContext()->ErrorCallbackUserData = &error;
        ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext *, void *data, const char *message) {
                *static_cast<std::string *>(data) = message;
        };
        unsigned char *pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try {
                Monitor owner("Statistics recorder fixture");
                owner.addScope("Statistics peek fixture");
                auto &scope = *owner.getScopes().at("Statistics peek fixture");
                scope.addChannel("sample");
                auto *channel = scope.findChannel("sample");
                channel->setDevice("LOCAL");
                channel->setType("f32");
                for (int i = 0; i < 100; ++i) channel->setRVal(float(i % 10), i * .01);
                channel->publishSnapshot();
                MonitorViewMode mode = MonitorViewMode::FULL;
                double xMin = 0, xMax = 1;
                std::string activePeekName;
                bool submitChildPopup = false, openChildPopup = false;
                auto frame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                        ui::interaction::beginFrame();
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(1000, 700));
                        ImGui::Begin("Statistics peek fixture");
                        scope.menu();
                        scope.draw(&xMin, &xMax, 1000, mode);
                        ImGui::End();
                        if (submitChildPopup) {
                                ImGui::Begin(activePeekName.c_str());
                                if (openChildPopup) {
                                        ImGui::OpenPopup("##PeekChildMenu");
                                        openChildPopup = false;
                                }
                                if (ImGui::BeginPopup("##PeekChildMenu")) {
                                        ImGui::TextUnformatted("Statistics display options");
                                        ImGui::EndPopup();
                                }
                                ImGui::End();
                        }
                        ImGui::Render();
                        check(error.empty(), error.c_str());
                };
                auto frames = [&](int count, ImVec2 mouse = ImVec2(-100, -100)) {
                        for (int i = 0; i < count; ++i) frame(mouse);
                };
                frames(5);
                auto *host = ImGui::FindWindowByName("Statistics peek fixture");
                auto *plot = ImPlot::GetCurrentContext()->Plots.GetByIndex(0);
                check(host && plot, "real scope and plot render");
                const ImRect beforePlot = plot->FrameRect;
                const ImVec2 trigger(host->WorkRect.Max.x - 20 - ImGui::CalcTextSize(tr("More", "更多")).x - ImGui::CalcTextSize(tr("Focus", "聚焦")).x - ImGui::GetStyle().FramePadding.x * 4 - ImGui::GetStyle().ItemInnerSpacing.x * 2, host->WorkRect.Min.y + ImGui::GetFrameHeight() * .5f + 3);
                frames(4, trigger);
                check(!ScopeInteractionTestAccess::peeking(scope), "brief pointer transit does not open statistics");
                frames(14, trigger);
                check(ScopeInteractionTestAccess::peeking(scope), "dwelling on the statistics edge tab opens it");
                check(plot->FrameRect.Min.x == beforePlot.Min.x && plot->FrameRect.Max.x == beforePlot.Max.x &&
                      plot->FrameRect.Min.y == beforePlot.Min.y && plot->FrameRect.Max.y == beforePlot.Max.y,
                      "hover statistics never resize or move the plot");
                char name[64];
                snprintf(name, sizeof(name), "##ScopeStatsPeek_%08X", host->ID);
                activePeekName = name;
                auto *peek = ImGui::FindWindowByName(name);
                check(peek && peek->Active, "statistics appear in a separate overlay");
                const ImVec2 panelPoint(peek->Pos.x + 16, peek->Pos.y + 16);
                frames(24, panelPoint);
                check(ScopeInteractionTestAccess::peeking(scope), "moving into the panel keeps it open");
                submitChildPopup = openChildPopup = true;
                frame(panelPoint);
                frames(26);
                check(ScopeInteractionTestAccess::peeking(scope), "a descendant menu holds the panel open outside its bounds");
                check(ImGui::GetCurrentContext()->OpenPopupStack.Size > 0,
                      "descendant popup remains visible while the panel is held");
                ImGui::ClosePopupsOverWindow(nullptr, false);
                submitChildPopup = false;
                frames(6);
                check(ScopeInteractionTestAccess::peeking(scope), "crossing a small gap gets a close grace period");
                frames(20);
                check(!ScopeInteractionTestAccess::peeking(scope), "leaving the panel retracts it automatically");
                frames(18, trigger);
                io.AddKeyEvent(ImGuiKey_Escape, true);
                frame(panelPoint);
                check(!ScopeInteractionTestAccess::peeking(scope) && ui::interaction::escapeConsumed,
                      "Escape closes only the statistics peek");
                io.AddKeyEvent(ImGuiKey_Escape, false);
                frame();
                frames(18, trigger);
                io.AddKeyEvent(ImGuiKey_Escape, true);
                frame(trigger);
                io.AddKeyEvent(ImGuiKey_Escape, false);
                frames(25, trigger);
                check(!ScopeInteractionTestAccess::peeking(scope),
                      "closing over the stats trigger does not reopen until the pointer leaves it");
                frames(3);

                // A saved/manual divider narrower than the recommended default
                // must survive subsequent frames and comparison-mode changes.
                scope.getShowSidePanel() = true;
                frames(5);
                auto *layout = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("##plotLayoutTable", 0, host->ID));
                check(layout && layout->ColumnsCount == 2, "fixed statistics use the resizable plot split");
                layout->Columns[1].WidthRequest = 110;
                layout->IsSettingsDirty = true;
                frames(5);
                check(std::abs(layout->Columns[1].WidthGiven - 110) < 3,
                      "manual statistics width is not forced back to the preferred size");
                scope.getCompareStatistics() = false;
                frames(3);
                check(std::abs(layout->Columns[1].WidthGiven - 110) < 3,
                      "switching statistics mode preserves the chosen divider width");
                scope.getCompareStatistics() = true;
                frames(3);
                check(std::abs(layout->Columns[1].WidthGiven - 110) < 3,
                      "switching back to comparison preserves the chosen divider width");
                scope.getShowSidePanel() = false;
                frames(5);
                scope.getShowSidePanel() = true;
                frames(5);
                layout = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("##plotLayoutTable", 0, host->ID));
                check(std::abs(layout->Columns[1].WidthGiven - 110) < 3,
                      "auto-hide and pin preserve the user's fixed statistics divider width");
                scope.getShowSidePanel() = false;
                frames(5);
                // Ordinary plot drags cannot open the nearby hover panel.
                frame(plot->PlotRect.GetCenter(), true);
                for (int i = 0; i < 20; ++i) frame(trigger, true);
                check(!ScopeInteractionTestAccess::peeking(scope), "dragging over the edge never steals a plot gesture");
                frame();
                {
                        MonitorScope statistics("statistics-bounds");
                        const std::string name = "g_mc.out.fdb_pvct.cur";
                        statistics.addChannel(name); statistics.addChannel("g_mc.in.ref_pvct.cur");
                        float panelWidth = 360;
                        std::string logged;
                        auto draw = [&](ImVec2 mouse = ImVec2(-100, -100), bool log = false) {
                                io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, false);
                                ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0));
                                ImGui::SetNextWindowSize(ImVec2(panelWidth, 260));
                                ImGui::Begin("statistics-bounds");
                                if (log) ImGui::LogToBuffer(0);
                                ScopeInteractionTestAccess::statistics(statistics, ImGui::GetContentRegionAvail().y);
                                if (log) { logged = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
                                ImGui::End(); ImGui::Render();
                                check(error.empty(), error.c_str());
                        };
                        const auto panel = [&]() -> ImGuiWindow * {
                                for (auto *window : ImGui::GetCurrentContext()->Windows)
                                        if (window->Active && std::strstr(window->Name, "statistics-bounds/##plotSidePanel")) return window;
                                return nullptr;
                        };
                        draw(); draw();
                        auto *body = panel(); check(body, "statistics have their own viewport");
                        auto *matrix = ImGui::TableFindByID(body->GetID("##statsComparison_0"));
                        check(matrix, "comparison matrix is rendered");
                        check(matrix->Columns[0].WorkMinX >= matrix->OuterRect.Min.x + ImGui::GetStyle().CellPadding.x - .5f,
                              "metric text keeps inner padding clear of the rounded card edge");
                        matrix->Columns[0].WidthRequest = 12;
                        for (int i = 0; i < 4; ++i) draw();
                        check(matrix->Columns[0].WorkMaxX - matrix->Columns[0].WorkMinX >= ImGui::CalcTextSize("Variance").x,
                              "a saved narrow metric column expands enough to contain its labels");
                        const ImVec2 heading((matrix->Columns[1].MinX + matrix->Columns[1].MaxX) * .5f,
                                             matrix->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + ImGui::GetTextLineHeight() * .5f);
                        for (int i = 0; i < 20; ++i) draw(heading);
                        draw(heading, true);
                        const auto first = logged.find(name);
                        check(first != std::string::npos && logged.find(name, first + name.size()) == std::string::npos,
                              "hovering the header shows an identical display name and source path only once");
                        statistics.findChannel(name)->setLabel("Current alias");
                        draw(heading); draw(heading, true);
                        check(logged.find(name) != std::string::npos && logged.find("Current alias") != std::string::npos,
                              "a distinct alias retains its canonical source in the tooltip");
                        for (const float width : {360.f, 145.f}) {
                                panelWidth = width; draw(); draw(); draw();
                                body = panel(); auto *owner = ImGui::FindWindowByName("statistics-bounds");
                                matrix = ImGui::TableFindByID(body->GetID("##statsComparison_0"));
                                check(body->Pos.y + body->Size.y <= owner->InnerClipRect.Max.y + 1,
                                      "wrapped statistics controls leave the body inside the panel");
                                check(matrix->OuterRect.Max.y <= body->InnerClipRect.Max.y + 1,
                                      "statistics table and its scrollbar stay inside the body viewport");
                        }
                }

                std::puts("PASS: statistics hover delay, overlay geometry, leave grace, Escape, plot drag exclusion and manual divider preservation");
        } catch (const std::exception &exception) {
                std::fprintf(stderr, "FAIL: %s\n", exception.what());
                result = 1;
        }
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        log_deinit(&g_log_ops);
        log_deinit(&g_log_perf);
        return result;
}
