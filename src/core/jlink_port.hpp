/**
 * @file  jlink_port.hpp
 * @brief JLinkPort — SEGGER J-Link debug probe communication port.
 *
 * Singleton that wraps the J-Link SDK for memory read/write, HSS
 * (High-Speed Sampling), and SEGGER RTT. All J-Link API calls
 * are
 * serialised through a FairMutex to avoid USB contention.
 */
#ifndef JLINK_PORT_HPP
#define JLINK_PORT_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ckit.h"
#include "core/firmware_match.hpp"
#include "core/remote_device.hpp"

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
// caller gets its share
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
        friend struct RttReconnectTestAccess;

      public:
        // J-Link HSS firmware prepends a 4-byte ts/sequence header to
        // every frame, regardless of the JLINK_HSS_FLAG_TIMESTAMP_US flag
        static constexpr int kHssHeaderBytes = 4;

        static JLinkPort &instance();

        bool usingRemote() const { return remoteSelected_.load() && !ava_peer::backendCall(); }
        bool isOpen() const { return usingRemote() ? ava_peer::client().connected() : isOpen_.load(std::memory_order_acquire); }
        bool isConnected() const
        {
                return usingRemote() ? ava_peer::client().connected() : isConnected_.load(std::memory_order_acquire);
        }
        void drawRemoteDevices();
        void startDeviceSharing(const std::string &name, int port, const std::string &key, bool writes);
        void connectRemoteDevice(const std::string &host, int port, const std::string &key);
        void selectLocalDevice();
        u64  targetEpoch() const { return targetEpoch_.load(std::memory_order_acquire); }
        u64  symbolEpoch() const { return symbolEpoch_.load(std::memory_order_acquire); }

        bool open();
        void close();
        bool connect(bool resetOnConnect = false); // uses deviceName_ + speed_
        bool resetTarget(u64 epoch = 0);
        // Programs BIN/HEX directly through the DLL and ELF/AXF PT_LOAD
        // segments through the flash download API. This call is blocking and
        // serialised with all other J-Link traffic; invoke it from a worker
        bool programFile(const std::string                &path,
                         u32                               binAddress,
                         bool                              eraseChip,
                         bool                              resetAfter,
                         std::string                      &error,
                         const std::function<void(float)> &onProgress = {});
        /**
         * @brief Runs SEGGER Commander with the current probe serial number and target
         * Leaves the target halted and the application disconnected after erase
         */
        bool eraseFlash(const std::string &expectedDevice,
                        u64                expectedEpoch,
                        bool               entireChip,
                        uint32_t           sectorMask,
                        int                speedKHz,
                        std::string       &error,
                        std::string       &logPath);

        /**
         * @brief Async wrappers: run the blocking USB operation on a detached worker so
         * the GUI thread never stalls. isBusy() reflects an in-flight operation
         */
        void connectAsync(bool resetOnConnect = false);
        void disconnectAsync();
        void resetAsync();
        bool isBusy() const { return busy_.load(std::memory_order_acquire); }
        void setFirmwareSource(uintptr_t owner, std::optional<firmware_match::Image> image, bool loading = false);
        void removeFirmwareSource(uintptr_t owner);
        firmware_match::Status firmwareStatus() const;
        /**
         * @brief 显式放行本次连接的固件不匹配, 换文件或设备后失效
         */
        bool setFirmwareMismatchOverride(bool enabled);
        bool readMem(u32 addr, u32 numBytes, void *dst);
        bool readFlash(u32                               addr,
                       u32                               numBytes,
                       std::vector<uint8_t>             &out,
                       std::string                      &error,
                       const std::atomic<bool>          &cancel,
                       const std::function<void(float)> &progress);
        bool writeMem(u32 addr, u32 numBytes, const void *src);
        bool readMemAtEpoch(u64 epoch, u32 addr, u32 numBytes, void *dst);
        bool writeMemAtEpoch(u64 epoch, u32 addr, u32 numBytes, const void *src);
        /**
         * @brief Atomically with respect to other host-side J-Link operations, preserve
         * every bit outside [bitOffset, bitOffset + bitSize) in the storage unit
         */
        bool writeMemBitfield(u32 addr, u32 numBytes, const void *encodedValue, u32 bitOffset, u32 bitSize, u64 epoch = 0);

        u32  readReg(u32 regIndex);
        bool isHalted();
        bool halt();
        bool resume();

        bool hssStart(const std::vector<HssBlock> &blocks, int periodUs, u64 epoch = 0);
        void hssStop();
        int  hssRead(void *buf, u32 bufSize);
        bool isHssRunning() const
        {
                return usingRemote() ? remoteSampling_.load() && ava_peer::client().connected() : hssRunning_;
        }
        int hssFrameSize() const { return usingRemote() ? remoteFrameSize_.load() : hssFrameSize_; }

        /**
         * @brief Actual sample rate (Hz). Fed by the sampler thread periodically
         */
        double sampleBytesPerSecond() const
        {
                return isConnected() && get_mono_ts_ms() - sampleRateTick_.load(std::memory_order_relaxed) < 1500
                           ? sampleBytesPerSecond_.load(std::memory_order_relaxed)
                           : 0;
        }
        void setSampleBytesPerSecond(double rate)
        {
                sampleBytesPerSecond_.store(rate, std::memory_order_relaxed);
                sampleRateTick_.store(get_mono_ts_ms(), std::memory_order_relaxed);
        }
        f32  actualHz() const { return hssActualHz_.load(std::memory_order_relaxed); }
        void setActualHz(f32 hz) { hssActualHz_.store(hz, std::memory_order_relaxed); }

        u64  totalPoints() const { return totalPoints_.load(std::memory_order_relaxed); }
        void addPoints(u64 n) { totalPoints_.fetch_add(n, std::memory_order_relaxed); }
        void resetPoints() { totalPoints_.store(0, std::memory_order_relaxed); }

        /**
         * @brief Config getters/setters use a lightweight mutex (cfgMtx_) separate from the
         *        J-Link I/O FairMutex. The GUI reads these every frame; using the I/O lock
         *        would stall the render thread behind slow J-Link reads (low SWD speed)
         */
        std::string deviceName() const
        {
                std::lock_guard lk(cfgMtx_);
                return deviceName_;
        }
        void setDeviceName(const std::string &n)
        {
                std::lock_guard lk(cfgMtx_);
                if (deviceName_ != n) {
                        deviceName_ = n;
                        configModified_.store(true, std::memory_order_release);
                }
        }
        int  speed() const { return speedKHz_.load(std::memory_order_relaxed); }
        void setSpeed(int s)
        {
                if (speedKHz_.exchange(s, std::memory_order_relaxed) != s)
                        configModified_.store(true, std::memory_order_release);
        }
        int        &hssPeriodUs() { return hssPeriodUs_; }
        std::string prevError() const;

        void drawUI(bool stacked = false);
        void drawWindows();
        void drawDllSettingsMenu();
        void openTraceWindow();
        void configureDwtWriteTrace(u32 address, u32 size, const std::string &name);
        void setTraceAddressResolver(std::function<std::string(u32)> resolver);
        void saveSession(void *node) const;
        void loadSession(const void *node);
        bool isConfigModified() const { return configModified_.load(std::memory_order_acquire); }
        void clearConfigModified() { configModified_.store(false, std::memory_order_release); }

      private:
        JLinkPort() = default;
        ~JLinkPort();
        std::atomic<bool> remoteSelected_{false}, remoteSampling_{false};
        std::atomic<int>  remoteFrameSize_{0};
        bool              remoteWindowOpen_{false};
        struct RemoteUiResult {
                std::string                 message;
                std::vector<ava_peer::Peer> peers;
                bool                        error = false;
        };
        std::future<RemoteUiResult> remoteJob_;
        // Keep a routed operation and its generation check on the same source.
        // Export callbacks use the physical device and must bypass this lock,
        // including when this instance connects to its own exported endpoint.
        mutable std::recursive_mutex           routeMtx_;
        std::unique_lock<std::recursive_mutex> lockRoute() const
        {
                std::unique_lock<std::recursive_mutex> lock(routeMtx_, std::defer_lock);
                if (!ava_peer::backendCall())
                        lock.lock();
                return lock;
        }

        mutable FairMutex  mtx_{};       // serialises J-Link SDK I/O (held during slow reads)
        mutable std::mutex cfgMtx_{};    // guards deviceName_ only (never held during I/O)
        std::atomic<bool>  busy_{false}; // an async connect/disconnect/reset is in flight
        std::atomic<bool>  isOpen_{false};
        std::atomic<bool>  isConnected_{false};
        // Incremented after target connection/reset and symbol-table changes. Clients
        // whose target-side control blocks live in volatile RAM use this to
        // re-submit their configuration after the MCU has restarted
        std::atomic<u64>                      targetEpoch_{0};
        std::atomic<u64>                      symbolEpoch_{0};
        std::atomic<u64>                      deviceEpoch_{0};
        std::string                           deviceName_{""};
        std::string                           connectedDeviceName_;
        std::atomic<int>                      speedKHz_{4000};
        std::atomic<bool>                     configModified_{false};
        std::string                           prevErr_{};
        firmware_match::Gate                  firmwareGate_;
        mutable std::mutex                    firmwareStatusMtx_;
        firmware_match::Status                firmwareStatus_;
        std::chrono::steady_clock::time_point firmwareCheckedAt_{};
        u64                                   firmwareCheckedEpoch_{};
        bool                                  firmwareAllowsAccessLocked(bool refresh);

        struct DllCandidate {
                std::string path;
                std::string version;
        };
        mutable std::mutex        dllMtx_{};
        std::string               selectedDllPath_{}; // empty = bundled/default search
        std::string               loadedDllPath_{};   // resolved path of the DLL actually loaded
        std::vector<DllCandidate> dllCandidates_{};
        std::string               dllScanLocation_{};
        std::atomic<bool>         dllScanning_{false};
        std::atomic<bool>         dllScanCancel_{false};
        std::atomic<bool>         dllScanCompleted_{false};
        std::thread               dllScanThread_{};
        void                      startDllScan();
        std::string               selectedDllPath() const;

        static constexpr int kMaxReadFails = 10;
        std::atomic<int>     readFailCount_{0};
        // The public flags can be cleared after a USB/firmware-update failure
        // while the J-Link DLL still owns its old USB session. The next connect
        // must close that SDK session even though isOpen_ is already false
        std::atomic<bool> sdkSessionDirty_{false};

        void recordTransportResultLocked(bool success, const char *operation);

        bool                hssRunning_{false};
        int                 hssPeriodUs_{1000};
        int                 hssFrameSize_{0};
        std::atomic<f32>    hssActualHz_{0.0f};
        std::atomic<double> sampleBytesPerSecond_{0};
        std::atomic<u64>    sampleRateTick_{0};
        std::atomic<u64>    totalPoints_{0};
        std::atomic<bool>   hssReqRestart_{false};

        enum class RttStatus { Stopped, Searching, Running, UpChannelUnavailable, Disconnected, Error };

        char                    rttControlBlock_[32]{};
        char                    rttTxBuf_[512]{};
        int                     rttUpChannel_{0};
        int                     rttDownChannel_{0};
        bool                    rttAppendNewline_{true};
        bool                    rttWindowOpen_{false};
        bool                    rttWindowFocusRequested_{false};
        bool                    rttAutoScroll_{true};
        std::atomic<bool>       rttActive_{false};
        std::atomic<bool>       rttResumeRequested_{false}; // enabled by START, cleared only by explicit STOP
        std::atomic<bool>       rttReaderRunning_{false};
        std::atomic<RttStatus>  rttStatus_{RttStatus::Stopped};
        std::atomic<int>        rttUpBufferCount_{-1};
        std::atomic<int>        rttDownBufferCount_{-1};
        std::atomic<u64>        rttRxBytes_{0};
        std::atomic<u64>        rttTxBytes_{0};
        std::thread             rttThread_{};
        mutable std::mutex      rttLifecycleMtx_{};
        mutable std::mutex      rttDataMtx_{};
        mutable std::mutex      rttWakeMtx_{};
        std::condition_variable rttWakeCv_{};
        std::string             rttRxLog_{};
        std::string             rttTxPending_{};
        std::string             rttError_{};

        bool        rttStart(bool resumeOnly = false);
        void        rttStop();
        void        rttStopImpl(); // caller holds rttLifecycleMtx_
        void        rttReaderLoop();
        bool        rttSend(const char *data, usize size);
        std::string rttRxSnapshot() const;
        std::string rttErrorSnapshot() const;
        usize       rttTxPendingBytes() const;
        void        rttClearRx();
        void        setRttError(const std::string &error);
        void        drawRttWindow();

        enum class SwoStatus { Stopped, Running, Disconnected, Error };
        enum class TraceEventKind { PcSample, Exception, DataWrite };

        struct TraceEvent {
                TraceEventKind kind{TraceEventKind::Exception};
                double         timeUs{0.0};
                u32            pc{0};
                u64            val{0};
                u16            exceptionNumber{0};
                u8             action{0};
                u8             valueSize{0};
        };

        // Avoid J-Link's blocking CPU-clock auto measurement on STM32H7
        int                                   swoCpuMHz_{200};
        int                                   swoSpeedKHz_{1000};
        int                                   swoItmPort_{0};
        int                                   swoActivePort_{0};
        bool                                  swoExceptionTrace_{true};
        bool                                  swoPcSampling_{true};
        int                                   swoPcSampleRate_{1};
        bool                                  swoWatchWrite_{false};
        char                                  swoWatchAddress_[24]{"0x20000000"};
        char                                  swoWatchName_[128]{};
        int                                   swoWatchSize_{4};
        bool                                  swoWindowOpen_{false};
        bool                                  swoWindowFocusRequested_{false};
        bool                                  swoAutoScroll_{true};
        bool                                  swoTimelineFollow_{true};
        int                                   swoTimelineScrollFrames_{3};
        std::atomic<bool>                     swoActive_{false};
        std::atomic<bool>                     swoReaderRunning_{false};
        std::atomic<SwoStatus>                swoStatus_{SwoStatus::Stopped};
        std::atomic<u64>                      swoRxBytes_{0};
        std::atomic<u64>                      swoOverflowPackets_{0};
        std::thread                           swoThread_{};
        mutable std::mutex                    swoLifecycleMtx_{};
        mutable std::mutex                    swoDataMtx_{};
        mutable std::mutex                    swoWakeMtx_{};
        std::condition_variable               swoWakeCv_{};
        std::string                           swoTextLog_{};
        std::vector<u8>                       swoDecodePending_{};
        std::deque<TraceEvent>                swoEvents_{};
        std::vector<TraceEvent>               swoTimelineView_{};
        std::unordered_map<u32, u64>          swoPcSamples_{};
        u64                                   swoPcSampleTotal_{0};
        u32                                   swoLastWatchPc_{0};
        double                                swoLastTimelineUs_{0.0};
        std::chrono::steady_clock::time_point swoStartTime_{};
        std::string                           swoError_{};
        std::function<std::string(u32)>       traceAddressResolver_{};

        bool swoTargetStateSaved_{false};
        bool swoWatchStateSaved_{false};
        bool swoUsesStm32H7SystemSwo_{false};
        u32  swoSavedDemcr_{0};
        u32  swoSavedDwtCtrl_{0};
        u32  swoSavedDwtCyccnt_{0};
        u32  swoSavedItmTcr_{0};
        u32  swoSavedTpiuAcpr_{0};
        u32  swoSavedTpiuSppr_{0};
        u32  swoSavedTpiuFfcr_{0};
        u32  swoSavedH7DbgMcuCr_{0};
        u32  swoSavedH7SwoCodr_{0};
        u32  swoSavedH7SwoSppr_{0};
        u32  swoSavedH7SwtfCtrl_{0};
        u32  swoSavedDwtComp0_{0};
        u32  swoSavedDwtMask0_{0};
        u32  swoSavedDwtFunction0_{0};

        bool        swoStart();
        void        swoStop();
        void        swoStopImpl(); // caller holds swoLifecycleMtx_
        void        swoReaderLoop();
        void        swoConsumeRaw(const u8 *data, usize size);
        bool        swoConfigureHardwareLocked();
        void        swoRestoreHardwareLocked();
        std::string resolveTraceAddress(u32 address) const;
        std::string swoTextSnapshot() const;
        std::string swoErrorSnapshot() const;
        void        swoClear();
        void        setSwoError(const std::string &error);
        void        drawSwoTraceWindow();

      public:
        void reqRestart() { hssReqRestart_.store(true); }
        bool hasRestartReq() { return hssReqRestart_.exchange(false); }
};

#endif // !JLINK_PORT_HPP
