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
        void                   setDraw(DrawEnum d) { e_draw = d; }
        DrawEnum               getDraw() const { return e_draw; }
        f32                   &getHeight() { return height_; }
        bool                  &getShowFft() { return showFft_; }
        int                   &getFftPoints() { return fftPoints_; }
        int                   &getFftPeakCount() { return fftPeakCount_; }
        ChannelMapType        &getChannels() { return chs_; }
        std::set<std::string> &getExpandedGroups() { return expandedGroups_; }
        void                   reinitFft(int newPoints);

      private:
        std::string           name_{};
        ChannelMapType        chs_{};
        DrawEnum              e_draw{};
        f32                   height_{200.0f};
        bool                  showFft_{false};
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
        // Full paths (e.g. "foo.bar") of group nodes the user has expanded;
        // persisted across sessions via Gui save/load.
        bool                  hidden_{false};
        std::set<std::string> expandedGroups_{};

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

        void tableDraw();
        void tableMenu();

        void plotDraw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        void plotMenu();

        void drawTableRow(const std::string               &chName,
                          std::shared_ptr<MonitorChannel> &ch,
                          int                              idx,
                          const std::vector<std::string>  &allKeys,
                          const std::string               &displayLabel = {});

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

        void            menu();
        void            draw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
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
};

#endif // !MONITOR_SCOPE_HPP
