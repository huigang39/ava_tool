#include "core/updater.hpp"

#include "app_log.hpp"
#include "core/http_win.hpp"
#include "core/remote_files.hpp"
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int
Updater::compareVersions(const std::string &a, const std::string &b)
{
        // Parse up to three dotted numeric components, ignoring a leading 'v' and
        // anything after a '-' (pre-release) or '+' (build metadata)
        auto parse = [](const std::string &s, int out[3]) {
                out[0] = out[1] = out[2] = 0;
                size_t i                 = 0;
                if (i < s.size() && (s[i] == 'v' || s[i] == 'V'))
                        ++i;
                int  idx = 0;
                int  cur = 0;
                bool any = false;
                for (; i < s.size() && idx < 3; ++i) {
                        char c = s[i];
                        if (c >= '0' && c <= '9') {
                                cur = cur * 10 + (c - '0');
                                any = true;
                        } else if (c == '.') {
                                out[idx++] = cur;
                                cur        = 0;
                                any        = false;
                        } else {
                                break; // stop at '-', '+', or any other separator
                        }
                }
                if (idx < 3)
                        out[idx] = cur;
                (void)any;
        };

        int va[3], vb[3];
        parse(a, va);
        parse(b, vb);
        for (int i = 0; i < 3; ++i) {
                if (va[i] != vb[i])
                        return va[i] < vb[i] ? -1 : 1;
        }
        return 0;
}

void
Updater::checkAsync()
{
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true))
                return; // already running

        const auto config = remote::settings();
        if (checkThread_.joinable())
                checkThread_.join();
        checkThread_ = std::thread([this, config]() {
                Info result;
                result.currentVersion = AVA_VERSION;

                const auto &cfg     = config;
                const auto &toolUrl = cfg.toolUrl;
                result.sourceUrl    = toolUrl;

                if (toolUrl.empty()) {
                        result.checked = true;
                        result.error   = "Configure the update address in Settings > Server > Remote servers.";
                        LOG_W("Update check: %s", result.error.c_str());
                } else {
                        try {
                                static const std::regex versionPattern(R"(ava_tool_setup_(\d+\.\d+\.\d+)\.exe)",
                                                                       std::regex::icase);
                                const auto              files = remote::list(toolUrl, cfg, {".exe"});

                                for (const auto &file : files) {
                                        std::smatch m;
                                        if (!std::regex_match(file.name, m, versionPattern))
                                                continue;
                                        result.installers.push_back({m[1].str(), file.url});
                                }
                                std::sort(result.installers.begin(),
                                          result.installers.end(),
                                          [](const Installer &a, const Installer &b) {
                                                  const int comparison = compareVersions(a.version, b.version);
                                                  return comparison != 0 ? comparison > 0 : a.url < b.url;
                                          });
                                result.installers.erase(
                                    std::unique(result.installers.begin(),
                                                result.installers.end(),
                                                [](const Installer &a, const Installer &b) { return a.version == b.version; }),
                                    result.installers.end());

                                result.checked = true;
                                if (result.installers.empty()) {
                                        result.error = "No installer found on the update server.";
                                        LOG_W("Update check: %s", result.error.c_str());
                                } else {
                                        const auto &bestVersion = result.installers.front().version;
                                        result.latestVersion    = bestVersion;
                                        result.assetUrl         = result.installers.front().url;
                                        result.releaseUrl       = toolUrl;
                                        if (compareVersions(bestVersion, result.currentVersion) > 0) {
                                                result.available = true;
                                                LOG_I("Update available: %s -> %s",
                                                      result.currentVersion.c_str(),
                                                      result.latestVersion.c_str());
                                        } else {
                                                LOG_I("Up to date (current %s, latest %s)",
                                                      result.currentVersion.c_str(),
                                                      result.latestVersion.c_str());
                                        }
                                }
                        } catch (const std::exception &e) {
                                result.checked = true;
                                result.error   = std::string("Update check failed: ") + e.what();
                                LOG_W("Update check: %s", result.error.c_str());
                        }
                }

                {
                        std::lock_guard lk(mtx_);
                        info_ = std::move(result);
                }
                running_.store(false, std::memory_order_release);
        });
}

Updater::Info
Updater::get() const
{
        std::lock_guard lk(mtx_);
        return info_;
}

bool
Updater::launchUpdater(const std::string &assetUrl)
{
#ifdef _WIN32
        if (assetUrl.empty())
                return false;

        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir = exePath;
        auto        sep = dir.find_last_of("\\/");
        if (sep != std::string::npos)
                dir.resize(sep + 1);

        const std::string updaterPath = dir + "updater.exe";

        // updater.exe <downloadUrl> <parentPid> "<relaunchExePath>"
        std::string cmd =
            "\"" + updaterPath + "\" \"" + assetUrl + "\" " + std::to_string(GetCurrentProcessId()) + " \"" + exePath + "\"";

        STARTUPINFOA        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        std::string mutableCmd = cmd; // CreateProcess may modify the buffer
        BOOL        ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
        if (!ok) {
                LOG_E("Failed to launch updater.exe (err=%lu)", GetLastError());
                return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOG_I("Launched updater.exe for %s", assetUrl.c_str());
        return true;
#else
        (void)assetUrl;
        return false;
#endif
}

void
Updater::downloadAsync(const std::string &assetUrl)
{
        // Don't start if already downloading
        DownloadState expected = DownloadState::Idle;
        if (!dlState_.compare_exchange_strong(expected, DownloadState::Downloading)) {
                // Also allow re-download after a failure
                expected = DownloadState::Failed;
                if (!dlState_.compare_exchange_strong(expected, DownloadState::Downloading))
                        return;
        }

        dlProgress_.store(0, std::memory_order_release);
        {
                std::lock_guard lk(dlMtx_);
                dlPath_.clear();
                dlError_.clear();
        }

        std::string url = assetUrl; // copy for the thread

        const auto config = remote::settings();
        if (downloadThread_.joinable())
                downloadThread_.join();
        downloadThread_ = std::thread([this, url, config]() {
#ifdef _WIN32
                char tmpDir[MAX_PATH] = {0};
                GetTempPathA(MAX_PATH, tmpDir);
                const std::string setupPath = std::string(tmpDir) + "ava_tool_setup.exe";
#else
                const std::string setupPath = "/tmp/ava_tool_setup";
#endif

                LOG_I("Background download started: %s -> %s", url.c_str(), setupPath.c_str());

                auto onProgress = [this](uint64_t received, uint64_t total) {
                        if (total > 0) {
                                int pct = static_cast<int>((received * 100) / total);
                                if (pct > 100)
                                        pct = 100;
                                dlProgress_.store(pct, std::memory_order_release);
                        }
                };

                std::string              err;
                std::vector<std::string> headers{"Accept: application/octet-stream"};
                // Public folder servers need no login endpoint. Authenticate only
                // when the download actually requires credentials
                bool ok            = http::download(url, setupPath, onProgress, &err, headers);
                auto installerFile = [&] {
                        std::ifstream installer(setupPath, std::ios::binary);
                        char          magic[2]{};
                        return static_cast<bool>(installer.read(magic, 2)) && magic[0] == 'M' && magic[1] == 'Z';
                };
                // Some authenticated servers redirect to a login page with HTTP
                // 200 rather than returning 401. Preserve their cookie login too
                const bool needsLogin = (ok && !installerFile()) ||
                                        (!ok && (err.find("401") != std::string::npos || err.find("403") != std::string::npos));
                if (needsLogin && !config.username.empty()) {
                        // The internal file server requires a login — authenticate with the
                        // configured credentials and retry once with the session cookie
                        std::string loginErr;
                        const auto  cookie = remote::login(url, config, &loginErr);
                        if (!cookie.empty())
                                ok = http::download(url, setupPath, onProgress, &err, {"Cookie: " + cookie});
                        else if (!loginErr.empty())
                                err = loginErr;
                }

                if (ok) {
                        ok = installerFile();
                        if (!ok)
                                err = "The server did not return a Windows installer.";
                }
                {
                        std::lock_guard lk(dlMtx_);
                        if (ok) {
                                dlPath_ = setupPath;
                                LOG_I("Background download completed: %s", setupPath.c_str());
                        } else {
                                dlError_ = err.empty() ? "download failed" : err;
                                LOG_E("Background download failed: %s", dlError_.c_str());
                        }
                }
                dlState_.store(ok ? DownloadState::Done : DownloadState::Failed, std::memory_order_release);
        });
}

std::string
Updater::getDownloadedPath() const
{
        std::lock_guard lk(dlMtx_);
        return dlPath_;
}

std::string
Updater::getDownloadError() const
{
        std::lock_guard lk(dlMtx_);
        return dlError_;
}

void
Updater::resetDownload()
{
        dlState_.store(DownloadState::Idle, std::memory_order_release);
        dlProgress_.store(0, std::memory_order_release);
        std::lock_guard lk(dlMtx_);
        dlPath_.clear();
        dlError_.clear();
}

bool
Updater::launchInstaller(const std::string &setupPath)
{
#ifdef _WIN32
        if (setupPath.empty())
                return false;

        SHELLEXECUTEINFOA sei{};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb       = "open";
        sei.lpFile       = setupPath.c_str();
        sei.lpParameters = "/SILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS";
        sei.nShow        = SW_SHOWNORMAL;
        if (!ShellExecuteExA(&sei)) {
                LOG_E("Failed to launch installer: %s (err=%lu)", setupPath.c_str(), GetLastError());
                return false;
        }
        if (sei.hProcess)
                CloseHandle(sei.hProcess);
        LOG_I("Launched installer: %s", setupPath.c_str());
        return true;
#else
        (void)setupPath;
        return false;
#endif
}

Updater::~Updater()
{
        if (checkThread_.joinable())
                checkThread_.join();
        if (downloadThread_.joinable())
                downloadThread_.join();
}
