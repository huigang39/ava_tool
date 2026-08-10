/**
 * @file  monitor_scope.hpp
 * @brief MonitorScope — a group of channels plotted on the same axes.
 *
 * Each scope can render in either PLOT or TABLE mode and optionally
 * shows an FFT view side-by-side with the time-domain plot.
 */
#ifndef MONITOR_SCOPE_HPP
#define MONITOR_SCOPE_HPP

#include "gui/monitor_channel.hpp"
#include "gui/monitor_types.hpp"
#include "module.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Monitor;

class MonitorScope
{
      public:
        Monitor *parent_{nullptr};
        enum class DrawEnum {
                PLOT,
                TABLE,
        };
        using ChannelMapType = std::unordered_map<std::string, std::shared_ptr<MonitorChannel>>;
        const std::string     &getName() const { return name_; }
        const std::string     &getLabel() const { return label_.empty() ? name_ : label_; }
        void                   setLabel(const std::string &l) { label_ = l; }
        void                   setDraw(DrawEnum d) { e_draw = d; }
        DrawEnum               getDraw() const { return e_draw; }
        f32                   &getHeight() { return height_; }
        bool                  &getShowFft() { return showFft_; }
        bool                  &getFftBars() { return fftBars_; }
        bool                  &getShowSidePanel() { return showSidePanel_; }
        int                   &getFftPoints() { return fftPoints_; }
        int                   &getFftPeakCount() { return fftPeakCount_; }
        ChannelMapType        &getChannels() { return chs_; }
        std::set<std::string> &getExpandedGroups() { return expandedGroups_; }
        void                   reinitFft(int newPoints);

      private:
        std::string           name_{};
        std::string           label_{}; // User-facing alias; falls back to name_ when empty
        char                  renameBuf_[64]{};
        ChannelMapType        chs_{};
        DrawEnum              e_draw{DrawEnum::PLOT};
        f32                   height_{200.0f};
        bool                  showFft_{false};
        bool                  fftBars_{false};      // FFT render style: false = line, true = bar chart
        bool                  showSidePanel_{true}; // show the right-side Stats/Peaks table in plot view
        int                   fftPoints_{1024};
        int                   fftPeakCount_{5};
        fft_t                 fft_;
        std::vector<f32>      fftInBuf_;
        std::vector<f32>      fftMagF32_;
        std::vector<f64>      fftMagBuf_;
        std::vector<f32>      fftOutBuf_;
        std::vector<f32>      fftLoBuf_;
        std::atomic<bool>     pendingDelete_{false};
        std::vector<f64>      dxs_{};
        std::vector<f64>      dys_{};
        bool                  isManualHeight_{false};
        bool                  paused_{false};
        int                   lastSelectedIndex_{-1};
        std::set<std::string> selectedGroupPaths_{};
        bool                  forceManualTableOrder_{false};
        // Full paths (e.g. "foo.bar") of group nodes the user has expanded;
        // persisted across sessions via Gui save/load.
        bool                  hidden_{false};
        std::set<std::string> expandedGroups_{};
        // Monotonic counter handing each new channel an insertion-order index so
        // the table/plot can display channels in the order they were added (e.g.
        // matching CSV column order) instead of hash order.
        i64 nextChannelOrder_{0};
        // Display order of this scope within its Monitor (user-reorderable).
        i64         order_{0};
        int         moveDir_{0};      // pending reorder request (-1 up, +1 down), consumed by Monitor
        std::string pendingSwapWith_; // drag-drop swap target scope name, consumed by Monitor

        bool pendingAxisReset_{false};
        struct Peak {
                f64 freq;
                f64 mag;
        };
        std::map<std::string, std::vector<Peak>> channelPeaks_;

        struct Stats {
                f64   rms{0};
                f64   max{0};
                f64   min{0};
                f64   mean{0};
                f64   pkpk{0};
                usize count{0};
        };
        std::map<std::string, Stats> channelStats_;

        // FFT results published by the background FFT worker thread (fftWorkerStep)
        // and consumed by the GUI thread in plotDraw — so the expensive transform
        // no longer runs on the render thread and stops dragging down FPS.
        struct FftResult {
                std::vector<f64>  freqs;
                std::vector<f64>  mags;
                std::vector<Peak> peaks;
                f64               df{0};
        };
        std::map<std::string, FftResult> fftPublished_;   // chName -> latest result
        std::mutex                       fftPubMtx_;      // guards fftPublished_ (worker write / GUI read)
        std::mutex                       fftObjMtx_;      // guards fft_ + fft buffers (worker exec / reinit)
        std::atomic<f64>                 fftWinMin_{0.0}; // current time window the GUI wants transformed
        std::atomic<f64>                 fftWinMax_{1.0};
        std::atomic<u64>                 fftLastRunMs_{0};

        void tableDraw();
        void tableMenu();

        void plotDraw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        void plotMenu();

        void drawTableRow(const std::string               &chName,
                          std::shared_ptr<MonitorChannel> &ch,
                          int                              idx,
                          const std::vector<std::string>  &allKeys,
                          const std::string               &displayLabel = {});

        // Reorder channel `src` so it sits at `dst`'s display position, then
        // renumber all channels' display order sequentially. Used by drag-reorder.
        void reorderChannelTo(const std::string &src, const std::string &dst);
        // Move every channel under the group path `srcGroup` (a struct/array) so the group
        // sits just before `dst`'s display position, preserving the members' relative order.
        void reorderGroupBefore(const std::string &srcGroup, const std::string &dst);
        // The lowest-display-order leaf under `groupPath` — the group's anchor position.
        std::string groupAnchorKey(const std::string &groupPath) const;
        // Transfer the channel(s) described by a DND_CHANNEL_MOVE payload from another scope
        // into this one. No-op when the payload originates from this scope.
        bool moveChannelsFrom(ChannelMovePayload *data, bool copy);
        // Accept a DND_CHANNEL_MOVE payload: reorder inside this scope, move across scopes.
        void applyChannelMoveDrop(ChannelMovePayload *data, const std::string &dst);

      public:
        static void shmInit(MonitorChannel &ch);
        explicit MonitorScope(std::string scopeName) : name_(std::move(scopeName))
        {
                fftInBuf_.resize(fftPoints_);
                fftMagF32_.resize(fftPoints_ / 2 + 1);
                fftMagBuf_.resize(fftPoints_ / 2 + 1);
                fftOutBuf_.resize((fftPoints_ / 2 + 1) * 2);
                fftLoBuf_.resize(fftPoints_);

                fft_cfg_t cfg;
                cfg.npoints  = fftPoints_;
                cfg.fs       = 1000.0f;
                cfg.e_window = FFT_WINDOW_HANNING;
                cfg.in_buf   = fftInBuf_.data();
                cfg.mag_buf  = fftMagF32_.data();
                cfg.out_buf  = (decltype(cfg.out_buf))fftOutBuf_.data();
                cfg.buf      = fftLoBuf_.data();
                fft_init(&fft_, cfg);
        }
        ~MonitorScope() { fft_destroy(&fft_); }

        void menu();
        void draw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        // Run one FFT pass for this scope: copy the visible window under monitorMtx,
        // transform off-lock, and publish the result. Called by the FFT worker thread.
        void            fftWorkerStep(std::mutex &monitorMtx);
        void            dropTarget();
        int             addChannel(const std::string &chName);
        int             setValue(const std::string &chName, f32 val);
        MonitorChannel *findChannel(const std::string &chName);
        void            markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool            isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }
        bool            isManual() const { return isManualHeight_; }
        void            setManual(bool m) { isManualHeight_ = m; }

        bool isPaused() const { return paused_; }
        void setPaused(bool p) { paused_ = p; }
        bool isFftEnabled() const { return showFft_; }

        bool isHidden() const { return hidden_; }
        void setHidden(bool h) { hidden_ = h; }

        // Display order within the parent Monitor (lower = earlier). Scopes are drawn
        // sorted by this value so the user can reorder them.
        i64  getOrder() const { return order_; }
        void setOrder(i64 o) { order_ = o; }

        // Reorder request set by the scope's own toolbar (up/down buttons) and consumed
        // by Monitor::updateDisplay after the draw loop. -1 = up, +1 = down, 0 = none.
        void requestMove(int dir) { moveDir_ = dir; }
        int  consumeMoveRequest()
        {
                int d    = moveDir_;
                moveDir_ = 0;
                return d;
        }

        // Drag-drop swap: set by a drop event in the scope's menu bar, consumed by Monitor.
        void        requestSwap(const std::string &name) { pendingSwapWith_ = name; }
        std::string consumeSwap()
        {
                std::string s;
                s.swap(pendingSwapWith_);
                return s;
        }

        // Purge channels marked for deletion (called by sampler at safe point).
        void purgeDeleted()
        {
                for (auto it = chs_.begin(); it != chs_.end();) {
                        if (it->second && it->second->isPendingDelete())
                                it = chs_.erase(it);
                        else
                                ++it;
                }
        }

        void clearData()
        {
                for (auto &pair : chs_)
                        if (pair.second)
                                pair.second->clearData();
        }

        // Returns the mean value computed over the visible window for `chName`.
        // Returns 0.0 when the channel has no stats yet (scope not yet rendered or no data).
        f64 getChannelMean(const std::string &chName) const
        {
                auto it = channelStats_.find(chName);
                return (it != channelStats_.end() && it->second.count > 0) ? it->second.mean : 0.0;
        }
};

#endif // !MONITOR_SCOPE_HPP
