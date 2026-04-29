#ifndef GUI_HPP
#define GUI_HPP

#include <memory>
#include <string>
#include <vector>

#include "module.h"

#include "GLFW/glfw3.h"

#include "editor.hpp"
#include "monitor.hpp"

class Gui
{
      public:
        using MonitorMapType = std::unordered_map<std::string, std::unique_ptr<Monitor>>;
        using EditorMapType  = std::unordered_map<std::string, std::unique_ptr<Editor>>;

      private:
        static void glfwErrCb(int err, const char *desc);
        static void glfwDropCb(GLFWwindow *window, int count, const char **paths);

        static std::vector<std::string> sDroppedFiles_;

        static constexpr char *glslVer_ = (char *)"#version 130";

        std::string fontFile_ = "C:/Windows/Fonts/msyh.ttc";

        GLFWwindow *window_      = nullptr;
        std::string windowTitle_ = "Ava Tool";
        int         windowWidth_{1280}, windowHeight_{720};
        f32         xScale_{}, yScale_{};

        MonitorMapType monitors_{};
        EditorMapType  editors_{};

        void drawBar();

      public:
        Gui();
        ~Gui();

        void loop();

        MonitorMapType &getMonitors() { return monitors_; }

        static std::vector<std::string> &getDroppedFiles() { return sDroppedFiles_; }
};

#endif // !GUI_HPP
