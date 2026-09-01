#ifndef DYNAMIC_WAVE_HPP
#define DYNAMIC_WAVE_HPP

#include <algorithm>
#include <cmath>

#include "ckit.h"

enum class WaveGeneratorMode : int { BASIC = 0, DYNAMIC = 1 };
enum class WaveDynamicLaw : int { FIXED = 0, LINEAR = 1, STEP = 2 };

struct WaveDynamicParam {
        WaveDynamicLaw law{WaveDynamicLaw::FIXED};
        f32            start{0.0f}, end{0.0f}, duration{1.0f}, step{1.0f}, interval{1.0f};
        bool           repeat{false};
};

inline f32
evaluateWaveDynamicParam(const WaveDynamicParam &cfg, f64 elapsedSeconds)
{
        const f64 elapsed = std::max(0.0, elapsedSeconds);
        switch (cfg.law) {
                case WaveDynamicLaw::LINEAR: {
                        const f64 duration = std::max(1.0e-6, static_cast<f64>(cfg.duration));
                        const f64 t        = cfg.repeat ? std::fmod(elapsed, duration) : std::min(elapsed, duration);
                        return static_cast<f32>(cfg.start + (cfg.end - cfg.start) * (t / duration));
                }
                case WaveDynamicLaw::STEP: {
                        const f64 interval = std::max(1.0e-6, static_cast<f64>(cfg.interval));
                        const f64 delta    = static_cast<f64>(cfg.end) - cfg.start;
                        const f64 step     = cfg.step;
                        if (delta == 0.0 || step == 0.0 || (delta > 0.0) != (step > 0.0))
                                return cfg.start;
                        const u64 transitions = static_cast<u64>(std::ceil(std::abs(delta / step)));
                        u64       state       = static_cast<u64>(std::floor(elapsed / interval));
                        state                 = cfg.repeat ? state % (transitions + 1) : std::min(state, transitions);
                        const f64 val         = cfg.start + static_cast<f64>(state) * step;
                        return static_cast<f32>(delta > 0.0 ? std::min(val, static_cast<f64>(cfg.end))
                                                            : std::max(val, static_cast<f64>(cfg.end)));
                }
                default:
                        return cfg.start;
        }
}

#endif
