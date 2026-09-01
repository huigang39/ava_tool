#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/flash_tool.hpp"
#include "core/flash_erase.hpp"
#include "gui/remote_files.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>
static void check(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
struct FlashToolTestAccess {
    static void setup(FlashTool &tool) {
        std::vector<uint8_t> bytes(4097);
        for (size_t i=0;i<bytes.size();++i) bytes[i]=uint8_t(i);
        tool.readSnapshot_=flasharray::decode(std::move(bytes),0x08000000,flasharray::Type::U8,false,1,"FlashArray");
    }
    static void draw(FlashTool &tool) { tool.drawReadFlash(); }
    static void erase(FlashTool &tool) { tool.drawEraseFlash(); }
    static void tail(FlashTool &tool) { tool.hexSelection_=4096; }
};
int main(int argc,char **argv) {
    if(argc!=2)return 2; module_init();
    try {
        const auto sectors = flasherase::sectors("STM32H745II_M7");
        check(sectors.size() == 16 && sectors.front().address == 0x08000000 && sectors.back().address + sectors.back().size == 0x08200000,
              "H745II layout covers exactly 2 MiB");
        for(size_t i=0;i<sectors.size();++i) {
            check(sectors[i].size == 128*1024 && sectors[i].bank == i/8+1 && sectors[i].index == i%8,"sector bank/index and size");
            if(i)check(sectors[i-1].address + sectors[i-1].size == sectors[i].address,"no sector overlaps or gaps");
        }
        check(flasherase::sectors("STM32H745IG_M7").empty() && flasherase::sectors("STM32H7B3II").empty(),"other flash layouts are not guessed");
        const auto selective = flasherase::script("STM32H745II_M7",false,(1u<<1)|(1u<<15));
        check(selective.find("Erase 0x08020000 0x0803FFFF noreset")!=std::string::npos &&
              selective.find("Erase 0x081E0000 0x081FFFFF noreset")!=std::string::npos &&
              selective.find("Erase\n")==std::string::npos && selective.find("Erase 0x08000000")==std::string::npos,
              "selective erase never expands to whole chip or adjacent sectors");
        check(selective.find("r\nh\n")!=std::string::npos && selective.find("SetRestartOnClose = 0")!=std::string::npos,
              "erase prepares and leaves the target halted");
        check(flasherase::script("STM32H745II_M7",true,0).find("Erase\n")!=std::string::npos,"whole-chip erase works without a sector selection");
        for(uint32_t mask : {0u,0x10000u}) { bool rejected=false;try { flasherase::script("STM32H745II_M7",false,mask); } catch(const std::invalid_argument &) { rejected=true; }check(rejected,"invalid erase selections are rejected"); }
        std::string error, log;
        check(!JLinkPort::instance().eraseFlash("STM32H745II_M7",0,false,1,1000,error,log) && !error.empty() && !JLinkPort::instance().isBusy(),
              "disconnected erase fails before launching Commander and releases busy state");
        check(glfwInit(),"GLFW"); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        std::ofstream(argv[1]) << "{\"monitors\":[],\"variables\":[]}";
        auto gui=std::make_unique<Gui>(argv[1]);
        auto &io=ImGui::GetIO(); io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; io.IniFilename=nullptr;
        auto window=glfwGetCurrentContext();glfwSetWindowSize(window,1200,900);
        FlashTool tool;FlashToolTestAccess::setup(tool);
        bool showErase = false;
        auto frame=[&](bool settings=false,LocalFileServer *server=nullptr) {
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(1200,900));
            ImGui::Begin("Flash Read");
            if(settings)drawRemoteFileSettings(*server,std::string(argv[1])+".server.json");
            else if(showErase) FlashToolTestAccess::erase(tool);
            else FlashToolTestAccess::draw(tool);
            ImGui::End();ImGui::Render();glViewport(0,0,1200,900);glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());glFinish();
        };
        frame();frame();
        std::vector<unsigned char> pixels(1200*900*3);glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,1200,900,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream out(std::string(argv[1])+".ppm",std::ios::binary);out<<"P6\n1200 900\n255\n";
        for(int row=899;row>=0;--row)out.write(reinterpret_cast<char*>(pixels.data()+row*1200*3),1200*3);out.close();
        FlashToolTestAccess::tail(tool);frame();
        JLinkPort::instance().setDeviceName("STM32H745II_M7");showErase=true;frame();frame();
        glReadPixels(0,0,1200,900,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream eraseImage(std::string(argv[1])+".erase.ppm",std::ios::binary);eraseImage<<"P6\n1200 900\n255\n";
        for(int row=899;row>=0;--row)eraseImage.write(reinterpret_cast<char*>(pixels.data()+row*1200*3),1200*3);eraseImage.close();
        showErase=false;

        LocalFileServer server;auto root=std::filesystem::path(argv[1]).parent_path()/"flash-read-share";
        std::filesystem::create_directories(root/"Firmware");std::filesystem::create_directories(root/"Sessions");std::filesystem::create_directories(root/"Updates");
        std::filesystem::create_directories(root/"Other");
        server.config.folder=root.string();server.config.port=0;server.config.firmwareFolder="Firmware";server.config.sessionsFolder="Sessions";server.config.updatesFolder="Updates";check(server.start(),"server starts");
        frame(true,&server);auto first=remote::settings().firmwareUrl;
        check(first.find("/Firmware/")!=std::string::npos,"shared firmware address fills automatically");
        server.config.firmwareFolder="Other";frame(true,&server);
        check(remote::settings().firmwareUrl!=first && remote::settings().firmwareUrl.ends_with("/Other/"),"shared subfolder changes sync automatically");
        server.stop();gui.reset();puts("PASS: Flash viewer, H745II sector bounds, scoped erase scripts, disconnected rejection, erase panel, automatic shared addresses");return 0;
    }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
