#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct VariableRewriteRegression {
    static void initialize(Variable &v) {
        VarEntry parent{};
        parent.name = "parent"; parent.addr = 0x20000000; parent.type = DataType::U32;
        parent.port = PortType::JLINK; parent.writable = true; parent.isStruct = true;
        v.vars_.push_back(parent);
    }
    static void openMember(Variable &v) { v.beginMemberProperties(0, "parent.child", 0x20000004, DataType::F32, true, true); }
    static void editMember(Variable &v) {
        snprintf(v.editPropBuf_.name, sizeof(v.editPropBuf_.name), "renamed child");
        snprintf(v.editPropBuf_.addrBuf, sizeof(v.editPropBuf_.addrBuf), "20000010");
        v.editPropBuf_.type = DataType::I16; v.editPropBuf_.writable = false;
    }
    static void openRoot(Variable &v) {
        v.editPropIdx_ = 0; v.editPropBuf_ = {};
        snprintf(v.editPropBuf_.name, sizeof(v.editPropBuf_.name), "renamed parent");
        snprintf(v.editPropBuf_.addrBuf, sizeof(v.editPropBuf_.addrBuf), "20000100");
        v.editPropBuf_.type = DataType::U32; v.editPropBuf_.port = PortType::JLINK; v.editPropBuf_.writable = true;
    }
    static void draw(Variable &v) { v.drawEditPropertiesPopup(); }
    static auto &parent(Variable &v) { return v.vars_[0]; }
    static bool open(Variable &v) { return v.editPropIdx_ >= 0; }
};
int main(int, char **) {
    module_init();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 720); io.DeltaTime = 1.0f / 60;
    unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int result = 0;
    try {
        Variable variable("property-test"); VariableRewriteRegression::initialize(variable);
        auto frame = [&](ImVec2 pos = ImVec2(-1000, -1000), bool down = false) {
            io.AddMousePosEvent(pos.x, pos.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame(); ImGui::Begin("Property test host");
            VariableRewriteRegression::draw(variable); ImGui::End(); ImGui::Render();
        };
        auto popup = [&] { return ImGui::FindWindowByName(tr("Edit Variable Properties###EditVarProperties", "编辑变量属性###EditVarProperties")); };
        auto clickButton = [&](int index) {
            auto *window = popup(); check(window && window->Active, "shared property modal is visible");
            ImVec2 pos(window->Pos.x + ImGui::GetStyle().WindowPadding.x + index * (100 + ImGui::GetStyle().ItemSpacing.x) + 50,
                       window->Pos.y + window->Size.y - ImGui::GetStyle().WindowPadding.y -
                       (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2) * 0.5f);
            frame(pos); frame(pos, true); frame(pos); frame();
        };
        VariableRewriteRegression::openMember(variable);
        VariableRewriteRegression::editMember(variable);
        for (int i = 0; i < 3; ++i) frame();
        auto *sharedPopup = popup();
        check(sharedPopup && !ImGui::FindWindowByName("Edit Member Properties###EditMemberProperties"), "member uses ordinary variable property popup");
        clickButton(2);
        check(!VariableRewriteRegression::open(variable) && VariableRewriteRegression::parent(variable).memberOverrides.empty(), "cancel leaves member and parent untouched");
        VariableRewriteRegression::openMember(variable); VariableRewriteRegression::editMember(variable);
        for (int i = 0; i < 3; ++i) frame(); clickButton(0);
        auto &parent = VariableRewriteRegression::parent(variable);
        const auto &member = parent.memberOverrides.at("parent.child");
        check(member.alias == "renamed child" && member.address == 0x20000010 && member.type == DataType::I16 && !member.writable,
              "apply edits child name/address/type/access using unified controls");
        check(parent.name == "parent" && parent.addr == 0x20000000 && parent.type == DataType::U32 && parent.writable && parent.isStruct,
              "child apply preserves parent identity, layout and access");
        VariableRewriteRegression::openMember(variable); for (int i = 0; i < 3; ++i) frame(); clickButton(1);
        check(parent.memberOverrides.empty(), "reset removes child overrides");
        VariableRewriteRegression::openRoot(variable); for (int i = 0; i < 3; ++i) frame();
        check(popup() == sharedPopup, "root and child literally share one modal"); clickButton(0);
        check(parent.name == "renamed parent" && parent.addr == 0x20000100 && !parent.isStruct, "ordinary property editing still applies correctly");
        puts("PASS: one shared variable/member modal, cancel, child apply without parent mutation, reset and root apply");
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
    ImGui::DestroyContext(); log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
