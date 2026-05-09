#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "implot.h"
#include "implot_internal.h"

#include "jlink_dev.hpp"
#include "monitor.hpp"

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
        if (ImGui::Button(e_draw == DrawEnum::PLOT ? "PLOT" : "TABLE")) {
                e_draw = (e_draw == DrawEnum::PLOT) ? DrawEnum::TABLE : DrawEnum::PLOT;
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Delete Scope")) {
                markPendingDelete();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Delete Channels")) {
                for (auto &pair : chs_) {
                        if (pair.second->selected_)
                                pair.second->markPendingDelete();
                }
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
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

        ImGui::SameLine();
        ImGui::SameLine();
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
                int         currentIdx     = 0;
                for (int i = 0; i < (int)(sizeof(pointOptions) / sizeof(pointOptions[0])); ++i) {
                        if (fftPoints_ == atoi(pointOptions[i])) {
                                currentIdx = i;
                                break;
                        }
                }

                if (ImGui::Combo(
                        "##fftPoints", &currentIdx, pointOptions, (int)(sizeof(pointOptions) / sizeof(pointOptions[0])))) {
                        int nextPoints = atoi(pointOptions[currentIdx]);
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

                for (int i = 0; i < static_cast<int>(keys.size()); ++i)
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
MonitorScope::drawTableRow(const std::string               &chName,
                           std::unique_ptr<MonitorChannel> &ch,
                           int                              idx,
                           const std::vector<std::string>  &allKeys)
{
        ImGui::PushID(chName.c_str());
        ImGui::TableNextRow();

        // 1. Name (Selectable for Shift/Ctrl support)
        ImGui::TableNextColumn();
        bool isSelected = ch->selected_;
        if (ImGui::Selectable(chName.c_str(), isSelected)) {
                if (ImGui::GetIO().KeyCtrl) {
                        // Ctrl + Click: Toggle current
                        ch->selected_ = !ch->selected_;
                } else if (ImGui::GetIO().KeyShift && lastSelectedIndex_ != -1) {
                        // Shift + Click: Select range
                        int start = std::min(lastSelectedIndex_, idx);
                        int end   = std::max(lastSelectedIndex_, idx);
                        for (int i = start; i <= end; ++i) {
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
                        int v = static_cast<int>(ch->getRVal());
                        ImGui::InputInt("##val", &v, 0, 0);
                        if (ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->setWVal(static_cast<f32>(v));
                                ch->markWValDirty();
                        }
                } else {
                        float v = ch->getRVal();
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
        float availX  = ImGui::GetContentRegionAvail().x;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float btnW    = (availX - spacing) * 0.65f;
        float cfgW    = (availX - spacing) * 0.35f;

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
                int         currentType = static_cast<int>(ch->wave_.cfg.type);
                if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
                        ch->wave_.cfg.type = static_cast<wave_type_t>(currentType);
                }

                ImGui::SetNextItemWidth(100);
                float f = ch->wave_.cfg.freq;
                if (ImGui::InputFloat("Freq (Hz)", &f, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.freq = f;
                }
                ImGui::SetNextItemWidth(100);
                float a = ch->wave_.cfg.amp;
                if (ImGui::InputFloat("Amp", &a, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.amp = a;
                }
                ImGui::SetNextItemWidth(100);
                float o = ch->wave_.cfg.offset;
                if (ImGui::InputFloat("Offset", &o, 0.0f, 0.0f, "%.1f") && ImGui::IsItemDeactivated() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        ch->wave_.cfg.offset = o;
                }
                if (ch->wave_.cfg.type != WAVE_TYPE_SINE) {
                        ImGui::SetNextItemWidth(100);
                        float d = ch->wave_.cfg.duty;
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
                                            std::lower_bound(ch->rTs_.begin(), ch->rTs_.end(), static_cast<f32>(xmin));
                                        auto itEnd = std::upper_bound(ch->rTs_.begin(), ch->rTs_.end(), static_cast<f32>(xmax));

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
                                                for (size_t i = 0; i < copyCount; ++i) {
                                                        float v      = ch->rVals_[startIdx + readOffset + i];
                                                        fftLoBuf_[i] = v;
                                                        mean        += v;
                                                }
                                                mean /= (float)copyCount;

                                                // DC removal
                                                for (size_t i = 0; i < copyCount; ++i) {
                                                        fftLoBuf_[i] -= mean;
                                                }

                                                // Update FS based on average delta in the window
                                                float fs = (parent_) ? (f32)parent_->getHz() : 1000.0f;
                                                if (copyCount > 1) {
                                                        float totalTime = ch->rTs_[startIdx + readOffset + copyCount - 1] -
                                                                          ch->rTs_[startIdx + readOffset];
                                                        if (totalTime > 1e-9f) {
                                                                fs = (float)(copyCount - 1) / totalTime;
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

                                                float df = fs / (float)fftPoints_;
                                                dxs_.resize(fftPoints_ / 2 + 1);
                                                for (int i = 0; i < (int)dxs_.size(); ++i) {
                                                        dxs_[i]       = (float)i * df;
                                                        fftMagBuf_[i] = fftMagBuf_[i] * 2.0f / (float)fftPoints_;
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
                                std::lock_guard lk(ch->valMutex_);
                                const size_t    total = ch->rTs_.size();
                                if (total > 0) {
                                        size_t startIdx = 0, endIdx = total;
                                        if (linkXMin) {
                                                const f32 xmin = static_cast<f32>(*linkXMin);
                                                auto      it   = std::lower_bound(ch->rTs_.begin(), ch->rTs_.end(), xmin);
                                                startIdx       = static_cast<size_t>(std::distance(ch->rTs_.begin(), it));
                                        }
                                        if (linkXMax) {
                                                const f32 xmax = static_cast<f32>(*linkXMax);
                                                auto it = std::upper_bound(ch->rTs_.begin() + startIdx, ch->rTs_.end(), xmax);
                                                endIdx  = static_cast<size_t>(std::distance(ch->rTs_.begin(), it));
                                        }

                                        const size_t visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;
                                        if (visibleCount > 0) {
                                                tempTs_.assign(ch->rTs_.begin() + startIdx, ch->rTs_.begin() + endIdx);
                                                tempVals_.assign(ch->rVals_.begin() + startIdx, ch->rVals_.begin() + endIdx);

                                                const f32 *pTs   = tempTs_.data();
                                                const f32 *pVals = tempVals_.data();

                                                dxs_.clear();
                                                dys_.clear();

                                                if (maxDisplayPoints > 0 && visibleCount > maxDisplayPoints) {
                                                        size_t buckets = maxDisplayPoints / 2;
                                                        if (buckets < 1)
                                                                buckets = 1;
                                                        double samplesPerBucket = static_cast<double>(visibleCount) / buckets;

                                                        for (size_t b = 0; b < buckets; ++b) {
                                                                size_t bStart = static_cast<size_t>(b * samplesPerBucket);
                                                                size_t bEnd   = static_cast<size_t>((b + 1) * samplesPerBucket);
                                                                if (bEnd > visibleCount)
                                                                        bEnd = visibleCount;
                                                                if (bStart >= bEnd)
                                                                        continue;

                                                                size_t minI = bStart, maxI = bStart;
                                                                f32    minVal = pVals[bStart], maxVal = minVal;
                                                                for (size_t i = bStart + 1; i < bEnd; ++i) {
                                                                        const f32 val = pVals[i];
                                                                        if (val < minVal) {
                                                                                minVal = val;
                                                                                minI   = i;
                                                                        } else if (val > maxVal) {
                                                                                maxVal = val;
                                                                                maxI   = i;
                                                                        }
                                                                }
                                                                if (minI < maxI) {
                                                                        dxs_.push_back(pTs[minI]);
                                                                        dys_.push_back(minVal);
                                                                        dxs_.push_back(pTs[maxI]);
                                                                        dys_.push_back(maxVal);
                                                                } else if (minI > maxI) {
                                                                        dxs_.push_back(pTs[maxI]);
                                                                        dys_.push_back(maxVal);
                                                                        dxs_.push_back(pTs[minI]);
                                                                        dys_.push_back(minVal);
                                                                } else {
                                                                        dxs_.push_back(pTs[minI]);
                                                                        dys_.push_back(minVal);
                                                                }
                                                        }
                                                } else {
                                                        dxs_.assign(tempTs_.begin(), tempTs_.end());
                                                        dys_.assign(tempVals_.begin(), tempVals_.end());
                                                }

                                                if (ch->getPlotStyle() == 1) // 1 = Stairs
                                                        ImPlot::PlotStairs(chName.c_str(),
                                                                           dxs_.data(),
                                                                           dys_.data(),
                                                                           static_cast<int>(dxs_.size()));
                                                else
                                                        ImPlot::PlotLine(chName.c_str(),
                                                                         dxs_.data(),
                                                                         dys_.data(),
                                                                         static_cast<int>(dxs_.size()));
                                                plotted = true;
                                        }
                                }
                        }

                        if (!plotted) {
                                ImPlot::PlotLine(chName.c_str(), (const f32 *)nullptr, (const f32 *)nullptr, 0);
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
                                float colArr[4] = {curCol.x, curCol.y, curCol.z, curCol.w};
                                if (ImGui::ColorEdit4("Color", colArr, ImGuiColorEditFlags_NoInputs)) {
                                        ch->useAutoColor() = false;
                                        memcpy(ch->getColor(), colArr, sizeof(colArr));
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox("Auto", &ch->useAutoColor())) {
                                        if (ch->useAutoColor()) {
                                                static int shuffleIdx = 0;
                                                shuffleIdx            = (shuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 newCol         = ImPlot::GetColormapColor(shuffleIdx);
                                                memcpy(ch->getColor(), &newCol.x, sizeof(float) * 4);
                                        }
                                }

                                ImGui::SetNextItemWidth(100);
                                const char *styleNames[] = {"Line", "Stairs"};
                                int         currentStyle = ch->getPlotStyle();
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
        auto ch = std::make_unique<MonitorChannel>(chName);

        // Assign an initial color from the colormap
        static int globalColorIdx = 0;
        ImVec4     c              = ImPlot::GetColormapColor(globalColorIdx);
        globalColorIdx            = (globalColorIdx + 1) % ImPlot::GetColormapSize();
        memcpy(ch->getColor(), &c.x, sizeof(float) * 4);
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
        if (ch.getDevice() != "LOCAL")
                return;
        shm_cfg_t cfg = {ch.getName().c_str(), SHM_READONLY, ch.getNumBytes()};
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
                                                if (chPayload->numEnums > 0) {
                                                        std::vector<MonitorChannel::EnumEntry> ents;
                                                        for (int i = 0; i < chPayload->numEnums; ++i)
                                                                ents.push_back(
                                                                    {chPayload->enums[i].name, chPayload->enums[i].value});
                                                        ch->setEnums(std::move(ents));
                                                }
                                                if (ch->getDevice() == "LOCAL")
                                                        shmInit(*ch);
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
MonitorScope::reinitFft(int newPoints)
{
        fftPoints_ = newPoints;
        fft_destroy(&fft_);

        fftInBuf_.assign(fftPoints_, 0.0f);
        fftMagBuf_.assign(fftPoints_ / 2 + 1, 0.0f);
        fftOutBuf_.assign((fftPoints_ / 2 + 1) * 2, 0.0f);
        fftLoBuf_.assign(fftPoints_, 0.0f);

        fft_cfg_t cfg;
        cfg.npoints  = fftPoints_;
        cfg.fs       = (parent_) ? (f32)parent_->getHz() : 1000.0f;
        cfg.e_window = FFT_WINDOW_HANNING;
        cfg.in_buf   = fftInBuf_.data();
        cfg.mag_buf  = fftMagBuf_.data();
        cfg.out_buf  = (decltype(cfg.out_buf))fftOutBuf_.data();
        cfg.buf      = fftLoBuf_.data();
        fft_init(&fft_, cfg);

        for (auto &pair : chs_) {
                pair.second->minKeepPoints_ = static_cast<size_t>(newPoints);
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
        scopes_[scopeName] = std::make_unique<MonitorScope>(scopeName);
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
                ImGui::SetNextItemWidth(100);
                float h = historySeconds_;
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
                int maxPts = static_cast<int>(maxDisplayPoints_);
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
                ImGui::SameLine();
                ImGui::TextDisabled("MaxHz");

                ImGui::SameLine();
                if (actualHz_ > 0.1f) {
                        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%.0f Hz", actualHz_);
                } else {
                        ImGui::TextDisabled("-- Hz");
                }

                // Right-aligned mode control group
                float spacing    = ImGui::GetStyle().ItemSpacing.x;
                float totalWidth = 70 + spacing + 90 + spacing + ImGui::CalcTextSize("T-Mode").x +
                                   spacing + 90 + spacing + ImGui::CalcTextSize("F-Mode").x;
                
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - totalWidth);

                const char *sampModes[] = {"HSS", "POLL"};
                int         curSamp     = (samplingMode_ == SamplingMode::HSS) ? 0 : 1;
                ImGui::SetNextItemWidth(70);
                if (ImGui::Combo("##SampMode", &curSamp, sampModes, 2)) {
                        samplingMode_ = (curSamp == 0) ? SamplingMode::HSS : SamplingMode::POLL;
                        clearData();
                        JLinkDev::instance().reqRestart();
                }
                ImGui::SameLine();

                const char *viewModeNames[] = {"FULL", "FOLLOW", "MANUAL"};
                int         curViewMode     = (int)viewMode_;
                int         curFftViewMode  = (int)fftViewMode_;

                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("##TimeMode", &curViewMode, viewModeNames, 3)) {
                        viewMode_ = static_cast<MonitorViewMode>(curViewMode);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time Domain View Mode");
                ImGui::SameLine();
                ImGui::TextDisabled("T-Mode");

                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("##FftMode", &curFftViewMode, viewModeNames, 3)) {
                        fftViewMode_ = static_cast<MonitorViewMode>(curFftViewMode);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frequency Domain (FFT) View Mode");
                ImGui::SameLine();
                ImGui::TextDisabled("F-Mode");

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
                        float totalSplitterHeight = static_cast<float>(keys.size() - 1) * 8.0f;
                        float equalHeight         = (avail.y - totalSplitterHeight) / static_cast<float>(keys.size());
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
                                double now      = sessionTimeSec();

                                // When FULL mode is active, always fit to exact data bounds
                                if (viewMode_ == MonitorViewMode::FULL) {
                                        if (!isPaused) {
                                                double earliest = now;
                                                double latest   = 0.0;
                                                bool   hasData  = false;
                                                for (const auto &[_, sc] : scopes_) {
                                                        for (const auto &[__, ch] : sc->getChannels()) {
                                                                const f32 e = ch->earliestTs();
                                                                const f32 l = ch->latestTs();
                                                                if (e >= 0.0f) {
                                                                        hasData = true;
                                                                        if (e < earliest)
                                                                                earliest = static_cast<double>(e);
                                                                        if (l >= 0.0f && l > latest)
                                                                                latest = static_cast<double>(l);
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
                                                double span        = linkXMax_ - linkXMin_;
                                                double currentSpan = (span > 0.001) ? span : 1.0;
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

                        ImGui::PushID(static_cast<int>(i));
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
