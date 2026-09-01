#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/scope_layout.hpp"
#include "imgui_internal.h"
#include "implot_internal.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

static void
check(bool value, const char *message)
{
        if (!value)
                throw std::runtime_error(message);
}

struct MonitorSearchTestAccess {
        static void query(Monitor &monitor, const char *query)
        {
                std::snprintf(monitor.channelSearch_, sizeof(monitor.channelSearch_), "%s", query);
        }
};

static void
frame(Monitor &monitor, ImVec2 size)
{
        ImGui::GetIO().DisplaySize = size;
        ui::interaction::beginFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        monitor.updateDisplay();
        ImGui::Render();
}

static std::vector<ImGuiWindow *>
scopeWindows()
{
        std::vector<ImGuiWindow *> result;
        for (auto *window : ImGui::GetCurrentContext()->Windows) {
                const std::string name = window->Name;
                const std::string leaf = name.substr(name.rfind('/') + 1);
                if (window->Active && leaf.starts_with("##scope_") && !leaf.starts_with("##scope_area"))
                        result.push_back(window);
        }
        return result;
}

int
main(int, char **)
{
        module_init();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime   = 1.0f / 60.0f;
        // Report recoverable ImGui assertions as test failures without opening
        // the Windows CRT dialog (which would block unattended regressions).
        std::string layoutError;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        ImGui::GetCurrentContext()->ErrorCallbackUserData = &layoutError;
        ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext *, void *data, const char *message) {
                *static_cast<std::string *>(data) = message;
        };
        unsigned char *pixels;
        int            width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try {
                {
                        Monitor fresh("Fresh monitor size regression");
                        fresh.setTitle("Fresh monitor");
                        fresh.addScope("Signal");
                        auto freshFrame = [&] {
                                io.DisplaySize = ImVec2(1000, 700);
                                ImGui::NewFrame();
                                fresh.updateDisplay();
                                ImGui::Render();
                        };
                        for (int i = 0; i < 4; ++i)
                                freshFrame();
                        auto *window = ImGui::FindWindowByName("Fresh monitor###Fresh monitor size regression");
                        check(window && window->Size.x >= 640 && window->Size.y >= 480,
                              "New monitor opens with usable waveform workspace rather than toolbar-only auto-fit");
                        check(window->Pos.x >= 0 && window->Pos.y >= 0 && window->Pos.x + window->Size.x <= 1001 &&
                                  window->Pos.y + window->Size.y <= 701,
                              "New monitor is placed within the viewport");
                        ImGui::SetWindowSize(window, ImVec2(560, 420), ImGuiCond_Always);
                        ImGui::SetWindowPos(window, ImVec2(30, 40), ImGuiCond_Always);
                        freshFrame();
                        check(window->Size.x == 560 && window->Size.y == 420 && window->Pos.x == 30 && window->Pos.y == 40,
                              "Default geometry does not override later manual size or position");
                }
                Monitor monitor("Scope layout regression");
                monitor.externalFeed_ = true;
                for (int i = 0; i < 6; ++i) {
                        const std::string name = "Scope " + std::to_string(i);
                        monitor.addScope(name);
                        auto scope = monitor.getScopes().at(name);
                        scope->addChannel("signal");
                        auto *channel = scope->findChannel("signal");
                        channel->setType("f32");
                        channel->setDevice("LOCAL");
                        for (int sample = 0; sample < 10; ++sample)
                                channel->setRVal(static_cast<float>(sample + i), sample * 0.01);
                        channel->publishSnapshot();
                        const auto uniqueName = "needle_" + std::to_string(i);
                        scope->addChannel(uniqueName);
                        scope->findChannel(uniqueName)->setAddr(0x30000000u + static_cast<usize>(i * 4));
                }
                check(monitor.containsChannelIdentity("signal", 0, "f32") &&
                          !monitor.containsChannelIdentity("signal", 0, "u32") &&
                          !monitor.containsChannelIdentity("signal", 4, "f32"),
                      "recorder identity includes channel name, address and type");
                for (int i = 0; i < 4; ++i)
                        frame(monitor, ImVec2(1000, 700));
                const float minimum = ui::ScopeMinimumHeight(ImGui::GetFontSize(), ImGui::GetFrameHeightWithSpacing() + 15);
                auto        panes   = scopeWindows();
                check(panes.size() == 6, "Stacked view submits every scope");
                for (auto *pane : panes)
                        check(pane->Size.y >= minimum - 1, "Many scopes retain readable waveform height");
                bool scrolls = false;
                for (auto *window : ImGui::GetCurrentContext()->Windows)
                        if (window->Active && std::string(window->Name).find("##scope_area") != std::string::npos)
                                scrolls |= window->ScrollMax.y > 0;
                check(scrolls, "Stacked overflow scrolls instead of compressing all waveforms");
                MonitorSearchTestAccess::query(monitor, "NEEDLE_3");
                for (int i = 0; i < 2; ++i)
                        frame(monitor, ImVec2(1000, 700));
                check(scopeWindows().size() == 6, "Recorder name search keeps the scope overview alongside inline results");
                MonitorSearchTestAccess::query(monitor, "0x30000004");
                for (int i = 0; i < 2; ++i)
                        frame(monitor, ImVec2(1000, 700));
                check(scopeWindows().size() == 6, "Recorder address search keeps the scope overview alongside inline results");
                MonitorSearchTestAccess::query(monitor, "");
                for (int i = 0; i < 2; ++i)
                        frame(monitor, ImVec2(1000, 700));
                check(scopeWindows().size() == 6, "Clearing recorder search preserves every scope");
                auto manual         = monitor.getScopes().at("Scope 0");
                manual->getHeight() = 333;
                manual->setManual(true);
                monitor.toggleScopeFocus("Scope 2");
                for (int i = 0; i < 3; ++i)
                        frame(monitor, ImVec2(1000, 700));
                check(scopeWindows().size() == 1, "Focused view renders one scope");
                check(manual->getHeight() == 333 && manual->isManual(), "Focus preserves manual overview heights");
                check(!monitor.isSamplingPaused() && !monitor.consumeClearDataRequest(),
                      "Focus never pauses or clears acquisition");
                for (const auto &[_, scope] : monitor.getScopes()) {
                        check(!scope->isPaused() && !scope->isHidden(), "Focus leaves scope sampling and visibility intact");
                        check(scope->findChannel("signal")->read_.rawSize() == 10, "Focus preserves every scope's history");
                }
                monitor.restoreScopeOverview();
                for (int i = 0; i < 3; ++i)
                        frame(monitor, ImVec2(1000, 700));
                check(scopeWindows().size() == 6 && manual->getHeight() == 333, "Restore returns all scopes and manual sizing");
                monitor.setScopeLayout(Monitor::ScopeLayout::Grid);
                for (int i = 0; i < 4; ++i)
                        frame(monitor, ImVec2(1800, 1000));
                panes = scopeWindows();
                std::sort(panes.begin(), panes.end(), [](auto *a, auto *b) {
                        return a->Pos.y != b->Pos.y ? a->Pos.y < b->Pos.y : a->Pos.x < b->Pos.x;
                });
                check(panes.size() == 6 && std::abs(panes[0]->Pos.y - panes[1]->Pos.y) < 1 && panes[0]->Pos.x < panes[1]->Pos.x,
                      "Wide grid presents two scopes side by side");
                for (int i = 0; i < 4; ++i)
                        frame(monitor, ImVec2(500, 700));
                panes = scopeWindows();
                for (auto *pane : panes)
                        check(pane->Size.y >= minimum - 1, "Narrow grid falls back without compressing charts");
                check(ui::ScopeGridColumns(500, 18) == 1 && ui::ScopeGridColumns(2000, 27) == 2,
                      "Grid breakpoint respects font size and DPI");
                check(ui::ScopePaneHeight(400, 20, 100, minimum) == minimum,
                      "Large scope count cannot force height below minimum");
                Monitor dragFixture("scope-interactions");
                dragFixture.addScope("first");
                dragFixture.addScope("second");
                for (int i = 0; i < 4; ++i) frame(dragFixture, ImVec2(1000, 900));
                auto dragPanes = scopeWindows();
                std::sort(dragPanes.begin(), dragPanes.end(), [](auto *a, auto *b) { return a->Pos.y < b->Pos.y; });
                check(dragPanes.size() == 2, "Interaction fixture has two visible scopes");
                const ImVec2 background(dragPanes[0]->InnerRect.Min.x + 3, dragPanes[0]->InnerRect.Min.y + 3);
                const ImVec2 originalContent = dragPanes[0]->ContentSize;
                io.AddMousePosEvent(background.x, background.y);
                for (int i = 0; i < 3; ++i) frame(dragFixture, ImVec2(1000, 900));
                check(layoutError.empty(), layoutError.c_str());
                check(dragPanes[0]->ContentSize.x == originalContent.x && dragPanes[0]->ContentSize.y == originalContent.y,
                      "Hovering scope background does not grow layout or scroll extents");
                auto locate = [&](ImGuiWindow *window, const char *label) {
                        const ImGuiID id = window->GetID(label);
                        ImGui::SetFocusID(id, window);
                        window->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                        frame(dragFixture, ImVec2(1000, 900));
                        auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
                        check(rect.Min.x >= 0 && rect.Min.y >= 0, "view/options control exists");
                        return rect.GetCenter();
                };
                auto plotFor = [](ImGuiWindow *pane) {
                        const ImGuiID layoutId = ImHashStr("##plotLayoutTable", 0, pane->ID);
                        return ImPlot::GetCurrentContext()->Plots.GetByKey(ImHashStr("##plot", 0, layoutId));
                };
                auto *firstPlot = plotFor(dragPanes[0]);
                check(firstPlot && firstPlot->FrameRect.Min.y <= dragPanes[0]->WorkRect.Min.y + ImGui::GetStyle().CellPadding.y + 1,
                      "Waveform starts at the scope top without a separate title or action row");
                const ImVec2 from = locate(dragPanes[0], "##scopeTitle");
                const ImVec2 to   = locate(dragPanes[1], "##scopeTitle");
                auto mouse = [&](ImVec2 pos, bool down) {
                        io.AddMousePosEvent(pos.x, pos.y);
                        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                        frame(dragFixture, ImVec2(1000, 900));
                };
                mouse(background, false); mouse(background, true);
                mouse(ImVec2(background.x + 20, background.y), true);
                check(ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType("SCOPE_REORDER"),
                      "Scope blank background still starts dragging without changing layout");
                mouse(background, true); mouse(background, false);
                frame(dragFixture, ImVec2(1000, 900));
                check(layoutError.empty(), layoutError.c_str());
                mouse(from, false); mouse(from, true);
                mouse(ImVec2(from.x + 20, from.y), true);
                check(ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType("SCOPE_REORDER"),
                      "Scope title starts dragging without a grip button");
                mouse(to, true); mouse(to, true); mouse(to, false);
                frame(dragFixture, ImVec2(1000, 900));
                check(dragFixture.getScopes().at("first")->getOrder() > dragFixture.getScopes().at("second")->getOrder(),
                      "Dropping anywhere in another scope reorders scopes");
                const auto body = dragPanes[1]->InnerRect.GetCenter();
                io.AddMousePosEvent(body.x, body.y);
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
                frame(dragFixture, ImVec2(1000, 900));
                io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
                frame(dragFixture, ImVec2(1000, 900));
                frame(dragFixture, ImVec2(1000, 900));
                check(ImGui::GetCurrentContext()->OpenPopupStack.Size > 0,
                      "Scope body offers a right-click menu");
                ImGui::ClosePopupsOverWindow(nullptr, false);
                auto *monitorWindow = ImGui::FindWindowByName("scope-interactions###scope-interactions");
                const auto optionsPoint = locate(monitorWindow, tr("Options", "选项"));
                for (int i = 0; i < 20; ++i) mouse(optionsPoint, false);
                auto *options = ImGui::FindWindowByName("##MonitorOptions_scope-interactions");
                check(!options || !options->Active, "Monitor Options stays closed on hover");
                mouse(optionsPoint, true); mouse(optionsPoint, false);
                options = ImGui::FindWindowByName("##MonitorOptions_scope-interactions");
                check(options && options->Active, "Monitor Options opens on click");
                for (int i = 0; i < 35; ++i) mouse(ImVec2(-1000, -1000), false);
                check(options->Active, "Click-open Options stays open away from the pointer");
                mouse(optionsPoint, false); mouse(optionsPoint, true); mouse(optionsPoint, false);
                check(!options->Active, "Clicking Options again closes it");
                mouse(optionsPoint, true); mouse(optionsPoint, false);
                mouse(background, false); mouse(background, true); mouse(background, false);
                check(!options->Active, "Clicking outside Monitor Options closes it");
                dragPanes = scopeWindows();
                std::sort(dragPanes.begin(), dragPanes.end(), [](auto *a, auto *b) { return a->Pos.y < b->Pos.y; });
                auto visibleScope = dragFixture.getScopes().at("second");
                const auto focusPoint = locate(dragPanes.front(), tr("Focus", "聚焦"));
                const float overviewHeight = visibleScope->getHeight();
                for (int i = 0; i < 26; ++i) mouse(focusPoint, false);
                check(dragFixture.isScopeFocused("second") && scopeWindows().size() == 1,
                      "Dwelling on Focus previews the scope at full height");
                for (int i = 0; i < 8; ++i) mouse(focusPoint, false);
                check(scopeWindows().size() == 1, "Moving layout does not break a stationary focus hover");
                for (int i = 0; i < 4; ++i) mouse(ImVec2(-1000, -1000), false);
                check(!dragFixture.hasFocusedScope() && scopeWindows().size() == 2 && visibleScope->getHeight() == overviewHeight,
                      "Leaving Focus restores all panes and their original height");
                mouse(focusPoint, false); mouse(focusPoint, true); mouse(focusPoint, false);
                frame(dragFixture, ImVec2(1000, 900));
                check(dragFixture.isScopeFocused("second") && scopeWindows().size() == 1,
                      "Floating Focus expands the scope without starting a plot gesture");
                const auto restorePoint = locate(dragPanes.front(), tr("Restore", "还原"));
                mouse(restorePoint, false); mouse(restorePoint, true); mouse(restorePoint, false);
                frame(dragFixture, ImVec2(1000, 900));
                check(!dragFixture.hasFocusedScope() && scopeWindows().size() == 2,
                      "Floating Restore returns to the scope overview");
                dragPanes = scopeWindows();
                std::sort(dragPanes.begin(), dragPanes.end(), [](auto *a, auto *b) { return a->Pos.y < b->Pos.y; });
                const float paneGap = dragPanes[1]->Pos.y - dragPanes[0]->Pos.y - dragPanes[0]->Size.y;
                check(paneGap >= 0 && paneGap <= 7, "Scope cards keep only a compact resize gap");
                const auto morePoint = locate(dragPanes.front(), tr("More", "更多"));
                const ImRect beforeOptions = plotFor(dragPanes.front())->FrameRect;
                for (int i = 0; i < 20; ++i) mouse(morePoint, false);
                char scopeOptionsName[80];
                snprintf(scopeOptionsName, sizeof(scopeOptionsName), "##ScopeOptions_%p", static_cast<void *>(visibleScope.get()));
                auto *scopeOptions = ImGui::FindWindowByName(scopeOptionsName);
                check(!scopeOptions || !scopeOptions->Active, "Scope More stays closed on hover");
                mouse(morePoint, true); mouse(morePoint, false);
                scopeOptions = ImGui::FindWindowByName(scopeOptionsName);
                check(scopeOptions && scopeOptions->Active, "Scope More opens on click");
                const ImRect afterOptions = plotFor(dragPanes.front())->FrameRect;
                check(beforeOptions.Min.x == afterOptions.Min.x && beforeOptions.Min.y == afterOptions.Min.y &&
                          beforeOptions.Max.x == afterOptions.Max.x && beforeOptions.Max.y == afterOptions.Max.y,
                      "Floating More preserves the complete plotting area");
                auto chooseView = [&](const char *label, MonitorScope::DrawEnum mode) {
                        const int  index = mode == MonitorScope::DrawEnum::PLOT ? 0 : 1;
                        const auto seed  = ImHashData(&index, sizeof(index), scopeOptions->GetID("##draw_mode"));
                        ImGui::SetFocusID(ImHashStr(label, 0, seed), scopeOptions);
                        scopeOptions->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                        frame(dragFixture, ImVec2(1000, 900));
                        const auto rect = ImGui::WindowRectRelToAbs(scopeOptions, scopeOptions->NavRectRel[ImGuiNavLayer_Main]);
                        check(rect.Min.x >= 0 && rect.Min.y >= 0, "view segment exists");
                        const auto point = rect.GetCenter();
                        mouse(point, false);
                        mouse(point, true);
                        mouse(point, false);
                        check(visibleScope->getDraw() == mode, "View segment switches the single active renderer");
                };
                chooseView(tr("Table", "表格"), MonitorScope::DrawEnum::TABLE);
                chooseView(tr("Waveform", "波形"), MonitorScope::DrawEnum::PLOT);
                mouse(background, false); mouse(background, true); mouse(background, false);
                check(!scopeOptions->Active, "Clicking outside Scope More closes it");
                check(layoutError.empty(), layoutError.c_str());
                std::puts("PASS: new monitor usable default/manual geometry, stacked/grid/focus, minimum chart height, scroll "
                          "and acquisition preservation");
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
