#ifndef JLINK_DEV_HPP
#define JLINK_DEV_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "module.h"

struct HssBlock {
        u32  addr;
        u32  numBytes;
        bool operator==(const HssBlock &other) const { return addr == other.addr && numBytes == other.numBytes; }
};

class JLinkDev
{
      public:
        // J-Link HSS 固件给每帧前面加 4B timestamp/sequence header,
        // 不论 Flags 是否带 JLINK_HSS_FLAG_TIMESTAMP_US 都一样, 解析时必须跳过.
        static constexpr int kHssHeaderBytes = 4;

        static JLinkDev &instance();

        bool isOpen() const { return isOpen_; }
        bool isConnected() const { return isConnected_; }

        bool open();
        void close();
        bool connect(); // uses deviceName_ + speed_
        bool readMem(u32 addr, u32 numBytes, void *dst);
        bool writeMem(u32 addr, u32 numBytes, const void *src);

        bool hssStart(const std::vector<HssBlock> &blocks, int periodUs);
        void hssStop();
        int  hssRead(void *buf, u32 bufSize);
        bool isHssRunning() const { return hssRunning_; }
        int  hssFrameSize() const { return hssFrameSize_; }

        // 实际帧率 (Hz). 由 threadFunc 周期性 setActualHz 喂进来.
        f32  actualHz() const { return hssActualHz_.load(std::memory_order_relaxed); }
        void setActualHz(f32 hz) { hssActualHz_.store(hz, std::memory_order_relaxed); }

        u64  totalPoints() const { return totalPoints_.load(std::memory_order_relaxed); }
        void addPoints(u64 n) { totalPoints_.fetch_add(n, std::memory_order_relaxed); }
        void resetPoints() { totalPoints_.store(0, std::memory_order_relaxed); }

        std::string &deviceName() { return deviceName_; }
        int         &speed() { return speedKHz_; }
        int         &hssPeriodUs() { return hssPeriodUs_; }
        std::string  lastError() const;

        void drawUI();

      private:
        JLinkDev() = default;

        mutable std::mutex mtx_{};
        bool               isOpen_{false};
        bool               isConnected_{false};
        std::string        deviceName_{"STM32H745II"};
        int                speedKHz_{4000};
        std::string        lastErr_{};

        bool              hssRunning_{false};
        int               hssPeriodUs_{1000};
        int               hssFrameSize_{0};
        std::atomic<f32>  hssActualHz_{0.0f};
        std::atomic<u64>  totalPoints_{0};
        std::atomic<bool> hssReqRestart_{false};

      public:
        void reqRestart() { hssReqRestart_.store(true); }
        bool hasRestartReq() { return hssReqRestart_.exchange(false); }
};

#endif // !JLINK_DEV_HPP
