#include "gui/unit_conversion_ui.hpp"
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

struct Item {
        ImRect         rect;
        ImGuiItemFlags flags{};
        std::string    label, window;
};
static std::map<ImGuiID, Item> items;

// Observe the real ImGui items, rather than duplicating the card's layout or
// writing its private draft state. The production helper is compiled unchanged.
void
ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id, const ImRect &rect, const ImGuiLastItemData *data)
{
        if (id)
                items[id] = {rect, data ? data->ItemFlags : context->CurrentItemFlags, {}, context->CurrentWindow->Name};
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
check(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

static const Item &
item(const char *label)
{
        for (const auto &[_, record] : items)
                if (record.label == label)
                        return record;
        throw std::runtime_error(std::string("Rendered item not found: ") + label);
}

static bool
hasItem(const char *label)
{
        for (const auto &[_, record] : items)
                if (record.label == label)
                        return true;
        return false;
}

struct Fixture {
        unit_conversion::Settings first, second;
        double                    rawFirst = 3.141592653589793, rawSecond = 120;
        int                       changesFirst = 0, changesSecond = 0;
        bool                      showFirst = true;
        ImRect                    firstRect, secondRect;

        Fixture()
        {
                ImGui::CreateContext();
                auto &io                        = ImGui::GetIO();
                io.IniFilename                  = nullptr;
                io.DisplaySize                  = ImVec2(1280, 900);
                io.DeltaTime                    = 0.05f;
                io.ConfigInputTrickleEventQueue = false;
                io.Fonts->AddFontDefault();
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                GImGui->TestEngineHookItems = true;
                io.AddMousePosEvent(-1000, -1000);
                frame();
                frame();
        }
        ~Fixture() { ImGui::DestroyContext(); }

        void source(const char                *name,
                    const ImVec2              &position,
                    double                    &raw,
                    unit_conversion::Settings &settings,
                    int                       &changes,
                    ImRect                    &rect)
        {
                ImGui::SetNextWindowPos(position);
                ImGui::SetNextWindowSize(ImVec2(380, 160));
                ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
                ImGui::SetNextItemWidth(240);
                ImGui::InputDouble("Raw value", &raw, 0, 0, "%.12g");
                rect                = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                const auto before   = GImGui->LastItemData;
                const bool hovered  = ImGui::IsItemHovered();
                changes            += ui::UnitConversionHover("conversion", name, raw, settings, hovered) ? 1 : 0;
                const auto after    = GImGui->LastItemData;
                check(before.ID == after.ID && before.ItemFlags == after.ItemFlags && before.StatusFlags == after.StatusFlags &&
                          before.Rect.Min.x == after.Rect.Min.x && before.Rect.Min.y == after.Rect.Min.y &&
                          before.Rect.Max.x == after.Rect.Max.x && before.Rect.Max.y == after.Rect.Max.y,
                      "Hover helper must preserve the source's LastItemData for callers");
                ImGui::End();
        }

        void frame(int count = 1)
        {
                for (int i = 0; i < count; ++i) {
                        items.clear();
                        ImGui::NewFrame();
                        if (showFirst)
                                source("Source A", ImVec2(20, 20), rawFirst, first, changesFirst, firstRect);
                        source("Source B", ImVec2(850, 20), rawSecond, second, changesSecond, secondRect);
                        ui::UnitConversionEndFrame();
                        ImGui::Render();
                }
        }

        void move(const ImVec2 &point, int frames = 1)
        {
                ImGui::GetIO().AddMousePosEvent(point.x, point.y);
                frame(frames);
        }
        void click(const ImVec2 &point)
        {
                move(point);
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                frame();
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                frame(3);
        }
        void click(const char *label) { click(item(label).rect.GetCenter()); }
        void key(ImGuiKey key)
        {
                ImGui::GetIO().AddKeyEvent(key, true);
                frame();
                ImGui::GetIO().AddKeyEvent(key, false);
                frame();
        }
        void hoverFirst()
        {
                move(firstRect.GetCenter(), 12);
                check(ui::UnitConversionVisible(), "Delayed source hover opens a conversion card");
        }
        void edit()
        {
                click(first.enabled ? "Edit units..." : "Set up conversion...");
                check(hasItem("Apply") && hasItem("Cancel"), "Setup button opens the editable card");
        }
        ImVec2 combo(const char *scope = nullptr)
        {
                const auto *card = ImGui::FindWindowByName("###UnitConversionHover");
                check(card != nullptr, "Conversion card must exist");
                const ImGuiID seed  = scope ? ImHashStr(scope, 0, card->ID) : card->ID;
                const auto    found = items.find(ImHashStr(scope ? "##unit" : "##quantity", 0, seed));
                check(found != items.end(), "Real combo item must be rendered");
                return found->second.rect.GetCenter();
        }
        void chooseQuantity(const char *label)
        {
                click(combo());
                click(label);
        }
        void typeCounts(const char *text)
        {
                const auto *card  = ImGui::FindWindowByName("###UnitConversionHover");
                const auto  found = items.find(ImHashStr("##countsPerRevolution", 0, card->ID));
                check(found != items.end(), "Count source exposes its numeric scale input");
                click(found->second.rect.GetCenter());
                ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
                ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
                frame();
                ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
                ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
                frame();
                ImGui::GetIO().AddInputCharactersUTF8(text);
                frame();
                key(ImGuiKey_Enter);
        }
};

int
main()
{
        try {
                {
                        Fixture f;
                        f.move(f.firstRect.GetCenter(), 3);
                        check(!ui::UnitConversionVisible(), "Card must not appear immediately on initial hover");
                        f.move(ImVec2(700, 800), 15);
                        f.move(f.firstRect.GetCenter(), 2);
                        check(!ui::UnitConversionVisible(), "Leaving before the delay must reset the hover timer");
                        f.frame(10);
                        check(ui::UnitConversionVisible(), "Continuous hover reaches the display delay");
                        const double raw = f.rawFirst;
                        f.edit();
                        f.move(f.secondRect.GetCenter(), 20);
                        check(hasItem("Apply") && !hasItem("Set up conversion..."),
                              "Moving over a different source keeps the current draft pinned");
                        check(!f.second.enabled && f.changesSecond == 0, "Pinned editor never modifies another source");
                        f.chooseQuantity("Linear position");
                        check(!f.first.enabled && f.changesFirst == 0, "Changing selectors only edits a draft");
                        f.click("Cancel");
                        check(!f.first.enabled && f.changesFirst == 0,
                              "Cancel discards the draft without marking settings changed");
                        f.edit();
                        f.chooseQuantity("Rotational speed");
                        f.click("Apply");
                        check(f.first.enabled && f.first.quantity == unit_conversion::Quantity::AngularSpeed &&
                                  f.first.source == unit_conversion::Unit::RadPerSecond &&
                                  f.first.target == unit_conversion::Unit::Rpm && f.changesFirst == 1,
                              "Apply commits chosen units once");
                        check(f.rawFirst == raw, "Applying display conversion cannot modify the raw numeric source");
                        f.click("Disable");
                        check(!f.first.enabled && f.changesFirst == 2 && f.rawFirst == raw,
                              "Disable only updates display settings");
                }
                {
                        Fixture f;
                        f.hoverFirst();
                        f.edit();
                        f.click(f.combo("Source unit"));
                        check(GImGui->OpenPopupStack.Size > 0, "Source unit opens a real combo popup");
                        f.move(ImVec2(700, 800), 20);
                        check(ui::UnitConversionVisible() && GImGui->OpenPopupStack.Size > 0,
                              "Open unit selector keeps the conversion card alive while navigating");
                        f.click("count");
                        check(item("Apply").flags & ImGuiItemFlags_Disabled, "Missing count scale disables Apply");
                        f.click("Apply");
                        check(!f.first.enabled && f.changesFirst == 0, "Disabled Apply cannot commit invalid count conversion");
                        f.typeCounts("4096");
                        check(!(item("Apply").flags & ImGuiItemFlags_Disabled), "Positive count scale enables Apply");
                        f.click("Apply");
                        check(f.first.enabled && f.first.source == unit_conversion::Unit::Count &&
                                  f.first.countsPerRevolution == 4096,
                              "Count selector and text input commit through real interaction");
                        f.edit();
                        f.click(f.combo("Display unit"));
                        f.key(ImGuiKey_Escape);
                        check(!ui::UnitConversionVisible() && GImGui->OpenPopupStack.Size == 0,
                              "Escape dismisses card and its selector without applying changes");
                        check(f.changesFirst == 1, "Escape does not commit a draft");
                        f.hoverFirst();
                        f.edit();
                        f.click(ImVec2(700, 800));
                        check(!ui::UnitConversionVisible() && f.changesFirst == 1,
                              "Outside click dismisses editing without committing");
                        f.hoverFirst();
                        f.edit();
                        f.click(f.combo());
                        f.showFirst = false;
                        f.frame(2);
                        check(!ui::UnitConversionVisible() && GImGui->OpenPopupStack.Size == 0,
                              "Removing a source dismisses its card and owned popup at end of frame");
                }
                {
                        Fixture f;
                        f.click(f.firstRect.GetCenter());
                        f.frame(20);
                        check(GImGui->ActiveId != 0 && !ui::UnitConversionVisible(),
                              "An actively edited source must retain input focus without opening a conversion card");
                        check(!f.first.enabled && f.changesFirst == 0, "Editing source does not change conversion settings");
                }
                std::puts("PASS: delayed hover, pinned drafts, selectors, Apply/Cancel/Disable, count validation, source item "
                          "preservation, source editing and dismissal");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
