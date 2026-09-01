#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
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
#include "implot_internal.h"

#include "core/dwarf_parser.hpp"
#include "core/jlink_port.hpp"
#include "fft.h"
#include "gui/bode.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/ui_theme.hpp"
#include "gui/variable.hpp"

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
        // key format: "monitorName/scopeName/chName"
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
        // key format: "monitorName/scopeName/chName" — only the monitor part matters
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

/**
 * @brief Offline transfer-function estimate from the data already displayed in the
 *        plots. Computes the FFT of the Input and Output chs over their visible
 *        (linked-X) window, then H = Y/X per FFT bin. Bins where the input carries
 *        negligible energy are dropped so swept/chirp excitations yield a clean curve
 */
void
Bode::computeFromData_()
{
        bodeOfflineStatus_.clear();

        MonitorChannel *inCh  = findChannelByKey_(bodeInputKey_);
        MonitorChannel *outCh = findChannelByKey_(bodeOutputKey_);
        if (!inCh || !outCh) {
                bodeOfflineStatus_ = "Select Input and Output chs.";
                return;
        }

        // Visible window = intersection of the owning monitors' linked-X ranges
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

        // Gather the visible samples of a ch; returns its effective sample rate
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

        // Largest power of two that fits both chs (FFT-bin resolution), capped
        const usize avail = std::min(inSmp.size(), outSmp.size());
        int         N     = 1;
        while (static_cast<usize>(N << 1) <= avail && (N << 1) <= 16384)
                N <<= 1;
        if (N < 32) {
                bodeOfflineStatus_ = "Not enough points in window (need >= 32).";
                return;
        }

        // Keep the most recent N samples of each ch and remove the DC mean
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
        // contention with the background FFT worker which only ever fft_exec()s)
        const int        half = N / 2 + 1;
        std::vector<f32> inBuf(N, 0.0f), loBuf(N, 0.0f), magBuf(half, 0.0f);
        std::vector<f32> cplxBuf(static_cast<usize>(half) * 2, 0.0f); // interleaved re/im

        fft_t     fft{};
        fft_cfg_t cfg{};
        cfg.point_cnt     = static_cast<usize>(N);
        cfg.fs            = inFs;
        cfg.window        = FFT_WINDOW_HANNING; // same window on both → cancels in the ratio
        cfg.in_buf        = inBuf.data();
        cfg.magnitude_buf = magBuf.data();
        cfg.out_buf       = reinterpret_cast<decltype(cfg.out_buf)>(cplxBuf.data());
        cfg.buf           = loBuf.data();
        fft_init(&fft, cfg);

        auto runFft = [&](const std::vector<f32> &smp, std::vector<f64> &re, std::vector<f64> &im) {
                std::copy(smp.begin(), smp.end(), loBuf.begin());
                fft.cfg.fs          = inFs;
                fft.lo.exec_pending = 1;
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

        // Peak input bin energy (skip DC) → threshold for "excited" bins
        f64 peakInMag = 0.0;
        for (int i = 1; i < half; ++i)
                peakInMag = std::max(peakInMag, std::hypot(inRe[i], inIm[i]));
        const f64 thresh = peakInMag * (static_cast<f64>(bodeOfflineThreshPct_) / 100.0);

        const f64 df = static_cast<f64>(inFs) / static_cast<f64>(N);
        bodeData_.clear();
        bodeAnalysisDirty_ = true;
        bodeMeasureCount_  = 0;
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

namespace
{
/**
 * @brief Keep logarithmic grid lines, but label intermediate frequencies when they fit
 */
void
bodeFrequencyTicks(
    ImPlotTicker &ticker, const ImPlotRange &range, float pixels, bool vertical, ImPlotFormatter formatter, void *data)
{
        ImPlot::Locator_Log10(ticker, range, pixels, vertical, formatter, data);
        if (vertical || range.Min <= 0 || range.Max <= range.Min)
                return;
        const double scale        = pixels / (std::log10(range.Max) - std::log10(range.Min));
        const int    priorities[] = {2, 5, 3, 4, 6, 7, 8, 9};
        for (int multiplier : priorities) {
                for (auto &tick : ticker.Ticks) {
                        if (tick.Major || tick.ShowLabel || tick.PlotPos <= 0)
                                continue;
                        const double decade = std::pow(10.0, std::floor(std::log10(tick.PlotPos)));
                        if (std::abs(tick.PlotPos / decade - multiplier) > 1e-6)
                                continue;
                        char label[IMPLOT_LABEL_MAX_SIZE];
                        formatter(tick.PlotPos, label, sizeof(label), data);
                        const auto size = ImGui::CalcTextSize(label);
                        bool       fits = true;
                        for (const auto &other : ticker.Ticks) {
                                if (other.ShowLabel && std::abs(std::log10(tick.PlotPos) - std::log10(other.PlotPos)) * scale <
                                                           (size.x + other.LabelSize.x) * 0.5f + ImGui::GetFontSize() * 0.75f) {
                                        fits = false;
                                        break;
                                }
                        }
                        if (fits) {
                                tick.ShowLabel  = true;
                                tick.TextOffset = ticker.TextBuffer.size();
                                tick.LabelSize  = size;
                                ticker.TextBuffer.append(label, label + std::strlen(label) + 1);
                                ticker.MaxSize.x = std::max(ticker.MaxSize.x, size.x);
                                ticker.MaxSize.y = std::max(ticker.MaxSize.y, size.y);
                        }
                }
        }
}

const dwarf::Type *
bodeResolveType(const dwarf::Info &info, u64 typeOff)
{
        for (int guard = 0; guard < 32; ++guard) {
                const auto it = info.types.find(typeOff);
                if (it == info.types.end())
                        return nullptr;
                const dwarf::Type &t = it->second;
                if (t.kind == dwarf::TypeKind::TYPEDEF || t.kind == dwarf::TypeKind::MODIFIER) {
                        if (t.inner == 0)
                                return &t;
                        typeOff = t.inner;
                        continue;
                }
                return &t;
        }
        return nullptr;
}

const dwarf::Type::Member *
bodeFindMember(const dwarf::Type *t, const std::string &name)
{
        if (!t || (t->kind != dwarf::TypeKind::STRUCT && t->kind != dwarf::TypeKind::UNION))
                return nullptr;
        for (const auto &m : t->members)
                if (m.name == name)
                        return &m;
        return nullptr;
}
} // namespace

void
Bode::readFromVariable_(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars)
{
        bodeVarStatus_.clear();

        struct Binding {
                const dwarf::Type *type{};
                u32                address{};
                const dwarf::Info *info{};
        };
        const auto resolve = [&](const char *input, Binding &binding) {
                std::string path  = input;
                const auto  begin = path.find_first_not_of(" \t");
                if (begin == std::string::npos) {
                        bodeVarStatus_ = tr("Fill in all four variable paths.", "请填写四个变量路径。");
                        return false;
                }
                path               = path.substr(begin, path.find_last_not_of(" \t") - begin + 1);
                const auto dot     = path.find('.');
                const auto global  = path.substr(0, dot);
                u64        address = 0;
                for (const auto &[_, variable] : vars) {
                        if (!variable || variable->getElfPath().empty())
                                continue;
                        const auto &info = variable->getDwarfInfo();
                        if (!info.present)
                                continue;
                        for (const auto &symbol : info.variables)
                                if (symbol.name == global) {
                                        binding.info = &info;
                                        binding.type = bodeResolveType(info, symbol.type);
                                        address      = symbol.addr;
                                        break;
                                }
                        if (binding.info)
                                break;
                }
                if (!binding.info) {
                        bodeVarStatus_ = tr("Symbol not found (load the ELF first): ", "找不到符号（请先加载 ELF）：") + global;
                        return false;
                }
                size_t position = dot;
                while (position != std::string::npos) {
                        const auto next = path.find('.', position + 1);
                        const auto memberName =
                            path.substr(position + 1, next == std::string::npos ? next : next - position - 1);
                        const auto *member = bodeFindMember(binding.type, memberName);
                        if (!member || member->bitSize || member->offset > UINT32_MAX ||
                            address > UINT32_MAX - member->offset) {
                                bodeVarStatus_ = tr("Invalid member path: ", "成员路径无效：") + path;
                                return false;
                        }
                        address      += member->offset;
                        binding.type  = bodeResolveType(*binding.info, member->type);
                        position      = next;
                }
                if (!binding.type || address > UINT32_MAX) {
                        bodeVarStatus_ = tr("Invalid variable type or address: ", "变量类型或地址无效：") + path;
                        return false;
                }
                binding.address = static_cast<u32>(address);
                return true;
        };
        Binding countBinding, frequencyBinding, gainBinding, phaseBinding;
        if (!resolve(bodeCountPath_, countBinding) || !resolve(bodeFreqPath_, frequencyBinding) ||
            !resolve(bodeGainPath_, gainBinding) || !resolve(bodePhasePath_, phaseBinding))
                return;
        const auto &countType   = *countBinding.type;
        const bool  signedCount = countType.encoding == 5 || countType.encoding == 6;
        if (countType.kind != dwarf::TypeKind::BASE || (!signedCount && countType.encoding != 7 && countType.encoding != 8) ||
            (countType.size != 1 && countType.size != 2 && countType.size != 4 && countType.size != 8)) {
                bodeVarStatus_ = tr("cnt must be an integer variable.", "cnt 必须是整数变量。");
                return;
        }
        const auto arrayCount = [](const Binding &binding) -> u64 {
                const auto &type = *binding.type;
                if (type.kind != dwarf::TypeKind::ARRAY || type.dims.size() != 1)
                        return 0;
                const auto *element = bodeResolveType(*binding.info, type.inner);
                return element && element->kind == dwarf::TypeKind::BASE && element->encoding == 4 && element->size == 4
                           ? type.dims[0]
                           : 0;
        };
        const u64 capacity = std::min({arrayCount(frequencyBinding), arrayCount(gainBinding), arrayCount(phaseBinding)});
        if (!capacity) {
                bodeVarStatus_ = tr("freq_hz/gain/phase_deg must be one-dimensional float32 arrays.",
                                    "freq_hz、gain、phase_deg 必须是一维 float32 数组。");
                return;
        }
        auto      &port  = JLinkPort::instance();
        const u64  epoch = port.targetEpoch();
        const auto read  = [&](const Binding &binding, u32 bytes, void *destination) {
                if (bytes && u64(binding.address) + bytes <= u64(UINT32_MAX) + 1 &&
                    port.readMemAtEpoch(epoch, binding.address, bytes, destination))
                        return true;
                char message[160];
                std::snprintf(message,
                              sizeof(message),
                              tr("J-Link read failed or target changed at 0x%08X", "J-Link 读取失败或目标已变化，地址：0x%08X"),
                              binding.address);
                bodeVarStatus_ = message;
                return false;
        };
        u64 count = 0;
        if (!read(countBinding, static_cast<u32>(countType.size), &count))
                return;
        if (signedCount && (count & (u64(1) << (countType.size * 8 - 1)))) {
                bodeVarStatus_ = tr("cnt cannot be negative.", "cnt 不能为负数。");
                return;
        }
        count = std::min(count, capacity);
        if (!count || count > 16384) {
                bodeVarStatus_ = tr("Valid point count must be between 1 and 16384.", "有效频点数必须在 1～16384 之间。");
                return;
        }
        std::vector<float> frequencies(count), gains(count), phases(count);
        const auto         bytes = static_cast<u32>(count * sizeof(float));
        if (!read(frequencyBinding, bytes, frequencies.data()) || !read(gainBinding, bytes, gains.data()) ||
            !read(phaseBinding, bytes, phases.data()))
                return;

        bodeFreqsV_.clear();
        bodeMagsV_.clear();
        bodePhsV_.clear();
        bodeData_.clear();
        bodeAnalysisDirty_ = true;
        bodeMeasureCount_  = 0;
        for (size_t i = 0; i < count; ++i) {
                const float freq = frequencies[i], gain = gains[i], phase = phases[i];
                if (!std::isfinite(freq) || freq <= 0.0f || !std::isfinite(gain) || !std::isfinite(phase))
                        continue;
                const double magDb = gain > 1e-12f ? 20.0 * std::log10(static_cast<double>(gain)) : -240.0;
                bodeFreqsV_.push_back(freq);
                bodeMagsV_.push_back(magDb);
                bodePhsV_.push_back(phase);
                bodeData_.push_back(BodePoint{freq, magDb, phase});
        }
        bodeFitPending_    = !bodeData_.empty();
        bodeAnalysisDirty_ = true;
        char status[128];
        std::snprintf(
            status, sizeof(status), "%s %u", tr("Read points:", "读取点数："), static_cast<unsigned>(bodeData_.size()));
        bodeVarStatus_ = status;
}

void
Bode::drawMeasurement_(bool magnitude)
{
        if (!bodeMeasureEnabled_)
                return;
        const auto nearest = [&](double frequency) -> const BodePoint * {
                if (!std::isfinite(frequency) || frequency <= 0)
                        return nullptr;
                const BodePoint *point    = nullptr;
                double           distance = std::numeric_limits<double>::infinity();
                for (const auto &candidate : bodeData_) {
                        if (!std::isfinite(candidate.freq) || candidate.freq <= 0 || !std::isfinite(candidate.magDb) ||
                            !std::isfinite(candidate.phaseDeg))
                                continue;
                        const double current = std::abs(std::log10(candidate.freq) - std::log10(frequency));
                        if (current < distance) {
                                distance = current;
                                point    = &candidate;
                        }
                }
                return point;
        };
        const auto mouse   = ImGui::GetMousePos();
        const bool hovered = ImPlot::IsPlotHovered() && !ImPlot::GetCurrentPlot()->Items.Legend.Rect.Contains(mouse);
        if (hovered && bodeMeasureCount_ < 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                if (const auto *point = nearest(ImPlot::GetPlotMousePos().x))
                        bodeMeasureFrequency_[bodeMeasureCount_++] = point->freq;
        const ImVec4     colors[] = {ImVec4(1, .75f, .2f, 1), ImVec4(.3f, .8f, 1, 1)};
        const char      *unit     = magnitude ? "dB" : "deg";
        const BodePoint *points[2]{};
        for (int i = 0; i < bodeMeasureCount_; ++i) {
                const auto flags = ImPlotDragToolFlags_NoFit | (bodeMeasureCount_ < 2 ? ImPlotDragToolFlags_NoInputs : 0);
                ImPlot::DragLineX(0x424F4400 + i, &bodeMeasureFrequency_[i], colors[i], 1, flags);
                points[i] = nearest(bodeMeasureFrequency_[i]);
                if (!points[i])
                        continue;
                bodeMeasureFrequency_[i] = points[i]->freq;
                if (!magnitude)
                        ImPlot::TagX(points[i]->freq, colors[i], "%c: %.6g Hz", 'A' + i, points[i]->freq);
                const double val = magnitude ? points[i]->magDb : points[i]->phaseDeg;
                ImPlot::PushPlotClipRect();
                ImPlot::GetPlotDrawList()->AddCircleFilled(
                    ImPlot::PlotToPixels(points[i]->freq, val), 4, ImGui::ColorConvertFloat4ToU32(colors[i]));
                ImPlot::PopPlotClipRect();
        }
        char text[384];
        if (points[0] && points[1]) {
                const double a = magnitude ? points[0]->magDb : points[0]->phaseDeg;
                const double b = magnitude ? points[1]->magDb : points[1]->phaseDeg;
                std::snprintf(text,
                              sizeof(text),
                              "A: %.7g Hz | %.7g %s\nB: %.7g Hz | %.7g %s\n|B-A|: %.7g Hz | B-A: %.7g %s",
                              points[0]->freq,
                              a,
                              unit,
                              points[1]->freq,
                              b,
                              unit,
                              std::abs(points[1]->freq - points[0]->freq),
                              b - a,
                              unit);
        } else if (points[0]) {
                std::snprintf(text,
                              sizeof(text),
                              "A: %.7g Hz | %.7g %s\n%s",
                              points[0]->freq,
                              magnitude ? points[0]->magDb : points[0]->phaseDeg,
                              unit,
                              tr("Click point B", "点击选择 B 点"));
        } else {
                std::snprintf(text, sizeof(text), "%s", tr("Click point A", "点击选择 A 点"));
        }
        const auto   pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize(), textSize = ImGui::CalcTextSize(text);
        const ImVec2 origin(pos.x + std::max(4.0f, size.x - textSize.x - 8), pos.y + 4);
        ImPlot::PushPlotClipRect();
        auto *draw = ImPlot::GetPlotDrawList();
        draw->AddRectFilled(ImVec2(origin.x - 3, origin.y - 2),
                            ImVec2(origin.x + textSize.x + 3, origin.y + textSize.y + 2),
                            ImGui::GetColorU32(ImGuiCol_PopupBg));
        draw->AddText(origin, ImGui::GetColorU32(ImGuiCol_Text), text);
        ImPlot::PopPlotClipRect();
}

void
Bode::drawAnalysis_()
{
        if (ImGui::Button(tr("Automatic analysis##bodeAnalysis", "自动分析##bodeAnalysis")))
                ImGui::OpenPopup("##bodeAnalysisPopup");
        ImGui::SetNextWindowSize(ImVec2(570, 0), ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("##bodeAnalysisPopup"))
                return;
        const char *modes[] = {tr("Open loop", "开环"), tr("Closed loop", "闭环")};
        ImGui::SetNextItemWidth(180);
        if (ui::Choice(tr("Analysis mode", "分析模式"), &bodeAnalysisMode_, modes, 2))
                modified_ = true;
        if (bodeAnalysisDirty_) {
                bodeAnalysis_      = bode_analysis::analyze(bodeData_);
                bodeAnalysisDirty_ = false;
        }
        const auto &result = bodeAnalysis_;
        if (result.points.empty()) {
                ImGui::TextUnformatted(tr("Load response points first.", "请先读取或生成响应数据。"));
                ImGui::EndPopup();
                return;
        }
        const auto number = [](double val, const char *unit) {
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "%.7g %s", val, unit);
                return std::string(buffer);
        };
        ImGui::Text(tr("%u points | %.7g to %.7g Hz", "%u 个点 | %.7g 至 %.7g Hz"),
                    static_cast<unsigned>(result.points.size()),
                    result.points.front().freq,
                    result.points.back().freq);
        ImGui::TextWrapped("%s",
                           tr("Estimates within the sampled range; crossings use logarithmic-frequency interpolation.",
                              "结果限于采样频段，交越频率使用对数频率插值估算。"));
        const char *missing  = tr("Not detected in sampled range", "采样范围内未检出");
        std::string report   = tr("Bode analysis: ", "伯德图分析：");
        report              += modes[bodeAnalysisMode_];
        report += "\n" + number(result.points.front().freq, "Hz") + " .. " + number(result.points.back().freq, "Hz") + "\n";
        if (ImGui::BeginTable("##bodeAnalysisMetrics", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn(tr("Metric", "指标"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(tr("Value", "结果"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(tr("Frequency", "对应频率"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                const auto row = [&](const char *label, const std::string &val, const std::string &frequency = "--") {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(label);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", val.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(frequency.c_str());
                        report += std::string(label) + ": " + val + " @ " + frequency + "\n";
                };
                if (bodeAnalysisMode_ == 0) {
                        const auto margin =
                            [&](const char *label, const std::vector<bode_analysis::Margin> &values, const char *unit) {
                                    if (values.empty()) {
                                            row(label, missing);
                                            return;
                                    }
                                    const auto critical =
                                        std::min_element(values.begin(), values.end(), [](const auto &a, const auto &b) {
                                                return std::abs(a.val) < std::abs(b.val);
                                        });
                                    row(label, number(critical->val, unit), number(critical->frequency, "Hz"));
                            };
                        margin(tr("Phase margin (PM)", "相位裕度 PM"), result.phaseMargins, "deg");
                        margin(tr("Gain margin (GM)", "增益裕度 GM"), result.gainMargins, "dB");
                } else {
                        row(tr("Low-frequency reference", "低频参考幅值"),
                            number(result.points.front().magDb, "dB"),
                            number(result.points.front().freq, "Hz"));
                        row(tr("-3 dB bandwidth", "-3 dB 带宽"), result.bandwidth ? number(*result.bandwidth, "Hz") : missing);
                        row(tr("Peak magnitude", "采样峰值"), number(result.peak.magDb, "dB"), number(result.peak.freq, "Hz"));
                        row(tr("Peak above reference", "相对参考峰值"),
                            number(result.peak.magDb - result.points.front().magDb, "dB"));
                }
                ImGui::EndTable();
        }
        if (bodeAnalysisMode_ == 0) {
                ImGui::TextWrapped("%s",
                                   tr("Use open-loop response with unit negative feedback. Multiple crossings: summary shows "
                                      "margins closest to zero.",
                                      "使用单位负反馈的开环响应；存在多个交越点时，摘要显示最接近零的裕度。"));
                if (ImGui::TreeNode(tr("All crossover points", "全部交越点"))) {
                        if (ImGui::BeginChild("##bodeCrossings", ImVec2(0, 150), ImGuiChildFlags_Borders)) {
                                for (const auto &margin : result.phaseMargins)
                                        ImGui::Text("PM: %.7g deg @ %.7g Hz", margin.val, margin.frequency);
                                for (const auto &margin : result.gainMargins)
                                        ImGui::Text("GM: %.7g dB @ %.7g Hz", margin.val, margin.frequency);
                        }
                        ImGui::EndChild();
                        ImGui::TreePop();
                }
        } else {
                ImGui::TextWrapped(
                    "%s",
                    tr("Bandwidth is relative to the first frequency point. The peak is the highest sampled magnitude.",
                       "带宽以首个频点幅值为参考；峰值取采样点中的最大幅值。"));
                if (result.peakAtBoundary)
                        ImGui::TextWrapped(
                            "%s",
                            tr("Peak is at the sampled boundary; a resonance peak inside the range was not confirmed.",
                               "峰值位于采样边界，未确认频段内存在谐振峰。"));
        }
        if (ImGui::Button(tr("Copy report", "复制分析报告"))) {
                if (bodeAnalysisMode_ == 0) {
                        for (const auto &margin : result.phaseMargins)
                                report += "PM: " + number(margin.val, "deg") + " @ " + number(margin.frequency, "Hz") + "\n";
                        for (const auto &margin : result.gainMargins)
                                report += "GM: " + number(margin.val, "dB") + " @ " + number(margin.frequency, "Hz") + "\n";
                }
                ImGui::SetClipboardText(report.c_str());
        }
        ImGui::EndPopup();
}

void
Bode::draw_(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars)
{
        if (!bodeStyleInit_) {
                bodeStyleInit_          = true;
                static i32 bodeColorIdx = 0;
                auto       assign       = [&](BodeCurveStyle &style) {
                        if (style.useAutoColor) {
                                ImVec4 c     = ImPlot::GetColormapColor(bodeColorIdx);
                                bodeColorIdx = (bodeColorIdx + 1) % ImPlot::GetColormapSize();
                                memcpy(style.color, &c.x, sizeof(f32) * 4);
                        }
                };
                assign(bodeMagStyle_);
                assign(bodePhsStyle_);
        }

        // ---------- Channel selection ----------
        // Collect all chs from every monitor instance
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
                ImGui::SetNextItemWidth(-1);
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

                const auto savedInput = ImPlot::GetInputMap();
                if (bodeMeasureEnabled_) {
                        ImPlot::GetInputMap().Pan = ImGuiMouseButton_Middle;
                        ImPlot::GetInputMap().Fit = ImGuiMouseButton_Middle;
                }
                int         drawnPlots = 0;
                ImPlotPlot *hoverPlots[2]{};
                ImDrawList *hoverDraws[2]{};
                double      hoverFrequency = 0;
                const auto  captureHover   = [&](int index) {
                        hoverPlots[index] = ImPlot::GetCurrentPlot();
                        hoverDraws[index] = ImPlot::GetPlotDrawList();
                        if (ImPlot::IsPlotHovered() && !hoverPlots[index]->Items.Legend.Rect.Contains(ImGui::GetMousePos()))
                                hoverFrequency = ImPlot::GetPlotMousePos().x;
                };
                if (bodeFitPending_)
                        ImPlot::SetNextAxesToFit();
                if (ImPlot::BeginPlot("##bodeMag")) {
                        ++drawnPlots;
                        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoTickLabels);
                        ImPlot::SetupAxis(ImAxis_Y1, tr("Magnitude (dB)", "幅值 (dB)"));
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                        ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].Locator = bodeFrequencyTicks;
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
                        drawMeasurement_(true);
                        captureHover(0);
                        ImPlot::EndPlot();
                }
                if (bodeFitPending_)
                        ImPlot::SetNextAxesToFit();
                if (ImPlot::BeginPlot("##bodePhs")) {
                        ++drawnPlots;
                        ImPlot::SetupAxis(ImAxis_X1, tr("Frequency (Hz)", "频率 (Hz)"));
                        ImPlot::SetupAxis(ImAxis_Y1, tr("Phase (deg)", "相位 (deg)"));
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                        ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].Locator = bodeFrequencyTicks;
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
                        drawMeasurement_(false);
                        captureHover(1);
                        ImPlot::EndPlot();
                }
                ImPlot::EndSubplots();
                // Defer until both plots have been visited so hovering either one
                // draws the same sample cursor in both plots in this frame
                const BodePoint *hoverPoint = nullptr;
                double           distance   = std::numeric_limits<double>::infinity();
                if (hoverFrequency > 0 && std::isfinite(hoverFrequency)) {
                        for (const auto &point : bodeData_) {
                                if (point.freq <= 0 || !std::isfinite(point.freq) || !std::isfinite(point.magDb) ||
                                    !std::isfinite(point.phaseDeg))
                                        continue;
                                const double d = std::abs(std::log10(point.freq) - std::log10(hoverFrequency));
                                if (d < distance) {
                                        distance   = d;
                                        hoverPoint = &point;
                                }
                        }
                }
                if (hoverPoint) {
                        for (int i = 0; i < 2; ++i) {
                                auto *plot = hoverPlots[i];
                                if (!plot || !plot->Axes[ImAxis_X1].Range.Contains(hoverPoint->freq))
                                        continue;
                                auto       *draw = hoverDraws[i];
                                const auto  rect = plot->PlotRect;
                                const float x    = plot->Axes[ImAxis_X1].PlotToPixels(hoverPoint->freq);
                                const float y =
                                    plot->Axes[ImAxis_Y1].PlotToPixels(i ? hoverPoint->phaseDeg : hoverPoint->magDb);
                                draw->PushClipRect(rect.Min, rect.Max, true);
                                draw->AddLine(
                                    ImVec2(x, rect.Min.y), ImVec2(x, rect.Max.y), ImGui::GetColorU32(ImGuiCol_TextDisabled));
                                draw->AddCircleFilled(
                                    ImVec2(x, y),
                                    4,
                                    ImGui::ColorConvertFloat4ToU32(i ? ImVec4(.3f, .8f, 1, 1) : ImVec4(1, .75f, .2f, 1)));
                                draw->PopClipRect();
                        }
                        ImGui::BeginTooltip();
                        ImGui::Text(tr("Frequency: %.7g Hz", "频率：%.7g Hz"), hoverPoint->freq);
                        ImGui::Text(tr("Magnitude: %.7g dB", "幅值：%.7g dB"), hoverPoint->magDb);
                        ImGui::Text(tr("Phase: %.7g deg", "相位：%.7g deg"), hoverPoint->phaseDeg);
                        ImGui::EndTooltip();
                }
                ImPlot::GetInputMap() = savedInput;
                if (drawnPlots == 2)
                        bodeFitPending_ = false;
        };

        const float toolbarRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const auto  nextControl  = [&](const char *label, bool checkbox = false) {
                const float width = ImGui::CalcTextSize(label, nullptr, true).x +
                                    (checkbox ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x
                                                : ImGui::GetStyle().FramePadding.x * 2);
                if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= toolbarRight)
                        ImGui::SameLine();
        };
        const auto clearData = [&] {
                bodeData_.clear();
                bodeFreqsV_.clear();
                bodeMagsV_.clear();
                bodePhsV_.clear();
                bodeAnalysisDirty_ = true;
                bodeMeasureCount_  = 0;
                bodeVarStatus_.clear();
                bodeOfflineStatus_.clear();
        };
        const char *modeNames[] = {tr("Sweep", "扫频"), tr("From Data", "来自数据"), tr("From Var", "从变量")};
        int         modeIndex   = static_cast<int>(bodeMode_);
        ImGui::BeginDisabled(bodeSweepRunning_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.5f);
        if (ui::Choice("##bodeMode", &modeIndex, modeNames, IM_ARRAYSIZE(modeNames))) {
                bodeMode_ = static_cast<Mode>(modeIndex);
                modified_ = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s",
                                  tr("Response source: live sweep, recorded data, or firmware variables",
                                     "响应数据来源：实时扫频、已采集数据或固件变量"));
        if (bodeMode_ == Mode::FromVariable) {
                nextControl(tr("Read", "读取"));
                if (ui::Button(tr("Read##bodeRd", "读取##bodeRd"), ui::BtnStyle::Success))
                        readFromVariable_(vars);
        } else if (bodeMode_ == Mode::FromData) {
                nextControl(tr("Compute", "计算"));
                if (ui::Button(tr("Compute##bodeCmp", "计算##bodeCmp"), ui::BtnStyle::Success))
                        computeFromData_();
        } else {
                nextControl(bodeSweepRunning_ ? tr("Stop", "停止") : tr("Start", "开始"));
                if (!bodeSweepRunning_) {
                        if (ui::Button(tr("Start##bodeSt", "开始##bodeSt"), ui::BtnStyle::Success)) {
                                generateBodeFreqs_();
                                if (!bodeFreqList_.empty()) {
                                        clearData();
                                        bodeSweepFreqIdx_   = 0;
                                        bodeSweepRunning_   = true;
                                        bodeSweepStepStart_ = get_mono_ts_us();
                                        if (auto *channel = findChannelByKey_(bodeWriteKey_)) {
                                                channel->waveCfgPending_.mode.store(static_cast<int>(WaveGeneratorMode::BASIC));
                                                channel->waveCfgPending_.freq.store(static_cast<float>(bodeFreqList_[0]));
                                                channel->waveCfgPending_.amp.store(bodeAmp_);
                                                channel->waveCfgPending_.dirty.store(true);
                                                channel->waveCfgPending_.restart.store(true);
                                                channel->waveEnable_ = true;
                                        }
                                }
                        }
                } else if (ui::Button(tr("Stop##bodeSt", "停止##bodeSt"), ui::BtnStyle::Danger)) {
                        bodeSweepRunning_ = false;
                        if (auto *channel = findChannelByKey_(bodeWriteKey_)) {
                                channel->waveEnable_ = false;
                                channel->setWVal(0.0f);
                                channel->markWValDirty();
                        }
                }
        }
        nextControl(tr("Clear", "清除"));
        ImGui::BeginDisabled(bodeSweepRunning_);
        const char *clearLabel = bodeMode_ == Mode::FromVariable ? tr("Clear##bodeClV", "清除##bodeClV")
                                 : bodeMode_ == Mode::FromData   ? tr("Clear##bodeClD", "清除##bodeClD")
                                                                 : tr("Clear##bodeCl", "清除##bodeCl");
        if (ui::Button(clearLabel, ui::BtnStyle::Warning))
                clearData();
        ImGui::EndDisabled();
        nextControl(tr("Show all", "显示全部"));
        ImGui::BeginDisabled(bodeData_.empty());
        if (ImGui::Button(tr("Show all##bodeFit", "显示全部##bodeFit")))
                bodeFitPending_ = true;
        ImGui::EndDisabled();
        nextControl(tr("Measure", "测量"), true);
        if (ImGui::Checkbox(tr("Measure##bodeMeasure", "测量##bodeMeasure"), &bodeMeasureEnabled_))
                bodeMeasureCount_ = 0;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Select A/B in either plot; drag cursors to move them. Middle-drag pans.",
                                     "在任一图中选择 A/B 点，游标吸附数据点；拖动游标调整，中键拖拽平移。"));
        if (bodeMeasureEnabled_ && bodeMeasureCount_ > 0) {
                nextControl(tr("Clear measurement", "清除测量"));
                if (ImGui::SmallButton(tr("Clear measurement##bodeClearMeasure", "清除测量##bodeClearMeasure")))
                        bodeMeasureCount_ = 0;
        }
        nextControl(tr("Automatic analysis", "自动分析"));
        drawAnalysis_();

        const bool  fromVariable    = bodeMode_ == Mode::FromVariable;
        const bool  fromData        = bodeMode_ == Mode::FromData;
        const bool  missingBindings = fromVariable
                                          ? (!bodeCountPath_[0] || !bodeFreqPath_[0] || !bodeGainPath_[0] || !bodePhasePath_[0])
                                          : (!bodeInputKey_[0] || !bodeOutputKey_[0] || (!fromData && !bodeWriteKey_[0]));
        std::string settingsLabel   = fromVariable ? tr("Variable mapping", "变量映射")
                                      : fromData   ? tr("Data settings", "数据设置")
                                                   : tr("Sweep settings", "扫频设置");
        if (!bodeData_.empty()) {
                char summary[96];
                std::snprintf(
                    summary, sizeof(summary), tr("  %u points", "  %u 个点"), static_cast<unsigned>(bodeData_.size()));
                settingsLabel += summary;
        }
        settingsLabel += fromVariable ? "###bodeVariableSettings" : fromData ? "###bodeDataSettings" : "###bodeSweepSettings";
        ImGui::SetNextItemOpen(missingBindings, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(settingsLabel.c_str())) {
                const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 48.0f;
                if (ImGui::BeginTable("##bodeSourceSettings",
                                      wide ? 4 : 2,
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
                        for (int i = 0; i < (wide ? 2 : 1); ++i) {
                                ImGui::TableSetupColumn(
                                    "##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
                                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                        }
                        const auto field = [&](const char *label, auto draw) {
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(label);
                                ImGui::TableNextColumn();
                                ImGui::SetNextItemWidth(-1);
                                draw();
                        };
                        const auto pathField = [&](const char *label, char *path, size_t size, const char *hint) {
                                field(label, [&] {
                                        ImGui::PushID(label);
                                        if (ImGui::InputTextWithHint("##path", hint, path, size))
                                                modified_ = true;
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", path[0] ? path : hint);
                                        ImGui::PopID();
                                });
                        };
                        if (fromVariable) {
                                pathField("cnt", bodeCountPath_, sizeof(bodeCountPath_), "g_foc.lo.ctrl.sweep.cnt");
                                pathField("freq_hz", bodeFreqPath_, sizeof(bodeFreqPath_), "g_foc.lo.ctrl.sweep.freq_hz");
                                pathField("gain", bodeGainPath_, sizeof(bodeGainPath_), "g_foc.lo.ctrl.sweep.gain");
                                pathField("phase_deg", bodePhasePath_, sizeof(bodePhasePath_), "g_foc.lo.ctrl.sweep.phase_deg");
                        } else {
                                ImGui::BeginDisabled(bodeSweepRunning_);
                                if (!fromData)
                                        field(tr("Write", "写入通道"), [&] {
                                                drawCombo(
                                                    "##bodeWr", tr("Write", "写入"), bodeWriteKey_, sizeof(bodeWriteKey_));
                                        });
                                field(tr("Input", "输入通道"), [&] {
                                        drawCombo("##bodeIn", tr("Input", "输入"), bodeInputKey_, sizeof(bodeInputKey_));
                                });
                                field(tr("Output", "输出通道"), [&] {
                                        drawCombo("##bodeOut", tr("Output", "输出"), bodeOutputKey_, sizeof(bodeOutputKey_));
                                });
                                const auto valueField = [&](const char *label, const char *id, float &val, const char *format) {
                                        field(label, [&] {
                                                if (ImGui::InputFloat(id, &val, 0, 0, format))
                                                        modified_ = true;
                                        });
                                };
                                if (fromData) {
                                        valueField(tr("Threshold %", "阈值 %"), "##bodeThr", bodeOfflineThreshPct_, "%.2f");
                                        bodeOfflineThreshPct_ = std::clamp(bodeOfflineThreshPct_, 0.0f, 100.0f);
                                } else {
                                        valueField(tr("Start Hz", "起始 Hz"), "##bFS", bodeFStart_, "%.3g");
                                        valueField(tr("Stop Hz", "截止 Hz"), "##bFE", bodeFStop_, "%.3g");
                                        valueField(tr("Step Hz", "步进 Hz"), "##bSTP", bodeFStep_, "%.3g");
                                        valueField(tr("Dwell s", "驻留 s"), "##bDw", bodeDwellSec_, "%.2f");
                                        valueField(tr("Amplitude", "幅值"), "##bAmp", bodeAmp_, "%.3g");
                                        if (bodeFStep_ <= 0)
                                                bodeFStep_ = 1;
                                        bodeDwellSec_ = std::max(0.05f, bodeDwellSec_);
                                        bodeAmp_      = std::max(0.0f, bodeAmp_);
                                }
                                ImGui::EndDisabled();
                        }
                        ImGui::EndTable();
                }
        }
        if (bodeSweepRunning_) {
                const int    total     = static_cast<int>(bodeFreqList_.size());
                const double frequency = bodeSweepFreqIdx_ < total ? bodeFreqList_[bodeSweepFreqIdx_] : bodeFStop_;
                char         progress[96];
                std::snprintf(progress, sizeof(progress), "%d / %d  %.7g Hz", bodeSweepFreqIdx_, total, frequency);
                ImGui::ProgressBar(total > 0 ? static_cast<float>(bodeSweepFreqIdx_) / total : 0, ImVec2(-1, 0), progress);
        }
        const auto &status = fromVariable ? bodeVarStatus_ : bodeOfflineStatus_;
        if ((fromVariable || fromData) && !status.empty()) {
                const auto   position = ImGui::GetCursorScreenPos();
                const ImVec2 end(position.x + ImGui::GetContentRegionAvail().x, position.y + ImGui::GetTextLineHeight());
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), position, end, end.x, status.c_str(), nullptr, nullptr);
                ImGui::Dummy(ImVec2(end.x - position.x, ImGui::GetTextLineHeight()));
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", status.c_str());
        }
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
                // Use a sliding window of the prev bodeDwellSec_ seconds
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
                bodeAnalysisDirty_             = true;
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
Bode::updateDisplay(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars)
{
        if (!show_)
                return;

        ImGui::SetNextWindowSize(ImVec2(920.0f, 680.0f), ImGuiCond_FirstUseEver);
        if (dock_close::begin(tr("Bode Plot###BodePlot", "伯德图###BodePlot"), &show_))
                draw_(vars);
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
        cJSON_AddNumberToObject(bode, "analysisMode", bodeAnalysisMode_);
        cJSON_AddStringToObject(bode, "varCountPath", bodeCountPath_);
        cJSON_AddStringToObject(bode, "varFreqPath", bodeFreqPath_);
        cJSON_AddStringToObject(bode, "varGainPath", bodeGainPath_);
        cJSON_AddStringToObject(bode, "varPhasePath", bodePhasePath_);

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
        // Migrate older sessions that selected one result structure
        if (const auto *legacy = cJSON_GetObjectItem(bode, "varPath"); cJSON_IsString(legacy)) {
                snprintf(bodeCountPath_, sizeof(bodeCountPath_), "%s.count", legacy->valuestring);
                snprintf(bodeFreqPath_, sizeof(bodeFreqPath_), "%s.freq_hz", legacy->valuestring);
                snprintf(bodeGainPath_, sizeof(bodeGainPath_), "%s.gain", legacy->valuestring);
                snprintf(bodePhasePath_, sizeof(bodePhasePath_), "%s.phase_deg", legacy->valuestring);
        }
        loadText("varCountPath", bodeCountPath_, sizeof(bodeCountPath_));
        loadText("varFreqPath", bodeFreqPath_, sizeof(bodeFreqPath_));
        loadText("varGainPath", bodeGainPath_, sizeof(bodeGainPath_));
        loadText("varPhasePath", bodePhasePath_, sizeof(bodePhasePath_));
        loadFloat("fStart", bodeFStart_);
        loadFloat("fStop", bodeFStop_);
        loadFloat("fStep", bodeFStep_);
        loadFloat("dwellSec", bodeDwellSec_);
        loadFloat("amplitude", bodeAmp_);
        loadFloat("offlineThresholdPct", bodeOfflineThreshPct_);
        if (const auto *v = cJSON_GetObjectItem(bode, "analysisMode"); cJSON_IsNumber(v))
                bodeAnalysisMode_ = v->valueint == 1 ? 1 : 0;
        if (const cJSON *v = cJSON_GetObjectItem(bode, "mode"); cJSON_IsNumber(v))
                bodeMode_ = v->valueint == static_cast<int>(Mode::FromData)       ? Mode::FromData
                            : v->valueint == static_cast<int>(Mode::FromVariable) ? Mode::FromVariable
                                                                                  : Mode::Sweep;

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
        loadStyle(bode, "magnitudeStyle", bodeMagStyle_);
        loadStyle(bode, "phaseStyle", bodePhsStyle_);
        modified_ = false;
}
