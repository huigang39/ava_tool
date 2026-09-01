#include "gui/hover_panel.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

static void check(bool value, const char *message)
{
        if (!value) throw std::runtime_error(message);
}

struct Fixture {
        ui::HoverReveal reveal;
        ImRect trigger, editor, combo, childChoice, outsideAction;
        ImVec2 size{800, 600};
        bool placeAtEdge{}, requestModal{}, modalOpen{true}, panelDrawn{}, editCommitted{}, outsideActivated{};
        char value[64]{"editable"};
        int selection{};
        ui::interaction::Action parentAction{ui::interaction::Action::None};
        std::string layoutError;

        Fixture()
        {
                ImGui::CreateContext();
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                io.ConfigErrorRecoveryEnableAssert = false;
                io.ConfigErrorRecoveryEnableTooltip = false;
                ImGui::GetCurrentContext()->ErrorCallbackUserData = &layoutError;
                ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext *, void *data, const char *message) {
                        *static_cast<std::string *>(data) = message;
                };
                unsigned char *pixels; int width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                ui::interaction::reset();
                for (int i = 0; i < 3; ++i) frame(i == 0);
        }
        ~Fixture() { ImGui::DestroyContext(); }
        ImGuiWindow *panel() { return ImGui::FindWindowByName("##HoverFixture"); }
        void frame(bool focus = false)
        {
                auto &io = ImGui::GetIO();
                io.DisplaySize = size;
                io.DeltaTime = .05f;
                ui::interaction::beginFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(size);
                if (focus) ImGui::SetNextWindowFocus();
                ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
                ui::interaction::registerRegion("details", false, false, true,
                    [&](ui::interaction::Action action) { parentAction = action; }, [](const std::string &) {});
                if (placeAtEdge) ImGui::SetCursorPos(ImVec2(size.x - 90, size.y - 35));
                const bool clicked = ImGui::Button("Utility", ImVec2(82, 25));
                trigger = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                panelDrawn = ui::BeginHoverPanel("##HoverFixture", "Utility", reveal, clicked, 330, ImGui::GetMainViewport());
                if (panelDrawn) {
                        ImGui::InputText("Draft", value, sizeof(value));
                        editor = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        editCommitted |= ImGui::IsItemDeactivatedAfterEdit();
                        if (ImGui::BeginCombo("Choice", "Select an option", ImGuiComboFlags_HeightLarge)) {
                                for (int i = 0; i < 18; ++i) {
                                        char label[32]; std::snprintf(label, sizeof(label), "Option %d", i);
                                        if (ImGui::Selectable(label, selection == i)) selection = i;
                                        if (i == 8) childChoice = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                                }
                                ImGui::EndCombo();
                        }
                        combo = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        ImGui::TextUnformatted("Auxiliary controls");
                        ImGui::End();
                }
                if (size.x >= 800) {
                        ImGui::SetCursorPos(ImVec2(620, 440));
                        outsideActivated |= ImGui::Button("Outside action");
                        outsideAction = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                }
                if (requestModal) { modalOpen = true; ImGui::OpenPopup("Other modal"); requestModal = false; }
                if (ImGui::BeginPopupModal("Other modal", &modalOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted("Unrelated interaction");
                        ImGui::Button("Keep open");
                        ImGui::EndPopup();
                }
                ImGui::End();
                ui::interaction::process();
                ImGui::Render();
                check(layoutError.empty(), layoutError.c_str());
        }
        void move(ImVec2 point, int frames = 1)
        {
                ImGui::GetIO().AddMousePosEvent(point.x, point.y);
                for (int i = 0; i < frames; ++i) frame();
        }
        void click(ImVec2 point, ImGuiMouseButton button = ImGuiMouseButton_Left)
        {
                move(point);
                ImGui::GetIO().AddMouseButtonEvent(button, true); frame();
                ImGui::GetIO().AddMouseButtonEvent(button, false); frame();
        }
        void key(ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent(key, down); frame(); }
        void escape()
        {
                key(ImGuiKey_Escape, true);
                key(ImGuiKey_Escape, false);
        }
        void clickOpen()
        {
                move(trigger.GetCenter(), 12);
                check(!reveal.isOpen(), "Dwelling never opens a utility panel");
                click(trigger.GetCenter());
                check(reveal.isOpen() && panelDrawn && panel()->Active, "Click opens the real utility panel");
        }
};

int main()
{
        try {
                {
                        Fixture f;
                        f.move(f.trigger.GetCenter(), 4);
                        check(!f.reveal.isOpen(), "Quick pointer pass does not open the utility panel");
                        f.move(ImVec2(700, 500), 2);
                        f.clickOpen();
                        check(ImGui::GetCurrentContext()->NavWindow == f.panel(), "An explicit click focuses the utility panel");
                        const ImVec2 gap(f.trigger.GetCenter().x, f.trigger.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f);
                        f.move(gap, 4);
                        check(f.panelDrawn, "Pointer can cross the trigger-to-panel gap");
                        f.move(ImVec2(f.panel()->Pos.x + 10, f.panel()->Pos.y + 10), 12);
                        check(f.panelDrawn, "Pointer over panel keeps preview open");
                        f.move(ImVec2(700, 500), 12);
                        check(f.reveal.isOpen() && f.panelDrawn, "Click-open panel stays visible away from the pointer");
                        f.click(f.trigger.GetCenter());
                        check(!f.reveal.isOpen(), "A second trigger click closes the panel");
                        f.clickOpen();
                        f.click(ImVec2(700, 500));
                        check(!f.reveal.isOpen() && !f.panelDrawn, "Clicking outside closes the utility panel");
                        f.clickOpen();
                        f.click(ImVec2(700, 500), ImGuiMouseButton_Right);
                        check(!f.reveal.isOpen() && !f.panelDrawn, "An outside right click also dismisses the panel");
                        f.clickOpen();
                        f.click(f.outsideAction.GetCenter());
                        check(!f.reveal.isOpen() && f.outsideActivated,
                              "Outside controls still receive the click that dismisses the panel");
                        f.clickOpen();
                        const ImVec2 panelBefore = f.panel()->Pos;
                        const ImVec2 dragStart(f.panel()->Pos.x + f.panel()->WindowPadding.x + 20,
                                               f.panel()->Pos.y + f.panel()->WindowPadding.y +
                                                   ImGui::GetFrameHeight() * .5f);
                        f.move(dragStart);
                        ImGui::GetIO().AddMouseButtonEvent(0, true);
                        f.frame();
                        ImGui::GetIO().AddMousePosEvent(dragStart.x + 70, dragStart.y + 40);
                        f.frame();
                        ImGui::GetIO().AddMouseButtonEvent(0, false);
                        f.frame();
                        check(f.panel()->Pos.x > panelBefore.x + 60 && f.panel()->Pos.y > panelBefore.y + 30,
                              "Pinned utility panel moves freely by its title");
                        const ImVec2 resized(f.panel()->Size.x + 90, f.panel()->Size.y + 70);
                        ImGui::SetWindowSize(f.panel(), resized);
                        f.frame();
                        check(std::abs(f.panel()->Size.x - resized.x) < .5f &&
                                  std::abs(f.panel()->Size.y - resized.y) < .5f,
                              "Pinned utility panel keeps a manually resized width and height");
                        f.click(ImVec2(f.panel()->Pos.x + 30, f.panel()->Pos.y + f.panel()->Size.y - 25));
                        check(f.reveal.isOpen() && f.panelDrawn, "Clicking blank space inside the panel keeps it open");
                        const ImVec2 reduced(280, 150);
                        ImGui::SetWindowSize(f.panel(), reduced);
                        f.frame();
                        check(std::abs(f.panel()->Size.x - reduced.x) < .5f &&
                                  std::abs(f.panel()->Size.y - reduced.y) < .5f,
                              "Utility panel can also be manually reduced in both directions");
                        const ImVec2 beforeResize = f.panel()->Size;
                        const ImVec2 corner(f.panel()->Pos.x + beforeResize.x - 3, f.panel()->Pos.y + beforeResize.y - 3);
                        f.move(corner, 2);
                        ImGui::GetIO().AddMouseButtonEvent(0, true); f.frame();
                        ImGui::GetIO().AddMousePosEvent(corner.x + 60, corner.y + 45); f.frame();
                        ImGui::GetIO().AddMouseButtonEvent(0, false); f.frame();
                        check(f.panel()->Size.x > beforeResize.x + 50 && f.panel()->Size.y > beforeResize.y + 35,
                              "Native corner resizes both dimensions");
                        f.move(ImVec2(700, 500), 14);
                        check(f.panelDrawn, "Pinned utility remains open away from the pointer");
                }
                {
                        Fixture f;
                        f.clickOpen();
                        f.click(f.editor.GetCenter());
                        check(ImGui::GetActiveID() != 0, "Text field becomes active");
                        f.move(ImVec2(700, 500), 14);
                        check(f.panelDrawn && f.reveal.isOpen(), "Active input stays visible while pointer is away");
                        f.parentAction = ui::interaction::Action::None;
                        f.key(ImGuiKey_Escape, true);
                        check(f.reveal.isOpen() && !ui::interaction::escapeConsumed &&
                              f.parentAction == ui::interaction::Action::None,
                              "First Escape cancels the editor without closing panel or navigating parent");
                        f.key(ImGuiKey_Escape, false);
                        f.move(f.trigger.GetCenter(), 2);
                        f.key(ImGuiKey_Escape, true);
                        check(!f.reveal.isOpen() && ui::interaction::escapeConsumed &&
                              f.parentAction == ui::interaction::Action::None,
                              "Plain Escape closes preview and consumes parent navigation");
                        f.key(ImGuiKey_Escape, false);
                        f.move(f.trigger.GetCenter(), 14);
                        check(!f.reveal.isOpen(), "Dismissed preview does not reopen under a stationary pointer");
                        f.move(ImVec2(700, 500), 2);
                        f.clickOpen();
                }
                {
                        Fixture f;
                        f.clickOpen();
                        f.click(f.editor.GetCenter());
                        ImGui::GetIO().AddInputCharactersUTF8("x"); f.frame();
                        const std::string edited = f.value;
                        check(edited != "editable", "Typing changes the panel's text field");
                        f.click(ImVec2(700, 500));
                        check(!f.reveal.isOpen() && !f.panelDrawn && f.editCommitted && edited == f.value,
                              "Outside click commits an active edit before the panel disappears");
                }
                {
                        Fixture f;
                        f.clickOpen();
                        f.click(f.combo.GetCenter());
                        f.frame();
                        check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1, "Combo opens a child popup");
                        auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                        check(popup && popup->Pos.y + popup->Size.y > f.panel()->Pos.y + f.panel()->Size.y,
                              "Combo popup extends beyond the hover panel");
                        f.move(ImVec2(popup->Pos.x + 15, popup->Pos.y + popup->Size.y - 15), 14);
                        check(f.reveal.isOpen() && f.panelDrawn, "Pointer in an outlying child popup keeps the preview open");
                        f.parentAction = ui::interaction::Action::None;
                        f.key(ImGuiKey_Escape, true);
                        check(f.reveal.isOpen() && !ui::interaction::escapeConsumed &&
                              f.parentAction == ui::interaction::Action::None,
                              "Escape dismisses child popup before preview or parent");
                        f.key(ImGuiKey_Escape, false);
                        f.click(f.combo.GetCenter()); f.frame();
                        check(!f.panel()->Rect().Contains(f.childChoice.GetCenter()),
                              "Child choice lies outside the parent panel bounds");
                        f.click(f.childChoice.GetCenter());
                        check(f.selection == 8 && f.reveal.isOpen() && f.panelDrawn,
                              "Selecting an outlying child menu item does not dismiss its parent panel");
                        f.click(f.combo.GetCenter()); f.frame();
                        f.click(ImVec2(700, 500)); f.frame();
                        check(!f.reveal.isOpen() && !f.panelDrawn && ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                              "Clicking outside the panel and its child menu closes both");
                }
                {
                        Fixture f;
                        f.requestModal = true; f.frame(); f.frame();
                        check(ImGui::GetTopMostPopupModal() != nullptr, "Unrelated modal fixture opens");
                        f.move(f.trigger.GetCenter(), 14);
                        check(!f.reveal.isOpen() && !f.panelDrawn, "An unrelated modal blocks hover expansion");
                        f.reveal.setPinned(true);
                        f.frame();
                        check(!f.panelDrawn, "Pinned panel does not draw over an unrelated modal");
                }
                {
                        Fixture f;
                        f.size = ImVec2(260, 200); f.placeAtEdge = true;
                        f.reveal.setPinned(true);
                        for (int i = 0; i < 5; ++i) f.frame();
                        const auto *panel = f.panel();
                        check(panel && panel->Active && panel->Pos.x >= 0 && panel->Pos.y >= 0 &&
                              panel->Pos.x + panel->Size.x <= 260.5f && panel->Pos.y + panel->Size.y <= 200.5f,
                              "Utility panel stays within a constrained viewport near its bottom-right edge");
                }
                std::puts("PASS: click-only opening, outside dismissal, edit commit, child menus, free drag, native resizing, Escape, modal "
                          "and viewport bounds");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
