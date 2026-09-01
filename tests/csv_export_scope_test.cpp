// Exercise production CSV snapshots/files and the real range-selection menu
#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "platform/native_dlg.hpp"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

struct CsvExportTestAccess {
    using Kind = Monitor::CsvExportKind;
    using Target = Monitor::CsvExportTarget;
    static auto menu(Monitor &m) { return m.drawCsvExportMenu(); }
    static bool save(Monitor &m, const std::string &path, Target target) { return m.exportCsvFile(path, target); }
    static void wait(Monitor &m) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (m.csvExport_->running.load(std::memory_order_acquire)) {
            check(std::chrono::steady_clock::now() < deadline, "CSV export completes");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
using Inspect = CsvExportTestAccess;

static std::string readFile(const std::string &path) {
    FILE *f = nativeFopen(path, "rb");
    check(f != nullptr, "exported CSV exists");
    std::string text;
    char buf[4096];
    while (auto n = fread(buf, 1, sizeof(buf), f)) text.append(buf, n);
    fclose(f);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

struct ExportUi {
    Monitor &monitor;
    std::optional<Inspect::Target> chosen;
    void frame(ImVec2 mouse = ImVec2(-1000, -1000), bool down = false) {
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
        auto &io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 700));
        ImGui::Begin("CSV range test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (auto target = Inspect::menu(monitor)) chosen = *target;
        ImGui::End(); ImGui::Render();
    }
    ImGuiWindow *popup() {
        auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
        check(!stack.empty() && stack.back().Window, "export range popup is open");
        return stack.back().Window;
    }
    ImVec2 locate(ImGuiWindow *w, ImGuiID id) {
        ImGui::SetFocusID(id, w);
        w->NavRectRel[0] = ImRect(-1000, -1000, -999, -999);
        frame();
        auto r = ImGui::WindowRectRelToAbs(w, w->NavRectRel[0]);
        check(r.Min.x >= 0 && r.Min.y >= 0, "CSV menu item is submitted");
        return r.GetCenter();
    }
    void click(ImVec2 p) { frame(p); frame(p, true); frame(p); frame(); }
    void open() {
        chosen.reset();
        auto *w = ImGui::FindWindowByName("CSV range test");
        click(locate(w, w->GetID(tr("Export CSV", "导出 CSV"))));
    }
    void scopeMenu() {
        open();
        auto *w = popup();
        click(locate(w, w->GetID("A###csvScope_A")));
        check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 2, "scope submenu opens");
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    int result = 0;
    std::unique_ptr<Gui> gui;
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
        Monitor monitor("CSV monitor");
        monitor.addScope("A"); monitor.addScope("B");
        auto a = monitor.getScopes().at("A"), b = monitor.getScopes().at("B");
        a->setOrder(0); b->setOrder(1);
        a->addChannel("same"); a->addChannel("hidden"); a->addChannel("empty");
        b->addChannel("same");
        auto *same = a->findChannel("same"), *hidden = a->findChannel("hidden");
        auto *empty = a->findChannel("empty"), *other = b->findChannel("same");
        same->setOrder(0); hidden->setOrder(1); empty->setOrder(2);
        same->setRVal(1.25f, 0); same->setRVal(-2.5f, 1);
        hidden->setRVal(99, 0); other->setRVal(77, 0);
        for (auto *c : {same, hidden, empty, other}) c->publishSnapshot();
        hidden->setShow(false);
        const std::string path = std::string(argv[1]) + ".csv";
        auto save = [&](Inspect::Target target) {
            check(Inspect::save(monitor, path, target), "requested export starts");
            Inspect::wait(monitor);
            return readFile(path);
        };
        const auto all = save({});
        check(all == "A::same::Time,A::same::Value,A::hidden::Time,A::hidden::Value,A::empty::Time,A::empty::Value,B::same::Time,B::same::Value\n0,1.25,0,99,,,0,77\n1,-2.5,,,,,,\n",
              "all-scope CSV retains deterministic order, hidden channels and sparse rows");
        const auto scope = save({Inspect::Kind::Scope, "A", {}});
        check(scope == "A::same::Time,A::same::Value,A::hidden::Time,A::hidden::Value,A::empty::Time,A::empty::Value\n0,1.25,0,99,,\n1,-2.5,,,,\n",
              "scope CSV contains exactly its channels, including hidden and empty");
        check(save({Inspect::Kind::Channel, "B", "same"}) == "B::same::Time,B::same::Value\n0,77\n",
              "single-channel CSV does not leak another scope's same-named channel");
        check(save({Inspect::Kind::Channel, "A", "empty"}) == "A::empty::Time,A::empty::Value\n",
              "empty channel exports a valid header");
        check(!Inspect::save(monitor, path, {Inspect::Kind::Scope, "missing", {}}) &&
              !Inspect::save(monitor, path, {Inspect::Kind::Channel, "A", "missing"}),
              "stale targets never fall back to exporting all data");
        check(readFile(path) == "A::empty::Time,A::empty::Value\n", "invalid targets leave existing files intact");
        auto flash = std::make_shared<flasharray::Snapshot>();
        flash->step = .25; flash->values = {3.125, 4.5};
        hidden->arraySnapshot_ = flash;
        check(save({Inspect::Kind::Channel, "A", "hidden"}) == "A::hidden::Time,A::hidden::Value\n0,3.125\n0.25,4.5\n",
              "selected Flash channel exports its snapshot and sample interval");
        check(Inspect::save(monitor, path, {Inspect::Kind::Channel, "A", "same"}), "snapshot export starts");
        same->clearData();
        Inspect::wait(monitor);
        check(readFile(path) == "A::same::Time,A::same::Value\n0,1.25\n1,-2.5\n",
              "background export is isolated from subsequent live data changes");
        ExportUi ui{monitor}; ui.frame(); ui.frame();
        ui.open(); auto *w = ui.popup();
        ui.click(ui.locate(w, w->GetID("###csvExportAll")));
        check(ui.chosen && ui.chosen->kind == Inspect::Kind::Monitor, "menu selects whole monitor");
        ui.scopeMenu(); w = ui.popup();
        const auto scopeSeed = w->GetID("A");
        ui.click(ui.locate(w, ImHashStr("###csvExportScope", 0, scopeSeed)));
        check(ui.chosen && ui.chosen->kind == Inspect::Kind::Scope && ui.chosen->scope == "A",
              "submenu selects one scope");
        ui.scopeMenu(); w = ui.popup();
        const auto channelSeed = ImHashStr("hidden", 0, w->GetID("A"));
        ui.click(ui.locate(w, ImHashStr("###csvExportChannel", 0, channelSeed)));
        check(ui.chosen && ui.chosen->kind == Inspect::Kind::Channel && ui.chosen->scope == "A" && ui.chosen->channel == "hidden",
              "submenu selects exactly one channel");
        hidden->markPendingDelete();
        check(!Inspect::save(monitor, path, {Inspect::Kind::Channel, "A", "hidden"}), "pending deletion channels cannot export");
        b->markPendingDelete();
        check(save({}).find("B::") == std::string::npos, "pending deletion scopes are omitted");
        puts("PASS: Monitor/Scope/channel CSV files, sparse/empty/hidden/Flash channels, snapshot isolation, stale targets and real selection menu");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what()); result = 1;
    }
    gui.reset(); JLinkPort::instance().close();
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
