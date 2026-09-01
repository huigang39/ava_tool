#include "core/sample_statistics.hpp"
#include <cassert>
#include <limits>
int
main()
{
        sample_statistics::Distribution s;
        for (double x : {1., 2., 3., 4.})
                s.add(x);
        s.add(std::numeric_limits<double>::quiet_NaN());
        s.finish();
        assert(s.values.size() == 4 && s.median == 2.5 && s.variance == 1.25);
        assert(std::abs(s.p95 - 3.85) < 1e-12 && std::abs(s.stddev - std::sqrt(1.25)) < 1e-12);
        sample_statistics::Distribution transformed;
        for (double x : {1., 2., 3., 4.})
                transformed.add(-2 * x + 10);
        transformed.finish();
        assert(transformed.median == 5 && transformed.variance == 5);
        assert(std::abs(transformed.p95 - 7.7) < 1e-12);
        sample_statistics::Distribution constant;
        constant.add(1e12);
        constant.add(1e12);
        constant.finish();
        assert(constant.stddev == 0 && constant.median == 1e12);
        sample_statistics::Distribution empty;
        empty.finish();
        assert(empty.values.empty() && empty.variance == 0);
}
