// Exercise the actual GLFW backend with injected display enumeration failures
#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include <GLFW/glfw3.h>
#include <limits>
#include <stdexcept>

static int monitorCount = 0;
static int failure = 0;
static GLFWmonitor *fakeMonitors[] = {reinterpret_cast<GLFWmonitor *>(1), reinterpret_cast<GLFWmonitor *>(2)};
static GLFWmonitor **testGetMonitors(int *count) { *count = monitorCount; return monitorCount ? fakeMonitors : nullptr; }
static const GLFWvidmode *testGetVideoMode(GLFWmonitor *monitor) {
    static GLFWvidmode mode{1920, 1080, 8, 8, 8, 60};
    if (failure == 1 || (failure == 5 && monitor == fakeMonitors[0])) return nullptr;
    mode.width = failure == 4 ? 0 : 1920;
    return &mode;
}
static void testGetMonitorPos(GLFWmonitor *monitor, int *x, int *y) { *x = monitor == fakeMonitors[0] ? 0 : 1920; *y = 0; }
static void testGetMonitorWorkarea(GLFWmonitor *monitor, int *x, int *y, int *w, int *h) {
    testGetMonitorPos(monitor, x, y); *w = 1920; *h = 1040;
}
static void testGetMonitorContentScale(GLFWmonitor *, float *x, float *y) {
    *x = *y = failure == 2 ? 0.0f : failure == 3 ? std::numeric_limits<float>::quiet_NaN() : 1.25f;
}
#define glfwGetMonitors testGetMonitors
#define glfwGetVideoMode testGetVideoMode
#define glfwGetMonitorPos testGetMonitorPos
#define glfwGetMonitorWorkarea testGetMonitorWorkarea
#define glfwGetMonitorContentScale testGetMonitorContentScale
#include "../thirdparty/imgui/backends/imgui_impl_glfw.cpp"
#undef glfwGetMonitors
#undef glfwGetVideoMode
#undef glfwGetMonitorPos
#undef glfwGetMonitorWorkarea
#undef glfwGetMonitorContentScale
#include "imgui_impl_opengl3.h"

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        check(glfwInit(), "GLFW init");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow *window = glfwCreateWindow(640, 480, "Monitor regression", nullptr, nullptr);
        check(window != nullptr, "test window");
        glfwMakeContextCurrent(window);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        check(ImGui_ImplGlfw_InitForOpenGL(window, true), "platform init");
        check(ImGui_ImplOpenGL3_Init("#version 130"), "renderer init");
        auto &monitors = ImGui::GetPlatformIO().Monitors;
        auto frame = [&] {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            check(monitors.Size > 0, "empty monitor list");
            for (const auto &m : monitors)
                check(m.MainSize.x > 0 && m.MainSize.y > 0 && m.DpiScale > 0, "invalid geometry");
            ImGui::NewFrame();             // Runs the exact sanity assertion from the report
            ImGui::EndFrame();
            ImGui::UpdatePlatformWindows();
        };
        frame();
        check(monitors.Size == 1 && monitors[0].MainSize.x == 640, "startup fallback");
        for (int cycle = 0; cycle < 100; ++cycle) {
            monitorCount = 2; failure = 0; frame();
            check(monitors.Size == 2 && monitors[1].MainPos.x == 1920 && monitors[0].DpiScale == 1.25f, "two-monitor recovery");
            for (int mode = 1; mode <= 4; ++mode) {
                failure = mode; frame();
                check(monitors.Size == 2 && monitors[1].MainPos.x == 1920, "lost previous geometry");
                check(monitors[0].PlatformHandle == nullptr, "stale monitor handle");
            }
            monitorCount = 0; frame();
            check(monitors.Size == 2, "zero monitors lost cached layout");
            monitorCount = 2; failure = 5; frame();
            check(monitors.Size == 1 && monitors[0].MainPos.x == 1920, "partially valid monitor list");
            // No cache available, all reported displays invalid (startup case)
            monitors.clear(); failure = 1; frame();
            check(monitors.Size == 1 && monitors[0].MainSize.x == 640, "all-invalid startup fallback");
        }
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window); glfwTerminate();
        std::puts("PASS: 100 display loss/recovery cycles, null video modes, zero/NaN DPI, zero geometry, partial displays and startup fallback");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
