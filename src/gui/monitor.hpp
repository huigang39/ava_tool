#ifndef MONITOR_HPP
#define MONITOR_HPP

#include <atomic>
#include "timeops.h"
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app_log.hpp"
#include "core/jlink_dev.hpp"
#include "module.h"

extern std::atomic<bool> g_monitorPaused;

class Monitor;

struct ChannelDropPayload {
        static constexpr int kMaxEnums = 16;
        struct EnumEntry {
                char name[64];
                i64  value;
        };

        char      name[128];
        u64       addr;
        char      type[8];   // "F32"/"F64"/"I8"/"I16"/"I32"/"I64"/"U8"/"U16"/"U32"/"U64"
        char      device[8]; // "SHM" / "JLINK" / "UDP"
        u8        numBytes;  // 1/2/4/8 - 派生自 type
        u8        numEnums;  // 0 表示不是枚举
        u64       typeOff;   // DWARF type offset
        EnumEntry enums[kMaxEnums];
};

// type 字符串 → 字节数. 默认 4B.
inline u32
typeBytes(const std::string &t)
{
        if (t == "F64" || t == "I64" || t == "U64")
                return 8;
        if (t == "I16" || t == "U16")
                return 2;
        if (t == "I8" || t == "U8")
                return 1;
        return 4; // F32 / I32 / U32 / 空 / 未知
}

// 全局会话起始点 (微秒)
inline u64 &
getSessionStartUs()
{
        static u64 start = get_mono_ts_us();
        return start;
}

inline f64
sessionTimeSec()
{
        u64 now = get_mono_ts_us();
        u64 start = getSessionStartUs();
        if (now < start) return 0.0;
        return static_cast<f64>(now - start) / 1000000.0;
}

inline void
resetSessionTime()
{
        getSessionStartUs() = get_mono_ts_us();
}

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
        std::atomic<bool> wValDirty_{false};
        std::atomic<bool> pendingDelete_{false};

        std::deque<f32>        rVals_{};
        std::deque<f64>        rTs_{}; // 与 rVals_ 一一对应的时间戳 (秒, 相对 session start)
        mutable std::mutex     valMutex_{};
        std::vector<EnumEntry> enums_{};

        u64                                   sampleCount_{0};
        u64                                   lastRateCount_{0};
        u64 lastRateTime_{0};
        bool                                  rateInited_{false};
        f32                                   sampleHz_{0.0f};
        usize                                 minKeepPoints_{4096};

        f32  color_[4]{1.0f, 1.0f, 1.0f, 1.0f};
        bool useAutoColor_{true};
        f32  lineWeight_{1.5f};
        int  plotStyle_{0}; // 0 = Line, 1 = Stairs
        bool showMarkers_{false};
        bool show_{true};

        shm_t shm_{};

      public:
        explicit MonitorChannel(std::string chName) : name_(std::move(chName))
        {
                // Default wave config: 1000Hz sample rate, 1Hz Sine wave, 1.0 Amp
                wave_cfg_t cfg = {1000.0f, WAVE_TYPE_SINE, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 1.0f};
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
        shm_t       &getShm() { return shm_; }

        f32  &getRVal() { return rVal_; }
        bool &show() { return show_; }
        void  setShow(bool s) { show_ = s; }
        bool &selected() { return selected_; }
        bool  selected_{false};

        // Wave generator
        bool               waveEnable_{false};
        wave_t             wave_{};
        mutable std::mutex waveMtx_{};
        // 每个采样点都带一个绝对时间戳 (相对 session start), 多通道按帧时间戳同步.
        void setRVal(f32 val, f64 ts)
        {
                std::lock_guard lk(valMutex_);
                f64             fts = ts;
                if (!rTs_.empty() && fts <= rTs_.back()) {
                        fts = rTs_.back() + 0.000001; // Enforce strict monotonicity (1us)
                }
                rTs_.push_back(fts);
                rVals_.push_back(val);
                rVal_ = val;
                sampleCount_++;
                updateRate_();

                // History pruning: keep at least minKeepPoints_ OR points within historySeconds_
                if (historySeconds_ > 0.0f) {
                        while (rTs_.size() > minKeepPoints_ && (fts - rTs_.front()) > (f64)historySeconds_) {
                                rTs_.pop_front();
                                rVals_.pop_front();
                        }
                }
        }

        // 批量插入: 一次锁内完成所有 push + 裁剪, 大幅减少锁竞争.
        void pushBatch(const f32 *vals, const f64 *ts, const usize count)
        {
                if (count == 0)
                        return;
                std::lock_guard lk(valMutex_);
                for (size_t i = 0; i < count; ++i) {
                        rVals_.push_back(vals[i]);
                        f64 fts = ts[i];
                        if (!rTs_.empty() && fts <= rTs_.back()) {
                                fts = rTs_.back() + 0.000001;
                        }
                        rTs_.push_back(fts);
                }
                rVal_ = vals[count - 1];

                // Optimized block pruning: find the first element to keep
                if (historySeconds_ > 0.0f) {
                        const f64 cutoff   = ts[count - 1] - (f64)historySeconds_;
                        auto      itTs     = rTs_.begin();
                        auto      itVal    = rVals_.begin();
                        usize     popCount = 0;
                        // Never prune below minKeepPoints_
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
                // Only update rate every 100 samples to save CPU
                if (sampleCount_ % 100 < count)
                        updateRate_();
        }

        f32 getHz() const
        {
                std::lock_guard lk(valMutex_);
                return sampleHz_;
        }

        f32  *getColor() { return color_; }
        bool &useAutoColor() { return useAutoColor_; }
        f32  &getLineWeight() { return lineWeight_; }
        int  &getPlotStyle() { return plotStyle_; }
        bool &showMarkers() { return showMarkers_; }

        f32 &getWVal() { return wVal_; }
        void setWVal(const f32 val)
        {
                std::lock_guard lk(valMutex_);
                wVal_ = val;
                // Note: wVals_ is removed as it was unused and inefficient
        }

        u32  getNumBytes() const { return numBytes_; }
        void setNumBytes(const u32 nb) { numBytes_ = nb; }

        // 用户在 InputFloat 上按 Enter 后由 UI 标记为 dirty,
        // threadFunc 一次性消费并真正写到 target.
        void markWValDirty() { wValDirty_.store(true, std::memory_order_release); }
        bool consumeWValDirty(f32 &out)
        {
                if (!wValDirty_.exchange(false, std::memory_order_acq_rel))
                        return false;
                std::lock_guard lk(valMutex_);
                out = wVal_;
                return true;
        }

        // 同时拷出 X (时间) / Y (值) 两条 series, 一一对应.
        void copyRVals(std::vector<f64> &xs, std::vector<f32> &ys) const
        {
                std::lock_guard lk(valMutex_);
                xs.assign(rTs_.begin(), rTs_.end());
                ys.assign(rVals_.begin(), rVals_.end());
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
        // 删除标记: UI 设置, threadFunc / Monitor 在安全点真正擦除.
        void markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }

        const std::vector<EnumEntry> &getEnums() const { return enums_; }
        void                          setEnums(std::vector<EnumEntry> e) { enums_ = std::move(e); }
        bool                          isEnum() const { return !enums_.empty(); }
        // 找到 value 对应的枚举名, 没匹配返回 nullptr
        const char *findEnumName(const i64 v) const
        {
                for (const auto &e : enums_)
                        if (e.value == v)
                                return e.name.c_str();
                return nullptr;
        }

        // Return number of points currently stored in memory (excluding discarded)
        usize storedCount() const
        {
                std::lock_guard lk(valMutex_);
                return rVals_.size();
        }

        // Return the earliest timestamp in memory, or -1 if empty
        f64 earliestTs() const
        {
                std::lock_guard lk(valMutex_);
                return rTs_.empty() ? -1.0 : rTs_.front();
        }
        f64 latestTs() const
        {
                std::lock_guard lk(valMutex_);
                return rTs_.empty() ? -1.0 : rTs_.back();
        }

        // Clear all stored data points in the channel
        void clearData()
        {
                std::lock_guard lk(valMutex_);
                rVals_.clear();
                rTs_.clear();
        }

        f32 historySeconds_{1.0f};
        u32 maxDisplayPoints_{5000};
};

enum class MonitorViewMode { FULL, FOLLOW, MANUAL };

class MonitorScope
{
      public:
        Monitor *parent_{nullptr};
        enum class DrawEnum {
                PLOT,
                TABLE,
        };
        using ChannelMapType = std::unordered_map<std::string, std::shared_ptr<MonitorChannel>>;
        const std::string &getName() const { return name_; }
        void               setDraw(DrawEnum d) { e_draw = d; }
        DrawEnum           getDraw() const { return e_draw; }
        f32               &getHeight() { return height_; }
        bool              &getShowFft() { return showFft_; }
        int               &getFftPoints() { return fftPoints_; }
        int               &getFftPeakCount() { return fftPeakCount_; }
        ChannelMapType    &getChannels() { return chs_; }
        void               reinitFft(int newPoints);

      private:
        std::string       name_{};
        ChannelMapType    chs_{};
        DrawEnum          e_draw{};
        f32               height_{200.0f};
        bool              showFft_{false};
        int               fftPoints_{1024};
        int               fftPeakCount_{5};
        fft_t             fft_;
        std::vector<f32>  fftInBuf_;
        std::vector<f32>  fftMagF32_;
        std::vector<f64>  fftMagBuf_;
        std::vector<f32>  fftOutBuf_;
        std::vector<f32>  fftLoBuf_;
        std::atomic<bool> pendingDelete_{false};
        std::vector<f64>  dxs_{};
        std::vector<f64>  dys_{};
        std::vector<f64>  tempTs_{};
        std::vector<f64>  tempVals_{};
        bool              isManualHeight_{false};
        bool              paused_{false};
        int               lastSelectedIndex_{-1};

        bool pendingAxisReset_{false};
        struct Peak {
                f64 freq;
                f64 mag;
        };
        std::map<std::string, std::vector<Peak>> channelPeaks_;

        void tableDraw();
        void tableMenu();

        void plotDraw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        void plotMenu();

        void drawTableRow(const std::string               &chName,
                          std::shared_ptr<MonitorChannel> &ch,
                          int                              idx,
                          const std::vector<std::string>  &allKeys);

      public:
        static void shmInit(MonitorChannel &ch);
        explicit MonitorScope(std::string scopeName) : name_(std::move(scopeName))
        {
                fftInBuf_.resize(fftPoints_);
                fftMagF32_.resize(fftPoints_ / 2 + 1);
                fftMagBuf_.resize(fftPoints_ / 2 + 1);
                fftOutBuf_.resize((fftPoints_ / 2 + 1) * 2); // 2 floats per complex point
                fftLoBuf_.resize(fftPoints_);

                fft_cfg_t cfg;
                cfg.npoints  = fftPoints_;
                cfg.fs       = 1000.0f;
                cfg.e_window = FFT_WINDOW_HANNING;
                cfg.in_buf   = fftInBuf_.data();
                cfg.mag_buf  = fftMagF32_.data();
                cfg.out_buf  = (decltype(cfg.out_buf))fftOutBuf_.data();
                cfg.buf      = fftLoBuf_.data();
                fft_init(&fft_, cfg);
        }
        ~MonitorScope() { fft_destroy(&fft_); }

        void            menu();
        void            draw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        void            dropTarget();
        int             addChannel(const std::string &chName);
        int             setValue(const std::string &chName, f32 val);
        MonitorChannel *findChannel(const std::string &chName);
        void            markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool            isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }
        bool            isManual() const { return isManualHeight_; }
        void            setManual(bool m) { isManualHeight_ = m; }

        bool isPaused() const { return paused_; }
        void setPaused(bool p) { paused_ = p; }
        bool isFftEnabled() const { return showFft_; }

        // threadFunc 在每轮迭代结束时调用, 真正擦除被打了删除标记的通道.
        void purgeDeleted()
        {
                for (auto it = chs_.begin(); it != chs_.end();) {
                        if (it->second && it->second->isPendingDelete())
                                it = chs_.erase(it);
                        else
                                ++it;
                }
        }

        void clearData()
        {
                for (auto &pair : chs_)
                        if (pair.second)
                                pair.second->clearData();
                // Reset total sampled points counter after clearing data
                JLinkDev::instance().resetPoints();
        }
};

class Monitor
{
      public:
        using ScopeMapType = std::unordered_map<std::string, std::shared_ptr<MonitorScope>>;

      private:
        std::string      name_{};
        std::vector<f32> timestamps_{};
        bool             paused_{false};
        ScopeMapType     scopes_{};

      public:
        enum class SamplingMode { HSS, POLL };
        SamplingMode samplingMode_{SamplingMode::POLL};

      private:
        void menu();

        MonitorViewMode viewMode_{MonitorViewMode::FULL};
        MonitorViewMode fftViewMode_{MonitorViewMode::FULL};
        bool            needsLayout_{true};
        float           lastAvailY_{0.0f};

      public:
        f64               linkXMin_{0.0}, linkXMax_{1.0};
        f64               lastNow_{0.0};
        f64               dataStartTime_{0.0};
        f64               pauseXMax_{-1.0};
        bool              wasPaused_{false};
        float             historySeconds_{10.0f};
        u32               maxDisplayPoints_{5000};
        bool              hssAutoPeriod_{true};
        int               maxSampleHz_{1000};
        std::atomic<bool> pendingDelete_{false};

        f32                                   actualHz_{0.0f};
        u64                                   pointAccum_{0};
        u64 lastHzTick_{get_mono_ts_ms()};
        static std::vector<Monitor *>         sInstances_;
        static std::mutex                     sMtxInstances_;

      public:
        explicit Monitor(std::string monitorName) : name_(std::move(monitorName))
        {
                LOG_I("Monitor()");
                std::lock_guard lk(sMtxInstances_);
                sInstances_.push_back(this);
        }
        Monitor()
        {
                LOG_I("Monitor()");
                std::lock_guard lk(sMtxInstances_);
                sInstances_.push_back(this);
        };
        ~Monitor()
        {
                LOG_I("~Monitor()");
                std::lock_guard lk(sMtxInstances_);
                auto            it = std::find(sInstances_.begin(), sInstances_.end(), this);
                if (it != sInstances_.end())
                        sInstances_.erase(it);
        };

        void updateDisplay();
        void clearData()
        {
                for (auto &pair : scopes_)
                        if (pair.second)
                                pair.second->clearData();
                purgeDeletedScopes();
                needsLayout_ = true;
                pointAccum_  = 0;
                actualHz_    = 0.0f;
        }

        void addPoints(u64 n) { pointAccum_ += n; }
        f32  getHz() const { return actualHz_; }

        void updateHz()
        {
                auto now  = get_mono_ts_ms();
                auto dtMs = now - lastHzTick_;
                if (dtMs >= 500) {
                        actualHz_   = static_cast<f32>(static_cast<f64>(pointAccum_) * 1000.0 / static_cast<f64>(dtMs));
                        pointAccum_ = 0;
                        lastHzTick_ = now;
                }
        }

        void markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }

        std::string     getName() { return name_; }
        int             addScope(const std::string &scopeName);
        ScopeMapType   &getScopes() { return scopes_; }
        MonitorChannel *findChannel(const std::string &scopeName, const std::string &chName);

        // 真正擦除被标记删除的 scope (在 threadFunc 安全点调用).
        void purgeDeletedScopes()
        {
                for (auto it = scopes_.begin(); it != scopes_.end();) {
                        if (it->second && it->second->isPendingDelete())
                                it = scopes_.erase(it);
                        else
                                ++it;
                }
        }

        static void clearAll()
        {
                std::lock_guard lk(sMtxInstances_);
                resetSessionTime();
                for (auto *m : sInstances_) {
                        m->linkXMin_      = 0.0;
                        m->linkXMax_      = 1.0;
                        m->lastNow_       = 0.0;
                        m->dataStartTime_ = 0.0;
                        m->clearData();
                }
                JLinkDev::instance().reqRestart();
        }
};

#endif // !MONITOR_HPP
