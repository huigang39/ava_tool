/**
 * @file  monitor_scope.hpp
 * @brief MonitorScope — a group of chs plotted on the same axes.
 *
 * Each scope can render in either PLOT or TABLE mode and optionally
 * shows an FFT view side-by-side with the time-domain plot.
 */
#ifndef MONITOR_SCOPE_HPP
#define MONITOR_SCOPE_HPP
#include "gui/hover_focus.hpp"
#include "gui/value_flash.hpp"

#include "ckit.h"
#include "gui/hover_reveal.hpp"
#include "gui/monitor_channel.hpp"
#include "gui/monitor_types.hpp"
#include "gui/plot_fit.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Monitor;

/**
 * @brief GUI-thread bridge used by a LOCAL monitor ch to open the owning
 *        Variable window's function-binding dialog
 */
void        requestMonitorFunctionBinding(const std::string &variableName);
std::string consumeMonitorFunctionBindingRequest();

class MonitorScope
{
        friend struct FftStartupTestAccess;
        friend struct ScopeInteractionTestAccess;
        friend struct VariableRewriteRegression;
        friend class Monitor;

      public:
        Monitor *parent_{nullptr};
        enum class DrawEnum {
                PLOT,
                TABLE,
        };
        using ChannelMapType = std::unordered_map<std::string, std::shared_ptr<MonitorChannel>>;
        const std::string &getName() const { return name_; }
        const std::string &getLabel() const { return label_.empty() ? name_ : label_; }
        void               setLabel(const std::string &l) { label_ = l; }
        void               setDraw(DrawEnum d) { e_draw = d; }
        DrawEnum           getDraw() const { return e_draw; }
        f32               &getHeight() { return height_; }
        void               resetYFit() { yFit_.reset(); }
        void               setAxisLabels(const std::string &x, const std::string &y)
        {
                xAxisLabel_ = x;
                yAxisLabel_ = y;
        }
        bool                       &getShowFft() { return showFft_; }
        bool                       &getFftBars() { return fftBars_; }
        bool                       &getShowSidePanel() { return showSidePanel_; }
        bool                       &getCompareStatistics() { return compareStatistics_; }
        int                        &getFftPoints() { return fftPoints_; }
        int                        &getFftPeakCount() { return fftPeakCount_; }
        ChannelMapType             &getChannels() { return chs_; }
        std::set<std::string>      &getExpandedGroups() { return expandedGroups_; }
        void                        setSearchQuery(std::string query) { searchQuery_ = std::move(query); }
        bool                        hasSearchMatch() const;
        const std::array<float, 6> &getTableColumnWidths() const { return tableColumnWidths_; }
        int                         getTableSortColumn() const { return tableSortColumn_; }
        int                         getTableSortDirection() const { return tableSortDirection_; }
        bool                        usesManualTableOrder() const { return forceManualTableOrder_; }
        void setTableUiState(const std::array<float, 6> &widths, int sortColumn, int sortDirection, bool manualOrder)
        {
                tableColumnWidths_     = widths;
                tableSortColumn_       = sortColumn;
                tableSortDirection_    = sortDirection;
                forceManualTableOrder_ = manualOrder || sortColumn < 0;
                restoreTableWidths_    = true;
                restoreTableSort_      = true;
        }
        void noteChannelOrder(i64 order)
        {
                if (order >= nextChannelOrder_)
                        nextChannelOrder_ = order + 1;
        }
        void reinitFft(int newPoints);

      private:
        std::string                   name_{};
        std::string                   label_{}; // User-facing alias; falls back to name_ when empty
        std::string                   xAxisLabel_, yAxisLabel_;
        bool                          showXAxis_{true}; // Parent assigns the bottom shared time axis each frame
        char                          renameBuf_[64]{};
        ChannelMapType                chs_{};
        DrawEnum                      e_draw{DrawEnum::PLOT};
        f32                           height_{200.0f};
        bool                          showFft_{false};
        bool                          fftBars_{false};          // FFT render style: false = line, true = bar chart
        bool                          showSidePanel_{false};    // show the right-side Stats/Peaks table in plot view
        bool                          compareStatistics_{true}; // metrics in rows, channels in columns
        ui::HoverReveal               statisticsPeek_;
        ui::HoverReveal               optionsReveal_;
        double                        axisTimeOriginSeconds_{0.0};
        bool                          hasTimeAxis() const;
        ui::HoverFocus<std::string>   tableFocusPreview_;
        ui::ValueFlash                valueFlash_;
        int                           statisticsPeekFrame_{-1};
        int                           menuFrame_{-1};
        void                          drawMenu(bool overlay = false, float reservedRight = 0.0f);
        MonitorTimeMeasurement        localTimeMeasurement_;
        MonitorTimeMeasurement       &timeMeasurement();
        const MonitorTimeMeasurement &timeMeasurement() const;
        struct HoverSample {
                bool        valid{false};
                double      x{0}, y{0};
                std::string label;
                std::string channel;
                double      raw{0};
        } hoverSample_;
        struct HoverValue {
                bool        valid{false};
                double      y{0};
                std::string label;
                u32         color{0};
                i64         order{0};
                std::string channel;
                double      sampleX{std::numeric_limits<double>::quiet_NaN()};
                double      raw{std::numeric_limits<double>::quiet_NaN()};
        };
        std::vector<HoverValue> hoverValues_;
        double                  hoverCursorX_{0};
        struct DiscardSelection {
                bool   enabled{false}, dragging{false}, hasRange{false};
                double start{0}, end{0};
                float  startPixel{0};
                // Private X links freeze only this scope while the other scopes keep following
                double viewMin{0}, viewMax{1};
        } discardSelection_;
        double      lastPlotXMin_{0}, lastPlotXMax_{1};
        bool        pendingDiscardTimeRange_{false};
        double      pendingDiscardMin_{0}, pendingDiscardMax_{0};
        usize       lastDiscardCount_{0};
        u64         discardResultUntilMs_{0};
        bool        discardRejected_{false};
        const char *discardUnavailableReason() const;
        void        beginDiscardSelection();
        void        cancelDiscardSelection() { discardSelection_ = {}; }
        bool        discardDataAvailable() const;
        usize       discardPointCount(double minimum, double maximum) const;
        usize       selectedDiscardPointCount() const;
        bool        submitDiscardSelection();
        void        drawDiscardSelectionControls();
        void        plotDiscardSelection();
        void  collectCursorValues(double x, const std::function<void(double, double, double, const std::string &)> &consider);
        int   fftPoints_{1024};
        int   fftPeakCount_{5};
        fft_t fft_{};
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
        bool                  recordedData_{false};
        std::set<std::string> selectedGroupPaths_{};
        bool                  forceManualTableOrder_{false};
        std::array<float, 6>  tableColumnWidths_{};
        bool                  tableWidthsInitialized_{false};
        bool                  restoreTableWidths_{false};
        int                   tableSortColumn_{-1};
        int                   tableSortDirection_{1}; // ImGuiSortDirection_Ascending
        bool                  restoreTableSort_{false};
        // Full paths (e.g. "foo.bar") of group nodes the user has expanded;
        // persisted across sessions via Gui save/load
        bool                  hidden_{false};
        std::string           searchQuery_;
        std::string           searchSelectedChannel_;
        bool                  searchScroll_{false};
        std::set<std::string> expandedGroups_{};
        // A temporary table root; channels keep their canonical names and sampling state
        std::string              tableFocusPath_;
        bool                     tableFocusScrollReset_{false};
        std::vector<std::string> tableVisibleKeys_;
        // Monotonic counter handing each new ch an insertion-order index so
        // the table/plot can display chs in the order they were added (e.g
        // matching CSV column order) instead of hash order
        i64 nextChannelOrder_{0};
        // Display order of this scope within its Monitor (user-reorderable)
        i64         order_{0};
        int         moveDir_{0};      // pending reorder request (-1 up, +1 down), consumed by Monitor
        std::string pendingSwapWith_; // drag-drop swap target scope name, consumed by Monitor

        bool                pendingAxisReset_{false};
        ui::IndependentYFit yFit_;
        struct Peak {
                f64 freq;
                f64 mag;
        };
        std::map<std::string, std::vector<Peak>> chPeaks_;

        struct Stats {
                f64   rms{0};
                f64   max{0};
                f64   min{0};
                f64   mean{0};
                f64   pkpk{0};
                f64   stddev{0}, variance{0}, median{0}, p95{0};
                bool  approximate{false};
                usize count{0};
        };
        std::map<std::string, Stats> chStats_;

        struct ChannelPropertyEdit {
                std::string                            targetName;
                char                                   name[256]{};
                char                                   alias[256]{};
                char                                   address[32]{};
                char                                   shmName[64]{};
                int                                    simField{0};
                int                                    typeIndex{4};
                int                                    deviceIndex{0};
                bool                                   writable{true};
                unit_conversion::Settings              unitConversion{};
                std::vector<MonitorChannel::EnumEntry> enums;
        } chPropertyEdit_;

        // FFT results published by the background FFT worker thread (fftWorkerStep)
        // and consumed by the GUI thread in plotDraw — so the expensive transform
        // no longer runs on the render thread and stops dragging down FPS
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
        std::atomic<u64>                 fftDataGeneration_{0}; // reject work copied before a history edit

        void tableDraw();
        bool matchesSearch(const std::string &name, const MonitorChannel &channel) const;
        void focusTableGroup(const std::string &path);
        void tableMenu();
        void beginChannelProperties(const std::string &chName);
        void drawChannelPropertiesPopup();

        void plotDraw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        void drawStatisticsPanel(float contentHeight);
        void plotInspect(MonitorViewMode &mode);
        void plotMenu();

        void drawTableRow(const std::string &chName, std::shared_ptr<MonitorChannel> &ch, const std::string &displayLabel = {});

        /**
         * @brief Reorder ch `src` so it sits at `dst`'s display position, then
         * renumber all chs' display order sequentially. Used by drag-reorder
         */
        void reorderChannelTo(const std::string &src, const std::string &dst);
        /**
         * @brief Move every ch under the group path `srcGroup` (a struct/array) so the group
         * sits just before `dst`'s display position, preserving the members' relative order
         */
        void reorderGroupBefore(const std::string &srcGroup, const std::string &dst);
        /**
         * @brief The lowest-display-order leaf under `groupPath` — the group's anchor position
         */
        std::string groupAnchorKey(const std::string &groupPath) const;
        /**
         * @brief Transfer the ch(s) described by a DND_CHANNEL_MOVE payload from another scope
         * into this one. No-op when the payload originates from this scope
         */
        bool moveChannelsFrom(ChannelMovePayload *data, bool copy);
        /**
         * @brief Accept a DND_CHANNEL_MOVE payload: reorder inside this scope, move across scopes
         */
        void        applyChannelMoveDrop(ChannelMovePayload *data, const std::string &dst);
        std::string clipboardText(const std::vector<std::string> &names) const;
        void        drawDisplayFormatMenu(MonitorChannel &channel);

      public:
        static void shmInit(MonitorChannel &ch);
        explicit MonitorScope(std::string scopeName) : name_(std::move(scopeName)) { reinitFft(fftPoints_); }
        ~MonitorScope() { fft_destroy(&fft_); }

        void menu(bool recordedData = false);
        void draw(f64 *linkXMin, f64 *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode);
        /**
         * @brief Run one FFT pass for this scope: copy the visible window under monitorMtx,
         * transform off-lock, and publish the result. Called by the FFT worker thread
         */
        void fftWorkerStep(std::mutex &monitorMtx);
        /**
         * @brief GUI queues a range; the owner consumes it at a sampler-safe point while holding monitorMtx
         */
        bool            hasPendingDiscardTimeRange() const { return pendingDiscardTimeRange_; }
        bool            consumeDiscardTimeRange(double &minimum, double &maximum);
        usize           processDiscardTimeRangeRequest();
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
        bool isDisplayHidden() const;
        void setHidden(bool h);

        /**
         * @brief Display order within the parent Monitor (lower = earlier). Scopes are drawn
         *        sorted by this value so the user can reorder them
         */
        i64  getOrder() const { return order_; }
        void setOrder(i64 o) { order_ = o; }

        /**
         * @brief Reorder request set by the scope's own toolbar (up/down buttons) and consumed
         *        by Monitor::updateDisplay after the draw loop. -1 = up, +1 = down, 0 = none
         */
        void requestMove(int dir) { moveDir_ = dir; }
        int  consumeMoveRequest()
        {
                int d    = moveDir_;
                moveDir_ = 0;
                return d;
        }

        /**
         * @brief Drag-drop swap: set by a drop event in the scope's menu bar, consumed by Monitor
         */
        void        requestSwap(const std::string &name) { pendingSwapWith_ = name; }
        std::string consumeSwap()
        {
                std::string s;
                s.swap(pendingSwapWith_);
                return s;
        }

        /**
         * @brief Purge chs marked for deletion (called by sampler at safe point)
         */
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
                cancelDiscardSelection();
                pendingDiscardTimeRange_ = false;
                discardResultUntilMs_    = 0;
                for (auto &pair : chs_)
                        if (pair.second)
                                pair.second->clearData();
        }

        /**
         * @brief Returns the mean value computed over the visible window for `chName`
         *        Returns 0.0 when the ch has no stats yet (scope not yet rendered or no data)
         */
        f64 getChannelMean(const std::string &chName) const
        {
                auto it = chStats_.find(chName);
                return (it != chStats_.end() && it->second.count > 0) ? it->second.mean : 0.0;
        }
};

#endif // !MONITOR_SCOPE_HPP
