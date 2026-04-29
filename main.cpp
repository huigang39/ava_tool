#include <cstring>
#include <memory>
#include <ranges>
#include <thread>
#include <vector>

#include "module.h"

#include "gui.hpp"
#include "jlink_dev.hpp"

fft_t fft;

u64 cnt;

static f32
decodeAs(const u8 *raw, const std::string &type)
{
        if (type == "F32") {
                f32 f;
                std::memcpy(&f, raw, sizeof(f));
                return f;
        }
        if (type == "I32") {
                i32 v;
                std::memcpy(&v, raw, sizeof(v));
                return static_cast<f32>(v);
        }
        u32 v;
        std::memcpy(&v, raw, sizeof(v));
        return static_cast<f32>(v);
}

void
threadFunc(Gui::MonitorMapType *monitors)
{
        std::vector<HssBlock>         lastBlocks;
        std::vector<MonitorChannel *> lastChans;
        std::vector<std::string>      lastTypes;
        int                           lastPeriodUs = 0;

        static constexpr usize kBufCap = 64 * 1024;
        static u8              readBuf[kBufCap];
        usize                  carryLen = 0;

        while (true) {
                if (g_monitorPaused.load()) {
                        if (JLinkDev::instance().isHssRunning())
                                JLinkDev::instance().hssStop();
                        lastBlocks.clear();
                        lastChans.clear();
                        lastTypes.clear();
                        carryLen = 0;
                        delay_us(20000);
                        continue;
                }

                std::vector<HssBlock>        blocks;
                std::vector<MonitorChannel *> chans;
                std::vector<std::string>     types;

                for (const auto &monitor : *monitors | std::views::values) {
                        for (auto &scope : monitor->getScopes() | std::views::values) {
                                for (auto &ch : scope->getChannels() | std::views::values) {
                                        const std::string &dev = ch->getDevice();
                                        if (dev == "LOCAL") {
                                                u64 val = 0;
                                                shm_read(&ch->getShm(), &val, sizeof(val));
                                                u8 raw[8];
                                                std::memcpy(raw, &val, sizeof(raw));
                                                ch->setRVal(decodeAs(raw, ch->getType()));
                                        } else if (dev == "JLINK" && ch->getAddr() != 0) {
                                                blocks.push_back({static_cast<u32>(ch->getAddr()), 4});
                                                chans.push_back(ch.get());
                                                types.push_back(ch->getType());
                                        }
                                }
                        }
                }

                const int periodUs = JLinkDev::instance().hssPeriodUs();
                bool      changed  = (blocks.size() != lastBlocks.size()) || (periodUs != lastPeriodUs);
                if (!changed) {
                        for (usize i = 0; i < blocks.size(); ++i) {
                                if (blocks[i].addr != lastBlocks[i].addr || lastChans[i] != chans[i]) {
                                        changed = true;
                                        break;
                                }
                        }
                }

                const bool desiredRunning = JLinkDev::instance().isConnected() && !blocks.empty();

                if (JLinkDev::instance().isHssRunning() && (changed || !desiredRunning))
                        JLinkDev::instance().hssStop();

                if (desiredRunning && !JLinkDev::instance().isHssRunning()) {
                        if (JLinkDev::instance().hssStart(blocks, periodUs)) {
                                lastBlocks   = blocks;
                                lastChans    = chans;
                                lastTypes    = types;
                                lastPeriodUs = periodUs;
                        } else {
                                lastBlocks.clear();
                                lastChans.clear();
                                lastTypes.clear();
                        }
                } else if (changed) {
                        lastBlocks   = blocks;
                        lastChans    = chans;
                        lastTypes    = types;
                        lastPeriodUs = periodUs;
                }

                if (JLinkDev::instance().isHssRunning() && !lastChans.empty()) {
                        const int frameSize = JLinkDev::instance().hssFrameSize();
                        if (frameSize > 0) {
                                static u8 buf[64 * 1024];
                                const int n = JLinkDev::instance().hssRead(buf, sizeof(buf));
                                if (n >= frameSize) {
                                        const int frames = n / frameSize;
                                        for (int f = 0; f < frames; ++f) {
                                                const u8 *p = buf + f * frameSize;
                                                for (usize i = 0; i < lastChans.size(); ++i) {
                                                        u8 raw[4];
                                                        std::memcpy(raw, p + i * 4, 4);
                                                        lastChans[i]->setRVal(decodeAs(raw, lastTypes[i]));
                                                }
                                        }
                                }
                        }
                }

                delay_us(2000);
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
        module_init();

        Gui gui;

        std::thread t(threadFunc, &gui.getMonitors());
        t.detach();

        gui.loop();

        return 0;
}
