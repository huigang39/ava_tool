/**
 * @file  delta_ts_buffer.hpp
 * @brief Compact, file-backed store for a monotonic f64 timestamp series.
 *
 * Instead of one f64 (8 bytes) per sample, we keep one f64 "anchor" per fixed
 * block of kBlock samples plus one f32 "offset" (= ts - blockAnchor) per sample.
 * That roughly halves the on-disk timestamp cost (~8 B -> ~4 B per point) while
 * keeping O(1) random access — which binary search / plotting rely on.
 *
 * Precision: an offset is f32, so the within-block timing resolution is
 * ~blockSpan * 2^-23. With kBlock=256 and normal sample rates a block spans
 * well under a second, giving sub-microsecond resolution — finer than the
 * sample period, so reconstructed timestamps stay strictly increasing.
 *
 * Threading: not thread-safe; the caller serializes access (mtxMonitors_).
 */
#ifndef DELTA_TS_BUFFER_HPP
#define DELTA_TS_BUFFER_HPP

#include "module.h"

#include "mmap_vector.hpp"

class DeltaTsBuffer
{
      public:
        static constexpr usize kBlock = 256;

        usize size() const { return off_.size(); }
        bool  empty() const { return off_.empty(); }

        // Absolute timestamp at live index i. O(1).
        f64 operator[](usize i) const
        {
                const u64   abs  = frontAbs_ + i;
                const usize ablk = static_cast<usize>(abs / kBlock - anchBaseBlock_);
                return anch_[ablk] + static_cast<f64>(off_[i]);
        }
        f64 front() const { return (*this)[0]; }
        f64 back() const { return (*this)[size() - 1]; }

        void push_back(f64 ts)
        {
                const u64 abs = frontAbs_ + off_.size(); // absolute push index
                const u64 blk = abs / kBlock;
                if (blk >= anchBaseBlock_ + anch_.size())
                        anch_.push_back(ts); // first sample of a new block becomes its anchor
                const usize ablk = static_cast<usize>(blk - anchBaseBlock_);
                off_.push_back(static_cast<f32>(ts - anch_[ablk]));
        }

        void pop_front()
        {
                if (off_.empty())
                        return;
                off_.pop_front();
                ++frontAbs_;
                // Drop the anchor of a block once the live front has moved past it.
                // (pop_front advances frontAbs_ by 1, so at most one block is freed.)
                const u64 frontBlk = frontAbs_ / kBlock;
                while (anchBaseBlock_ < frontBlk && !anch_.empty()) {
                        anch_.pop_front();
                        ++anchBaseBlock_;
                }
        }

        void clear()
        {
                off_.clear();
                anch_.clear();
                frontAbs_      = 0;
                anchBaseBlock_ = 0;
        }

        // First index with value >= t (series is monotonic non-decreasing).
        usize lowerBound(f64 t) const
        {
                usize lo = 0, hi = off_.size();
                while (lo < hi) {
                        const usize mid = lo + (hi - lo) / 2;
                        if ((*this)[mid] < t)
                                lo = mid + 1;
                        else
                                hi = mid;
                }
                return lo;
        }

        // First index with value > t.
        usize upperBound(f64 t) const
        {
                usize lo = 0, hi = off_.size();
                while (lo < hi) {
                        const usize mid = lo + (hi - lo) / 2;
                        if ((*this)[mid] <= t)
                                lo = mid + 1;
                        else
                                hi = mid;
                }
                return lo;
        }

      private:
        MmapVector<f32> off_{};            // per-sample offset from its block anchor
        MmapVector<f64> anch_{};           // one anchor (absolute ts) per kBlock block
        u64             frontAbs_{0};      // absolute push-index of the live front
        u64             anchBaseBlock_{0}; // block index of anch_[0]
};

#endif // !DELTA_TS_BUFFER_HPP
