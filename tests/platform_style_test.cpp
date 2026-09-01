#include "gui/island_style.hpp"
#include "gui/platform_fonts.hpp"
#include "gui/platform_style.hpp"
#include "gui/table_text.hpp"
#include "gui/ui_icons.hpp"
#include "gui/ui_theme.hpp"
#include "platform/appearance.hpp"
#include "platform/native_appearance.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

static float
luminance(const ImVec4 &color)
{
        const auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * linear(color.x) + 0.7152f * linear(color.y) + 0.0722f * linear(color.z);
}

static float
contrast(const ImVec4 &foreground, const ImVec4 &background)
{
        const float a = luminance(foreground), b = luminance(background);
        return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f);
}

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}

static void
frame()
{
        ImGui::NewFrame();
        ImGui::Begin("Platform controls");
        const float nameWidth    = ui::TableColumnWidth("Variable", "motor.current.reference", 12);
        const float addressWidth = ui::TableColumnWidth("Address###address", "0xFFFFFFFF");
        const float valueWidth   = ui::TableColumnWidth("Value", "-1.23456789e+123");
        if (ImGui::BeginTable("Readability",
                              3,
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_NoSavedSettings,
                              ImVec2(220, 90),
                              nameWidth + addressWidth + valueWidth)) {
                ImGui::TableSetupColumn("Variable", ImGuiTableColumnFlags_WidthFixed, nameWidth);
                ImGui::TableSetupColumn("Address###address", ImGuiTableColumnFlags_WidthFixed, addressWidth);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, valueWidth);
                ImGui::TableHeadersRow();
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ui::TableText("motor.current.reference");
                ImGui::TableSetColumnIndex(1);
                check(ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize("0xFFFFFFFF").x,
                      "Horizontal scroll must preserve address width in narrow windows");
                ui::TableText("0xFFFFFFFF");
                ImGui::TableSetColumnIndex(2);
                check(ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize("-1.23456789e+123").x,
                      "Horizontal scroll must preserve scientific values in narrow windows");
                ui::TableText(std::string("-1.23456789e+123"), ImGui::GetStyle().Colors[ImGuiCol_Text]);
                ImGui::EndTable();
        }
        ImGui::Button("Save");
        for (bool frozen : {false, true}) {
                const int pushed = ui::PushPausedDockTabStyle(true, frozen);
                for (auto state : {ImGuiCol_Tab, ImGuiCol_TabDimmed, ImGuiCol_TabSelected, ImGuiCol_TabHovered,
                                   ImGuiCol_TitleBg, ImGuiCol_TitleBgActive})
                        check(contrast(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(state)) >= 4.5f,
                              "Paused and frozen state labels stay readable on selected and inactive tabs");
                ImGui::PopStyleColor(pushed);
        }
        const ImVec4 originalText = ImGui::GetStyle().Colors[ImGuiCol_Text];
        ui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 0.4f), "%s: %d", "Status", 7);
        check(ImGui::GetStyle().Colors[ImGuiCol_Text].x == originalText.x,
              "Theme-aware status text must restore surrounding text style");
        ui::IconButton("Device###stable-device", ui::Icon::Device, ui::BtnStyle::Primary);
        check(ImGui::GetItemID() == ImGui::GetID("Device###stable-device"), "Icon buttons retain label-based native IDs");
        check(ImGui::GetItemRectSize().x >= ImGui::CalcTextSize("Device").x + ImGui::GetFontSize(),
              "Icon button sizing must include both icon and full label");
        check(ImGui::GetStyle().Colors[ImGuiCol_Text].x == originalText.x, "Icon buttons must restore surrounding text style");
        ImGui::BeginDisabled();
        check(!ui::IconButton("Disabled", ui::Icon::Connect, ui::BtnStyle::Success, ImVec2(40, 0)),
              "Disabled icon buttons cannot activate");
        ImGui::EndDisabled();
        for (int i = static_cast<int>(ui::Icon::Home); i <= static_cast<int>(ui::Icon::Properties); ++i)
                ui::DrawIcon(ImGui::GetWindowDrawList(),
                             static_cast<ui::Icon>(i),
                             ImVec2(20.0f + i * 22.0f, 20),
                             20,
                             ImGui::GetColorU32(ImGuiCol_Text));
        for (auto semantic :
             {ui::BtnStyle::Primary, ui::BtnStyle::Success, ui::BtnStyle::Warning, ui::BtnStyle::Danger, ui::BtnStyle::Muted}) {
                ImGui::PushID(static_cast<int>(semantic));
                ui::Button("Action", semantic);
                ui::SmallButton("Compact action", semantic);
                ImGui::PopID();
                check(ImGui::GetStyle().Colors[ImGuiCol_Text].x == originalText.x,
                      "Semantic buttons must restore text color for surrounding controls");
        }
        static bool enabled = true;
        ImGui::Checkbox("Enabled", &enabled);
        ImGui::End();
        ImGui::Render();
}

int
main()
{
        using appearance::Mode;
        using appearance::ResolveDark;
        check(!ResolveDark(Mode::Light, true, 23), "Explicit light overrides system and clock");
        check(ResolveDark(Mode::Dark, false, 12), "Explicit dark overrides system and clock");
        check(ResolveDark(Mode::System, true, 12) && !ResolveDark(Mode::System, false, 23), "System ignores clock");
        check(ResolveDark(Mode::Schedule, false, 6) && !ResolveDark(Mode::Schedule, true, 7), "Daytime starts at 07:00");
        check(!ResolveDark(Mode::Schedule, true, 18) && ResolveDark(Mode::Schedule, false, 19), "Night starts at 19:00");
        check(ResolveDark(Mode::Schedule, false, 0) && ResolveDark(Mode::Schedule, false, 23), "Schedule covers midnight");
        check(!ResolveDark(Mode::Schedule, true, 19, 19, 7) && !ResolveDark(Mode::Schedule, true, 6, 19, 7),
              "Daytime can span midnight");
        check(ResolveDark(Mode::Schedule, false, 7, 19, 7) && ResolveDark(Mode::Schedule, false, 18, 19, 7),
              "Overnight day interval has exclusive end");
        check(ResolveDark(Mode::Schedule, true, 12, 7, 7) && !ResolveDark(Mode::Schedule, false, 12, 7, 7),
              "Equal bounds follow system");
        check(!ResolveDark(Mode::Schedule, false, 24) && ResolveDark(Mode::Schedule, true, 12, -1, 19),
              "Invalid schedule input follows system");
        using ui::PlatformStyle;
        check(ui::LinuxStyle("ubuntu:GNOME", nullptr) == PlatformStyle::Adwaita, "GNOME session");
        check(ui::LinuxStyle("KDE", nullptr) == PlatformStyle::Breeze, "KDE session");
        check(ui::LinuxStyle("", "plasmawayland") == PlatformStyle::Breeze, "Plasma fallback");
        check(ui::LinuxStyle(nullptr, nullptr) == PlatformStyle::Adwaita, "Unknown desktop fallback");
        check(!ui::FontFileExists("/ava-font-does-not-exist.ttf"), "Missing font must not reach ImGui assertion");
        native_appearance::applyDarkWindow(nullptr);
        native_appearance::applyWindow(nullptr, false);
        for (bool mac : {false, true}) {
                ImGui::CreateContext();
                auto &io                 = ImGui::GetIO();
                io.IniFilename           = nullptr;
                io.ConfigMacOSXBehaviors = mac;
                io.DisplaySize           = ImVec2(1280, 720);
                io.DeltaTime             = 1.0f / 60.0f;
                const auto fonts         = ui::LoadPlatformFonts(*io.Fonts, 18.0f, "/ava-font-does-not-exist.ttf");
                check(fonts.font != nullptr, "System fonts must have a built-in fallback");
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                check(fonts.font->IsGlyphInFont('A'), "Platform font must retain Latin text");
                if (!fonts.chinesePath.empty())
                        check(fonts.font->IsGlyphInFont(0x4e2d), "CJK fallback must supply Chinese text");
                for (auto platform : {PlatformStyle::Auto,
                                      PlatformStyle::Windows,
                                      PlatformStyle::MacOS,
                                      PlatformStyle::Adwaita,
                                      PlatformStyle::Breeze}) {
                        for (bool dark : {true, false}) {
                                ui::ApplyPlatformStyle(platform, 1.5f, true, dark);
                                const auto first = ImGui::GetStyle();
                                for (int i = 0; i < 10; ++i)
                                        ui::ApplyPlatformStyle(platform, 1.5f, true, dark);
                                const auto &again = ImGui::GetStyle();
                                check(first.FramePadding.x == again.FramePadding.x &&
                                          first.ScrollbarSize == again.ScrollbarSize,
                                      "Repeated switching must not compound DPI");
                                check(again.WindowRounding > 0 && again.Colors[ImGuiCol_WindowBg].w == 1,
                                      "In-app floating windows retain rounding with multi-viewports enabled");
                                check(io.ConfigMacOSXBehaviors == mac, "Preview style must not change keyboard semantics");
                                for (const auto &color : again.Colors)
                                        check(std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) &&
                                                  color.x >= 0 && color.x <= 1 && color.y >= 0 && color.y <= 1 &&
                                                  color.z >= 0 && color.z <= 1 && color.w >= 0 && color.w <= 1,
                                              "Theme colors must stay valid in light and dark");
                                for (auto background : {ImGuiCol_WindowBg,
                                                        ImGuiCol_PopupBg,
                                                        ImGuiCol_FrameBg,
                                                        ImGuiCol_Button,
                                                        ImGuiCol_ButtonHovered,
                                                        ImGuiCol_TabSelected,
                                                        ImGuiCol_TableHeaderBg})
                                        check(contrast(again.Colors[ImGuiCol_Text], again.Colors[background]) >= 4.5f,
                                              "Light and dark standard controls require readable labels");
                                check((luminance(again.Colors[ImGuiCol_WindowBg]) < 0.4f) == dark,
                                      "Requested appearance must be applied");
                                check(ui::ReadableTextColor(ImVec4(1, 1, 0, 0.35f), again.Colors[ImGuiCol_WindowBg]).w == 0.35f,
                                      "Readable text must preserve fade opacity");
                                for (const auto &color : {ImVec4(1, 1, 0, 1), ImVec4(0.1f, 0.1f, 0.1f, 1), ImVec4(0, 1, 1, 1)})
                                        check(contrast(ui::ReadableTextColor(color, again.Colors[ImGuiCol_WindowBg]),
                                                       again.Colors[ImGuiCol_WindowBg]) >= 4.5f,
                                              "User series colors must produce readable text on either background");
                                for (auto semantic : {ui::BtnStyle::Neutral,
                                                      ui::BtnStyle::Primary,
                                                      ui::BtnStyle::Success,
                                                      ui::BtnStyle::Warning,
                                                      ui::BtnStyle::Danger,
                                                      ui::BtnStyle::Muted}) {
                                        const auto p = ui::SemanticButtonPalette(semantic, again);
                                        for (const auto &surface : {p.idle, p.hovered, p.active}) {
                                                if (contrast(p.text, surface) < 4.5f)
                                                        std::fprintf(stderr,
                                                                     "Contrast %.2f: platform %d, semantic %d, dark=%d\n",
                                                                     contrast(p.text, surface),
                                                                     static_cast<int>(platform),
                                                                     static_cast<int>(semantic),
                                                                     dark);
                                                check(contrast(p.text, surface) >= 4.5f,
                                                      "Semantic button text needs 4.5:1 contrast");
                                        }
                                }
                                frame();
                                ui::ApplyPlatformStyle(platform, 1.0f, false, dark);
                                check(ImGui::GetStyle().FramePadding.x < first.FramePadding.x, "DPI scale must be reversible");
                                frame();
                        }
                }
                for (int scheme = 0; scheme < static_cast<int>(ui::ColorScheme::Count); ++scheme)
                        for (bool dark : {false, true}) {
                                ui::ApplyAppStyle(static_cast<ui::ColorScheme>(scheme), 1.5f, true, dark);
                                const auto first = ImGui::GetStyle();
                                ui::ApplyAppStyle(static_cast<ui::ColorScheme>(scheme), 1.5f, true, dark);
                                check(ImGui::GetStyle().WindowPadding.x == first.WindowPadding.x, "Island DPI is idempotent");
                                check(first.DockingSeparatorSize >= 12, "Island cards have canvas gutters");
                                check(contrast(first.Colors[ImGuiCol_Text], first.Colors[ImGuiCol_WindowBg]) >= 4.5f,
                                      "Every island palette has readable text");
                                for (auto background : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
                                                        ImGuiCol_Button, ImGuiCol_TabSelected, ImGuiCol_TableHeaderBg}) {
                                        check(contrast(first.Colors[ImGuiCol_Text], first.Colors[background]) >= 4.5f,
                                              "Values and control labels stay readable in every island palette");
                                        check(contrast(first.Colors[ImGuiCol_TextDisabled], first.Colors[background]) >= 4.5f,
                                              "Search hints and secondary text remain readable on control surfaces");
                                }
                                for (auto tone : {ui::BtnStyle::Primary, ui::BtnStyle::Success, ui::BtnStyle::Warning, ui::BtnStyle::Danger}) {
                                        const auto button = ui::SemanticButtonPalette(tone, first);
                                        for (auto surface : {button.idle, button.hovered, button.active})
                                                check(contrast(button.text, surface) >= 4.5f, "Semantic actions stay readable in every palette");
                                }
                        }
                for (float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
                        ui::ApplyAppStyle(ui::ColorScheme::Ocean, scale, false, true);
                        const auto &style = ImGui::GetStyle();
                        check(ui::design::FontSize(ui::design::body, scale) >= 12,
                              "Small display scales keep body text at least 12 pixels");
                        check(std::abs(style.ChildRounding - (style.WindowRounding - 4 * scale)) < 1,
                              "Nested surfaces retain a consistent radius and inset at every display scale");
                        check(style.FrameBorderSize > 0 && style.FrameBorderSize <= 1,
                              "Scaled controls retain restrained visible borders");
                }
                ui::ApplyPlatformStyle(PlatformStyle::Windows, std::numeric_limits<float>::infinity(), false);
                check(std::isfinite(ImGui::GetStyle().FramePadding.x), "Invalid DPI must fall back to logical pixels");
                // Use real ImGui input processing: recent ImGui versions swap
                // Super/Ctrl on macOS before updating KeyCtrl. Do not swap twice.
                io.AddKeyEvent(mac ? ImGuiMod_Super : ImGuiMod_Ctrl, true);
                frame();
                check(io.KeyCtrl, "Host primary modifier must reach application shortcuts");
                check(std::string(ui::ShortcutLabel("Ctrl+S", "Cmd+S")) == (mac ? "Cmd+S" : "Ctrl+S"),
                      "Shortcut hint must follow input behavior, not visual preview");
                io.AddKeyEvent(mac ? ImGuiMod_Super : ImGuiMod_Ctrl, false);
                frame();
                check(!io.KeyCtrl, "Primary modifier released");
                if (mac) {
                        io.AddKeyEvent(ImGuiMod_Ctrl, true);
                        frame();
                        check(!io.KeyCtrl, "Physical Control must not masquerade as Command");
                }
                ImGui::DestroyContext();
        }
        std::puts("Platform styles: light/dark, schedule boundaries, font/CJK, semantic contrast, state restoration, DPI and "
                  "Ctrl/Cmd passed");
}
