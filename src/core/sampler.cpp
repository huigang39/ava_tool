/**
 * @file  sampler.cpp
 * @brief Sampler thread implementation — HSS / POLL / SHM / Wave processing.
 */
#include <cstring>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

void
threadFunc(Gui *gui)
{
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
        };
        LoopDiag loopDiag{};
        u64      diagLastDumpMs = get_mono_ts_ms();
        u64      sessionStartMs = get_mono_ts_ms();
        u64      iterCounter    = 0;

        static constexpr usize kBufCap = 64 * 1024;
        static u8              buf[kBufCap];
        usize                  carryLen     = 0;
        bool                   discardFirst = false;

        u64 hzFrameAccum       = 0;
        u64 hzLastTick         = get_mono_ts_ms();
        u64 lastHssStart       = get_mono_ts_ms();
        f64 lastHssSessionTime = 0;

        bool lastPaused      = false;
        f64  nextHssTs       = -1.0;
        f64  hssBaseWallTime = 0;
        u32  hssBaseHwTs     = 0;
        bool hssTimeSynced   = false;

        // Internal task structs
        struct TempCh {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             addr;
                u32                             nb;
                std::string                     type;
        };
        struct WValTask {
                std::shared_ptr<MonitorChannel> ch;
                u32                             addr, nb;
                std::string                     type;
                f32                             wv;
        };
        struct ShmTask {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             nb;
                std::string                     type;
        };
        struct WaveTask {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             addr;
                u32                             nb;
        };

        while (g_appRunning.load()) {
                const u64 iterStartUs = get_mono_ts_us();
                iterCounter++;
                bool isPaused = g_monitorPaused.load();
                if (!isPaused && lastPaused) {
                        LOG_I("Resuming from pause, restarting HSS");
                        if (JLinkPort::instance().isHssRunning()) {
                                JLinkPort::instance().hssStop();
                        }
                        carryLen     = 0;
                        discardFirst = true;
                        nextHssTs    = -1.0;
                        lastBlocks.clear();
                }
                lastPaused = isPaused;

                std::vector<TempCh>   tempChs;
                std::vector<TempCh>   pollTasks;
                std::vector<WValTask> wvalTasks;
                std::vector<ShmTask>  shmTasks;
                std::vector<WaveTask> waveTasks;

                i32 maxHssHz = 1;

                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                if (monitor->consumeClearDataRequest()) {
                                        LOG_I("Sampler applying clear request: monitor=%s", monitor->getName().c_str());
                                        monitor->clearData();
                                        JLinkPort::instance().resetPoints();
                                        carryLen      = 0;
                                        discardFirst  = true;
                                        hssTimeSynced = false;
                                }

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
                                                        shmTasks.push_back({ch, monitor, nb, ch->getType()});
                                                } else if (dev == "JLINK" && ch->getAddr() != 0) {
                                                        if (monitor->samplingMode_ == Monitor::SamplingMode::HSS) {
                                                                tempChs.push_back({ch,
                                                                                   monitor,
                                                                                   static_cast<u32>(ch->getAddr()),
                                                                                   nb,
                                                                                   ch->getType()});
                                                                hasHss = true;
                                                        } else {
                                                                pollTasks.push_back({ch,
                                                                                     monitor,
                                                                                     static_cast<u32>(ch->getAddr()),
                                                                                     nb,
                                                                                     ch->getType()});
                                                        }
                                                        f32 wv;
                                                        if (ch->consumeWValDirty(wv))
                                                                wvalTasks.push_back({ch,
                                                                                     static_cast<u32>(ch->getAddr()),
                                                                                     nb,
                                                                                     ch->getType(),
                                                                                     wv});
                                                        if (ch->waveEnable_)
                                                                waveTasks.push_back(
                                                                    {ch, monitor, static_cast<u32>(ch->getAddr()), nb});
                                                }
                                        }
                                }
                                if (hasHss && monitor->maxSampleHz_ > maxHssHz) {
                                        maxHssHz = monitor->maxSampleHz_;
                                }
                        }
                }

                // Drain SHM buffers outside the monitor lock
                for (auto &st : shmTasks) {
                        u8 raw[8];
                        while (shm_read(&st.ch->getShm(), raw, st.nb) == st.nb) {
                                st.ch->setRVal(decodeAs(raw, st.type), sessionTimeSec());
                                st.monitor->addPoints(1);
                                hzFrameAccum++;
                        }
                }

                // Write wVal dirty values
                if (JLinkPort::instance().isConnected()) {
                        for (auto &wt : wvalTasks) {
                                u8 wbuf[8] = {0};
                                encodeFromF32(wt.wv, wt.type, wbuf);
                                JLinkPort::instance().writeMem(wt.addr, wt.nb, wbuf);
                        }
                }

                // Wave generation
                if (JLinkPort::instance().isConnected() && !waveTasks.empty()) {
                        std::unordered_map<std::shared_ptr<Monitor>, std::vector<WaveTask>> waveGroups;
                        for (auto &wt : waveTasks)
                                waveGroups[wt.monitor].push_back(wt);

                        for (auto &[m, tasks] : waveGroups) {
                                const u64 waveNow        = get_mono_ts_us();
                                i32       targetPeriodUs = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 1000)
                                        targetPeriodUs = 1000;

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
                                        // Apply any pending config from GUI (lock-free)
                                        wt.ch->applyPendingWaveCfg();
                                        wt.ch->wave_.cfg.fs = 1.0f / dt;
                                        f32 phaseSnapshot   = wt.ch->wave_.cfg.phase;
                                        wave_exec(&wt.ch->wave_);
                                        f32 outVal = wt.ch->wave_.out.val;

                                        const u64 wMemStart = get_mono_ts_us();
                                        JLinkPort::instance().writeMem(wt.addr, wt.nb, &outVal);
                                        const u64 wMemDur   = get_mono_ts_us() - wMemStart;
                                        loopDiag.sumWMemUs += wMemDur;
                                        loopDiag.wMemCount++;
                                        if (wMemDur > loopDiag.maxWMemUs)
                                                loopDiag.maxWMemUs = wMemDur;
                                        if (isOutlier) {
                                                LOG_I("[WAVE-LAG] iter=%llu tNow_ms=%llu mon=%s dt=%lluus "
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
                                                LOG_I("[WMEM-LAG] iter=%llu tNow_ms=%llu mon=%s wMemDur=%lluus addr=0x%x",
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

                static u64 loopCount = 0;
                if (loopCount++ % 1000 == 0 && JLinkPort::instance().isConnected()) {
                        LOG_I("Sampler Loop: isConnected=%d, isPaused=%d, tempChs=%zu, pollTasks=%zu, maxHssHz=%d",
                              (int)JLinkPort::instance().isConnected(),
                              (int)isPaused,
                              tempChs.size(),
                              pollTasks.size(),
                              maxHssHz);
                }

                // Collect current HSS needs
                std::vector<HssBlock>                        blocks;
                std::vector<std::shared_ptr<MonitorChannel>> chans;
                std::vector<std::shared_ptr<Monitor>>        chMonitors;
                std::vector<std::string>                     lastTypes_current;
                std::vector<u32>                             offsets;
                u32                                          curOff = 0;
                for (auto &tc : tempChs) {
                        blocks.push_back({tc.addr, tc.nb});
                        chans.push_back(tc.ch);
                        chMonitors.push_back(tc.monitor);
                        lastTypes_current.push_back(tc.type);
                        offsets.push_back(curOff);
                        curOff += tc.nb;
                }
                i32 periodUs = 1000000 / maxHssHz;

                static bool lastConnected = false;
                bool        isConnected   = JLinkPort::instance().isConnected();
                bool        changed       = (blocks != lastBlocks || periodUs != lastPeriodUs || isConnected != lastConnected ||
                                JLinkPort::instance().hasRestartReq());
                lastConnected             = isConnected;

                bool desiredRunning = !blocks.empty() && !isPaused && isConnected;
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

                auto processHss = [&]() {
                        if (isPaused || !JLinkPort::instance().isHssRunning() || lastChans.empty())
                                return;

                        const u64 hssReadStart = get_mono_ts_us();
                        i32       total = JLinkPort::instance().hssRead(buf + carryLen, static_cast<u32>(kBufCap - carryLen));
                        const u64 hssReadDur = get_mono_ts_us() - hssReadStart;
                        loopDiag.hssReads++;
                        loopDiag.sumHssUs += hssReadDur;
                        if (hssReadDur > loopDiag.maxHssUs)
                                loopDiag.maxHssUs = hssReadDur;
                        if (hssReadDur > 2000) {
                                loopDiag.longHssReads++;
                                LOG_I("[HSS-LAG] iter=%llu tNow_ms=%llu hssReadDur=%lluus rawBytes=%d",
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
                                return;

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
                                        return; // skip this batch entirely
                                }

                                hzFrameAccum += static_cast<u64>(frames);
                                std::vector<std::vector<f32>> pVals(lastChans.size());
                                std::vector<std::vector<f64>> pTs(lastChans.size());
                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        pVals[i].reserve(frames);
                                        pTs[i].reserve(frames);
                                }

                                for (usize f = 0; f < frames; ++f) {
                                        const u8 *pFrame = buf + f * frameSize;
                                        u32       hwTs;
                                        std::memcpy(&hwTs, pFrame, 4);

                                        if (!hssTimeSynced) {
                                                hssBaseHwTs     = hwTs;
                                                hssBaseWallTime = sessionTimeSec();
                                                hssTimeSynced   = true;
                                        }

                                        u32 diff = 0;
                                        if (hwTs >= hssBaseHwTs) {
                                                diff = hwTs - hssBaseHwTs;
                                        } else {
                                                diff = (0xFFFFFFFF - hssBaseHwTs) + hwTs + 1;
                                        }

                                        const f64 ts = hssBaseWallTime + (static_cast<f64>(diff) * 1e-6);

                                        const u8 *pData = pFrame + JLinkPort::kHssHeaderBytes;
                                        for (usize i = 0; i < lastChans.size(); ++i) {
                                                u8  raw[8]  = {0};
                                                u32 varSize = typeBytes(lastTypes[i]);
                                                std::memcpy(raw, pData + lastOffsets[i], varSize);
                                                pVals[i].push_back(decodeAs(raw, lastTypes[i]));
                                                pTs[i].push_back(ts);
                                        }
                                }

                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        lastChans[i]->pushBatch(pVals[i].data(), pTs[i].data(), pVals[i].size());
                                }

                                std::unordered_set<std::shared_ptr<Monitor>> uniqueMonitors;
                                for (const auto &m : lastChMonitors)
                                        uniqueMonitors.insert(m);
                                for (const auto &m : uniqueMonitors)
                                        m->addPoints(static_cast<u64>(frames));

                                JLinkPort::instance().addPoints(static_cast<u64>(frames) * static_cast<u64>(lastChans.size()));
                                const usize consumed = frames * static_cast<usize>(frameSize);
                                carryLen             = total - consumed;
                                if (carryLen > 0 && consumed > 0)
                                        std::memmove(buf, buf + consumed, carryLen);
                        }
                };

                // Execute POLL tasks.
                // Cap reads per monitor to kMaxPollPerIter to bound iteration
                // time and prevent wave-write starvation on the J-Link bus.
                static constexpr i32 kMaxPollPerIter = 4;
                if (!isPaused && !pollTasks.empty()) {
                        std::unordered_map<std::shared_ptr<Monitor>, std::vector<TempCh>> monitorPollGroups;
                        for (auto &pt : pollTasks)
                                monitorPollGroups[pt.monitor].push_back(pt);

                        for (auto &[m, tasks] : monitorPollGroups) {
                                static std::unordered_map<std::shared_ptr<Monitor>, u64>   lastMonitorPollTicks;
                                static std::unordered_map<std::shared_ptr<Monitor>, usize> pollRoundRobin;
                                auto                                                       nowPoll = get_mono_ts_us();
                                i32 targetPeriodUs                                                 = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 1000)
                                        targetPeriodUs = 1000;

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
                                                if (JLinkPort::instance().isConnected()) {
                                                        if (!counted) {
                                                                hzFrameAccum++;
                                                                m->addPoints(1);
                                                                counted = true;
                                                        }
                                                        u8 rbuf[8] = {0};
                                                        if (JLinkPort::instance().readMem(pt.addr, pt.nb, rbuf)) {
                                                                pt.ch->setRVal(decodeAs(rbuf, pt.type), sessionTimeSec());
                                                        }
                                                }
                                        }
                                        rrIdx = (rrIdx + nReads) % total;
                                }
                        }
                }

                processHss();

                // Single lock acquisition: publish snapshots + updateHz + purge
                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                monitor->updateHz();
                                for (auto &scope : monitor->getScopes() | std::views::values) {
                                        for (auto &[_, ch] : scope->getChannels())
                                                ch->publishSnapshot();
                                        scope->purgeDeleted();
                                }
                                monitor->purgeDeletedScopes();
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
                                LOG_I("[LOOP-LAG] iter=%llu tNow_ms=%llu iterDur=%lluus "
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
                                        LOG_I("[DIAG-LOOP %llums] tNow_ms=%llu iter=%llu avgIter=%lluus "
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
                                }
                                for (auto &[m, d] : waveDiag) {
                                        if (d.count == 0)
                                                continue;
                                        const u64 avgDt = d.sumDtUs / d.count;
                                        LOG_I("[DIAG-WAVE %llums] tNow_ms=%llu mon=%s writes=%llu "
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
                }

                // Shorter sleep when wave is active to keep write timing tight.
                delay_us(waveTasks.empty() ? 1000 : 200);
        }
}
