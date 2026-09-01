#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/choice_control.hpp"
#include "imgui_internal.h"
#include <stdexcept>

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}

struct VariableRewriteRegression {
        static void prepare(Variable &v)
        {
                v.cfgPath_           = "symbol-fixture.json";
                auto &type           = v.dwarfInfo_.types[1];
                type.kind            = dwarf::TypeKind::BASE;
                type.name            = "unsigned int";
                type.size            = 4;
                type.encoding        = 7;
                v.dwarfInfo_.present = true;
                for (int i = 0; i < 650; ++i) {
                        dwarf::Variable symbol{};
                        char            name[40];
                        snprintf(name, sizeof(name), "symbol_%04d", i);
                        symbol.name = name;
                        symbol.type = 1;
                        symbol.addr = 0x20000000 + i * 4;
                        v.dwarfInfo_.variables.push_back(symbol);
                }
                v.rebuildSearchPool();
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "SYMBOL_");
        }
        static void verifySearch(Variable &v)
        {
                check(v.refreshSymbolSearch() && v.searchResults_.size() == 650,
                      "search includes all case-insensitive matches");
                check(!v.refreshSymbolSearch(), "unchanged query is cached");
                v.dwarfInfo_.variables.back().addr = 0x20010000;
                v.rebuildSearchPool();
                check(v.refreshSymbolSearch() && v.searchResults_.back().addr == 0x20010000,
                      "same text refreshes when symbols reload, including a same-size pool");
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "0x20010000");
                check(v.refreshSymbolSearch() && v.searchResults_.size() == 1 &&
                          v.searchResults_.front().addr == 0x20010000,
                      "symbol search resolves an exact memory address");
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "0649");
                check(v.refreshSymbolSearch() && v.searchResults_.size() == 1, "query narrows without clearing first");
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "missing");
                check(v.refreshSymbolSearch() && v.searchResults_.empty(), "no matches clear previous rows");
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "symbol_");
                v.refreshSymbolSearch();
        }
        static void draw(Variable &v) { v.draw(); }
        static void pin(Variable &v) { v.symbolBrowserHover_.setPinned(true); }
        static bool isPinned(Variable &v) { return v.symbolBrowserHover_.isPinned(); }
        static bool isOpen(Variable &v) { return v.symbolBrowserHover_.isOpen(); }
        static void close(Variable &v) { v.symbolBrowserHover_.close(); }
        static void search(Variable &v, const char *query) {
                snprintf(v.searchBuf_, sizeof(v.searchBuf_), "%s", query);
        }
        static size_t results(const Variable &v) { return v.searchResults_.size(); }
        static void verifySave(Variable &v)
        {
                v.cfgPath_.clear();
                parameter_json::Json state(cJSON_CreateObject(), cJSON_Delete);
                v.save(state.get());
                Variable restored("browser-restored");
                restored.load(state.get());
                check(isPinned(restored), "pinned symbol browser survives session restore");
        }
        static void prepareParameters(Variable &v)
        {
                parameter_json::Document doc;
                doc.name   = "PSC3_GAN";
                doc.target = "g_mc.cfg";
                for (const char *section : parameter_json::componentSections) {
                        parameter_json::Field field;
                        field.row.path = std::string(section) + ".value";
                        field.row.type = "uint32_t";
                        field.row.val  = "1";
                        doc.fields.push_back(field);
                }
                parameter_json::Field shared;
                shared.row.path = "bkp.enable"; shared.row.type = "uint32_t"; shared.row.val = "1";
                doc.fields.push_back(shared);
                v.parameterFile_.load(parameter_json::format(doc));
                v.parameterFileOpen_    = true;
                v.parameterReadPending_ = false;
        }
        static void selectParameterTab(Variable &v, int tab) { v.parameterNavigateTab_ = tab; }
        static void verifyCommonPersistence(Variable &v) {
                parameter_json::Json saved(cJSON_CreateObject(), cJSON_Delete); v.save(saved.get());
                Variable restored("common-restored"); restored.load(saved.get());
                check(restored.parameterComponentTab_ == 3, "Common parameter page survives session restore");
        }
        static void drawParameters(Variable &v) { v.drawParameterFileEditor(); }
        static void filterTarget(Variable &v) { snprintf(v.parameterTargetSearch_, sizeof(v.parameterTargetSearch_), "0649"); }
        static const std::string &target(const Variable &v) { return v.parameterFile_.document.target; }
};

static ImGuiWindow *
findWindow(const char *part)
{
        for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && std::strstr(window->Name, part))
                        return window;
        return nullptr;
}

int
main()
{
        module_init();
        g_lang = Lang::EN;
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = ImVec2(1000, 760);
        io.DeltaTime                    = .05f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels;
        int            width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try {
                int          selected        = 0;
                bool         writable        = false;
                bool         accessActivated = false;
                float        fixtureWidth    = 700;
                ImRect       choiceRect, accessRect;
                ImGuiWindow *window   = nullptr;
                const char  *labels[] = {"Decimal", "Hexadecimal", "Octal", "Binary"};
                auto         frame    = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(fixtureWidth, 260));
                        ImGui::Begin("Choices", nullptr, ImGuiWindowFlags_NoSavedSettings);
                        window = ImGui::GetCurrentWindow();
                        ui::Choice("##format", &selected, labels, 4);
                        choiceRect      = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        accessActivated = ui::AccessChoice("##access", writable, false);
                        accessRect      = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        ImGui::End();
                        ImGui::Render();
                };
                auto rect = [&](const char *group, int index, const char *label) {
                        const auto seed     = window->GetID(group);
                        const auto itemSeed = ImHashData(&index, sizeof(index), seed);
                        ImGui::SetFocusID(ImHashStr(label, 0, itemSeed), window);
                        frame();
                        return ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
                };
                frame();
                frame();
                const auto decimal = rect("##format", 0, labels[0]);
                const auto hex     = rect("##format", 1, labels[1]);
                check(std::abs(decimal.Min.y - hex.Min.y) < 1 && hex.Min.x > decimal.Max.x,
                      "fixed options occupy one horizontal row when space is available");
                frame(hex.GetCenter());
                frame(hex.GetCenter(), true);
                frame(hex.GetCenter(), false);
                check(selected == 1, "clicking a segment selects exactly one option");
                // Disabled write access must not be enabled by clicking its visual segment.
                const ImVec2 disabledWrite(accessRect.Max.x - ImGui::CalcTextSize("Read / write").x * .5f,
                                           accessRect.GetCenter().y);
                frame(disabledWrite);
                frame(disabledWrite, true);
                frame(disabledWrite, false);
                check(!writable, "disabled write access stays disabled");
                const ImVec2 readOnly(accessRect.Min.x + ImGui::CalcTextSize("Read only").x * .5f, accessRect.GetCenter().y);
                frame(readOnly);
                frame(readOnly, true);
                frame(readOnly, false);
                check(accessActivated && !writable,
                      "selecting the current access mode still applies to mixed selections and inherited members");
                fixtureWidth = 155;
                frame();
                frame();
                check(choiceRect.GetHeight() > ImGui::GetFrameHeight() && choiceRect.Max.x <= window->WorkRect.Max.x + 1,
                      "narrow option groups wrap inside the panel");

                Variable browser("browser-fixture");
                VariableRewriteRegression::prepare(browser);
                VariableRewriteRegression::verifySearch(browser);
                auto browserFrame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(1000, 740));
                        ImGui::Begin("Browser fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
                        VariableRewriteRegression::draw(browser);
                        ImGui::End();
                        ImGui::Render();
                };
                browserFrame();
                browserFrame();
                auto *top = findWindow("/TopSection");
                check(top, "watch table owns the full panel");
                const auto triggerId = top->GetID("Symbol File###symbol_file_toggle");
                ImGui::SetFocusID(triggerId, top);
                browserFrame();
                const auto trigger = ImGui::WindowRectRelToAbs(top, top->NavRectRel[ImGuiNavLayer_Main]);
                check(trigger.Min.y < top->Pos.y + ImGui::GetFrameHeight() * 3, "symbol browser trigger is in the top toolbar");
                for (int i = 0; i < 10; ++i)
                        browserFrame(trigger.GetCenter());
                check(!VariableRewriteRegression::isOpen(browser), "Symbol browser stays closed on hover");
                browserFrame(trigger.GetCenter(), true);
                browserFrame(trigger.GetCenter(), false);
                check(VariableRewriteRegression::isPinned(browser), "click opens the browser");
                for (int i = 0; i < 4; ++i)
                        browserFrame();
                auto *section = findWindow("/SymbolBrowserSection");
                check(section, "hover browser owns a bounded scrolling region");
                auto *table = ImGui::TableFindByID(section->GetID("SymbolSearchTableV2"));
                check(table && table->InnerWindow->ScrollMax.y > 0, "symbol search table scrolls");
                ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
                browserFrame();
                browserFrame();
                check(table->RowPosY1 >= table->InnerWindow->InnerClipRect.Min.y &&
                          table->RowPosY2 <= table->InnerWindow->InnerClipRect.Max.y + 1,
                      "the final symbol row is fully visible at the bottom");
                check(table->OuterRect.Max.y <= section->InnerClipRect.Max.y + 1,
                      "symbol table stays within the hover panel bounds");
                VariableRewriteRegression::search(browser, "symbol_0649");
                browserFrame();
                browserFrame();
                check(VariableRewriteRegression::results(browser) == 1, "single-symbol search fixture is visible");
                const ImVec2 symbolRow(table->WorkRect.Min.x + ImGui::GetFontSize() * 4,
                                       (table->RowPosY1 + table->RowPosY2) * .5f);
                browserFrame(symbolRow, true);
                browserFrame(symbolRow, false);
                browserFrame(symbolRow, true);
                browserFrame(symbolRow, false);
                check(browser.vars_.size() == 1 && browser.vars_.front().name == "symbol_0649",
                      "double-clicking a symbol in the browser adds it to the variable manager");
                const u64      symbolAddress = browser.vars_.front().addr;
                const DataType symbolType    = browser.vars_.front().type;
                const DataType differentType = symbolType == DataType::U32 ? DataType::I32 : DataType::U32;
                check(browser.watchHasIdentity("symbol_0649", symbolAddress, symbolType) &&
                          !browser.watchHasIdentity("symbol_0649", symbolAddress + 4, symbolType) &&
                          !browser.watchHasIdentity("symbol_0649", symbolAddress, differentType),
                      "variable identity includes name, address and type");
                browserFrame(symbolRow, true);
                browserFrame(symbolRow, false);
                browserFrame(symbolRow, true);
                browserFrame(symbolRow, false);
                check(browser.vars_.size() == 1, "double-click does not duplicate an existing variable");
                VariableRewriteRegression::verifySave(browser);
                VariableRewriteRegression::close(browser);
                VariableRewriteRegression::prepareParameters(browser);
                float parameterWidth = 1100;
                auto  parameterFrame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.DisplaySize = ImVec2(1200, 760);
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame();
                        if (auto *pane =
                                ImGui::FindWindowByName("Parameter Configuration###ParameterFileEditorbrowser-fixture")) {
                                ImGui::SetWindowPos(pane, ImVec2(0, 0));
                                ImGui::SetWindowSize(pane, ImVec2(parameterWidth, 650));
                        }
                        VariableRewriteRegression::drawParameters(browser);
                        ImGui::Render();
                };
                parameterFrame();
                parameterFrame();
                auto *parameterWindow = findWindow("###ParameterFileEditorbrowser-fixture");
                check(parameterWindow, "parameter editor renders");
                auto parameterRect = [&](const char *label) {
                        ImGui::SetFocusID(parameterWindow->GetID(label), parameterWindow);
                        parameterFrame();
                        return ImGui::WindowRectRelToAbs(parameterWindow, parameterWindow->NavRectRel[ImGuiNavLayer_Main]);
                };
                const auto presetRect  = parameterRect("###parameter_configuration");
                const auto saveRect    = parameterRect("Save");
                const auto restoreRect = parameterRect("Restore default parameters");
                check(std::abs(presetRect.Min.y - saveRect.Min.y) < 1 && std::abs(saveRect.Min.y - restoreRect.Min.y) < 1,
                      "presets and parameter actions share one wide toolbar row");
                parameterWidth = 420;
                parameterFrame();
                parameterFrame();
                for (const char *label :
                     {"###parameter_configuration", "Save", "Save as...", "Restore default parameters", "Write parameters"}) {
                        const auto item = parameterRect(label);
                        check(item.Min.x >= parameterWindow->WorkRect.Min.x - 1 &&
                                  item.Max.x <= parameterWindow->WorkRect.Max.x + 1,
                              "parameter actions wrap within a narrow panel");
                }
                parameterWidth = 1100;
                parameterFrame(); parameterFrame();
                const auto targetTrigger = parameterRect("###parameter_target_picker");
                VariableRewriteRegression::filterTarget(browser);
                for (int i = 0; i < 20; ++i) parameterFrame(targetTrigger.GetCenter());
                auto *targetPanel = findWindow("##ParameterTarget_");
                check(!targetPanel || !targetPanel->Active, "Target selector does not open on hover");
                parameterFrame(targetTrigger.GetCenter(), true); parameterFrame(targetTrigger.GetCenter(), false);
                targetPanel = findWindow("##ParameterTarget_");
                auto *targetList = findWindow("##target_list");
                check(targetPanel && targetPanel->Active && targetList && targetList->Active,
                      "Target selector opens on click and lists manager variables");
                ImGui::SetFocusID(targetList->GetID("symbol_0649"), targetList);
                parameterFrame(targetTrigger.GetCenter());
                const auto targetChoice = ImGui::WindowRectRelToAbs(targetList, targetList->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                parameterFrame(targetChoice); parameterFrame(targetChoice, true); parameterFrame(targetChoice, false);
                check(VariableRewriteRegression::target(browser) == "symbol_0649",
                      "Clicking a searched manager variable applies the target without text entry or a separate Apply button");
                auto parameterTable = [&](int tab) {
                        ImGuiID id = ImHashData(&tab, sizeof(tab), parameterWindow->ID);
                        return ImGui::TableFindByID(ImHashStr("##parameter_file", 0, id));
                };
                for (int tab = 0; tab < 4; ++tab) {
                        VariableRewriteRegression::selectParameterTab(browser, tab);
                        parameterFrame(); parameterFrame();
                        auto *fields = parameterTable(tab);
                        check(fields && fields->CurrentRow == (tab == 3 ? 2 : 1), "each parameter page contains only its own field or shared group");
                }
                VariableRewriteRegression::verifyCommonPersistence(browser);
                std::puts("PASS: segmented choices, disabled write access, narrow wrapping, unchanged-query reload, all search "
                          "results, address lookup, double-click add without duplicates and click-open browser");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                result = 1;
        }
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        log_deinit(&g_log_ops);
        log_deinit(&g_log_perf);
        return result;
}
