#ifndef AVA_DOCK_FOLD_HPP
#define AVA_DOCK_FOLD_HPP

#include "gui/i18n.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Preserve the dock tree and tab identities while lending a folded pane's
// space to its sibling. The original ratio also survives viewport resizing
namespace dock_fold
{
struct Fold {
        ImGuiID node{}, parent{};
        int     axis{};
        float   ratio{};
};
inline std::vector<Fold> folded;

inline bool
matches(const Fold &fold, const ImGuiDockNode *node)
{
        return node && node->IsLeafNode() && node->ParentNode && node->ParentNode->ID == fold.parent &&
               node->ParentNode->SplitAxis == fold.axis;
}

inline bool
isFolded(const ImGuiDockNode *node)
{
        return node && std::any_of(folded.begin(), folded.end(), [&](const Fold &fold) {
                       return fold.node == node->ID && matches(fold, node);
               });
}

// During session restore, hidden dock branches can have a valid split axis
// but no measured size yet. Do not commit either sibling until both are ready.
inline bool
canResize(const ImGuiDockNode *node)
{
        if (!node || !node->ParentNode || !node->ParentNode->IsSplitNode())
                return false;
        const auto *parent = node->ParentNode;
        if ((parent->SplitAxis != ImGuiAxis_X && parent->SplitAxis != ImGuiAxis_Y) ||
            (parent->ChildNodes[0] != node && parent->ChildNodes[1] != node))
                return false;
        for (const auto *part : {parent,
                                 static_cast<const ImGuiDockNode *>(parent->ChildNodes[0]),
                                 static_cast<const ImGuiDockNode *>(parent->ChildNodes[1])}) {
                if (!part || !std::isfinite(part->Size.x) || !std::isfinite(part->Size.y) || part->Size.x <= 0 ||
                    part->Size.y <= 0)
                        return false;
        }
        const float available = parent->Size[parent->SplitAxis] - ImGui::GetStyle().DockingSeparatorSize;
        return std::isfinite(available) && available > 2;
}

inline void
resize(ImGuiDockNode *node, float extent)
{
        if (!canResize(node) || !std::isfinite(extent))
                return;
        auto       *parent    = node->ParentNode;
        auto       *sibling   = parent->ChildNodes[parent->ChildNodes[0] == node ? 1 : 0];
        const int   axis      = parent->SplitAxis;
        const float available = parent->Size[axis] - ImGui::GetStyle().DockingSeparatorSize;
        extent                = std::clamp(extent, 1.0f, available - 1.0f);
        ImVec2 size = node->Size, other = sibling->Size;
        size[axis]  = extent;
        other[axis] = available - extent;
        ImGui::DockBuilderSetNodeSize(node->ID, size);
        ImGui::DockBuilderSetNodeSize(sibling->ID, other);
        // Docking's central-node policy must not override an explicit fold
        node->WantLockSizeOnce    = true;
        sibling->WantLockSizeOnce = false;
}

inline void
restore(const Fold &fold)
{
        auto *node = ImGui::DockBuilderGetNode(fold.node);
        if (matches(fold, node))
                resize(node, (node->ParentNode->Size[fold.axis] - ImGui::GetStyle().DockingSeparatorSize) * fold.ratio);
}

inline void
toggle(ImGuiDockNode *node)
{
        if (!canResize(node))
                return;
        const auto found = std::find_if(
            folded.begin(), folded.end(), [&](const Fold &fold) { return fold.node == node->ID && matches(fold, node); });
        if (found != folded.end()) {
                restore(*found);
                folded.erase(found);
        } else {
                // Two siblings cannot both borrow the same space. Expanding the
                // first before folding the second preserves their original ratio
                for (auto it = folded.begin(); it != folded.end();) {
                        if (it->parent == node->ParentNode->ID) {
                                restore(*it);
                                it = folded.erase(it);
                        } else {
                                ++it;
                        }
                }
                const int   axis      = node->ParentNode->SplitAxis;
                const float available = node->ParentNode->Size[axis] - ImGui::GetStyle().DockingSeparatorSize;
                if (available <= 2)
                        return;
                folded.push_back(
                    {node->ID, node->ParentNode->ID, axis, std::clamp(node->Size[axis] / available, 0.01f, 0.99f)});
        }
        ImGui::MarkIniSettingsDirty();
}

inline void
registerSettings()
{
        folded.clear();
        ImGuiSettingsHandler handler;
        handler.TypeName   = "AvADockFold";
        handler.TypeHash   = ImHashStr(handler.TypeName);
        handler.ClearAllFn = handler.ReadInitFn = [](ImGuiContext *, ImGuiSettingsHandler *) { folded.clear(); };
        handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *) -> void * { return &folded; };
        handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line) {
                Fold fold;
                if (std::sscanf(
                        line, "Node=%x Parent=%x Axis=%d Ratio=%f", &fold.node, &fold.parent, &fold.axis, &fold.ratio) == 4 &&
                    fold.node && fold.parent && (fold.axis == ImGuiAxis_X || fold.axis == ImGuiAxis_Y) &&
                    std::isfinite(fold.ratio) && fold.ratio > 0 && fold.ratio < 1 && folded.size() < 256 &&
                    std::none_of(
                        folded.begin(),
                        folded.end(),
                        [&](const Fold &old) { return old.node == fold.node || old.parent == fold.parent; }))
                        folded.push_back(fold);
        };
        handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *, ImGuiTextBuffer *output) {
                if (folded.empty())
                        return;
                output->append("[AvADockFold][State]\n");
                for (const auto &fold : folded)
                        if (matches(fold, ImGui::DockBuilderGetNode(fold.node)))
                                output->appendf("Node=%08X Parent=%08X Axis=%d Ratio=%.9g\n",
                                                fold.node,
                                                fold.parent,
                                                fold.axis,
                                                fold.ratio);
                output->append("\n");
        };
        ImGui::AddSettingsHandler(&handler);
}

inline bool
isTabLabelHovered(const ImGuiWindow *window)
{
        if (!window || !window->DockIsActive || !window->DockNode || !window->DockNode->TabBar ||
            ImGui::GetCurrentContext()->HoveredId != window->TabId)
                return false;
        auto *tab = ImGui::TabBarFindTabByID(window->DockNode->TabBar, window->TabId);
        if (!tab)
                return false;
        ImRect label = window->DC.DockTabItemRect;
        if (!(tab->Flags & ImGuiTabItemFlags_NoCloseButton))
                label.Max.x -= ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize();
        return ImGui::IsMouseHoveringRect(label.Min, label.Max, false);
}

inline ImRect
emptyTitleBarRect(const ImGuiDockNode *node)
{
        if (!node || !node->TabBar)
                return ImRect();
        const auto *bar   = node->TabBar;
        ImRect      empty = bar->BarRect;
        // DockNodeCalcTabBarLayout reserves this strip even when tabs overflow
        const auto &style = ImGui::GetStyle();
        empty.Max.x       = node->Pos.x + node->Size.x - style.WindowBorderSize - style.FramePadding.x;
        if (node->HasCloseButton)
                empty.Max.x -= ImGui::GetFontSize() + style.ItemInnerSpacing.x;
        if (node->HasWindowMenuButton && style.WindowMenuButtonPosition == ImGuiDir_Right)
                empty.Max.x -= ImGui::GetFontSize() + style.ItemInnerSpacing.x;
        for (auto *window : node->Windows)
                if (window->LastFrameActive == ImGui::GetFrameCount())
                        empty.Min.x = std::max(empty.Min.x, std::min(window->DC.DockTabItemRect.Max.x, bar->BarRect.Max.x));
        if (bar->ScrollButtonEnabled)
                empty.Min.x = std::max(empty.Min.x, empty.Max.x - ImGui::GetFrameHeight());
        return empty;
}

/**
 * @brief Called after all dockable windows. Only the empty title-bar area folds panes
 */
inline void
draw()
{
        auto *context = ImGui::GetCurrentContext();
        folded.erase(std::remove_if(folded.begin(),
                                    folded.end(),
                                    [](const Fold &fold) { return !matches(fold, ImGui::DockBuilderGetNode(fold.node)); }),
                     folded.end());
        for (const auto &entry : context->DockContext.Nodes.Data) {
                auto *node = static_cast<ImGuiDockNode *>(entry.val_p);
                if (!node || !node->IsLeafNode() || !node->ParentNode || !node->IsVisible || !node->VisibleWindow ||
                    node->VisibleWindow->LastFrameActive != context->FrameCount ||
                    !node->ParentNode->ChildNodes[0]->IsVisible || !node->ParentNode->ChildNodes[1]->IsVisible ||
                    node->IsHiddenTabBar() || !node->TabBar)
                        continue;
                const auto empty   = emptyTitleBarRect(node);
                const auto titleId = ImHashStr("#TITLEBAR", 0, node->ID);
                if (ImGui::IsMouseHoveringRect(empty.Min, empty.Max, false) && context->HoveredWindow &&
                    context->HoveredWindow->RootWindow == node->HostWindow->RootWindow &&
                    (context->HoveredId == 0 || context->HoveredId == titleId) &&
                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                    !context->DragDropActive) {
                        ImGui::SetTooltip(
                            "%s",
                            node->ParentNode->SplitAxis == ImGuiAxis_Y
                                ? tr("Double-click left button here to fold/expand vertically", "此处双击左键，上下折叠/展开")
                                : tr("Double-click right button here to fold/expand horizontally",
                                     "此处双击右键，左右折叠/展开"));
                        const auto button =
                            node->ParentNode->SplitAxis == ImGuiAxis_Y ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
                        if (ImGui::IsMouseDoubleClicked(button))
                                toggle(node);
                }
        }
        for (const auto &fold : folded) {
                auto *node = ImGui::DockBuilderGetNode(fold.node);
                if (!matches(fold, node))
                        continue;
                const float available = node->ParentNode->Size[fold.axis] - ImGui::GetStyle().DockingSeparatorSize;
                const float compact   = fold.axis == ImGuiAxis_Y
                                            ? std::max(ImGui::GetStyle().WindowMinSize.y, ImGui::GetFrameHeight())
                                            : ImGui::GetFrameHeight() * 5;
                resize(node, std::min(compact, available * 0.5f));
        }
}
} // namespace dock_fold
#endif
