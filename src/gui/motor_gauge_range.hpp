#pragma once
#include <array>
#include <cmath>

struct MotorGaugeRange {
        float  minimum, maximum;
        bool   valid() const { return std::isfinite(minimum) && std::isfinite(maximum) && minimum < maximum; }
        double span() const { return double(maximum) - minimum; }
};
inline constexpr std::array<MotorGaugeRange, 9> defaultMotorGaugeRanges{{{-3.14159265359f, 3.14159265359f},
                                                                         {-100, 100},
                                                                         {-40, 40},
                                                                         {-100, 100},
                                                                         {-100, 100},
                                                                         {0, 100},
                                                                         {0, 150},
                                                                         {0, 150},
                                                                         {0, 150}}};
