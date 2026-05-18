/**
 * @file  monitor_channel.hpp
 * @brief MonitorChannel — a single telemetry variable within a scope.
 *
 * Holds the time-series data (values + timestamps), display style, wave
 * generator state, and a lock-free snapshot mechanism for GUI rendering.
 *
 * Threading model:
 *   - Sampler thread writes via setRVal() / pushBatch() — NO LOCK required.
 *   - GUI thread reads via snap_ — protected by Gui::mtxMonitors_ externally.
 *   - publishSnapshot() is called by the sampler thread inside mtxMonitors_.
 */
#ifndef MONITOR_CHANNEL_HPP
#define MONITOR_CHANNEL_HPP

#include "core/session_time.hpp"
#include "core/type_codec.hpp"
#include "module.h"
#include "timeops.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app_log.hpp"

class MonitorScope;

class MonitorChannel
{
        friend class MonitorScope;

      public:
        enum class DeviceEnum {
                JLINK,
                UDP,
                SHM,
        };

        struct EnumEntry {
                std::string name;
                i64         value;
        };

        static constexpr usize kMaxSamples = 4096;

      private:
        std::string       name_{};
        std::string       type_{};
        usize             addr_{};
        std::string       symbolName_{};
        u32               numBytes_{4};
        f32               rVal_{}, wVal_{};
        std::string       device_{};
        std::string       shmRegionName_{};
        std::atomic<bool> wValDirty_{false};
        std::atomic<bool> pendingDelete_{false};

        // Primary data store — written ONLY by sampler thread (no lock needed).
        std::deque<f32> rVals_{};
        std::deque<f64> rTs_{};

        std::vector<EnumEntry> enums_{};

        u64   sampleCount_{0};
        u64   lastRateCount_{0};
        u64   lastRateTime_{0};
        u64   snapStartSample_{0};
        bool  rateInited_{false};
        f32   sampleHz_{0.0f};
        usize minKeepPoints_{4096};

        f32  color_[4]{1.0f, 1.0f, 1.0f, 1.0f};
        bool useAutoColor_{true};
        f32  lineWeight_{1.5f};
        int  plotStyle_{0}; // 0 = Line, 1 = Stairs
        bool showMarkers_{false};
        bool show_{true};

        shm_t shm_{};

      public:
        // --- Snapshot for GUI rendering (read by GUI, written by publishSnapshot) ---
        struct Snapshot {
                std::vector<f64> ts;
                std::vector<f32> vals;
                f32              lastVal{0};
        };
        Snapshot snap_;

        // Called by sampler thread inside Gui::mtxMonitors_ to publish current
        // data for GUI consumption. This replaces the old valMutex_ approach.
        void publishSnapshot()
        {
                const u64 storedStart = sampleCount_ - static_cast<u64>(rTs_.size());

                if (snap_.ts.empty()) {
                        snapStartSample_ = storedStart;
                } else if (storedStart > snapStartSample_) {
                        const u64   staleCount64 = storedStart - snapStartSample_;
                        const usize staleCount   = static_cast<usize>(std::min<u64>(staleCount64, snap_.ts.size()));
                        snap_.ts.erase(snap_.ts.begin(), snap_.ts.begin() + staleCount);
                        snap_.vals.erase(snap_.vals.begin(), snap_.vals.begin() + staleCount);
                        snapStartSample_ += staleCount;
                        if (snapStartSample_ < storedStart) {
                                snap_.ts.clear();
                                snap_.vals.clear();
                                snapStartSample_ = storedStart;
                        }
                } else if (storedStart < snapStartSample_) {
                        snap_.ts.clear();
                        snap_.vals.clear();
                        snapStartSample_ = storedStart;
                }

                const u64 publishedEnd = snapStartSample_ + static_cast<u64>(snap_.ts.size());
                if (publishedEnd < storedStart || publishedEnd > sampleCount_) {
                        snap_.ts.clear();
                        snap_.vals.clear();
                        snapStartSample_ = storedStart;
                }

                const usize appendStart = static_cast<usize>(snapStartSample_ + snap_.ts.size() - storedStart);
                if (appendStart < rTs_.size()) {
                        const usize appendCount = rTs_.size() - appendStart;
                        snap_.ts.reserve(snap_.ts.size() + appendCount);
                        snap_.vals.reserve(snap_.vals.size() + appendCount);
                        for (usize i = appendStart; i < rTs_.size(); ++i) {
                                snap_.ts.push_back(rTs_[i]);
                                snap_.vals.push_back(rVals_[i]);
                        }
                }
                snap_.lastVal = rVal_;
        }

      public:
        explicit MonitorChannel(std::string chName) : name_(std::move(chName))
        {
                wave_cfg_t cfg = {1000.0f, WAVE_TYPE_SINE, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f};
                wave_init(&wave_, cfg);
        }
        MonitorChannel()  = default;
        ~MonitorChannel() = default;

        std::string &getName() { return name_; }
        void         setName(const std::string &name) { name_ = name; }
        std::string &getType() { return type_; }
        void         setType(const std::string &type)
        {
                type_     = type;
                numBytes_ = typeBytes(type);
        }
        usize       &getAddr() { return addr_; }
        std::string &getSymbolName() { return symbolName_; }
        void         setAddr(const usize addr) { addr_ = addr; }
        std::string &getDevice() { return device_; }
        void         setDevice(const std::string &device) { device_ = device; }
        std::string &getShmRegionName() { return shmRegionName_; }
        void         setShmRegionName(const std::string &n) { shmRegionName_ = n; }
        shm_t       &getShm() { return shm_; }

        f32  &getRVal() { return rVal_; }
        bool &show() { return show_; }
        void  setShow(bool s) { show_ = s; }
        bool &selected() { return selected_; }
        bool  selected_{false};

        // Wave generator — config written by GUI via atomic shadow fields,
        // wave_exec() driven by sampler thread.
        bool   waveEnable_{false};
        wave_t wave_{};

        // Atomic shadow fields for wave config (GUI writes, sampler reads).
        struct WaveCfgPending {
                std::atomic<f32>  freq{0.0f};
                std::atomic<f32>  amp{0.0f};
                std::atomic<f32>  offset{0.0f};
                std::atomic<f32>  duty{0.5f};
                std::atomic<int>  type{WAVE_TYPE_SINE};
                std::atomic<bool> dirty{false};
        };
        WaveCfgPending waveCfgPending_;

        // Apply any pending wave config from GUI to the actual wave state.
        // Called by sampler thread before wave_exec().
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

        // Sampler thread: append a single sample (no lock required).
        void setRVal(f32 val, f64 ts)
        {
                f64 fts = ts;
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

        // Sampler thread: batch insert (no lock required).
        void pushBatch(const f32 *vals, const f64 *ts, const usize count)
        {
                if (count == 0)
                        return;
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
                        auto      itTs     = rTs_.begin();
                        auto      itVal    = rVals_.begin();
                        usize     popCount = 0;
                        while (rTs_.size() - popCount > minKeepPoints_ && itTs != rTs_.end() && *itTs < cutoff) {
                                ++itTs;
                                ++itVal;
                                ++popCount;
                        }
                        if (popCount > 0) {
                                rTs_.erase(rTs_.begin(), itTs);
                                rVals_.erase(rVals_.begin(), itVal);
                        }
                }

                sampleCount_ += count;
                if (sampleCount_ % 100 < count)
                        updateRate_();
        }

        f32 getHz() const { return sampleHz_; }

        f32  *getColor() { return color_; }
        bool &useAutoColor() { return useAutoColor_; }
        f32  &getLineWeight() { return lineWeight_; }
        int  &getPlotStyle() { return plotStyle_; }
        bool &showMarkers() { return showMarkers_; }

        f32 &getWVal() { return wVal_; }
        void setWVal(const f32 val) { wVal_ = val; }

        u32  getNumBytes() const { return numBytes_; }
        void setNumBytes(const u32 nb) { numBytes_ = nb; }

        // UI marks dirty → sampler consumes and writes to target.
        void markWValDirty() { wValDirty_.store(true, std::memory_order_release); }
        bool consumeWValDirty(f32 &out)
        {
                if (!wValDirty_.exchange(false, std::memory_order_acq_rel))
                        return false;
                out = wVal_;
                return true;
        }

        // Returns (re, im) of the DFT at frequency f for samples within [tMin, tMax].
        // Reads from snapshot (call from GUI thread).
        std::pair<f64, f64> dftSlice(f64 tMin, f64 tMax, f64 f) const
        {
                const auto  &ts   = snap_.ts;
                const auto  &vals = snap_.vals;
                const size_t si = static_cast<size_t>(std::distance(ts.begin(), std::lower_bound(ts.begin(), ts.end(), tMin)));
                const size_t ei = static_cast<size_t>(std::distance(ts.begin(), std::upper_bound(ts.begin(), ts.end(), tMax)));
                double       re = 0.0, im = 0.0;
                for (size_t i = si; i < ei; ++i) {
                        const double ph  = 2.0 * M_PI * f * static_cast<double>(ts[i]);
                        re              += static_cast<double>(vals[i]) * std::cos(ph);
                        im              -= static_cast<double>(vals[i]) * std::sin(ph);
                }
                return {re, im};
        }

      private:
        void updateRate_()
        {
                const u64 now = get_mono_ts_ms();
                if (!rateInited_) {
                        lastRateTime_  = now;
                        lastRateCount_ = sampleCount_;
                        rateInited_    = true;
                } else {
                        const auto dtMs = now - lastRateTime_;
                        if (dtMs >= 500) {
                                const u64 dCount = sampleCount_ - lastRateCount_;
                                sampleHz_        = static_cast<f32>(dCount * 1000.0 / static_cast<f64>(dtMs));
                                lastRateTime_    = now;
                                lastRateCount_   = sampleCount_;
                        }
                }
        }

      public:
        // Delete mark: UI sets, sampler purges at safe point.
        void markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }

        const std::vector<EnumEntry> &getEnums() const { return enums_; }
        void                          setEnums(std::vector<EnumEntry> e) { enums_ = std::move(e); }
        bool                          isEnum() const { return !enums_.empty(); }
        const char                   *findEnumName(const i64 v) const
        {
                for (const auto &e : enums_)
                        if (e.value == v)
                                return e.name.c_str();
                return nullptr;
        }

        // Number of points currently in memory (sampler thread context).
        usize storedCount() const { return rVals_.size(); }

        // Earliest / latest timestamp in memory (sampler thread context).
        f64 earliestTs() const { return rTs_.empty() ? -1.0 : rTs_.front(); }
        f64 latestTs() const { return rTs_.empty() ? -1.0 : rTs_.back(); }

        // Clear all stored data points.
        void clearData()
        {
                rVals_.clear();
                rTs_.clear();
                snap_.ts.clear();
                snap_.vals.clear();
                snap_.lastVal    = 0;
                sampleCount_     = 0;
                lastRateCount_   = 0;
                rateInited_      = false;
                sampleHz_        = 0.0f;
                snapStartSample_ = 0;
        }

        f32 historySeconds_{1.0f};
        u32 maxDisplayPoints_{5000};
};

#endif // !MONITOR_CHANNEL_HPP
