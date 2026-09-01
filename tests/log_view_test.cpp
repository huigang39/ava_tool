#include "gui/log_view.hpp"
#include <cstdio>
#include <stdexcept>

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}

int
main()
{
        ImGui::CreateContext();
        try {
                auto &io                        = ImGui::GetIO();
                io.IniFilename                  = nullptr;
                io.DisplaySize                  = ImVec2(900, 650);
                io.DeltaTime                    = 1.0f / 60;
                io.ConfigInputTrickleEventQueue = false;
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                static std::string clipboard;
                ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext *, const char *val) {
                        clipboard = val;
                };
                ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext *) { return clipboard.c_str(); };
                ui::LogText log;
                const auto  red   = ImGui::GetColorU32(ImVec4(1, 0.4f, 0.4f, 1));
                const auto  green = ImGui::GetColorU32(ImVec4(0.3f, 1, 0.5f, 1));
                log.timestamp(1700000000123);
                log.append("Call(\"中文\") ", red);
                log.append("OK: 123\n", green);
                for (int i = 0; i < 100; ++i)
                        log.append("entry " + std::to_string(i) + "\n");
                bool         follow = true;
                ImGuiID      id     = 0;
                ImGuiWindow *child  = nullptr;
                ImVec2       copyButton;
                auto         frame = [&] {
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(10, 10));
                        ImGui::SetNextWindowSize(ImVec2(700, 500));
                        ImGui::Begin("Log view check");
                        ui::CopyLogButton(log.text);
                        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                        copyButton = ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2);
                        id         = ImGui::GetID("log");
                        ui::LogView("log", log, ImVec2(600, 180), &follow);
                        for (auto *window : ImGui::GetCurrentWindow()->DC.ChildWindows)
                                if (window->ChildId == id)
                                        child = window;
                        ImGui::End();
                        ImGui::Render();
                };
                auto key = [&](ImGuiKey val, bool ctrl = false, bool shift = false) {
                        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
                        io.AddKeyEvent(ImGuiMod_Shift, shift);
                        io.AddKeyEvent(val, true);
                        frame();
                        io.AddKeyEvent(val, false);
                        io.AddKeyEvent(ImGuiMod_Ctrl, false);
                        io.AddKeyEvent(ImGuiMod_Shift, false);
                        frame();
                };
                auto click = [&](ImVec2 pos) {
                        io.AddMousePosEvent(pos.x, pos.y);
                        frame();
                        io.AddMouseButtonEvent(0, true);
                        frame();
                        io.AddMouseButtonEvent(0, false);
                        frame();
                };
                frame();
                frame();
                frame();
                check(child && child->Scroll.y >= child->ScrollMax.y - 1, "new log follows the bottom");
                click(ImVec2(child->Pos.x + 50, child->Pos.y + 30));
                key(ImGuiKey_Home, true);
                key(ImGuiKey_End, false, true);
                key(ImGuiKey_C, true);
                check(clipboard == log.text.substr(0, log.text.find('\n')),
                      "partial selection copies timestamp, Unicode, call and result");
                bool sawRed = false, sawGreen = false;
                for (const auto &vertex : child->DrawList->VtxBuffer) {
                        sawRed   |= vertex.col == red;
                        sawGreen |= vertex.col == green;
                }
                check(sawRed && sawGreen, "success and failure text keep their colors while selected");
                key(ImGuiKey_A, true);
                check(!follow, "selection pauses following new output");
                log.append("new output while selecting\n");
                frame();
                key(ImGuiKey_A, true);
                key(ImGuiKey_C, true);
                check(clipboard == log.text, "select-all copy includes current output and line breaks");
                const auto original = log.text;
                io.AddInputCharactersUTF8("cannot edit");
                frame();
                key(ImGuiKey_V, true);
                key(ImGuiKey_Backspace);
                check(log.text == original, "typing, paste and deletion cannot edit logs");
                key(ImGuiKey_End, true);
                frame();
                frame();
                check(follow && child->Scroll.y >= child->ScrollMax.y - 1, "returning to the bottom resumes following");
                io.AddMousePosEvent(child->Pos.x + 50, child->Pos.y + 50);
                frame();
                io.AddMouseWheelEvent(0, 3);
                frame();
                frame();
                check(!follow && child->Scroll.y < child->ScrollMax.y - 1, "scrolling upward pauses following");
                const float historyPosition = child->Scroll.y;
                log.append("more new output\n");
                frame();
                frame();
                check(std::abs(child->Scroll.y - historyPosition) < 1, "new output preserves the history position");
                click(copyButton);
                check(clipboard == log.text, "copy-all button copies the full log while browsing history");
                log = {};
                frame();
                frame();
                click(copyButton);
                check(clipboard.empty(), "copy-all after clearing does not copy stale output");
                std::string plain = "plain log\n";
                ImGui::NewFrame();
                ImGui::Begin("Plain log check");
                ui::LogView("plain", plain, ImVec2(300, 100));
                ImGui::End();
                ImGui::Render();
                ImGui::DestroyContext();
                puts("PASS: partial/full/Unicode clipboard, live output, read-only, colors, follow/history, clear and plain "
                     "logs");
                return 0;
        } catch (const std::exception &error) {
                fprintf(stderr, "FAIL: %s\n", error.what());
                ImGui::DestroyContext();
                return 1;
        }
}
