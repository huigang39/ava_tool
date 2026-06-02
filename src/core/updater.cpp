#include "core/updater.hpp"

#include "app_log.hpp"
#include "core/http_win.hpp"
#include "version.hpp"

#include "cJSON.h"

#include <cctype>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int
Updater::compareVersions(const std::string &a, const std::string &b)
{
        // Parse up to three dotted numeric components, ignoring a leading 'v' and
        // anything after a '-' (pre-release) or '+' (build metadata).
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

        std::thread([this]() {
                Info result;
                result.currentVersion = AVA_VERSION;

                const std::string apiUrl = std::string("https://api.github.com/repos/") + AVA_GITHUB_OWNER + "/" +
                                           AVA_GITHUB_REPO + "/releases/latest";

                std::string body, err;
                if (!http::get(apiUrl, body, &err)) {
                        result.checked = true;
                        // 404 = no published release yet, or the repo is private and the
                        // unauthenticated API can't see it. That's a normal state, not a
                        // failure — report it gently rather than as an error.
                        if (err.find("404") != std::string::npos)
                                result.error = "No published release found yet (or the repository is private).";
                        else if (err.find("403") != std::string::npos || err.find("429") != std::string::npos)
                                result.error = "GitHub rate limit reached (60 checks/hour per IP). Please try again later.";
                        else
                                result.error = std::string("Update check failed: ") + (err.empty() ? "unknown error" : err);
                        LOG_W("Update check: %s", result.error.c_str());
                } else if (cJSON *root = cJSON_Parse(body.c_str())) {
                        if (const cJSON *tag = cJSON_GetObjectItem(root, "tag_name"); cJSON_IsString(tag))
                                result.latestVersion = tag->valuestring;
                        if (const cJSON *html = cJSON_GetObjectItem(root, "html_url"); cJSON_IsString(html))
                                result.releaseUrl = html->valuestring;
                        if (const cJSON *notes = cJSON_GetObjectItem(root, "body"); cJSON_IsString(notes)) {
                                result.notes = notes->valuestring;
                                if (result.notes.size() > 1500)
                                        result.notes.resize(1500);
                        }
                        // Prefer a Windows installer asset (.exe); fall back to the first asset.
                        if (const cJSON *assets = cJSON_GetObjectItem(root, "assets"); cJSON_IsArray(assets)) {
                                std::string firstUrl;
                                for (const cJSON *a = assets->child; a; a = a->next) {
                                        const cJSON *name = cJSON_GetObjectItem(a, "name");
                                        const cJSON *url  = cJSON_GetObjectItem(a, "browser_download_url");
                                        if (!cJSON_IsString(url))
                                                continue;
                                        if (firstUrl.empty())
                                                firstUrl = url->valuestring;
                                        if (cJSON_IsString(name)) {
                                                std::string n = name->valuestring;
                                                if (n.size() > 4 && n.substr(n.size() - 4) == ".exe") {
                                                        result.assetUrl = url->valuestring;
                                                        break;
                                                }
                                        }
                                }
                                if (result.assetUrl.empty())
                                        result.assetUrl = firstUrl;
                        }
                        cJSON_Delete(root);

                        result.checked = true;
                        if (!result.latestVersion.empty() && compareVersions(result.latestVersion, result.currentVersion) > 0) {
                                result.available = true;
                                LOG_I(
                                    "Update available: %s -> %s", result.currentVersion.c_str(), result.latestVersion.c_str());
                        } else {
                                LOG_I("Up to date (current %s, latest %s)",
                                      result.currentVersion.c_str(),
                                      result.latestVersion.c_str());
                        }
                } else {
                        result.checked = true;
                        result.error   = "could not parse release info";
                }

                {
                        std::lock_guard lk(mtx_);
                        info_ = std::move(result);
                }
                running_.store(false, std::memory_order_release);
        }).detach();
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

        std::thread([this, url]() {
#ifdef _WIN32
                char tmpDir[MAX_PATH] = {0};
                GetTempPathA(MAX_PATH, tmpDir);
                const std::string setupPath = std::string(tmpDir) + "ava_tool_setup.exe";
#else
                const std::string setupPath = "/tmp/ava_tool_setup";
#endif

                LOG_I("Background download started: %s -> %s", url.c_str(), setupPath.c_str());

                std::string err;
                bool        ok = http::download(
                    url,
                    setupPath,
                    [this](uint64_t received, uint64_t total) {
                            if (total > 0) {
                                    int pct = static_cast<int>((received * 100) / total);
                                    if (pct > 100)
                                            pct = 100;
                                    dlProgress_.store(pct, std::memory_order_release);
                            }
                    },
                    &err);

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
        }).detach();
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
