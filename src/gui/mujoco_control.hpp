#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace ava_mujoco
{
enum class Mode { Position, Velocity, PD };
struct Control {
        Mode   mode       = Mode::PD;
        double positionKp = 10, velocityKp = 10, velocityKi = 20, pdKp = 100, pdKd = 10;
        double positionTarget = 0, velocityTarget = 0, velocityFeedforward = 0, torqueFeedforward = 0;
};
inline bool
valid(const Control &c)
{
        if (static_cast<int>(c.mode) < 0 || static_cast<int>(c.mode) > 2)
                return false;
        for (double gain : {c.positionKp, c.velocityKp, c.velocityKi, c.pdKp, c.pdKd})
                if (!std::isfinite(gain) || gain < 0 || gain > 1e6)
                        return false;
        for (double val : {c.positionTarget, c.velocityTarget, c.velocityFeedforward, c.torqueFeedforward})
                if (!std::isfinite(val) || std::abs(val) > 1e6)
                        return false;
        return true;
}
inline double
activeKp(const Control &c)
{
        return c.mode == Mode::Position ? c.positionKp : c.mode == Mode::Velocity ? c.velocityKp : c.pdKp;
}
class Controller
{
        double integral_ = 0;
        Mode   lastMode_ = Mode::PD;
        double lastKi_   = 0;

      public:
        void   reset() { integral_ = 0; }
        double integral() const { return integral_; }
        double step(const Control &c,
                    double         q,
                    double         dq,
                    double         dt,
                    double         low  = -std::numeric_limits<double>::infinity(),
                    double         high = std::numeric_limits<double>::infinity())
        {
                if (c.mode != lastMode_ || c.velocityKi != lastKi_)
                        reset();
                lastMode_ = c.mode;
                lastKi_   = c.velocityKi;
                if (c.mode == Mode::PD)
                        return std::clamp(c.pdKp * (c.positionTarget - q) + c.pdKd * (c.velocityTarget - dq) +
                                              c.torqueFeedforward,
                                          low,
                                          high);
                const double reference =
                    c.mode == Mode::Position ? c.positionKp * (c.positionTarget - q) + c.velocityFeedforward : c.velocityTarget;
                const double error = reference - dq;
                if (c.velocityKi == 0)
                        integral_ = 0;
                const double candidate = integral_ + c.velocityKi * error * dt;
                const double trial     = c.velocityKp * error + candidate + c.torqueFeedforward;
                // Conditional integration prevents windup against the actual torque limits
                if (!((trial > high && error > 0) || (trial < low && error < 0)))
                        integral_ = candidate;
                return std::clamp(c.velocityKp * error + integral_ + c.torqueFeedforward, low, high);
        }
};
} // namespace ava_mujoco
