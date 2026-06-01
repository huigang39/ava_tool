#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h must precede commdlg.h (commdlg.h → prsht.h needs CALLBACK/HWND from windows.h)
#include <windows.h>

#include <GLFW/glfw3.h>
#include <commdlg.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <GLFW/glfw3.h>
#endif

#include "timeops.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <ranges>
#include <sstream>

#include "cJSON.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "platform/native_dlg.hpp"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/sampler.hpp"
#include "gui/gui.hpp"
#include "gui/monitor.hpp"
#include "gui/variable.hpp"
#include <cstdlib>
#include <filesystem>
#include <thread>

std::vector<std::string> Gui::sDroppedFiles_{};

void
Gui::glfwErrCb(const i32 err, const char *desc)
{
        LOG_E("Glfw Error %d: %s", err, desc);
}

void
Gui::glfwDropCb(GLFWwindow * /*window*/, const i32 count, const char **paths)
{
        for (i32 i = 0; i < count; ++i)
                sDroppedFiles_.emplace_back(paths[i]);
}

Gui::Gui(const std::string &initialPath)
{
        LOG_I("Gui(%s)", initialPath.c_str());

        glfwSetErrorCallback(glfwErrCb);
        if (!glfwInit()) {
                LOG_E("Failed to init GLFW");
                return;
        }
        LOG_I("GLFW Inited");

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
#ifdef __APPLE__
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

        window_ = glfwCreateWindow(windowWidth_, windowHeight_, windowTitle_.c_str(), nullptr, nullptr);
        if (window_ == nullptr) {
                LOG_E("Failed to create GLFW window");
                glfwTerminate();
                return;
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        glfwSetDropCallback(window_, glfwDropCb);

        glfwGetWindowContentScale(window_, &xScale_, &yScale_);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io     = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // On macOS Retina the GLFW backend already sets DisplayFramebufferScale = 2,
        // so ImGui renders in logical pixels and the backend up-scales to physical.
        // On Windows/Linux the framebuffer equals the window in pixels, so we must
        // bake the DPI scale into the font ourselves.
        // uiScale = contentScale / (fb/window), e.g. 1.0 on macOS Retina, 1.5 on Win 150%
        int winW = 1, fbW = 1;
        glfwGetWindowSize(window_, &winW, nullptr);
        glfwGetFramebufferSize(window_, &fbW, nullptr);
        const float fbRatio = (winW > 0) ? static_cast<float>(fbW) / static_cast<float>(winW) : 1.0f;
        const float uiScale = xScale_ / fbRatio;

        const float fontSize = std::round(18.0f * uiScale);
        if (!io.Fonts->AddFontFromFileTTF(fontFile_.c_str(), fontSize)) {
                ImFontConfig cfg;
                cfg.SizePixels = fontSize;
                io.Fonts->AddFontDefault(&cfg);
        }

        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(uiScale);

        static std::string iniPath = getAppDir() + "/imgui.ini";
        io.IniFilename             = iniPath.c_str();

        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init(glslVer_);

        ImPlot::CreateContext();

        if (!initialPath.empty())
                loadSession(initialPath);
        else
                loadSession();

        if (motorProfiles_.empty())
                motorProfiles_.push_back(MotorProfile{});
}

Gui::~Gui()
{
        LOG_I("~Gui()");

        // Hide window immediately to give user instant feedback
        if (window_) {
                glfwHideWindow(window_);
        }

        if (isModified_ || ImGui::GetIO().WantSaveIniSettings) {
                saveSession();
        }

        ImPlot::DestroyContext();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (window_) {
                glfwDestroyWindow(window_);
                window_ = nullptr;
        }
        glfwTerminate();
}

void
Gui::hide()
{
        if (window_) {
                glfwHideWindow(window_);
        }
}

std::string
Gui::getAppDir()
{
        const std::filesystem::path path = std::filesystem::current_path() / ".ava_tool";
        std::filesystem::create_directories(path);
        return path.string();
}

void
Gui::saveSession(const std::string &path)
{
        if (path.empty() && isFirstSave_) {
                saveSessionAs();
                return;
        }
        u64 start = get_mono_ts_ms();

        std::lock_guard lk(mtxMonitors_);
        std::string     targetPath = path.empty() ? currentSessionPath_ : path;
        cJSON          *root       = cJSON_CreateObject();

        cJSON *jlink = cJSON_CreateObject();
        cJSON_AddStringToObject(jlink, "device", JLinkPort::instance().deviceName().c_str());
        cJSON_AddNumberToObject(jlink, "speedKHz", JLinkPort::instance().speed());
        cJSON_AddNumberToObject(jlink, "hssPeriodUs", JLinkPort::instance().hssPeriodUs());
        cJSON_AddItemToObject(root, "jlink", jlink);

        cJSON_AddBoolToObject(root, "showCalculator", showCalculator_);

        cJSON *monitorsArr = cJSON_CreateArray();
        for (const auto &m : monitors_ | std::views::values) {
                cJSON *mObj = cJSON_CreateObject();
                cJSON_AddStringToObject(mObj, "name", m->getName().c_str());
                cJSON_AddStringToObject(mObj, "samplingMode", m->samplingMode_ == Monitor::SamplingMode::HSS ? "HSS" : "POLL");
                cJSON_AddNumberToObject(mObj, "maxSampleHz", m->maxSampleHz_);
                cJSON_AddNumberToObject(mObj, "historySeconds", static_cast<f64>(m->historySeconds_));
                cJSON_AddNumberToObject(mObj, "maxDisplayPoints", static_cast<f64>(m->maxDisplayPoints_));

                cJSON *scopesArr = cJSON_CreateArray();
                for (auto &s : m->getScopes() | std::views::values) {
                        cJSON *sObj = cJSON_CreateObject();
                        cJSON_AddStringToObject(sObj, "name", s->getName().c_str());
                        cJSON_AddStringToObject(sObj, "draw", s->getDraw() == MonitorScope::DrawEnum::PLOT ? "PLOT" : "TABLE");
                        cJSON_AddNumberToObject(sObj, "height", static_cast<f64>(s->getHeight()));
                        cJSON_AddBoolToObject(sObj, "showFft", s->getShowFft());
                        cJSON_AddNumberToObject(sObj, "fftPoints", static_cast<f64>(s->getFftPoints()));
                        cJSON_AddNumberToObject(sObj, "fftPeakCount", static_cast<f64>(s->getFftPeakCount()));

                        if (!s->getExpandedGroups().empty()) {
                                cJSON *egArr = cJSON_CreateArray();
                                for (const auto &p : s->getExpandedGroups())
                                        cJSON_AddItemToArray(egArr, cJSON_CreateString(p.c_str()));
                                cJSON_AddItemToObject(sObj, "expandedGroups", egArr);
                        }

                        cJSON *chsArr = cJSON_CreateArray();
                        for (auto &ch : s->getChannels() | std::views::values) {
                                cJSON *chObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(chObj, "name", ch->getName().c_str());
                                cJSON_AddStringToObject(chObj, "type", ch->getType().c_str());
                                char addrHex[32];
                                snprintf(addrHex, sizeof(addrHex), "0x%zX", ch->getAddr());
                                cJSON_AddStringToObject(chObj, "addr", addrHex);
                                cJSON_AddStringToObject(chObj, "device", ch->getDevice().c_str());
                                if (!ch->getShmRegionName().empty())
                                        cJSON_AddStringToObject(chObj, "shmRegionName", ch->getShmRegionName().c_str());

                                cJSON *colorArr = cJSON_CreateArray();
                                for (i32 i = 0; i < 4; ++i)
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
                                                cJSON_AddNumberToObject(eObj, "value", static_cast<f64>(e.value));
                                                cJSON_AddItemToArray(enumsArr, eObj);
                                        }
                                        cJSON_AddItemToObject(chObj, "enums", enumsArr);
                                }
                                cJSON_AddItemToArray(chsArr, chObj);
                        }
                        cJSON_AddItemToObject(sObj, "channels", chsArr);
                        cJSON_AddItemToArray(scopesArr, sObj);
                }
                cJSON_AddItemToObject(mObj, "scopes", scopesArr);
                cJSON_AddItemToArray(monitorsArr, mObj);
        }
        cJSON_AddItemToObject(root, "monitors", monitorsArr);

        cJSON *VariableArr = cJSON_CreateArray();
        for (const auto &p : vars_ | std::views::values) {
                cJSON *pObj = cJSON_CreateObject();
                cJSON_AddStringToObject(pObj, "name", p->getName().c_str());
                cJSON_AddStringToObject(pObj, "cfgPath", p->getCfgPath().c_str());
                cJSON_AddStringToObject(pObj, "binPath", p->getBinPath().c_str());
                cJSON_AddStringToObject(pObj, "elfPath", p->getElfPath().c_str());
                p->save(pObj);
                p->clearModified();
                cJSON_AddItemToArray(VariableArr, pObj);
        }
        cJSON_AddItemToObject(root, "Variables", VariableArr);
        for (const auto &m : monitors_ | std::views::values)
                m->clearModified();
        isModified_ = false;

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
        cJSON_AddStringToObject(root, "imguiLayout", ImGui::SaveIniSettingsToMemory());

        u64           pStart = get_mono_ts_ms();
        char         *out    = cJSON_PrintUnformatted(root);
        u64           pEnd   = get_mono_ts_ms();
        std::ofstream ofs(targetPath);
        if (ofs && out) {
                ofs << out;
                currentSessionPath_ = targetPath;
                isModified_         = false;
                isFirstSave_        = false;
        }
        size_t outLen = out ? strlen(out) : 0;
        cJSON_free(out);
        cJSON_Delete(root);

        u64 end = get_mono_ts_ms();
        LOG_I("SaveSession Profile: Total %llu ms, JSON Print %llu ms, Size %zu bytes", end - start, pEnd - pStart, outLen);
}

bool
Gui::saveSessionAs()
{
        std::string path = nativeDlgSave("Save Session As", {{"Session Files", {"ava"}}}, "session.ava");
        if (path.empty())
                return false;
        if (path.find(".ava") == std::string::npos)
                path += ".ava";
        saveSession(path);
        saveToastAlpha_ = 2.0f;
        LOG_I("Session saved As: %s", path.c_str());
        return true;
}

void
Gui::loadSession(const std::string &path)
{
        u64             start = get_mono_ts_ms();
        std::lock_guard lk(mtxMonitors_);
        std::string     targetPath = path.empty() ? currentSessionPath_ : path;
        std::ifstream   ifs(targetPath);
        if (!ifs.is_open())
                return;

        isFirstSave_        = false;
        currentSessionPath_ = targetPath;

        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string content = ss.str();
        u64               fEnd    = get_mono_ts_ms();
        if (content.empty())
                return;

        cJSON *root = cJSON_Parse(content.c_str());
        u64    pEnd = get_mono_ts_ms();
        if (!root)
                return;

        u64 monitorStart = get_mono_ts_ms();
        monitors_.clear();
        vars_.clear();

        if (const cJSON *jlink = cJSON_GetObjectItem(root, "jlink")) {
                if (const cJSON *dev = cJSON_GetObjectItem(jlink, "device"); cJSON_IsString(dev))
                        JLinkPort::instance().deviceName() = dev->valuestring;
                if (const cJSON *spd = cJSON_GetObjectItem(jlink, "speedKHz"); cJSON_IsNumber(spd))
                        JLinkPort::instance().speed() = spd->valueint;
                if (const cJSON *p = cJSON_GetObjectItem(jlink, "hssPeriodUs"); cJSON_IsNumber(p))
                        JLinkPort::instance().hssPeriodUs() = p->valueint;
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
                        monitors_[mName]  = std::make_shared<Monitor>(mName);
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

                                if (const cJSON *drawItem = cJSON_GetObjectItem(sItem, "draw"); cJSON_IsString(drawItem)) {
                                        if (std::string(drawItem->valuestring) == "TABLE")
                                                scope->setDraw(MonitorScope::DrawEnum::TABLE);
                                        else
                                                scope->setDraw(MonitorScope::DrawEnum::PLOT);
                                }
                                if (const cJSON *hItem = cJSON_GetObjectItem(sItem, "height"); cJSON_IsNumber(hItem))
                                        scope->getHeight() = static_cast<f32>(hItem->valuedouble);

                                if (const cJSON *fftShowItem = cJSON_GetObjectItem(sItem, "showFft"); cJSON_IsBool(fftShowItem))
                                        scope->getShowFft() = cJSON_IsTrue(fftShowItem);

                                if (const cJSON *fftPtsItem = cJSON_GetObjectItem(sItem, "fftPoints");
                                    cJSON_IsNumber(fftPtsItem)) {
                                        i32 pts = fftPtsItem->valueint;
                                        if (pts != scope->getFftPoints()) {
                                                scope->reinitFft(pts);
                                        }
                                }

                                if (const cJSON *fftPkItem = cJSON_GetObjectItem(sItem, "fftPeakCount");
                                    cJSON_IsNumber(fftPkItem))
                                        scope->getFftPeakCount() = fftPkItem->valueint;

                                if (const cJSON *egArr = cJSON_GetObjectItem(sItem, "expandedGroups"); cJSON_IsArray(egArr)) {
                                        for (int k = 0; k < cJSON_GetArraySize(egArr); ++k) {
                                                const cJSON *e = cJSON_GetArrayItem(egArr, k);
                                                if (cJSON_IsString(e))
                                                        scope->getExpandedGroups().insert(e->valuestring);
                                        }
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
                                        ch->historySeconds_ = monitor->historySeconds_;

                                        if (const cJSON *sn = cJSON_GetObjectItem(chItem, "symbolName"); cJSON_IsString(sn))
                                                ch->getSymbolName() = sn->valuestring;

                                        if (const cJSON *devItem = cJSON_GetObjectItem(chItem, "device");
                                            cJSON_IsString(devItem)) {
                                                std::string d = devItem->valuestring;
                                                if (d.empty())
                                                        d = "JLINK";
                                                ch->setDevice(d);
                                                LOG_I("Loaded channel '%s' with device '%s'",
                                                      ch->getSymbolName().c_str(),
                                                      ch->getDevice().c_str());
                                        } else {
                                                ch->setDevice("JLINK");
                                                LOG_I("Loaded channel '%s' with device 'JLINK' (default)",
                                                      ch->getSymbolName().c_str());
                                        }

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
                                        if (const cJSON *colArr = cJSON_GetObjectItem(chItem, "color"); cJSON_IsArray(colArr)) {
                                                i32 idx = 0;
                                                for (const cJSON *c = colArr->child; c && idx < 4; c = c->next, ++idx)
                                                        ch->getColor()[idx] = static_cast<f32>(c->valuedouble);
                                        }
                                        if (const cJSON *uac = cJSON_GetObjectItem(chItem, "useAutoColor"); cJSON_IsBool(uac))
                                                ch->useAutoColor() = cJSON_IsTrue(uac);
                                        if (const cJSON *lw = cJSON_GetObjectItem(chItem, "lineWeight"); cJSON_IsNumber(lw))
                                                ch->getLineWeight() = static_cast<f32>(lw->valuedouble);
                                        if (const cJSON *ps = cJSON_GetObjectItem(chItem, "plotStyle"); cJSON_IsNumber(ps))
                                                ch->getPlotStyle() = ps->valueint;
                                        if (const cJSON *sm = cJSON_GetObjectItem(chItem, "showMarkers"); cJSON_IsBool(sm))
                                                ch->showMarkers() = cJSON_IsTrue(sm);
                                        if (const cJSON *sh = cJSON_GetObjectItem(chItem, "show"); cJSON_IsBool(sh))
                                                ch->show() = cJSON_IsTrue(sh);

                                        if (const cJSON *enumsArr = cJSON_GetObjectItem(chItem, "enums");
                                            cJSON_IsArray(enumsArr)) {
                                                std::vector<MonitorChannel::EnumEntry> ents;
                                                for (const cJSON *eItem = enumsArr->child; eItem; eItem = eItem->next) {
                                                        const cJSON *nObj = cJSON_GetObjectItem(eItem, "name");
                                                        const cJSON *vObj = cJSON_GetObjectItem(eItem, "value");
                                                        if (cJSON_IsString(nObj) && cJSON_IsNumber(vObj))
                                                                ents.push_back(
                                                                    {nObj->valuestring, static_cast<i64>(vObj->valuedouble)});
                                                }
                                                ch->setEnums(std::move(ents));
                                        }

                                        if (const cJSON *shmRN = cJSON_GetObjectItem(chItem, "shmRegionName");
                                            cJSON_IsString(shmRN))
                                                ch->setShmRegionName(shmRN->valuestring);
                                        if (ch->getDevice() == "SHM")
                                                MonitorScope::shmInit(*ch);
                                }
                        }
                }
        }

        u64 varStart = get_mono_ts_ms();
        if (const cJSON *VarArr = cJSON_GetObjectItem(root, "Variables"); cJSON_IsArray(VarArr)) {
                for (const cJSON *pItem = VarArr->child; pItem; pItem = pItem->next) {
                        const cJSON *nItem = cJSON_GetObjectItem(pItem, "name");
                        if (!cJSON_IsString(nItem))
                                continue;
                        std::string pName = nItem->valuestring;
                        vars_[pName]      = std::make_shared<Variable>(pName);
                        Variable *v       = vars_[pName].get();

                        const cJSON      *cfgItem = cJSON_GetObjectItem(pItem, "cfgPath");
                        const cJSON      *binItem = cJSON_GetObjectItem(pItem, "binPath");
                        const cJSON      *elfItem = cJSON_GetObjectItem(pItem, "elfPath");
                        const std::string cfg     = cJSON_IsString(cfgItem) ? cfgItem->valuestring : "";
                        const std::string bin     = cJSON_IsString(binItem) ? binItem->valuestring : "";
                        const std::string elf     = cJSON_IsString(elfItem) ? elfItem->valuestring : "";

                        if (!cfg.empty())
                                v->loadCfg(cfg);
                        if (!bin.empty())
                                v->loadBin(bin);
                        if (!elf.empty())
                                v->loadElf(elf);
                        v->load(pItem);
                }
        }

        if (const cJSON *motorsArr = cJSON_GetObjectItem(root, "motorProfiles"); cJSON_IsArray(motorsArr)) {
                motorProfiles_.clear();
                for (const cJSON *mpItem = motorsArr->child; mpItem; mpItem = mpItem->next) {
                        MotorProfile mp;
                        if (const cJSON *nameItem = cJSON_GetObjectItem(mpItem, "modelName"); cJSON_IsString(nameItem))
                                snprintf(mp.modelName, sizeof(mp.modelName), "%s", nameItem->valuestring);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Rs"); cJSON_IsNumber(item))
                                mp.Rs = static_cast<f32>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Ld"); cJSON_IsNumber(item))
                                mp.Ld = static_cast<f32>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Lq"); cJSON_IsNumber(item))
                                mp.Lq = static_cast<f32>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "polePairs"); cJSON_IsNumber(item))
                                mp.polePairs = item->valueint;
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "Kt"); cJSON_IsNumber(item))
                                mp.Kt = static_cast<f32>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "backEmfFreq"); cJSON_IsNumber(item))
                                mp.backEmfFreq = static_cast<f32>(item->valuedouble);
                        if (const cJSON *item = cJSON_GetObjectItem(mpItem, "backEmfVpp"); cJSON_IsNumber(item))
                                mp.backEmfVpp = static_cast<f32>(item->valuedouble);
                        motorProfiles_.push_back(mp);
                }
        }
        u64 layoutStart = get_mono_ts_ms();
        if (const cJSON *layout = cJSON_GetObjectItem(root, "imguiLayout"); cJSON_IsString(layout)) {
                ImGui::LoadIniSettingsFromMemory(layout->valuestring);
        }

        cJSON_Delete(root);

        u64 end = get_mono_ts_ms();
        LOG_I("LoadSession Profile Detail: Parse %llu ms, Monitors %llu ms, Variables %llu ms, Layout %llu ms",
              pEnd - fEnd,
              varStart - monitorStart,
              layoutStart - varStart,
              end - layoutStart);
}

void
Gui::drawBar()
{
        if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                        if (ImGui::MenuItem("New Session")) {
                                monitors_.clear();
                                vars_.clear();
                                currentSessionPath_ = "session.ava";
                                isModified_         = false;
                                isFirstSave_        = true;
                        }
                        if (ImGui::MenuItem("Open Session...")) {
                                std::string p = nativeDlgOpen("Open Session", {{"Session Files", {"ava", "json"}}});
                                if (!p.empty())
                                        loadSession(p);
                        }
                        if (ImGui::MenuItem("Save Session", "Ctrl+S")) {
                                saveSession();
                                saveToastAlpha_ = 2.0f;
                                LOG_I("Session saved via Menu");
                        }
                        if (ImGui::MenuItem("Save Session As...")) {
                                saveSessionAs();
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Exit", "Alt+F4")) {
                                glfwSetWindowShouldClose(window_, GLFW_TRUE);
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Window")) {
                        if (ImGui::MenuItem("Add Monitor")) {
                                std::string monitorName = "Monitor_" + std::to_string(monitors_.size());
                                monitors_[monitorName]  = std::make_shared<Monitor>(monitorName);
                                isModified_             = true;
                                LOG_I("Add Monitor: %s", monitorName.c_str());
                        }
                        if (ImGui::MenuItem("Add Variable")) {
                                std::string varName = "Variable_" + std::to_string(vars_.size());
                                vars_[varName]      = std::make_shared<Variable>(varName);
                                isModified_         = true;
                                LOG_I("Add Variable Window: %s", varName.c_str());
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Tools")) {
                        ImGui::MenuItem("Joint Calculator", nullptr, &showCalculator_);
                        ImGui::Separator();
                        ImGui::MenuItem("Bode Plot", nullptr, &bode_.show_);
                        ImGui::Separator();
                        ImGui::MenuItem("Assembly Viewer", nullptr, &asmViewer_.show_);
                        ImGui::MenuItem("Registers", nullptr, &regViewer_.show_);
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Settings")) {
                        if (ImGui::BeginMenu("Sampler CPU Core")) {
#ifdef _WIN32
                                // Show elevation status
                                BOOL                     isAdmin    = FALSE;
                                PSID                     adminGroup = NULL;
                                SID_IDENTIFIER_AUTHORITY ntAuth     = SECURITY_NT_AUTHORITY;
                                if (AllocateAndInitializeSid(&ntAuth,
                                                             2,
                                                             SECURITY_BUILTIN_DOMAIN_RID,
                                                             DOMAIN_ALIAS_RID_ADMINS,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             &adminGroup)) {
                                        CheckTokenMembership(NULL, adminGroup, &isAdmin);
                                        FreeSid(adminGroup);
                                }
                                if (!isAdmin) {
                                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Not Admin (priority capped)");
                                        ImGui::Separator();
                                }
#endif
                                const int cur = g_samplerCpuCore.load();
                                const int n   = static_cast<int>(std::thread::hardware_concurrency());

                                auto applyCoreChange = [&](int newCore) {
#ifdef _WIN32
                                        if (!isAdmin) {
                                                pendingElevationCore_ = newCore;
                                                showElevationModal_   = true;
                                                return;
                                        }
#endif
                                        g_samplerCpuCore.store(newCore);
                                        g_samplerCpuRebind.store(true);
                                };

                                if (ImGui::MenuItem("Auto (highest)", nullptr, cur < 0)) {
                                        applyCoreChange(-1);
                                }
                                ImGui::Separator();
                                for (int i = 0; i < n; ++i) {
                                        char label[32];
                                        snprintf(label, sizeof(label), "Core %d", i);
                                        if (ImGui::MenuItem(label, nullptr, cur == i)) {
                                                applyCoreChange(i);
                                        }
                                }
                                ImGui::EndMenu();
                        }
                        ImGui::EndMenu();
                }

                ImGui::Separator();
                bool wasConnected = JLinkPort::instance().isConnected();
                JLinkPort::instance().drawUI();
                if (!wasConnected && JLinkPort::instance().isConnected()) {
                        Monitor::clearAll();
                }
                const bool paused = g_monitorPaused.load();
                if (paused) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)); // Red (State: Paused)
                        if (ImGui::SmallButton("RESUME"))
                                g_monitorPaused.store(false);
                        ImGui::PopStyleColor();
                } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f)); // Green (State: Running)
                        if (ImGui::SmallButton("PAUSE"))
                                g_monitorPaused.store(true);
                        ImGui::PopStyleColor();
                }

                // Total points currently in memory (across all channels)
                u64 totalPts = 0;
                {
                        std::lock_guard lk(mtxMonitors_);
                        for (const auto &m : monitors_ | std::views::values)
                                for (const auto &s : m->getScopes() | std::views::values)
                                        for (const auto &ch : s->getChannels() | std::views::values)
                                                totalPts += ch->storedCount();
                }
                char ptsBuf[32];
                if (totalPts >= 1000000)
                        snprintf(ptsBuf, sizeof(ptsBuf), "%.2f M pts", totalPts / 1000000.0);
                else if (totalPts >= 1000)
                        snprintf(ptsBuf, sizeof(ptsBuf), "%.1f k pts", totalPts / 1000.0);
                else
                        snprintf(ptsBuf, sizeof(ptsBuf), "%llu pts", totalPts);

                char fpsBuf[32];
                snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", ImGui::GetIO().Framerate);

                const f32 ptsWidthFixed = ImGui::CalcTextSize("999.99 M pts").x;
                const f32 fpsWidthFixed = ImGui::CalcTextSize("9999 FPS").x;
                const f32 spacing       = ImGui::GetStyle().ItemSpacing.x;

                const f32 totalWidth = fpsWidthFixed + ptsWidthFixed + spacing;
                const f32 startX     = ImGui::GetWindowWidth() - totalWidth - spacing * 2;

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
        LOG_I("Gui::loop start");
        while (!glfwWindowShouldClose(window_) || showQuitModal_) {
                glfwPollEvents();

                if (glfwWindowShouldClose(window_) && !showQuitModal_ && !wantsToQuit_) {
                        bool anyModified = isModified_;
                        if (!anyModified) {
                                for (const auto &v : vars_ | std::views::values) {
                                        if (v->isModified()) {
                                                anyModified = true;
                                                break;
                                        }
                                }
                        }
                        if (!anyModified) {
                                std::lock_guard lk(mtxMonitors_);
                                for (const auto &m : monitors_ | std::views::values) {
                                        if (m->isModified()) {
                                                anyModified = true;
                                                break;
                                        }
                                }
                        }
                        if (anyModified || isFirstSave_) {
                                glfwSetWindowShouldClose(window_, GLFW_FALSE);
                                showQuitModal_ = true;
                        } else {
                                break;
                        }
                }

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                drawBar();

                if (const ImGuiIO &io = ImGui::GetIO(); !io.WantTextInput) {
                        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                                g_monitorPaused.store(!g_monitorPaused.load());
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                                saveSession();
                                saveToastAlpha_ = 2.0f;
                                LOG_I("Session saved via Ctrl+S");
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
                                bool wasConnected = JLinkPort::instance().isConnected();
                                JLinkPort::instance().close();
                                if (JLinkPort::instance().open() && JLinkPort::instance().connect()) {
                                        LOG_I("J-Link connected via Ctrl+L");
                                        if (!wasConnected)
                                                Monitor::clearAll();
                                }
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                                Monitor::clearAll();
                                LOG_I("Clear data triggered via Ctrl+R");
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                                JLinkPort::instance().close();
                                LOG_I("J-Link disconnected via Ctrl+C");
                        }
                }

                if (saveToastAlpha_ > 0.0f) {
                        saveToastAlpha_ -= ImGui::GetIO().DeltaTime;
                        ImGui::SetNextWindowPos(
                            ImVec2(ImGui::GetIO().DisplaySize.x / 2.0f, ImGui::GetIO().DisplaySize.y - 50.0f),
                            ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowBgAlpha(std::min(1.0f, saveToastAlpha_) * 0.8f);
                        if (ImGui::Begin("##save_toast",
                                         nullptr,
                                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
                                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, std::min(1.0f, saveToastAlpha_)),
                                                   "Session Saved Successfully!");
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
                        for (auto it = vars_.begin(); it != vars_.end();) {
                                if (it->second->isPendingDelete()) {
                                        it          = vars_.erase(it);
                                        isModified_ = true;
                                } else {
                                        it->second->updateDisplay();
                                        if (it->second->consumeElfReloaded()) {
                                                syncSymbolAddresses(it->second.get());
                                        }
                                        ++it;
                                }
                        }
                }

                bode_.updateDisplay();
                asmViewer_.draw(this);
                regViewer_.draw();

                processPendingCsvImports();

                for (const auto &file : sDroppedFiles_) {
                        if (file.ends_with(".ava")) {
                                loadSession(file);
                        } else if (file.ends_with(".csv") || file.ends_with(".CSV")) {
                                // Create the monitor immediately (shows loading spinner)
                                std::string stem = std::filesystem::path(file).stem().string();
                                std::string monName;
                                {
                                        std::lock_guard lk(mtxMonitors_);
                                        monName   = stem;
                                        int idx   = 1;
                                        while (monitors_.count(monName))
                                                monName = stem + "_" + std::to_string(idx++);
                                        auto mon                  = std::make_shared<Monitor>(monName);
                                        mon->csvLoading_.store(true, std::memory_order_release);
                                        monitors_[monName]        = mon;
                                        isModified_               = true;
                                }
                                importCsvAsync(file, monName);
                        }
                }
                sDroppedFiles_.clear();
#ifdef _WIN32
                if (showElevationModal_) {
                        ImGui::OpenPopup("Elevate?");
                        if (ImGui::BeginPopupModal("Elevate?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("Changing the sampler core requires administrator privileges.");
                                ImGui::Text("The app will save the current session and relaunch elevated.");
                                ImGui::Separator();
                                if (ImGui::Button("Relaunch as Admin", ImVec2(160, 0))) {
                                        const int newCore = pendingElevationCore_;
                                        saveSession();
                                        LOG_I("UAC elevation: relaunching as admin (core=%d)", newCore);

                                        char exePath[MAX_PATH];
                                        GetModuleFileNameA(NULL, exePath, MAX_PATH);

                                        SHELLEXECUTEINFOA sei = {};
                                        sei.cbSize            = sizeof(sei);
                                        sei.lpVerb            = "runas";
                                        sei.lpFile            = exePath;
                                        sei.lpParameters      = currentSessionPath_.c_str();
                                        sei.nShow             = SW_SHOWNORMAL;
                                        sei.fMask             = SEE_MASK_NOASYNC;

                                        if (ShellExecuteExA(&sei)) {
                                                glfwSetWindowShouldClose(window_, GLFW_TRUE);
                                                wantsToQuit_ = true;
                                        } else {
                                                LOG_W("UAC elevation cancelled or failed");
                                        }
                                        showElevationModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                                        showElevationModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                }
#endif
                if (showQuitModal_) {
                        ImGui::OpenPopup("Quit?");
                        if (ImGui::BeginPopupModal("Quit?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("Save changes to session before quitting?");
                                ImGui::Separator();
                                if (ImGui::Button("Save", ImVec2(120, 0))) {
                                        bool saved;
                                        if (isFirstSave_) {
                                                saved = saveSessionAs();
                                        } else {
                                                saveSession();
                                                saved = true;
                                        }
                                        showQuitModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                        if (saved)
                                                wantsToQuit_ = true;
                                        // If user cancelled the native dialog, saved==false: stay in app
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
                                        wantsToQuit_   = true;
                                        showQuitModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                                        showQuitModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                }

                if (wantsToQuit_) {
                        break;
                }

                ImGui::Render();
                i32 display_w, display_h;
                glfwGetFramebufferSize(window_, &display_w, &display_h);
                glViewport(0, 0, display_w, display_h);
                glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                glfwSwapBuffers(window_);
        }
}

void
Gui::syncSymbolAddresses(Variable *reloadedVar)
{
        if (!reloadedVar)
                return;
        const auto     &searchPool = reloadedVar->searchPool_;
        std::lock_guard lk(mtxMonitors_);
        i32             count     = 0;
        i32             enumCount = 0;
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (ch->getSymbolName().empty())
                                        continue;

                                for (const auto &se : searchPool) {
                                        if (se.path == ch->getSymbolName()) {
                                                ch->setAddr(se.addr);
                                                count++;
                                                break;
                                        }
                                }
                                // Refresh enum entries from the reloaded ELF +
                                // any user overrides on the owning VarEntry.
                                if (reloadedVar->refreshChannelEnums(ch))
                                        enumCount++;
                        }
                }
        }

        for (auto &pair : vars_) {
                for (auto &v : pair.second->vars_) {
                        if (v.port != PortType::JLINK)
                                continue;

                        for (const auto &se : searchPool) {
                                if (se.path == v.name) {
                                        v.addr    = se.addr;
                                        v.typeOff = se.typeOff;
                                        count++;
                                        break;
                                }
                        }
                }
        }

        if (count > 0 || enumCount > 0) {
                LOG_I("Synced %d symbol addresses, refreshed %d channel enums with new ELF", count, enumCount);
                JLinkPort::instance().reqRestart(); // Force sampler thread to rebuild HSS list
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
        if (currentMotorProfile_ >= static_cast<i32>(motorProfiles_.size())) {
                currentMotorProfile_ = 0;
        }

        ImGui::Text("Saved Profiles:");
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##motor_profiles", motorProfiles_[currentMotorProfile_].modelName)) {
                for (i32 i = 0; i < static_cast<i32>(motorProfiles_.size()); ++i) {
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
                snprintf(mp.modelName, sizeof(mp.modelName), "Motor_%d", static_cast<i32>(motorProfiles_.size() + 1));
                motorProfiles_.push_back(mp);
                currentMotorProfile_ = static_cast<i32>(motorProfiles_.size()) - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Del") && motorProfiles_.size() > 1) {
                motorProfiles_.erase(motorProfiles_.begin() + currentMotorProfile_);
                if (currentMotorProfile_ > 0)
                        currentMotorProfile_--;
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
        const f32 pi    = 3.14159265358979323846f;
        f32       psi_m = mp.backEmfVpp / (4.0f * sqrtf(3.0f) * pi * mp.backEmfFreq);

        // Kt (Nm/A) = 1.5 * P * Psi_m
        f32 kt = 1.5f * static_cast<f32>(mp.polePairs) * psi_m;

        // Kv (RPM/V_LL_peak) = 120 * f_e / (P * Vpp)
        f32 kv = (120.0f * mp.backEmfFreq) / (static_cast<f32>(mp.polePairs) * mp.backEmfVpp);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "--- Calculated Results ---");
        ImGui::Text("Flux Linkage (Psi_m): %.6f Wb", static_cast<f64>(psi_m));
        ImGui::Text("KV value: %.2f RPM/V", static_cast<f64>(kv));
        ImGui::Text("Calculated Kt (Ref): %.6f Nm/A", static_cast<f64>(kt));

        ImGui::End();
}

/* --------------------------------------------------------------------------
 * CSV drag-and-drop import
 * -------------------------------------------------------------------------- */

void
Gui::importCsvAsync(const std::string &path, const std::string &monitorName)
{
        std::thread([this, path, monitorName]() {
                LOG_I("CSV import started: %s", path.c_str());

                std::ifstream f(path);
                if (!f.is_open()) {
                        LOG_E("CSV import: cannot open %s", path.c_str());
                        return;
                }

                // --- Parse header row ---
                std::string line;
                if (!std::getline(f, line)) {
                        LOG_E("CSV import: empty file %s", path.c_str());
                        return;
                }

                // Split header into column names
                std::vector<std::string> headers;
                {
                        std::istringstream ss(line);
                        std::string        tok;
                        while (std::getline(ss, tok, ',')) {
                                // Strip CR and surrounding whitespace
                                while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' '))
                                        tok.pop_back();
                                while (!tok.empty() && tok.front() == ' ')
                                        tok.erase(tok.begin());
                                headers.push_back(tok);
                        }
                }

                if (headers.empty()) {
                        LOG_E("CSV import: no columns in %s", path.c_str());
                        return;
                }

                // Decide whether first column is a timestamp.
                // Treat it as time if its header contains "time", "t", "ts", "timestamp", "index" (case-insensitive).
                bool firstIsTime = false;
                {
                        std::string h0 = headers[0];
                        for (auto &c : h0)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        firstIsTime =
                            (h0 == "t" || h0 == "time" || h0 == "ts" || h0 == "timestamp" || h0 == "index" || h0 == "x");
                }

                const int timeCol   = firstIsTime ? 0 : -1;
                const int dataStart = firstIsTime ? 1 : 0;

                std::vector<std::string> dataHeaders;
                for (int i = dataStart; i < static_cast<int>(headers.size()); ++i)
                        dataHeaders.push_back(headers[i]);

                const int nCols = static_cast<int>(dataHeaders.size());
                if (nCols == 0) {
                        LOG_E("CSV import: no data columns in %s", path.c_str());
                        return;
                }

                std::vector<double>             timestamps;
                std::vector<std::vector<float>> columns(nCols);

                // --- Parse data rows ---
                double rowIdx = 0.0;
                while (std::getline(f, line)) {
                        if (line.empty() || line[0] == '#')
                                continue;

                        std::vector<double> rowVals;
                        rowVals.reserve(headers.size());
                        {
                                std::istringstream ss(line);
                                std::string        tok;
                                while (std::getline(ss, tok, ',')) {
                                        while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' '))
                                                tok.pop_back();
                                        while (!tok.empty() && tok.front() == ' ')
                                                tok.erase(tok.begin());
                                        double v       = 0.0;
                                        auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
                                        rowVals.push_back((ec == std::errc{}) ? v : 0.0);
                                }
                        }

                        // Pad / trim to expected column count
                        rowVals.resize(headers.size(), 0.0);

                        const double ts = (timeCol >= 0) ? rowVals[timeCol] : rowIdx;
                        timestamps.push_back(ts);

                        for (int c = 0; c < nCols; ++c)
                                columns[c].push_back(static_cast<float>(rowVals[dataStart + c]));

                        rowIdx += 1.0;
                }

                if (timestamps.empty()) {
                        LOG_E("CSV import: no data rows in %s", path.c_str());
                        return;
                }

                LOG_I("CSV import done: %s  rows=%zu  cols=%d", path.c_str(), timestamps.size(), nCols);

                CsvImportPending result;
                result.monitorName = monitorName;
                result.headers     = std::move(dataHeaders);
                result.timestamps  = std::move(timestamps);
                result.columns     = std::move(columns);

                {
                        std::lock_guard lk(mtxCsvPending_);
                        csvPendingList_.push_back(std::move(result));
                }
        }).detach();
}

void
Gui::processPendingCsvImports()
{
        std::vector<CsvImportPending> pending;
        {
                std::lock_guard lk(mtxCsvPending_);
                if (csvPendingList_.empty())
                        return;
                pending = std::move(csvPendingList_);
                csvPendingList_.clear();
        }

        std::lock_guard lk(mtxMonitors_);
        for (auto &imp : pending) {
                // Find the monitor that was created immediately on file drop
                auto   it      = monitors_.find(imp.monitorName);
                Monitor *monitor = (it != monitors_.end()) ? it->second.get() : nullptr;
                if (!monitor) {
                        // Fallback: create it now (shouldn't normally happen)
                        auto m             = std::make_shared<Monitor>(imp.monitorName);
                        monitors_[imp.monitorName] = m;
                        monitor            = m.get();
                }
                const std::string &name = imp.monitorName;

                const std::string scopeName = "scope_0";
                monitor->addScope(scopeName);
                MonitorScope *scope = monitor->getScopes()[scopeName].get();
                if (!scope)
                        continue;

                const usize nPts = imp.timestamps.size();

                for (int c = 0; c < static_cast<int>(imp.headers.size()); ++c) {
                        const std::string &chName = imp.headers[c];
                        scope->addChannel(chName);
                        MonitorChannel *ch = scope->findChannel(chName);
                        if (!ch)
                                continue;

                        ch->historySeconds_   = 0.0f; // static data — no time-based pruning
                        ch->maxDisplayPoints_ = 5000;

                        ch->pushBatch(imp.columns[c].data(), imp.timestamps.data(), nPts);
                        ch->publishSnapshot();
                }

                // Set x-axis range to cover the full data span
                if (nPts > 0) {
                        monitor->linkXMin_      = imp.timestamps.front();
                        monitor->linkXMax_      = imp.timestamps.back();
                        monitor->dataStartTime_ = imp.timestamps.front();
                        monitor->lastNow_       = imp.timestamps.back();
                }

                // Clear loading flag — monitor will show data on the next frame
                monitor->csvLoading_.store(false, std::memory_order_release);

                LOG_I("CSV import: monitor '%s' ready: %d channels, %zu points",
                      name.c_str(),
                      static_cast<int>(imp.headers.size()),
                      nPts);
        }
}
