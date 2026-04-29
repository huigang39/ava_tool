#include <cstdio>
#include <ranges>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include "gui.hpp"
#include "jlink_dev.hpp"
#include "monitor.hpp"

std::vector<std::string> Gui::sDroppedFiles_{};

void
Gui::glfwErrCb(const int err, const char *desc)
{
        print_error(true, "Glfw Error %d: %s", err, desc);
}

void
Gui::glfwDropCb(GLFWwindow * /*window*/, const int count, const char **paths)
{
        for (int i = 0; i < count; ++i)
                sDroppedFiles_.emplace_back(paths[i]);
}

Gui::Gui()
{
        print_info(true, "Gui()");

        glfwSetErrorCallback(glfwErrCb);
        if (!glfwInit())
                print_error(true, "Failed to init GLFW");

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

        window_ = glfwCreateWindow(windowWidth_, windowWidth_, windowTitle_.c_str(), nullptr, nullptr);
        if (window_ == nullptr)
                print_error(true, "Failed to create GLFW window");

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        glfwSetDropCallback(window_, glfwDropCb);

        glfwGetWindowContentScale(window_, &xScale_, &yScale_);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();

        io.FontGlobalScale  = xScale_;
        io.ConfigFlags     |= ImGuiConfigFlags_DockingEnable;
        io.FontGlobalScale  = 1.5f;
        io.Fonts->AddFontFromFileTTF(fontFile_.c_str(), 20.0f);

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init(glslVer_);

        ImPlot::CreateContext();
}

Gui::~Gui()
{
        print_info(true, "~Gui()");

        ImPlot::DestroyContext();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(window_);
        glfwTerminate();
}

void
Gui::drawBar()
{
        if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("Window")) {
                        if (ImGui::MenuItem("Add Monitor")) {
                                std::string monitorName = "Monitor_" + std::to_string(monitors_.size());
                                monitors_[monitorName]  = std::make_unique<Monitor>(monitorName);
                                print_info(true, "Add Monitor: %s", monitorName.c_str());
                        }
                        if (ImGui::MenuItem("Add Editor")) {
                                std::string editorName = "Editor_" + std::to_string(editors_.size());
                                editors_[editorName]   = std::make_unique<Editor>(editorName);
                                print_info(true, "Add Editor: %s", editorName.c_str());
                        }
                        ImGui::EndMenu();
                }

                ImGui::Separator();
                JLinkDev::instance().drawUI();

                ImGui::Separator();
                const bool paused = g_monitorPaused.load();
                if (ImGui::SmallButton(paused ? "Resume" : "Pause"))
                        g_monitorPaused.store(!paused);
                if (paused) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "[PAUSED]");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(space)");

                ImGui::EndMainMenuBar();
        }
}

void
Gui::loop()
{
        while (!glfwWindowShouldClose(window_)) {
                glfwPollEvents();

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                drawBar();

                if (const ImGuiIO &io = ImGui::GetIO();
                    !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))
                        g_monitorPaused.store(!g_monitorPaused.load());

                ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

                {
                        for (const auto &monitor : monitors_ | std::views::values)
                                monitor->updateDisplay();

                        for (const auto &editor : editors_ | std::views::values)
                                editor->updateDisplay();
                }

                sDroppedFiles_.clear();

                ImGui::Render();
                int display_w, display_h;
                glfwGetFramebufferSize(window_, &display_w, &display_h);
                glViewport(0, 0, display_w, display_h);
                glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                glfwSwapBuffers(window_);
        }
}
