#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "gui/wave_input.hpp"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    std::unique_ptr<Gui> gui;
    int result = 0;
    try {
        std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
        check(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(argv[1]);
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        float applied = 10;
        WaveInputDraft draft;
        char other[64]{};
        int commits = 0;
        auto frame = [&](int focus = 0) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::Begin("Wave inputs");
            if (focus == 1) ImGui::SetKeyboardFocusHere();
            if (inputWaveFloatOnEnter("Frequency", applied, draft, "%.3f")) ++commits;
            if (focus == 2) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("Other", other, sizeof(other));
            ImGui::End();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        };
        auto type = [&](const char *text) {
            frame(1); frame();
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
            io.AddInputCharactersUTF8(text); frame();
        };
        auto enter = [&]() {
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiKey_Enter, true); frame();
            io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        };
        frame(); frame();
        type("123.5");
        check(applied == 10 && commits == 0, "typing does not apply configuration");
        frame(2); frame();
        check(applied == 10 && commits == 0, "leaving input does not apply configuration");
        type("123.5"); enter();
        check(applied == 123.5f && commits == 1, "Enter applies complete numeric value exactly once");
        type("nan"); enter();
        check(applied == 123.5f && commits == 1, "invalid numeric input is rejected");
        type("-2.5"); enter();
        check(applied == -2.5f && commits == 2, "negative value is committed on Enter");
        puts("PASS: real wave input typing, focus loss, Enter commit and invalid input rejection");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what()); result = 1;
    }
    gui.reset();
    JLinkPort::instance().close();
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
