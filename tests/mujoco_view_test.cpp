#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/mujoco_view.hpp"
#include "sim_stream.hpp"
#include "implot.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>
#include <limits>

static void check(bool val, const char *message) { if (!val) throw std::runtime_error(message); }
struct SimSamplerTestAccess {
        static void save(Gui &gui, const std::string &path) { gui.saveSession(path); }
};
struct MuJoCoViewTestAccess {
        static void mode(Monitor &monitor, MonitorViewMode mode) { monitor.viewMode_ = mode; }
};
struct SceneCapture {
        ImVec2 min{}, max{};
        int width = 0, height = 0;
        std::vector<unsigned char> pixels;
};
static SceneCapture captureScene()
{
        auto *draw = ImGui::GetDrawData();
        for (int i = 0; i < draw->CmdListsCount; ++i)
                for (const auto &command : draw->CmdLists[i]->CmdBuffer) {
                        if (command.GetTexID() == ImGui::GetIO().Fonts->TexRef.GetTexID() || command.ElemCount != 6)
                                continue;
                        SceneCapture result;
                        result.min = ImVec2(FLT_MAX, FLT_MAX); result.max = ImVec2(-FLT_MAX, -FLT_MAX);
                        for (unsigned n = 0; n < command.ElemCount; ++n) {
                                const auto &vertex = draw->CmdLists[i]->VtxBuffer[
                                    command.VtxOffset + draw->CmdLists[i]->IdxBuffer[command.IdxOffset + n]];
                                result.min.x = std::min(result.min.x, vertex.pos.x); result.min.y = std::min(result.min.y, vertex.pos.y);
                                result.max.x = std::max(result.max.x, vertex.pos.x); result.max.y = std::max(result.max.y, vertex.pos.y);
                        }
                        GLint previous = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
                        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(command.GetTexID()));
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &result.width);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &result.height);
                        result.pixels.resize(result.width * result.height * 3);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, result.pixels.data());
                        glBindTexture(GL_TEXTURE_2D, previous);
                        return result;
                }
        throw std::runtime_error("scene image draw command is present");
}

int main(int argc, char **argv)
{
        setvbuf(stdout, nullptr, _IONBF, 0);
        if (argc != 4) return 2;
        module_init();
        check(glfwInit(), "GLFW");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        GLFWwindow *window = glfwCreateWindow(1000, 800, "MuJoCo test", nullptr, nullptr);
        check(window != nullptr, "OpenGL window"); glfwMakeContextCurrent(window);
        ImGui::CreateContext(); ImPlot::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigInputTrickleEventQueue = false;
        ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 130");
        int result = 0;
        try {
                ava_mujoco::Control cfg; ava_mujoco::Controller controller;
                cfg.positionTarget=.5;cfg.velocityTarget=2;cfg.torqueFeedforward=3;
                check(std::abs(controller.step(cfg,.2,1,.01)-43)<1e-10,"PD includes both references and torque feedforward");
                cfg.mode=ava_mujoco::Mode::Velocity;cfg.velocityKp=2;cfg.velocityKi=3;
                cfg.velocityTarget=1;cfg.torqueFeedforward=.25;
                check(std::abs(controller.step(cfg,100,.5,.1)-1.4)<1e-10,"velocity PI ignores position and adds torque feedforward");
                cfg.mode=ava_mujoco::Mode::Position;cfg.positionKp=4;cfg.velocityKi=0;cfg.velocityFeedforward=.3;
                check(std::abs(controller.step(cfg,0,.1,.1)-4.65)<1e-10,"position P feeds velocity PI with velocity and torque feedforward");
                cfg.mode=ava_mujoco::Mode::Velocity;cfg.velocityTarget=100;cfg.velocityKi=20;cfg.torqueFeedforward=0;
                for(int i=0;i<100;++i)check(controller.step(cfg,0,0,.01,-2,2)==2,"velocity torque is limited");
                cfg.velocityTarget=0;
                check(controller.step(cfg,0,0,.01,-2,2)==0,"saturation does not leave integral windup");
                puts("PASS: PD, velocity PI, cascaded position, feedforward, torque limits and anti-windup");
                const std::string region = std::string(ava_sim::defaultRegion) + ".test." + std::to_string(GetCurrentProcessId());
                Monitor monitor("MuJoCo test"); monitor.mujocoEnabled_ = true;
                monitor.mujocoModelPath_ = argv[2]; monitor.addScope("Position");
                for (unsigned i : {0u, 1u}) {
                        auto scope = monitor.getScopes().at("Position"); scope->addChannel(ava_sim::fields[i]);
                        auto *ch = scope->findChannel(ava_sim::fields[i]); ch->setDevice(ava_sim::device);
                        ch->setType("f32"); ch->setShmRegionName(region); ch->setSimField(i);
                }
                auto frame = [&](ImVec2 mouse = ImVec2(-100, -100), int button = -1, bool down = false,
                                 bool ctrl = false, float wheel = 0, float dpi = 1) {
                        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                        auto &io = ImGui::GetIO(); io.DisplayFramebufferScale = ImVec2(dpi, dpi);
                        io.AddMousePosEvent(mouse.x, mouse.y); io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
                        for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, b == button && down);
                        if (wheel) io.AddMouseWheelEvent(0, wheel);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1000, 800));
                        monitor.updateDisplay(); ImGui::Render(); glViewport(0, 0, 1000, 800);
                        glClearColor(0.05f, 0.05f, 0.05f, 1); glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
                };
                frame(); check(monitor.mujocoView_ && monitor.mujocoView_->error().empty(), "load model and render scene");
                auto &view = *monitor.mujocoView_;
                const auto originalScene = captureScene();
                check(originalScene.min.x > 500 && originalScene.max.x <= 1000,
                      "MuJoCo model is rendered in the right-hand panel beside the waveform area");
                frame(ImVec2(-100, -100), -1, false, false, 0, 2);
                const auto highDpi = captureScene();
                check(highDpi.width >= (originalScene.max.x - originalScene.min.x) * 1.9f &&
                      highDpi.height >= (originalScene.max.y - originalScene.min.y) * 1.9f, "2x DPI gets a 2x framebuffer");
                frame();
                auto scene = captureScene();
                const ImVec2 center((scene.min.x + scene.max.x) * 0.5f, (scene.min.y + scene.max.y) * 0.5f);
                frame(center); frame(center, 0, true);
                frame(ImVec2(center.x + 70, center.y + 20), 0, true);
                check(captureScene().pixels != scene.pixels, "left drag rotates the camera");
                frame(); scene = captureScene();
                frame(center); frame(center, 1, true); frame(ImVec2(center.x + 40, center.y + 20), 1, true);
                check(captureScene().pixels != scene.pixels, "right drag pans the camera");
                frame(); scene = captureScene();
                frame(center, -1, false, false, 2);
                check(captureScene().pixels != scene.pixels, "wheel zoom changes the scene");
                frame(); scene = captureScene(); frame(center); frame(center, 0, true);
                frame(ImVec2(scene.max.x + 30, center.y), 0, true);
                check(captureScene().pixels != scene.pixels, "drag stays captured outside the image");
                frame(); check(view.loadModel(argv[2]), "restore camera after mouse checks"); frame();
                ImGuiTable *controlTable=nullptr;
                auto &controlTables=ImGui::GetCurrentContext()->Tables;
                for(int i=0;i<controlTables.GetBufSize();++i) {
                        auto *t=controlTables.GetByIndex(i);
                        if(t && t->LastFrameActive==ImGui::GetFrameCount() && t->ColumnsCount==2 &&
                           std::strstr(t->OuterWindow->Name,"##mujoco_controls"))controlTable=t;
                }
                check(controlTable,"mode-specific parameter table is shown");
                const float rowHeight=ImGui::GetFrameHeight()+ImGui::GetStyle().CellPadding.y*2;
                const ImVec2 localTarget(controlTable->Columns[1].WorkMinX+12,
                                        controlTable->OuterRect.Min.y+2*rowHeight+rowHeight/2);
                frame(localTarget);frame(localTarget,0,true);frame(localTarget);
                auto &controlIo=ImGui::GetIO();
                controlIo.AddKeyEvent(ImGuiMod_Ctrl,true);controlIo.AddKeyEvent(ImGuiKey_A,true);frame(localTarget);
                controlIo.AddKeyEvent(ImGuiKey_A,false);controlIo.AddKeyEvent(ImGuiMod_Ctrl,false);frame(localTarget);
                controlIo.AddInputCharactersUTF8("0.25");frame(localTarget);
                controlIo.AddKeyEvent(ImGuiKey_Enter,true);frame(localTarget);controlIo.AddKeyEvent(ImGuiKey_Enter,false);frame();
                check(view.control().positionTarget==.25,"local target input stays at the entered value");
                check(view.start(region), "start built-in simulation");
                check(view.control().positionTarget==.25,"start preserves configured targets");
                ava_sim::Reader reader; check(reader.open(region), "telemetry connection");
                ava_sim::Publisher commands; check(commands.open(ava_sim::commandRegion(region)), "command connection");
                ava_sim::Sample sample;
                auto advance = [&] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20)); frame();
                        bool got = false;
                        while (reader.read(sample)) {
                                check(ava_sim::valid(sample), "valid telemetry"); got = true;
                                monitor.findChannel("Position", "q_ref")->setRVal(sample.q_ref, sample.time);
                                monitor.findChannel("Position", "q")->setRVal(sample.q, sample.time);
                                ++monitor.simFramesReceived_;
                        }
                        check(got, "physics emits timestamped frames");
                        monitor.findChannel("Position", "q_ref")->publishSnapshot();
                        monitor.findChannel("Position", "q")->publishSnapshot();
                        monitor.simConnected_.store(true);
                };
                advance(); const auto epoch = sample.epoch;
                ava_sim::Command command; command.epoch = epoch; command.field = 0; command.val = 0.5;
                check(commands.publish(command), "send position target"); advance();
                check(sample.q_ref == 0.5f && std::abs(sample.tau) > 0, "PD target and torque echo");
                command.field = 6; command.val = 50; check(commands.publish(command), "send kp"); advance();
                check(sample.kp == 50, "kp echo");
                view.setPaused(true); std::this_thread::sleep_for(std::chrono::milliseconds(20)); frame();
                check(!reader.read(sample), "pause stops simulation time and frames");
                view.setPaused(false); advance();
                view.reset(); advance(); check(sample.epoch != epoch && sample.q_ref == 0 && sample.kp == 50, "reset epoch/targets keeps gains");
                command.epoch = epoch; command.field = 0; command.val = 9; commands.publish(command); advance();
                check(sample.q_ref == 0, "old-epoch commands rejected");
                scene = captureScene();
                double redX = 0, redY = 0; unsigned redCount = 0;
                for (int y = 0; y < scene.height; ++y)
                        for (int x = 0; x < scene.width; ++x) {
                                const auto pixel = scene.pixels.data() + (y * scene.width + x) * 3;
                                if (pixel[0] > 60 && pixel[0] > pixel[1] * 1.6 && pixel[0] > pixel[2] * 1.6) {
                                        redX += x; redY += y; ++redCount;
                                }
                        }
                check(redCount > 0, "moving red payload is visible for picking");
                const ImVec2 pick(scene.min.x + static_cast<float>(redX / redCount / scene.width) * (scene.max.x - scene.min.x),
                                  scene.max.y - static_cast<float>(redY / redCount / scene.height) * (scene.max.y - scene.min.y));
                frame(pick, -1, false, true); frame(pick, 0, true, true);
                frame(ImVec2(pick.x + 60, pick.y + 25), 0, true, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                frame(ImVec2(pick.x + 60, pick.y + 25), 0, true, true);
                while (reader.read(sample)) {}
                check(std::abs(sample.q) > 1e-5f, "Ctrl+drag applies a physical force to the selected body");
                frame(); advance();
                MuJoCoView other; check(other.loadModel(argv[2]), "second renderer can load");
                check(!other.start(region) && !other.error().empty(), "second producer rejected without replacing active model");
                check(!view.loadModel("missing-mujoco-model.xml") && view.running(), "failed model load preserves running model");
                auto modeControl=view.control();
                view.reset();modeControl.mode=ava_mujoco::Mode::Velocity;
                modeControl.velocityKp=30;modeControl.velocityKi=100;modeControl.velocityTarget=1.5;modeControl.torqueFeedforward=0;
                check(view.setControl(modeControl),"set velocity PI mode");
                for(int i=0;i<50;++i)advance();
                check(std::abs(sample.dq-1.5)<.3 &&
                      std::abs(ava_sim::val(sample,5)-(sample.dq_ref-sample.dq))<1e-6,
                      "real MuJoCo velocity PI tracks speed and publishes velocity error");
                view.reset();modeControl.mode=ava_mujoco::Mode::Position;
                modeControl.positionTarget=.3;modeControl.positionKp=10;modeControl.velocityFeedforward=0;
                check(view.setControl(modeControl),"set cascaded position mode");
                for(int i=0;i<50;++i)advance();
                check(std::abs(sample.q-.3)<.1,"real cascaded position loop tracks its position target");
                modeControl.mode=ava_mujoco::Mode::PD;modeControl.pdKp=0;modeControl.pdKd=0;modeControl.torqueFeedforward=.25;
                check(view.setControl(modeControl),"set pure PD torque feedforward");advance();
                check(std::abs(sample.tau-.25)<1e-6,"torque feedforward reaches the MuJoCo actuator");
                modeControl={};view.setControl(modeControl);view.stop();
                const auto finalTime = sample.time; std::this_thread::sleep_for(std::chrono::milliseconds(10)); frame();
                check(!reader.read(sample) && !view.running() && finalTime > 0, "stop produces no new frames");
                check(view.loadModel(argv[2]), "reload a valid preview after the intentional error"); frame();
                std::vector<unsigned char> rgb(1000 * 800 * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, 1000, 800, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                std::ofstream capture(argv[3], std::ios::binary); capture << "P6\n1000 800\n255\n";
                for (int y = 799; y >= 0; --y) capture.write(reinterpret_cast<char *>(rgb.data() + y * 1000 * 3), 3000);
                capture.close();
                ava_actuator::Config actuator;
                const auto defaultActuatorPath = std::string(argv[3]) + ".actuator.xml";
                check(view.createActuator(defaultActuatorPath, actuator), "generate and load default PMSM actuator");
                auto generated = ava_actuator::generate(actuator);
                auto countNames = [&](const std::string &prefix) {
                        size_t count = 0, pos = 0;
                        while ((pos = generated.xml.find("name=\"" + prefix, pos)) != std::string::npos) { ++count; ++pos; }
                        return count;
                };
                check(countNames("magnet_") == 14 && countNames("stator_tooth_") == 12,
                      "PMSM generates exactly 2p magnets and the requested stator slot count");
                auto idealEncoder = actuator; idealEncoder.eccentricMm = idealEncoder.tiltDeg = idealEncoder.zeroDeg = 0;
                check(std::abs(ava_actuator::encoder(idealEncoder, 1) - 1) < 0.0001,
                      "ideal encoder preserves angle within its resolution");
                auto widerGap = actuator; widerGap.gapMm = 2;
                check(std::abs(ava_actuator::encoder(widerGap, 1) - 1) > std::abs(ava_actuator::encoder(actuator, 1) - 1),
                      "encoder air-gap configuration changes its approximate eccentricity response");
                check(generated.stages.size() == 2 && std::abs(generated.ratio - 9) < 1e-10,
                      "default reducer realizes 9:1 with two integer-tooth planetary stages");
                for (double requested : {2.5, 5.0, 8.0, 9.0, 25.0, 50.0, 64.0, 80.0, 100.0}) {
                        auto cfg = actuator; cfg.ratio = requested; auto variant = ava_actuator::generate(cfg);
                        check(std::abs(variant.ratio / requested - 1) <= .005, "requested gear ratio tolerance");
                        for (auto st : variant.stages)
                                check(st.ring == st.sun + 2 * st.planet && (st.ring + st.sun) % 3 == 0,
                                      "planetary tooth counts satisfy concentric and three-planet assembly conditions");
                        check(view.createActuator(std::string(argv[3]) + ".variant.xml", cfg), "MuJoCo compiles gear variants");
                }
                actuator.polePairs = 10; actuator.slots = 18; actuator.ratio = 50;
                actuator.zeroDeg = 2; actuator.motorTorque = .1;
                check(view.createActuator(std::string(argv[3]) + ".variant.xml", actuator), "load parameterized actuator");
                frame(); check(view.start(region), "start generated actuator"); advance();
                check(std::abs(sample.q - 2 * ava_actuator::pi / 180) < .02, "encoder zero error reaches telemetry");
                command.epoch = sample.epoch; command.field = 0; command.val = 1;
                commands.publish(command); advance();
                const double limit = ava_actuator::generate(actuator).ratio * actuator.motorTorque * actuator.efficiency;
                check(sample.q_ref == 1 && std::abs(sample.tau) <= limit + 1e-5 && sample.q > .01f,
                      "generated actuator uses output-axis PD, transmission and motor torque limit");
                auto invalid = actuator; invalid.slots = 0;
                check(!view.createActuator(std::string(argv[3]) + ".invalid.xml", invalid) && view.running(),
                      "invalid generator parameters preserve the running model");
                view.stop(); check(view.loadModel(defaultActuatorPath), "XML restores actuator parameters"); frame();
                std::vector<unsigned char> actuatorPixels(1000 * 800 * 3);
                glReadPixels(0, 0, 1000, 800, GL_RGB, GL_UNSIGNED_BYTE, actuatorPixels.data());
                std::ofstream actuatorCapture(std::string(argv[3]) + ".actuator.ppm", std::ios::binary);
                actuatorCapture << "P6\n1000 800\n255\n";
                for (int y = 799; y >= 0; --y)
                        actuatorCapture.write(reinterpret_cast<char *>(actuatorPixels.data() + y * 3000), 3000);
                actuatorCapture.close();
                ImVec2 builderButton;
                for (auto *w : ImGui::GetCurrentContext()->Windows)
                        if (w->Active && std::strstr(w->Name, "##mujoco_scene"))
                                builderButton = ImVec2(w->WorkRect.Min.x + 40, w->WorkRect.Min.y + ImGui::GetFrameHeight() / 2);
                frame(builderButton); frame(builderButton, 0, true); frame(builderButton);
                frame(builderButton); frame(builderButton);
                auto *builder = ImGui::FindWindowByName("##actuator_builder");
                check(builder && builder->Active && !builder->Hidden, "new actuator button opens the parameter builder");
                glReadPixels(0, 0, 1000, 800, GL_RGB, GL_UNSIGNED_BYTE, actuatorPixels.data());
                std::ofstream builderCapture(std::string(argv[3]) + ".builder.ppm", std::ios::binary);
                builderCapture << "P6\n1000 800\n255\n";
                for (int y = 799; y >= 0; --y)
                        builderCapture.write(reinterpret_cast<char *>(actuatorPixels.data() + y * 3000), 3000);
                builderCapture.close();
                std::puts("PASS: generated PMSM magnets/slots, integer planetary ratios, encoder errors, torque limits and XML reload");
                monitor.mujocoView_.reset();
                std::puts("PASS: high-DPI MuJoCo rendering, camera mouse gestures, captured drags, Ctrl physical perturbation, PD commands, telemetry, reset and producer ownership");
        } catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); result = 1; }
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImPlot::DestroyContext(); ImGui::DestroyContext();
        glfwDestroyWindow(window); glfwTerminate();
        if (result == 0) {
                std::thread liveSampler;
                std::unique_ptr<Gui> liveGui;
                try {
                        std::ofstream(argv[1]) << R"({"monitors":[{"name":"MuJoCo legacy","mujocoHeight":280,"scopes":[]}],"variables":[]})";
                        check(glfwInit(), "reinitialize GLFW"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                        auto &gui = liveGui;
                        gui = std::make_unique<Gui>(argv[1]);
                        auto monitor = gui->getMonitors().at("MuJoCo legacy");
                        check(monitor->mujocoEnabled_, "legacy MuJoCo dock migrates to merged view");
                        monitor->mujocoModelPath_ = argv[2]; monitor->mujocoHeight_ = 360;
                        monitor->mujocoControl_.mode=ava_mujoco::Mode::Velocity;
                        monitor->mujocoControl_.velocityKi=17;monitor->mujocoControl_.velocityTarget=2;
                        monitor->mujocoControl_.torqueFeedforward=.4;
                        SimSamplerTestAccess::save(*gui, argv[1]);
                        monitor.reset(); gui.reset();
                        check(glfwInit(), "restore GLFW"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                        gui = std::make_unique<Gui>(argv[1]); monitor = gui->getMonitors().at("MuJoCo legacy");
                        check(monitor->mujocoEnabled_ && monitor->mujocoModelPath_ == argv[2] && monitor->mujocoHeight_ == 360,
                              "model and split height survive session roundtrip");
                        check(!monitor->mujocoView_, "session loading never auto-starts simulation");
                        check(monitor->mujocoControl_.mode==ava_mujoco::Mode::Velocity &&
                              monitor->mujocoControl_.velocityKi==17 && monitor->mujocoControl_.velocityTarget==2 &&
                              monitor->mujocoControl_.torqueFeedforward==.4,"control mode, PI gains, targets and feedforward survive session restore");
                        monitor->mujocoControl_={};
                        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
                        ImGui::GetIO().IniFilename = nullptr;
                        ImGui::GetIO().ConfigInputTrickleEventQueue = false;
                        const std::string liveRegion = std::string(ava_sim::defaultRegion) + ".integrated." +
                                                       std::to_string(GetCurrentProcessId());
                        const char *scopeNames[] = {"Targets", "PD Parameters", "Position", "Velocity", "Torque", "Error"};
                        for (const auto *name : scopeNames) monitor->addScope(name);
                        monitor->getScopes().at("Targets")->setDraw(MonitorScope::DrawEnum::TABLE);
                        monitor->getScopes().at("PD Parameters")->setDraw(MonitorScope::DrawEnum::TABLE);
                        const unsigned locations[] = {2, 2, 3, 3, 4, 5, 1, 1};
                        auto add = [&](unsigned scope, unsigned field) {
                                auto s = monitor->getScopes().at(scopeNames[scope]); s->addChannel(ava_sim::fields[field]);
                                auto *ch = s->findChannel(ava_sim::fields[field]); ch->setDevice(ava_sim::device);
                                ch->setType("f32"); ch->setShmRegionName(liveRegion); ch->setSimField(field);
                                ch->setWritable(ava_sim::writableField(field));
                        };
                        for (unsigned i = 0; i < 8; ++i) add(locations[i], i);
                        add(0, 0); add(0, 2);
                        auto liveFrame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                                ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
                                ImGui::GetIO().AddMouseButtonEvent(0, down);
                                ImGui::NewFrame();
                                {
                                        std::lock_guard lock(gui->getMonitorMtx());
                                        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1000, 750));
                                        monitor->updateDisplay();
                                }
                                ImGui::Render();
                                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        };
                        liveFrame();
                        check(monitor->mujocoView_ && monitor->mujocoView_->error().empty(), "live dock loads model");
                        check(monitor->getScopes().at("Position")->getHeight() >= 180,
                              "six-scope dock keeps a usable waveform area beside separate controls");
                        g_appRunning.store(true); g_samplerRunMode.store(0);
                        liveSampler = std::thread(threadFunc, gui.get());
                        check(monitor->mujocoView_->start(liveRegion), "start integrated simulation");
                        auto waitLive = [&](auto predicate, const char *message) {
                                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                                while (std::chrono::steady_clock::now() < deadline) {
                                        liveFrame();
                                        { std::lock_guard lock(gui->getMonitorMtx()); if (predicate()) return; }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                }
                                throw std::runtime_error(message);
                        };
                        waitLive([&] { return monitor->simFramesReceived_.load() > 20 &&
                                                   monitor->findChannel("Position", "q")->storedCount() > 20; },
                                 "built-in physics reaches actual sampler and waveform storage");
                        monitor->mujocoView_->setPaused(true);
                        {
                                std::lock_guard lock(gui->getMonitorMtx());
                                monitor->findChannel("Position", "q_ref")->waveEnable_ = true;
                        }
                        ImGuiWindow *controlWindow=nullptr;
                        for(auto *w:ImGui::GetCurrentContext()->Windows)
                                if(w->Active && std::strstr(w->Name,"##mujoco_controls"))controlWindow=w;
                        check(controlWindow,"right control window exists");
                        controlWindow->StateStorage.SetInt(controlWindow->GetID("SHM command tables"),1);
                        liveFrame();
                        ImGui::SetScrollY(controlWindow,200);liveFrame();
                        ImVec2 targetInput;
                        float firstTableY = FLT_MAX;
                        auto &tables = ImGui::GetCurrentContext()->Tables;
                        for (int i = 0; i < tables.GetBufSize(); ++i) {
                                auto *table = tables.GetByIndex(i);
                                if (table && table->LastFrameActive == ImGui::GetFrameCount() && table->ColumnsCount == 6 &&
                                    std::strstr(table->OuterWindow->Name, "##mujoco_controls") && table->OuterRect.Min.y < firstTableY) {
                                        firstTableY = table->OuterRect.Min.y;
                                        targetInput = ImVec2(table->Columns[1].WorkMinX + 8,
                                                            firstTableY + ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() / 2);
                                }
                        }
                        check(firstTableY != FLT_MAX && targetInput.x > 500, "target input is in the right control panel");
                        liveFrame(targetInput); liveFrame(targetInput, true); liveFrame(targetInput);
                        auto &io = ImGui::GetIO();
                        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); liveFrame(targetInput);
                        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); liveFrame(targetInput);
                        io.AddInputCharactersUTF8("0.5"); liveFrame(targetInput);
                        io.AddKeyEvent(ImGuiKey_Enter, true); liveFrame(targetInput);
                        io.AddKeyEvent(ImGuiKey_Enter, false); liveFrame(targetInput);
                        for (int i = 0; i < 8; ++i) { liveFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                        {
                                std::lock_guard lock(gui->getMonitorMtx());
                                auto *target = monitor->findChannel("Targets", "q_ref");
                                check(target->simWritePending() && target->simInputValue() == .5f && target->getDispRawVal() == 0,
                                      "committed target remains visible while paused physics cannot echo the new value");
                                check(!monitor->findChannel("Position", "q_ref")->waveEnable_,
                                      "manual target disables competing waveform on another scope of the same field");
                        }
                        monitor->mujocoView_->setPaused(false);
                        waitLive([&] { return monitor->findChannel("Position", "q_ref")->getDispRawVal() == 0.5f &&
                                                   std::abs(monitor->findChannel("Position", "q")->getDispRawVal()) > 0.01f; },
                                 "table target reaches embedded model and real plotted feedback");
                        check(!monitor->findChannel("Targets", "q_ref")->simWritePending(), "telemetry acknowledges the committed target");
                        liveFrame(targetInput); liveFrame(targetInput, true); liveFrame(targetInput);
                        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); liveFrame(targetInput);
                        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); liveFrame(targetInput);
                        io.AddInputCharactersUTF8("0.75"); liveFrame(targetInput);
                        liveFrame(ImVec2(500, 70), true); liveFrame(ImVec2(500, 70));
                        waitLive([&] { return monitor->findChannel("Position", "q_ref")->getDispRawVal() == .75f; },
                                 "leaving the native target input commits its value without reverting to telemetry");
                        liveFrame(); glFinish();
                        int width = 0, height = 0;
                        glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
                        std::vector<unsigned char> pixels(width * height * 3);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                        std::ofstream integrated(std::string(argv[3]) + ".integrated.ppm", std::ios::binary);
                        integrated << "P6\n" << width << ' ' << height << "\n255\n";
                        for (int y = height - 1; y >= 0; --y)
                                integrated.write(reinterpret_cast<char *>(pixels.data() + y * width * 3), width * 3);
                        integrated.close();
                        {
                                std::lock_guard lock(gui->getMonitorMtx());
                                monitor->samplingMode_ = Monitor::SamplingMode::HSS;
                                MuJoCoViewTestAccess::mode(*monitor, MonitorViewMode::FOLLOW);
                                monitor->linkXMin_ = 100; monitor->linkXMax_ = 101;
                        }
                        liveFrame();
                        check(monitor->linkXMin_ < 1 && monitor->linkXMax_ < 2,
                              "simulation follows its clock in HSS mode without a J-Link stream");
                        {
                                std::lock_guard lock(gui->getMonitorMtx());
                                MuJoCoViewTestAccess::mode(*monitor, MonitorViewMode::MANUAL);
                                monitor->linkXMin_ = 100; monitor->linkXMax_ = 101;
                        }
                        const auto previousEpoch = monitor->simEpoch_.load();
                        monitor->mujocoView_->reset();
                        waitLive([&] { return monitor->findChannel("Position", "q_ref")->getDispRawVal() == 0 &&
                                                   monitor->findChannel("Position", "q")->storedCount() > 5 &&
                                                   monitor->simEpoch_.load() != previousEpoch &&
                                                   monitor->displayedSimEpoch_ == monitor->simEpoch_.load(); },
                                 "reset restarts waveform timestamps without losing the plot");
                        check(monitor->linkXMin_ < 1 && monitor->linkXMax_ < 2,
                              "reset fits new simulation data after a manually shifted viewport");
                        monitor->mujocoView_->stop();
                        g_appRunning.store(false); liveSampler.join();
                        monitor.reset(); gui.reset();
                        std::puts("PASS: merged MuJoCo dock session migration and model/height persistence without auto-start");
                        std::puts("PASS: six-scope layout and embedded simulation -> actual sampler -> waveform -> table command loop");
                } catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); result = 1; }
                g_appRunning.store(false);
                if (liveSampler.joinable()) liveSampler.join();
                liveGui.reset();
        }
        return result;
}
