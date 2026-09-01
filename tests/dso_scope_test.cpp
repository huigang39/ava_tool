#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void
check(bool v, const char *s)
{
        if (!v)
                throw std::runtime_error(s);
}
struct SdkPanelRegression {
        static void openDso(SdkPanel &panel, DsoScope &scope)
        {
                panel.onDrawDsoGui_ = [&scope] { scope.drawEmbedded(); };
                check(panel.loadSdkPreset(2), "DSOX2024A shortcut loads bundled SDK");
                check(panel.isDsoGuiActive(), "shortcut selects embedded oscilloscope GUI");
        }
};
struct DsoScopeTestAccess {
        static void connect(DsoScope &s, int port)
        {
                s.config_.usb     = false;
                s.config_.port    = port;
                s.config_.timeout = 150;
                snprintf(s.host_, sizeof(s.host_), "127.0.0.1");
                s.setOpen(true);
                s.submit(dso::Action::Connect);
        }
        static void wait(DsoScope &s)
        {
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (s.busy() || s.disconnectPending_) {
                        s.poll();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        check(std::chrono::steady_clock::now() < end, "worker completion timeout");
                }
        }
        static void capture(DsoScope &s)
        {
                s.config_.points = 1000;
                s.config_.capture.fill(true);
                s.submit(dso::Action::Capture);
                wait(s);
        }
        static Monitor &monitor(DsoScope &s) { return *s.monitor_; }
        static bool     connected(DsoScope &s) { return s.connected_; }
        static void     timeout(DsoScope &s)
        {
                snprintf(s.command_, sizeof(s.command_), "TEST:TIMEOUT?");
                s.autoRead_ = true;
                s.submit(dso::Action::Query);
                wait(s);
                check(s.error_ != 0 && !s.connected_ && !s.autoRead_, "timeout disconnects and stops refresh");
        }
};
int
main(int argc, char **argv)
{
        if (argc != 3)
                return 2;
        module_init();
        try {
                const int       port = std::stoi(argv[2]);
                dso::Controller device;
                dso::Config     cfg;
                cfg.usb     = false;
                cfg.host    = "127.0.0.1";
                cfg.port    = port;
                cfg.timeout = 150;
                auto r      = device.execute(dso::Action::Connect, cfg);
                check(!r.error && r.connected && r.settingsValid && r.settings.channels[3].probe == 10,
                      "connect reads actual settings");
                cfg                      = r.settings;
                cfg.channel              = 2;
                cfg.channels[2].scale    = .2;
                cfg.channels[2].offset   = -.5;
                cfg.channels[2].coupling = 1;
                r                        = device.execute(dso::Action::Channel, cfg);
                check(!r.error && r.settings.channels[2].scale == .2 && r.settings.channels[2].offset == -.5 &&
                          r.settings.channels[2].coupling == 1,
                      "channel applies and reads back");
                cfg.timeScale    = 2e-6;
                cfg.timePosition = -3e-6;
                r                = device.execute(dso::Action::Timebase, cfg);
                check(!r.error && r.settings.timePosition == -3e-6 && r.settings.timeScale == 2e-6, "timebase readback");
                for (auto a : {dso::Action::Trigger,
                               dso::Action::Run,
                               dso::Action::Stop,
                               dso::Action::Single,
                               dso::Action::Digitize,
                               dso::Action::Autoscale,
                               dso::Action::Reset})
                        check(!device.execute(a, cfg).error, "instrument action");
                cfg.points = 16;
                cfg.capture.fill(true);
                r = device.execute(dso::Action::Capture, cfg);
                check(!r.error && r.captured && r.waves[3].values.size() == 32,
                      "four-channel capture retries actual point count");
                check(std::abs(r.waves[0].times.front() + 18e-9) < 1e-20, "preamble origin/reference preserved");
                check(std::abs(r.waves[0].times[1] - r.waves[0].times[0] - 1e-9) < 1e-20, "nanosecond sample interval");
                check(std::abs(r.waves[0].values[0] - 1.0f) < 1e-5, "calibrated voltages");
                r = device.execute(dso::Action::Measure, cfg);
                check(!r.error && r.measured && r.measurements[3][0] == 12500000, "measurements");
                cfg.channels[2].scale = -1;
                check(device.execute(dso::Action::Channel, cfg).error, "invalid scale rejected");
                cfg.command = "INVALID";
                r           = device.execute(dso::Action::Write, cfg);
                check(r.error == -7 && r.connected, "instrument error shown without losing connection");
                cfg.command = "TEST:MALFORMED";
                check(!device.execute(dso::Action::Write, cfg).error, "enable malformed fixture");
                r = device.execute(dso::Action::Capture, cfg);
                check(r.error == -5 && !r.captured && !r.connected,
                      "invalid preamble rejected without publishing or allocating unbounded memory");
                device.close();
                std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
                check(glfwInit(), "GLFW");
                glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                auto gui        = std::make_unique<Gui>(argv[1]);
                g_lang          = Lang::ZH;
                auto &io        = ImGui::GetIO();
                io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
                io.IniFilename  = nullptr;
                DsoScope scope;
                SdkPanel panel;
                SdkPanelRegression::openDso(panel, scope);
                using T = DsoScopeTestAccess;
                T::connect(scope, port);
                T::wait(scope);
                check(T::connected(scope), "GUI connects asynchronously");
                const auto baseline = Monitor::getGlobalPointCount();
                T::capture(scope);
                check(Monitor::getGlobalPointCount() == baseline + 4000, "scope points included in global total");
                auto render = [&] {
                        ImGui_ImplOpenGL3_NewFrame();
                        ImGui_ImplGlfw_NewFrame();
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(1250, 820));
                        panel.draw();
                        ImGui::Render();
                        glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                };
                for (int i = 0; i < 4; ++i)
                        render();
                check(!ImGui::FindWindowByName("DSOX2024A 示波器###DSOX2024A"), "no separate instrument window");
                float waveformX = -1, controlsX = -1;
                for (auto *window : ImGui::GetCurrentContext()->Windows) {
                        const std::string name = window->Name;
                        if (name.find("/scopeMonitor_") != std::string::npos)
                                waveformX = window->Pos.x;
                        if (name.find("/controls_") != std::string::npos)
                                controlsX = window->Pos.x;
                }
                check(waveformX >= 0 && controlsX > waveformX, "embedded waveform is left of instrument controls");
                auto &monitor = T::monitor(scope);
                check(monitor.linkXMin_ < 0 && monitor.linkXMax_ > 0 && monitor.linkXMax_ - monitor.linkXMin_ < 2e-6,
                      "negative trigger time and sub-microsecond viewport");
                int width, height;
                glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                std::vector<unsigned char> pixels(width * height * 3);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                std::ofstream image(std::string(argv[1]) + ".ppm", std::ios::binary);
                image << "P6\n" << width << ' ' << height << "\n255\n";
                for (int y = height - 1; y >= 0; --y)
                        image.write((char *)pixels.data() + y * width * 3, width * 3);
                image.close();
                monitor.setSamplingPaused(true);
                T::capture(scope);
                check(Monitor::getGlobalPointCount() == baseline + 4000, "paused display retains last capture");
                monitor.setSamplingPaused(false);
                T::capture(scope);
                check(Monitor::getGlobalPointCount() == baseline + 4000,
                      "new captures replace instead of accumulating history");
                T::timeout(scope);
                T::connect(scope, port);
                T::wait(scope);
                scope.setOpen(false);
                T::wait(scope);
                check(!T::connected(scope), "closing panel disconnects");
                auto *saved = cJSON_CreateObject();
                scope.save(saved);
                scope.load(saved);
                cJSON_Delete(saved);
                check(!T::connected(scope), "session restore never reconnects");
                scope.shutdown();
                gui.reset();
                puts("PASS: DSOX2024A controls, calibrated four-channel waveform, buffer resize, measurements, errors, async "
                     "lifecycle, session, shared monitor and negative nanosecond axis");
                return 0;
        } catch (const std::exception &e) {
                fprintf(stderr, "FAIL: %s\n", e.what());
                return 1;
        }
}
