/**
 * @file  main.cpp
 * @brief Application entry point and logging initialisation.
 */
#include <memory>
#include <thread>

#include "module.h"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/sampler.hpp"
#include "gui/gui.hpp"
#include "timeops.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <windows.h>
#pragma comment(lib, "shlwapi.lib")
#endif

u64               cnt;
std::atomic<bool> g_appRunning{true};

// Global log objects
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

static i32
module_init()
{
        g_log_mp.buf = g_log_mp_buf;
        g_log_mp.cap = sizeof(g_log_mp_buf);
        mempool_init(&g_log_mp);

#ifdef _WIN32
        static std::string logDir = Gui::getAppDir() + "\\log";
#else
        static std::string logDir = Gui::getAppDir() + "/log";
#endif
        log_cfg_t cfg = {
            .e_mode     = LOG_MODE_ASYNC,
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
            .f_flush    = NULL,
        };

        log_init(&g_log, cfg);

        LOG_I("module init");
        LOG_I("app dir: %s", Gui::getAppDir().c_str());
        LOG_I("log dir: %s", logDir.c_str());
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
        gui->loop();

        LOG_I("Stopping sampler thread...");
        g_appRunning.store(false);
        if (t1.joinable())
                t1.join();
        LOG_I("Sampler thread stopped.");

        JLinkPort::instance().close();

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
