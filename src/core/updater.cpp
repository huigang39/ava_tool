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
                        result.error   = err.empty() ? "update check failed" : err;
                        LOG_W("Update check failed: %s", result.error.c_str());
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
