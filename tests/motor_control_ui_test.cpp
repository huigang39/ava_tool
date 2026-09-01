#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/motor_control.hpp"
#include "gui/motor_gauge_average.hpp"
#include "core/motor_faults.hpp"
#include "gui/fault_decoder.hpp"
#include "core/fault_decoder.hpp"
#include "core/fault_groups.hpp"
#include "gui/i18n.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "implot.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

struct FaultDecoderTestAccess {
    static const auto &group(const FaultDecoder &decoder, unsigned index) { return decoder.groups_.at(index); }
    static size_t groups(const FaultDecoder &decoder) { return decoder.groups_.size(); }
    static void requestGroup(FaultDecoder &decoder, unsigned index, bool requested) { for(auto &row:decoder.groups_.at(index).rows) row.requested=requested; }
    static uint32_t val(const FaultDecoder &decoder) {
        const auto found=decoder.bindings_.find(decoder.decoderId_);
        if(found!=decoder.bindings_.end() && found->second.hasValue) return uint32_t(found->second.val.bits);
        uint32_t val=0;
        if(!motor_faults::parseWord(decoder.manualInput_,val)) throw std::runtime_error("decoder has no numeric value");
        return val;
    }
    static void page(FaultDecoder &decoder, const char *id) { decoder.decoderId_=id; }
    static bool valid(const FaultDecoder &decoder) {
        const auto found=decoder.bindings_.find(decoder.decoderId_);
        return found!=decoder.bindings_.end() && found->second.valid;
    }
    static bool hasValue(const FaultDecoder &decoder) {
        const auto found=decoder.bindings_.find(decoder.decoderId_);
        return found!=decoder.bindings_.end() && found->second.hasValue;
    }
    static const auto &profile(const FaultDecoder &decoder) { return decoder.profile_; }
    static const std::string &decoderId(const FaultDecoder &decoder) { return decoder.decoderId_; }
    static bool bindingExists(const FaultDecoder &decoder,const char *id) { return decoder.bindings_.contains(id); }
    static void simulateLegacyBinding(FaultDecoder &decoder,const char *canonical,const char *alias) {
        auto node=decoder.bindings_.extract(canonical);
        if(node.empty()) throw std::runtime_error("missing canonical binding");
        node.key()=alias; decoder.bindings_.insert(std::move(node)); decoder.decoderId_=alias;
    }
    static std::string report(const FaultDecoder &decoder,unsigned index) { return decoder.groupReport(decoder.groups_.at(index)); }
};

struct VariableRewriteRegression {
    static void diagnosticDwarf(Variable &variable, const dwarf::Info &info) { variable.dwarfInfo_ = info; }
};

struct MotorControlTestAccess {
    static void pulse(MotorControl &p) { p.controller_.pulse(0); }
    static auto state(MotorControl &p) { return p.controller_.state(); }
    static void connect(MotorControl &p) {
        // Test-only injection: never connect the application's hardware-only backend
        MotorController::Command c; c.action=MotorController::Action::Connect;
        c.config.backend=AVA_ETH_USER_SIM; p.controller_.post(c);
    }
    static void connectDenied(MotorControl &p, const char *fixture) {
        MotorController::Command c; c.action=MotorController::Action::Connect;
        c.config.backend=AVA_ETH_NPCAP; c.config.library=fixture; c.config.device="npcap-access-denied";
        p.controller_.post(c);
    }
    static void connectModeFixture(MotorControl &p, const char *fixture) {
        MotorController::Command c; c.action=MotorController::Action::Connect;
        c.config.backend=AVA_ETH_NPCAP; c.config.library=fixture; c.config.device="live-mode";
        p.controller_.post(c);
    }
    static void enable(MotorControl &p) { p.requestConfirmation(MotorController::Action::Enable); }
    static void mode(MotorControl &p,int m) { p.requestConfirmation(MotorController::Action::SetMode,m); }
    static void cancel(MotorControl &p) { p.cancelConfirmation(); }
    static bool pending(MotorControl &p) { return p.pendingCommand_.has_value(); }
    static void input(MotorControl &p, int channel=0) { p.targetField(p.controller_.state(),channel); }
    static void units(MotorControl &p, int position, int velocity) { p.positionUnit_=position; p.velocityUnit_=velocity; p.targetDrafts_={}; }
    static Monitor *monitor(MotorControl &p) { return p.monitor_.get(); }
    static auto center(MotorControl &p) { return ImVec2(p.dialCenterX_,p.dialCenterY_); }
    static float radius(MotorControl &p) { return p.dialRadius_; }
    static float target(MotorControl &p) { return p.position_; }
    static void stalePosition(MotorControl &p, float val) { p.position_=val; }
    static void defaultCsp(MotorControl &p) { p.mode_=7; }
    static void page(MotorControl &p,int index) { p.rightPageRequest_=index; }
    static void acceptedTarget(MotorControl &p, float val) { p.setTargetEditor({0,p.selectedMode(),val,0,0,0}); }
    static const char *positionText(MotorControl &p) { return p.targetDrafts_[0].text; }
    static void prompt(MotorControl &p) { p.npcapPrompt_=true; }
    static void faults(MotorControl &p, const MotorController::State &s) { p.faultDetails(s); }
    static void special(MotorControl &p, int val) { p.requestConfirmation(MotorController::Action::Special,val); }
    static auto pendingCommand(MotorControl &p) { return *p.pendingCommand_; }
    static void controls(MotorControl &p, const MotorController::State &s) { p.controls(s); }
    static void confirmation(MotorControl &p, const MotorController::State &s) { p.confirmation(s); }
    static auto &ranges(MotorControl &p) { return p.gaugeRanges_; }
    static void range(MotorControl &p) { p.gaugeRangeEditor(1); }
    static void connection(MotorControl &p, const MotorController::State &s) { p.connection(s); }
    static void dashboard(MotorControl &p, const MotorController::State &s) { p.dashboard(s); }
    static void variables(MotorControl &p, const MotorController::State &s) { p.variables(s); }
    static void pid(MotorControl &p, const MotorController::State &s) { p.pidParameters(s); }
    static auto config(MotorControl &p) { return p.config(); }
    static auto &pidDraft(MotorControl &p) { return p.pidDraft_; }
    static auto &pidSelection(MotorControl &p) { return p.pidSelection_; }
    static bool pidPending(MotorControl &p) { return p.pendingPid_.has_value(); }
    static void jlink(MotorControl &p) {
        p.backend_ = 4;
        p.jlinkSource_ = "D:/固件/CM7.axf";
        p.refreshJlinkSymbols(true);
    }
    static bool resolveJlink(MotorControl &p) { p.refreshJlinkSymbols(false); return p.resolveJlinkConfig(); }
};
static void check(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
static void capture(GLFWwindow *w,const std::string &path) {
    int width,height; glfwGetFramebufferSize(w,&width,&height);
    std::vector<unsigned char> pixels(width*height*3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    std::ofstream out(path,std::ios::binary); out<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for(int y=height-1;y>=0;--y) out.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
}
int main(int argc,char **argv) {
    try {
        uint32_t pages[8]{};
#ifdef _WIN32
        // Session paths survive runas command-line quoting, including Chinese and spaces
        const std::wstring session=L"D:\\中文 会话\\电机.ava";
        const auto line=L"ava_tool.exe "+elevation::quote(session);
        int argumentCount=0;
        auto arguments=CommandLineToArgvW(line.c_str(),&argumentCount);
        check(arguments && argumentCount==2 && session==arguments[1],"elevation session argument quoting");
        LocalFree(arguments);
#endif
        check(motor_faults::decode(pages).empty(),"zero pages should not be a fault");
        pages[0]=(1u<<3)|(1u<<19)|(1u<<23)|(1u<<31);
        pages[1]=1u<<3; pages[2]=1u<<9; pages[3]=(1u<<7)|(1u<<31); pages[7]=1u<<31;
        auto decoded=motor_faults::decode(pages);
        check(decoded.size()==9,"multiple firmware fault bits lost");
        check(std::string(decoded[0].symbol)=="under_bus_volt","wire layout confused with internal safety page");
        check(std::string(decoded[1].detail->cause).find("无法仅凭此位")!=std::string::npos,"aggregate parameter bit over-specified");
        check(std::string(decoded[2].detail->clear).find("不会被普通清故障")!=std::string::npos,"power latch meaning lost");
        check(decoded[4].kind==motor_faults::Kind::Unknown,"retired Error2 guessed from old firmware");
        check(std::string(decoded[3].symbol)=="ctrl_cfg_invalid","unsigned bit31 decoding");
        check(decoded[5].kind==motor_faults::Kind::Warning,"warning mislabeled as fault");
        check(decoded[6].kind==motor_faults::Kind::Calibration && std::string(decoded[6].symbol)=="tor_sensor_cali_timeout","calibration layout");
        check(decoded.back().kind==motor_faults::Kind::Unknown,"unknown Error8 bit guessed");
        check(std::string(motor_faults::controllerReason(0x80000008u)).find("CiA402")!=std::string::npos,"controller error decoded as firmware bitmask");
        for(auto &page : pages) page=~0u;
        check(motor_faults::decode(pages).size()==256,"unknown or high bits discarded");
        check(argc>=3,"output folder required"); check(glfwInit(),"GLFW");
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        auto *window=glfwCreateWindow(1600,1100,"Motor monitor test",nullptr,nullptr); check(window,"window");
        glfwMakeContextCurrent(window); ImGui::CreateContext(); ImPlot::CreateContext();
        ImGui::GetIO().IniFilename=nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc",18,nullptr,ImGui::GetIO().Fonts->GetGlyphRangesChineseFull());
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window,false); ImGui_ImplOpenGL3_Init("#version 130");
        {
            MotorControl page;
            {
                MotorController::State s;
                s.connected=true; s.snapshot.count=1;
                auto &f=s.snapshot.axes[0];
                f.flags=AVA_ETH_ONLINE|AVA_ETH_FEEDBACK_VALID|AVA_ETH_TEMPERATURE_VALID|AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID;
                f.mode=8; f.timestamp_ns=1000000000; f.position=float(2*motor_units::pi+.1);
                f.velocity=10; f.current=4; f.motor_temperature=40;
                f.torque=-4; f.electromagnetic_torque=6;
                auto older=f; older.timestamp_ns=700000000; older.velocity=1000;
                s.history.push_back({.7,older,{}});
                older=f; older.timestamp_ns=900000000; older.position=float(2*motor_units::pi-.1);
                older.velocity=2; older.current=2; older.motor_temperature=100;
                older.torque=-2; older.electromagnetic_torque=2;
                older.flags &= ~AVA_ETH_TEMPERATURE_VALID;
                s.history.push_back({.9,older,{}});
                s.history.push_back({.91,older,{}});                   // Same SDK sample rendered/polled twice
                s.history.push_back({1,f,{}});
                const auto averaged=motorGaugeAverage(s,0);
                check(std::abs(averaged.velocity-6)<1e-6 && averaged.current==3,"200 ms average excludes old/duplicate samples");
                check(std::abs(averaged.position-2*motor_units::pi)<1e-6,"average continuous position before wrapping");
                check(averaged.motor_temperature==40,"invalid temperatures excluded");
                check(averaged.torque==-3 && averaged.electromagnetic_torque==4,
                      "load and electromagnetic torque must be averaged separately");
                for(auto &sample:s.history) sample.feedback.flags &= ~AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID;
                check(motorGaugeAverage(s,0).electromagnetic_torque==6,"legacy/invalid torque excluded from average");
                check(f.velocity==10 && f.position!=averaged.position && averaged.flags==f.flags,
                      "display averaging must not change feedback or control status");
                check(motorGaugeAverage(s,0).velocity==averaged.velocity,"averaging independent of render frequency");
                f.mode=-9;
                check(motorGaugeAverage(s,0).velocity==10,"mode switch starts a new display window");
                f.flags=0;
                check(motorGaugeAverage(s,0).flags==0 && motorGaugeAverage(s,0).velocity==10,
                      "invalid feedback is not hidden by historical averages");
            }
            check(std::abs(defaultMotorGaugeRanges[0].minimum+motor_units::pi)<1e-6 &&
                  std::abs(defaultMotorGaugeRanges[0].maximum-motor_units::pi)<1e-6,"centered default range");
            for(double angle : {0.0, -0.1, motor_units::pi, -motor_units::pi, 1039.2, -1039.2}) {
                auto split=motor_units::splitTurns(angle);
                check(split.angle>=-motor_units::pi && split.angle<motor_units::pi,"single-turn range");
                check(std::abs(split.count*2*motor_units::pi+split.angle-angle)<1e-9,"turn decomposition");
            }
            check(std::abs(motor_units::velocityScale(2)*2*motor_units::pi-60)<1e-9,"rpm conversion");
            check(std::abs(motor_units::positionScale(1)*motor_units::pi-180)<1e-9,"degree conversion");
            auto *legacy=cJSON_Parse(R"({"motorControl":{"gauge_ranges":[{"minimum":0,"maximum":6.28318530718}],"position_unit":1,"velocity_unit":2}})");
            page.load(legacy); cJSON_Delete(legacy);
            check(MotorControlTestAccess::ranges(page)[0].minimum<0,"old default migration");
            auto *unitSave=cJSON_CreateObject(); page.save(unitSave);
            check(cJSON_GetObjectItem(cJSON_GetObjectItem(unitSave,"motorControl"),"position_unit")->valueint==1 &&
                  cJSON_GetObjectItem(cJSON_GetObjectItem(unitSave,"motorControl"),"velocity_unit")->valueint==2,"unit persistence");
            cJSON_Delete(unitSave);
            // Legacy simulator sessions migrate to Npcap without reusing their device path
            for (int backend : {0,1,2,3}) {
                auto *session=cJSON_CreateObject(); auto *settings=cJSON_AddObjectToObject(session,"motorControl");
                cJSON_AddNumberToObject(settings,"backend",backend); cJSON_AddStringToObject(settings,"device","saved-device");
                page.load(session); cJSON_Delete(session);
                auto *saved=cJSON_CreateObject(); page.save(saved); settings=cJSON_GetObjectItem(saved,"motorControl");
                check(cJSON_GetObjectItem(settings,"backend")->valueint==(backend<2?3:backend),"hardware backend session migration");
                check(std::string(cJSON_GetObjectItem(settings,"device")->valuestring)==(backend<2?"":"saved-device"),"legacy simulator device reused");
                cJSON_Delete(saved);
            }
            auto *custom=cJSON_Parse(R"({"motorControl":{"backend":3,"device":"test-path","device_name":"电机网卡·以太网","gauge_ranges":[{"minimum":-2,"maximum":10},{"minimum":-20,"maximum":30},{"minimum":5,"maximum":5}]}})");
            page.load(custom); cJSON_Delete(custom);
            check(MotorControlTestAccess::ranges(page)[0].minimum==-2 && MotorControlTestAccess::ranges(page)[0].maximum==10,"position range load");
            check(MotorControlTestAccess::ranges(page)[2].minimum==-40,"invalid range should use defaults");
            auto *saved=cJSON_CreateObject(); page.save(saved);
            auto *settings=cJSON_GetObjectItem(saved,"motorControl");
            check(std::string(cJSON_GetObjectItem(settings,"device_name")->valuestring)=="电机网卡·以太网","UTF-8 adapter name session");
            check(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(settings,"gauge_ranges"),1),"maximum")->valuedouble==30,"range persistence");
            cJSON_Delete(saved);
            page.setOpen(true); g_lang=Lang::ZH;
            bool probe=false, faultProbe=false, specialProbe=false, rangeProbe=false, nicProbe=false, modeProbe=false;
            bool torqueProbe=false, pidProbe=false;
            ImVec2 pidButton;
            ImVec2 probeInput, rangeButton, modeButton, specialButton;
            int probeChannel=0;
            MotorController::State faultState{};
            auto frame=[&](const char *file=nullptr) {
                glfwPollEvents(); ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0,0),ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(1600,1100),ImGuiCond_Always);
                if (!probe) page.draw(); else MotorControlTestAccess::pulse(page);
                if(probe) {
                    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(300,100));
                    ImGui::Begin("Input probe"); MotorControlTestAccess::input(page,probeChannel);
                    probeInput=ImVec2((ImGui::GetItemRectMin().x+ImGui::GetItemRectMax().x)/2,
                                     (ImGui::GetItemRectMin().y+ImGui::GetItemRectMax().y)/2); ImGui::End();
                }
                if(faultProbe) {
                    ImGui::SetNextWindowPos(ImVec2(380,120)); ImGui::SetNextWindowSize(ImVec2(900,650));
                    ImGui::Begin("Firmware fault details"); MotorControlTestAccess::faults(page,faultState); ImGui::End();
                }
                if(specialProbe) {
                    ImGui::SetNextWindowPos(ImVec2(400,120)); ImGui::SetNextWindowSize(ImVec2(600,620));
                    ImGui::Begin("Actuator controls");
                    auto *window=ImGui::GetCurrentWindow();
                    ImGui::SetFocusID(window->GetID("##special_cmd"),window);
                    MotorControlTestAccess::controls(page,faultState);
                    MotorControlTestAccess::confirmation(page,faultState);
                    specialButton=ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                    ImGui::End();
                }
                if(modeProbe) {
                    ImGui::SetNextWindowPos(ImVec2(400,120)); ImGui::SetNextWindowSize(ImVec2(600,620));
                    ImGui::Begin("Live mode controls");
                    auto *window=ImGui::GetCurrentWindow();
                    ImGui::SetFocusID(window->GetID("##mode"),window);
                    MotorControlTestAccess::controls(page,MotorControlTestAccess::state(page));
                    modeButton=ImGui::WindowRectRelToAbs(window,window->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                    ImGui::End();
                }
                if(rangeProbe) {
                    ImGui::SetNextWindowPos(ImVec2(350,100)); ImGui::SetNextWindowSize(ImVec2(500,300));
                    ImGui::Begin("Gauge range"); MotorControlTestAccess::range(page);
                    if(!ImGui::IsPopupOpen("##gaugeRange")) rangeButton=ImVec2(ImGui::GetItemRectMin().x+20,ImGui::GetItemRectMin().y+10);
                    ImGui::End();
                }
                if(nicProbe) {
                    ImGui::SetNextWindowPos(ImVec2(350,100)); ImGui::SetNextWindowSize(ImVec2(600,500));
                    ImGui::Begin("Adapter names"); MotorControlTestAccess::connection(page,faultState); ImGui::End();
                }
                if(torqueProbe) {
                    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1600,1050));
                    ImGui::Begin("Torque feedback");
                    if(ImGui::BeginTable("torqueView",2)) {
                        ImGui::TableNextColumn(); MotorControlTestAccess::variables(page,faultState);
                        ImGui::TableNextColumn(); MotorControlTestAccess::dashboard(page,faultState);
                        ImGui::EndTable();
                    }
                    ImGui::End();
                }
                if(pidProbe) {
                    ImGui::SetNextWindowPos(ImVec2(250,100)); ImGui::SetNextWindowSize(ImVec2(1000,600));
                    ImGui::Begin("PID parameters"); ImGui::SetNextItemOpen(true,ImGuiCond_Always);
                    MotorControlTestAccess::pid(page,faultState);
                    pidButton=ImVec2(ImGui::GetItemRectMin().x+30,ImGui::GetItemRectMin().y+10);
                    ImGui::End();
                }
                ImGui::Render(); glViewport(0,0,1600,1100); glClearColor(.06f,.06f,.06f,1); glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(file) capture(window,std::string(argv[2])+"/"+file);
                glfwSwapBuffers(window);
            };
            auto frames=[&](int n) { for(int i=0;i<n;++i) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } };
            auto click=[&](ImVec2 pos) {
                ImGui::GetIO().AddMousePosEvent(pos.x,pos.y); ImGui::GetIO().AddMouseButtonEvent(0,true); frame(); frame();
                ImGui::GetIO().AddMouseButtonEvent(0,false); frame(); frame();
            };
            auto chooseComboRow=[&](int row) {
                auto *popup=ImGui::FindWindowByName("##Combo_00");
                check(popup && popup->Active,"combo popup missing");
                const float rowHeight=ImGui::GetTextLineHeight()+ImGui::GetStyle().ItemSpacing.y;
                click(ImVec2(popup->Pos.x+popup->Size.x*.5f,
                             popup->Pos.y+popup->WindowPadding.y+(row+.5f)*rowHeight));
            };
            auto confirm=[&] {
                frames(2); auto *popup=ImGui::FindWindowByName("###motorConfirm"); check(popup && popup->Active,"confirmation popup missing");
                click(ImVec2(popup->Pos.x+24,popup->Pos.y+popup->Size.y-20)); frames(20);
                check(!MotorControlTestAccess::pending(page),"confirmation was not accepted");
            };
            frames(3); frame("motor-reused-monitor-disconnected.ppm");
            pidProbe=true; faultState={}; faultState.pidSentMask=255; faultState.pidRevision=1;
            faultState.connected=true; faultState.snapshot.count=1;
            faultState.snapshot.axes[0].flags=AVA_ETH_FEEDBACK_VALID|AVA_ETH_ONLINE|AVA_ETH_ENABLED;
            faultState.pidConfig=MotorControlTestAccess::config(page);
            for(int i=0;i<8;++i) faultState.pid.values[i]=float(i+1);
            frames(2);
            check(MotorControlTestAccess::pidDraft(page).values[7]==8,"last transmitted precision populates editor");
            check(MotorControlTestAccess::pidSelection(page)==0,"PID history must not auto-select writes");
            MotorControlTestAccess::pidSelection(page)=4;
            MotorControlTestAccess::pidDraft(page).values[2]=3.5f;
            frame("motor-pid-parameters.ppm"); click(pidButton); frames(2);
            check(MotorControlTestAccess::pidPending(page),"PID write must open native confirmation");
            auto *pidPopup=ImGui::FindWindowByName("###pidConfirm");
            check(pidPopup && pidPopup->Active,"PID native confirmation missing");
            frame("motor-pid-confirm.ppm");
            const float cancelX=pidPopup->Pos.x+ImGui::GetStyle().WindowPadding.x+
                ImGui::CalcTextSize("确认写入").x+2*ImGui::GetStyle().FramePadding.x+
                ImGui::GetStyle().ItemSpacing.x+ImGui::GetStyle().FramePadding.x+ImGui::CalcTextSize("取消").x/2;
            click(ImVec2(cancelX,pidPopup->Pos.y+pidPopup->Size.y-20)); frames(2);
            check(!MotorControlTestAccess::pidPending(page),"PID cancel must discard pending write");
            check(!MotorControlTestAccess::state(page).pidSentMask,"PID cancel must not contact hardware");
            pidProbe=false;
            MotorControlTestAccess::connect(page); frames(30);
            check(MotorControlTestAccess::state(page).connected,"connect");
            auto *monitor=MotorControlTestAccess::monitor(page);
            check(monitor && monitor->externalFeed_ && monitor->getScopes().size()==6,"real Monitor not embedded");
            check(monitor->findChannel("Position","feedback")->read_.rawSize()>0,"SDK data not published to MonitorChannel");
            check(!monitor->findChannel("Position","feedback")->isWritable(),"feedback must be read only");
            frame("motor-reused-monitor-connected.ppm");
            MotorControlTestAccess::enable(page); frames(2);
            check(!(MotorControlTestAccess::state(page).snapshot.axes[0].flags&AVA_ETH_ENABLED),"enable bypassed confirmation");
            frame("motor-confirm-native.ppm"); confirm();
            check(MotorControlTestAccess::state(page).snapshot.axes[0].flags&AVA_ETH_ENABLED,"confirmed enable");
            // Real shared target input + Enter event, not a direct command call
            probe=true; frames(2); click(probeInput);
            ImGui::GetIO().AddInputCharactersUTF8("999"); frame();
            MotorControlTestAccess::acceptedTarget(page,2.5f); frames(2);
            check(std::abs(std::strtof(MotorControlTestAccess::positionText(page),nullptr)-2.5f)<.0001f &&
                  MotorControlTestAccess::target(page)==2.5f,"focused input retained the old target after synchronization");
            check(MotorControlTestAccess::state(page).targets[0].position==0,"editor synchronization sent a motion command");
            click(probeInput);
            ImGui::GetIO().AddInputCharactersUTF8("0.75"); frame();
            check(!MotorControlTestAccess::pending(page),"typing should not send");
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true); frame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false); frame(); probe=false; frames(2);
            check(MotorControlTestAccess::pending(page),"Enter did not open confirmation");
            check(MotorControlTestAccess::state(page).targets[0].position==0,"Enter sent before confirmation");
            confirm(); check(std::abs(MotorControlTestAccess::state(page).targets[0].position-.75f)<.001,"Enter target not sent");
            MotorControlTestAccess::mode(page,10); frames(2);
            check(MotorControlTestAccess::state(page).targets[0].mode==8,"mode sent before confirmation");
            MotorControlTestAccess::cancel(page); ImGui::ClosePopupToLevel(0,true); frames(2);
            check(MotorControlTestAccess::state(page).targets[0].mode==8,"cancel changed mode");
            MotorControlTestAccess::mode(page,10); confirm();
            auto changed=MotorControlTestAccess::state(page);
            check(changed.targets[0].mode==-9 && (changed.snapshot.axes[0].flags&AVA_ETH_ENABLED),"mode switch did not preserve enable");
            // Reproduce the reported interaction through the actual mode segment, with an
            // old position deliberately left in the editor. Do not call requestConfirmation
            MotorControlTestAccess::stalePosition(page,999);
            modeProbe=true; frames(2); click(modeButton); frames(2);
            frame("motor-mode-dropdown.ppm");
            chooseComboRow(7); frames(2);
            check(MotorControlTestAccess::pending(page),"actual mode did not open confirmation");
            const auto modeRequest=MotorControlTestAccess::pendingCommand(page);
            check(modeRequest.action==MotorController::Action::SetMode && modeRequest.target.mode==8,
                  "CSP segment used the wrong command path");
            check(std::abs(modeRequest.target.position-999)>1,
                  "CSP confirmation retained the old editor target");
            const float heldPosition=MotorControlTestAccess::state(page).snapshot.axes[0].position;
            modeProbe=false; confirm(); frames(10);
            const auto held=MotorControlTestAccess::state(page);
            check(held.targets[0].mode==8 && (held.snapshot.axes[0].flags&AVA_ETH_ENABLED),
                  "actual CSP selection lost mode or enable");
            check(std::abs(held.targets[0].position-heldPosition)<.01f &&
                  std::abs(held.snapshot.axes[0].position-heldPosition)<.01f &&
                  std::abs(MotorControlTestAccess::target(page)-held.targets[0].position)<.0001f,
                  "actual CSP selection chased the old target or failed to synchronize the dial");
            frame("motor-csp-old-target-replaced.ppm");
            // Re-selecting the displayed mode must still execute a fresh hold-position
            // request (also covers the default CSP selection immediately after connect)
            MotorControlTestAccess::stalePosition(page,999);
            modeProbe=true; frames(2); click(modeButton); frames(2); chooseComboRow(7); frames(2);
            check(MotorControlTestAccess::pending(page),"reselecting CSP skipped the fresh-position request");
            check(std::abs(MotorControlTestAccess::target(page)-999)>1,"reselected CSP retained editor target");
            modeProbe=false; confirm(); frames(2);
            check(std::abs(MotorControlTestAccess::target(page)-MotorControlTestAccess::state(page).targets[0].position)<.0001f,
                  "reselected CSP editor does not match accepted target");
            for(int mode : {8,0,1,12,7}) {
                const float current=MotorControlTestAccess::state(page).snapshot.axes[0].position;
                MotorControlTestAccess::mode(page,mode); confirm(); frames(2);
                const auto switched=MotorControlTestAccess::state(page);
                check(switched.snapshot.axes[0].flags&AVA_ETH_ENABLED,"position mode switch lost enable");
                check(std::abs(switched.targets[0].position-current)<.01f,"position mode did not start at feedback");
                check(std::abs(MotorControlTestAccess::target(page)-switched.targets[0].position)<.0001f,
                      "position pointer not synchronized with accepted initial target");
            }
            auto center=MotorControlTestAccess::center(page); auto radius=MotorControlTestAccess::radius(page)*.84f;
            MotorControlTestAccess::page(page,2); frames(3);
            center=MotorControlTestAccess::center(page); radius=MotorControlTestAccess::radius(page)*.84f;
            const double from=MotorControlTestAccess::target(page);
            const auto positionRange=MotorControlTestAccess::ranges(page)[0];
            const double scale=2*PositionDialDrag::pi/positionRange.span();
            const double pointer=(from-positionRange.minimum)*scale;
            ImGui::GetIO().AddMousePosEvent(center.x+float(cos(pointer))*radius,center.y-float(sin(pointer))*radius);
            ImGui::GetIO().AddMouseButtonEvent(0,true); frames(2);
            ImGui::GetIO().AddMousePosEvent(center.x+float(cos(pointer+.4*scale))*radius,center.y-float(sin(pointer+.4*scale))*radius); frames(2);
            ImGui::GetIO().AddMouseButtonEvent(0,false); frames(2);
            check(std::abs(MotorControlTestAccess::target(page)-float(from+.4))<.04,"position drag");
            frame("motor-reused-monitor-final.ppm");
            monitor->setSamplingPaused(true); auto count=monitor->findChannel("Position","feedback")->read_.rawSize(); frames(12);
            check(monitor->findChannel("Position","feedback")->read_.rawSize()==count,"monitor pause");
            monitor->setSamplingPaused(false); frames(10);
            check(monitor->findChannel("Position","feedback")->read_.rawSize()>count,"monitor resume");
            auto positionScope=monitor->getScopes().at("Position");
            positionScope->setPaused(true);
            count=monitor->findChannel("Position","feedback")->read_.rawSize();
            auto velocityCount=monitor->findChannel("Velocity","feedback")->read_.rawSize(); frames(10);
            check(monitor->findChannel("Position","feedback")->read_.rawSize()==count,"scope pause");
            check(monitor->findChannel("Velocity","feedback")->read_.rawSize()>velocityCount,"scope pause affected other scopes");
            positionScope->setPaused(false); frames(10);
            check(monitor->findChannel("Position","feedback")->read_.rawSize()>count,"scope resume");
            monitor->requestClearData(); frames(2);
            check(monitor->findChannel("Position","feedback")->read_.rawSize()<count,"monitor clear");
            MotorControlTestAccess::units(page,1,2);
            auto enterUnitTarget=[&](int channel,const char *text) {
                probeChannel=channel; probe=true; frames(2); click(probeInput);
                ImGui::GetIO().AddInputCharactersUTF8(text); frame();
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true); frame();
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false); frame(); probe=false; frames(2);
                check(MotorControlTestAccess::pending(page),"unit input did not prompt"); confirm();
            };
            enterUnitTarget(0,"180");
            check(std::abs(MotorControlTestAccess::state(page).targets[0].position-motor_units::pi)<1e-5,"degrees not converted to radians");
            MotorControlTestAccess::mode(page,10); confirm();
            enterUnitTarget(1,"60");
            check(std::abs(MotorControlTestAccess::state(page).targets[0].velocity-2*motor_units::pi)<1e-5,"rpm not converted to rad/s");
            MotorControlTestAccess::ranges(page)[0]=defaultMotorGaugeRanges[0];
            frames(3); frame("motor-units-turns.ppm");
            g_lang=Lang::EN; frames(2); frame("motor-reused-monitor-en.ppm");
            MotorControlTestAccess::prompt(page); frames(2); frame("motor-npcap-native.ppm");
            ImGui::ClosePopupToLevel(0,true); page.setOpen(false);
            g_lang=Lang::ZH; faultProbe=true;
            faultState.connected=true; faultState.diagnostics_valid=true; faultState.snapshot.count=1;
            faultState.snapshot.axes[0].flags=AVA_ETH_ONLINE|AVA_ETH_FEEDBACK_VALID|AVA_ETH_FAULT;
            faultState.snapshot.axes[0].error_code=AVA_ETH_ERR_DRIVE_FAULT;
            faultState.diagnostics.error_pages[0]=(1u<<3)|(1u<<19)|(1u<<23);
            faultState.diagnostics.error_pages[2]=1u<<9;
            faultState.diagnostics.error_pages[3]=1u<<7;
            faultState.diagnostics.error_pages[7]=1u<<31;
            frames(2); frame("motor-fault-chinese.ppm");
            faultProbe=false; specialProbe=true;
            faultState.info.axes=1; faultState.info.capabilities=AVA_ETH_CAP_HARDWARE|AVA_ETH_CAP_SPECIAL_CMD;
            faultState.snapshot.axes[0].flags=AVA_ETH_ONLINE|AVA_ETH_FEEDBACK_VALID;
            faultState.snapshot.axes[0].error_code=0;
            frames(2); frame("motor-special-controls.ppm");
            click(specialButton); frames(2); frame("motor-special-dropdown.ppm");
            ImGui::ClosePopupToLevel(0,true); frames(2);
            MotorControlTestAccess::special(page,3); frames(2);
            check(MotorControlTestAccess::pendingCommand(page).action==MotorController::Action::Special &&
                  MotorControlTestAccess::pendingCommand(page).special_cmd==3,"special command payload");
            frame("motor-special-confirm.ppm");
            MotorControlTestAccess::cancel(page); ImGui::ClosePopupToLevel(0,true); frames(2);
            check(!MotorControlTestAccess::pending(page),"cancel special command");
            specialProbe=false; rangeProbe=true; frames(2); click(rangeButton); frames(2);
            frame("motor-range-editor.ppm"); ImGui::ClosePopupToLevel(0,true); rangeProbe=false;
            nicProbe=true; faultState={};
            faultState.devices={{"test-path","电机网卡·以太网","中文网卡描述"},
                                {"\\Device\\NPF_{demo-a}","以太网 2","Realtek"},
                                {"\\Device\\NPF_{demo-b}","生产线一号","Intel"}};
            frames(2); frame("motor-nic-chinese.ppm");
            nicProbe=false;
            check(!page.consumeElevationRequest(),"ordinary motor UI requested elevation");
            if(argc>=4) {
                page.setOpen(true);
                MotorControlTestAccess::units(page,0,0);
                probeChannel=0;
                MotorControlTestAccess::connectModeFixture(page,argv[3]); frames(20);
                check(MotorControlTestAccess::state(page).snapshot.axes[0].position==73.125f,"mode fixture feedback");
                check(MotorControlTestAccess::target(page)==73.125f,"connect did not initialize target pointer from first feedback");
                MotorControlTestAccess::defaultCsp(page);
                MotorControlTestAccess::stalePosition(page,999);
                const auto initialModeRevision=MotorControlTestAccess::state(page).mode_revisions[0];
                MotorControlTestAccess::enable(page); confirm(); frames(2);
                auto firstEnable=MotorControlTestAccess::state(page);
                check(firstEnable.targets[0].position==73.125f && MotorControlTestAccess::target(page)==73.125f &&
                      (firstEnable.snapshot.axes[0].flags&AVA_ETH_ENABLED),"default CSP enable did not hold and synchronize current position");
                check(firstEnable.mode_revisions[0]==initialModeRevision,"test must exercise Enable without a SetMode request");
                frame("motor-default-csp-enable-holds-feedback.ppm");
                for(int mode : {7,8,0,1,12}) {
                    const float before=MotorControlTestAccess::state(page).snapshot.axes[0].position;
                    MotorControlTestAccess::stalePosition(page,999);
                    MotorControlTestAccess::mode(page,mode); confirm(); frames(2);
                    const auto state=MotorControlTestAccess::state(page);
                    check(state.targets[0].position==before+1.25f,"actor ignored freshest target returned by SDK");
                    check(MotorControlTestAccess::target(page)==state.targets[0].position,
                          "confirmed mode did not update target pointer from SDK result");
                    probe=true; frames(2);
                    check(std::abs(std::strtof(MotorControlTestAccess::positionText(page),nullptr)-state.targets[0].position)<.0001f,
                          "confirmed mode did not update visible target input from SDK result");
                    probe=false;
                }
                frame("motor-position-mode-target-synced.ppm");
            }
            if(argc>=4 && !elevation::isAdministrator()) {
                page.setOpen(true);
                MotorControlTestAccess::connectDenied(page,argv[3]); frames(20);
                check(page.consumeElevationRequest(),"denied device access did not request a permission prompt");
                frames(5);
                check(!page.consumeElevationRequest(),"dismissed permission prompt repeated every frame");
                frame("motor-permission-denied.ppm");
            }
            page.setOpen(false); torqueProbe=true; faultState={};
            faultState.connected=true; faultState.snapshot.count=1;
            auto &torqueFeedback=faultState.snapshot.axes[0];
            torqueFeedback.flags=AVA_ETH_ONLINE|AVA_ETH_FEEDBACK_VALID|AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID;
            torqueFeedback.torque=-3.25f; torqueFeedback.electromagnetic_torque=6.5f;
            torqueFeedback.timestamp_ns=100000000000;
            faultState.history.push_back({100,torqueFeedback,{}});
            frames(2);
            check(monitor->findChannel("Torque","feedback")->getRVal()==-3.25f &&
                  monitor->findChannel("Torque","electromagnetic_torque")->getRVal()==6.5f,
                  "monitor swapped or dropped load/electromagnetic torque");
            frame("motor-two-torques.ppm");
            torqueFeedback.flags &= ~AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID;
            torqueFeedback.timestamp_ns+=1000000;
            faultState.history.push_back({100.001,torqueFeedback,{}}); frames(2);
            check(std::isnan(monitor->findChannel("Torque","electromagnetic_torque")->getRVal()),
                  "old runtime or invalid electromagnetic torque must show a waveform gap");
            torqueProbe=false; probe=false;
            std::vector<motor_jlink::Symbol> symbols;
            for (int i=0; i<motor_jlink::Count; ++i) {
                const bool analog = (i>=motor_jlink::TargetPosition && i<=motor_jlink::TargetTorque) ||
                    (i>=motor_jlink::FeedVelocity && i<=motor_jlink::InverterTemperature);
                symbols.push_back({"D:/固件/CM7.axf",motor_jlink::preset(motor_jlink::Field(i)),uint32_t(0x20000000+i*16),4,analog,!analog});
            }
            page.onJlinkSymbols=[&]{return symbols;};
            MotorControlTestAccess::jlink(page);
            check(MotorControlTestAccess::resolveJlink(page),"J-Link preset selection");
            auto *jlinkSave=cJSON_CreateObject(); page.save(jlinkSave);
            page.load(jlinkSave); cJSON_Delete(jlinkSave);
            auto restored=MotorControlTestAccess::config(page);
            check(restored.backend==motor_jlink::backend && !restored.jlink.fields[motor_jlink::Mode].address,
                  "J-Link session must restore names/backend without trusting stale addresses");
            check(MotorControlTestAccess::resolveJlink(page),"J-Link named mapping session resolution");
            symbols[0].address+=0x10000;
            check(MotorControlTestAccess::resolveJlink(page) &&
                MotorControlTestAccess::config(page).jlink.fields[0].address==symbols[0].address,
                "J-Link did not resolve relocated ELF address");
            const auto modeSymbol=symbols.front();
            symbols.erase(symbols.begin());
            check(!MotorControlTestAccess::resolveJlink(page),"missing J-Link mode symbol accepted");
            symbols.insert(symbols.begin(),modeSymbol);
            check(MotorControlTestAccess::resolveJlink(page),"restored J-Link symbol");
            page.setOpen(true); frames(3); frame("motor-jlink-mapping.ppm");
            page.setOpen(false);
        }
        {
            const auto profilePath=std::filesystem::path(argv[2])/"safety-ui-profile.json";
            const auto invalidPath=std::filesystem::path(argv[2])/"safety-ui-invalid.json";
            struct ProfileCleanup {
                std::filesystem::path first,second;
                ~ProfileCleanup() { std::error_code ec; std::filesystem::remove(first,ec); std::filesystem::remove(second,ec); }
            } cleanup{profilePath,invalidPath};
            std::ofstream(profilePath) << R"json({"format":"safety","version":1,"name":"UI JSON regression",
              "description":"Arbitrary paths and typed values",
              "decoders":[
                {"id":"bus_error_1","aliases":["ext1"],"title":"Error1 JSON","kind":"bits","entries":[
                  {"mask":"0x00000001","name":"test_error","title":"JSON test error","severity":"error","cause":"Test cause","action":"Inspect source","clear":"Source recovers"}]},
                {"id":"bus_error_3","aliases":["ext3"],"title":"Error3 JSON","kind":"bits","entries":[]},
                {"id":"signed","title":"Signed status","kind":"enum","type":"i32","entries":[
                  {"value":"-7","name":"test_negative","title":"Negative enum","severity":"warning"}]},
                {"id":"float","title":"Floating point","kind":"number","type":"f32"},
                {"id":"counter","title":"Counter","kind":"number","type":"u64"}],
              "groups":[
                {"id":"motor","title":"Motor Safety","root":"g_mc.lo.safety","expand":true,
                  "fields":[{"path":"fault.page.hw.val","title":"Hardware fault","decoder":"ext1"}]},
                {"id":"shared","title":"Shared errors","root":"g_shm.rt_ctrl_data.err_code","expand":true},
                {"id":"custom","title":"Custom object","root":"custom.runtime","expand":true,
                  "fields":[{"path":"status","title":"Signed result","decoder":"signed"},{"path":"temperature","title":"Temperature","decoder":"float","unit":"C"}]},
                {"id":"conditional","title":"Conditional object","root":"custom.runtime","expand":false,
                  "when":{"path":"protocol","value":"1"},"fields":[{"path":"counter","title":"Counter","decoder":"counter"}]}
              ]})json";
            std::ofstream(invalidPath) << R"json({"format":"safety","version":1,"name":"Broken","decoders":[],"groups":[{"id":"bad","root":"a","fields":[{"path":"x","decoder":"missing"}]}]})json";
            FaultDecoder decoder;
            check(decoder.loadProfile(profilePath.string()),"load JSON safety definition");
            check(!decoder.loadProfile(invalidPath.string()) && FaultDecoderTestAccess::profile(decoder).name=="UI JSON regression",
                  "invalid JSON replaced working profile");
            FaultDecoder::Variables sources;
            auto source = std::make_shared<Variable>("fault-test");
            sources.emplace("fault-test", source);
            source->addLocalVar("Error1", DataType::U32, 4);
            source->setLocalScalar("Error1", 0x80000001u);
            source->addLocalVar("float_value", DataType::F32, 4);
            decoder.open = true;
            check(decoder.bind(sources,"fault-test","Error1"), "bind integer variable");
            check(decoder.refresh(sources) && FaultDecoderTestAccess::val(decoder)==0x80000001u,
                  "raw u32 bits lost through float conversion");
            source->setLocalScalar("Error1", 0x00880008u);
            check(decoder.refresh(sources) && FaultDecoderTestAccess::val(decoder)==0x00880008u,
                  "bound variable did not update automatically");
            check(decoder.bind(sources,"fault-test","float_value") && decoder.refresh(sources), "typed float binding rejected");
            check(decoder.bind(sources,"fault-test","Error1") && decoder.refresh(sources), "restore integer binding");
            source->addLocalVar("signed_mask", DataType::I32, 4);
            source->setLocalScalar("signed_mask", -2147483647.0);
            uint32_t raw=0; std::string status;
            check(source->readDiagnosticWord("signed_mask",raw,status) && raw==0x80000001u,"signed raw bit pattern");
            std::vector<VarEntry::StructField> fields(1);
            std::strcpy(fields[0].name,"error"); fields[0].type=DataType::U32; fields[0].byteOffset=4;
            source->addLocalStructVar("feedback",fields,8);
            auto expected=source->snapshotLocalBuffers().at("feedback"); auto desired=expected;
            const uint32_t member=0xF0000001u; std::memcpy(desired.data()+4,&member,4);
            check(source->compareExchangeLocalBuffer("feedback",expected,desired),"publish SDK table bytes");
            check(source->readDiagnosticWord("feedback.error",raw,status) && raw==member,"LOCAL member exact decoding");
            FaultDecoderTestAccess::page(decoder,"ext3");
            check(decoder.bind(sources,"fault-test","feedback.error") && decoder.refresh(sources),"per-page binding");
            auto *saved=cJSON_CreateObject(); decoder.save(saved);
            auto *savedDecoder=cJSON_GetObjectItemCaseSensitive(saved,"faultDecoder");
            cJSON_ReplaceItemInObjectCaseSensitive(savedDecoder,"page",cJSON_CreateString("ext3"));
            cJSON *savedBinding=nullptr;
            cJSON_ArrayForEach(savedBinding,cJSON_GetObjectItemCaseSensitive(savedDecoder,"bindings")) {
                const auto *id=cJSON_GetObjectItemCaseSensitive(savedBinding,"page");
                if(cJSON_IsString(id) && std::string(id->valuestring)=="bus_error_1")
                    cJSON_ReplaceItemInObjectCaseSensitive(savedBinding,"page",cJSON_CreateString("ext1"));
                else if(cJSON_IsString(id) && std::string(id->valuestring)=="bus_error_3")
                    cJSON_ReplaceItemInObjectCaseSensitive(savedBinding,"page",cJSON_CreateString("ext3"));
            }
            FaultDecoder restored; restored.load(saved); cJSON_Delete(saved);
            check(FaultDecoderTestAccess::decoderId(restored)=="bus_error_3" &&
                  !FaultDecoderTestAccess::bindingExists(restored,"ext1") &&
                  !FaultDecoderTestAccess::bindingExists(restored,"ext3"),"legacy decoder aliases were not canonicalized");
            check(!FaultDecoderTestAccess::hasValue(restored),"session trusted stale diagnostic bytes");
            check(restored.refresh(sources) && FaultDecoderTestAccess::val(restored)==member,"restore named variable binding");
            FaultDecoderTestAccess::page(restored,"ext1");
            check(restored.refresh(sources) && FaultDecoderTestAccess::val(restored)==0x00880008u,"independent Error1 binding lost");
            FaultDecoderTestAccess::simulateLegacyBinding(restored,"bus_error_1","ext1");
            check(restored.loadProfile(profilePath.string()) && FaultDecoderTestAccess::decoderId(restored)=="bus_error_1" &&
                  !FaultDecoderTestAccess::bindingExists(restored,"ext1") && !FaultDecoderTestAccess::hasValue(restored),
                  "same-file reload did not migrate alias binding / reset cached sample");
            check(restored.refresh(sources) && FaultDecoderTestAccess::val(restored)==0x00880008u,"alias reload lost live source");
            sources.clear();
            check(!restored.refresh(sources) && !FaultDecoderTestAccess::valid(restored) &&
                FaultDecoderTestAccess::val(restored)==0x00880008u,"closed manager not marked stale");
            sources.emplace("fault-test",source);
            check(restored.refresh(sources),"manager restored");
            g_lang=Lang::ZH;
            auto renderDecoder=[&](const char *file) {
                for(int frame=0;frame<3;++frame) {
                    glfwPollEvents(); ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(12,12),ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(1150,1000),ImGuiCond_Always);
                    restored.draw(sources);
                    ImGui::Render(); glViewport(0,0,1600,1100); glClearColor(.06f,.07f,.09f,1); glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    if(frame==2) capture(window,std::string(argv[2])+"/"+file);
                    glfwSwapBuffers(window);
                }
            };
            renderDecoder("motor-fault-decoder-live.ppm");
            source->vars_.erase(source->vars_.begin());
            check(!restored.refresh(sources),"deleted variable still read by old address");
            renderDecoder("motor-fault-decoder-stale.ppm");
            restored.inspect(0,0x80080008u,19);
            check(FaultDecoderTestAccess::decoderId(restored)=="bus_error_1","motor-page inspect alias was not canonicalized");
            renderDecoder("motor-fault-decoder-offline.ppm");
            restored.load(nullptr);
            check(!restored.open && !restored.refresh(sources),"new session retained binding");
            // 验证整组绑定后可读取全部字段
            sources.clear();
            auto whole=std::make_shared<Variable>("whole-faults");
            sources.emplace("whole-faults",whole);
            for(unsigned group=0;group<2;++group) {
                const auto specs=motor_faults::groupFields(group==1);
                std::vector<VarEntry::StructField> members(specs.size());
                for(size_t i=0;i<specs.size();++i) {
                    std::snprintf(members[i].name,sizeof(members[i].name),"%s",specs[i].suffixes[0].substr(1).c_str());
                    members[i].type=DataType::U32; members[i].byteOffset=uint32_t(i*4);
                }
                const std::string name=group==0 ? "g_mc.lo.safety" : "g_shm.rt_ctrl_data.err_code";
                whole->addLocalStructVar(name,members,members.size()*4);
                auto old=whole->snapshotLocalBuffers().at(name), next=old;
                const uint32_t hw=group==0 ? 0x80000001u : 0x80080008u;
                std::memcpy(next.data()+(group==0 ? 4 : 0),&hw,4);
                const uint32_t high=0x80000000u;
                if(group==1) std::memcpy(next.data()+7*4,&high,4);
                check(whole->compareExchangeLocalBuffer(name,old,next),"publish whole diagnostic object");
            }
            check(restored.loadProfile(profilePath.string()),"reload fixture profile after new session");
            restored.open=true;
            restored.detectGroups(sources);
            restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::group(restored,0).rows.size()==motor_faults::groupFields(false).size(),"safety fields not all bound");
            check(FaultDecoderTestAccess::group(restored,1).rows.size()==8,"shared Error1..8 not all bound");
            for(unsigned group=0;group<2;++group)
                for(const auto &row:FaultDecoderTestAccess::group(restored,group).rows) check(row.valid,"whole-group LOCAL field unavailable");
            check(FaultDecoderTestAccess::group(restored,0).rows[0].field.path=="g_mc.lo.safety.fault.page.hw.val",
                  "configured diagnostic priority ignored");
            check(FaultDecoderTestAccess::group(restored,0).rows[0].val.bits==0x80000001u,"safety integer precision");
            check(FaultDecoderTestAccess::group(restored,1).rows[7].val.bits==0x80000000u,"unknown shared Error8 bits lost");
            FaultDecoderTestAccess::requestGroup(restored,0,false);
            restored.refreshGroups(sources);
            for(const auto &row:FaultDecoderTestAccess::group(restored,0).rows)
                check(!row.valid && row.hasValue && row.status.find("暂停采样")!=std::string::npos,"hidden group must retain only stale values");
            FaultDecoderTestAccess::requestGroup(restored,0,true);
            restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::group(restored,0).rows[0].valid,"expanded group must resume sampling");
            renderDecoder("motor-fault-groups.ppm");
            auto *groupSave=cJSON_CreateObject(); restored.save(groupSave);
            restored.load(groupSave); cJSON_Delete(groupSave);
            check(FaultDecoderTestAccess::groups(restored)==0,"group session restored stale readings");
            restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::group(restored,0).rows[0].valid,"group session failed to resolve source");
            whole->vars_[0].structFields.erase(whole->vars_[0].structFields.begin()+1);
            restored.detectGroups(sources);
            restored.refreshGroups(sources);
            check(!FaultDecoderTestAccess::group(restored,0).rows[0].valid && FaultDecoderTestAccess::group(restored,0).rows[0].hasValue,
                  "missing member mistaken for healthy zero");
            sources.clear(); restored.refreshGroups(sources);
            for(unsigned group=0;group<2;++group)
                for(const auto &row:FaultDecoderTestAccess::group(restored,group).rows) check(!row.valid,"closed group source still valid");
            // 验证任意类型对象, 非零状态或计数不视为故障
            auto custom=std::make_shared<Variable>("custom-source");
            sources.emplace("custom-source",custom);
            custom->addLocalVar("protocol",DataType::U32,4); custom->setLocalScalar("protocol",0);
            std::vector<VarEntry::StructField> typed(4);
            const char *typedNames[]={"status","temperature","counter","ready"};
            const DataType typedTypes[]={DataType::I32,DataType::F32,DataType::U64,DataType::U8};
            const unsigned offsets[]={0,4,8,16};
            for(unsigned i=0;i<4;++i) {
                std::strcpy(typed[i].name,typedNames[i]); typed[i].type=typedTypes[i]; typed[i].byteOffset=offsets[i];
            }
            custom->addLocalStructVar("custom.runtime",typed,17);
            auto typedBefore=custom->snapshotLocalBuffers().at("custom.runtime"),typedAfter=typedBefore;
            const int32_t negative=-7; const float temperature=3.125f; const uint64_t count=0x8000000000000001ull;
            std::memcpy(typedAfter.data(),&negative,4); std::memcpy(typedAfter.data()+4,&temperature,4);
            std::memcpy(typedAfter.data()+8,&count,8); typedAfter[16]=1;
            check(custom->compareExchangeLocalBuffer("custom.runtime",typedBefore,typedAfter),"typed object publish");
            restored.load(nullptr); check(restored.loadProfile(profilePath.string()),"custom profile load");
            restored.open=true; restored.detectGroups(sources); restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::groups(restored)==2,"JSON arbitrary object discovery");
            const auto &typedRows=FaultDecoderTestAccess::group(restored,0).rows;
            check(typedRows.size()==4 && typedRows[0].valid && diagnostics::signValue(typedRows[0].val)==-7,
                  "signed value lost");
            check(typedRows[1].val.kind==diagnostics::Kind::Floating && typedRows[1].val.real==3.125,"float read lost");
            check(typedRows[2].val.bits==count && diagnostics::formatValue(typedRows[2].val)=="9223372036854775809",
                  "64-bit counter rounded");
            check(safety::describe(nullptr,typedRows[3].val).findings.empty(),"nonzero ready flag marked as error");
            check(!FaultDecoderTestAccess::group(restored,1).rows[0].valid,"unmatched group condition read");
            custom->setLocalScalar("protocol",1); restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::group(restored,1).rows[0].valid,"matched group condition ignored");
            renderDecoder("motor-safety-system.ppm");
            custom->setLocalScalar("protocol",0); restored.refreshGroups(sources);
            check(!FaultDecoderTestAccess::group(restored,1).rows[0].valid &&
                  FaultDecoderTestAccess::group(restored,1).rows[0].hasValue,"condition change lost stale marker");
            check(FaultDecoderTestAccess::report(restored,1).find("旧值")!=std::string::npos,"report treated stale data as current");
            auto *typedSave=cJSON_CreateObject(); restored.save(typedSave);
            restored.load(typedSave); cJSON_Delete(typedSave);
            check(FaultDecoderTestAccess::groups(restored)==0,"persisted typed samples");
            restored.refreshGroups(sources);
            check(FaultDecoderTestAccess::group(restored,0).rows[0].valid,"typed session binding restore");
            sources.clear(); restored.refreshGroups(sources);
            check(!FaultDecoderTestAccess::group(restored,0).rows[0].valid &&
                  FaultDecoderTestAccess::group(restored,0).rows[0].hasValue,"disconnected typed sample not stale");
            renderDecoder("motor-safety-system-stale.ppm");
            std::cout<<"Safety JSON UI: atomic load, arbitrary roots, i32/f32/u64, normal nonzero values, conditions and stale reports passed\n";
            // 离线验证固件 DWARF 中的指针和数组布局
            if(const char *elfPath=std::getenv("AVA_FOC_DIAGNOSTIC_ELF")) {
                ElfParser elf; check(elf.parse(elfPath),"diagnostic ELF parse");
                auto firmware=std::make_shared<Variable>("firmware");
                VariableRewriteRegression::diagnosticDwarf(*firmware,elf.getDwarfInfo());
                Variable::DiagnosticPath plan;
                for(const auto &spec:motor_faults::groupFields(false)) {
                    bool found=false;
                    for(const auto &suffix:spec.suffixes) found |= firmware->hasDiagnosticWord("g_mc.lo.safety"+suffix);
                    check(found,("missing real safety field: "+spec.title).c_str());
                }
                check(firmware->resolveDiagnosticPath("g_shm.rt_ctrl_data.err_code.err_arr[7]",plan),"NEO shared pointer/array resolution");
                check(plan.pointerOffsets.size()==2 && plan.size==4,"shared pointer was treated as error word");
                const auto neoOffset=plan.pointerOffsets.back();
                check(firmware->resolveDiagnosticPath("g_shm.rt_ctrl_data.err_code.err_arr[0]",plan) && plan.pointerOffsets.back()+28==neoOffset,
                      "Error8 offset incorrect");
                check(!firmware->resolveDiagnosticPath("g_shm.rt_ctrl_data.err_code.err_arr[8]",plan),"array bounds ignored");
                check(!firmware->resolveDiagnosticPath("g_shm.rt_ctrl_data.err_code.err_arr[-1]",plan),"negative array index accepted");
                check(firmware->resolveDiagnosticPath("g_comm_shm_v25_rt.err_code[7]",plan) && plan.pointerOffsets.size()==2,"V25 shared layout");
                sources.emplace("firmware",firmware);
                restored.load(nullptr); check(restored.loadProfile(profilePath.string()),"real ELF fixture profile"); restored.detectGroups(sources);
                check(FaultDecoderTestAccess::group(restored,0).root=="g_mc.lo.safety","real safety auto detection");
                check(FaultDecoderTestAccess::group(restored,1).root=="g_shm.rt_ctrl_data.err_code","JSON shared root not applied");
                if(const char *userProfile=std::getenv("AVA_SAFETY_PROFILE")) {
                    check(restored.loadProfile(userProfile),"user-provided safety profile load");
                    restored.open=true; restored.detectGroups(sources); restored.refreshGroups(sources);
                    size_t expectedGroups=0;
                    for(const auto &definition:FaultDecoderTestAccess::profile(restored).groups)
                        if(!firmware->diagnosticFields(definition.root).empty()) ++expectedGroups;
                    check(expectedGroups>0 && FaultDecoderTestAccess::groups(restored)==expectedGroups,
                          "user JSON did not discover all configured firmware safety objects");
                    renderDecoder("motor-safety-firmware.ppm");
                    std::cout<<"Real firmware ELF: "<<expectedGroups<<" user JSON objects, shared pointer dereference and NEO/V25 conditions passed\n";
                }
            }
            std::cout<<"Whole diagnostics: configured safety fields + 8 shared pages, automatic discovery, persistence and missing/closed sources passed\n";
            std::cout<<"Fault decoder: automatic u32/signed/SDK member reads, per-page bindings, session restore, deleted variables and stale UI passed\n";
        }
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImPlot::DestroyContext(); ImGui::DestroyContext();
        glfwDestroyWindow(window); glfwTerminate();
        std::cout<<"Motor UI: real Monitor feed/pause/clear, Enter confirmation, mode cancel/switch, enable and drag passed\n";
        std::cout<<"Firmware faults: wire layout, aggregate causes, warnings, calibration, unknown/high bits and Chinese UI passed\n";
        std::cout<<"Gauge range persistence/validation/position drag and UTF-8 adapter UI passed\n";
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
