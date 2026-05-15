#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "implot.h"
#include "implot_internal.h"

#include "core/jlink_dev.hpp"
#include "gui/monitor.hpp"

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
                const char *pointOptions[] = {"256", "512", "1024", "2048", "4096", "8192", "16384", "32768", "65536", "131072", "262144", "524288", "1048576"};
                i32         currentIdx     = 0;
                for (i32 i = 0; i < (i32)(sizeof(pointOptions) / sizeof(pointOptions[0])); ++i) {
                        if (fftPoints_ == atoi(pointOptions[i])) {
                                currentIdx = i;
                                break;
                        }
                }

                if (ImGui::Combo("##fftPoints", &currentIdx, pointOptions, (i32)(sizeof(pointOptions) / sizeof(pointOptions[0])))) {
                        i32 nextPoints = atoi(pointOptions[currentIdx]);
                        if (nextPoints != fftPoints_) {
                                reinitFft(nextPoints);
                        }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("FFT Points (Resolution)");

                ImGui::SameLine();
                ImGui::TextDisabled("Peaks");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(30);
                char pkBuf[16];
                snprintf(pkBuf, sizeof(pkBuf), "%d", fftPeakCount_);
                if (ImGui::InputText("##fftPeakCount", pkBuf, sizeof(pkBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
                        fftPeakCount_ = atoi(pkBuf);
                        if (fftPeakCount_ < 0) fftPeakCount_ = 0;
                        if (fftPeakCount_ > 20) fftPeakCount_ = 20;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter number of peaks and press Enter to confirm");
        } else {
                if (ImGui::Button("TIME")) {
                        showFft_ = true;
                }
        }

        // Right-aligned buttons: Delete Scope and Pause/Resume
        float delBtnWidth     = ImGui::CalcTextSize("Delete Scope").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float pauseBtnWidth   = std::max(ImGui::CalcTextSize("PAUSE").x, ImGui::CalcTextSize("RESUME").x) +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
        float spacing         = ImGui::GetStyle().ItemSpacing.x;
        float totalRightWidth = delBtnWidth + spacing + pauseBtnWidth;
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
        if (ImGui::BeginTable("MonitorTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Port");
                ImGui::TableHeadersRow();

                std::vector<std::string> keys;
                for (auto &pair : chs_)
                        keys.push_back(pair.first);
                std::sort(keys.begin(), keys.end());

                for (i32 i = 0; i < static_cast<i32>(keys.size()); ++i)
                        drawTableRow(keys[i], chs_[keys[i]], i, keys);

                // Detect click on blank space inside the table to deselect all
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
                        for (auto &pair : chs_)
                                pair.second->selected_ = false;
                        lastSelectedIndex_ = -1;
                }

                ImGui::EndTable();
        }
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
MonitorScope::drawTableRow(const std::string &chName, std::shared_ptr<MonitorChannel> &ch, i32 idx, const std::vector<std::string> &allKeys)
{
        ImGui::PushID(chName.c_str());
        ImGui::TableNextRow();

        // 1. Name (Selectable for Shift/Ctrl support)
        ImGui::TableNextColumn();
        bool isSelected = ch->selected_;
        if (ImGui::Selectable(chName.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
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
                        for (auto &pair : chs_) pair.second->selected_ = false;
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

        // 3. Wave Control
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
                std::lock_guard lk(ch->waveMtx_);
                ImGui::Text("Wave Generator: %s", chName.c_str());
                ImGui::Separator();

                const char *types[]     = {"Sine", "Square", "Triangle"};
                i32         currentType = static_cast<i32>(ch->wave_.cfg.type);
                if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
                        ch->wave_.cfg.type = static_cast<wave_type_t>(currentType);
                }

                ImGui::SetNextItemWidth(100);
                f32 f = ch->wave_.cfg.freq;
                if (ImGui::InputFloat("Freq (Hz)", &f, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.freq = f;
                }
                ImGui::SetNextItemWidth(100);
                f32 a = ch->wave_.cfg.amp;
                if (ImGui::InputFloat("Amp", &a, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.amp = a;
                }
                ImGui::SetNextItemWidth(100);
                f32 o = ch->wave_.cfg.offset;
                if (ImGui::InputFloat("Offset", &o, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.offset = o;
                }
                if (ch->wave_.cfg.type != WAVE_TYPE_SINE) {
                        ImGui::SetNextItemWidth(100);
                        f32 d = ch->wave_.cfg.duty;
                        if (ImGui::InputFloat("Duty", &d, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->wave_.cfg.duty = d;
                        }
                }

                ImGui::Separator();
                if (ImGui::Button("Close"))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
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

        if (showFft_) {
                if (!ImGui::BeginTable("##fftLayoutTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                        return;
                }
                ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthStretch, 0.75f);
                ImGui::TableSetupColumn("Peaks", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
        }

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
                for (auto &[chName, ch] : chs_) {
                        // 1. Handle Visibility
                        ImPlot::HideNextItem(!ch->show_, ImPlotCond_Once);

                        // 2. Handle Style & Color
                        const f32 *col = ch->getColor();
                        ImPlot::SetNextLineStyle(ImVec4(col[0], col[1], col[2], col[3]), ch->getLineWeight());
                        if (ch->showMarkers())
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);

                        // 3. Prepare and Plot Data
                        bool plotted = false;
                        if (showFft_) {
                                if (ch->show_ && fftPoints_ > 0) {
                                        std::lock_guard lk(ch->valMutex_);

                                        // Dynamic extraction: Find points within the current time-domain window
                                        const double xmin = (linkXMin) ? *linkXMin : 0.0;
                                        const double xmax = (linkXMax) ? *linkXMax : 1.0;

                                        auto itStart =
                                            std::lower_bound(ch->rTs_.begin(), ch->rTs_.end(), xmin);
                                        auto itEnd = std::upper_bound(ch->rTs_.begin(), ch->rTs_.end(), xmax);

                                        size_t startIdx     = std::distance(ch->rTs_.begin(), itStart);
                                        size_t endIdx       = std::distance(ch->rTs_.begin(), itEnd);
                                        size_t visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;

                                        if (visibleCount > 0) {
                                                // Prepare buffer: Zero-fill first
                                                std::fill(fftLoBuf_.begin(), fftLoBuf_.end(), 0.0f);

                                                // Copy up to fftPoints_ points
                                                size_t copyCount = std::min(visibleCount, static_cast<size_t>(fftPoints_));
                                                // If window has more points than FFT resolution, take the latest ones in that
                                                // window
                                                size_t readOffset = (visibleCount > copyCount) ? (visibleCount - copyCount) : 0;

                                                f32 mean = 0;
                                                for (usize i = 0; i < copyCount; ++i) {
                                                        f32 v        = ch->rVals_[startIdx + readOffset + i];
                                                        fftLoBuf_[i] = v;
                                                        mean        += v;
                                                }
                                                mean /= (f32)copyCount;

                                                // DC removal
                                                for (size_t i = 0; i < copyCount; ++i) {
                                                        fftLoBuf_[i] -= mean;
                                                }

                                                // Update FS based on average delta in the window
                                                f32 fs = (parent_) ? (f32)parent_->getHz() : 1000.0f;
                                                if (copyCount > 1) {
                                                        f64 totalTime = ch->rTs_[startIdx + readOffset + copyCount - 1] -
                                                                           ch->rTs_[startIdx + readOffset];
                                                        if (totalTime > 1e-9f) {
                                                                 fs = static_cast<f32>((f64)(copyCount - 1) / totalTime);
                                                        }
                                                }
                                                // Sanity check for FS
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

                                                ImPlot::PlotLine(chName.c_str(), dxs_.data(), fftMagBuf_.data(), (int)dxs_.size());
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
                                // Stride decimation directly on the deque under the lock.
                                // O(maxDisplayPoints) deque random-accesses — no temp allocation.
                                dxs_.clear();
                                dys_.clear();
                                {
                                        std::lock_guard lk(ch->valMutex_);
                                        const size_t total = ch->rTs_.size();
                                        if (total > 0) {
                                                size_t startIdx = 0, endIdx = total;
                                                if (linkXMin) {
                                                        auto it  = std::lower_bound(ch->rTs_.begin(), ch->rTs_.end(), *linkXMin);
                                                        startIdx = static_cast<size_t>(std::distance(ch->rTs_.begin(), it));
                                                }
                                                if (linkXMax) {
                                                        auto it  = std::upper_bound(ch->rTs_.begin() + startIdx, ch->rTs_.end(), *linkXMax);
                                                        endIdx   = static_cast<size_t>(std::distance(ch->rTs_.begin(), it));
                                                }
                                                const size_t visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;
                                                if (visibleCount > 0) {
                                                        if (maxDisplayPoints > 0 && visibleCount > (size_t)maxDisplayPoints) {
                                                                // Stride-based: visit only maxDisplayPoints elements
                                                                const usize stride = visibleCount / (usize)maxDisplayPoints;
                                                                dxs_.reserve((usize)maxDisplayPoints + 1);
                                                                dys_.reserve((usize)maxDisplayPoints + 1);
                                                                for (usize i = startIdx; i < endIdx; i += stride) {
                                                                        dxs_.push_back(ch->rTs_[i]);
                                                                        dys_.push_back(static_cast<f64>(ch->rVals_[i]));
                                                                }
                                                        } else {
                                                                dxs_.reserve(visibleCount);
                                                                dys_.reserve(visibleCount);
                                                                for (usize i = startIdx; i < endIdx; ++i) {
                                                                        dxs_.push_back(ch->rTs_[i]);
                                                                        dys_.push_back(static_cast<f64>(ch->rVals_[i]));
                                                                }
                                                        }
                                                }
                                        }
                                } // valMutex_ released

                                if (!dxs_.empty()) {
                                        if (ch->getPlotStyle() == 1)
                                                ImPlot::PlotStairs(chName.c_str(),
                                                                   dxs_.data(), dys_.data(),
                                                                   static_cast<i32>(dxs_.size()));
                                        else
                                                ImPlot::PlotLine(chName.c_str(),
                                                                 dxs_.data(), dys_.data(),
                                                                 static_cast<i32>(dxs_.size()));
                                        plotted = true;
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
                if (pushedPadding) {
                        ImPlot::PopStyleVar();
                }
        }

        if (showFft_) {
                ImGui::TableSetColumnIndex(1);
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
                ImGui::EndTable();
        }
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
        const std::string &regionName =
            ch.getShmRegionName().empty() ? ch.getName() : ch.getShmRegionName();
        shm_cfg_t cfg = {regionName.c_str(), SHM_READWRITE, 4096};
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
        auto scope = std::make_shared<MonitorScope>(scopeName);
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

void
Monitor::generateBodeFreqs_()
{
        bodeFreqList_.clear();
        if (bodeFStart_ <= 0.0f || bodeFStop_ <= bodeFStart_ || bodeFStep_ <= 0.0f)
                return;
        for (float f = bodeFStart_; f <= bodeFStop_ + bodeFStep_ * 0.01f; f += bodeFStep_)
                bodeFreqList_.push_back(static_cast<double>(f));
}

MonitorChannel *
Monitor::findChannelByKey_(const char *key)
{
        const char *sep = std::strchr(key, '/');
        if (!sep)
                return nullptr;
        return findChannel(std::string(key, sep), std::string(sep + 1));
}

void
Monitor::bodeDraw_()
{
        // Initialize curve colors from colormap on first render
        if (!bodeStyleInit_) {
                ImVec4 c0 = ImPlot::GetColormapColor(0);
                ImVec4 c1 = ImPlot::GetColormapColor(1);
                memcpy(bodeMagStyle_.color, &c0.x, sizeof(f32) * 4);
                memcpy(bodePhsStyle_.color, &c1.x, sizeof(f32) * 4);
                bodeStyleInit_ = true;
        }

        // ---------- Channel selection ----------
        std::vector<std::string> keys;
        for (auto &[sn, sc] : scopes_)
                for (auto &[cn, _] : sc->getChannels())
                        keys.push_back(sn + "/" + cn);
        std::sort(keys.begin(), keys.end());

        auto drawCombo = [&](const char *id, const char *labelText, char *buf, size_t bufSz) {
                ImGui::TextDisabled("%s", labelText);
                ImGui::SameLine();
                int curIdx = -1;
                for (int i = 0; i < (int)keys.size(); ++i)
                        if (std::strcmp(buf, keys[i].c_str()) == 0) { curIdx = i; break; }
                const char *preview = (curIdx >= 0) ? keys[curIdx].c_str() : "(none)";
                ImGui::SetNextItemWidth(180);
                if (ImGui::BeginCombo(id, preview)) {
                        for (int i = 0; i < (int)keys.size(); ++i) {
                                bool sel = (i == curIdx);
                                if (ImGui::Selectable(keys[i].c_str(), sel))
                                        std::snprintf(buf, bufSz, "%s", keys[i].c_str());
                                if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
        };

        drawCombo("##bodeIn",  "Input",  bodeInputKey_,  sizeof(bodeInputKey_));
        ImGui::SameLine();
        drawCombo("##bodeOut", "Output", bodeOutputKey_, sizeof(bodeOutputKey_));

        // ---------- Sweep parameters ----------
        if (bodeSweepRunning_) ImGui::BeginDisabled();

        ImGui::TextDisabled("F Start(Hz)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputFloat("##bFS", &bodeFStart_, 0, 0, "%.3g");
        ImGui::SameLine();
        ImGui::TextDisabled("F Stop(Hz)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputFloat("##bFE", &bodeFStop_, 0, 0, "%.3g");
        ImGui::SameLine();
        ImGui::TextDisabled("Step(Hz)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputFloat("##bSTP", &bodeFStep_, 0, 0, "%.3g");
        if (bodeFStep_ <= 0.0f) bodeFStep_ = 1.0f;
        ImGui::SameLine();
        ImGui::TextDisabled("Dwell(s)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat("##bDw", &bodeDwellSec_, 0, 0, "%.2f");
        if (bodeDwellSec_ < 0.05f) bodeDwellSec_ = 0.05f;
        ImGui::SameLine();
        ImGui::TextDisabled("Amp");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputFloat("##bAmp", &bodeAmp_, 0, 0, "%.3g");
        if (bodeAmp_ < 0.0f) bodeAmp_ = 0.0f;

        if (bodeSweepRunning_) ImGui::EndDisabled();

        // ---------- Control buttons ----------
        ImGui::SameLine();
        if (!bodeSweepRunning_) {
                if (ImGui::Button("Start##bodeSt")) {
                        generateBodeFreqs_();
                        if (!bodeFreqList_.empty()) {
                                bodeData_.clear();
                                bodeFreqsV_.clear();
                                bodeMagsV_.clear();
                                bodePhsV_.clear();
                                bodeSweepFreqIdx_  = 0;
                                bodeSweepRunning_  = true;
                                bodeSweepStepStart_ = get_mono_ts_us();
                                MonitorChannel *inCh2 = findChannelByKey_(bodeInputKey_);
                                if (inCh2) {
                                        std::lock_guard lk(inCh2->waveMtx_);
                                        inCh2->wave_.cfg.freq = static_cast<float>(bodeFreqList_[0]);
                                        inCh2->wave_.cfg.amp  = bodeAmp_;
                                        inCh2->waveEnable_ = true;
                                }
                        }
                }
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
                if (ImGui::Button("Stop##bodeSt")) {
                        bodeSweepRunning_ = false;
                        MonitorChannel *inCh2 = findChannelByKey_(bodeInputKey_);
                        if (inCh2) {
                                {
                                        std::lock_guard lk(inCh2->waveMtx_);
                                        inCh2->waveEnable_ = false;
                                }
                                inCh2->setWVal(0.0f);
                                inCh2->markWValDirty();
                        }
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                const int   total = static_cast<int>(bodeFreqList_.size());
                const float pct   = (total > 0) ? static_cast<float>(bodeSweepFreqIdx_) / total : 0.0f;
                ImGui::ProgressBar(pct, ImVec2(100.0f, 0.0f));
                ImGui::SameLine();
                const double curF = (bodeSweepFreqIdx_ < total)
                                        ? bodeFreqList_[bodeSweepFreqIdx_]
                                        : static_cast<double>(bodeFStop_);
                ImGui::TextDisabled("%d/%d  %.3g Hz", bodeSweepFreqIdx_, total, curF);
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear##bodeCl")) {
                bodeData_.clear();
                bodeFreqsV_.clear();
                bodeMagsV_.clear();
                bodePhsV_.clear();
        }

        ImGui::Separator();

        // ---------- Bode subplots ----------
        if (ImPlot::BeginSubplots("##bodeSP", 2, 1, ImVec2(-1.0f, -1.0f),
                                   ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle)) {
                auto legendPopup = [](const char *label, BodeCurveStyle &style) {
                        if (ImPlot::BeginLegendPopup(label)) {
                                f32 colArr[4];
                                memcpy(colArr, style.color, sizeof(colArr));
                                if (ImGui::ColorEdit4("Color", colArr, ImGuiColorEditFlags_NoInputs)) {
                                        style.useAutoColor = false;
                                        memcpy(style.color, colArr, sizeof(colArr));
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox("Auto", &style.useAutoColor)) {
                                        if (style.useAutoColor) {
                                                static i32 bodeShuffleIdx = 0;
                                                bodeShuffleIdx = (bodeShuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 nc = ImPlot::GetColormapColor(bodeShuffleIdx);
                                                memcpy(style.color, &nc.x, sizeof(f32) * 4);
                                        }
                                }
                                ImGui::SetNextItemWidth(100);
                                ImGui::SliderFloat("Width", &style.lineWeight, 0.5f, 5.0f, "%.1f");
                                ImGui::Checkbox("Markers", &style.showMarkers);
                                ImPlot::EndLegendPopup();
                        }
                };

                if (ImPlot::BeginPlot("##bodeMag")) {
                        ImPlot::SetupAxis(ImAxis_X1, "Frequency (Hz)");
                        ImPlot::SetupAxis(ImAxis_Y1, "Magnitude (dB)");
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                        ImPlot::SetNextLineStyle(
                            ImVec4(bodeMagStyle_.color[0], bodeMagStyle_.color[1],
                                   bodeMagStyle_.color[2], bodeMagStyle_.color[3]),
                            bodeMagStyle_.lineWeight);
                        if (bodeMagStyle_.showMarkers)
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);
                        if (!bodeFreqsV_.empty())
                                ImPlot::PlotLine("H(jw)##mag", bodeFreqsV_.data(), bodeMagsV_.data(),
                                                 static_cast<int>(bodeFreqsV_.size()));
                        legendPopup("H(jw)##mag", bodeMagStyle_);
                        ImPlot::EndPlot();
                }
                if (ImPlot::BeginPlot("##bodePhs")) {
                        ImPlot::SetupAxis(ImAxis_X1, "Frequency (Hz)");
                        ImPlot::SetupAxis(ImAxis_Y1, "Phase (deg)");
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                        ImPlot::SetNextLineStyle(
                            ImVec4(bodePhsStyle_.color[0], bodePhsStyle_.color[1],
                                   bodePhsStyle_.color[2], bodePhsStyle_.color[3]),
                            bodePhsStyle_.lineWeight);
                        if (bodePhsStyle_.showMarkers)
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);
                        if (!bodeFreqsV_.empty())
                                ImPlot::PlotLine("H(jw)##phs", bodeFreqsV_.data(), bodePhsV_.data(),
                                                 static_cast<int>(bodeFreqsV_.size()));
                        legendPopup("H(jw)##phs", bodePhsStyle_);
                        ImPlot::EndPlot();
                }
                ImPlot::EndSubplots();
        }
}

/* -------------------------------------------------------------------------- */

void
Monitor::updateDisplay()
{
        bool open = true;
        if (ImGui::Begin(name_.c_str(), &open)) {
                if (!open)
                        markPendingDelete();

                if (ImGui::Button("Add Scope")) {
                        char nameBuf[32];
                        snprintf(nameBuf, sizeof(nameBuf), "scope_%zu", scopes_.size());
                        addScope(nameBuf);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.5f, 0.0f, 0.6f)); // Orange
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.6f, 0.1f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
                if (ImGui::Button("Clear Data"))
                        clearData();
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (showBode_) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 0.8f));
                        if (ImGui::Button("Bode")) showBode_ = false;
                        ImGui::PopStyleColor();
                } else {
                        if (ImGui::Button("Bode")) showBode_ = true;
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
                        JLinkDev::instance().reqRestart();
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
                        clearData();
                        JLinkDev::instance().reqRestart();
                }
                ImGui::SameLine();

                // Single MODE combo — targets FFT axis when any scope is in FFT view,
                // otherwise targets the time-domain axis.
                bool anyFft = false;
                for (const auto &sp : scopes_)
                        if (sp.second && sp.second->isFftEnabled()) { anyFft = true; break; }

                const char *viewModeNames[] = {"FULL", "FOLLOW", "MANUAL"};
                i32         curMode = anyFft ? (i32)fftViewMode_ : (i32)viewMode_;

                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("##Mode", &curMode, viewModeNames, 3)) {
                        if (anyFft) fftViewMode_ = static_cast<MonitorViewMode>(curMode);
                        else        viewMode_    = static_cast<MonitorViewMode>(curMode);
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

                for (size_t i = 0; i < keys.size(); ++i) {
                        auto &scope = scopes_[keys[i]];

                        if (ImGui::BeginChild(keys[i].c_str(), ImVec2(avail.x, scope->getHeight()), true)) {
                                scope->menu();

                                bool   isPaused = g_monitorPaused.load();
                                f64    now      = sessionTimeSec();

                                // When FULL mode is active, always fit to exact data bounds
                                if (viewMode_ == MonitorViewMode::FULL) {
                                        if (!isPaused) {
                                                f64 earliest = now;
                                                f64 latest   = 0.0;
                                                bool   hasData  = false;
                                                for (const auto &[_, sc] : scopes_) {
                                                        for (const auto &[__, ch] : sc->getChannels()) {
                                                                const f64 e = ch->earliestTs();
                                                                const f64 l = ch->latestTs();
                                                                if (e >= 0.0f) {
                                                                        hasData = true;
                                                                        if (e < earliest)
                                                                                earliest = static_cast<f64>(e);
                                                                        if (l >= 0.0f && l > latest)
                                                                                latest = static_cast<f64>(l);
                                                                }
                                                        }
                                                }
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
                                } else if (!isPaused && JLinkDev::instance().isHssRunning()) {
                                        // FOLLOW mode logic (only updates when running)
                                        // CRITICAL: If user is clicking or scrolling, STOP auto-updating to allow ImPlot to
                                        // handle interaction
                                        if (viewMode_ == MonitorViewMode::FOLLOW &&
                                            !ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetIO().MouseWheel == 0) {
                                                f64 span        = linkXMax_ - linkXMin_;
                                                f64 currentSpan = (span > 0.001) ? span : 1.0;
                                                if (historySeconds_ > 0.0f && currentSpan > historySeconds_) {
                                                        currentSpan = historySeconds_;
                                                }
                                                linkXMax_ = now;
                                                linkXMin_ = now - currentSpan;
                                                if (linkXMin_ < 0.0)
                                                        linkXMin_ = 0.0;
                                                lastNow_ = now;
                                        }
                                }

                                wasPaused_ = isPaused;
                                if (scope->isFftEnabled()) {
                                        scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, fftViewMode_);
                                } else {
                                        scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, viewMode_);
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

                // ---- Bode sweep advancement (runs every frame while dwelling) ----
                if (bodeSweepRunning_ && !bodeFreqList_.empty() &&
                    bodeSweepFreqIdx_ < (int)bodeFreqList_.size()) {
                        const double    f     = bodeFreqList_[bodeSweepFreqIdx_];
                        MonitorChannel *inCh  = findChannelByKey_(bodeInputKey_);
                        MonitorChannel *outCh = findChannelByKey_(bodeOutputKey_);

                        if (inCh && outCh) {
                                auto [reX, imX] = inCh->dftSlice(linkXMin_, linkXMax_, f);
                                auto [reY, imY] = outCh->dftSlice(linkXMin_, linkXMax_, f);
                                const double magX  = std::hypot(reX, imX);
                                const double magY  = std::hypot(reY, imY);
                                const double magDb = (magX > 1e-10) ? 20.0 * std::log10(magY / magX) : -120.0;
                                double       phRad = std::atan2(imY, reY) - std::atan2(imX, reX);
                                phRad              = std::remainder(phRad, 2.0 * M_PI);

                                // Update the in-progress point for this frequency index
                                const BodePoint pt{f, magDb, phRad * (180.0 / M_PI)};
                                if (bodeSweepFreqIdx_ < (int)bodeData_.size()) {
                                        bodeData_[bodeSweepFreqIdx_] = pt;
                                } else {
                                        bodeData_.push_back(pt);
                                        bodeFreqsV_.push_back(0.0);
                                        bodeMagsV_.push_back(0.0);
                                        bodePhsV_.push_back(0.0);
                                }
                                bodeFreqsV_[bodeSweepFreqIdx_] = pt.freq;
                                bodeMagsV_[bodeSweepFreqIdx_]  = pt.magDb;
                                bodePhsV_[bodeSweepFreqIdx_]   = pt.phaseDeg;
                        }

                        // Advance frequency when dwell time expires
                        const u64 now = get_mono_ts_us();
                        if (now - bodeSweepStepStart_ >= static_cast<u64>(bodeDwellSec_ * 1.0e6f)) {
                                ++bodeSweepFreqIdx_;
                                if (bodeSweepFreqIdx_ >= (int)bodeFreqList_.size()) {
                                        bodeSweepRunning_ = false;
                                        MonitorChannel *inChStop = findChannelByKey_(bodeInputKey_);
                                        if (inChStop) {
                                                {
                                                        std::lock_guard lk(inChStop->waveMtx_);
                                                        inChStop->waveEnable_ = false;
                                                }
                                                inChStop->setWVal(0.0f);
                                                inChStop->markWValDirty();
                                        }
                                } else {
                                        MonitorChannel *inCh2 = findChannelByKey_(bodeInputKey_);
                                        if (inCh2) {
                                                std::lock_guard lk(inCh2->waveMtx_);
                                                inCh2->wave_.cfg.freq =
                                                    static_cast<float>(bodeFreqList_[bodeSweepFreqIdx_]);
                                        }
                                        bodeSweepStepStart_ = now;
                                }
                        }
                }
        }
        ImGui::End();

        // ---- Bode plot window (separate floating window) ----
        if (showBode_) {
                char bodeTitle[320];
                std::snprintf(bodeTitle, sizeof(bodeTitle), "Bode Plot  [%s]##bode%s",
                              name_.c_str(), name_.c_str());
                ImGui::SetNextWindowSize(ImVec2(700.0f, 520.0f), ImGuiCond_FirstUseEver);
                if (ImGui::Begin(bodeTitle, &showBode_))
                        bodeDraw_();
                ImGui::End();
        }
}
