#ifndef MONITOR_HPP
#define MONITOR_HPP

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "module.h"

extern std::atomic<bool> g_monitorPaused;

struct ChannelDropPayload {
        char name[128];
        u64  addr;
        char type[8];   // "F32" / "U32" / "I32"
        char device[8]; // "LOCAL" / "FSA" / "JLINK"
};

class MonitorChannel
{
      public:
        enum class DeviceEnum {
                LOCAL,
                FSA,
                JLINK,
        };

        static constexpr usize kMaxSamples = 4096;

      private:
        std::string name_{};
        std::string type_{};
        usize       addr_{};
        f32         rVal_{}, wVal_{};
        std::string device_{};

        std::deque<f32>    rVals_{}, wVals_{};
        mutable std::mutex valMutex_{};

        u64                                   sampleCount_{0};
        u64                                   lastRateCount_{0};
        std::chrono::steady_clock::time_point lastRateTime_{};
        bool                                  rateInited_{false};
        f32                                   sampleHz_{0.0f};

        f32  color_[4]{1.0f, 1.0f, 1.0f, 1.0f};
        bool useAutoColor_{true};
        f32  lineWeight_{1.5f};
        int  plotStyle_{0}; // 0 = Line, 1 = Line+Markers

        shm_t shm_{};

      public:
        explicit MonitorChannel(std::string chName) : name_(std::move(chName)) {}
        MonitorChannel()  = default;
        ~MonitorChannel() = default;

        std::string &getName() { return name_; }
        void         setName(const std::string &name) { name_ = name; }
        std::string &getType() { return type_; }
        void         setType(const std::string &type) { type_ = type; }
        usize       &getAddr() { return addr_; }
        void         setAddr(const usize addr) { addr_ = addr; }
        std::string &getDevice() { return device_; }
        void         setDevice(const std::string &device) { device_ = device; }
        shm_t       &getShm() { return shm_; }

        f32 &getRVal() { return rVal_; }
        void setRVal(const f32 val)
        {
                std::lock_guard lk(valMutex_);
                rVal_ = val;
                rVals_.push_back(val);
                if (rVals_.size() > kMaxSamples)
                        rVals_.pop_front();

                ++sampleCount_;
                using clock     = std::chrono::steady_clock;
                const auto now  = clock::now();
                if (!rateInited_) {
                        lastRateTime_  = now;
                        lastRateCount_ = sampleCount_;
                        rateInited_    = true;
                } else {
                        const auto dtMs =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRateTime_).count();
                        if (dtMs >= 500) {
                                const u64 dCount = sampleCount_ - lastRateCount_;
                                sampleHz_        = static_cast<f32>(dCount * 1000.0 / static_cast<double>(dtMs));
                                lastRateTime_    = now;
                                lastRateCount_   = sampleCount_;
                        }
                }
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

        f32 &getWVal() { return wVal_; }
        void setWVal(const f32 val)
        {
                std::lock_guard lk(valMutex_);
                wVal_ = val;
                wVals_.push_back(val);
                if (wVals_.size() > kMaxSamples)
                        wVals_.pop_front();
        }

        void copyRVals(std::vector<f32> &out) const
        {
                std::lock_guard lk(valMutex_);
                out.assign(rVals_.begin(), rVals_.end());
        }
};

class MonitorScope
{
      public:
        enum class DrawEnum {
                PLOT,
                TABLE,
        };
        using ChannelMapType = std::unordered_map<std::string, std::unique_ptr<MonitorChannel>>;

      private:
        std::string    name_{};
        ChannelMapType chs_{};
        DrawEnum       e_draw{};

        void tableDraw();
        void tableMenu();

        void plotDraw() const;
        void plotMenu();

        void drawTableRow(const std::string &chName, std::unique_ptr<MonitorChannel> &ch);

      public:
        static void shmInit(MonitorChannel &ch);
        explicit MonitorScope(std::string scopeName) : name_(std::move(scopeName)) {}
        MonitorScope()  = default;
        ~MonitorScope() = default;

        void menu();
        void draw();

        int addChannel(const std::string &chName);
        int setValue(const std::string &chName, f32 val);

        std::string     getName() { return name_; }
        ChannelMapType &getChannels() { return chs_; }
        DrawEnum       &getDraw() { return e_draw; }
        void            setDraw(const DrawEnum d) { e_draw = d; }
        MonitorChannel *findChannel(const std::string &chName);
};

class Monitor
{
      public:
        using ScopeMapType = std::unordered_map<std::string, std::unique_ptr<MonitorScope>>;

      private:
        std::string      name_{};
        std::vector<f32> timestamps_{};
        bool             paused_{false};
        ScopeMapType     scopes_{};

        void menu();

      public:
        explicit Monitor(std::string monitorName) : name_(std::move(monitorName)) { print_info(true, "Monitor()"); }
        Monitor() { print_info(true, "Monitor()"); };
        ~Monitor() { print_info(true, "~Monitor()"); };

        void updateDisplay();

        std::string     getName() { return name_; }
        int             addScope(const std::string &scopeName);
        ScopeMapType   &getScopes() { return scopes_; }
        MonitorChannel *findChannel(const std::string &scopeName, const std::string &chName);
};

#endif // !MONITOR_HPP
