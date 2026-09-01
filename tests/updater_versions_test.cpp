#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "version.hpp"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct SimSamplerTestAccess {
    static Updater &updater(Gui &gui) { return gui.updater_; }
    static void beginCheck(Gui &gui, bool manual = true) {
        gui.updateManualCheck_ = manual; gui.updatePendingResult_ = true; gui.updater_.checkAsync();
    }
    static void draw(Gui &gui) { gui.drawUpdateUI(); }
    static int selection(Gui &gui) { return gui.updateSelectedVersion_; }
    static void dismissDownload(Gui &gui) { gui.showDownloadDonePopup_ = true; }
};
static void waitFor(auto done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(done(), "background operation completed");
}
struct IsolatedTemp {
    std::string previous = std::getenv("TMP") ? std::getenv("TMP") : "";
    ~IsolatedTemp() { _putenv_s("TMP", previous.c_str()); }
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    std::unique_ptr<Gui> gui;
    int result = 0;
    try {
        const auto root = std::filesystem::path(argv[1]).parent_path() /
            ("version-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        for (const char *folder : {"all", "older", "current", "temp"}) std::filesystem::create_directories(root / folder);
        std::ofstream(root / "session.ava") << "{\"monitors\":[],\"variables\":[]}";
        for (const char *version : {"999.0.0", "0.10.0", "0.2.0", AVA_VERSION, "0.0.1"})
            std::ofstream(root / "all" / (std::string("ava_tool_setup_") + version + ".exe")) << "MZ-version-" << version;
        std::ofstream(root / "older/ava_tool_setup_0.0.1.exe") << "MZ-version-0.0.1";
        std::ofstream(root / "current" / (std::string("ava_tool_setup_") + AVA_VERSION + ".exe")) << "MZ-current";
        LocalFileServer server; server.config.folder = root.string(); server.config.port = 0;
        check(server.start(), "start version fixture server");
        check(glfwInit() == GLFW_TRUE, "GLFW initialization"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>((root / "session.ava").string());
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        IsolatedTemp isolatedTemp; _putenv_s("TMP", (root / "temp").string().c_str());
        auto frame = [&] {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
            ImGui::Begin("Update test host"); SimSamplerTestAccess::draw(*gui); ImGui::End(); ImGui::Render();
        };
        auto dialog = [&] { return ImGui::FindWindowByName(tr("Install Application Version###UpdateAvailable", "安装应用版本###UpdateAvailable")); };
        auto beginCheck = [&](const char *folder, bool manual = true) {
            remote::settings().toolUrl = server.directoryUrl("127.0.0.1", folder);
            SimSamplerTestAccess::beginCheck(*gui, manual);
            waitFor([&] { return !SimSamplerTestAccess::updater(*gui).isChecking(); });
            frame(); frame(); frame();
        };
        beginCheck("all");
        check(dialog() && dialog()->Active && SimSamplerTestAccess::selection(*gui) == 0, "manual version dialog defaults to newest installer");
        ImGui::ActivateItemByID(dialog()->GetID(tr("Version", "版本"))); frame(); frame();
        ImGuiWindow *combo = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string(window->Name).starts_with("##Combo_")) combo = window;
        check(combo != nullptr, "real version dropdown opens");
        ImGui::ActivateItemByID(combo->GetID("0.0.1")); frame(); frame();
        check(SimSamplerTestAccess::selection(*gui) == 4, "dropdown selects older version");
        ImGui::ActivateItemByID(dialog()->GetID(tr("Download and Install", "下载并安装"))); frame();
        auto &updater = SimSamplerTestAccess::updater(*gui);
        waitFor([&] { return updater.getDownloadState() != Updater::DownloadState::Downloading; });
        check(updater.getDownloadState() == Updater::DownloadState::Done, "selected version download completes");
        std::ifstream downloaded(updater.getDownloadedPath(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(downloaded)), {});
        check(body == "MZ-version-0.0.1", "real download button downloads selected older version, not latest");
        SimSamplerTestAccess::dismissDownload(*gui);
        beginCheck("older");
        check(!updater.get().available && dialog()->Active && SimSamplerTestAccess::selection(*gui) == 0,
              "manual check permits downgrade even when no newer version exists");
        ImGui::ActivateItemByID(dialog()->GetID(tr("Later", "稍后"))); frame(); frame();
        beginCheck("current");
        check(!updater.get().available && dialog()->Active && updater.get().installers.front().version == AVA_VERSION,
              "manual check permits reinstalling the current version");
        ImGui::ActivateItemByID(dialog()->GetID(tr("Later", "稍后"))); frame(); frame();
        beginCheck("older", false);
        check(!dialog()->Active, "automatic check does not prompt to downgrade");
        puts("PASS: real version dropdown, newest default, selected older download, manual downgrade/reinstall and silent automatic check");
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
    gui.reset(); JLinkPort::instance().close(); log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
