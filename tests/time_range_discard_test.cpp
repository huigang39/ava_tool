#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
struct Sample {
        double time;
        float val;
};
using History = std::vector<Sample>;

void check(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

History snapshot(const MonitorChannel &channel)
{
        History result;
        for (usize i = 0; i < channel.read_.rawSize(); ++i)
                result.push_back({channel.read_.rawTs(i), channel.read_.rawVal(i)});
        return result;
}

History without(History input, double minimum, double maximum)
{
        std::erase_if(input, [&](const Sample &sample) { return sample.time >= minimum && sample.time <= maximum; });
        return input;
}

void same(const MonitorChannel &channel, const History &expected, const char *message)
{
        check(channel.read_.rawSize() == expected.size(), message);
        for (usize i = 0; i < expected.size(); ++i) {
                check(channel.read_.rawTs(i) == expected[i].time, message);
                check(channel.read_.rawVal(i) == expected[i].val, message);
        }
}

void fill(MonitorChannel &channel, int count, double start = 0, double step = 1)
{
        channel.historySeconds_ = 0;
        for (int i = 0; i < count; ++i)
                channel.setRVal(float(i), start + i * step);
        channel.publishSnapshot();
}

void basicRanges()
{
        // Negative times are valid for captured/pre-trigger waveforms. Exact
        // endpoints must be inclusive, including a single-point selection
        const std::pair<double, double> intervals[] = {
            {-100, -8}, {-3, 3}, {8, 100}, {-100, 100}, {30, 40}, {0, 0}};
        for (auto [minimum, maximum] : intervals) {
                MonitorChannel channel("range");
                fill(channel, 21, -10);
                const History before = snapshot(channel);
                const History expected = without(before, minimum, maximum);
                check(channel.discardTimeRange(minimum, maximum) == before.size() - expected.size(),
                      "closed range deletion count");
                same(channel, expected, "range keeps exact surviving samples");
                check(channel.storedCount() == expected.size(), "writer removes the same samples");
                channel.publishSnapshot();
                same(channel, expected, "publish does not resurrect discarded samples");
                channel.publishSnapshot();
                same(channel, expected, "repeated publish stays stable");
        }

        MonitorChannel channel("invalid");
        fill(channel, 80, -20);
        const History before = snapshot(channel);
        const usize lodSize = channel.read_.lodSize(0);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        for (auto [minimum, maximum] : {std::pair{nan, 1.0}, std::pair{0.0, nan}, std::pair{-infinity, 1.0},
                                        std::pair{0.0, infinity}, std::pair{3.0, 2.0}}) {
                check(channel.discardTimeRange(minimum, maximum) == 0, "invalid ranges are ignored");
                same(channel, before, "invalid range preserves data");
                check(channel.read_.lodSize(0) == lodSize, "invalid range preserves LOD");
        }
        channel.discardTimeRange(2, 12);
        const History withGap = snapshot(channel);
        check(channel.discardTimeRange(3, 11) == 0, "selecting an empty gap is a no-op");
        same(channel, withGap, "empty gap does not shift time");
}

void pausedAndPruned()
{
        MonitorChannel paused("paused");
        fill(paused, 80);
        for (int i = 80; i < 100; ++i)
                paused.setRVal(float(i), i);
        const History frozen = snapshot(paused);
        paused.discardTimeRange(70, 89);
        same(paused, without(frozen, 70, 89), "deletion keeps unpublished tail frozen");
        check(paused.storedCount() == 80, "writer discards published and unpublished overlap");
        paused.publishSnapshot();
        History expected;
        for (int i = 0; i < 100; ++i)
                if (i < 70 || i > 89)
                        expected.push_back({double(i), float(i)});
        same(paused, expected, "resume publishes only retained hidden tail");

        // The frozen GUI contains a prefix which the writer has already pruned
        // Remove from that old prefix, then from the shared window and hidden
        // tail before publishing: none may corrupt the implicit publish cursor
        MonitorChannel pruned("pruned");
        fill(pruned, 5000);
        const History old = snapshot(pruned);
        pruned.historySeconds_ = 1;
        for (int i = 5000; i < 7000; ++i)
                pruned.setRVal(float(i), i);
        check(pruned.storedCount() == 4096, "writer history has pruned its old prefix");
        pruned.discardTimeRange(100, 199);
        pruned.discardTimeRange(4000, 4100);
        pruned.discardTimeRange(6200, 6300);
        same(pruned, without(without(old, 100, 199), 4000, 4100), "paused old snapshot edits stay frozen");
        pruned.publishSnapshot();
        expected.clear();
        for (int i = 2904; i < 7000; ++i)
                if ((i < 4000 || i > 4100) && (i < 6200 || i > 6300))
                        expected.push_back({double(i), float(i)});
        same(pruned, expected, "pruned reader prefix and unpublished cursor stay aligned");

        MonitorChannel spanning("spanning");
        fill(spanning, 5000);
        spanning.historySeconds_ = 1;
        for (int i = 5000; i < 7000; ++i)
                spanning.setRVal(float(i), i);
        check(spanning.discardTimeRange(100, 6300) == 6201,
              "count includes old reader prefix and unpublished writer tail once");
        spanning.publishSnapshot();
        expected.clear();
        for (int i = 6301; i < 7000; ++i)
                expected.push_back({double(i), float(i)});
        same(spanning, expected, "spanning deletion preserves the remaining writer tail");

        MonitorChannel tail("tail");
        fill(tail, 32);
        for (int i = 32; i < 64; ++i)
                tail.setRVal(float(i), i);
        const History tailFrozen = snapshot(tail);
        tail.discardTimeRange(32, 63);
        same(tail, tailFrozen, "writer-only deletion does not reveal tail");
        tail.publishSnapshot();
        same(tail, tailFrozen, "fully deleted hidden tail stays absent");
        tail.discardTimeRange(-100, 100);
        tail.setRVal(7, 1000);
        tail.publishSnapshot();
        same(tail, {{1000, 7}}, "new sampling works after deleting all history");
}

void exactTimestampsAndClone()
{
        MonitorChannel channel("precision");
        fill(channel, 1400, -3.25000013, 0.001123456789);
        History expected = snapshot(channel);
        const double minimum = expected[235].time;
        const double maximum = expected[520].time;
        expected = without(expected, minimum, maximum);
        channel.discardTimeRange(minimum, maximum);
        channel.publishSnapshot();
        same(channel, expected, "middle erase preserves timestamps across delta blocks exactly");
        const double secondMin = expected[511].time;
        const double secondMax = expected[900].time;
        expected = without(expected, secondMin, secondMax);
        channel.discardTimeRange(secondMin, secondMax);
        const float values[] = {17, 18};
        const double times[] = {20.12345678901, 20.12355678901};
        channel.pushBatch(values, times, 2);
        channel.publishSnapshot();
        expected.push_back({times[0], values[0]});
        expected.push_back({times[1], values[1]});
        same(channel, expected, "repeat deletion and batch append preserve exact timestamps");
        auto clone = channel.cloneWithHistory();
        same(*clone, expected, "clone/export snapshot contains only retained samples");
        clone->publishSnapshot();
        same(*clone, expected, "clone writer matches its edited snapshot exactly");

        // Its first publish happens after pruning, so reader/writer delta blocks
        // have different anchors. Endpoint selection still refers to GUI data
        MonitorChannel delayed("delayed");
        delayed.historySeconds_ = 0.1f;
        for (int i = 0; i < 5000; ++i)
                delayed.setRVal(float(i), 10.12345678 + i * 0.000123456789);
        delayed.publishSnapshot();
        expected = snapshot(delayed);
        const double delayedMin = expected[17].time;
        const double delayedMax = expected[1111].time;
        expected = without(expected, delayedMin, delayedMax);
        delayed.discardTimeRange(delayedMin, delayedMax);
        delayed.publishSnapshot();
        same(delayed, expected, "delete exact visible endpoints after a delayed first publish");
}

void lodAndRate()
{
        MonitorChannel channel("lod");
        channel.historySeconds_ = 0;
        for (int i = 0; i < 129; ++i)
                channel.setRVal(i == 126 ? -100000.0f : i == 127 ? 100000.0f : 5.0f, i);
        channel.publishSnapshot();
        channel.discardTimeRange(126, 127);
        for (int i = 129; i < 300; ++i)
                channel.setRVal(5, i);
        channel.publishSnapshot();
        for (int level = 0; level < TimeSeriesBuffer::kMaxLevels; ++level)
                for (usize i = 0; i < channel.read_.lodSize(level); ++i) {
                        const auto &sample = channel.read_.lodAt(level, i);
                        check(sample.vmin == 5 && sample.vmax == 5, "LOD and pending buckets contain no removed extrema");
                }

        MonitorChannel rate("rate");
        fill(rate, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        rate.setRVal(5000, 5000);
        rate.publishSnapshot();
        rate.discardTimeRange(0, 4999);
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        rate.setRVal(5001, 5001);
        check(std::isfinite(rate.getHz()) && rate.getHz() >= 0 && rate.getHz() < 100,
              "history deletion cannot underflow the cumulative sample-rate counter");

        MonitorChannel array("array");
        fill(array, 4);
        auto snapshot = std::make_shared<flasharray::Snapshot>();
        snapshot->values = {1, 2, 3, 4};
        array.arraySnapshot_ = snapshot;
        check(array.discardTimeRange(0, 3) == 0 && array.arraySnapshot_ == snapshot && array.storedCount() == 4,
              "immutable index arrays reject time-range deletion");
}
} // namespace

int main()
{
        module_init();
        try {
                basicRanges();
                pausedAndPruned();
                exactTimestampsAndClone();
                lodAndRate();
                std::cout << "PASS: time-range discard, pause/prune cursors, exact timestamps, clone, LOD and rate\n";
                return 0;
        } catch (const std::exception &error) {
                std::cerr << "FAIL: " << error.what() << '\n';
                return 1;
        }
}
