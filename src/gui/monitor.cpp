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
#include <vector>

#include "imgui.h"
#include "implot.h"
#include "implot_internal.h"

#include "gui/monitor.hpp"
#include "platform/native_dlg.hpp"

std::atomic<bool> g_monitorPaused{false};

std::vector<Monitor *> Monitor::sInstances_;
std::mutex             Monitor::sMtxInstances_;

class MonitorChannel;
class MonitorScope;

struct ChannelMovePayload {
        MonitorScope *srcScope;
        char          chName[128];
};

/* -------------------------------------------------------------------------- */

void
MonitorScope::menu()
{
        // Scope Toolbar
        // Scope Toolbar
        if (ImGui::Button(e_draw == DrawEnum::PLOT ? "PLOT" : "TABLE")) {
                e_draw = (e_draw == DrawEnum::PLOT) ? DrawEnum::TABLE : DrawEnum::PLOT;
        }

        ImGui::SameLine();
        if (showFft_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f)); // Blue
                if (ImGui::Button("FREQ")) {
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
                ImGui::TextDisabled("Peaks");
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
                        ImGui::SetTooltip("Enter number of peaks and press Enter to confirm");
        } else {
                if (ImGui::Button("TIME")) {
                        showFft_ = true;
                }
        }

        // Hide All / Show All channels
        ImGui::SameLine();
        {
                bool anyVisible = false;
                for (auto &[_, ch] : chs_)
                        if (ch && ch->show_) { anyVisible = true; break; }

                if (anyVisible) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        if (ImGui::Button("Hide All")) {
                                for (auto &[_, ch] : chs_)
                                        if (ch) ch->show_ = false;
                        }
                        ImGui::PopStyleColor();
                } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.7f, 0.3f, 0.6f));
                        if (ImGui::Button("Show All")) {
                                for (auto &[_, ch] : chs_)
                                        if (ch) ch->show_ = true;
                        }
                        ImGui::PopStyleColor();
                }
        }

        // Right-aligned buttons: Delete Scope, Hide Scope, and Pause/Resume
        float delBtnWidth  = ImGui::CalcTextSize("Delete Scope").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float hideBtnWidth = std::max(ImGui::CalcTextSize("Hide Scope").x, ImGui::CalcTextSize("Show Scope").x) +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
        float pauseBtnWidth =
            std::max(ImGui::CalcTextSize("PAUSE").x, ImGui::CalcTextSize("RESUME").x) + ImGui::GetStyle().FramePadding.x * 2.0f;
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
                if (ImGui::Button("RESUME"))
                        paused_ = false;
                ImGui::PopStyleColor();
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 0.6f)); // Green
                if (ImGui::Button("PAUSE"))
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
        if (!ImGui::BeginTable("MonitorTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                return;

        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Port");
        ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        std::vector<std::string> keys;
        for (auto &pair : chs_)
                keys.push_back(pair.first);
        std::sort(keys.begin(), keys.end());

        // Build a prefix trie on '.' so struct members render as an expandable tree.
        struct TNode {
                std::map<std::string, TNode> children;
                std::string                  leafKey; // non-empty → this node is a channel
        };
        TNode root;
        for (const auto &k : keys) {
                TNode      *cur = &root;
                std::string rem = k;
                size_t      pos;
                while ((pos = rem.find('.')) != std::string::npos) {
                        cur = &cur->children[rem.substr(0, pos)];
                        rem = rem.substr(pos + 1);
                }
                cur->children[rem].leafKey = k;
        }

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
                                for (auto &[childLabel, childNode] : node.children)
                                        drawNode(childLabel, childNode, fullPath + "." + childLabel);
                                ImGui::TreePop();
                        }
                }
        };

        for (auto &[childLabel, childNode] : root.children)
                drawNode(childLabel, childNode, childLabel);

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
                const char *currentName = ch->findEnumName(static_cast<i64>(ch->getRVal()));
                char        previewBuf[128];
                if (currentName)
                        snprintf(previewBuf, sizeof(previewBuf), "%s (%lld)", currentName, static_cast<i64>(ch->getRVal()));
                else
                        snprintf(previewBuf, sizeof(previewBuf), "Unknown (%f)", ch->getRVal());

                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##enum", previewBuf)) {
                        for (const auto &e : ch->getEnums()) {
                                bool isSelected = (static_cast<i64>(ch->getRVal()) == e.value);
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
                        i32 v = static_cast<i32>(ch->getRVal());
                        ImGui::InputInt("##val", &v, 0, 0);
                        if (ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->setWVal(static_cast<f32>(v));
                                ch->markWValDirty();
                        }
                } else {
                        f32 v = ch->getRVal();
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
                if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
                        pending.type.store(currentType);
                        pending.dirty.store(true);
                }

                ImGui::SetNextItemWidth(100);
                f32 f = pending.freq.load();
                if (ImGui::InputFloat("Freq (Hz)", &f, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.freq.store(f);
                        pending.dirty.store(true);
                }
                ImGui::SetNextItemWidth(100);
                f32 a = pending.amp.load();
                if (ImGui::InputFloat("Amp", &a, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.amp.store(a);
                        pending.dirty.store(true);
                }
                ImGui::SetNextItemWidth(100);
                f32 o = pending.offset.load();
                if (ImGui::InputFloat("Offset", &o, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        pending.offset.store(o);
                        pending.dirty.store(true);
                }
                if (pending.type.load() != WAVE_TYPE_SINE) {
                        ImGui::SetNextItemWidth(100);
                        f32 d = pending.duty.load();
                        if (ImGui::InputFloat("Duty", &d, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                pending.duty.store(d);
                                pending.dirty.store(true);
                        }
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

        if (!ImGui::BeginTable("##plotLayoutTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                return;
        }
        ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthStretch, 0.75f);
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

                for (auto &[chName, ch] : chs_) {
                        // 1. Handle Visibility
                        // For existing items: directly set Show so "Hide All/Show All"
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
                                if (ch->show_ && fftPoints_ > 0) {
                                        // FFT reads from the raw ring under mtxMonitors_.
                                        const double xmin = (linkXMin) ? *linkXMin : 0.0;
                                        const double xmax = (linkXMax) ? *linkXMax : 1.0;

                                        const usize startIdx     = rd.rawLowerBound(xmin);
                                        const usize endIdx       = rd.rawUpperBound(xmax);
                                        const usize visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;

                                        if (visibleCount > 0) {
                                                std::fill(fftLoBuf_.begin(), fftLoBuf_.end(), 0.0f);

                                                const usize copyCount = std::min(visibleCount, static_cast<usize>(fftPoints_));
                                                const usize readOffset =
                                                    (visibleCount > copyCount) ? (visibleCount - copyCount) : 0;

                                                f32 mean = 0;
                                                for (usize i = 0; i < copyCount; ++i) {
                                                        f32 v         = rd.rawVal(startIdx + readOffset + i);
                                                        fftLoBuf_[i]  = v;
                                                        mean         += v;
                                                }
                                                mean /= (f32)copyCount;
                                                for (usize i = 0; i < copyCount; ++i) {
                                                        fftLoBuf_[i] -= mean;
                                                }

                                                f32 fs = (parent_) ? (f32)parent_->getHz() : 1000.0f;
                                                if (copyCount > 1) {
                                                        f64 totalTime = rd.rawTs(startIdx + readOffset + copyCount - 1) -
                                                                        rd.rawTs(startIdx + readOffset);
                                                        if (totalTime > 1e-9f) {
                                                                fs = static_cast<f32>((f64)(copyCount - 1) / totalTime);
                                                        }
                                                }
                                                if (fs < 0.1f)
                                                        fs = 0.1f;
                                                if (fs > 10000000.0f)
                                                        fs = 10000000.0f;
                                                fft_.cfg.fs = fs;

                                                fft_.lo.need_exec = 1;
                                                fft_exec(&fft_);

                                                f32 df = fs / (f32)fftPoints_;
                                                dxs_.resize(fftPoints_ / 2 + 1);
                                                for (i32 i = 0; i < (i32)dxs_.size(); ++i) {
                                                        dxs_[i]       = (f64)i * (f64)df;
                                                        fftMagBuf_[i] = (f64)fftMagF32_[i] * 2.0 / (f64)fftPoints_;
                                                }

                                                ImPlot::PlotLine(
                                                    chName.c_str(), dxs_.data(), fftMagBuf_.data(), (int)dxs_.size());
                                                plotted = true;

                                                if (fftPeakCount_ > 0) {
                                                        std::vector<Peak> peaks;
                                                        for (int i = 2; i < (int)fftMagBuf_.size() - 2; ++i) {
                                                                if (fftMagBuf_[i] > fftMagBuf_[i - 1] &&
                                                                    fftMagBuf_[i] > fftMagBuf_[i + 1]) {
                                                                        peaks.push_back({dxs_[i], fftMagBuf_[i]});
                                                                }
                                                        }
                                                        std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) {
                                                                return a.mag > b.mag;
                                                        });
                                                        if ((int)peaks.size() > fftPeakCount_)
                                                                peaks.resize(fftPeakCount_);
                                                        channelPeaks_[chName] = std::move(peaks);
                                                }
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
                                if (ImGui::Combo("Style", &currentStyle, styleNames, 2)) {
                                        ch->getPlotStyle() = currentStyle;
                                }
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
        if (ImGui::Begin(name_.c_str(), &open)) {
                if (!open)
                        markPendingDelete();

                if (csvLoading_.load(std::memory_order_acquire)) {
                        const char *frames[] = {"|", "/", "-", "\\"};
                        int         fi       = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                                           "%s  正在解析 CSV...",
                                           frames[fi]);
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
                if (ImGui::Button("Export CSV")) {
                        char dfltBuf[128];
                        auto now = std::time(nullptr);
                        std::strftime(dfltBuf, sizeof(dfltBuf), "%Y%m%d_%H%M%S_", std::localtime(&now));
                        std::string defaultName = std::string(dfltBuf) + name_ + ".csv";

                        std::string fullpath = nativeDlgSave("Export CSV", {{"CSV Files", {"csv"}}}, defaultName);
                        if (fullpath.empty()) {
                                LOG_I("Monitor[%s] export cancelled", name_.c_str());
                        } else {
                                if (fullpath.find(".csv") == std::string::npos)
                                        fullpath += ".csv";

                                FILE *f = fopen(fullpath.c_str(), "w");
                                if (f) {
                                        std::vector<std::pair<std::string, MonitorChannel *>> channels;
                                        for (auto &[scopeName, scope] : scopes_) {
                                                for (auto &[chName, ch] : scope->getChannels()) {
                                                        channels.push_back({scopeName + "_" + chName, ch.get()});
                                                }
                                        }

                                        // Write header
                                        for (size_t i = 0; i < channels.size(); ++i) {
                                                fprintf(f,
                                                        "%s_Time,%s_Value%s",
                                                        channels[i].first.c_str(),
                                                        channels[i].first.c_str(),
                                                        (i == channels.size() - 1) ? "" : ",");
                                        }
                                        fprintf(f, "\n");

                                        // Find max length
                                        size_t maxLen = 0;
                                        for (auto &c : channels) {
                                                if (c.second->read_.rawSize() > maxLen) {
                                                        maxLen = c.second->read_.rawSize();
                                                }
                                        }

                                        // Write data
                                        for (size_t row = 0; row < maxLen; ++row) {
                                                for (size_t i = 0; i < channels.size(); ++i) {
                                                        if (row < channels[i].second->read_.rawSize()) {
                                                                fprintf(f,
                                                                        "%.6f,%.6f",
                                                                        channels[i].second->read_.rawTs(row),
                                                                        channels[i].second->read_.rawVal(row));
                                                        } else {
                                                                fprintf(f, ",");
                                                        }
                                                        if (i < channels.size() - 1)
                                                                fprintf(f, ",");
                                                }
                                                fprintf(f, "\n");
                                        }
                                        fclose(f);
                                        LOG_I("Monitor[%s] exported to %s", name_.c_str(), fullpath.c_str());
                                } else {
                                        LOG_E("Monitor[%s] failed to open %s for export", name_.c_str(), fullpath.c_str());
                                }
                        }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                f32 h = historySeconds_;
                if (ImGui::InputFloat("##History", &h, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        if (h < 0.0f)
                                h = 0.0f;
                        historySeconds_ = h;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("History(s)");
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
                ImGui::SameLine();
                ImGui::TextDisabled("Max Pts");

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
                        ImGui::SetTooltip("HSS sample rate (Hz). Most J-Link hardware supports up to ~200Hz reliably.");
                ImGui::SameLine();
                ImGui::TextDisabled("MaxHz");

                ImGui::SameLine();
                if (actualHz_ > 0.1f) {
                        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%.0f Hz", actualHz_);
                } else {
                        ImGui::TextDisabled("-- Hz");
                }

                // Right-aligned mode control group
                f32 spacing    = ImGui::GetStyle().ItemSpacing.x;
                f32 totalWidth = 70 + spacing + 90 + spacing + ImGui::CalcTextSize("MODE").x;

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
                ImGui::SameLine();
                ImGui::TextDisabled("MODE");

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
