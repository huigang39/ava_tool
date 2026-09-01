#ifndef AVA_UI_ICONS_HPP
#define AVA_UI_ICONS_HPP

#include "ui_theme.hpp"
#include <cstring>
#include <initializer_list>

namespace ui
{
// A shared 24-unit outline set. Geometry scales with the current font/DPI;
// color always comes from the active theme rather than an icon font or bitmap.
enum class Icon {
        Home,
        Open,
        Save,
        Search,
        Add,
        Device,
        Motor,
        Connect,
        Disconnect,
        Variables,
        Chart,
        Grid,
        Focus,
        More,
        Settings,
        Play,
        Pause,
        Camera,
        Record,
        Stop,
        Delete,
        Back,
        Library,
        Properties,
        Success,
        Warning,
        Error,
        Info,
        Close
};

inline void
DrawIcon(ImDrawList *draw, Icon icon, const ImVec2 &topLeft, float size, ImU32 color, float stroke = 0)
{
        if (!draw || size <= 0 || (color & IM_COL32_A_MASK) == 0)
                return;
        const float unit = size / 24.0f;
        if (stroke <= 0)
                stroke = std::max(1.0f, size * 0.075f);
        const auto point = [&](float x, float y) { return ImVec2(topLeft.x + x * unit, topLeft.y + y * unit); };
        const auto line  = [&](float x1, float y1, float x2, float y2) {
                draw->AddLine(point(x1, y1), point(x2, y2), color, stroke);
        };
        const auto box = [&](float x1, float y1, float x2, float y2, float rounding = 1.0f) {
                draw->AddRect(point(x1, y1), point(x2, y2), color, rounding * unit, 0, stroke);
        };
        const auto circle = [&](float x, float y, float radius) {
                draw->AddCircle(point(x, y), radius * unit, color, 0, stroke);
        };
        const auto path = [&](std::initializer_list<ImVec2> points, bool closed = false) {
                for (const auto &p : points)
                        draw->PathLineTo(point(p.x, p.y));
                draw->PathStroke(color, closed ? ImDrawFlags_Closed : 0, stroke);
        };
        switch (icon) {
                case Icon::Home:
                        path({{3, 11}, {12, 3}, {21, 11}});
                        path({{5, 10}, {5, 21}, {10, 21}, {10, 15}, {14, 15}, {14, 21}, {19, 21}, {19, 10}});
                        break;
                case Icon::Open:
                        path({{3, 19}, {3, 5}, {9, 5}, {12, 8}, {20, 8}, {20, 11}});
                        path({{3, 19}, {6, 11}, {22, 11}, {19, 19}}, true);
                        break;
                case Icon::Save:
                        path({{4, 3}, {17, 3}, {21, 7}, {21, 21}, {3, 21}, {3, 4}, {4, 3}}, true);
                        path({{7, 3}, {7, 9}, {16, 9}, {16, 3}});
                        box(7, 14, 17, 21, 0);
                        break;
                case Icon::Search:
                        circle(10.5f, 10.5f, 6.5f);
                        line(15.5f, 15.5f, 21, 21);
                        break;
                case Icon::Add:
                        line(12, 4, 12, 20);
                        line(4, 12, 20, 12);
                        break;
                case Icon::Device:
                        box(6, 6, 18, 18, 2);
                        box(9, 9, 15, 15, 0);
                        for (float coordinate : {9.0f, 15.0f}) {
                                line(coordinate, 3, coordinate, 6);
                                line(coordinate, 18, coordinate, 21);
                                line(3, coordinate, 6, coordinate);
                                line(18, coordinate, 21, coordinate);
                        }
                        break;
                case Icon::Motor:
                        // Industrial electric motor: ribbed housing, terminal box,
                        // output shaft and mounting feet, distinct from a chip.
                        box(3, 7, 17, 18, 2);
                        box(7, 4, 13, 7, 0.5f);
                        path({{17, 11}, {22, 11}, {22, 14}, {17, 14}});
                        for (float x : {6.0f, 10.0f, 14.0f})
                                line(x, 10, x, 15);
                        line(6, 18, 6, 21);
                        line(14, 18, 14, 21);
                        line(4, 21, 16, 21);
                        break;
                case Icon::Connect:
                        line(9, 3, 9, 7);
                        line(15, 3, 15, 7);
                        path({{6, 7}, {18, 7}, {18, 11}, {15, 15}, {9, 15}, {6, 11}}, true);
                        line(12, 15, 12, 21);
                        break;
                case Icon::Disconnect:
                        line(9, 3, 9, 6);
                        line(15, 3, 15, 6);
                        line(6, 6, 18, 6);
                        path({{6, 11}, {6, 14}, {9, 18}, {15, 18}, {18, 14}, {18, 11}});
                        line(12, 18, 12, 22);
                        line(3, 8, 21, 11);
                        break;
                case Icon::Variables:
                        path({{8, 3}, {6, 3}, {5, 5}, {5, 9}, {3, 12}, {5, 15}, {5, 19}, {6, 21}, {8, 21}});
                        path({{16, 3}, {18, 3}, {19, 5}, {19, 9}, {21, 12}, {19, 15}, {19, 19}, {18, 21}, {16, 21}});
                        line(10, 10, 14, 14);
                        line(14, 10, 10, 14);
                        break;
                case Icon::Chart:
                        path({{3, 3}, {3, 21}, {21, 21}});
                        path({{6, 16}, {10, 10}, {14, 13}, {20, 5}});
                        break;
                case Icon::Grid:
                        box(3, 3, 10, 10);
                        box(14, 3, 21, 10);
                        box(3, 14, 10, 21);
                        box(14, 14, 21, 21);
                        break;
                case Icon::Focus:
                        path({{3, 9}, {3, 3}, {9, 3}});
                        path({{15, 3}, {21, 3}, {21, 9}});
                        path({{21, 15}, {21, 21}, {15, 21}});
                        path({{9, 21}, {3, 21}, {3, 15}});
                        circle(12, 12, 3);
                        break;
                case Icon::More:
                        for (float x : {5.0f, 12.0f, 19.0f})
                                draw->AddCircleFilled(point(x, 12), 1.5f * unit, color);
                        break;
                case Icon::Settings:
                        for (int i = 0; i < 32; ++i) {
                                const float angle  = static_cast<float>(i) * 3.14159265f / 16.0f;
                                const float radius = (i % 4 == 0 || i % 4 == 3) ? 7.5f : 9.5f;
                                draw->PathLineTo(point(12 + std::cos(angle) * radius, 12 + std::sin(angle) * radius));
                        }
                        draw->PathStroke(color, ImDrawFlags_Closed, stroke);
                        circle(12, 12, 3);
                        break;
                case Icon::Play:
                        path({{7, 3}, {21, 12}, {7, 21}}, true);
                        break;
                case Icon::Pause:
                        box(6, 4, 9, 20, 0.5f);
                        box(15, 4, 18, 20, 0.5f);
                        break;
                case Icon::Camera:
                        path({{3, 7}, {7, 7}, {9, 4}, {15, 4}, {17, 7}, {21, 7}, {21, 20}, {3, 20}}, true);
                        circle(12, 13, 4);
                        break;
                case Icon::Record:
                        box(3, 5, 16, 19, 2);
                        path({{16, 9}, {22, 6}, {22, 18}, {16, 15}}, true);
                        break;
                case Icon::Stop:
                        draw->AddRectFilled(point(6, 6), point(18, 18), color, unit);
                        break;
                case Icon::Delete:
                        line(3, 6, 21, 6);
                        path({{9, 6}, {9, 3}, {15, 3}, {15, 6}});
                        path({{5, 6}, {6, 21}, {18, 21}, {19, 6}});
                        line(10, 10, 10, 17);
                        line(14, 10, 14, 17);
                        break;
                case Icon::Back:
                        path({{10, 5}, {3, 12}, {10, 19}});
                        line(3, 12, 21, 12);
                        break;
                case Icon::Library:
                        box(3, 4, 7, 21, 0.5f);
                        box(9, 3, 13, 21, 0.5f);
                        path({{15, 5}, {19, 4}, {23, 20}, {19, 21}}, true);
                        break;
                case Icon::Properties:
                        line(3, 6, 8, 6);
                        line(12, 6, 21, 6);
                        circle(10, 6, 2);
                        line(3, 12, 14, 12);
                        line(18, 12, 21, 12);
                        circle(16, 12, 2);
                        line(3, 18, 6, 18);
                        line(10, 18, 21, 18);
                        circle(8, 18, 2);
                        break;
                case Icon::Success:
                        circle(12, 12, 9);
                        path({{7, 12}, {10.5f, 15.5f}, {17, 8.5f}});
                        break;
                case Icon::Warning:
                        path({{12, 3}, {22, 21}, {2, 21}}, true);
                        line(12, 9, 12, 14);
                        draw->AddCircleFilled(point(12, 17.5f), unit, color);
                        break;
                case Icon::Error:
                        circle(12, 12, 9);
                        line(8.5f, 8.5f, 15.5f, 15.5f);
                        line(15.5f, 8.5f, 8.5f, 15.5f);
                        break;
                case Icon::Info:
                        circle(12, 12, 9);
                        draw->AddCircleFilled(point(12, 7.5f), unit, color);
                        line(12, 11, 12, 17);
                        break;
                case Icon::Close:
                        line(6, 6, 18, 18);
                        line(18, 6, 6, 18);
                        break;
        }
}

// Use the normal Button for stable label IDs, focus, keyboard/gamepad
// activation, disabled state and native item sizing. Only its content is drawn
// here so the icon and full label are measured together without icon fonts.
inline bool
IconButton(const char *label, Icon icon, BtnStyle semantic = BtnStyle::Neutral, const ImVec2 &size = ImVec2(0, 0))
{
        const ImGuiStyle &style    = ImGui::GetStyle();
        const char       *labelEnd = std::strstr(label, "##");
        if (!labelEnd)
                labelEnd = label + std::strlen(label);
        const ImVec2 textSize   = ImGui::CalcTextSize(label, labelEnd);
        const float  iconSize   = ImGui::GetFontSize() * 1.1f;
        const float  gap        = textSize.x > 0 ? style.ItemInnerSpacing.x + 1.0f : 0;
        ImVec2       buttonSize = size;
        if (buttonSize.x == 0)
                buttonSize.x = iconSize + gap + textSize.x + style.FramePadding.x * 2;
        if (buttonSize.y == 0)
                buttonSize.y = std::max(iconSize, textSize.y) + style.FramePadding.y * 2;
        const int   colors     = PushButtonStyle(semantic);
        const ImU32 foreground = ImGui::GetColorU32(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        const bool pressed = ImGui::Button(label, buttonSize);
        ImGui::PopStyleColor();
        const ImVec2 start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
        const float  usableWidth  = std::max(0.0f, end.x - start.x - style.FramePadding.x * 2);
        const float  contentWidth = iconSize + gap + textSize.x;
        const float  left =
            start.x + style.FramePadding.x + std::max(0.0f, usableWidth - contentWidth) * style.ButtonTextAlign.x;
        const float top = start.y + (end.y - start.y - iconSize) * 0.5f;
        if (ImGui::IsItemVisible()) {
                ImDrawList *draw = ImGui::GetWindowDrawList();
                draw->PushClipRect(ImVec2(start.x + style.FramePadding.x, start.y),
                                   ImVec2(std::max(start.x + style.FramePadding.x, end.x - style.FramePadding.x), end.y),
                                   true);
                DrawIcon(draw, icon, ImVec2(left, top), iconSize, foreground);
                if (textSize.x > 0)
                        draw->AddText(ImVec2(left + iconSize + gap, start.y + (end.y - start.y - textSize.y) * 0.5f),
                                      foreground,
                                      label,
                                      labelEnd);
                draw->PopClipRect();
        }
        if (colors)
                ImGui::PopStyleColor(colors);
        if (contentWidth > usableWidth + 0.5f && textSize.x > 0 &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
                ImGui::TextUnformatted(label, labelEnd);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
        }
        return pressed;
}
} // namespace ui
#endif
