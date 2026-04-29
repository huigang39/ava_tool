#ifndef JLINK_DEV_HPP
#define JLINK_DEV_HPP

#include <mutex>
#include <string>
#include <vector>

#include "module.h"

struct HssBlock {
        u32 addr;
        u32 numBytes;
};

class JLinkDev {
      public:
        static JLinkDev &instance();

        bool isOpen() const { return isOpen_; }
        bool isConnected() const { return isConnected_; }

        bool open();
        void close();
        bool connect(); // uses deviceName_ + speed_
        bool readMem(u32 addr, u32 numBytes, void *dst);

        bool hssStart(const std::vector<HssBlock> &blocks, int periodUs);
        void hssStop();
        int  hssRead(void *buf, u32 bufSize);
        bool isHssRunning() const { return hssRunning_; }
        int  hssFrameSize() const { return hssFrameSize_; }

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

        bool               hssRunning_{false};
        int                hssPeriodUs_{1000};
        int                hssFrameSize_{0};
};

#endif // !JLINK_DEV_HPP
