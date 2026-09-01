#ifndef AVA_WINDOW_CORNERS_HPP
#define AVA_WINDOW_CORNERS_HPP
#include "gui/design_tokens.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>

namespace ui
{
// Decorate ImGui's existing splitter without adding another hit target. The
// host draw list keeps grips underneath dock contents, floating windows and menus.
inline void
DrawDockSplitterGrips(ImGuiDockNode *node, ImGuiWindow *host)
{
        if (!node || !node->IsVisible || node->IsLeafNode())
                return;
        auto *first  = node->ChildNodes[0];
        auto *second = node->ChildNodes[1];
        if (first && second && first->IsVisible && second->IsVisible && node->SplitAxis != ImGuiAxis_None) {
                const int axis = node->SplitAxis;
                ImRect    gap(first->Pos, second->Pos);
                gap.Min[axis]     += first->Size[axis];
                gap.Max[axis ^ 1] += second->Size[axis ^ 1];
                if (gap.GetWidth() > 0 && gap.GetHeight() > 0) {
                        auto *draw = host->DrawList;
                        draw->AddRectFilled(gap.Min, gap.Max, ImGui::GetColorU32(ImGuiCol_DockingEmptyBg));
                        const auto flags  = first->MergedFlags | second->MergedFlags;
                        const auto locked = ImGuiDockNodeFlags_NoResize |
                                            (axis == ImGuiAxis_X ? ImGuiDockNodeFlags_NoResizeX : ImGuiDockNodeFlags_NoResizeY);
                        if (!(flags & locked)) {
                                // Use the native splitter's ID for hover/drag feedback.
                                const auto  seed    = ImHashData(&node->ID, sizeof(node->ID), host->ID);
                                const auto  id      = ImHashStr("##Splitter", 0, seed);
                                const auto &g       = *ImGui::GetCurrentContext();
                                const bool  active  = g.ActiveId == id;
                                const bool  hovered = g.HoveredId == id;
                                const ImU32 color =
                                    ImGui::GetColorU32(active || hovered ? ImGuiCol_SliderGrab : ImGuiCol_TextDisabled,
                                                       active    ? 1.0f
                                                       : hovered ? .9f
                                                                 : .65f);
                                const float width  = gap.Max[axis] - gap.Min[axis];
                                const float radius = width * .12f;
                                const float pitch  = width * .45f;
                                if (gap.Max[axis ^ 1] - gap.Min[axis ^ 1] >= (pitch + radius) * 2) {
                                        for (int dot = -1; dot <= 1; ++dot) {
                                                ImVec2 center     = gap.GetCenter();
                                                center[axis ^ 1] += dot * pitch;
                                                draw->AddCircleFilled(center, radius, color, 12);
                                        }
                                }
                        }
                }
        }
        DrawDockSplitterGrips(first, host);
        DrawDockSplitterGrips(second, host);
}

// Dock tabs and contents are drawn separately by ImGui. Finish each visible
// leaf on its own draw list so popup windows still render above the corners.
inline void
DrawDockCorners()
{
        auto &g = *ImGui::GetCurrentContext();
        for (const auto &entry : g.DockContext.Nodes.Data) {
                auto *node = static_cast<ImGuiDockNode *>(entry.val_p);
                if (!node || !node->IsRootNode() || !node->IsVisible || !node->HostWindow || !node->HostWindow->Active ||
                    node->HostWindow->Hidden)
                        continue;
                auto *host = node->HostWindow;
                host->DrawList->PushClipRect(host->ClipRect.Min, host->ClipRect.Max, false);
                DrawDockSplitterGrips(node, host);
                host->DrawList->PopClipRect();
        }
        const float radius = ImMin(g.Style.WindowRounding, g.Style.WindowPadding.y);
        if (radius <= 0)
                return;
        for (auto *window : g.Windows) {
                if (!window->Active || window->Hidden || !window->DockIsActive || !window->DockTabIsVisible ||
                    !window->DockNode)
                        continue;
                auto        *node = window->DockNode;
                const ImVec2 lo   = node->Pos, hi(node->Pos.x + node->Size.x, node->Pos.y + node->Size.y);
                const float  r    = ImMin(radius, ImMin(node->Size.x, node->Size.y) * 0.5f);
                auto        *draw = window->DrawList;
                draw->PushClipRect(lo, hi, false);
                const ImU32 bg = ImGui::GetColorU32(ImGuiCol_DockingEmptyBg);
                for (int corner = 0; corner < 4; ++corner) {
                        const float  sx = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
                        const float  sy = corner < 2 ? -1.0f : 1.0f;
                        const ImVec2 tip(sx < 0 ? lo.x : hi.x, sy < 0 ? lo.y : hi.y);
                        const ImVec2 center(tip.x - sx * r, tip.y - sy * r);
                        ImVec2       previous(center.x + sx * r, center.y);
                        for (int segment = 1; segment <= 12; ++segment) {
                                const float  angle = segment * (IM_PI * 0.5f / 12.0f);
                                const ImVec2 next(center.x + sx * r * std::cos(angle), center.y + sy * r * std::sin(angle));
                                draw->AddTriangleFilled(tip, previous, next, bg);
                                previous = next;
                        }
                }
                draw->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_Border), r, 0, design::Stroke());
                draw->PopClipRect();
        }
}
} // namespace ui
#endif
