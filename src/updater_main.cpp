/**
 * @file  updater_main.cpp
 * @brief Standalone updater.exe. Launched by the main app when the user accepts
 *        an update. It waits for the app to exit, downloads the new installer
 *        from GitHub, and runs it silently — the installer replaces the files and
 *        relaunches the app.
 *
 * Usage (set up by Updater::launchUpdater):
 *   updater.exe <downloadUrl> <parentPid> "<relaunchExePath>"
 */
#ifdef _WIN32

#include <windows.h>
// <shellapi.h> must be included after <windows.h> (keep the blank-comment line
// so clang-format does not reorder it ahead of windows.h).
#include <shellapi.h>

#include <string>

#include "core/http_win.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace
{

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

} // namespace

int WINAPI
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
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

        // 2. Download the installer to %TEMP%.
        char tmpDir[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmpDir);
        const std::string setupPath = std::string(tmpDir) + "ava_tool_setup.exe";

        std::string err;
        if (!http::download(url, setupPath, {}, &err)) {
                std::string msg = "Failed to download the update:\n" + err + "\n\nURL: " + url;
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
