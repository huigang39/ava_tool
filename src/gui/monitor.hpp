/**
 * @file  monitor.hpp
 * @brief Monitor — a named container of scopes for telemetry visualisation.
 *
 * Each Monitor holds one or more MonitorScope instances and manages
 * session-level state such as axis linkage and sampling configuration.
 */
#ifndef MONITOR_HPP
#define MONITOR_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/session_time.hpp"
#include "gui/monitor_channel.hpp"
#include "gui/monitor_scope.hpp"
#include "gui/monitor_types.hpp"
#include "module.h"
#include "timeops.h"

extern std::atomic<bool> g_monitorPaused;
// Global "pause all J-Link sampling" switch (top bar). Unlike g_monitorPaused
// (which is display-only and keeps acquiring), this stops J-Link acquisition
// entirely: the sampler drops all J-Link read tasks, so HSS auto-stops.
extern std::atomic<bool> g_jlinkSamplingPaused;
// Global HSS target sample rate (Hz) — shared by all monitors in HSS mode.
// Displayed and edited in the app top bar; the per-monitor maxSampleHz_ is
// used only for POLL mode.
extern std::atomic<int> g_maxHssHz;

// Progress/lifetime state for a background CSV export. Held via shared_ptr so
// the writer thread can outlive the Monitor (e.g. window closed mid-export)
// without dangling.
struct CsvExportState {
        std::atomic<bool> running{false};
        std::atomic<u64>  rows{0};  // rows written so far
        std::atomic<u64>  total{0}; // total rows to write
};

class Monitor
{
      public:
        using ScopeMapType = std::unordered_map<std::string, std::shared_ptr<MonitorScope>>;

      private:
        std::string      name_{};  // Stable internal id: map key + ImGui window id (never changes)
        std::string      title_{}; // User-facing dock title; falls back to name_ when empty
        std::vector<f32> timestamps_{};
        bool             paused_{false};
        ScopeMapType     scopes_{};
        bool             isModified_{false};

        // Inline rename of the dock title (double-click the title bar / tab).
        char renameBuf_[64]{};

      public:
        enum class SamplingMode { HSS, POLL };
        SamplingMode samplingMode_{SamplingMode::POLL};

      private:
        void menu();

        MonitorViewMode   viewMode_{MonitorViewMode::FULL};
        MonitorViewMode   fftViewMode_{MonitorViewMode::FULL};
        bool              needsLayout_{true};
        float             lastAvailY_{0.0f};
        std::atomic<bool> pendingClearData_{false};
        i64               nextScopeOrder_{0}; // monotonic scope display-order allocator

      public:
        f64               linkXMin_{0.0}, linkXMax_{1.0};
        f64               lastNow_{0.0};
        f64               dataStartTime_{0.0};
        f64               pauseXMax_{-1.0};
        bool              wasPaused_{false};
        float             historySeconds_{0.0f};
        u32               maxDisplayPoints_{5000};
        bool              hssAutoPeriod_{true};
        int               maxSampleHz_{100};
        std::atomic<bool> pendingDelete_{false};
        std::atomic<bool> csvLoading_{false};
        std::string       importSourcePath_{};
        std::string       importSourceType_{};
        // Generic CSV X axis: -2 = auto-detect, -1 = row number, >= 0 = CSV column index.
        int importTimeColumn_{-2};
        // 0 = auto, 1 = seconds, 2 = milliseconds, 3 = microseconds.
        int         importTimeUnit_{0};
        std::string importTimeOriginText_{};
        bool        fillAppOnNextDraw_{false};
        bool        maximizeWhenImportCompletes_{false};
        // Pause acquisition for every scope in this monitor at once (monitor toolbar).
        // The sampler skips the whole monitor while set.
        std::atomic<bool> samplingPaused_{false};
        // Background CSV export progress; shared with the detached writer thread.
        std::shared_ptr<CsvExportState> csvExport_{std::make_shared<CsvExportState>()};

        f32                           actualHz_{0.0f};
        f32                           displayHz_{0.0f}; // EMA-smoothed Hz for stable display
        u64                           pointAccum_{0};
        u64                           lastHzTick_{get_mono_ts_ms()};
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

        void updateDisplay();
        bool isModified() const { return isModified_; }
        void setModified() { isModified_ = true; }
        void clearModified() { isModified_ = false; }
        void requestClearData() { pendingClearData_.store(true, std::memory_order_release); }

        bool consumeClearDataRequest() { return pendingClearData_.exchange(false, std::memory_order_acq_rel); }

        void clearData()
        {
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

        // Returns the max actualHz_ across all Monitor instances that are in HSS mode.
        // Safe to call on the GUI thread; actualHz_ reads may race with sampler writes
        // but are acceptable for display (float reads are atomic on x86/x64).
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
                auto dtMs = now - lastHzTick_;
                if (dtMs >= 500) {
                        actualHz_   = static_cast<f32>(static_cast<f64>(pointAccum_) * 1000.0 / static_cast<f64>(dtMs));
                        pointAccum_ = 0;
                        lastHzTick_ = now;
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

        // Scope display-order allocation. Each new scope gets the next index so the
        // default order is insertion order; the user can then reorder via the toolbar.
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
                        m->linkXMin_      = 0.0;
                        m->linkXMax_      = 1.0;
                        m->lastNow_       = 0.0;
                        m->dataStartTime_ = 0.0;
                        m->requestClearData();
                }
                JLinkPort::instance().reqRestart();
                JLinkPort::instance().resetPoints();
                LOG_I("Monitor::clearAll requested for %zu monitors", sInstances_.size());
        }
};

#endif // !MONITOR_HPP
