/**
 * @file  updater_main.cpp
 * @brief Standalone updater.exe. Launched by the main app when the user accepts
 *        an update. It waits for the app to exit, downloads the new installer
 *        from GitHub (showing a progress window), and runs it — the installer
 *        replaces the files and relaunches the app.
 *
 * Usage (set up by Updater::launchUpdater):
 *   updater.exe <downloadUrl> <parentPid> "<relaunchExePath>"
 */
#ifdef _WIN32

#include <windows.h>
// <shellapi.h>/<commctrl.h> must be included after <windows.h> (keep the
// blank-comment line so clang-format does not reorder them ahead of windows.h).
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "core/http_win.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")

namespace
{

// --- Shared state between the download worker and the UI message loop ---
std::atomic<uint64_t> g_received{0};
std::atomic<uint64_t> g_total{0};
std::atomic<bool>     g_done{false};
std::atomic<bool>     g_ok{false};
std::string           g_err;
HWND                  g_hProgress = nullptr;
HWND                  g_hStatus   = nullptr;

std::string
wideToUtf8(const wchar_t *w)
{
        if (!w)
                return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0)
                return {};
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
}

void
waitForParent(DWORD pid)
{
        if (pid == 0)
                return;
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h)
                return;                // already gone
        WaitForSingleObject(h, 30000); // up to 30s for a graceful exit
        CloseHandle(h);
}

LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
        switch (msg) {
                case WM_TIMER: {
                        const uint64_t r = g_received.load();
                        const uint64_t t = g_total.load();
                        wchar_t        buf[160];
                        if (t > 0) {
                                const int pct = (int)((r * 100) / t);
                                SendMessageW(g_hProgress, PBM_SETPOS, (WPARAM)pct, 0);
                                swprintf(buf,
                                         160,
                                         L"Downloading update...  %.1f / %.1f MB  (%d%%)",
                                         r / 1048576.0,
                                         t / 1048576.0,
                                         pct);
                        } else {
                                swprintf(buf, 160, L"Downloading update...  %.1f MB", r / 1048576.0);
                        }
                        SetWindowTextW(g_hStatus, buf);
                        if (g_done.load()) {
                                KillTimer(hwnd, 1);
                                PostQuitMessage(0);
                        }
                        return 0;
                }
                case WM_DESTROY:
                        PostQuitMessage(0);
                        return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create a small fixed progress window centred on the primary monitor.
HWND
createProgressWindow(HINSTANCE hInst)
{
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&icc);

        WNDCLASSW wc{};
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AvaToolUpdaterWnd";
        RegisterClassW(&wc);

        const int w = 440, h = 130;
        const int x    = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y    = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        HWND      hwnd = CreateWindowExW(WS_EX_TOPMOST,
                                    L"AvaToolUpdaterWnd",
                                    L"ava_tool Updater",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                    x,
                                    y,
                                    w,
                                    h,
                                    nullptr,
                                    nullptr,
                                    hInst,
                                    nullptr);
        if (!hwnd)
                return nullptr;

        g_hStatus = CreateWindowExW(
            0, L"STATIC", L"Preparing update...", WS_CHILD | WS_VISIBLE, 20, 18, 400, 20, hwnd, nullptr, hInst, nullptr);
        g_hProgress =
            CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 20, 50, 400, 24, hwnd, nullptr, hInst, nullptr);
        SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 100);

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        return hwnd;
}

} // namespace

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
        int     argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv || argc < 2) {
                MessageBoxW(nullptr, L"updater: missing download URL argument.", L"ava_tool Updater", MB_ICONERROR);
                return 1;
        }
        const std::string url      = wideToUtf8(argv[1]);
        const DWORD       parentId = (argc >= 3) ? (DWORD)_wtoi(argv[2]) : 0;
        LocalFree(argv);

        // 1. Wait for the running app to close so its files can be replaced.
        waitForParent(parentId);

        // 2. Show the progress window and download the installer to %TEMP%.
        char tmpDir[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmpDir);
        const std::string setupPath = std::string(tmpDir) + "ava_tool_setup.exe";

        HWND hwnd = createProgressWindow(hInst);

        std::thread worker([&]() {
                std::string err;
                bool        ok = http::download(
                    url,
                    setupPath,
                    [](uint64_t received, uint64_t total) {
                            g_received.store(received);
                            g_total.store(total);
                    },
                    &err);
                g_ok.store(ok);
                if (!ok)
                        g_err = err;
                g_done.store(true);
        });

        if (hwnd) {
                SetTimer(hwnd, 1, 100, nullptr);
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                }
                DestroyWindow(hwnd);
        }
        worker.join();

        if (!g_ok.load()) {
                std::string msg = "Failed to download the update:\n" + g_err + "\n\nURL: " + url;
                MessageBoxA(nullptr, msg.c_str(), "ava_tool Updater", MB_ICONERROR);
                return 2;
        }

        // 3. Run the installer silently; it closes/relaunches the app itself.
        SHELLEXECUTEINFOA sei{};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb       = "open";
        sei.lpFile       = setupPath.c_str();
        sei.lpParameters = "/SILENT /SUPPRESSMSGBOXES /NOCANCEL /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS";
        sei.nShow        = SW_SHOWNORMAL;
        if (!ShellExecuteExA(&sei)) {
                std::string msg = "Failed to launch the installer:\n" + setupPath;
                MessageBoxA(nullptr, msg.c_str(), "ava_tool Updater", MB_ICONERROR);
                return 3;
        }
        if (sei.hProcess)
                CloseHandle(sei.hProcess);
        return 0;
}

#else // !_WIN32

int
main()
{
        return 0; // updater is Windows-only
}

#endif // _WIN32
