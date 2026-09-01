#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <stdexcept>
static void check(bool ok,const char *text){if(!ok)throw std::runtime_error(text);}
int main(int argc,char **argv){
 if(argc!=2)return 2;module_init();
 try{
  check(glfwInit(),"GLFW");glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
  std::ofstream(argv[1])<<"{\"monitors\":[],\"variables\":[]}";
  auto gui=std::make_unique<Gui>(argv[1]);auto &io=ImGui::GetIO();
  io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;io.ConfigInputTrickleEventQueue=false;io.IniFilename=nullptr;
  io.MouseDoubleClickTime=0.0f;                          // Exercise separate single clicks regardless of test execution speed
  char text[64]="example",hint[64]="firmware.elf",multi[64]="first\nsecond",readonly[64]="readonly",automatic[64]="replace me";int number=12345;
  ImVec2 positions[6];ImGuiID ids[6];
  auto frame=[&](ImVec2 mouse=ImVec2(-100,-100),bool down=false){
   ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();io.AddMousePosEvent(mouse.x,mouse.y);io.AddMouseButtonEvent(0,down);ImGui::NewFrame();
   ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(600,500));ImGui::Begin("Inputs");
   auto remember=[&](int i){auto a=ImGui::GetItemRectMin();positions[i]=ImVec2(a.x+25,a.y+8);ids[i]=ImGui::GetItemID();};
   ImGui::InputText("Text",text,sizeof(text));remember(0);
   ImGui::InputTextWithHint("Hint","Path",hint,sizeof(hint));remember(1);
   ImGui::InputInt("Number",&number,0,0);remember(2);
   ImGui::InputTextMultiline("Multiline",multi,sizeof(multi),ImVec2(300,80));remember(3);
   ImGui::InputText("Read only",readonly,sizeof(readonly),ImGuiInputTextFlags_ReadOnly);remember(4);
   ImGui::InputText("Auto select",automatic,sizeof(automatic),ImGuiInputTextFlags_AutoSelectAll);remember(5);
   ImGui::End();ImGui::Render();ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  };
  frame();frame();
  auto allSelected=[&](int i){auto *state=ImGui::GetInputTextState(ids[i]);return state && state->HasSelection() && std::min(state->GetSelectionStart(),state->GetSelectionEnd())==0 && std::max(state->GetSelectionStart(),state->GetSelectionEnd())==state->TextLen;};
  for(int i=0;i<6;++i){
   frame(positions[i],true);
   if(i==2 || i==5)check(allSelected(i),"numeric and explicit AutoSelectAll inputs select on activation");
   else check(ImGui::GetInputTextState(ids[i]) && !ImGui::GetInputTextState(ids[i])->HasSelection(),"single click positions the cursor without selecting text");
   frame(positions[i],false);frame(positions[i],true);
   check(ImGui::GetInputTextState(ids[i]) && !ImGui::GetInputTextState(ids[i])->HasSelection(),"repeated single click positions the cursor even in auto-select inputs");
   frame(positions[i],false);
  }
  frame(positions[0],true);frame(positions[0],false);
  auto expected=std::string(text);expected.insert(ImGui::GetInputTextState(ids[0])->GetCursorPos(),"new");
  io.AddInputCharactersUTF8("new");frame();check(std::string(text)==expected,"typing inserts at the clicked cursor position");
  io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame();check(allSelected(0),"Ctrl+A still selects the complete input");
  io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();
  io.AddInputCharactersUTF8("new");frame();check(std::string(text)=="new","typing replaces explicitly selected text");
  frame(positions[2],true);frame(positions[2],false);io.AddInputCharactersUTF8("7");frame();check(number==7,"typing replaces selected numeric value");
  gui.reset();puts("PASS: single clicks position the cursor; explicit auto-select, numeric replacement and Ctrl+A still work");return 0;
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
