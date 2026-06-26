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
#include "core/mmap_vector.hpp"
#include "core/sampler.hpp"
#include "core/session_time.hpp"
#include "gui/gui.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_theme.hpp"
#include "gui/variable.hpp"
#include "platform/native_dlg.hpp"
#include "version.hpp"
#include <cstdlib>
#include <filesystem>
#include <thread>

std::vector<std::string> Gui::sDroppedFiles_{};

// ── Efficiency tab persistent state ──────────────────────────────────────────
// Shared by drawCalculator(), saveSession(), and loadSession() so the selected
// channels, units, map range, and recorded data survive save/load round-trips.
static struct {
        std::string torqueLink, speedLink, voltageLink, currentLink;
        int         refreshMs  = 200;
        int         torqueUnit = 0, speedUnit = 0, voltageUnit = 0, currentUnit = 0;
        float       spdMin = 0.f, spdMax = 6000.f, tqMin = 0.f, tqMax = 50.f;
        int         activeTab = 0;
        bool        forceTab  = false;
        struct EffPoint {
                float spdRpm, tqNm, eta;
        };
        std::vector<EffPoint> rawData; // raw recorded (speed, torque, eta) table
} s_eff;

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

        loadCacheDirSetting(); // apply the saved mmap cache dir before any buffers are created
        loadRecentList();

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
        // Build the atlas with both Latin and common Simplified-Chinese glyphs so the
        // UI can switch languages at runtime without rebuilding the font.
        if (!io.Fonts->AddFontFromFileTTF(
                fontFile_.c_str(), fontSize, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
                ImFontConfig cfg;
                cfg.SizePixels = fontSize;
                io.Fonts->AddFontDefault(&cfg);
        }

        // Restore the persisted UI language (defaults to English on first launch).
        loadLangSetting();

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

        TutorialGuide::instance().loadState(getAppDir());
}

Gui::~Gui()
{
        LOG_I("~Gui()");

        // Hide window immediately to give user instant feedback
        if (window_) {
                glfwHideWindow(window_);
        }

        if (!skipAutoSave_ && (isModified_ || ImGui::GetIO().WantSaveIniSettings)) {
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
Gui::loadRecentList()
{
        recentSessions_.clear();
        std::ifstream f(getAppDir() + "/recent.txt");
        std::string   line;
        while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                        line.pop_back();
                if (!line.empty())
                        recentSessions_.push_back(line);
        }
}

void
Gui::saveRecentList()
{
        std::ofstream f(getAppDir() + "/recent.txt", std::ios::trunc);
        for (const auto &p : recentSessions_)
                f << p << "\n";
}

void
Gui::addRecent(const std::string &path)
{
        if (path.empty())
                return;
        std::error_code ec;
        std::string     abs = std::filesystem::absolute(path, ec).string();
        if (ec)
                abs = path;

        // Move to front, de-duplicated, capped at 10 entries.
        for (auto it = recentSessions_.begin(); it != recentSessions_.end();) {
                if (*it == abs)
                        it = recentSessions_.erase(it);
                else
                        ++it;
        }
        recentSessions_.insert(recentSessions_.begin(), abs);
        if (recentSessions_.size() > 10)
                recentSessions_.resize(10);
        saveRecentList();
}

void
Gui::loadCacheDirSetting()
{
        std::ifstream f(getAppDir() + "/cache_dir.txt");
        std::string   line;
        if (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                        line.pop_back();
                if (!line.empty() && std::filesystem::exists(line))
                        mmapVectorSetCacheDir(line);
        }
}

void
Gui::setCacheDir(const std::string &dir)
{
        mmapVectorSetCacheDir(dir);
        std::ofstream f(getAppDir() + "/cache_dir.txt", std::ios::trunc);
        f << dir;
        LOG_I("Cache directory set to: %s", dir.empty() ? "(system temp)" : dir.c_str());
}

void
Gui::loadLangSetting()
{
        std::ifstream f(getAppDir() + "/lang.txt");
        std::string   line;
        if (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                        line.pop_back();
                if (line == "zh")
                        g_lang = Lang::ZH;
                else
                        g_lang = Lang::EN;
        }
}

void
Gui::setLang(Lang lang)
{
        g_lang = lang;
        std::ofstream f(getAppDir() + "/lang.txt", std::ios::trunc);
        f << (lang == Lang::ZH ? "zh" : "en");
        LOG_I("UI language set to: %s", lang == Lang::ZH ? "zh" : "en");
}

bool
Gui::shouldAutoCheckUpdate()
{
        // Skip the startup check if we already checked within the last 6 hours.
        // GitHub's unauthenticated API allows only 60 requests/hour per IP, so an
        // app that checks on every launch can trip a 403 across repeated launches.
        std::ifstream f(getAppDir() + "/update_check.txt");
        long long     last = 0;
        if (f >> last) {
                const long long now = static_cast<long long>(std::time(nullptr));
                if (now - last < 6 * 3600)
                        return false;
        }
        return true;
}

void
Gui::recordUpdateCheck()
{
        std::ofstream f(getAppDir() + "/update_check.txt", std::ios::trunc);
        f << static_cast<long long>(std::time(nullptr));
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
        cJSON_AddNumberToObject(jlink, "maxHssHz", g_maxHssHz.load(std::memory_order_relaxed));
        cJSON_AddItemToObject(root, "jlink", jlink);

        cJSON_AddBoolToObject(root, "showCalculator", showCalculator_);

        {
                cJSON *eff = cJSON_CreateObject();
                cJSON_AddNumberToObject(eff, "activeTab", s_eff.activeTab);
                cJSON_AddNumberToObject(eff, "refreshMs", s_eff.refreshMs);
                cJSON_AddStringToObject(eff, "torqueLink", s_eff.torqueLink.c_str());
                cJSON_AddStringToObject(eff, "speedLink", s_eff.speedLink.c_str());
                cJSON_AddStringToObject(eff, "voltageLink", s_eff.voltageLink.c_str());
                cJSON_AddStringToObject(eff, "currentLink", s_eff.currentLink.c_str());
                cJSON_AddNumberToObject(eff, "torqueUnit", s_eff.torqueUnit);
                cJSON_AddNumberToObject(eff, "speedUnit", s_eff.speedUnit);
                cJSON_AddNumberToObject(eff, "voltageUnit", s_eff.voltageUnit);
                cJSON_AddNumberToObject(eff, "currentUnit", s_eff.currentUnit);
                cJSON_AddNumberToObject(eff, "spdMin", static_cast<double>(s_eff.spdMin));
                cJSON_AddNumberToObject(eff, "spdMax", static_cast<double>(s_eff.spdMax));
                cJSON_AddNumberToObject(eff, "tqMin", static_cast<double>(s_eff.tqMin));
                cJSON_AddNumberToObject(eff, "tqMax", static_cast<double>(s_eff.tqMax));
                cJSON *ptArr = cJSON_CreateArray();
                for (const auto &p : s_eff.rawData) {
                        cJSON *ptObj = cJSON_CreateObject();
                        cJSON_AddNumberToObject(ptObj, "s", static_cast<double>(p.spdRpm));
                        cJSON_AddNumberToObject(ptObj, "t", static_cast<double>(p.tqNm));
                        cJSON_AddNumberToObject(ptObj, "e", static_cast<double>(p.eta));
                        cJSON_AddItemToArray(ptArr, ptObj);
                }
                cJSON_AddItemToObject(eff, "mapData", ptArr);
                cJSON_AddItemToObject(root, "efficiency", eff);
        }

        cJSON *monitorsArr = cJSON_CreateArray();
        for (const auto &m : monitors_ | std::views::values) {
                cJSON *mObj = cJSON_CreateObject();
                cJSON_AddStringToObject(mObj, "name", m->getName().c_str());
                cJSON_AddStringToObject(mObj, "title", m->getTitle().c_str());
                cJSON_AddStringToObject(mObj, "samplingMode", m->samplingMode_ == Monitor::SamplingMode::HSS ? "HSS" : "POLL");
                cJSON_AddNumberToObject(mObj, "maxSampleHz", m->maxSampleHz_);
                cJSON_AddNumberToObject(mObj, "historySeconds", static_cast<f64>(m->historySeconds_));
                cJSON_AddNumberToObject(mObj, "maxDisplayPoints", static_cast<f64>(m->maxDisplayPoints_));

                cJSON *scopesArr = cJSON_CreateArray();
                // Persist scopes in their user-defined display order so it survives a
                // save/load round-trip (the map itself is unordered).
                std::vector<MonitorScope *> orderedScopes;
                for (auto &s : m->getScopes() | std::views::values)
                        orderedScopes.push_back(s.get());
                std::sort(orderedScopes.begin(), orderedScopes.end(), [](const MonitorScope *a, const MonitorScope *b) {
                        return a->getOrder() < b->getOrder();
                });
                for (auto *s : orderedScopes) {
                        cJSON *sObj = cJSON_CreateObject();
                        cJSON_AddStringToObject(sObj, "name", s->getName().c_str());
                        if (!s->getLabel().empty() && s->getLabel() != s->getName())
                                cJSON_AddStringToObject(sObj, "label", s->getLabel().c_str());
                        cJSON_AddNumberToObject(sObj, "order", static_cast<f64>(s->getOrder()));
                        cJSON_AddStringToObject(sObj, "draw", s->getDraw() == MonitorScope::DrawEnum::PLOT ? "PLOT" : "TABLE");
                        cJSON_AddNumberToObject(sObj, "height", static_cast<f64>(s->getHeight()));
                        cJSON_AddBoolToObject(sObj, "showFft", s->getShowFft());
                        cJSON_AddBoolToObject(sObj, "fftBars", s->getFftBars());
                        cJSON_AddBoolToObject(sObj, "showSidePanel", s->getShowSidePanel());
                        cJSON_AddNumberToObject(sObj, "fftPoints", static_cast<f64>(s->getFftPoints()));
                        cJSON_AddNumberToObject(sObj, "fftPeakCount", static_cast<f64>(s->getFftPeakCount()));

                        if (!s->getExpandedGroups().empty()) {
                                cJSON *egArr = cJSON_CreateArray();
                                for (const auto &p : s->getExpandedGroups())
                                        cJSON_AddItemToArray(egArr, cJSON_CreateString(p.c_str()));
                                cJSON_AddItemToObject(sObj, "expandedGroups", egArr);
                        }

                        cJSON *chsArr = cJSON_CreateArray();
                        // Persist channels in insertion order so the order survives a
                        // save/load round-trip (the map itself is unordered).
                        std::vector<MonitorChannel *> orderedChs;
                        for (auto &ch : s->getChannels() | std::views::values)
                                orderedChs.push_back(ch.get());
                        std::sort(orderedChs.begin(), orderedChs.end(), [](const MonitorChannel *a, const MonitorChannel *b) {
                                return a->getOrder() < b->getOrder();
                        });
                        for (auto *ch : orderedChs) {
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
                                cJSON_AddNumberToObject(chObj, "gain", ch->getGain());
                                cJSON_AddNumberToObject(chObj, "bias", ch->getBias());
                                if (ch->getLabel() != ch->getName())
                                        cJSON_AddStringToObject(chObj, "alias", ch->getLabel().c_str());
                                cJSON_AddStringToObject(chObj, "symbolName", ch->getSymbolName().c_str());
                                cJSON_AddNumberToObject(chObj, "plotStyle", ch->getPlotStyle());
                                cJSON_AddBoolToObject(chObj, "showMarkers", ch->showMarkers());
                                cJSON_AddBoolToObject(chObj, "show", ch->show());
                                cJSON_AddBoolToObject(chObj, "writable", ch->isWritable());

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

        // Store file paths relative to the .ava file so the session stays portable
        // when the project folder is moved. Cross-drive paths fall back to absolute.
        const std::filesystem::path baseDir = std::filesystem::path(targetPath).parent_path();
        auto                        toRel   = [&](const std::string &p) -> std::string {
                if (p.empty() || baseDir.empty())
                        return p;
                std::error_code ec;
                auto            rel = std::filesystem::relative(p, baseDir, ec);
                if (ec || rel.empty())
                        return p;
                return rel.generic_string();
        };

        cJSON *VariableArr = cJSON_CreateArray();
        for (const auto &p : vars_ | std::views::values) {
                cJSON *pObj = cJSON_CreateObject();
                cJSON_AddStringToObject(pObj, "name", p->getName().c_str());
                cJSON_AddStringToObject(pObj, "title", p->getTitle().c_str());
                cJSON_AddStringToObject(pObj, "cfgPath", toRel(p->getCfgPath()).c_str());
                cJSON_AddStringToObject(pObj, "binPath", toRel(p->getBinPath()).c_str());
                cJSON_AddStringToObject(pObj, "elfPath", toRel(p->getElfPath()).c_str());
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
        seqEditor_.saveSession(root);

        // Tool page visibility
        cJSON_AddBoolToObject(root, "showBode", bode_.show_);
        cJSON_AddBoolToObject(root, "showAudioFft", audioFft_.show_);
        cJSON_AddBoolToObject(root, "showAsmViewer", asmViewer_.show_);
        cJSON_AddBoolToObject(root, "showSeqEditor", seqEditor_.isOpen());

        // SDK panels
        const std::string sdkBaseDir = std::filesystem::path(targetPath).parent_path().string();
        cJSON_AddNumberToObject(root, "nextSdkWinId", nextSdkWinId_);
        cJSON *sdkArr = cJSON_CreateArray();
        for (const auto &sp : sdkPanels_) {
                cJSON *spObj = cJSON_CreateObject();
                sp->save(spObj, sdkBaseDir);
                cJSON_AddItemToArray(sdkArr, spObj);
        }
        cJSON_AddItemToObject(root, "sdkPanels", sdkArr);

        u64                   pStart = get_mono_ts_ms();
        char                 *out    = cJSON_Print(root); // formatted/pretty-printed for human readability
        u64                   pEnd   = get_mono_ts_ms();
        std::filesystem::path p(reinterpret_cast<const char8_t *>(targetPath.c_str()));
        std::ofstream         ofs(p);
        if (ofs && out) {
                ofs << out;
                currentSessionPath_ = targetPath;
                isModified_         = false;
                isFirstSave_        = false;
                addRecent(targetPath);
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
        u64                   start = get_mono_ts_ms();
        std::lock_guard       lk(mtxMonitors_);
        std::string           targetPath = path.empty() ? currentSessionPath_ : path;
        std::filesystem::path p(reinterpret_cast<const char8_t *>(targetPath.c_str()));
        std::ifstream         ifs(p);
        if (!ifs.is_open())
                return;

        isFirstSave_        = false;
        currentSessionPath_ = targetPath;
        addRecent(targetPath);

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
                        JLinkPort::instance().setDeviceName(dev->valuestring);
                if (const cJSON *spd = cJSON_GetObjectItem(jlink, "speedKHz"); cJSON_IsNumber(spd))
                        JLinkPort::instance().setSpeed(spd->valueint);
                if (const cJSON *p = cJSON_GetObjectItem(jlink, "hssPeriodUs"); cJSON_IsNumber(p))
                        JLinkPort::instance().hssPeriodUs() = p->valueint;
                if (const cJSON *hz = cJSON_GetObjectItem(jlink, "maxHssHz"); cJSON_IsNumber(hz))
                        g_maxHssHz.store(hz->valueint, std::memory_order_relaxed);
        }

        if (const cJSON *sc = cJSON_GetObjectItem(root, "showCalculator")) {
                showCalculator_ = cJSON_IsTrue(sc);
        }

        if (const cJSON *eff = cJSON_GetObjectItem(root, "efficiency"); cJSON_IsObject(eff)) {
                auto getStr = [&](const char *k, std::string &out) {
                        if (const cJSON *v = cJSON_GetObjectItem(eff, k); cJSON_IsString(v))
                                out = v->valuestring;
                };
                auto getInt = [&](const char *k, int &out) {
                        if (const cJSON *v = cJSON_GetObjectItem(eff, k); cJSON_IsNumber(v))
                                out = v->valueint;
                };
                auto getFlt = [&](const char *k, float &out) {
                        if (const cJSON *v = cJSON_GetObjectItem(eff, k); cJSON_IsNumber(v))
                                out = static_cast<float>(v->valuedouble);
                };
                getStr("torqueLink", s_eff.torqueLink);
                getStr("speedLink", s_eff.speedLink);
                getStr("voltageLink", s_eff.voltageLink);
                getStr("currentLink", s_eff.currentLink);
                getInt("refreshMs", s_eff.refreshMs);
                getInt("torqueUnit", s_eff.torqueUnit);
                getInt("speedUnit", s_eff.speedUnit);
                getInt("voltageUnit", s_eff.voltageUnit);
                getInt("currentUnit", s_eff.currentUnit);
                getFlt("spdMin", s_eff.spdMin);
                getFlt("spdMax", s_eff.spdMax);
                getFlt("tqMin", s_eff.tqMin);
                getFlt("tqMax", s_eff.tqMax);
                getInt("activeTab", s_eff.activeTab);
                s_eff.forceTab = true;
                s_eff.rawData.clear();
                if (const cJSON *pts = cJSON_GetObjectItem(eff, "mapData"); cJSON_IsArray(pts)) {
                        for (const cJSON *pt = pts->child; pt; pt = pt->next) {
                                const cJSON *s = cJSON_GetObjectItem(pt, "s");
                                const cJSON *t = cJSON_GetObjectItem(pt, "t");
                                const cJSON *e = cJSON_GetObjectItem(pt, "e");
                                if (cJSON_IsNumber(s) && cJSON_IsNumber(t) && cJSON_IsNumber(e))
                                        s_eff.rawData.push_back({static_cast<float>(s->valuedouble),
                                                                 static_cast<float>(t->valuedouble),
                                                                 static_cast<float>(e->valuedouble)});
                        }
                }
        }

        if (const cJSON *monitorsArr = cJSON_GetObjectItem(root, "monitors"); cJSON_IsArray(monitorsArr)) {
                for (const cJSON *mItem = monitorsArr->child; mItem; mItem = mItem->next) {
                        const cJSON *nameItem = cJSON_GetObjectItem(mItem, "name");
                        if (!cJSON_IsString(nameItem))
                                continue;
                        std::string mName = nameItem->valuestring;
                        monitors_[mName]  = std::make_shared<Monitor>(mName);
                        Monitor *monitor  = monitors_[mName].get();

                        if (const cJSON *titleItem = cJSON_GetObjectItem(mItem, "title");
                            cJSON_IsString(titleItem) && titleItem->valuestring[0] != '\0') {
                                monitor->setTitle(titleItem->valuestring);
                                monitor->clearModified(); // loading is not a user edit
                        }

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

                                // Restore the saved display order (older sessions without it
                                // keep the default insertion order from addScope).
                                if (const cJSON *orderItem = cJSON_GetObjectItem(sItem, "order"); cJSON_IsNumber(orderItem)) {
                                        const i64 ord = static_cast<i64>(orderItem->valuedouble);
                                        scope->setOrder(ord);
                                        monitor->noteScopeOrder(ord);
                                }

                                // Restore the saved label alias.
                                if (const cJSON *labelItem = cJSON_GetObjectItem(sItem, "label");
                                    cJSON_IsString(labelItem) && labelItem->valuestring[0] != '\0') {
                                        scope->setLabel(labelItem->valuestring);
                                }

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

                                if (const cJSON *fftBarsItem = cJSON_GetObjectItem(sItem, "fftBars"); cJSON_IsBool(fftBarsItem))
                                        scope->getFftBars() = cJSON_IsTrue(fftBarsItem);

                                if (const cJSON *sidePanelItem = cJSON_GetObjectItem(sItem, "showSidePanel");
                                    cJSON_IsBool(sidePanelItem))
                                        scope->getShowSidePanel() = cJSON_IsTrue(sidePanelItem);

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
                                        if (const cJSON *gn = cJSON_GetObjectItem(chItem, "gain"); cJSON_IsNumber(gn))
                                                ch->getGain() = static_cast<f32>(gn->valuedouble);
                                        if (const cJSON *bs = cJSON_GetObjectItem(chItem, "bias"); cJSON_IsNumber(bs))
                                                ch->getBias() = static_cast<f32>(bs->valuedouble);
                                        if (const cJSON *al = cJSON_GetObjectItem(chItem, "alias"); cJSON_IsString(al))
                                                ch->setLabel(al->valuestring);
                                        if (const cJSON *ps = cJSON_GetObjectItem(chItem, "plotStyle"); cJSON_IsNumber(ps))
                                                ch->getPlotStyle() = ps->valueint;
                                        if (const cJSON *sm = cJSON_GetObjectItem(chItem, "showMarkers"); cJSON_IsBool(sm))
                                                ch->showMarkers() = cJSON_IsTrue(sm);
                                        if (const cJSON *sh = cJSON_GetObjectItem(chItem, "show"); cJSON_IsBool(sh))
                                                ch->show() = cJSON_IsTrue(sh);
                                        if (const cJSON *wrt = cJSON_GetObjectItem(chItem, "writable"); cJSON_IsBool(wrt))
                                                ch->setWritable(cJSON_IsTrue(wrt));

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

        // Resolve file paths that were stored relative to the .ava file back to
        // absolute paths. Already-absolute paths (old sessions) are used as-is.
        const std::filesystem::path baseDir = std::filesystem::path(targetPath).parent_path();
        auto                        toAbs   = [&](const std::string &p) -> std::string {
                if (p.empty() || baseDir.empty())
                        return p;
                std::filesystem::path fp(p);
                if (fp.is_absolute())
                        return p;
                return (baseDir / fp).lexically_normal().string();
        };

        u64 varStart = get_mono_ts_ms();
        if (const cJSON *VarArr = cJSON_GetObjectItem(root, "Variables"); cJSON_IsArray(VarArr)) {
                for (const cJSON *pItem = VarArr->child; pItem; pItem = pItem->next) {
                        const cJSON *nItem = cJSON_GetObjectItem(pItem, "name");
                        if (!cJSON_IsString(nItem))
                                continue;
                        std::string pName = nItem->valuestring;
                        vars_[pName]      = std::make_shared<Variable>(pName);
                        Variable *v       = vars_[pName].get();

                        if (const cJSON *titleItem = cJSON_GetObjectItem(pItem, "title");
                            cJSON_IsString(titleItem) && titleItem->valuestring[0] != '\0') {
                                v->setTitle(titleItem->valuestring);
                                v->clearModified(); // loading is not a user edit
                        }

                        const cJSON      *cfgItem = cJSON_GetObjectItem(pItem, "cfgPath");
                        const cJSON      *binItem = cJSON_GetObjectItem(pItem, "binPath");
                        const cJSON      *elfItem = cJSON_GetObjectItem(pItem, "elfPath");
                        const std::string cfg     = cJSON_IsString(cfgItem) ? toAbs(cfgItem->valuestring) : "";
                        const std::string bin     = cJSON_IsString(binItem) ? toAbs(binItem->valuestring) : "";
                        const std::string elf     = cJSON_IsString(elfItem) ? toAbs(elfItem->valuestring) : "";

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

        seqEditor_.loadSession(root);

        // Tool page visibility
        if (const cJSON *v = cJSON_GetObjectItem(root, "showBode"); cJSON_IsBool(v))
                bode_.show_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(root, "showAudioFft"); cJSON_IsBool(v))
                audioFft_.show_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(root, "showAsmViewer"); cJSON_IsBool(v))
                asmViewer_.show_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(root, "showSeqEditor"); cJSON_IsBool(v))
                seqEditor_.setOpen(cJSON_IsTrue(v));

        // SDK panels
        sdkPanels_.clear();
        const std::string sdkBaseDir = std::filesystem::path(targetPath).parent_path().string();
        if (const cJSON *sdkArr = cJSON_GetObjectItem(root, "sdkPanels"); cJSON_IsArray(sdkArr)) {
                for (const cJSON *spObj = sdkArr->child; spObj; spObj = spObj->next) {
                        newSdkPanel();
                        sdkPanels_.back()->load(spObj, sdkBaseDir);
                }
        }
        // nextSdkWinId_ must be > any restored winId so new panels don't collide.
        if (const cJSON *n = cJSON_GetObjectItem(root, "nextSdkWinId"); cJSON_IsNumber(n))
                nextSdkWinId_ = std::max(nextSdkWinId_, n->valueint);

        for (auto &pair : monitors_)
                pair.second->clearModified();

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
                // A recent session chosen from the submenu is loaded after the menu is
                // built (loadSession mutates recentSessions_, so we can't load mid-iteration).
                std::string sessionToOpen;
                if (ImGui::BeginMenu(tr("File", "文件"))) {
                        if (ImGui::MenuItem(tr("New Session", "新建会话"))) {
                                monitors_.clear();
                                vars_.clear();
                                currentSessionPath_ = "session.ava";
                                isModified_         = false;
                                isFirstSave_        = true;
                        }
                        if (ImGui::MenuItem(tr("Open Session...", "打开会话..."))) {
                                std::string p = nativeDlgOpen("Open Session", {{"Session Files", {"ava", "json"}}});
                                if (!p.empty())
                                        loadSession(p);
                        }
                        if (ImGui::BeginMenu(tr("Recent Sessions", "最近会话"), !recentSessions_.empty())) {
                                for (size_t i = 0; i < recentSessions_.size(); ++i) {
                                        const std::string &p      = recentSessions_[i];
                                        const bool         exists = std::filesystem::exists(p);
                                        std::string        label =
                                            std::filesystem::path(p).filename().string() + "##recent" + std::to_string(i);
                                        if (ImGui::MenuItem(label.c_str(), nullptr, false, exists))
                                                sessionToOpen = p;
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s%s", p.c_str(), exists ? "" : "  (missing)");
                                }
                                ImGui::Separator();
                                if (ImGui::MenuItem(tr("Clear Recent", "清空最近列表"))) {
                                        recentSessions_.clear();
                                        saveRecentList();
                                }
                                ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem(tr("Save Session", "保存会话"), "Ctrl+S")) {
                                saveSession();
                                saveToastAlpha_ = 2.0f;
                                LOG_I("Session saved via Menu");
                        }
                        if (ImGui::MenuItem(tr("Save Session As...", "会话另存为..."))) {
                                saveSessionAs();
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem(tr("Exit", "退出"), "Alt+F4")) {
                                glfwSetWindowShouldClose(window_, GLFW_TRUE);
                        }
                        ImGui::EndMenu();
                }
                if (!sessionToOpen.empty())
                        loadSession(sessionToOpen);

                // mark() right after BeginMenu so the highlight tracks the (closed) menu
                // header — guiding the user to open it, not just appearing once it's open.
                const bool windowMenuOpen = ImGui::BeginMenu(tr("Window", "窗口"));
                TutorialGuide::instance().mark("menu_window");
                if (windowMenuOpen) {
                        if (ImGui::MenuItem(tr("Add Variable Monitor", "添加变量监视器"))) {
                                std::string monitorName;
                                size_t      idx = monitors_.size();
                                do {
                                        monitorName = "变量监视器_" + std::to_string(idx++);
                                } while (monitors_.count(monitorName));
                                monitors_[monitorName] = std::make_shared<Monitor>(monitorName);
                                monitors_[monitorName]->setTitle("变量监视器 [" + std::to_string(idx - 1) + "]");
                                isModified_ = true;
                                LOG_I("Add Variable Monitor: %s", monitorName.c_str());
                        }
                        if (ImGui::MenuItem(tr("Add Variable Manager", "添加变量管理器"))) {
                                std::string varName;
                                size_t      idx = vars_.size();
                                do {
                                        varName = "变量管理器_" + std::to_string(idx++);
                                } while (vars_.count(varName));
                                vars_[varName] = std::make_shared<Variable>(varName);
                                vars_[varName]->setTitle("变量管理器 [" + std::to_string(idx - 1) + "]");
                                isModified_ = true;
                                LOG_I("Add Variable Manager Window: %s", varName.c_str());
                        }
                        if (ImGui::MenuItem(tr("Add SDK Caller", "添加 SDK 调用器")))
                                newSdkPanel();
                        ImGui::EndMenu();
                }

                const bool toolsMenuOpen = ImGui::BeginMenu(tr("Tools", "工具"));
                TutorialGuide::instance().mark("menu_tools");
                if (toolsMenuOpen) {
                        ImGui::MenuItem(tr("Joint Calculator", "电机参数计算器"), nullptr, &showCalculator_);
                        ImGui::MenuItem(tr("Bode Plot", "伯德图"), nullptr, &bode_.show_);
                        ImGui::MenuItem(tr("Audio FFT", "音频FFT"), nullptr, &audioFft_.show_);
                        ImGui::MenuItem(tr("Assembly Viewer", "汇编查看器"), nullptr, &asmViewer_.show_);
                        bool seqOpen = seqEditor_.isOpen();
                        if (ImGui::MenuItem(tr("Sequence Editor", "序列编辑器"), nullptr, &seqOpen)) {
                                seqEditor_.setOpen(seqOpen);
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(tr("Settings", "设置"))) {
                        if (ImGui::BeginMenu(tr("Sampler Run Mode", "采样器运行模式"))) {
                                const int cur       = g_samplerRunMode.load();
                                auto      applyMode = [&](int m) {
                                        g_samplerRunMode.store(m);
                                        g_samplerRebind.store(true);
                                };
                                if (ImGui::MenuItem(tr("Low CPU Usage", "低占用模式"), nullptr, cur == 0))
                                        applyMode(0);
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip(
                                            "%s", tr("Sleep 1ms per iter, normal priority", "每次迭代睡眠 1ms，普通优先级"));
                                if (ImGui::MenuItem(tr("Normal", "普通模式"), nullptr, cur == 1))
                                        applyMode(1);
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip(
                                            "%s", tr("Adaptive sleep, slightly elevated priority", "自适应睡眠，稍高优先级"));
                                if (ImGui::MenuItem(tr("CPU-Bound (High Perf)", "CPU 绑定高占用模式"), nullptr, cur == 2))
                                        applyMode(2);
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s",
                                                          tr("Spin-loop, highest priority, core pinned",
                                                             "自旋循环，最高优先级，绑定 CPU 核心"));

                                // Core selection — only meaningful in CPU-Bound mode.
                                // Pinning our own thread needs no admin rights.
                                ImGui::Separator();
                                const unsigned hc       = std::thread::hardware_concurrency();
                                const int      nCores   = hc > 0 ? static_cast<int>(hc) : 1;
                                const int      reqCore  = g_samplerCoreReq.load();
                                const bool     cpuBound = (cur == 2);
                                if (ImGui::BeginMenu(tr("Bind to Core", "绑定 CPU 核心"), cpuBound)) {
                                        auto pickCore = [&](int c) {
                                                g_samplerCoreReq.store(c);
                                                g_samplerRebind.store(true);
                                        };
                                        if (ImGui::MenuItem(
                                                tr("Auto (highest core)", "自动（最高核心）"), nullptr, reqCore < 0))
                                                pickCore(-1);
                                        ImGui::Separator();
                                        char lbl[32];
                                        for (int c = 0; c < nCores; ++c) {
                                                snprintf(lbl, sizeof(lbl), tr("Core %d", "核心 %d"), c);
                                                if (ImGui::MenuItem(lbl, nullptr, reqCore == c))
                                                        pickCore(c);
                                        }
                                        ImGui::EndMenu();
                                }
                                if (!cpuBound && ImGui::IsItemHovered())
                                        ImGui::SetTooltip(
                                            "%s",
                                            tr("Switch to CPU-Bound mode to pin a core", "切换到 CPU 绑定模式后才能指定核心"));
                                ImGui::EndMenu();
                        }

                        if (ImGui::BeginMenu(tr("Disk Cache Folder", "磁盘缓存目录"))) {
                                const std::string cur = mmapVectorCacheDir();
                                ImGui::TextDisabled("%s", tr("Current:", "当前："));
                                ImGui::TextWrapped("%s", cur.empty() ? tr("(system temp)", "（系统临时目录）") : cur.c_str());
                                ImGui::Separator();
                                if (ImGui::MenuItem(tr("Change...", "更改..."))) {
                                        std::string d = nativeDlgPickDir("Select disk-cache folder");
                                        if (!d.empty())
                                                setCacheDir(d);
                                }
                                if (ImGui::MenuItem(
                                        tr("Reset to system temp", "重置为系统临时目录"), nullptr, false, !cur.empty()))
                                        setCacheDir("");
                                ImGui::Separator();
                                ImGui::TextDisabled("%s",
                                                    tr("Applies to newly created buffers.\nRestart to move all cache here.",
                                                       "应用于新创建的缓冲区。\n重启后所有缓存迁移至此。"));
                                ImGui::EndMenu();
                        }

                        if (ImGui::BeginMenu(tr("Language", "语言"))) {
                                if (ImGui::MenuItem("English", nullptr, g_lang == Lang::EN))
                                        setLang(Lang::EN);
                                if (ImGui::MenuItem("中文", nullptr, g_lang == Lang::ZH))
                                        setLang(Lang::ZH);
                                ImGui::EndMenu();
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(tr("Help", "帮助"))) {
                        ImGui::MenuItem("Version " AVA_VERSION, nullptr, false, false);
                        ImGui::Separator();
                        const bool checking = updater_.isChecking();
                        if (ImGui::MenuItem(tr("Check for Updates...", "检查更新..."), nullptr, false, !checking)) {
                                updateManualCheck_   = true;
                                updatePendingResult_ = true;
                                recordUpdateCheck();
                                updater_.checkAsync();
                        }
                        if (checking)
                                ImGui::TextDisabled("%s", tr("  checking...", "  检查中..."));
                        ImGui::Separator();
                        if (ImGui::MenuItem(tr("Show Tutorial", "显示新手引导"))) {
                                TutorialGuide::instance().start();
                        }
                        ImGui::EndMenu();
                }

                ImGui::Separator();
                bool wasConnected = JLinkPort::instance().isConnected();
                JLinkPort::instance().drawUI();
                if (!wasConnected && JLinkPort::instance().isConnected()) {
                        Monitor::clearAll();
                }
                // Resume = go (green); Pause = caution (amber).
                const bool paused = g_monitorPaused.load();
                if (paused) {
                        if (ui::SmallButton(tr("RESUME", "继续"), ui::BtnStyle::Success))
                                g_monitorPaused.store(false);
                        TutorialGuide::instance().mark("pause_btn");
                } else {
                        if (ui::SmallButton(tr("PAUSE", "暂停"), ui::BtnStyle::Warning))
                                g_monitorPaused.store(true);
                        TutorialGuide::instance().mark("pause_btn");
                }

                // Pause all J-Link acquisition (separate from the display-only pause above).
                ImGui::SameLine();
                const bool jlinkPaused = g_jlinkSamplingPaused.load();
                if (jlinkPaused) {
                        if (ui::SmallButton(tr("RESUME J-Link", "恢复 J-Link 采样"), ui::BtnStyle::Success))
                                g_jlinkSamplingPaused.store(false);
                } else {
                        if (ui::SmallButton(tr("PAUSE J-Link", "暂停 J-Link 采样"), ui::BtnStyle::Warning))
                                g_jlinkSamplingPaused.store(true);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Pause/resume all J-Link sampling (acquisition stops; display pause is separate)",
                                             "暂停/恢复所有 J-Link 采样（停止采集；与显示暂停相互独立）"));

                // HSS global sample rate — shared by all monitors in HSS mode.
                ImGui::SameLine();
                {
                        int hssHz = g_maxHssHz.load(std::memory_order_relaxed);
                        ImGui::SetNextItemWidth(100);
                        if (ImGui::SliderInt("##HssHz", &hssHz, 1, 50000, "%d Hz", ImGuiSliderFlags_Logarithmic))
                                g_maxHssHz.store(hssHz, std::memory_order_relaxed);
                        if (ImGui::IsItemDeactivatedAfterEdit())
                                JLinkPort::instance().reqRestart();
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s",
                                                  tr("HSS sample rate (Hz) — applies to all J-Link monitors in HSS mode",
                                                     "HSS 采样率 (Hz) — 对所有处于 HSS 模式的监视器生效"));
                }
                ImGui::SameLine();
                {
                        static f32 s_smoothHssHz = 0.0f;
                        const f32  rawHz         = Monitor::getGlobalHssHz();
                        if (rawHz > 0.1f)
                                s_smoothHssHz = s_smoothHssHz * 0.85f + rawHz * 0.15f;
                        else
                                s_smoothHssHz = 0.0f;
                        if (s_smoothHssHz > 0.1f)
                                ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%.0f Hz", s_smoothHssHz);
                        else
                                ImGui::TextDisabled("-- Hz");
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

                // Download progress indicator (shown between normal content and pts/fps)
                char       dlBuf[48] = {0};
                f32        dlWidth   = 0.0f;
                const auto dlState   = updater_.getDownloadState();
                if (dlState == Updater::DownloadState::Downloading) {
                        const int pct = updater_.getDownloadProgress();
                        snprintf(dlBuf, sizeof(dlBuf), tr("Updating: %d%%", "更新中：%d%%"), pct);
                        dlWidth = ImGui::CalcTextSize(dlBuf).x;
                }

                const f32 ptsWidthFixed = ImGui::CalcTextSize("999.99 M pts").x;
                const f32 fpsWidthFixed = ImGui::CalcTextSize("9999 FPS").x;
                const f32 spacing       = ImGui::GetStyle().ItemSpacing.x;

                const f32 totalWidth = fpsWidthFixed + ptsWidthFixed + spacing + (dlWidth > 0 ? dlWidth + spacing : 0);
                const f32 startX     = ImGui::GetWindowWidth() - totalWidth - spacing * 2;

                // Download progress (Cyan, pulsing)
                if (dlWidth > 0) {
                        ImGui::SetCursorPosX(startX);
                        const float t     = (float)ImGui::GetTime();
                        const float alpha = 0.6f + 0.4f * sinf(t * 3.0f);
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, alpha), "%s", dlBuf);
                        ImGui::SameLine();
                }

                const f32 ptsStartX = startX + (dlWidth > 0 ? dlWidth + spacing : 0);

                // PTS (Green, Right-aligned in its block)
                ImGui::SetCursorPosX(ptsStartX + ptsWidthFixed - ImGui::CalcTextSize(ptsBuf).x);
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", ptsBuf);

                // FPS (Red, Right-aligned in its block)
                ImGui::SameLine();
                ImGui::SetCursorPosX(ptsStartX + ptsWidthFixed + spacing + fpsWidthFixed - ImGui::CalcTextSize(fpsBuf).x);
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
                        if (!anyModified) {
                                if (seqEditor_.isModified()) {
                                        anyModified = true;
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
                                JLinkPort::instance().connectAsync(); // off the GUI thread
                                LOG_I("J-Link connect requested via Ctrl+L");
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                                Monitor::clearAll();
                                LOG_I("Clear data triggered via Ctrl+R");
                        }

                        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                                JLinkPort::instance().disconnectAsync();
                                LOG_I("J-Link disconnect requested via Ctrl+C");
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
                                                   "%s",
                                                   tr("Session Saved Successfully!", "会话保存成功！"));
                        }
                        ImGui::End();
                }

                ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

                // Kick off a one-time update check shortly after launch (throttled to
                // once every 6h so repeated launches don't trip GitHub's rate limit).
                if (!updateCheckStarted_) {
                        updateCheckStarted_ = true;
                        if (shouldAutoCheckUpdate()) {
                                updatePendingResult_ = true;
                                recordUpdateCheck();
                                updater_.checkAsync();
                        }
                }
                drawUpdateUI();

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
                                        if (it->second->consumePropertiesChanged()) {
                                                syncVariableProperties(it->second.get());
                                        }
                                        ++it;
                                }
                        }
                }

                // Feed LOCAL variable data into matching monitor scope channels.
                // JLINK/SHM channels are sampled by the dedicated sampler thread, but
                // LOCAL data lives only in each Variable's localBufs_ (written by the
                // SDK/sequence panels). Resample it here each frame and push to any
                // scope channel named "<var>" / "<var>.<field>" (device == LOCAL).
                {
                        std::unordered_map<std::string, float> localVals;
                        for (auto &vw : vars_ | std::views::values)
                                for (auto &[chName, val] : vw->collectLocalChannelValues())
                                        localVals[chName] = val;

                        if (!localVals.empty()) {
                                const double    ts = sessionTimeSec();
                                std::lock_guard lk(mtxMonitors_);
                                for (auto &m : monitors_ | std::views::values)
                                        for (auto &s : m->getScopes() | std::views::values)
                                                for (auto &[cn, ch] : s->getChannels()) {
                                                        if (ch->getDevice() != "LOCAL")
                                                                continue;
                                                        const std::string &key =
                                                            ch->getSymbolName().empty() ? cn : ch->getSymbolName();
                                                        auto it = localVals.find(key);
                                                        if (it == localVals.end())
                                                                continue;
                                                        float v = it->second;
                                                        ch->pushBatch(&v, &ts, 1);
                                                        ch->publishSnapshot();
                                                }
                        }
                }

                // Mirror symbols dropped from a symbol browser onto a monitor back into the
                // originating Variable's watch list (requests queued by MonitorScope::dropTarget).
                if (auto &mirrorQ = watchMirrorQueue(); !mirrorQ.empty()) {
                        for (const auto &req : mirrorQ)
                                for (auto &v : vars_ | std::views::values)
                                        if (static_cast<const void *>(v.get()) == req.target) {
                                                v->mirrorFromMonitorDrop(req);
                                                break;
                                        }
                        mirrorQ.clear();
                }

                bode_.updateDisplay();
                audioFft_.draw();
                asmViewer_.draw(this);
                seqEditor_.draw();

                // Draw SDK windows; prune closed ones.
                for (auto &sp : sdkPanels_)
                        sp->draw();
                sdkPanels_.erase(std::remove_if(sdkPanels_.begin(),
                                                sdkPanels_.end(),
                                                [](const std::shared_ptr<SdkPanel> &p) { return !p->open_; }),
                                 sdkPanels_.end());

                TutorialGuide::instance().draw();

                processPendingCsvImports();

                for (const auto &file : sDroppedFiles_) {
                        if (file.ends_with(".ava")) {
                                loadSession(file);
                        } else if (file.ends_with(".dll") || file.ends_with(".h") || file.ends_with(".hpp")) {
                                for (auto &sp : sdkPanels_)
                                        sp->pushDroppedFiles({file});
                        } else if (file.ends_with(".csv") || file.ends_with(".CSV")) {
                                // Create the monitor immediately (shows loading spinner)
                                std::string stem = std::filesystem::path(file).stem().string();
                                std::string monName;
                                {
                                        std::lock_guard lk(mtxMonitors_);
                                        monName = stem;
                                        int idx = 1;
                                        while (monitors_.count(monName))
                                                monName = stem + "_" + std::to_string(idx++);
                                        auto mon = std::make_shared<Monitor>(monName);
                                        mon->csvLoading_.store(true, std::memory_order_release);
                                        monitors_[monName] = mon;
                                        isModified_        = true;
                                }
                                importCsvAsync(file, monName);
                        }
                }
                sDroppedFiles_.clear();
                if (showQuitModal_) {
                        ImGui::OpenPopup("###Quit");
                        if (ImGui::BeginPopupModal(
                                tr("Quit?###Quit", "退出？###Quit"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("%s", tr("Save changes to session before quitting?", "退出前保存会话更改吗？"));
                                ImGui::Separator();
                                if (ui::Button(tr("Save", "保存"), ui::BtnStyle::Success, ImVec2(120, 0))) {
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
                                if (ui::Button(tr("Don't Save", "不保存"), ui::BtnStyle::Danger, ImVec2(120, 0))) {
                                        skipAutoSave_  = true;
                                        wantsToQuit_   = true;
                                        showQuitModal_ = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(120, 0))) {
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
        std::lock_guard lk(mtxMonitors_);

        // Build a symbol map from EVERY loaded ELF (not just the reloaded one) so a
        // symbol provided by another open ELF isn't falsely flagged as missing.
        std::unordered_map<std::string, std::pair<u64, u64>> symMap; // name -> {addr, typeOff}
        for (auto &vp : vars_)
                for (const auto &se : vp.second->searchPool_)
                        symMap.try_emplace(se.path, se.addr, se.typeOff);

        i32 count     = 0;
        i32 unknown   = 0;
        i32 enumCount = 0;
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (ch->getSymbolName().empty())
                                        continue; // manual / CSV channel — no symbol to resolve

                                auto it = symMap.find(ch->getSymbolName());
                                if (it != symMap.end()) {
                                        ch->setAddr(it->second.first);
                                        ch->setAddrUnknown(false);
                                        count++;
                                } else {
                                        // Symbol no longer exists in any loaded ELF → mark unknown.
                                        ch->setAddrUnknown(true);
                                        unknown++;
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

                        auto it = symMap.find(v.name);
                        if (it != symMap.end()) {
                                v.addr        = it->second.first;
                                v.typeOff     = it->second.second;
                                v.addrUnknown = false;
                                count++;
                        } else {
                                v.addrUnknown = true;
                                unknown++;
                        }
                }
        }

        if (count > 0 || unknown > 0 || enumCount > 0) {
                LOG_I("ELF sync: %d resolved, %d unknown, %d enums refreshed", count, unknown, enumCount);
                JLinkPort::instance().reqRestart(); // Force sampler thread to rebuild HSS list
        }
}

void
Gui::syncVariableProperties(Variable *var)
{
        std::lock_guard lk(mtxMonitors_);
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (ch->getSymbolName().empty())
                                        continue;
                                for (const auto &v : var->vars_) {
                                        bool isMatch = false;
                                        if (ch->getSymbolName() == v.name) {
                                                isMatch = true;
                                        } else if (ch->getSymbolName().size() > v.name.size()) {
                                                if (ch->getSymbolName().compare(0, v.name.size(), v.name) == 0) {
                                                        char nextChar = ch->getSymbolName()[v.name.size()];
                                                        if (nextChar == '.' || nextChar == '[') {
                                                                isMatch = true;
                                                        }
                                                }
                                        }

                                        if (isMatch) {
                                                ch->setWritable(v.writable);
                                                ch->setAddr(v.addr);
                                                const char *deviceNames[] = {"JLINK", "UDP", "SHM", "MANUAL"};
                                                ch->setDevice(deviceNames[(int)v.port]);
                                                if (v.port == PortType::UDP) {
                                                        ch->setDevice("LOCAL");
                                                }
                                                if (v.port == PortType::SHM) {
                                                        ch->setShmRegionName(v.shm.name);
                                                        MonitorScope::shmInit(*ch);
                                                }
                                                break;
                                        }
                                }
                        }
                }
        }
}

void
Gui::drawCalculator()
{
        if (!ImGui::Begin(tr("Motor Parameter Calculator###MotorCalc", "电机参数计算器###MotorCalc"), &showCalculator_)) {
                ImGui::End();
                return;
        }

        if (!ImGui::BeginTabBar("##MotorCalcTabs")) {
                ImGui::End();
                return;
        }

        ImGuiTabItemFlags tabFlag0 = (s_eff.forceTab && s_eff.activeTab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags tabFlag1 = (s_eff.forceTab && s_eff.activeTab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
        if (s_eff.forceTab)
                s_eff.forceTab = false;

        // ══════════════════════════════════════════════════════════════════════
        // Tab 1: Motor Parameters (existing)
        // ══════════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem(tr("Motor Parameters", "电机参数"), nullptr, tabFlag0)) {
                s_eff.activeTab = 0;
                if (motorProfiles_.empty()) {
                        motorProfiles_.push_back(MotorProfile{});
                        currentMotorProfile_ = 0;
                }
                if (currentMotorProfile_ >= static_cast<i32>(motorProfiles_.size()))
                        currentMotorProfile_ = 0;

                ImGui::Text("%s", tr("Saved Profiles:", "已保存配置："));
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
                if (ImGui::Button(tr("Add", "添加"))) {
                        MotorProfile mp;
                        snprintf(mp.modelName, sizeof(mp.modelName), "Motor_%d", static_cast<i32>(motorProfiles_.size() + 1));
                        motorProfiles_.push_back(mp);
                        currentMotorProfile_ = static_cast<i32>(motorProfiles_.size()) - 1;
                }
                ImGui::SameLine();
                if (ui::Button(tr("Del", "删除"), ui::BtnStyle::Danger) && motorProfiles_.size() > 1) {
                        motorProfiles_.erase(motorProfiles_.begin() + currentMotorProfile_);
                        if (currentMotorProfile_ > 0)
                                currentMotorProfile_--;
                }

                ImGui::Separator();
                MotorProfile &mp = motorProfiles_[currentMotorProfile_];

                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("--- Input Parameters ---", "--- 输入参数 ---"));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputText("##MotorModel", mp.modelName, sizeof(mp.modelName));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Model", "型号"));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorRs", &mp.Rs, 0.0f, 0.0f, "%.6f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Rs (Ohm)");
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorLd", &mp.Ld, 0.0f, 0.0f, "%.8f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Ld (H)");
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorLq", &mp.Lq, 0.0f, 0.0f, "%.8f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Lq (H)");
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputInt("##MotorPolePairs", &mp.polePairs, 0, 0);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Pole Pairs", "极对数"));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorKt", &mp.Kt, 0.0f, 0.0f, "%.8f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Kt (Nm/A)");

                ImGui::Separator();
                ImGui::TextColored(
                    ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("--- Back-EMF Measurement ---", "--- 反电动势测量 ---"));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorBackEmfFreq", &mp.backEmfFreq, 0.0f, 0.0f, "%.3f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Frequency (Hz)", "频率 (Hz)"));
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputFloat("##MotorBackEmfVpp", &mp.backEmfVpp, 0.0f, 0.0f, "%.3f");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Vpp (Line-to-Line) (V)", "线电压峰峰值 Vpp (V)"));

                if (mp.polePairs < 1)
                        mp.polePairs = 1;
                if (mp.backEmfFreq <= 0)
                        mp.backEmfFreq = 0.001f;

                const f32 pi    = 3.14159265358979323846f;
                f32       psi_m = mp.backEmfVpp / (4.0f * sqrtf(3.0f) * pi * mp.backEmfFreq);
                f32       kt    = 1.5f * static_cast<f32>(mp.polePairs) * psi_m;
                f32       kv    = (120.0f * mp.backEmfFreq) / (static_cast<f32>(mp.polePairs) * mp.backEmfVpp);

                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("--- Calculated Results ---", "--- 计算结果 ---"));
                ImGui::Text(tr("Flux Linkage (Psi_m): %.6f Wb", "磁链 (Psi_m): %.6f Wb"), static_cast<f64>(psi_m));
                ImGui::Text(tr("KV value: %.2f RPM/V", "KV 值: %.2f RPM/V"), static_cast<f64>(kv));
                ImGui::Text(tr("Calculated Kt (Ref): %.6f Nm/A", "计算 Kt (参考): %.6f Nm/A"), static_cast<f64>(kt));

                ImGui::EndTabItem();
        }

        // ══════════════════════════════════════════════════════════════════════
        // Tab 2: Efficiency Calculator
        // ══════════════════════════════════════════════════════════════════════
        if (ImGui::BeginTabItem(tr("Efficiency", "效率计算"), nullptr, tabFlag1)) {
                s_eff.activeTab         = 1;
                static float effTorque  = 1.0f;
                static float effSpeed   = 1000.0f;
                static float effVoltage = 48.0f;
                static float effCurrent = 1.0f;
                // Unit dropdowns and channel links delegate to s_eff (saved with session)
                auto               &effTorqueUnit    = s_eff.torqueUnit;
                auto               &effSpeedUnit     = s_eff.speedUnit;
                auto               &effVoltageUnit   = s_eff.voltageUnit;
                auto               &effCurrentUnit   = s_eff.currentUnit;
                auto               &effTorqueLink    = s_eff.torqueLink;
                auto               &effSpeedLink     = s_eff.speedLink;
                auto               &effVoltageLink   = s_eff.voltageLink;
                auto               &effCurrentLink   = s_eff.currentLink;
                static std::string *pendingVarLink   = nullptr;
                static bool         doOpenVarPick    = false;
                auto               &effRefreshMs     = s_eff.refreshMs;
                static u64          effLastRefreshTs = 0;
                // ── Efficiency map state ─────────────────────────────────────
                constexpr int kMapBins     = 30;
                static bool   effMapRec    = false;
                auto         &effMapSpdMin = s_eff.spdMin;
                auto         &effMapSpdMax = s_eff.spdMax;
                auto         &effMapTqMin  = s_eff.tqMin;
                auto         &effMapTqMax  = s_eff.tqMax;

                // Conversion to SI
                const float kTorqueToNm[]  = {1.0f, 0.001f, 0.00706155f, 0.11298f, 1.35582f};
                const float kSpeedToRadS[] = {3.14159265f / 30.0f, 1.0f, 6.28318530f};
                const float kVoltToV[]     = {1.0f, 0.001f, 1000.0f};
                const float kCurrToA[]     = {1.0f, 0.001f};

                // Read current value from a linked key.
                // Monitor channel key: "@monName@scopeName@chName"  → getDispVal()
                // Variable key:        "winKey::varName"             → valueStr
                //                      "winKey::varName.field"       → struct field
                auto getVarVal = [&](const std::string &key, float fallback) -> float {
                        if (key.empty())
                                return fallback;
                        // ── Monitor channel path ─────────────────────────────
                        if (key[0] == '@') {
                                auto p1 = key.find('@', 1);
                                if (p1 == std::string::npos)
                                        return fallback;
                                auto p2 = key.find('@', p1 + 1);
                                if (p2 == std::string::npos)
                                        return fallback;
                                const std::string mname  = key.substr(1, p1 - 1);
                                const std::string sname  = key.substr(p1 + 1, p2 - p1 - 1);
                                const std::string chname = key.substr(p2 + 1);
                                auto              mit    = monitors_.find(mname);
                                if (mit == monitors_.end())
                                        return fallback;
                                auto sit = mit->second->getScopes().find(sname);
                                if (sit == mit->second->getScopes().end())
                                        return fallback;
                                if (sit->second->getChannels().find(chname) == sit->second->getChannels().end())
                                        return fallback;
                                return static_cast<float>(sit->second->getChannelMean(chname));
                        }
                        // ── Variable manager path ────────────────────────────
                        auto sep = key.find("::");
                        if (sep == std::string::npos)
                                return fallback;
                        auto wit = vars_.find(key.substr(0, sep));
                        if (wit == vars_.end())
                                return fallback;
                        const std::string rest = key.substr(sep + 2); // "varName" or "varName.path"
                        auto              dot  = rest.find('.');
                        if (dot == std::string::npos) {
                                // Scalar top-level variable
                                for (const auto &ve : wit->second->vars_) {
                                        if (ve.name == rest && !ve.valueStr.empty()) {
                                                try {
                                                        return std::stof(ve.valueStr);
                                                } catch (...) {
                                                }
                                        }
                                }
                                return fallback;
                        }
                        const std::string varName = rest.substr(0, dot);
                        for (const auto &ve : wit->second->vars_) {
                                if (ve.name != varName)
                                        continue;
                                if (!ve.structFields.empty()) {
                                        // LOCAL struct: single-level field
                                        float val = fallback;
                                        wit->second->readLocalFieldAsFloat(varName, rest.substr(dot + 1), val);
                                        return val;
                                }
                                // DWARF struct member: read from display-value cache
                                float val = fallback;
                                wit->second->getDwarfMemberAsFloat(rest, val);
                                return val;
                        }
                        return fallback;
                };

                // Pull linked Monitor channel values at the selected refresh interval
                bool effRefreshFired = false;
                {
                        u64 nowMs = static_cast<u64>(ImGui::GetTime() * 1000.0);
                        if (nowMs - effLastRefreshTs >= static_cast<u64>(effRefreshMs)) {
                                effLastRefreshTs = nowMs;
                                effRefreshFired  = true;
                                if (!effTorqueLink.empty())
                                        effTorque = getVarVal(effTorqueLink, effTorque);
                                if (!effSpeedLink.empty())
                                        effSpeed = getVarVal(effSpeedLink, effSpeed);
                                if (!effVoltageLink.empty())
                                        effVoltage = getVarVal(effVoltageLink, effVoltage);
                                if (!effCurrentLink.empty())
                                        effCurrent = getVarVal(effCurrentLink, effCurrent);
                        }
                }

                // col1=label col2=input col3=unit dropdown col4=link button
                constexpr ImGuiTableFlags tfl  = ImGuiTableFlags_SizingFixedFit;
                const float               col1 = 120.0f, col2 = 100.0f, col3 = 90.0f, col4 = 24.0f;

                // Draw one input row with an optional variable-link button (col4)
                auto inputRow = [&](const char  *lbl,
                                    float       &val,
                                    const char  *inputId,
                                    const char  *fmt,
                                    int         &unitIdx,
                                    const char  *units,
                                    const char  *unitId,
                                    std::string &link,
                                    int          fi) {
                        char lnkId[16], unlId[16];
                        snprintf(lnkId, sizeof(lnkId), "~##lb%d", fi);
                        snprintf(unlId, sizeof(unlId), "X##ub%d", fi);

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(lbl);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (!link.empty()) {
                                ImGui::BeginDisabled(true);
                                ImGui::InputFloat(inputId, &val, 0, 0, fmt);
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                        if (!link.empty() && link[0] == '@') {
                                                // "@monName@scopeName@chName" → show "scopeName / chName"
                                                auto p1 = link.find('@', 1);
                                                auto p2 =
                                                    (p1 != std::string::npos) ? link.find('@', p1 + 1) : std::string::npos;
                                                if (p2 != std::string::npos) {
                                                        const std::string scopePart = link.substr(p1 + 1, p2 - p1 - 1);
                                                        const std::string chPart    = link.substr(p2 + 1);
                                                        ImGui::SetTooltip("%s / %s", scopePart.c_str(), chPart.c_str());
                                                } else {
                                                        ImGui::SetTooltip("%s", link.c_str());
                                                }
                                        } else {
                                                auto sep = link.find("::");
                                                ImGui::SetTooltip(
                                                    "%s", sep != std::string::npos ? link.c_str() + sep + 2 : link.c_str());
                                        }
                                }
                        } else {
                                ImGui::InputFloat(inputId, &val, 0, 0, fmt);
                        }

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (!link.empty()) {
                                ImGui::BeginDisabled(true);
                                ImGui::Combo(unitId, &unitIdx, units);
                                ImGui::EndDisabled();
                        } else {
                                ImGui::Combo(unitId, &unitIdx, units);
                        }

                        ImGui::TableSetColumnIndex(3);
                        if (!link.empty()) {
                                if (ImGui::SmallButton(unlId))
                                        link.clear();
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Unlink variable", "取消关联变量"));
                        } else {
                                if (ImGui::SmallButton(lnkId)) {
                                        pendingVarLink = &link;
                                        doOpenVarPick  = true;
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Link to variable", "关联变量"));
                        }
                };

                // ── Mechanical Power input ──────────────────────────────────
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("Mechanical Power", "机械功率"));
                if (ImGui::BeginTable("##effMech", 4, tfl)) {
                        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, col1);
                        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthFixed, col2);
                        ImGui::TableSetupColumn("##u", ImGuiTableColumnFlags_WidthFixed, col3);
                        ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthFixed, col4);
                        inputRow(tr("Torque", "扭矩"),
                                 effTorque,
                                 "##effTq",
                                 "%.4f",
                                 effTorqueUnit,
                                 "Nm\0mNm\0oz.in\0lb.in\0lb.ft\0",
                                 "##effTqU",
                                 effTorqueLink,
                                 0);
                        inputRow(tr("Speed", "转速"),
                                 effSpeed,
                                 "##effSpd",
                                 "%.3f",
                                 effSpeedUnit,
                                 "RPM\0rad/s\0Hz\0",
                                 "##effSpdU",
                                 effSpeedLink,
                                 1);
                        ImGui::EndTable();
                }

                // ── Refresh rate ────────────────────────────────────────────
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("Refresh rate:", "刷新率:"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::SliderInt("##effRefreshMs", &effRefreshMs, 10, 2000);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Refresh (ms)", "刷新间隔(毫秒)"));

                // ── DC Bus Power input ──────────────────────────────────────
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("DC Bus Power", "直流母线功率"));
                if (ImGui::BeginTable("##effElec", 4, tfl)) {
                        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, col1);
                        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthFixed, col2);
                        ImGui::TableSetupColumn("##u", ImGuiTableColumnFlags_WidthFixed, col3);
                        ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthFixed, col4);
                        inputRow(tr("Bus Voltage", "母线电压"),
                                 effVoltage,
                                 "##effVdc",
                                 "%.4f",
                                 effVoltageUnit,
                                 "V\0mV\0kV\0",
                                 "##effVdcU",
                                 effVoltageLink,
                                 2);
                        inputRow(tr("Bus Current", "母线电流"),
                                 effCurrent,
                                 "##effIdc",
                                 "%.4f",
                                 effCurrentUnit,
                                 "A\0mA\0",
                                 "##effIdcU",
                                 effCurrentLink,
                                 3);
                        ImGui::EndTable();
                }

                // ── Variable picker popup ───────────────────────────────────
                // OpenPopup must be in the same ID-stack context as BeginPopup
                if (doOpenVarPick) {
                        ImGui::OpenPopup("##varPick");
                        doOpenVarPick = false;
                }
                if (ImGui::BeginPopup("##varPick")) {
                        static char varSearch[128]{};
                        ImGui::SetNextItemWidth(220.0f);
                        ImGui::InputTextWithHint("##vpSearch", tr("Search...", "搜索..."), varSearch, sizeof(varSearch));
                        ImGui::Separator();

                        const bool isSearching = varSearch[0] != '\0';

                        // Case-insensitive substring match
                        auto matches = [](const char *haystack, const char *needle) -> bool {
                                if (!needle || needle[0] == '\0')
                                        return true;
                                for (const char *p = haystack; *p; ++p) {
                                        const char *a = p, *b = needle;
                                        while (*a && *b && tolower((u8)*a) == tolower((u8)*b)) {
                                                ++a;
                                                ++b;
                                        }
                                        if (!*b)
                                                return true;
                                }
                                return false;
                        };

                        auto doSelect = [&](const std::string &key) {
                                if (pendingVarLink)
                                        *pendingVarLink = key;
                                pendingVarLink = nullptr;
                                varSearch[0]   = '\0';
                                ImGui::CloseCurrentPopup();
                        };

                        if (ImGui::MenuItem(tr("-- manual --", "-- 手动输入 --"))) {
                                if (pendingVarLink)
                                        pendingVarLink->clear();
                                pendingVarLink = nullptr;
                                varSearch[0]   = '\0';
                                ImGui::CloseCurrentPopup();
                        }

                        // ── Monitor channels (primary / recommended) ────────
                        bool anyMonCh = false;
                        for (auto &[mname, mon] : monitors_) {
                                for (auto &[sname, scope] : mon->getScopes()) {
                                        for (auto &[chname, ch] : scope->getChannels()) {
                                                char fullName[256];
                                                snprintf(fullName, sizeof(fullName), "[%s] %s", sname.c_str(), chname.c_str());
                                                if (!matches(fullName, varSearch) && !matches(chname.c_str(), varSearch))
                                                        continue;
                                                if (!anyMonCh) {
                                                        ImGui::TextDisabled("%s", tr("Monitor Channels", "监视器通道"));
                                                        anyMonCh = true;
                                                }
                                                char lbl[320];
                                                snprintf(lbl,
                                                         sizeof(lbl),
                                                         "%s  (avg %.4g)##mon_%s_%s_%s",
                                                         fullName,
                                                         scope->getChannelMean(chname),
                                                         mname.c_str(),
                                                         sname.c_str(),
                                                         chname.c_str());
                                                if (ImGui::MenuItem(lbl))
                                                        doSelect("@" + mname + "@" + sname + "@" + chname);
                                        }
                                }
                        }
                        if (!anyMonCh)
                                ImGui::TextDisabled("%s", tr("(no monitor channels)", "(无监视器通道)"));

                        ImGui::EndPopup();
                }

                // ── Calculations ────────────────────────────────────────────
                float torque_Nm  = effTorque * kTorqueToNm[effTorqueUnit];
                float speed_rads = effSpeed * kSpeedToRadS[effSpeedUnit];
                float voltage_V  = effVoltage * kVoltToV[effVoltageUnit];
                float current_A  = effCurrent * kCurrToA[effCurrentUnit];

                float P_mech    = torque_Nm * speed_rads;
                float P_elec    = voltage_V * current_A;
                float P_elec_ab = std::abs(P_elec);
                float eta       = (P_elec_ab > 1e-9f) ? (std::abs(P_mech) / P_elec_ab * 100.0f) : 0.0f;

                // ── Efficiency map recording ─────────────────────────────────
                // Every refresh cycle appends a new scatter point.
                if (effMapRec && effRefreshFired && eta > 0.f && eta <= 100.f) {
                        float spdRpm = speed_rads * (30.f / 3.14159265f);
                        s_eff.rawData.push_back({spdRpm, torque_Nm, eta});
                }

                // ── Results ─────────────────────────────────────────────────
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("Results", "计算结果"));

                if (ImGui::BeginTable("##effRes", 2, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("##rl", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                        ImGui::TableSetupColumn("##rv", ImGuiTableColumnFlags_WidthStretch);

                        auto row = [](const char *label, const char *fmt, float val) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(label);
                                ImGui::TableSetColumnIndex(1);
                                char buf[64];
                                snprintf(buf, sizeof(buf), fmt, val);
                                ImGui::TextUnformatted(buf);
                        };

                        row(tr("Torque (Nm)", "扭矩 (Nm)"), "%.4f Nm", torque_Nm);
                        row(tr("Speed (rad/s)", "转速 (rad/s)"), "%.3f rad/s", speed_rads);
                        row(tr("Mech. Power", "机械功率"), "%.3f W", P_mech);
                        row(tr("Bus Voltage (V)", "母线电压 (V)"), "%.4f V", voltage_V);
                        row(tr("Bus Current (A)", "母线电流 (A)"), "%.4f A", current_A);
                        row(tr("Elec. Power", "电气功率"), "%.3f W", P_elec);
                        ImGui::EndTable();
                }

                ImGui::Spacing();
                // Efficiency bar
                ImVec4 etaColor = (eta >= 90.0f)   ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                  : (eta >= 70.0f) ? ImVec4(1.0f, 0.85f, 0.1f, 1.0f)
                                                   : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(etaColor, tr("Efficiency:  %.2f %%", "效率：  %.2f %%"), eta);
                float barW = ImGui::GetContentRegionAvail().x;
                float frac = std::min(std::max(eta / 100.0f, 0.0f), 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, etaColor);
                ImGui::ProgressBar(frac, ImVec2(barW, 12.0f), "");
                ImGui::PopStyleColor();

                if (eta > 100.0f)
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f),
                                           "%s",
                                           tr("! Efficiency > 100%% — check inputs.", "! 效率 > 100%%，请检查输入。"));

                // ── Efficiency Map ───────────────────────────────────────────
                ImGui::Spacing();
                if (ImGui::CollapsingHeader(tr("Efficiency Map", "效率图"))) {
                        // ── Control buttons ───────────────────────────────────
                        if (effMapRec) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.12f, 0.12f, 1.f));
                                if (ImGui::Button(tr("■  Stop", "■  停止记录")))
                                        effMapRec = false;
                                ImGui::PopStyleColor();
                        } else {
                                if (ImGui::Button(tr("▶  Record", "▶  开始记录")))
                                        effMapRec = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Clear##emap", "清除##emap")))
                                s_eff.rawData.clear();
                        ImGui::SameLine();
                        ImGui::Text(tr("Pts: %zu", "点数: %zu"), s_eff.rawData.size());
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Export Table##etbl", "导出表格##etbl"))) {
                                auto f = nativeDlgSave(
                                    tr("Export Efficiency Table", "导出效率数据"), {{"CSV", {"csv"}}}, "efficiency_table");
                                if (!f.empty()) {
                                        std::ofstream ofs(f);
                                        if (ofs) {
                                                ofs << "Speed_RPM,Torque_Nm,Efficiency_%\n";
                                                for (const auto &pt : s_eff.rawData)
                                                        ofs << pt.spdRpm << "," << pt.tqNm << "," << pt.eta << "\n";
                                        }
                                }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Export Map##emap2", "导出效率图##emap2"))) {
                                auto f = nativeDlgSave(
                                    tr("Export Efficiency Map", "导出效率图"), {{"CSV", {"csv"}}}, "efficiency_map");
                                if (!f.empty()) {
                                        std::ofstream ofs(f);
                                        if (ofs) {
                                                // Bin raw data into grid for CSV export
                                                float mapVals[kMapBins * kMapBins]{};
                                                float mapSum[kMapBins * kMapBins]{};
                                                int   mapCnt[kMapBins * kMapBins]{};
                                                float spdRange = effMapSpdMax - effMapSpdMin;
                                                float tqRange  = effMapTqMax - effMapTqMin;
                                                if (spdRange > 1.f && tqRange > 1e-3f) {
                                                        for (const auto &pt : s_eff.rawData) {
                                                                int xi = static_cast<int>((pt.spdRpm - effMapSpdMin) /
                                                                                          spdRange * kMapBins);
                                                                int yi = kMapBins - 1 -
                                                                         static_cast<int>((pt.tqNm - effMapTqMin) / tqRange *
                                                                                          kMapBins);
                                                                xi           = std::max(0, std::min(xi, kMapBins - 1));
                                                                yi           = std::max(0, std::min(yi, kMapBins - 1));
                                                                int idx      = yi * kMapBins + xi;
                                                                mapSum[idx] += pt.eta;
                                                                mapCnt[idx]++;
                                                        }
                                                        for (int i = 0; i < kMapBins * kMapBins; ++i)
                                                                mapVals[i] = mapCnt[i] > 0
                                                                                 ? mapSum[i] / static_cast<float>(mapCnt[i])
                                                                                 : 0.f;
                                                }
                                                // Column headers: speed bin centres
                                                ofs << "Torque_Nm\\Speed_RPM";
                                                for (int x = 0; x < kMapBins; ++x) {
                                                        float spd = effMapSpdMin +
                                                                    (x + 0.5f) * (effMapSpdMax - effMapSpdMin) / kMapBins;
                                                        ofs << "," << spd;
                                                }
                                                ofs << "\n";
                                                // Rows: high torque first (row 0 = top of map)
                                                for (int y = 0; y < kMapBins; ++y) {
                                                        float tq =
                                                            effMapTqMax - (y + 0.5f) * (effMapTqMax - effMapTqMin) / kMapBins;
                                                        ofs << tq;
                                                        for (int x = 0; x < kMapBins; ++x)
                                                                ofs << "," << mapVals[y * kMapBins + x];
                                                        ofs << "\n";
                                                }
                                        }
                                }
                        }

                        // ── Range controls ────────────────────────────────────
                        ImGui::TextUnformatted(tr("Speed:", "转速:"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(60.f);
                        ImGui::InputFloat("##mspd0", &effMapSpdMin, 0, 0, "%.0f");
                        ImGui::SameLine();
                        ImGui::TextUnformatted("-");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(60.f);
                        ImGui::InputFloat("##mspd1", &effMapSpdMax, 0, 0, "%.0f");
                        ImGui::SameLine();
                        ImGui::TextUnformatted("RPM");
                        ImGui::SameLine();
                        ImGui::Spacing();
                        ImGui::SameLine();
                        ImGui::TextUnformatted(tr("Torque:", "转矩:"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.f);
                        ImGui::InputFloat("##mtq0", &effMapTqMin, 0, 0, "%.1f");
                        ImGui::SameLine();
                        ImGui::TextUnformatted("-");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.f);
                        ImGui::InputFloat("##mtq1", &effMapTqMax, 0, 0, "%.1f");
                        ImGui::SameLine();
                        ImGui::TextUnformatted("Nm");

                        // ── Scatter plot (one dot per sampled point) ──────────
                        ImPlot::PushColormap(ImPlotColormap_Hot);
                        if (ImPlot::BeginPlot(
                                "##effmap", ImVec2(ImGui::GetContentRegionAvail().x - 70.f, 280.f), ImPlotFlags_NoMouseText)) {
                                ImPlot::SetupAxis(ImAxis_X1, tr("Speed (RPM)", "转速 (RPM)"));
                                ImPlot::SetupAxis(ImAxis_Y1, tr("Torque (Nm)", "转矩 (Nm)"));
                                ImPlot::SetupAxisLimits(ImAxis_X1, effMapSpdMin, effMapSpdMax, ImGuiCond_Always);
                                ImPlot::SetupAxisLimits(ImAxis_Y1, effMapTqMin, effMapTqMax, ImGuiCond_Always);
                                // Draw each point as a colored scatter marker
                                const int cmapSize = ImPlot::GetColormapSize();
                                for (size_t i = 0; i < s_eff.rawData.size(); ++i) {
                                        const auto &pt  = s_eff.rawData[i];
                                        float       t   = std::max(0.f, std::min(pt.eta / 100.f, 1.f));
                                        int         idx = static_cast<int>(t * (cmapSize - 1));
                                        ImVec4      col = ImPlot::GetColormapColor(idx);
                                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5.f, col, IMPLOT_AUTO, col);
                                        char label[32];
                                        snprintf(label, sizeof(label), "##pt%zu", i);
                                        ImPlot::PlotScatter(label, &pt.spdRpm, &pt.tqNm, 1);
                                }
                                ImPlot::EndPlot();
                        }
                        ImGui::SameLine();
                        ImPlot::ColormapScale("##cscale", 0.0, 100.0, ImVec2(60.f, 280.f));
                        ImPlot::PopColormap();
                }

                ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
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

                // Split a CSV line into trimmed fields (empty fields preserved).
                auto splitCsv = [](const std::string &s) {
                        std::vector<std::string> out;
                        std::istringstream       ss(s);
                        std::string              tok;
                        while (std::getline(ss, tok, ',')) {
                                while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' '))
                                        tok.pop_back();
                                while (!tok.empty() && tok.front() == ' ')
                                        tok.erase(tok.begin());
                                out.push_back(tok);
                        }
                        return out;
                };

                // --- Parse header row ---
                std::string line;
                if (!std::getline(f, line)) {
                        LOG_E("CSV import: empty file %s", path.c_str());
                        return;
                }

                std::vector<std::string> headers = splitCsv(line);
                if (headers.empty()) {
                        LOG_E("CSV import: no columns in %s", path.c_str());
                        return;
                }

                // Recognise this tool's own export format: every column is a
                // "<scope>::<channel>::Time" / "<scope>::<channel>::Value" pair.
                // scope/channel may contain '_' but never "::", so the split is
                // unambiguous (channel takes everything after the first "::").
                auto parseNative =
                    [](const std::string &h, std::string &scope, std::string &channel, std::string &kind) -> bool {
                        auto pK = h.rfind("::");
                        if (pK == std::string::npos)
                                return false;
                        kind = h.substr(pK + 2);
                        if (kind != "Time" && kind != "Value")
                                return false;
                        const std::string rest = h.substr(0, pK);
                        auto              pS   = rest.find("::");
                        if (pS == std::string::npos)
                                return false;
                        scope   = rest.substr(0, pS);
                        channel = rest.substr(pS + 2);
                        return !scope.empty() && !channel.empty();
                };

                bool isNative = true;
                for (const auto &h : headers) {
                        std::string s, c, k;
                        if (!parseNative(h, s, c, k)) {
                                isNative = false;
                                break;
                        }
                }

                std::vector<CsvChannelImport> outChannels;

                if (isNative) {
                        // Map each column to its channel slot; preserve first-seen order.
                        std::vector<std::string> keys; // "<scope>\x1f<channel>"
                        std::vector<int>         timeCol, valCol;
                        auto                     slotFor = [&](const std::string &scope, const std::string &channel) {
                                const std::string key = scope + '\x1f' + channel;
                                for (size_t i = 0; i < keys.size(); ++i)
                                        if (keys[i] == key)
                                                return static_cast<int>(i);
                                keys.push_back(key);
                                timeCol.push_back(-1);
                                valCol.push_back(-1);
                                CsvChannelImport ci;
                                ci.scope   = scope;
                                ci.channel = channel;
                                outChannels.push_back(std::move(ci));
                                return static_cast<int>(keys.size() - 1);
                        };
                        for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
                                std::string s, c, k;
                                parseNative(headers[i], s, c, k);
                                const int slot                         = slotFor(s, c);
                                (k == "Time" ? timeCol : valCol)[slot] = i;
                        }

                        // Each channel carries its own Time/Value columns; a row whose
                        // Time+Value cells are both empty means that channel has no sample
                        // there (channels may differ in length), so it is simply skipped.
                        while (std::getline(f, line)) {
                                if (line.empty() || line[0] == '#')
                                        continue;
                                std::vector<std::string> toks = splitCsv(line);
                                toks.resize(headers.size());

                                for (size_t ch = 0; ch < outChannels.size(); ++ch) {
                                        const int tc = timeCol[ch], vc = valCol[ch];
                                        if (tc < 0 || vc < 0)
                                                continue;
                                        const std::string &tt = toks[tc];
                                        const std::string &vt = toks[vc];
                                        if (tt.empty() && vt.empty())
                                                continue;
                                        double tv = 0.0, vv = 0.0;
                                        std::from_chars(tt.data(), tt.data() + tt.size(), tv);
                                        std::from_chars(vt.data(), vt.data() + vt.size(), vv);
                                        outChannels[ch].timestamps.push_back(tv);
                                        outChannels[ch].values.push_back(static_cast<float>(vv));
                                }
                        }
                } else {
                        // Generic CSV: optional leading time column + data columns,
                        // all dropped into a single scope.
                        bool firstIsTime = false;
                        {
                                std::string h0 = headers[0];
                                for (auto &c : h0)
                                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                firstIsTime = (h0 == "t" || h0 == "time" || h0 == "ts" || h0 == "timestamp" || h0 == "index" ||
                                               h0 == "x");
                        }

                        const int timeCol   = firstIsTime ? 0 : -1;
                        const int dataStart = firstIsTime ? 1 : 0;

                        std::vector<std::string> dataHeaders(headers.begin() + dataStart, headers.end());
                        const int                nCols = static_cast<int>(dataHeaders.size());
                        if (nCols == 0) {
                                LOG_E("CSV import: no data columns in %s", path.c_str());
                                return;
                        }

                        std::vector<double>             timestamps;
                        std::vector<std::vector<float>> columns(nCols);

                        double rowIdx = 0.0;
                        while (std::getline(f, line)) {
                                if (line.empty() || line[0] == '#')
                                        continue;
                                std::vector<std::string> toks = splitCsv(line);
                                toks.resize(headers.size());

                                std::vector<double> rowVals(headers.size(), 0.0);
                                for (size_t i = 0; i < toks.size(); ++i)
                                        std::from_chars(toks[i].data(), toks[i].data() + toks[i].size(), rowVals[i]);

                                timestamps.push_back((timeCol >= 0) ? rowVals[timeCol] : rowIdx);
                                for (int c = 0; c < nCols; ++c)
                                        columns[c].push_back(static_cast<float>(rowVals[dataStart + c]));
                                rowIdx += 1.0;
                        }

                        if (timestamps.empty()) {
                                LOG_E("CSV import: no data rows in %s", path.c_str());
                                return;
                        }

                        for (int c = 0; c < nCols; ++c) {
                                CsvChannelImport ci;
                                ci.scope      = "scope_0";
                                ci.channel    = dataHeaders[c];
                                ci.timestamps = timestamps;
                                ci.values     = std::move(columns[c]);
                                outChannels.push_back(std::move(ci));
                        }
                }

                if (outChannels.empty()) {
                        LOG_E("CSV import: no data in %s", path.c_str());
                        return;
                }

                LOG_I("CSV import done: %s  native=%d  channels=%zu", path.c_str(), isNative ? 1 : 0, outChannels.size());

                CsvImportPending result;
                result.monitorName = monitorName;
                result.channels    = std::move(outChannels);

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
                auto     it      = monitors_.find(imp.monitorName);
                Monitor *monitor = (it != monitors_.end()) ? it->second.get() : nullptr;
                if (!monitor) {
                        // Fallback: create it now (shouldn't normally happen)
                        auto m                     = std::make_shared<Monitor>(imp.monitorName);
                        monitors_[imp.monitorName] = m;
                        monitor                    = m.get();
                }
                const std::string &name = imp.monitorName;

                bool   haveSpan = false;
                double spanMin = 0.0, spanMax = 0.0;
                usize  totalPts = 0;

                for (auto &cd : imp.channels) {
                        // Restore the originating scope (no-op if already present).
                        monitor->addScope(cd.scope);
                        MonitorScope *scope = monitor->getScopes()[cd.scope].get();
                        if (!scope)
                                continue;

                        scope->addChannel(cd.channel);
                        MonitorChannel *ch = scope->findChannel(cd.channel);
                        if (!ch)
                                continue;

                        ch->historySeconds_   = 0.0f; // static data — no time-based pruning
                        ch->maxDisplayPoints_ = 5000;

                        const usize nPts = cd.values.size();
                        ch->pushBatch(cd.values.data(), cd.timestamps.data(), nPts);
                        ch->publishSnapshot();
                        totalPts += nPts;

                        if (!cd.timestamps.empty()) {
                                const double lo = cd.timestamps.front();
                                const double hi = cd.timestamps.back();
                                if (!haveSpan) {
                                        spanMin  = lo;
                                        spanMax  = hi;
                                        haveSpan = true;
                                } else {
                                        spanMin = std::min(spanMin, lo);
                                        spanMax = std::max(spanMax, hi);
                                }
                        }
                }

                // Set x-axis range to cover the full data span across all scopes
                if (haveSpan) {
                        monitor->linkXMin_      = spanMin;
                        monitor->linkXMax_      = spanMax;
                        monitor->dataStartTime_ = spanMin;
                        monitor->lastNow_       = spanMax;
                }

                // Clear loading flag — monitor will show data on the next frame
                monitor->csvLoading_.store(false, std::memory_order_release);

                LOG_I("CSV import: monitor '%s' ready: %zu channels, %zu points", name.c_str(), imp.channels.size(), totalPts);
        }
}

/* --------------------------------------------------------------------------
 * Auto-update UI
 * -------------------------------------------------------------------------- */

void
Gui::drawUpdateUI()
{
        const Updater::Info info = updater_.get();

        // When a check finishes, open the appropriate popup exactly once.
        if (updatePendingResult_ && !updater_.isChecking() && info.checked) {
                updatePendingResult_ = false;
                if (info.available)
                        ImGui::OpenPopup("###UpdateAvailable");
                else if (updateManualCheck_)
                        ImGui::OpenPopup("###UpdateStatus");
                updateManualCheck_ = false;
        }

        // --- Detect background download completion ---
        const auto dlState = updater_.getDownloadState();
        if (dlState == Updater::DownloadState::Done && !showDownloadDonePopup_) {
                showDownloadDonePopup_ = true;
                ImGui::OpenPopup("###UpdateReady");
        } else if (dlState == Updater::DownloadState::Failed && !showDownloadDonePopup_) {
                showDownloadDonePopup_ = true;
                ImGui::OpenPopup("###DownloadFailed");
        }

        // --- Update available ---
        if (ImGui::BeginPopupModal(tr("Update Available###UpdateAvailable", "有可用更新###UpdateAvailable"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", tr("A new version of ava_tool is available.", "ava_tool 有新版本可用。"));
                ImGui::Separator();
                ImGui::Text(tr("Current: %s", "当前版本: %s"), info.currentVersion.c_str());
                ImGui::Text(tr("Latest:  %s", "最新版本: %s"), info.latestVersion.c_str());
                if (!info.notes.empty()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("%s", tr("Release notes:", "更新说明："));
                        ImGui::BeginChild("##notes", ImVec2(460, 160), true);
                        ImGui::TextWrapped("%s", info.notes.c_str());
                        ImGui::EndChild();
                }
                ImGui::Spacing();

                const bool canAutoUpdate = !info.assetUrl.empty();
                if (canAutoUpdate) {
                        if (ui::Button(tr("Upgrade Now", "立即升级"), ui::BtnStyle::Success, ImVec2(130, 0))) {
                                // Start background download — don't exit the app
                                updater_.downloadAsync(info.assetUrl);
                                showDownloadDonePopup_ = false; // reset so we detect completion
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                } else if (!info.releaseUrl.empty()) {
                        if (ImGui::Button(tr("Open Release Page", "打开发布页面"), ImVec2(160, 0))) {
#ifdef _WIN32
                                ShellExecuteA(nullptr, "open", info.releaseUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                }
                if (ImGui::Button(tr("Later", "稍后"), ImVec2(120, 0)))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }

        // --- Update downloaded & ready to install ---
        if (ImGui::BeginPopupModal(
                tr("Update Ready###UpdateReady", "更新就绪###UpdateReady"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", tr("Update has been downloaded successfully.", "更新已下载完成。"));
                ImGui::Text("%s", tr("Restart the application to install the update?", "重启程序以安装更新？"));
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ui::Button(tr("Restart Now", "立即重启"), ui::BtnStyle::Warning, ImVec2(130, 0))) {
                        const std::string setupPath = updater_.getDownloadedPath();
                        if (updater_.launchInstaller(setupPath)) {
                                saveSession();
                                glfwSetWindowShouldClose(window_, GLFW_TRUE);
                                wantsToQuit_ = true;
                        }
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Later", "稍后"), ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }

        // --- Download failed ---
        if (ImGui::BeginPopupModal(tr("Download Failed###DownloadFailed", "下载失败###DownloadFailed"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                const std::string dlErr = updater_.getDownloadError();
                ImGui::Text("%s", tr("Failed to download the update.", "更新下载失败。"));
                if (!dlErr.empty()) {
                        ImGui::Spacing();
                        ImGui::TextWrapped(tr("Error: %s", "错误: %s"), dlErr.c_str());
                }
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                        updater_.resetDownload();
                        showDownloadDonePopup_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }

        // --- Manual "check finished" status ---
        if (ImGui::BeginPopupModal(
                tr("Update Status###UpdateStatus", "更新状态###UpdateStatus"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (!info.error.empty())
                        ImGui::TextWrapped("%s", info.error.c_str());
                else
                        ImGui::Text(tr("You're up to date (version %s).", "已是最新版本（版本 %s）。"),
                                    info.currentVersion.c_str());
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(120, 0)))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
}

// Map a parser CType to the variable manager's DataType (for "+"-created LOCAL vars).
static DataType
ctypeToVarDataType(CType t)
{
        switch (t) {
                case CType::F32:
                        return DataType::F32;
                case CType::F64:
                        return DataType::F64;
                case CType::I8:
                        return DataType::I8;
                case CType::I16:
                        return DataType::I16;
                case CType::I32:
                        return DataType::I32;
                case CType::I64:
                        return DataType::I64;
                case CType::U8:
                case CType::Bool:
                        return DataType::U8;
                case CType::U16:
                        return DataType::U16;
                case CType::U32:
                        return DataType::U32;
                case CType::U64:
                case CType::Ptr:
                        return DataType::U64;
                default:
                        return DataType::I32;
        }
}

void
Gui::newSdkPanel()
{
        auto sp = std::make_shared<SdkPanel>();
        sp->setWindowId(nextSdkWinId_++);
        // LOCAL Variable buffer lookup — search all Variable windows for the named entry.
        sp->onGetVarBuf_ = [this](const std::string &varName) -> void * {
                std::lock_guard<std::mutex> lk(mtxMonitors_);
                for (auto &[_, vw] : vars_) {
                        for (const auto &ve : vw->vars_) {
                                if (ve.port == PortType::LOCAL && ve.name == varName)
                                        return vw->getLocalBuf(varName);
                        }
                }
                return nullptr;
        };
        sp->onVarWritten_ = [this](const std::string &varName) {
                for (auto &[_, vw] : vars_) {
                        for (const auto &ve : vw->vars_) {
                                if (ve.port == PortType::LOCAL && ve.name == varName) {
                                        vw->notifyLocalWrite(varName);
                                        return;
                                }
                        }
                }
        };
        sp->onListLocalVars_ = [this]() -> std::vector<std::string> {
                std::vector<std::string>    names;
                std::lock_guard<std::mutex> lk(mtxMonitors_);
                for (auto &[_, vw] : vars_)
                        for (const auto &ve : vw->vars_)
                                if (ve.port == PortType::LOCAL)
                                        names.push_back(ve.name);
                return names;
        };

        // Store a call's return value into an EXISTING LOCAL variable (the one the user
        // created via the "+" button). Never creates — a no-op if no such variable exists.
        // It plots only if the user also adds it to a monitor (LOCAL→monitor feed).
        sp->onWriteLocalScalar_ = [this](const std::string &name, double value, bool /*isFloat*/) {
                std::lock_guard<std::mutex> lk(mtxMonitors_);
                for (auto &[_, vw] : vars_)
                        for (const auto &ve : vw->vars_)
                                if (ve.name == name) {
                                        vw->setLocalScalar(name, value);
                                        return;
                                }
        };
        // Create a LOCAL variable in the variable manager (the "+" buttons). Scalar when
        // structDecl is null, otherwise a struct variable with the decl's fields. Creates
        // a Variable manager window if none is open. No-op if the name already exists.
        sp->onCreateLocalVar_ = [this](const std::string &name, CType scalarType, const CStructDecl *sd) {
                std::lock_guard<std::mutex> lk(mtxMonitors_);
                if (vars_.empty()) {
                        std::string key = "变量管理器_" + std::to_string(vars_.size());
                        while (vars_.count(key))
                                key += "_";
                        vars_[key] = std::make_shared<Variable>(key);
                        vars_[key]->setTitle("变量管理器 [0]");
                }
                Variable *target = vars_.begin()->second.get();
                if (sd) {
                        std::vector<VarEntry::StructField> fields;
                        for (const auto &f : sd->fields) {
                                auto emit = [&](const std::string &fname, CType ct, size_t off) {
                                        VarEntry::StructField vf{};
                                        strncpy(vf.name, fname.c_str(), sizeof(vf.name) - 1);
                                        vf.type       = ctypeToVarDataType(ct);
                                        vf.byteOffset = (u32)off;
                                        fields.push_back(vf);
                                };
                                if (f.isArray && f.arrayCount > 0) {
                                        size_t esz = ctypeSize(f.arrayElemType);
                                        for (size_t ai = 0; ai < f.arrayCount; ++ai)
                                                emit(f.name + "[" + std::to_string(ai) + "]", f.arrayElemType, f.offset + ai * esz);
                                } else {
                                        emit(f.name, f.type, f.offset);
                                }
                        }
                        target->addLocalStructVar(name, fields, sd->totalSize);
                } else {
                        target->addLocalVar(name, ctypeToVarDataType(scalarType), 8);
                }
        };
        // Wire sequence editor LOCAL variable lookup (same lambda as SdkPanel).
        if (!seqEditor_.onGetLocalBuf_) {
                seqEditor_.onGetLocalBuf_ = [this](const std::string &varName) -> void * {
                        std::lock_guard<std::mutex> lk(mtxMonitors_);
                        for (auto &[_, vw] : vars_) {
                                for (const auto &ve : vw->vars_) {
                                        if (ve.port == PortType::LOCAL && ve.name == varName)
                                                return vw->getLocalBuf(varName);
                                }
                        }
                        return nullptr;
                };
        }
        if (!seqEditor_.onGetLocalVarDataType_) {
                seqEditor_.onGetLocalVarDataType_ = [this](const std::string &varName) -> DataType {
                        std::lock_guard<std::mutex> lk(mtxMonitors_);
                        for (auto &[_, vw] : vars_) {
                                for (const auto &ve : vw->vars_) {
                                        if (ve.port == PortType::LOCAL && ve.name == varName)
                                                return ve.type;
                                }
                        }
                        return DataType::I64;
                };
        }
        if (!seqEditor_.onLocalVarWritten_) {
                seqEditor_.onLocalVarWritten_ = [this](const std::string &varName) {
                        for (auto &[_, vw] : vars_) {
                                for (const auto &ve : vw->vars_) {
                                        if (ve.port == PortType::LOCAL && ve.name == varName) {
                                                vw->notifyLocalWrite(varName);
                                                return;
                                        }
                                }
                        }
                };
        }
        // Wire sequence editor LOCAL variable creation (for "→ Create output vars" button).
        if (!seqEditor_.onAddLocalVar_) {
                seqEditor_.onAddLocalVar_ = [this](const std::string &name, DataType type, size_t bufSize) {
                        std::lock_guard<std::mutex> lk(mtxMonitors_);
                        if (vars_.empty())
                                return;
                        vars_.begin()->second->addLocalVar(name, type, bufSize);
                };
        }
        if (!seqEditor_.onAddLocalStructVar_) {
                seqEditor_.onAddLocalStructVar_ =
                    [this](const std::string &name, const std::vector<SeqStructField> &fields, size_t totalSize) {
                            std::lock_guard<std::mutex> lk(mtxMonitors_);
                            if (vars_.empty())
                                    return;
                            std::vector<VarEntry::StructField> vfs;
                            vfs.reserve(fields.size());
                            for (const auto &f : fields) {
                                    VarEntry::StructField vf{};
                                    strncpy(vf.name, f.name.c_str(), sizeof(vf.name) - 1);
                                    vf.type       = f.type;
                                    vf.byteOffset = f.byteOffset;
                                    vfs.push_back(vf);
                            }
                            vars_.begin()->second->addLocalStructVar(name, vfs, totalSize);
                    };
        }
        seqEditor_.registerSdkPanel(sp);
        sdkPanels_.push_back(std::move(sp));
}
