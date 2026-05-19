/**
 * @file  main.cpp
 * @brief Application entry point and logging initialisation.
 */
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "module.h"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/sampler.hpp"
#include "gui/gui.hpp"
#include "timeops.h"

#ifdef _WIN32
#include <dbghelp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <windows.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

u64               cnt;
std::atomic<bool> g_appRunning{true};

// Global log objects
log_t     g_log_ops;
log_t     g_log_perf;
mempool_t g_log_ops_mp;
mempool_t g_log_perf_mp;
static u8 g_log_ops_mp_buf[2 * 1024 * 1024];  // 2MB
static u8 g_log_perf_mp_buf[2 * 1024 * 1024]; // 2MB

thread_local int        g_log_idx = -1;
static std::mutex       g_log_idx_mtx;
static std::vector<int> g_free_log_idx;
static int              g_next_log_idx_val = 0;

struct LogIdxReclaimer {
        ~LogIdxReclaimer()
        {
                if (g_log_idx != -1) {
                        std::lock_guard<std::mutex> lock(g_log_idx_mtx);
                        g_free_log_idx.push_back(g_log_idx);
                        g_log_idx = -1;
                }
        }
};

int
get_log_idx()
{
        if (g_log_idx == -1) {
                std::lock_guard<std::mutex> lock(g_log_idx_mtx);
                if (!g_free_log_idx.empty()) {
                        g_log_idx = g_free_log_idx.back();
                        g_free_log_idx.pop_back();
                } else {
                        g_log_idx = g_next_log_idx_val++;
                }
                thread_local LogIdxReclaimer reclaimer;
        }
        return g_log_idx;
}

static i32
module_init()
{
        g_log_ops_mp.buf = g_log_ops_mp_buf;
        g_log_ops_mp.cap = sizeof(g_log_ops_mp_buf);
        mempool_init(&g_log_ops_mp);

        g_log_perf_mp.buf = g_log_perf_mp_buf;
        g_log_perf_mp.cap = sizeof(g_log_perf_mp_buf);
        mempool_init(&g_log_perf_mp);

#ifdef _WIN32
        static std::string logDirOps  = Gui::getAppDir() + "\\log\\ops";
        static std::string logDirPerf = Gui::getAppDir() + "\\log\\perf";
        CreateDirectoryA((Gui::getAppDir() + "\\log").c_str(), NULL);
        CreateDirectoryA(logDirOps.c_str(), NULL);
        CreateDirectoryA(logDirPerf.c_str(), NULL);
#else
        static std::string logDirOps  = Gui::getAppDir() + "/log/ops";
        static std::string logDirPerf = Gui::getAppDir() + "/log/perf";
#endif
        log_cfg_t cfg_ops = {
            .e_mode     = LOG_MODE_ASYNC,
            .e_level    = LOG_LEVEL_INFO,
            .e_format   = LOG_FORMAT_TEXT,
            .mempool    = &g_log_ops_mp,
            .file_path  = logDirOps.c_str(),
            .fd         = NULL,
            .file_size  = SIZE_16MB,
            .max_files  = 256,
            .e_ring     = LOG_RING_ROTATE,
            .chunk_size = SIZE_4KB,
            .flush_cap  = SIZE_8KB,
            .nproducers = 32,
            .f_get_ts   = get_real_ts_ms,
            .f_flush    = NULL,
        };

        log_init(&g_log_ops, cfg_ops);

        log_cfg_t cfg_perf = cfg_ops;
        cfg_perf.mempool   = &g_log_perf_mp;
        cfg_perf.file_path = logDirPerf.c_str();
        log_init(&g_log_perf, cfg_perf);

        LOG_I("module init");
        LOG_I("app dir: %s", Gui::getAppDir().c_str());
        LOG_I("log dir ops: %s", logDirOps.c_str());
        return 0;
}

static std::atomic<bool> g_flusherRunning{true};

// Background thread that periodically drains the log MPSC queue into the
// mmap'd file and tells the OS to write dirty pages to disk. Without this,
// async-mode log data only reaches the file on log_deinit — so a crash
// loses everything since session start. With this, crash loses at most
// ~200ms of recent log lines.
static void
log_flusher_func()
{
        // Demote flusher to low priority — it must never preempt the sampler.
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
        // SCHED_OTHER with nice 19 (lowest normal priority)
        nice(19);
#endif
        while (g_flusherRunning.load(std::memory_order_acquire)) {
                log_flush(&g_log_ops);
                log_flush(&g_log_perf);
#ifdef _WIN32
                if (g_log_ops.lo.mmap_ptr) {
                        FlushViewOfFile(g_log_ops.lo.mmap_ptr, g_log_ops.lo.file_offset);
                }
                if (g_log_perf.lo.mmap_ptr) {
                        FlushViewOfFile(g_log_perf.lo.mmap_ptr, g_log_perf.lo.file_offset);
                }
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // Final drain after the stop signal so anything written between the
        // last tick and shutdown reaches the file.
        log_flush(&g_log_ops);
        log_flush(&g_log_perf);
}

#ifdef _WIN32
// Captured at startup; used by the crash handler since std::string and any
// non-trivial code is unsafe to invoke after an access violation.
static char g_crashAppDir[MAX_PATH] = {0};

// Top-level SEH handler — fires when no other __try block catches an exception
// (access violation, stack overflow, illegal instruction, etc). MUST stay
// crash-safe: no heap allocation, no C++ exceptions, no STL — just stack
// buffers + Win32 APIs.
static LONG WINAPI
crashHandler(EXCEPTION_POINTERS *ep)
{
        // Build dump path: <appdir>\crashdumps\YYYYMMDD_HHMMSS.dmp
        SYSTEMTIME st;
        GetLocalTime(&st);
        char dumpDir[MAX_PATH];
        snprintf(dumpDir, sizeof(dumpDir), "%s\\crashdumps", g_crashAppDir);
        CreateDirectoryA(dumpDir, NULL);

        char dumpPath[MAX_PATH];
        snprintf(dumpPath,
                 sizeof(dumpPath),
                 "%s\\%04d%02d%02d_%02d%02d%02d_pid%lu.dmp",
                 dumpDir,
                 st.wYear,
                 st.wMonth,
                 st.wDay,
                 st.wHour,
                 st.wMinute,
                 st.wSecond,
                 GetCurrentProcessId());

        HANDLE hf = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
                // Normal + indirectly-referenced memory = small (usually <1MB)
                // but pulls in anything reachable from registers/stack, which
                // is enough to see the crashing object and its referents in
                // WinDbg. Avoid MiniDumpWithFullMemory — that's GB.
                const MINIDUMP_TYPE type =
                    (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf, type, &mei, NULL, NULL);
                CloseHandle(hf);
        }

        // Best-effort: shove the crash record into the ops log and force a
        // sync flush so a debugger can correlate the .dmp with the last log
        // line. LOG_E goes through the async queue → log_flush drains it →
        // FlushViewOfFile pushes mmap'd pages to disk.
        LOG_E("CRASH code=0x%08lx addr=%p dump=%s",
              ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
              ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL,
              dumpPath);
        log_flush(&g_log_ops);
        log_flush(&g_log_perf);
        if (g_log_ops.lo.mmap_ptr)
                FlushViewOfFile(g_log_ops.lo.mmap_ptr, g_log_ops.lo.file_offset);
        if (g_log_perf.lo.mmap_ptr)
                FlushViewOfFile(g_log_perf.lo.mmap_ptr, g_log_perf.lo.file_offset);

        // Let Windows show the standard "app stopped working" dialog and
        // terminate. Returning EXCEPTION_CONTINUE_SEARCH would chain to any
        // debugger; EXECUTE_HANDLER just kills us cleanly.
        return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int
main(int argc, char **argv)
{
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        PathRemoveFileSpecA(exePath);
        SetCurrentDirectoryA(exePath);

        // Cache app dir for the crash handler (it can't call any C++ code).
        snprintf(g_crashAppDir, sizeof(g_crashAppDir), "%s", exePath);
        SetUnhandledExceptionFilter(crashHandler);

        timeBeginPeriod(1);
#endif
        module_init();

        std::thread flusher(log_flusher_func);

        std::string initialSession = (argc > 1) ? argv[1] : "";
        auto        gui            = std::make_unique<Gui>(initialSession);

        std::thread t1(threadFunc, gui.get());

        // ---- Core isolation: evict GUI + flusher from sampler core ----
        // Wait for the sampler thread to bind (typically < 1ms).
        while (g_samplerBoundCore.load() < 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const int samplerCore = g_samplerBoundCore.load();
#ifdef _WIN32
        {
                DWORD_PTR procMask = 0, sysMask = 0;
                GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
                DWORD_PTR excludeMask = procMask & ~(1ull << samplerCore);
                if (excludeMask) {
                        // Main thread (will become GUI thread in gui->loop())
                        SetThreadAffinityMask(GetCurrentThread(), excludeMask);
                        // Log flusher thread
                        SetThreadAffinityMask(flusher.native_handle(), excludeMask);
                        LOG_I("Core isolation: GUI+flusher excluded from core %d (mask=0x%llx)", samplerCore, (u64)excludeMask);
                }
        }
#elif defined(__linux__)
        {
                const int n = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                if (n >= 2) {
                        cpu_set_t cs;
                        CPU_ZERO(&cs);
                        for (int i = 0; i < n; ++i) {
                                if (i != samplerCore)
                                        CPU_SET(i, &cs);
                        }
                        // Main thread (GUI)
                        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
                        // Log flusher thread
                        pthread_setaffinity_np(flusher.native_handle(), sizeof(cs), &cs);
                        LOG_I("Core isolation: GUI+flusher excluded from core %d", samplerCore);
                }
        }
#endif

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

        LOG_I("Stopping log flusher...");
        g_flusherRunning.store(false, std::memory_order_release);
        if (flusher.joinable())
                flusher.join();

        log_deinit(&g_log_ops);
        log_deinit(&g_log_perf);
        return 0;
}

#ifdef _WIN32
int WINAPI
WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/)
{
        return main(__argc, __argv);
}
#endif
