#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/inverter_simulator.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static const ava_spice::Result::Trace &trace(const ava_spice::Result &r, const std::string &key) {
        auto it = std::find_if(r.traces.begin(), r.traces.end(), [&](const auto &t) { return t.key == key; });
        check(it != r.traces.end(), "requested probe is exported"); return *it;
}
struct InverterSimulatorTestAccess {
        static void complete(InverterSimulator &t, const ava_spice::Config &c, ava_spice::Result r) {
                t.draft_ = c; t.pending_ = c; t.acceptResult(std::move(r));
        }
        static const auto &probes(const InverterSimulator &t) { return t.probes_; }
        static void preset(InverterSimulator &t,int val) { t.applyProbePreset(val); }
        static void toggle(InverterSimulator &t, const std::string &key) { t.toggleProbe(key); }
        static auto &scope(InverterSimulator &t, int kind) { return *t.scopes_[kind]; }
};
int main(int argc, char **argv) {
        if (argc != 2) return 2; module_init(); setvbuf(stdout, nullptr, _IONBF, 0);
        try {
                ava_spice::Config c; c.topology = 1; c.device = 0; c.package = false;
                c.pwmHz=40000; c.pwmCycles=2; c.stepNs=50;
                auto result = ava_spice::run(c);
                if (!result.error.empty()) fprintf(stderr, "%s\n%s\n", result.error.c_str(), result.log.c_str());
                check(result.error.empty() && result.times.size() > 1000 && result.traces.size() > 50,
                      "real SPICE solves six MOS switches, PWM and motor windings");
                for (size_t i = 0; i < result.times.size(); ++i) {
                        double sum = trace(result,"i_a").values[i] + trace(result,"i_b").values[i] + trace(result,"i_c").values[i];
                        check(std::abs(sum - trace(result,"neutral").values[i] / 1e9) < 1e-5, "star neutral satisfies KCL");
                        check(std::abs(trace(result,"vab").values[i] - trace(result,"a").values[i] + trace(result,"b").values[i]) < 1e-9,
                              "line-to-line probe uses differential voltage");
                        for (const auto *p : {"a","b","c"})
                                check(!(trace(result,std::string("drive_h")+p).values[i] > 1 &&
                                        trace(result,std::string("drive_l")+p).values[i] > 1),
                                      "complementary PWM drivers never overlap during dead time");
                }
                check(result.peakCurrent > .01 && result.peakV > 40, "winding currents and switch voltages are nonzero");
                auto stationary=c; stationary.rpm=0;stationary.pwmHz=200000;stationary.pwmCycles=1;stationary.deadNs=5;
                auto atRest=ava_spice::run(stationary);
                check(atRest.error.empty(),"stationary R-L windings solve");
                for(const auto *key:{"emf_a","emf_b","emf_c"})
                        for(double val:trace(atRest,key).values)check(std::abs(val)<1e-12,"zero speed produces zero back EMF");
                check(std::abs(result.times.back()-ava_spice::threePhaseEnd(c))<1e-12,"frequency and integer cycle count determine complete transient duration");
                const auto boundary=std::lower_bound(result.times.begin(),result.times.end(),1e-6+1/c.pwmHz);
                const auto atBoundary=static_cast<size_t>(boundary-result.times.begin());
                for(const auto *p:{"a","b","c"})
                        check(trace(result,std::string("drive_l")+p).values[atBoundary]>c.gateOn*.9,
                              "low-side PWM stays continuous across the carrier boundary without extra pulses");
                auto fractional=c;fractional.pwmCycles=1.5;
                check(!ava_spice::run(fractional).error.empty(),"fractional cycle count is rejected");
                auto invalid = c; invalid.deadNs = 10000;
                check(!ava_spice::run(invalid).error.empty(), "invalid dead time is rejected");
                auto vendor = c; vendor.device = 1; vendor.package = true; vendor.library = ava_spice::defaultLibrary();
                vendor.pwmHz=20000; vendor.pwmCycles=2;
                auto actual = ava_spice::run(vendor);
                if (!actual.error.empty()) fprintf(stderr, "%s\n%s\n", actual.error.c_str(), actual.log.c_str());
                check(actual.error.empty() && actual.times.size() > 100, "three actual Infineon dual-MOS half bridges run in ngspice");
                check(std::abs(actual.times.back()-ava_spice::threePhaseEnd(vendor))<1e-12,
                      "vendor transient completes both 20 kHz PWM cycles without timestep collapse");
                puts("PASS: six-switch SPICE, star KCL, differential probes, dead time, actual Infineon packages");
                check(glfwInit(), "GLFW"); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
                auto gui = std::make_unique<Gui>(argv[1]); auto &io = ImGui::GetIO();
                io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; io.IniFilename = nullptr;
                io.ConfigInputTrickleEventQueue = false; glfwSetWindowSize(glfwGetCurrentContext(),1400,1600);
                InverterSimulator tool; tool.show_ = true;
                InverterSimulatorTestAccess::complete(tool,c,result);
                auto frame = [&](ImVec2 mouse=ImVec2(-100,-100),bool down=false) {
                        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); io.AddMousePosEvent(mouse.x,mouse.y);
                        io.AddMouseButtonEvent(0,down); ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1400,1600)); tool.draw();
                        ImGui::Render(); glViewport(0,0,1400,1600); glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
                };
                frame(); frame();
                check(InverterSimulatorTestAccess::scope(tool,1).findChannel("current_i_a")->storedCount() == result.times.size(),
                      "probe channels retain complete adaptive waveform data");
                for(int preset=1;preset<=6;++preset) {
                        InverterSimulatorTestAccess::preset(tool,preset);frame();
                        for(const auto &key:InverterSimulatorTestAccess::probes(tool)) {
                                const auto &signal=trace(result,key);
                                int kind=signal.unit=="A"?1:0;
                                check(InverterSimulatorTestAccess::scope(tool,kind).findChannel("current_"+key),"preset maps every selected probe to the proper shared scope");
                        }
                }
                InverterSimulatorTestAccess::preset(tool,1);frame();
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_vds_la") &&
                      InverterSimulatorTestAccess::scope(tool,0).findChannel("current_vgs_la") &&
                      InverterSimulatorTestAccess::scope(tool,1).findChannel("current_id_la") &&
                      !InverterSimulatorTestAccess::scope(tool,0).findChannel("current_a"),"QAL preset replaces selection with actual low-side VDS VGS ID");
                InverterSimulatorTestAccess::preset(tool,4);frame();
                InverterSimulatorTestAccess::toggle(tool,"vgs_ha"); frame();
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_vgs_ha"), "VGS selection feeds shared monitor scope");
                ImGuiWindow *right=nullptr;
                for (auto *w: ImGui::GetCurrentContext()->Windows)
                        if(w->Active && std::strstr(w->Name,"##inverter_parameters")) right=w;
                check(right && right->Pos.x > 600, "circuit and parameters occupy right panel");
                // Locate the long DC rail in the rendered schematic instead of
                // hard-coding window or text-wrap-dependent screen coordinates
                ImVec2 railMin, railMax; bool foundRail=false;
                const auto &vertices = right->DrawList->VtxBuffer;
                for (int i=0;i<vertices.Size && !foundRail;++i) {
                        if(vertices[i].col!=IM_COL32(155,180,210,255))continue;
                        for(int j=i+1;j<std::min(i+9,vertices.Size);++j) {
                                if(vertices[j].col!=vertices[i].col)continue;
                                auto a=vertices[i].pos,b=vertices[j].pos;
                                if(std::abs(b.x-a.x)>250 && std::abs(b.y-a.y)<10) {
                                        railMin=ImVec2(std::min(a.x,b.x),std::min(a.y,b.y));
                                        railMax=ImVec2(std::max(a.x,b.x),std::max(a.y,b.y));
                                        foundRail=true;break;
                                }
                        }
                }
                check(foundRail,"rendered DC rail is present");
                ImVec2 railPoint((railMin.x+railMax.x)/2,(railMin.y+railMax.y)/2);
                frame(railPoint);frame(railPoint,true);frame(railPoint);
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_bus"),
                      "native mouse click on a wire adds its voltage probe");
                frame(railPoint,true);frame(railPoint);
                check(!InverterSimulatorTestAccess::scope(tool,0).findChannel("current_bus"),
                      "clicking the same wire removes the probe");
                float bridgeWidth=railMax.x-railMin.x+24;
                float canvasWidth=bridgeWidth/.48f;
                float canvasX=railMin.x-12,canvasY=railPoint.y-35;
                ImVec2 mosPoint(canvasX+12+(bridgeWidth-24)/6,canvasY+110);
                frame(mosPoint);frame(mosPoint,true);frame(mosPoint);frame();
                auto &popups=ImGui::GetCurrentContext()->OpenPopupStack;
                check(popups.Size && popups.back().Window,"clicking MOS opens VDS/VGS/ID probe choices");
                auto *popup=popups.back().Window;
                ImVec2 vdsPoint(popup->WorkRect.Min.x+35,popup->WorkRect.Min.y+ImGui::GetTextLineHeight()/2);
                frame(vdsPoint);frame(vdsPoint,true);frame(vdsPoint);
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_vds_ha"),
                      "native popup selection adds the MOS differential VDS probe");
                frame(ImVec2(700,190),true);frame();
                const float radius=std::min(130.f,canvasWidth*.19f);
                const float mx=canvasX+canvasWidth*.78f,my=canvasY+210;
                ImVec2 phaseWire(canvasX+bridgeWidth+12,canvasY+130);
                frame(phaseWire);frame(phaseWire,true);frame(phaseWire);
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_a"),"clicking a orthogonal phase wire selects its node voltage");
                frame(phaseWire,true);frame(phaseWire);
                check(!InverterSimulatorTestAccess::scope(tool,0).findChannel("current_a"),"orthogonal wire probe can be removed by a second click");
                ImVec2 windingPoint(mx-radius*.3f,my-radius*.519615f);
                frame(windingPoint);frame(windingPoint,true);frame(windingPoint);frame();
                check(popups.Size && popups.back().Window,"clicking the circular A winding opens winding voltage/current choices");
                popup=popups.back().Window;
                ImVec2 windingChoice(popup->WorkRect.Min.x+35,popup->WorkRect.Min.y+ImGui::GetTextLineHeight()/2);
                frame(windingChoice);frame(windingChoice,true);frame(windingChoice);
                check(InverterSimulatorTestAccess::scope(tool,0).findChannel("current_winding_a"),"round winding click selects its actual phase-to-neutral voltage");
                frame(ImVec2(700,190),true);frame();
                InverterSimulatorTestAccess::preset(tool,5);frame();
                auto *json=cJSON_CreateObject(); tool.saveSession(json); InverterSimulator restored; restored.loadSession(json);
                auto *json2=cJSON_CreateObject(); restored.saveSession(json2);
                auto *saved=cJSON_GetObjectItemCaseSensitive(json2,"inverterSimulation");
                check(cJSON_GetObjectItemCaseSensitive(saved,"topology")->valueint==1 &&
                      cJSON_GetObjectItemCaseSensitive(saved,"probes")->child, "three-phase settings and probes restore without a run");
                check(cJSON_GetObjectItemCaseSensitive(saved,"pwmCycles")->valuedouble==2,"PWM cycle count persists");
                cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"inverterSimulation"),"pwmCycles");
                cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(json,"inverterSimulation"),"durationUs",75);
                restored.loadSession(json);auto *legacy=cJSON_CreateObject();restored.saveSession(legacy);
                check(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(legacy,"inverterSimulation"),"pwmCycles")->valuedouble==3,
                      "old duration session migrates to sufficient full PWM cycles");cJSON_Delete(legacy);
                cJSON_Delete(json); cJSON_Delete(json2);
                InverterSimulatorTestAccess::complete(tool,vendor,actual); frame();
                check(InverterSimulatorTestAccess::scope(tool,1).findChannel("previous_i_a"), "previous-run phase probes overlay");
                std::vector<unsigned char> pixels(1400*1600*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
                glReadPixels(0,0,1400,1600,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
                std::ofstream image(std::string(argv[1])+".ppm",std::ios::binary); image<<"P6\n1400 1600\n255\n";
                for(int y=1599;y>=0;--y)image.write(reinterpret_cast<char*>(pixels.data()+y*4200),4200);
                gui.reset(); puts("PASS: selectable probe channels, shared scope, comparison, settings persistence and GUI rendering");
                return 0;
        } catch(const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
