/**
 * @file  sampler.cpp
 * @brief Sampler thread implementation — HSS / POLL / SHM / Wave processing.
 */
#include <algorithm>
#include <cstring>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/sampler.hpp"
#include "core/session_time.hpp"
#include "core/type_codec.hpp"
#include "gui/gui.hpp"
#include "gui/monitor.hpp"
#include "module.h"
#include "timeops.h"
#include "wave.h"

extern std::atomic<bool> g_appRunning;
extern std::atomic<bool> g_monitorPaused;
extern std::atomic<bool> g_jlinkSamplingPaused;

std::atomic<int>  g_samplerRunMode{1}; // 0=Low, 1=Normal, 2=CPUBound
std::atomic<bool> g_samplerRebind{false};
std::atomic<int>  g_samplerBoundCore{-1}; // -1=uninit, -2=no binding, >=0=core id
std::atomic<int>  g_samplerCoreReq{-1};   // -1=auto (highest), >=0=user-picked core

// ---------------------------------------------------------------------------
// Apply the requested run mode to the calling (sampler) thread.
//
//  0 = Low      : normal priority, no affinity binding, always sleep 1ms.
//  1 = Normal   : ABOVE_NORMAL priority, no affinity, adaptive sleep.
//  2 = CPUBound : ABOVE_NORMAL + HIGHEST priority, pinned to highest core,
//                 spin-loop (no sleep) for minimum jitter.
//
// g_samplerBoundCore is set to:
//   >= 0  in CPUBound mode (the actual core); main.cpp uses this to isolate
//         GUI / flusher threads away from that core.
//   -2    in Low / Normal mode (no pinning); main.cpp skips isolation.
// ---------------------------------------------------------------------------
static void
setupThread(int mode)
{
#ifdef _WIN32
        HANDLE hProc   = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();

        if (mode == 0) {
                // Low: normal priority, let OS schedule freely
                SetPriorityClass(hProc, NORMAL_PRIORITY_CLASS);
                SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
                DWORD_PTR procMask = 0, sysMask = 0;
                GetProcessAffinityMask(hProc, &procMask, &sysMask);
                SetThreadAffinityMask(hThread, procMask); // clear any previous pinning
                g_samplerBoundCore.store(-2);
                LOG_I("Sampler: Low mode (normal priority, no affinity)");

        } else if (mode == 1) {
                // Normal: slightly elevated priority, no fixed core
                SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
                SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                DWORD_PTR procMask = 0, sysMask = 0;
                GetProcessAffinityMask(hProc, &procMask, &sysMask);
                SetThreadAffinityMask(hThread, procMask);
                g_samplerBoundCore.store(-2);
                LOG_I("Sampler: Normal mode (ABOVE_NORMAL priority, no affinity)");

        } else {
                // CPUBound: elevated priority + pin to highest available core
                SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
                SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

                // Note: pinning a thread of *our own* process needs no elevation;
                // SetThreadAffinityMask / THREAD_PRIORITY_HIGHEST work for any user.
                // (Only REALTIME_PRIORITY_CLASS would require admin, which we avoid.)
                DWORD_PTR procMask = 0, sysMask = 0;
                GetProcessAffinityMask(hProc, &procMask, &sysMask);
                int       targetCore = -1;
                DWORD_PTR targetMask = 0;

                const int reqCore = g_samplerCoreReq.load();
                if (reqCore >= 0 && reqCore < 64 && (procMask & (1ull << reqCore))) {
                        // User explicitly picked a core that this process may run on.
                        targetCore = reqCore;
                        targetMask = (1ull << reqCore);
                } else {
                        // Auto: pin to the highest available core.
                        for (int b = 63; b >= 0; --b) {
                                if (procMask & (1ull << b)) {
                                        targetCore = b;
                                        targetMask = (1ull << b);
                                        break;
                                }
                        }
                }
                if (targetMask)
                        SetThreadAffinityMask(hThread, targetMask);
                g_samplerBoundCore.store(targetCore);
                LOG_I("Sampler: CPUBound mode (HIGHEST priority, pinned to core %d%s, mask=0x%llx)",
                      targetCore,
                      reqCore >= 0 ? " [user]" : " [auto]",
                      (u64)targetMask);
        }

#elif defined(__linux__)
        if (mode == 0) {
                sched_param sp{};
                sp.sched_priority = 0;
                pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
                const int n = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                cpu_set_t cs;
                CPU_ZERO(&cs);
                for (int i = 0; i < n; ++i)
                        CPU_SET(i, &cs);
                pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
                g_samplerBoundCore.store(-2);
                LOG_I("Sampler: Low mode (SCHED_OTHER, no affinity)");

        } else if (mode == 1) {
                sched_param sp{};
                sp.sched_priority = 0;
                pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
                const int n = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                cpu_set_t cs;
                CPU_ZERO(&cs);
                for (int i = 0; i < n; ++i)
                        CPU_SET(i, &cs);
                pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
                g_samplerBoundCore.store(-2);
                LOG_I("Sampler: Normal mode (SCHED_OTHER, no affinity)");

        } else {
                sched_param sp{};
                sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
                if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
                        LOG_W("Sampler: SCHED_FIFO failed, trying SCHED_RR");
                        sp.sched_priority = sched_get_priority_max(SCHED_RR);
                        pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
                }
                // Affinity itself needs no privileges; only SCHED_FIFO above does
                // (handled with the SCHED_RR/SCHED_OTHER fallback when not permitted).
                const int n    = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                const int req  = g_samplerCoreReq.load();
                const int core = (req >= 0 && req < n) ? req : ((n > 1) ? (n - 1) : 0);
                cpu_set_t cs;
                CPU_ZERO(&cs);
                CPU_SET(core, &cs);
                pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
                g_samplerBoundCore.store(core);
                LOG_I("Sampler: CPUBound mode (SCHED_FIFO prio=%d, pinned to core %d%s)",
                      sp.sched_priority,
                      core,
                      req >= 0 ? " [user]" : " [auto]");
        }

#elif defined(__APPLE__)
        if (mode == 2) {
                sched_param sp{};
                sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
                pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
                thread_affinity_policy_data_t pol{1};
                thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                  THREAD_AFFINITY_POLICY,
                                  (thread_policy_t)&pol,
                                  THREAD_AFFINITY_POLICY_COUNT);
                g_samplerBoundCore.store(0);
                LOG_I("Sampler: CPUBound mode (SCHED_FIFO prio=%d)", sp.sched_priority);
        } else {
                g_samplerBoundCore.store(-2);
                LOG_I("Sampler: %s mode", mode == 0 ? "Low" : "Normal");
        }
#endif
}

void
threadFunc(Gui *gui)
{
        setupThread(g_samplerRunMode.load());
        LOG_I("Sampler thread started.");
        u64                                          lastTime = get_mono_ts_us();
        std::vector<HssBlock>                        lastBlocks;
        std::vector<std::shared_ptr<MonitorChannel>> lastChans;
        std::vector<std::shared_ptr<Monitor>>        lastChMonitors;
        std::vector<std::string>                     lastTypes;
        std::vector<u32>                             lastOffsets;
        i32                                          lastPeriodUs = 0;

        // Per-monitor last-wave-tick. Wave generation is driven from this thread
        // to keep tick spacing regular: no separate thread fighting for JLink mtx.
        std::unordered_map<std::shared_ptr<Monitor>, u64> lastMonitorWaveTicks;

        // -------------------- Diagnostics --------------------
        struct WaveDiag {
                u64 count{0};
                u64 outliers{0};
                u64 sumDtUs{0};
                u64 minDtUs{~0ull};
                u64 maxDtUs{0};
        };
        std::unordered_map<std::shared_ptr<Monitor>, WaveDiag> waveDiag;
        struct LoopDiag {
                u64 iter{0};
                u64 sumIterUs{0};
                u64 maxIterUs{0};
                u64 longIters{0};
                u64 hssReads{0};
                u64 sumHssUs{0};
                u64 maxHssUs{0};
                u64 longHssReads{0};
                u64 maxWMemUs{0};
                u64 sumWMemUs{0};
                u64 wMemCount{0};
                // Lock contention counters
                u64 refreshTryOk{0};         // try_to_lock succeeded
                u64 refreshTryFail{0};       // try_to_lock failed, used cached state
                u64 refreshOverdueBlock{0};  // forced lk.lock() because >50ms stale
                u64 refreshOverdueWaitUs{0}; // total time spent waiting on overdue lock
                u64 maxRefreshOverdueUs{0};
                u64 publishOk{0};
                u64 publishSkip{0};       // publishSnapshot try_lock failed
                u64 maxStaleRefreshUs{0}; // max time between successful refreshes
                u64 shmSamples{0};
                u64 pollSamples{0};
                u64 hssSamples{0};
                u64 waveWrites{0};
                u64 wvalWrites{0};
        };
        LoopDiag loopDiag{};
        u64      diagLastDumpMs  = get_mono_ts_ms();
        u64      hbLastDumpMs    = get_mono_ts_ms();
        u64      sessionStartMs  = get_mono_ts_ms();
        u64      iterCounter     = 0;
        u64      lastRefreshOkUs = get_mono_ts_us();

        static constexpr usize kBufCap = 64 * 1024;
        static u8              buf[kBufCap];
        usize                  carryLen     = 0;
        bool                   discardFirst = false;

        u64 hzFrameAccum       = 0;
        u64 hzLastTick         = get_mono_ts_ms();
        u64 lastHssStart       = get_mono_ts_ms();
        f64 lastHssSessionTime = 0;

        // HSS health watchdog. The J-Link HSS hardware FIFO can wedge into a
        // backlogged/overflowed state in which JLINK_HSS_Read either returns no
        // frames or returns a full buffer on every call. In the latter case each
        // read takes much longer and, since it holds the shared J-Link FairMutex,
        // it starves GUI-thread J-Link reads (register/variable watch) and tanks
        // the render frame rate — exactly the symptom a manual disconnect/connect
        // cured by restarting the stream. We reproduce that recovery automatically:
        // drain the FIFO fully each iteration (below) so a backlog can't persist,
        // and force a clean HSS restart if no frames arrive for a sustained window.
        u64 lastHssFrameMs    = get_mono_ts_ms();
        u64 lastHssWatchdogMs = 0;

        f64  hssBaseWallTime = 0; // session time captured at HSS (re)sync
        u32  hssLastHwTs     = 0; // previous raw 32-bit µs counter (for wrap detection)
        u64  hssAccumUs      = 0; // elapsed µs since sync, accumulated across 2^32 wraps
        bool hssTimeSynced   = false;

        // Internal task structs
        struct TempCh {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             addr;
                u32                             nb;
                std::string                     type;
                u32                             bitOffset;
                u32                             bitSize;
        };
        struct WValTask {
                std::shared_ptr<MonitorChannel> ch;
                u32                             addr, nb;
                std::string                     type;
                std::string                     device;
                f32                             wv;
        };
        struct ShmTask {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             nb;
                std::string                     type;
                u32                             bitOffset;
                u32                             bitSize;
        };
        struct WaveTask {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             addr;
                u32                             nb;
                std::string                     type;
                std::string                     device;
        };

        // Task lists persist across iterations. They're refreshed under
        // mtxMonitors_ when the lock is available; otherwise the sampler
        // reuses the previous iteration's snapshot. This keeps wave/POLL
        // timing tight even while the GUI thread holds the lock during
        // rendering. Captured fields (addr/nb/type) live in the task structs,
        // so the lock-free hot path never touches live channel state besides
        // atomic flags (isPendingDelete, consumeWValDirty, waveEnable_).
        std::vector<TempCh>   tempChs;
        std::vector<TempCh>   pollTasks;
        std::vector<ShmTask>  shmTasks;
        std::vector<WaveTask> waveTasks;
        i32                   maxHssHz      = 1;
        u64                   lastRefreshUs = 0;
        constexpr u64         kRefreshMaxUs = 50000; // force a blocking refresh after 50ms

        while (g_appRunning.load()) {
                if (g_samplerRebind.exchange(false, std::memory_order_acq_rel)) {
                        setupThread(g_samplerRunMode.load());
                }
                const u64 iterStartUs = get_mono_ts_us();
                iterCounter++;
                // Pause is display-only: acquisition keeps running so data is still
                // captured into each channel's store; only the publish-to-GUI step is
                // skipped (see below). HSS therefore stays running across pause and
                // needs no stop/restart on resume.
                bool isPaused        = g_monitorPaused.load();
                bool jlinkSampPaused = g_jlinkSamplingPaused.load();

                std::vector<WValTask> wvalTasks; // built fresh each iter from cached channels

                {
                        std::unique_lock<std::mutex> lk(gui->getMonitorMtx(), std::try_to_lock);
                        const bool                   overdue   = (iterStartUs - lastRefreshUs) > kRefreshMaxUs;
                        const bool                   tryFailed = !lk.owns_lock();
                        if (tryFailed && overdue) {
                                const u64 waitStart = get_mono_ts_us();
                                lk.lock();
                                const u64 waitedUs = get_mono_ts_us() - waitStart;
                                loopDiag.refreshOverdueBlock++;
                                loopDiag.refreshOverdueWaitUs += waitedUs;
                                if (waitedUs > loopDiag.maxRefreshOverdueUs)
                                        loopDiag.maxRefreshOverdueUs = waitedUs;
                        }

                        if (lk.owns_lock()) {
                                loopDiag.refreshTryOk++;
                                const u64 staleUs = iterStartUs - lastRefreshOkUs;
                                if (staleUs > loopDiag.maxStaleRefreshUs)
                                        loopDiag.maxStaleRefreshUs = staleUs;
                                lastRefreshOkUs = iterStartUs;
                                tempChs.clear();
                                pollTasks.clear();
                                shmTasks.clear();
                                waveTasks.clear();
                                maxHssHz = 1;

                                for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                        if (monitor->consumeClearDataRequest()) {
                                                LOG_I("Sampler applying clear request: monitor=%s", monitor->getName().c_str());
                                                monitor->clearData();
                                                JLinkPort::instance().resetPoints();
                                                carryLen      = 0;
                                                discardFirst  = true;
                                                hssTimeSynced = false;
                                        }

                                        // Monitor-level master pause: skip acquisition for all of
                                        // this monitor's scopes at once.
                                        if (monitor->isSamplingPaused())
                                                continue;

                                        bool hasHss = false;
                                        for (auto &scope : monitor->getScopes() | std::views::values) {
                                                if (scope->isPendingDelete() || scope->isPaused())
                                                        continue;
                                                for (auto &ch : scope->getChannels() | std::views::values) {
                                                        if (ch->isPendingDelete())
                                                                continue;
                                                        const std::string &dev = ch->getDevice();
                                                        u32                nb  = ch->getNumBytes();
                                                        if (nb == 0)
                                                                nb = typeBytes(ch->getType());
                                                        if (dev == "SHM" && ch->getShm().lo.spsc != nullptr) {
                                                                shmTasks.push_back({ch,
                                                                                    monitor,
                                                                                    nb,
                                                                                    ch->getType(),
                                                                                    ch->getBitOffset(),
                                                                                    ch->getBitSize()});
                                                                if (ch->waveEnable_)
                                                                        waveTasks.push_back(
                                                                            {ch, monitor, 0, nb, ch->getType(), "SHM"});
                                                        } else if (dev == "JLINK" && ch->getAddr() != 0) {
                                                                // Global "pause all J-Link sampling": drop read tasks so
                                                                // HSS auto-stops (blocks become empty). Wave output below
                                                                // is unaffected — this pauses acquisition only.
                                                                if (!jlinkSampPaused) {
                                                                        if (monitor->samplingMode_ ==
                                                                            Monitor::SamplingMode::HSS) {
                                                                                tempChs.push_back(
                                                                                    {ch,
                                                                                     monitor,
                                                                                     static_cast<u32>(ch->getAddr()),
                                                                                     nb,
                                                                                     ch->getType(),
                                                                                     ch->getBitOffset(),
                                                                                     ch->getBitSize()});
                                                                                hasHss = true;
                                                                        } else {
                                                                                pollTasks.push_back(
                                                                                    {ch,
                                                                                     monitor,
                                                                                     static_cast<u32>(ch->getAddr()),
                                                                                     nb,
                                                                                     ch->getType(),
                                                                                     ch->getBitOffset(),
                                                                                     ch->getBitSize()});
                                                                        }
                                                                }
                                                                if (ch->waveEnable_)
                                                                        waveTasks.push_back({ch,
                                                                                             monitor,
                                                                                             static_cast<u32>(ch->getAddr()),
                                                                                             nb,
                                                                                             ch->getType(),
                                                                                             "JLINK"});
                                                        }
                                                }
                                        }
                                        if (hasHss) {
                                                int gh = g_maxHssHz.load(std::memory_order_relaxed);
                                                if (gh > maxHssHz)
                                                        maxHssHz = gh;
                                        }
                                }
                                // Sort by address so block order is stable regardless of
                                // unordered_map iteration order (which can change after
                                // channel insertions trigger a rehash). Without this, adding
                                // a LOCAL channel reshuffles JLINK entries, producing a
                                // spurious `blocks != lastBlocks` → unnecessary HSS restart
                                // and data gap.
                                std::sort(tempChs.begin(), tempChs.end(), [](const TempCh &a, const TempCh &b) {
                                        return a.addr < b.addr;
                                });
                                lastRefreshUs = iterStartUs;
                        } else {
                                loopDiag.refreshTryFail++;
                        }
                }

                // wValDirty is an atomic — check cached channels lock-free.
                auto collectJlinkWVal = [&](const std::vector<TempCh> &src) {
                        for (const auto &t : src) {
                                if (t.ch->isPendingDelete())
                                        continue;
                                f32 wv;
                                if (t.ch->consumeWValDirty(wv))
                                        wvalTasks.push_back({t.ch, t.addr, t.nb, t.type, "JLINK", wv});
                        }
                };
                auto collectShmWVal = [&]() {
                        for (const auto &t : shmTasks) {
                                if (t.ch->isPendingDelete())
                                        continue;
                                f32 wv;
                                if (t.ch->consumeWValDirty(wv))
                                        wvalTasks.push_back({t.ch, 0, t.nb, t.type, "SHM", wv});
                        }
                };
                collectJlinkWVal(tempChs);
                collectJlinkWVal(pollTasks);
                collectShmWVal();

                // Drain SHM buffers outside the monitor lock. Cached entries
                // may include channels that GUI just marked for deletion;
                // skip those (the atomic pendingDelete is the gate).
                for (auto &st : shmTasks) {
                        if (st.ch->isPendingDelete())
                                continue;
                        u8 raw[8];
                        while (shm_read(&st.ch->getShm(), raw, st.nb) == st.nb) {
                                st.ch->setRVal(decodeAs(raw, st.type, st.bitOffset, st.bitSize), sessionTimeSec());
                                st.monitor->addPoints(1);
                                hzFrameAccum++;
                                loopDiag.shmSamples++;
                        }
                }

                // Write wVal dirty values (wvalTasks already filtered by collectWVal).
                for (auto &wt : wvalTasks) {
                        u8 wbuf[8] = {0};
                        encodeFromF32(wt.wv, wt.type, wbuf);
                        if (wt.device == "JLINK") {
                                if (!JLinkPort::instance().isConnected())
                                        continue;
                                JLinkPort::instance().writeMem(wt.addr, wt.nb, wbuf);
                        } else if (wt.device == "SHM") {
                                shm_write(&wt.ch->getShm(), wbuf, wt.nb);
                                wt.ch->setRVal(decodeAs(wbuf, wt.type, 0, 0), sessionTimeSec());
                        } else {
                                continue;
                        }
                        loopDiag.wvalWrites++;
                }

                // Wave generation
                if (!waveTasks.empty()) {
                        static std::vector<std::pair<std::shared_ptr<Monitor>, std::vector<WaveTask>>> waveGroups;
                        for (auto &pair : waveGroups)
                                pair.second.clear();
                        waveGroups.clear();

                        for (auto &wt : waveTasks) {
                                bool found = false;
                                for (auto &pair : waveGroups) {
                                        if (pair.first == wt.monitor) {
                                                pair.second.push_back(wt);
                                                found = true;
                                                break;
                                        }
                                }
                                if (!found) {
                                        waveGroups.push_back({wt.monitor, {wt}});
                                }
                        }

                        for (auto &[m, tasks] : waveGroups) {
                                const u64 waveNow        = get_mono_ts_us();
                                i32       targetPeriodUs = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 20)
                                        targetPeriodUs = 20; // honor up to 50kHz (matches UI cap)

                                auto tickIt = lastMonitorWaveTicks.find(m);
                                if (tickIt == lastMonitorWaveTicks.end()) {
                                        lastMonitorWaveTicks[m] = waveNow;
                                        LOG_I("[WAVE-INIT] iter=%llu mon=%s tNow_ms=%llu target=%dus channels=%zu",
                                              iterCounter,
                                              m->getName().c_str(),
                                              get_mono_ts_ms() - sessionStartMs,
                                              targetPeriodUs,
                                              tasks.size());
                                        continue;
                                }
                                const u64 elapsedUs = waveNow - tickIt->second;
                                if (elapsedUs < static_cast<u64>(targetPeriodUs))
                                        continue;

                                const f32 dt   = static_cast<f32>(elapsedUs) * 1e-6f;
                                tickIt->second = waveNow;

                                auto &d = waveDiag[m];
                                d.count++;
                                d.sumDtUs += elapsedUs;
                                if (elapsedUs < d.minDtUs)
                                        d.minDtUs = elapsedUs;
                                if (elapsedUs > d.maxDtUs)
                                        d.maxDtUs = elapsedUs;

                                const bool isOutlier = (elapsedUs > (u64)targetPeriodUs * 2);
                                if (isOutlier)
                                        d.outliers++;

                                for (auto &wt : tasks) {
                                        if (wt.ch->isPendingDelete() || !wt.ch->waveEnable_)
                                                continue;
                                        // Apply any pending config from GUI (lock-free)
                                        wt.ch->applyPendingWaveCfg();
                                        wt.ch->wave_.cfg.fs = 1.0f / dt;
                                        f32 phaseSnapshot   = wt.ch->wave_.cfg.phase;
                                        wave_exec(&wt.ch->wave_);
                                        f32 outVal = wt.ch->wave_.out.val;

                                        const u64 wMemStart = get_mono_ts_us();
                                        if (wt.device == "JLINK") {
                                                if (!JLinkPort::instance().isConnected())
                                                        continue;
                                                JLinkPort::instance().writeMem(wt.addr, wt.nb, &outVal);
                                        } else if (wt.device == "SHM") {
                                                u8 wbuf[8] = {0};
                                                encodeFromF32(outVal, wt.type, wbuf);
                                                shm_write(&wt.ch->getShm(), wbuf, wt.nb);
                                                wt.ch->setRVal(decodeAs(wbuf, wt.type, 0, 0), sessionTimeSec());
                                        } else {
                                                continue;
                                        }
                                        const u64 wMemDur   = get_mono_ts_us() - wMemStart;
                                        loopDiag.sumWMemUs += wMemDur;
                                        loopDiag.wMemCount++;
                                        loopDiag.waveWrites++;
                                        if (wMemDur > loopDiag.maxWMemUs)
                                                loopDiag.maxWMemUs = wMemDur;
                                        if (isOutlier) {
                                                LOG_PERF("[WAVE-LAG] iter=%llu tNow_ms=%llu mon=%s dt=%lluus "
                                                         "target=%dus phase=%.4f val=%.4f wMemDur=%lluus addr=0x%x",
                                                         iterCounter,
                                                         get_mono_ts_ms() - sessionStartMs,
                                                         m->getName().c_str(),
                                                         elapsedUs,
                                                         targetPeriodUs,
                                                         phaseSnapshot,
                                                         outVal,
                                                         wMemDur,
                                                         wt.addr);
                                        } else if (wMemDur > 500) {
                                                LOG_PERF("[WMEM-LAG] iter=%llu tNow_ms=%llu mon=%s wMemDur=%lluus addr=0x%x",
                                                         iterCounter,
                                                         get_mono_ts_ms() - sessionStartMs,
                                                         m->getName().c_str(),
                                                         wMemDur,
                                                         wt.addr);
                                        }
                                }
                        }
                }

                // Drop tick entries for monitors that no longer have wave-enabled channels
                for (auto it = lastMonitorWaveTicks.begin(); it != lastMonitorWaveTicks.end();) {
                        bool stillActive = false;
                        for (const auto &wt : waveTasks) {
                                if (wt.monitor == it->first) {
                                        stillActive = true;
                                        break;
                                }
                        }
                        if (!stillActive)
                                it = lastMonitorWaveTicks.erase(it);
                        else
                                ++it;
                }
                // Same prune for waveDiag — without this, every deleted Monitor
                // stays pinned alive by its shared_ptr key (along with all its
                // scopes/channels/MmapVector files), leaking commit on every
                // add/remove cycle.
                for (auto it = waveDiag.begin(); it != waveDiag.end();) {
                        bool stillActive = false;
                        for (const auto &wt : waveTasks) {
                                if (wt.monitor == it->first) {
                                        stillActive = true;
                                        break;
                                }
                        }
                        if (!stillActive)
                                it = waveDiag.erase(it);
                        else
                                ++it;
                }

                static u64 loopCount = 0;
                if (loopCount++ % 1000 == 0 && JLinkPort::instance().isConnected()) {
                        LOG_PERF("Sampler Loop: isConnected=%d, isPaused=%d, tempChs=%zu, pollTasks=%zu, maxHssHz=%d",
                                 (int)JLinkPort::instance().isConnected(),
                                 (int)isPaused,
                                 tempChs.size(),
                                 pollTasks.size(),
                                 maxHssHz);
                }

                // Collect current HSS needs
                static std::vector<HssBlock>                        blocks;
                static std::vector<std::shared_ptr<MonitorChannel>> chans;
                static std::vector<std::shared_ptr<Monitor>>        chMonitors;
                static std::vector<std::string>                     lastTypes_current;
                static std::vector<u32>                             offsets;
                static std::vector<u32>                             bitOffsets;
                static std::vector<u32>                             bitSizes;
                blocks.clear();
                chans.clear();
                chMonitors.clear();
                lastTypes_current.clear();
                offsets.clear();
                bitOffsets.clear();
                bitSizes.clear();
                u32 curOff = 0;
                for (auto &tc : tempChs) {
                        blocks.push_back({tc.addr, tc.nb});
                        chans.push_back(tc.ch);
                        chMonitors.push_back(tc.monitor);
                        lastTypes_current.push_back(tc.type);
                        offsets.push_back(curOff);
                        bitOffsets.push_back(tc.bitOffset);
                        bitSizes.push_back(tc.bitSize);
                        curOff += tc.nb;
                }
                i32 periodUs = 1000000 / maxHssHz;

                static bool lastConnected = false;
                bool        isConnected   = JLinkPort::instance().isConnected();
                bool        changed       = (blocks != lastBlocks || periodUs != lastPeriodUs || isConnected != lastConnected ||
                                JLinkPort::instance().hasRestartReq());
                lastConnected             = isConnected;

                bool desiredRunning = !blocks.empty() && isConnected; // keep sampling even when paused
                if (desiredRunning && !JLinkPort::instance().isHssRunning()) {
                        changed = true;
                }

                if (changed && JLinkPort::instance().isHssRunning()) {
                        JLinkPort::instance().hssStop();
                        carryLen = 0;
                }

                if (desiredRunning && !JLinkPort::instance().isHssRunning()) {
                        auto nowTick     = get_mono_ts_ms();
                        bool justStopped = ((nowTick - lastHssStart) < 200);

                        if (!justStopped || changed) {
                                LOG_I("Attempting hssStart with %zu blocks, period %d us", blocks.size(), periodUs);
                                if (JLinkPort::instance().hssStart(blocks, periodUs)) {
                                        LOG_I("hssStart SUCCEEDED");
                                        lastHssStart       = nowTick;
                                        lastHssSessionTime = sessionTimeSec();
                                        lastBlocks         = blocks;
                                        lastChans          = chans;
                                        lastChMonitors     = chMonitors;
                                        lastTypes          = lastTypes_current;
                                        lastOffsets        = offsets;
                                        lastPeriodUs       = periodUs;
                                        carryLen           = 0;
                                        discardFirst       = true;
                                        hssTimeSynced      = false;
                                } else {
                                        LOG_E("hssStart FAILED");
                                        lastBlocks.clear();
                                        lastChans.clear();
                                        lastChMonitors.clear();
                                        lastTypes.clear();
                                        lastOffsets.clear();
                                }
                        }
                } else if (changed) {
                        lastBlocks     = blocks;
                        lastChans      = chans;
                        lastChMonitors = chMonitors;
                        lastTypes      = lastTypes_current;
                        lastOffsets    = offsets;
                        lastPeriodUs   = periodUs;
                        carryLen       = 0;
                        discardFirst   = true;
                        hssTimeSynced  = false;
                }

                // Returns true if the host read buffer came back full, i.e. the
                // device FIFO likely still holds more frames and processHss should
                // be invoked again this iteration to fully drain it.
                auto processHss = [&]() -> bool {
                        if (!JLinkPort::instance().isHssRunning() || lastChans.empty())
                                return false;

                        const u32 reqBytes     = static_cast<u32>(kBufCap - carryLen);
                        const u64 hssReadStart = get_mono_ts_us();
                        i32       total        = JLinkPort::instance().hssRead(buf + carryLen, reqBytes);
                        const u64 hssReadDur   = get_mono_ts_us() - hssReadStart;
                        // A full buffer means more frames are queued on the device.
                        const bool bufferFull = (total > 0 && static_cast<u32>(total) >= reqBytes);
                        loopDiag.hssReads++;
                        loopDiag.sumHssUs += hssReadDur;
                        if (hssReadDur > loopDiag.maxHssUs)
                                loopDiag.maxHssUs = hssReadDur;
                        if (hssReadDur > 2000) {
                                loopDiag.longHssReads++;
                                LOG_PERF("[HSS-LAG] iter=%llu tNow_ms=%llu hssReadDur=%lluus rawBytes=%d",
                                         iterCounter,
                                         get_mono_ts_ms() - sessionStartMs,
                                         hssReadDur,
                                         total);
                        }
                        if (total < 0)
                                total = 0;
                        total += static_cast<i32>(carryLen);

                        const i32 frameSize = JLinkPort::instance().hssFrameSize();
                        if (frameSize <= 0)
                                return false;

                        i32 frames = total / frameSize;
                        if (frames > 0) {
                                // After HSS restart the J-Link hardware buffer may still
                                // contain stale frames from the previous session.  Drop
                                // the entire first batch so these don't create a glitch
                                // point on the waveform.
                                if (discardFirst) {
                                        discardFirst  = false;
                                        hssTimeSynced = false;
                                        LOG_I("[HSS-DISCARD] dropping first %d frames (%d bytes) after restart",
                                              frames,
                                              frames * frameSize);
                                        const usize consumed = frames * static_cast<usize>(frameSize);
                                        carryLen             = total - consumed;
                                        if (carryLen > 0 && consumed > 0)
                                                std::memmove(buf, buf + consumed, carryLen);
                                        lastHssFrameMs = get_mono_ts_ms(); // stream is alive
                                        return bufferFull;                 // skip this batch entirely
                                }

                                lastHssFrameMs  = get_mono_ts_ms(); // stream is alive
                                hzFrameAccum   += static_cast<u64>(frames);
                                static std::vector<std::vector<f32>> pVals;
                                static std::vector<std::vector<f64>> pTs;
                                pVals.resize(lastChans.size());
                                pTs.resize(lastChans.size());
                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        pVals[i].clear();
                                        pTs[i].clear();
                                        pVals[i].reserve(frames);
                                        pTs[i].reserve(frames);
                                }

                                for (usize f = 0; f < frames; ++f) {
                                        const u8 *pFrame = buf + f * frameSize;
                                        u32       hwTs;
                                        std::memcpy(&hwTs, pFrame, 4);

                                        if (!hssTimeSynced) {
                                                hssLastHwTs     = hwTs;
                                                hssAccumUs      = 0;
                                                hssBaseWallTime = sessionTimeSec();
                                                hssTimeSynced   = true;
                                        }

                                        // The J-Link HSS hardware timestamp is a 32-bit microsecond counter
                                        // that wraps every 2^32 µs (~71.6 min). Accumulate the wrap-safe
                                        // per-frame delta into a 64-bit total so the timeline stays correct
                                        // over arbitrarily long runs. (The previous single-wrap diff was a
                                        // u32, so after ~71 min it overflowed — pinning the time axis to a
                                        // ~4295 s window and breaking history pruning → runaway memory.)
                                        const u32 deltaUs  = hwTs - hssLastHwTs; // modular u32 sub handles wrap
                                        hssLastHwTs        = hwTs;
                                        hssAccumUs        += deltaUs;

                                        const f64 ts = hssBaseWallTime + (static_cast<f64>(hssAccumUs) * 1e-6);

                                        const u8 *pData = pFrame + JLinkPort::kHssHeaderBytes;
                                        for (usize i = 0; i < lastChans.size(); ++i) {
                                                u8  raw[8]  = {0};
                                                u32 varSize = typeBytes(lastTypes[i]);
                                                std::memcpy(raw, pData + lastOffsets[i], varSize);
                                                pVals[i].push_back(decodeAs(raw, lastTypes[i], bitOffsets[i], bitSizes[i]));
                                                pTs[i].push_back(ts);
                                        }
                                }

                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        if (lastChans[i]->isPendingDelete())
                                                continue;
                                        lastChans[i]->pushBatch(pVals[i].data(), pTs[i].data(), pVals[i].size());
                                        loopDiag.hssSamples += pVals[i].size();
                                }

                                static std::vector<std::shared_ptr<Monitor>> uniqueMonitors;
                                uniqueMonitors.clear();
                                for (const auto &m : lastChMonitors) {
                                        if (std::find(uniqueMonitors.begin(), uniqueMonitors.end(), m) ==
                                            uniqueMonitors.end()) {
                                                uniqueMonitors.push_back(m);
                                        }
                                }
                                for (const auto &m : uniqueMonitors)
                                        m->addPoints(static_cast<u64>(frames));

                                JLinkPort::instance().addPoints(static_cast<u64>(frames) * static_cast<u64>(lastChans.size()));
                                const usize consumed = frames * static_cast<usize>(frameSize);
                                carryLen             = total - consumed;
                                if (carryLen > 0 && consumed > 0)
                                        std::memmove(buf, buf + consumed, carryLen);
                        }
                        return bufferFull;
                };

                // Execute POLL tasks.
                // Cap reads per monitor to kMaxPollPerIter to bound iteration
                // time and prevent wave-write starvation on the J-Link bus.
                static constexpr i32 kMaxPollPerIter = 4;
                // Hoisted out of the inner for-loop so we can prune entries
                // whose Monitor no longer has poll tasks (otherwise the
                // shared_ptr key keeps the deleted Monitor — and its entire
                // scope/channel/MmapVector graph — alive forever, leaking
                // commit on every Monitor add/remove cycle).
                static std::unordered_map<std::shared_ptr<Monitor>, u64>   lastMonitorPollTicks;
                static std::unordered_map<std::shared_ptr<Monitor>, usize> pollRoundRobin;
                for (auto it = lastMonitorPollTicks.begin(); it != lastMonitorPollTicks.end();) {
                        bool stillActive = false;
                        for (const auto &pt : pollTasks) {
                                if (pt.monitor == it->first) {
                                        stillActive = true;
                                        break;
                                }
                        }
                        if (!stillActive) {
                                pollRoundRobin.erase(it->first);
                                it = lastMonitorPollTicks.erase(it);
                        } else {
                                ++it;
                        }
                }
                if (!pollTasks.empty()) {
                        static std::vector<std::pair<std::shared_ptr<Monitor>, std::vector<TempCh>>> monitorPollGroups;
                        for (auto &pair : monitorPollGroups)
                                pair.second.clear();
                        monitorPollGroups.clear();

                        for (auto &pt : pollTasks) {
                                bool found = false;
                                for (auto &pair : monitorPollGroups) {
                                        if (pair.first == pt.monitor) {
                                                pair.second.push_back(pt);
                                                found = true;
                                                break;
                                        }
                                }
                                if (!found) {
                                        monitorPollGroups.push_back({pt.monitor, {pt}});
                                }
                        }

                        for (auto &[m, tasks] : monitorPollGroups) {
                                auto nowPoll        = get_mono_ts_us();
                                i32  targetPeriodUs = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 20)
                                        targetPeriodUs = 20; // best-effort up to 50kHz; real rate limited by USB latency

                                if (lastMonitorPollTicks.find(m) == lastMonitorPollTicks.end()) {
                                        lastMonitorPollTicks[m] = nowPoll;
                                        pollRoundRobin[m]       = 0;
                                        continue;
                                }
                                auto elapsedUs = nowPoll - lastMonitorPollTicks[m];
                                if (elapsedUs >= (u64)targetPeriodUs) {
                                        lastMonitorPollTicks[m] = nowPoll;

                                        const usize total   = tasks.size();
                                        usize      &rrIdx   = pollRoundRobin[m];
                                        const i32   nReads  = static_cast<i32>(std::min(total, (usize)kMaxPollPerIter));
                                        bool        counted = false;

                                        for (i32 i = 0; i < nReads; ++i) {
                                                auto &pt = tasks[(rrIdx + i) % total];
                                                if (pt.ch->isPendingDelete())
                                                        continue;
                                                if (JLinkPort::instance().isConnected()) {
                                                        if (!counted) {
                                                                hzFrameAccum++;
                                                                m->addPoints(1);
                                                                counted = true;
                                                        }
                                                        u8 rbuf[8] = {0};
                                                        if (JLinkPort::instance().readMem(pt.addr, pt.nb, rbuf)) {
                                                                pt.ch->setRVal(
                                                                    decodeAs(rbuf, pt.type, pt.bitOffset, pt.bitSize),
                                                                    sessionTimeSec());
                                                                loopDiag.pollSamples++;
                                                        }
                                                }
                                        }
                                        rrIdx = (rrIdx + nReads) % total;
                                }
                        }
                }

                // Drain the device FIFO this iteration. processHss() returns true
                // while the host buffer keeps coming back full, meaning the device
                // still has queued frames. Looping here prevents a backlog from
                // persisting across frames (each call still takes the J-Link lock
                // independently, so GUI-thread reads interleave between drains). The
                // cap bounds worst-case iteration time.
                static constexpr int kMaxHssDrains = 8;
                for (int d = 0; d < kMaxHssDrains && processHss(); ++d) {
                }

                // HSS watchdog: if the stream is supposed to be running but no
                // frames have arrived for a sustained window, the FIFO has wedged.
                // Force a clean restart (same recovery a manual reconnect performs),
                // rate-limited so a genuinely silent target can't thrash the link.
                if (desiredRunning && JLinkPort::instance().isHssRunning()) {
                        const u64 nowMs = get_mono_ts_ms();
                        if (nowMs - lastHssFrameMs > 2000 && nowMs - lastHssWatchdogMs > 5000) {
                                LOG_W("[HSS-WATCHDOG] no frames for %llums while streaming — forcing HSS restart",
                                      nowMs - lastHssFrameMs);
                                JLinkPort::instance().reqRestart();
                                lastHssWatchdogMs = nowMs;
                                lastHssFrameMs    = nowMs; // avoid immediate re-trigger before restart settles
                        }
                } else {
                        // Not streaming (paused/disconnected/no channels): keep the
                        // frame clock fresh so the watchdog doesn't fire on resume.
                        lastHssFrameMs = get_mono_ts_ms();
                }

                // Publish snapshots — try_lock only. If GUI is holding the
                // lock (rendering), skip this iteration; publishSnapshot is
                // incremental so the next successful publish catches up.
                {
                        std::unique_lock<std::mutex> lk(gui->getMonitorMtx(), std::try_to_lock);
                        if (lk.owns_lock()) {
                                loopDiag.publishOk++;
                                for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                        monitor->updateHz();
                                        for (auto &scope : monitor->getScopes() | std::views::values) {
                                                // While paused, keep capturing but don't push new
                                                // samples to the GUI view (freezes the plot/table).
                                                if (!isPaused)
                                                        for (auto &[_, ch] : scope->getChannels())
                                                                ch->publishSnapshot();
                                                scope->purgeDeleted();
                                        }
                                        monitor->purgeDeletedScopes();
                                }
                        } else {
                                loopDiag.publishSkip++;
                        }
                }

                {
                        const auto now  = get_mono_ts_ms();
                        const auto dtMs = now - hzLastTick;
                        if (dtMs >= 500) {
                                const f32 hz =
                                    static_cast<f32>(static_cast<f64>(hzFrameAccum) * 1000.0 / static_cast<f64>(dtMs));
                                JLinkPort::instance().setActualHz(hz);
                                hzFrameAccum = 0;
                                hzLastTick   = now;
                        }
                }

                // ---------- Diagnostics ----------
                {
                        const u64 iterDurUs = get_mono_ts_us() - iterStartUs;
                        loopDiag.iter++;
                        loopDiag.sumIterUs += iterDurUs;
                        if (iterDurUs > loopDiag.maxIterUs)
                                loopDiag.maxIterUs = iterDurUs;
                        if (iterDurUs > 3000) {
                                loopDiag.longIters++;
                                LOG_PERF("[LOOP-LAG] iter=%llu tNow_ms=%llu iterDur=%lluus "
                                         "hssReadsThisIter=%llu waveTasks=%zu pollTasks=%zu",
                                         iterCounter,
                                         get_mono_ts_ms() - sessionStartMs,
                                         iterDurUs,
                                         loopDiag.hssReads,
                                         waveTasks.size(),
                                         pollTasks.size());
                        }

                        const u64 nowMs = get_mono_ts_ms();
                        if (nowMs - diagLastDumpMs >= 500) {
                                const u64 spanMs = nowMs - diagLastDumpMs;
                                if (loopDiag.iter > 0) {
                                        const u64 avgIter = loopDiag.sumIterUs / loopDiag.iter;
                                        const u64 avgHss  = loopDiag.hssReads ? loopDiag.sumHssUs / loopDiag.hssReads : 0;
                                        const u64 avgWMem = loopDiag.wMemCount ? loopDiag.sumWMemUs / loopDiag.wMemCount : 0;
                                        LOG_PERF("[DIAG-LOOP %llums] tNow_ms=%llu iter=%llu avgIter=%lluus "
                                                 "maxIter=%lluus longIter(>3ms)=%llu "
                                                 "hssReads=%llu avgHss=%lluus maxHss=%lluus longHss(>2ms)=%llu "
                                                 "wMemCount=%llu avgWMem=%lluus maxWMem=%lluus",
                                                 spanMs,
                                                 nowMs - sessionStartMs,
                                                 loopDiag.iter,
                                                 avgIter,
                                                 loopDiag.maxIterUs,
                                                 loopDiag.longIters,
                                                 loopDiag.hssReads,
                                                 avgHss,
                                                 loopDiag.maxHssUs,
                                                 loopDiag.longHssReads,
                                                 loopDiag.wMemCount,
                                                 avgWMem,
                                                 loopDiag.maxWMemUs);

                                        // Lock contention summary
                                        const u64 refreshTotal = loopDiag.refreshTryOk + loopDiag.refreshTryFail;
                                        const u64 publishTotal = loopDiag.publishOk + loopDiag.publishSkip;
                                        const u64 avgOverdueWait =
                                            loopDiag.refreshOverdueBlock
                                                ? loopDiag.refreshOverdueWaitUs / loopDiag.refreshOverdueBlock
                                                : 0;
                                        LOG_PERF(
                                            "[DIAG-LOCK %llums] refreshOk=%llu/%llu(%.1f%% miss) "
                                            "overdueBlock=%llu avgOverdueWait=%lluus maxOverdueWait=%lluus "
                                            "maxStaleRefresh=%lluus publishOk=%llu/%llu(%.1f%% skip)",
                                            spanMs,
                                            loopDiag.refreshTryOk,
                                            refreshTotal,
                                            refreshTotal ? 100.0 * (double)loopDiag.refreshTryFail / (double)refreshTotal : 0.0,
                                            loopDiag.refreshOverdueBlock,
                                            avgOverdueWait,
                                            loopDiag.maxRefreshOverdueUs,
                                            loopDiag.maxStaleRefreshUs,
                                            loopDiag.publishOk,
                                            publishTotal,
                                            publishTotal ? 100.0 * (double)loopDiag.publishSkip / (double)publishTotal : 0.0);

                                        // Per-source sample throughput (this 500ms window)
                                        LOG_PERF("[DIAG-RATE %llums] shm=%llu poll=%llu hss=%llu waveWr=%llu wvalWr=%llu",
                                                 spanMs,
                                                 loopDiag.shmSamples,
                                                 loopDiag.pollSamples,
                                                 loopDiag.hssSamples,
                                                 loopDiag.waveWrites,
                                                 loopDiag.wvalWrites);
                                }
                                for (auto &[m, d] : waveDiag) {
                                        if (d.count == 0)
                                                continue;
                                        const u64 avgDt = d.sumDtUs / d.count;
                                        LOG_PERF("[DIAG-WAVE %llums] tNow_ms=%llu mon=%s writes=%llu "
                                                 "outliers(>2xT)=%llu minDt=%lluus avgDt=%lluus maxDt=%lluus",
                                                 spanMs,
                                                 nowMs - sessionStartMs,
                                                 m->getName().c_str(),
                                                 d.count,
                                                 d.outliers,
                                                 d.minDtUs,
                                                 avgDt,
                                                 d.maxDtUs);
                                        d = WaveDiag{};
                                }
                                loopDiag       = LoopDiag{};
                                diagLastDumpMs = nowMs;
                        }

                        // 1Hz heartbeat — proves the sampler is alive and gives a
                        // snapshot of session state so post-crash log analysis
                        // shows what the app was doing right before the crash.
                        if (nowMs - hbLastDumpMs >= 1000) {
                                usize totalChans = 0, totalScopes = 0, totalMons = 0;
                                u64   totalPts = 0;
                                {
                                        std::unique_lock<std::mutex> lk(gui->getMonitorMtx(), std::try_to_lock);
                                        if (lk.owns_lock()) {
                                                for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                                        ++totalMons;
                                                        for (auto &scope : monitor->getScopes() | std::views::values) {
                                                                ++totalScopes;
                                                                for (auto &[_, ch] : scope->getChannels()) {
                                                                        ++totalChans;
                                                                        totalPts += ch->storedCount();
                                                                }
                                                        }
                                                }
                                        }
                                }
                                // Process memory + sampler-side container sizes — surfaces
                                // any growth that's invisible from the GUI side (leaked
                                // shared_ptr<Monitor> keys, etc).
                                size_t privBytes = 0, wsBytes = 0;
#ifdef _WIN32
                                PROCESS_MEMORY_COUNTERS_EX pmc{};
                                if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
                                        privBytes = pmc.PrivateUsage;
                                        wsBytes   = pmc.WorkingSetSize;
                                }
#endif
                                LOG_PERF("[HB-MEM] privBytes=%zu wsBytes=%zu "
                                         "waveDiag=%zu lastWaveT=%zu lastPollT=%zu pollRR=%zu",
                                         privBytes,
                                         wsBytes,
                                         waveDiag.size(),
                                         lastMonitorWaveTicks.size(),
                                         lastMonitorPollTicks.size(),
                                         pollRoundRobin.size());

                                LOG_PERF("[HB tNow_ms=%llu] connected=%d hssRunning=%d paused=%d "
                                         "monitors=%zu scopes=%zu channels=%zu pts=%llu "
                                         "shmTasks=%zu pollTasks=%zu waveTasks=%zu tempChs=%zu runMode=%d",
                                         nowMs - sessionStartMs,
                                         (int)JLinkPort::instance().isConnected(),
                                         (int)JLinkPort::instance().isHssRunning(),
                                         (int)g_monitorPaused.load(),
                                         totalMons,
                                         totalScopes,
                                         totalChans,
                                         totalPts,
                                         shmTasks.size(),
                                         pollTasks.size(),
                                         waveTasks.size(),
                                         tempChs.size(),
                                         g_samplerRunMode.load());
                                hbLastDumpMs = nowMs;
                        }
                }

                // Sleep strategy depends on run mode.
                const int runMode = g_samplerRunMode.load(std::memory_order_relaxed);
                if (runMode == 0) {
                        // Low: always sleep 1ms — minimal CPU footprint.
#ifdef _WIN32
                        Sleep(1);
#else
                        usleep(1000);
#endif
                } else if (runMode == 1) {
                        // Normal: adaptive — sleep when idle, yield when active.
                        if (waveTasks.empty() && !JLinkPort::instance().isHssRunning() && pollTasks.empty()) {
#ifdef _WIN32
                                Sleep(1);
#else
                                usleep(1000);
#endif
                        } else {
#ifdef _WIN32
                                SwitchToThread();
#else
                                sched_yield();
#endif
                        }
                } else {
                        // CPUBound: spin — no sleep, minimum jitter.
                        // The J-Link USB I/O already blocks 200-500µs per call so
                        // throughput is not affected; we just skip voluntary yields.
#ifdef _WIN32
                        YieldProcessor();
#elif defined(__GNUC__)
                        __builtin_ia32_pause();
#endif
                }
        }
}

// Background FFT worker. Each pass: snapshot the FFT-enabled scopes (holding
// shared_ptrs so they stay alive), then run each scope's transform off the GUI
// thread. Capped at ~60 Hz — recomputing faster than the display refreshes is
// wasted work, and the GUI always draws the most recent published spectrum.
void
fftThreadFunc(Gui *gui)
{
        LOG_I("FFT worker thread started.");
        while (g_appRunning.load()) {
                // Hold both the scope AND its owning monitor alive (scope->parent_ is a
                // raw Monitor*; keeping the monitor shared_ptr prevents it dangling if
                // the monitor is removed between enumeration and processing).
                std::vector<std::pair<std::shared_ptr<Monitor>, std::shared_ptr<MonitorScope>>> work;
                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values)
                                for (auto &scope : monitor->getScopes() | std::views::values)
                                        if (scope && !scope->isPendingDelete() && scope->getShowFft())
                                                work.emplace_back(monitor, scope);
                }

                for (auto &[monitor, scope] : work)
                        scope->fftWorkerStep(gui->getMonitorMtx());

                delay_us(16000); // ~60 Hz
        }
        LOG_I("FFT worker thread stopped.");
}
