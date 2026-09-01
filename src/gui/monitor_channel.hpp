/**
 * @file  monitor_channel.hpp
 * @brief MonitorChannel — a single telemetry variable within a scope.
 *
 * Holds the time-series data (values + tss), display style, wave
 * generator state, and a lock-free snapshot mechanism for GUI rendering.
 *
 * Threading model:
 *   - Sampler thread writes via setRVal() / pushBatch() — NO LOCK required.
 *   - GUI thread reads via snap_ — protected by Gui::mtxMonitors_ externally.
 *   - publishSnapshot() is called by the sampler thread inside mtxMonitors_.
 */
#ifndef MONITOR_CHANNEL_HPP
#define MONITOR_CHANNEL_HPP
#include "core/flash_array.hpp"

#include "ckit.h"
#include "core/dynamic_wave.hpp"
#include "core/session_time.hpp"
#include "core/type_codec.hpp"
#include "core/unit_conversion.hpp"
#include "gui/time_series_buffer.hpp"
#include "gui/wave_input.hpp"
#include "sim_stream.hpp"
#include "timeops.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app_log.hpp"

class MonitorScope;

class MonitorChannel
{
        friend class MonitorScope;
        friend class Monitor;

      public:
        enum class DeviceEnum {
                JLINK,
                UDP,
                SHM,
        };

        struct EnumEntry {
                std::string name;
                i64         val;
        };

        static constexpr usize kMaxSamples = 4096;

      private:
        std::string name_{};
        std::string label_{}; // User-facing alias; falls back to name_ when empty
        std::string type_{};
        usize       addr_{};
        std::string symbolName_{};
        u32         numBytes_{4};
        f32         rVal_{}, wVal_{};
        // GUI-facing latest value, refreshed only on publishSnapshot(). Because
        // publishing is skipped while paused, this stays frozen so the TABLE view
        // doesn't update during pause (rVal_ keeps moving as sampling continues)
        f32               dispVal_{};
        u64               rawBits_{}, dispBits_{};
        bool              rawBitsValid_{false}, dispBitsValid_{false};
        std::string       device_{};
        std::string       shmRegionName_{};
        u32               bitOffset_{0};
        u32               bitSize_{0};
        unsigned          simField_{0};
        std::atomic<bool> wValDirty_{false};
        std::atomic<bool> pendingDelete_{false};
        bool              addrUnknown_{false}; // symbol missing from the (reloaded) ELF
        bool              writable_{true};     // RW or RO — shown in the table R/W column

        // Primary data store — written ONLY by sampler thread (no lock needed)
        MmapVector<f32> rVals_{};
        DeltaTsBuffer   rTs_{}; // block-anchored f32-delta tss (compact, ~half the bytes)

        std::vector<EnumEntry> enums_{};
        int                    displayRadix_{10};
        char                   valueEditBuf_[96]{};
        bool                   valueEditActive_{false};
        bool                   simWritePending_{false};
        uint64_t               simWriteEpoch_{0};

        u64              sampleCount_{0};
        u64              prevRateCount_{0};
        u64              prevRateTime_{0};
        u64              publishedEnd_{0}; // abs idx (exclusive) of prev sample mirrored to read_
        bool             preserveHistoryTimestamps_{false};
        bool             rateInited_{false};
        std::atomic<f32> sampleHz_{0.0f};
        std::atomic<u64> sampleRateUpdatedAt_{0};
        usize            minKeepPoints_{4096};

        f32  color_[4]{1.0f, 1.0f, 1.0f, 1.0f};
        bool useAutoColor_{true};
        f32  lineWeight_{1.5f};
        int  plotStyle_{0}; // 0 = Line, 1 = Stairs
        f32  gain_{1.0f};   // display/calculation multiplier applied to every raw sample
        f32  bias_{0.0f};   // display/calculation offset added after the gain (value * gain + bias)
        bool showMarkers_{false};
        bool show_{true};
        i64  order_{0}; // Display/insertion order within the scope (lower = earlier)

        shm_t shm_{};

      public:
        // Optional hover readout only. Never participates in sampling, gain/bias,
        // stored history, plotting, or write-back values.
        unit_conversion::Settings unitConversion_;

        // Sampler-owned cursor. GUI changes mappings while holding mtxMonitors_
        ava_sim::Cursor simCursor_{};
        unsigned        getSimField() const { return simField_; }
        void            setSimField(unsigned field)
        {
                simField_  = std::min(field, ava_sim::fieldCount - 1);
                simCursor_ = {};
        }

        // GUI-side view of the ch. Built incrementally by publishSnapshot
        // under mtxMonitors_; read by GUI under the same lock. Combines a raw
        // ring (for FFT / high-zoom rendering) with a min/max LOD pyramid
        // (for downsampled plot rendering)
        TimeSeriesBuffer                            read_;
        std::shared_ptr<const flasharray::Snapshot> arraySnapshot_;

        /**
         * @brief Called by sampler thread inside Gui::mtxMonitors_ to publish current
         *        data for GUI consumption. O(delta) — only mirrors samples appended
         *        since the prev publish, and drops aged-out samples by advancing
         *        ring heads
         */
        void publishSnapshot()
        {
                const u64 storedStart = sampleCount_ - static_cast<u64>(rTs_.size());
                if (preserveHistoryTimestamps_)
                        read_.preserveExactTimestamps();
                const u64 readSize     = static_cast<u64>(read_.rawSize());
                const u64 readFirstAbs = (publishedEnd_ >= readSize) ? (publishedEnd_ - readSize) : 0;

                // Detect divergence (clearData / huge gap) — hard reset
                if (publishedEnd_ > sampleCount_ || publishedEnd_ < storedStart || readFirstAbs > sampleCount_) {
                        read_.clear();
                        if (preserveHistoryTimestamps_)
                                read_.preserveExactTimestamps();
                        publishedEnd_ = storedStart;
                } else if (readFirstAbs < storedStart) {
                        // Drop samples that aged out of the writer's front
                        const u64 toDrop = std::min<u64>(storedStart - readFirstAbs, readSize);
                        read_.dropFront(static_cast<usize>(toDrop));
                }

                // Append newly arrived samples (writer's tail since prev publish)
                if (publishedEnd_ < sampleCount_) {
                        const usize srcStart = static_cast<usize>(publishedEnd_ - storedStart);
                        const usize n        = static_cast<usize>(sampleCount_ - publishedEnd_);
                        for (usize i = 0; i < n; ++i) {
                                read_.push(rVals_[srcStart + i], rTs_[srcStart + i]);
                        }
                        publishedEnd_ = sampleCount_;
                }

                // Mirror the live value for the GUI; frozen while paused (not called)
                dispVal_       = rVal_;
                dispBits_      = rawBits_;
                dispBitsValid_ = rawBitsValid_;
        }

      public:
        explicit MonitorChannel(std::string chName) : name_(std::move(chName))
        {
                wave_cfg_t cfg = {1000.0f, WAVE_TYPE_SINE, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f};
                wave_init(&wave_, cfg);
                waveCfgPending_.params[WaveCfgPending::DUTY].start.store(0.5f);
                waveCfgPending_.params[WaveCfgPending::DUTY].end.store(0.5f);
        }
        MonitorChannel()  = default;
        ~MonitorChannel() = default;

        /**
         * @brief Explicit clone used by Shift+drag. MmapVector and atomics are not
         *        copyable, so rebuild an independent history buffer and copy settings
         */
        std::shared_ptr<MonitorChannel> cloneWithHistory() const
        {
                auto out                        = std::make_shared<MonitorChannel>(name_);
                out->label_                     = label_;
                out->arraySnapshot_             = arraySnapshot_;
                out->type_                      = type_;
                out->addr_                      = addr_;
                out->symbolName_                = symbolName_;
                out->numBytes_                  = numBytes_;
                out->rVal_                      = rVal_;
                out->wVal_                      = wVal_;
                out->dispVal_                   = dispVal_;
                out->rawBits_                   = rawBits_;
                out->dispBits_                  = dispBits_;
                out->rawBitsValid_              = rawBitsValid_;
                out->dispBitsValid_             = dispBitsValid_;
                out->device_                    = device_;
                out->shmRegionName_             = shmRegionName_;
                out->simField_                  = simField_;
                out->bitOffset_                 = bitOffset_;
                out->bitSize_                   = bitSize_;
                out->addrUnknown_               = addrUnknown_;
                out->writable_                  = writable_;
                out->enums_                     = enums_;
                out->displayRadix_              = displayRadix_;
                out->unitConversion_            = unitConversion_;
                out->sampleHz_                  = sampleHz_.load(std::memory_order_relaxed);
                out->minKeepPoints_             = minKeepPoints_;
                out->useAutoColor_              = useAutoColor_;
                out->lineWeight_                = lineWeight_;
                out->plotStyle_                 = plotStyle_;
                out->gain_                      = gain_;
                out->bias_                      = bias_;
                out->showMarkers_               = showMarkers_;
                out->show_                      = show_;
                out->waveEnable_                = waveEnable_;
                out->wave_                      = wave_;
                out->historySeconds_            = historySeconds_;
                out->maxDisplayPoints_          = maxDisplayPoints_;
                out->preserveHistoryTimestamps_ = preserveHistoryTimestamps_;
                std::copy_n(color_, 4, out->color_);

                out->waveCfgPending_.freq.store(waveCfgPending_.freq.load());
                out->waveCfgPending_.amp.store(waveCfgPending_.amp.load());
                out->waveCfgPending_.offset.store(waveCfgPending_.offset.load());
                out->waveCfgPending_.duty.store(waveCfgPending_.duty.load());
                out->waveCfgPending_.type.store(waveCfgPending_.type.load());
                out->waveCfgPending_.mode.store(waveCfgPending_.mode.load());
                out->waveCfgPending_.dynamicInitialized.store(waveCfgPending_.dynamicInitialized.load());
                out->waveCfgPending_.dirty.store(waveCfgPending_.dirty.load());
                for (usize i = 0; i < WaveCfgPending::PARAM_COUNT; ++i)
                        out->waveCfgPending_.params[i].copyFrom(waveCfgPending_.params[i]);
                out->waveCfgPending_.restart.store(true);

                // The GUI snapshot is the exact history currently visible to the
                // user. Rebuild both writer and reader stores from that snapshot so
                // the clone owns all memory and can continue sampling independently
                const usize n = read_.rawSize();
                if (out->preserveHistoryTimestamps_) {
                        out->rTs_.preserveExactTimestamps();
                        out->read_.preserveExactTimestamps();
                }
                for (usize i = 0; i < n; ++i) {
                        const f64 ts  = read_.rawTs(i);
                        const f32 val = read_.rawVal(i);
                        out->rTs_.push_back(ts);
                        out->rVals_.push_back(val);
                        out->read_.push(val, ts);
                }
                out->sampleCount_  = n;
                out->publishedEnd_ = n;
                return out;
        }

        std::string       &getName() { return name_; }
        void               setName(const std::string &name) { name_ = name; }
        const std::string &getLabel() const { return label_.empty() ? name_ : label_; }
        void               setLabel(const std::string &l) { label_ = l; }
        const std::string &getType() const { return type_; }
        /**
         * @brief Sole entry point for a type token: normalizing here keeps every caller
         *        (symbol sync, session load, drag payloads) consistent, and keeps the
         *        width in step for tokens saved by older versions in uppercase
         */
        void setType(const std::string &type)
        {
                if (type_ != normalizeTypeName(type))
                        rawBitsValid_ = dispBitsValid_ = false;
                type_     = normalizeTypeName(type);
                numBytes_ = typeBytes(type_);
        }
        usize       &getAddr() { return addr_; }
        std::string &getSymbolName() { return symbolName_; }
        void         setAddr(const usize addr) { addr_ = addr; }
        /**
         * @brief Set when a reloaded ELF no longer defines this ch's symbol — the
         *        address is then shown as "UNKNOWN" instead of a stale value
         */
        bool               isAddrUnknown() const { return addrUnknown_; }
        void               setAddrUnknown(bool u) { addrUnknown_ = u; }
        std::string       &getDevice() { return device_; }
        const std::string &getDevice() const { return device_; }
        void               setDevice(const std::string &device) { device_ = device; }
        std::string       &getShmRegionName() { return shmRegionName_; }
        void               setShmRegionName(const std::string &n) { shmRegionName_ = n; }
        shm_t             &getShm() { return shm_; }

        u32  getBitOffset() const { return bitOffset_; }
        void setBitOffset(u32 o) { bitOffset_ = o; }
        u32  getBitSize() const { return bitSize_; }
        void setBitSize(u32 s) { bitSize_ = s; }

        i64  getOrder() const { return order_; }
        void setOrder(i64 o) { order_ = o; }

        bool isWritable() const { return writable_ && (device_ != ava_sim::device || ava_sim::writableField(simField_)); }
        void setWritable(bool w) { writable_ = w && (device_ != ava_sim::device || ava_sim::writableField(simField_)); }

        int  getDisplayRadix() const { return displayRadix_; }
        void setDisplayRadix(int radix) { displayRadix_ = radix == 2 || radix == 8 || radix == 16 ? radix : 10; }

        f32 &getRVal() { return rVal_; }
        /**
         * @brief GUI display value (frozen while paused), gain & bias applied: value * gain + bias
         */
        f32  getDispVal() const { return dispVal_ * gain_ + bias_; }
        f32  getDispRawVal() const { return dispVal_; }
        void setLatestRawBytes(const void *bytes, u32 size)
        {
                rawBits_      = 0;
                rawBitsValid_ = bytes && size == numBytes_ && size > 0 && size <= sizeof(rawBits_);
                if (rawBitsValid_)
                        std::memcpy(&rawBits_, bytes, size);
        }
        bool getDisplayBits(u64 &bits) const
        {
                bits = dispBits_;
                return dispBitsValid_;
        }
        f32  &getGain() { return gain_; }
        f32  &getBias() { return bias_; }
        bool &show() { return show_; }
        void  setShow(bool s) { show_ = s; }
        bool &selected() { return selected_; }
        bool  selected_{false};

        // Wave generator — config written by GUI via atomic shadow fields,
        // wave_exec() driven by sampler thread
        bool   waveEnable_{false};
        wave_t wave_{};

        // Atomic shadow fields for wave config (GUI writes, sampler reads)
        struct WaveCfgPending {
                enum ParamIndex : usize { FREQUENCY, AMPLITUDE, PHASE, OFFSET, DUTY, PARAM_COUNT };

                struct DynamicParam {
                        std::atomic<int>  law{static_cast<int>(WaveDynamicLaw::FIXED)};
                        std::atomic<f32>  start{0.0f};
                        std::atomic<f32>  end{0.0f};
                        std::atomic<f32>  duration{1.0f};
                        std::atomic<f32>  step{1.0f};
                        std::atomic<f32>  interval{1.0f};
                        std::atomic<bool> repeat{false};

                        WaveDynamicParam snapshot() const
                        {
                                return {static_cast<WaveDynamicLaw>(law.load()),
                                        start.load(),
                                        end.load(),
                                        duration.load(),
                                        step.load(),
                                        interval.load(),
                                        repeat.load()};
                        }

                        void copyFrom(const DynamicParam &other)
                        {
                                law.store(other.law.load());
                                start.store(other.start.load());
                                end.store(other.end.load());
                                duration.store(other.duration.load());
                                step.store(other.step.load());
                                interval.store(other.interval.load());
                                repeat.store(other.repeat.load());
                        }
                };

                std::atomic<f32>  freq{0.0f};
                std::atomic<f32>  amp{0.0f};
                std::atomic<f32>  offset{0.0f};
                std::atomic<f32>  duty{0.5f};
                std::atomic<int>  type{WAVE_TYPE_SINE};
                std::atomic<int>  mode{static_cast<int>(WaveGeneratorMode::BASIC)};
                std::atomic<bool> dynamicInitialized{false};
                std::atomic<bool> dirty{false};
                std::atomic<bool> restart{false};
                DynamicParam      params[PARAM_COUNT]{};
        };
        WaveCfgPending waveCfgPending_;
        // GUI drafts are intentionally not copied or saved as applied settings
        std::unordered_map<ImGuiID, WaveInputDraft> waveInputDrafts_;
        // Sampler-owned simulation-clock state for waveform commands
        bool     simWaveInitialized_{};
        uint64_t simWaveEpoch_{};
        double   simWaveTime_{};

        // Sampler-thread-only state for the dynamic timeline
        f64  waveDynamicElapsed_{0.0};
        f32  waveDynamicPhaseRad_{0.0f};
        bool waveDynamicActive_{false};

        /**
         * @brief Apply any pending wave config from GUI to the actual wave state
         *        Called by sampler thread before wave_exec()
         */
        void applyPendingWaveCfg()
        {
                if (waveCfgPending_.dirty.exchange(false)) {
                        wave_.cfg.freq   = waveCfgPending_.freq.load();
                        wave_.cfg.amp    = waveCfgPending_.amp.load();
                        wave_.cfg.offset = waveCfgPending_.offset.load();
                        wave_.cfg.duty   = waveCfgPending_.duty.load();
                        wave_.cfg.type   = static_cast<wave_type_t>(waveCfgPending_.type.load());
                }
        }

        /**
         * @brief Apply the dynamic laws immediately before wave_exec(). Phase is an
         *        offset on top of the oscillator's accumulated phase, so changing it
         *        does not destroy frequency integration
         */
        void applyDynamicWaveCfg(f32 dt)
        {
                const bool restart = waveCfgPending_.restart.exchange(false);
                const auto mode    = static_cast<WaveGeneratorMode>(waveCfgPending_.mode.load());

                if (restart || mode != WaveGeneratorMode::DYNAMIC) {
                        if (waveDynamicActive_)
                                wave_.cfg.phase -= waveDynamicPhaseRad_;
                        waveDynamicElapsed_  = 0.0;
                        waveDynamicPhaseRad_ = 0.0f;
                        waveDynamicActive_   = false;
                }
                if (mode != WaveGeneratorMode::DYNAMIC)
                        return;

                waveDynamicActive_ = true;
                wave_.cfg.freq =
                    evaluateWaveDynamicParam(waveCfgPending_.params[WaveCfgPending::FREQUENCY].snapshot(), waveDynamicElapsed_);
                wave_.cfg.amp =
                    evaluateWaveDynamicParam(waveCfgPending_.params[WaveCfgPending::AMPLITUDE].snapshot(), waveDynamicElapsed_);
                wave_.cfg.offset =
                    evaluateWaveDynamicParam(waveCfgPending_.params[WaveCfgPending::OFFSET].snapshot(), waveDynamicElapsed_);
                wave_.cfg.duty = std::clamp(
                    evaluateWaveDynamicParam(waveCfgPending_.params[WaveCfgPending::DUTY].snapshot(), waveDynamicElapsed_),
                    0.001f,
                    0.999f);

                constexpr f32 DEG_TO_RAD = 0.01745329251994329577f;
                const f32     phaseRad =
                    evaluateWaveDynamicParam(waveCfgPending_.params[WaveCfgPending::PHASE].snapshot(), waveDynamicElapsed_) *
                    DEG_TO_RAD;
                wave_.cfg.phase      += phaseRad - waveDynamicPhaseRad_;
                waveDynamicPhaseRad_  = phaseRad;
                waveDynamicElapsed_  += std::max(0.0f, dt);
        }

        /**
         * @brief Sampler thread: append a single sample (no lock required)
         */
        void setRVal(f32 val, f64 ts)
        {
                rawBitsValid_ = false;
                f64 fts       = ts;
                if (!rTs_.empty() && fts <= rTs_.back()) {
                        fts = rTs_.back() + 0.000001; // Enforce strict monotonicity (1us)
                }
                rTs_.push_back(fts);
                rVals_.push_back(val);
                rVal_ = val;
                sampleCount_++;
                updateRate_();

                // History pruning
                if (historySeconds_ > 0.0f) {
                        while (rTs_.size() > minKeepPoints_ && (fts - rTs_.front()) > (f64)historySeconds_) {
                                rTs_.pop_front();
                                rVals_.pop_front();
                        }
                }
        }

        /**
         * @brief Sampler thread: batch insert (no lock required)
         */
        void pushBatch(const f32 *vals, const f64 *ts, const usize count)
        {
                if (count == 0)
                        return;
                rawBitsValid_ = false;
                for (size_t i = 0; i < count; ++i) {
                        rVals_.push_back(vals[i]);
                        f64 fts = ts[i];
                        if (!rTs_.empty() && fts <= rTs_.back()) {
                                fts = rTs_.back() + 0.000001;
                        }
                        rTs_.push_back(fts);
                }
                rVal_ = vals[count - 1];

                // Optimized block pruning
                if (historySeconds_ > 0.0f) {
                        const f64 cutoff   = ts[count - 1] - (f64)historySeconds_;
                        usize     popCount = 0;
                        while (rTs_.size() - popCount > minKeepPoints_ && popCount < rTs_.size() && rTs_[popCount] < cutoff) {
                                ++popCount;
                        }
                        for (usize i = 0; i < popCount; ++i) {
                                rTs_.pop_front();
                                rVals_.pop_front();
                        }
                }

                sampleCount_ += count;
                updateRate_();
        }

        f32 getHz() const
        {
                const u64 updated = sampleRateUpdatedAt_.load(std::memory_order_relaxed);
                return updated && get_mono_ts_ms() - updated < 1500 ? sampleHz_.load(std::memory_order_relaxed) : 0.0f;
        }

        f32  *getColor() { return color_; }
        bool &useAutoColor() { return useAutoColor_; }
        f32  &getLineWeight() { return lineWeight_; }
        int  &getPlotStyle() { return plotStyle_; }
        bool &showMarkers() { return showMarkers_; }

        f32 &getWVal() { return wVal_; }
        void setWVal(const f32 val) { wVal_ = val; }

        u32  getNumBytes() const { return numBytes_; }
        void setNumBytes(const u32 nb) { numBytes_ = nb; }

        /**
         * @brief UI marks dirty → sampler consumes and writes to target
         */
        void markWValDirty()
        {
                if (device_ == ava_sim::device) {
                        simWritePending_ = true;
                        simWriteEpoch_   = 0;
                }
                wValDirty_.store(true, std::memory_order_release);
        }
        bool simWritePending() const { return simWritePending_; }
        f32  simInputValue() const { return simWritePending_ ? wVal_ * gain_ + bias_ : getDispVal(); }
        void simWriteSent(uint64_t epoch, float val)
        {
                if (simWritePending_ && val == wVal_)
                        simWriteEpoch_ = epoch;
        }
        void confirmSimWrite(uint64_t epoch, float val)
        {
                if (simWritePending_ && ((simWriteEpoch_ && simWriteEpoch_ != epoch) || val == wVal_)) {
                        simWritePending_ = false;
                        simWriteEpoch_   = 0;
                }
        }
        bool consumeWValDirty(f32 &out)
        {
                if (!wValDirty_.exchange(false, std::memory_order_acq_rel))
                        return false;
                out = wVal_;
                return true;
        }

        /**
         * @brief Returns (re, im) of the DFT at frequency f for samples within [tMin, tMax]
         *        Reads from the GUI-side raw ring (call from GUI thread under mtxMonitors_)
         */
        std::pair<f64, f64> dftSlice(f64 tMin, f64 tMax, f64 f) const
        {
                const usize si = read_.rawLowerBound(tMin);
                const usize ei = read_.rawUpperBound(tMax);
                double      re = 0.0, im = 0.0;
                for (usize i = si; i < ei; ++i) {
                        const double t   = read_.rawTs(i);
                        const double v   = static_cast<double>(read_.rawVal(i));
                        const double ph  = 2.0 * M_PI * f * t;
                        re              += v * std::cos(ph);
                        im              -= v * std::sin(ph);
                }
                return {re, im};
        }

      private:
        void updateRate_()
        {
                const u64 now = get_mono_ts_ms();
                if (!rateInited_) {
                        prevRateTime_  = now;
                        prevRateCount_ = sampleCount_;
                        rateInited_    = true;
                } else {
                        const auto dtMs = now - prevRateTime_;
                        if (dtMs >= 500) {
                                const u64 dCount = sampleCount_ - prevRateCount_;
                                sampleHz_        = static_cast<f32>(dCount * 1000.0 / static_cast<f64>(dtMs));
                                sampleRateUpdatedAt_.store(now, std::memory_order_relaxed);
                                prevRateTime_  = now;
                                prevRateCount_ = sampleCount_;
                        }
                }
        }

      public:
        /**
         * @brief Delete mark: UI sets, sampler purges at safe point
         */
        void markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }

        const std::vector<EnumEntry> &getEnums() const { return enums_; }
        void                          setEnums(std::vector<EnumEntry> e) { enums_ = std::move(e); }
        bool                          isEnum() const { return !enums_.empty(); }
        const char                   *findEnumName(const i64 v) const
        {
                for (const auto &e : enums_)
                        if (e.val == v)
                                return e.name.c_str();
                return nullptr;
        }

        /**
         * @brief Number of points currently in memory (sampler thread context)
         */
        usize storedCount() const { return rVals_.size(); }

        /**
         * @brief Earliest / latest ts in memory (sampler thread context)
         */
        f64 earliestTs() const { return arraySnapshot_ ? 0.0 : read_.firstTs(); }
        f64 latestTs() const
        {
                return arraySnapshot_ ? double(arraySnapshot_->values.size() - 1) * arraySnapshot_->step : read_.prevTs();
        }

        /**
         * @brief Apply only at the writer's safe point while holding the monitor lock
         *        A paused GUI can have a different retained window from the writer;
         *        edit each independently without publishing the hidden tail. Absolute
         *        publish coordinates are kept relative to the writer's retained end
         */
        usize discardTimeRange(f64 tMin, f64 tMax)
        {
                if (arraySnapshot_ || !std::isfinite(tMin) || !std::isfinite(tMax) || tMin > tMax)
                        return 0;
                const u64 storedStart = sampleCount_ - static_cast<u64>(rTs_.size());
                const u64 readStart   = publishedEnd_ >= read_.rawSize() ? publishedEnd_ - read_.rawSize() : 0;
                // If the first publish followed writer pruning, float-delta
                // timestamp blocks can have different anchors in the two
                // stores. Use the visible timestamp for their shared samples
                // so selecting an exact endpoint always erases the same sample
                // in both, despite any pre-existing rounding difference
                const auto visibleWriterTs = [&](usize i) {
                        const u64 absolute = storedStart + i;
                        return absolute >= readStart && absolute < publishedEnd_
                                   ? read_.rawTs(static_cast<usize>(absolute - readStart))
                                   : rTs_[i];
                };
                const auto bound = [&](f64 t, bool upper) {
                        usize lo = 0, hi = rTs_.size();
                        while (lo < hi) {
                                const usize mid = lo + (hi - lo) / 2;
                                const f64   val = visibleWriterTs(mid);
                                if (val < t || (upper && val == t))
                                        lo = mid + 1;
                                else
                                        hi = mid;
                        }
                        return lo;
                };
                const usize first            = bound(tMin, false);
                const usize last             = bound(tMax, true);
                const usize unpublishedStart = publishedEnd_ > storedStart
                                                   ? static_cast<usize>(std::min<u64>(publishedEnd_ - storedStart, rTs_.size()))
                                                   : 0;
                const usize unpublishedRemoved =
                    last > std::max(first, unpublishedStart) ? last - std::max(first, unpublishedStart) : 0;
                const usize writerRemoved = first < last ? rTs_.eraseRange(first, last) : 0;
                if (writerRemoved)
                        rVals_.eraseRange(first, last);
                const usize readerRemoved = read_.discardTimeRange(tMin, tMax);
                if (writerRemoved || readerRemoved) {
                        preserveHistoryTimestamps_ = true;
                        rTs_.preserveExactTimestamps();
                        read_.preserveExactTimestamps();
                        // sampleCount_ - publishedEnd_ must still equal the
                        // number of unpublished samples after the deletion
                        publishedEnd_ += unpublishedRemoved;
                }
                // sampleCount_ remains the cumulative acquisition counter used
                // for Hz; deleting history must never underflow rate deltas
                // Count shared samples once, plus the writer's unpublished tail
                return readerRemoved + unpublishedRemoved;
        }

        /**
         * @brief Clear all stored data points
         */
        void clearData()
        {
                arraySnapshot_.reset();
                rVals_.clear();
                rTs_.clear();
                read_.clear();
                sampleCount_               = 0;
                prevRateCount_             = 0;
                rateInited_                = false;
                sampleHz_                  = 0.0f;
                sampleRateUpdatedAt_       = 0;
                publishedEnd_              = 0;
                preserveHistoryTimestamps_ = false;
        }

        f32 historySeconds_{1.0f};
        u32 maxDisplayPoints_{5000};
};

#endif // !MONITOR_CHANNEL_HPP
