#ifndef GUI_HPP
#define GUI_HPP

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GLFW/glfw3.h"
#include "module.h"

#include "gui/assembly_viewer.hpp"
#include "gui/bode.hpp"
#include "gui/monitor.hpp"
#include "gui/register_viewer.hpp"
#include "gui/variable.hpp"

class Gui
{
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

        GLFWwindow *window_      = nullptr;
        std::string windowTitle_ = "AvA Tool";
        int         windowWidth_{1280}, windowHeight_{720};
        f32         xScale_{}, yScale_{};

        MonitorMapType     monitors_{};
        mutable std::mutex mtxMonitors_{};
        VariableMapType    vars_{};

        std::string currentSessionPath_   = "session.ava";
        bool        isModified_           = false;
        bool        showQuitModal_        = false;
        bool        showElevationModal_   = false;
        int         pendingElevationCore_ = -1;
        bool        wantsToQuit_          = false;
        bool        isFirstSave_          = true;
        f32         saveToastAlpha_       = 0.0f;

        void drawBar();
        void loadSession(const std::string &path = "");
        void saveSession(const std::string &path = "");
        bool saveSessionAs(); // returns false if user cancelled the dialog
        void drawCalculator();
        void syncSymbolAddresses(Variable *reloadedVar);

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

        bool           showCalculator_{false};
        Bode           bode_{};
        RegisterViewer regViewer_{};
        AssemblyViewer asmViewer_{};

        struct CsvImportPending {
                std::string                     monitorName;
                std::vector<std::string>        headers;
                std::vector<double>             timestamps;
                std::vector<std::vector<float>> columns;
        };
        std::mutex                    mtxCsvPending_{};
        std::vector<CsvImportPending> csvPendingList_{};

        void importCsvAsync(const std::string &path, const std::string &monitorName);
        void processPendingCsvImports();

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
