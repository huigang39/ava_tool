#pragma once

#include "../../sdk/ava_eth/include/ava/sdk.h"
#include "core/motor_jlink.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A single actor owns the SDK handle. The GUI never waits for EtherCAT IO
class MotorController
{
      public:
        struct Config {
                std::string         library, device;
                uint32_t            backend = AVA_ETH_USER_SIM, axes = 1, period_us = 1000, watchdog_ms = 300;
                bool                relaxed = false;
                motor_jlink::Config jlink;
        };
        enum class Action { Connect, Scan, Apply, SetMode, Special, Enable, Disable, ResetFault, Stop, Disconnect, WritePid };
        struct Command {
                Action              action = Action::Stop;
                Config              config;
                AvaEthTarget        target{};
                uint32_t            axis         = 0;
                uint32_t            special_cmd  = 0;
                bool                interpolate  = false;
                float               max_velocity = 1, max_acceleration = 2;
                std::string         reason;
                AvaEthPidParameters pid{};
                uint32_t            pidMask = 0;
        };
        struct Sample {
                double         seconds;
                AvaEthFeedback feedback;
                AvaEthTarget   target;
        };
        struct Device {
                std::string path, name, description;
        };
        static std::vector<Device> networkAdapters();
        struct State {
                bool              connected = false, busy = false, error = false, diagnostics_valid = false;
                bool              elevation_required = false;
                uint64_t          elevation_revision = 0;
                uint32_t          backend            = 0;
                AvaEthInfo        info{};
                AvaEthSnapshot    snapshot{};
                AvaEthDiagnostics diagnostics{};
                std::array<AvaEthTarget, AVA_ETH_MAX_AXES> targets{};
                std::array<uint64_t, AVA_ETH_MAX_AXES>     mode_revisions{};
                std::array<uint64_t, AVA_ETH_MAX_AXES>     target_sync_revisions{};
                std::vector<Device>                        devices;
                std::deque<std::string>                    events;
                std::deque<Sample>                         history;
                std::string                                message = "Ready / 就绪";
                AvaEthPidParameters                        pid{};
                Config                                     pidConfig;
                uint32_t                                   pidAxis     = 0;
                uint64_t                                   pidRevision = 0;
                uint32_t                                   pidSentMask = 0;
                bool                                       pidPending  = false;
        };

        MotorController();
        /**
         * @brief Extracts the executable's bundled runtime on Windows; no driver installation
         */
        static std::string bundledLibrary();
        ~MotorController();
        MotorController(const MotorController &)            = delete;
        MotorController &operator=(const MotorController &) = delete;
        void             post(Command command);
        void             pulse(uint32_t selectedAxis);
        State            state() const;
        void             shutdown();

      private:
        void                                  run();
        mutable std::mutex                    mutex_;
        std::condition_variable               wake_;
        std::deque<Command>                   commands_;
        State                                 state_;
        bool                                  quitting_ = false, processing_ = false;
        uint32_t                              selectedAxis_ = 0;
        std::chrono::steady_clock::time_point lastPulse_    = std::chrono::steady_clock::now();
        std::thread                           worker_;
};
