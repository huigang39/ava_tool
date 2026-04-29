#include <cstdio>
#include <fstream>
#include <ranges>
#include <sstream>

#include "cJSON.h"
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

        loadSession();
}

Gui::~Gui()
{
        print_info(true, "~Gui()");

        saveSession();

        ImPlot::DestroyContext();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(window_);
        glfwTerminate();
}

void
Gui::saveSession() const
{
        cJSON *root = cJSON_CreateObject();

        cJSON *jlink = cJSON_CreateObject();
        cJSON_AddStringToObject(jlink, "device", JLinkDev::instance().deviceName().c_str());
        cJSON_AddNumberToObject(jlink, "speedKHz", JLinkDev::instance().speed());
        cJSON_AddNumberToObject(jlink, "hssPeriodUs", JLinkDev::instance().hssPeriodUs());
        cJSON_AddItemToObject(root, "jlink", jlink);

        cJSON *monitorsArr = cJSON_CreateArray();
        for (const auto &m : monitors_ | std::views::values) {
                cJSON *mObj = cJSON_CreateObject();
                cJSON_AddStringToObject(mObj, "name", m->getName().c_str());

                cJSON *scopesArr = cJSON_CreateArray();
                for (auto &s : m->getScopes() | std::views::values) {
                        cJSON *sObj = cJSON_CreateObject();
                        cJSON_AddStringToObject(sObj, "name", s->getName().c_str());
                        cJSON_AddStringToObject(
                            sObj, "draw", s->getDraw() == MonitorScope::DrawEnum::PLOT ? "PLOT" : "TABLE");

                        cJSON *chsArr = cJSON_CreateArray();
                        for (auto &ch : s->getChannels() | std::views::values) {
                                cJSON *chObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(chObj, "name", ch->getName().c_str());
                                cJSON_AddStringToObject(chObj, "type", ch->getType().c_str());
                                cJSON_AddNumberToObject(chObj, "addr", static_cast<double>(ch->getAddr()));
                                cJSON_AddStringToObject(chObj, "device", ch->getDevice().c_str());

                                cJSON *colorArr = cJSON_CreateArray();
                                for (int i = 0; i < 4; ++i)
                                        cJSON_AddItemToArray(colorArr, cJSON_CreateNumber(ch->getColor()[i]));
                                cJSON_AddItemToObject(chObj, "color", colorArr);

                                cJSON_AddBoolToObject(chObj, "useAutoColor", ch->useAutoColor());
                                cJSON_AddNumberToObject(chObj, "lineWeight", ch->getLineWeight());
                                cJSON_AddNumberToObject(chObj, "plotStyle", ch->getPlotStyle());
                                cJSON_AddItemToArray(chsArr, chObj);
                        }
                        cJSON_AddItemToObject(sObj, "channels", chsArr);
                        cJSON_AddItemToArray(scopesArr, sObj);
                }
                cJSON_AddItemToObject(mObj, "scopes", scopesArr);
                cJSON_AddItemToArray(monitorsArr, mObj);
        }
        cJSON_AddItemToObject(root, "monitors", monitorsArr);

        cJSON *editorsArr = cJSON_CreateArray();
        for (const auto &e : editors_ | std::views::values) {
                cJSON *eObj = cJSON_CreateObject();
                cJSON_AddStringToObject(eObj, "name", e->getName().c_str());
                cJSON_AddStringToObject(eObj, "cfgPath", e->getCfgPath().c_str());
                cJSON_AddStringToObject(eObj, "binPath", e->getBinPath().c_str());
                cJSON_AddStringToObject(eObj, "elfPath", e->getElfPath().c_str());
                cJSON_AddItemToArray(editorsArr, eObj);
        }
        cJSON_AddItemToObject(root, "editors", editorsArr);

        char         *out = cJSON_Print(root);
        std::ofstream ofs(sessionPath_);
        if (ofs && out)
                ofs << out;
        cJSON_free(out);
        cJSON_Delete(root);
}

void
Gui::loadSession()
{
        std::ifstream ifs(sessionPath_);
        if (!ifs.is_open())
                return;
        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string content = ss.str();
        if (content.empty())
                return;

        cJSON *root = cJSON_Parse(content.c_str());
        if (!root)
                return;

        if (const cJSON *jlink = cJSON_GetObjectItem(root, "jlink")) {
                if (const cJSON *dev = cJSON_GetObjectItem(jlink, "device"); cJSON_IsString(dev))
                        JLinkDev::instance().deviceName() = dev->valuestring;
                if (const cJSON *spd = cJSON_GetObjectItem(jlink, "speedKHz"); cJSON_IsNumber(spd))
                        JLinkDev::instance().speed() = spd->valueint;
                if (const cJSON *p = cJSON_GetObjectItem(jlink, "hssPeriodUs"); cJSON_IsNumber(p))
                        JLinkDev::instance().hssPeriodUs() = p->valueint;
        }

        if (const cJSON *monitorsArr = cJSON_GetObjectItem(root, "monitors"); cJSON_IsArray(monitorsArr)) {
                for (const cJSON *mItem = monitorsArr->child; mItem; mItem = mItem->next) {
                        const cJSON *nameItem = cJSON_GetObjectItem(mItem, "name");
                        if (!cJSON_IsString(nameItem))
                                continue;
                        std::string mName = nameItem->valuestring;
                        monitors_[mName]  = std::make_unique<Monitor>(mName);
                        Monitor *monitor  = monitors_[mName].get();

                        const cJSON *scopesArr = cJSON_GetObjectItem(mItem, "scopes");
                        if (!cJSON_IsArray(scopesArr))
                                continue;
                        for (const cJSON *sItem = scopesArr->child; sItem; sItem = sItem->next) {
                                const cJSON *snItem = cJSON_GetObjectItem(sItem, "name");
                                if (!cJSON_IsString(snItem))
                                        continue;
                                std::string sName = snItem->valuestring;
                                if (monitor->addScope(sName) != 0)
                                        continue;
                                MonitorScope *scope = monitor->getScopes()[sName].get();

                                if (const cJSON *drawItem = cJSON_GetObjectItem(sItem, "draw");
                                    cJSON_IsString(drawItem)) {
                                        if (std::string(drawItem->valuestring) == "TABLE")
                                                scope->setDraw(MonitorScope::DrawEnum::TABLE);
                                        else
                                                scope->setDraw(MonitorScope::DrawEnum::PLOT);
                                }

                                const cJSON *chsArr = cJSON_GetObjectItem(sItem, "channels");
                                if (!cJSON_IsArray(chsArr))
                                        continue;
                                for (const cJSON *chItem = chsArr->child; chItem; chItem = chItem->next) {
                                        const cJSON *cnItem = cJSON_GetObjectItem(chItem, "name");
                                        if (!cJSON_IsString(cnItem))
                                                continue;
                                        std::string chName = cnItem->valuestring;
                                        if (scope->addChannel(chName) != 0)
                                                continue;
                                        MonitorChannel *ch = scope->findChannel(chName);
                                        if (!ch)
                                                continue;

                                        if (const cJSON *t = cJSON_GetObjectItem(chItem, "type"); cJSON_IsString(t))
                                                ch->setType(t->valuestring);
                                        if (const cJSON *a = cJSON_GetObjectItem(chItem, "addr"); cJSON_IsNumber(a))
                                                ch->setAddr(static_cast<usize>(a->valuedouble));
                                        if (const cJSON *d = cJSON_GetObjectItem(chItem, "device"); cJSON_IsString(d))
                                                ch->setDevice(d->valuestring);
                                        if (const cJSON *colArr = cJSON_GetObjectItem(chItem, "color");
                                            cJSON_IsArray(colArr)) {
                                                int idx = 0;
                                                for (const cJSON *c = colArr->child; c && idx < 4;
                                                     c = c->next, ++idx)
                                                        ch->getColor()[idx] = static_cast<f32>(c->valuedouble);
                                        }
                                        if (const cJSON *uac = cJSON_GetObjectItem(chItem, "useAutoColor");
                                            cJSON_IsBool(uac))
                                                ch->useAutoColor() = cJSON_IsTrue(uac);
                                        if (const cJSON *lw = cJSON_GetObjectItem(chItem, "lineWeight");
                                            cJSON_IsNumber(lw))
                                                ch->getLineWeight() = static_cast<f32>(lw->valuedouble);
                                        if (const cJSON *ps = cJSON_GetObjectItem(chItem, "plotStyle");
                                            cJSON_IsNumber(ps))
                                                ch->getPlotStyle() = ps->valueint;

                                        if (ch->getDevice() == "LOCAL")
                                                MonitorScope::shmInit(*ch);
                                }
                        }
                }
        }

        if (const cJSON *editorsArr = cJSON_GetObjectItem(root, "editors"); cJSON_IsArray(editorsArr)) {
                for (const cJSON *eItem = editorsArr->child; eItem; eItem = eItem->next) {
                        const cJSON *nItem = cJSON_GetObjectItem(eItem, "name");
                        if (!cJSON_IsString(nItem))
                                continue;
                        std::string eName = nItem->valuestring;
                        editors_[eName]   = std::make_unique<Editor>(eName);

                        const cJSON      *cfgItem = cJSON_GetObjectItem(eItem, "cfgPath");
                        const cJSON      *binItem = cJSON_GetObjectItem(eItem, "binPath");
                        const cJSON      *elfItem = cJSON_GetObjectItem(eItem, "elfPath");
                        const std::string cfg     = cJSON_IsString(cfgItem) ? cfgItem->valuestring : "";
                        const std::string bin     = cJSON_IsString(binItem) ? binItem->valuestring : "";
                        const std::string elf     = cJSON_IsString(elfItem) ? elfItem->valuestring : "";
                        editors_[eName]->restoreFromPaths(cfg, bin, elf);
                }
        }

        cJSON_Delete(root);
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
