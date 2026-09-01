#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include "gui/value_clipboard.hpp"
#include <stdexcept>

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}
static std::string clipboard;

struct VariableRewriteRegression {
        static void prepare(Variable &v)
        {
                v.updateIntervalMs_ = 0;
                dwarf::Type f;
                f.kind                = dwarf::TypeKind::BASE;
                f.size                = 4;
                f.encoding            = 4;
                f.name                = "float";
                v.dwarfInfo_.types[1] = f;
                f.size                = 8;
                f.name                = "double";
                v.dwarfInfo_.types[2] = f;
                f.size                = 2;
                f.name                = "_Float16";
                v.dwarfInfo_.types[3] = f;
                f.size                = 4;
                f.encoding            = 7;
                f.name                = "unsigned int";
                v.dwarfInfo_.types[4] = f;
                dwarf::Type array;
                array.kind            = dwarf::TypeKind::ARRAY;
                array.inner           = 4;
                array.dims            = {2};
                array.size            = 8;
                v.dwarfInfo_.types[5] = array;
                dwarf::Type child;
                child.kind            = dwarf::TypeKind::STRUCT;
                child.size            = 10;
                child.members         = {{"exact", 0, 2}, {"half", 8, 3}};
                v.dwarfInfo_.types[6] = child;
                dwarf::Type object;
                object.kind           = dwarf::TypeKind::STRUCT;
                object.size           = 24;
                object.members        = {{"f", 0, 1}, {"numbers", 4, 5}, {"child", 12, 6}};
                v.dwarfInfo_.types[7] = object;
                VarEntry row{};
                row.name                                   = "obj";
                row.port                                   = PortType::LOCAL;
                row.type                                   = DataType::U32;
                row.typeOff                                = 7;
                row.isStruct                               = true;
                row.displayRadix                           = 16;
                row.memberDisplayRadices["obj.numbers[0]"] = 10;
                row.memberDisplayRadices["obj.child.half"] = 2;
                row.hiddenMembers.insert("obj.numbers[1]");
                v.vars_.push_back(row);
                auto &bytes = v.localBufs_[row.name];
                bytes.resize(24);
                const u32 single = 0x3f800000, first = 42, second = 255;
                const u64 exact = 0x3ff0000000000001;
                const u16 half  = 0x8000;
                std::memcpy(bytes.data(), &single, 4);
                std::memcpy(bytes.data() + 4, &first, 4);
                std::memcpy(bytes.data() + 8, &second, 4);
                std::memcpy(bytes.data() + 12, &exact, 8);
                std::memcpy(bytes.data() + 20, &half, 2);
        }
        static std::string copy(Variable &v, const std::string &path) { return v.watchClipboardText(0, path); }
        static void        actions(Variable &v, const std::string &path) { v.drawWatchValueActions(0, path); }
        static void        flush(Variable &v) { v.updateWatchCopy(); }
        static void        verify(Variable &v)
        {
                std::string escaped;
                ui::appendClipboardRow(escaped, "obj,a", "a\"b\nc", "0x1234");
                check(escaped == "\"obj,a\",\"a\"\"b\nc\",0x1234\r\n", "CSV escapes commas, quotes and embedded newlines");
                const auto all = copy(v, "obj");
                check(all.find("obj.f,0x3F800000,\r\n") != std::string::npos, "float bit pattern and absent LOCAL address");
                check(all.find("obj.numbers[0],42,\r\n") != std::string::npos, "array child has independent decimal format");
                check(all.find("obj.numbers[1],0x000000FF,\r\n") != std::string::npos,
                      "collapsed and hidden elements are copied");
                check(all.find("obj.child.exact,0x3FF0000000000001") != std::string::npos, "binary64 copy preserves all bits");
                check(all.find("obj.child.half,0b1000000000000000") != std::string::npos,
                      "binary16 negative zero is preserved");
                check(copy(v, "obj.numbers").find("obj.f,") == std::string::npos, "subtree copy excludes sibling variables");
                check(copy(v, "obj.numbers[1]").find("obj.numbers[0]") == std::string::npos, "element copy is independent");
                v.vars_[0].displayRadix = 8;
                check(v.vars_[0].radixForPath("obj.child.half") == 2 && v.vars_[0].radixForPath("obj.numbers[0]") == 10,
                      "parent display changes preserve explicit child formats");
                v.vars_[0].displayRadix = 16;
                parameter_json::Json state(cJSON_CreateObject(), cJSON_Delete);
                v.save(state.get());
                Variable loaded("format-restore");
                loaded.load(state.get());
                check(loaded.vars_[0].memberDisplayRadices == v.vars_[0].memberDisplayRadices,
                      "child formats survive session reload");
                auto *root = cJSON_GetObjectItem(state.get(), "vars")->child;
                cJSON_ReplaceItemInObject(root, "displayRadix", cJSON_CreateNumber(0));
                cJSON_ReplaceItemInObject(state.get(), "displayRadix", cJSON_CreateNumber(8));
                loaded.load(state.get());
                loaded.displayRadix_ = 16;
                check(loaded.variableRadix(loaded.vars_[0]) == 8,
                      "legacy follow-manager setting is migrated to an explicit format");

                auto &owner = v.vars_[0];
                owner.port  = PortType::JLINK;
                owner.addr  = 0x20000000;
                for (auto [offset, size] : {std::pair{0u, 4u}, {4u, 4u}, {8u, 4u}, {12u, 8u}, {20u, 2u}}) {
                        auto &sample = v.syncReadCache_[owner.addr + offset];
                        sample.ok    = true;
                        sample.sz    = size;
                        std::copy_n(v.localBufs_[owner.name].data() + offset, size, sample.buf);
                }
                check(copy(v, "obj.child.exact").find("0x2000000C") != std::string::npos,
                      "member copy includes its own absolute address");
                v.syncReadCache_.erase(owner.addr + 12);
                bool waiting = false;
                v.watchClipboardText(0, "obj.child", &waiting);
                check(waiting && v.pendingCopyRequests_[owner.addr + 12] == 8,
                      "copy queues missing collapsed members for background reads");
                v.pendingCopyRequests_.clear();
                owner.addrUnknown = true;
                check(copy(v, "obj.f").find("UNKNOWN,\r\n") != std::string::npos,
                      "unresolved addresses and values are not fabricated");
                owner.addrUnknown = false;
                owner.port        = PortType::LOCAL;
                owner.addr        = 0;

                Variable scalar("float-raw");
                scalar.updateIntervalMs_ = 0;
                VarEntry value{};
                value.name         = "nan";
                value.type         = DataType::F64;
                value.port         = PortType::LOCAL;
                value.displayRadix = 16;
                scalar.vars_.push_back(value);
                const u64 payload = 0x7ff8000000000042;
                scalar.localBufs_[value.name].resize(8);
                std::memcpy(scalar.localBufs_[value.name].data(), &payload, 8);
                scalar.updateVariables();
                check(scalar.vars_[0].valueStr == "0x7FF8000000000042", "NaN payload bits are not lost during formatting");
                check(scalar.localBufs_[value.name][0] == 0x42, "formatting leaves source bytes unchanged");
        }
};

struct ScopeInteractionTestAccess {
        static void        focus(MonitorScope &scope) { scope.focusTableGroup("array"); }
        static bool        focused(MonitorScope &scope) { return scope.tableFocusPath_ == "array"; }
        static std::string copy(MonitorScope &scope) { return scope.clipboardText({"obj.exact", "obj.count"}); }
        static void        verifyEnumDropdown()
        {
                MonitorScope scope("enum-dropdown");
                scope.addChannel("mode");
                auto *channel = scope.findChannel("mode");
                channel->setDevice("LOCAL");
                channel->setType("i16");
                channel->setEnums({{"Off", -1}, {"Idle", 0}, {"Run", 7}});
                channel->setRVal(-1, 0);
                i16 raw = -1;
                channel->setLatestRawBytes(&raw, sizeof(raw));
                channel->publishSnapshot();
                auto &io    = ImGui::GetIO();
                auto  frame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(800, 500));
                        ImGui::Begin("Enum table fixture");
                        scope.tableDraw();
                        ImGui::End();
                        ImGui::Render();
                };
                frame();
                frame();
                auto *window = ImGui::FindWindowByName("Enum table fixture");
                auto *table  = ImGui::TableFindByID(window->GetID("MonitorTable"));
                check(table, "enum table renders");
                auto seed = ImHashStr("mode", 0, table->ID);
                const auto enumId = ImHashStr("##enum", 0, seed);
                const auto locate = [&](ImGuiWindow *owner, ImGuiID id) {
                        ImGui::SetFocusID(id, owner);
                        owner->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
                        frame();
                        const auto rect = ImGui::WindowRectRelToAbs(owner, owner->NavRectRel[ImGuiNavLayer_Main]);
                        check(rect.Min.x >= 0 && rect.Min.y >= 0, "enum control is visible");
                        return rect;
                };
                const auto click = [&](ImVec2 point) {
                        frame(point); frame(point, true); frame(point, false);
                };
                const auto combo = locate(table->InnerWindow, enumId);
                const float rowHeight = table->RowPosY2 - table->RowPosY1;
                auto entries = channel->getEnums();
                for (int i = 3; i < 40; ++i)
                        entries.push_back({"CIA402_MODE_LONG_NAME_" + std::to_string(i), i});
                channel->setEnums(entries);
                frame(); frame();
                check(std::abs(table->RowPosY2 - table->RowPosY1 - rowHeight) < 1,
                      "many enum values keep the same single-row height");
                f32 written = 0;
                const auto chooseRun = [&] {
                        click(combo.GetCenter()); frame(combo.GetCenter()); frame(combo.GetCenter());
                        check(!ImGui::GetCurrentContext()->OpenPopupStack.empty(), "enum arrow opens a dropdown");
                        auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                        check(popup && popup->ScrollbarY && popup->Size.y < io.DisplaySize.y,
                              "long enum lists scroll within a bounded dropdown");
                        check(!channel->consumeWValDirty(written), "opening the enum dropdown never writes to the device");
                        const int runIndex = 2;
                        const auto id = ImHashStr("Run (7)", 0, ImHashData(&runIndex, sizeof(runIndex), popup->ID));
                        const auto option = locate(popup, id);
                        click(option.GetCenter());
                        check(ImGui::GetCurrentContext()->OpenPopupStack.empty(), "selecting an enum closes the dropdown");
                };
                check(!channel->consumeWValDirty(written), "rendering an enum does not write");
                chooseRun();
                check(channel->consumeWValDirty(written) && written == 7 && channel->getDispVal() == -1,
                      "dropdown selection writes its mapped value without changing readback");
                raw = 7;
                channel->setRVal(7, 1);
                channel->setLatestRawBytes(&raw, sizeof(raw));
                channel->publishSnapshot();
                chooseRun();
                check(channel->consumeWValDirty(written) && written == 7, "current enum value can be explicitly written again");
                channel->setWritable(false);
                frame(); click(combo.GetCenter());
                check(!channel->consumeWValDirty(written) && ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                      "read-only enum values cannot open a write control or queue a write");

        }
        static void verify()
        {
                MonitorScope scope("copy-monitor");
                scope.addChannel("obj.exact");
                scope.addChannel("obj.count");
                auto *f = scope.findChannel("obj.exact");
                f->setType("f64");
                f->setDevice("JLINK");
                f->setAddr(0x20000000);
                f->setDisplayRadix(16);
                f->setRVal(1, 0);
                const u64 first = 0x3ff0000000000001;
                f->setLatestRawBytes(&first, 8);
                f->publishSnapshot();
                auto *integer = scope.findChannel("obj.count");
                integer->setType("u64");
                integer->setDevice("LOCAL");
                integer->setRVal(0, 0);
                const u64 maximum = UINT64_MAX;
                integer->setLatestRawBytes(&maximum, 8);
                integer->publishSnapshot();
                check(copy(scope).find("obj.exact,0x3FF0000000000001,0x20000000") != std::string::npos,
                      "monitor copy keeps exact f64 bits and address");
                check(copy(scope).find("obj.count,18446744073709551615,\r\n") != std::string::npos,
                      "monitor decimal copy preserves uint64 precision");
                f->setRVal(2, 1);
                const u64 next = 0x4000000000000001;
                f->setLatestRawBytes(&next, 8);
                check(copy(scope).find("0x3FF0000000000001") != std::string::npos, "display freeze also freezes raw bits");
                f->publishSnapshot();
                check(copy(scope).find("0x4000000000000001") != std::string::npos,
                      "raw bits advance with the published snapshot");
                u64        bits  = 0;
                const auto clone = f->cloneWithHistory();
                check(clone->getDisplayBits(bits) && bits == next, "cloning preserves raw display data");
                f->setType("f32");
                check(!f->getDisplayBits(bits), "changing type invalidates old raw bits");
                MonitorScope source("array-source"), destination("array-destination");
                source.addChannel("array[0]");
                source.addChannel("array[1]");
                source.addChannel("array2[0]");
                ChannelMovePayload payload{};
                payload.srcScope = &source;
                payload.isGroup  = true;
                snprintf(payload.chName, sizeof(payload.chName), "array");
                check(destination.moveChannelsFrom(&payload, true) && destination.getChannels().size() == 2 &&
                          source.getChannels().size() == 3,
                      "copying array groups respects index path boundaries");
        }
};

int
main()
{
        module_init();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = ImVec2(800, 600);
        io.DeltaTime                    = .025f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels;
        int            w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) { clipboard = text; };
        ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext *) { return clipboard.c_str(); };
        int result                                         = 0;
        try {
                Variable v("copy-format");
                VariableRewriteRegression::prepare(v);
                VariableRewriteRegression::verify(v);
                ScopeInteractionTestAccess::verify();
                ScopeInteractionTestAccess::verifyEnumDropdown();
                MonitorScope array("array-view");
                array.setDraw(MonitorScope::DrawEnum::TABLE);
                array.addChannel("array[0]");
                array.addChannel("array[1]");
                ScopeInteractionTestAccess::focus(array);
                MonitorViewMode mode = MonitorViewMode::FULL;
                for (int i = 0; i < 3; ++i) {
                        ImGui::NewFrame();
                        ImGui::SetNextWindowSize(ImVec2(700, 400));
                        ImGui::Begin("Array fixture");
                        array.draw(nullptr, nullptr, 1000, mode);
                        ImGui::End();
                        ImGui::Render();
                }
                check(ScopeInteractionTestAccess::focused(array), "array elements form a focusable monitor group");
                bool         open  = true;
                ImGuiWindow *menu  = nullptr;
                auto         frame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        io.AddMouseButtonEvent(0, down);
                        ImGui::NewFrame();
                        VariableRewriteRegression::flush(v);
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(800, 600));
                        ImGui::Begin("Copy menu fixture");
                        if (open) {
                                ImGui::OpenPopup("copy_menu");
                                open = false;
                        }
                        if (ImGui::BeginPopup("copy_menu")) {
                                menu = ImGui::GetCurrentWindow();
                                VariableRewriteRegression::actions(v, "obj.numbers");
                                ImGui::EndPopup();
                        }
                        ImGui::End();
                        ImGui::Render();
                };
                for (int i = 0; i < 4; ++i)
                        frame();
                check(menu && menu->Active, "real variable context actions render");
                const auto id = menu->GetID(tr("Copy", "复制"));
                ImGui::SetFocusID(id, menu);
                frame();
                const auto button = ImGui::WindowRectRelToAbs(menu, menu->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
                frame(button);
                frame(button, true);
                frame(button, false);
                frame();
                check(clipboard.find("obj.numbers[0],42") != std::string::npos &&
                          clipboard.find("obj.f,") == std::string::npos,
                      "one click Copy writes the selected subtree to the clipboard");
                std::puts("PASS: recursive copy, addresses, independent child formats, persistence, IEEE 754 bits, exact "
                          "integers, frozen raw snapshots and native Copy action");
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
