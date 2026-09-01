/**
 * @file  monitor.hpp
 * @brief Monitor — a named container of scopes for telemetry visualisation.
 *
 * Each Monitor holds one or more MonitorScope instances and manages
 * session-level state such as axis linkage and sampling configuration.
 */
#ifndef MONITOR_HPP
#define MONITOR_HPP

#include "gui/hover_focus.hpp"
#include "gui/mujoco_view.hpp"
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app_log.hpp"
#include "ckit.h"
#include "core/jlink_port.hpp"
#include "core/session_time.hpp"
#include "gui/hover_reveal.hpp"
#include "gui/interaction_history.hpp"
#include "gui/monitor_channel.hpp"
#include "gui/monitor_scope.hpp"
#include "gui/monitor_types.hpp"
#include "timeops.h"

extern std::atomic<bool> g_monitorPaused;
// Global "pause all J-Link sampling" switch (top bar). Unlike g_monitorPaused
// (which is display-only and keeps acquiring), this stops J-Link acquisition
// entirely: the sampler drops all J-Link read tasks, so HSS auto-stops
extern std::atomic<bool> g_jlinkSamplingPaused;
// Global HSS target sample rate (Hz) — shared by all monitors in HSS mode
// Displayed and edited in the app top bar; the per-monitor maxSampleHz_ is
// used only for POLL mode
extern std::atomic<int> g_maxHssHz;

// Progress/lifetime state for a background CSV export. Held via shared_ptr so
// the writer thread can outlive the Monitor (e.g. window closed mid-export)
// without dangling
struct CsvExportState {
        std::atomic<bool> running{false};
        std::atomic<u64>  rows{0};  // rows written so far
        std::atomic<u64>  total{0}; // total rows to write
};

class Monitor
{
        friend struct MuJoCoViewTestAccess;
        friend class MonitorScope;
        friend struct ScopeInteractionTestAccess;
        friend struct CsvExportTestAccess;
        friend struct MonitorSearchTestAccess;

      public:
        void showFullData()
        {
                viewMode_ = MonitorViewMode::FULL;
                for (auto &[_, scope] : scopes_)
                        scope->resetYFit();
        }
        using ScopeMapType = std::unordered_map<std::string, std::shared_ptr<MonitorScope>>;

      private:
        std::string      name_{};  // Stable internal id: map key + ImGui window id (never changes)
        std::string      title_{}; // User-facing dock title; falls back to name_ when empty
        std::vector<f32> tss_{};
        bool             paused_{false};
        ScopeMapType     scopes_{};
        bool             isModified_{false};

        // Inline rename of the dock title (double-click the title bar / tab)
        char renameBuf_[64]{};

      public:
        enum class SamplingMode { HSS, POLL };
        SamplingMode samplingMode_{SamplingMode::POLL};
        enum class ScopeLayout { Stacked = 0, Grid = 1 };
        ScopeLayout scopeLayout() const { return scopeLayout_; }
        void        setScopeLayout(ScopeLayout layout)
        {
                if (scopeLayout_ != layout) {
                        scopeLayout_ = layout;
                        needsLayout_ = true;
                        isModified_  = true;
                }
                focusedScope_.clear();
        }
        bool        isScopeFocused(const std::string &name) const { return focusedScope_ == name; }
        bool        hasFocusedScope() const { return !focusedScope_.empty(); }
        const char *channelSearch() const { return channelSearch_; }
        void        toggleScopeFocus(const std::string &name)
        {
                scopeFocusPreview_.commit();
                focusedScope_          = focusedScope_ == name ? std::string{} : name;
                scopeFocusScrollReset_ = true;
                revealFocusedScope();
        }
        void restoreScopeOverview()
        {
                scopeFocusPreview_.commit();
                focusedScope_.clear();
                scopeFocusScrollReset_ = true;
        }

      private:
        void revealFocusedScope()
        {
                const auto scope = scopes_.find(focusedScope_);
                if (scope != scopes_.end() && scope->second->isHidden()) {
                        scope->second->setHidden(false);
                        setModified();
                }
        }
        void menu();
        struct ChannelUiState {
                std::string                     key, label;
                std::shared_ptr<MonitorChannel> channel;
                i64                             order{};
                bool                            operator==(const ChannelUiState &) const = default;
        };
        struct ScopeUiState {
                std::string                   key, label;
                std::shared_ptr<MonitorScope> scope;
                i64                           order{};
                std::vector<ChannelUiState>   channels;
                bool                          operator==(const ScopeUiState &) const = default;
        };
        struct UiState {
                std::string               title;
                ScopeLayout               layout{};
                std::vector<ScopeUiState> scopes;
                bool                      operator==(const UiState &) const = default;
        };
        UiState                               captureUiState() const;
        void                                  restoreUiState(const UiState &state);
        ui::interaction::UndoHistory<UiState> uiHistory_;
        ui::interaction::Action               pendingUiAction_ = ui::interaction::Action::None;
        std::optional<std::string>            pendingUiRoute_;

        MonitorViewMode                          viewMode_{MonitorViewMode::FULL};
        MonitorViewMode                          fftViewMode_{MonitorViewMode::FULL};
        bool                                     needsLayout_{true};
        ScopeLayout                              scopeLayout_{ScopeLayout::Stacked};
        std::string                              focusedScope_; // Temporary view; never changes acquisition or session contents
        char                                     channelSearch_[128]{};
        std::string                              channelSearchQuery_;
        ui::HoverFocus<std::string>              scopeFocusPreview_;
        bool                                     scopeFocusScrollReset_{false};
        int                                      pendingSamplingMode_{-1};
        float                                    prevAvailY_{0.0f};
        f64                                      prevLocalSampleTs_{-1.0};
        std::atomic<bool>                        pendingClearData_{false};
        i64                                      nextScopeOrder_{0}; // monotonic scope display-order allocator
        bool                                     linkedCursorActive_{false};
        double                                   linkedCursorTime_{0};
        std::vector<std::function<void(double)>> linkedCursorDraws_;
        MonitorTimeMeasurement                   timeMeasurement_;
        bool                                     discardSelectionEnabled_{false};
        void                                     setDiscardSelectionEnabled(bool enabled);
        void                                     drawDiscardSelectionControls();
        enum class CsvExportKind { Monitor, Scope, Channel };
        struct CsvExportTarget {
                CsvExportKind kind{CsvExportKind::Monitor};
                std::string   scope, channel;
        };
        std::optional<CsvExportTarget> drawCsvExportMenu();
        void                           exportCsvDialog(const CsvExportTarget &target);
        bool                           exportCsvFile(const std::string &path, const CsvExportTarget &target);

      public:
        f64               linkXMin_{0.0}, linkXMax_{1.0};
        f64               prevNow_{0.0};
        f64               dataStartTime_{0.0};
        f64               pauseXMax_{-1.0};
        bool              wasPaused_{false};
        float             historySeconds_{0.0f};
        ui::HoverReveal   optionsReveal_;
        bool              linkScopeCursor_{false};
        u32               maxDisplayPoints_{5000};
        bool              hssAutoPeriod_{true};
        int               maxSampleHz_{100};
        int               displayRadix_{10};
        std::atomic<bool> pendingDelete_{false};
        std::atomic<bool> csvLoading_{false};
        std::string       importSourcePath_{};
        std::string       importSourceType_{};
        // Generic CSV X axis: -2 = auto-detect, -1 = row number, >= 0 = CSV column index
        int importTimeColumn_{-2};
        // 0 = auto, 1 = seconds, 2 = milliseconds, 3 = microseconds
        int    importTimeUnit_{0};
        bool   importFormatTimeOfDay_{false};
        double importTimeOriginSeconds_{0.0};
        // -1 preserves legacy imported-file formatting; 0 relative, 1 calendar.
        int    timeAxisFormat_{-1};
        bool   formattedTimeAxis() const;
        bool   absoluteTimeAvailable() const;
        double timeAxisOriginSeconds() const;
        void   drawTimeAxisOptions();
        bool   fillAppOnNextDraw_{false};
        bool   maximizeWhenImportCompletes_{false};
        // Pause acquisition for every scope in this monitor at once (monitor toolbar)
        // The sampler skips the whole monitor while set
        std::atomic<bool>           samplingPaused_{false};
        std::atomic<u64>            simFramesReceived_{0}, simDroppedFrames_{0};
        std::atomic<u64>            simEpoch_{0};
        u64                         displayedSimEpoch_{0};
        std::atomic<bool>           simConnected_{false};
        bool                        mujocoEnabled_ = false;
        float                       mujocoHeight_  = 320.0f;
        std::string                 mujocoModelPath_;
        ava_mujoco::Control         mujocoControl_;
        std::unique_ptr<MuJoCoView> mujocoView_;
        // Background CSV export progress; shared with the detached writer thread
        std::shared_ptr<CsvExportState> csvExport_{std::make_shared<CsvExportState>()};

        f32                           actualHz_{0.0f};
        f32                           displayHz_{0.0f}; // EMA-smoothed Hz for stable display
        u64                           pointAccum_{0};
        u64                           prevHzTick_{get_mono_ts_ms()};
        static std::vector<Monitor *> sInstances_;
        static std::mutex             sMtxInstances_;

      public:
        explicit Monitor(std::string monitorName) : name_(std::move(monitorName))
        {
                LOG_I("Monitor()");
                std::lock_guard lk(sMtxInstances_);
                sInstances_.push_back(this);
        }
        Monitor()
        {
                LOG_I("Monitor()");
                std::lock_guard lk(sMtxInstances_);
                sInstances_.push_back(this);
        };
        ~Monitor()
        {
                LOG_I("~Monitor()");
                std::lock_guard lk(sMtxInstances_);
                auto            it = std::find(sInstances_.begin(), sInstances_.end(), this);
                if (it != sInstances_.end())
                        sInstances_.erase(it);
        };

        // Embedded live sources reuse the same monitor UI without J-Link acquisition controls
        bool externalFeed_{false};
        // Hide only the axis captions; retain units for sample tooltips and conversions
        bool hideAxisLabels_{false};
        // Instrument captures can start before the trigger and span nanoseconds
        bool triggerTimeAxis_{false};
        void updateDisplay(bool embedded = false);
        bool isModified() const { return isModified_; }
        void setModified() { isModified_ = true; }
        void requestScopeLayout() { needsLayout_ = true; }
        void clearModified() { isModified_ = false; }
        void requestClearData() { pendingClearData_.store(true, std::memory_order_release); }

        bool consumeClearDataRequest() { return pendingClearData_.exchange(false, std::memory_order_acq_rel); }

        /**
         * @brief Execute on the history writer's thread, with the monitor mutex held
         *        Unlike clearing all data, discarding a range never resets acquisition
         *        clocks, rate counters or any other scope's history
         */
        void processDiscardRequests()
        {
                for (auto &[_, scope] : scopes_)
                        if (scope && !scope->isPendingDelete())
                                scope->processDiscardTimeRangeRequest();
        }

        bool hasPendingDataDiscard() const
        {
                for (const auto &[_, scope] : scopes_)
                        if (scope && !scope->isPendingDelete() && scope->hasPendingDiscardTimeRange())
                                return true;
                return false;
        }

        void clearData()
        {
                setDiscardSelectionEnabled(false);
                u64 totalCleared = 0;
                for (auto &pair : scopes_)
                        if (pair.second) {
                                for (auto &[_, ch] : pair.second->getChannels())
                                        if (ch)
                                                totalCleared += ch->storedCount();
                                pair.second->clearData();
                        }
                purgeDeletedScopes();
                pointAccum_ = 0;
                actualHz_   = 0.0f;
                LOG_I("Monitor[%s] data cleared: %llu points", name_.c_str(), totalCleared);
        }

        void addPoints(u64 n) { pointAccum_ += n; }
        f32  getHz() const { return actualHz_; }

        /**
         * @brief GUI snapshot points across every registered Monitor, including embedded tools
         * Call on the GUI thread while holding Gui::mtxMonitors_ to exclude sampler publication
         */
        static u64 getGlobalPointCount();

        /**
         * @brief Returns the max actualHz_ across all Monitor instances that are in HSS mode
         *        Safe to call on the GUI thread; actualHz_ reads may race with sampler writes
         *        but are acceptable for display (float reads are atomic on x86/x64)
         */
        static f32 getGlobalHssHz()
        {
                std::lock_guard lk(sMtxInstances_);
                f32             hz = 0.0f;
                for (auto *m : sInstances_)
                        if (m->samplingMode_ == SamplingMode::HSS && m->actualHz_ > hz)
                                hz = m->actualHz_;
                return hz;
        }

        void updateHz()
        {
                auto now  = get_mono_ts_ms();
                auto dtMs = now - prevHzTick_;
                if (dtMs >= 500) {
                        actualHz_   = static_cast<f32>(static_cast<f64>(pointAccum_) * 1000.0 / static_cast<f64>(dtMs));
                        pointAccum_ = 0;
                        prevHzTick_ = now;
                }
        }

        void markPendingDelete() { pendingDelete_.store(true, std::memory_order_release); }
        bool isPendingDelete() const { return pendingDelete_.load(std::memory_order_acquire); }

        bool isSamplingPaused() const { return samplingPaused_.load(std::memory_order_acquire); }
        void setSamplingPaused(bool p)
        {
                if (samplingPaused_.exchange(p, std::memory_order_acq_rel) != p)
                        setModified();
        }

        bool consumeLocalSampleDue(f64 now)
        {
                const f64 period = 1.0 / static_cast<f64>(std::max(1, maxSampleHz_));
                if (prevLocalSampleTs_ < 0.0 || now < prevLocalSampleTs_) {
                        prevLocalSampleTs_ = now;
                        return true;
                }
                const f64 elapsed = now - prevLocalSampleTs_;
                if (elapsed < period)
                        return false;
                prevLocalSampleTs_ += std::floor(elapsed / period) * period;
                return true;
        }

        std::string        getName() { return name_; }
        const std::string &getTitle() const { return title_.empty() ? name_ : title_; }
        void               setTitle(const std::string &t)
        {
                title_      = t;
                isModified_ = true;
        }
        int             addScope(const std::string &scopeName);
        ScopeMapType   &getScopes() { return scopes_; }
        MonitorChannel *findChannel(const std::string &scopeName, const std::string &chName);
        bool            containsChannelIdentity(const std::string &name, u64 address, const std::string &type) const;
        static void
        updateUnitConversion(const std::string &name, std::optional<u64> address, const unit_conversion::Settings &settings);

        /**
         * @brief Scope display-order allocation. Each new scope gets the next index so the
         *        default order is insertion order; the user can then reorder via the toolbar
         */
        i64  allocScopeOrder() { return nextScopeOrder_++; }
        void noteScopeOrder(i64 o)
        {
                if (o >= nextScopeOrder_)
                        nextScopeOrder_ = o + 1;
        }

        void purgeDeletedScopes()
        {
                for (auto it = scopes_.begin(); it != scopes_.end();) {
                        if (it->second && it->second->isPendingDelete())
                                it = scopes_.erase(it);
                        else
                                ++it;
                }
        }

        static void clearAll()
        {
                std::lock_guard lk(sMtxInstances_);
                resetSessionTime();
                for (auto *m : sInstances_) {
                        // Clearing restarts acquisition time, not the user's follow zoom
                        const f64 span = m->linkXMax_ - m->linkXMin_;
                        if (m->viewMode_ == MonitorViewMode::FOLLOW && std::isfinite(span) && span > 0.0) {
                                m->linkXMin_ = -span;
                                m->linkXMax_ = 0.0;
                        } else {
                                m->linkXMin_ = 0.0;
                                m->linkXMax_ = 1.0;
                        }
                        m->prevNow_       = 0.0;
                        m->dataStartTime_ = 0.0;
                        m->requestClearData();
                }
                JLinkPort::instance().reqRestart();
                JLinkPort::instance().resetPoints();
                LOG_I("Monitor::clearAll requested for %zu monitors", sInstances_.size());
        }
};

#endif // !MONITOR_HPP
