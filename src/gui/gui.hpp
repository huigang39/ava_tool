#ifndef GUI_HPP
#define GUI_HPP

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GLFW/glfw3.h"
#include "ckit.h"

#include "core/updater.hpp"
#include "gui/assembly_viewer.hpp"
#include "gui/audio_input.hpp"
#include "gui/bkp_sram_debugger.hpp"
#include "gui/bode.hpp"
#include "gui/device_manager.hpp"
#include "gui/dock_session.hpp"
#include "gui/dso_scope.hpp"
#include "gui/flash_tool.hpp"
#include "gui/hover_reveal.hpp"
#include "gui/i18n.hpp"
#include "gui/inverter_simulator.hpp"
#include "gui/island_style.hpp"
#include "gui/midi_tool.hpp"
#include "gui/monitor.hpp"
#include "gui/motor_control.hpp"
#include "gui/platform_style.hpp"
#include "gui/remote_files.hpp"
#include "gui/sdk_panel.hpp"
#include "gui/variable.hpp"
#include "platform/app_capture.hpp"
#include "platform/appearance.hpp"
#include "platform/system_tray.hpp"
#include "sequence_editor.hpp"

class Gui
{
        friend struct SimSamplerTestAccess;

      public:
        using MonitorMapType  = std::unordered_map<std::string, std::shared_ptr<Monitor>>;
        using VariableMapType = std::unordered_map<std::string, std::shared_ptr<Variable>>;

        static std::string getAppDir();

      private:
        static void glfwErrCb(int err, const char *desc);
        static void glfwDropCb(GLFWwindow *window, int count, const char **paths);

        static std::vector<std::string> sDroppedFiles_;

#ifdef __APPLE__
        static constexpr char *glslVer_ = (char *)"#version 150";
#else
        static constexpr char *glslVer_ = (char *)"#version 130";
#endif

#ifdef _WIN32
        std::string fontFile_ = "C:/Windows/Fonts/msyh.ttc";
#elif defined(__APPLE__)
        std::string fontFile_ = "/System/Library/Fonts/STHeiti Medium.ttc";
#else
        std::string fontFile_ = "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc";
#endif

        GLFWwindow           *window_ = nullptr;
        app_capture::Recorder recorder_;
        bool                  screenshotRequested_{};
        void                  drawCaptureControls(bool compact);
        void                  captureRenderedFrame(int width, int height);
        std::string           windowTitle_ = "AvA Tool";
        int                   windowWidth_{1280}, windowHeight_{720};
        f32                   xScale_{}, yScale_{};
        float                 uiScale_            = 1.0f;
        ui::ColorScheme       colorScheme_        = ui::ColorScheme::Ocean;
        bool                  platformStyleDirty_ = false;
        appearance::Mode      appearanceMode_     = appearance::Mode::System;
        bool                  appearanceDark_ = true, systemAppearanceDark_ = true;
        int                   lightStartHour_ = 7, darkStartHour_ = 19;
        double                nextAppearanceQuery_ = 0;
        std::future<bool>     appearanceQuery_;
        void                  loadAppearance();
        void                  saveAppearance();
        void                  updateAppearance();
        void                  drawAppearanceMenu();
        void                  drawStorageSettings();

        MonitorMapType     monitors_{};
        mutable std::mutex mtxMonitors_{};
        VariableMapType    vars_{};

        std::string currentSessionPath_ = "session.ava";
        bool        isModified_         = false;
        bool        showQuitModal_      = false;
        bool        wantsToQuit_        = false;
        bool        exitRequested_      = false;
        bool        minimizeRequested_  = false;
        enum class CloseBehavior { Ask, Exit, Tray };
        CloseBehavior    closeBehavior_       = CloseBehavior::Ask;
        bool             showCloseChoice_     = false;
        bool             rememberCloseChoice_ = false;
        bool             closeChoiceError_    = false;
        bool             inTray_              = false;
        SystemTray       systemTray_;
        std::vector<u32> trayViewportIds_;
        bool             minimizeToTray();
        void             restoreFromTray();
        void             requestExit();
        void             processWindowActions();
        void             updatePlatformWindows();
        void             drawCloseChoicePopup();
        bool             chooseCloseBehavior(CloseBehavior behavior, bool remember);
        void             saveWindowPreferences();
        bool             skipAutoSave_        = false; // set when user explicitly chose "Don't Save"
        bool             showElevationPrompt_ = false;
        std::string      elevationError_;
        void             drawElevationPrompt();
        bool             isFirstSave_ = true;
        DockSession      dockSession_;
        LocalFileServer  localFileServer_;
        LocalFileServer  websiteServer_{LocalFileServer::Mode::Website};

        void drawBar();
        enum class AppCommand {
                None,
                Home,
                Open,
                New,
                Save,
                SaveAs,
                Variables,
                Monitor,
                Devices,
                Flash,
                Parameters,
                Bode,
                Motor,
                Calculator,
                Scope,
                Sdk,
                Trace,
                Sequence,
                Safety,
                Connect,
                Disconnect,
                FreezeDisplay,
                PauseAcquisition,
                ClearData,
                Commands
        };
        AppCommand      pendingCommand_ = AppCommand::None;
        bool            showHome_       = false;
        ImGuiID         workspaceDock_  = 0;
        bool            showCommands_   = false;
        bool            commandFocus_   = false;
        char            commandQuery_[128]{};
        int             commandSelection_ = 0;
        bool            showClearData_    = false;
        ui::HoverReveal jlinkReveal_, workspaceActionsReveal_;
        enum class SessionChange { None, New, Open };
        SessionChange  sessionChange_      = SessionChange::None;
        bool           sessionChangeReady_ = false;
        std::string    sessionChangePath_, sessionChangeSource_;
        void           drawWorkspaceChrome();
        void           drawHome();
        void           drawCommands();
        void           drawStartupSessionPicker();
        bool           chooseStartupSession(const std::string &path);
        void           startBlankStartupSession();
        void           executeCommand();
        void           addVariableWindow();
        void           addMonitorWindow();
        bool           hasSessionChanges() const;
        std::string    sessionUiState() const;
        void           captureSessionBaseline(bool settleLayout = false);
        std::string    sessionUiBaseline_, sessionLayoutBaseline_;
        int            sessionLayoutSettle_{};
        void           requestSessionChange(SessionChange action, const std::string &path = "", const std::string &source = "");
        void           applySessionChange();
        void           drawSessionChangePrompt();
        void           newSession();
        void           loadSession(const std::string &path = "", const std::string &sourceUrl = "");
        void           pollRemoteSession();
        RemoteFileMenu remoteSessions_;
        std::future<std::string> remoteSessionDownload_;
        std::string              remoteSessionUrl_;
        bool                     saveSession(const std::string &path = "");
        bool                     saveSessionAs(); // returns false if user cancelled the dialog

        // Recent sessions (File menu). Persisted to <appdir>/recent.txt
        std::vector<std::string> recentSessions_;
        void                     loadRecentList();
        void                     saveRecentList();
        void                     addRecent(const std::string &path);
        void                     rememberLastSession();
        bool                     startupSessionPickerPending_ = false;
        size_t                   startupSessionSelection_     = 0;

        /**
         * @brief mmap disk-cache directory (Settings). Persisted to <appdir>/cache_dir.txt
         */
        void loadCacheDirSetting(); // reads + applies the saved cache dir (call early)
        void setCacheDir(const std::string &dir);

        /**
         * @brief UI language (Settings). Persisted to <appdir>/lang.txt
         */
        void loadLangSetting();
        void setLang(Lang lang);
        void drawCalculator();
        void syncSymbolAddresses(Variable *reloadedVar);
        void syncVariableProperties(Variable *var);

        struct MotorProfile {
                char  modelName[64] = "Motor_A";
                float Rs            = 0.1f;
                float Ld            = 0.0001f;
                float Lq            = 0.0001f;
                int   polePairs     = 7;
                float Kt            = 0.05f;
                float backEmfFreq   = 100.0f;
                float backEmfVpp    = 20.0f;
        };
        std::vector<MotorProfile> motorProfiles_;
        int                       currentMotorProfile_ = 0;

        bool              showCalculator_{false};
        Bode              bode_{};
        InverterSimulator inverterSimulator_{};
        BkpSramDebugger   bkpSramDebugger_{};
        AssemblyViewer    asmViewer_{};
        SequenceEditor    seqEditor_{};
        DeviceManager     deviceManager_{};
        FlashTool         flashTool_{};
        MidiTool          midiTool_{};
        MotorControl      motorControl_{};
        DsoScope          dsoScope_{};

        // SDK Debug — multi-instance windows
        std::vector<std::shared_ptr<SdkPanel>> sdkPanels_;
        int                                    nextSdkWinId_{0};
        void                                   newSdkPanel();

        struct CsvChannelImport {
                std::string         scope; // scope this ch belongs to
                std::string         ch;    // ch (column) name
                std::vector<double> tss;
                std::vector<float>  values;
        };
        struct CsvImportPending {
                std::string                   monitorName;
                std::vector<CsvChannelImport> chs;
                bool                          formatTimeOfDay{false};
                double                        timeOriginSeconds{0.0};
        };
        std::mutex                    mtxCsvPending_{};
        std::vector<CsvImportPending> csvPendingList_{};

        void importCsvAsync(const std::string &path, const std::string &monitorName, int timeColumn = -2, int timeUnit = 0);
        void importAudioAsync(const std::string &path, const std::string &monitorName);
        void processPendingCsvImports();

        struct CsvImportDialog {
                bool                     open{false};
                std::string              path;
                std::vector<std::string> headers;
                int                      timeColumn{-1};
                int                      timeUnit{0};
        } csvImportDialog_{};
        std::vector<std::string> csvImportQueue_{};
        bool                     showEmptyImportPopup_{false};
        std::string              emptyImportFile_{};
        bool                     emptyImportIsCsv_{true};
        bool                     showUnsupportedFilePopup_{false};
        std::vector<std::string> unsupportedFiles_{};
        void                     beginCsvImport(const std::string &path);
        void                     drawCsvImportDialog();
        void                     drawEmptyImportPopup();
        void                     drawUnsupportedFilePopup();
        void                     startCsvImport(const std::string &path, int timeColumn, int timeUnit = 0);

        // Available installers from the configured update directory
        Updater     updater_{};
        bool        updateCheckStarted_    = false; // startup check kicked off
        bool        updateManualCheck_     = false; // user-initiated → also report "up to date"
        bool        updatePendingResult_   = false; // a check is in flight; show result when done
        bool        showDownloadDonePopup_ = false; // download finished; show restart prompt
        int         updateSelectedVersion_ = 0;
        std::string updateDownloadVersion_;
        void        drawUpdateUI();
        /**
         * @brief Throttle auto-checks (persisted to <appdir>/update_check.txt)
         */
        bool shouldAutoCheckUpdate();
        void recordUpdateCheck();

      public:
        Gui(const std::string &initialPath = "");
        ~Gui();

        void loop();
        void hide();

        MonitorMapType  &getMonitors() { return monitors_; }
        std::mutex      &getMonitorMtx() { return mtxMonitors_; }
        VariableMapType &getVars() { return vars_; }

        static std::vector<std::string> &getDroppedFiles() { return sDroppedFiles_; }
        static void                      clearDroppedFiles() { sDroppedFiles_.clear(); }
};

#endif // !GUI_HPP
