#include "gui/interaction_history.hpp"
#include <cstdio>
#include <stdexcept>
using namespace ui::interaction;
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static std::string route = "list", pendingRoute;
static Action received = Action::None;
static bool input = false, popup = false;
static char buffer[64] = "draft";
static void frame(bool focus = false)
{
        beginFrame();
        ImGui::NewFrame();
        if (!pendingRoute.empty()) { route = pendingRoute; pendingRoute.clear(); }
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(420, 350));
        if (focus) ImGui::SetNextWindowFocus();
        ImGui::Begin("Editor");
        registerRegion(route, true, true, route != "list",
                       [](Action action) { received = action; },
                       [](const std::string &target) { pendingRoute = target; });
        if (input) { ImGui::SetKeyboardFocusHere(); ImGui::InputText("value", buffer, sizeof(buffer)); }
        if (popup) ImGui::OpenPopup("menu");
        if (ImGui::BeginPopup("menu")) { ImGui::TextUnformatted("Options"); ImGui::EndPopup(); }
        ImGui::BeginChild("scroll", ImVec2(0, 160));
        for (int i = 0; i < 60; ++i) ImGui::Text("row %d", i);
        ImGui::EndChild();
        ImGui::End();
        process();
        ImGui::Render();
}
int main()
{
        UndoHistory<int> history;
        history.record(0, 1); history.record(1, 2);
        check(*history.undo() == 1 && *history.undo() == 0 && !history.undo(), "Undo stops at the boundary");
        check(*history.redo() == 1, "Redo restores the next edit");
        history.record(1, 3);
        check(!history.canRedo() && *history.undo() == 1, "New edit after undo discards only the redo branch");
        history.clear(); check(!history.canUndo(), "Boundary clears old edits");
        ImGui::CreateContext();
        auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1000, 700);
        unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        reset(); frame(true); frame();
        ImGuiWindow *scroll = nullptr;
        // Resolve the child by hierarchy rather than depending on generated IDs.
        for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->ParentWindow && window->ParentWindow->ID == ImGui::FindWindowByName("Editor")->ID) scroll = window;
        check(scroll != nullptr, "Scrollable view exists");
        ImGui::SetScrollY(scroll, 100); frame(); frame();
        route = "details"; frame(); frame();
        requested = Action::Back; frame(); frame(); frame();
        check(route == "list" && canForward() && scroll->Scroll.y >= 99, "Back restores route and scroll");
        requested = Action::Forward; frame(); frame();
        check(route == "details", "Forward restores detail view");
        io.AddKeyEvent(ImGuiMod_Alt, true); io.AddKeyEvent(ImGuiKey_LeftArrow, true); frame();
        io.AddKeyEvent(ImGuiKey_LeftArrow, false); io.AddKeyEvent(ImGuiMod_Alt, false); frame(); frame();
        check(route == "list", "Alt+Left navigates back");
        io.AddMouseButtonEvent(4, true); frame(); io.AddMouseButtonEvent(4, false); frame(); frame();
        check(route == "details", "Mouse forward restores the next view");
        io.AddMouseButtonEvent(3, true); frame(); io.AddMouseButtonEvent(3, false); frame(); frame();
        check(route == "list", "Mouse back restores the previous view");
        io.AddKeyEvent(ImGuiMod_Alt, true); io.AddKeyEvent(ImGuiKey_RightArrow, true); frame();
        io.AddKeyEvent(ImGuiKey_RightArrow, false); io.AddKeyEvent(ImGuiMod_Alt, false); frame(); frame();
        check(route == "details", "Alt+Right navigates forward");
        io.AddKeyEvent(ImGuiMod_Alt, true); io.AddKeyEvent(ImGuiKey_UpArrow, true); frame();
        check(received == Action::Parent, "Alt+Up requests the current view's parent");
        io.AddKeyEvent(ImGuiKey_UpArrow, false); io.AddKeyEvent(ImGuiMod_Alt, false); frame();
        received = Action::None;
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_Z, true); frame();
        check(received == Action::Undo, "Ctrl+Z routes to focused panel");
        io.AddKeyEvent(ImGuiKey_Z, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
        received = Action::None; input = true; frame(); frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_Z, true); frame();
        check(received == Action::None, "Text editing keeps its own undo");
        io.AddKeyEvent(ImGuiKey_Z, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); input = false;
        ImGui::ClearActiveID(); frame(); frame();
        popup = true; frame(); popup = false; frame();
        io.AddKeyEvent(ImGuiKey_Escape, true); frame();
        check(received == Action::None, "Esc closing a popup cannot also leave the view");
        io.AddKeyEvent(ImGuiKey_Escape, false); frame(); frame();
        io.AddKeyEvent(ImGuiKey_Escape, true); frame();
        check(received == Action::Escape, "Esc outside editors and popups leaves one view level");
        reset(); ImGui::DestroyContext();
        puts("PASS: bounded undo/redo, navigation routes and scroll, local edit precedence, single-level Esc");
}
