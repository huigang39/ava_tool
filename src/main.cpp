#include <cstring>
#include <memory>
#include <ranges>
#include <thread>
#include <unordered_set>
#include <vector>

#include "module.h"

#include "app_log.hpp"
#include "core/jlink_dev.hpp"
#include "gui/gui.hpp"
#include "timeops.h"
#include "wave.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <windows.h>
#pragma comment(lib, "shlwapi.lib")
#endif

u64                      cnt;
std::atomic<bool>        g_appRunning{true};
extern std::atomic<bool> g_monitorPaused;

// 全局日志对象
log_t     g_log;
mempool_t g_log_mp;
static u8 g_log_mp_buf[2 * 1024 * 1024]; // 2MB

thread_local int        g_log_idx = -1;
static std::atomic<int> g_next_log_idx{0};

int
get_log_idx()
{
        if (g_log_idx == -1)
                g_log_idx = g_next_log_idx.fetch_add(1) % 8;
        return g_log_idx;
}

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
                f64 d;
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
                const f64 d = static_cast<f64>(val);
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

// Dedicated wave-generation thread.
// Completely independent of the sampler thread — only competes for JLinkDev::mtx_
// when it actually needs to write, which is the unavoidable serialization point.
void
waveFunc(Gui *gui)
{
        LOG_I("WaveGen thread started.");

        struct WaveTask {
                std::shared_ptr<MonitorChannel> ch;
                std::shared_ptr<Monitor>        monitor;
                u32                             addr;
                u32                             nb;
        };

        std::unordered_map<std::shared_ptr<Monitor>, u64> lastMonitorWaveTicks;

        while (g_appRunning.load()) {
                std::unordered_map<std::shared_ptr<Monitor>, std::vector<WaveTask>> groups;

                {
                        std::lock_guard lk(gui->getMonitorMtx());
                        for (const auto &monitor : gui->getMonitors() | std::views::values) {
                                for (auto &scope : monitor->getScopes() | std::views::values) {
                                        if (scope->isPendingDelete() || scope->isPaused())
                                                continue;
                                        for (auto &ch : scope->getChannels() | std::views::values) {
                                                if (ch->isPendingDelete())
                                                        continue;
                                                if (ch->getDevice() != "JLINK" || !ch->waveEnable_ || ch->getAddr() == 0)
                                                        continue;
                                                u32 nb = ch->getNumBytes();
                                                if (nb == 0)
                                                        nb = typeBytes(ch->getType());
                                                groups[monitor].push_back({ch, monitor, static_cast<u32>(ch->getAddr()), nb});
                                        }
                                }
                        }
                } // mtxMonitors_ released — wave exec + JLink I/O happen outside

                if (groups.empty() || !JLinkDev::instance().isConnected()) {
                        delay_us(1000);
                        continue;
                }

                for (auto &[m, tasks] : groups) {
                        const u64 waveNow        = get_mono_ts_us();
                        const i32 targetPeriodUs = 1000000 / m->maxSampleHz_;

                        auto tickIt = lastMonitorWaveTicks.find(m);
                        if (tickIt == lastMonitorWaveTicks.end()) {
                                lastMonitorWaveTicks[m] = waveNow;
                                continue;
                        }

                        const u64 elapsedUs = waveNow - tickIt->second;
                        if (elapsedUs < static_cast<u64>(targetPeriodUs))
                                continue;

                        const f32 dt   = static_cast<f32>(elapsedUs) * 1e-6f;
                        tickIt->second = waveNow;

                        for (auto &wt : tasks) {
                                std::lock_guard lk_ch(wt.ch->waveMtx_);
                                wt.ch->wave_.cfg.fs = 1.0f / dt;
                                wave_exec(&wt.ch->wave_);
                                const f32 outVal = wt.ch->wave_.out.val;
                                JLinkDev::instance().writeMem(wt.addr, wt.nb, &outVal);
                        }
                }

                delay_us(100); // up to 10 kHz wakeup; actual rate is capped by targetPeriodUs
        }

        LOG_I("WaveGen thread stopped.");
}

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

        while (g_appRunning.load()) {
                bool isPaused = g_monitorPaused.load();
                if (!isPaused && lastPaused) {
                        LOG_I("Resuming from pause, restarting HSS");
                        // Resume from pause: Flush J-Link buffers to avoid stale data distortion
                        if (JLinkDev::instance().isHssRunning()) {
                                JLinkDev::instance().hssStop();
                        }
                        carryLen     = 0;
                        discardFirst = true;
                        nextHssTs    = -1.0; // Reset timer
                        // Forcing a "changed" state will trigger hssStart below
                        lastBlocks.clear();
                }
                lastPaused = isPaused;

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

                std::vector<TempCh>   tempChs;
                std::vector<TempCh>   pollTasks;
                std::vector<WValTask> wvalTasks;
                std::vector<ShmTask>  shmTasks;

                i32 maxHssHz = 1;

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
                                                }
                                        }
                                }
                                if (hasHss && monitor->maxSampleHz_ > maxHssHz) {
                                        maxHssHz = monitor->maxSampleHz_;
                                }
                        }
                }

                // Drain SHM buffers outside the monitor lock (no blocking of UI thread)
                for (auto &st : shmTasks) {
                        u8 raw[8];
                        while (shm_read(&st.ch->getShm(), raw, st.nb) == st.nb) {
                                st.ch->setRVal(decodeAs(raw, st.type), sessionTimeSec());
                                st.monitor->addPoints(1);
                                hzFrameAccum++;
                        }
                }

                // Write wVal dirty values outside the monitor lock (JLinkDev::mtx_ is the real serializer)
                if (JLinkDev::instance().isConnected()) {
                        for (auto &wt : wvalTasks) {
                                u8 wbuf[8] = {0};
                                encodeFromF32(wt.wv, wt.type, wbuf);
                                JLinkDev::instance().writeMem(wt.addr, wt.nb, wbuf);
                        }
                }

                static u64 loopCount = 0;
                if (loopCount++ % 1000 == 0 && JLinkDev::instance().isConnected()) {
                        LOG_I("Sampler Loop: isConnected=%d, isPaused=%d, tempChs=%zu, pollTasks=%zu, maxHssHz=%d",
                              (int)JLinkDev::instance().isConnected(),
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
                bool        isConnected   = JLinkDev::instance().isConnected();
                bool        changed       = (blocks != lastBlocks || periodUs != lastPeriodUs || isConnected != lastConnected ||
                                             JLinkDev::instance().hasRestartReq());
                lastConnected             = isConnected;

                bool desiredRunning = !blocks.empty() && !isPaused && isConnected;
                if (desiredRunning && !JLinkDev::instance().isHssRunning()) {
                        changed = true;
                }

                if (changed && JLinkDev::instance().isHssRunning()) {
                        JLinkDev::instance().hssStop();
                        carryLen = 0;
                }

                if (desiredRunning && !JLinkDev::instance().isHssRunning()) {
                        auto nowTick = get_mono_ts_ms();
                        // Only cooldown if we just stopped it recently.
                        // If it's a 'cold start' (already stopped), start immediately.
                        bool justStopped = ((nowTick - lastHssStart) < 200);

                        if (!justStopped || changed) {
                                LOG_I("Attempting hssStart with %zu blocks, period %d us", blocks.size(), periodUs);
                                if (JLinkDev::instance().hssStart(blocks, periodUs)) {
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
                        if (isPaused || !JLinkDev::instance().isHssRunning() || lastChans.empty())
                                return;

                        i32 total = JLinkDev::instance().hssRead(buf + carryLen, static_cast<u32>(kBufCap - carryLen));
                        if (total < 0)
                                total = 0;
                        total += static_cast<i32>(carryLen);

                        const i32 frameSize = JLinkDev::instance().hssFrameSize();
                        if (frameSize <= 0)
                                return;

                        i32 frames = total / frameSize;
                        if (frames > 0) {
                                hzFrameAccum += static_cast<u64>(frames);
                                std::vector<std::vector<f32>> pVals(lastChans.size());
                                std::vector<std::vector<f64>> pTs(lastChans.size());
                                for (usize i = 0; i < lastChans.size(); ++i) {
                                        pVals[i].reserve(frames);
                                        pTs[i].reserve(frames);
                                }

                                if (discardFirst) {
                                        hssTimeSynced = false;
                                        discardFirst  = false;
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

                                        // Handle 32-bit wrap-around
                                        u32 diff = 0;
                                        if (hwTs >= hssBaseHwTs) {
                                                diff = hwTs - hssBaseHwTs;
                                        } else {
                                                diff = (0xFFFFFFFF - hssBaseHwTs) + hwTs + 1;
                                        }

                                        const f64 ts = hssBaseWallTime + (static_cast<f64>(diff) * 1e-6);

                                        const u8 *pData = pFrame + JLinkDev::kHssHeaderBytes;
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
                        std::unordered_map<std::shared_ptr<Monitor>, std::vector<TempCh>> monitorPollGroups;
                        for (auto &pt : pollTasks)
                                monitorPollGroups[pt.monitor].push_back(pt);

                        for (auto &[m, tasks] : monitorPollGroups) {
                                static std::unordered_map<std::shared_ptr<Monitor>, u64> lastMonitorPollTicks;
                                auto                                                     nowPoll = get_mono_ts_us();
                                i32 targetPeriodUs                                               = 1000000 / m->maxSampleHz_;
                                if (targetPeriodUs < 1000)
                                        targetPeriodUs = 1000;

                                if (lastMonitorPollTicks.find(m) == lastMonitorPollTicks.end()) {
                                        lastMonitorPollTicks[m] = nowPoll;
                                        continue;
                                }
                                auto elapsedUs = nowPoll - lastMonitorPollTicks[m];
                                if (elapsedUs >= (u64)targetPeriodUs) {
                                        lastMonitorPollTicks[m] = nowPoll;

                                        bool firstSuccess = true;
                                        i32  pollIdx      = 0;
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
                                                if (++pollIdx % 4 == 0)
                                                        processHss();
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
                        const auto now  = get_mono_ts_ms();
                        const auto dtMs = now - hzLastTick;
                        if (dtMs >= 500) {
                                const f32 hz =
                                    static_cast<f32>(static_cast<f64>(hzFrameAccum) * 1000.0 / static_cast<f64>(dtMs));
                                JLinkDev::instance().setActualHz(hz);
                                hzFrameAccum = 0;
                                hzLastTick   = now;
                        }
                }

                delay_us(1000);
        }
}

static i32
module_init()
{
        // 初始化内存池
        g_log_mp.buf = g_log_mp_buf;
        g_log_mp.cap = sizeof(g_log_mp_buf);
        mempool_init(&g_log_mp);

        // 配置日志
#ifdef _WIN32
        static std::string logDir = Gui::getAppDir() + "\\log";
#else
        static std::string logDir = Gui::getAppDir() + "/log";
#endif
        log_cfg_t cfg = {.e_mode     = LOG_MODE_ASYNC,
                         .e_level    = LOG_LEVEL_INFO,
                         .e_format   = LOG_FORMAT_TEXT,
                         .mempool    = &g_log_mp,
                         .file_path  = logDir.c_str(),
                         .fd         = NULL,
                         .file_size  = SIZE_16MB,
                         .max_files  = 10,
                         .e_ring     = LOG_RING_ROTATE,
                         .chunk_size = SIZE_4KB,
                         .flush_cap  = SIZE_8KB,
                         .nproducers = 8,
                         .f_get_ts   = get_real_ts_ms,
                         .f_flush    = NULL};

        log_init(&g_log, cfg);

        LOG_I("module init");
        return 0;
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        PathRemoveFileSpecA(exePath);
        SetCurrentDirectoryA(exePath);

        timeBeginPeriod(1);
#endif
        module_init();

        std::string initialSession = (argc > 1) ? argv[1] : "";
        auto        gui            = std::make_unique<Gui>(initialSession);

        std::thread t1(threadFunc, gui.get());
        std::thread t2(waveFunc, gui.get());
        gui->loop();

        LOG_I("Stopping sampler and wave-gen threads...");
        g_appRunning.store(false);
        if (t1.joinable())
                t1.join();
        if (t2.joinable())
                t2.join();
        LOG_I("Sampler and wave-gen threads stopped.");

        JLinkDev::instance().close();

        // Aggressive hide before anything else
        if (gui) {
                LOG_I("Hiding window from main...");
                gui->hide();
        }

        LOG_I("Explicitly destroying Gui...");
        gui.reset();
        LOG_I("Gui destroyed.");

        log_deinit(&g_log);
        return 0;
}

#ifdef _WIN32
int WINAPI
WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/)
{
        return main(__argc, __argv);
}
#endif
