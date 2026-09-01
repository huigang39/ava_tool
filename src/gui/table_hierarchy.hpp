#pragma once
#include "gui/ui_theme.hpp"
#include "imgui_internal.h"
#include <unordered_map>
#include <vector>

namespace ui
{
struct HierarchyNodeState {
        int    parent{-1}, depth{};
        ImVec2 anchor;
        ImRect label;
        bool   active{}, ancestor{}, leaf{};
};
struct HierarchyBranchState {
        int  row{-1}, node{-1};
        bool virtualRoot{};
};
struct HierarchyRowState {
        int                               row{-1}, depth{-1}, emphasis{}; // hover, selection, context target
        int                               node{-1};
        bool                              finished{};
        std::vector<HierarchyNodeState>   nodes;
        std::vector<HierarchyBranchState> branches;
};
inline std::unordered_map<ImGuiTable *, HierarchyRowState> hierarchyRows;

inline HierarchyRowState *
CurrentHierarchyRow()
{
        auto      *table = ImGui::GetCurrentTable();
        const auto found = hierarchyRows.find(table);
        if (found == hierarchyRows.end())
                return nullptr;
        auto &row = found->second;
        if (row.row != table->CurrentRow) {
                row.row   = table->CurrentRow;
                row.depth = row.node = -1;
                row.emphasis         = 0;
                row.finished         = false;
        }
        return &row;
}

inline int
HierarchyParent(const HierarchyRowState &row)
{
        for (auto branch = row.branches.rbegin(); branch != row.branches.rend(); ++branch)
                if (!branch->virtualRoot && branch->row < row.row && branch->node >= 0)
                        return branch->node;
        return -1;
}

// Indent only the name column. Deep structures use a smaller step so their
// values stay aligned and long member names retain room in the same column.
inline void
HierarchyName()
{
        auto *table = ImGui::GetCurrentTable();
        auto *row   = CurrentHierarchyRow();
        if (!table || !row || table->CurrentColumn != 0)
                return;
        const int   parent = HierarchyParent(*row);
        const int   depth  = parent >= 0 ? row->nodes[parent].depth + 1 : 0;
        const float indent = ImGui::GetFontSize() * (.75f * ImMin(depth, 6) + .35f * ImMax(0, depth - 6));
        ImGui::SetCursorScreenPos(ImVec2(table->Columns[0].WorkMinX + indent, ImGui::GetCursorScreenPos().y));
}

inline void
RecordHierarchyNode(const ImVec2 &anchor, const ImRect &label, bool leaf = false)
{
        auto *table = ImGui::GetCurrentTable();
        auto *row   = CurrentHierarchyRow();
        if (!table || !row || table->CurrentColumn != 0)
                return;
        const int parent = HierarchyParent(*row);
        row->node        = static_cast<int>(row->nodes.size());
        row->nodes.push_back({parent, parent >= 0 ? row->nodes[parent].depth + 1 : 0, anchor, label, false, false, leaf});
        for (auto &branch : row->branches)
                if (!branch.virtualRoot && branch.row == row->row)
                        branch.node = row->node;
}

inline void
HierarchyLeaf(const char *label)
{
        const auto position = ImGui::GetItemRectMin();
        const auto textSize = ImGui::CalcTextSize(label, nullptr, true);
        RecordHierarchyNode(ImVec2(position.x - ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetFontSize() * .5f,
                                   position.y + ImGui::GetFontSize() * .5f),
                            ImRect(position, ImVec2(position.x + textSize.x, position.y + textSize.y)),
                            true);
}

inline ImVec4
HierarchySurface()
{
        const auto row = ImGui::GetStyleColorVec4(ImGuiCol_TableRowBg);
        return row.w >= 1.0f ? row : ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
}

inline ImVec4
HierarchyColor(int)
{
        return HierarchySurface();
}

inline void
HierarchyRow(int depth)
{
        if (ImGui::GetCurrentTable())
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(HierarchyColor(depth)));
        if (auto *row = CurrentHierarchyRow())
                row->depth = ImMax(depth, 0);
}

inline void
HierarchySelection(bool contextTarget = false)
{
        if (auto *row = CurrentHierarchyRow())
                row->emphasis = ImMax(row->emphasis, contextTarget ? 3 : 2);
}

// Finish the native row first so editable cells determine its actual height.
// Painting in its background channel keeps the rounded fill below all text,
// editors and frozen columns, without changing selection or row geometry.
inline void
FinishHierarchyRow()
{
        auto *table = ImGui::GetCurrentTable();
        auto *row   = CurrentHierarchyRow();
        if (!table || !row)
                return;
        const auto clip = table->BgClipRect;
        if (table->IsInsideRow)
                ImGui::TableEndRow(table);
        if (row->finished)
                return;
        row->finished = true;
        if (row->node >= 0) {
                const auto mouse = ImGui::GetIO().MousePos;
                if (row->emphasis >= 2 ||
                    (clip.Contains(mouse) && mouse.y >= table->RowPosY1 && mouse.y < table->RowPosY2 &&
                     ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy)))
                        row->nodes[row->node].active = true;
        }
        const int emphasis = row->emphasis;
        row->emphasis      = 0;
        if (!emphasis)
                return;
        const float  left  = ImMax(table->WorkRect.Min.x, clip.Min.x);
        const float  right = ImMin(table->WorkRect.Max.x, clip.Max.x);
        const float  inset = ImMin((.12f + .22f * ImClamp(row->depth, 0, 8)) * ImGui::GetFontSize(), (right - left) * .15f);
        const ImRect rect(ImVec2(left + inset + 1, table->RowPosY1 + 1), ImVec2(right - inset - 1, table->RowPosY2 - 1));
        if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0 || !rect.Overlaps(clip))
                return;
        const auto base   = row->depth >= 0 ? HierarchyColor(row->depth) : HierarchySurface();
        const auto accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        auto       fill   = MixSurface(base, accent, emphasis == 3 ? .24f : emphasis == 2 ? .18f : .075f);
        const auto text   = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        for (int i = 0; i < 6 && ColorContrast(text, fill) < 4.5f; ++i)
                fill = MixSurface(fill, base, .2f);
        const auto  border  = ReadableTextColor(accent, fill, 2.0f);
        const float radius  = ImMin(ImGui::GetFontSize() * .35f, rect.GetHeight() * .5f);
        auto       *draw    = table->InnerWindow->DrawList;
        const int   channel = table->DrawSplitter->_Current;
        table->DrawSplitter->SetCurrentChannel(draw, 0);
        draw->PushClipRect(clip.Min, clip.Max, false);
        draw->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(fill), radius);
        if (emphasis >= 2)
                draw->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(border), radius, 0, design::Stroke());
        draw->PopClipRect();
        table->DrawSplitter->SetCurrentChannel(draw, channel);
}

inline void
HierarchyNextRow(ImGuiTableRowFlags flags = 0, float minimumHeight = 0)
{
        FinishHierarchyRow();
        ImGui::TableNextRow(flags, minimumHeight);
}

// ImGui paints table headers as a full-width rectangle. Keep its native row and
// hit targets intact, but mask the four corners in the following draw channel so
// the visible header follows the rounded cards.
inline void
HierarchyHeadersRow()
{
        ImGui::TableHeadersRow();

        auto *table = ImGui::GetCurrentTable();
        if (!table)
                return;
        const auto   clip  = table->BgClipRect;
        const float  left  = ImMax(table->WorkRect.Min.x, clip.Min.x);
        const float  right = ImMin(table->WorkRect.Max.x, clip.Max.x);
        const ImRect rect(ImVec2(left, table->RowPosY1), ImVec2(right, table->RowPosY1 + table->RowMinHeight));
        if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0 || !rect.Overlaps(clip))
                return;

        const auto  surface  = HierarchySurface();
        const auto  border   = ImGui::GetStyleColorVec4(ImGuiCol_TableBorderStrong);
        const float radius   = ImMin(ImGui::GetFontSize() * .45f, rect.GetHeight() * .5f);
        const ImU32 backdrop = ImGui::GetColorU32(surface);
        auto       *draw     = table->InnerWindow->DrawList;
        draw->PushClipRect(clip.Min, clip.Max, false);
        for (int corner = 0; corner < 4; ++corner) {
                const float  sx = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
                const float  sy = corner < 2 ? -1.0f : 1.0f;
                const ImVec2 tip(sx < 0 ? rect.Min.x : rect.Max.x, sy < 0 ? rect.Min.y : rect.Max.y);
                const ImVec2 center(tip.x - sx * radius, tip.y - sy * radius);
                ImVec2       previous(center.x + sx * radius, center.y);
                for (int segment = 1; segment <= 12; ++segment) {
                        const float  angle = segment * (IM_PI * .5f / 12.0f);
                        const ImVec2 next(center.x + sx * radius * std::cos(angle), center.y + sy * radius * std::sin(angle));
                        draw->AddTriangleFilled(tip, previous, next, backdrop);
                        previous = next;
                }
        }
        draw->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(border), radius, 0, design::Stroke());
        draw->PopClipRect();
}

inline bool
HierarchyContextItem()
{
        const auto id = ImGui::GetItemID();
        if (id && (ImGui::IsPopupOpen(id, ImGuiPopupFlags_None) ||
                   (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))))
                HierarchySelection(true);
        return ImGui::BeginPopupContextItem();
}

// Emphasize the expanded label without changing the native tree's hit area,
// column widths, font metrics or ID. This also works with merged CJK fonts.
inline bool
HierarchyNode(const char *label, ImGuiTreeNodeFlags flags = 0)
{
        HierarchyName();
        const float arrowX    = ImGui::GetCursorScreenPos().x + ImGui::GetFontSize() * .5f;
        auto        position  = ImGui::GetCursorScreenPos();
        position.x           += ImGui::GetTreeNodeToLabelSpacing();
        position.y           += ImMax(
            ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset,
            flags & (ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding) ? ImGui::GetStyle().FramePadding.y : 0.0f);
        const bool rounded = CurrentHierarchyRow() != nullptr;
        if (rounded) {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        }
        const bool open     = ImGui::TreeNodeEx(label, flags);
        const auto textSize = ImGui::CalcTextSize(label, nullptr, true);
        RecordHierarchyNode(ImVec2(arrowX, position.y + ImGui::GetFontSize() * .5f),
                            ImRect(position, ImVec2(position.x + textSize.x, position.y + textSize.y)),
                            (flags & ImGuiTreeNodeFlags_Leaf) != 0);
        if (rounded) {
                ImGui::PopStyleColor(3);
                if (auto *row = CurrentHierarchyRow()) {
                        if (flags & ImGuiTreeNodeFlags_Selected)
                                row->emphasis = ImMax(row->emphasis, 2);
                        else if (ImGui::IsItemHovered())
                                row->emphasis = ImMax(row->emphasis, 1);
                }
        }
        if (open && !(flags & ImGuiTreeNodeFlags_Leaf) && ImGui::IsItemVisible()) {
                const auto   clip = ImGui::GetCurrentWindow()->ClipRect;
                const ImVec4 textClip(clip.Min.x, clip.Min.y, clip.Max.x, clip.Max.y);
                position.x += .45f;
                ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),
                                                    ImGui::GetFontSize(),
                                                    position,
                                                    ImGui::GetColorU32(ImGuiCol_Text),
                                                    label,
                                                    ImGui::FindRenderedTextEnd(label),
                                                    0,
                                                    &textClip);
        }
        return open;
}

// Draw after the row backgrounds, in the name column only. Actual parent
// indices keep sibling branches separate even when several nodes share a name.
inline void
DrawHierarchyPaths()
{
        auto *table = ImGui::GetCurrentTable();
        auto *row   = CurrentHierarchyRow();
        if (!table || !row || row->nodes.empty())
                return;
        auto &nodes = row->nodes;
        for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i)
                if (nodes[i].active && nodes[i].parent >= 0) {
                        nodes[nodes[i].parent].active   = true;
                        nodes[nodes[i].parent].ancestor = true;
                }
        ImRect clip = table->BgClipRect;
        clip.ClipWith(table->Columns[0].ClipRect);
        if (clip.GetWidth() <= 0 || clip.GetHeight() <= 0)
                return;
        const auto  surface = HierarchySurface();
        const auto  accent  = ReadableTextColor(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), surface, 3.0f);
        const ImU32 quiet   = ImGui::GetColorU32(MixSurface(surface, ImGui::GetStyleColorVec4(ImGuiCol_Text), .28f));
        const ImU32 active  = ImGui::GetColorU32(accent);
        const float font    = ImGui::GetFontSize();
        auto       *draw    = table->InnerWindow->DrawList;
        const int   channel = table->DrawSplitter->_Current;
        table->DrawSplitter->SetCurrentChannel(draw, 0);
        draw->PushClipRect(clip.Min, clip.Max, false);
        for (int pass = 0; pass < 2; ++pass)
                for (const auto &node : nodes) {
                        if (node.parent < 0 || (pass == 1 && !node.active))
                                continue;
                        const auto &parent = nodes[node.parent];
                        if (node.anchor.y < clip.Min.y || parent.anchor.y > clip.Max.y)
                                continue;
                        const float endX = node.leaf ? node.label.Min.x - font * .3f : node.anchor.x - font * .4f;
                        draw->PathLineTo(ImVec2(parent.anchor.x, ImMax(parent.anchor.y + font * .4f, clip.Min.y)));
                        draw->PathLineTo(ImVec2(parent.anchor.x, ImMin(node.anchor.y, clip.Max.y)));
                        if (node.anchor.y <= clip.Max.y)
                                draw->PathLineTo(ImVec2(ImMax(parent.anchor.x, endX), node.anchor.y));
                        draw->PathStroke(
                            pass ? active : quiet, 0, pass ? ImMax(1.5f, font * .1f) : ImMax(1.0f, design::Stroke()));
                }
        for (const auto &node : nodes)
                if (node.ancestor && node.label.Overlaps(clip)) {
                        const ImVec2 minimum(node.label.Min.x - 2, node.label.Min.y - 1);
                        const ImVec2 maximum(node.label.Max.x + 3, node.label.Max.y + 2);
                        draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(MixSurface(surface, accent, .12f)), 3.0f);
                        draw->AddLine(ImVec2(minimum.x, maximum.y), maximum, active, ImMax(1.0f, design::Stroke()));
                }
        draw->PopClipRect();
        table->DrawSplitter->SetCurrentChannel(draw, channel);
}

// One shared header and native column/scroll settings; hierarchy lives in the
// name column, leaving data cells on a quiet, consistent surface.
class HierarchyTable
{
        bool open_{};

      public:
        HierarchyTable()                                  = default;
        HierarchyTable(const HierarchyTable &)            = delete;
        HierarchyTable &operator=(const HierarchyTable &) = delete;
        ~HierarchyTable() { end(); }

        bool begin(const char *id, int columns, ImGuiTableFlags flags, const ImVec2 &size)
        {
                IM_ASSERT(!open_);
                const auto surface = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
                ImGui::PushStyleColor(ImGuiCol_TableRowBg, surface);
                ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, surface);
                open_ = ImGui::BeginTable(id, columns, flags & ~ImGuiTableFlags_BordersOuter, size);
                if (!open_)
                        ImGui::PopStyleColor(3);
                else
                        hierarchyRows[ImGui::GetCurrentTable()] = {};
                return open_;
        }

        void end()
        {
                if (open_) {
                        FinishHierarchyRow();
                        DrawHierarchyPaths();
                        hierarchyRows.erase(ImGui::GetCurrentTable());
                        ImGui::EndTable();
                        ImGui::PopStyleColor(3);
                        open_ = false;
                }
        }
};

inline void
HierarchyGap()
{
        auto *table = ImGui::GetCurrentTable();
        if (!table)
                return;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 0));
        HierarchyNextRow(ImGuiTableRowFlags_None, ImGui::GetFontSize() * .18f);
        ImGui::TableEndRow(table);
        ImGui::PopStyleVar();
}

// Scope the current parent without adding windows, IDs or nested color blocks.
// Some callers begin before submitting their root node; focused views use a
// virtual root so their first visible level starts at the left edge.
class HierarchyCard
{
        ImGuiTable *table_{};
        int         depth_{};

      public:
        HierarchyCard()                                 = default;
        HierarchyCard(const HierarchyCard &)            = delete;
        HierarchyCard &operator=(const HierarchyCard &) = delete;
        ~HierarchyCard() { end(); }

        void begin(int depth, bool beforeFirstRow = false)
        {
                IM_ASSERT(!table_);
                table_    = ImGui::GetCurrentTable();
                auto *row = CurrentHierarchyRow();
                if (!table_ || !row) {
                        table_ = nullptr;
                        return;
                }
                if (beforeFirstRow)
                        FinishHierarchyRow();
                depth_ = ImMax(depth, 0);
                row->branches.push_back({table_->CurrentRow, beforeFirstRow ? -1 : row->node, beforeFirstRow});
        }

        void end(bool addTrailingGap = true)
        {
                if (!table_)
                        return;
                IM_ASSERT(table_ == ImGui::GetCurrentTable());
                FinishHierarchyRow();
                if (auto *row = CurrentHierarchyRow()) {
                        IM_ASSERT(!row->branches.empty());
                        row->branches.pop_back();
                }
                table_ = nullptr;
                if (depth_ == 0 && addTrailingGap)
                        HierarchyGap();
        }
};
} // namespace ui
