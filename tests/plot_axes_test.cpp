#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/plot_fit.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void check(bool val, const char *message) { if (!val) throw std::runtime_error(message); }
struct SimSamplerTestAccess {
        static void save(Gui &gui, const std::string &path) { gui.saveSession(path); }
        static void load(Gui &gui, const std::string &path) { gui.loadSession(path); }
};
struct ScopeInteractionTestAccess {
        static bool showsXAxis(const MonitorScope &s) { return s.showXAxis_; }
        static bool manualY(const MonitorScope &s) { return s.yFit_.manual; }
        static void follow(Monitor &m) { m.viewMode_ = MonitorViewMode::FOLLOW; }
        static bool following(const Monitor &m) { return m.viewMode_ == MonitorViewMode::FOLLOW; }
        static void measure(MonitorScope &s) { s.timeMeasurement().enabled = true; s.timeMeasurement().count = 0; }
        static void clear(MonitorScope &s) { s.timeMeasurement().count = 0; }
        static int count(const MonitorScope &s) { return s.timeMeasurement().count; }
        static double cursor(const MonitorScope &s, int i) { return s.timeMeasurement().cursors[i]; }
        static bool hover(const MonitorScope &s) { return s.hoverSample_.valid; }
        static double x(const MonitorScope &s) { return s.hoverSample_.x; }
        static double y(const MonitorScope &s) { return s.hoverSample_.y; }
        static size_t values(const MonitorScope &s) { return s.hoverValues_.size(); }
        static double commonX(const MonitorScope &s) { return s.hoverCursorX_; }
        static double val(const MonitorScope &s, const std::string &label) {
                for (const auto &v : s.hoverValues_) if (v.label == label && v.valid) return v.y;
                return std::numeric_limits<double>::quiet_NaN();
        }
        static void fft(MonitorScope &s, bool bars) {
                s.showFft_=true; s.fftBars_=bars;
                auto &r=s.fftPublished_["signal"]; r.freqs={0,1,2}; r.mags={10,20,30}; r.df=1;
        }
};
int main(int argc, char **argv) {
        if (argc != 2) return 2; module_init();
        try {
                std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
                check(glfwInit(), "GLFW"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                auto gui = std::make_unique<Gui>(argv[1]);
                g_lang=Lang::ZH;
                auto &io = ImGui::GetIO(); io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
                io.ConfigInputTrickleEventQueue = false; io.IniFilename = nullptr;
                auto totalPoints = [&] {
                        std::lock_guard lock(gui->getMonitorMtx());
                        return Monitor::getGlobalPointCount();
                };
                const auto baseline = totalPoints();
                {
                        Monitor ordinary("ordinary"), embedded("motorVariableMonitor");
                        embedded.externalFeed_ = true;
                        auto fill = [](Monitor &m, int count) {
                                m.addScope("scope");
                                auto scope = m.getScopes().at("scope");
                                scope->addChannel("signal");
                                auto *ch = scope->findChannel("signal");
                                ch->historySeconds_ = 0;
                                for (int i=0; i<count; ++i) ch->setRVal(float(i), i);
                                ch->publishSnapshot();
                                return ch;
                        };
                        fill(ordinary, 17);
                        auto *motor = fill(embedded, 12000);
                        check(totalPoints()==baseline+12017, "global total includes ordinary and embedded monitor snapshots once");
                        embedded.setSamplingPaused(true);
                        embedded.maxDisplayPoints_ = 4;
                        embedded.getScopes().at("scope")->setHidden(true);
                        motor->setShow(false);
                        check(totalPoints()==baseline+12017, "hidden/paused curves and display decimation retain all stored points");
                        motor->read_.pruneByTime(11990, 0);
                        check(totalPoints()==baseline+27, "total tracks retained history rather than lifetime sample count");
                        {
                                Monitor otherTool;
                                auto *array = fill(otherTool, 2);
                                auto snapshot = std::make_shared<flasharray::Snapshot>();
                                snapshot->values.resize(9);
                                array->arraySnapshot_ = snapshot;
                                check(totalPoints()==baseline+36, "array-backed monitor counts its active data without stale raw duplicates");
                                otherTool.clearData();
                                check(totalPoints()==baseline+27, "clearing an embedded monitor removes its points");
                                fill(otherTool, 3);
                                check(totalPoints()==baseline+30, "default-constructed tool monitors participate in totals");
                        }
                        check(totalPoints()==baseline+27, "destroyed tool monitors leave the global registry");
                        embedded.getScopes().clear();
                        check(totalPoints()==baseline+17, "deleting scopes removes their points");
                        ordinary.clearData();
                        check(totalPoints()==baseline, "clearing all remaining curves restores the baseline");
                }
                check(totalPoints()==baseline, "monitor destruction does not leave stale counts");
                {
                        Monitor following("Follow clear regression");
                        following.samplingMode_ = Monitor::SamplingMode::HSS;
                        check(!JLinkPort::instance().isHssRunning(), "LOCAL follow regression runs without J-Link HSS");
                        std::vector<MonitorChannel*> channels;
                        for (const auto *name : {"First", "Second"}) {
                                following.addScope(name);
                                auto scope = following.getScopes().at(name);
                                scope->addChannel("signal");
                                auto *ch = scope->findChannel("signal");
                                ch->setDevice("LOCAL");
                                ch->historySeconds_ = 0;
                                ch->setRVal(1, 100); ch->setRVal(2, 110); ch->publishSnapshot();
                                channels.push_back(ch);
                        }
                        auto followFrame = [&] {
                                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                                io.AddMousePosEvent(-100, -100); io.AddMouseButtonEvent(0, false);
                                ImGui::NewFrame();
                                ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1000,780));
                                following.updateDisplay(); ImGui::Render();
                        };
                        followFrame(); followFrame();
                        ScopeInteractionTestAccess::follow(following);
                        following.linkXMin_ = 50; following.linkXMax_ = 70;
                        followFrame();
                        check(std::abs(following.linkXMax_ - 110) < 1e-8,
                              "switching SDK/LOCAL channels to FOLLOW jumps to their newest data without HSS");
                        auto stableSpan = [&](double span) {
                                check(std::abs(following.linkXMax_ - following.linkXMin_ - span) < 1e-8,
                                      "follow viewport retains its time span after clearing/restarting");
                                check(ScopeInteractionTestAccess::following(following), "clearing keeps follow mode");
                        };
                        stableSpan(20);
                        for (auto *ch : channels) { ch->setRVal(3, 111); ch->publishSnapshot(); }
                        followFrame(); stableSpan(20);
                        check(std::abs(following.linkXMax_ - 111) < 1e-8,
                              "SDK/LOCAL follow advances as new samples arrive");
                        g_monitorPaused.store(true);
                        for (auto *ch : channels) { ch->setRVal(4, 112); ch->publishSnapshot(); }
                        followFrame();
                        check(std::abs(following.linkXMax_ - 111) < 1e-8, "paused SDK/LOCAL follow holds the viewport");
                        g_monitorPaused.store(false); followFrame(); stableSpan(20);
                        check(std::abs(following.linkXMax_ - 112) < 1e-8, "resuming SDK/LOCAL follow catches up immediately");
                        // The actual Ctrl+R action, followed by the sampler's safe point
                        Monitor::clearAll();
                        check(following.consumeClearDataRequest(), "Ctrl+R queues a clear");
                        following.clearData();
                        for (int i=0; i<12; ++i) { followFrame(); stableSpan(20); }
                        following.samplingMode_ = Monitor::SamplingMode::POLL;
                        const double emptyRight = following.linkXMax_;
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        for (int i=0; i<5; ++i) followFrame();
                        check(following.linkXMax_ == emptyRight, "Unconnected SDK/POLL channels do not follow the wall clock");
                        for (int i=0; i<60; ++i) {
                                const double t = i * .002;
                                for (auto *ch : channels) { ch->setRVal(float(i), t); ch->publishSnapshot(); }
                                followFrame(); stableSpan(20);
                                check(std::abs(following.linkXMax_ - t) < 1e-8, "follow right edge tracks restarted samples");
                        }
                        following.historySeconds_ = 5;
                        const double stoppedRight = following.linkXMax_;
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        for (int i=0; i<8; ++i) { followFrame(); stableSpan(5); }
                        check(following.linkXMax_ == stoppedRight, "Stale SDK samples keep the time axis at the last received sample");
                        g_monitorPaused.store(true);
                        Monitor::clearAll();
                        check(following.consumeClearDataRequest(), "repeated Ctrl+R queues a clear while paused");
                        following.clearData(); followFrame(); stableSpan(5);
                        g_monitorPaused.store(false);
                        for (int i=0; i<8; ++i) { followFrame(); stableSpan(5); }
                }
                {
                        Monitor calendar("calendar-axis-test"); calendar.addScope("calendar");
                        auto scope = calendar.getScopes().at("calendar");
                        scope->addChannel("signal"); auto *channel = scope->findChannel("signal");
                        channel->setDevice("LOCAL"); channel->setRVal(1, .125); channel->setRVal(2, 1.125); channel->publishSnapshot();
                        double lo = 0, hi = 2; auto mode = MonitorViewMode::MANUAL;
                        ImPlotPlot *plot = nullptr; float calendarWidth = 1000;
                        auto calendarFrame = [&](ImVec2 mouse = ImVec2(-100, -100), bool left = false, bool right = false) {
                                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                                io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, left); io.AddMouseButtonEvent(1, right);
                                ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(calendarWidth, 650));
                                ImGui::Begin("Calendar axis test");
                                const auto tableId = ImGui::GetID("##plotLayoutTable");
                                scope->menu(true); scope->draw(&lo, &hi, 5000, mode);
                                plot = ImPlot::GetCurrentContext()->Plots.GetByKey(ImHashStr("##plot", 0, tableId));
                                ImGui::End(); ImGui::Render();
                        };
                        calendarFrame(); calendarFrame(); check(plot, "timestamp scope renders a native plot");
                        const auto axis = plot->Axes[ImAxis_X1].HoverRect.GetCenter();
                        calendarFrame(axis); calendarFrame(axis, false, true); calendarFrame(axis); calendarFrame(axis); calendarFrame(axis);
                        ImGuiWindow *popup = nullptr;
                        for (auto *window : ImGui::GetCurrentContext()->Windows)
                                if (window->Active && (window->Flags & ImGuiWindowFlags_Popup)) popup = window;
                        check(popup, "right-clicking the actual time axis opens its format menu");
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        int captureW, captureH; glfwGetFramebufferSize(glfwGetCurrentContext(), &captureW, &captureH);
                        std::vector<unsigned char> capturePixels(captureW * captureH * 3);
                        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, captureW, captureH, GL_RGB, GL_UNSIGNED_BYTE, capturePixels.data());
                        std::ofstream capture(std::filesystem::path(argv[1]).parent_path().parent_path() / "timestamp-menu.ppm", std::ios::binary);
                        capture << "P6\n" << captureW << ' ' << captureH << "\n255\n";
                        for (int y = captureH - 1; y >= 0; --y) capture.write(reinterpret_cast<const char *>(capturePixels.data() + y * captureW * 3), captureW * 3);
                        capture.close();

                        const auto choiceId = ImHashStr("##time_axis_format", 0, popup->ID);
                        const int absolute = 1;
                        // Choice IDs use the native button label within a per-option ID scope.
                        const auto indexId = ImHashData(&absolute, sizeof(absolute), choiceId);
                        const auto buttonId = ImHashStr("绝对时间戳", 0, indexId);
                        popup->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                        ImGui::SetFocusID(buttonId, popup); calendarFrame(axis);
                        auto button = ImGui::WindowRectRelToAbs(popup, popup->NavRectRel[ImGuiNavLayer_Main]).GetCenter();

                        calendarFrame(button); calendarFrame(button, true); calendarFrame(button);
                        check(calendar.timeAxisFormat_ == 1, "native timestamp option switches the linked time axes");
                        calendarFrame(ImVec2(990, 640), true); calendarFrame(); calendarFrame();
                        auto format = [&](double x) {
                                char text[96]{}; auto &a = plot->Axes[ImAxis_X1];
                                a.Formatter(x, text, sizeof(text), a.FormatterData); return std::string(text);
                        };
                        check(format(1e300) == "--", "extreme axis ranges do not overflow timestamp conversion");
                        for (float width : {1000.f, 520.f, 220.f}) {
                                calendarWidth = width; calendarFrame(); calendarFrame();
                                float previousRight = -1e10f;
                                for (const auto &tick : plot->Axes[ImAxis_X1].Ticker.Ticks) {
                                        if (!tick.ShowLabel) continue;
                                        const float left = tick.PixelPos - tick.LabelSize.x * .5f;
                                        const float right = tick.PixelPos + tick.LabelSize.x * .5f;
                                        check(left >= plot->FrameRect.Min.x && right <= plot->FrameRect.Max.x && left > previousRight,
                                              "calendar tick labels stay complete and do not overlap in wide or narrow scopes");
                                        previousRight = right;
                                }
                        }
                        calendarWidth = 1000; calendarFrame(); calendarFrame();
                        const auto before = format(.125);
                        check(before.size() == 26 && before[4] == '-' && before[10] == ' ' && before[19] == '.',
                              "absolute labels contain calendar date, time and six microsecond digits");
                        calendar.setSamplingPaused(true); calendarFrame(); calendarFrame();
                        check(format(.125) == before && channel->read_.rawSize() == 2 && lo == 0 && hi == 2,
                              "format switching and pause preserve sample timestamps and zoom");
                        calendar.importSourcePath_ = "fixture.csv"; calendar.importTimeOriginSeconds_ = 1704067200.0;
                        calendarFrame();
                        const auto imported = format(.123456);
                        check(imported.substr(20) == "123456", "imported timestamps retain all six fractional digits");
                        check(format(86400.123456).substr(0, 10) != imported.substr(0, 10), "crossing midnight changes the calendar date");
                        calendar.importTimeOriginSeconds_ = 0;
                        check(!calendar.absoluteTimeAvailable(), "relative-only imports never invent an absolute date");
                        calendar.importSourcePath_.clear(); calendar.timeAxisFormat_ = 0; calendarFrame();
                        check(format(.125).find('-') == std::string::npos, "relative time can be restored after using timestamps");
                        auto persisted = std::make_shared<Monitor>("calendar-persistence"); persisted->timeAxisFormat_ = 1;
                        gui->getMonitors()[persisted->getName()] = persisted;
                        SimSamplerTestAccess::save(*gui, argv[1]); SimSamplerTestAccess::load(*gui, argv[1]);
                        check(gui->getMonitors().at("calendar-persistence")->timeAxisFormat_ == 1,
                              "time axis preference survives saving and reopening a session");
                        gui->getMonitors().erase("calendar-persistence");
                }

                ui::IndependentYFit fit[2]; ImPlotPlot *plots[2]{};
                double xmin = 0, xmax = 1, time[] = {0, 1}, data[2][2] = {{-2, 8}, {100, 200}};
                auto frame = [&](ImVec2 mouse = ImVec2(-100,-100), float wheel = 0, bool down = false) {
                        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                        io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, down);
                        if (wheel) io.AddMouseWheelEvent(0, wheel);
                        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(900,700));
                        ImGui::Begin("Axis test");
                        for (int i = 0; i < 2; ++i) {
                                ImGui::PushID(i);
                                if (ImPlot::BeginPlot("Scope", ImVec2(-1,250))) {
                                        ImPlot::SetupAxes("Time", "Value"); ImPlot::SetupAxisLinks(ImAxis_X1, &xmin, &xmax);
                                        ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0,ui::yFitPadding));
                                        fit[i].update(true); ImPlot::PlotLine("Signal",time,data[i],2);
                                        plots[i] = ImPlot::GetCurrentPlot(); ImPlot::EndPlot(); ImPlot::PopStyleVar();
                                }
                                ImGui::PopID();
                        }
                        ImGui::End(); ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                };
                frame(); frame();
                auto range = [&] { return plots[0]->Axes[ImAxis_Y1].Range; };
                check(std::abs(range().Min + 3) < 1e-5 && std::abs(range().Max - 9) < 1e-5,
                      "full fit adds exactly 10 percent of the data span above and below");
                auto second = plots[1]->Axes[ImAxis_Y1].Range;
                check(std::abs(second.Min - 90) < 1e-5 && std::abs(second.Max - 210) < 1e-5, "each scope fits its own Y range");
                auto axisCenter = [&] { return plots[0]->Axes[ImAxis_Y1].HoverRect.GetCenter(); };
                const auto original = range(); frame(axisCenter()); frame(axisCenter(), 2);
                const auto zoomed = range();
                check(zoomed.Size() < original.Size() && fit[0].manual, "wheel on Y axis zooms despite full-data display mode");
                frame(); frame();
                check(std::abs(range().Size() - zoomed.Size()) < 1e-6, "manual Y zoom survives subsequent renders");
                check(xmin == 0 && xmax == 1 && plots[1]->Axes[ImAxis_Y1].Range.Min == second.Min,
                      "Y-only zoom does not change linked X or another scope");
                io.MouseDoubleClickTime = .001f;
                auto start = axisCenter(); frame(start); frame(start, 0, true);
                frame(ImVec2(start.x, start.y + 30), 0, true); frame();
                check(std::abs(range().Min - zoomed.Min) > .01 && xmin == 0 && xmax == 1, "dragging Y axis pans only Y");
                fit[0].reset(); frame();
                check(std::abs(range().Min + 3) < 1e-5 && std::abs(range().Max - 9) < 1e-5, "Auto Y restores padded full fit");
                frame(axisCenter(), 2); frame();
                io.MouseDoubleClickTime = .3f;
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                start = axisCenter(); frame(start); frame(start, 0, true); frame(start);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                frame(start, 0, true); frame(start); frame();
                check(!fit[0].manual && std::abs(range().Min + 3) < 1e-5 && std::abs(range().Max - 9) < 1e-5,
                      "native double-click on Y axis restores automatic padded fit");
                MonitorScope recorded("Recorded"); recorded.addChannel("signal");
                check(!recorded.getShowSidePanel(), "scope statistics are hidden by default");
                auto *ch=recorded.findChannel("signal"); ch->historySeconds_=0; ch->setWritable(false);
                ch->setRVal(10, 0); ch->setRVal(20, .37);
                ch->setRVal(std::numeric_limits<float>::quiet_NaN(), 1.1);
                ch->setRVal(25, 1.63); ch->setRVal(30, 2);
                ch->publishSnapshot();
                double recordedMin=.75,recordedMax=1.25;
                auto recordedMode=MonitorViewMode::MANUAL;
                ImPlotPlot *recordedPlot=nullptr;
                auto recordedFrame=[&](ImVec2 mouse=ImVec2(-100,-100),bool down=false,float wheel=0){
                        ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();
                        io.AddMousePosEvent(mouse.x,mouse.y);io.AddMouseButtonEvent(0,down);
                        if (wheel) io.AddMouseWheelEvent(0,wheel);
                        ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(900,700));
                        ImGui::Begin("Recorded scope test"); recorded.menu(true);
                        recorded.draw(&recordedMin,&recordedMax,5000,recordedMode);
                        ImGui::End();ImGui::Render();ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        auto &allPlots=ImPlot::GetCurrentContext()->Plots;
                        for(int i=0;i<allPlots.GetBufSize();++i){
                                auto *p=allPlots.GetByIndex(i);
                                if(p!=plots[0]&&p!=plots[1])recordedPlot=p;
                        }
                };
                recordedFrame(); recordedFrame(); check(recordedPlot,"recorded plot exists");
                start=recordedPlot->PlotRect.GetCenter();
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                recordedFrame(start);recordedFrame(start,true);recordedFrame(start);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                recordedFrame(start,true);recordedFrame(start);recordedFrame();
                check(recordedMode==MonitorViewMode::FULL && std::abs(recordedMin)<1e-6 && std::abs(recordedMax-2)<1e-6,
                      "double-clicking a zoomed recorded scope restores the entire recorded time range in one gesture");
                check(std::abs(recordedPlot->Axes[ImAxis_Y1].Range.Min-8)<1e-5 &&
                      std::abs(recordedPlot->Axes[ImAxis_Y1].Range.Max-32)<1e-5,
                      "recorded double-click restores full padded Y fit");
                recordedFrame(recordedPlot->Axes[ImAxis_Y1].HoverRect.GetCenter());
                recordedFrame(recordedPlot->Axes[ImAxis_Y1].HoverRect.GetCenter(), false, 2);
                check(ScopeInteractionTestAccess::manualY(recorded), "Y-axis zoom can override full-view automatic fitting");
                recordedFrame(recordedPlot->Axes[ImAxis_X1].HoverRect.GetCenter());
                recordedFrame(recordedPlot->Axes[ImAxis_X1].HoverRect.GetCenter(), false, 2);
                recordedFrame();
                check(recordedMode == MonitorViewMode::FOLLOW && !ScopeInteractionTestAccess::manualY(recorded),
                      "X-axis zoom enters FOLLOW with automatic Y fit, clearing the previous manual Y range");
                ch->getGain() = 10;
                recordedFrame(); recordedFrame();
                check(recordedPlot->Axes[ImAxis_Y1].Range.Max >= 200,
                      "FOLLOW Y range automatically adapts to changing displayed amplitudes");
                recordedFrame(recordedPlot->PlotRect.GetCenter());
                recordedFrame(recordedPlot->PlotRect.GetCenter(), false, 1);
                check(!ScopeInteractionTestAccess::manualY(recorded), "plot-area X zoom keeps automatic Y fitting in FOLLOW");
                recordedFrame(recordedPlot->Axes[ImAxis_Y1].HoverRect.GetCenter());
                recordedFrame(recordedPlot->Axes[ImAxis_Y1].HoverRect.GetCenter(), false, 2);
                const auto manualFollowY = recordedPlot->Axes[ImAxis_Y1].Range;
                ch->getGain() = 100;
                recordedFrame(); recordedFrame();
                check(ScopeInteractionTestAccess::manualY(recorded) &&
                      std::abs(recordedPlot->Axes[ImAxis_Y1].Range.Min - manualFollowY.Min) < 1e-6 &&
                      std::abs(recordedPlot->Axes[ImAxis_Y1].Range.Max - manualFollowY.Max) < 1e-6,
                      "deliberate Y-axis zoom in FOLLOW remains manual across subsequent frames");
                ch->getGain() = 1;
                recordedMode = MonitorViewMode::FULL;
                recordedMin = 0; recordedMax = 2;
                recorded.resetYFit(); recordedFrame(); recordedFrame();
                auto pixel = [&](double x, double y) {
                        return ImVec2(recordedPlot->Axes[ImAxis_X1].PlotToPixels(x), recordedPlot->Axes[ImAxis_Y1].PlotToPixels(y));
                };
                using Inspect = ScopeInteractionTestAccess;
                recordedFrame(pixel(.37,20));
                recordedFrame(pixel(.37,20));  // ImPlot's AllowOverlap hover activates on the following frame
                check(Inspect::hover(recorded) && std::abs(Inspect::x(recorded)-.37)<1e-6 && Inspect::y(recorded)==20,
                      "hover reports the actual irregularly sampled time and value");
                ch->getGain()=-2; ch->getBias()=100; recordedFrame(); recordedFrame();
                recordedFrame(pixel(.37,60));
                recordedFrame(pixel(.37,60));
                check(Inspect::hover(recorded) && Inspect::y(recorded)==60, "hover respects gain, bias and displayed units");
                ch->setShow(false); recordedFrame(pixel(.37,60));
                check(!Inspect::hover(recorded), "hidden curves cannot provide hover samples");
                ch->setShow(true); ch->getGain()=1; ch->getBias()=0; recordedFrame(); recordedFrame();
                recordedFrame(pixel(1.1,22));
                check(!Inspect::hover(recorded), "missing samples do not interpolate a misleading hover value");
                recorded.addChannel("target");
                auto *target=recorded.findChannel("target"); target->historySeconds_=0;
                target->setRVal(0,0); target->setRVal(40,2); target->publishSnapshot();
                recorded.addChannel("partial");
                auto *partial=recorded.findChannel("partial"); partial->historySeconds_=0;
                partial->setRVal(5,.5); partial->setRVal(15,1.5); partial->publishSnapshot();
                recordedFrame(); recordedFrame();
                recordedFrame(pixel(.2,10)); recordedFrame(pixel(.2,10));
                check(Inspect::values(recorded)==3 && std::abs(Inspect::commonX(recorded)-.2)<.003,
                      "vertical cursor includes every visible curve independently of mouse Y");
                check(std::abs(Inspect::val(recorded,"signal")-(10+10*Inspect::commonX(recorded)/.37))<1e-4 &&
                      std::abs(Inspect::val(recorded,"target")-20*Inspect::commonX(recorded))<1e-4 && std::isnan(Inspect::val(recorded,"partial")),
                      "shared time interpolation handles different sample grids without extrapolating");
                ch->getGain()=-2; ch->getBias()=100;
                recordedFrame(pixel(.2,10)); recordedFrame(pixel(.2,10));
                check(std::abs(Inspect::val(recorded,"signal")-(100-2*(10+10*Inspect::commonX(recorded)/.37)))<1e-4,
                      "cursor values use displayed gain and bias even outside Y range");
                ch->getGain()=1; ch->getBias()=0;
                target->getPlotStyle()=1; recordedFrame(pixel(.2,10)); recordedFrame(pixel(.2,10));
                check(Inspect::val(recorded,"target")==0,"stairs cursor holds previous value instead of interpolating");
                target->getPlotStyle()=0;
                recordedFrame(pixel(.8,22)); recordedFrame(pixel(.8,22));
                check(std::isnan(Inspect::val(recorded,"signal")) && std::abs(Inspect::val(recorded,"target")-20*Inspect::commonX(recorded))<1e-4,
                      "one curve's gap does not hide other values or bridge missing samples");
                auto array=std::make_shared<flasharray::Snapshot>(); array->step=1; array->values={0,100,200};
                partial->arraySnapshot_=array;
                recordedFrame(pixel(.2,10)); recordedFrame(pixel(.2,10));
                check(std::abs(Inspect::val(recorded,"partial")-100*Inspect::commonX(recorded))<1e-4,"array waveform uses the same cursor time");
                {
                        int width,height; glfwGetFramebufferSize(glfwGetCurrentContext(),&width,&height);
                        std::vector<unsigned char> pixels(width*height*3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1);
                        glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
                        std::ofstream image(std::string(argv[1])+".cursor.ppm",std::ios::binary);
                        image<<"P6\n"<<width<<' '<<height<<"\n255\n";
                        for(int y=height-1;y>=0;--y) image.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
                }
                target->setShow(false); partial->setShow(false); partial->arraySnapshot_.reset();
                recordedFrame(pixel(.2,10));
                check(Inspect::values(recorded)==1,"hidden curves excluded from cursor readout");
                recordedFrame(); check(!Inspect::values(recorded),"cursor readout cleared after mouse leaves plot");
                Inspect::measure(recorded); recordedFrame();
                auto a=pixel(.37,20), b=pixel(1.63,25);
                recordedFrame(a); recordedFrame(a,true); recordedFrame(a);
                recordedFrame(b); recordedFrame(b,true); recordedFrame(b);
                check(Inspect::count(recorded)==2 && std::abs(Inspect::cursor(recorded,0)-.37)<1e-6 &&
                      std::abs(Inspect::cursor(recorded,1)-1.63)<1e-6, "two clicks snap rulers to actual sample timestamps");
                check(std::abs(std::abs(Inspect::cursor(recorded,1)-Inspect::cursor(recorded,0))-1.26)<1e-6,
                      "measurement uses elapsed time, not sample index or number of samples");
                check(recordedMode==MonitorViewMode::MANUAL && recordedMin==0 && recordedMax==2,
                      "measurement holds the visible time window without zooming or fitting");
                {
                        int width, height; glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                        std::vector<unsigned char> pixels(width * height * 3);
                        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                        std::ofstream image(std::string(argv[1]) + ".ppm", std::ios::binary);
                        image << "P6\n" << width << ' ' << height << "\n255\n";
                        for (int y=height-1; y>=0; --y)
                                image.write(reinterpret_cast<char *>(pixels.data()+y*width*3), width*3);
                }
                const auto beforeY=recordedPlot->Axes[ImAxis_Y1].Range;
                auto grab=pixel(1.63,15), moved=pixel(.2,15);
                recordedFrame(grab); recordedFrame(grab,true); recordedFrame(moved,true); recordedFrame(moved);
                check(std::abs(Inspect::cursor(recorded,1)-.2)<.01 && Inspect::cursor(recorded,1)<Inspect::cursor(recorded,0),
                      "rulers can be dragged freely past each other");
                check(recordedMin==0 && recordedMax==2 &&
                      std::abs(recordedPlot->Axes[ImAxis_Y1].Range.Min-beforeY.Min)<1e-6,
                      "dragging a ruler does not pan or fit the axes");
                check(ImPlot::GetInputMap().Pan==ImGuiMouseButton_Left, "measurement restores other scopes' input mapping");
                Inspect::fft(recorded,false); recordedFrame(); recordedFrame();
                recordedFrame(pixel(.2,15)); recordedFrame(pixel(.2,15));
                check(std::abs(Inspect::val(recorded,"signal")-(10+10*Inspect::commonX(recorded)))<1e-4,"FFT line cursor uses common frequency");
                Inspect::fft(recorded,true); recordedFrame(pixel(.2,15)); recordedFrame(pixel(.2,15));
                check(Inspect::val(recorded,"signal")==10,"FFT bar cursor reports the intersected bin");
                recordedFrame(pixel(.5,15)); recordedFrame(pixel(.5,15));
                check(std::isnan(Inspect::val(recorded,"signal")),"FFT bar gaps do not invent a magnitude");
                {
                        Monitor linked("linkedCursorTest");
                        linked.externalFeed_ = true;
                        linked.linkScopeCursor_ = true;
                        for (int i=0; i<2; ++i) {
                                const std::string name = i ? "Speed" : "Position";
                                linked.addScope(name);
                                auto scope = linked.getScopes().at(name);
                                scope->setOrder(i); scope->addChannel("feedback");
                                auto *channel = scope->findChannel("feedback");
                                channel->historySeconds_ = 0;
                                channel->setRVal(i ? 100 : 0, 0);
                                channel->setRVal(i ? 200 : 10, 2);
                                channel->publishSnapshot();
                        }
                        auto first=linked.getScopes().at("Position"), second=linked.getScopes().at("Speed");
                        std::vector<ImPlotPlot*> previous;
                        auto &pool=ImPlot::GetCurrentContext()->Plots;
                        for(int i=0;i<pool.GetBufSize();++i) previous.push_back(pool.GetByIndex(i));
                        auto linkedFrame=[&](ImVec2 mouse=ImVec2(-100,-100), bool down=false) {
                                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                                io.AddMousePosEvent(mouse.x,mouse.y);
                                io.AddMouseButtonEvent(0,down);
                                ImGui::NewFrame();
                                ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1000,680));
                                linked.updateDisplay();
                                ImGui::Render();
                                glClear(GL_COLOR_BUFFER_BIT);
                                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        };
                        linkedFrame(); linkedFrame(); linkedFrame(); linkedFrame();
                        std::vector<ImPlotPlot*> linkedPlots;
                        for(int i=0;i<pool.GetBufSize();++i) {
                                auto *plot=pool.GetByIndex(i);
                                if(std::find(previous.begin(),previous.end(),plot)==previous.end()) linkedPlots.push_back(plot);
                        }
                        check(linkedPlots.size()==2,"two linked plots");
                        std::sort(linkedPlots.begin(),linkedPlots.end(),[](auto *a,auto *b){return a->PlotRect.Min.y<b->PlotRect.Min.y;});
                        check((linkedPlots[0]->Axes[ImAxis_X1].Flags & ImPlotAxisFlags_NoTickLabels) &&
                              !(linkedPlots[1]->Axes[ImAxis_X1].Flags & ImPlotAxisFlags_NoTickLabels),
                              "stacked time scopes show one shared X axis on the bottom plot");
                        check(std::abs(linkedPlots[0]->PlotRect.Min.x-linkedPlots[1]->PlotRect.Min.x)<1 &&
                              std::abs(linkedPlots[0]->PlotRect.Max.x-linkedPlots[1]->PlotRect.Max.x)<1,
                              "shared time-axis plots align despite different Y tick widths");
                        second->setHidden(true); linkedFrame(); linkedFrame();
                        check(Inspect::showsXAxis(*first), "hiding the bottom scope moves the shared X axis to the last visible plot");
                        second->setHidden(false); linkedFrame(); linkedFrame();
                        first->setOrder(1); second->setOrder(0); linkedFrame(); linkedFrame();
                        check(Inspect::showsXAxis(*first) && !Inspect::showsXAxis(*second),
                              "scope reordering moves the shared X axis with display order");
                        first->setOrder(0); second->setOrder(1); linkedFrame(); linkedFrame();
                        second->getShowFft()=true; linkedFrame(); linkedFrame();
                        check(Inspect::showsXAxis(*first) && Inspect::showsXAxis(*second),
                              "FFT retains its frequency axis while the last time plot retains the time axis");
                        second->getShowFft()=false; linkedFrame(); linkedFrame();
                        auto onPlot=[&](int i,double t) {
                                return ImVec2(linkedPlots[i]->Axes[ImAxis_X1].PlotToPixels(t),linkedPlots[i]->PlotRect.GetCenter().y);
                        };
                        linkedFrame(onPlot(1,.7)); linkedFrame(onPlot(1,.7));
                        check(Inspect::values(*first)==1 && Inspect::values(*second)==1,"hovering lower scope measures all scopes on the same frame");
                        check(Inspect::commonX(*first)==Inspect::commonX(*second) &&
                              std::abs(Inspect::val(*first,"feedback")-5*Inspect::commonX(*first))<1e-4 &&
                              std::abs(Inspect::val(*second,"feedback")-(100+50*Inspect::commonX(*first)))<1e-4,
                              "linked scopes use identical time with independent values");
                        linkedFrame(onPlot(0,1.4)); linkedFrame(onPlot(0,1.4));
                        check(Inspect::commonX(*first)==Inspect::commonX(*second) && std::abs(Inspect::commonX(*second)-1.4)<.01,
                              "switching hover source updates all scopes without a frame of lag");
                        {
                                int width,height; glfwGetFramebufferSize(glfwGetCurrentContext(),&width,&height);
                                std::vector<unsigned char> pixels(width*height*3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1);
                                glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
                                std::ofstream image(std::string(argv[1])+".linked.ppm",std::ios::binary);
                                image<<"P6\n"<<width<<' '<<height<<"\n255\n";
                                for(int y=height-1;y>=0;--y) image.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
                        }
                        linkedFrame(); check(!Inspect::values(*first)&&!Inspect::values(*second),"linked cursors clear outside plots");
                        linked.linkScopeCursor_=false;
                        linkedFrame(onPlot(1,.7)); linkedFrame(onPlot(1,.7));
                        check(!Inspect::values(*first)&&Inspect::values(*second)==1,"disabling checkbox restores single-scope cursor");
                        // Time rulers share state even when linked hover readouts are disabled
                        Inspect::measure(*first); linkedFrame(); linkedFrame();
                        auto clickTime=[&](int scope,double time) {
                                auto point=onPlot(scope,time);
                                linkedFrame(point); linkedFrame(point);
                                linkedFrame(point,true); linkedFrame(point);
                        };
                        clickTime(0,.35);
                        check(Inspect::count(*first)==1 && Inspect::count(*second)==1,
                              "first ruler point is shared with other scopes");
                        clickTime(1,1.55);
                        check(Inspect::count(*first)==2 && Inspect::count(*second)==2 &&
                              Inspect::cursor(*first,0)==Inspect::cursor(*second,0) &&
                              Inspect::cursor(*first,1)==Inspect::cursor(*second,1) &&
                              std::abs(Inspect::cursor(*second,1)-Inspect::cursor(*first,0)-1.2)<.01,
                              "A and B selected in different scopes measure the same elapsed time");
                        const double heldMin=linked.linkXMin_, heldMax=linked.linkXMax_;
                        auto ruler=onPlot(0,Inspect::cursor(*second,1)), dragTo=onPlot(0,1.2);
                        linkedFrame(ruler); linkedFrame(ruler); linkedFrame(ruler,true);
                        linkedFrame(dragTo,true); linkedFrame(dragTo); linkedFrame();
                        check(std::abs(Inspect::cursor(*second,1)-1.2)<.01 &&
                              linked.linkXMin_==heldMin && linked.linkXMax_==heldMax,
                              "B selected in lower scope can be dragged in upper scope without moving axes");
                        {
                                int width,height; glfwGetFramebufferSize(glfwGetCurrentContext(),&width,&height);
                                std::vector<unsigned char> pixels(width*height*3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1);
                                glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
                                std::ofstream image(std::string(argv[1])+".cross-time.ppm",std::ios::binary);
                                image<<"P6\n"<<width<<' '<<height<<"\n255\n";
                                for(int y=height-1;y>=0;--y) image.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
                        }
                        Inspect::clear(*second);
                        check(Inspect::count(*first)==0,"clear from another scope clears shared measurement");
                        clickTime(1,1.6); clickTime(0,.4);
                        check(Inspect::count(*first)==2 &&
                              std::abs(std::abs(Inspect::cursor(*first,1)-Inspect::cursor(*first,0))-1.2)<.01,
                              "cross-scope measurement supports bottom-to-top and reverse time selection");
                        {
                                Monitor separate("separateMeasurement"); separate.addScope("scope");
                                check(Inspect::count(*separate.getScopes().at("scope"))==0,
                                      "measurement is isolated between monitors");
                        }
                        Inspect::clear(*first);
                        linked.linkScopeCursor_=true;
                        Inspect::fft(*second,false);
                        linkedFrame(); linkedFrame(); linkedFrame(onPlot(1,.7)); linkedFrame(onPlot(1,.7));
                        check(!Inspect::values(*first),"FFT frequency is not broadcast as time");
                        clickTime(1,.7);
                        check(Inspect::count(*first)==0,"FFT clicks cannot create shared time measurement points");
                }
                {
                        Monitor fixed("scrollScopeMonitor");
                        fixed.setSamplingPaused(true);
                        for (const char *scopeName : {"First", "Second"}) {
                                fixed.addScope(scopeName);
                                auto scope = fixed.getScopes().at(scopeName);
                                check(scope->getCompareStatistics(), "new scopes default to comparison by metric");
                                scope->getCompareStatistics() = false;
                                scope->getShowSidePanel() = true;
                                for (int i = 0; i < 6; ++i) {
                                        const auto name = "signal" + std::to_string(i);
                                        scope->addChannel(name);
                                        auto *channel = scope->findChannel(name);
                                        channel->historySeconds_ = 0;
                                        channel->setRVal(float(i), 0);
                                        channel->setRVal(float(i + 1), 1);
                                        channel->publishSnapshot();
                                }
                        }
                        auto scrollFrame = [&](ImVec2 mouse = ImVec2(-100,-100), float wheel = 0, bool down = false) {
                                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                                io.AddMousePosEvent(mouse.x, mouse.y);
                                io.AddMouseButtonEvent(0, down);
                                if (wheel) io.AddMouseWheelEvent(0, wheel);
                                ImGui::NewFrame();
                                ImGui::SetNextWindowPos(ImVec2(0,0));
                                ImGui::SetNextWindowSize(ImVec2(1200,700));
                                fixed.updateDisplay();
                                ImGui::Render();
                                glClear(GL_COLOR_BUFFER_BIT);
                                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        };
                        scrollFrame(); scrollFrame(); scrollFrame();
                        std::vector<ImGuiWindow *> panels;
                        for (auto *window : ImGui::GetCurrentContext()->Windows) {
                                const std::string name = window->Name;
                                const std::string leaf = name.substr(name.rfind('/') + 1);
                                if (window->WasActive && strstr(window->Name, "scrollScopeMonitor") &&
                                    leaf.starts_with("##plotSidePanel"))
                                        panels.push_back(window);
                        }
                        check(panels.size() == 2, "each scope has its own statistics viewport");
                        std::sort(panels.begin(), panels.end(), [](auto *a, auto *b) { return a->Pos.y < b->Pos.y; });
                        auto *firstScope = panels[0]->ParentWindow;
                        auto *secondScope = panels[1]->ParentWindow;
                        check(panels[0]->ScrollMax.y > 100 && panels[1]->ScrollMax.y > 100,
                              "multiple statistics tables overflow only their own viewport");
                        check(!firstScope->ScrollbarY && firstScope->ScrollMax.y < 1 &&
                              !secondScope->ScrollbarY && secondScope->ScrollMax.y < 1,
                              "statistics cannot create a vertical scrollbar around the plot");
                        const auto scopePosition = firstScope->Pos;
                        const auto panelPosition = panels[0]->Pos;
                        const auto overPanel = panels[0]->InnerClipRect.GetCenter();
                        scrollFrame(overPanel); scrollFrame(overPanel); scrollFrame(overPanel, -4);
                        scrollFrame(overPanel); scrollFrame(overPanel);
                        check(panels[0]->Scroll.y > 0 && panels[1]->Scroll.y == 0,
                              "wheel scrolls only the hovered scope statistics");
                        check(firstScope->Scroll.y == 0 && secondScope->Scroll.y == 0 &&
                              firstScope->Pos.y == scopePosition.y && panels[0]->Pos.y == panelPosition.y,
                              "scrolling statistics keeps the plot and other scopes fixed");
                        scrollFrame(overPanel, -100); scrollFrame(overPanel); scrollFrame(overPanel);
                        check(panels[0]->Scroll.y >= panels[0]->ScrollMax.y - 1,
                              "the last statistics table remains reachable");
                        {
                                int width, height;
                                glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                                std::vector<unsigned char> pixels(width * height * 3);
                                glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                                std::ofstream image(std::string(argv[1]) + ".scroll.ppm", std::ios::binary);
                                image << "P6\n" << width << ' ' << height << "\n255\n";
                                for (int y = height - 1; y >= 0; --y)
                                        image.write(reinterpret_cast<char *>(pixels.data() + y * width * 3), width * 3);
                        }
                        auto first = fixed.getScopes().at("First");
                        auto clickStatisticsMode = [&](int index) {
                                const auto tableId = firstScope->GetID("##plotLayoutTable");
                                const auto choiceId = ImHashStr("##statisticsMode", 0, tableId);
                                const auto optionId = ImHashData(&index, sizeof(index), choiceId);
                                const auto id = ImHashStr(index == 0 ? tr("Compare channels", "通道对比") : tr("Per channel", "逐通道"), 0, optionId);
                                firstScope->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                                ImGui::SetFocusID(id, firstScope); scrollFrame();
                                const auto selector = ImGui::WindowRectRelToAbs(firstScope, firstScope->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                                check(selector.x > 0 && selector.y > 0, "statistics mode segments are visible");
                                scrollFrame(selector); scrollFrame(selector, 0, true); scrollFrame(selector);
                                scrollFrame(); scrollFrame();
                        };
                        const float previousPanelWidth = panels[0]->Size.x;
                        clickStatisticsMode(0);
                        check(first->getCompareStatistics() && !fixed.getScopes().at("Second")->getCompareStatistics(),
                              "segmented choice changes only the selected scope to comparison by metric");
                        check(std::abs(panels[0]->Size.x - previousPanelWidth) < 3,
                              "switching comparison mode preserves the statistics divider width");
                        auto *comparison = ImGui::TableFindByID(panels[0]->GetID("##statsComparison_0"));
                        check(comparison && comparison->ColumnsCount == 7 && comparison->FreezeColumnsRequest == 1 &&
                              comparison->FreezeRowsRequest == 1 && comparison->InnerWindow->ScrollMax.x > 0,
                              "one comparison matrix holds all six channels with fixed metric labels and horizontal scrolling");
                        check(comparison->Columns[1].UserID == ImHashStr("signal0") &&
                              first->getChannelMean("signal0") == .5 && first->getChannelMean("signal5") == 5.5,
                              "comparison retains channel identity, display order and computed statistics");
                        first->findChannel("signal0")->setLabel("Alias A");
                        first->findChannel("signal0")->setOrder(50);
                        first->findChannel("signal2")->setShow(false);
                        scrollFrame(); scrollFrame();
                        comparison = ImGui::TableFindByID(panels[0]->GetID("##statsComparison_0"));
                        check(comparison->ColumnsCount == 6 && comparison->Columns[1].UserID == ImHashStr("signal1") &&
                              comparison->Columns[5].UserID == ImHashStr("signal0") &&
                              std::strcmp(comparison->ColumnsNames.c_str() + comparison->Columns[5].NameOffset, "Alias A") == 0,
                              "comparison respects hidden channels, aliases and manual channel order");
                        {
                                int width, height;
                                glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                                std::vector<unsigned char> pixels(width * height * 3);
                                glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                                std::ofstream image(std::string(argv[1]) + ".comparison.ppm", std::ios::binary);
                                image << "P6\n" << width << ' ' << height << "\n255\n";
                                for (int y = height - 1; y >= 0; --y)
                                        image.write(reinterpret_cast<char *>(pixels.data() + y * width * 3), width * 3);
                        }
                        clickStatisticsMode(1);
                        check(!first->getCompareStatistics() && panels[0]->ScrollMax.y > 100,
                              "switching back restores independent channel tables and vertical scrolling");
                        first->setDraw(MonitorScope::DrawEnum::TABLE);
                        for (int i = 6; i < 40; ++i)
                                first->addChannel("signal" + std::to_string(i));
                        scrollFrame(); scrollFrame();
                        auto *variableTable = ImGui::TableFindByID(firstScope->GetID("MonitorTable"));
                        check(variableTable && variableTable->FreezeRowsRequest == 1 && variableTable->InnerWindow->ScrollMax.y > 100,
                              "variable table view retains vertical scrolling with its header fixed");
                        auto overTable = variableTable->InnerWindow->InnerClipRect.GetCenter();
                        scrollFrame(overTable); scrollFrame(overTable); scrollFrame(overTable, -4);
                        scrollFrame(overTable); scrollFrame(overTable);
                        check(variableTable->InnerWindow->Scroll.y > 0 && firstScope->Scroll.y == 0,
                              "variable table rows scroll with the wheel while scope controls stay fixed");
                        first->setDraw(MonitorScope::DrawEnum::PLOT);
                        scrollFrame(); scrollFrame(); scrollFrame();
                        check(firstScope->Scroll.y == 0 && firstScope->ScrollMax.y < 1 && !firstScope->ScrollbarY,
                              "switching back to a plot clears table scrolling");
                }
                {
                        auto persisted = std::make_shared<Monitor>("StatisticsModePersistence");
                        persisted->addScope("Metric default"); persisted->addScope("Channel detail");
                        persisted->getScopes().at("Channel detail")->getCompareStatistics() = false;
                        gui->getMonitors().emplace(persisted->getName(), persisted);
                        const auto path = std::string(argv[1]) + ".statistics.ava";
                        SimSamplerTestAccess::save(*gui, path);
                        SimSamplerTestAccess::load(*gui, path);
                        auto loaded = gui->getMonitors().at(persisted->getName());
                        check(loaded->getScopes().at("Metric default")->getCompareStatistics() &&
                              !loaded->getScopes().at("Channel detail")->getCompareStatistics(),
                              "each scope statistics mode survives a session save and reload");
                        std::ifstream input(path);
                        const std::string text((std::istreambuf_iterator<char>(input)), {});
                        auto *document = cJSON_Parse(text.c_str());
                        auto *monitors = cJSON_GetObjectItem(document, "monitors");
                        auto *scopes = cJSON_GetObjectItem(cJSON_GetArrayItem(monitors, 0), "scopes");
                        cJSON *scope;
                        cJSON_ArrayForEach(scope, scopes) cJSON_DeleteItemFromObject(scope, "compareStatistics");
                        char *legacy = cJSON_Print(document);
                        std::ofstream(path) << legacy;
                        cJSON_free(legacy); cJSON_Delete(document);
                        SimSamplerTestAccess::load(*gui, path);
                        loaded = gui->getMonitors().at(persisted->getName());
                        for (const auto &[_, scope] : loaded->getScopes())
                                check(scope->getCompareStatistics(), "legacy sessions default to comparison by metric");
                }
                gui.reset(); puts("PASS: absolute timestamps, native axis menu, microseconds, date rollover, narrow labels, persistence, independent axes and measurement"); return 0;
        } catch (const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
