#ifndef AVA_TABLE_TEXT_HPP
#define AVA_TABLE_TEXT_HPP

#include "imgui.h"
#include "ui_theme.hpp"
#include <algorithm>
#include <cstring>
#include <string>

namespace ui
{
// Quiet horizontal rules; column resize handles appear on hover instead of
// drawing a boxed grid through every value.
inline constexpr ImGuiTableFlags DataTableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_PadOuterX;
// Hierarchy blocks span their member rows without horizontal rules cutting them apart.
inline constexpr ImGuiTableFlags HierarchyTableFlags = DataTableFlags & ~ImGuiTableFlags_BordersInnerH;
// Widths follow the actual font/DPI and translated header rather than fixed
// pixels. Pass a representative value such as "0xFFFFFFFF" for data columns.
inline float
TableColumnWidth(const char *heading, const char *sample = "", float minimumEm = 4.0f)
{
        const float text =
            std::max(ImGui::CalcTextSize(heading ? heading : "", nullptr, true).x, ImGui::CalcTextSize(sample ? sample : "").x);
        return std::max(text, ImGui::GetFontSize() * minimumEm) + ImGui::GetStyle().CellPadding.x * 2.0f;
}

inline void
ClippedTextTooltip(const char *text, float availableWidth)
{
        if (!text || ImGui::CalcTextSize(text).x <= availableWidth ||
            !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
}

// Keep values on one row. Resizable/scrolling columns retain normal behavior;
// when a user narrows a column, hover exposes its complete value.
inline void
TableText(const char *text)
{
        const float available = ImGui::GetContentRegionAvail().x;
        // These are acquisition error sentinels, not variable/field names that
        // happen to contain "error" (e.g. error_count).
        const bool error = text && (std::strcmp(text, "ERR") == 0 || std::strcmp(text, "ERROR") == 0);
        if (error)
                ImGui::PushStyleColor(ImGuiCol_Text, ErrorTextColor());
        ImGui::TextUnformatted(text ? text : "");
        ClippedTextTooltip(text, available);
        if (error)
                ImGui::PopStyleColor();
}

inline void
TableText(const std::string &text)
{
        TableText(text.c_str());
}

inline void
TableText(const char *text, const ImVec4 &color)
{
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        TableText(text);
        ImGui::PopStyleColor();
}

inline void
TableText(const std::string &text, const ImVec4 &color)
{
        TableText(text.c_str(), color);
}
} // namespace ui
#endif
