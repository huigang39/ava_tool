#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include "gui/dock_close.hpp"
#include "gui/scope_layout.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <fstream>
#include <limits>
#include <stdexcept>

struct SimSamplerTestAccess {
    static void save(Gui &gui, const std::string &path) { gui.saveSession(path); }
    static void load(Gui &gui, const std::string &path) { gui.loadSession(path); }
    static void afterWindows(Gui &gui) { gui.dockSession_.afterWindows(); }
    static bool modified(const Gui &gui) { return gui.isModified_; }
    static std::shared_ptr<SdkPanel> newSdk(Gui &gui) { gui.newSdkPanel(); return gui.sdkPanels_.back(); }
    static bool localEdit(Gui &gui,const std::string &name,double val) {
        for(const auto &[_,variable]:gui.vars_)if(variable->writeLocalScalarFromMonitor(name,val))return true;
        return false;
    }
    static void removeLocals(Gui &gui,const std::string &prefix) {
        for(const auto &[_,variable]:gui.vars_)
            std::erase_if(variable->vars_,[&](const auto &v){return v.port==PortType::LOCAL&&v.name.rfind(prefix,0)==0;});
    }
};
struct SdkPanelRegression {
    static void pinLog(SdkPanel &panel, bool show = true) { if (show) panel.dataLogHover_.setPinned(true); else panel.dataLogHover_.close(); }
    static std::string firstLocalName(const SdkPanel &panel) { return panel.dataTables_.front().localName; }
    static int fieldIndex(const SdkPanel &panel, const char *name) {
        const auto *table = panel.dataTableStruct(panel.dataTables_.front());
        for (int i=0;i<static_cast<int>(table->fields.size());++i) if(table->fields[i].name==name) return i;
        return -1;
    }
    static bool atTable(const SdkPanel &panel, size_t index) {
        return index < panel.dataTables_.size() && panel.navigationTable_ == panel.dataTables_[index].name;
    }

    static void refresh(SdkPanel &p) { p.refreshDataTableBindings(); }
    static bool linked(const SdkPanel &p) { return p.dataTables_.size()==1&&p.dataTables_[0].autoRead&&p.dataTables_[0].autoWrite; }
    static void call(SdkPanel &p,bool write) { p.runDataTableCall(0,write,false); }
};
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static constexpr ImGuiID rootId = 0xD0C00101;
static const char *names[] = {"Tool A###SessionDockA", "Tool B###SessionDockB", "Tool C###SessionDockC", "Tool D###SessionDockD"};
static ImGuiID leftId{}, rightId{};

static bool toolOpen[] = {true, true, true, true};
static ImGuiMouseButton testMouseButton = ImGuiMouseButton_Left;
static void frame(Gui &gui, bool build, const std::vector<int> &order,
                  ImVec2 mouse = ImVec2(-1000, -1000), bool down = false,
                  ImGuiDir direction = ImGuiDir_Left) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
    ImGui::GetIO().AddMouseButtonEvent(testMouseButton, down);
    ui::interaction::beginFrame();
    ImGui::NewFrame();
    if (build) {
        ImGui::DockBuilderRemoveNode(rootId);
        ImGui::DockBuilderAddNode(rootId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(rootId, ImGui::GetMainViewport()->WorkSize);
        ImGui::DockBuilderSplitNode(rootId, direction, 0.5f, &leftId, &rightId);
        for (int i = 0; i < 4; ++i) ImGui::DockBuilderDockWindow(names[i], i < 2 ? leftId : rightId);
        ImGui::DockBuilderFinish(rootId);
    }
    ImGui::DockSpaceOverViewport(rootId, ImGui::GetMainViewport());
    for (int index : order) {
        if (!toolOpen[index]) continue;
        if (dock_close::begin(names[index], &toolOpen[index]))
            ImGui::Text("Test tool %d", index);
        ImGui::End();
    }
    dock_close::draw();
    SimSamplerTestAccess::afterWindows(gui);
    ui::interaction::process();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
static ImGuiWindow *window(int index) { return ImGui::FindWindowByName(names[index]); }
static bool selected(ImGuiID nodeId, int index) {
    auto *node = ImGui::DockBuilderGetNode(nodeId);
    return node && node->TabBar && window(index) && node->TabBar->SelectedTabId == window(index)->TabId;
}
static void select(int index) {
    auto *w = window(index);
    check(w && w->DockNode && w->DockNode->TabBar, "dock tab exists");
    w->DockNode->TabBar->NextSelectedTabId = w->TabId;
    ImGui::FocusWindow(w);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    std::unique_ptr<Gui> gui;
    int result = 0;
    try {
        const std::string path = argv[1];
        std::ofstream(path) << "{\"monitors\":[],\"variables\":[]}";
        auto open = [&]() {
            check(glfwInit() == GLFW_TRUE, "GLFW initialization");
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            gui = std::make_unique<Gui>(path);
            // Keep every test surface in the hidden main window
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            ImGui::GetIO().IniFilename = nullptr;
        };
        open();
        const std::vector<int> original{0, 1, 2, 3}, changed{2, 3, 0, 1};
        for (int i = 0; i < 5; ++i) frame(*gui, i == 0, original);
        select(2);
        for (int i = 0; i < 2; ++i) frame(*gui, false, original);
        select(0);
        for (int i = 0; i < 2; ++i) frame(*gui, false, original);
        check(selected(leftId, 0) && selected(rightId, 2), "two dock groups have independently selected tabs");
        SimSamplerTestAccess::save(*gui, path);
        std::ifstream input(path);
        std::string saved((std::istreambuf_iterator<char>(input)), {});
        input.close();
        cJSON *document = cJSON_Parse(saved.c_str());
        check(document && cJSON_GetArraySize(cJSON_GetObjectItem(document, "activeDockTabs")) >= 2,
              "actual .ava save contains dock selections");
        cJSON_Delete(document);
        gui.reset();
        open();
        // Deliberately make undesired B/D tabs appear last
        for (int i = 0; i < 8; ++i) frame(*gui, false, changed);
        check(selected(leftId, 0) && selected(rightId, 2), "restart restores both selections despite reversed appearance order");
        check(ImGui::GetCurrentContext()->NavWindow && ImGui::GetCurrentContext()->NavWindow->RootWindow == window(0),
              "restart restores focused dock page");
        check(!SimSamplerTestAccess::modified(*gui), "restoration does not mark session data modified");
        select(1);
        for (int i = 0; i < 4; ++i) frame(*gui, false, changed);
        check(selected(leftId, 1), "normal tab switching works after startup restore completes");
        SimSamplerTestAccess::load(*gui, path);
        for (int i = 0; i < 8; ++i) frame(*gui, false, changed);
        check(selected(leftId, 0) && selected(rightId, 2), "opening a session in running app restores selected tabs");

        // Legacy .ava files already contain Selected= in imguiLayout
        document = cJSON_Parse(saved.c_str());
        cJSON_DeleteItemFromObject(document, "activeDockTabs");
        cJSON_DeleteItemFromObject(document, "focusedDockWindow");
        char *legacy = cJSON_Print(document);
        std::ofstream(path) << legacy;
        cJSON_free(legacy); cJSON_Delete(document);
        gui.reset(); open();
        for (int i = 0; i < 8; ++i) frame(*gui, false, changed);
        check(selected(leftId, 0) && selected(rightId, 2), "legacy layout selected tabs recover without new metadata");

        // A closed/missing saved tab must not be recreated or leave a permanent restore loop
        std::ofstream(path) << saved;
        gui.reset(); open();
        for (int i = 0; i < 35; ++i) frame(*gui, false, {3, 0, 1});
        check(window(2) == nullptr, "missing tab is not reopened");
        check(selected(leftId, 0) && selected(rightId, 3), "remaining dock groups get usable selections");
        select(1);
        for (int i = 0; i < 4; ++i) frame(*gui, false, {3, 0, 1});
        check(selected(leftId, 1), "missing saved page does not keep overriding user selection");
        // Click X on an unselected tab: cancellation must preserve its position
        select(0);
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        const auto tabRect = window(1)->DC.DockTabItemRect;
        const ImVec2 closePos(tabRect.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * 0.5f,
                             tabRect.GetCenter().y);
        const short dockOrder = window(1)->DockOrder;
        auto click = [&](ImVec2 pos) {
            frame(*gui, false, original, pos, false);
            frame(*gui, false, original, pos, true);
            frame(*gui, false, original, pos, false);
        };
        click(closePos);
        check(toolOpen[1] && dock_close::pending.size() == 1 && dock_close::pending.front().id == window(1)->ID,
              "X on inactive tab requests confirmation without closing");
        check(window(1)->DockNode && window(1)->DockOrder == dockOrder,
              "pending close keeps tab in original dock and order");
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        auto *popup = dock_close::confirmationWindow();
        check(popup && popup->Active && !(popup->Flags & ImGuiWindowFlags_Modal),
              "close confirmation is a nonmodal popup");
        check(ImGui::GetTopMostPopupModal() == nullptr && ImGui::GetCurrentContext()->DimBgRatio == 0,
              "close confirmation does not dim the workspace");
        check(std::abs(popup->Pos.x - closePos.x) < popup->Size.x + 12 &&
              std::abs(popup->Pos.y - closePos.y) < popup->Size.y + 12,
              "close confirmation stays adjacent to clicked tab");
        auto closeButtonPosition = [](ImGuiWindow *confirmation, bool cancel) {
            const auto &style = ImGui::GetStyle();
            const float buttonWidth = (confirmation->Size.x - style.WindowPadding.x * 2 - style.ItemSpacing.x) / 2;
            return ImVec2(confirmation->Pos.x + style.WindowPadding.x + buttonWidth * (cancel ? 1.5f : 0.5f) +
                              (cancel ? style.ItemSpacing.x : 0),
                          confirmation->Pos.y + confirmation->Size.y - style.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f);
        };
        const ImVec2 cancelPos = closeButtonPosition(popup, true);
        click(cancelPos);
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        check(toolOpen[1] && dock_close::pending.empty() && window(1)->DockOrder == dockOrder,
              "cancel preserves page and tab order");
        click(closePos);
        check(dock_close::pending.size() == 1, "X can be requested again after cancellation");
        const auto *viewport = ImGui::GetMainViewport();
        click(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 10, viewport->WorkPos.y + viewport->WorkSize.y - 10));
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        check(toolOpen[1] && dock_close::pending.empty() && window(1)->DockOrder == dockOrder,
              "outside click dismisses confirmation and preserves page");
        click(closePos);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        frame(*gui, false, original);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        check(toolOpen[1] && dock_close::pending.empty(), "Escape dismisses confirmation without closing page");
        click(closePos);
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        popup = dock_close::confirmationWindow();
        check(popup && popup->Active, "confirmation can reopen after outside click and Escape");
        const ImVec2 confirmPos = closeButtonPosition(popup, false);
        click(confirmPos);
        for (int i = 0; i < 3; ++i) frame(*gui, false, original);
        check(!toolOpen[1] && !window(1)->DockNode && dock_close::pending.empty(),
              "confirm really removes the requested page instead of appending it");
        check(toolOpen[0] && selected(leftId, 0), "other page stays open and usable");
        puts("PASS: inactive dock X confirmation, cancel/order preservation, confirmed removal");

        // Real Monitor uses a temporary open flag and must handle confirmed
        // closure even when begin() returns false
        Monitor closeA("CloseMonitorA"), closeB("CloseMonitorB");
        closeA.setTitle("Close monitor A"); closeB.setTitle("Close monitor B");
        Variable closeVariable("CloseVariable"); closeVariable.setTitle("Close variable");
        ImGui::DockBuilderDockWindow("Close monitor A###CloseMonitorA", leftId);
        ImGui::DockBuilderDockWindow("Close monitor B###CloseMonitorB", leftId);
        ImGui::DockBuilderDockWindow("Close variable###CloseVariable", leftId);
        ImGui::DockBuilderFinish(rootId);
        auto realFrame = [&](ImVec2 mouse = ImVec2(-1000, -1000), bool down = false) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
            ImGui::GetIO().AddMouseButtonEvent(testMouseButton, down);
            ImGui::NewFrame();
            ImGui::DockSpaceOverViewport(rootId, ImGui::GetMainViewport());
            if (!closeA.isPendingDelete()) closeA.updateDisplay();
            if (!closeB.isPendingDelete()) closeB.updateDisplay();
            if (!closeVariable.isPendingDelete()) closeVariable.updateDisplay();
            if (dock_close::begin(names[2], &toolOpen[2])) ImGui::TextUnformatted("Adjacent pane");
            ImGui::End();
            dock_close::draw();
            ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        };
        auto realClick = [&](ImVec2 pos) { realFrame(pos); realFrame(pos, true); realFrame(pos); };
        for (int i = 0; i < 4; ++i) realFrame();
        auto *realA = ImGui::FindWindowByName("Close monitor A###CloseMonitorA");
        ImGui::FocusWindow(realA);
        realA->DockNode->TabBar->NextSelectedTabId = realA->TabId;
        for (int i = 0; i < 2; ++i) realFrame();
        for (const char *name : {"Close monitor A###CloseMonitorA", "Close variable###CloseVariable"}) {
            auto *target = ImGui::FindWindowByName(name);
            ImGui::FocusWindow(target);
            target->DockNode->TabBar->NextSelectedTabId = target->TabId;
            for (int i = 0; i < 2; ++i) realFrame();
            const auto label = target->DC.DockTabItemRect;
            const ImVec2 labelPoint(label.Min.x + ImGui::GetStyle().FramePadding.x + 5, label.GetCenter().y);
            realClick(labelPoint); realClick(labelPoint);
            for (int i = 0; i < 3; ++i) realFrame();
            check(!dock_fold::isFolded(target->DockNode), "double-clicking a tab label does not fold the pane");
            const auto popupId = target->GetID(target == realA ? "Rename Dock" : "Rename Variable");
            check(!ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
                  ImGui::GetCurrentContext()->OpenPopupStack.back().PopupId == popupId,
                  "double-clicking real monitor and variable dock labels opens their rename popup");
            const auto windowId = target->ID, dockId = target->DockId;
            const char *newTitle = target == realA ? "Renamed monitor" : "Renamed variable";
            ImGui::GetIO().AddInputCharactersUTF8(newTitle); realFrame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); realFrame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); realFrame();
            for (int i = 0; i < 2; ++i) realFrame();
            check((target == realA ? closeA.getTitle() : closeVariable.getTitle()) == newTitle &&
                  target->ID == windowId && target->DockId == dockId &&
                  ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                  "Enter commits the new dock label while preserving window identity and docking");
            for (int gesture = 0; gesture < 2; ++gesture) {
                for (int i = 0; i < 22; ++i) realFrame();
                const auto rect = dock_fold::emptyTitleBarRect(target->DockNode);
                check(rect.GetWidth()>0, "real pane retains an empty title-bar target");
                const ImVec2 p = rect.GetCenter();
                testMouseButton = target->DockNode->ParentNode->SplitAxis == ImGuiAxis_Y ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
                realClick(p); realClick(p);
                testMouseButton = ImGuiMouseButton_Left;
                for (int i = 0; i < 3; ++i) realFrame();
                check(dock_fold::isFolded(target->DockNode) == (gesture == 0),
                      "real monitor and variable empty title bars double-click fold and expand");
                check(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                      "double-click folding never opens the old rename popup");
            }
        }
        puts("PASS: double-click monitor/variable dock labels, rename popup and Enter commit preserve docking");
        ImGui::FocusWindow(realA);
        realA->DockNode->TabBar->NextSelectedTabId = realA->TabId;
        for (int i = 0; i < 2; ++i) realFrame();
        for (const char *name : {"Close monitor B###CloseMonitorB", "Close variable###CloseVariable"}) {
            auto *target = ImGui::FindWindowByName(name);
            const auto rect = target->DC.DockTabItemRect;
            realClick(ImVec2(rect.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * 0.5f,
                             rect.GetCenter().y));
            check(target->ID == ImGui::FindWindowByName("Close monitor B###CloseMonitorB")->ID
                      ? !closeB.isPendingDelete() : !closeVariable.isPendingDelete(),
                  "real monitor/variable are alive until confirmation");
            for (int i = 0; i < 3; ++i) realFrame();
            check(dock_close::pending.size() == 1 && dock_close::pending.front().id == target->ID,
                  "real inactive page requests confirmation");
            popup = dock_close::confirmationWindow();
            check(popup && popup->Active, "real inactive page displays cursor-adjacent popup");
            realClick(closeButtonPosition(popup, false));
            for (int i = 0; i < 3; ++i) realFrame();
            check(!target->DockNode && !closeA.isPendingDelete(), "real confirmed page disappears, sibling survives");
        }
        check(closeB.isPendingDelete() && closeVariable.isPendingDelete(),
              "real monitor and variable lifetime flags receive confirmed closure");
        puts("PASS: real inactive monitor and variable confirmation and lifetime cleanup");

        // Render real monitor windows to verify visibility-triggered reflow
        Monitor layout("ScopeLayoutRegression");
        layout.setTitle("Scope layout");
        for (const char *name : {"A", "B", "C"}) {
            layout.addScope(name);
            layout.getScopes().at(name)->setDraw(MonitorScope::DrawEnum::TABLE);
        }
        auto layoutFrame = [&]() {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(1200, 900), ImGuiCond_Always);
            layout.updateDisplay();
            ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        };
        for (int i = 0; i < 3; ++i) layoutFrame();
        auto a = layout.getScopes().at("A"), b = layout.getScopes().at("B"), c = layout.getScopes().at("C");
        const float originalHeight = a->getHeight();
        const float readableMinimum = ui::ScopeMinimumHeight(ImGui::GetFontSize(), ImGui::GetFrameHeightWithSpacing() + 15);
        check(originalHeight >= readableMinimum - 1, "scope height follows readable font-scaled minimum");
        a->getHeight() = 100; a->setManual(true);
        b->setHidden(true);
        for (int i = 0; i < 3; ++i) layoutFrame();
        check(a->getHeight() >= originalHeight - 1 && std::abs(a->getHeight() - c->getHeight()) < 0.01f && !a->isManual(),
              "hiding a scope reflows equally without shrinking below the readable minimum or retaining manual height");
        c->setHidden(true);
        for (int i = 0; i < 3; ++i) layoutFrame();
        const float singleHeight = a->getHeight();
        check(originalHeight > readableMinimum + 1 ? singleHeight > originalHeight * 2 : singleHeight >= originalHeight,
              "one visible scope fills available area while honoring the font-scaled minimum");
        a->setHidden(true);
        for (int i = 0; i < 3; ++i) layoutFrame();
        a->setHidden(false); b->setHidden(false); c->setHidden(false);
        for (int i = 0; i < 3; ++i) layoutFrame();
        check(std::abs(a->getHeight() - b->getHeight()) < 0.01f &&
              std::abs(b->getHeight() - c->getHeight()) < 0.01f &&
              std::abs(a->getHeight() - originalHeight) < 1,
              "restoring all scopes returns to equal full-page layout");
        auto *rootWindow = ImGui::FindWindowByName("Scope layout###ScopeLayoutRegression");
        bool checkedArea = false;
        for (auto *w : ImGui::GetCurrentContext()->Windows) {
            const std::string name = w->Name;
            const std::string leaf = name.substr(name.rfind('/') + 1);
            if (rootWindow && w->RootWindow == rootWindow && leaf.starts_with("##scope_area")) {
                if (originalHeight > readableMinimum + 1)
                    check(w->ScrollMax.y < 2, "equalized scopes fit without leftover scroll range when minimum heights fit");
                checkedArea = true;
            }
        }
        check(checkedArea, "actual scrollable scope area was rendered");
        puts("PASS: scope hide/show reflow, manual height reset, all-hidden recovery and font-scaled minimum fit");
        // Exercise empty-title-bar double-clicks with real mouse input, without extra buttons
        for (auto &visible : toolOpen) visible = true;
        auto foldLabel = [&](ImGuiID id) {
            auto *node = ImGui::DockBuilderGetNode(id);
            check(node && node->TabBar, "fold target has a tab bar");
            for (const auto &tab : node->TabBar->Tabs)
                check(!(tab.Flags & ImGuiTabItemFlags_Button), "dock has no added fold button");
            const auto rect = dock_fold::emptyTitleBarRect(node);
            check(rect.GetWidth()>0, "even narrow folded panes retain an empty title-bar target");
            return rect.GetCenter();
        };
        auto foldClick = [&](ImGuiID id) {
            // Separate gestures so ImGui starts a fresh double-click sequence
            for (int i = 0; i < 22; ++i) frame(*gui, false, original);
            const auto point = foldLabel(id);
            testMouseButton = ImGui::DockBuilderGetNode(id)->ParentNode->SplitAxis == ImGuiAxis_Y ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
            frame(*gui, false, original, point);
            frame(*gui, false, original, point, true);
            frame(*gui, false, original, point);
            frame(*gui, false, original, point, true);
            frame(*gui, false, original, point);
            for (int i = 0; i < 4; ++i) frame(*gui, false, original);
            testMouseButton = ImGuiMouseButton_Left;
        };
        for (auto direction : {ImGuiDir_Up, ImGuiDir_Left}) {
            frame(*gui, true, original, ImVec2(-1000,-1000), false, direction);
            for (int i = 0; i < 5; ++i) frame(*gui, false, original);
            const int axis = direction == ImGuiDir_Up ? ImGuiAxis_Y : ImGuiAxis_X;
            auto *first = ImGui::DockBuilderGetNode(leftId);
            auto *second = ImGui::DockBuilderGetNode(rightId);
            const float before = first->Size[axis], siblingBefore = second->Size[axis];
            const auto selectedTab = first->SelectedTabId;
            const auto point = foldLabel(leftId);
            frame(*gui, false, original, point);
            frame(*gui, false, original, point, true);
            frame(*gui, false, original, point);
            check(!dock_fold::isFolded(first), "single-click on empty title bar does not fold");
            for(int i=0;i<22;++i) frame(*gui,false,original);
            testMouseButton=axis==ImGuiAxis_Y?ImGuiMouseButton_Right:ImGuiMouseButton_Left;
            frame(*gui,false,original,point);frame(*gui,false,original,point,true);frame(*gui,false,original,point);
            frame(*gui,false,original,point,true);frame(*gui,false,original,point);
            for(int i=0;i<3;++i)frame(*gui,false,original);
            check(!dock_fold::isFolded(first),"opposite mouse button does not fold along the wrong axis");
            testMouseButton=ImGuiMouseButton_Left;
            foldClick(leftId);
            check(dock_fold::isFolded(first) && first->Size[axis] < before * .5f &&
                  second->Size[axis] > siblingBefore + before * .4f,
                  "folding a pane lends its space to the adjacent pane in either orientation");
            check(first->SelectedTabId == selectedTab && window(0)->DockNode == first &&
                  window(1)->DockNode == first && toolOpen[0] && toolOpen[1] && dock_close::pending.empty(),
                  "folding preserves dock tabs, selection and open flags without closing a page");
            // Restored/hidden branches can temporarily have no cross-axis size.
            // Previously resizing either sibling passed that zero to ImGui and
            // triggered DockBuilderSetNodeSize's size.x/size.y assertion.
            const auto firstSize = first->Size, secondSize = second->Size;
            const auto firstRef = first->SizeRef, secondRef = second->SizeRef;
            const float foldRatio = dock_fold::folded.front().ratio;
            const bool firstLock = first->WantLockSizeOnce, secondLock = second->WantLockSizeOnce;
            for (auto *unmeasured : {first, second, first->ParentNode}) {
                const auto originalSize = unmeasured->Size;
                for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity()}) {
                    unmeasured->Size[1 - axis] = invalid;
                    dock_fold::resize(first, 60);
                    dock_fold::toggle(first);
                    check(first->SizeRef.x == firstRef.x && first->SizeRef.y == firstRef.y &&
                          second->SizeRef.x == secondRef.x && second->SizeRef.y == secondRef.y &&
                          first->WantLockSizeOnce == firstLock && second->WantLockSizeOnce == secondLock,
                          "unmeasured dock geometry never partially resizes either sibling");
                    check(dock_fold::isFolded(first) && dock_fold::folded.front().ratio == foldRatio,
                          "temporary invalid geometry preserves the saved fold and expansion ratio");
                    unmeasured->Size = originalSize;
                }
            }
            dock_fold::resize(first, std::numeric_limits<float>::quiet_NaN());
            check(first->Size.x == firstSize.x && first->Size.y == firstSize.y &&
                  second->Size.x == secondSize.x && second->Size.y == secondSize.y,
                  "non-finite requested extent leaves dock geometry unchanged");
            for (int i = 0; i < 3; ++i) frame(*gui, false, original);
            check(dock_fold::isFolded(first) && first->Size[axis] < before * .5f,
                  "folding resumes when dock layout dimensions become available");
            foldClick(leftId);
            check(!dock_fold::isFolded(first) && std::abs(first->Size[axis] - before) < 2,
                  "double-click restores the original split size");
            foldClick(rightId);
            check(dock_fold::isFolded(second) && second->Size[axis] < siblingBefore * .5f,
                  "bottom and right panes can be folded as well");
            foldClick(leftId);
            check(dock_fold::isFolded(first) && !dock_fold::isFolded(second),
                  "folding the other sibling expands the first rather than hiding both panes");
            foldClick(leftId);
            check(std::abs(first->Size[axis] - before) < 2 && std::abs(second->Size[axis] - siblingBefore) < 2,
                  "switching folded siblings retains the original split ratio");
        }
        foldClick(leftId);
        auto *foldedNode = ImGui::DockBuilderGetNode(leftId);
        auto *foldedParent = foldedNode->ParentNode;
        const float savedRatio = dock_fold::folded.front().ratio;
        SimSamplerTestAccess::save(*gui, path);
        {
            int width, height;
            glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
            std::vector<unsigned char> pixels(width * height * 3);
            glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream image(path + ".fold.ppm", std::ios::binary);
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for (int y = height - 1; y >= 0; --y)
                image.write(reinterpret_cast<char *>(pixels.data() + y * width * 3), width * 3);
        }
        gui.reset(); open();
        for (int i = 0; i < 8; ++i) frame(*gui, false, changed);
        foldedNode = ImGui::DockBuilderGetNode(leftId);
        check(dock_fold::isFolded(foldedNode), "saved session restores the folded pane and accessible tabs");
        int originalWidth, originalWindowHeight;
        glfwGetWindowSize(glfwGetCurrentContext(), &originalWidth, &originalWindowHeight);
        glfwSetWindowSize(glfwGetCurrentContext(), originalWidth - 120, originalWindowHeight - 70);
        glfwPollEvents();
        for (int i = 0; i < 5; ++i) frame(*gui, false, original);
        check(foldedNode->Size.x <= ImGui::GetFrameHeight() * 5 + 1,
              "folded width remains compact when the application window resizes");
        foldClick(leftId);
        foldedParent = foldedNode->ParentNode;
        const float available = foldedParent->Size.x - ImGui::GetStyle().DockingSeparatorSize;
        check(!dock_fold::isFolded(foldedNode) && std::abs(foldedNode->Size.x / available - savedRatio) < .005f,
              "expand after restart restores the pre-fold ratio");
        glfwSetWindowSize(glfwGetCurrentContext(), originalWidth, originalWindowHeight);
        glfwPollEvents();
        for (int i = 0; i < 4; ++i) frame(*gui, false, original);
        foldClick(leftId);
        ImGui::DockBuilderRemoveNode(rootId);
        frame(*gui, true, original, ImVec2(-1000,-1000), false, ImGuiDir_Up);
        for (int i = 0; i < 4; ++i) frame(*gui, false, original);
        check(dock_fold::folded.empty(), "changing dock topology drops stale fold state");
        ImGuiID nestedFirst, nestedSecond;
        ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Left, .4f, &nestedFirst, &nestedSecond);
        ImGui::DockBuilderDockWindow(names[0], nestedFirst);
        ImGui::DockBuilderDockWindow(names[1], nestedSecond);
        ImGui::DockBuilderFinish(rootId);
        for (int i = 0; i < 5; ++i) frame(*gui, false, original);
        const float unrelatedHeight = ImGui::DockBuilderGetNode(rightId)->Size.y;
        foldClick(nestedFirst);
        check(dock_fold::isFolded(ImGui::DockBuilderGetNode(nestedFirst)) &&
              ImGui::DockBuilderGetNode(rightId)->Size.y == unrelatedHeight,
              "folding inside a nested split leaves unrelated dock regions unchanged");
        foldClick(nestedFirst);
        puts("PASS: empty title-bar double-click folding, tab-label exclusion, no added buttons, all four sides, sibling handover, tab preservation and session restart");
        puts("PASS: .ava selected dock tabs and focus, restart/load, legacy sessions, missing tabs and later user switching");
        ui::interaction::reset();
        ImGui::FocusWindow(window(0));
        for (int i = 0; i < 5; ++i) frame(*gui, false, original);
        ImGui::FocusWindow(window(1));
        for (int i = 0; i < 5; ++i) frame(*gui, false, original);
        ui::interaction::requested = ui::interaction::Action::Back;
        for (int i = 0; i < 6; ++i) frame(*gui, false, original);
        check(ImGui::GetCurrentContext()->NavWindow && ImGui::GetCurrentContext()->NavWindow->RootWindow == window(0),
              "Navigation returns to a previous dock window");
        ui::interaction::requested = ui::interaction::Action::Forward;
        for (int i = 0; i < 6; ++i) frame(*gui, false, original);
        check(ImGui::GetCurrentContext()->NavWindow && ImGui::GetCurrentContext()->NavWindow->RootWindow == window(1),
              "Navigation forward restores dock focus");
        {
            SdkPanel sdk;
            g_lang=Lang::ZH;
            sdk.setWindowId(700);
            wchar_t exePath[32768]{}; GetModuleFileNameW(nullptr,exePath,32768);
            const auto sdkRoot=std::filesystem::path(exePath).parent_path()/"sdk/dyn200";
            auto *settings=cJSON_CreateObject();
            cJSON_AddStringToObject(settings,"dllPath",(sdkRoot/"dyn200.dll").generic_string().c_str());
            cJSON_AddStringToObject(settings,"headerPath",(sdkRoot/"dyn200.h").generic_string().c_str());
            sdk.load(settings,""); cJSON_Delete(settings);
            ImVec2 sdkMouse(-1000,-1000); bool sdkMouseDown=false;
            auto sdkFrame=[&]() {
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();
                ImGui::GetIO().AddMousePosEvent(sdkMouse.x,sdkMouse.y);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,sdkMouseDown);
                ui::interaction::beginFrame();ImGui::NewFrame();
                sdk.draw();ui::interaction::process();ImGui::Render();
                glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            };
            sdkFrame();
            ImGui::SetWindowPos(sdk.title(),ImVec2(0,0));ImGui::SetWindowSize(sdk.title(),ImVec2(1000,680));
            sdkFrame();sdkFrame();
            auto *w=ImGui::FindWindowByName(sdk.title());
            auto *languages=ImGui::TabBarFindByID(w->GetID("##sdktabs"));
            check(languages&&languages->Tabs.Size>0,"SDK language tabs render");
            const auto cppId=languages->Tabs[0].ID;
            auto *pages=ImGui::TabBarFindByID(ImHashStr("##ccpptabs",0,cppId));
            check(pages&&pages->Tabs.Size>0,"SDK content pages render with settings collapsed");
            ui::interaction::reset();
            ImGui::SetWindowFocus(sdk.title()); sdkFrame();
            auto *dataPages=ImGui::TabBarFindByID(ImHashStr("##sdk_data_pages",0,pages->Tabs[0].ID));
            int dataPageCount=0;
            if(dataPages)for(const auto &tab:dataPages->Tabs)if(!(tab.Flags&ImGuiTabItemFlags_Button))++dataPageCount;
            check(dataPages && dataPageCount==3, "DYN200 data groups render as three peer tabs");
            check(SdkPanelRegression::atTable(sdk,0), "First SDK data page opens directly without a list");
            auto selectDataPage=[&](int index) {
                dataPages->NextSelectedTabId=dataPages->Tabs[index].ID;
                sdkFrame();sdkFrame();sdkFrame();
                check(SdkPanelRegression::atTable(sdk,index), "Selecting a data tab displays its corresponding table");
            };
            selectDataPage(1);
            ui::interaction::requested = ui::interaction::Action::Back;
            sdkFrame(); sdkFrame(); sdkFrame();
            check(SdkPanelRegression::atTable(sdk,0), "Back restores the previous SDK data tab");
            ui::interaction::requested = ui::interaction::Action::Forward;
            sdkFrame(); sdkFrame(); sdkFrame();
            check(SdkPanelRegression::atTable(sdk,1), "Forward restores the next SDK data tab");
            selectDataPage(2);
            selectDataPage(0);
            const int tableIndex=0;
            const auto fieldTableId=ImHashStr("##sdk_data_fields_v4",0,ImHashData(&tableIndex,sizeof(tableIndex),pages->Tabs[0].ID));
            const auto *fieldTable=ImGui::GetCurrentContext()->Tables.GetByKey(fieldTableId);
            check(fieldTable&&fieldTable->ColumnsCount==3,"SDK table has field, value and type columns without a drag-handle column");
            auto dragName=[&](ImGuiWindow *owner, ImGuiID id, const char *payloadType) {
                ImGui::SetFocusID(id,owner);
                owner->NavRectRel[ImGuiNavLayer_Main]=ImRect(-1000,-1000,-999,-999);
                sdkFrame();
                const auto rect=ImGui::WindowRectRelToAbs(owner,owner->NavRectRel[ImGuiNavLayer_Main]);
                check(rect.Min.x>=0&&rect.Min.y>=0,"SDK name drag target is rendered");
                sdkMouse=ImVec2(rect.Min.x+18,rect.GetCenter().y);sdkFrame();
                sdkMouseDown=true;sdkFrame();sdkMouse.x+=22;sdkFrame();
                check(ImGui::GetDragDropPayload()&&ImGui::GetDragDropPayload()->IsDataType(payloadType),
                      "Dragging an SDK name starts the expected payload without an equals handle");
                sdkMouse=ImVec2(-1000,-1000);sdkMouseDown=false;sdkFrame();sdkFrame();
            };
            const auto tableScopeId=ImHashData(&tableIndex,sizeof(tableIndex),pages->Tabs[0].ID);
            dragName(w,ImHashStr(SdkPanelRegression::firstLocalName(sdk).c_str(),0,tableScopeId),"STRUCT_CHANNEL");
            const int countField=SdkPanelRegression::fieldIndex(sdk,"count"), firstElement=0;
            check(countField>=0,"DYN200 count field exists");
            const auto fieldScopeId=ImHashData(&firstElement,sizeof(firstElement),ImHashData(&countField,sizeof(countField),fieldTableId));
            dragName(fieldTable->InnerWindow,ImHashStr("count",0,fieldScopeId),"CHANNEL");
            const auto configId=ImHashStr("###sdkConfiguration",0,cppId);
            const auto logId=ImHashStr(tr("Call log##sdkCallLog","调用日志##sdkCallLog"),0,pages->Tabs[0].ID);
            check(w->StateStorage.GetInt(configId,-1)==0,"loaded SDK configuration defaults to collapsed");
            const auto findLog=[]() -> ImGuiWindow* {
                for(auto *candidate:ImGui::GetCurrentContext()->Windows)
                    if(std::strstr(candidate->Name,"##sdk_data_log"))return candidate;
                return nullptr;
            };
            check(!findLog()||!findLog()->Active,"SDK log takes no child area by default");
            auto screenshot=[&](const char *suffix) {
                int width,height;glfwGetFramebufferSize(glfwGetCurrentContext(),&width,&height);
                std::vector<unsigned char> pixels(width*height*3);glReadBuffer(GL_BACK);glPixelStorei(GL_PACK_ALIGNMENT,1);
                glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
                std::ofstream image(path+suffix,std::ios::binary);image<<"P6\n"<<width<<' '<<height<<"\n255\n";
                for(int y=height-1;y>=0;--y)image.write(reinterpret_cast<char*>(pixels.data()+y*width*3),width*3);
            };
            screenshot(".sdk-compact.ppm");
            w->StateStorage.SetInt(configId,1);SdkPanelRegression::pinLog(sdk);sdkFrame();sdkFrame();
            check(findLog()&&findLog()->Active,"SDK log can expand while data pages remain available");
            screenshot(".sdk-expanded.ppm");
            w->StateStorage.SetInt(configId,0);SdkPanelRegression::pinLog(sdk,false);sdkFrame();sdkFrame();
            check(!findLog()->Active,"SDK log releases its area when collapsed again");
            puts("PASS: SDK configuration and logs default to collapsed, expand and collapse without hiding content pages");
        }
        {
            const std::string header=R"(
                typedef struct fixture_command { uint32_t val; } fixture_command;
                typedef struct fixture_rt {
                    uint32_t write_command; fixture_command command;
                    uint64_t feedback; double torque; uint32_t values[2];
                } fixture_rt;
                int32_t sdk_sync_read_rt(fixture_rt *p);
                int32_t sdk_sync_write_rt(fixture_rt *p);
            )";
            const auto directory=std::filesystem::absolute(path).parent_path();
            const auto headerPath=directory/"sdk-link-fixture.h";
            std::ofstream(headerPath)<<header;
            auto panel=SimSamplerTestAccess::newSdk(*gui);
            auto *settings=cJSON_CreateObject();
            cJSON_AddStringToObject(settings,"dllPath",(directory/"sdk-caller-check/one.dll").generic_string().c_str());
            cJSON_AddStringToObject(settings,"headerPath",headerPath.generic_string().c_str());
            panel->load(settings,"");cJSON_Delete(settings);
            const auto parsed=parseHeaderFull(header);
            const auto *table=findStruct(parsed,"fixture_rt");check(table,"link fixture structure parses");
            const auto root="SDK"+std::to_string(panel->getWinId())+".fixture_rt";
            const auto command=root+".command.val";
            const auto get32=[&](const std::string &name,size_t offset=0) {
                const auto values=panel->onSnapshotLocalBuffers_();uint32_t val{};
                check(values.contains(name)&&values.at(name).size()>=offset+4,"linked LOCAL value exists");
                memcpy(&val,values.at(name).data()+offset,4);return val;
            };
            SdkPanelRegression::refresh(*panel);check(!SdkPanelRegression::linked(*panel),"unlinked SDK remains idle");
            panel->onCreateLocalVar_(command,CType::U32,nullptr);SdkPanelRegression::refresh(*panel);
            check(SdkPanelRegression::linked(*panel),"drag-created scalar activates SDK automatically");
            SdkPanelRegression::call(*panel,false);check(get32(command)==7,"SDK reads reach the real variable manager");
            check(SimSamplerTestAccess::localEdit(*gui,command,42),"monitor edit accepts linked scalar");
            SdkPanelRegression::call(*panel,true);SdkPanelRegression::call(*panel,false);
            check(get32(command)==42,"monitor scalar edit reaches SDK write and readback");
            SimSamplerTestAccess::removeLocals(*gui,root);SdkPanelRegression::refresh(*panel);
            check(!SdkPanelRegression::linked(*panel),"removing scalar stops automatic SDK binding");
            panel->onCreateLocalVar_(root,CType::Unknown,table);SdkPanelRegression::refresh(*panel);
            SdkPanelRegression::call(*panel,false);
            check(SimSamplerTestAccess::localEdit(*gui,command,66),"monitor edit accepts a member of a dragged whole table");
            SdkPanelRegression::call(*panel,true);SdkPanelRegression::call(*panel,false);
            check(get32(root,4)==66,"whole-table member edit reaches SDK and returns through real byte buffers");
            check(SimSamplerTestAccess::localEdit(*gui,root+".values[1]",99),"monitor accepts an array element of a whole-table link");
            SdkPanelRegression::call(*panel,true);SdkPanelRegression::call(*panel,false);
            check(get32(root,28)==99,"whole-table array edit reaches SDK write callback");
            SimSamplerTestAccess::removeLocals(*gui,root);SdkPanelRegression::refresh(*panel);
            check(!SdkPanelRegression::linked(*panel),"removing root stops automatic SDK binding");
            puts("PASS: SDK drag activation, real Variable/Monitor scalar, root and array writes, exact byte callbacks and removal");
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what()); result = 1;
    }
    gui.reset();
    JLinkPort::instance().close();
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
