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
#include "parser.hpp"
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
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.FontGlobalScale  = 1.0f;
        io.Fonts->AddFontFromFileTTF(fontFile_.c_str(), 18.0f);

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init(glslVer_);

        ImPlot::CreateContext();

        loadSession();

        if (motorProfiles_.empty())
                motorProfiles_.push_back(MotorProfile{});
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

        cJSON_AddBoolToObject(root, "showCalculator", showCalculator_);

        cJSON *monitorsArr = cJSON_CreateArray();
        for (const auto &m : monitors_ | std::views::values) {
                cJSON *mObj = cJSON_CreateObject();
                cJSON_AddStringToObject(mObj, "name", m->getName().c_str());
                cJSON_AddStringToObject(mObj, "samplingMode", m->samplingMode_ == Monitor::SamplingMode::HSS ? "HSS" : "POLL");
                cJSON_AddNumberToObject(mObj, "maxSampleHz", m->maxSampleHz_);
                cJSON_AddNumberToObject(mObj, "historySeconds", static_cast<double>(m->historySeconds_));
                cJSON_AddNumberToObject(mObj, "maxDisplayPoints", static_cast<double>(m->maxDisplayPoints_));

                cJSON *scopesArr = cJSON_CreateArray();
                for (auto &s : m->getScopes() | std::views::values) {
                        cJSON *sObj = cJSON_CreateObject();
                        cJSON_AddStringToObject(sObj, "name", s->getName().c_str());
                        cJSON_AddStringToObject(
                            sObj, "draw", s->getDraw() == MonitorScope::DrawEnum::PLOT ? "PLOT" : "TABLE");
                        cJSON_AddNumberToObject(sObj, "height", static_cast<double>(s->getHeight()));

                        cJSON *chsArr = cJSON_CreateArray();
                        for (auto &ch : s->getChannels() | std::views::values) {
                                cJSON *chObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(chObj, "name", ch->getName().c_str());
                                cJSON_AddStringToObject(chObj, "type", ch->getType().c_str());
                                char addrHex[32];
                                snprintf(addrHex, sizeof(addrHex), "0x%zX", ch->getAddr());
                                cJSON_AddStringToObject(chObj, "addr", addrHex);
                                cJSON_AddStringToObject(chObj, "device", ch->getDevice().c_str());

                                cJSON *colorArr = cJSON_CreateArray();
                                for (int i = 0; i < 4; ++i)
                                        cJSON_AddItemToArray(colorArr, cJSON_CreateNumber(ch->getColor()[i]));
                                cJSON_AddItemToObject(chObj, "color", colorArr);

                                cJSON_AddBoolToObject(chObj, "useAutoColor", ch->useAutoColor());
                                cJSON_AddNumberToObject(chObj, "lineWeight", ch->getLineWeight());
                                cJSON_AddStringToObject(chObj, "symbolName", ch->getSymbolName().c_str());
                                cJSON_AddNumberToObject(chObj, "plotStyle", ch->getPlotStyle());
                                cJSON_AddBoolToObject(chObj, "showMarkers", ch->showMarkers());
                                cJSON_AddBoolToObject(chObj, "show", ch->show());

                                if (ch->isEnum()) {
                                        cJSON *enumsArr = cJSON_CreateArray();
                                        for (const auto &e : ch->getEnums()) {
                                                cJSON *eObj = cJSON_CreateObject();
                                                cJSON_AddStringToObject(eObj, "name", e.name.c_str());
                                                cJSON_AddNumberToObject(eObj, "value",
                                                                        static_cast<double>(e.value));
                                                cJSON_AddItemToArray(enumsArr, eObj);
                                        }
                                        cJSON_AddItemToObject(chObj, "enums", enumsArr);
                                }
                                cJSON_AddItemToArray(chsArr, chObj);
                        }
                        cJSON_AddItemToObject(sObj, "channels", chsArr);
                        cJSON_AddItemToArray(scopesArr, sObj);
                }
                cJSON_AddNumberToObject(mObj, "historySeconds", static_cast<double>(m->historySeconds_));
                cJSON_AddItemToObject(mObj, "scopes", scopesArr);
                cJSON_AddItemToArray(monitorsArr, mObj);
        }
        cJSON_AddItemToObject(root, "monitors", monitorsArr);

        cJSON *parsersArr = cJSON_CreateArray();
        for (const auto &p : parsers_ | std::views::values) {
                cJSON *pObj = cJSON_CreateObject();
                cJSON_AddStringToObject(pObj, "name", p->getName().c_str());
                cJSON_AddStringToObject(pObj, "cfgPath", p->getCfgPath().c_str());
                cJSON_AddStringToObject(pObj, "binPath", p->getBinPath().c_str());
                cJSON_AddStringToObject(pObj, "elfPath", p->getElfPath().c_str());
                cJSON_AddItemToArray(parsersArr, pObj);
        }
        cJSON_AddItemToObject(root, "parsers", parsersArr);

        cJSON *motorsArr = cJSON_CreateArray();
        for (const auto &mp : motorProfiles_) {
                cJSON *mpObj = cJSON_CreateObject();
                cJSON_AddStringToObject(mpObj, "modelName", mp.modelName);
                cJSON_AddNumberToObject(mpObj, "Rs", mp.Rs);
                cJSON_AddNumberToObject(mpObj, "Ld", mp.Ld);
                cJSON_AddNumberToObject(mpObj, "Lq", mp.Lq);
                cJSON_AddNumberToObject(mpObj, "polePairs", mp.polePairs);
                cJSON_AddNumberToObject(mpObj, "Kt", mp.Kt);
                cJSON_AddNumberToObject(mpObj, "backEmfFreq", mp.backEmfFreq);
                cJSON_AddNumberToObject(mpObj, "backEmfVpp", mp.backEmfVpp);
                cJSON_AddItemToArray(motorsArr, mpObj);
        }
        cJSON_AddItemToObject(root, "motorProfiles", motorsArr);
        cJSON_AddNumberToObject(root, "currentMotorProfile", currentMotorProfile_);

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

        if (const cJSON *sc = cJSON_GetObjectItem(root, "showCalculator")) {
                showCalculator_ = cJSON_IsTrue(sc);
        }

        if (const cJSON *monitorsArr = cJSON_GetObjectItem(root, "monitors"); cJSON_IsArray(monitorsArr)) {
                for (const cJSON *mItem = monitorsArr->child; mItem; mItem = mItem->next) {
                        const cJSON *nameItem = cJSON_GetObjectItem(mItem, "name");
                        if (!cJSON_IsString(nameItem))
                                continue;
                        std::string mName = nameItem->valuestring;
                        monitors_[mName]  = std::make_unique<Monitor>(mName);
                        Monitor *monitor  = monitors_[mName].get();

                        if (const cJSON *hSec = cJSON_GetObjectItem(mItem, "historySeconds"); cJSON_IsNumber(hSec))
                                monitor->historySeconds_ = static_cast<f32>(hSec->valuedouble);
                        
                        if (const cJSON *mHz = cJSON_GetObjectItem(mItem, "maxSampleHz"); cJSON_IsNumber(mHz))
                                monitor->maxSampleHz_ = mHz->valueint;

                        if (const cJSON *mPts = cJSON_GetObjectItem(mItem, "maxDisplayPoints"); cJSON_IsNumber(mPts))
                                monitor->maxDisplayPoints_ = static_cast<u32>(mPts->valueint);

                        if (const cJSON *sMode = cJSON_GetObjectItem(mItem, "samplingMode"); cJSON_IsString(sMode)) {
                                if (std::string(sMode->valuestring) == "POLL")
                                        monitor->samplingMode_ = Monitor::SamplingMode::POLL;
                                else
                                        monitor->samplingMode_ = Monitor::SamplingMode::HSS;
                        }

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
                                if (const cJSON *hItem = cJSON_GetObjectItem(sItem, "height"); cJSON_IsNumber(hItem))
                                        scope->getHeight() = static_cast<f32>(hItem->valuedouble);

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
                                        ch->historySeconds_ = monitor->historySeconds_;

                                        if (const cJSON *sn = cJSON_GetObjectItem(chItem, "symbolName"); cJSON_IsString(sn))
                                                ch->getSymbolName() = sn->valuestring;

                                        if (const cJSON *t = cJSON_GetObjectItem(chItem, "type"); cJSON_IsString(t))
                                                ch->setType(t->valuestring);
                                        if (const cJSON *a = cJSON_GetObjectItem(chItem, "addr")) {
                                                if (cJSON_IsString(a)) {
                                                        usize addrVal = 0;
                                                        if (sscanf(a->valuestring, "%zx", &addrVal) == 1)
                                                                ch->setAddr(addrVal);
                                                } else if (cJSON_IsNumber(a)) {
                                                        ch->setAddr(static_cast<usize>(a->valuedouble));
                                                }
                                        }
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
                                        if (const cJSON *sm = cJSON_GetObjectItem(chItem, "showMarkers");
                                            cJSON_IsBool(sm))
                                                ch->showMarkers() = cJSON_IsTrue(sm);
                                        if (const cJSON *sh = cJSON_GetObjectItem(chItem, "show");
                                            cJSON_IsBool(sh))
                                                ch->show() = cJSON_IsTrue(sh);

                                        if (const cJSON *enumsArr = cJSON_GetObjectItem(chItem, "enums");
                                            cJSON_IsArray(enumsArr)) {
                                                std::vector<MonitorChannel::EnumEntry> ents;
                                                for (const cJSON *eItem = enumsArr->child; eItem;
                                                     eItem            = eItem->next) {
                                                        const cJSON *nObj = cJSON_GetObjectItem(eItem, "name");
                                                        const cJSON *vObj = cJSON_GetObjectItem(eItem, "value");
                                                        if (cJSON_IsString(nObj) && cJSON_IsNumber(vObj))
                                                                ents.push_back({nObj->valuestring,
                                                                                static_cast<i64>(vObj->valuedouble)});
                                                }
                                                ch->setEnums(std::move(ents));
                                        }

                                        if (ch->getDevice() == "LOCAL")
                                                MonitorScope::shmInit(*ch);
                                }
                        }
                }
        }

        if (const cJSON *parsersArr = cJSON_GetObjectItem(root, "parsers"); cJSON_IsArray(parsersArr)) {
                for (const cJSON *pItem = parsersArr->child; pItem; pItem = pItem->next) {
                        const cJSON *nItem = cJSON_GetObjectItem(pItem, "name");
                        if (!cJSON_IsString(nItem))
                                continue;
                        std::string pName = nItem->valuestring;
                        parsers_[pName]   = std::make_unique<Parser>(pName);

                        const cJSON      *cfgItem = cJSON_GetObjectItem(pItem, "cfgPath");
                        const cJSON      *binItem = cJSON_GetObjectItem(pItem, "binPath");
                        const cJSON      *elfItem = cJSON_GetObjectItem(pItem, "elfPath");
                        const std::string cfg     = cJSON_IsString(cfgItem) ? cfgItem->valuestring : "";
                        const std::string bin     = cJSON_IsString(binItem) ? binItem->valuestring : "";
                        const std::string elf     = cJSON_IsString(elfItem) ? elfItem->valuestring : "";
                        
                        // restoreFromPaths was removed in Parser, paths are loaded on demand or via draw loop logic.
                        // If we want to restore on boot, we should call loadCfg/loadBin/loadElf here.
                        if (!cfg.empty()) parsers_[pName]->loadCfg(cfg);
                        if (!bin.empty()) parsers_[pName]->loadBin(bin);
                        if (!elf.empty()) parsers_[pName]->loadElf(elf);
                }
        }

        if (const cJSON *motorsArr = cJSON_GetObjectItem(root, "motorProfiles"); cJSON_IsArray(motorsArr)) {
                motorProfiles_.clear();
                for (const cJSON *mpItem = motorsArr->child; mpItem; mpItem = mpItem->next) {
                        MotorProfile mp;
                        if (const cJSON *nameItem = cJSON_GetObjectItem(mpItem, "modelName"); cJSON_IsString(nameItem))
                                snprintf(mp.modelName, sizeof(mp.modelName), "%s", nameItem->valuestring);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Rs"); cJSON_IsNumber(item))
                                mp.Rs = static_cast<float>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Ld"); cJSON_IsNumber(item))
                                mp.Ld = static_cast<float>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Lq"); cJSON_IsNumber(item))
                                mp.Lq = static_cast<float>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "polePairs"); cJSON_IsNumber(item))
                                mp.polePairs = item->valueint;
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Kt"); cJSON_IsNumber(item))
                                mp.Kt = static_cast<float>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "backEmfFreq"); cJSON_IsNumber(item))
                                mp.backEmfFreq = static_cast<float>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "backEmfVpp"); cJSON_IsNumber(item))
                                mp.backEmfVpp = static_cast<float>(item->valuedouble);
                        motorProfiles_.push_back(mp);
                }
        }
        if (const cJSON *item = cJSON_GetObjectItem(root, "currentMotorProfile"); cJSON_IsNumber(item))
                currentMotorProfile_ = item->valueint;
        
        if (currentMotorProfile_ < 0 || currentMotorProfile_ >= static_cast<int>(motorProfiles_.size()))
                currentMotorProfile_ = 0;

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
                        if (ImGui::MenuItem("Add Parser")) {
                                std::string parserName = "Parser_" + std::to_string(parsers_.size());
                                parsers_[parserName]   = std::make_unique<Parser>(parserName);
                                print_info(true, "Add Parser: %s", parserName.c_str());
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Tools")) {
                        ImGui::MenuItem("Joint Calculator", nullptr, &showCalculator_);
                        ImGui::EndMenu();
                }

                ImGui::Separator();
                bool wasConnected = JLinkDev::instance().isConnected();
                JLinkDev::instance().drawUI();
                if (!wasConnected && JLinkDev::instance().isConnected()) {
                        Monitor::clearAll();
                }
                const bool paused = g_monitorPaused.load();
                if (paused) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)); // Red (State: Paused)
                        if (ImGui::SmallButton("RESUME")) g_monitorPaused.store(false);
                        ImGui::PopStyleColor();
                } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f)); // Green (State: Running)
                        if (ImGui::SmallButton("PAUSE")) g_monitorPaused.store(true);
                        ImGui::PopStyleColor();
                }


                // Total points currently in memory (across all channels)
                u64 totalPts = 0;
                for (const auto &m : monitors_ | std::views::values)
                        for (const auto &s : m->getScopes() | std::views::values)
                                for (const auto &ch : s->getChannels() | std::views::values)
                                        totalPts += ch->storedCount();
                char ptsBuf[32];
                if (totalPts >= 1000000)
                        snprintf(ptsBuf, sizeof(ptsBuf), "%.2f M pts", totalPts / 1000000.0);
                else if (totalPts >= 1000)
                        snprintf(ptsBuf, sizeof(ptsBuf), "%.1f k pts", totalPts / 1000.0);
                else
                        snprintf(ptsBuf, sizeof(ptsBuf), "%llu pts", totalPts);

                char fpsBuf[32];
                snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", ImGui::GetIO().Framerate);

                const float ptsWidthFixed = ImGui::CalcTextSize("999.99 M pts").x;
                const float fpsWidthFixed = ImGui::CalcTextSize("9999 FPS").x;
                const float spacing       = ImGui::GetStyle().ItemSpacing.x;

                const float totalWidth = fpsWidthFixed + ptsWidthFixed + spacing;
                const float startX     = ImGui::GetWindowWidth() - totalWidth - spacing * 2;

                // PTS (Green, Right-aligned in its block)
                ImGui::SetCursorPosX(startX + ptsWidthFixed - ImGui::CalcTextSize(ptsBuf).x);
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", ptsBuf);

                // FPS (Red, Right-aligned in its block)
                ImGui::SameLine();
                ImGui::SetCursorPosX(startX + ptsWidthFixed + spacing + fpsWidthFixed - ImGui::CalcTextSize(fpsBuf).x);
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", fpsBuf);

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

                static float saveToastAlpha = 0.0f;
                static float spaceLastTime   = 0.0f;
                static int   spaceClickCount = 0;
                if (const ImGuiIO &io = ImGui::GetIO(); !io.WantTextInput) {
                        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                                float now = static_cast<float>(ImGui::GetTime());
                                if (now - spaceLastTime < 0.35f) {
                                        spaceClickCount++;
                                } else {
                                        spaceClickCount = 1;
                                }
                                spaceLastTime = now;

                                if (spaceClickCount >= 3) {
                                        // Triple Click: Reconnect J-Link
                                        JLinkDev::instance().close();
                                        if (JLinkDev::instance().open()) {
                                                JLinkDev::instance().connect();
                                                print_info(true, "J-Link Reconnected via Triple Space");
                                        }
                                        spaceClickCount = 0;
                                } else {
                                        // Single Click: Toggle Pause
                                        g_monitorPaused.store(!g_monitorPaused.load());
                                }
                        }
                        
                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                                saveSession();
                                saveToastAlpha = 2.0f;
                                print_info(true, "Session saved via Ctrl+S");
                        }
                }

                if (saveToastAlpha > 0.0f) {
                        saveToastAlpha -= ImGui::GetIO().DeltaTime;
                        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2.0f, ImGui::GetIO().DisplaySize.y - 50.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowBgAlpha(std::min(1.0f, saveToastAlpha) * 0.8f);
                        if (ImGui::Begin("##save_toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
                                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, std::min(1.0f, saveToastAlpha)), "Session Saved Successfully!");
                        }
                        ImGui::End();
                }

                ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

                if (showCalculator_)
                        drawCalculator();

                {
                        std::lock_guard lk(mtxMonitors_);
                        for (auto it = monitors_.begin(); it != monitors_.end();) {
                                if (it->second->isPendingDelete())
                                        it = monitors_.erase(it);
                                else {
                                        it->second->updateDisplay();
                                        ++it;
                                }
                        }
                }

                {
                        for (auto it = parsers_.begin(); it != parsers_.end();) {
                                if (it->second->isPendingDelete())
                                        it = parsers_.erase(it);
                                else {
                                        it->second->updateDisplay();
                                        if (it->second->consumeElfReloaded()) {
                                                syncSymbolAddresses(it->second->getElfInfo());
                                        }
                                        ++it;
                                }
                        }
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

void
Gui::syncSymbolAddresses(const ElfInfo &elfInfo)
{
        std::lock_guard lk(mtxMonitors_);
        int             count = 0;
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (ch->getSymbolName().empty())
                                        continue;

                                // Try to find the symbol in the new elfInfo
                                bool found = false;
                                for (const auto &sym : elfInfo.symbols) {
                                        if (sym.name == ch->getSymbolName()) {
                                                ch->setAddr(sym.addr);
                                                found = true;
                                                count++;
                                                break;
                                        }
                                }
                        }
                }
        }
        if (count > 0) {
                print_info(true, "Synced %d channel addresses with new ELF", count);
        }
}

void
Gui::drawCalculator()
{
        if (!ImGui::Begin("Motor Parameter Calculator", &showCalculator_)) {
                ImGui::End();
                return;
        }

        if (motorProfiles_.empty()) {
                motorProfiles_.push_back(MotorProfile{});
                currentMotorProfile_ = 0;
        }
        if (currentMotorProfile_ >= static_cast<int>(motorProfiles_.size())) {
                currentMotorProfile_ = 0;
        }

        ImGui::Text("Saved Profiles:");
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##motor_profiles", motorProfiles_[currentMotorProfile_].modelName)) {
                for (int i = 0; i < static_cast<int>(motorProfiles_.size()); ++i) {
                        const bool is_selected = (currentMotorProfile_ == i);
                        if (ImGui::Selectable(motorProfiles_[i].modelName, is_selected))
                                currentMotorProfile_ = i;
                        if (is_selected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
                MotorProfile mp;
                snprintf(mp.modelName, sizeof(mp.modelName), "Motor_%d", static_cast<int>(motorProfiles_.size() + 1));
                motorProfiles_.push_back(mp);
                currentMotorProfile_ = static_cast<int>(motorProfiles_.size()) - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Del") && motorProfiles_.size() > 1) {
                motorProfiles_.erase(motorProfiles_.begin() + currentMotorProfile_);
                if (currentMotorProfile_ > 0) currentMotorProfile_--;
        }

        ImGui::Separator();
        MotorProfile &mp = motorProfiles_[currentMotorProfile_];

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "--- Input Parameters ---");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputText("Model", mp.modelName, sizeof(mp.modelName));
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Rs (Ohm)", &mp.Rs, 0.0f, 0.0f, "%.4f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Ld (H)", &mp.Ld, 0.0f, 0.0f, "%.6f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Lq (H)", &mp.Lq, 0.0f, 0.0f, "%.6f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Pole Pairs", &mp.polePairs, 0, 0);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Kt (Nm/A)", &mp.Kt, 0.0f, 0.0f, "%.6f");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "--- Back-EMF Measurement ---");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Frequency (Hz)", &mp.backEmfFreq, 0.0f, 0.0f, "%.3f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Vpp (Line-to-Line) (V)", &mp.backEmfVpp, 0.0f, 0.0f, "%.3f");

        if (mp.polePairs < 1)
                mp.polePairs = 1;
        if (mp.backEmfFreq <= 0.0f)
                mp.backEmfFreq = 0.001f;

        // Calculations
        // Flux linkage Psi_m = Vpp / (2 * sqrt(3) * 2 * pi * f_e)
        // Ke (Vpeak_phase / rad/s_elec) = Psi_m
        const float pi    = 3.14159265358979323846f;
        float       psi_m = mp.backEmfVpp / (4.0f * sqrtf(3.0f) * pi * mp.backEmfFreq);

        // Kt (Nm/A) = 1.5 * P * Psi_m
        float kt = 1.5f * static_cast<float>(mp.polePairs) * psi_m;

        // Kv (RPM/V_LL_peak) = 120 * f_e / (P * Vpp)
        float kv = (120.0f * mp.backEmfFreq) / (static_cast<float>(mp.polePairs) * mp.backEmfVpp);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "--- Calculated Results ---");
        ImGui::Text("Flux Linkage (Psi_m): %.6f Wb", static_cast<double>(psi_m));
        ImGui::Text("KV value: %.2f RPM/V", static_cast<double>(kv));
        ImGui::Text("Calculated Kt (Ref): %.6f Nm/A", static_cast<double>(kt));

        ImGui::End();
}
