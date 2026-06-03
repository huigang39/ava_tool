/**
 * @file  time_series_buffer.hpp
 * @brief Unbounded GUI-side telemetry buffer with min/max LOD pyramid.
 *
 * Built around MmapVector (file-backed) for O(1) push_back / pop_front and
 * zero heap allocation. Combined with a 4×-downsampled LOD pyramid (built
 * incrementally on push), render cost stays O(maxDisplayPoints) regardless
 * of total sample count. Raw and LOD storage both grow without bound; the
 * caller is responsible for any time-based pruning.
 *
 * Threading: this class is not thread-safe. Caller serializes via mtxMonitors_.
 */
#ifndef TIME_SERIES_BUFFER_HPP
#define TIME_SERIES_BUFFER_HPP

#include "module.h"

#include "delta_ts_buffer.hpp"
#include "mmap_vector.hpp"
#include <algorithm>
#include <limits>

struct LodSample {
        f64 t;
        f32 vmin;
        f32 vmax;
};

class TimeSeriesBuffer
{
      public:
        static constexpr int kBranch    = 4;
        static constexpr int kMaxLevels = 10; // 4^10 ≈ 1M decimation

        TimeSeriesBuffer() = default;

        void clear()
        {
                rawTs_.clear();
                rawVals_.clear();
                firstAbsIdx_ = 0;
                for (int k = 0; k < kMaxLevels; ++k) {
                        levels_[k].data.clear();
                        levels_[k].pending = PendingBucket{};
                }
        }

        // -- Raw level accessors --------------------------------------------
        usize rawSize() const { return rawTs_.size(); }
        bool  rawEmpty() const { return rawTs_.empty(); }
        f64   firstTs() const { return rawTs_.empty() ? -1.0 : rawTs_.front(); }
        f64   lastTs() const { return rawTs_.empty() ? -1.0 : rawTs_.back(); }
        f64   rawTs(usize i) const { return rawTs_[i]; }
        f32   rawVal(usize i) const { return rawVals_[i]; }

        u64 firstAbsIdx() const { return firstAbsIdx_; }
        u64 endAbsIdx() const { return firstAbsIdx_ + rawTs_.size(); }

        // -- LOD level accessors --------------------------------------------
        usize lodSize(int level) const
        {
                if (level < 0 || level >= kMaxLevels)
                        return 0;
                return levels_[level].data.size();
        }
        const LodSample &lodAt(int level, usize i) const { return levels_[level].data[i]; }

        // -- Mutation --------------------------------------------------------
        void push(f32 v, f64 t)
        {
                rawTs_.push_back(t);
                rawVals_.push_back(v);
                feedLevel_(0, v, v, t);
        }

        // Drop n samples from the raw front. LOD heads are left alone so
        // historical coarse data remains queryable after raw aged out.
        void dropFront(usize n)
        {
                n = std::min(n, rawTs_.size());
                if (n == 0)
                        return;
                for (usize i = 0; i < n; ++i) {
                        rawTs_.pop_front();
                        rawVals_.pop_front();
                }
                firstAbsIdx_ += n;

                // Also drop LOD samples that are now older than the oldest remaining
                // raw sample. Without this the LOD pyramid grows without bound even
                // though the raw window is pruned — the single largest source of the
                // mmap cache ballooning over long runs.
                const f64 cutoff = rawTs_.empty() ? std::numeric_limits<f64>::infinity() : rawTs_.front();
                for (int k = 0; k < kMaxLevels; ++k) {
                        auto &data = levels_[k].data;
                        while (!data.empty() && data.front().t < cutoff)
                                data.pop_front();
                }
        }

        // Advance the raw front past samples with ts < cutoff. minKeep keeps
        // at least N raw samples (mainly to satisfy FFT). LOD heads are also
        // advanced for memory reclamation.
        void pruneByTime(f64 cutoff, usize minKeep)
        {
                while (rawTs_.size() > minKeep && rawTs_.front() < cutoff) {
                        rawTs_.pop_front();
                        rawVals_.pop_front();
                        ++firstAbsIdx_;
                }
                for (int k = 0; k < kMaxLevels; ++k) {
                        auto &data = levels_[k].data;
                        while (!data.empty() && data.front().t < cutoff)
                                data.pop_front();
                }
        }

        // -- Binary search --------------------------------------------------
        usize rawLowerBound(f64 t) const { return rawTs_.lowerBound(t); }
        usize rawUpperBound(f64 t) const { return rawTs_.upperBound(t); }

        usize lodLowerBound(int level, f64 t) const
        {
                if (level < 0 || level >= kMaxLevels)
                        return 0;
                const auto &data = levels_[level].data;
                if (data.empty())
                        return 0;
                auto it = std::lower_bound(data.begin(), data.end(), t, [](const LodSample &s, f64 v) { return s.t < v; });
                return static_cast<usize>(std::distance(data.begin(), it));
        }
        usize lodUpperBound(int level, f64 t) const
        {
                if (level < 0 || level >= kMaxLevels)
                        return 0;
                const auto &data = levels_[level].data;
                if (data.empty())
                        return 0;
                auto it = std::upper_bound(data.begin(), data.end(), t, [](f64 v, const LodSample &s) { return v < s.t; });
                return static_cast<usize>(std::distance(data.begin(), it));
        }

        // Pick the LOD level whose bucket count in [tMin, tMax] fits the budget.
        // Returns -1 to indicate the raw level is fine.
        int pickLevel(f64 tMin, f64 tMax, usize budget) const
        {
                if (budget < 2)
                        budget = 2;
                const usize bucketBudget = std::max<usize>(1, budget / 2);
                const usize rawCount     = rawCountInRange_(tMin, tMax);
                if (rawCount <= bucketBudget)
                        return -1;
                for (int k = 0; k < kMaxLevels; ++k) {
                        const auto &data = levels_[k].data;
                        if (data.empty())
                                continue;
                        const usize lo  = lodLowerBound(k, tMin);
                        const usize hi  = lodUpperBound(k, tMax);
                        const usize cnt = (hi > lo) ? (hi - lo) : 0;
                        if (cnt <= bucketBudget)
                                return k;
                }
                return kMaxLevels - 1;
        }

      private:
        struct PendingBucket {
                f32 vmin{std::numeric_limits<f32>::infinity()};
                f32 vmax{-std::numeric_limits<f32>::infinity()};
                f64 tSum{0.0};
                u32 count{0};
        };
        struct Level {
                MmapVector<LodSample> data;
                PendingBucket         pending;
        };

        void feedLevel_(int k, f32 vmin, f32 vmax, f64 t)
        {
                if (k >= kMaxLevels)
                        return;
                auto &L = levels_[k];
                if (vmin < L.pending.vmin)
                        L.pending.vmin = vmin;
                if (vmax > L.pending.vmax)
                        L.pending.vmax = vmax;
                L.pending.tSum += t;
                ++L.pending.count;
                if (L.pending.count >= static_cast<u32>(kBranch)) {
                        LodSample s;
                        s.t    = L.pending.tSum / static_cast<f64>(L.pending.count);
                        s.vmin = L.pending.vmin;
                        s.vmax = L.pending.vmax;
                        L.data.push_back(s);
                        feedLevel_(k + 1, s.vmin, s.vmax, s.t);
                        L.pending = PendingBucket{};
                }
        }

        usize rawCountInRange_(f64 tMin, f64 tMax) const
        {
                const usize lo = rawLowerBound(tMin);
                const usize hi = rawUpperBound(tMax);
                return (hi > lo) ? (hi - lo) : 0;
        }

        DeltaTsBuffer   rawTs_; // block-anchored f32-delta timestamps (compact)
        MmapVector<f32> rawVals_;
        Level           levels_[kMaxLevels];
        u64             firstAbsIdx_{0};
};

#endif // !TIME_SERIES_BUFFER_HPP
