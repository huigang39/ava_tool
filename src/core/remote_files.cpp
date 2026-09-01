#include "remote_files.hpp"
#include "cJSON.h"
#include "http_win.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>

namespace remote
{
Settings &
settings()
{
        // Firmware and sessions use local shared folders; updates use GitHub Releases. A saved
        // remote_files.json (loadDefaults) overrides these on top
        static Settings val{
            .firmwareUrl = "http://127.0.0.1:8090/Firmware/",
            .sessionsUrl = "http://127.0.0.1:8090/Sessions/",
            .toolUrl     = "https://github.com/huigang39/ava_tool/releases",
        };
        return val;
}
bool
isUrl(const std::string &p)
{
        return p.starts_with("http://") || p.starts_with("https://");
}
static std::string
origin(const std::string &url)
{
        if (!isUrl(url))
                return {};
        return url.substr(0, url.find_first_of("/?#", url.find("://") + 3));
}
std::string
resolve(const std::string &base, const std::string &path)
{
        if (isUrl(path))
                return path;
        if (!isUrl(base))
                return path;
        if (path.starts_with("//"))
                return base.substr(0, base.find(':') + 1) + path;
        if (path.starts_with('/'))
                return origin(base) + path;
        const auto clean = base.substr(0, base.find_first_of("?#"));
        return clean.substr(0, clean.rfind('/') + 1) + path;
}
void
save(cJSON *root)
{
        auto       *node = cJSON_CreateObject();
        const auto &s    = settings();
        cJSON_AddStringToObject(node, "firmwareUrl", s.firmwareUrl.c_str());
        cJSON_AddStringToObject(node, "sessionsUrl", s.sessionsUrl.c_str());
        cJSON_AddStringToObject(node, "toolUrl", s.toolUrl.c_str());
        cJSON_AddItemToObject(root, "remoteFiles", node);
}
void
load(const cJSON *root)
{
        const auto *node = cJSON_GetObjectItemCaseSensitive(root, "remoteFiles");
        if (!cJSON_IsObject(node))
                return; // Older sessions retain software defaults
        auto &s = settings();
        // Only overwrite address defaults when the saved value is non-empty,
        // so a stale session (e.g. saved before a field like toolUrl existed, or
        // saved with a field cleared) doesn't wipe out the shared server defaults
        auto field = [&](const char *key, std::string &out) {
                const auto *v = cJSON_GetObjectItemCaseSensitive(node, key);
                if (cJSON_IsString(v) && v->valuestring[0] != '\0')
                        out = v->valuestring;
        };
        field("firmwareUrl", s.firmwareUrl);
        // Older sessions had a separate symbol-file address. Use it only when
        // no firmware address was configured; future saves use one shared URL
        const auto *firmware = cJSON_GetObjectItemCaseSensitive(node, "firmwareUrl");
        if (!cJSON_IsString(firmware) || firmware->valuestring[0] == '\0')
                field("symbolsUrl", s.firmwareUrl);
        field("sessionsUrl", s.sessionsUrl);
        field("toolUrl", s.toolUrl);
        s.username.clear();
        s.password.clear();
}
void
loadDefaults(const std::string &path)
{
        std::ifstream     f(std::filesystem::u8path(path));
        const std::string content((std::istreambuf_iterator<char>(f)), {});
        auto             *root = cJSON_Parse(content.c_str());
        if (root) {
                load(root);
                cJSON_Delete(root);
        }
}
bool
saveDefaults(const std::string &path)
{
        auto *root = cJSON_CreateObject();
        save(root);
        char         *text = cJSON_Print(root);
        std::ofstream f(std::filesystem::u8path(path), std::ios::trunc);
        if (text)
                f << text;
        f.flush();
        bool ok = text && f.good();
        cJSON_free(text);
        cJSON_Delete(root);
        return ok;
}
static std::string
encode(const std::string &s)
{
        static const char hex[] = "0123456789ABCDEF";
        std::string       out;
        for (unsigned char c : s) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~')
                        out += char(c);
                else {
                        out += '%';
                        out += hex[c >> 4];
                        out += hex[c & 15];
                }
        }
        return out;
}
static std::string
decode(const std::string &s)
{
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                    std::isxdigit((unsigned char)s[i + 2])) {
                        out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                        i   += 2;
                } else
                        out += s[i];
        }
        return out;
}
static bool
loginPage(const std::string &body)
{
        return body.find("<form") != std::string::npos && body.find("password") != std::string::npos;
}
static bool
knownServer(const std::string &host, const Settings &cfg)
{
        return host == origin(cfg.firmwareUrl) || host == origin(cfg.sessionsUrl) || host == origin(cfg.toolUrl);
}
std::string
login(const std::string &url, const Settings &cfg, std::string *err)
{
        const auto host = origin(url);
        if (!knownServer(host, cfg) || cfg.username.empty())
                return {};
        std::string body, error, headers;
        uint32_t    status = 0;
        if (!http::request("POST",
                           host + "/login",
                           {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"},
                           "username=" + encode(cfg.username) + "&password=" + encode(cfg.password),
                           body,
                           status,
                           &error,
                           &headers)) {
                if (err)
                        *err = "Server login failed: " + error;
                return {};
        }
        const auto cookie = http::sessionCookie(headers);
        if (cookie.empty() && err)
                *err = "Server login returned no session cookie.";
        return cookie;
}
static std::string
fetch(const std::string &url, const Settings &cfg)
{
        if (!isUrl(url))
                throw std::runtime_error("Remote address must use http:// or https://");
        std::string body, error;
        uint32_t    status = 0;
        if (http::request("GET", url, {"Accept: */*"}, {}, body, status, &error) && !loginPage(body))
                return body;
        if (status != 401 && status != 403 && !loginPage(body))
                throw std::runtime_error(error);
        if (!knownServer(origin(url), cfg) || cfg.username.empty())
                throw std::runtime_error(
                    "Configure the remote server address, username and password in Settings > Server > Remote servers.");
        std::string loginErr;
        const auto  cookie = login(url, cfg, &loginErr);
        if (cookie.empty())
                throw std::runtime_error(loginErr.empty() ? "Server login failed." : loginErr);
        if (!http::request("GET", url, {"Cookie: " + cookie, "Accept: */*"}, {}, body, status, &error) || loginPage(body))
                throw std::runtime_error("Remote file request failed: " + error);
        return body;
}
static bool
accepts(const std::string &url, const std::vector<std::string> &extensions)
{
        auto path = url.substr(0, url.find_first_of("?#"));
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const auto &ext : extensions)
                if (path.ends_with(ext))
                        return true;
        return false;
}
static std::string
nameOf(const std::string &url)
{
        auto path = url.substr(0, url.find_first_of("?#"));
        return decode(path.substr(path.rfind('/') + 1));
}
std::vector<File>
list(const std::string &url, const Settings &cfg, const std::vector<std::string> &extensions)
{
        if (!isUrl(url))
                throw std::runtime_error(
                    "Configure an HTTP(S) directory or file address in Settings > Server > Remote servers.");
        if (accepts(url, extensions))
                return {{nameOf(url), url}};
        static const std::regex githubReleases(R"(https://github\.com/([^/]+)/([^/]+)/releases/?)");
        std::smatch             repository;
        if (std::regex_match(url, repository, githubReleases)) {
                std::vector<File> files;
                const auto        api = "https://api.github.com/repos/" + repository[1].str() + "/" + repository[2].str() +
                                 "/releases?per_page=100&page=";
                for (int page = 1;; ++page) {
                        std::string body, error;
                        uint32_t    status = 0;
                        if (!http::request("GET",
                                           api + std::to_string(page),
                                           {"Accept: application/vnd.github+json"},
                                           {},
                                           body,
                                           status,
                                           &error))
                                throw std::runtime_error("GitHub release request failed: " + error);
                        auto *releases = cJSON_Parse(body.c_str());
                        if (!cJSON_IsArray(releases)) {
                                cJSON_Delete(releases);
                                throw std::runtime_error("Invalid GitHub release list.");
                        }
                        const int    count   = cJSON_GetArraySize(releases);
                        const cJSON *release = nullptr;
                        cJSON_ArrayForEach(release, releases)
                        {
                                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "draft")) ||
                                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "prerelease")))
                                        continue;
                                const auto  *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
                                const cJSON *asset  = nullptr;
                                cJSON_ArrayForEach(asset, assets)
                                {
                                        const auto *link = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
                                        if (cJSON_IsString(link) && isUrl(link->valuestring) &&
                                            accepts(link->valuestring, extensions))
                                                files.push_back({nameOf(link->valuestring), link->valuestring});
                                }
                        }
                        cJSON_Delete(releases);
                        if (count < 100)
                                break;
                }
                return files;
        }
        const auto              html = fetch(url, cfg);
        static const std::regex href(R"re(href\s*=\s*(["'])(.*?)\1)re", std::regex::icase);
        std::vector<File>       files;
        for (std::sregex_iterator i(html.begin(), html.end(), href), end; i != end; ++i) {
                auto   path = (*i)[2].str();
                size_t pos  = 0;
                while ((pos = path.find("&amp;", pos)) != std::string::npos)
                        path.replace(pos, 5, "&");
                // A directory browse URL without a trailing slash is still a directory
                auto full = resolve(url.substr(0, url.find_first_of("?#")) + (url.ends_with('/') ? "" : "/"), path);
                if (origin(full) != origin(url) || !accepts(full, extensions))
                        continue;
                if (std::none_of(files.begin(), files.end(), [&](const File &f) { return f.url == full; }))
                        files.push_back({nameOf(full), full});
        }
        std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.name < b.name; });
        return files;
}
std::string
download(const std::string &url, const Settings &cfg)
{
        const auto body = fetch(url, cfg);
        if (body.empty())
                throw std::runtime_error("Remote file is empty.");
        if (accepts(url, {".elf", ".axf"}) && (body.size() < 4 || body.compare(0,
                                                                               4,
                                                                               "\x7f"
                                                                               "ELF") != 0))
                throw std::runtime_error("The server did not return an ELF/AXF file.");
        if (accepts(url, {".ava"})) {
                auto      *root = cJSON_Parse(body.c_str());
                const bool ok   = cJSON_IsObject(root);
                cJSON_Delete(root);
                if (!ok)
                        throw std::runtime_error("The server did not return a valid AVA session.");
        }
        static std::atomic<unsigned long long> sequence{0};
        const auto                             stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto                                   dir   = std::filesystem::temp_directory_path() / "ava_tool_remote";
        std::filesystem::create_directories(dir);
        auto          ext  = accepts(url, {".ava"}) ? ".ava" : ".elf";
        auto          path = dir / (std::to_string(stamp) + "_" + std::to_string(sequence++) + ext);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        file.close();
        if (!file)
                throw std::runtime_error("Cannot write the downloaded file to the local cache.");
        return path.string();
}
} // namespace remote
