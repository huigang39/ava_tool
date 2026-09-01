#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/bode.hpp"
#include "imgui_internal.h"
#include "implot_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>

static void check(bool val, const char *message) { if (!val) throw std::runtime_error(message); }
struct BodeTestAccess {
    static void load(Bode &b, std::vector<bode_analysis::Point> points) {
        b.bodeData_ = std::move(points); b.bodeFreqsV_.clear(); b.bodeMagsV_.clear(); b.bodePhsV_.clear();
        for (const auto &p : b.bodeData_) { b.bodeFreqsV_.push_back(p.freq); b.bodeMagsV_.push_back(p.magDb); b.bodePhsV_.push_back(p.phaseDeg); }
        b.bodeFitPending_ = true; b.bodeAnalysisDirty_ = true; b.bodeMeasureCount_ = 0;
    }
    static bool measuring(const Bode &b) { return b.bodeMeasureEnabled_; }
    static int count(const Bode &b) { return b.bodeMeasureCount_; }
    static double cursor(const Bode &b, int i) { return b.bodeMeasureFrequency_[i]; }
    static bool fitted(const Bode &b) { return !b.bodeFitPending_; }
    static void mode(Bode &b, int mode) { b.bodeAnalysisMode_ = mode; }
    static const auto &analysis(const Bode &b) { return b.bodeAnalysis_; }
};
struct BodeUi {
    Bode &b;
    void frame(ImVec2 mouse = ImVec2(-1000,-1000), bool down = false) {
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
        auto &io = ImGui::GetIO(); io.DeltaTime = 1.0f/60;
        io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);
        ImGui::NewFrame(); b.updateDisplay({}); ImGui::Render();
        glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    ImPlotPlot *plot(int index) { return ImPlot::GetCurrentContext()->Plots.GetByIndex(index); }
    ImVec2 point(int index, double frequency, double val) {
        auto *p = plot(index);
        return ImVec2(p->Axes[ImAxis_X1].PlotToPixels(frequency), p->Axes[ImAxis_Y1].PlotToPixels(val));
    }
    void clickPoint(ImVec2 p) { frame(p); frame(p); frame(p,true); frame(p); frame(); }
    void click(const char *label) {
        auto *w = ImGui::FindWindowByName("###BodePlot");
        check(w, "Bode window exists");
        ImGui::SetFocusID(w->GetID(label),w); w->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000,-1000,-999,-999);
        frame(); auto r = ImGui::WindowRectRelToAbs(w,w->NavRectRel[ImGuiNavLayer_Main]);
        check(r.Min.x>=0 && r.Min.y>=0, "Bode toolbar control exists"); clickPoint(r.GetCenter());
    }
    void screenshot(const std::string &path) {
        int width,height; glfwGetFramebufferSize(glfwGetCurrentContext(),&width,&height);
        std::vector<unsigned char> pixels(width*height*3);
        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream f(path,std::ios::binary); f<<"P6\n"<<width<<' '<<height<<"\n255\n";
        for (int y=height-1;y>=0;--y) f.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
    }
};
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    module_init(); int result=0; std::unique_ptr<Gui> gui;
    try {
        std::ofstream(argv[1])<<"{\"monitors\":[],\"variables\":[]}";
        check(glfwInit()==GLFW_TRUE,"GLFW initialization"); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        gui=std::make_unique<Gui>(argv[1]);
        g_lang=Lang::ZH; auto &io=ImGui::GetIO(); io.ConfigFlags&=~ImGuiConfigFlags_ViewportsEnable;
        io.ConfigInputTrickleEventQueue=false; io.IniFilename=nullptr; io.MouseDoubleClickTime=.001f;
        Bode b; auto *settings=cJSON_Parse(R"({"bode":{"mode":2}})"); b.loadSession(settings); cJSON_Delete(settings); b.show_=true;
        BodeTestAccess::load(b,{{1,20,-20},{10,10,-90},{100,0,-150},{1000,-20,-230}});
        BodeUi ui{b}; ui.frame(); ImGui::SetWindowPos("###BodePlot",ImVec2(0,0)); ImGui::SetWindowSize("###BodePlot",ImVec2(1000,680)); ui.frame(); ui.frame();
        check(BodeTestAccess::fitted(b),"both Bode plots consume one-shot fit");
        for (int i=0;i<2;++i) {
            auto *p=ui.plot(i); check(p->Axes[ImAxis_X1].Range.Min<=1 && p->Axes[ImAxis_X1].Range.Max>=1000,"fit displays all response frequencies");
        }
        const auto hasHoverLine = [&](int index, double frequency) {
            auto *p=ui.plot(index); const float x=p->Axes[ImAxis_X1].PlotToPixels(frequency);
            const auto color=ImGui::GetColorU32(ImGuiCol_TextDisabled);
            bool top=false,bottom=false;
            for (const auto &v:ImGui::FindWindowByName("###BodePlot")->DrawList->VtxBuffer) {
                if(v.col!=color || std::abs(v.pos.x-x)>2) continue;
                top |= std::abs(v.pos.y-p->PlotRect.Min.y)<2;
                bottom |= std::abs(v.pos.y-p->PlotRect.Max.y)<2;
            }
            return top&&bottom;
        };
        check(!BodeTestAccess::measuring(b),"hover measurement requires no A/B mode");
        for(int i=0;i<2;++i) {
            auto mouse=ui.point(i,10,i?-90:10); ui.frame(mouse); ui.frame(mouse); ui.frame(mouse);
            check(hasHoverLine(0,10)&&hasHoverLine(1,10),"default hover cursor links magnitude and phase in either direction");
            check(BodeTestAccess::count(b)==0,"hover does not create A/B cursors");
            auto *tooltip=ImGui::FindWindowByName("##Tooltip_00");
            check(tooltip&&tooltip->Active&&!tooltip->Hidden&&tooltip->DrawList->VtxBuffer.Size>0,"default cursor displays the response readout");
        }
        ui.screenshot(std::string(argv[1])+".hover.ppm");
        ui.frame();
        check(!hasHoverLine(0,10)&&!hasHoverLine(1,10),"hover cursors disappear outside the plots");
        BodeTestAccess::load(b,{{1,20,-20},{10,10,-90},{100,0,-150}});ui.frame();ui.frame();
        const auto &ticks=ui.plot(1)->Axes[ImAxis_X1].Ticker;
        for(double frequency:{2.,5.,20.,50.}) {
            bool found=false;for(const auto &tick:ticks.Ticks) found|=tick.ShowLabel&&std::abs(tick.PlotPos-frequency)<1e-8;
            check(found,"frequency axis labels intermediate logarithmic ticks");
        }
        ui.screenshot(std::string(argv[1])+".frequency-ticks.ppm");
        BodeTestAccess::load(b,{{1,20,-20},{10,10,-90},{100,0,-150},{1000,-20,-230}});ui.frame();ui.frame();
        ui.click("测量##bodeMeasure"); check(BodeTestAccess::measuring(b),"measurement checkbox works");
        const auto xBefore=ui.plot(0)->Axes[ImAxis_X1].Range;
        ui.clickPoint(ui.point(0,10,10));
        check(BodeTestAccess::count(b)==1 && BodeTestAccess::cursor(b,0)==10,"magnitude click snaps A to a response point");
        ui.clickPoint(ui.point(1,100,-150));
        check(BodeTestAccess::count(b)==2 && BodeTestAccess::cursor(b,1)==100,"phase click sets shared B cursor");
        check(ui.plot(0)->Axes[ImAxis_X1].Range.Min==xBefore.Min && ui.plot(0)->Axes[ImAxis_X1].Range.Max==xBefore.Max,"measurement clicks do not pan or fit plots");
        check(ImPlot::GetInputMap().Pan==ImGuiMouseButton_Left,"Bode restores the global plot input map");
        auto from=ui.point(0,10,10),to=ui.point(0,1,10); ui.frame(from);ui.frame(from);ui.frame(from,true);ui.frame(to,true);ui.frame(to);ui.frame();
        check(BodeTestAccess::cursor(b,0)==1,"dragging A in magnitude plot updates the shared frequency cursor");
        ui.screenshot(std::string(argv[1])+".measure.ppm");
        ui.click("清除测量##bodeClearMeasure"); check(BodeTestAccess::count(b)==0,"clear resets A/B");
        BodeTestAccess::load(b,{{.1,20,-90},{10,0,-150},{1000,-20,-210}}); ui.frame();ui.frame();
        check(ui.plot(0)->Axes[ImAxis_X1].Range.Min<=.1 && ui.plot(0)->Axes[ImAxis_X1].Range.Max>=1000,"new read fits expanded data range");
        ui.click("自动分析##bodeAnalysis");
        check(BodeTestAccess::analysis(b).phaseMargins.size()==1 && BodeTestAccess::analysis(b).gainMargins.size()==1,"open-loop popup automatically analyzes response data");
        ui.screenshot(std::string(argv[1])+".open-loop.ppm");
        BodeTestAccess::mode(b,1); ui.frame();ui.frame();
        check(BodeTestAccess::analysis(b).bandwidth.has_value(),"closed-loop popup provides measured bandwidth");
        ui.screenshot(std::string(argv[1])+".closed-loop.ppm");
        auto *saved=cJSON_CreateObject();b.saveSession(saved);
        check(cJSON_GetObjectItem(cJSON_GetObjectItem(saved,"bode"),"analysisMode")->valueint==1,"analysis mode persists in sessions");cJSON_Delete(saved);
        ImGui::ClosePopupsOverWindow(nullptr,false);
        ImGui::FocusWindow(ImGui::FindWindowByName("###BodePlot")); ui.frame();
        ui.click("###bodeVariableSettings"); ui.frame();
        ui.screenshot(std::string(argv[1])+".collapsed.ppm");
        ImGui::SetWindowSize("###BodePlot",ImVec2(480,680)); ui.frame();ui.frame();
        ui.screenshot(std::string(argv[1])+".compact.ppm");
        ui.click("###bodeVariableSettings");ui.frame();
        ui.screenshot(std::string(argv[1])+".compact-settings.ppm");
        puts("PASS: Bode full-range fit, shared A/B cursors, sample snapping, drag/clear, input-map isolation, automatic open/closed-loop analysis and persistence");
    } catch(const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what());result=1; }
    gui.reset();JLinkPort::instance().close();log_deinit(&g_log_ops);log_deinit(&g_log_perf);return result;
}
