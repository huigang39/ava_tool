#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "core/http_win.hpp"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct SimSamplerTestAccess {
    static GLFWwindow *window(Gui &gui) { return gui.window_; }
    static bool hidden(Gui &gui) { return gui.inTray_; }
    static bool exiting(Gui &gui) { return gui.exitRequested_; }
    static void process(Gui &gui) { gui.processWindowActions(); }
    static void updatePlatforms(Gui &gui) { gui.updatePlatformWindows(); }
    static void clearExit(Gui &gui) { gui.exitRequested_ = false; glfwSetWindowShouldClose(gui.window_, GLFW_FALSE); }
    static void ask(Gui &gui) { gui.closeBehavior_ = Gui::CloseBehavior::Ask; }
    static void exitDefault(Gui &gui) { gui.closeBehavior_ = Gui::CloseBehavior::Exit; }
    static void setTrayDefault(Gui &gui) { gui.closeBehavior_ = Gui::CloseBehavior::Tray; }
    static bool choicePending(Gui &gui) { return gui.showCloseChoice_; }
    static bool trayDefault(Gui &gui) { return gui.closeBehavior_ == Gui::CloseBehavior::Tray; }
    static void remember(Gui &gui) { gui.rememberCloseChoice_ = true; }
    static void drawChoice(Gui &gui) { gui.drawCloseChoicePopup(); }
    static LocalFileServer &server(Gui &gui) { return gui.localFileServer_; }
};
static HWND trayWindow() {
    HWND result{};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        wchar_t name[128]{}; GetClassNameW(window, name, 128);
        if (std::wstring(name) == L"AvA.Tool.SystemTray") {
            *reinterpret_cast<HWND *>(data) = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    int result = 0;
    try {
        const auto fixture = std::filesystem::path(argv[1]).parent_path() /
            ("tray-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(fixture / "share");
        std::ofstream(fixture / "session.ava") << "{\"monitors\":[],\"variables\":[]}";
        std::ofstream(fixture / "share/alive.txt") << "server stays alive";
        check(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        auto gui = std::make_unique<Gui>((fixture / "session.ava").string());
        auto *window = SimSamplerTestAccess::window(*gui);
        auto &server = SimSamplerTestAccess::server(*gui);
        server.stop(); server.config.folder = (fixture / "share").string(); server.config.port = 0;
        server.config.name = "tray-test"; server.config.firmwareFolder.clear(); server.config.sessionsFolder.clear(); server.config.updatesFolder.clear();
        server.config.autoStart = true;
        SimSamplerTestAccess::ask(*gui);
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::choicePending(*gui) && !SimSamplerTestAccess::hidden(*gui) &&
              SimSamplerTestAccess::exiting(*gui) && glfwWindowShouldClose(window),
              "stopped server closes directly even when auto-start is configured");
        SimSamplerTestAccess::clearExit(*gui);
        check(server.start(), server.error().c_str());
        SimSamplerTestAccess::ask(*gui);
        auto choiceFrame = [&](ImVec2 position = ImVec2(-1000, -1000), bool down = false) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            ImGui::GetIO().AddMousePosEvent(position.x, position.y);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame(); ImGui::Begin("Close choice test host");
            SimSamplerTestAccess::drawChoice(*gui); ImGui::End(); ImGui::Render();
            SimSamplerTestAccess::updatePlatforms(*gui);
        };
        auto chooseButton = [&](int index) {
            for (int i = 0; i < 3; ++i) choiceFrame();
            auto *popup = ImGui::FindWindowByName(tr("Close AvA Tool###CloseAction", "关闭 AvA Tool###CloseAction"));
            check(popup && popup->Active, "Windows close action choice modal appears");
            ImVec2 position(popup->Pos.x + ImGui::GetStyle().WindowPadding.x + index * (120 + ImGui::GetStyle().ItemSpacing.x) + 60,
                            popup->Pos.y + popup->Size.y - ImGui::GetStyle().WindowPadding.y -
                            (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2) * 0.5f);
            choiceFrame(position); choiceFrame(position, true); choiceFrame(position);
        };
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(SimSamplerTestAccess::choicePending(*gui) && !SimSamplerTestAccess::hidden(*gui) && !glfwWindowShouldClose(window),
              "Windows close asks before hiding or exiting");
        chooseButton(2);
        check(!SimSamplerTestAccess::choicePending(*gui) && !SimSamplerTestAccess::exiting(*gui), "cancel keeps application running");
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        SimSamplerTestAccess::remember(*gui); chooseButton(1);
        check(SimSamplerTestAccess::hidden(*gui) && SimSamplerTestAccess::trayDefault(*gui), "tray choice can be remembered as default");
        std::ifstream preferences(Gui::getAppDir() + "/window_preferences.json");
        std::string preferenceText((std::istreambuf_iterator<char>(preferences)), {});
        check(preferenceText.find("\"closeBehavior\":\"tray\"") != std::string::npos, "chosen close default is persisted");
        SendMessageW(trayWindow(), WM_COMMAND, 1, 0); SimSamplerTestAccess::process(*gui);
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::choicePending(*gui), "remembered tray default skips the choice modal");
        check(SimSamplerTestAccess::hidden(*gui) && !glfwWindowShouldClose(window) && !glfwGetWindowAttrib(window, GLFW_VISIBLE),
              "window close moves to tray without requesting process exit");
        const HWND tray = trayWindow(); check(tray != nullptr, "native notification icon owner exists");
        ImGui::GetIO().ConfigViewportsNoAutoMerge = true;
        auto backgroundFrame = [&](bool dock) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
            if (dock) {
                auto *viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x + 100, viewport->Pos.y + 100), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(240, 120), ImGuiCond_Always);
                ImGui::Begin("Tray background dock", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextUnformatted("Background viewport lifecycle regression"); ImGui::End();
            }
            ImGui::Render();
            const auto show = ImGui::GetPlatformIO().Platform_ShowWindow;
            SimSamplerTestAccess::updatePlatforms(*gui);
            check(ImGui::GetCurrentContext()->FrameCountPlatformEnded == ImGui::GetFrameCount(),
                  "hidden frames complete platform update before next NewFrame (reported assertion regression)");
            check(ImGui::GetPlatformIO().Platform_ShowWindow == show, "temporary show suppression restores backend callback");
            check(glfwGetCurrentContext() == window, "platform update restores main OpenGL context");
            for (auto *viewport : ImGui::GetPlatformIO().Viewports)
                if (viewport->PlatformHandle)
                    check(!glfwGetWindowAttrib(static_cast<GLFWwindow *>(viewport->PlatformHandle), GLFW_VISIBLE),
                          "all native viewports remain hidden while in tray");
        };
        for (int i = 0; i < 6; ++i) backgroundFrame(true);
        check(ImGui::GetPlatformIO().Viewports.Size > 1, "detached native viewport is exercised during background frames");
        for (int i = 0; i < 6; ++i) backgroundFrame(false);
        puts("PASS: repeated hidden frames, required platform update, hidden detached viewport creation/destruction and callback/context restoration");
        std::string body, error;
        check(http::get(server.directoryUrl("127.0.0.1") + "alive.txt", body, &error) && body == "server stays alive",
              "file sharing still serves HTTP requests while window is in tray");
        auto discovered = LocalFileServer::discover(500);
        check(std::any_of(discovered.begin(), discovered.end(), [&](const auto &item) { return item.port == server.port() && item.name == "tray-test"; }),
              "LAN discovery remains active while window is in tray");
        SendMessageW(tray, WM_COMMAND, 1, 0); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::hidden(*gui) && glfwGetWindowAttrib(window, GLFW_VISIBLE) && !glfwWindowShouldClose(window),
              "native tray Open command restores window without exit");
        glfwIconifyWindow(window); glfwPollEvents(); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::hidden(*gui), "ordinary minimize does not move to tray");
        glfwRestoreWindow(window);
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(SimSamplerTestAccess::hidden(*gui), "remembered close behavior moves to tray");
        SendMessageW(tray, WM_COMMAND, 2, 0); SimSamplerTestAccess::process(*gui);
        check(SimSamplerTestAccess::exiting(*gui) && !SimSamplerTestAccess::hidden(*gui) && glfwWindowShouldClose(window) &&
              glfwGetWindowAttrib(window, GLFW_VISIBLE), "native tray Exit requests real exit with a visible save prompt host");
        SimSamplerTestAccess::clearExit(*gui);
        SimSamplerTestAccess::exitDefault(*gui);
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::hidden(*gui) && glfwWindowShouldClose(window), "exit default retains ordinary close behavior");
        SimSamplerTestAccess::clearExit(*gui);
        server.stop();
        SimSamplerTestAccess::setTrayDefault(*gui);
        glfwSetWindowShouldClose(window, GLFW_TRUE); SimSamplerTestAccess::process(*gui);
        check(!SimSamplerTestAccess::choicePending(*gui) && !SimSamplerTestAccess::hidden(*gui) &&
              SimSamplerTestAccess::exiting(*gui) && glfwWindowShouldClose(window) && SimSamplerTestAccess::trayDefault(*gui),
              "stopped server closes directly without changing remembered tray preference");
        gui.reset();
        check(trayWindow() == nullptr, "tray owner and notification icon are removed at shutdown");
        puts("PASS: close choice only with running server, stopped server exits with Ask/Tray preferences, close/minimize to tray, HTTP/discovery alive, native restore/exit and shutdown cleanup");
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
    JLinkPort::instance().close(); log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
