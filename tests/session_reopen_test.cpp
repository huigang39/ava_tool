#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include <fstream>
#include <stdexcept>

struct SimSamplerTestAccess {
    static const std::string &path(const Gui &gui) { return gui.currentSessionPath_; }
    static bool loaded(const Gui &gui) { return !gui.isFirstSave_; }
    static bool startupPickerPending(const Gui &gui) { return gui.startupSessionPickerPending_; }
    static const std::vector<std::string> &recentSessions(const Gui &gui) { return gui.recentSessions_; }
    static bool chooseStartup(Gui &gui, const std::string &path) { return gui.chooseStartupSession(path); }
    static void applySessionChange(Gui &gui) { gui.applySessionChange(); }
    static bool renderStartupPicker(Gui &gui) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        gui.drawStartupSessionPicker();
        bool found = false;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::strstr(window->Name, "###StartupSessionPicker")) {
                found = true;
                break;
            }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        gui.updatePlatformWindows();
        return found;
    }
    static void load(Gui &gui, const std::string &path) { gui.loadSession(path); }
    static void save(Gui &gui) { gui.saveSession(); }
    static void discardOnExit(Gui &gui) { gui.skipAutoSave_ = true; }
};

static void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
static std::unique_ptr<Gui> start(const std::string &path = "") {
    check(glfwInit() == GLFW_TRUE, "GLFW initializes");
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    return std::make_unique<Gui>(path);
}
static void session(const std::filesystem::path &path) {
    std::ofstream(path) << "{\"monitors\":[],\"variables\":[]}";
}
static bool isSession(const Gui &gui, const std::filesystem::path &path) {
    std::error_code ec;
    return SimSamplerTestAccess::loaded(gui) &&
        std::filesystem::equivalent(std::filesystem::u8path(SimSamplerTestAccess::path(gui)), path, ec) && !ec;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    const auto originalDirectory = std::filesystem::current_path();
    const auto root = std::filesystem::absolute(std::filesystem::u8path(std::string(argv[1]) + ".startup"));
    std::filesystem::create_directories(root);
    std::filesystem::current_path(root);
    int result = 0;
    try {
        const auto config = root / ".ava_tool";
        std::filesystem::create_directories(config);
        const auto first = root / std::filesystem::u8path("上次关闭的会话.ava");
        const auto second = root / "explicit.ava";
        const auto fallback = root / "session.ava";
        session(first); session(second); session(fallback);
        std::ofstream(config / "last_session.txt") << utf8(first) << '\n';
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "startup waits for the session chooser instead of auto-loading");
            check(!SimSamplerTestAccess::recentSessions(*gui).empty() &&
                      SimSamplerTestAccess::recentSessions(*gui).front() == utf8(first),
                  "legacy last_session.txt seeds the startup chooser");
            check(SimSamplerTestAccess::renderStartupPicker(*gui), "startup session chooser popup renders");
            check(SimSamplerTestAccess::renderStartupPicker(*gui), "startup session chooser remains open until a choice");
            check(SimSamplerTestAccess::chooseStartup(*gui, utf8(first)), "the startup chooser accepts an existing session");
            SimSamplerTestAccess::applySessionChange(*gui);
            check(isSession(*gui, first), "the selected recent session is loaded");
            SimSamplerTestAccess::discardOnExit(*gui);
        }
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "a remembered session is shown in the chooser but not opened automatically");
            check(SimSamplerTestAccess::chooseStartup(*gui, utf8(second)), "another recent session can be selected");
            SimSamplerTestAccess::applySessionChange(*gui);
            check(isSession(*gui, second), "choosing another recent session loads it");
        }
        {
            auto gui = start(utf8(first));
            check(isSession(*gui, first) && !SimSamplerTestAccess::startupPickerPending(*gui),
                  "an explicit file opens directly without the chooser");
            SimSamplerTestAccess::save(*gui);
            check(isSession(*gui, first), "saving preserves the Unicode session path");
        }
        std::filesystem::remove(first);
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "a missing remembered session does not fall back automatically");
            check(SimSamplerTestAccess::chooseStartup(*gui, utf8(second)), "an available session can still be selected");
            SimSamplerTestAccess::applySessionChange(*gui);
            check(isSession(*gui, second), "selection remains available after an old file disappears");
        }
        std::filesystem::remove(config / "last_session.txt");
        std::ofstream(config / "recent.txt") << utf8(second) << '\n';
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "recent.txt is offered without being opened automatically");
            check(SimSamplerTestAccess::chooseStartup(*gui, utf8(second)), "the recent session remains selectable");
            SimSamplerTestAccess::applySessionChange(*gui);
            check(isSession(*gui, second), "the recent selection loads after confirmation");
        }
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "an existing session.ava is not loaded automatically");
        }
        std::filesystem::remove(config / "last_session.txt");
        std::ofstream(config / "recent.txt", std::ios::trunc);
        std::filesystem::remove(fallback);
        {
            auto gui = start();
            check(SimSamplerTestAccess::startupPickerPending(*gui) && !SimSamplerTestAccess::loaded(*gui),
                  "fresh install also shows the chooser and starts empty");
        }
        check(!std::filesystem::exists(fallback), "empty startup and close do not create a session file");
        puts("PASS: startup chooser, selected recent sessions, explicit file override, missing file and cold startup");
    } catch (const std::exception &error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    std::filesystem::current_path(originalDirectory);
    return result;
}
