#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/inverter_simulator.hpp"
#include "imgui_internal.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

static void check(bool v, const char *message) { if (!v) throw std::runtime_error(message); }
struct InverterSimulatorTestAccess {
        static void setup(InverterSimulator &tool, const ava_spice::Config &config) { tool.draft_ = config; tool.start(); }
        static bool done(const InverterSimulator &tool) { return !tool.runner_.busy(); }
        static const auto &result(const InverterSimulator &tool) { return tool.result_; }
        static const auto &previous(const InverterSimulator &tool) { return tool.previous_; }
        static const auto &applied(const InverterSimulator &tool) { return tool.applied_; }
        static MonitorScope &scope(InverterSimulator &tool, int kind) { return *tool.scopes_[kind]; }
        static void compare(InverterSimulator &tool, bool enabled) { tool.compare_ = enabled; tool.syncScopes(); }
        static void complete(InverterSimulator &tool, const ava_spice::Config &config, ava_spice::Result result) {
                tool.pending_ = config; tool.acceptResult(std::move(result));
        }
};
int main(int argc, char **argv) {
        if (argc != 2) return 2; module_init();
        try {
                ava_spice::Config c; c.device = 0; c.executable = ava_spice::defaultExecutable();
                c.package = false;
                auto result = ava_spice::run(c);
                if (!result.error.empty()) fprintf(stderr, "%s\n%s\n", result.error.c_str(), result.log.c_str());
                check(result.error.empty() && result.samples.size() > 1000, "real ngspice produces complete double pulse data");
                double lowVds = c.bus, highVgs = 0;
                for (const auto &s : result.samples) {
                        if (s.time > .05e-6 && s.time < .4e-6)
                                check(std::abs(s.vds - c.bus) < 2 && std::abs(s.id) < .1,
                                      "preloaded operating point does not create spurious startup switching");
                        if (s.time > 1e-6 && s.time < 2e-6) lowVds = std::min(lowVds, s.vds);
                        highVgs = std::max(highVgs, s.vgs);
                }
                check(lowVds < 2 && highVgs > c.gateOn * .8 && result.peakV > c.bus, "SPICE shows gate charging, channel conduction and voltage overshoot");
                auto fineConfig = c; fineConfig.stepNs /= 2; auto fine = ava_spice::run(fineConfig);
                check(fine.error.empty() && std::abs(result.eon - fine.eon) / std::max(1e-9, std::abs(fine.eon)) < .1,
                      "SPICE terminal energy converges with smaller maximum step");
                const auto library = std::string(argv[1]) + ".lib";
                std::ofstream(library) << ".model fixture VDMOS(Vto=4 Kp=40 Rd=.005 Rs=.005 Cgs=2n Cgdmax=.7n Cgdmin=30p Cjo=1n Is=1e-12 Bv=120)\n"
                        << ".subckt Single D G S\nM1 D G S fixture\n.ends Single\n"
                        << ".subckt Package BUS HG SW G S\nMhigh BUS HG SW fixture\nMlow SW G S fixture\n.ends Package\n";
                auto list = ava_spice::models(library); check(list.size() == 2 && list[1].pins == "BUS HG SW G S", "model library lists subcircuits and actual pins");
                auto imported = c; imported.device = 1; imported.library = library; imported.model = "Single"; imported.pins = "D G S";
                auto single = ava_spice::run(imported); check(single.error.empty(), "SPICE imports an external MOS model");
                imported.package = true; imported.model = "Package"; imported.pins = "BUS HG SW G S";
                auto package = ava_spice::run(imported);
                if (!package.error.empty()) fprintf(stderr, "%s\n%s\n", package.error.c_str(), package.log.c_str());
                check(package.error.empty(), "SPICE drives an integrated half-bridge package");
                ava_spice::Config vendor; vendor.library = ava_spice::defaultLibrary(); vendor.executable = c.executable;
                auto vendorResult = ava_spice::run(vendor);
                if (!vendorResult.error.empty()) fprintf(stderr, "%s\n%s\n", vendorResult.error.c_str(), vendorResult.log.c_str());
                check(vendorResult.error.empty() && vendorResult.samples.size() > 1000, "actual Infineon ISG0616N10NM5HSC_L1 model completes SPICE double pulse");
                auto invalid = c; invalid.device = 2; invalid.library = "missing_vendor.lib";
                check(!ava_spice::run(invalid).error.empty(), "missing GaN model fails instead of substituting a MOS model");
                check(glfwInit(), "GLFW"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
                auto gui = std::make_unique<Gui>(argv[1]); ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
                ImGui::GetIO().IniFilename = nullptr; glfwSetWindowSize(glfwGetCurrentContext(), 1200, 1600);
                InverterSimulator tool; tool.show_ = true; InverterSimulatorTestAccess::setup(tool, vendor);
                auto frame = [&] {
                        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1200, 1600)); tool.draw();
                        ImGui::Render(); glViewport(0, 0, 1200, 1600); glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
                };
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!InverterSimulatorTestAccess::done(tool) && std::chrono::steady_clock::now() < deadline) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                frame(); check(InverterSimulatorTestAccess::done(tool) && InverterSimulatorTestAccess::result(tool).error.empty(), "dock runs async ngspice and renders real result");
                auto *vdsChannel = InverterSimulatorTestAccess::scope(tool, 1).findChannel("current_0");
                check(vdsChannel && vdsChannel->read_.rawSize() == vendorResult.samples.size() &&
                      std::abs(vdsChannel->latestTs() - vendorResult.samples.back().time * 1e6) < 1e-5,
                      "shared monitor scope retains all adaptive samples with microsecond timestamps");
                check(InverterSimulatorTestAccess::previous(tool).samples.empty(), "first successful run has no comparison baseline");
                InverterSimulatorTestAccess::complete(tool, fineConfig, fine);
                frame();
                check(InverterSimulatorTestAccess::previous(tool).peakV == vendorResult.peakV &&
                      InverterSimulatorTestAccess::result(tool).peakV == fine.peakV &&
                      InverterSimulatorTestAccess::applied(tool).stepNs == fineConfig.stepNs,
                      "comparison retains preceding successful waveform and binds current parameters to current result");
                InverterSimulatorTestAccess::setup(tool, invalid);
                check(InverterSimulatorTestAccess::result(tool).peakV == fine.peakV &&
                      InverterSimulatorTestAccess::applied(tool).device == 0,
                      "starting another run preserves the displayed result and its parameter snapshot");
                const auto failDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!InverterSimulatorTestAccess::done(tool) && std::chrono::steady_clock::now() < failDeadline) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                frame();
                check(InverterSimulatorTestAccess::done(tool) &&
                      InverterSimulatorTestAccess::result(tool).peakV == fine.peakV &&
                      InverterSimulatorTestAccess::previous(tool).peakV == vendorResult.peakV,
                      "failed run does not overwrite either successful waveform");
                ava_spice::Result cancelled; cancelled.error = "cancelled";
                InverterSimulatorTestAccess::complete(tool, invalid, std::move(cancelled));
                check(InverterSimulatorTestAccess::result(tool).peakV == fine.peakV &&
                      InverterSimulatorTestAccess::previous(tool).peakV == vendorResult.peakV,
                      "cancelled run preserves comparison baseline");
                InverterSimulatorTestAccess::complete(tool, c, result);
                frame();
                check(InverterSimulatorTestAccess::previous(tool).peakV == fine.peakV &&
                      InverterSimulatorTestAccess::result(tool).peakV == result.peakV,
                      "third successful run advances comparison to the immediately preceding success");
                auto &sharedScope = InverterSimulatorTestAccess::scope(tool, 1);
                check(sharedScope.findChannel("previous_0") && sharedScope.findChannel("current_0") &&
                      sharedScope.findChannel("previous_0")->read_.rawSize() == fine.samples.size(),
                      "previous and current runs use channels in the same monitor scope");
                InverterSimulatorTestAccess::compare(tool, false); frame();
                check(!sharedScope.findChannel("previous_0") && sharedScope.findChannel("current_0"),
                      "disabling comparison removes the previous run from plotting, stats and axis fitting");
                InverterSimulatorTestAccess::compare(tool, true); frame();
                check(sharedScope.findChannel("previous_0")->read_.rawSize() == fine.samples.size(),
                      "reenabling comparison restores the complete previous run");
                std::vector<unsigned char> pixels(1200*1600*3); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, 1200, 1600, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                std::ofstream image(std::string(argv[1]) + ".ppm", std::ios::binary); image << "P6\n1200 1600\n255\n";
                for (int y = 1599; y >= 0; --y) image.write(reinterpret_cast<char *>(pixels.data() + y*3600), 3600); image.close();
                auto *json = cJSON_CreateObject(); tool.saveSession(json); InverterSimulator restored; restored.loadSession(json);
                check(restored.show_ && InverterSimulatorTestAccess::done(restored), "session restoration does not start SPICE"); cJSON_Delete(json);
                gui.reset(); puts("PASS: real ngspice double pulse, convergence, model import, package mapping, missing-model errors, async dock, previous-run comparison and persistence");
                return 0;
        } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
