#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/search_match.hpp"
#include "gui/search_results.hpp"
#include "gui/hover_panel.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

struct VariableRewriteRegression {
    static constexpr u64 base = 0x20000000;
    static constexpr const char *deep = "g_foc.lo.ctrl.pd.lo.eff_cfg";
    static constexpr const char *host = "Variable focus test host";

    static void initialize(Variable &v) {
        auto &info = v.dwarfInfo_;
        info.present = true;
        auto &scalar = info.types[1];
        scalar.kind = dwarf::TypeKind::BASE; scalar.name = "float"; scalar.size = 4; scalar.encoding = 4;
        auto aggregate = [&](u64 id, u64 size, std::vector<dwarf::Type::Member> members) {
            auto &type = info.types[id];
            type.kind = dwarf::TypeKind::STRUCT; type.size = size; type.members = std::move(members);
        };
        aggregate(10, 256, {{"lo", 4, 11}, {"banks", 128, 30}});
        aggregate(11, 96, {{"ctrl", 8, 12}});
        aggregate(12, 80, {{"pd", 16, 13}});
        aggregate(13, 64, {{"lo", 4, 14}});
        aggregate(14, 60, {{"eff_cfg", 32, 20}});
        aggregate(15, 32, {{"kp", 0, 1}, {"ki", 4, 1}, {"kd", 8, 1}, {"limits", 16, 16}});
        aggregate(16, 8, {{"min", 0, 1}, {"max", 4, 1}});
        auto &alias = info.types[20]; alias.kind = dwarf::TypeKind::TYPEDEF; alias.inner = 15;
        auto &array = info.types[30];
        array.kind = dwarf::TypeKind::ARRAY; array.inner = 20; array.size = 96; array.dims = {3};
        VarEntry root{};
        root.name = "g_foc"; root.addr = base; root.type = DataType::U32;
        root.port = PortType::JLINK; root.writable = true; root.isStruct = true; root.typeOff = 10;
        root.expandedMembers = {"g_foc", "g_foc.lo", "g_foc.lo.ctrl"};
        v.vars_.push_back(std::move(root));
        sample(v, base + 64, 10.0f); sample(v, base + 68, 0.0f); sample(v, base + 72, 1.0f);
        sample(v, base + 80, -50.0f); sample(v, base + 84, 50.0f);
    }

    template <typename T> static void sample(Variable &v, u64 address, T val) {
        auto &cached = v.syncReadCache_[address];
        cached = {}; cached.ok = true; cached.sz = sizeof(val);
        std::memcpy(cached.buf, &val, sizeof(val));
    }

    static void frame(Variable &v) {
        v.memberPollReqs_.clear();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1500, 900));
        ImGui::Begin(host, nullptr, ImGuiWindowFlags_NoSavedSettings);
        v.drawVariableList();
        v.drawUnitConversionEditor();
        ImGui::End(); ImGui::Render();
    }

    static void click(Variable &v, const char *id) {
        auto *window = ImGui::FindWindowByName(host);
        check(window && window->Active, "watch window exists before navigation");
        ImGui::ActivateItemByID(window->GetID(id)); frame(v); frame(v);
    }

    /**
     * @brief Use ImGui's navigation rectangle to locate a submitted widget, then drive
     *        real mouse/key events. No product focus setters participate in these tests
     */
    static ImRect locate(ImGuiWindow *window, ImGuiID id, const std::function<void()> &draw) {
        check(window && window->Active, "interaction target window is active");
        ImGui::SetFocusID(id, window);
        window->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
        draw();
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        if (rect.Min.x < 0 || rect.Min.y < 0)
            fprintf(stderr, "Missing interaction target: %08X in %s\n", id, window->Name);
        check(rect.Min.x >= 0 && rect.Min.y >= 0, "interaction target was submitted this frame");
        return rect;
    }

    static void mouse(const std::function<void()> &draw, ImVec2 pos, bool down) {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        draw();
    }

    static void gesture(const std::function<void()> &draw, ImVec2 pos, bool twice = false) {
        // Advance simulated frame time beyond the previous gesture's double-click window
        for (int i = 0; i < 25; ++i) mouse(draw, ImVec2(-1000, -1000), false);
        mouse(draw, pos, false); mouse(draw, pos, true); mouse(draw, pos, false);
        if (twice) { mouse(draw, pos, true); mouse(draw, pos, false); }
        mouse(draw, ImVec2(-1000, -1000), false);
    }

    static std::vector<std::string> paths(const std::string &path) {
        std::vector<std::string> out;
        for (size_t i = 0; i < path.size(); ++i)
            if (path[i] == '.' || path[i] == '[') out.push_back(path.substr(0, i));
        out.push_back(path);
        return out;
    }

    static ImVec2 variablePoint(Variable &v, const std::string &path, bool arrow = false) {
        auto *window = ImGui::FindWindowByName(host);
        auto *table = ImGui::TableFindByID(window->GetID("VarMonitorTableV2"));
        check(table, "variable table exists");
        ImGuiID scope = table->ID;
        const auto ancestors = paths(path);
        size_t start = 0;
        if (v.watchFocus_.path.empty()) {
            const int ownerIndex = 0;
            scope = ImHashData(&ownerIndex, sizeof(ownerIndex), scope);
        } else {
            scope = ImHashStr("watch_focus", 0, scope);
            scope = ImHashStr(v.watchFocus_.path.c_str(), 0, scope);
            while (start < ancestors.size() && ancestors[start] != v.watchFocus_.path) ++start;
            ++start;
        }
        for (size_t i = start; i + 1 < ancestors.size(); ++i)
            scope = ImHashStr((i == 0 ? ancestors[i] : "###member_" + ancestors[i]).c_str(), 0, scope);
        const std::string labelId = ancestors.size() == 1 ? path : "###member_" + path;
        const auto rect = locate(table->InnerWindow, ImHashStr(labelId.c_str(), 0, scope), [&] { frame(v); });
        const float nodeX = rect.Min.x;
        const float x = nodeX + (arrow ? ImGui::GetFontSize() * 0.5f :
            ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2 + 4);
        return ImVec2(x, rect.GetCenter().y);
    }

    static void watchSearch() {
        Variable v("watch-search"); initialize(v);
        v.vars_[0].expandedMembers.clear();
        auto draw = [&] { frame(v); };
        auto *top = ImGui::FindWindowByName(host);
        frame(v);
        top = ImGui::FindWindowByName(host);
        check(top, "variable manager host exists");
        const auto searchInput = locate(top, top->GetID("##watch_search"), draw);
        check(searchInput.GetWidth() > ImGui::GetFontSize() * 8 &&
                  searchInput.GetWidth() < ImGui::GetFontSize() * 14,
              "variable manager exposes a compact directly editable search input");
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "EFF_CFG.KP");
        v.watchSearchHover_.setPinned(true);
        frame(v); frame(v);
        check(v.watchSearchResults_.size() == 1 && v.watchSearchResults_[0].path == std::string(deep) + ".kp",
              "manager search finds deeply nested collapsed members case-insensitively");
        check(!ImGui::FindWindowByName("##watch_search_panel"),
              "manager search results stay inline instead of opening a hover panel");
        const auto kpAddress = v.watchSearchResults_[0].address;
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "0x%08llX",
                 static_cast<unsigned long long>(kpAddress));
        frame(v); frame(v);
        check(std::any_of(v.watchSearchResults_.begin(), v.watchSearchResults_.end(), [&](const auto &result) {
                  return result.path == std::string(deep) + ".kp" && result.address == kpAddress;
              }),
              "manager search resolves a nested member by address");
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "EFF_CFG.KP");
        frame(v); frame(v);
        check(v.watchSearchResults_.size() == 1 && v.watchSearchResults_[0].path == std::string(deep) + ".kp",
              "name search restores after address search");
        ImGuiWindow *results = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::strstr(window->Name, "##watch_search_results")) results = window;
        check(results, "manager search has a scrollable result list");
        const auto point = ImVec2(results->InnerRect.GetCenter().x,
                                  results->InnerRect.Min.y + ImGui::GetTextLineHeight() * .5f);
        gesture(draw, point);
        check(v.watchSearchSelectedPath_ == std::string(deep) + ".kp" && v.vars_[0].expandedMembers.contains(deep),
              "search result click reveals its ancestors and selects the exact member");
        check(!v.watchSearchScroll_, "revealed search result scrolls into the table viewport");
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "banks[2].limits.max");
        v.watchSearchHover_.setPinned(true); frame(v); frame(v);
        check(v.watchSearchResults_.size() == 1 && v.watchSearchResults_[0].path == "g_foc.banks[2].limits.max",
              "array member search uses canonical indexed paths");
        v.vars_[0].memberOverrides[std::string(deep) + ".kp"].alias = "CustomGain";
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "CUSTOMGAIN"); frame(v);
        check(v.watchSearchResults_.size() == 1, "member alias edits invalidate search results");
        v.vars_[0].memberOverrides[deep].hasType = true;
        v.vars_[0].memberOverrides[deep].type = DataType::F32;
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "eff_cfg.kp"); frame(v);
        check(v.watchSearchResults_.empty(), "scalar type overrides do not expose unreachable descendants in search");
        snprintf(v.watchSearchText_, sizeof(v.watchSearchText_), "not_found"); frame(v);
        check(v.watchSearchResults_.empty(), "editing a search clears stale results without erasing the query first");
        v.watchSearchHover_.close();
        puts("PASS: manager search, collapsed descendants, array paths, native result click and reveal");
    }

    static void hoverFocusAndChangeFlash() {
        Variable v("hover-focus"); initialize(v);
        v.vars_[0].expandedMembers.clear();
        auto draw = [&] { frame(v); };
        frame(v); frame(v);
        auto *table = ImGui::TableFindByID(ImGui::FindWindowByName(host)->GetID("VarMonitorTableV2"));
        const int owner = 0;
        const auto row = ImHashData(&owner, sizeof(owner), table->ID);
        const auto point = locate(table->InnerWindow, ImHashStr("###focus_group", 0, row), draw).GetCenter();
        for (int i = 0; i < 28; ++i) mouse(draw, point, false);
        check(v.watchFocus_.path.empty() && !v.watchFocusPreview_.active() && v.vars_[0].expandedMembers.empty(),
              "Variable manager Focus never opens from a stationary hover");
        mouse(draw, point, true); mouse(draw, point, false);
        mouse(draw, ImVec2(-1000, -1000), false); frame(v);
        check(v.watchFocus_.path == "g_foc" && !v.watchFocusPreview_.active(), "Variable manager Focus opens on click");
        ui::ValueFlash flash;
        check(flash.update("reading", "10") == 0 && flash.update("reading", "10") == 0,
              "Initial and unchanged values do not flash");
        check(flash.update("reading", "11") > 0, "A changed value starts a highlight");
        for (int i = 0; i < 70; ++i) frame(v);
        check(flash.update("reading", "11") == 0 && flash.update("reading", "ERR") == 0,
              "Stable values fade back; read errors retain their own error styling");
        puts("PASS: variable click-only focus and transient value changes");
    }

    static void variableMouseFocus() {
        Variable v("focus-real-input"); initialize(v);
        auto draw = [&] { frame(v); };
        v.vars_[0].expandedMembers.clear(); frame(v); frame(v);
        gesture(draw, variablePoint(v, "g_foc", true));
        check(v.watchFocus_.path.empty() && v.vars_[0].expandedMembers.contains("g_foc"),
              "root arrow expands without focusing");
        const auto expandedRoot = v.vars_[0].expandedMembers;
        gesture(draw, variablePoint(v, "g_foc", true), true);
        check(v.watchFocus_.path.empty() && v.vars_[0].expandedMembers == expandedRoot,
              "double click on the root arrow only toggles expansion twice");
        gesture(draw, variablePoint(v, "g_foc"));
        check(v.watchFocus_.path.empty() && v.vars_[0].expandedMembers == expandedRoot,
              "single click on root name preserves expansion and focus");
        check(std::ranges::none_of(v.vars_, [](const VarEntry &entry) { return entry.selected; }),
              "variable manager left click does not select or highlight a variable");
        gesture(draw, variablePoint(v, "g_foc"), true);
        check(v.watchFocus_.path == "g_foc" && v.vars_[0].expandedMembers == expandedRoot,
              "double click on root name focuses without collapsing it");
        click(v, "###watch_focus_all");

        for (const auto &path : paths(deep)) v.vars_[0].expandedMembers.insert(path);
        frame(v); frame(v);
        const auto expandedDeep = v.vars_[0].expandedMembers;
        auto blank = variablePoint(v, deep);
        blank.x = ImGui::TableFindByID(ImGui::FindWindowByName(host)->GetID("VarMonitorTableV2"))->Columns[0].MaxX - 12;
        gesture(draw, blank, true);
        check(v.watchFocus_.path.empty() && v.vars_[0].expandedMembers == expandedDeep,
              "double click on the empty part of the variable name column does not focus");
        gesture(draw, variablePoint(v, deep), true);
        check(v.watchFocus_.path == deep && v.vars_[0].expandedMembers == expandedDeep,
              "double click on an expanded nested aggregate focuses without changing the tree");
        request(v, base + 64, 4); request(v, base + 68, 4); request(v, base + 72, 4);
        gesture(draw, variablePoint(v, std::string(deep) + ".kp"), true);
        check(v.watchFocus_.path == deep, "double click on a scalar cannot replace the focused root");
        gesture(draw, variablePoint(v, std::string(deep) + ".limits", true));
        check(v.watchFocus_.path == deep && v.vars_[0].expandedMembers.contains(std::string(deep) + ".limits"),
              "member arrow expands in focused view without navigating");
        click(v, "###watch_focus_all");

        v.vars_[0].expandedMembers.insert("g_foc.banks"); frame(v);
        const auto expandedArray = v.vars_[0].expandedMembers;
        gesture(draw, variablePoint(v, "g_foc.banks[1]"), true);
        check(v.watchFocus_.path == "g_foc.banks[1]" && v.vars_[0].expandedMembers == expandedArray,
              "double click on a collapsed array element focuses without expanding it");
        request(v, base + 160, 4); request(v, base + 164, 4); request(v, base + 168, 4);
        click(v, "###watch_focus_back");
        check(v.watchFocus_.path == "g_foc.banks", "array element Back returns to its array");
        click(v, "###watch_focus_all");
        puts("PASS: actual name double clicks focus root/member/array; single clicks and arrows preserve navigation semantics");
    }

    static constexpr const char *monitorHost = "Monitor focus test host";

    static void frame(MonitorScope &scope) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1500, 900));
        ImGui::Begin(monitorHost, nullptr, ImGuiWindowFlags_NoSavedSettings);
        scope.tableDraw();
        ImGui::End(); ImGui::Render();
    }

    static ImRect monitorItem(MonitorScope &scope, const std::string &path,
                              const char *item = nullptr) {
        auto *window = ImGui::FindWindowByName(monitorHost);
        auto *table = ImGui::TableFindByID(window->GetID("MonitorTable"));
        check(table, "monitor table exists");
        ImGuiID seed = table->ID;
        std::string relative = path.substr(scope.tableFocusPath_.size());
        if (relative.starts_with('.')) relative.erase(0,1);
        std::vector<std::string> parts;
        for (size_t at = 0; at < relative.size();) {
            if (relative[at] == '.') { ++at; continue; }
            const bool index = relative[at] == '[';
            auto end = index ? relative.find(']',at) : relative.find_first_of(".[",at);
            if (end == std::string::npos) end = relative.size(); else if(index) ++end;
            parts.push_back(relative.substr(at,end-at)); at=end;
        }
        for(size_t i=0;i+1<parts.size();++i) seed=ImHashStr(parts[i].c_str(),0,seed);
        if (item) {
            seed = ImHashStr(path.c_str(), 0, seed);
            const auto *channel = scope.findChannel(path);
            const auto nameLabel = channel && channel->getLabel() != path ? channel->getLabel() : path.substr(path.rfind('.')+1);
            seed = ImHashStr(std::string(item) == "__rowName" ? nameLabel.c_str() : item, 0, seed);
        } else {
            seed = ImHashStr(parts.back().c_str(), 0, seed);
        }
        return locate(table->InnerWindow, seed, [&] { frame(scope); });
    }

    static ImVec2 monitorPoint(MonitorScope &scope, const std::string &path,
                               bool arrow = false, bool leaf = false) {
        const auto rect = monitorItem(scope, path, leaf ? "__rowName" : nullptr);
        const float x = rect.Min.x + (leaf ? 4 : arrow ?
            ImGui::GetFontSize() * 0.5f : ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2 + 4);
        return ImVec2(x, rect.GetCenter().y);
    }

    static void monitorButton(MonitorScope &scope, const char *id, const char *path = nullptr) {
        auto *window = ImGui::FindWindowByName(monitorHost);
        const auto seed = path ? ImHashStr(path, 0, window->ID) : window->ID;
        const auto rect = locate(window, ImHashStr(id, 0, seed), [&] { frame(scope); });
        gesture([&] { frame(scope); }, rect.GetCenter());
    }

    static void monitorMouseFocusAndEditing() {
        MonitorScope scope("focus-monitor");
        scope.setDraw(MonitorScope::DrawEnum::TABLE);
        const std::string kp = std::string(deep) + ".kp";
        const std::string ki = std::string(deep) + ".ki";
        const std::string kd = std::string(deep) + ".kd";
        const std::string limits = std::string(deep) + ".limits";
        const std::vector<std::string> names = {"unrelated", kp, ki, kd, limits + ".min", limits + ".max",
                                                "g_foc.banks[1].kp", "g_foc.enable"};
        for (size_t i = 0; i < names.size(); ++i) {
            scope.addChannel(names[i]);
            auto *channel = scope.findChannel(names[i]);
            channel->setDevice("JLINK"); channel->setType("u32");
            channel->setAddr(base + i * 4); channel->setWritable(true);
            channel->setRVal(static_cast<float>(i + 1), 0.0); channel->publishSnapshot();
        }
        scope.findChannel(kp)->setLabel("Proportional gain");
        const auto channels = scope.chs_;
        for (const auto &path : paths(deep)) if (path != deep) scope.expandedGroups_.insert(path);
        auto draw = [&] { frame(scope); };
        frame(scope); frame(scope);
        gesture(draw, monitorPoint(scope, deep, true));
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_.contains(deep),
              "monitor group arrow expands without focusing");
        const auto expanded = scope.expandedGroups_;
        gesture(draw, monitorPoint(scope, deep, true), true);
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_ == expanded,
              "double click on the monitor arrow only toggles expansion twice");
        gesture(draw, monitorPoint(scope, deep));
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_ == expanded &&
                  !scope.selectedGroupPaths_.contains(deep),
              "monitor single click does not select, collapse, or focus a group");
        auto blank = monitorPoint(scope, deep);
        blank.x = ImGui::TableFindByID(ImGui::FindWindowByName(monitorHost)->GetID("MonitorTable"))->Columns[0].MaxX - 12;
        gesture(draw, blank, true);
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_ == expanded,
              "double click on the empty part of the monitor name column does not focus");
        gesture(draw, monitorPoint(scope, deep), true);
        check(scope.tableFocusPath_ == deep && scope.expandedGroups_ == expanded,
              "monitor double click focuses the group without altering expansion");
        check(scope.tableVisibleKeys_ == std::vector<std::string>{kp, ki, kd},
              "focused monitor lists only the subtree's currently visible scalar rows");
        check(scope.chs_ == channels, "focus preserves every original channel object including hidden siblings");
        for (size_t i = 0; i < names.size(); ++i) {
            auto *channel = scope.findChannel(names[i]);
            check(channel->getAddr() == base + i * 4 && channel->getType() == "u32" &&
                  channel->getDevice() == "JLINK" && channel->getOrder() == static_cast<i64>(i) &&
                  channel->read_.rawSize() == 1 && channel->getDispVal() == static_cast<float>(i + 1),
                  "monitor focus retains channel address/type/device/order and sampled history");
        }

        gesture(draw, monitorPoint(scope, kp, false, true), true);
        check(scope.tableFocusPath_ == deep, "monitor scalar name double click keeps the current focus");
        gesture(draw, monitorPoint(scope, kp, false, true));
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
        gesture(draw, monitorPoint(scope, kd, false, true));
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false); frame(scope);
        for (const auto &name : names)
            check(!scope.findChannel(name)->selected_,
                  "left click and Shift+click never select monitor table rows");

        const auto valueRect = monitorItem(scope, kp, "##val");
        gesture(draw, valueRect.GetCenter());
        auto &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame(scope);
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame(scope);
        io.AddInputCharactersUTF8("123"); frame(scope);
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(scope);
        io.AddKeyEvent(ImGuiKey_Enter, false); frame(scope);
        float written = 0;
        check(scope.findChannel(kp)->consumeWValDirty(written) && written == 123 &&
              scope.findChannel(kp)->getAddr() == base + 4,
              "editing the focused monitor value queues a write on the original addressed channel");
        for (const auto &name : names)
            if (name != kp) check(!scope.findChannel(name)->consumeWValDirty(written),
                                  "focused value edit does not write another channel");
        scope.findChannel(kp)->setRVal(77, 0.1); scope.findChannel(kp)->publishSnapshot(); frame(scope);
        check(scope.findChannel(kp)->read_.rawSize() == 2 && scope.findChannel(kp)->getDispVal() == 77,
              "the original channel continues publishing live samples while focused");

        gesture(draw, monitorPoint(scope, limits), true);
        check(scope.tableFocusPath_ == limits && scope.expandedGroups_ == expanded &&
              scope.tableVisibleKeys_ == std::vector<std::string>{limits + ".min", limits + ".max"},
              "double clicking a collapsed focused child shows its leaves without expanding the original tree");
        monitorButton(scope, "###monitor_focus_back");
        check(scope.tableFocusPath_ == deep, "monitor Back returns to the immediate parent");
        monitorButton(scope, "###monitor_focus_crumb", "g_foc");
        check(scope.tableFocusPath_ == "g_foc", "monitor breadcrumb returns directly to an ancestor");
        monitorButton(scope, "###monitor_focus_back");
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_ == expanded,
              "monitor Back from root restores the original whole tree");

        gesture(draw, monitorPoint(scope, "g_foc.banks", true));
        const auto arrayExpanded = scope.expandedGroups_;
        gesture(draw, monitorPoint(scope, "g_foc.banks[1]"), true);
        check(scope.tableFocusPath_ == "g_foc.banks[1]" &&
              scope.tableVisibleKeys_ == std::vector<std::string>{"g_foc.banks[1].kp"},
              "monitor array-like channel groups can be focused by double click");
        monitorButton(scope, "###monitor_focus_all");
        check(scope.tableFocusPath_.empty() && scope.expandedGroups_ == arrayExpanded,
              "monitor All variables restores the original expansion state");
        gesture(draw, monitorPoint(scope, deep), true);
        for (const auto &name : names) if (name.starts_with(std::string(deep) + ".")) scope.chs_.erase(name);
        frame(scope);
        check(scope.tableFocusPath_.empty(), "removing the focused monitor subtree safely returns to all variables");
        puts("PASS: monitor double-click/arrow/navigation without left-click selection, original channel edits and live sampling");
    }

    static void request(const Variable &v, u64 address, u32 bytes) {
        auto it = v.memberPollReqs_.find(address);
        check(it != v.memberPollReqs_.end() && it->second == bytes, "visible focused field requests its effective address and size");
    }

    static void hoverPanelResize() {
        ui::HoverReveal state; state.setPinned(true);
        auto draw = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(900,700));
            ImGui::Begin("Resize host"); ImGui::Button("Options");
            if (ui::BeginHoverPanel("Resize panel", "Options", state, false, 320, ImGui::GetMainViewport())) {
                ImGui::TextUnformatted("Content"); ImGui::End();
            }
            ImGui::End(); ImGui::Render();
        };
        draw(); draw(); draw();
        auto *panel = ImGui::FindWindowByName("Resize panel");
        check(panel && !(panel->Flags & ImGuiWindowFlags_NoResize), "hover panel enables native resizing");
        const auto before = panel->Size;
        const ImVec2 grip(panel->Pos.x + panel->Size.x - 3, panel->Pos.y + panel->Size.y - 3);
        mouse(draw, grip, false); mouse(draw, grip, true);
        mouse(draw, ImVec2(grip.x + 80, grip.y + 60), true);
        mouse(draw, ImVec2(grip.x + 80, grip.y + 60), false);
        check(panel->Size.x > before.x + 40 && panel->Size.y > before.y + 30,
              "dragging the native bottom-right grip resizes both dimensions");
        const auto resized = panel->Size;
        mouse(draw, ImVec2(-1000,-1000), false); draw();
        check(panel->Size.x == resized.x && panel->Size.y == resized.y && state.isPinned(),
              "panel retains its size after releasing the resize grip");
        const ImVec2 resizeAgain(panel->Pos.x + panel->Size.x - 3, panel->Pos.y + panel->Size.y - 3);
        mouse(draw, resizeAgain, false); mouse(draw, resizeAgain, true);
        mouse(draw, ImVec2(resizeAgain.x + 70, resizeAgain.y + 40), true);
        check(panel->Size.x > resized.x + 30, "second resize starts from the retained size");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); draw();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        mouse(draw, ImVec2(-1000, -1000), false); draw();
        check(std::abs(panel->Size.x - resized.x) < 1 && std::abs(panel->Size.y - resized.y) < 1 && state.isPinned(),
              "Escape restores the resize baseline without closing the panel");
        state.close(); draw();
        puts("PASS: native hover-panel corner resizing, retained size and Escape rollback");
    }

    static void scopeTitleAlignment() {
        Monitor monitor("title-owner");
        MonitorScope scope("Short title"); scope.parent_ = &monitor;
        auto draw = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(900,400));
            ImGui::SetNextWindowFocus();
            ImGui::Begin("Scope title test"); scope.menu(); ImGui::End(); ImGui::Render();
        };
        ImGui::GetIO().AddMousePosEvent(-1000, -1000);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        for (bool hidden : {false, true}) {
            scope.setDraw(hidden ? MonitorScope::DrawEnum::PLOT : MonitorScope::DrawEnum::TABLE);
            scope.setHidden(hidden); draw(); draw();
            auto *window = ImGui::FindWindowByName("Scope title test");
            const auto focus = locate(window, window->GetID("Focus"), draw);
            const auto more = locate(window, window->GetID("More"), draw);
            check(focus.Min.x > window->Pos.x + window->Size.x * .5f,
                  "table and hidden scope titles stay on the right");
            check(focus.Max.x <= more.Min.x,
                  "right aligned title leaves focus and more buttons unobstructed");
        }
        puts("PASS: table and hidden scope titles align right without overlapping actions");
    }

    static void nameDragging() {
        Variable variable("name-drag"); initialize(variable);
        frame(variable); frame(variable);
        const auto contextPoint = variablePoint(variable, "g_foc");
        ImGui::GetIO().AddMousePosEvent(contextPoint.x, contextPoint.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true); frame(variable);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false); frame(variable); frame(variable);
        check(!ImGui::GetCurrentContext()->OpenPopupStack.empty(),
              "Right clicking the variable name opens its row actions after removing the grip");
        ImGui::ClosePopupsOverWindow(nullptr, false);
        mouse([&] { frame(variable); }, ImVec2(-1000, -1000), false);
        auto dragVariable = [&](const std::string &path, const char *type) {
            const auto point = variablePoint(variable, path);
            auto draw = [&] { frame(variable); };
            mouse(draw, point, false); mouse(draw, point, true);
            mouse(draw, ImVec2(point.x + 22, point.y), true);
            check(ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType(type),
                  "Variable name starts a monitor drag without a separate handle");
            mouse(draw, ImVec2(-1000, -1000), false); frame(variable);
        };
        dragVariable("g_foc", "STRUCT_CHANNEL");
        variable.focusWatch(0, deep); frame(variable); frame(variable);
        dragVariable(std::string(deep) + ".kp", "CHANNEL");
        MonitorScope scope("name-drag-scope");
        scope.addChannel("group.first"); scope.addChannel("group.second");
        scope.expandedGroups_.insert("group");
        frame(scope); frame(scope);
        auto dragScope = [&](const std::string &path, bool group) {
            const auto point = monitorPoint(scope, path, false, !group);
            auto draw = [&] { frame(scope); };
            mouse(draw, point, false); mouse(draw, point, true);
            mouse(draw, ImVec2(point.x + 22, point.y), true);
            const auto *payload = ImGui::GetDragDropPayload();
            check(payload && payload->IsDataType("DND_CHANNEL_MOVE"),
                  "Scope channel/group name starts a move without a separate handle");
            check(static_cast<const ChannelMovePayload *>(payload->Data)->isGroup == group,
                  "Name dragging preserves group versus scalar payload identity");
            mouse(draw, ImVec2(-1000, -1000), false); frame(scope);
        };
        dragScope("group", true); dragScope("group.first", false);
        puts("PASS: variable roots/members and monitor groups/channels drag directly by name");
    }

    static void deepMembersAndOverrides() {
        Variable v("focus-deep"); initialize(v);
        const auto expanded = v.vars_[0].expandedMembers;
        frame(v); frame(v);
        check(v.memberPollReqs_.empty(), "collapsed ancestors initially hide deep members");
        v.focusWatch(0, deep);
        std::vector<Variable::WatchFocusNode> nodes;
        check(v.resolveWatchFocus(nodes) == 0 && nodes.size() == 6, "deep focus resolves each breadcrumb through typedefs");
        check(nodes.front().path == "g_foc" && nodes.back().path == deep && nodes.back().addr == base + 64,
              "focus retains canonical full path and accumulated member address");
        frame(v); frame(v);
        check(v.memberPollReqs_.size() == 3, "focused root immediately renders its scalar children without opening ancestors");
        request(v, base + 64, 4); request(v, base + 68, 4); request(v, base + 72, 4);
        check(v.memberValueCache_.at(std::string(deep) + ".kp") == "10.000000" &&
              !v.memberValueCache_.contains("kp"), "focused samples retain canonical paths for edits and monitors");
        check(v.vars_[0].expandedMembers == expanded, "entering focus preserves the ordinary tree expansion state");

        auto &root = v.vars_[0];
        auto &gain = root.memberOverrides[std::string(deep) + ".kp"];
        gain.alias = "Proportional gain"; gain.hasAddress = true; gain.address = 0x30000040;
        gain.hasType = true; gain.type = DataType::I16; gain.hasWritable = true; gain.writable = false;
        root.hiddenMembers.insert(std::string(deep) + ".kd");
        root.expandedMembers.insert(std::string(deep) + ".limits");
        sample(v, gain.address, static_cast<i16>(123));
        frame(v);
        check(v.memberPollReqs_.size() == 4 && !v.memberPollReqs_.contains(base + 64) &&
              !v.memberPollReqs_.contains(base + 72), "focused members honor hidden fields and replacement addresses");
        request(v, gain.address, 2); request(v, base + 80, 4); request(v, base + 84, 4);
        check(v.memberValueCache_.at(std::string(deep) + ".kp") == "123", "focused scalar type override controls decoding");
        const auto focusedRequests = v.memberPollReqs_;
        click(v, "###watch_focus_all");
        check(v.watchFocus_.path.empty(), "All variables navigation exits focus");
        for (const auto &node : nodes) root.expandedMembers.insert(node.path);
        frame(v);
        check(v.memberPollReqs_ == focusedRequests, "normal and focused trees share overrides and nested expansion behavior");
        puts("PASS: deep focus, canonical sampling, effective addresses/types, hidden fields and shared expansion");
    }

    static void navigationAndSession() {
        Variable v("focus-navigation"); initialize(v);
        const auto expanded = v.vars_[0].expandedMembers;
        v.focusWatch(0, deep); frame(v); frame(v);
        click(v, "###watch_focus_back");
        check(v.watchFocus_.path == "g_foc.lo.ctrl.pd.lo", "Back navigates to the parent aggregate");
        auto *window = ImGui::FindWindowByName(host);
        const ImGuiID rootScope = ImHashStr("g_foc", 0, window->ID);
        ImGui::ActivateItemByID(ImHashStr("###crumb", 0, rootScope)); frame(v); frame(v);
        check(v.watchFocus_.path == "g_foc", "breadcrumb navigates directly to the chosen ancestor");
        click(v, "###watch_focus_back");
        check(v.watchFocus_.path.empty(), "Back from root returns to all variables");
        check(v.vars_[0].expandedMembers == expanded, "returning restores the original tree without opening the focus path");
        v.focusWatch(0, deep); frame(v);
        parameter_json::Json saved(cJSON_CreateObject(), cJSON_Delete); v.save(saved.get());
        Variable restored("focus-session"); restored.load(saved.get());
        check(restored.watchFocus_.path.empty() && restored.vars_.size() == 1 &&
              restored.vars_[0].expandedMembers == expanded, "session saves ordinary expansion but not temporary focus");
        click(v, "###watch_focus_all");
        check(v.watchFocus_.path.empty() && v.vars_[0].expandedMembers == expanded, "All variables preserves tree state");
        puts("PASS: Back, ancestor breadcrumb, All variables and temporary focus across session roundtrip");
    }

    static void arrayAndIdentity() {
        Variable v("focus-array"); initialize(v);
        v.focusWatch(0, "g_foc.banks[1].limits");
        std::vector<Variable::WatchFocusNode> nodes;
        check(v.resolveWatchFocus(nodes) == 0 && nodes.back().addr == base + 176,
              "array aggregate focus applies element stride and nested offset");
        auto &element = v.vars_[0].memberOverrides["g_foc.banks[1]"];
        element.alias = "Second bank"; element.hasAddress = true; element.address = 0x30001000;
        check(v.resolveWatchFocus(nodes) == 0 && nodes.back().addr == element.address + 16 &&
              nodes[nodes.size() - 2].label == "Second bank", "array ancestor address and alias overrides apply to focused descendants");
        frame(v); frame(v);
        check(v.memberPollReqs_.size() == 2, "focused array element subtree renders its leaves");
        request(v, element.address + 16, 4); request(v, element.address + 20, 4);

        auto other = v.vars_[0]; other.name = "a_first"; other.addr = base + 0x1000;
        v.vars_.insert(v.vars_.begin(), other);
        check(v.resolveWatchFocus(nodes) == 1 && nodes.back().path == "g_foc.banks[1].limits",
              "focus follows owner identity after insertion or sorting changes indices");
        v.vars_[1].memberOverrides.clear(); v.vars_[1].addr = base + 0x2000;
        v.dwarfInfo_.types[15].members[3].offset = 20;
        check(v.resolveWatchFocus(nodes) == 1 && nodes.back().addr == base + 0x2000 + 180,
              "focus re-resolves current root addresses and DWARF member layout");
        frame(v); request(v, base + 0x2000 + 184, 4);

        auto duplicate = v.vars_[1]; duplicate.addr += 0x1000;
        v.vars_.push_back(duplicate);
        v.focusWatch(1, deep);
        std::swap(v.vars_[0], v.vars_[1]);
        check(v.resolveWatchFocus(nodes) == 0, "same-name owners at distinct addresses remain distinguishable after reorder");
        v.vars_.push_back(v.vars_[0]);
        check(v.resolveWatchFocus(nodes) < 0, "ambiguous duplicate owners do not silently focus the wrong variable");
        puts("PASS: array stride, ancestor overrides, reorder identity, address/layout refresh and duplicate safety");
    }

    static void invalidation() {
        for (int reason = 0; reason != 5; ++reason) {
            Variable v("focus-invalid-" + std::to_string(reason)); initialize(v);
            v.focusWatch(0, deep); frame(v);
            if (reason == 0) v.vars_.clear();
            if (reason == 1) v.vars_[0].hiddenMembers.insert("g_foc.lo.ctrl");
            if (reason == 2) {
                auto &scalar = v.vars_[0].memberOverrides["g_foc.lo.ctrl"];
                scalar.hasType = true; scalar.type = DataType::F32;
            }
            if (reason == 3) v.dwarfInfo_.types[14].members.clear();
            if (reason == 4) v.vars_[0].typeOff = 1;
            std::vector<Variable::WatchFocusNode> nodes;
            check(v.resolveWatchFocus(nodes) < 0, "deleted, hidden, scalar or removed aggregate invalidates focus");
            frame(v);
            check(v.watchFocus_.path.empty(), "invalid focus automatically returns to the ordinary watch list");
        }
        for (const char *path : {"g_foc.banks[3]", "g_foc.lo.ctrl.pd.lo.eff_cfg.kp", "g_foc.missing"}) {
            Variable v("focus-invalid-path"); initialize(v); v.focusWatch(0, path); frame(v);
            check(v.watchFocus_.path.empty(), "out-of-range array, scalar and missing paths cannot become focused roots");
        }
        puts("PASS: deleted/hidden/type-replaced/stale paths safely leave focus and invalid roots are rejected");
    }

    static void unitConversions() {
        using namespace unit_conversion;
        Variable v("conversion-values"); initialize(v);
        auto angle = DefaultSettings(Quantity::AngularPosition);
        angle.enabled = true;
        auto speed = DefaultSettings(Quantity::AngularSpeed);
        speed.enabled = true; speed.source = Unit::CountPerSecond; speed.target = Unit::Rpm;
        speed.countsPerRevolution = 4096;
        v.vars_[0].memberUnitConversions[std::string(deep) + ".kp"] = angle;
        v.vars_[0].memberUnitConversions["g_foc.banks[2].limits.max"] = speed;

        // The same cached bytes used by the table feed the hover, regardless of radix.
        v.vars_[0].displayRadix = 16;
        sample(v, base + 64, 1.234567890123456);
        auto exact = v.unitConversionMemberValue(v.vars_[0], base + 64, DataType::F64);
        check(exact && *exact == 1.234567890123456, "hover retains F64 precision without parsing displayed text");
        sample(v, base + 80, uint16_t(0xF8));
        auto signedBits = v.unitConversionMemberValue(v.vars_[0], base + 80, DataType::I16, 3, 5);
        auto unsignedBits = v.unitConversionMemberValue(v.vars_[0], base + 80, DataType::U16, 3, 5);
        check(signedBits && *signedBits == -1 && unsignedBits && *unsignedBits == 31,
              "hover decodes signed and unsigned bitfields from raw storage");
        sample(v, base + 84, uint16_t(0xBC00));
        check(v.unitConversionMemberValue(v.vars_[0], base + 84, DataType::F16) == -1.0,
              "hover decodes binary16 without formatting loss");
        check(!v.unitConversionMemberValue(v.vars_[0], base + 84, DataType::F64),
              "incomplete raw samples cannot masquerade as a wider scalar");
        sample(v, base + 88, std::numeric_limits<double>::infinity());
        check(!v.unitConversionMemberValue(v.vars_[0], base + 88, DataType::F64),
              "nonfinite cached samples are unavailable to conversion");

        VarEntry scalar{};
        scalar.name = "local_angle"; scalar.port = PortType::LOCAL; scalar.type = DataType::F64;
        scalar.writable = false; scalar.addr = 0; scalar.unitConversion = angle;
        v.vars_.push_back(scalar);
        const double rawAngle = 3.14159265358979323846;
        v.localBufs_[scalar.name].resize(sizeof(rawAngle));
        std::memcpy(v.localBufs_[scalar.name].data(), &rawAngle, sizeof(rawAngle));
        const auto before = v.localBufs_[scalar.name];
        const auto sampleValue = v.unitConversionMemberValue(v.vars_[1], 0, DataType::F64);
        check(sampleValue && std::abs(Convert(*sampleValue, angle).value - 180) < 1e-12,
              "readonly LOCAL numeric buffer converts to degrees");
        check(v.localBufs_[scalar.name] == before, "conversion never changes raw LOCAL bytes");

        VarEntry manual{};
        manual.name = "manual_motor"; manual.port = PortType::LOCAL; manual.type = DataType::U32;
        manual.writable = false; manual.addr = 0; manual.isStruct = true;
        VarEntry::StructField field{};
        snprintf(field.name, sizeof(field.name), "position"); field.type = DataType::F64; field.byteOffset = 8;
        manual.structFields.push_back(field);
        manual.memberUnitConversions["manual_motor.position"] = angle;
        v.localBufs_[manual.name].resize(16);
        std::memcpy(v.localBufs_[manual.name].data() + 8, &rawAngle, sizeof(rawAngle));
        v.vars_.push_back(manual);
        check(v.unitConversionMemberValue(v.vars_[2], 8, DataType::F64) == rawAngle,
              "manual LOCAL members honor their byte offset");
        auto copy = v.vars_[2];
        check(copy.memberUnitConversions.at("manual_motor.position").enabled,
              "copying a variable preserves per-member display configuration");

        parameter_json::Json saved(cJSON_CreateObject(), cJSON_Delete); v.save(saved.get());
        Variable restored("conversion-restored"); restored.load(saved.get());
        auto verify = [&](const Variable &loaded) {
            check(loaded.vars_.size() == 3 && loaded.vars_[1].unitConversion.enabled &&
                  loaded.vars_[1].unitConversion.target == Unit::Degree,
                  "scalar conversion settings persist");
            const auto &members = loaded.vars_[0].memberUnitConversions;
            check(members.at(std::string(deep) + ".kp").source == Unit::Radian &&
                  members.at("g_foc.banks[2].limits.max").target == Unit::Rpm &&
                  members.at("g_foc.banks[2].limits.max").countsPerRevolution == 4096,
                  "deep struct and array paths retain independent conversion settings");
            check(loaded.vars_[2].memberUnitConversions.at("manual_motor.position").enabled,
                  "manual member conversion settings persist");
        };
        verify(restored);
        const auto path = std::filesystem::current_path() / "unit-conversion-roundtrip.var";
        v.exportVarFile(path.string());
        Variable imported("conversion-imported"); imported.importVarFile(path.string());
        verify(imported);
        std::filesystem::remove(path);
        check(v.localBufs_[scalar.name] == before, "persistence leaves the source numeric buffer unchanged");

        // Older sessions contain no conversion key and remain disabled.
        auto *entries = cJSON_GetObjectItem(saved.get(), "vars");
        for (auto *entry = entries->child; entry; entry = entry->next) {
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "unitConversion");
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "memberUnitConversions");
        }
        Variable legacy("conversion-legacy"); legacy.load(saved.get());
        check(!legacy.vars_[1].unitConversion.enabled && legacy.vars_[0].memberUnitConversions.empty(),
              "old sessions load without implicit unit conversion");
        puts("PASS: raw F16/F64/bitfield hover values, readonly LOCAL values, canonical member settings, session/.var persistence and no mutation");
    }

    static void unitConversionEditor() {
        Variable v("unit-editor"); initialize(v);
        const std::string path = std::string(deep) + ".kp";
        v.unitConversionEditOwner_ = v.vars_[0].name;
        v.unitConversionEditPath_ = path;
        v.unitConversionEditDraft_ =
            unit_conversion::DefaultSettings(unit_conversion::Quantity::AngularPosition);
        v.unitConversionEditDraft_.enabled = true;
        v.unitConversionEditRequested_ = true;
        frame(v);
        auto *editor = ImGui::FindWindowByName("Unit conversion###UnitConversionEditor");
        check(editor, "right-click unit conversion editor opens a modal");
        ImGui::ActivateItemByID(editor->GetID("Apply"));
        frame(v);
        frame(v);
        check(v.vars_[0].memberUnitConversions.contains(path) &&
                  v.vars_[0].memberUnitConversions.at(path).enabled,
              "applying the variable settings stores member unit conversion");
        puts("PASS: right-click unit conversion editor persists settings");
    }
};

struct MonitorSearchTestAccess {
    static void hiddenScopeFocus() {
        using Input = VariableRewriteRegression;
        for (auto mode : {MonitorScope::DrawEnum::PLOT, MonitorScope::DrawEnum::TABLE}) {
            Monitor monitor(mode == MonitorScope::DrawEnum::PLOT ? "hidden-plot-focus" : "hidden-table-focus");
            monitor.addScope("visible"); monitor.addScope("hidden");
            auto scope = monitor.scopes_.at("hidden");
            monitor.scopes_.at("visible")->setOrder(0);
            scope->setOrder(1); scope->setDraw(mode); scope->setHidden(true);
            scope->addChannel("motor.position");
            auto frame = [&] {
                ui::interaction::beginFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1200, 800));
                ImGui::Begin("Hidden scope focus host");
                monitor.updateDisplay(true);
                ImGui::End(); ImGui::Render();
            };
            Input::mouse(frame, ImVec2(-1000, -1000), false); frame(); frame();
            ImGuiWindow *scopeWindow = nullptr;
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (!window->Active || !window->ParentWindow || !std::strstr(window->Name, "/##scope_")) continue;
                const void *pointer = scope.get();
                const auto seed = ImHashData(&pointer, sizeof(pointer), window->ParentWindow->ID);
                if (window->ChildId == ImHashStr("##scope", 0, seed)) scopeWindow = window;
            }
            check(scopeWindow, "hidden scope keeps its own toolbar");
            const auto originalHeight = scopeWindow->Size.y;
            const auto point = Input::locate(scopeWindow, scopeWindow->GetID("Focus"), frame).GetCenter();
            for (int i = 0; i < 28; ++i) Input::mouse(frame, point, false);
            check(monitor.scopeFocusPreview_.active() && monitor.isScopeFocused("hidden"),
                  "hidden scope focus starts a preview");
            for (int i = 0; i < 45; ++i) {
                Input::mouse(frame, point, false);
                check(monitor.scopeFocusPreview_.active() && monitor.isScopeFocused("hidden"),
                      "hidden scope preview stays focused without alternating to the overview");
                check(scope->isHidden(), "preview does not change the saved hidden flag");
                check(ImGui::GetCurrentContext()->HoveredIdPreviousFrameItemCount <= 1,
                      "Focus preview never submits a duplicate visible widget ID");
            }
            check(scopeWindow->Size.y > originalHeight * 2, "hidden preview has room for its waveform or table");
            Input::mouse(frame, ImVec2(-1000, -1000), false); frame(); frame();
            check(!monitor.hasFocusedScope() && !monitor.scopeFocusPreview_.active() && scope->isHidden() &&
                      std::abs(scopeWindow->Size.y - originalHeight) < 1,
                  "leaving restores the hidden toolbar and overview");
            for (int i = 0; i < 28; ++i) Input::mouse(frame, point, false);
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); frame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
            Input::mouse(frame, ImVec2(-1000, -1000), false); frame();
            check(!monitor.hasFocusedScope() && scope->isHidden(), "Escape cancels the hidden scope preview");
            for (int i = 0; i < 28; ++i) Input::mouse(frame, point, false);
            Input::mouse(frame, point, true); Input::mouse(frame, point, false);
            Input::mouse(frame, ImVec2(-1000, -1000), false); frame();
            check(monitor.isScopeFocused("hidden") && !monitor.scopeFocusPreview_.active() && !scope->isHidden(),
                  "clicking the preview reveals the hidden scope and commits focus");
            monitor.restoreScopeOverview(); scope->setHidden(true); frame(); frame();
            Input::gesture(frame, point);
            check(monitor.isScopeFocused("hidden") && !scope->isHidden(),
                  "a direct click reveals and focuses a hidden scope without waiting for preview");
        }
        puts("PASS: hidden waveform/table focus preview stability, leave/Escape restoration and click reveal");
    }

    static void run() {
        Monitor monitor("recorder-search-test");
        monitor.addScope("first"); monitor.addScope("second");
        auto scope = monitor.scopes_.at("second");
        scope->addChannel("motor.position"); scope->setHidden(true);
        monitor.scopes_.at("first")->addChannel("other");
        std::snprintf(monitor.channelSearch_, sizeof(monitor.channelSearch_), "%s", "POSITION");
        auto frame = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1200,800));
            ImGui::Begin("Recorder search host");
            monitor.updateDisplay(true);
            ImGui::End(); ImGui::Render();
        };
        frame();
        ImGuiWindow *recorder = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && window->ParentWindow && std::string(window->ParentWindow->Name) == "Recorder search host") recorder = window;
        check(recorder, "recorder child exists");
        recorder->StateStorage.SetBool(recorder->GetID("##monitorSearch"), true);
        frame(); frame();
        ImGuiWindow *results = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::strstr(window->Name, "/results_")) results = window;
        check(results, "recorder search uses inline results including hidden scopes");
        const int index = 0;
        const auto rowId = ImHashData(&index, sizeof(index), results->IDStack.back());
        ImGui::ActivateItemByID(ImHashStr("second / motor.position", 0, rowId));
        frame(); frame();
        check(!scope->isHidden() && scope->getDraw() == MonitorScope::DrawEnum::TABLE,
              "result selection reveals the hidden scope in table mode");
        check(scope->getExpandedGroups().contains("motor"), "result selection expands collapsed ancestors");
        check(monitor.scopes_.size() == 2 && !monitor.scopes_.at("first")->isHidden(),
              "search navigation preserves unrelated scopes");
        puts("PASS: recorder inline search, case-insensitive match, native result activation and hidden-scope reveal");
    }
};

int main(int, char **) {
    module_init(); ImGui::CreateContext(); ImPlot::CreateContext();
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1500, 900); io.DeltaTime = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto language = g_lang; g_lang = Lang::EN;
    int result = 0;
    try {
        MonitorSearchTestAccess::hiddenScopeFocus();
        MonitorSearchTestAccess::run();
        VariableRewriteRegression::watchSearch();
        VariableRewriteRegression::hoverFocusAndChangeFlash();
        VariableRewriteRegression::variableMouseFocus();
        VariableRewriteRegression::monitorMouseFocusAndEditing();
        VariableRewriteRegression::hoverPanelResize();
        VariableRewriteRegression::scopeTitleAlignment();
        VariableRewriteRegression::nameDragging();
        VariableRewriteRegression::deepMembersAndOverrides();
        VariableRewriteRegression::navigationAndSession();
        VariableRewriteRegression::arrayAndIdentity();
        VariableRewriteRegression::invalidation();
        VariableRewriteRegression::unitConversions();
        VariableRewriteRegression::unitConversionEditor();
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
    g_lang = language; ImPlot::DestroyContext(); ImGui::DestroyContext(); log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
