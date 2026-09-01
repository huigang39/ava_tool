#ifndef AVA_SAMPLE_STATISTICS_HPP
#define AVA_SAMPLE_STATISTICS_HPP
#include <algorithm>
#include <cmath>
#include <vector>

namespace sample_statistics
{
struct Distribution {
        double              mean{}, m2{}, variance{}, stddev{}, median{}, p95{};
        std::vector<double> values;
        void                add(double value)
        {
                if (!std::isfinite(value))
                        return;
                values.push_back(value);
                const double delta  = value - mean;
                mean               += delta / values.size();
                m2                 += delta * (value - mean);
        }
        void finish()
        {
                if (values.empty())
                        return;
                variance            = std::max(0.0, m2 / values.size()); // population variance
                stddev              = std::sqrt(variance);
                const auto quantile = [&](double p) {
                        const double index = p * (values.size() - 1);
                        const auto   lo    = static_cast<size_t>(index);
                        std::nth_element(values.begin(), values.begin() + lo, values.end());
                        const double lower = values[lo];
                        if (index == lo || lo + 1 == values.size())
                                return lower;
                        const double upper = *std::min_element(values.begin() + lo + 1, values.end());
                        return lower + (upper - lower) * (index - lo);
                };
                median = quantile(0.5);
                p95    = quantile(0.95);
        }
};
} // namespace sample_statistics
#endif
