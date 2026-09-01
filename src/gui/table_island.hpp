#pragma once

#include "gui/ui_theme.hpp"
#include "imgui_internal.h"
#include <cmath>

namespace ui
{
// Keep native table/column IDs, scrolling and hit targets. This adds only the
// card surface and finishing corners, without another child window or ID scope.
class TableIsland
{
        bool   open_{};
        float  rounding_{};
        ImVec4 backdrop_{};
        ImRect clip_{};

      public:
        TableIsland()                               = default;
        TableIsland(const TableIsland &)            = delete;
        TableIsland &operator=(const TableIsland &) = delete;
        ~TableIsland() { end(); }

        bool
        begin(const char *id, int columns, ImGuiTableFlags flags = 0, const ImVec2 &size = ImVec2(0, 0), float innerWidth = 0)
        {
                IM_ASSERT(!open_);
                const auto &style  = ImGui::GetStyle();
                const auto *window = ImGui::GetCurrentWindow();
                clip_              = window->ClipRect;
                backdrop_ = style.Colors[(window->Flags & ImGuiWindowFlags_Popup) ? ImGuiCol_PopupBg : ImGuiCol_WindowBg];
                if (window->Flags & ImGuiWindowFlags_ChildWindow) {
                        const auto child = style.Colors[ImGuiCol_ChildBg];
                        backdrop_        = MixSurface(backdrop_, child, child.w);
                }
                const auto surface = MixSurface(backdrop_, style.Colors[ImGuiCol_FrameBg], .35f);
                const auto accent  = style.Colors[ImGuiCol_CheckMark];
                rounding_          = style.ChildRounding;
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding_);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
                ImGui::PushStyleColor(ImGuiCol_TableRowBg, surface);
                ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, MixSurface(surface, accent, .018f));
                ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, MixSurface(surface, accent, .075f));
                ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, MixSurface(surface, accent, .10f));
                ImGui::PushStyleColor(ImGuiCol_TableBorderLight, MixSurface(surface, accent, .035f));
                flags = (flags & ~ImGuiTableFlags_BordersOuter) | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_RowBg;
                open_ = ImGui::BeginTable(id, columns, flags, size, innerWidth);
                if (!open_) {
                        ImGui::PopStyleColor(6);
                        ImGui::PopStyleVar();
                }
                return open_;
        }

        void end()
        {
                if (!open_)
                        return;
                auto *table = ImGui::GetCurrentTable();
                auto *draw  = table->InnerWindow->DrawList;
                ImGui::EndTable();
                // Non-scrolling table height is only final after EndTable().
                const ImRect rect = table->OuterRect;
                ImGui::PopStyleColor(6);
                ImGui::PopStyleVar();
                open_ = false;
                if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
                        return;
                const float r = ImMin(rounding_, ImMin(rect.GetWidth(), rect.GetHeight()) * .5f);
                draw->PushClipRect(clip_.Min, clip_.Max, false);
                // Table row fills are rectangular. Mask only the outer corners,
                // leaving every member of a hierarchy block joined seamlessly.
                const ImU32 background = ImGui::ColorConvertFloat4ToU32(backdrop_);
                for (int corner = 0; corner < 4; ++corner) {
                        const float  sx = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
                        const float  sy = corner < 2 ? -1.0f : 1.0f;
                        const ImVec2 tip(sx < 0 ? rect.Min.x : rect.Max.x, sy < 0 ? rect.Min.y : rect.Max.y);
                        const ImVec2 center(tip.x - sx * r, tip.y - sy * r);
                        ImVec2       previous(center.x + sx * r, center.y);
                        for (int segment = 1; segment <= 12; ++segment) {
                                const float  angle = segment * (IM_PI * .5f / 12.0f);
                                const ImVec2 next(center.x + sx * r * std::cos(angle), center.y + sy * r * std::sin(angle));
                                draw->AddTriangleFilled(tip, previous, next, background);
                                previous = next;
                        }
                }
                draw->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(ImGuiCol_Border, .65f), r, 0, design::Stroke());
                draw->PopClipRect();
        }
};
} // namespace ui
