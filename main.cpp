#include <cstring>
#include <memory>
#include <ranges>
#include <thread>
#include <vector>
#include <unordered_set>

#include "module.h"

#include "gui.hpp"
#include "jlink_dev.hpp"
#include "wave.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

fft_t fft;

u64               cnt;
std::atomic<bool> g_appRunning{true};
extern std::atomic<bool> g_monitorPaused;

static f32
decodeAs(const u8 *raw, const std::string &type)
{
        if (type == "U8") {
                u8 v;
                std::memcpy(&v, raw, 1);
                return static_cast<f32>(v);
        }
        if (type == "I8") {
                i8 v;
                std::memcpy(&v, raw, 1);
                return static_cast<f32>(v);
        }
        if (type == "U16") {
                u16 v;
                std::memcpy(&v, raw, 2);
                return static_cast<f32>(v);
        }
        if (type == "I16") {
                i16 v;
                std::memcpy(&v, raw, 2);
                return static_cast<f32>(v);
        }
        if (type == "I32") {
                i32 v;
                std::memcpy(&v, raw, 4);
                return static_cast<f32>(v);
        }

        if (type == "F32") {
                f32 f;
                std::memcpy(&f, raw, 4);
                return f;
        }
        if (type == "U64") {
                u64 v;
                std::memcpy(&v, raw, 8);
                return static_cast<f32>(v);
        }
        if (type == "I64") {
                i64 v;
                std::memcpy(&v, raw, 8);
                return static_cast<f32>(v);
        }
        if (type == "F64") {
                double d;
                std::memcpy(&d, raw, 8);
                return static_cast<f32>(d);
        }
        // 默认 / U32
        u32 v;
        std::memcpy(&v, raw, 4);
        return static_cast<f32>(v);
}

// 用于写回 target. 把 f32 转成对应类型的字节序列, 长度 = typeBytes(type).
static void
encodeFromF32(const f32 val, const std::string &type, u8 *out)
{
        if (type == "F32") {
                std::memcpy(out, &val, 4);
                return;
        }
        if (type == "F64") {
                const double d = static_cast<double>(val);
                std::memcpy(out, &d, 8);
                return;
        }
        if (type == "I8") {
                const i8 v = static_cast<i8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "I16") {
                const i16 v = static_cast<i16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "I32") {
                const i32 v = static_cast<i32>(val);
                std::memcpy(out, &v, 4);
                return;
        }
        if (type == "I64") {
                const i64 v = static_cast<i64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        if (type == "U8") {
                const u8 v = static_cast<u8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "U16") {
                const u16 v = static_cast<u16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "U64") {
                const u64 v = static_cast<u64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        // 默认 / U32
        const u32 v = static_cast<u32>(val);
        std::memcpy(out, &v, 4);
}

void
threadFunc(Gui *gui)
{
        std::vector<HssBlock>         lastBlocks;
        std::vector<MonitorChannel *> lastChans;
        std::vector<Monitor *>        lastChMonitors;
        std::vector<std::string>      lastTypes;
        std::vector<u32>              lastOffsets; 
        int                           lastPeriodUs = 0;

        static constexpr usize kBufCap = 64 * 1024;
        static u8              buf[kBufCap];
        usize                  carryLen     = 0;
        bool                   discardFirst = false;

        u64                                   hzFrameAccum = 0;
        std::chrono::steady_clock::time_point hzLastTick   = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastHssStart = std::chrono::steady_clock::now();
        double                                lastHssSessionTime = 0;
        std::chrono::steady_clock::time_point lastWaveTick = std::chrono::steady_clock::now();

        bool   lastPaused = false;
        double nextHssTs  = -1.0;
        double hssBaseWallTime = 0;
        u32    hssBaseHwTs     = 0;
        bool   hssTimeSynced   = false;

        while (g_appRunning.load()) {
                bool isPaused = g_monitorPaused.load();
                if (!isPaused && lastPaused) {
                        // Resume from pause: Flush J-Link buffers to avoid stale data distortion
                        if (JLinkDev::instance().isHssRunning()) {
                                JLinkDev::instance().hssStop();
                        }
                        carryLen = 0;
                        discardFirst = true;
                        nextHssTs = -1.0; // Reset timer
                        // Forcing a "changed" state will trigger hssStart below
                        lastBlocks.clear(); 
                }
                lastPaused = isPaused;

                struct TempCh {
                        MonitorChannel *ch;
                        Monitor        *monitor;
                        u32             addr;
                        u32             nb;
                        std::string     type;
                };
                std::vector<TempCh> tempChs;
                std::vector<TempCh> pollTasks;
                std::vector<TempCh> waveTasks;

                int maxHssHz = 1;

                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                bool hasHss = false;
                                for (auto &scope : monitor->getScopes() | std::views::values) {
                                        if (scope->isPendingDelete() || scope->isPaused())
                                                continue;
                                        for (auto &ch : scope->getChannels() | std::views::values) {
                                                if (ch->isPendingDelete())
                                                        continue;
                                                const std::string &dev = ch->getDevice();
                                                if (dev == "LOCAL") {
                                                        u64 val = 0;
                                                        shm_read(&ch->getShm(), &val, sizeof(val));
                                                        u8 raw[8];
                                                        std::memcpy(raw, &val, sizeof(raw));
                                                        ch->setRVal(decodeAs(raw, ch->getType()), sessionTimeSec());
                                                } else if (dev == "JLINK" && ch->getAddr() != 0) {
                                                        u32 nb = ch->getNumBytes();
                                                        if (nb == 0) nb = typeBytes(ch->getType());
                                                        
                                                        if (monitor->samplingMode_ == Monitor::SamplingMode::HSS) {
                                                                tempChs.push_back({ch.get(), monitor.get(), static_cast<u32>(ch->getAddr()), nb, ch->getType()});
                                                                hasHss = true;
                                                        } else {
                                                                pollTasks.push_back({ch.get(), monitor.get(), static_cast<u32>(ch->getAddr()), nb, ch->getType()});
                                                        }

                                                        if (ch->waveEnable_) {
                                                                waveTasks.push_back({ch.get(), monitor.get(), static_cast<u32>(ch->getAddr()), nb, ch->getType()});
                                                        }

                                                        f32 wv;
                                                        if (ch->consumeWValDirty(wv) && JLinkDev::instance().isConnected()) {
                                                                u8 wbuf[8] = {0};
                                                                encodeFromF32(wv, ch->getType(), wbuf);
                                                                JLinkDev::instance().writeMem(static_cast<u32>(ch->getAddr()), nb, wbuf);
                                                        }
                                                }
                                        }
                                }
                                if (hasHss && monitor->maxSampleHz_ > maxHssHz) {
                                        maxHssHz = monitor->maxSampleHz_;
                                }
                        }
                }

                // Execute WaveGen logic (Grouped by monitor to respect MaxHz)
                if (!waveTasks.empty() && JLinkDev::instance().isConnected()) {
                        std::unordered_map<Monitor*, std::vector<TempCh>> monitorWaveGroups;
                        for (auto &wt : waveTasks) monitorWaveGroups[wt.monitor].push_back(wt);

                        for (auto &[m, tasks] : monitorWaveGroups) {
                                static std::unordered_map<Monitor*, std::chrono::steady_clock::time_point> lastMonitorWaveTicks;
                                auto waveNow = std::chrono::steady_clock::now();
                                int targetPeriodUs = 1000000 / m->maxSampleHz_;
                                // No artificial cap here, as waveforms can be high-speed if desired, 
                                // but we follow the user's monitor setting.

                                auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(waveNow - lastMonitorWaveTicks[m]).count();
                                if (elapsedUs >= targetPeriodUs) {
                                        float dt = static_cast<float>(elapsedUs) * 1e-6f;
                                        lastMonitorWaveTicks[m] = waveNow;

                                        for (auto &wt : tasks) {
                                                std::lock_guard lk_ch(wt.ch->waveMtx_);
                                                wt.ch->wave_.cfg.fs = 1.0f / dt;
                                                wave_exec(&wt.ch->wave_);
                                                f32 outVal = wt.ch->wave_.out.val;
                                                JLinkDev::instance().writeMem(wt.addr, wt.nb, &outVal);
                                        }
                                }
                        }
                }

                // Collect current HSS needs
                std::vector<HssBlock>         blocks;
                std::vector<MonitorChannel *> chans;
                std::vector<Monitor *>        chMonitors;
                std::vector<std::string>      lastTypes_current;
                std::vector<u32>              offsets;
                u32                           curOff = 0;
                for (auto &tc : tempChs) {
                        blocks.push_back({tc.addr, tc.nb});
                        chans.push_back(tc.ch);
                        chMonitors.push_back(tc.monitor);
                        lastTypes_current.push_back(tc.type);
                        offsets.push_back(curOff);
                        curOff += tc.nb;
                }
                int periodUs = 1000000 / maxHssHz;

                bool changed = (blocks != lastBlocks || periodUs != lastPeriodUs || JLinkDev::instance().hasRestartReq());
                bool desiredRunning = !blocks.empty() && !isPaused && JLinkDev::instance().isConnected();

                if (changed && JLinkDev::instance().isHssRunning()) {
                        JLinkDev::instance().hssStop();
                        carryLen = 0;
                }

                if (desiredRunning && !JLinkDev::instance().isHssRunning()) {
                        auto nowTick = std::chrono::steady_clock::now();
                        // Only cooldown if we just stopped it recently. 
                        // If it's a 'cold start' (already stopped), start immediately.
                        bool justStopped = (std::chrono::duration_cast<std::chrono::milliseconds>(nowTick - lastHssStart).count() < 200);
                        
                        if (!justStopped || changed) { 
                                if (JLinkDev::instance().hssStart(blocks, periodUs)) {
                                        lastHssStart = nowTick;
                                        lastHssSessionTime = sessionTimeSec();
                                        lastBlocks   = blocks;
                                        lastChans    = chans;
                                        lastChMonitors = chMonitors;
                                        lastTypes    = lastTypes_current;
                                        lastOffsets  = offsets;
                                        lastPeriodUs = periodUs;
                                        carryLen     = 0;
                                        discardFirst = true;
                                        hssTimeSynced = false;
                                } else {
                                        lastBlocks.clear();
                                        lastChans.clear();
                                        lastChMonitors.clear();
                                        lastTypes.clear();
                                        lastOffsets.clear();
                                }
                        }
                } else if (changed) {
                        lastBlocks   = blocks;
                        lastChans    = chans;
                        lastChMonitors = chMonitors;
                        lastTypes    = lastTypes_current;
                        lastOffsets  = offsets;
                        lastPeriodUs = periodUs;
                        carryLen     = 0;
                        discardFirst = true;
                        hssTimeSynced = false;
                }

                auto processHss = [&]() {
                        if (isPaused || !JLinkDev::instance().isHssRunning() || lastChans.empty())
                                return;

                        int total = JLinkDev::instance().hssRead(buf + carryLen, kBufCap - carryLen);
                        if (total < 0) total = 0;
                        total += static_cast<int>(carryLen);

                        const int frameSize = JLinkDev::instance().hssFrameSize();
                        if (frameSize <= 0) return;

                        int frames = total / frameSize;
                        if (frames > 0) {
                                hzFrameAccum += static_cast<u64>(frames);
                                std::vector<std::vector<f32>> pVals(lastChans.size());
                                std::vector<std::vector<f32>> pTs(lastChans.size());
                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        pVals[i].reserve(frames);
                                        pTs[i].reserve(frames);
                                }

                                if (discardFirst) {
                                        hssTimeSynced = false;
                                        discardFirst = false;
                                }

                                for (usize f = 0; f < frames; ++f) {
                                        const u8 *pFrame = buf + f * frameSize;
                                        u32 hwTs;
                                        std::memcpy(&hwTs, pFrame, 4);

                                        if (!hssTimeSynced) {
                                                hssBaseHwTs = hwTs;
                                                hssBaseWallTime = sessionTimeSec();
                                                hssTimeSynced = true;
                                        }

                                        // Handle 32-bit wrap-around
                                        u32 diff = 0;
                                        if (hwTs >= hssBaseHwTs) {
                                                diff = hwTs - hssBaseHwTs;
                                        } else {
                                                diff = (0xFFFFFFFF - hssBaseHwTs) + hwTs + 1;
                                        }
                                        
                                        const double ts = hssBaseWallTime + (static_cast<double>(diff) * 1e-6);

                                        const u8 *pData = pFrame + JLinkDev::kHssHeaderBytes;
                                        for (usize i = 0; i < lastChans.size(); ++i) {
                                                u8 raw[8] = {0};
                                                u32 varSize = typeBytes(lastTypes[i]);
                                                std::memcpy(raw, pData + lastOffsets[i], varSize);
                                                pVals[i].push_back(decodeAs(raw, lastTypes[i]));
                                                pTs[i].push_back(static_cast<f32>(ts));
                                        }
                                }

                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        lastChans[i]->pushBatch(pVals[i].data(), pTs[i].data(), pVals[i].size());
                                }

                                std::unordered_set<Monitor*> uniqueMonitors;
                                for (auto* m : lastChMonitors) uniqueMonitors.insert(m);
                                for (auto* m : uniqueMonitors) m->addPoints(static_cast<u64>(frames));

                                JLinkDev::instance().addPoints(static_cast<u64>(frames) * static_cast<u64>(lastChans.size()));
                                const usize consumed = frames * static_cast<usize>(frameSize);
                                carryLen             = total - consumed;
                                if (carryLen > 0 && consumed > 0)
                                        std::memmove(buf, buf + consumed, carryLen);
                        }
                };

                // Execute POLL tasks
                if (!isPaused && !pollTasks.empty()) {
                        // Group poll tasks by monitor for individual rate limiting
                        std::unordered_map<Monitor*, std::vector<TempCh>> monitorPollGroups;
                        for (auto &pt : pollTasks) monitorPollGroups[pt.monitor].push_back(pt);

                        for (auto &[m, tasks] : monitorPollGroups) {
                                static std::unordered_map<Monitor*, std::chrono::steady_clock::time_point> lastMonitorPollTicks;
                                auto nowPoll = std::chrono::steady_clock::now();
                                int targetPeriodUs = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 1000) targetPeriodUs = 1000;

                                auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(nowPoll - lastMonitorPollTicks[m]).count();
                                if (elapsedUs >= targetPeriodUs) {
                                        lastMonitorPollTicks[m] = nowPoll;
                                        
                                        bool firstSuccess = true;
                                        int  pollIdx      = 0;
                                        for (auto &pt : tasks) {
                                                if (JLinkDev::instance().isConnected()) {
                                                        if (firstSuccess) {
                                                                hzFrameAccum++;
                                                                m->addPoints(1);
                                                                firstSuccess = false;
                                                        }
                                                        u8 rbuf[8] = {0};
                                                        if (JLinkDev::instance().readMem(pt.addr, pt.nb, rbuf)) {
                                                                pt.ch->setRVal(decodeAs(rbuf, pt.type), sessionTimeSec());
                                                        }
                                                }
                                                if (++pollIdx % 4 == 0) processHss();
                                        }
                                }
                        }
                }

                processHss();

                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                monitor->updateHz();
                                for (auto &scope : monitor->getScopes() | std::views::values)
                                        scope->purgeDeleted();
                                monitor->purgeDeletedScopes();
                        }
                }

                {
                        const auto now  = std::chrono::steady_clock::now();
                        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - hzLastTick).count();
                        if (dtMs >= 500) {
                                const f32 hz = static_cast<f32>(static_cast<double>(hzFrameAccum) * 1000.0 / static_cast<double>(dtMs));
                                JLinkDev::instance().setActualHz(hz);
                                hzFrameAccum = 0;
                                hzLastTick   = now;
                        }
                }

                delay_us(1000); 
        }
}

static int
module_init()
{
        print_info(true, "module init");
        return 0;
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
        module_init();

        Gui gui;

        std::thread t1(threadFunc, &gui);
        gui.loop();
        g_appRunning.store(false);

        if (t1.joinable())
                t1.join();

        JLinkDev::instance().close();

        return 0;
}
