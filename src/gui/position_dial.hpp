#pragma once

#include <cmath>

// Accumulate relative motion so crossing +/-pi never adds a full-turn jump
struct PositionDialDrag {
        static constexpr double pi    = 3.14159265358979323846;
        double                  angle = 0, target = 0;

        void begin(double position, double pointerAngle)
        {
                target = position;
                angle  = pointerAngle;
        }

        double update(double pointerAngle)
        {
                if (!std::isfinite(pointerAngle))
                        return target;
                target += std::remainder(pointerAngle - angle, 2 * pi);
                angle   = pointerAngle;
                return target;
        }
};
