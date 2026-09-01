#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace bode_analysis
{
struct Point {
        double freq{}, magDb{}, phaseDeg{};
};
struct Margin {
        double frequency{}, val{};
};
struct Result {
        std::vector<Point>    points;
        std::vector<Margin>   phaseMargins, gainMargins;
        std::optional<double> bandwidth;
        Point                 peak{};
        bool                  peakAtBoundary{};
};

/**
 * @brief Frequency-response samples use Hz. Crossings interpolate linearly in dB/deg
 *        against log10(Hz); no extrapolation beyond the measured frequency range
 */
inline Result
analyze(std::vector<Point> points)
{
        std::erase_if(points, [](const Point &p) {
                return !std::isfinite(p.freq) || p.freq <= 0 || !std::isfinite(p.magDb) || !std::isfinite(p.phaseDeg);
        });
        std::stable_sort(points.begin(), points.end(), [](const Point &a, const Point &b) { return a.freq < b.freq; });
        Result out;
        for (const auto &p : points) {
                if (!out.points.empty() && out.points.back().freq == p.freq)
                        out.points.back() = p;
                else
                        out.points.push_back(p);
        }
        if (out.points.empty())
                return out;
        auto peak = std::max_element(
            out.points.begin(), out.points.end(), [](const Point &a, const Point &b) { return a.magDb < b.magDb; });
        out.peak           = *peak;
        out.peakAtBoundary = peak == out.points.begin() || peak == out.points.end() - 1;
        for (size_t i = 1; i < out.points.size(); ++i) {
                const double previous  = out.points[i - 1].phaseDeg;
                const double delta     = out.points[i].phaseDeg - previous;
                const double step      = std::isfinite(delta)
                                             ? delta
                                             : std::remainder(out.points[i].phaseDeg, 360.0) - std::remainder(previous, 360.0);
                out.points[i].phaseDeg = previous + std::remainder(step, 360.0);
        }
        const auto append = [](std::vector<Margin> &margins, double frequency, double val) {
                if (margins.empty() || std::abs(std::log(frequency / margins.back().frequency)) > 1e-10)
                        margins.push_back({frequency, val});
        };
        const double cutoff = out.points.front().magDb - 3.0;
        for (size_t i = 0; i < out.points.size(); ++i) {
                const auto &a = out.points[i];
                if (a.magDb == 0 && (i == 0 || out.points[i - 1].magDb != 0))
                        append(out.phaseMargins, a.freq, std::remainder(180.0 + a.phaseDeg, 360.0));
                if (std::abs(std::remainder(a.phaseDeg - 180.0, 360.0)) < 1e-10 &&
                    (i == 0 || out.points[i - 1].phaseDeg != a.phaseDeg))
                        append(out.gainMargins, a.freq, -a.magDb);
                if (i + 1 == out.points.size())
                        break;
                const auto &b         = out.points[i + 1];
                const auto  frequency = [&](double t) {
                        return std::pow(10.0, std::log10(a.freq) + t * (std::log10(b.freq) - std::log10(a.freq)));
                };
                if ((a.magDb < 0 && b.magDb > 0) || (a.magDb > 0 && b.magDb < 0)) {
                        const double t = -a.magDb / (b.magDb - a.magDb);
                        append(out.phaseMargins,
                               frequency(t),
                               std::remainder(180.0 + a.phaseDeg + t * (b.phaseDeg - a.phaseDeg), 360.0));
                }
                const double lower = std::min(a.phaseDeg, b.phaseDeg), upper = std::max(a.phaseDeg, b.phaseDeg);
                const double target = 180.0 + 360.0 * std::ceil((lower - 180.0) / 360.0);
                if (target > lower && target < upper) {
                        const double t = (target - a.phaseDeg) / (b.phaseDeg - a.phaseDeg);
                        append(out.gainMargins, frequency(t), -(a.magDb + t * (b.magDb - a.magDb)));
                }
                if (!out.bandwidth && a.magDb > cutoff && b.magDb <= cutoff)
                        out.bandwidth = frequency((cutoff - a.magDb) / (b.magDb - a.magDb));
        }
        return out;
}
} // namespace bode_analysis
