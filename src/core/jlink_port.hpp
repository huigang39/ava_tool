/**
 * @file  jlink_port.hpp
 * @brief JLinkPort — SEGGER J-Link debug probe communication port.
 *
 * Singleton that wraps the J-Link SDK for memory read/write and HSS
 * (High-Speed Sampling) streaming. All J-Link API calls are serialised
 * through a FairMutex to avoid USB contention.
 */
#ifndef JLINK_PORT_HPP
#define JLINK_PORT_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "module.h"

struct HssBlock {
        u32  addr;
        u32  numBytes;
        bool operator==(const HssBlock &other) const { return addr == other.addr && numBytes == other.numBytes; }
};

// FIFO ticket lock. std::mutex on Windows (SRWLOCK) is unfair: a thread that
// just released the lock and immediately re-requests it tends to win over
// already-waiting threads. The HSS sampler issues back-to-back JLink ops at
// high rates, which can starve the lower-frequency wave-gen writer and make
// its output stutter. FairMutex enforces strict acquisition order so every
// caller gets its share.
class FairMutex
{
      public:
        void lock()
        {
                std::unique_lock lk(mtx_);
                const unsigned   my = next_++;
                cv_.wait(lk, [&] { return serving_ == my; });
        }
        void unlock()
        {
                {
                        std::lock_guard lk(mtx_);
                        ++serving_;
                }
                cv_.notify_all();
        }
        bool try_lock()
        {
                std::lock_guard lk(mtx_);
                if (next_ != serving_)
                        return false;
                ++next_;
                return true;
        }

      private:
        std::mutex              mtx_{};
        std::condition_variable cv_{};
        unsigned                next_{0};
        unsigned                serving_{0};
};

class JLinkPort
{
      public:
        // J-Link HSS firmware prepends a 4-byte timestamp/sequence header to
        // every frame, regardless of the JLINK_HSS_FLAG_TIMESTAMP_US flag.
        static constexpr int kHssHeaderBytes = 4;

        static JLinkPort &instance();

        bool isOpen() const { return isOpen_; }
        bool isConnected() const { return isConnected_; }

        bool open();
        void close();
        bool connect(); // uses deviceName_ + speed_
        bool resetTarget();

        // Async wrappers: run the blocking USB operation on a detached worker so
        // the GUI thread never stalls. isBusy() reflects an in-flight operation.
        void connectAsync();
        void disconnectAsync();
        void resetAsync();
        bool isBusy() const { return busy_.load(std::memory_order_acquire); }
        bool readMem(u32 addr, u32 numBytes, void *dst);
        bool writeMem(u32 addr, u32 numBytes, const void *src);

        u32  readReg(u32 regIndex);
        bool isHalted();
        bool halt();
        bool resume();

        bool hssStart(const std::vector<HssBlock> &blocks, int periodUs);
        void hssStop();
        int  hssRead(void *buf, u32 bufSize);
        bool isHssRunning() const { return hssRunning_; }
        int  hssFrameSize() const { return hssFrameSize_; }

        // Actual sample rate (Hz). Fed by the sampler thread periodically.
        f32  actualHz() const { return hssActualHz_.load(std::memory_order_relaxed); }
        void setActualHz(f32 hz) { hssActualHz_.store(hz, std::memory_order_relaxed); }

        u64  totalPoints() const { return totalPoints_.load(std::memory_order_relaxed); }
        void addPoints(u64 n) { totalPoints_.fetch_add(n, std::memory_order_relaxed); }
        void resetPoints() { totalPoints_.store(0, std::memory_order_relaxed); }

        // Config getters/setters use a lightweight mutex (cfgMtx_) separate from the
        // J-Link I/O FairMutex. The GUI reads these every frame; using the I/O lock
        // would stall the render thread behind slow J-Link reads (low SWD speed).
        std::string deviceName() const
        {
                std::lock_guard lk(cfgMtx_);
                return deviceName_;
        }
        void setDeviceName(const std::string &n)
        {
                std::lock_guard lk(cfgMtx_);
                deviceName_ = n;
        }
        int         speed() const { return speedKHz_.load(std::memory_order_relaxed); }
        void        setSpeed(int s) { speedKHz_.store(s, std::memory_order_relaxed); }
        int        &hssPeriodUs() { return hssPeriodUs_; }
        std::string lastError() const;

        void drawUI();

      private:
        JLinkPort() = default;

        mutable FairMutex  mtx_{};       // serialises J-Link SDK I/O (held during slow reads)
        mutable std::mutex cfgMtx_{};    // guards deviceName_ only (never held during I/O)
        std::atomic<bool>  busy_{false}; // an async connect/disconnect/reset is in flight
        bool               isOpen_{false};
        bool               isConnected_{false};
        std::string        deviceName_{"STM32H745II"};
        std::atomic<int>   speedKHz_{4000};
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

#endif // !JLINK_PORT_HPP
