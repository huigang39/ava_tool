#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "imgui.h"
#include "implot.h"

#include "gui/bode.hpp"
#include "gui/monitor.hpp"

void
Bode::generateBodeFreqs_()
{
        bodeFreqList_.clear();
        if (bodeFStart_ <= 0.0f || bodeFStop_ <= bodeFStart_ || bodeFStep_ <= 0.0f)
                return;
        for (float f = bodeFStart_; f <= bodeFStop_ + bodeFStep_ * 0.01f; f += bodeFStep_)
                bodeFreqList_.push_back(static_cast<double>(f));
}

MonitorChannel *
Bode::findChannelByKey_(const char *key)
{
        // key format: "monitorName/scopeName/channelName"
        const char *sep1 = std::strchr(key, '/');
        if (!sep1)
                return nullptr;
        const char *sep2 = std::strchr(sep1 + 1, '/');
        if (!sep2)
                return nullptr;
        std::string     monName(key, sep1);
        std::lock_guard lk(Monitor::sMtxInstances_);
        for (auto *m : Monitor::sInstances_)
                if (m->getName() == monName)
                        return m->findChannel(std::string(sep1 + 1, sep2), std::string(sep2 + 1));
        return nullptr;
}

void
Bode::draw_()
{
        if (!bodeStyleInit_) {
                ImVec4 c0 = ImPlot::GetColormapColor(0);
                ImVec4 c1 = ImPlot::GetColormapColor(1);
                memcpy(bodeMagStyle_.color, &c0.x, sizeof(f32) * 4);
                memcpy(bodePhsStyle_.color, &c1.x, sizeof(f32) * 4);
                bodeStyleInit_ = true;
        }

        // ---------- Channel selection ----------
        // Collect all channels from every monitor instance.
        std::vector<std::string> keys;
        {
                std::lock_guard lk(Monitor::sMtxInstances_);
                for (auto *m : Monitor::sInstances_)
                        for (auto &[sn, sc] : m->getScopes())
                                for (auto &[cn, _] : sc->getChannels())
                                        keys.push_back(m->getName() + "/" + sn + "/" + cn);
        }
        std::sort(keys.begin(), keys.end());

        auto varName = [](const std::string &key) -> std::string {
                auto pos = key.rfind('/');
                return (pos != std::string::npos) ? key.substr(pos + 1) : key;
        };

        auto drawCombo = [&](const char *id, const char *labelText, char *buf, size_t bufSz) {
                ImGui::TextDisabled("%s", labelText);
                ImGui::SameLine();
                int curIdx = -1;
                for (int i = 0; i < (int)keys.size(); ++i)
                        if (std::strcmp(buf, keys[i].c_str()) == 0) {
                                curIdx = i;
                                break;
                        }
                std::string previewStr = (curIdx >= 0) ? varName(keys[curIdx]) : "(none)";
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo(id, previewStr.c_str())) {
                        for (int i = 0; i < (int)keys.size(); ++i) {
                                bool        sel  = (i == curIdx);
                                std::string disp = varName(keys[i]);
                                ImGui::PushID(i);
                                if (ImGui::Selectable(disp.c_str(), sel, 0))
                                        std::snprintf(buf, bufSz, "%s", keys[i].c_str());
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", keys[i].c_str());
                                if (sel)
                                        ImGui::SetItemDefaultFocus();
                                ImGui::PopID();
                        }
                        ImGui::EndCombo();
                }
        };

        drawCombo("##bodeWr", "Write", bodeWriteKey_, sizeof(bodeWriteKey_));
        ImGui::SameLine();
        drawCombo("##bodeIn", "Input", bodeInputKey_, sizeof(bodeInputKey_));
        ImGui::SameLine();
        drawCombo("##bodeOut", "Output", bodeOutputKey_, sizeof(bodeOutputKey_));

        // ---------- Sweep parameters ----------
        if (bodeSweepRunning_)
                ImGui::BeginDisabled();

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
        if (bodeFStep_ <= 0.0f)
                bodeFStep_ = 1.0f;
        ImGui::SameLine();
        ImGui::TextDisabled("Dwell(s)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputFloat("##bDw", &bodeDwellSec_, 0, 0, "%.2f");
        if (bodeDwellSec_ < 0.05f)
                bodeDwellSec_ = 0.05f;
        ImGui::SameLine();
        ImGui::TextDisabled("Amp");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputFloat("##bAmp", &bodeAmp_, 0, 0, "%.3g");
        if (bodeAmp_ < 0.0f)
                bodeAmp_ = 0.0f;

        if (bodeSweepRunning_)
                ImGui::EndDisabled();

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
                                bodeSweepFreqIdx_    = 0;
                                bodeSweepRunning_    = true;
                                bodeSweepStepStart_  = get_mono_ts_us();
                                MonitorChannel *wrCh = findChannelByKey_(bodeWriteKey_);
                                if (wrCh) {
                                        wrCh->waveCfgPending_.freq.store(static_cast<float>(bodeFreqList_[0]));
                                        wrCh->waveCfgPending_.amp.store(bodeAmp_);
                                        wrCh->waveCfgPending_.dirty.store(true);
                                        wrCh->waveEnable_ = true;
                                }
                        }
                }
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
                if (ImGui::Button("Stop##bodeSt")) {
                        bodeSweepRunning_    = false;
                        MonitorChannel *wrCh = findChannelByKey_(bodeWriteKey_);
                        if (wrCh) {
                                wrCh->waveEnable_ = false;
                                wrCh->setWVal(0.0f);
                                wrCh->markWValDirty();
                        }
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                const int   total = static_cast<int>(bodeFreqList_.size());
                const float pct   = (total > 0) ? static_cast<float>(bodeSweepFreqIdx_) / total : 0.0f;
                ImGui::ProgressBar(pct, ImVec2(100.0f, 0.0f));
                ImGui::SameLine();
                const double curF =
                    (bodeSweepFreqIdx_ < total) ? bodeFreqList_[bodeSweepFreqIdx_] : static_cast<double>(bodeFStop_);
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
        if (ImPlot::BeginSubplots(
                "##bodeSP", 2, 1, ImVec2(-1.0f, -1.0f), ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle)) {
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
                                                bodeShuffleIdx            = (bodeShuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 nc                 = ImPlot::GetColormapColor(bodeShuffleIdx);
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
                            ImVec4(
                                bodeMagStyle_.color[0], bodeMagStyle_.color[1], bodeMagStyle_.color[2], bodeMagStyle_.color[3]),
                            bodeMagStyle_.lineWeight);
                        if (bodeMagStyle_.showMarkers)
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);
                        if (!bodeFreqsV_.empty())
                                ImPlot::PlotLine(
                                    "H(jw)##mag", bodeFreqsV_.data(), bodeMagsV_.data(), static_cast<int>(bodeFreqsV_.size()));
                        legendPopup("H(jw)##mag", bodeMagStyle_);
                        ImPlot::EndPlot();
                }
                if (ImPlot::BeginPlot("##bodePhs")) {
                        ImPlot::SetupAxis(ImAxis_X1, "Frequency (Hz)");
                        ImPlot::SetupAxis(ImAxis_Y1, "Phase (deg)");
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                        ImPlot::SetNextLineStyle(
                            ImVec4(
                                bodePhsStyle_.color[0], bodePhsStyle_.color[1], bodePhsStyle_.color[2], bodePhsStyle_.color[3]),
                            bodePhsStyle_.lineWeight);
                        if (bodePhsStyle_.showMarkers)
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);
                        if (!bodeFreqsV_.empty())
                                ImPlot::PlotLine(
                                    "H(jw)##phs", bodeFreqsV_.data(), bodePhsV_.data(), static_cast<int>(bodeFreqsV_.size()));
                        legendPopup("H(jw)##phs", bodePhsStyle_);
                        ImPlot::EndPlot();
                }
                ImPlot::EndSubplots();
        }
}

void
Bode::advanceSweep_()
{
        if (!bodeSweepRunning_ || bodeFreqList_.empty() || bodeSweepFreqIdx_ >= (int)bodeFreqList_.size())
                return;

        const double    f     = bodeFreqList_[bodeSweepFreqIdx_];
        MonitorChannel *inCh  = findChannelByKey_(bodeInputKey_);
        MonitorChannel *outCh = findChannelByKey_(bodeOutputKey_);

        if (inCh && outCh) {
                // Use a sliding window of the last bodeDwellSec_ seconds.
                f64 dwellXMax = sessionTimeSec();
                f64 dwellXMin = dwellXMax - static_cast<f64>(bodeDwellSec_);
                if (dwellXMin < 0.0)
                        dwellXMin = 0.0;

                auto [reX, imX]    = inCh->dftSlice(dwellXMin, dwellXMax, f);
                auto [reY, imY]    = outCh->dftSlice(dwellXMin, dwellXMax, f);
                const double magX  = std::hypot(reX, imX);
                const double magY  = std::hypot(reY, imY);
                const double magDb = (magX > 1e-10) ? 20.0 * std::log10(magY / magX) : -120.0;
                double       phRad = std::atan2(imY, reY) - std::atan2(imX, reX);
                phRad              = std::remainder(phRad, 2.0 * M_PI);

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

        const u64 now = get_mono_ts_us();
        if (now - bodeSweepStepStart_ >= static_cast<u64>(bodeDwellSec_ * 1.0e6f)) {
                ++bodeSweepFreqIdx_;
                if (bodeSweepFreqIdx_ >= (int)bodeFreqList_.size()) {
                        bodeSweepRunning_        = false;
                        MonitorChannel *wrChStop = findChannelByKey_(bodeWriteKey_);
                        if (wrChStop) {
                                wrChStop->waveEnable_ = false;
                                wrChStop->setWVal(0.0f);
                                wrChStop->markWValDirty();
                        }
                } else {
                        MonitorChannel *wrCh2 = findChannelByKey_(bodeWriteKey_);
                        if (wrCh2) {
                                wrCh2->waveCfgPending_.freq.store(static_cast<float>(bodeFreqList_[bodeSweepFreqIdx_]));
                                wrCh2->waveCfgPending_.dirty.store(true);
                        }
                        bodeSweepStepStart_ = now;
                }
        }
}

void
Bode::updateDisplay()
{
        if (!show_)
                return;

        ImGui::SetNextWindowSize(ImVec2(700.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Bode Plot", &show_))
                draw_();
        ImGui::End();

        advanceSweep_();
}
