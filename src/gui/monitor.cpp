#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#include "gui/monitor.hpp"
#include "platform/native_dlg.hpp"

std::atomic<bool> g_monitorPaused{false};

std::vector<Monitor *> Monitor::sInstances_;
std::mutex             Monitor::sMtxInstances_;

class MonitorChannel;
class MonitorScope;

namespace
{
// Self-contained copy of one channel's series for a background CSV write.
struct CsvExportChannel {
        std::string      tag; // "<scope>::<channel>"
        std::vector<f64> ts;
        std::vector<f32> val;
};

std::string
humanSize(u64 bytes)
{
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        f64         v       = static_cast<f64>(bytes);
        int         u       = 0;
        while (v >= 1024.0 && u < 4) {
                v /= 1024.0;
                ++u;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
        return buf;
}
} // namespace

/* -------------------------------------------------------------------------- */

void
MonitorScope::menu()
{
        // Scope Toolbar
        // Scope Toolbar
        if (ImGui::Button(e_draw == DrawEnum::PLOT ? "Plot view" : "Table view")) {
                e_draw = (e_draw == DrawEnum::PLOT) ? DrawEnum::TABLE : DrawEnum::PLOT;
        }

        ImGui::SameLine();
        if (showFft_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f)); // Blue
                if (ImGui::Button("Freq domain")) {
                        showFft_ = false;
                }
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                const char *pointOptions[] = {"256",
                                              "512",
                                              "1024",
                                              "2048",
                                              "4096",
                                              "8192",
                                              "16384",
                                              "32768",
                                              "65536",
                                              "131072",
                                              "262144",
                                              "524288",
                                              "1048576"};
                i32         currentIdx     = 0;
                for (i32 i = 0; i < (i32)(sizeof(pointOptions) / sizeof(pointOptions[0])); ++i) {
                        if (fftPoints_ == atoi(pointOptions[i])) {
                                currentIdx = i;
                                break;
                        }
                }

                if (ImGui::Combo(
                        "##fftPoints", &currentIdx, pointOptions, (i32)(sizeof(pointOptions) / sizeof(pointOptions[0])))) {
                        i32 nextPoints = atoi(pointOptions[currentIdx]);
                        if (nextPoints != fftPoints_) {
                                reinitFft(nextPoints);
                        }
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("FFT Points (Resolution)");

                ImGui::SameLine();
                ImGui::SetNextItemWidth(30);
                char pkBuf[16];
                snprintf(pkBuf, sizeof(pkBuf), "%d", fftPeakCount_);
                if (ImGui::InputText("##fftPeakCount",
                                     pkBuf,
                                     sizeof(pkBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
                        fftPeakCount_ = atoi(pkBuf);
                        if (fftPeakCount_ < 0)
                                fftPeakCount_ = 0;
                        if (fftPeakCount_ > 20)
                                fftPeakCount_ = 20;
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Peaks: enter number of peaks and press Enter to confirm");

                ImGui::SameLine();
                if (fftBars_) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f)); // Blue (active)
                        if (ImGui::Button("Bar"))
                                fftBars_ = false;
                        ImGui::PopStyleColor();
                } else {
                        if (ImGui::Button("Line"))
                                fftBars_ = true;
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Toggle FFT render style: line / bar chart");
        } else {
                if (ImGui::Button("Time domain")) {
                        showFft_ = true;
                }
        }

        // Toggle the right-side data panel (Stats in time view / Peaks in freq view).
        ImGui::SameLine();
        if (showSidePanel_) {
                if (ImGui::Button("Hide table"))
                        showSidePanel_ = false;
        } else {
                if (ImGui::Button("Show table"))
                        showSidePanel_ = true;
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show/hide the side data panel (Stats / Peaks)");

        // Hide/show all plot lines in this scope.
        ImGui::SameLine();
        {
                bool anyVisible = false;
                for (auto &[_, ch] : chs_)
                        if (ch && ch->show_) {
                                anyVisible = true;
                                break;
                        }

                if (anyVisible) {
                        if (ImGui::Button("Hide line")) {
                                for (auto &[_, ch] : chs_)
                                        if (ch)
                                                ch->show_ = false;
                        }
                } else {
                        if (ImGui::Button("Show line")) {
                                for (auto &[_, ch] : chs_)
                                        if (ch)
                                                ch->show_ = true;
                        }
                }
        }

        // Right-aligned buttons: Delete Scope, Hide Scope, and Pause/Resume
        float delBtnWidth  = ImGui::CalcTextSize("Delete Scope").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float hideBtnWidth = std::max(ImGui::CalcTextSize("Hide Scope").x, ImGui::CalcTextSize("Show Scope").x) +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
        float pauseBtnWidth =
            std::max(ImGui::CalcTextSize("Pause").x, ImGui::CalcTextSize("Resume").x) + ImGui::GetStyle().FramePadding.x * 2.0f;
        float spacing         = ImGui::GetStyle().ItemSpacing.x;
        float totalRightWidth = delBtnWidth + spacing + hideBtnWidth + spacing + pauseBtnWidth;
        float availWidth      = ImGui::GetContentRegionAvail().x;

        if (availWidth > totalRightWidth) {
                ImGui::SameLine(ImGui::GetCursorPosX() + availWidth - totalRightWidth);
        } else {
                ImGui::SameLine();
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Delete Scope")) {
                markPendingDelete();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (hidden_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));
                if (ImGui::Button("Show Scope"))
                        hidden_ = false;
                ImGui::PopStyleColor();
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
                if (ImGui::Button("Hide Scope"))
                        hidden_ = true;
                ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        if (paused_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.8f, 0.2f, 0.6f)); // Yellow/Orange
                if (ImGui::Button("Resume"))
                        paused_ = false;
                ImGui::PopStyleColor();
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 0.6f)); // Green
                if (ImGui::Button("Pause"))
                        paused_ = true;
                ImGui::PopStyleColor();
        }

        ImGui::Separator();
}

void
MonitorScope::draw(double *linkXMin, double *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode)
{
        if (e_draw == DrawEnum::PLOT)
                plotDraw(linkXMin, linkXMax, maxDisplayPoints, mode);
        else if (e_draw == DrawEnum::TABLE)
                tableDraw();
}

void
MonitorScope::tableMenu()
{
}

void
MonitorScope::tableDraw()
{
        if (!ImGui::BeginTable("MonitorTable",
                               6,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_Sortable))
                return;

        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 100.0f);
        ImGui::TableHeadersRow();

        ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs();

        auto chOrder = [&](const std::string &k) -> i64 {
                auto it = chs_.find(k);
                return it != chs_.end() ? it->second->getOrder() : 0;
        };

        std::vector<std::string> keys;
        for (auto &pair : chs_)
                keys.push_back(pair.first);
        // Order channels by insertion order (e.g. CSV column order) rather than
        // alphabetically; fall back to name for a stable tiebreak.
        std::sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
                const i64 oa = chOrder(a), ob = chOrder(b);
                return oa != ob ? oa < ob : a < b;
        });

        // Build a prefix trie on '.' so struct members render as an expandable tree.
        struct TNode {
                std::map<std::string, TNode> children;
                std::string                  leafKey;                                // non-empty → this node is a channel
                i64                          order{std::numeric_limits<i64>::max()}; // min insertion order of leaves beneath
        };
        TNode root;
        for (const auto &k : keys) {
                const i64 ord   = chOrder(k);
                TNode    *cur   = &root;
                cur->order      = std::min(cur->order, ord);
                std::string rem = k;
                size_t      pos;
                while ((pos = rem.find('.')) != std::string::npos) {
                        cur        = &cur->children[rem.substr(0, pos)];
                        cur->order = std::min(cur->order, ord);
                        rem        = rem.substr(pos + 1);
                }
                TNode &leaf  = cur->children[rem];
                leaf.leafKey = k;
                leaf.order   = std::min(leaf.order, ord);
        }

        // Children of a trie node ordered by min insertion order (then label), or by column sort specs.
        auto orderedChildren = [&](TNode &n) {
                std::vector<std::pair<const std::string *, TNode *>> v;
                v.reserve(n.children.size());
                for (auto &[lbl, child] : n.children)
                        v.emplace_back(&lbl, &child);

                if (sortSpecs && sortSpecs->SpecsCount > 0) {
                        const auto *spec = &sortSpecs->Specs[0];
                        std::sort(v.begin(), v.end(), [&](const auto &a, const auto &b) -> bool {
                                bool isLeafA = !a.second->leafKey.empty();
                                bool isLeafB = !b.second->leafKey.empty();
                                if (isLeafA != isLeafB)
                                        return isLeafB; // Groups before leaves

                                int cmp = 0;
                                if (isLeafA && isLeafB) {
                                        auto &chA = chs_[a.second->leafKey];
                                        auto &chB = chs_[b.second->leafKey];
                                        switch (spec->ColumnIndex) {
                                                case 0: // Name
                                                        cmp = a.second->leafKey.compare(b.second->leafKey);
                                                        break;
                                                case 1: // Value
                                                        cmp = (chA->getDispVal() < chB->getDispVal())
                                                                  ? -1
                                                                  : (chA->getDispVal() > chB->getDispVal() ? 1 : 0);
                                                        break;
                                                case 2: // Type
                                                        cmp = chA->getType().compare(chB->getType());
                                                        break;
                                                case 3: // Address
                                                        cmp = (chA->getAddr() < chB->getAddr())
                                                                  ? -1
                                                                  : (chA->getAddr() > chB->getAddr() ? 1 : 0);
                                                        break;
                                                case 4: // Port
                                                        cmp = chA->getDevice().compare(chB->getDevice());
                                                        break;
                                        }
                                } else {
                                        cmp = a.first->compare(*b.first);
                                }

                                if (cmp == 0) {
                                        cmp = a.second->order < b.second->order ? -1
                                                                                : (a.second->order > b.second->order ? 1 : 0);
                                }
                                return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0) : (cmp > 0);
                        });
                } else {
                        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
                                return a.second->order != b.second->order ? a.second->order < b.second->order
                                                                          : *a.first < *b.first;
                        });
                }
                return v;
        };

        // Collect all leaf channel keys under a trie node (recursive).
        std::function<void(TNode &, std::vector<std::string> &)> collectLeaves;
        collectLeaves = [&](TNode &n, std::vector<std::string> &out) {
                if (!n.leafKey.empty()) {
                        out.push_back(n.leafKey);
                } else {
                        for (auto &[_, child] : n.children)
                                collectLeaves(child, out);
                }
        };

        // A group is highlighted if it equals a selected path or is a descendant of one.
        auto isGroupHighlighted = [&](const std::string &path) {
                for (const auto &sel : selectedGroupPaths_) {
                        if (path == sel)
                                return true;
                        if (path.size() > sel.size() + 1 && path.compare(0, sel.size(), sel) == 0 && path[sel.size()] == '.')
                                return true;
                }
                return false;
        };

        i32                                                                    rowIdx = 0;
        std::function<void(const std::string &, TNode &, const std::string &)> drawNode;
        drawNode = [&](const std::string &label, TNode &node, const std::string &fullPath) {
                if (!node.leafKey.empty()) {
                        // Leaf — draw a full interactive row, but show only the short label.
                        drawTableRow(node.leafKey, chs_[node.leafKey], rowIdx++, keys, label);
                } else {
                        // Collect all leaves under this group (for selection/deletion).
                        std::vector<std::string> groupLeaves;
                        collectLeaves(node, groupLeaves);

                        bool isGroupSelected = isGroupHighlighted(fullPath);

                        // Group header — show collapsible tree node, empty other columns.
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth;
                        if (isGroupSelected)
                                treeFlags |= ImGuiTreeNodeFlags_Selected;
                        const bool wasOpen = expandedGroups_.count(fullPath) > 0;
                        ImGui::SetNextItemOpen(wasOpen);
                        bool open = ImGui::TreeNodeEx(label.c_str(), treeFlags);
                        if (open != wasOpen) {
                                if (open)
                                        expandedGroups_.insert(fullPath);
                                else
                                        expandedGroups_.erase(fullPath);
                                if (parent_)
                                        parent_->setModified();
                        }

                        // Left-click on header row (not the expand arrow) → select this group.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                                if (ImGui::GetIO().KeyCtrl) {
                                        // Ctrl+click: toggle this group without deselecting others.
                                        if (isGroupSelected) {
                                                selectedGroupPaths_.erase(fullPath);
                                                for (auto &lk : groupLeaves) {
                                                        auto it = chs_.find(lk);
                                                        if (it != chs_.end())
                                                                it->second->selected_ = false;
                                                }
                                        } else {
                                                selectedGroupPaths_.insert(fullPath);
                                                for (auto &lk : groupLeaves) {
                                                        auto it = chs_.find(lk);
                                                        if (it != chs_.end())
                                                                it->second->selected_ = true;
                                                }
                                        }
                                } else {
                                        for (auto &pair : chs_)
                                                pair.second->selected_ = false;
                                        selectedGroupPaths_.clear();
                                        selectedGroupPaths_.insert(fullPath);
                                        for (auto &lk : groupLeaves) {
                                                auto it = chs_.find(lk);
                                                if (it != chs_.end())
                                                        it->second->selected_ = true;
                                        }
                                }
                        }

                        // Right-click: auto-select this group when not selected, then show unified menu.
                        if (ImGui::BeginPopupContextItem()) {
                                if (!isGroupSelected) {
                                        for (auto &pair : chs_)
                                                pair.second->selected_ = false;
                                        selectedGroupPaths_.clear();
                                        selectedGroupPaths_.insert(fullPath);
                                        for (auto &lk : groupLeaves) {
                                                auto it = chs_.find(lk);
                                                if (it != chs_.end())
                                                        it->second->selected_ = true;
                                        }
                                }
                                if (ImGui::MenuItem("Delete Selected")) {
                                        for (auto &pair : chs_)
                                                if (pair.second->selected_)
                                                        pair.second->markPendingDelete();
                                        selectedGroupPaths_.clear();
                                }
                                ImGui::EndPopup();
                        }

                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        if (open) {
                                for (auto &[childLabel, childNode] : orderedChildren(node))
                                        drawNode(*childLabel, *childNode, fullPath + "." + *childLabel);
                                ImGui::TreePop();
                        }
                }
        };

        for (auto &[childLabel, childNode] : orderedChildren(root))
                drawNode(*childLabel, *childNode, *childLabel);

        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
                for (auto &pair : chs_)
                        pair.second->selected_ = false;
                selectedGroupPaths_.clear();
                lastSelectedIndex_ = -1;
        }

        ImGui::EndTable();
}

static bool
isIntegerType(const std::string &t)
{
        if (t.empty())
                return false;
        char c = t[0];
        return (c == 'U' || c == 'I') && (t != "U32_HEX");
}

void
MonitorScope::drawTableRow(const std::string               &chName,
                           std::shared_ptr<MonitorChannel> &ch,
                           i32                              idx,
                           const std::vector<std::string>  &allKeys,
                           const std::string               &displayLabel)
{
        ImGui::PushID(chName.c_str());
        ImGui::TableNextRow();

        // 1. Name (Selectable for Shift/Ctrl support)
        ImGui::TableNextColumn();
        const char *label      = displayLabel.empty() ? chName.c_str() : displayLabel.c_str();
        bool        isSelected = ch->selected_;
        if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                selectedGroupPaths_.clear();
                if (ImGui::GetIO().KeyCtrl) {
                        // Ctrl + Click: Toggle current
                        ch->selected_ = !ch->selected_;
                } else if (ImGui::GetIO().KeyShift && lastSelectedIndex_ != -1) {
                        // Shift + Click: Select range
                        i32 start = std::min(lastSelectedIndex_, idx);
                        i32 end   = std::max(lastSelectedIndex_, idx);
                        for (i32 i = start; i <= end; ++i) {
                                auto it = chs_.find(allKeys[i]);
                                if (it != chs_.end())
                                        it->second->selected_ = true;
                        }
                } else {
                        // Normal Click: Deselect ALL and select this one
                        for (auto &pair : chs_)
                                pair.second->selected_ = false;
                        ch->selected_ = true;
                }
                lastSelectedIndex_ = idx;
        }

        if (ImGui::BeginPopupContextItem()) {
                // Right-click implies selection — auto-select this row so Delete works
                // without requiring a prior left-click.
                if (!ch->selected_) {
                        for (auto &pair : chs_)
                                pair.second->selected_ = false;
                        ch->selected_ = true;
                }
                if (ImGui::MenuItem("Delete Selected")) {
                        for (auto &pair : chs_)
                                if (pair.second->selected_)
                                        pair.second->markPendingDelete();
                }
                ImGui::EndPopup();
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ChannelMovePayload payload;
                payload.srcScope = this;
                snprintf(payload.chName, sizeof(payload.chName), "%s", chName.c_str());
                ImGui::SetDragDropPayload("DND_CHANNEL_MOVE", &payload, sizeof(ChannelMovePayload));
                ImGui::Text("Move: %s", chName.c_str());
                ImGui::EndDragDropSource();
        }

        // 2. Value (Interactive)
        ImGui::TableNextColumn();
        if (ch->isEnum()) {
                const f32   dispVal     = ch->getDispVal();
                const char *currentName = ch->findEnumName(static_cast<i64>(dispVal));
                char        previewBuf[128];
                if (currentName)
                        snprintf(previewBuf, sizeof(previewBuf), "%s (%lld)", currentName, static_cast<i64>(dispVal));
                else
                        snprintf(previewBuf, sizeof(previewBuf), "Unknown (%f)", dispVal);

                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##enum", previewBuf)) {
                        for (const auto &e : ch->getEnums()) {
                                bool isSelected = (static_cast<i64>(dispVal) == e.value);
                                char label[128];
                                snprintf(label, sizeof(label), "%s (%lld)", e.name.c_str(), e.value);
                                if (ImGui::Selectable(label, isSelected)) {
                                        ch->setWVal(static_cast<f32>(e.value));
                                        ch->markWValDirty();
                                }
                        }
                        ImGui::EndCombo();
                }
        } else {
                const std::string &t = ch->getType();
                ImGui::SetNextItemWidth(-1);

                if (isIntegerType(t)) {
                        i32 v = static_cast<i32>(ch->getDispVal());
                        ImGui::InputInt("##val", &v, 0, 0);
                        if (ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->setWVal(static_cast<f32>(v));
                                ch->markWValDirty();
                        }
                } else {
                        f32 v = ch->getDispVal();
                        ImGui::InputFloat("##val", &v, 0, 0, "%.3f");
                        if (ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->setWVal(v);
                                ch->markWValDirty();
                        }
                }
        }

        // 3. Type
        ImGui::TableNextColumn();
        ImGui::Text("%s", ch->getType().c_str());

        // 4. Address
        ImGui::TableNextColumn();
        if (ch->isAddrUnknown())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "UNKNOWN");
        else
                ImGui::Text("0x%zX", ch->getAddr());

        // 5. Port
        ImGui::TableNextColumn();
        const std::string &dev = ch->getDevice();
        if (dev == "LOCAL")
                ImGui::Text("UDP");
        else if (dev == "JLINK")
                ImGui::Text("J-Link");
        else
                ImGui::Text("%s", dev.c_str());

        // 6. Wave Control
        ImGui::TableNextColumn();
        f32 availX  = ImGui::GetContentRegionAvail().x;
        f32 spacing = ImGui::GetStyle().ItemSpacing.x;
        f32 btnW    = (availX - spacing) * 0.65f;
        f32 cfgW    = (availX - spacing) * 0.35f;

        if (ch->waveEnable_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
                if (ImGui::Button("ON", ImVec2(btnW, 0)))
                        ch->waveEnable_ = false;
                ImGui::PopStyleColor();
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("OFF", ImVec2(btnW, 0)))
                        ch->waveEnable_ = true;
                ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (ImGui::Button("..", ImVec2(cfgW, 0)))
                ImGui::OpenPopup("WaveCfg");

        if (ImGui::BeginPopup("WaveCfg")) {
                // Read current values from atomic shadow fields
                auto &pending = ch->waveCfgPending_;
                ImGui::Text("Wave Generator: %s", chName.c_str());
                ImGui::Separator();

                const char *types[]     = {"Sine", "Square", "Triangle"};
                i32         currentType = pending.type.load();
                if (ImGui::Combo("##WaveType", &currentType, types, IM_ARRAYSIZE(types))) {
                        pending.type.store(currentType);
                        pending.dirty.store(true);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Type");

                ImGui::SetNextItemWidth(100);
                f32 f = pending.freq.load();
                if (ImGui::InputFloat("##WaveFreq", &f, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.freq.store(f);
                        pending.dirty.store(true);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Freq (Hz)");
                ImGui::SetNextItemWidth(100);
                f32 a = pending.amp.load();
                if (ImGui::InputFloat("##WaveAmp", &a, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.amp.store(a);
                        pending.dirty.store(true);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Amp");
                ImGui::SetNextItemWidth(100);
                f32 o = pending.offset.load();
                if (ImGui::InputFloat("##WaveOffset", &o, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.offset.store(o);
                        pending.dirty.store(true);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Offset");
                if (pending.type.load() != WAVE_TYPE_SINE) {
                        ImGui::SetNextItemWidth(100);
                        f32 d = pending.duty.load();
                        if (ImGui::InputFloat("##WaveDuty", &d, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                pending.duty.store(d);
                                pending.dirty.store(true);
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Duty");
                }

                ImGui::Separator();
                if (ImGui::Button("Close"))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }

        ImGui::PopID();
}

void
MonitorScope::plotMenu()
{
}

// Runs on the dedicated FFT worker thread. Copies the visible window for each
// shown channel (briefly holding monitorMtx), transforms off-lock, and publishes
// freqs/mags/peaks for the GUI to draw. Keeps fft_exec off the render thread.
void
MonitorScope::fftWorkerStep(std::mutex &monitorMtx)
{
        if (!showFft_) {
                // Drop any stale results so old spectra don't linger after switching to TIME.
                std::lock_guard lk(fftPubMtx_);
                if (!fftPublished_.empty())
                        fftPublished_.clear();
                return;
        }

        const f64 xmin = fftWinMin_.load(std::memory_order_relaxed);
        const f64 xmax = fftWinMax_.load(std::memory_order_relaxed);
        if (fftPoints_ <= 0)
                return;

        const u64 nowMs       = get_mono_ts_ms();
        const u64 minPeriodMs = (fftPoints_ >= 1048576)  ? 500
                                : (fftPoints_ >= 524288) ? 250
                                : (fftPoints_ >= 131072) ? 100
                                : (fftPoints_ >= 32768)  ? 33
                                                         : 16;
        const u64 lastRunMs   = fftLastRunMs_.load(std::memory_order_relaxed);
        if (lastRunMs != 0 && nowMs - lastRunMs < minPeriodMs)
                return;
        fftLastRunMs_.store(nowMs, std::memory_order_relaxed);

        // 1. Gather visible samples per shown channel (short lock on the monitor mtx).
        struct Job {
                std::string      name;
                std::vector<f32> samples; // already mean-detrended
                f32              fs;
        };
        std::vector<Job> jobs;
        {
                std::unique_lock lk(monitorMtx, std::try_to_lock);
                if (!lk.owns_lock())
                        return;
                const int fftN = fftPoints_;
                for (auto &[name, ch] : chs_) {
                        if (!ch || !ch->show_)
                                continue;
                        const auto &rd           = ch->read_;
                        const usize startIdx     = rd.rawLowerBound(xmin);
                        const usize endIdx       = rd.rawUpperBound(xmax);
                        const usize visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;
                        if (visibleCount == 0)
                                continue;

                        const usize copyCount  = std::min(visibleCount, static_cast<usize>(fftN));
                        const usize readOffset = (visibleCount > copyCount) ? (visibleCount - copyCount) : 0;

                        Job j;
                        j.name = name;
                        j.samples.resize(copyCount);
                        f64 sum = 0;
                        for (usize i = 0; i < copyCount; ++i) {
                                f32 v         = rd.rawVal(startIdx + readOffset + i);
                                j.samples[i]  = v;
                                sum          += v;
                        }
                        const f32 mean = static_cast<f32>(sum / static_cast<f64>(copyCount));
                        for (auto &v : j.samples)
                                v -= mean;

                        f32 fs = (parent_) ? (f32)parent_->getHz() : 1000.0f;
                        if (copyCount > 1) {
                                f64 totalTime =
                                    rd.rawTs(startIdx + readOffset + copyCount - 1) - rd.rawTs(startIdx + readOffset);
                                if (totalTime > 1e-9)
                                        fs = static_cast<f32>((f64)(copyCount - 1) / totalTime);
                        }
                        if (fs < 0.1f)
                                fs = 0.1f;
                        if (fs > 10000000.0f)
                                fs = 10000000.0f;
                        j.fs = fs;
                        jobs.push_back(std::move(j));
                }
        }

        if (jobs.empty()) {
                std::lock_guard lk(fftPubMtx_);
                fftPublished_.clear();
                return;
        }

        // 2. Transform off-lock. fft_ + its buffers are guarded by fftObjMtx_ so a
        //    concurrent reinitFft() can't pull the buffers out from under us.
        std::map<std::string, FftResult> out;
        {
                std::lock_guard lk(fftObjMtx_);
                const int       N    = fftPoints_; // re-read under the same lock as reinitFft
                const int       half = N / 2 + 1;
                if (N <= 0 || (int)fftMagF32_.size() < half || (int)fftLoBuf_.size() < N)
                        return;

                for (auto &j : jobs) {
                        std::fill(fftLoBuf_.begin(), fftLoBuf_.end(), 0.0f);
                        const usize cc = std::min(j.samples.size(), static_cast<usize>(N));
                        for (usize i = 0; i < cc; ++i)
                                fftLoBuf_[i] = j.samples[i];

                        fft_.cfg.fs       = j.fs;
                        fft_.lo.need_exec = 1;
                        fft_exec(&fft_);

                        const f32 df = j.fs / (f32)N;
                        FftResult r;
                        auto      magAt = [&](int i) { return (f64)fftMagF32_[i] * 2.0 / (f64)N; };

                        if (fftPeakCount_ > 0) {
                                for (int i = 2; i < half - 2; ++i)
                                        if (magAt(i) > magAt(i - 1) && magAt(i) > magAt(i + 1))
                                                r.peaks.push_back({(f64)i * (f64)df, magAt(i)});
                                std::sort(
                                    r.peaks.begin(), r.peaks.end(), [](const Peak &a, const Peak &b) { return a.mag > b.mag; });
                                if ((int)r.peaks.size() > fftPeakCount_)
                                        r.peaks.resize(fftPeakCount_);
                        }

                        constexpr int kMaxFftPlotBins = 8192;
                        if (half <= kMaxFftPlotBins) {
                                r.df = df;
                                r.freqs.resize(half);
                                r.mags.resize(half);
                                for (int i = 0; i < half; ++i) {
                                        r.freqs[i] = (f64)i * (f64)df;
                                        r.mags[i]  = magAt(i);
                                }
                        } else {
                                r.df = (f64)df * (f64)half / (f64)kMaxFftPlotBins;
                                r.freqs.reserve(kMaxFftPlotBins);
                                r.mags.reserve(kMaxFftPlotBins);
                                for (int b = 0; b < kMaxFftPlotBins; ++b) {
                                        const int b0 = (int)((i64)b * half / kMaxFftPlotBins);
                                        const int b1 = std::max(b0 + 1, (int)(((i64)b + 1) * half / kMaxFftPlotBins));
                                        int       mi = b0;
                                        f64       mm = magAt(b0);
                                        for (int i = b0 + 1; i < b1 && i < half; ++i) {
                                                const f64 m = magAt(i);
                                                if (m > mm) {
                                                        mm = m;
                                                        mi = i;
                                                }
                                        }
                                        r.freqs.push_back((f64)mi * (f64)df);
                                        r.mags.push_back(mm);
                                }
                        }
                        out[j.name] = std::move(r);
                }
        }

        // 3. Publish.
        {
                std::lock_guard lk(fftPubMtx_);
                fftPublished_ = std::move(out);
        }
}

void
MonitorScope::plotDraw(double *linkXMin, double *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode)
{
        bool isPaused = g_monitorPaused.load();
        if (showFft_ && mode != MonitorViewMode::MANUAL && !isPaused) {
                // We keep a single-shot auto-fit when NOT in manual mode?
                // Actually the user said "NEVER align automatically even for the first time".
                // But let's keep it linked to the mode. If they are in FOLLOW/FULL, it fits.
                ImPlot::SetNextAxesToFit();
        }

        const int layoutCols = showSidePanel_ ? 2 : 1;
        if (!ImGui::BeginTable("##plotLayoutTable", layoutCols, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                return;
        }
        ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthStretch, 0.75f);
        if (showSidePanel_)
                ImGui::TableSetupColumn(showFft_ ? "Peaks" : "Stats", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        if (ImPlot::BeginPlot("##plot", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                if (!showFft_ && linkXMin && linkXMax)
                        ImPlot::SetupAxisLinks(ImAxis_X1, linkXMin, linkXMax);

                // Mode-based Y Axis setup
                ImPlotAxisFlags yFlags = ImPlotAxisFlags_None;

                // Add Y-axis margin in FULL mode by pushing style padding
                bool pushedPadding = false;
                if (mode == MonitorViewMode::FULL && !isPaused) {
                        ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0, 0.1f)); // 10% vertical padding
                        pushedPadding = true;
                }

                if (showFft_) {
                        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_None);
                } else {
                        yFlags = (mode == MonitorViewMode::FULL && !isPaused) ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
                        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None, yFlags);
                }

                // Interaction Detection (Area or Axes)
                bool plotHovered  = ImPlot::IsPlotHovered();
                bool xAxisHovered = ImPlot::IsAxisHovered(ImAxis_X1);
                bool yAxisHovered = ImPlot::IsAxisHovered(ImAxis_Y1);
                bool anyHovered   = plotHovered || xAxisHovered || yAxisHovered;

                // Capture if the plot is being interacted with (dragged/panned/zoomed)
                bool plotActive = ImGui::IsItemActive();

                bool isDoubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (anyHovered) {
                        if (isDoubleClick) {
                                if (mode == MonitorViewMode::MANUAL)
                                        mode = MonitorViewMode::FOLLOW;
                                else if (mode == MonitorViewMode::FOLLOW)
                                        mode = MonitorViewMode::FULL;
                        } else if (ImGui::GetIO().MouseWheel != 0) {
                                if (mode == MonitorViewMode::FULL) {
                                        mode = MonitorViewMode::FOLLOW;
                                }
                        }
                }

                if (!isDoubleClick && plotActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                        if (mode == MonitorViewMode::FULL || mode == MonitorViewMode::FOLLOW) {
                                mode = MonitorViewMode::MANUAL;
                        }
                }

                channelPeaks_.clear();
                channelStats_.clear();

                // Collect (channel, item) pairs here, inside the BeginPlot context
                // where ImGui ID stack is correct. Used after EndPlot to sync
                // legend-click state back to ch->show_ via raw pointer (no ID lookup).
                std::vector<std::pair<MonitorChannel *, ImPlotItem *>> syncItems;
                syncItems.reserve(chs_.size());

                // Iterate channels in insertion order so the legend/plot follow the
                // order channels were added (e.g. CSV column order), not hash order.
                std::vector<ChannelMapType::value_type *> orderedChs;
                orderedChs.reserve(chs_.size());
                for (auto &pair : chs_)
                        orderedChs.push_back(&pair);
                std::sort(orderedChs.begin(), orderedChs.end(), [](const auto *a, const auto *b) {
                        return a->second->getOrder() < b->second->getOrder();
                });

                for (auto *pairPtr : orderedChs) {
                        auto &chName = pairPtr->first;
                        auto &ch     = pairPtr->second;
                        // 1. Handle Visibility
                        // For existing items: directly set Show so hide/show line
                        // takes effect immediately (bypasses ImPlotCond limitations).
                        // For brand-new items: HideNextItem(Once) sets the initial state.
                        if (auto *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(chName.c_str())) {
                                        item->Show = ch->show_;
                                        syncItems.emplace_back(ch.get(), item);
                                }
                        }
                        ImPlot::HideNextItem(!ch->show_, ImPlotCond_Once);

                        // 2. Handle Style & Color
                        const f32 *col = ch->getColor();
                        ImPlot::SetNextLineStyle(ImVec4(col[0], col[1], col[2], col[3]), ch->getLineWeight());
                        if (ch->showMarkers())
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);

                        // 3. Prepare and Plot Data
                        bool        plotted = false;
                        const auto &rd      = ch->read_;
                        if (showFft_) {
                                // Publish the current time window for the FFT worker thread, then draw
                                // the latest transform it produced — no fft_exec on the render thread.
                                const f64 xmin = (linkXMin) ? *linkXMin : 0.0;
                                const f64 xmax = (linkXMax) ? *linkXMax : 1.0;
                                fftWinMin_.store(xmin, std::memory_order_relaxed);
                                fftWinMax_.store(xmax, std::memory_order_relaxed);

                                if (ch->show_) {
                                        std::lock_guard lk(fftPubMtx_);
                                        auto            it = fftPublished_.find(chName);
                                        if (it != fftPublished_.end() && !it->second.freqs.empty()) {
                                                const FftResult &r = it->second;
                                                if (fftBars_) {
                                                        ImPlot::SetNextFillStyle(ImVec4(col[0], col[1], col[2], col[3]));
                                                        ImPlot::PlotBars(chName.c_str(),
                                                                         r.freqs.data(),
                                                                         r.mags.data(),
                                                                         (int)r.freqs.size(),
                                                                         r.df * 0.9);
                                                } else {
                                                        ImPlot::PlotLine(
                                                            chName.c_str(), r.freqs.data(), r.mags.data(), (int)r.freqs.size());
                                                }
                                                plotted = true;
                                                if (fftPeakCount_ > 0 && !r.peaks.empty())
                                                        channelPeaks_[chName] = r.peaks;
                                        }
                                }
                        } else {
                                // Time-domain: pick an LOD level whose visible bucket count fits the
                                // display budget. -1 = use raw level directly.
                                dxs_.clear();
                                dys_.clear();

                                const f64 xmin   = (linkXMin) ? *linkXMin : 0.0;
                                const f64 xmax   = (linkXMax) ? *linkXMax : 1.0;
                                const u32 budget = (maxDisplayPoints > 0) ? maxDisplayPoints : 5000;
                                const int level  = rd.pickLevel(xmin, xmax, budget);

                                if (level < 0) {
                                        // Raw level fits — iterate the raw ring directly.
                                        const usize si = rd.rawLowerBound(xmin);
                                        const usize ei = rd.rawUpperBound(xmax);
                                        if (ei > si) {
                                                const usize n = ei - si;
                                                dxs_.reserve(n);
                                                dys_.reserve(n);
                                                for (usize i = si; i < ei; ++i) {
                                                        dxs_.push_back(rd.rawTs(i));
                                                        dys_.push_back(static_cast<f64>(rd.rawVal(i)));
                                                }
                                        }
                                } else {
                                        // Walk LOD level: each bucket emits (t, vmin) then (t, vmax).
                                        const usize si = rd.lodLowerBound(level, xmin);
                                        const usize ei = rd.lodUpperBound(level, xmax);
                                        if (ei > si) {
                                                const usize n = ei - si;
                                                dxs_.reserve(n * 2);
                                                dys_.reserve(n * 2);
                                                for (usize i = si; i < ei; ++i) {
                                                        const LodSample &s = rd.lodAt(level, i);
                                                        dxs_.push_back(s.t);
                                                        dys_.push_back(static_cast<f64>(s.vmin));
                                                        dxs_.push_back(s.t);
                                                        dys_.push_back(static_cast<f64>(s.vmax));
                                                }
                                        }
                                }

                                if (!dxs_.empty()) {
                                        if (ch->getPlotStyle() == 1 && level < 0)
                                                ImPlot::PlotStairs(
                                                    chName.c_str(), dxs_.data(), dys_.data(), static_cast<i32>(dxs_.size()));
                                        else
                                                ImPlot::PlotLine(
                                                    chName.c_str(), dxs_.data(), dys_.data(), static_cast<i32>(dxs_.size()));
                                        plotted = true;
                                }

                                // Window stats: min/max are exact; mean/RMS are exact at raw level and
                                // approximated from LOD bucket midpoints when downsampled.
                                if (ch->show_) {
                                        Stats s;
                                        s.min     = std::numeric_limits<f64>::infinity();
                                        s.max     = -std::numeric_limits<f64>::infinity();
                                        f64 sum   = 0.0;
                                        f64 sumSq = 0.0;
                                        if (level < 0) {
                                                const usize si = rd.rawLowerBound(xmin);
                                                const usize ei = rd.rawUpperBound(xmax);
                                                for (usize i = si; i < ei; ++i) {
                                                        const f64 v = static_cast<f64>(rd.rawVal(i));
                                                        if (v < s.min)
                                                                s.min = v;
                                                        if (v > s.max)
                                                                s.max = v;
                                                        sum   += v;
                                                        sumSq += v * v;
                                                        ++s.count;
                                                }
                                        } else {
                                                const usize si = rd.lodLowerBound(level, xmin);
                                                const usize ei = rd.lodUpperBound(level, xmax);
                                                for (usize i = si; i < ei; ++i) {
                                                        const LodSample &ls = rd.lodAt(level, i);
                                                        if (ls.vmin < s.min)
                                                                s.min = ls.vmin;
                                                        if (ls.vmax > s.max)
                                                                s.max = ls.vmax;
                                                        const f64 mid =
                                                            (static_cast<f64>(ls.vmin) + static_cast<f64>(ls.vmax)) * 0.5;
                                                        sum   += mid;
                                                        sumSq += mid * mid;
                                                        ++s.count;
                                                }
                                        }
                                        if (s.count > 0) {
                                                s.pkpk                = s.max - s.min;
                                                s.mean                = sum / static_cast<f64>(s.count);
                                                s.rms                 = std::sqrt(sumSq / static_cast<f64>(s.count));
                                                channelStats_[chName] = s;
                                        }
                                }
                        }

                        if (!plotted) {
                                ImPlot::PlotLine(chName.c_str(), (const f64 *)nullptr, (const f64 *)nullptr, 0);
                        }

                        // 4. Update Legend Toggle
                        if (ImPlotContext *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(chName.c_str())) {
                                        ch->show_ = item->Show;
                                }
                        }

                        // 5. Popup Configuration
                        if (ImPlot::BeginLegendPopup(chName.c_str())) {
                                ImGui::Text("Channel: %s", chName.c_str());
                                if (ch->isEnum()) {
                                        const char *currentName = ch->findEnumName(static_cast<i64>(ch->getRVal()));
                                        ImGui::Text("Value: %s (%lld)",
                                                    currentName ? currentName : "Unknown",
                                                    static_cast<i64>(ch->getRVal()));
                                } else {
                                        ImGui::Text("Value: %f", ch->getRVal());
                                }
                                ImGui::Separator();

                                ImVec4 curCol = ImVec4(col[0], col[1], col[2], col[3]);
                                if (ch->useAutoColor()) {
                                        if (ImPlotContext *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(chName.c_str())) {
                                                        curCol = ImGui::ColorConvertU32ToFloat4(item->Color);
                                                }
                                        }
                                }
                                f32 colArr[4] = {curCol.x, curCol.y, curCol.z, curCol.w};
                                if (ImGui::ColorEdit4("Color", colArr, ImGuiColorEditFlags_NoInputs)) {
                                        ch->useAutoColor() = false;
                                        memcpy(ch->getColor(), colArr, sizeof(colArr));
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox("Auto", &ch->useAutoColor())) {
                                        if (ch->useAutoColor()) {
                                                static i32 shuffleIdx = 0;
                                                shuffleIdx            = (shuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 newCol         = ImPlot::GetColormapColor(shuffleIdx);
                                                memcpy(ch->getColor(), &newCol.x, sizeof(f32) * 4);
                                        }
                                }

                                ImGui::SetNextItemWidth(100);
                                const char *styleNames[] = {"Line", "Stairs"};
                                i32         currentStyle = ch->getPlotStyle();
                                if (ImGui::Combo("##PlotStyle", &currentStyle, styleNames, 2)) {
                                        ch->getPlotStyle() = currentStyle;
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("Style");
                                ImGui::Checkbox("Markers", &ch->showMarkers());
                                if (ImGui::Button("Delete Channel"))
                                        ch->markPendingDelete();
                                ImPlot::EndLegendPopup();
                        }
                }
                ImPlot::EndPlot();

                // Sync legend-click state back to ch->show_.
                // EndPlot() is where ImPlot processes legend clicks (toggles item->Show).
                // We use the raw pointers saved before EndPlot to avoid re-doing
                // the ID lookup (which would use the wrong ImGui context after EndPlot).
                for (auto &[ch, item] : syncItems)
                        ch->show_ = item->Show;

                if (pushedPadding) {
                        ImPlot::PopStyleVar();
                }
        }

        if (showSidePanel_) {
                ImGui::TableSetColumnIndex(1);
                if (showFft_) {
                        if (!channelPeaks_.empty()) {
                                for (auto &[chName, peaks] : channelPeaks_) {
                                        if (peaks.empty())
                                                continue;
                                        ImGui::Text("Peaks: %s", chName.c_str());
                                        if (ImGui::BeginTable(("##peaksTable_" + chName).c_str(),
                                                              2,
                                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                                                ImGui::TableSetupColumn("Freq (Hz)");
                                                ImGui::TableSetupColumn("Mag");
                                                ImGui::TableHeadersRow();

                                                for (const auto &p : peaks) {
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::Text("%.1f", p.freq);
                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::Text("%.3f", p.mag);
                                                }
                                                ImGui::EndTable();
                                        }
                                        ImGui::Spacing();
                                }
                        } else {
                                ImGui::Text("No peaks detected");
                        }
                } else {
                        if (!channelStats_.empty()) {
                                for (auto &[chName, s] : channelStats_) {
                                        ImGui::Text("%s", chName.c_str());
                                        if (ImGui::BeginTable(("##statsTable_" + chName).c_str(),
                                                              2,
                                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                                                ImGui::TableSetupColumn("Metric");
                                                ImGui::TableSetupColumn("Value");
                                                ImGui::TableHeadersRow();
                                                auto row = [](const char *k, const char *fmt, f64 v) {
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::TextUnformatted(k);
                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::Text(fmt, v);
                                                };
                                                row("Min", "%.4f", s.min);
                                                row("Max", "%.4f", s.max);
                                                row("Pk-Pk", "%.4f", s.pkpk);
                                                row("Mean", "%.4f", s.mean);
                                                row("RMS", "%.4f", s.rms);
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::TextUnformatted("N");
                                                ImGui::TableSetColumnIndex(1);
                                                ImGui::Text("%zu", s.count);
                                                ImGui::EndTable();
                                        }
                                        ImGui::Spacing();
                                }
                        } else {
                                ImGui::Text("No data in window");
                        }
                }
        } // showSidePanel_
        ImGui::EndTable();
}

int
MonitorScope::addChannel(const std::string &chName)
{
        if (chs_.find(chName) != chs_.end())
                return -1;
        auto ch = std::make_shared<MonitorChannel>(chName);

        // Assign an initial color from the colormap
        static i32 globalColorIdx = 0;
        ImVec4     c              = ImPlot::GetColormapColor(globalColorIdx);
        globalColorIdx            = (globalColorIdx + 1) % ImPlot::GetColormapSize();
        memcpy(ch->getColor(), &c.x, sizeof(f32) * 4);
        ch->minKeepPoints_ = fftPoints_;
        ch->setOrder(nextChannelOrder_++); // preserve insertion order for display

        chs_[chName] = std::move(ch);
        return 0;
}

int
MonitorScope::setValue(const std::string &chName, const f32 val)
{
        auto it = chs_.find(chName);
        if (it == chs_.end())
                return -1;
        it->second->setRVal(val, sessionTimeSec());
        return 0;
}

MonitorChannel *
MonitorScope::findChannel(const std::string &chName)
{
        auto it = chs_.find(chName);
        if (it == chs_.end())
                return nullptr;
        return it->second.get();
}

void
MonitorScope::shmInit(MonitorChannel &ch)
{
        if (ch.getDevice() != "SHM")
                return;
        // Use the dedicated SHM region name if set; fall back to channel name.
        // Use SHM_READWRITE because spsc_read_buf updates spsc->rp (read pointer).
        // Use 4096 as the minimum region capacity to accommodate the spsc header.
        const std::string &regionName = ch.getShmRegionName().empty() ? ch.getName() : ch.getShmRegionName();
        shm_cfg_t          cfg        = {regionName.c_str(), SHM_READWRITE, 4096};
        shm_init(&ch.getShm(), cfg);
}

void
MonitorScope::dropTarget()
{
        if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CHANNEL")) {
                        if (payload->DataSize == sizeof(ChannelDropPayload)) {
                                auto *chPayload = static_cast<ChannelDropPayload *>(payload->Data);
                                if (addChannel(chPayload->name) == 0) {
                                        MonitorChannel *ch = findChannel(chPayload->name);
                                        if (ch) {
                                                ch->setType(chPayload->type);
                                                ch->setAddr(chPayload->addr);
                                                ch->getSymbolName() = chPayload->name;
                                                ch->setDevice(chPayload->device);
                                                if (chPayload->shmName[0] != '\0')
                                                        ch->setShmRegionName(chPayload->shmName);
                                                ch->setBitOffset(chPayload->bitOffset);
                                                ch->setBitSize(chPayload->bitSize);
                                                if (chPayload->numEnums > 0) {
                                                        std::vector<MonitorChannel::EnumEntry> ents;
                                                        for (int i = 0; i < chPayload->numEnums; ++i)
                                                                ents.push_back(
                                                                    {chPayload->enums[i].name, chPayload->enums[i].value});
                                                        ch->setEnums(std::move(ents));
                                                }
                                                if (ch->getDevice() == "SHM")
                                                        shmInit(*ch);
                                                if (parent_)
                                                        parent_->setModified();
                                        }
                                }
                        }
                }

                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                        if (payload->DataSize == sizeof(StructChannelPayload)) {
                                auto *sp = static_cast<const StructChannelPayload *>(payload->Data);
                                for (int ei = 0; ei < sp->count; ++ei) {
                                        const auto &e = sp->entries[ei];
                                        if (addChannel(e.name) == 0) {
                                                MonitorChannel *ch = findChannel(e.name);
                                                if (ch) {
                                                        ch->setType(e.type);
                                                        ch->setAddr(e.addr);
                                                        ch->getSymbolName() = e.name;
                                                        ch->setDevice(sp->device);
                                                        if (sp->shmName[0] != '\0')
                                                                ch->setShmRegionName(sp->shmName);
                                                        ch->setBitOffset(e.bitOffset);
                                                        ch->setBitSize(e.bitSize);
                                                        if (e.numEnums > 0) {
                                                                std::vector<MonitorChannel::EnumEntry> ents;
                                                                for (int k = 0; k < e.numEnums; ++k)
                                                                        ents.push_back({e.enums[k].name, e.enums[k].value});
                                                                ch->setEnums(std::move(ents));
                                                        }
                                                        if (ch->getDevice() == "SHM")
                                                                shmInit(*ch);
                                                        if (parent_)
                                                                parent_->setModified();
                                                }
                                        }
                                }
                        }
                }

                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                        auto *data = static_cast<ChannelMovePayload *>(payload->Data);
                        if (data->srcScope != this) {
                                auto it = data->srcScope->getChannels().find(data->chName);
                                if (it != data->srcScope->getChannels().end()) {
                                        // Transfer ownership
                                        this->getChannels()[data->chName] = std::move(it->second);
                                        data->srcScope->getChannels().erase(it);
                                }
                        }
                }
                ImGui::EndDragDropTarget();
        }
}

void
MonitorScope::reinitFft(i32 newPoints)
{
        // Guard against the FFT worker thread using fft_ / its buffers mid-transform.
        std::lock_guard lk(fftObjMtx_);

        fftPoints_ = newPoints;
        fft_destroy(&fft_);

        fftInBuf_.assign(fftPoints_, 0.0f);
        fftMagF32_.assign(fftPoints_ / 2 + 1, 0.0f);
        fftMagBuf_.assign(fftPoints_ / 2 + 1, 0.0);
        fftOutBuf_.assign((fftPoints_ / 2 + 1) * 2, 0.0f);
        fftLoBuf_.assign(fftPoints_, 0.0f);

        fft_cfg_t cfg;
        cfg.npoints  = fftPoints_;
        cfg.fs       = (parent_) ? (f32)parent_->getHz() : 1000.0f;
        cfg.e_window = FFT_WINDOW_HANNING;
        cfg.in_buf   = fftInBuf_.data();
        cfg.mag_buf  = fftMagF32_.data();
        cfg.out_buf  = (decltype(cfg.out_buf))fftOutBuf_.data();
        cfg.buf      = fftLoBuf_.data();
        fft_init(&fft_, cfg);

        for (auto &pair : chs_) {
                pair.second->minKeepPoints_ = static_cast<usize>(newPoints);
        }
}

/* -------------------------------------------------------------------------- */
/*                                   Monitor                                  */
/* -------------------------------------------------------------------------- */

void
Monitor::menu()
{
}

int
Monitor::addScope(const std::string &scopeName)
{
        if (scopes_.find(scopeName) != scopes_.end())
                return -1;
        auto scope         = std::make_shared<MonitorScope>(scopeName);
        scopes_[scopeName] = scope;
        needsLayout_       = true;
        for (auto &pair : scopes_)
                pair.second->setManual(false); // Reset layout mode
        return 0;
}

MonitorChannel *
Monitor::findChannel(const std::string &scopeName, const std::string &chName)
{
        auto it = scopes_.find(scopeName);
        if (it == scopes_.end())
                return nullptr;
        return it->second->findChannel(chName);
}

/* -------------------------------------------------------------------------- */

void
Monitor::updateDisplay()
{
        bool open = true;
        // "VisibleTitle###name_": the visible label tracks the user-editable title
        // while the trailing id keeps the ImGui window id (and dock layout) stable.
        const std::string winLabel = getTitle() + "###" + name_;
        if (ImGui::Begin(winLabel.c_str(), &open)) {
                if (!open)
                        markPendingDelete();

                // Double-click the title bar (or dock tab) to rename this dock.
                {
                        ImGuiWindow *win = ImGui::GetCurrentWindow();
                        ImRect       tr  = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                        if (ImGui::IsMouseHoveringRect(tr.Min, tr.Max, false) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                snprintf(renameBuf_, sizeof(renameBuf_), "%s", getTitle().c_str());
                                ImGui::OpenPopup("Rename Dock");
                        }
                        if (ImGui::BeginPopup("Rename Dock")) {
                                ImGui::TextDisabled("重命名");
                                ImGui::SetNextItemWidth(220);
                                if (ImGui::IsWindowAppearing())
                                        ImGui::SetKeyboardFocusHere();
                                bool commit =
                                    ImGui::InputText("##renameDock",
                                                     renameBuf_,
                                                     sizeof(renameBuf_),
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                                ImGui::SameLine();
                                if (ImGui::Button("OK"))
                                        commit = true;
                                if (commit) {
                                        if (renameBuf_[0] != '\0')
                                                setTitle(renameBuf_);
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                }

                if (csvLoading_.load(std::memory_order_acquire)) {
                        const char *frames[] = {"|", "/", "-", "\\"};
                        int         fi       = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s  正在解析 CSV...", frames[fi]);
                        ImGui::End();
                        return;
                }

                if (ImGui::Button("Add Scope")) {
                        char nameBuf[32];
                        snprintf(nameBuf, sizeof(nameBuf), "scope_%zu", scopes_.size());
                        addScope(nameBuf);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.5f, 0.0f, 0.6f)); // Orange
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.6f, 0.1f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
                if (ImGui::Button("Clear Data")) {
                        requestClearData();
                        JLinkPort::instance().reqRestart();
                        LOG_I("Monitor[%s] clear data requested from UI", name_.c_str());
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f)); // Green
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));

                // Cheap per-frame estimate of the resulting CSV size, shown as a tooltip so the
                // user knows what they're about to write before picking a path.
                u64    estMaxRows = 0; // longest channel → number of data rows
                u64    estCells   = 0; // total populated "ts,val" pairs across all channels
                size_t estChans   = 0;
                for (auto &[scopeName, scope] : scopes_) {
                        for (auto &[chName, ch] : scope->getChannels()) {
                                const u64 n = static_cast<u64>(ch->read_.rawSize());
                                if (n > estMaxRows)
                                        estMaxRows = n;
                                estCells += n;
                                ++estChans;
                        }
                }
                // ~9 chars per "%.6f" number, two numbers per populated cell, plus the comma
                // separators (2 per channel per row) and one newline per row. Header negligible.
                const u64 estBytes = estCells * 2ull * 9ull + estMaxRows * static_cast<u64>(estChans) * 2ull + estMaxRows;

                const bool exporting = csvExport_->running.load(std::memory_order_acquire);
                ImGui::BeginDisabled(exporting);
                const bool exportClicked = ImGui::Button("Export CSV");
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered() && !exporting) {
                        if (estChans == 0)
                                ImGui::SetTooltip("无数据可导出");
                        else
                                ImGui::SetTooltip("预计导出大小: ~%s\n通道数: %zu   最大行数: %llu",
                                                  humanSize(estBytes).c_str(),
                                                  estChans,
                                                  static_cast<unsigned long long>(estMaxRows));
                }
                if (exportClicked) {
                        char dfltBuf[128];
                        auto now = std::time(nullptr);
                        std::strftime(dfltBuf, sizeof(dfltBuf), "%Y%m%d_%H%M%S_", std::localtime(&now));
                        std::string defaultName = std::string(dfltBuf) + getTitle() + ".csv";

                        std::string fullpath = nativeDlgSave("Export CSV", {{"CSV Files", {"csv"}}}, defaultName);
                        if (fullpath.empty()) {
                                LOG_I("Monitor[%s] export cancelled", name_.c_str());
                        } else {
                                if (fullpath.find(".csv") == std::string::npos)
                                        fullpath += ".csv";

                                // Snapshot the data now, while we hold mtxMonitors_, into a
                                // self-contained buffer. The background writer then never touches
                                // shared channel state, so sampling/UI stay unblocked.
                                //
                                // Each column pair is tagged "<scope>::<channel>::Time" /
                                // "<scope>::<channel>::Value" so the importer can restore the
                                // originating scope. "::" is used (instead of "_") because both
                                // scope and channel names may legitimately contain underscores.
                                auto   snapshot = std::make_shared<std::vector<CsvExportChannel>>();
                                size_t maxLen   = 0;
                                for (auto &[scopeName, scope] : scopes_) {
                                        for (auto &[chName, ch] : scope->getChannels()) {
                                                CsvExportChannel c;
                                                c.tag         = scopeName + "::" + chName;
                                                const usize n = ch->read_.rawSize();
                                                c.ts.reserve(n);
                                                c.val.reserve(n);
                                                for (usize i = 0; i < n; ++i) {
                                                        c.ts.push_back(ch->read_.rawTs(i));
                                                        c.val.push_back(ch->read_.rawVal(i));
                                                }
                                                if (n > maxLen)
                                                        maxLen = n;
                                                snapshot->push_back(std::move(c));
                                        }
                                }

                                auto state = csvExport_; // shared_ptr keeps progress state alive past Monitor
                                state->rows.store(0, std::memory_order_release);
                                state->total.store(static_cast<u64>(maxLen), std::memory_order_release);
                                state->running.store(true, std::memory_order_release);

                                std::string monName = name_;
                                std::thread([snapshot, state, fullpath, maxLen, monName]() {
                                        FILE *f = fopen(fullpath.c_str(), "w");
                                        if (!f) {
                                                LOG_E("Monitor[%s] failed to open %s for export",
                                                      monName.c_str(),
                                                      fullpath.c_str());
                                                state->running.store(false, std::memory_order_release);
                                                return;
                                        }
                                        const auto &channels = *snapshot;

                                        // Header
                                        for (size_t i = 0; i < channels.size(); ++i) {
                                                fprintf(f,
                                                        "%s::Time,%s::Value%s",
                                                        channels[i].tag.c_str(),
                                                        channels[i].tag.c_str(),
                                                        (i + 1 == channels.size()) ? "" : ",");
                                        }
                                        fprintf(f, "\n");

                                        // Data
                                        for (size_t row = 0; row < maxLen; ++row) {
                                                for (size_t i = 0; i < channels.size(); ++i) {
                                                        if (row < channels[i].ts.size()) {
                                                                fprintf(
                                                                    f, "%.6f,%.6f", channels[i].ts[row], channels[i].val[row]);
                                                        } else {
                                                                fprintf(f, ",");
                                                        }
                                                        if (i + 1 < channels.size())
                                                                fprintf(f, ",");
                                                }
                                                fprintf(f, "\n");
                                                if ((row & 0x3FF) == 0)
                                                        state->rows.store(static_cast<u64>(row), std::memory_order_release);
                                        }
                                        fclose(f);
                                        state->rows.store(static_cast<u64>(maxLen), std::memory_order_release);
                                        state->running.store(false, std::memory_order_release);
                                        LOG_I(
                                            "Monitor[%s] exported to %s (%zu rows)", monName.c_str(), fullpath.c_str(), maxLen);
                                }).detach();
                        }
                }
                ImGui::PopStyleColor(3);

                if (exporting) {
                        ImGui::SameLine();
                        const u64   done     = csvExport_->rows.load(std::memory_order_acquire);
                        const u64   total    = csvExport_->total.load(std::memory_order_acquire);
                        const char *frames[] = {"|", "/", "-", "\\"};
                        const int   fi       = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                        const float pct      = total ? (100.0f * static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s  正在后台导出 CSV... %.0f%%", frames[fi], pct);
                }

                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                f32 h = historySeconds_;
                if (ImGui::InputFloat("##History", &h, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        if (h < 0.0f)
                                h = 0.0f;
                        historySeconds_ = h;
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("History(s)");
                // Sync to all channels every frame to ensure newly added channels inherit the setting immediately
                for (auto &[_, scope] : scopes_)
                        for (auto &[__, ch] : scope->getChannels())
                                ch->historySeconds_ = historySeconds_;

                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                i32 maxPts = static_cast<i32>(maxDisplayPoints_);
                if (ImGui::InputInt("##MaxPts", &maxPts, 0, 0) && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        if (maxPts < 100)
                                maxPts = 100;
                        maxDisplayPoints_ = static_cast<u32>(maxPts);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Max Pts");

                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("##MaxHz", &maxSampleHz_, 0, 0)) {
                        if (maxSampleHz_ < 1)
                                maxSampleHz_ = 1;
                        if (maxSampleHz_ > 50000)
                                maxSampleHz_ = 50000;
                        JLinkPort::instance().reqRestart();
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Target sample rate (Hz). HSS mode on J-Link Pro/Ultra+ can reach several "
                                          "kHz to tens of kHz; POLL mode is limited by USB latency (~1-2kHz).");

                ImGui::SameLine();
                if (actualHz_ > 0.1f) {
                        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%.0f Hz", actualHz_);
                } else {
                        ImGui::TextDisabled("-- Hz");
                }

                // Right-aligned mode control group
                f32 spacing    = ImGui::GetStyle().ItemSpacing.x;
                f32 totalWidth = 70 + spacing + 90;

                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - totalWidth);

                const char *sampModes[] = {"HSS", "POLL"};
                i32         curSamp     = (samplingMode_ == SamplingMode::HSS) ? 0 : 1;
                ImGui::SetNextItemWidth(70);
                if (ImGui::Combo("##SampMode", &curSamp, sampModes, 2)) {
                        samplingMode_ = (curSamp == 0) ? SamplingMode::HSS : SamplingMode::POLL;
                        requestClearData();
                        JLinkPort::instance().reqRestart();
                        LOG_I("Monitor[%s] sampling mode changed to %s; clear requested",
                              name_.c_str(),
                              samplingMode_ == SamplingMode::HSS ? "HSS" : "POLL");
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Sampling Mode");
                ImGui::SameLine();

                // Single MODE combo — targets FFT axis when any scope is in FFT view,
                // otherwise targets the time-domain axis.
                bool anyFft = false;
                for (const auto &sp : scopes_)
                        if (sp.second && sp.second->isFftEnabled()) {
                                anyFft = true;
                                break;
                        }

                const char *viewModeNames[] = {"FULL", "FOLLOW", "MANUAL"};
                i32         curMode         = anyFft ? (i32)fftViewMode_ : (i32)viewMode_;

                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("##Mode", &curMode, viewModeNames, 3)) {
                        if (anyFft)
                                fftViewMode_ = static_cast<MonitorViewMode>(curMode);
                        else
                                viewMode_ = static_cast<MonitorViewMode>(curMode);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(anyFft ? "Frequency Domain View Mode" : "Time Domain View Mode");

                ImGui::Separator();

                std::vector<std::string> keys;
                for (auto &pair : scopes_)
                        keys.push_back(pair.first);
                std::sort(keys.begin(), keys.end());

                ImVec2 avail = ImGui::GetContentRegionAvail();

                // Adaptive Layout Engine
                bool anyManual = false;
                for (auto &key : keys) {
                        if (scopes_[key]->isManual()) {
                                anyManual = true;
                                break;
                        }
                }

                if (keys.size() == 1) {
                        // Force fill if only one scope
                        scopes_[keys[0]]->getHeight() = avail.y;
                } else if (needsLayout_ || (!anyManual && std::abs(avail.y - lastAvailY_) > 1.0f)) {
                        // Distribute equally if no manual overrides OR window resized while in auto mode
                        f32 totalSplitterHeight = static_cast<f32>(keys.size() - 1) * 8.0f;
                        f32 equalHeight         = (avail.y - totalSplitterHeight) / static_cast<f32>(keys.size());
                        if (equalHeight < 40.0f)
                                equalHeight = 40.0f;
                        for (auto &key : keys) {
                                scopes_[key]->getHeight() = equalHeight;
                        }
                        needsLayout_ = false;
                }
                lastAvailY_ = avail.y;

                // Compute data bounds ONCE per frame (was O(scopes² × channels) — hoisted out of the per-scope loop).
                const bool isPaused = g_monitorPaused.load();
                const f64  now      = sessionTimeSec();
                f64        earliest = now;
                f64        latest   = 0.0;
                bool       hasData  = false;
                for (const auto &[_, sc] : scopes_) {
                        for (const auto &[__, ch] : sc->getChannels()) {
                                const f64 e = ch->earliestTs();
                                const f64 l = ch->latestTs();
                                if (e >= 0.0f) {
                                        hasData = true;
                                        if (e < earliest)
                                                earliest = e;
                                        if (l >= 0.0f && l > latest)
                                                latest = l;
                                }
                        }
                }

                // Drive linkXMin_ / linkXMax_ from the view mode (also once per frame).
                if (viewMode_ == MonitorViewMode::FULL) {
                        if (!isPaused) {
                                if (hasData) {
                                        if (latest < dataStartTime_)
                                                latest = dataStartTime_;
                                        linkXMin_ = earliest;
                                        linkXMax_ = latest;
                                        if (linkXMax_ - linkXMin_ < 1.0)
                                                linkXMax_ = linkXMin_ + 1.0;
                                } else {
                                        linkXMin_ = dataStartTime_;
                                        linkXMax_ = dataStartTime_ + 1.0;
                                }
                                lastNow_ = now;
                        }
                } else if (!isPaused && (JLinkPort::instance().isHssRunning() || samplingMode_ == SamplingMode::POLL)) {
                        // FOLLOW mode: don't auto-update while the user is interacting with the plot.
                        if (viewMode_ == MonitorViewMode::FOLLOW && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                            ImGui::GetIO().MouseWheel == 0) {
                                f64 span        = linkXMax_ - linkXMin_;
                                f64 currentSpan = (span > 0.001) ? span : 1.0;
                                if (historySeconds_ > 0.0f && currentSpan > historySeconds_) {
                                        currentSpan = historySeconds_;
                                }
                                f64 refTime = hasData ? latest : now;
                                linkXMax_   = refTime;
                                linkXMin_   = refTime - currentSpan;
                                if (linkXMin_ < 0.0)
                                        linkXMin_ = 0.0;
                                lastNow_ = refTime;
                        }
                }
                wasPaused_ = isPaused;

                for (size_t i = 0; i < keys.size(); ++i) {
                        auto &scope = scopes_[keys[i]];

                        float childHeight =
                            scope->isHidden() ? (ImGui::GetFrameHeightWithSpacing() + 15.0f) : scope->getHeight();
                        if (ImGui::BeginChild(keys[i].c_str(), ImVec2(avail.x, childHeight), true)) {
                                scope->menu();

                                if (!scope->isHidden()) {
                                        if (scope->isFftEnabled()) {
                                                scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, fftViewMode_);
                                        } else {
                                                scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, viewMode_);
                                        }
                                }
                                scope->dropTarget();
                        }
                        ImGui::EndChild();

                        ImGui::PushID(static_cast<i32>(i));
                        ImGui::InvisibleButton("##splitter", ImVec2(-1, 8.0f));
                        if (ImGui::IsItemActive()) {
                                scope->getHeight() += ImGui::GetIO().MouseDelta.y;
                                if (scope->getHeight() < 40.0f)
                                        scope->getHeight() = 40.0f;
                                scope->setManual(true); // User touched the splitter, mark as manual
                        }
                        if (ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                        needsLayout_ = true;
                                        for (auto &pair : scopes_)
                                                pair.second->setManual(false);
                                }
                        }
                        ImGui::PopID();
                }

                bool anyDeleted = false;
                for (auto it = scopes_.begin(); it != scopes_.end();) {
                        if (it->second && it->second->isPendingDelete()) {
                                it         = scopes_.erase(it);
                                anyDeleted = true;
                        } else {
                                ++it;
                        }
                }
                if (anyDeleted) {
                        needsLayout_ = true;
                        for (auto &pair : scopes_)
                                pair.second->setManual(false);
                }
        }
        ImGui::End();
}
