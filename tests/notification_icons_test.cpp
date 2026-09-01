#include "gui/notification_icons.hpp"
#include "gui/platform_style.hpp"
#include <chrono>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

struct Item {
        ImRect      rect;
        std::string label, window;
};
static std::map<ImGuiID, Item>       items;
static std::map<ImGuiToastType, int> icons;
static int                           closeGeometry = 0;

static void
check(bool valid, const char *message)
{
        if (!valid)
                throw std::runtime_error(message);
}

void
ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id, const ImRect &rect, const ImGuiLastItemData *)
{
        if (id)
                items[id] = {rect, {}, context->CurrentWindow->Name};
}
void
ImGuiTestEngineHook_ItemInfo(ImGuiContext *context, ImGuiID id, const char *label, ImGuiItemStatusFlags)
{
        if (id) {
                items[id].label  = label;
                items[id].window = context->CurrentWindow->Name;
        }
}
void
ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...)
{
}
const char *
ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id)
{
        const auto found = items.find(id);
        return found == items.end() ? nullptr : found->second.label.c_str();
}

static void
observeIcon(ImGuiToastType type, float opacity)
{
        auto     *draw   = ImGui::GetWindowDrawList();
        const int before = draw->VtxBuffer.Size;
        ui::DrawNotificationIcon(type, opacity);
        const int added = draw->VtxBuffer.Size - before;
        if (opacity > 0.1f) {
                check(added > 8, "Each status icon must submit vector geometry without an icon font");
                ++icons[type];
                int maximumAlpha = 0;
                for (int i = before; i < draw->VtxBuffer.Size; ++i)
                        maximumAlpha =
                            std::max(maximumAlpha, static_cast<int>((draw->VtxBuffer[i].col >> IM_COL32_A_SHIFT) & 255));
                const int expected = static_cast<int>(opacity * ImGui::GetStyle().Alpha * 255 + 0.5f);
                check(maximumAlpha > 0 && std::abs(maximumAlpha - expected) <= 1,
                      "Status vectors preserve toast fade and global alpha");
        }
}

static bool
observeClose(const char *id, float opacity)
{
        auto      *draw    = ImGui::GetWindowDrawList();
        const int  before  = draw->VtxBuffer.Size;
        const auto style   = ImGui::GetStyle();
        const bool clicked = ui::NotificationDismissButton(id, opacity);
        if (opacity > 0.1f && draw->VtxBuffer.Size - before > 8)
                ++closeGeometry;
        check(ImGui::GetStyle().Alpha == style.Alpha && ImGui::GetStyle().FramePadding.x == style.FramePadding.x &&
                  ImGui::GetStyle().FramePadding.y == style.FramePadding.y,
              "Close vector button restores alpha and padding");
        return clicked;
}

static void
frame(int count = 1)
{
        for (int i = 0; i < count; ++i) {
                items.clear();
                ImGui::NewFrame();
                const auto before = ImGui::GetStyle();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, before.PopupRounding);
                ImGui::RenderNotifications();
                ImGui::PopStyleVar();
                check(ImGui::GetStyle().Alpha == before.Alpha && ImGui::GetStyle().WindowRounding == before.WindowRounding &&
                          ImGui::GetStyle().Colors[ImGuiCol_Text].x == before.Colors[ImGuiCol_Text].x,
                      "Notification rendering preserves surrounding theme and rounding styles");
                ImGui::Render();
        }
}

static void
click(const char *window, const char *label)
{
        ImVec2 position;
        bool   found = false;
        for (const auto &[_, item] : items)
                if (item.window == window && item.label == label) {
                        position = item.rect.GetCenter();
                        found    = true;
                        break;
                }
        check(found, "Notification control must be present as a real ImGui item");
        ImGui::GetIO().AddMousePosEvent(position.x, position.y);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(3);
}

int
main()
{
        try {
                ImGui::CreateContext();
                auto &io                        = ImGui::GetIO();
                io.IniFilename                  = nullptr;
                io.DisplaySize                  = ImVec2(1280, 900);
                io.DeltaTime                    = 1.0f / 60;
                io.ConfigInputTrickleEventQueue = false;
                auto          *font             = io.Fonts->AddFontDefault();
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                check(!font->IsGlyphInFont(0xf058) && !font->IsGlyphInFont(0xf00d),
                      "Fixture intentionally contains no Font Awesome status or close glyphs");
                GImGui->TestEngineHookItems = true;
                ui::InstallNotificationIcons();
                check(ImGui::notificationIconRenderer == ui::DrawNotificationIcon &&
                          ImGui::notificationCloseButtonRenderer == ui::NotificationDismissButton,
                      "Application installs both status and dismiss vector callbacks");
                ImGui::SetNotificationRenderers(observeIcon, observeClose);
                for (bool dark : {true, false}) {
                        ui::ApplyPlatformStyle(ui::PlatformStyle::Windows, 1, false, dark);
                        ImGui::GetStyle().Alpha = 0.85f;
                        ImGui::notifications.clear();
                        icons.clear();
                        closeGeometry = 0;
                        for (auto type : {ImGuiToastType::None,
                                          ImGuiToastType::Success,
                                          ImGuiToastType::Warning,
                                          ImGuiToastType::Error,
                                          ImGuiToastType::Info})
                                ImGui::InsertNotification(ImGuiToast(type, 60000, "Notification content"));
                        std::this_thread::sleep_for(std::chrono::milliseconds(NOTIFY_FADE_IN_OUT_TIME + 10));
                        frame(4);
                        for (auto type :
                             {ImGuiToastType::Success, ImGuiToastType::Warning, ImGuiToastType::Error, ImGuiToastType::Info})
                                check(icons[type] > 0, "Every status notification uses its vector renderer in both themes");
                        const auto errorStyle = ui::NotificationAppearance(ImGuiToastType::Error);
                        const auto successStyle = ui::NotificationAppearance(ImGuiToastType::Success);
                        check(successStyle.text.y > successStyle.text.x && successStyle.text.y > successStyle.text.z &&
                                  ui::ColorContrast(successStyle.text, successStyle.background) >= 4.49f,
                              "Success messages use readable green text in both themes");
                        check(errorStyle.text.x > errorStyle.text.y && errorStyle.text.x > errorStyle.text.z &&
                                  ui::ColorContrast(errorStyle.text, errorStyle.background) >= 4.49f,
                              "Error content stays red and readable in light and dark notifications");
                        float previousTop = io.DisplaySize.y;
                        for (int i = 0; i < 5; ++i) {
                                auto *toast = ImGui::FindWindowByName(("##TOAST" + std::to_string(i)).c_str());
                                check(toast && toast->Active && toast->Size.x >= 350 && toast->Size.y >= 80,
                                      "Short notifications retain a comfortably sized card and hit target");
                                check(toast->Pos.x >= 0 && toast->Pos.y >= 0 &&
                                          toast->Pos.x + toast->Size.x <= io.DisplaySize.x &&
                                          toast->Pos.y + toast->Size.y < previousTop,
                                      "Stacked notification cards remain inside the viewport and do not overlap");
                                previousTop = toast->Pos.y;
                        }
                        check(closeGeometry >= 5, "All toast types including None have a vector close button");
                        click("##TOAST0", "##NotificationDismiss");
                        check(ImGui::notifications.size() == 4 &&
                                  ImGui::notifications.front().getType() == ImGuiToastType::Success,
                              "One real close click removes only its toast");
                }

                ImGui::notifications.clear();
                int actionCount = 0;
                ImGui::InsertNotification(ImGuiToast(
                    ImGuiToastType::Info,
                    60000,
                    "Run action",
                    [&] {
                            ++actionCount;
                            // Force queue growth during rendering; copied toast data
                            // and its action callback must remain valid.
                            for (int i = 0; i < 16; ++i)
                                    ImGui::InsertNotification(
                                        ImGuiToast(ImGuiToastType::Success, 60000, "Action result %d", i));
                    },
                    "Action notification"));
                std::this_thread::sleep_for(std::chrono::milliseconds(NOTIFY_FADE_IN_OUT_TIME + 10));
                frame(4);
                click("##TOAST0", "Run action");
                check(actionCount == 1 && ImGui::notifications.size() == 17,
                      "Action callbacks still run once and can grow the queue safely");
                check(std::string(ImGui::notifications.front().getContent()) == "Action notification",
                      "Action leaves original notification intact");
                click("##TOAST0", "##NotificationDismiss");
                check(actionCount == 1 && ImGui::notifications.size() == 16 &&
                          std::string(ImGui::notifications.front().getContent()) == "Action result 0",
                      "Dismiss does not invoke the action and leaves later notifications intact");
                ImGui::notifications.clear();
                io.DisplaySize = ImVec2(320, 260);
                ImGuiToast longError(ImGuiToastType::Error, 60000, "%s", std::string(1200, 'x').c_str());
                longError.setTitle("A long failure title that must wrap without covering the close control");
                ImGui::InsertNotification(longError);
                std::this_thread::sleep_for(std::chrono::milliseconds(NOTIFY_FADE_IN_OUT_TIME + 10));
                frame(5);
                auto *longWindow = ImGui::FindWindowByName("##TOAST0");
                check(longWindow && longWindow->Pos.x >= 0 && longWindow->Pos.y >= 0 &&
                          longWindow->Pos.x + longWindow->Size.x <= io.DisplaySize.x &&
                          longWindow->Pos.y + longWindow->Size.y <= io.DisplaySize.y && longWindow->ScrollMax.y > 0,
                      "Long errors wrap and scroll inside a narrow viewport");
                click("##TOAST0", "##NotificationDismiss");
                check(ImGui::notifications.empty(), "Long error can still be dismissed in a narrow viewport");
                ImGui::notifications.clear();
                ImGui::InsertNotification(ImGuiToast(ImGuiToastType::Info, -1000, "Expired one"));
                ImGui::InsertNotification(ImGuiToast(ImGuiToastType::Info, -1000, "Expired two"));
                frame();
                check(ImGui::notifications.empty(), "Consecutive expired notifications are removed without lifetime errors");
                ImGui::DestroyContext();
                std::puts("PASS: no-icon-font status/close vectors, fade, both themes, style restoration, real dismiss/action "
                          "clicks and queue lifetimes");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
