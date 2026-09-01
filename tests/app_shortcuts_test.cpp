#include "gui/app_shortcuts.hpp"
#include "imgui.h"
#include <cstdio>
#include <stdexcept>

static void
check(bool value, const char *message)
{
        if (!value)
                throw std::runtime_error(message);
}

static void
frame()
{
        ImGui::NewFrame();
        ImGui::EndFrame();
}

int
main()
{
        using ui::AppShortcut;
        using ui::AppShortcutInput;
        using ui::ResolveAppShortcut;
        AppShortcutInput state;
        state.primary     = true;
        state.savePressed = true;
        check(ResolveAppShortcut(state) == AppShortcut::Save, "Primary+S saves");
        state.shift = true;
        check(ResolveAppShortcut(state) == AppShortcut::SaveAs, "Primary+Shift+S saves as");
        for (auto block : {&AppShortcutInput::alt,
                           &AppShortcutInput::secondary,
                           &AppShortcutInput::textInput,
                           &AppShortcutInput::activeWidget,
                           &AppShortcutInput::popupOpen}) {
                state.*block = true;
                check(ResolveAppShortcut(state) == AppShortcut::None,
                      "Uncommitted editor/modal/extra modifier blocks shortcut");
                state.*block = false;
        }
        state                   = {};
        state.disconnectPressed = true;
        check(ResolveAppShortcut(state) == AppShortcut::None, "Unmodified D never disconnects");
        state.primary = true;
        check(ResolveAppShortcut(state) == AppShortcut::None, "Primary+D never disconnects");
        state.shift = true;
        check(ResolveAppShortcut(state) == AppShortcut::Disconnect, "Primary+Shift+D disconnects");
        for (auto binding :
             {&AppShortcutInput::openPressed, &AppShortcutInput::commandsPressed, &AppShortcutInput::connectPressed}) {
                state          = {};
                state.primary  = true;
                state.*binding = true;
                check(ResolveAppShortcut(state) != AppShortcut::None, "Primary command is available");
                state.shift = true;
                check(ResolveAppShortcut(state) == AppShortcut::None, "Unassigned shift combination stays unassigned");
        }
        state = {};
        state.spacePressed = true;
        check(ResolveAppShortcut(state) == AppShortcut::FreezeDisplay, "Space toggles display freeze");
        state.textInput = true;
        check(ResolveAppShortcut(state) == AppShortcut::None, "Space remains available to text editors");
        state = {};
        state.primary = state.shift = state.clearPressed = true;
        check(ResolveAppShortcut(state) == AppShortcut::ClearData, "Primary+Shift+Delete clears recorded data");
        state.popupOpen = true;
        check(ResolveAppShortcut(state) == AppShortcut::None, "Clear never runs through a popup");
        state.popupOpen = state.shift = false;
        check(ResolveAppShortcut(state) == AppShortcut::None, "Primary+Delete does not clear data");
        for (bool mac : {false, true}) {
                ImGui::CreateContext();
                auto &io                 = ImGui::GetIO();
                io.IniFilename           = nullptr;
                io.ConfigMacOSXBehaviors = mac;
                io.DisplaySize           = ImVec2(800, 600);
                io.DeltaTime             = 1.0f / 60.0f;
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                io.AddKeyEvent(mac ? ImGuiMod_Super : ImGuiMod_Ctrl, true);
                frame();
                state             = {};
                state.primary     = io.KeyCtrl;
                state.secondary   = io.KeySuper;
                state.savePressed = true;
                check(ResolveAppShortcut(state) == AppShortcut::Save, "Physical host primary key reaches Save exactly once");
                io.AddKeyEvent(mac ? ImGuiMod_Super : ImGuiMod_Ctrl, false);
                frame();
                if (mac) {
                        io.AddKeyEvent(ImGuiMod_Ctrl, true);
                        frame();
                        state.primary   = io.KeyCtrl;
                        state.secondary = io.KeySuper;
                        check(ResolveAppShortcut(state) == AppShortcut::None, "macOS physical Control is not Command");
                }
                ImGui::DestroyContext();
        }
        std::puts("PASS: exact application shortcuts, focus/modal guards and host Ctrl/Command mapping");
}
