#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "gui/help_manual.hpp"
#include "gui/plot_style.hpp"
#include "gui/window_corners.hpp"
#include "gui/tutorial_guide.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

struct ScopeInteractionTestAccess {
        static void options(Monitor &monitor, bool show) {
                if (show) monitor.optionsReveal_.setPinned(true);
                else monitor.optionsReveal_.reset();
        }
        static void options(MonitorScope &scope, bool show) {
                if (show) scope.optionsReveal_.setPinned(true);
                else scope.optionsReveal_.reset();
        }
        static void preview(MonitorScope &scope, bool show) {
                if (show) scope.statisticsPeek_.setPinned(true);
                else scope.statisticsPeek_.reset();
        }
};

struct SimSamplerTestAccess {
        inline static bool openJLink = false;
        static void closeJLink(Gui &gui) { gui.jlinkReveal_.close(); }
        static bool save(Gui &gui, const std::string &path) { return gui.saveSession(path); }
        static void dirty(Gui &gui) { gui.isModified_ = true; }
        static bool dirty(const Gui &gui) { return gui.hasSessionChanges(); }
        static bool home(const Gui &gui) { return gui.showHome_; }
        static void variables(Gui &gui) { gui.addVariableWindow(); }
        static void requestNew(Gui &gui) { gui.requestSessionChange(Gui::SessionChange::New); }
        static bool pending(const Gui &gui) { return gui.sessionChange_ != Gui::SessionChange::None; }
        static void cancel(Gui &gui) { gui.sessionChange_ = Gui::SessionChange::None; }
        static void discard(Gui &gui) { gui.applySessionChange(); }
        static void skipSave(Gui &gui) { gui.skipAutoSave_ = true; }
        static void requestQuit(Gui &gui) { gui.showQuitModal_ = true; }
        static void load(Gui &gui, const std::string &path) { gui.loadSession(path); }
        static void commands(Gui &gui)
        {
                gui.showCommands_ = true;
                gui.commandFocus_ = true;
        }
        static void frame(Gui &gui)
        {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ui::interaction::beginFrame();
                ImGui::NewFrame();
                gui.drawBar();
                if (openJLink) {
                        gui.jlinkReveal_.setPinned(true);
                        openJLink = false;
                }
                gui.drawWorkspaceChrome();
                gui.workspaceDock_ = ui::IslandDockSpace(ImGui::GetMainViewport());
                gui.drawHome();
                for (auto &[_, m] : gui.monitors_) {
                        ImGui::SetNextWindowDockID(gui.workspaceDock_, ImGuiCond_Always);
                        m->updateDisplay();
                }
                gui.drawCommands();
                gui.drawSessionChangePrompt();
                ui::DrawDockCorners();
                ui::DrawShortcutManual();
                TutorialGuide::instance().draw();
                // Use the same root popup ordering as Gui::loop(). Its contents
                // are irrelevant here: reopening a lower-priority popup must
                // not make the exit modal repeatedly appear/lose focus.
                if (gui.showQuitModal_) {
                        ImGui::OpenPopup("###Quit");
                        if (ImGui::BeginPopupModal("Quit?###Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::TextUnformatted("Save changes before quitting?");
                                ImGui::Button("Cancel");
                                ImGui::EndPopup();
                        }
                }
                ui::interaction::process();
                ImGui::Render();
                int width, height;
                glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                glViewport(0, 0, width, height);
                glClearColor(.1f, .1f, .1f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                gui.updatePlatformWindows();
        }
};

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}
static std::string
read(const std::filesystem::path &path)
{
        std::ifstream file(path);
        return {(std::istreambuf_iterator<char>(file)), {}};
}
static void capture(const char *name)
{
        int width, height;
        glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
        std::vector<unsigned char> pixels(size_t(width) * height * 3);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream out(name, std::ios::binary);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        for (int row = height - 1; row >= 0; --row)
                out.write(reinterpret_cast<const char *>(pixels.data() + size_t(row) * width * 3), width * 3);
}

int
main(int argc, char **argv)
{
        const bool preview = argc > 1 && std::string(argv[1]) == "--preview";
        module_init();
        glfwInit();
        glfwWindowHint(GLFW_VISIBLE, preview ? GLFW_TRUE : GLFW_FALSE);
        Gui gui;
        SimSamplerTestAccess::skipSave(gui);
        try {
                check(SimSamplerTestAccess::home(gui), "Fresh startup has workspace home without creating data");
                SimSamplerTestAccess::variables(gui);
                check(SimSamplerTestAccess::dirty(static_cast<const Gui &>(gui)), "New panel marks workspace changed");
                const auto path = std::filesystem::current_path() / "workspace-test.ava";
                check(SimSamplerTestAccess::save(gui, path.string()), "Valid session save succeeds");
                const auto baseline = read(path);
                SimSamplerTestAccess::dirty(gui);
                const auto blocked = std::filesystem::current_path() / "directory.ava";
                std::filesystem::create_directories(blocked);
                check(!SimSamplerTestAccess::save(gui, blocked.string()), "Unreplaceable target reports failure");
                check(SimSamplerTestAccess::dirty(static_cast<const Gui &>(gui)), "Failed save preserves unsaved state");
                check(read(path) == baseline, "Failed save leaves prior session untouched");
                SimSamplerTestAccess::requestNew(gui);
                check(SimSamplerTestAccess::pending(gui) && !gui.getVars().empty(), "New session waits for unsaved decision");
                SimSamplerTestAccess::cancel(gui);
                check(!SimSamplerTestAccess::pending(gui) && !gui.getVars().empty(), "Cancel preserves panels");
                SimSamplerTestAccess::requestNew(gui);
                SimSamplerTestAccess::discard(gui);
                check(gui.getVars().empty(), "Explicit discard completes new workspace");

                auto monitor = std::make_shared<Monitor>("Native UX fixture");
                monitor->setTitle("Signals / 数据曲线");
                monitor->setScopeLayout(Monitor::ScopeLayout::Grid);
                for (int i = 0; i < 6; ++i) {
                        const auto name = "Signal " + std::to_string(i + 1);
                        monitor->addScope(name);
                        auto scope = monitor->getScopes().at(name);
                        for (int j = 0; j < 2; ++j) {
                                const auto channelName =
                                    "g_mc.control.position." + std::string(j ? "measured_value" : "reference_value");
                                scope->addChannel(channelName);
                                auto *channel = scope->findChannel(channelName);
                                channel->setDevice("LOCAL");
                                channel->setType("f32");
                                if (i == 0 && j == 0)
                                        channel->unitConversion_ = {true, unit_conversion::Quantity::AngularSpeed,
                                                                   unit_conversion::Unit::CountPerSecond,
                                                                   unit_conversion::Unit::Rpm, 4096};
                                channel->historySeconds_ = 0;
                                for (int k = 0; k < 600; ++k) {
                                        const double time  = k * .01;
                                        const float  value = static_cast<float>(std::sin(time * (i + 1)) * (j ? .9 : 1));
                                        channel->pushBatch(&value, &time, 1);
                                }
                                channel->publishSnapshot();
                        }
                }
                gui.getMonitors()[monitor->getName()] = monitor;
                for (int i = 0; i < 3; ++i)
                        SimSamplerTestAccess::frame(gui);
                const auto *commands = ImGui::FindWindowByName("##WorkspaceCommands");
                const auto *status   = ImGui::FindWindowByName("##WorkspaceStatus");
                check(commands && status, "Native command and status areas exist");
                check(commands->Pos.y + commands->Size.y <= status->Pos.y, "Chrome leaves room for data workspace");
                monitor->toggleScopeFocus("Signal 3");
                check(monitor->hasFocusedScope(), "Focus scope is available");
                SimSamplerTestAccess::frame(gui);
                check(SimSamplerTestAccess::save(gui, path.string()), "Grid layout saves");
                SimSamplerTestAccess::load(gui, path.string());
                check(gui.getMonitors().begin()->second->scopeLayout() == Monitor::ScopeLayout::Grid,
                      "Grid layout round trips");
                check(!gui.getMonitors().begin()->second->hasFocusedScope(), "Transient focus is not saved into a session");
                const auto &conversion = gui.getMonitors().begin()->second->getScopes().at("Signal 1")
                                             ->findChannel("g_mc.control.position.reference_value")->unitConversion_;
                check(conversion.enabled && conversion.quantity == unit_conversion::Quantity::AngularSpeed &&
                          conversion.source == unit_conversion::Unit::CountPerSecond &&
                          conversion.target == unit_conversion::Unit::Rpm && conversion.countsPerRevolution == 4096,
                      "Per-channel hover conversion survives the real workspace save/load path");
                if (!preview) {
                        TutorialGuide::instance().dismiss();
                        monitor->restoreScopeOverview();
                        monitor->setScopeLayout(Monitor::ScopeLayout::Stacked);
                        gui.getMonitors().clear();
                        gui.getMonitors()[monitor->getName()] = monitor;
                        monitor->getScopes().begin()->second->getShowSidePanel() = true;
                        monitor->setSamplingPaused(true);
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("paused-workspace.ppm");
                        auto previewScope = monitor->getScopes().begin()->second;
                        ScopeInteractionTestAccess::options(*monitor, true);
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("monitor-options.ppm");
                        ScopeInteractionTestAccess::options(*monitor, false);
                        ScopeInteractionTestAccess::options(*previewScope, true);
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("scope-options.ppm");
                        ScopeInteractionTestAccess::options(*previewScope, false);
                        previewScope->getShowSidePanel() = false;
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        ScopeInteractionTestAccess::preview(*previewScope, true);
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("statistics-preview.ppm");
                        ScopeInteractionTestAccess::preview(*previewScope, false);
                        previewScope->getShowSidePanel() = true;
                        SimSamplerTestAccess::openJLink = true;
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        check(ImGui::FindWindowByName("##JLinkControls") && ImGui::FindWindowByName("##JLinkControls")->Active,
                              "J-Link toolbar panel opens without stealing popup focus");
                        capture("jlink-panel.ppm");
                        const auto savedStyle = ImGui::GetStyle();
                        const auto savedLanguage = g_lang;
                        g_lang = Lang::ZH;
                        for (bool dark : {false, true}) {
                                SimSamplerTestAccess::closeJLink(gui);
                                ui::ApplyAppStyle(ui::ColorScheme::Ocean, 1.0f, false, dark);
                                ui::ApplyPlotStyle(dark);
                                SimSamplerTestAccess::openJLink = true;
                                for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                                capture(dark ? "jlink-panel-zh-dark.ppm" : "jlink-panel-zh-light.ppm");
                        }
                        int originalWidth, originalHeight;
                        glfwGetWindowSize(glfwGetCurrentContext(), &originalWidth, &originalHeight);
                        for (int width : {1280, 800, 640, 480}) {
                                SimSamplerTestAccess::closeJLink(gui);
                                glfwSetWindowSize(glfwGetCurrentContext(), width, 720);
                                glfwPollEvents();
                                for (int i = 0; i < 5; ++i) SimSamplerTestAccess::frame(gui);
                                auto *toolbar = ImGui::FindWindowByName("##WorkspaceCommands");
                                if (toolbar->DC.CursorMaxPos.x > toolbar->InnerRect.Max.x + 1 ||
                                    toolbar->DC.CursorMaxPos.y > toolbar->InnerRect.Max.y + 1)
                                        fprintf(stderr, "Toolbar overflow at %d: content=(%.0f,%.0f), edge=(%.0f,%.0f)\n",
                                                width, toolbar->DC.CursorMaxPos.x, toolbar->DC.CursorMaxPos.y,
                                                toolbar->InnerRect.Max.x, toolbar->InnerRect.Max.y);
                                check(toolbar->DC.CursorMaxPos.x <= toolbar->InnerRect.Max.x + 1 &&
                                          toolbar->DC.CursorMaxPos.y <= toolbar->InnerRect.Max.y + 1,
                                      "Workspace actions fit narrow and wide viewports without clipping");
                                const auto filename = "workspace-zh-" + std::to_string(width) + ".ppm";
                                capture(filename.c_str());
                                SimSamplerTestAccess::openJLink = true;
                                for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                                auto *panel = ImGui::FindWindowByName("##JLinkControls");
                                auto *viewport = ImGui::GetMainViewport();
                                check(panel && panel->Active && panel->Pos.x >= viewport->Pos.x &&
                                          panel->Pos.y >= viewport->Pos.y &&
                                          panel->Pos.x + panel->Size.x <= viewport->Pos.x + viewport->Size.x + 1 &&
                                          panel->Pos.y + panel->Size.y <= viewport->Pos.y + viewport->Size.y + 1,
                                      "Anchored panels stay within the viewport after resizing");
                                if (width == 480) capture("jlink-panel-zh-narrow.ppm");
                        }
                        glfwSetWindowSize(glfwGetCurrentContext(), originalWidth, originalHeight);
                        glfwPollEvents();
                        ImGui::GetStyle() = savedStyle;
                        g_lang = savedLanguage;
                        SimSamplerTestAccess::closeJLink(gui);
                        ImGui::ClosePopupsOverWindow(nullptr, false);
                        ui::shortcutManualOpen = true;
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        check(ImGui::FindWindowByName("###ShortcutManual")->Active, "Shortcut manual is available");
                        capture("shortcut-manual.ppm");
                        ui::shortcutManualOpen = false;
                        TutorialGuide::instance().start();
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        auto *guide = ImGui::FindWindowByName("##tutorial_step_0");
                        check(guide && guide->Active && guide->Pos.y + guide->Size.y <= ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y,
                              "Tutorial stays within viewport work area");
                        capture("tutorial.ppm");
                        g_lang = Lang::ZH;
                        TutorialGuide::instance().start(TutorialGuide::Topic::Recorder);
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("tutorial-recorder-zh.ppm");
                        TutorialGuide::instance().start(TutorialGuide::Topic::Parameters);
                        TutorialGuide::instance().openCatalog();
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        capture("panel-guides-zh.ppm");
                        glfwSetWindowSize(glfwGetCurrentContext(), 480, 560);
                        glfwPollEvents();
                        TutorialGuide::instance().dismiss();
                        TutorialGuide::instance().openCatalog();
                        for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                        const auto *guideLibrary = ImGui::FindWindowByName("###PanelGuide");
                        const auto *guideViewport = ImGui::GetMainViewport();
                        check(guideLibrary && guideLibrary->Pos.x >= guideViewport->Pos.x &&
                                  guideLibrary->Pos.x + guideLibrary->Size.x <= guideViewport->Pos.x + guideViewport->Size.x,
                              "Live-resized guide library keeps its close button and text in view");
                        capture("panel-guides-zh-narrow.ppm");
                        glfwSetWindowSize(glfwGetCurrentContext(), originalWidth, originalHeight);
                        glfwPollEvents();
                        g_lang = savedLanguage;
                        TutorialGuide::instance().dismiss();
                        SimSamplerTestAccess::commands(gui);
                        SimSamplerTestAccess::frame(gui);
                        SimSamplerTestAccess::frame(gui);
                        const auto *palette = ImGui::FindWindowByName("###CommandPalette");
                        check(palette && palette->Active, "Command palette opens as a real modal");
                        SimSamplerTestAccess::requestQuit(gui);
                        for (int i = 0; i < 4; ++i)
                                SimSamplerTestAccess::frame(gui);
                        const auto *quit = ImGui::FindWindowByName("###Quit");
                        check(quit && quit->Active && !quit->Appearing,
                              "Quit modal stays interactive instead of reopening against the command palette");
                        check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1 &&
                                  ImGui::GetCurrentContext()->OpenPopupStack.back().Window == quit,
                              "Quit keeps a single stable root modal");
                        // A remote open or queued navigation may also arrive
                        // while a modal is open. It must wait behind exit.
                        SimSamplerTestAccess::dirty(gui);
                        SimSamplerTestAccess::requestNew(gui);
                        for (int i = 0; i < 3; ++i)
                                SimSamplerTestAccess::frame(gui);
                        check(quit->Active && !quit->Appearing && SimSamplerTestAccess::pending(gui),
                              "Pending session changes wait without replacing the exit modal");
                } else {
                        // Use the filled buffers for the visual inspection, since session saves configuration only.
                        monitor->restoreScopeOverview();
                        gui.getMonitors().clear();
                        gui.getMonitors()[monitor->getName()] = monitor;
                        gui.loop();
                }
                std::puts("PASS: save failure preserves data, dirty navigation, workspace chrome, multi-scope render, grid "
                          "persistence, command dialog and exit-modal priority");
        } catch (const std::exception &e) {
                std::fprintf(stderr, "FAIL: %s\n", e.what());
                return 1;
        }
        return 0;
}
