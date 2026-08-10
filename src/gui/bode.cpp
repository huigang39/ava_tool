#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "imgui.h"
#include "implot.h"

#include "fft.h"
#include "gui/bode.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/ui_theme.hpp"

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

Monitor *
Bode::findMonitorByKey_(const char *key)
{
        // key format: "monitorName/scopeName/channelName" — only the monitor part matters.
        const char *sep1 = std::strchr(key, '/');
        if (!sep1)
                return nullptr;
        std::string     monName(key, sep1);
        std::lock_guard lk(Monitor::sMtxInstances_);
        for (auto *m : Monitor::sInstances_)
                if (m->getName() == monName)
                        return m;
        return nullptr;
}

// Offline transfer-function estimate from the data already displayed in the
// plots. Computes the FFT of the Input and Output channels over their visible
// (linked-X) window, then H = Y/X per FFT bin. Bins where the input carries
// negligible energy are dropped so swept/chirp excitations yield a clean curve.
void
Bode::computeFromData_()
{
        bodeOfflineStatus_.clear();

        MonitorChannel *inCh  = findChannelByKey_(bodeInputKey_);
        MonitorChannel *outCh = findChannelByKey_(bodeOutputKey_);
        if (!inCh || !outCh) {
                bodeOfflineStatus_ = "Select Input and Output channels.";
                return;
        }

        // Visible window = intersection of the owning monitors' linked-X ranges.
        Monitor  *inMon   = findMonitorByKey_(bodeInputKey_);
        Monitor  *outMon  = findMonitorByKey_(bodeOutputKey_);
        const f64 NEG_INF = -std::numeric_limits<f64>::infinity();
        const f64 POS_INF = std::numeric_limits<f64>::infinity();
        f64       tMin    = NEG_INF;
        f64       tMax    = POS_INF;
        if (inMon) {
                tMin = std::max(tMin, inMon->linkXMin_);
                tMax = std::min(tMax, inMon->linkXMax_);
        }
        if (outMon) {
                tMin = std::max(tMin, outMon->linkXMin_);
                tMax = std::min(tMax, outMon->linkXMax_);
        }
        if (!(tMax > tMin)) {
                bodeOfflineStatus_ = "No overlapping visible window.";
                return;
        }

        // Gather the visible samples of a channel; returns its effective sample rate.
        auto gather = [&](MonitorChannel *ch, std::vector<f32> &out) -> f32 {
                const auto &rd = ch->read_;
                const usize si = rd.rawLowerBound(tMin);
                const usize ei = rd.rawUpperBound(tMax);
                if (ei <= si) {
                        out.clear();
                        return 0.0f;
                }
                const usize n = ei - si;
                out.resize(n);
                for (usize i = 0; i < n; ++i)
                        out[i] = rd.rawVal(si + i);
                f32 fs = 1000.0f;
                if (n > 1) {
                        const f64 tt = rd.rawTs(si + n - 1) - rd.rawTs(si);
                        if (tt > 1e-9)
                                fs = static_cast<f32>(static_cast<f64>(n - 1) / tt);
                }
                return fs;
        };

        std::vector<f32> inSmp, outSmp;
        const f32        inFs = gather(inCh, inSmp);
        gather(outCh, outSmp);
        if (inSmp.empty() || outSmp.empty()) {
                bodeOfflineStatus_ = "No data in the visible window.";
                return;
        }

        // Largest power of two that fits both channels (FFT-bin resolution), capped.
        const usize avail = std::min(inSmp.size(), outSmp.size());
        int         N     = 1;
        while (static_cast<usize>(N << 1) <= avail && (N << 1) <= 16384)
                N <<= 1;
        if (N < 32) {
                bodeOfflineStatus_ = "Not enough points in window (need >= 32).";
                return;
        }

        // Keep the most recent N samples of each channel and remove the DC mean.
        auto prep = [&](std::vector<f32> &smp) {
                const usize off = smp.size() - static_cast<usize>(N);
                f64         sum = 0.0;
                for (int i = 0; i < N; ++i)
                        sum += smp[off + i];
                const f32        mean = static_cast<f32>(sum / static_cast<f64>(N));
                std::vector<f32> w(N);
                for (int i = 0; i < N; ++i)
                        w[i] = smp[off + i] - mean;
                return w;
        };
        std::vector<f32> inW  = prep(inSmp);
        std::vector<f32> outW = prep(outSmp);

        // Set up a one-shot real FFT (FFTW_ESTIMATE plan — render-thread only, no
        // contention with the background FFT worker which only ever fft_exec()s).
        const int        half = N / 2 + 1;
        std::vector<f32> inBuf(N, 0.0f), loBuf(N, 0.0f), magBuf(half, 0.0f);
        std::vector<f32> cplxBuf(static_cast<usize>(half) * 2, 0.0f); // interleaved re/im

        fft_t     fft{};
        fft_cfg_t cfg{};
        cfg.npoints  = static_cast<usize>(N);
        cfg.fs       = inFs;
        cfg.e_window = FFT_WINDOW_HANNING; // same window on both → cancels in the ratio
        cfg.in_buf   = inBuf.data();
        cfg.mag_buf  = magBuf.data();
        cfg.out_buf  = reinterpret_cast<decltype(cfg.out_buf)>(cplxBuf.data());
        cfg.buf      = loBuf.data();
        fft_init(&fft, cfg);

        auto runFft = [&](const std::vector<f32> &smp, std::vector<f64> &re, std::vector<f64> &im) {
                std::copy(smp.begin(), smp.end(), loBuf.begin());
                fft.cfg.fs       = inFs;
                fft.lo.need_exec = 1;
                fft_exec(&fft);
                re.resize(half);
                im.resize(half);
                for (int i = 0; i < half; ++i) {
                        re[i] = static_cast<f64>(cplxBuf[2 * i]);
                        im[i] = static_cast<f64>(cplxBuf[2 * i + 1]);
                }
        };

        std::vector<f64> inRe, inIm, outRe, outIm;
        runFft(inW, inRe, inIm);
        runFft(outW, outRe, outIm);
        fft_destroy(&fft);

        // Peak input bin energy (skip DC) → threshold for "excited" bins.
        f64 peakInMag = 0.0;
        for (int i = 1; i < half; ++i)
                peakInMag = std::max(peakInMag, std::hypot(inRe[i], inIm[i]));
        const f64 thresh = peakInMag * (static_cast<f64>(bodeOfflineThreshPct_) / 100.0);

        const f64 df = static_cast<f64>(inFs) / static_cast<f64>(N);
        bodeData_.clear();
        bodeFreqsV_.clear();
        bodeMagsV_.clear();
        bodePhsV_.clear();

        int kept = 0;
        for (int i = 1; i < half; ++i) {
                const f64 xr = inRe[i], xi = inIm[i];
                const f64 denom = xr * xr + xi * xi;
                if (denom <= 0.0 || std::sqrt(denom) < thresh)
                        continue; // no excitation here → ratio would be noise
                const f64 yr = outRe[i], yi = outIm[i];
                // H = Y / X = Y * conj(X) / |X|^2
                const f64 hr    = (yr * xr + yi * xi) / denom;
                const f64 hi    = (yi * xr - yr * xi) / denom;
                const f64 mag   = std::hypot(hr, hi);
                const f64 magDb = (mag > 1e-12) ? 20.0 * std::log10(mag) : -240.0;
                const f64 phDeg = std::atan2(hi, hr) * (180.0 / M_PI);
                const f64 freq  = static_cast<f64>(i) * df;

                bodeData_.push_back(BodePoint{freq, magDb, phDeg});
                bodeFreqsV_.push_back(freq);
                bodeMagsV_.push_back(magDb);
                bodePhsV_.push_back(phDeg);
                ++kept;
        }

        if (kept == 0) {
                bodeOfflineStatus_ = "No excited bins (lower Thresh%% or check Input).";
        } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "N=%d  fs=%.1f Hz  df=%.3g Hz  %d bins", N, inFs, df, kept);
                bodeOfflineStatus_ = buf;
        }
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
                int curIdx = -1;
                for (int i = 0; i < (int)keys.size(); ++i)
                        if (std::strcmp(buf, keys[i].c_str()) == 0) {
                                curIdx = i;
                                break;
                        }
                std::string previewStr = (curIdx >= 0) ? varName(keys[curIdx]) : tr("(none)", "（无）");
                ImGui::SetNextItemWidth(150);
                bool comboOpen = ImGui::BeginCombo(id, previewStr.c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", labelText);
                if (comboOpen) {
                        for (int i = 0; i < (int)keys.size(); ++i) {
                                bool        sel  = (i == curIdx);
                                std::string disp = varName(keys[i]);
                                ImGui::PushID(i);
                                if (ImGui::Selectable(disp.c_str(), sel, 0)) {
                                        std::snprintf(buf, bufSz, "%s", keys[i].c_str());
                                        modified_ = true;
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", keys[i].c_str());
                                if (sel)
                                        ImGui::SetItemDefaultFocus();
                                ImGui::PopID();
                        }
                        ImGui::EndCombo();
                }
        };

        // ---------- Bode subplots (shared by both modes) ----------
        auto drawBodeSubplots = [&]() {
                if (!ImPlot::BeginSubplots(
                        "##bodeSP", 2, 1, ImVec2(-1.0f, -1.0f), ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle))
                        return;
                auto legendPopup = [this](const char *label, BodeCurveStyle &style) {
                        if (ImPlot::BeginLegendPopup(label)) {
                                f32 colArr[4];
                                memcpy(colArr, style.color, sizeof(colArr));
                                if (ImGui::ColorEdit4(tr("Color", "颜色"), colArr, ImGuiColorEditFlags_NoInputs)) {
                                        style.useAutoColor = false;
                                        memcpy(style.color, colArr, sizeof(colArr));
                                        modified_ = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox(tr("Auto", "自动"), &style.useAutoColor)) {
                                        modified_ = true;
                                        if (style.useAutoColor) {
                                                static i32 bodeShuffleIdx = 0;
                                                bodeShuffleIdx            = (bodeShuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 nc                 = ImPlot::GetColormapColor(bodeShuffleIdx);
                                                memcpy(style.color, &nc.x, sizeof(f32) * 4);
                                        }
                                }
                                ImGui::SetNextItemWidth(100);
                                if (ImGui::SliderFloat("##LineWidth", &style.lineWeight, 0.5f, 5.0f, "%.1f"))
                                        modified_ = true;
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Width", "线宽"));
                                if (ImGui::Checkbox(tr("Markers", "标记点"), &style.showMarkers))
                                        modified_ = true;
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
        };

        // ---------- Mode selection ----------
        if (bodeSweepRunning_)
                ImGui::BeginDisabled();
        const char *modeNames[] = {tr("Sweep", "扫频"), tr("From Data", "来自数据")};
        int         modeIdx     = static_cast<int>(bodeMode_);
        ImGui::SetNextItemWidth(100);
        if (ImGui::Combo("##bodeMode", &modeIdx, modeNames, IM_ARRAYSIZE(modeNames))) {
                bodeMode_ = static_cast<Mode>(modeIdx);
                modified_ = true;
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Sweep: drive a live swept-sine\nFrom Data: offline FFT of the displayed data",
                                     "扫频：驱动实时扫频正弦信号\n来自数据：对已显示数据做离线 FFT"));
        if (bodeSweepRunning_)
                ImGui::EndDisabled();
        ImGui::SameLine();

        if (bodeMode_ == Mode::FromData) {
                drawCombo("##bodeIn", tr("Input", "输入"), bodeInputKey_, sizeof(bodeInputKey_));
                ImGui::SameLine();
                drawCombo("##bodeOut", tr("Output", "输出"), bodeOutputKey_, sizeof(bodeOutputKey_));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                if (ImGui::InputFloat("##bodeThr", &bodeOfflineThreshPct_, 0, 0, "%.2f"))
                        modified_ = true;
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Thresh%: keep only FFT bins where |Input| >= peak * this%",
                                             "阈值%：仅保留 |输入| >= 峰值 * 此百分比 的 FFT 频点"));
                if (bodeOfflineThreshPct_ < 0.0f)
                        bodeOfflineThreshPct_ = 0.0f;
                if (bodeOfflineThreshPct_ > 100.0f)
                        bodeOfflineThreshPct_ = 100.0f;
                ImGui::SameLine();
                if (ui::Button(tr("Compute##bodeCmp", "计算##bodeCmp"), ui::BtnStyle::Success))
                        computeFromData_();
                ImGui::SameLine();
                if (ui::Button(tr("Clear##bodeClD", "清除##bodeClD"), ui::BtnStyle::Warning)) {
                        bodeData_.clear();
                        bodeFreqsV_.clear();
                        bodeMagsV_.clear();
                        bodePhsV_.clear();
                        bodeOfflineStatus_.clear();
                }
                if (!bodeOfflineStatus_.empty())
                        ImGui::TextDisabled("%s", bodeOfflineStatus_.c_str());
                ImGui::Separator();

                drawBodeSubplots();
                return;
        }

        drawCombo("##bodeWr", tr("Write", "写入"), bodeWriteKey_, sizeof(bodeWriteKey_));
        ImGui::SameLine();
        drawCombo("##bodeIn", tr("Input", "输入"), bodeInputKey_, sizeof(bodeInputKey_));
        ImGui::SameLine();
        drawCombo("##bodeOut", tr("Output", "输出"), bodeOutputKey_, sizeof(bodeOutputKey_));

        // ---------- Sweep parameters ----------
        if (bodeSweepRunning_)
                ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(80);
        if (ImGui::InputFloat("##bFS", &bodeFStart_, 0, 0, "%.3g"))
                modified_ = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("F Start(Hz)", "起始频率(Hz)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputFloat("##bFE", &bodeFStop_, 0, 0, "%.3g"))
                modified_ = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("F Stop(Hz)", "终止频率(Hz)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::InputFloat("##bSTP", &bodeFStep_, 0, 0, "%.3g"))
                modified_ = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Step(Hz)", "步进(Hz)"));
        if (bodeFStep_ <= 0.0f)
                bodeFStep_ = 1.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        if (ImGui::InputFloat("##bDw", &bodeDwellSec_, 0, 0, "%.2f"))
                modified_ = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Dwell(s)", "驻留(秒)"));
        if (bodeDwellSec_ < 0.05f)
                bodeDwellSec_ = 0.05f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::InputFloat("##bAmp", &bodeAmp_, 0, 0, "%.3g"))
                modified_ = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Amp", "幅值"));
        if (bodeAmp_ < 0.0f)
                bodeAmp_ = 0.0f;

        if (bodeSweepRunning_)
                ImGui::EndDisabled();

        // ---------- Control buttons ----------
        ImGui::SameLine();
        if (!bodeSweepRunning_) {
                if (ui::Button(tr("Start##bodeSt", "开始##bodeSt"), ui::BtnStyle::Success)) {
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
                if (ui::Button(tr("Stop##bodeSt", "停止##bodeSt"), ui::BtnStyle::Danger)) {
                        bodeSweepRunning_    = false;
                        MonitorChannel *wrCh = findChannelByKey_(bodeWriteKey_);
                        if (wrCh) {
                                wrCh->waveEnable_ = false;
                                wrCh->setWVal(0.0f);
                                wrCh->markWValDirty();
                        }
                }
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
        if (ui::Button(tr("Clear##bodeCl", "清除##bodeCl"), ui::BtnStyle::Warning)) {
                bodeData_.clear();
                bodeFreqsV_.clear();
                bodeMagsV_.clear();
                bodePhsV_.clear();
        }

        ImGui::Separator();

        drawBodeSubplots();
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
        if (ImGui::Begin(tr("Bode Plot###BodePlot", "伯德图###BodePlot"), &show_))
                draw_();
        ImGui::End();

        advanceSweep_();
}

void
Bode::saveSession(void *node) const
{
        cJSON *root = static_cast<cJSON *>(node);
        if (!cJSON_IsObject(root))
                return;

        cJSON *bode = cJSON_CreateObject();
        cJSON_AddStringToObject(bode, "writeKey", bodeWriteKey_);
        cJSON_AddStringToObject(bode, "inputKey", bodeInputKey_);
        cJSON_AddStringToObject(bode, "outputKey", bodeOutputKey_);
        cJSON_AddNumberToObject(bode, "fStart", bodeFStart_);
        cJSON_AddNumberToObject(bode, "fStop", bodeFStop_);
        cJSON_AddNumberToObject(bode, "fStep", bodeFStep_);
        cJSON_AddNumberToObject(bode, "dwellSec", bodeDwellSec_);
        cJSON_AddNumberToObject(bode, "amplitude", bodeAmp_);
        cJSON_AddNumberToObject(bode, "mode", static_cast<int>(bodeMode_));
        cJSON_AddNumberToObject(bode, "offlineThresholdPct", bodeOfflineThreshPct_);

        auto saveStyle = [](cJSON *parent, const char *name, const BodeCurveStyle &style) {
                cJSON *item  = cJSON_CreateObject();
                cJSON *color = cJSON_CreateArray();
                for (float component : style.color)
                        cJSON_AddItemToArray(color, cJSON_CreateNumber(component));
                cJSON_AddItemToObject(item, "color", color);
                cJSON_AddBoolToObject(item, "autoColor", style.useAutoColor);
                cJSON_AddNumberToObject(item, "lineWeight", style.lineWeight);
                cJSON_AddBoolToObject(item, "markers", style.showMarkers);
                cJSON_AddItemToObject(parent, name, item);
        };
        saveStyle(bode, "magnitudeStyle", bodeMagStyle_);
        saveStyle(bode, "phaseStyle", bodePhsStyle_);
        cJSON_AddItemToObject(root, "bode", bode);
}

void
Bode::loadSession(const void *node)
{
        const cJSON *root = static_cast<const cJSON *>(node);
        const cJSON *bode = cJSON_GetObjectItem(root, "bode");
        if (!cJSON_IsObject(bode)) {
                modified_ = false;
                return;
        }

        auto loadText = [bode](const char *name, char *dst, size_t size) {
                if (const cJSON *v = cJSON_GetObjectItem(bode, name); cJSON_IsString(v))
                        snprintf(dst, size, "%s", v->valuestring);
        };
        auto loadFloat = [bode](const char *name, float &dst) {
                if (const cJSON *v = cJSON_GetObjectItem(bode, name); cJSON_IsNumber(v))
                        dst = static_cast<float>(v->valuedouble);
        };
        loadText("writeKey", bodeWriteKey_, sizeof(bodeWriteKey_));
        loadText("inputKey", bodeInputKey_, sizeof(bodeInputKey_));
        loadText("outputKey", bodeOutputKey_, sizeof(bodeOutputKey_));
        loadFloat("fStart", bodeFStart_);
        loadFloat("fStop", bodeFStop_);
        loadFloat("fStep", bodeFStep_);
        loadFloat("dwellSec", bodeDwellSec_);
        loadFloat("amplitude", bodeAmp_);
        loadFloat("offlineThresholdPct", bodeOfflineThreshPct_);
        if (const cJSON *v = cJSON_GetObjectItem(bode, "mode"); cJSON_IsNumber(v))
                bodeMode_ = v->valueint == static_cast<int>(Mode::FromData) ? Mode::FromData : Mode::Sweep;

        auto loadStyle = [](const cJSON *parent, const char *name, BodeCurveStyle &style) {
                const cJSON *item = cJSON_GetObjectItem(parent, name);
                if (!cJSON_IsObject(item))
                        return false;
                if (const cJSON *color = cJSON_GetObjectItem(item, "color"); cJSON_IsArray(color)) {
                        for (int i = 0; i < 4; ++i)
                                if (const cJSON *v = cJSON_GetArrayItem(color, i); cJSON_IsNumber(v))
                                        style.color[i] = static_cast<float>(v->valuedouble);
                }
                if (const cJSON *v = cJSON_GetObjectItem(item, "autoColor"); cJSON_IsBool(v))
                        style.useAutoColor = cJSON_IsTrue(v);
                if (const cJSON *v = cJSON_GetObjectItem(item, "lineWeight"); cJSON_IsNumber(v))
                        style.lineWeight = std::clamp(static_cast<float>(v->valuedouble), 0.5f, 5.0f);
                if (const cJSON *v = cJSON_GetObjectItem(item, "markers"); cJSON_IsBool(v))
                        style.showMarkers = cJSON_IsTrue(v);
                return true;
        };
        const bool magLoaded = loadStyle(bode, "magnitudeStyle", bodeMagStyle_);
        const bool phsLoaded = loadStyle(bode, "phaseStyle", bodePhsStyle_);
        bodeStyleInit_       = magLoaded || phsLoaded;
        modified_            = false;
}
