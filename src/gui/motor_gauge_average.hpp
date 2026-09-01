#pragma once
#include "core/motor_controller.hpp"
#include <cmath>

/**
 * @brief Display-only moving average of SDK samples, independent of rendering frequency
 *        Never use these values for commands, mode switching or validity decisions
 */
inline AvaEthFeedback
motorGaugeAverage(const MotorController::State &state, unsigned axis)
{
        if (axis >= state.snapshot.count || axis >= AVA_ETH_MAX_AXES)
                return {};
        auto               display = state.snapshot.axes[axis];
        constexpr uint32_t valid   = AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID;
        if (!state.connected || (display.flags & valid) != valid)
                return display;
        constexpr uint64_t window                 = 200000000; // 200 ms
        constexpr float AvaEthFeedback::*fields[] = {&AvaEthFeedback::position,
                                                     &AvaEthFeedback::velocity,
                                                     &AvaEthFeedback::current,
                                                     &AvaEthFeedback::torque,
                                                     &AvaEthFeedback::vbus,
                                                     &AvaEthFeedback::motor_temperature,
                                                     &AvaEthFeedback::drive_temperature,
                                                     &AvaEthFeedback::electromagnetic_torque};
        double                           sum[8]{};
        unsigned                         count[8]{};
        auto                             add = [&](const AvaEthFeedback &sample) {
                for (unsigned i = 0; i < 8; ++i) {
                        if ((i == 5 || i == 6) && !(sample.flags & AVA_ETH_TEMPERATURE_VALID))
                                continue;
                        if (i == 7 && !(sample.flags & AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID))
                                continue;
                        const float val = sample.*fields[i];
                        if (std::isfinite(val)) {
                                sum[i] += val;
                                ++count[i];
                        }
                }
        };
        add(display);
        uint64_t previous = display.timestamp_ns;
        for (auto it = state.history.rbegin(); it != state.history.rend(); ++it) {
                const auto &sample = it->feedback;
                if (sample.axis != axis)
                        continue;
                if (sample.timestamp_ns >= previous)
                        continue; // Current snapshot is already included; ignore repeated samples
                if (display.timestamp_ns - sample.timestamp_ns > window || (sample.flags & valid) != valid ||
                    sample.mode != display.mode)
                        break;
                previous = sample.timestamp_ns;
                add(sample);
        }
        for (unsigned i = 0; i < 8; ++i)
                if (count[i] && std::isfinite(display.*fields[i]))
                        display.*fields[i] = float(sum[i] / count[i]);
        return display;
}
