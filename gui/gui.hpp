#ifndef GUI_HPP
#define GUI_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>


#include "module.h"

#include "GLFW/glfw3.h"

#include "monitor.hpp"
#include "parser.hpp"


class Gui
{
      public:
        using MonitorMapType = std::unordered_map<std::string, std::unique_ptr<Monitor>>;
        using ParserMapType  = std::unordered_map<std::string, std::unique_ptr<Parser>>;

      private:
        static void glfwErrCb(int err, const char *desc);
        static void glfwDropCb(GLFWwindow *window, int count, const char **paths);

        static std::vector<std::string> sDroppedFiles_;

        static constexpr char *glslVer_ = (char *)"#version 130";

        std::string fontFile_ = "C:/Windows/Fonts/msyh.ttc";

        GLFWwindow *window_      = nullptr;
        std::string windowTitle_ = "AvA Tool";
        int         windowWidth_{1280}, windowHeight_{720};
        f32         xScale_{}, yScale_{};

        MonitorMapType monitors_{};
        std::mutex     mtxMonitors_{};
        ParserMapType  parsers_{};

        static constexpr const char *sessionPath_ = "session.json";

        void drawBar();
        void loadSession();
        void saveSession() const;
        void drawCalculator();
        void syncSymbolAddresses(const ElfInfo &elfInfo);

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

        bool showCalculator_{false};

      public:
        Gui();
        ~Gui();

        void loop();

        MonitorMapType &getMonitors() { return monitors_; }
        std::mutex     &getMonitorMtx() { return mtxMonitors_; }

        static std::vector<std::string> &getDroppedFiles() { return sDroppedFiles_; }
        static void                      clearDroppedFiles() { sDroppedFiles_.clear(); }
};

#endif // !GUI_HPP
