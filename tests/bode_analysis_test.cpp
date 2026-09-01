#include "core/bode_analysis.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace bode_analysis;
static bool near(double a, double b) { return std::abs(a-b) < 1e-8 * std::max(1.0, std::abs(b)); }
int main() {
    auto open = analyze({{1,20,-90},{100,-20,-210}});
    assert(open.phaseMargins.size()==1 && open.gainMargins.size()==1);
    assert(near(open.phaseMargins[0].frequency,10) && near(open.phaseMargins[0].val,30));
    assert(near(open.gainMargins[0].frequency,std::pow(100,.75)) && near(open.gainMargins[0].val,10));
    auto closed = analyze({{100,-6,-140},{1,0,0},{10,6,-70}});
    assert(closed.bandwidth && near(*closed.bandwidth,std::pow(10,1.75)));
    assert(closed.peak.freq==10 && closed.peak.magDb==6 && !closed.peakAtBoundary);
    auto multiple = analyze({{1,10,-90},{10,-10,-150},{100,10,-210},{1000,-10,-270}});
    assert(multiple.phaseMargins.size()==3 && multiple.gainMargins.size()==1);
    assert(near(multiple.phaseMargins[0].val,60) && near(multiple.phaseMargins[1].val,0) && near(multiple.phaseMargins[2].val,-60));
    auto wrapped = analyze({{1,10,-170},{100,-10,170}});
    assert(wrapped.gainMargins.size()==1 && near(wrapped.gainMargins[0].frequency,10));
    assert(wrapped.phaseMargins.size()==1 && near(wrapped.phaseMargins[0].val,0));
    auto exact = analyze({{1,10,-90},{10,0,-180},{100,-10,-270}});
    assert(exact.phaseMargins.size()==1 && exact.gainMargins.size()==1);
    assert(exact.phaseMargins[0].frequency==10 && exact.gainMargins[0].frequency==10);
    auto bad = analyze({{0,0,0},{-1,0,0},{1,std::numeric_limits<double>::infinity(),0},{10,1,20},{10,2,30}});
    assert(bad.points.size()==1 && bad.points[0].magDb==2 && !bad.bandwidth);
    auto none = analyze({{1,-10,-20},{10,-20,-40}});
    assert(none.phaseMargins.empty() && none.gainMargins.empty());
    auto negative = analyze({{1,10,-180},{100,-10,-240}});
    assert(near(negative.phaseMargins[0].val,-30) && near(negative.gainMargins[0].val,-10));
    auto plateau = analyze({{1,0,-180},{10,0,-180},{100,-3,-180}});
    assert(plateau.phaseMargins.size()==1 && plateau.gainMargins.size()==1);
    assert(plateau.bandwidth && *plateau.bandwidth==100);
    assert(analyze({}).points.empty());
    std::cout << "PASS: Bode open/closed-loop metrics, log interpolation, multiple/exact/wrapped crossings, signed margins, invalid/duplicate points and finite-range results\n";
}
