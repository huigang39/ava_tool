#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/platform_style.hpp"
#include "imgui_internal.h"
#include "implot.h"
#include <stdexcept>

static void check(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

static Json snapshot(const DeviceManager &manager)
{
        Json result(cJSON_CreateObject(), cJSON_Delete);
        manager.save(result.get(), ".");
        return result;
}

static const cJSON *field(const cJSON *object, const char *name)
{
        const auto *value = cJSON_GetObjectItemCaseSensitive(object, name);
        check(value != nullptr, "serialized device field exists");
        return value;
}

static const char *stringField(const cJSON *object, const char *name)
{
        const auto *value = field(object, name);
        check(cJSON_IsString(value), "serialized device field is a string");
        return value->valuestring;
}

static void frame(DeviceManager &manager, ImVec2 size)
{
        ImGui::GetIO().DisplaySize = size;
        ImGui::NewFrame();
        if (auto *window = ImGui::FindWindowByName("Device Manager###DeviceManager")) {
                ImGui::SetWindowPos(window, ImVec2(0, 0), ImGuiCond_Always);
                ImGui::SetWindowSize(window, size, ImGuiCond_Always);
        }
        manager.draw();
        ImGui::Render();
}

static ImGuiWindow *child(const char *prefix)
{
        for (auto *window : ImGui::GetCurrentContext()->Windows) {
                const std::string name = window->Name;
                if (window->Active && name.substr(name.rfind('/') + 1).starts_with(prefix))
                        return window;
        }
        return nullptr;
}

static ImRect locate(DeviceManager &manager, ImVec2 size, ImGuiWindow *window, ImGuiID id)
{
        check(window && window->Active, "device interaction surface is active");
        ImGui::SetFocusID(id, window);
        window->NavRectRel[ImGuiNavLayer_Main] = ImRect(-1000, -1000, -999, -999);
        frame(manager, size);
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        check(rect.Min.x >= 0 && rect.Min.y >= 0, "device interaction target is submitted");
        return rect;
}

static void click(DeviceManager &manager, ImVec2 size, ImVec2 point)
{
        for (bool down : {false, true, false}) {
                ImGui::GetIO().AddMousePosEvent(point.x, point.y);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
                frame(manager, size);
        }
        frame(manager, size);
}

int main(int, char **)
{
        module_init();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char *pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try {
                // LOCAL/manual fixtures only: no target address, SDK library, or hardware call.
                Json fixture(cJSON_Parse(R"json({
                    "version":3,"open":true,"selectedDeviceTypeId":"10","selectedInstanceId":"0",
                    "deviceTypes":[
                      {"id":"10","name":"Motor controller with a deliberately long descriptive name",
                       "properties":[{"id":"1","name":"Exact unsigned feedback accumulator value",
                         "type":"u64","value":"18446744073709551615","source":"manual"}],
                       "methods":[],"instances":[{"id":"11","name":"Bench motor A",
                         "values":[{"propertyId":"1","value":"42","source":"manual"}]}]},
                      {"id":"20","name":"Oscilloscope","properties":[],"methods":[],"instances":[]}
                    ]})json"), cJSON_Delete);
                DeviceManager manager;
                check(fixture && manager.load(fixture.get(), "."), "manual device fixture loads");
                int openedSdk = 0;
                manager.setSdkCatalog({{77, "Fixture SDK", "not-loaded-test-sdk.dll", "", false, 3}});
                manager.setOpenSdkHandler([&](int id) { openedSdk = id; });
                manager.clearModified();
                auto before = snapshot(manager);
                const ImVec2 wide(1400, 900), narrow(480, 800);
                ui::ApplyPlatformStyle(ui::PlatformStyle::Windows, 1, false, true);
                for (int i = 0; i < 4; ++i)
                        frame(manager, wide);
                check(openedSdk == 0, "browsing catalog never opens or invokes an SDK automatically");
                auto *sdkCard = child("##sdk_catalog_card");
                check(sdkCard, "SDK metadata appears as a device catalog card");
                const auto openSdk = locate(manager, wide, sdkCard, sdkCard->GetID("Open SDK controls"));
                click(manager, wide, openSdk.GetCenter());
                check(openedSdk == 77, "explicit SDK card action routes the exact caller identity");
                auto *library = child("##device_list");
                check(library && child("##device_detail"), "wide device view shows library and detail together");
                const ImGuiID type10 = library->GetID("device_type_10");
                const auto templateRect = locate(manager, wide, library, ImHashStr("###type_node", 0, type10));
                const auto instanceRect = locate(manager, wide, library,
                                                 ImHashStr("###instance_node", 0, ImHashStr("instance_11", 0, type10)));
                check(templateRect.Max.y <= instanceRect.Min.y && templateRect.GetHeight() >= ImGui::GetFontSize() * 3,
                      "long device card text wraps into its own row without overlapping the instance");
                click(manager, wide, instanceRect.GetCenter());
                auto selected = snapshot(manager);
                check(std::string(stringField(selected.get(), "selectedDeviceTypeId")) == "10" &&
                          std::string(stringField(selected.get(), "selectedInstanceId")) == "11",
                      "clicking an instance card selects its exact saved identity");
                for (int i = 0; i < 4; ++i)
                        frame(manager, narrow);
                check(!child("##device_list") && child("##device_detail"),
                      "narrow selected device uses full-width detail instead of squeezing two columns");
                auto *root = ImGui::FindWindowByName("Device Manager###DeviceManager");
                const auto back = locate(manager, narrow, root, root->GetID("Device library"));
                click(manager, narrow, back.GetCenter());
                library = child("##device_list");
                check(library && library->Size.x > narrow.x * 0.85f && !child("##device_detail"),
                      "Device library returns to full-width navigation in narrow view");
                const auto search = locate(manager, narrow, library, library->GetID("##device_search"));
                click(manager, narrow, search.GetCenter());
                io.AddInputCharactersUTF8("Oscilloscope");
                frame(manager, narrow);
                frame(manager, narrow);
                const auto resultCard = locate(manager, narrow, library,
                                               ImHashStr("###type_node", 0, library->GetID("device_type_20")));
                check(resultCard.Min.y < instanceRect.Min.y, "search brings matching device card directly below the filter");
                click(manager, narrow, resultCard.GetCenter());
                selected = snapshot(manager);
                check(std::string(stringField(selected.get(), "selectedDeviceTypeId")) == "20" &&
                          std::string(stringField(selected.get(), "selectedInstanceId")) == "0",
                      "filtered template selection does not confuse type and instance identities");
                for (bool dark : {true, false}) {
                        ui::ApplyPlatformStyle(ui::PlatformStyle::Windows, 1, false, dark);
                        for (const auto size : {ImVec2(1400, 900), ImVec2(480, 800)}) {
                                for (int i = 0; i < 4; ++i)
                                        frame(manager, size);
                                auto *window = ImGui::FindWindowByName("Device Manager###DeviceManager");
                                check(window && window->Active, "device manager renders at wide and narrow widths");
                                check(std::abs(window->Size.x - size.x) < 1, "device manager honors narrow dock width");
                        }
                }
                auto after = snapshot(manager);
                check(cJSON_Compare(field(before.get(), "deviceTypes"), field(after.get(), "deviceTypes"), true),
                      "browsing appearance and resizing preserve device types, instances and exact values");
                check(!manager.isModified(), "rendering and resizing do not dirty device data");
                const auto *first = cJSON_GetArrayItem(field(after.get(), "deviceTypes"), 0);
                const auto *property = cJSON_GetArrayItem(field(first, "properties"), 0);
                check(std::string(stringField(property, "value")) == "18446744073709551615",
                      "exact 64-bit value survives responsive UI");
                DeviceManager restored;
                check(restored.load(after.get(), "."), "device manager session roundtrip loads");
                auto restoredData = snapshot(restored);
                check(cJSON_Compare(field(after.get(), "deviceTypes"), field(restoredData.get(), "deviceTypes"), true),
                      "device schema and instance values roundtrip unchanged");
                std::puts("PASS: device cards/search/narrow navigation, light/dark UI, exact values and session roundtrip without hardware");
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
