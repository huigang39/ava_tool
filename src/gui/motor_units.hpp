#pragma once
#include <cmath>
namespace motor_units
{
inline constexpr double pi = 3.14159265358979323846;
inline double
positionScale(int unit)
{
        return unit == 1 ? 180.0 / pi : 1.0;
}
inline double
velocityScale(int unit)
{
        return unit == 1 ? 180.0 / pi : unit == 2 ? 30.0 / pi : 1.0;
}
inline const char *
positionLabel(int unit)
{
        return unit == 1 ? "deg" : "rad";
}
inline const char *
velocityLabel(int unit)
{
        return unit == 1 ? "deg/s" : unit == 2 ? "rpm" : "rad/s";
}
struct Turns {
        double count, angle;
};
inline Turns
splitTurns(double position)
{
        const double count = std::floor((position + pi) / (2 * pi));
        return {count, position - count * 2 * pi};
}
} // namespace motor_units
