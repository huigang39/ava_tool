#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h must precede commdlg.h (commdlg.h → prsht.h needs CALLBACK/HWND from windows.h)
#include <windows.h>

#include <GLFW/glfw3.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <wrl/client.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <GLFW/glfw3.h>
#endif

#include "timeops.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ranges>
#include <sstream>

#include "ImGuiNotify.hpp"
#include "cJSON.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "platform/elevation.hpp"
#include "platform/native_dlg.hpp"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/mmap_vector.hpp"
#include "core/sampler.hpp"
#include "core/session_time.hpp"
#include "gui/app_shortcuts.hpp"
#include "gui/fault_decoder.hpp"
#include "gui/gui.hpp"
#include "gui/help_manual.hpp"
#include "gui/i18n.hpp"
#include "gui/interaction_history.hpp"
#include "gui/monitor.hpp"
#include "gui/notification_icons.hpp"
#include "gui/numeric_format.hpp"
#include "gui/platform_fonts.hpp"
#include "gui/plot_style.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_theme.hpp"
#include "gui/unit_conversion_json.hpp"
#include "gui/unit_conversion_ui.hpp"
#include "gui/variable.hpp"
#include "gui/window_corners.hpp"
#include "platform/native_appearance.hpp"
#include "platform/native_dlg.hpp"
#include "version.hpp"
#include <cstdlib>
#include <filesystem>
#include <thread>

std::vector<std::string> Gui::sDroppedFiles_{};

// ── Efficiency tab persistent state ──────────────────────────────────────────
// Shared by drawCalculator(), saveSession(), and loadSession() so the selected
// chs, units, map range, and recorded data survive save/load round-trips
static struct {
        std::string torqueLink, speedLink, volLink, currentLink;
        int         refreshMs  = 200;
        int         torqueUnit = 0, speedUnit = 0, volUnit = 0, currentUnit = 0;
        float       spdMin = 0.f, spdMax = 6000.f, tqMin = 0.f, tqMax = 50.f;
        int         activeTab = 0;
        bool        forceTab  = false;
        struct EffPoint {
                float spdRpm, tqNm, eta;
        };
        std::vector<EffPoint> rawData; // raw recorded (speed, torque, eta) table
} s_eff;

// loadSession() owns mtxMonitors_ while reconstructing every monitor/variable
// SDK restore callbacks run synchronously on that same thread and must not try
// to acquire the non-recursive mutex a second time. Other threads keep their
// normal locking behaviour because this marker is thread-local
static thread_local bool s_monitorMutexOwnedBySessionLoad = false;

class ScopedSessionMonitorLockOwner
{
      public:
        ScopedSessionMonitorLockOwner() : prev_(s_monitorMutexOwnedBySessionLoad) { s_monitorMutexOwnedBySessionLoad = true; }
        ~ScopedSessionMonitorLockOwner() { s_monitorMutexOwnedBySessionLoad = prev_; }

      private:
        bool prev_;
};

static bool
hasAudioExtension(const std::string &path)
{
        std::string ext = std::filesystem::path(path).extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".m4a" || ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".flac" || ext == ".wma";
}

static std::string
sanitizeRecentPath(std::string path)
{
        // A recent.txt accidentally saved as UTF-16 contains embedded NULs.  Such
        // a string is non-empty to C++, but becomes an empty label when passed to
        // ImGui as a C string, causing its root-window empty-ID assertion
        if (path.find('\0') != std::string::npos)
                return {};
        if (path.size() >= 3 && static_cast<unsigned char>(path[0]) == 0xEF && static_cast<unsigned char>(path[1]) == 0xBB &&
            static_cast<unsigned char>(path[2]) == 0xBF)
                path.erase(0, 3);

        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        const auto first    = std::ranges::find_if(path, notSpace);
        if (first == path.end())
                return {};
        const auto prev = std::find_if(path.rbegin(), path.rend(), notSpace).base();
        path            = std::string(first, prev);
        if (std::ranges::any_of(path, [](unsigned char c) { return c < 0x20; }))
                return {};
        return path;
}

static std::string
pathToUtf8(const std::filesystem::path &path)
{
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
}

static std::string
utf8Filename(const std::string &path)
{
        try {
                return pathToUtf8(std::filesystem::u8path(path).filename());
        } catch (const std::filesystem::filesystem_error &) {
                return path;
        }
}

static std::string
utf8Stem(const std::string &path)
{
        try {
                return pathToUtf8(std::filesystem::u8path(path).stem());
        } catch (const std::filesystem::filesystem_error &) {
                return {};
        }
}

static std::string
recentDisplayName(const std::string &path)
{
        try {
                std::string name = pathToUtf8(std::filesystem::u8path(path).filename());
                if (name.empty() || name.find('\0') != std::string::npos)
                        name = path;
                if (!name.empty() && name.find('\0') == std::string::npos)
                        return name;
        } catch (const std::filesystem::filesystem_error &) {
        }
        return "(invalid session)";
}

static bool
recentPathExists(const std::string &path)
{
        if (remote::isUrl(path))
                return true;
        try {
                std::error_code ec;
                return std::filesystem::exists(std::filesystem::u8path(path), ec) && !ec;
        } catch (const std::filesystem::filesystem_error &) {
                return false;
        }
}

void
Gui::glfwErrCb(const i32 err, const char *desc)
{
        LOG_E("Glfw Error %d: %s", err, desc);
}

void
// window
Gui::glfwDropCb(GLFWwindow *, const i32 count, const char **paths)
{
        for (i32 i = 0; i < count; ++i)
                sDroppedFiles_.emplace_back(paths[i]);
}

Gui::Gui(const std::string &initialPath)
{
        dock_close::reset();
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
        ui::interaction::reset();
        ui::InstallNotificationIcons();
        dock_fold::registerSettings();
        ImGuiIO &io     = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        // On macOS Retina the GLFW backend already sets DisplayFramebufferScale = 2,
        // so ImGui renders in logical pixels and the backend up-scales to physical
        // On Windows/Linux the framebuffer equals the window in pixels, so we must
        // bake the DPI scale into the font ourselves
        // uiScale = contentScale / (fb/window), e.g. 1.0 on macOS Retina, 1.5 on Win 150%
        int winW = 1, fbW = 1;
        glfwGetWindowSize(window_, &winW, nullptr);
        glfwGetFramebufferSize(window_, &fbW, nullptr);
        const float fbRatio = (winW > 0) ? static_cast<float>(fbW) / static_cast<float>(winW) : 1.0f;
        const float uiScale = xScale_ / fbRatio;
        uiScale_            = uiScale;

        const float fontSize = ui::design::FontSize(ui::design::body, uiScale);
        ui::LoadPlatformFonts(*io.Fonts, fontSize);

        // Restore the persisted UI language (defaults to English on first launch)
        loadLangSetting();
#ifdef _WIN32
        {
                std::ifstream     file(getAppDir() + "/window_preferences.json");
                const std::string text((std::istreambuf_iterator<char>(file)), {});
                if (auto *root = cJSON_Parse(text.c_str())) {
                        if (auto *val = cJSON_GetObjectItemCaseSensitive(root, "closeBehavior"); cJSON_IsString(val)) {
                                if (std::string(val->valuestring) == "exit")
                                        closeBehavior_ = CloseBehavior::Exit;
                                if (std::string(val->valuestring) == "tray")
                                        closeBehavior_ = CloseBehavior::Tray;
                        }
                        cJSON_Delete(root);
                }
        }
#endif
        remote::loadDefaults(getAppDir() + "/remote_files.json");
        localFileServer_.loadSettings(getAppDir() + "/local_file_server.json");
        if (localFileServer_.config.autoStart)
                localFileServer_.start();
        websiteServer_.config.port = 8091;
        websiteServer_.loadSettings(getAppDir() + "/website_server.json");
        if (websiteServer_.config.autoStart)
                websiteServer_.start();

        {
                // Appearance belongs to this computer, not to a portable .ava session.
                std::ifstream file(getAppDir() + "/color_scheme.txt");
                int           saved = 0;
                if (file >> saved && saved >= 0 && saved < static_cast<int>(ui::ColorScheme::Count))
                        colorScheme_ = static_cast<ui::ColorScheme>(saved);
        }
        loadAppearance();
        ui::ApplyAppStyle(colorScheme_, uiScale_, io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable, appearanceDark_);
#ifdef _WIN32
        native_appearance::applyWindow(glfwGetWin32Window(window_), appearanceDark_);
#endif

        static std::string iniPath = getAppDir() + "/imgui.ini";
        io.IniFilename             = iniPath.c_str();

        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init(glslVer_);

        ImPlot::CreateContext();
        ui::ApplyPlotStyle(appearanceDark_);

        if (!initialPath.empty()) {
                LOG_I("Startup session explicitly requested: %s", initialPath.c_str());
                loadSession(initialPath);
        } else {
                startupSessionPickerPending_ = true;
                startupSessionSelection_     = static_cast<size_t>(std::distance(
                    recentSessions_.begin(),
                    std::ranges::find_if(recentSessions_, [](const std::string &path) { return recentPathExists(path); })));
                if (startupSessionSelection_ >= recentSessions_.size())
                        startupSessionSelection_ = 0;
                LOG_I("Startup waiting for session selection (%zu recent sessions)", recentSessions_.size());
        }

        if (motorProfiles_.empty())
                motorProfiles_.push_back(MotorProfile{});

        JLinkPort::instance().setTraceAddressResolver([this](const u32 address) {
                for (const auto &variableWindow : vars_ | std::views::values) {
                        std::string resolved = variableWindow->resolveFunctionAddress(address);
                        if (!resolved.empty())
                                return resolved;
                }
                return std::string{};
        });
        bkpSramDebugger_.setSymbolResolver([this](const std::string &name, std::uint32_t &address) {
                for (const auto &variableWindow : vars_ | std::views::values) {
                        if (variableWindow->findElfSymbolAddress(name, address))
                                return true;
                }
                return false;
        });
        deviceManager_.setSymbolResolver([this](const std::string &name, std::uint32_t &address) {
                for (const auto &variableWindow : vars_ | std::views::values) {
                        if (variableWindow->findElfSymbolAddress(name, address))
                                return true;
                }
                return false;
        });
        deviceManager_.setOpenSdkHandler([this](int id) {
                for (const auto &panel : sdkPanels_)
                        if (panel->getWinId() == id) {
                                panel->open_ = true;
                                ImGui::SetWindowFocus(panel->title());
                                break;
                        }
        });
        midiTool_.setSymbolResolver([this](const std::string &name, std::uint32_t &address) {
                for (const auto &variableWindow : vars_ | std::views::values) {
                        if (variableWindow->findElfSymbolAddress(name, address))
                                return true;
                }
                return false;
        });

        TutorialGuide::instance().loadState(getAppDir());
        if (isFirstSave_)
                showHome_ = vars_.empty() && monitors_.empty() && sdkPanels_.empty();
        captureSessionBaseline(true);
        // Help retains the explicit tour; the home surface does not cover controls with an automatic overlay.
        TutorialGuide::instance().dismiss();
}

Gui::~Gui()
{
        LOG_I("~Gui()");
        recorder_.finish();
        systemTray_.remove();
        localFileServer_.stop();
        websiteServer_.stop();
        motorControl_.shutdown();
        dsoScope_.shutdown();
        midiTool_.stop();
        JLinkPort::instance().setTraceAddressResolver({});
        bkpSramDebugger_.setSymbolResolver({});
        deviceManager_.setSymbolResolver({});
        midiTool_.setSymbolResolver({});

        // Hide window immediately to give user instant feedback
        if (window_) {
                glfwHideWindow(window_);
        }

        // imgui.ini owns ordinary window/dock layout persistence.  WantSaveIniSettings
        // is not a session-data edit and must not create/overwrite a .ava file
        if (!skipAutoSave_ && isModified_) {
                saveSession();
        }
        rememberLastSession();

        ImPlot::DestroyContext();

        for (auto &[name, monitor] : monitors_)
                monitor->mujocoView_.reset();

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

bool
Gui::minimizeToTray()
{
        if (inTray_)
                return true;
        if (!systemTray_.show()) {
                LOG_W("Cannot create tray icon; keeping ordinary window close/minimize behavior");
                return false;
        }
        trayViewportIds_.clear();
        for (auto *viewport : ImGui::GetPlatformIO().Viewports) {
                auto *window = static_cast<GLFWwindow *>(viewport->PlatformHandle);
                if (window && glfwGetWindowAttrib(window, GLFW_VISIBLE)) {
                        trayViewportIds_.push_back(viewport->ID);
                        glfwHideWindow(window);
                }
        }
        glfwHideWindow(window_);
        glfwSetWindowShouldClose(window_, GLFW_FALSE);
        inTray_ = true;
        return true;
}

void
Gui::restoreFromTray()
{
        if (!inTray_)
                return;
        // A dock/popup viewport can disappear during background data updates
        // Restore only current viewports, rather than retaining native pointers
        for (auto *viewport : ImGui::GetPlatformIO().Viewports)
                if (viewport->PlatformHandle &&
                    std::find(trayViewportIds_.begin(), trayViewportIds_.end(), viewport->ID) != trayViewportIds_.end())
                        glfwShowWindow(static_cast<GLFWwindow *>(viewport->PlatformHandle));
        trayViewportIds_.clear();
        glfwRestoreWindow(window_);
        glfwShowWindow(window_);
        glfwFocusWindow(window_);
        inTray_ = false;
        systemTray_.remove();
}

void
Gui::requestExit()
{
        showCloseChoice_ = false;
        restoreFromTray();
        exitRequested_ = true;
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void
Gui::drawElevationPrompt()
{
#ifdef _WIN32
        if (motorControl_.consumeElevationRequest()) {
                showElevationPrompt_ = true;
                elevationError_.clear();
        }
        const char *title = tr("Device permissions###DeviceElevation", "设备访问权限###DeviceElevation");
        if (showElevationPrompt_) {
                ImGui::OpenPopup(title);
                showElevationPrompt_ = false;
        }
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;
        ImGui::TextUnformatted(tr("Device access was denied. Administrator rights may be required.",
                                  "设备访问被拒绝，当前驱动可能需要管理员权限。"));
        ImGui::TextUnformatted(
            tr("Save the session and restart with administrator rights?", "是否保存当前会话，并以管理员权限重启？"));
        ImGui::TextUnformatted(tr("Device connections will close. The motor will not reconnect or enable automatically.",
                                  "重启将关闭设备连接，电机不会自动重连或使能。"));
        if (!elevationError_.empty())
                ui::ErrorTextWrapped("%s", elevationError_.c_str());
        if (ImGui::Button(tr("Save and restart", "保存并重启"))) {
                // Force a checked save even when connection settings were the only edits
                isModified_ = true;
                saveSession();
                if (isModified_ || isFirstSave_)
                        elevationError_ = tr("Session was not saved; restart cancelled.", "会话未保存，已取消重启。");
                else {
                        const auto path = std::filesystem::absolute(
                            std::filesystem::path(std::u8string(currentSessionPath_.begin(), currentSessionPath_.end())));
                        unsigned long error  = 0;
                        const auto    result = elevation::relaunchAsAdministrator(error, path.wstring());
                        if (result == elevation::Result::Relaunched) {
                                // The new process waits for our exit before loading the session
                                skipAutoSave_ = true;
                                wantsToQuit_  = true;
                                ImGui::CloseCurrentPopup();
                        } else if (result == elevation::Result::Failed) {
                                elevationError_ =
                                    error == ERROR_CANCELLED
                                        ? tr("Authorization cancelled; continue using the current window.",
                                             "已取消授权，可继续使用当前窗口。")
                                        : std::string(tr("Restart failed, Windows error: ", "重启失败，Windows 错误：")) +
                                              std::to_string(error);
                        }
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel", "取消")))
                ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
#endif
}

void
Gui::processWindowActions()
{
        switch (systemTray_.takeAction()) {
                case SystemTray::Action::Restore:
                        restoreFromTray();
                        break;
                case SystemTray::Action::Exit:
                        requestExit();
                        break;
                default:
                        break;
        }
#ifdef _WIN32
        const bool closing = glfwWindowShouldClose(window_) && !exitRequested_ && !wantsToQuit_ && !showQuitModal_;
        if (closing && !localFileServer_.running() && !websiteServer_.running()) {
                // Only keep the application in the background when it is serving files
                requestExit();
        } else if (closing) {
                glfwSetWindowShouldClose(window_, GLFW_FALSE);
                if (!showCloseChoice_) {
                        if (closeBehavior_ == CloseBehavior::Ask) {
                                showCloseChoice_     = true;
                                rememberCloseChoice_ = closeChoiceError_ = false;
                        } else
                                chooseCloseBehavior(closeBehavior_, false);
                }
        }
        if (minimizeRequested_)
                minimizeToTray();
#endif
        minimizeRequested_ = false;
}

void
Gui::updatePlatformWindows()
{
        if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
                return;
        auto &platform   = ImGui::GetPlatformIO();
        auto  showWindow = platform.Platform_ShowWindow;
        if (inTray_)
                platform.Platform_ShowWindow = [](ImGuiViewport *viewport) {
                        if (viewport->PlatformHandle)
                                glfwHideWindow(static_cast<GLFWwindow *>(viewport->PlatformHandle));
                };
        GLFWwindow *context = glfwGetCurrentContext();
        // Required after every rendered frame, even when all windows are hidden
        ImGui::UpdatePlatformWindows();
        for (auto *viewport : platform.Viewports)
                native_appearance::applyWindow(viewport->PlatformHandleRaw, appearanceDark_);
        platform.Platform_ShowWindow = showWindow;
        glfwMakeContextCurrent(context);
        if (inTray_) {
                trayViewportIds_.clear();
                for (auto *viewport : platform.Viewports) {
                        if (!viewport->PlatformHandle)
                                continue;
                        glfwHideWindow(static_cast<GLFWwindow *>(viewport->PlatformHandle));
                        trayViewportIds_.push_back(viewport->ID);
                }
        }
}

void
Gui::saveWindowPreferences()
{
        std::ofstream file(getAppDir() + "/window_preferences.json", std::ios::trunc);
        const char   *behavior = closeBehavior_ == CloseBehavior::Tray   ? "tray"
                                 : closeBehavior_ == CloseBehavior::Exit ? "exit"
                                                                         : "ask";
        file << "{\"closeBehavior\":\"" << behavior << "\"}";
        if (!file)
                LOG_W("Cannot save window preferences");
}

bool
Gui::chooseCloseBehavior(CloseBehavior behavior, bool remember)
{
        if (behavior == CloseBehavior::Tray && !minimizeToTray()) {
                closeChoiceError_ = true;
                showCloseChoice_  = true;
                return false;
        }
        showCloseChoice_ = false;
        if (remember) {
                closeBehavior_ = behavior;
                saveWindowPreferences();
        }
        if (behavior == CloseBehavior::Exit)
                requestExit();
        return true;
}

void
Gui::drawCloseChoicePopup()
{
        if (!showCloseChoice_)
                return;
        const char *title = tr("Close AvA Tool###CloseAction", "关闭 AvA Tool###CloseAction");
        ImGui::OpenPopup(title);
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    tr("Exit the application or minimize it to the system tray?", "退出程序，还是缩小到系统托盘？"));
                ImGui::TextDisabled("%s", tr("File sharing continues while in the tray.", "缩小到托盘后，文件共享继续运行。"));
                ImGui::Checkbox(tr("Remember my choice as the default", "记住我的选择，设为默认行为"), &rememberCloseChoice_);
                if (closeChoiceError_)
                        ImGui::TextWrapped("%s",
                                           tr("Cannot create the tray icon. Try again or exit the application.",
                                              "无法创建托盘图标，请重试或选择退出程序。"));
                ImGui::Separator();
                if (ImGui::Button(tr("Exit application", "退出程序"), ImVec2(120, 0))) {
                        if (chooseCloseBehavior(CloseBehavior::Exit, rememberCloseChoice_))
                                ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Minimize to tray", "缩小到托盘"), ImVec2(120, 0))) {
                        if (chooseCloseBehavior(CloseBehavior::Tray, rememberCloseChoice_))
                                ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        showCloseChoice_ = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::EndPopup();
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
        bool          repaired = false;
        while (std::getline(f, line)) {
                std::string path = sanitizeRecentPath(std::move(line));
                if (path.empty()) {
                        repaired = true;
                        continue;
                }
                if (std::ranges::find(recentSessions_, path) != recentSessions_.end()) {
                        repaired = true;
                        continue;
                }
                recentSessions_.push_back(std::move(path));
                if (recentSessions_.size() == 10) {
                        repaired = f.peek() != std::ifstream::traits_type::eof();
                        break;
                }
        }

        // Older builds recorded only last_session.txt. Keep that file available
        // in the startup chooser without loading it automatically.
        std::ifstream lastFile(getAppDir() + "/last_session.txt");
        if (std::getline(lastFile, line)) {
                std::string last = sanitizeRecentPath(std::move(line));
                if (!last.empty() && std::ranges::find(recentSessions_, last) == recentSessions_.end()) {
                        recentSessions_.insert(recentSessions_.begin(), std::move(last));
                        if (recentSessions_.size() > 10)
                                recentSessions_.resize(10);
                        repaired = true;
                }
        }
        if (repaired)
                saveRecentList();
}

void
Gui::saveRecentList()
{
        std::ofstream f(getAppDir() + "/recent.txt", std::ios::trunc);
        for (const auto &p : recentSessions_)
                f << p << "\n";
}

void
Gui::rememberLastSession()
{
        if (isFirstSave_ || remote::isUrl(currentSessionPath_))
                return;
        std::error_code ec;
        const auto      path = std::filesystem::absolute(std::filesystem::u8path(currentSessionPath_), ec);
        if (ec || !std::filesystem::is_regular_file(path, ec))
                return;
        std::ofstream file(getAppDir() + "/last_session.txt", std::ios::trunc);
        file << pathToUtf8(path) << '\n';
}

void
Gui::addRecent(const std::string &path)
{
        const std::string cleanPath = sanitizeRecentPath(path);
        if (cleanPath.empty())
                return;
        std::string abs = cleanPath;
        try {
                std::error_code ec;
                const auto      absPath = remote::isUrl(cleanPath)
                                              ? std::filesystem::u8path(cleanPath)
                                              : std::filesystem::absolute(std::filesystem::u8path(cleanPath), ec);
                if (!ec && !remote::isUrl(cleanPath))
                        abs = pathToUtf8(absPath);
        } catch (const std::filesystem::filesystem_error &) {
        }

        // Move to front, de-duplicated, capped at 10 entries
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

bool
Gui::chooseStartupSession(const std::string &path)
{
        if (path.empty() || !recentPathExists(path))
                return false;
        startupSessionPickerPending_ = false;
        sessionChange_               = SessionChange::Open;
        sessionChangePath_           = path;
        sessionChangeSource_.clear();
        sessionChangeReady_ = true;
        LOG_I("Startup session selected: %s", path.c_str());
        return true;
}

void
Gui::startBlankStartupSession()
{
        startupSessionPickerPending_ = false;
        showHome_                    = true;
        LOG_I("Startup session selection skipped; using a blank workspace");
}

void
Gui::drawStartupSessionPicker()
{
        if (!startupSessionPickerPending_)
                return;

        const char *title = tr("Open AVA session###StartupSessionPicker", "打开 AVA 会话###StartupSessionPicker");
        if (!ImGui::IsPopupOpen("###StartupSessionPicker"))
                ImGui::OpenPopup("###StartupSessionPicker");
        ImGui::SetNextWindowSizeConstraints(ImVec2(560 * uiScale_, 0), ImVec2(760 * uiScale_, 620 * uiScale_));
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

        ImGui::Spacing();

        std::string sessionToOpen;
        bool        startBlank = false;
        if (recentSessions_.empty()) {
                ImGui::TextDisabled("%s", tr("No recent AVA sessions.", "没有最近使用的 AVA 会话。"));
        } else {
                if (startupSessionSelection_ >= recentSessions_.size())
                        startupSessionSelection_ = 0;
                const auto  &style       = ImGui::GetStyle();
                const float  rowHeight   = ImGui::GetTextLineHeight() * 2.0f + style.FramePadding.y * 2.0f + 4.0f;
                const size_t visibleRows = std::min<size_t>(recentSessions_.size(), 8);
                const float  listHeight  = visibleRows * rowHeight + (visibleRows - 1) * style.ItemSpacing.y + 4.0f;
                ImGui::BeginChild("##startup_recent_sessions", ImVec2(0.0f, listHeight), true);
                for (size_t i = 0; i < recentSessions_.size(); ++i) {
                        const std::string &path      = recentSessions_[i];
                        const bool         available = recentPathExists(path);
                        const bool         selected  = i == startupSessionSelection_;
                        ImGui::PushID(static_cast<int>(i));
                        const float width = ImGui::GetContentRegionAvail().x;
                        ImGui::InvisibleButton("##session", ImVec2(width, rowHeight));
                        const bool hovered = ImGui::IsItemHovered();
                        const auto min     = ImGui::GetItemRectMin();
                        const auto max     = ImGui::GetItemRectMax();
                        auto      *draw    = ImGui::GetWindowDrawList();
                        const auto accent  = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                        const auto base    = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
                        auto       fill    = selected
                                                 ? ui::MixSurface(base, accent, .34f)
                                                 : (available && hovered ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : base);
                        if (!available)
                                fill = ui::MixSurface(base, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), .35f);
                        const float radius = ImGui::GetStyle().FrameRounding;
                        draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), radius);
                        if (selected)
                                draw->AddRect(min, max, ImGui::GetColorU32(accent), radius, 0, 1.0f);

                        const float  inset = style.FramePadding.x + 3.0f;
                        const ImVec4 clipMin(min.x + inset, min.y, max.x - inset, max.y);
                        const auto   textColor = ImGui::GetStyleColorVec4(available ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                        std::string  name      = recentDisplayName(path);
                        if (!available)
                                name += std::string("  (") + tr("missing", "文件不存在") + ")";
                        draw->AddText(ImGui::GetFont(),
                                      ImGui::GetFontSize(),
                                      ImVec2(min.x + inset, min.y + style.FramePadding.y),
                                      ImGui::GetColorU32(textColor),
                                      name.c_str(),
                                      nullptr,
                                      0.0f,
                                      &clipMin);
                        std::string parent = path;
                        if (!remote::isUrl(path)) {
                                try {
                                        parent = pathToUtf8(std::filesystem::u8path(path).parent_path());
                                } catch (const std::filesystem::filesystem_error &) {
                                }
                        }
                        draw->AddText(ImGui::GetFont(),
                                      ImGui::GetFontSize(),
                                      ImVec2(min.x + inset, min.y + style.FramePadding.y + ImGui::GetTextLineHeight()),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                      parent.c_str(),
                                      nullptr,
                                      0.0f,
                                      &clipMin);

                        if (available && hovered)
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if (available && ImGui::IsItemClicked()) {
                                startupSessionSelection_ = i;
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                        sessionToOpen = path;
                        }
                        if (hovered)
                                ImGui::SetTooltip("%s%s", path.c_str(), available ? "" : "  (missing)");
                        ImGui::PopID();
                }
                ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button(tr("Open another file...", "打开其他文件...")))
                sessionToOpen = nativeDlgOpen(tr("Open Session", "打开会话"), {{"Session Files", {"ava", "json"}}});
        ImGui::SameLine();
        if (ImGui::Button(tr("Start blank", "空白工作区")) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                startBlank = true;

        const bool hasSelection =
            startupSessionSelection_ < recentSessions_.size() && recentPathExists(recentSessions_[startupSessionSelection_]);
        const float openWidth = 152.0f * uiScale_;
        const float buttonX   = ImGui::GetWindowContentRegionMax().x - openWidth;
        if (ImGui::GetCursorPosX() < buttonX)
                ImGui::SameLine(buttonX);
        ImGui::BeginDisabled(!hasSelection);
        if (ui::Button(tr("Open selected", "打开选中会话"), ui::BtnStyle::Primary, ImVec2(openWidth, 0.0f)) && hasSelection)
                sessionToOpen = recentSessions_[startupSessionSelection_];
        ImGui::SetItemDefaultFocus();
        ImGui::EndDisabled();

        if (!sessionToOpen.empty() && chooseStartupSession(sessionToOpen)) {
                ImGui::CloseCurrentPopup();
        } else if (startBlank) {
                startBlankStartupSession();
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
        // Skip the startup check if we already checked within the prev 6 hours
        // GitHub's unauthenticated API allows only 60 requests/hour per IP, so an
        // app that checks on every launch can trip a 403 across repeated launches
        std::ifstream f(getAppDir() + "/update_check.txt");
        long long     prev = 0;
        if (f >> prev) {
                const long long now = static_cast<long long>(std::time(nullptr));
                if (now - prev < 6 * 3600)
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

bool
Gui::saveSession(const std::string &path)
{
        if ((path.empty() && (isFirstSave_ || remote::isUrl(currentSessionPath_))) || remote::isUrl(path)) {
                return saveSessionAs();
        }
        u64 start = get_mono_ts_ms();

        std::lock_guard lk(mtxMonitors_);
        std::string     targetPath = path.empty() ? currentSessionPath_ : path;
        cJSON          *root       = cJSON_CreateObject();
        remote::save(root);
        flashTool_.saveSession(root);

        const std::filesystem::path monitorBaseDir = std::filesystem::u8path(targetPath).parent_path();
        auto                        monitorToRel   = [&](const std::string &p) -> std::string {
                if (p.empty() || remote::isUrl(p) || monitorBaseDir.empty())
                        return p;
                std::error_code ec;
                auto            rel = std::filesystem::relative(std::filesystem::u8path(p), monitorBaseDir, ec);
                if (ec || rel.empty())
                        return p;
                const auto utf8 = rel.generic_u8string();
                return std::string(utf8.begin(), utf8.end());
        };

        cJSON *jlink = cJSON_CreateObject();
        JLinkPort::instance().saveSession(jlink);
        cJSON_AddNumberToObject(jlink, "maxHssHz", g_maxHssHz.load(std::memory_order_relaxed));
        cJSON_AddItemToObject(root, "jlink", jlink);

        cJSON_AddBoolToObject(root, "showCalculator", showCalculator_);
        cJSON_AddBoolToObject(root, "showHome", showHome_);

        {
                cJSON *eff = cJSON_CreateObject();
                cJSON_AddNumberToObject(eff, "activeTab", s_eff.activeTab);
                cJSON_AddNumberToObject(eff, "refreshMs", s_eff.refreshMs);
                cJSON_AddStringToObject(eff, "torqueLink", s_eff.torqueLink.c_str());
                cJSON_AddStringToObject(eff, "speedLink", s_eff.speedLink.c_str());
                cJSON_AddStringToObject(eff, "volLink", s_eff.volLink.c_str());
                cJSON_AddStringToObject(eff, "currentLink", s_eff.currentLink.c_str());
                cJSON_AddNumberToObject(eff, "torqueUnit", s_eff.torqueUnit);
                cJSON_AddNumberToObject(eff, "speedUnit", s_eff.speedUnit);
                cJSON_AddNumberToObject(eff, "volUnit", s_eff.volUnit);
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
                cJSON_AddBoolToObject(mObj, "jlinkSamplingPaused", m->isSamplingPaused());
                cJSON_AddNumberToObject(mObj, "maxSampleHz", m->maxSampleHz_);
                cJSON_AddNumberToObject(mObj, "historySeconds", static_cast<f64>(m->historySeconds_));
                cJSON_AddNumberToObject(mObj, "scopeLayout", static_cast<int>(m->scopeLayout()));
                cJSON_AddBoolToObject(mObj, "linkScopeCursor", m->linkScopeCursor_);
                cJSON_AddBoolToObject(mObj, "mujocoEnabled", m->mujocoEnabled_);
                cJSON_AddStringToObject(mObj, "mujocoModelPath", m->mujocoModelPath_.c_str());
                cJSON_AddNumberToObject(mObj, "mujocoHeight", m->mujocoHeight_);
                if (m->mujocoEnabled_) {
                        const auto &control = m->mujocoView_ ? m->mujocoView_->control() : m->mujocoControl_;
                        auto       *obj     = cJSON_AddObjectToObject(mObj, "mujocoControl");
                        cJSON_AddNumberToObject(obj, "mode", static_cast<int>(control.mode));
                        for (auto [name, member] : {std::pair{"positionKp", &ava_mujoco::Control::positionKp},
                                                    {"velocityKp", &ava_mujoco::Control::velocityKp},
                                                    {"velocityKi", &ava_mujoco::Control::velocityKi},
                                                    {"pdKp", &ava_mujoco::Control::pdKp},
                                                    {"pdKd", &ava_mujoco::Control::pdKd},
                                                    {"positionTarget", &ava_mujoco::Control::positionTarget},
                                                    {"velocityTarget", &ava_mujoco::Control::velocityTarget},
                                                    {"velocityFeedforward", &ava_mujoco::Control::velocityFeedforward},
                                                    {"torqueFeedforward", &ava_mujoco::Control::torqueFeedforward}})
                                cJSON_AddNumberToObject(obj, name, control.*member);
                }
                cJSON_AddNumberToObject(mObj, "maxDisplayPoints", static_cast<f64>(m->maxDisplayPoints_));
                cJSON_AddNumberToObject(mObj, "timeAxisFormat", m->timeAxisFormat_);
                if (!m->importSourcePath_.empty()) {
                        const std::string importType = !m->importSourceType_.empty()
                                                           ? m->importSourceType_
                                                           : (hasAudioExtension(m->importSourcePath_) ? "audio" : "csv");
                        cJSON_AddStringToObject(mObj, "importSourcePath", monitorToRel(m->importSourcePath_).c_str());
                        cJSON_AddStringToObject(mObj, "importSourceType", importType.c_str());
                        if (importType == "csv")
                                cJSON_AddNumberToObject(mObj, "importTimeColumn", m->importTimeColumn_);
                        if (importType == "csv")
                                cJSON_AddNumberToObject(mObj, "importTimeUnit", m->importTimeUnit_);
                }

                cJSON *scopesArr = cJSON_CreateArray();
                // Persist scopes in their user-defined display order so it survives a
                // save/load round-trip (the map itself is unordered)
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
                        cJSON_AddBoolToObject(sObj, "jlinkSamplingPaused", s->isPaused());
                        cJSON_AddNumberToObject(sObj, "height", static_cast<f64>(s->getHeight()));
                        cJSON_AddBoolToObject(sObj, "showFft", s->getShowFft());
                        cJSON_AddBoolToObject(sObj, "fftBars", s->getFftBars());
                        cJSON_AddBoolToObject(sObj, "showSidePanel", s->getShowSidePanel());
                        cJSON_AddBoolToObject(sObj, "compareStatistics", s->getCompareStatistics());
                        cJSON_AddNumberToObject(sObj, "fftPoints", static_cast<f64>(s->getFftPoints()));
                        cJSON_AddNumberToObject(sObj, "fftPeakCount", static_cast<f64>(s->getFftPeakCount()));

                        cJSON *tableUi = cJSON_CreateObject();
                        cJSON *widths  = cJSON_CreateArray();
                        for (float width : s->getTableColumnWidths())
                                cJSON_AddItemToArray(widths, cJSON_CreateNumber(width));
                        cJSON_AddItemToObject(tableUi, "columnWidths", widths);
                        cJSON_AddNumberToObject(tableUi, "sortColumn", s->getTableSortColumn());
                        cJSON_AddNumberToObject(tableUi, "sortDirection", s->getTableSortDirection());
                        cJSON_AddBoolToObject(tableUi, "manualOrder", s->usesManualTableOrder());
                        cJSON_AddItemToObject(sObj, "tableUi", tableUi);

                        if (!s->getExpandedGroups().empty()) {
                                cJSON *egArr = cJSON_CreateArray();
                                for (const auto &p : s->getExpandedGroups())
                                        cJSON_AddItemToArray(egArr, cJSON_CreateString(p.c_str()));
                                cJSON_AddItemToObject(sObj, "expandedGroups", egArr);
                        }

                        cJSON *chsArr = cJSON_CreateArray();
                        // Persist chs in insertion order so the order survives a
                        // save/load round-trip (the map itself is unordered)
                        std::vector<MonitorChannel *> orderedChs;
                        for (auto &ch : s->getChannels() | std::views::values)
                                orderedChs.push_back(ch.get());
                        std::sort(orderedChs.begin(), orderedChs.end(), [](const MonitorChannel *a, const MonitorChannel *b) {
                                return a->getOrder() < b->getOrder();
                        });
                        for (auto *ch : orderedChs) {
                                cJSON *chObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(chObj, "name", ch->getName().c_str());
                                cJSON_AddNumberToObject(chObj, "order", static_cast<f64>(ch->getOrder()));
                                cJSON_AddStringToObject(chObj, "type", ch->getType().c_str());
                                char addrHex[32];
                                snprintf(addrHex, sizeof(addrHex), "0x%zX", ch->getAddr());
                                cJSON_AddStringToObject(chObj, "addr", addrHex);
                                cJSON_AddStringToObject(chObj, "device", ch->getDevice().c_str());
                                if (ch->getDevice() == ava_sim::device) {
                                        cJSON_AddNumberToObject(chObj, "simField", ch->getSimField());
                                        cJSON_AddBoolToObject(chObj, "simReferenceControl", true);
                                }
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
                                cJSON_AddNumberToObject(chObj, "displayRadix", ch->getDisplayRadix());
                                unit_conversion::SaveSettings(chObj, "unitConversion", ch->unitConversion_);
                                cJSON_AddNumberToObject(chObj, "bitOffset", ch->getBitOffset());
                                cJSON_AddNumberToObject(chObj, "bitSize", ch->getBitSize());

                                {
                                        const auto &wave    = ch->waveCfgPending_;
                                        cJSON      *waveObj = cJSON_CreateObject();
                                        cJSON_AddNumberToObject(waveObj, "mode", wave.mode.load());
                                        cJSON_AddNumberToObject(waveObj, "type", wave.type.load());
                                        cJSON_AddNumberToObject(waveObj, "frequency", wave.freq.load());
                                        cJSON_AddNumberToObject(waveObj, "amplitude", wave.amp.load());
                                        cJSON_AddNumberToObject(waveObj, "offset", wave.offset.load());
                                        cJSON_AddNumberToObject(waveObj, "duty", wave.duty.load());

                                        static constexpr const char *paramNames[] = {
                                            "frequency", "amplitude", "phase", "offset", "duty"};
                                        cJSON *paramsObj = cJSON_CreateObject();
                                        for (usize i = 0; i < MonitorChannel::WaveCfgPending::PARAM_COUNT; ++i) {
                                                const auto &param    = wave.params[i];
                                                cJSON      *paramObj = cJSON_CreateObject();
                                                cJSON_AddNumberToObject(paramObj, "law", param.law.load());
                                                cJSON_AddNumberToObject(paramObj, "start", param.start.load());
                                                cJSON_AddNumberToObject(paramObj, "end", param.end.load());
                                                cJSON_AddNumberToObject(paramObj, "duration", param.duration.load());
                                                cJSON_AddNumberToObject(paramObj, "step", param.step.load());
                                                cJSON_AddNumberToObject(paramObj, "interval", param.interval.load());
                                                cJSON_AddBoolToObject(paramObj, "repeat", param.repeat.load());
                                                cJSON_AddItemToObject(paramsObj, paramNames[i], paramObj);
                                        }
                                        cJSON_AddItemToObject(waveObj, "params", paramsObj);
                                        cJSON_AddItemToObject(chObj, "waveGenerator", waveObj);
                                }

                                if (ch->isEnum()) {
                                        cJSON *enumsArr = cJSON_CreateArray();
                                        for (const auto &e : ch->getEnums()) {
                                                cJSON *eObj = cJSON_CreateObject();
                                                cJSON_AddStringToObject(eObj, "name", e.name.c_str());
                                                cJSON_AddNumberToObject(eObj, "value", static_cast<f64>(e.val));
                                                cJSON_AddItemToArray(enumsArr, eObj);
                                        }
                                        cJSON_AddItemToObject(chObj, "enums", enumsArr);
                                }
                                cJSON_AddItemToArray(chsArr, chObj);
                        }
                        // The persisted key is "channels" (the pre-rewrite session
                        // format used that name).  Keep it stable so old .ava files
                        // still load their monitor channels
                        cJSON_AddItemToObject(sObj, "channels", chsArr);
                        cJSON_AddItemToArray(scopesArr, sObj);
                }
                cJSON_AddItemToObject(mObj, "scopes", scopesArr);
                cJSON_AddItemToArray(monitorsArr, mObj);
        }
        cJSON_AddItemToObject(root, "monitors", monitorsArr);

        // Store file paths relative to the .ava file so the session stays portable
        // when the project folder is moved. Cross-drive paths fall back to absolute
        const std::filesystem::path baseDir = std::filesystem::u8path(targetPath).parent_path();
        auto                        toRel   = [&](const std::string &p) -> std::string {
                if (p.empty() || remote::isUrl(p) || baseDir.empty())
                        return p;
                std::error_code ec;
                auto            rel = std::filesystem::relative(std::filesystem::u8path(p), baseDir, ec);
                if (ec || rel.empty())
                        return p;
                const auto utf8 = rel.generic_u8string();
                return std::string(utf8.begin(), utf8.end());
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
                cJSON_AddItemToArray(VariableArr, pObj);
        }
        cJSON_AddItemToObject(root, "Variables", VariableArr);

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
        dockSession_.save(root);
        seqEditor_.saveSession(root);

        // Tool page visibility
        cJSON_AddBoolToObject(root, "showBode", bode_.show_);
        bode_.saveSession(root);
        inverterSimulator_.saveSession(root);
        bkpSramDebugger_.saveSession(root);
        cJSON_AddBoolToObject(root, "showAsmViewer", asmViewer_.show_);
        cJSON_AddBoolToObject(root, "showSeqEditor", seqEditor_.isOpen());
        midiTool_.save(root);
        motorControl_.save(root);
        faultDecoder().save(root);
        dsoScope_.save(root);

        // SDK panels
        const std::string sdkBaseDir = pathToUtf8(std::filesystem::u8path(targetPath).parent_path());
        cJSON_AddNumberToObject(root, "nextSdkWinId", nextSdkWinId_);
        cJSON *sdkArr = cJSON_CreateArray();
        for (const auto &sp : sdkPanels_) {
                cJSON *spObj = cJSON_CreateObject();
                sp->save(spObj, sdkBaseDir);
                cJSON_AddItemToArray(sdkArr, spObj);
        }
        cJSON_AddItemToObject(root, "sdkPanels", sdkArr);

        // Device definitions, typed property values, and native-function
        // bindings live with the session. External file paths are stored
        // relative to the .ava file by DeviceManager
        cJSON *deviceManagerObj = cJSON_CreateObject();
        deviceManager_.save(deviceManagerObj, sdkBaseDir);
        cJSON_AddItemToObject(root, "deviceManager", deviceManagerObj);

        u64                   pStart = get_mono_ts_ms();
        char                 *out    = cJSON_Print(root); // formatted/pretty-printed for human readability
        u64                   pEnd   = get_mono_ts_ms();
        std::filesystem::path p(reinterpret_cast<const char8_t *>(targetPath.c_str()));
        // Write beside the destination, then replace it only after a complete flush.
        // A failed save must preserve both the old file and the dirty session.
        auto temp            = p;
        temp                += ".saving-" + std::to_string(get_mono_ts_ms());
        bool          saved  = false;
        std::ofstream ofs;
        if (out)
                ofs.open(temp, std::ios::binary | std::ios::trunc);
        if (ofs && out) {
                ofs << out;
                ofs.close(); // Check the flush before reporting success or restarting
                if (ofs.good()) {
#ifdef _WIN32
                        saved = MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                        std::error_code error;
                        std::filesystem::rename(temp, p, error);
                        saved = !error;
#endif
                }
                if (saved) {
                        for (const auto &v : vars_ | std::views::values)
                                v->clearModified();
                        for (const auto &m : monitors_ | std::views::values)
                                m->clearModified();
                        seqEditor_.clearModified();
                        deviceManager_.clearModified();
                        bode_.clearModified();
                        inverterSimulator_.clearModified();
                        bkpSramDebugger_.clearModified();
                        for (const auto &panel : sdkPanels_)
                                panel->clearModified();
                        JLinkPort::instance().clearConfigModified();
                        currentSessionPath_ = targetPath;
                        isModified_         = false;
                        isFirstSave_        = false;
                        addRecent(targetPath);
                        captureSessionBaseline();
                }
        }
        size_t outLen = out ? strlen(out) : 0;
        cJSON_free(out);
        cJSON_Delete(root);
        if (!saved) {
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                ImGui::InsertNotification(
                    {ImGuiToastType::Error,
                     7000,
                     "%s",
                     tr("Session could not be saved. Check the folder and available space; your changes are still open.",
                        "会话保存失败。请检查目录和剩余空间；当前更改仍保留在应用中。")});
        } else {
                ImGui::InsertNotification(
                    {ImGuiToastType::Success, 3500, "%s", tr("Session saved successfully.", "会话保存成功。")});
        }

        u64 end = get_mono_ts_ms();
        LOG_I("SaveSession Profile: Total %llu ms, JSON Print %llu ms, Size %zu bytes", end - start, pEnd - pStart, outLen);
        return saved;
}

bool
Gui::saveSessionAs()
{
        std::string path = nativeDlgSave("Save Session As", {{"Session Files", {"ava"}}}, "session.ava");
        if (path.empty())
                return false;
        if (std::filesystem::u8path(path).extension() != ".ava")
                path += ".ava";
        return saveSession(path);
}

void
Gui::loadSession(const std::string &path, const std::string &sourceUrl)
{
        u64         start      = get_mono_ts_ms();
        std::string targetPath = path.empty() ? currentSessionPath_ : path;
        if (remote::isUrl(targetPath)) {
                if (remoteSessionDownload_.valid()) {
                        ImGui::InsertNotification(
                            {ImGuiToastType::Info,
                             3000,
                             "%s",
                             tr("A remote session is already downloading.", "正在下载远程会话，请稍候。")});
                        return;
                }
                remoteSessionUrl_ = targetPath;
                const auto config = remote::settings();
                remoteSessionDownload_ =
                    std::async(std::launch::async, [targetPath, config] { return remote::download(targetPath, config); });
                return;
        }
        remoteSessionUrl_.clear(); // A local open supersedes a pending remote open
        std::filesystem::path p(reinterpret_cast<const char8_t *>(targetPath.c_str()));
        std::ifstream         ifs(p);
        if (!ifs.is_open()) {
                ImGui::InsertNotification({ImGuiToastType::Error,
                                           5000,
                                           "%s",
                                           tr("Cannot open this session. Check that the file still exists and is readable.",
                                              "无法打开会话，请检查文件是否存在且可读取。")});
                return;
        }

        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string content = ss.str();
        u64               fEnd    = get_mono_ts_ms();
        if (content.empty()) {
                ImGui::InsertNotification(
                    {ImGuiToastType::Error, 5000, "%s", tr("This session file is empty.", "会话文件为空。")});
                return;
        }

        cJSON *root = cJSON_Parse(content.c_str());
        u64    pEnd = get_mono_ts_ms();
        if (!cJSON_IsObject(root)) {
                cJSON_Delete(root);
                ImGui::InsertNotification(
                    {ImGuiToastType::Error, 5000, "%s", tr("Invalid session file.", "会话文件格式无效。")});
                return;
        }
        isFirstSave_        = !sourceUrl.empty();
        currentSessionPath_ = sourceUrl.empty() ? targetPath : sourceUrl;
        addRecent(currentSessionPath_);
        remote::load(root);
        flashTool_.loadSession(root);

        // Stop sequence execution before clearing its SDK panels and LOCAL buffers
        seqEditor_.stop();
        // Stop SDK workers before taking mtxMonitors_. A worker may itself be
        // waiting to publish LOCAL data under that mutex, so clearing panels
        // after locking can deadlock while joining its thread
        sdkPanels_.clear();
        std::lock_guard               lk(mtxMonitors_);
        ScopedSessionMonitorLockOwner monitorLockOwner;

        u64 monitorStart = get_mono_ts_ms();
        ui::interaction::reset();
        monitors_.clear();
        vars_.clear();
        bool sessionMigrated = false;

        // Resolve file paths that were stored relative to the .ava file back to
        // absolute paths. Already-absolute paths (old sessions) are used as-is
        const std::filesystem::path baseDir = std::filesystem::u8path(targetPath).parent_path();
        auto                        toAbs   = [&](const std::string &p) -> std::string {
                if (p.empty() || remote::isUrl(p) || baseDir.empty())
                        return p;
                std::filesystem::path fp = std::filesystem::u8path(p);
                if (fp.is_absolute())
                        return p;
                if (!sourceUrl.empty())
                        return remote::resolve(sourceUrl, p);
                return pathToUtf8((baseDir / fp).lexically_normal());
        };

        struct PendingImportSource {
                std::string monitorName;
                std::string path;
                std::string type;
                int         timeColumn{-2};
                int         timeUnit{0};
        };
        std::vector<PendingImportSource> pendingImportSources;

        if (const cJSON *jlink = cJSON_GetObjectItem(root, "jlink")) {
                JLinkPort::instance().loadSession(jlink);
                if (const cJSON *hz = cJSON_GetObjectItem(jlink, "maxHssHz"); cJSON_IsNumber(hz))
                        g_maxHssHz.store(hz->valueint, std::memory_order_relaxed);
        }

        if (const cJSON *home = cJSON_GetObjectItem(root, "showHome"); cJSON_IsBool(home))
                showHome_ = cJSON_IsTrue(home);
        else
                showHome_ = false;
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
                // Accept both the current "volLink"/"volUnit" names and the
                // pre-rewrite "voltageLink"/"voltageUnit" names so old sessions
                // keep their efficiency-tab voltage channel selection
                getStr("volLink", s_eff.volLink);
                if (s_eff.volLink.empty())
                        getStr("voltageLink", s_eff.volLink);
                getStr("currentLink", s_eff.currentLink);
                getInt("refreshMs", s_eff.refreshMs);
                getInt("torqueUnit", s_eff.torqueUnit);
                getInt("speedUnit", s_eff.speedUnit);
                getInt("volUnit", s_eff.volUnit);
                if (s_eff.volUnit == 0)
                        getInt("voltageUnit", s_eff.volUnit);
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
                        if (mName.empty()) {
                                size_t suffix = monitors_.size();
                                do {
                                        mName = "monitor_" + std::to_string(suffix++);
                                } while (monitors_.find(mName) != monitors_.end());
                                sessionMigrated = true;
                        }
                        monitors_[mName] = std::make_shared<Monitor>(mName);
                        Monitor *monitor = monitors_[mName].get();
                        if (const auto *layout = cJSON_GetObjectItem(mItem, "scopeLayout"); cJSON_IsNumber(layout))
                                monitor->setScopeLayout(layout->valueint == 1 ? Monitor::ScopeLayout::Grid
                                                                              : Monitor::ScopeLayout::Stacked);
                        const auto *mujocoEnabled = cJSON_GetObjectItem(mItem, "mujocoEnabled");
                        monitor->mujocoEnabled_   = mujocoEnabled ? cJSON_IsTrue(mujocoEnabled) : mName.starts_with("MuJoCo");
                        if (const auto *path = cJSON_GetObjectItem(mItem, "mujocoModelPath"); cJSON_IsString(path))
                                monitor->mujocoModelPath_ = path->valuestring;
                        if (const auto *height = cJSON_GetObjectItem(mItem, "mujocoHeight");
                            cJSON_IsNumber(height) && std::isfinite(height->valuedouble))
                                monitor->mujocoHeight_ = std::clamp(static_cast<float>(height->valuedouble), 120.0f, 1000.0f);

                        if (auto *obj = cJSON_GetObjectItemCaseSensitive(mItem, "mujocoControl"); cJSON_IsObject(obj)) {
                                auto &control = monitor->mujocoControl_;
                                if (auto *mode = cJSON_GetObjectItemCaseSensitive(obj, "mode"); cJSON_IsNumber(mode))
                                        control.mode = static_cast<ava_mujoco::Mode>(mode->valueint);
                                for (auto [name, member] : {std::pair{"positionKp", &ava_mujoco::Control::positionKp},
                                                            {"velocityKp", &ava_mujoco::Control::velocityKp},
                                                            {"velocityKi", &ava_mujoco::Control::velocityKi},
                                                            {"pdKp", &ava_mujoco::Control::pdKp},
                                                            {"pdKd", &ava_mujoco::Control::pdKd},
                                                            {"positionTarget", &ava_mujoco::Control::positionTarget},
                                                            {"velocityTarget", &ava_mujoco::Control::velocityTarget},
                                                            {"velocityFeedforward", &ava_mujoco::Control::velocityFeedforward},
                                                            {"torqueFeedforward", &ava_mujoco::Control::torqueFeedforward}})
                                        if (auto *val = cJSON_GetObjectItemCaseSensitive(obj, name); cJSON_IsNumber(val))
                                                control.*member = val->valuedouble;
                                if (!ava_mujoco::valid(control)) {
                                        control         = {};
                                        sessionMigrated = true;
                                }
                        }

                        if (const cJSON *titleItem = cJSON_GetObjectItem(mItem, "title");
                            cJSON_IsString(titleItem) && titleItem->valuestring[0] != '\0') {
                                std::string title = titleItem->valuestring;
                                for (const auto *prefix :
                                     {"变量监视器_", "变量监视器 ", "监视器 ", "Variable Monitor ", "Monitor "}) {
                                        const std::string oldPrefix = prefix;
                                        if (title.starts_with(oldPrefix) && title.size() > oldPrefix.size() &&
                                            std::all_of(title.begin() + oldPrefix.size(), title.end(), [](unsigned char c) {
                                                    return c >= '0' && c <= '9';
                                            }))
                                                title = std::string(tr("Variable Recorder ", "变量记录器 ")) +
                                                        title.substr(oldPrefix.size());
                                }
                                monitor->setTitle(title);
                                monitor->clearModified(); // loading is not a user edit
                        }

                        monitor->linkScopeCursor_ = cJSON_IsTrue(cJSON_GetObjectItem(mItem, "linkScopeCursor"));
                        if (const cJSON *hSec = cJSON_GetObjectItem(mItem, "historySeconds"); cJSON_IsNumber(hSec))
                                monitor->historySeconds_ = static_cast<f32>(hSec->valuedouble);

                        if (const cJSON *mHz = cJSON_GetObjectItem(mItem, "maxSampleHz"); cJSON_IsNumber(mHz))
                                monitor->maxSampleHz_ = mHz->valueint;
                        if (const cJSON *radix = cJSON_GetObjectItem(mItem, "displayRadix"); cJSON_IsNumber(radix))
                                monitor->displayRadix_ = normalizeDisplayRadix(radix->valueint);

                        if (const cJSON *mPts = cJSON_GetObjectItem(mItem, "maxDisplayPoints"); cJSON_IsNumber(mPts))
                                monitor->maxDisplayPoints_ = static_cast<u32>(mPts->valueint);

                        if (const cJSON *timeFormat = cJSON_GetObjectItem(mItem, "timeAxisFormat"); cJSON_IsNumber(timeFormat))
                                monitor->timeAxisFormat_ = std::clamp(timeFormat->valueint, -1, 1);

                        if (const cJSON *sMode = cJSON_GetObjectItem(mItem, "samplingMode"); cJSON_IsString(sMode)) {
                                if (std::string(sMode->valuestring) == "POLL")
                                        monitor->samplingMode_ = Monitor::SamplingMode::POLL;
                                else
                                        monitor->samplingMode_ = Monitor::SamplingMode::HSS;
                        }

                        if (const cJSON *paused = cJSON_GetObjectItem(mItem, "jlinkSamplingPaused"); cJSON_IsBool(paused))
                                monitor->samplingPaused_.store(cJSON_IsTrue(paused), std::memory_order_release);

                        if (const cJSON *srcPath = cJSON_GetObjectItem(mItem, "importSourcePath");
                            cJSON_IsString(srcPath) && srcPath->valuestring[0] != '\0') {
                                monitor->importSourcePath_ = toAbs(srcPath->valuestring);
                                if (const cJSON *srcType = cJSON_GetObjectItem(mItem, "importSourceType");
                                    cJSON_IsString(srcType) && srcType->valuestring[0] != '\0') {
                                        monitor->importSourceType_ = srcType->valuestring;
                                } else {
                                        monitor->importSourceType_ =
                                            hasAudioExtension(monitor->importSourcePath_) ? "audio" : "csv";
                                }
                                if (const cJSON *timeCol = cJSON_GetObjectItem(mItem, "importTimeColumn");
                                    cJSON_IsNumber(timeCol))
                                        monitor->importTimeColumn_ = timeCol->valueint;
                                if (const cJSON *timeUnit = cJSON_GetObjectItem(mItem, "importTimeUnit");
                                    cJSON_IsNumber(timeUnit))
                                        monitor->importTimeUnit_ = timeUnit->valueint;
                                pendingImportSources.push_back({mName,
                                                                monitor->importSourcePath_,
                                                                monitor->importSourceType_,
                                                                monitor->importTimeColumn_,
                                                                monitor->importTimeUnit_});
                        }

                        const cJSON *scopesArr = cJSON_GetObjectItem(mItem, "scopes");
                        if (!cJSON_IsArray(scopesArr))
                                continue;
                        for (const cJSON *sItem = scopesArr->child; sItem; sItem = sItem->next) {
                                const cJSON *snItem = cJSON_GetObjectItem(sItem, "name");
                                if (!cJSON_IsString(snItem))
                                        continue;
                                std::string sName = snItem->valuestring;
                                if (sName.empty()) {
                                        size_t suffix = monitor->getScopes().size();
                                        do {
                                                sName = "scope_" + std::to_string(suffix++);
                                        } while (monitor->getScopes().find(sName) != monitor->getScopes().end());
                                        // Keep loading the legacy session instead of crashing or
                                        // silently losing the scope and all of its chs
                                        sessionMigrated = true;
                                }
                                if (monitor->addScope(sName) != 0)
                                        continue;
                                MonitorScope *scope = monitor->getScopes()[sName].get();

                                // Restore the saved display order (older sessions without it
                                // keep the default insertion order from addScope)
                                if (const cJSON *orderItem = cJSON_GetObjectItem(sItem, "order"); cJSON_IsNumber(orderItem)) {
                                        const i64 ord = static_cast<i64>(orderItem->valuedouble);
                                        scope->setOrder(ord);
                                        monitor->noteScopeOrder(ord);
                                }

                                // Restore the saved label alias
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
                                if (const cJSON *paused = cJSON_GetObjectItem(sItem, "jlinkSamplingPaused");
                                    cJSON_IsBool(paused))
                                        scope->setPaused(cJSON_IsTrue(paused));
                                if (const cJSON *hItem = cJSON_GetObjectItem(sItem, "height"); cJSON_IsNumber(hItem))
                                        scope->getHeight() = static_cast<f32>(hItem->valuedouble);

                                if (const cJSON *fftShowItem = cJSON_GetObjectItem(sItem, "showFft"); cJSON_IsBool(fftShowItem))
                                        scope->getShowFft() = cJSON_IsTrue(fftShowItem);

                                if (const cJSON *fftBarsItem = cJSON_GetObjectItem(sItem, "fftBars"); cJSON_IsBool(fftBarsItem))
                                        scope->getFftBars() = cJSON_IsTrue(fftBarsItem);

                                if (const cJSON *sidePanelItem = cJSON_GetObjectItem(sItem, "showSidePanel");
                                    cJSON_IsBool(sidePanelItem))
                                        scope->getShowSidePanel() = cJSON_IsTrue(sidePanelItem);

                                if (const cJSON *compare = cJSON_GetObjectItem(sItem, "compareStatistics");
                                    cJSON_IsBool(compare))
                                        scope->getCompareStatistics() = cJSON_IsTrue(compare);

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

                                if (const cJSON *tableUi = cJSON_GetObjectItem(sItem, "tableUi"); cJSON_IsObject(tableUi)) {
                                        std::array<float, 6> widths{};
                                        if (const cJSON *items = cJSON_GetObjectItem(tableUi, "columnWidths");
                                            cJSON_IsArray(items)) {
                                                for (int i = 0; i < 6; ++i) {
                                                        const cJSON *item = cJSON_GetArrayItem(items, i);
                                                        if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
                                                                widths[i] = static_cast<float>(item->valuedouble);
                                                }
                                        }
                                        int sortColumn = -1;
                                        if (const cJSON *item = cJSON_GetObjectItem(tableUi, "sortColumn");
                                            cJSON_IsNumber(item) && item->valueint >= -1 && item->valueint < 5)
                                                sortColumn = item->valueint;
                                        int sortDirection = ImGuiSortDirection_Ascending;
                                        if (const cJSON *item = cJSON_GetObjectItem(tableUi, "sortDirection");
                                            cJSON_IsNumber(item) && (item->valueint == ImGuiSortDirection_Ascending ||
                                                                     item->valueint == ImGuiSortDirection_Descending))
                                                sortDirection = item->valueint;
                                        bool manualOrder = sortColumn < 0;
                                        if (const cJSON *item = cJSON_GetObjectItem(tableUi, "manualOrder"); cJSON_IsBool(item))
                                                manualOrder = cJSON_IsTrue(item);
                                        scope->setTableUiState(widths, sortColumn, sortDirection, manualOrder);
                                }

                                if (const cJSON *egArr = cJSON_GetObjectItem(sItem, "expandedGroups"); cJSON_IsArray(egArr)) {
                                        for (int k = 0; k < cJSON_GetArraySize(egArr); ++k) {
                                                const cJSON *e = cJSON_GetArrayItem(egArr, k);
                                                if (cJSON_IsString(e))
                                                        scope->getExpandedGroups().insert(e->valuestring);
                                        }
                                }

                                // Accept both the canonical "channels" key and the
                                // short-lived "chs" key emitted by an intermediate
                                // build, so neither format silently drops channels
                                const cJSON *chsArr = cJSON_GetObjectItem(sItem, "channels");
                                if (!cJSON_IsArray(chsArr))
                                        chsArr = cJSON_GetObjectItem(sItem, "chs");
                                if (!cJSON_IsArray(chsArr))
                                        continue;
                                for (const cJSON *chItem = chsArr->child; chItem; chItem = chItem->next) {
                                        const cJSON *cnItem = cJSON_GetObjectItem(chItem, "name");
                                        if (!cJSON_IsString(cnItem))
                                                continue;
                                        std::string chName = cnItem->valuestring;
                                        if (chName.empty()) {
                                                size_t suffix = scope->getChannels().size();
                                                do {
                                                        chName = "ch_" + std::to_string(suffix++);
                                                } while (scope->getChannels().find(chName) != scope->getChannels().end());
                                                sessionMigrated = true;
                                        }
                                        if (scope->addChannel(chName) != 0)
                                                continue;
                                        MonitorChannel *ch = scope->findChannel(chName);
                                        if (!ch)
                                                continue;
                                        ch->historySeconds_ = monitor->historySeconds_;
                                        // Older sessions stored one monitor-wide radix. Keep it
                                        // only as the migration default for chs without one
                                        ch->setDisplayRadix(monitor->displayRadix_);
                                        if (const cJSON *orderItem = cJSON_GetObjectItem(chItem, "order");
                                            cJSON_IsNumber(orderItem)) {
                                                const i64 order = static_cast<i64>(orderItem->valuedouble);
                                                ch->setOrder(order);
                                                scope->noteChannelOrder(order);
                                        }

                                        if (const cJSON *sn = cJSON_GetObjectItem(chItem, "symbolName"); cJSON_IsString(sn))
                                                ch->getSymbolName() = sn->valuestring;

                                        if (const cJSON *devItem = cJSON_GetObjectItem(chItem, "device");
                                            cJSON_IsString(devItem)) {
                                                std::string d = devItem->valuestring;
                                                if (d.empty())
                                                        d = "JLINK";
                                                ch->setDevice(d);
                                                LOG_I("Loaded ch '%s' with device '%s'",
                                                      ch->getSymbolName().c_str(),
                                                      ch->getDevice().c_str());
                                        } else {
                                                ch->setDevice("JLINK");
                                                LOG_I("Loaded ch '%s' with device 'JLINK' (default)",
                                                      ch->getSymbolName().c_str());
                                        }

                                        if (const cJSON *t = cJSON_GetObjectItem(chItem, "type"); cJSON_IsString(t))
                                                ch->setType(t->valuestring);
                                        ch->setAddrUnknown(true);
                                        if (const cJSON *a = cJSON_GetObjectItem(chItem, "addr")) {
                                                usize address      = 0;
                                                bool  validAddress = false;
                                                if (cJSON_IsString(a)) {
                                                        const char *begin = a->valuestring;
                                                        const char *end   = begin + strlen(begin);
                                                        if (end - begin >= 2 && begin[0] == '0' &&
                                                            (begin[1] == 'x' || begin[1] == 'X'))
                                                                begin += 2;
                                                        const auto parsed = std::from_chars(begin, end, address, 16);
                                                        validAddress      = parsed.ec == std::errc{} && parsed.ptr == end;
                                                } else if (cJSON_IsNumber(a) && std::isfinite(a->valuedouble) &&
                                                           a->valuedouble >= 0.0 &&
                                                           a->valuedouble < std::ldexp(1.0, sizeof(usize) * 8) &&
                                                           std::floor(a->valuedouble) == a->valuedouble) {
                                                        address      = static_cast<usize>(a->valuedouble);
                                                        validAddress = true;
                                                }
                                                if (validAddress) {
                                                        ch->setAddr(address);
                                                        ch->setAddrUnknown(false);
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
                                        if (const cJSON *radix = cJSON_GetObjectItem(chItem, "displayRadix");
                                            cJSON_IsNumber(radix))
                                                ch->setDisplayRadix(radix->valueint);
                                        ch->unitConversion_ = unit_conversion::LoadSettings(chItem, "unitConversion");
                                        if (const cJSON *bo = cJSON_GetObjectItem(chItem, "bitOffset"); cJSON_IsNumber(bo))
                                                ch->setBitOffset(static_cast<u32>(bo->valueint));
                                        if (const cJSON *bsz = cJSON_GetObjectItem(chItem, "bitSize"); cJSON_IsNumber(bsz))
                                                ch->setBitSize(static_cast<u32>(bsz->valueint));

                                        if (const cJSON *waveObj = cJSON_GetObjectItem(chItem, "waveGenerator");
                                            cJSON_IsObject(waveObj)) {
                                                auto &wave = ch->waveCfgPending_;
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "mode"); cJSON_IsNumber(v))
                                                        wave.mode.store(std::clamp(v->valueint, 0, 1));
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "type"); cJSON_IsNumber(v))
                                                        wave.type.store(std::clamp(v->valueint,
                                                                                   static_cast<int>(WAVE_TYPE_SINE),
                                                                                   static_cast<int>(WAVE_TYPE_TRIANGLE)));
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "frequency");
                                                    cJSON_IsNumber(v))
                                                        wave.freq.store(static_cast<f32>(v->valuedouble));
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "amplitude");
                                                    cJSON_IsNumber(v))
                                                        wave.amp.store(static_cast<f32>(v->valuedouble));
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "offset"); cJSON_IsNumber(v))
                                                        wave.offset.store(static_cast<f32>(v->valuedouble));
                                                if (const cJSON *v = cJSON_GetObjectItem(waveObj, "duty"); cJSON_IsNumber(v))
                                                        wave.duty.store(static_cast<f32>(v->valuedouble));

                                                static constexpr const char *paramNames[] = {
                                                    "frequency", "amplitude", "phase", "offset", "duty"};
                                                if (const cJSON *paramsObj = cJSON_GetObjectItem(waveObj, "params");
                                                    cJSON_IsObject(paramsObj)) {
                                                        for (usize i = 0; i < MonitorChannel::WaveCfgPending::PARAM_COUNT;
                                                             ++i) {
                                                                const cJSON *paramObj =
                                                                    cJSON_GetObjectItem(paramsObj, paramNames[i]);
                                                                if (!cJSON_IsObject(paramObj))
                                                                        continue;
                                                                auto &param = wave.params[i];
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "law");
                                                                    cJSON_IsNumber(v))
                                                                        param.law.store(std::clamp(v->valueint, 0, 2));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "start");
                                                                    cJSON_IsNumber(v))
                                                                        param.start.store(static_cast<f32>(v->valuedouble));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "end");
                                                                    cJSON_IsNumber(v))
                                                                        param.end.store(static_cast<f32>(v->valuedouble));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "duration");
                                                                    cJSON_IsNumber(v))
                                                                        param.duration.store(std::max(
                                                                            0.000001f, static_cast<f32>(v->valuedouble)));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "step");
                                                                    cJSON_IsNumber(v))
                                                                        param.step.store(static_cast<f32>(v->valuedouble));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "interval");
                                                                    cJSON_IsNumber(v))
                                                                        param.interval.store(std::max(
                                                                            0.000001f, static_cast<f32>(v->valuedouble)));
                                                                if (const cJSON *v = cJSON_GetObjectItem(paramObj, "repeat");
                                                                    cJSON_IsBool(v))
                                                                        param.repeat.store(cJSON_IsTrue(v));
                                                        }
                                                }
                                                wave.dynamicInitialized.store(true);
                                                wave.dirty.store(true);
                                                wave.restart.store(true);
                                        }

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
                                        if (ch->getDevice() == ava_sim::device) {
                                                const cJSON *field = cJSON_GetObjectItem(chItem, "simField");
                                                ch->setSimField(cJSON_IsNumber(field)
                                                                    ? static_cast<unsigned>(std::clamp(
                                                                          field->valueint, 0, int(ava_sim::fieldCount - 1)))
                                                                    : 0);
                                                ch->setType("f32");
                                                if (!cJSON_IsTrue(cJSON_GetObjectItem(chItem, "simReferenceControl"))) {
                                                        // Older simulation sessions made all fields read-only
                                                        ch->setWritable(ava_sim::writableField(ch->getSimField()));
                                                        sessionMigrated = true;
                                                } else {
                                                        const cJSON *writable = cJSON_GetObjectItem(chItem, "writable");
                                                        ch->setWritable(cJSON_IsTrue(writable));
                                                }
                                                ch->setAddrUnknown(false);
                                        }
                                        if (ch->getDevice() == "SHM")
                                                MonitorScope::shmInit(*ch);
                                }
                        }
                }
        }

        // Keep generated PD controls separate from reference targets, including
        // sessions saved by the previous combined Targets/PD template
        for (auto &monitor : monitors_ | std::views::values) {
                if (!monitor)
                        continue;
                std::string region;
                bool        hasKp = false, hasKd = false, combined = false;
                for (auto &[scopeName, scope] : monitor->getScopes())
                        for (auto &ch : scope->getChannels() | std::views::values) {
                                if (ch->getDevice() != ava_sim::device)
                                        continue;
                                if (region.empty())
                                        region = ch->getShmRegionName();
                                hasKp    |= ch->getSimField() == 6;
                                hasKd    |= ch->getSimField() == 7;
                                combined |= scopeName == "Targets" && ch->getSimField() >= 6;
                        }
                if (region.empty() || (hasKp && hasKd && !combined))
                        continue;
                if (!monitor->getScopes().count("PD Parameters"))
                        monitor->addScope("PD Parameters");
                auto scope = monitor->getScopes().at("PD Parameters");
                scope->setLabel(tr("PD Parameters", "PD 参数"));
                scope->setDraw(MonitorScope::DrawEnum::TABLE);
                if (combined) {
                        auto targets = monitor->getScopes().at("Targets");
                        targets->setLabel(tr("Reference Targets", "目标设置"));
                        auto &channels = targets->getChannels();
                        for (auto it = channels.begin(); it != channels.end();) {
                                auto ch = it->second;
                                if (ch->getDevice() != ava_sim::device || ch->getSimField() < 6) {
                                        ++it;
                                        continue;
                                }
                                std::string name = it->first;
                                while (scope->findChannel(name))
                                        name += "_";
                                scope->getChannels()[name] = ch;
                                scope->noteChannelOrder(ch->getOrder());
                                it = channels.erase(it);
                        }
                }
                for (unsigned field : {6u, 7u}) {
                        if ((field == 6 && hasKp) || (field == 7 && hasKd))
                                continue;
                        std::string name = ava_sim::fields[field];
                        while (scope->findChannel(name))
                                name += "_";
                        scope->addChannel(name);
                        auto *ch = scope->findChannel(name);
                        ch->setDevice(ava_sim::device);
                        ch->setType("f32");
                        ch->setShmRegionName(region);
                        ch->setSimField(field);
                        ch->setWritable(true);
                        ch->historySeconds_ = monitor->historySeconds_;
                }
                sessionMigrated = true;
        }

        // The top-bar switch represents the aggregate monitor state. Mixed
        // per-monitor states intentionally show as "Pause all"
        bool hasMonitor        = false;
        bool allMonitorsPaused = true;
        for (const auto &monitor : monitors_ | std::views::values) {
                if (!monitor)
                        continue;
                hasMonitor         = true;
                allMonitorsPaused &= monitor->isSamplingPaused();
        }
        g_jlinkSamplingPaused.store(hasMonitor && allMonitorsPaused, std::memory_order_release);

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
                        v->load(pItem);
                        if (!elf.empty())
                                v->loadElf(elf);
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
        dock_close::reset();
        dockSession_.load(root);

        seqEditor_.loadSession(root);

        // Tool page visibility
        if (const cJSON *v = cJSON_GetObjectItem(root, "showBode"); cJSON_IsBool(v))
                bode_.show_ = cJSON_IsTrue(v);
        bode_.loadSession(root);
        inverterSimulator_.loadSession(root);
        bkpSramDebugger_.loadSession(root);
        if (const cJSON *v = cJSON_GetObjectItem(root, "showAsmViewer"); cJSON_IsBool(v))
                asmViewer_.show_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(root, "showSeqEditor"); cJSON_IsBool(v))
                seqEditor_.setOpen(cJSON_IsTrue(v));
        midiTool_.load(root);
        motorControl_.load(root);
        faultDecoder().load(root);
        dsoScope_.load(root);

        // SDK panels
        const std::string sdkBaseDir = pathToUtf8(std::filesystem::u8path(targetPath).parent_path());
        if (const cJSON *sdkArr = cJSON_GetObjectItem(root, "sdkPanels"); cJSON_IsArray(sdkArr)) {
                for (const cJSON *spObj = sdkArr->child; spObj; spObj = spObj->next) {
                        newSdkPanel();
                        sdkPanels_.back()->load(spObj, sdkBaseDir);
                }
        }
        // nextSdkWinId_ must be > any restored winId so new panels don't collide
        if (const cJSON *n = cJSON_GetObjectItem(root, "nextSdkWinId"); cJSON_IsNumber(n))
                nextSdkWinId_ = std::max(nextSdkWinId_, n->valueint);

        bool deviceManagerLoaded = true;
        if (const cJSON *dm = cJSON_GetObjectItem(root, "deviceManager"); cJSON_IsObject(dm))
                deviceManagerLoaded = deviceManager_.load(dm, sdkBaseDir);
        else
                deviceManager_.clear();
        deviceManager_.clearModified();
        // Loading, compatibility migration and unavailable external bindings are not
        // user edits.  Keep the loaded session clean; a later real edit will save the
        // migrated representation naturally
        isModified_ = false;
        if (!deviceManagerLoaded)
                LOG_E("Device manager session data was only partially restored; session remains unmodified");
        if (sessionMigrated)
                LOG_I("Session compatibility migration applied in memory; session remains unmodified");

        for (auto &pair : monitors_)
                pair.second->clearModified();

        for (const auto &src : pendingImportSources) {
                std::error_code ec;
                if (!std::filesystem::exists(src.path, ec)) {
                        LOG_E("Session import source missing for monitor '%s': %s", src.monitorName.c_str(), src.path.c_str());
                        continue;
                }

                auto it = monitors_.find(src.monitorName);
                if (it == monitors_.end() || !it->second)
                        continue;

                it->second->csvLoading_.store(true, std::memory_order_release);
                if (src.type == "audio" || (src.type.empty() && hasAudioExtension(src.path)))
                        importAudioAsync(src.path, src.monitorName);
                else
                        importCsvAsync(src.path, src.monitorName, src.timeColumn, src.timeUnit);
        }

        cJSON_Delete(root);

        captureSessionBaseline(true);
        u64 end = get_mono_ts_ms();
        LOG_I("LoadSession Profile Detail: Parse %llu ms, Monitors %llu ms, Variables %llu ms, Layout %llu ms",
              pEnd - fEnd,
              varStart - monitorStart,
              layoutStart - varStart,
              end - layoutStart);
}

void
Gui::pollRemoteSession()
{
        if (!remoteSessionDownload_.valid() ||
            remoteSessionDownload_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
        const auto source = remoteSessionUrl_;
        remoteSessionUrl_.clear();
        try {
                const auto local = remoteSessionDownload_.get();
                if (!source.empty())
                        requestSessionChange(SessionChange::Open, local, source);
        } catch (const std::exception &e) {
                if (!source.empty())
                        ImGui::InsertNotification({ImGuiToastType::Error, 7000, "%s", e.what()});
        }
}

void
Gui::newSession()
{
        ui::interaction::reset();
        remoteSessionUrl_.clear();
        // Workers may publish LOCAL values under mtxMonitors_. Stop and join
        // them before taking that mutex, while their variable buffers still exist
        seqEditor_.loadSession(nullptr);
        sdkPanels_.clear();
        motorControl_.load(nullptr);
        faultDecoder().load(nullptr);
        dsoScope_.load(nullptr);
        midiTool_.clear();
        midiTool_.setOpen(false);
        midiTool_.consumeModified();
        deviceManager_.clear();
        bkpSramDebugger_.resetSession();
        {
                std::lock_guard lock(mtxMonitors_);
                // The sampler/FFT worker may still own a shared snapshot. Retire
                // its objects before removing them from the live container
                for (auto &monitorEntry : monitors_) {
                        auto &monitor = monitorEntry.second;
                        monitor->markPendingDelete();
                        for (auto &scopeEntry : monitor->getScopes()) {
                                auto &scope = scopeEntry.second;
                                scope->markPendingDelete();
                                for (auto &channelEntry : scope->getChannels())
                                        channelEntry.second->markPendingDelete();
                        }
                }
                monitors_.clear();
                vars_.clear();
        }
        dock_close::reset();
        currentSessionPath_ = "session.ava";
        // Retained tool/device preferences become the new session's baseline.
        bode_.clearModified();
        inverterSimulator_.clearModified();
        JLinkPort::instance().clearConfigModified();
        isModified_  = false;
        isFirstSave_ = true;
        showHome_    = true;
        captureSessionBaseline(true);
}

void
Gui::drawBar()
{
        pollRemoteSession();
        if (!remoteSessionUrl_.empty()) {
                ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
                ImGui::Begin(tr("Remote Session", "远程会话"), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::TextUnformatted(tr("Downloading session...", "正在下载会话……"));
                if (ImGui::Button(tr("Cancel", "取消")))
                        remoteSessionUrl_.clear();
                ImGui::End();
        }
        if (ImGui::BeginMainMenuBar()) {
                // A recent session chosen from the submenu is loaded after the menu is
                // built (loadSession mutates recentSessions_, so we can't load mid-iteration)
                std::string sessionToOpen;
                if (ImGui::BeginMenu(tr("File", "文件"))) {
                        if (ImGui::MenuItem(tr("New Session", "新建会话"))) {
                                pendingCommand_ = AppCommand::New;
                        }
                        if (ImGui::MenuItem(tr("Open Session...", "打开会话..."))) {
                                std::string p = nativeDlgOpen("Open Session", {{"Session Files", {"ava", "json"}}});
                                if (!p.empty())
                                        requestSessionChange(SessionChange::Open, p);
                        }
                        if (ImGui::BeginMenu(tr("Recent Sessions", "最近会话"))) {
                                ImGui::TextDisabled("%s", tr("Remote AVA sessions", "远程 AVA 会话"));
                                sessionToOpen = remoteSessions_.draw(remote::settings().sessionsUrl, {".ava"});
                                ImGui::Separator();
                                for (size_t i = 0; i < recentSessions_.size(); ++i) {
                                        const std::string &p      = recentSessions_[i];
                                        const bool         exists = recentPathExists(p);
                                        const std::string  label  = recentDisplayName(p);
                                        // Keep the ID independent of file-name bytes. This also
                                        // protects against malformed legacy entries containing an
                                        // early C-string terminator
                                        ImGui::PushID(static_cast<int>(i));
                                        if (ImGui::MenuItem(label.c_str(), nullptr, false, exists))
                                                sessionToOpen = p;
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s%s", p.c_str(), exists ? "" : "  (missing)");
                                        ImGui::PopID();
                                }
                                ImGui::Separator();
                                if (ImGui::MenuItem(tr("Clear Recent", "清空最近列表"))) {
                                        recentSessions_.clear();
                                        saveRecentList();
                                }
                                ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem(tr("Save Session", "保存会话"), ui::ShortcutLabel("Ctrl+S", "Cmd+S"))) {
                                pendingCommand_ = AppCommand::Save;
                        }
                        if (ImGui::MenuItem(tr("Save Session As...", "会话另存为..."))) {
                                pendingCommand_ = AppCommand::SaveAs;
                        }
                        ImGui::Separator();
#ifdef _WIN32
                        if (ImGui::MenuItem(tr("Minimize to system tray", "最小化到系统托盘")))
                                minimizeRequested_ = true;
#endif
                        if (ImGui::MenuItem(tr("Exit", "退出"))) {
                                requestExit();
                        }
                        ImGui::EndMenu();
                }
                if (!sessionToOpen.empty())
                        requestSessionChange(SessionChange::Open, sessionToOpen);

                if (ImGui::BeginMenu(tr("Edit", "编辑"))) {
                        if (ImGui::MenuItem(tr("Undo", "撤销"),
                                            ui::ShortcutLabel("Ctrl+Z", "Cmd+Z"),
                                            false,
                                            ui::interaction::undoAvailable))
                                ui::interaction::requested = ui::interaction::Action::Undo;
                        if (ImGui::MenuItem(tr("Redo", "重做"),
                                            ui::ShortcutLabel("Ctrl+Y", "Cmd+Shift+Z"),
                                            false,
                                            ui::interaction::redoAvailable))
                                ui::interaction::requested = ui::interaction::Action::Redo;
                        ImGui::EndMenu();
                }

                // mark() right after BeginMenu so the highlight tracks the (closed) menu
                // header — guiding the user to open it, not just appearing once it's open
                const bool windowMenuOpen = ImGui::BeginMenu(tr("Window", "窗口"));
                TutorialGuide::instance().mark("menu_window");
                if (windowMenuOpen) {
                        if (ImGui::MenuItem(tr("Workspace home", "工作区首页")))
                                pendingCommand_ = AppCommand::Home;
                        if (ImGui::MenuItem(tr("Find command...", "查找功能..."), ui::ShortcutLabel("Ctrl+K", "Cmd+K")))
                                pendingCommand_ = AppCommand::Commands;
                        ImGui::Separator();
                        if (ImGui::MenuItem(tr("Add Variable Recorder", "添加变量记录器")))
                                pendingCommand_ = AppCommand::Monitor;
                        if (ImGui::MenuItem(tr("Add Variable Manager", "添加变量管理器")))
                                pendingCommand_ = AppCommand::Variables;
                        if (ImGui::MenuItem(tr("Add Dynamic Library Caller", "添加动态库调用器")))
                                newSdkPanel();
                        ImGui::EndMenu();
                }

                const bool toolsMenuOpen = ImGui::BeginMenu(tr("Tools", "工具"));
                TutorialGuide::instance().mark("menu_tools");
                if (toolsMenuOpen) {
                        if (ImGui::MenuItem("MuJoCo")) {
                                std::lock_guard lk(mtxMonitors_);
                                std::string     name = "MuJoCo";
                                for (unsigned i = 2; monitors_.count(name); ++i)
                                        name = "MuJoCo " + std::to_string(i);
                                auto monitor = std::make_shared<Monitor>(name);
                                monitor->setTitle(name);
                                monitor->mujocoEnabled_  = true;
                                monitor->historySeconds_ = 10.0f;
                                monitor->addScope("Targets");
                                auto targetScope = monitor->getScopes().at("Targets");
                                targetScope->setLabel(tr("Reference Targets", "目标设置"));
                                targetScope->setDraw(MonitorScope::DrawEnum::TABLE);
                                monitor->addScope("PD Parameters");
                                auto pdScope = monitor->getScopes().at("PD Parameters");
                                pdScope->setLabel(tr("PD Parameters", "PD 参数"));
                                pdScope->setDraw(MonitorScope::DrawEnum::TABLE);
                                for (unsigned field : {0u, 2u, 6u, 7u}) {
                                        auto targetScope = field < 6 ? monitor->getScopes().at("Targets") : pdScope;
                                        targetScope->addChannel(ava_sim::fields[field]);
                                        auto *ch = targetScope->findChannel(ava_sim::fields[field]);
                                        ch->setDevice(ava_sim::device);
                                        ch->setType("f32");
                                        ch->setShmRegionName(ava_sim::defaultRegion);
                                        ch->setSimField(field);
                                        ch->setWritable(true);
                                        ch->historySeconds_ = 10.0f;
                                }
                                const char *scopeNames[] = {"Position (rad)", "Velocity (rad/s)", "Torque (Nm)", "Error (rad)"};
                                for (const char *scopeName : scopeNames)
                                        monitor->addScope(scopeName);
                                const unsigned fieldScopes[] = {0, 0, 1, 1, 2, 3};
                                for (unsigned field = 0; field < 6; ++field) {
                                        auto scope = monitor->getScopes().at(scopeNames[fieldScopes[field]]);
                                        scope->addChannel(ava_sim::fields[field]);
                                        auto *ch = scope->findChannel(ava_sim::fields[field]);
                                        ch->setDevice(ava_sim::device);
                                        ch->setType("f32");
                                        ch->setShmRegionName(ava_sim::defaultRegion);
                                        ch->setSimField(field);
                                        ch->setWritable(ava_sim::writableField(field));
                                        ch->historySeconds_ = 10.0f;
                                }
                                monitors_[name] = std::move(monitor);
                                isModified_     = true;
                        }
                        bool deviceManagerOpen = deviceManager_.isOpen();
                        if (ImGui::MenuItem(tr("Device Manager", "设备管理器"), nullptr, &deviceManagerOpen))
                                deviceManager_.setOpen(deviceManagerOpen);
                        if (ImGui::MenuItem("J-Flash"))
                                flashTool_.open();
                        if (ImGui::MenuItem(tr("Parameter Configuration", "参数配置"))) {
                                if (vars_.empty()) {
                                        const std::string key = "parameter_editor";
                                        vars_[key]            = std::make_shared<Variable>(key);
                                        vars_[key]->setTitle("变量管理器");
                                }
                                vars_.begin()->second->openParameterEditor();
                        }
                        ImGui::MenuItem(tr("Joint Calculator", "电机参数计算器"), nullptr, &showCalculator_);
                        ImGui::MenuItem(tr("Safety", "功能安全"), nullptr, &faultDecoder().open);
                        ImGui::MenuItem(tr("Bode Plot", "伯德图"), nullptr, &bode_.show_);
                        ImGui::MenuItem(tr("Inverter Simulation", "逆变器仿真"), nullptr, &inverterSimulator_.show_);
                        ImGui::MenuItem(tr("Assembly Viewer", "汇编查看器"), nullptr, &asmViewer_.show_);
                        if (ImGui::MenuItem(tr("SWO / ITM Trace", "SWO / ITM 跟踪")))
                                JLinkPort::instance().openTraceWindow();
                        if (ImGui::MenuItem(tr("Ring Buffer Capture", "环形数据采集")))
                                bkpSramDebugger_.open();
                        bool motorOpen = motorControl_.isOpen();
                        if (ImGui::MenuItem(tr("Motor Control", "电机控制"), nullptr, &motorOpen))
                                motorControl_.setOpen(motorOpen);
                        bool midiOpen = midiTool_.isOpen();
                        if (ImGui::MenuItem(tr("MIDI Player", "MIDI播放器"), nullptr, &midiOpen))
                                midiTool_.setOpen(midiOpen);
                        bool seqOpen = seqEditor_.isOpen();
                        if (ImGui::MenuItem(tr("Sequence Editor", "序列编辑器"), nullptr, &seqOpen)) {
                                seqEditor_.setOpen(seqOpen);
                        }
                        ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(tr("Settings", "设置"))) {
                        drawAppearanceMenu();

#ifdef _WIN32
                        if (ImGui::BeginMenu(tr("Window close behavior", "窗口关闭行为"))) {
                                const char *labels[] = {tr("Ask every time", "每次询问"),
                                                        tr("Exit application", "退出程序"),
                                                        tr("Minimize to tray", "缩小到托盘")};
                                int         behavior = static_cast<int>(closeBehavior_);
                                if (ui::Choice("##close_behavior", &behavior, labels, 3)) {
                                        closeBehavior_ = static_cast<CloseBehavior>(behavior);
                                        saveWindowPreferences();
                                }
                                ImGui::EndMenu();
                        }
#endif
                        if (ImGui::BeginMenu(tr("Server", "服务器"))) {
                                if (drawServerSettings(localFileServer_, websiteServer_, getAppDir())) {
                                        isModified_ = true;
                                        if (!remote::saveDefaults(getAppDir() + "/remote_files.json"))
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error,
                                                     4000,
                                                     "%s",
                                                     tr("Cannot save remote file settings.", "无法保存远程文件设置。")});
                                }
                                ImGui::EndMenu();
                        }
                        if (ImGui::BeginMenu(tr("J-Link DLL", "J-Link DLL"))) {
                                JLinkPort::instance().drawDllSettingsMenu();
                                ImGui::EndMenu();
                        }
                        if (ImGui::BeginMenu(tr("Sampler Run Mode", "采样器运行模式"))) {
                                int         cur     = g_samplerRunMode.load();
                                const char *modes[] = {tr("Low CPU Usage", "低占用模式"),
                                                       tr("Normal", "普通模式"),
                                                       tr("CPU-Bound", "CPU 绑定模式")};
                                if (ui::Choice("##sampler_run_mode", &cur, modes, 3)) {
                                        g_samplerRunMode.store(cur);
                                        g_samplerRebind.store(true);
                                }
                                const char *details[] = {
                                    tr("Sleep 1ms per iter, normal priority", "每次迭代睡眠 1ms，普通优先级"),
                                    tr("Adaptive sleep, slightly elevated priority", "自适应睡眠，稍高优先级"),
                                    tr("Spin-loop, highest priority, core pinned", "自旋循环，最高优先级，绑定 CPU 核心")};
                                ImGui::TextDisabled("%s", details[std::clamp(cur, 0, 2)]);

                                // Core selection — only meaningful in CPU-Bound mode
                                // Pinning our own thread needs no admin rights
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

                        if (ImGui::BeginMenu(tr("Storage and cleanup", "存储与清理"))) {
                                drawStorageSettings();
                                ImGui::EndMenu();
                        }

                        if (ImGui::BeginMenu(tr("Language", "语言"))) {
                                int         language    = g_lang == Lang::EN ? 0 : 1;
                                const char *languages[] = {"English", "中文"};
                                if (ui::Choice("##language", &language, languages, 2))
                                        setLang(language == 0 ? Lang::EN : Lang::ZH);
                                ImGui::EndMenu();
                        }
                        ImGui::EndMenu();
                }

                const bool helpMenuOpen = ImGui::BeginMenu(tr("Help", "帮助"));
                TutorialGuide::instance().mark("menu_help");
                if (helpMenuOpen) {
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
                        if (ImGui::MenuItem(tr("Keyboard shortcuts", "快捷键手册")))
                                ui::shortcutManualOpen = true;
                        TutorialGuide::instance().mark("shortcut_manual");
                        if (ImGui::MenuItem(tr("Panel guides", "面板指南")))
                                TutorialGuide::instance().openCatalog();
                        if (ImGui::MenuItem(tr("Current panel guide", "当前面板引导"), "F1"))
                                TutorialGuide::instance().showForCurrentDock();
                        if (ImGui::MenuItem(tr("Getting started", "新手引导"))) {
                                TutorialGuide::instance().start();
                        }
                        ImGui::EndMenu();
                }

                ImGui::EndMainMenuBar();
        }
}

void
Gui::loop()
{
        LOG_I("Gui::loop start");
        while (!wantsToQuit_) {
                glfwPollEvents();
                processWindowActions();
                // Session loads rebuild docking/layout state; only apply between frames.
                if (sessionChangeReady_)
                        applySessionChange();

                if (glfwWindowShouldClose(window_) && !showQuitModal_ && !wantsToQuit_) {
                        const bool anyModified = hasSessionChanges();
                        LOG_I("Quit dirty check: modified=%d firstSave=%d", (int)anyModified, (int)isFirstSave_);
                        if (anyModified) {
                                glfwSetWindowShouldClose(window_, GLFW_FALSE);
                                showQuitModal_ = true;
                        } else {
                                break;
                        }
                }

                updateAppearance();
                // Apply outside a frame, after all window/menu style stacks are closed.
                if (platformStyleDirty_) {
                        ui::ApplyAppStyle(colorScheme_,
                                          uiScale_,
                                          ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable,
                                          appearanceDark_);
                        ui::ApplyPlotStyle(appearanceDark_);
#ifdef _WIN32
                        native_appearance::applyWindow(glfwGetWin32Window(window_), appearanceDark_);
#endif
                        platformStyleDirty_ = false;
                }
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ui::interaction::beginFrame();
                ImGui::NewFrame();

                drawBar();
                drawStartupSessionPicker();

                drawWorkspaceChrome();
                {
                        const auto                &io = ImGui::GetIO();
                        const ui::AppShortcutInput input{
                            io.KeyCtrl,
                            io.KeyShift,
                            io.KeyAlt,
                            io.KeySuper,
                            io.WantTextInput,
                            ImGui::IsAnyItemActive() || ImGui::GetCurrentContext()->NavActivateId != 0,
                            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
                            ImGui::IsKeyPressed(ImGuiKey_S, false),
                            ImGui::IsKeyPressed(ImGuiKey_O, false),
                            ImGui::IsKeyPressed(ImGuiKey_K, false),
                            ImGui::IsKeyPressed(ImGuiKey_L, false),
                            ImGui::IsKeyPressed(ImGuiKey_D, false),
                            ImGui::IsKeyPressed(ImGuiKey_Space, false),
                            ImGui::IsKeyPressed(ImGuiKey_Delete, false)};
                        switch (ui::ResolveAppShortcut(input)) {
                                case ui::AppShortcut::FreezeDisplay:
                                        pendingCommand_ = AppCommand::FreezeDisplay;
                                        break;
                                case ui::AppShortcut::ClearData: {
                                        std::lock_guard lock(mtxMonitors_);
                                        Monitor::clearAll();
                                        break;
                                }
                                case ui::AppShortcut::Save:
                                        pendingCommand_ = AppCommand::Save;
                                        break;
                                case ui::AppShortcut::SaveAs:
                                        pendingCommand_ = AppCommand::SaveAs;
                                        break;
                                case ui::AppShortcut::Open:
                                        pendingCommand_ = AppCommand::Open;
                                        break;
                                case ui::AppShortcut::Commands:
                                        pendingCommand_ = AppCommand::Commands;
                                        break;
                                case ui::AppShortcut::Connect:
                                        pendingCommand_ = AppCommand::Connect;
                                        break;
                                case ui::AppShortcut::Disconnect:
                                        pendingCommand_ = AppCommand::Disconnect;
                                        break;
                                default:
                                        break;
                        }
                }

                workspaceDock_ = ui::IslandDockSpace(ImGui::GetMainViewport());
                drawHome();

                // Dockable J-Link tool windows must be submitted after their host
                // dockspace. drawBar() only renders the toolbar controls
                JLinkPort::instance().drawWindows();
                bkpSramDebugger_.draw();
                flashTool_.draw();
                if (flashTool_.consumeModified())
                        isModified_ = true;

                // Kick off a one-time update check shortly after launch (throttled to
                // once every 6h so repeated launches don't hammer the internal file server)
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
                                if (it->second->isPendingDelete()) {
                                        it          = monitors_.erase(it);
                                        isModified_ = true;
                                } else {
                                        it->second->updateDisplay();
                                        ++it;
                                }
                        }
                }

                {
                        const std::string bindVariable = consumeMonitorFunctionBindingRequest();
                        if (!bindVariable.empty()) {
                                bool opened = false;
                                for (auto &variableWindow : vars_ | std::views::values)
                                        if (variableWindow->openFunctionBindingFor(bindVariable)) {
                                                opened = true;
                                                break;
                                        }
                                if (!opened)
                                        LOG_W("LOCAL variable '%s' was not found in Variable Manager", bindVariable.c_str());
                        }
                        std::unordered_map<std::string, u32> localMonitorRates;
                        {
                                std::lock_guard lk(mtxMonitors_);
                                for (const auto &m : monitors_ | std::views::values) {
                                        if (m->isSamplingPaused())
                                                continue;
                                        const u32 hz = static_cast<u32>(std::max(1, m->maxSampleHz_));
                                        for (const auto &s : m->getScopes() | std::views::values)
                                                for (const auto &[cn, ch] : s->getChannels()) {
                                                        if (ch->getDevice() != "LOCAL")
                                                                continue;
                                                        const std::string &key =
                                                            ch->getSymbolName().empty() ? cn : ch->getSymbolName();
                                                        localMonitorRates[key] = std::max(localMonitorRates[key], hz);
                                                }
                                }
                        }
                        for (auto it = vars_.begin(); it != vars_.end();) {
                                if (it->second->isPendingDelete()) {
                                        it          = vars_.erase(it);
                                        isModified_ = true;
                                } else {
                                        it->second->setLocalMonitorSampleRates(localMonitorRates);
                                        it->second->updateDisplay();
                                        if (it->second->consumeElfReloaded()) {
                                                syncSymbolAddresses(it->second.get());
                                                it->second->completeElfReload();
                                        }
                                        if (it->second->consumePropertiesChanged()) {
                                                syncVariableProperties(it->second.get());
                                        }
                                        ++it;
                                }
                        }
                }

                // Feed LOCAL variable data into matching monitor scope chs
                // JLINK/SHM chs are sampled by the dedicated sampler thread, but
                // Manual/sequence LOCAL values use the configured cadence. SDK
                // sources supply timestamped events below, so a cached buffer is
                // never mistaken for a newly acquired sample after disconnect.
                {
                        std::vector<std::pair<std::string, double>> localWrites;
                        {
                                std::lock_guard lk(mtxMonitors_);
                                for (auto &m : monitors_ | std::views::values)
                                        for (auto &s : m->getScopes() | std::views::values)
                                                for (auto &[cn, ch] : s->getChannels()) {
                                                        if (ch->getDevice() != "LOCAL" || !ch->isWritable())
                                                                continue;
                                                        float val = 0.0f;
                                                        if (!ch->consumeWValDirty(val))
                                                                continue;
                                                        const std::string &key =
                                                            ch->getSymbolName().empty() ? cn : ch->getSymbolName();
                                                        localWrites.emplace_back(key, val);
                                                }
                        }
                        for (const auto &[name, val] : localWrites)
                                for (auto &vw : vars_ | std::views::values)
                                        if (vw->writeLocalScalarFromMonitor(name, val))
                                                break;

                        std::unordered_map<std::string, float>               localVals;
                        std::unordered_map<std::string, Variable::RawSample> localRaw;
                        for (auto &vw : vars_ | std::views::values)
                                for (auto &[chName, val] : vw->collectLocalChannelValues()) {
                                        localVals[chName] = val;
                                        Variable::RawSample raw;
                                        if (vw->readLocalChannelBytes(chName, raw))
                                                localRaw[chName] = raw;
                                }

                        if (!localVals.empty()) {
                                const double    ts = sessionTimeSec();
                                std::lock_guard lk(mtxMonitors_);
                                for (auto &m : monitors_ | std::views::values) {
                                        if (m->isSamplingPaused() || !m->consumeLocalSampleDue(ts))
                                                continue;
                                        u64 pushed = 0;
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
                                                        if (const auto raw = localRaw.find(key); raw != localRaw.end())
                                                                ch->setLatestRawBytes(raw->second.bytes.data(),
                                                                                      raw->second.size);
                                                        ch->publishSnapshot();
                                                        ++pushed;
                                                }
                                        m->addPoints(pushed);
                                }
                        }
                        // SDK buffers are snapshots, not continuously sampled local
                        // signals. Only successful fresh reads produce these batches.
                        std::lock_guard lock(mtxMonitors_);
                        for (auto &variable : vars_ | std::views::values) {
                                const auto batches = variable->drainLocalSamples();
                                if (batches.empty())
                                        continue;
                                for (auto &m : monitors_ | std::views::values) {
                                        if (m->isSamplingPaused())
                                                continue;
                                        u64 pushed = 0;
                                        for (auto &scope : m->getScopes() | std::views::values) {
                                                if (scope->isPaused())
                                                        continue;
                                                for (auto &[name, ch] : scope->getChannels()) {
                                                        if (ch->getDevice() != "LOCAL" || ch->isPendingDelete())
                                                                continue;
                                                        const auto &key =
                                                            ch->getSymbolName().empty() ? name : ch->getSymbolName();
                                                        const auto it = batches.find(key);
                                                        if (it == batches.end())
                                                                continue;
                                                        const auto &batch = it->second;
                                                        ch->pushBatch(
                                                            batch.values.data(), batch.times.data(), batch.values.size());
                                                        if (!batch.values.empty())
                                                                ch->setLatestRawBytes(batch.raw.bytes.data(), batch.raw.size);
                                                        if (!g_monitorPaused.load())
                                                                ch->publishSnapshot();
                                                        pushed += batch.values.size();
                                                }
                                        }
                                        m->addPoints(pushed);
                                }
                        }
                }

                // Feed AUDIO variables into matching monitor chs. Audio capture
                // runs in WaveIn callbacks; the GUI drains complete sample batches and
                // pushes them through the same MonitorChannel store used by J-Link/LOCAL
                {
                        std::unordered_map<std::string, int> audioVars;
                        for (auto &vw : vars_ | std::views::values) {
                                for (const auto &ve : vw->vars_) {
                                        if (ve.port == PortType::AUDIO)
                                                audioVars[ve.name] = ve.audio.deviceIndex;
                                }
                        }

                        std::vector<int> activeAudioDevices;
                        if (!audioVars.empty()) {
                                std::lock_guard lk(mtxMonitors_);
                                for (auto &m : monitors_ | std::views::values)
                                        for (auto &s : m->getScopes() | std::views::values)
                                                for (auto &[cn, ch] : s->getChannels()) {
                                                        if (ch->getDevice() != "AUDIO")
                                                                continue;
                                                        const std::string &key =
                                                            ch->getSymbolName().empty() ? cn : ch->getSymbolName();
                                                        auto it = audioVars.find(key);
                                                        if (it != audioVars.end())
                                                                activeAudioDevices.push_back(it->second);
                                                }
                        }

                        std::sort(activeAudioDevices.begin(), activeAudioDevices.end());
                        activeAudioDevices.erase(std::unique(activeAudioDevices.begin(), activeAudioDevices.end()),
                                                 activeAudioDevices.end());
                        AudioInput::instance().setActiveDevices(activeAudioDevices);

                        if (!activeAudioDevices.empty()) {
                                std::unordered_map<int, std::vector<AudioSample>> batches;
                                for (int devIdx : activeAudioDevices) {
                                        std::vector<AudioSample> samples;
                                        if (AudioInput::instance().drainSamples(devIdx, samples) && !samples.empty())
                                                batches.emplace(devIdx, std::move(samples));
                                }

                                if (!batches.empty()) {
                                        std::lock_guard lk(mtxMonitors_);
                                        for (auto &m : monitors_ | std::views::values)
                                                for (auto &s : m->getScopes() | std::views::values)
                                                        for (auto &[cn, ch] : s->getChannels()) {
                                                                if (ch->getDevice() != "AUDIO")
                                                                        continue;
                                                                const std::string &key =
                                                                    ch->getSymbolName().empty() ? cn : ch->getSymbolName();
                                                                auto vit = audioVars.find(key);
                                                                if (vit == audioVars.end())
                                                                        continue;
                                                                auto bit = batches.find(vit->second);
                                                                if (bit == batches.end())
                                                                        continue;

                                                                std::vector<float>  vals;
                                                                std::vector<double> ts;
                                                                vals.reserve(bit->second.size());
                                                                ts.reserve(bit->second.size());
                                                                for (const auto &sample : bit->second) {
                                                                        vals.push_back(sample.val);
                                                                        ts.push_back(sample.ts);
                                                                }
                                                                ch->pushBatch(vals.data(), ts.data(), vals.size());
                                                                ch->publishSnapshot();
                                                                m->addPoints(vals.size());
                                                        }
                                }
                        }
                }

                // Mirror symbols dropped from a symbol browser onto a monitor back into the
                // originating Variable's watch list (requests queued by MonitorScope::dropTarget)
                if (auto &mirrorQ = watchMirrorQueue(); !mirrorQ.empty()) {
                        for (const auto &req : mirrorQ)
                                for (auto &v : vars_ | std::views::values)
                                        if (static_cast<const void *>(v.get()) == req.target) {
                                                v->mirrorFromMonitorDrop(req);
                                                break;
                                        }
                        mirrorQ.clear();
                }

                bode_.updateDisplay(vars_);
                inverterSimulator_.draw();
                asmViewer_.draw(this);
                seqEditor_.draw();
                if (deviceManager_.isOpen()) {
                        std::vector<DeviceManager::SdkCatalogEntry> catalog;
                        for (const auto &panel : sdkPanels_)
                                if (panel->open_ && (panel->libraryPath()[0] || panel->headerPath()[0]))
                                        catalog.push_back({panel->getWinId(),
                                                           panel->displayName(),
                                                           panel->libraryPath(),
                                                           panel->headerPath(),
                                                           panel->isLibraryLoaded(),
                                                           panel->functionCount()});
                        deviceManager_.setSdkCatalog(std::move(catalog));
                }
                deviceManager_.draw();
                if (deviceManager_.consumeModified())
                        isModified_ = true;
                motorControl_.onJlinkSymbols = [this] {
                        std::vector<motor_jlink::Symbol> result;
                        for (const auto &[name, variable] : vars_) {
                                auto symbols = motor_jlink::symbols(variable->getDwarfInfo(), variable->getElfPath());
                                result.insert(result.end(), symbols.begin(), symbols.end());
                        }
                        return result;
                };
                motorControl_.draw();
                faultDecoder().draw(vars_);
                dsoScope_.setOpen(std::any_of(
                    sdkPanels_.begin(), sdkPanels_.end(), [](const auto &panel) { return panel->isDsoGuiActive(); }));
                dsoScope_.tick();
                if (dsoScope_.consumeModified())
                        isModified_ = true;
                if (motorControl_.consumeModified())
                        isModified_ = true;
                drawElevationPrompt();
                midiTool_.draw();
                if (midiTool_.consumeModified())
                        isModified_ = true;

                // Draw SDK windows; prune closed ones
                for (auto &sp : sdkPanels_)
                        sp->draw();
                const auto sdkCount = sdkPanels_.size();
                sdkPanels_.erase(std::remove_if(sdkPanels_.begin(),
                                                sdkPanels_.end(),
                                                [](const std::shared_ptr<SdkPanel> &p) { return !p->open_; }),
                                 sdkPanels_.end());
                if (sdkPanels_.size() != sdkCount)
                        isModified_ = true;

                ui::DrawDockCorners();
                ui::DrawShortcutManual();
                TutorialGuide::instance().draw();

                processPendingCsvImports();
                drawEmptyImportPopup();

                for (const auto &file : sDroppedFiles_) {
                        std::string lowerFile = file;
                        std::ranges::transform(
                            lowerFile, lowerFile.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        const bool isNativeLibrary = lowerFile.ends_with(".dll") || lowerFile.ends_with(".so") ||
                                                     lowerFile.ends_with(".dylib") ||
                                                     lowerFile.find(".so.") != std::string::npos;
                        const bool isNativeHeader = lowerFile.ends_with(".h") || lowerFile.ends_with(".hpp") ||
                                                    lowerFile.ends_with(".hh") || lowerFile.ends_with(".hxx");

                        if (lowerFile.ends_with(".ava")) {
                                requestSessionChange(SessionChange::Open, file);
                        } else if (lowerFile.ends_with(".mid") || lowerFile.ends_with(".midi")) {
                                midiTool_.loadFile(file);
                        } else if (isNativeLibrary || isNativeHeader) {
                                for (auto &sp : sdkPanels_)
                                        sp->pushDroppedFiles({file});
                                deviceManager_.pushDroppedFiles({file});
                        } else if (lowerFile.ends_with(".csv") || hasAudioExtension(file)) {
                                const bool isAudioImport = hasAudioExtension(file);
                                if (!isAudioImport) {
                                        beginCsvImport(file);
                                        continue;
                                }
                                std::error_code   absEc;
                                const std::string importPath =
                                    std::filesystem::absolute(std::filesystem::path(file), absEc).string();
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
                                        mon->importSourcePath_ = absEc ? file : importPath;
                                        mon->importSourceType_ = isAudioImport ? "audio" : "csv";
                                        monitors_[monName]     = mon;
                                        isModified_            = true;
                                }
                                importAudioAsync(file, monName);
                        } else if (lowerFile.ends_with(".elf") || lowerFile.ends_with(".axf") || lowerFile.ends_with(".out") ||
                                   lowerFile.ends_with(".json") || lowerFile.ends_with(".bin")) {
                                // Variable Manager consumes these from the same drop list
                        } else {
                                unsupportedFiles_.push_back(file);
                                showUnsupportedFilePopup_ = true;
                                LOG_W("Unsupported dropped file format: %s", file.c_str());
                        }
                }
                sDroppedFiles_.clear();
                drawCsvImportDialog();
                drawUnsupportedFilePopup();
                drawCloseChoicePopup();
                drawCommands();
                ui::interaction::process();
                executeCommand();
                drawSessionChangePrompt();
                if (showQuitModal_) {
                        ImGui::OpenPopup("###Quit");
                        if (ImGui::BeginPopupModal(
                                tr("Quit?###Quit", "退出？###Quit"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("%s", tr("Save changes to session before quitting?", "退出前保存会话更改吗？"));
                                ImGui::Separator();
                                if (ui::Button(tr("Save", "保存"), ui::BtnStyle::Success, ImVec2(120, 0))) {
                                        const bool saved = saveSession();
                                        showQuitModal_   = false;
                                        ImGui::CloseCurrentPopup();
                                        if (saved)
                                                wantsToQuit_ = true;
                                        else {
                                                exitRequested_ = false;
                                                glfwSetWindowShouldClose(window_, GLFW_FALSE);
                                        }
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
                                        exitRequested_ = false;
                                        glfwSetWindowShouldClose(window_, GLFW_FALSE);
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                }

                if (wantsToQuit_) {
                        break;
                }

                ui::UnitConversionEndFrame();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().PopupRounding);
                ImGui::RenderNotifications();
                ImGui::PopStyleVar();
                dock_close::draw();
                dockSession_.afterWindows();
                if (sessionLayoutSettle_ > 0 && --sessionLayoutSettle_ == 0)
                        sessionLayoutBaseline_ = ImGui::SaveIniSettingsToMemory();
                ImGui::Render();
                updatePlatformWindows();
                if (inTray_) {
                        // Continue data/platform updates while skipping GPU drawing
                        glfwWaitEventsTimeout(0.033);
                        continue;
                }
                i32 display_w, display_h;
                glfwGetFramebufferSize(window_, &display_w, &display_h);
                glViewport(0, 0, display_w, display_h);
                glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                        GLFWwindow *backupCurrentContext = glfwGetCurrentContext();
                        ImGui::RenderPlatformWindowsDefault();
                        glfwMakeContextCurrent(backupCurrentContext);
                }

                captureRenderedFrame(display_w, display_h);
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
        // symbol provided by another open ELF isn't falsely flagged as missing
        struct SymbolSyncInfo {
                u64      addr{0};
                u64      typeOff{0};
                DataType type{DataType::UNKNOWN};
                u32      bitOffset{0};
                u32      bitSize{0};
        };
        std::unordered_map<std::string, SymbolSyncInfo> symMap;
        for (auto &vp : vars_)
                for (const auto &se : vp.second->searchPool_)
                        symMap.try_emplace(se.path, SymbolSyncInfo{se.addr, se.typeOff, se.type, se.bitOffset, se.bitSize});

        i32 count     = 0;
        i32 unknown   = 0;
        i32 enumCount = 0;
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (ch->getSymbolName().empty())
                                        continue; // manual / CSV ch — no symbol to resolve

                                auto it = symMap.find(ch->getSymbolName());
                                if (it != symMap.end()) {
                                        ch->setAddr(it->second.addr);
                                        ch->setType(Parser::dataTypeToStr(it->second.type));
                                        ch->setBitOffset(it->second.bitOffset);
                                        ch->setBitSize(it->second.bitSize);
                                        ch->setAddrUnknown(false);
                                        count++;
                                } else {
                                        // Symbol no longer exists in any loaded ELF → mark unknown
                                        ch->setAddrUnknown(true);
                                        unknown++;
                                }
                                // Refresh enum entries from the reloaded ELF +
                                // any user overrides on the owning VarEntry
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
                                v.addr        = it->second.addr;
                                v.typeOff     = it->second.typeOff;
                                v.type        = it->second.type;
                                v.bitOffset   = it->second.bitOffset;
                                v.bitSize     = it->second.bitSize;
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
        if (!var)
                return;

        std::unordered_map<std::string, const SearchEntry *> exactSymbols;
        for (const auto &se : var->searchPool_)
                exactSymbols.try_emplace(se.path, &se);

        std::lock_guard lk(mtxMonitors_);
        for (auto &pair : monitors_) {
                for (auto &spair : pair.second->getScopes()) {
                        for (auto &cpair : spair.second->getChannels()) {
                                MonitorChannel *ch = cpair.second.get();
                                if (!ch || ch->getSymbolName().empty())
                                        continue;

                                const std::string &symbol = ch->getSymbolName();
                                auto               seIt   = exactSymbols.find(symbol);

                                for (const auto &v : var->vars_) {
                                        bool isMatch = false;
                                        if (symbol == v.name) {
                                                isMatch = true;
                                        } else if (symbol.size() > v.name.size() &&
                                                   symbol.compare(0, v.name.size(), v.name) == 0) {
                                                const char nextChar = symbol[v.name.size()];
                                                isMatch             = (nextChar == '.' || nextChar == '[');
                                        }

                                        if (!isMatch)
                                                continue;

                                        ch->setWritable(v.writable);

                                        if (seIt != exactSymbols.end()) {
                                                const SearchEntry *se = seIt->second;
                                                ch->setAddr(se->addr);
                                                ch->setType(Parser::dataTypeToStr(se->type));
                                                ch->setBitOffset(se->bitOffset);
                                                ch->setBitSize(se->bitSize);
                                                ch->setAddrUnknown(false);
                                        } else if (symbol == v.name) {
                                                ch->setAddr(v.addr);
                                                ch->setType(Parser::dataTypeToStr(v.type));
                                                ch->setBitOffset(v.bitOffset);
                                                ch->setBitSize(v.bitSize);
                                        }

                                        // Apply explicit sub-variable properties after the
                                        // ELF-derived defaults, while retaining the canonical
                                        // symbol path for future address re-resolution
                                        if (auto ovIt = v.memberOverrides.find(symbol); ovIt != v.memberOverrides.end()) {
                                                const auto &ov = ovIt->second;
                                                if (!ov.alias.empty())
                                                        ch->setLabel(ov.alias);
                                                if (ov.hasAddress) {
                                                        ch->setAddr(ov.address);
                                                        ch->setAddrUnknown(false);
                                                }
                                                if (ov.hasType)
                                                        ch->setType(Parser::dataTypeToStr(ov.type));
                                                if (ov.hasWritable)
                                                        ch->setWritable(ov.writable);
                                        }

                                        const char *deviceName = "JLINK";
                                        switch (v.port) {
                                                case PortType::JLINK:
                                                        deviceName = "JLINK";
                                                        break;
                                                case PortType::UDP:
                                                case PortType::LOCAL:
                                                        deviceName = "LOCAL";
                                                        break;
                                                case PortType::SHM:
                                                        deviceName = "SHM";
                                                        break;
                                                case PortType::AUDIO:
                                                        deviceName = "AUDIO";
                                                        break;
                                        }
                                        ch->setDevice(deviceName);
                                        if (v.port == PortType::SHM) {
                                                ch->setShmRegionName(v.shm.name);
                                                MonitorScope::shmInit(*ch);
                                        } else if (v.port == PortType::AUDIO) {
                                                ch->setType("f32");
                                                ch->setAddr(static_cast<usize>(std::max(0, v.audio.deviceIndex)));
                                                ch->setBitOffset(0);
                                                ch->setBitSize(0);
                                                ch->setShmRegionName(v.audio.deviceName);
                                        }
                                        break;
                                }
                        }
                }
        }
}

void
Gui::drawCalculator()
{
        if (!dock_close::begin(tr("Motor Parameter Calculator###MotorCalc", "电机参数计算器###MotorCalc"), &showCalculator_)) {
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

                ui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("--- Input Parameters ---", "--- 输入参数 ---"));
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
                ui::TextColored(
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
                ui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("--- Calculated Results ---", "--- 计算结果 ---"));
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
                // Unit dropdowns and ch links delegate to s_eff (saved with session)
                auto               &effTorqueUnit    = s_eff.torqueUnit;
                auto               &effSpeedUnit     = s_eff.speedUnit;
                auto               &effVoltageUnit   = s_eff.volUnit;
                auto               &effCurrentUnit   = s_eff.currentUnit;
                auto               &effTorqueLink    = s_eff.torqueLink;
                auto               &effSpeedLink     = s_eff.speedLink;
                auto               &effVoltageLink   = s_eff.volLink;
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

                // Read current value from a linked key
                // Monitor ch key: "@monName@scopeName@chName"  → getDispVal()
                // Variable key:        "winKey::varName"             → valueStr
                // "winKey::varName.field"       → struct field
                auto getVarVal = [&](const std::string &key, float fallback) -> float {
                        if (key.empty())
                                return fallback;
                        // ── Monitor ch path ─────────────────────────────
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
                                        if (ve.name == rest && ve.displayNumericValue)
                                                return static_cast<float>(*ve.displayNumericValue);
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

                // Pull linked Monitor ch values at the selected refresh interval
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
                                ui::Choice(unitId, &unitIdx, units);
                                ImGui::EndDisabled();
                        } else {
                                ui::Choice(unitId, &unitIdx, units);
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
                ui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("Mechanical Power", "机械功率"));
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
                if (ImGui::SliderInt("##effRefreshMs", &effRefreshMs, 10, 2000))
                        isModified_ = true;
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Refresh (ms)", "刷新间隔(毫秒)"));

                // ── DC Bus Power input ──────────────────────────────────────
                ImGui::Spacing();
                ui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("DC Bus Power", "直流母线功率"));
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

                        // ── Monitor chs (primary / recommended) ────────
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
                                ImGui::TextDisabled("%s", tr("(no monitor chs)", "(无监视器通道)"));

                        ImGui::EndPopup();
                }

                // ── Calculations ────────────────────────────────────────────
                float torque_Nm  = effTorque * kTorqueToNm[effTorqueUnit];
                float speed_rads = effSpeed * kSpeedToRadS[effSpeedUnit];
                float vol_V      = effVoltage * kVoltToV[effVoltageUnit];
                float current_A  = effCurrent * kCurrToA[effCurrentUnit];

                float P_mech    = torque_Nm * speed_rads;
                float P_elec    = vol_V * current_A;
                float P_elec_ab = std::abs(P_elec);
                float eta       = (P_elec_ab > 1e-9f) ? (std::abs(P_mech) / P_elec_ab * 100.0f) : 0.0f;

                // ── Efficiency map recording ─────────────────────────────────
                // Every refresh cycle appends a new scatter point
                if (effMapRec && effRefreshFired && eta > 0.f && eta <= 100.f) {
                        float spdRpm = speed_rads * (30.f / 3.14159265f);
                        s_eff.rawData.push_back({spdRpm, torque_Nm, eta});
                }

                // ── Results ─────────────────────────────────────────────────
                ImGui::Spacing();
                ImGui::Separator();
                ui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", tr("Results", "计算结果"));

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
                        row(tr("Bus Voltage (V)", "母线电压 (V)"), "%.4f V", vol_V);
                        row(tr("Bus Current (A)", "母线电流 (A)"), "%.4f A", current_A);
                        row(tr("Elec. Power", "电气功率"), "%.3f W", P_elec);
                        ImGui::EndTable();
                }

                ImGui::Spacing();
                // Efficiency bar
                ImVec4 etaColor = (eta >= 90.0f)   ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                  : (eta >= 70.0f) ? ImVec4(1.0f, 0.85f, 0.1f, 1.0f)
                                                   : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ui::TextColored(etaColor, tr("Efficiency:  %.2f %%", "效率：  %.2f %%"), eta);
                float barW = ImGui::GetContentRegionAvail().x;
                float frac = std::min(std::max(eta / 100.0f, 0.0f), 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, etaColor);
                ImGui::ProgressBar(frac, ImVec2(barW, 12.0f), "");
                ImGui::PopStyleColor();

                if (eta > 100.0f)
                        ui::ErrorText("%s", tr("! Efficiency > 100%% — check inputs.", "! 效率 > 100%%，请检查输入。"));

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
Gui::startCsvImport(const std::string &path, int timeColumn, int timeUnit)
{
        std::error_code   absEc;
        const auto        absPath    = std::filesystem::absolute(std::filesystem::u8path(path), absEc);
        const std::string importPath = absEc ? path : pathToUtf8(absPath);
        std::string       stem       = utf8Stem(path);
        if (stem.empty())
                stem = "csv_import";
        std::string monName;
        {
                std::lock_guard lk(mtxMonitors_);
                monName = stem;
                int idx = 1;
                while (monitors_.count(monName))
                        monName = stem + "_" + std::to_string(idx++);
                auto mon = std::make_shared<Monitor>(monName);
                mon->csvLoading_.store(true, std::memory_order_release);
                mon->importSourcePath_            = importPath;
                mon->importSourceType_            = "csv";
                mon->importTimeColumn_            = timeColumn;
                mon->importTimeUnit_              = timeUnit;
                mon->maximizeWhenImportCompletes_ = true;
                monitors_[monName]                = mon;
                isModified_                       = true;
        }
        importCsvAsync(path, monName, timeColumn, timeUnit);
}

void
Gui::beginCsvImport(const std::string &path)
{
        if (csvImportDialog_.open) {
                csvImportQueue_.push_back(path);
                return;
        }
        std::ifstream file(std::filesystem::u8path(path));
        std::string   line;
        if (!file.is_open() || !std::getline(file, line)) {
                // Preserve the existing asynchronous error handling for unreadable files
                startCsvImport(path, -2, 0);
                return;
        }

        auto splitHeader = [](const std::string &text) {
                std::vector<std::string> result;
                std::istringstream       stream(text);
                std::string              token;
                while (std::getline(stream, token, ',')) {
                        while (!token.empty() && (token.back() == '\r' || token.back() == ' '))
                                token.pop_back();
                        while (!token.empty() && token.front() == ' ')
                                token.erase(token.begin());
                        result.push_back(token);
                }
                return result;
        };

        auto headers = splitHeader(line);
        // Native exports already carry one Time column per ch and do not use
        // a shared X-axis column, so keep their lossless automatic path
        bool native = !headers.empty();
        for (const auto &header : headers) {
                const size_t kindSplit = header.rfind("::");
                if (kindSplit == std::string::npos) {
                        native = false;
                        break;
                }
                const std::string kind       = header.substr(kindSplit + 2);
                const std::string rest       = header.substr(0, kindSplit);
                const size_t      scopeSplit = rest.find("::");
                if ((kind != "Time" && kind != "Value") || scopeSplit == std::string::npos || scopeSplit == 0 ||
                    scopeSplit + 2 >= rest.size()) {
                        native = false;
                        break;
                }
        }
        if (native) {
                startCsvImport(path, -2, 0);
                return;
        }

        csvImportDialog_.path       = path;
        csvImportDialog_.headers    = std::move(headers);
        csvImportDialog_.timeColumn = -1;
        csvImportDialog_.timeUnit   = 0;
        for (int i = 0; i < static_cast<int>(csvImportDialog_.headers.size()); ++i) {
                std::string lower = csvImportDialog_.headers[i];
                std::ranges::transform(
                    lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == "t" || lower == "time" || lower == "ts" || lower == "ts" || lower == "index" || lower == "x") {
                        csvImportDialog_.timeColumn = i;
                        break;
                }
        }
        csvImportDialog_.open = true;
}

void
Gui::drawCsvImportDialog()
{
        if (!csvImportDialog_.open && !csvImportQueue_.empty()) {
                std::string next = std::move(csvImportQueue_.front());
                csvImportQueue_.erase(csvImportQueue_.begin());
                beginCsvImport(next);
        }
        if (!csvImportDialog_.open)
                return;
        ImGui::OpenPopup("###CsvImportOptions");
        if (!ImGui::BeginPopupModal(
                tr("CSV Import###CsvImportOptions", "导入 CSV###CsvImportOptions"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

        const std::string displayName = utf8Filename(csvImportDialog_.path);
        ImGui::TextUnformatted(displayName.c_str());
        ImGui::Separator();
        ImGui::Text("%s", tr("X axis / ts", "X 轴 / 时间戳"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280.0f);
        const char *preview = csvImportDialog_.timeColumn < 0 ? tr("Row number", "行号")
                                                              : csvImportDialog_.headers[csvImportDialog_.timeColumn].c_str();
        if (ImGui::BeginCombo("##csvTimeColumn", preview)) {
                if (ImGui::Selectable(tr("Row number", "行号"), csvImportDialog_.timeColumn == -1))
                        csvImportDialog_.timeColumn = -1;
                for (int i = 0; i < static_cast<int>(csvImportDialog_.headers.size()); ++i)
                        if (ImGui::Selectable(csvImportDialog_.headers[i].c_str(), csvImportDialog_.timeColumn == i))
                                csvImportDialog_.timeColumn = i;
                ImGui::EndCombo();
        }
        if (csvImportDialog_.timeColumn >= 0) {
                static const char *unitNamesEn[] = {"Auto detect", "Seconds (s)", "Milliseconds (ms)", "Microseconds (us)"};
                static const char *unitNamesZh[] = {"自动识别", "秒 (s)", "毫秒 (ms)", "微秒 (us)"};
                ImGui::Text("%s", tr("Timestamp unit", "时间戳单位"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(280.0f);
                const char *const *unitNames = g_lang == Lang::ZH ? unitNamesZh : unitNamesEn;
                ui::Choice("##csvTimeUnit", &csvImportDialog_.timeUnit, unitNames, IM_ARRAYSIZE(unitNamesEn));
        }
        ImGui::TextDisabled("%s",
                            tr("The original time origin is retained and displayed as HH:MM:SS.ffffff.",
                               "保留原始时间原点，并按 时:分:秒.毫秒微秒 显示。"));
        ImGui::Separator();
        if (ui::Button(tr("Import", "导入"), ui::BtnStyle::Success, ImVec2(120, 0))) {
                const std::string path       = csvImportDialog_.path;
                const int         timeColumn = csvImportDialog_.timeColumn;
                const int         timeUnit   = csvImportDialog_.timeUnit;
                csvImportDialog_.open        = false;
                ImGui::CloseCurrentPopup();
                startCsvImport(path, timeColumn, timeUnit);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel", "取消"), ImVec2(120, 0))) {
                csvImportDialog_.open = false;
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
}

void
Gui::drawEmptyImportPopup()
{
        if (!showEmptyImportPopup_)
                return;
        ImGui::OpenPopup("###EmptyImportData");
        if (!ImGui::BeginPopupModal(tr("Import Failed###EmptyImportData", "导入失败###EmptyImportData"),
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
                return;

        ui::ErrorText("%s",
                      emptyImportIsCsv_ ? tr("CSV data is empty.", "CSV 数据为空。")
                                        : tr("Imported data is empty.", "导入的数据为空。"));
        if (!emptyImportFile_.empty()) {
                const std::string displayName = utf8Filename(emptyImportFile_);
                ImGui::TextDisabled("%s", displayName.c_str());
        }
        ImGui::TextWrapped("%s",
                           emptyImportIsCsv_ ? tr("No importable numeric data rows were found. Please check the file contents "
                                                  "and the selected ts column.",
                                                  "没有找到可导入的数值数据行，请检查文件内容和所选时间戳列。")
                                             : tr("No importable data was found. Please check the source file.",
                                                  "没有找到可导入的数据，请检查源文件。"));
        ImGui::Separator();
        if (ui::Button(tr("OK", "确定"), ui::BtnStyle::Primary, ImVec2(120, 0))) {
                showEmptyImportPopup_ = false;
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
}

void
Gui::drawUnsupportedFilePopup()
{
        if (!showUnsupportedFilePopup_)
                return;
        ImGui::OpenPopup("###UnsupportedFileFormat");
        if (!ImGui::BeginPopupModal(tr("Unsupported File###UnsupportedFileFormat", "不支持的文件###UnsupportedFileFormat"),
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
                return;

        ui::TextColored(
            ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", tr("Parsing this file format is not supported.", "不支持解析该格式文件。"));
        ImGui::Separator();
        for (const auto &file : unsupportedFiles_) {
                const std::filesystem::path path      = std::filesystem::u8path(file);
                std::string                 extension = pathToUtf8(path.extension());
                if (extension.empty())
                        extension = tr("no extension", "无后缀");
                const std::string filename = pathToUtf8(path.filename());
                ImGui::BulletText("%s  (%s)", filename.c_str(), extension.c_str());
        }
        ImGui::Separator();
        if (ui::Button(tr("OK", "确定"), ui::BtnStyle::Primary, ImVec2(120, 0))) {
                showUnsupportedFilePopup_ = false;
                unsupportedFiles_.clear();
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
}

void
Gui::importCsvAsync(const std::string &path, const std::string &monitorName, int selectedTimeColumn, int selectedTimeUnit)
{
        std::thread([this, path, monitorName, selectedTimeColumn, selectedTimeUnit]() {
                LOG_I("CSV import started: %s", path.c_str());

                auto publishResult = [&](CsvImportPending &&result) {
                        std::lock_guard lk(mtxCsvPending_);
                        csvPendingList_.push_back(std::move(result));
                };
                auto publishEmpty = [&]() {
                        CsvImportPending result;
                        result.monitorName = monitorName;
                        publishResult(std::move(result));
                };

                try {
                        std::ifstream f(std::filesystem::u8path(path));
                        if (!f.is_open()) {
                                LOG_E("CSV import: cannot open %s", path.c_str());
                                publishEmpty();
                                return;
                        }

                        // Split a CSV line into trimmed fields (empty fields preserved)
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
                                publishEmpty();
                                return;
                        }

                        std::vector<std::string> headers = splitCsv(line);
                        if (headers.empty()) {
                                LOG_E("CSV import: no columns in %s", path.c_str());
                                publishEmpty();
                                return;
                        }

                        // Recognise this tool's own export format: every column is a
                        // "<scope>::<ch>::Time" / "<scope>::<ch>::Value" pair
                        // scope/ch may contain '_' but never "::", so the split is
                        // unambiguous (ch takes everything after the first "::")
                        auto parseNative =
                            [](const std::string &h, std::string &scope, std::string &ch, std::string &kind) -> bool {
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
                                scope = rest.substr(0, pS);
                                ch    = rest.substr(pS + 2);
                                return !scope.empty() && !ch.empty();
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
                        bool                          formatTimeOfDay   = false;
                        double                        timeOriginSeconds = 0.0;

                        if (isNative) {
                                formatTimeOfDay = true;
                                // Map each column to its ch slot; preserve first-seen order
                                std::vector<std::string> keys; // "<scope>\x1f<ch>"
                                std::vector<int>         timeCol, valCol;
                                auto                     slotFor = [&](const std::string &scope, const std::string &ch) {
                                        const std::string key = scope + '\x1f' + ch;
                                        for (size_t i = 0; i < keys.size(); ++i)
                                                if (keys[i] == key)
                                                        return static_cast<int>(i);
                                        keys.push_back(key);
                                        timeCol.push_back(-1);
                                        valCol.push_back(-1);
                                        CsvChannelImport ci;
                                        ci.scope = scope;
                                        ci.ch    = ch;
                                        outChannels.push_back(std::move(ci));
                                        return static_cast<int>(keys.size() - 1);
                                };
                                for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
                                        std::string s, c, k;
                                        parseNative(headers[i], s, c, k);
                                        const int slot                         = slotFor(s, c);
                                        (k == "Time" ? timeCol : valCol)[slot] = i;
                                }

                                // Each ch carries its own Time/Value columns; a row whose
                                // Time+Value cells are both empty means that ch has no sample
                                // there (chs may differ in length), so it is simply skipped
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
                                                outChannels[ch].tss.push_back(tv);
                                                outChannels[ch].values.push_back(static_cast<float>(vv));
                                        }
                                }
                                double firstTimestamp = std::numeric_limits<double>::infinity();
                                for (const auto &ch : outChannels)
                                        if (!ch.tss.empty())
                                                firstTimestamp = std::min(firstTimestamp, ch.tss.front());
                                if (std::isfinite(firstTimestamp)) {
                                        if (std::abs(firstTimestamp) >= 1.0e8)
                                                timeOriginSeconds = firstTimestamp;
                                        for (auto &ch : outChannels) {
                                                for (double &ts : ch.tss)
                                                        ts -= firstTimestamp;
                                                if (!ch.tss.empty())
                                                        ch.tss.front() = 0.0;
                                        }
                                }
                        } else {
                                // Generic CSV: the X axis can be any explicitly selected
                                // column. -1 keeps the legacy row-number mode; -2 auto-detects
                                // common ts header names for old saved sessions
                                int timeCol = selectedTimeColumn;
                                if (timeCol == -2) {
                                        timeCol = -1;
                                        for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
                                                std::string h = headers[i];
                                                for (auto &c : h)
                                                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                                if (h == "t" || h == "time" || h == "ts" || h == "ts" || h == "index" ||
                                                    h == "x") {
                                                        timeCol = i;
                                                        break;
                                                }
                                        }
                                }
                                if (timeCol < -1 || timeCol >= static_cast<int>(headers.size()))
                                        timeCol = -1;

                                std::vector<int>         dataCols;
                                std::vector<std::string> dataHeaders;
                                for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
                                        if (i == timeCol)
                                                continue;
                                        dataCols.push_back(i);
                                        dataHeaders.push_back(headers[i]);
                                }
                                const int nCols = static_cast<int>(dataCols.size());
                                if (nCols == 0) {
                                        LOG_E("CSV import: no data columns in %s", path.c_str());
                                        publishEmpty();
                                        return;
                                }

                                std::vector<double>             tss;
                                std::vector<std::vector<float>> columns(nCols);

                                double rowIdx = 0.0;
                                while (std::getline(f, line)) {
                                        if (line.empty() || line[0] == '#')
                                                continue;
                                        std::vector<std::string> toks = splitCsv(line);
                                        toks.resize(headers.size());

                                        double rawTime = rowIdx;
                                        if (timeCol >= 0) {
                                                const std::string &token = toks[timeCol];
                                                const auto         parsed =
                                                    std::from_chars(token.data(), token.data() + token.size(), rawTime);
                                                if (token.empty() || parsed.ec != std::errc{} ||
                                                    parsed.ptr != token.data() + token.size())
                                                        continue;
                                        }

                                        std::vector<float> rowData(nCols, 0.0f);
                                        bool               hasNumericData = false;
                                        for (int c = 0; c < nCols; ++c) {
                                                const std::string &token = toks[dataCols[c]];
                                                double             val   = 0.0;
                                                const auto         parsed =
                                                    std::from_chars(token.data(), token.data() + token.size(), val);
                                                if (!token.empty() && parsed.ec == std::errc{} &&
                                                    parsed.ptr == token.data() + token.size()) {
                                                        rowData[c]     = static_cast<float>(val);
                                                        hasNumericData = true;
                                                }
                                        }
                                        if (!hasNumericData)
                                                continue;

                                        tss.push_back(rawTime);
                                        for (int c = 0; c < nCols; ++c)
                                                columns[c].push_back(rowData[c]);
                                        rowIdx += 1.0;
                                }

                                if (tss.empty()) {
                                        LOG_E("CSV import: no data rows in %s", path.c_str());
                                        publishEmpty();
                                        return;
                                }

                                if (timeCol >= 0) {
                                        int unit        = std::clamp(selectedTimeUnit, 0, 3);
                                        formatTimeOfDay = true;
                                        if (unit == 0) {
                                                const double magnitude = std::abs(tss.front());
                                                if (magnitude >= 1.0e14)
                                                        unit = 3; // Unix microseconds
                                                else if (magnitude >= 1.0e11)
                                                        unit = 2; // Unix milliseconds
                                                else
                                                        unit = 1; // seconds, including already-relative tss
                                        }
                                        const double scale          = unit == 3 ? 1.0e-6 : (unit == 2 ? 1.0e-3 : 1.0);
                                        const double firstTimestamp = tss.front() * scale;
                                        if (std::abs(firstTimestamp) >= 1.0e8)
                                                timeOriginSeconds = firstTimestamp;
                                        for (double &ts : tss)
                                                ts = ts * scale - firstTimestamp;
                                        tss.front() = 0.0;
                                }

                                for (int c = 0; c < nCols; ++c) {
                                        CsvChannelImport ci;
                                        ci.scope  = "scope_0";
                                        ci.ch     = dataHeaders[c];
                                        ci.tss    = tss;
                                        ci.values = std::move(columns[c]);
                                        outChannels.push_back(std::move(ci));
                                }
                        }

                        if (outChannels.empty()) {
                                LOG_E("CSV import: no data in %s", path.c_str());
                                publishEmpty();
                                return;
                        }

                        LOG_I("CSV import done: %s  native=%d  chs=%zu", path.c_str(), isNative ? 1 : 0, outChannels.size());

                        CsvImportPending result;
                        result.monitorName       = monitorName;
                        result.chs               = std::move(outChannels);
                        result.formatTimeOfDay   = formatTimeOfDay;
                        result.timeOriginSeconds = timeOriginSeconds;

                        publishResult(std::move(result));
                } catch (const std::exception &e) {
                        LOG_E("CSV import failed for %s: %s", path.c_str(), e.what());
                        publishEmpty();
                } catch (...) {
                        LOG_E("CSV import failed for %s: unknown exception", path.c_str());
                        publishEmpty();
                }
        }).detach();
}

void
Gui::importAudioAsync(const std::string &path, const std::string &monitorName)
{
        std::thread([this, path, monitorName]() {
                LOG_I("Audio import started: %s", path.c_str());

                auto publishResult = [&](CsvImportPending &&result) {
                        std::lock_guard lk(mtxCsvPending_);
                        csvPendingList_.push_back(std::move(result));
                };

                CsvImportPending result;
                result.monitorName = monitorName;

#ifdef _WIN32
                HRESULT coHr       = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                bool    coDone     = SUCCEEDED(coHr);
                auto    utf8ToWide = [](const std::string &s) {
                        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                        if (len <= 0)
                                len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
                        std::wstring out(static_cast<size_t>(std::max(1, len)), L'\0');
                        if (len > 0) {
                                if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len) <= 0)
                                        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), len);
                        }
                        if (!out.empty() && out.back() == L'\0')
                                out.pop_back();
                        return out;
                };

                HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
                if (FAILED(hr)) {
                        LOG_E("Audio import: MFStartup failed 0x%08lx", (unsigned long)hr);
                        if (coDone)
                                CoUninitialize();
                        publishResult(std::move(result));
                        return;
                }

                Microsoft::WRL::ComPtr<IMFSourceReader> reader;
                {
                        Microsoft::WRL::ComPtr<IMFAttributes> attr;
                        MFCreateAttributes(&attr, 1);
                        if (attr)
                                attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                        const std::wstring wpath = utf8ToWide(path);
                        hr                       = MFCreateSourceReaderFromURL(wpath.c_str(), attr.Get(), &reader);
                }
                if (FAILED(hr) || !reader) {
                        LOG_E("Audio import: cannot open %s (0x%08lx)", path.c_str(), (unsigned long)hr);
                        MFShutdown();
                        if (coDone)
                                CoUninitialize();
                        publishResult(std::move(result));
                        return;
                }

                auto setOutputSubtype = [&](const GUID &subtype) -> HRESULT {
                        Microsoft::WRL::ComPtr<IMFMediaType> mt;
                        HRESULT                              h = MFCreateMediaType(&mt);
                        if (FAILED(h))
                                return h;
                        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        mt->SetGUID(MF_MT_SUBTYPE, subtype);
                        return reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt.Get());
                };

                GUID requestedSubtype = MFAudioFormat_Float;
                hr                    = setOutputSubtype(MFAudioFormat_Float);
                if (FAILED(hr)) {
                        requestedSubtype = MFAudioFormat_PCM;
                        hr               = setOutputSubtype(MFAudioFormat_PCM);
                }
                if (FAILED(hr)) {
                        LOG_E("Audio import: no PCM decoder for %s (0x%08lx)", path.c_str(), (unsigned long)hr);
                        MFShutdown();
                        if (coDone)
                                CoUninitialize();
                        publishResult(std::move(result));
                        return;
                }

                Microsoft::WRL::ComPtr<IMFMediaType> actualType;
                hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType);
                if (FAILED(hr) || !actualType) {
                        LOG_E("Audio import: GetCurrentMediaType failed 0x%08lx", (unsigned long)hr);
                        MFShutdown();
                        if (coDone)
                                CoUninitialize();
                        publishResult(std::move(result));
                        return;
                }

                GUID subtype = requestedSubtype;
                actualType->GetGUID(MF_MT_SUBTYPE, &subtype);
                const UINT32 chs  = MFGetAttributeUINT32(actualType.Get(), MF_MT_AUDIO_NUM_CHANNELS, 1);
                const UINT32 rate = MFGetAttributeUINT32(actualType.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
                const UINT32 bits = MFGetAttributeUINT32(
                    actualType.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, subtype == MFAudioFormat_Float ? 32 : 16);
                const UINT32 bytesPerSample = std::max<UINT32>(1, bits / 8);
                const UINT32 frameBytes     = std::max<UINT32>(1, chs * bytesPerSample);
                const bool   isFloat        = (subtype == MFAudioFormat_Float);

                CsvChannelImport ch;
                ch.scope = "scope_0";
                ch.ch    = "audio";

                uint64_t sampleIndex = 0;
                for (;;) {
                        DWORD                             streamIndex = 0;
                        DWORD                             flags       = 0;
                        LONGLONG                          sampleTime  = 0;
                        Microsoft::WRL::ComPtr<IMFSample> sample;
                        hr = reader->ReadSample(
                            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &sampleTime, &sample);
                        if (FAILED(hr)) {
                                LOG_E("Audio import: ReadSample failed 0x%08lx", (unsigned long)hr);
                                break;
                        }
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                                break;
                        if (!sample)
                                continue;

                        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
                        hr = sample->ConvertToContiguousBuffer(&buffer);
                        if (FAILED(hr) || !buffer)
                                continue;

                        BYTE *data   = nullptr;
                        DWORD maxLen = 0, curLen = 0;
                        hr = buffer->Lock(&data, &maxLen, &curLen);
                        if (FAILED(hr) || !data)
                                continue;

                        const UINT32 frames = curLen / frameBytes;
                        ch.tss.reserve(ch.tss.size() + frames);
                        ch.values.reserve(ch.values.size() + frames);

                        for (UINT32 f = 0; f < frames; ++f) {
                                double mono = 0.0;
                                for (UINT32 c = 0; c < chs; ++c) {
                                        const BYTE *p = data + static_cast<size_t>(f) * frameBytes +
                                                        static_cast<size_t>(c) * bytesPerSample;
                                        double v = 0.0;
                                        if (isFloat && bytesPerSample >= 4) {
                                                float fv = 0.0f;
                                                std::memcpy(&fv, p, sizeof(float));
                                                v = fv;
                                        } else if (bits == 8) {
                                                v = (static_cast<int>(*p) - 128) / 128.0;
                                        } else if (bits == 16) {
                                                int16_t sv = 0;
                                                std::memcpy(&sv, p, sizeof(sv));
                                                v = sv / 32768.0;
                                        } else if (bits == 24) {
                                                int32_t sv = (static_cast<int32_t>(p[0]) << 8) |
                                                             (static_cast<int32_t>(p[1]) << 16) |
                                                             (static_cast<int32_t>(p[2]) << 24);
                                                sv >>= 8;
                                                v    = sv / 8388608.0;
                                        } else if (bits >= 32) {
                                                int32_t sv = 0;
                                                std::memcpy(&sv, p, sizeof(sv));
                                                v = sv / 2147483648.0;
                                        }
                                        mono += v;
                                }
                                mono /= static_cast<double>(std::max<UINT32>(1, chs));
                                ch.tss.push_back(static_cast<double>(sampleIndex) / static_cast<double>(rate));
                                ch.values.push_back(static_cast<float>(std::clamp(mono, -1.0, 1.0)));
                                ++sampleIndex;
                        }
                        buffer->Unlock();
                }

                MFShutdown();
                if (coDone)
                        CoUninitialize();

                if (!ch.values.empty()) {
                        result.chs.push_back(std::move(ch));
                        LOG_I("Audio import finished: %s (%zu samples, %u Hz, %u ch)",
                              path.c_str(),
                              result.chs.front().values.size(),
                              rate,
                              chs);
                } else {
                        LOG_E("Audio import: no samples decoded from %s", path.c_str());
                }
#else
                LOG_E("Audio import is only implemented on Windows/MSVC for now: %s", path.c_str());
#endif

                publishResult(std::move(result));
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
                auto       it      = monitors_.find(imp.monitorName);
                Monitor   *monitor = (it != monitors_.end()) ? it->second.get() : nullptr;
                const bool hasData = std::ranges::any_of(
                    imp.chs, [](const CsvChannelImport &ch) { return !ch.values.empty() && !ch.tss.empty(); });
                if (!hasData) {
                        emptyImportFile_  = monitor ? monitor->importSourcePath_ : imp.monitorName;
                        emptyImportIsCsv_ = !monitor || monitor->importSourceType_ != "audio";
                        if (it != monitors_.end())
                                monitors_.erase(it);
                        showEmptyImportPopup_ = true;
                        isModified_           = true;
                        LOG_W("CSV import: no data rows in %s", emptyImportFile_.c_str());
                        continue;
                }
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

                for (auto &cd : imp.chs) {
                        // Restore the originating scope (no-op if already present)
                        monitor->addScope(cd.scope);
                        MonitorScope *scope = monitor->getScopes()[cd.scope].get();
                        if (!scope)
                                continue;

                        scope->addChannel(cd.ch);
                        MonitorChannel *ch = scope->findChannel(cd.ch);
                        if (!ch)
                                continue;

                        ch->historySeconds_   = 0.0f; // static data — no time-based pruning
                        ch->maxDisplayPoints_ = 5000;

                        const usize nPts = std::min(cd.values.size(), cd.tss.size());
                        if (nPts == 0)
                                continue;
                        ch->pushBatch(cd.values.data(), cd.tss.data(), nPts);
                        ch->publishSnapshot();
                        totalPts += nPts;

                        if (!cd.tss.empty()) {
                                const double lo = cd.tss.front();
                                const double hi = cd.tss.back();
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
                        monitor->prevNow_       = spanMax;
                }
                monitor->importFormatTimeOfDay_   = imp.formatTimeOfDay;
                monitor->importTimeOriginSeconds_ = imp.timeOriginSeconds;

                // Clear loading flag — monitor will show data on the next frame
                monitor->csvLoading_.store(false, std::memory_order_release);
                if (monitor->maximizeWhenImportCompletes_) {
                        monitor->fillAppOnNextDraw_           = true;
                        monitor->maximizeWhenImportCompletes_ = false;
                }

                LOG_I("CSV import: monitor '%s' ready: %zu chs, %zu points", name.c_str(), imp.chs.size(), totalPts);
        }
}

/* --------------------------------------------------------------------------
 * Auto-update UI
 * -------------------------------------------------------------------------- */

void
Gui::drawUpdateUI()
{
        const Updater::Info info = updater_.get();

        // When a check finishes, open the appropriate popup exactly once
        if (updatePendingResult_ && !updater_.isChecking() && info.checked) {
                updatePendingResult_   = false;
                updateSelectedVersion_ = 0;
                if (info.available || (updateManualCheck_ && !info.installers.empty()))
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
        if (ImGui::BeginPopupModal(tr("Install Application Version###UpdateAvailable", "安装应用版本###UpdateAvailable"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", tr("Choose a version to install.", "选择要安装的版本。"));
                ImGui::Separator();
                ImGui::Text(tr("Current: %s", "当前版本: %s"), info.currentVersion.c_str());
                ImGui::Text(tr("Latest:  %s", "最新版本: %s"), info.latestVersion.c_str());
                ImGui::TextWrapped(tr("Update URL: %s", "更新地址：%s"), info.sourceUrl.c_str());
                if (!info.notes.empty()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("%s", tr("Release notes:", "更新说明："));
                        ImGui::BeginChild("##notes", ImVec2(460, 160), true);
                        ImGui::TextWrapped("%s", info.notes.c_str());
                        ImGui::EndChild();
                }
                ImGui::Spacing();

                const bool canAutoUpdate = !info.installers.empty();
                if (canAutoUpdate) {
                        updateSelectedVersion_ =
                            std::clamp(updateSelectedVersion_, 0, static_cast<int>(info.installers.size()) - 1);
                        const auto &selected = info.installers[updateSelectedVersion_];
                        ImGui::SetNextItemWidth(240);
                        if (ImGui::BeginCombo(tr("Version", "版本"), selected.version.c_str())) {
                                for (int i = 0; i < static_cast<int>(info.installers.size()); ++i) {
                                        const auto       &installer = info.installers[i];
                                        const std::string label =
                                            installer.version + (i == 0 ? tr(" (latest)", "（最新）") : "");
                                        if (ImGui::Selectable(label.c_str(), updateSelectedVersion_ == i))
                                                updateSelectedVersion_ = i;
                                        if (updateSelectedVersion_ == i)
                                                ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                        }
                        const auto &install    = info.installers[updateSelectedVersion_];
                        const int   comparison = Updater::compareVersions(install.version, info.currentVersion);
                        if (comparison < 0)
                                ImGui::TextDisabled("%s", tr("This will install an older version.", "将安装旧版本。"));
                        else if (comparison == 0)
                                ImGui::TextDisabled("%s",
                                                    tr("This will reinstall the current version.", "将重新安装当前版本。"));
                        ImGui::BeginDisabled(dlState == Updater::DownloadState::Downloading);
                        if (ui::Button(tr("Download and Install", "下载并安装"), ui::BtnStyle::Success, ImVec2(150, 0))) {
                                // Start background download — don't exit the app
                                updater_.resetDownload();
                                updateDownloadVersion_ = install.version;
                                updater_.downloadAsync(install.url);
                                showDownloadDonePopup_ = false; // reset so we detect completion
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndDisabled();
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
                if (!updateDownloadVersion_.empty())
                        ImGui::Text(tr("Version: %s", "版本：%s"), updateDownloadVersion_.c_str());
                ImGui::Text("%s", tr("Restart the application to install the update?", "重启程序以安装更新？"));
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ui::Button(tr("Restart Now", "立即重启"), ui::BtnStyle::Warning, ImVec2(130, 0))) {
                        const std::string setupPath = updater_.getDownloadedPath();
                        if ((!hasSessionChanges() || saveSession()) && updater_.launchInstaller(setupPath)) {
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
                ui::ErrorText("%s", tr("Failed to download the update.", "更新下载失败。"));
                if (!dlErr.empty()) {
                        ImGui::Spacing();
                        ui::ErrorTextWrapped(tr("Error: %s", "错误: %s"), dlErr.c_str());
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
                ImGui::TextWrapped(tr("Update URL: %s", "更新地址：%s"), info.sourceUrl.c_str());
                if (!info.error.empty())
                        ui::ErrorTextWrapped("%s", info.error.c_str());
                else
                        ImGui::Text(tr("You're up to date (version %s).", "已是最新版本（版本 %s）。"),
                                    info.currentVersion.c_str());
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(120, 0)))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
}

/**
 * @brief Map a parser CType to the variable manager's DataType (for "+"-created LOCAL vars)
 */
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
        sp->onDrawDsoGui_ = [this] {
                dsoScope_.setOpen(true);
                dsoScope_.drawEmbedded();
        };
        // LOCAL Variable buffer lookup — search all Variable windows for the named entry
        sp->onGetVarBuf_ = [this](const std::string &varName) -> void * {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                for (auto &[_, vw] : vars_) {
                        for (const auto &ve : vw->vars_) {
                                if (ve.port == PortType::LOCAL && ve.name == varName)
                                        return vw->getLocalBuf(varName);
                        }
                }
                return nullptr;
        };
        // LOCAL Variable value read — for "$var" value substitution in arguments
        sp->onReadLocalVar_ = [this](const std::string &varName, double &out) -> bool {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                for (auto &[_, vw] : vars_)
                        if (vw->readLocalScalar(varName, out))
                                return true;
                return false;
        };
        sp->onVarWritten_ = [this](const std::string &varName) {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
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
                std::vector<std::string> names;
                std::unique_lock         lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                for (auto &[_, vw] : vars_)
                        for (const auto &ve : vw->vars_)
                                if (ve.port == PortType::LOCAL)
                                        names.push_back(ve.name);
                return names;
        };
        sp->onSnapshotLocalBuffers_ = [this]() {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                SdkPanel::LocalBuffers buffers;
                for (const auto &[_, variable] : vars_) {
                        auto snapshot = variable->snapshotLocalBuffers();
                        buffers.insert(std::make_move_iterator(snapshot.begin()), std::make_move_iterator(snapshot.end()));
                }
                return buffers;
        };
        sp->onLocalSampleSource_ = [this](const std::string &name) {
                std::unique_lock lock(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lock.lock();
                for (auto &variable : vars_ | std::views::values)
                        variable->markLocalSampleSource(name);
        };
        sp->onLocalSample_ = [this](const std::string &name, const std::vector<uint8_t> &bytes, double time) {
                std::unique_lock lock(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lock.lock();
                for (auto &variable : vars_ | std::views::values)
                        variable->publishLocalSample(name, bytes, time);
        };
        sp->onCompareExchangeLocalBuffer_ =
            [this](const std::string &name, const std::vector<uint8_t> &expected, const std::vector<uint8_t> &desired) {
                    std::unique_lock lk(mtxMonitors_, std::defer_lock);
                    if (!s_monitorMutexOwnedBySessionLoad)
                            lk.lock();
                    for (const auto &[_, variable] : vars_)
                            for (const auto &entry : variable->vars_)
                                    if (entry.port == PortType::LOCAL && entry.name == name)
                                            return variable->compareExchangeLocalBuffer(name, expected, desired);
                    return false;
            };

        // Store a call's return value into an EXISTING LOCAL variable (the one the user
        // created via the "+" button). Never creates — a no-op if no such variable exists
        // It plots only if the user also adds it to a monitor (LOCAL→monitor feed)
        // isFloat
        sp->onWriteLocalScalar_ = [this](const std::string &name, double val, bool) {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                for (auto &[_, vw] : vars_)
                        for (const auto &ve : vw->vars_)
                                if (ve.name == name) {
                                        vw->setLocalScalar(name, val);
                                        vw->notifyLocalWrite(name);
                                        return;
                                }
        };
        // Create a LOCAL variable in the variable manager (the "+" buttons). Scalar when
        // structDecl is null, otherwise a struct variable with the decl's fields. Creates
        // a Variable manager window if none is open. No-op if the name already exists
        sp->onCreateLocalVar_ = [this](const std::string &name, CType scalarType, const CStructDecl *sd) {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
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
                                if (f.name == "reserved" || f.name.rfind("reserved.", 0) == 0 ||
                                    f.name.find(".reserved") != std::string::npos)
                                        continue;
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
                                                emit(f.name + "[" + std::to_string(ai) + "]",
                                                     f.arrayElemType,
                                                     f.offset + ai * esz);
                                } else {
                                        emit(f.name, f.type, f.offset);
                                }
                        }
                        target->addLocalStructVar(name, fields, sd->totalSize);
                } else {
                        target->addLocalVar(name, ctypeToVarDataType(scalarType), 8);
                }
                for (auto &variable : vars_ | std::views::values)
                        variable->markLocalSampleSource(name);
        };
        // Publish the SDK header's enum definitions onto the LOCAL variable's members
        // and into any ch already plotted from them, so editing an enum in the
        // header reaches the monitor without re-dragging the field
        sp->onPublishLocalEnums_ = [this](const std::string                                                  &varName,
                                          const std::vector<std::pair<std::string, std::vector<CEnumValue>>> &memberEnums) {
                std::unique_lock lk(mtxMonitors_, std::defer_lock);
                if (!s_monitorMutexOwnedBySessionLoad)
                        lk.lock();
                std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> defs;
                for (const auto &[path, values] : memberEnums) {
                        auto &slot = defs[path];
                        slot.reserve(values.size());
                        for (const auto &ev : values)
                                slot.push_back({ev.name, ev.val});
                }
                for (auto &[_, vw] : vars_) {
                        if (!vw->setLocalMemberEnums(varName, defs))
                                continue;
                        for (auto &mp : monitors_)
                                for (auto &spair : mp.second->getScopes())
                                        for (auto &cpair : spair.second->getChannels())
                                                vw->refreshLocalChannelEnums(varName, cpair.second.get());
                }
        };
        // Wire sequence editor LOCAL variable lookup (same lambda as SdkPanel)
        if (!seqEditor_.onGetLocalBuf_) {
                seqEditor_.onGetLocalBuf_ = [this](const std::string &varName) -> void * {
                        std::unique_lock lk(mtxMonitors_, std::defer_lock);
                        if (!s_monitorMutexOwnedBySessionLoad)
                                lk.lock();
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
                        std::unique_lock lk(mtxMonitors_, std::defer_lock);
                        if (!s_monitorMutexOwnedBySessionLoad)
                                lk.lock();
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
        // Wire sequence editor LOCAL variable creation (for "→ Create output vars" button)
        if (!seqEditor_.onAddLocalVar_) {
                seqEditor_.onAddLocalVar_ = [this](const std::string &name, DataType type, size_t bufSize) {
                        std::unique_lock lk(mtxMonitors_, std::defer_lock);
                        if (!s_monitorMutexOwnedBySessionLoad)
                                lk.lock();
                        if (vars_.empty())
                                return;
                        vars_.begin()->second->addLocalVar(name, type, bufSize);
                };
        }
        if (!seqEditor_.onAddLocalStructVar_) {
                seqEditor_.onAddLocalStructVar_ =
                    [this](const std::string &name, const std::vector<SeqStructField> &fields, size_t totalSize) {
                            std::unique_lock lk(mtxMonitors_, std::defer_lock);
                            if (!s_monitorMutexOwnedBySessionLoad)
                                    lk.lock();
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
