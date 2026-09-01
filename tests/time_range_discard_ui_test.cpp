// Real mouse input against the production Scope toolbar/ImPlot interaction
#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

struct ScopeInteractionTestAccess {
    static void monitorToolbar(Monitor &m) { m.drawDiscardSelectionControls(); }
    static void cancel(Monitor &m) { m.setDiscardSelectionEnabled(false); }
    static void sync(Monitor &m, bool enabled) { m.linkScopeCursor_ = enabled; }
    static bool selecting(const MonitorScope &s) { return s.discardSelection_.enabled; }
    static bool selected(const MonitorScope &s) { return s.discardSelection_.hasRange; }
    static size_t selectedPoints(const MonitorScope &s) { return s.selectedDiscardPointCount(); }
    static void measure(MonitorScope &s) {
        s.timeMeasurement().enabled = true;
        s.timeMeasurement().count = 0;
    }
    static int measuredPoints(const MonitorScope &s) { return s.timeMeasurement().count; }
    static size_t statCount(const MonitorScope &s, const std::string &name) {
        auto it = s.chStats_.find(name);
        return it == s.chStats_.end() ? 0 : it->second.count;
    }
};

using Inspect = ScopeInteractionTestAccess;
using Samples = std::vector<std::pair<double, float>>;

static Samples samples(const MonitorChannel &ch) {
    // The CSV exporter snapshots these same raw accessors, independently of
    // visibility, Y range and display decimation
    Samples out;
    for (size_t i = 0; i < ch.read_.rawSize(); ++i)
        out.emplace_back(ch.read_.rawTs(i), ch.read_.rawVal(i));
    return out;
}

static Samples withoutRange(const Samples &before, double first, double last) {
    Samples out;
    for (const auto &sample : before)
        if (sample.first < first || sample.first > last) out.push_back(sample);
    return out;
}

struct ScopeUi {
    static constexpr const char *host = "Discard interval regression";
    Monitor &monitor;
    MonitorScope &scope;
    double xmin{-1}, xmax{11};
    MonitorViewMode mode{MonitorViewMode::MANUAL};
    ImPlotPlot *plot{};

    void frame(ImVec2 mouse = ImVec2(-1000, -1000), bool down = false, float wheel = 0) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        auto &io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        if (wheel) io.AddMouseWheelEvent(0, wheel);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 800));
        ImGui::Begin(host, nullptr, ImGuiWindowFlags_NoSavedSettings);
        Inspect::monitorToolbar(monitor);
        scope.menu(true);
        scope.draw(&xmin, &xmax, 5000, mode);
        ImGui::End();
        ImGui::Render();
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        auto &pool = ImPlot::GetCurrentContext()->Plots;
        check(pool.GetBufSize() == 1, "fixture contains exactly one real plot");
        plot = pool.GetByIndex(0);
    }

    ImRect locate(const char *label) {
        auto *window = ImGui::FindWindowByName(host);
        check(window && window->WasActive, "toolbar host is active");
        ImGui::SetFocusID(window->GetID(label), window);
        window->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
        frame();
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        if (rect.Min.x < 0 || rect.Min.y < 0)
            fprintf(stderr, "Missing toolbar control: %s\n", label);
        check(rect.Min.x >= 0 && rect.Min.y >= 0, "toolbar control is submitted");
        return rect;
    }

    void click(const char *label) {
        const auto point = locate(label).GetCenter();
        frame(point); frame(point, true); frame(point); frame();
    }

    ImVec2 point(double time, float heightFraction = .5f) const {
        return ImVec2(plot->Axes[ImAxis_X1].PlotToPixels(time),
                      plot->PlotRect.Min.y + plot->PlotRect.GetHeight() * heightFraction);
    }

    void drag(double first, double last) {
        const auto start = point(first, .45f), end = point(last, .50f);
        // Two hover frames also exercise ImPlot's AllowOverlap activation
        frame(start); frame(start); frame(start, true);
        frame(end, true); frame(end); frame();
    }

    void select(double first, double last) {
        if (!Inspect::selecting(scope)) click("###discardSelect");
        check(Inspect::selecting(scope), "toolbar enters interval selection mode");
        drag(first, last);
        check(Inspect::selected(scope), "real left drag creates a selected interval");
    }

    void screenshot(const std::string &path) {
        int width, height;
        glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
        std::vector<unsigned char> pixels(width * height * 3);
        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream image(path, std::ios::binary);
        image << "P6\n" << width << ' ' << height << "\n255\n";
        for (int y = height - 1; y >= 0; --y)
            image.write(reinterpret_cast<char *>(pixels.data() + y * width * 3), width * 3);
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    int result = 0;
    std::unique_ptr<Gui> gui;
    std::thread sampler;
    try {
        std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
        check(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(argv[1]);
        g_lang = Lang::ZH;
        auto &io = ImGui::GetIO();
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        io.ConfigInputTrickleEventQueue = false;
        io.IniFilename = nullptr;
        io.MouseDoubleClickTime = .001f;

        auto monitor = std::make_shared<Monitor>("Discard monitor");
        monitor->setSamplingPaused(true);
        monitor->addScope("Selected scope");
        monitor->addScope("Other scope");
        auto scope = monitor->getScopes().at("Selected scope");
        auto other = monitor->getScopes().at("Other scope");
        scope->addChannel("visible"); scope->addChannel("hidden");
        other->addChannel("untouched");
        auto *visible = scope->findChannel("visible");
        auto *hidden = scope->findChannel("hidden");
        auto *untouched = other->findChannel("untouched");
        for (auto *ch : {visible, hidden, untouched}) ch->historySeconds_ = 0;
        for (int i = 0; i < 10; ++i) {
            visible->setRVal(float(i * i), i);
            hidden->setRVal(float(1000 + i), i);
            untouched->setRVal(float(-i), i);
        }
        for (auto *ch : {visible, hidden, untouched}) ch->publishSnapshot();
        hidden->setShow(false);
        scope->getShowSidePanel() = true;
        const auto originalVisible = samples(*visible), originalHidden = samples(*hidden);
        const auto originalOther = samples(*untouched);
        ScopeUi ui{*monitor, *scope};
        ui.frame(); ui.frame();
        Inspect::measure(*scope);
        ui.click("###discardSelect");
        const auto beforeX = ui.plot->Axes[ImAxis_X1].Range;
        const auto beforeY = ui.plot->Axes[ImAxis_Y1].Range;
        ui.drag(6.25, 2.25);
        check(Inspect::selected(*scope) && Inspect::selectedPoints(*scope) == 8,
              "reverse time selection includes all selected-scope channels, including hidden values outside the box Y range");
        check(!scope->hasPendingDiscardTimeRange() && samples(*visible) == originalVisible &&
              samples(*hidden) == originalHidden, "selection alone does not modify writer or published data");
        check(Inspect::measuredPoints(*scope) == 0, "discard drag does not place time measurement cursors");
        check(ui.mode == MonitorViewMode::MANUAL && ui.xmin == beforeX.Min && ui.xmax == beforeX.Max &&
              ui.plot->Axes[ImAxis_Y1].Range.Min == beforeY.Min &&
              ui.plot->Axes[ImAxis_Y1].Range.Max == beforeY.Max,
              "discard drag does not pan, zoom or change view mode");
        ui.frame(ui.point(4), false, 2);
        check(ui.xmin == beforeX.Min && ui.xmax == beforeX.Max &&
              Inspect::selectedPoints(*scope) == 8 && ui.mode == MonitorViewMode::MANUAL,
              "selection-mode wheel zoom leaves shared time links, view mode and selected timestamps unchanged");
        ui.frame(); ui.screenshot(std::string(argv[1]) + ".selection.ppm");
        ui.click("###discardCancel");
        check(!scope->hasPendingDiscardTimeRange() && !Inspect::selecting(*scope) &&
              samples(*visible) == originalVisible && samples(*hidden) == originalHidden,
              "cancel leaves every selected-scope sample intact");

        ui.select(6.25, 2.25);
        ui.click("###discardApply");
        check(scope->hasPendingDiscardTimeRange() && samples(*visible) == originalVisible &&
              samples(*hidden) == originalHidden, "confirmation only queues a sampler-safe mutation");
        scope->processDiscardTimeRangeRequest();
        const auto retainedVisible = withoutRange(originalVisible, 2.25, 6.25);
        const auto retainedHidden = withoutRange(originalHidden, 2.25, 6.25);
        check(!scope->hasPendingDiscardTimeRange() && samples(*visible) == retainedVisible &&
              samples(*hidden) == retainedHidden, "safe point removes the interval from all scope curves and preserves surviving timestamps");
        check(samples(*untouched) == originalOther, "another scope in the same monitor is unchanged");
        ui.frame(); ui.frame();
        check(Inspect::statCount(*scope, "visible") == 6 && Inspect::statCount(*scope, "hidden") == 6 &&
              std::abs(scope->getChannelMean("visible") - 199.0 / 6) < 1e-9 &&
              std::abs(scope->getChannelMean("hidden") - 1000.0 - 27.0 / 6) < 1e-9,
              "Stats and CSV raw data sources reflect the removed interval, including hidden curves");

        visible->setRVal(100, 10); hidden->setRVal(1010, 10);
        for (int i = 0; i < 3; ++i) { visible->publishSnapshot(); hidden->publishSnapshot(); }
        auto appendedVisible = retainedVisible, appendedHidden = retainedHidden;
        appendedVisible.emplace_back(10, 100); appendedHidden.emplace_back(10, 1010);
        check(samples(*visible) == appendedVisible && samples(*hidden) == appendedHidden,
              "subsequent append and repeated publication cannot resurrect discarded points");

        ui.select(3.25, 5.75);
        check(Inspect::selectedPoints(*scope) == 0, "empty gap reports no data to discard");
        ui.click("###discardApply");
        check(!scope->hasPendingDiscardTimeRange() && samples(*visible) == appendedVisible &&
              samples(*hidden) == appendedHidden, "confirm is disabled for an interval with no samples");
        ui.click("###discardCancel");
        ui.click("###discardSelect");
        auto point = ui.point(2);
        ui.frame(point); ui.frame(point, true); ui.frame(point); ui.frame();
        check(!Inspect::selected(*scope) && !scope->hasPendingDiscardTimeRange(),
              "a click without a horizontal drag cannot discard a time instant");
        ui.click("###discardSelect");

        ui.click("###discardSelect");
        scope->getShowFft() = true;
        ui.frame(); ui.frame();
        check(!Inspect::selecting(*scope) && !scope->hasPendingDiscardTimeRange(),
              "switching to FFT cancels selection rather than treating frequency as time");
        ui.drag(.2, .8);
        check(!scope->hasPendingDiscardTimeRange() && samples(*visible) == appendedVisible,
              "FFT mouse dragging cannot discard time samples");
        scope->getShowFft() = false;
        Inspect::cancel(*monitor);
        ui.xmin = -1; ui.xmax = 11; ui.mode = MonitorViewMode::MANUAL;
        ui.frame(); ui.frame();
        ui.click("###discardSelect");
        auto flash = std::make_shared<flasharray::Snapshot>();
        flash->step = 1; flash->values = {10, 20, 30};
        hidden->arraySnapshot_ = flash;
        ui.frame(); ui.frame();
        check(!Inspect::selecting(*scope) && !scope->hasPendingDiscardTimeRange(),
              "a Flash-backed channel cancels the entire scope selection, even when hidden");
        ui.drag(6.25, 2.25);
        check(hidden->arraySnapshot_ == flash && samples(*visible) == appendedVisible &&
              samples(*hidden) == appendedHidden && !scope->hasPendingDiscardTimeRange(),
              "Flash-backed scope cannot be partially or accidentally edited");
        hidden->arraySnapshot_.reset();
        ui.xmin = -1; ui.xmax = 11; ui.mode = MonitorViewMode::MANUAL;
        ui.frame(); ui.frame();

        // Hand a real UI request to the actual sampler while both monitor and
        // scope acquisition, and global display publication, are paused
        scope->setPaused(true);
        ui.select(.5, 1.5);
        ui.click("###discardApply");
        check(scope->hasPendingDiscardTimeRange(), "paused monitor accepts the discard request");
        {
            std::lock_guard lock(gui->getMonitorMtx());
            gui->getMonitors().emplace(monitor->getName(), monitor);
        }
        g_appRunning.store(true); g_monitorPaused.store(true); g_samplerRunMode.store(0);
        sampler = std::thread(threadFunc, gui.get());
        bool processed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(gui->getMonitorMtx());
                processed = !scope->hasPendingDiscardTimeRange() && visible->read_.rawSize() == 6;
                if (processed) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        g_appRunning.store(false); sampler.join(); g_monitorPaused.store(false);
        check(processed && samples(*visible) == withoutRange(appendedVisible, .5, 1.5) &&
              samples(*hidden) == withoutRange(appendedHidden, .5, 1.5),
              "real sampler consumes discard before monitor/scope/global pause checks and republishes it");
        check(samples(*untouched) == originalOther, "paused sampler discard remains isolated to the selected scope");
        // A source-scope gap must still allow discarding samples in sibling scopes
        monitor->addScope("Hidden scope");
        monitor->addScope("Array scope");
        auto hiddenScope = monitor->getScopes().at("Hidden scope");
        auto arrayScope = monitor->getScopes().at("Array scope");
        hiddenScope->addChannel("hidden samples");
        arrayScope->addChannel("array samples");
        auto *hiddenSamples = hiddenScope->findChannel("hidden samples");
        auto *arraySamples = arrayScope->findChannel("array samples");
        hiddenSamples->historySeconds_ = arraySamples->historySeconds_ = 0;
        for (int i = 0; i < 10; ++i) {
            hiddenSamples->setRVal(float(200 + i), i);
            arraySamples->setRVal(float(300 + i), i);
        }
        hiddenSamples->publishSnapshot(); arraySamples->publishSnapshot();
        hiddenSamples->setShow(false);
        hiddenScope->setHidden(true);
        other->getShowFft() = true;
        arraySamples->arraySnapshot_ = flash;
        const auto beforeHiddenScope = samples(*hiddenSamples), beforeArrayScope = samples(*arraySamples);
        const auto beforeSource = samples(*visible);
        auto independent = std::make_shared<Monitor>("Independent monitor");
        independent->addScope("Independent scope");
        auto independentScope = independent->getScopes().at("Independent scope");
        independentScope->addChannel("independent samples");
        auto *independentSamples = independentScope->findChannel("independent samples");
        independentSamples->historySeconds_ = 0;
        for (int i = 0; i < 10; ++i) independentSamples->setRVal(float(i), i);
        independentSamples->publishSnapshot();
        const auto beforeIndependent = samples(*independentSamples);

        Inspect::sync(*monitor, true);
        ui.select(5.75, 3.25);
        check(Inspect::selectedPoints(*scope) == 4,
              "sync preview totals the selected interval across FFT and hidden sibling scopes even when the source has a gap");
        ui.click("###discardApply");
        check(scope->hasPendingDiscardTimeRange() && other->hasPendingDiscardTimeRange() &&
              hiddenScope->hasPendingDiscardTimeRange() && !arrayScope->hasPendingDiscardTimeRange(),
              "sync confirmation queues the same interval for all temporal scopes, excluding Flash array scopes");
        check(samples(*untouched) == originalOther && samples(*hiddenSamples) == beforeHiddenScope,
              "sync confirmation defers all data changes to the sampler safe point");
        Inspect::sync(*monitor, false);
        monitor->processDiscardRequests();
        check(samples(*visible) == beforeSource && samples(*untouched) == withoutRange(originalOther, 3.25, 5.75) &&
              samples(*hiddenSamples) == withoutRange(beforeHiddenScope, 3.25, 5.75),
              "queued sync discard retains its captured scope set after the sync toggle changes");
        check(samples(*arraySamples) == beforeArrayScope && arraySamples->arraySnapshot_ == flash &&
              samples(*independentSamples) == beforeIndependent,
              "sync discard leaves Flash arrays and other monitors unchanged");
        untouched->publishSnapshot(); hiddenSamples->publishSnapshot();
        check(samples(*untouched) == withoutRange(originalOther, 3.25, 5.75) &&
              samples(*hiddenSamples) == withoutRange(beforeHiddenScope, 3.25, 5.75),
              "republishing synchronized scopes cannot resurrect discarded samples");
        puts("PASS: real reverse drag/confirm/cancel, hidden channels, deferred mutation, axes, Stats/CSV source, append, empty gap, FFT/Flash, paused sampler and synchronized monitor discard");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        result = 1;
    }
    g_appRunning.store(false);
    if (sampler.joinable()) sampler.join();
    g_monitorPaused.store(false);
    gui.reset();
    JLinkPort::instance().close();
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
