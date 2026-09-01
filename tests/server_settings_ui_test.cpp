#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/remote_files.hpp"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

static void check(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

int main()
{
        try {
                module_init();
                ImGui::CreateContext();
                ImGuiIO &io    = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplaySize = ImVec2(960, 760);
                std::string error;
                ImGui::GetCurrentContext()->ErrorCallbackUserData = &error;
                ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext *, void *data, const char *message) {
                        *static_cast<std::string *>(data) = message;
                };
                unsigned char *pixels = nullptr;
                int            width = 0, height = 0;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

                const auto settingsDir = std::filesystem::current_path() / "server-settings-ui";
                std::filesystem::create_directories(settingsDir / "website");
                LocalFileServer sharing;
                LocalFileServer website;
                sharing.config.folder = (settingsDir / "shared").string();
                website.config.folder = (settingsDir / "website").string();
                website.config.port   = 0;

                auto frame = [&] {
                        io.DeltaTime = 1.0f / 60.0f;
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
                        ImGui::SetNextWindowSize(ImVec2(860, 650), ImGuiCond_Always);
                        ImGui::Begin("Server Settings###ServerSettingsTest",
                                     nullptr,
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
                        drawServerSettings(sharing, website, settingsDir.string());
                        ImGui::End();
                        ImGui::Render();
                        check(error.empty(), error.c_str());
                };
                frame();
                frame();

                ImGuiWindow *host = ImGui::FindWindowByName("Server Settings###ServerSettingsTest");
                check(host != nullptr, "server settings host renders");
                auto findChild = [] {
                        for (auto *window : ImGui::GetCurrentContext()->Windows)
                                if (window->Active && std::strstr(window->Name, "/##server_settings_fixed"))
                                        return window;
                        return static_cast<ImGuiWindow *>(nullptr);
                };
                ImGuiWindow *child = findChild();
                check(child != nullptr, "server settings content uses a fixed-size child");
                const ImVec2 size = child->Size;
                check(std::abs(size.x - 720.0f) < 0.5f && std::abs(size.y - 500.0f) < 0.5f,
                      "server settings panel starts at its fixed width and height");

                ImGuiTabBar *tabs = ImGui::TabBarFindByID(child->GetID("##server_settings"));
                check(tabs != nullptr && tabs->Tabs.Size == 3, "all server settings pages are present");
                for (int tab = 0; tab < tabs->Tabs.Size; ++tab) {
                        tabs->NextSelectedTabId = tabs->Tabs[tab].ID;
                        frame();
                        frame();
                        child = findChild();
                        check(child && std::abs(child->Size.x - size.x) < 0.5f &&
                                  std::abs(child->Size.y - size.y) < 0.5f,
                              "switching server settings pages never resizes the popup");
                }

                check(website.start(), website.error().c_str());
                tabs->NextSelectedTabId = tabs->Tabs[1].ID;
                frame();
                frame();
                child = findChild();
                check(child && std::abs(child->Size.x - size.x) < 0.5f && std::abs(child->Size.y - size.y) < 0.5f,
                      "deploying the website keeps the settings popup size fixed");
                website.stop();
                frame();
                frame();
                child = findChild();
                check(child && std::abs(child->Size.x - size.x) < 0.5f && std::abs(child->Size.y - size.y) < 0.5f,
                      "stopping the website keeps the settings popup size fixed");

                website.config.folder.clear();
                ImGui::DestroyContext();
                std::puts("PASS: fixed-size server settings, stable tab switching and conflict-free IDs");
                return 0;
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
