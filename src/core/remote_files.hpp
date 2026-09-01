#pragma once
#include <string>
#include <vector>
struct cJSON;
namespace remote
{
struct Settings {
        std::string firmwareUrl;
        std::string sessionsUrl;
        std::string toolUrl; // directory hosting ava_tool_setup_*.exe installers (in-app update)
        std::string username;
        std::string password;
        bool        operator==(const Settings &) const = default;
};
// UI-thread configuration. Workers receive a value snapshot
Settings   &settings();
void        save(cJSON *root);
void        load(const cJSON *root);
void        loadDefaults(const std::string &path);
bool        saveDefaults(const std::string &path);
bool        isUrl(const std::string &path);
std::string resolve(const std::string &baseUrl, const std::string &path);

/**
 * @brief Log in at the origin of `url` using cfg's credentials and return the session
 *        cookie header value ("name=value[; name2=value2]"). Empty + *err set on
 *        failure; empty + *err untouched when cfg has no credentials to try
 */
std::string login(const std::string &url, const Settings &cfg, std::string *err = nullptr);
struct File {
        std::string name;
        std::string url;
};
std::vector<File> list(const std::string &url, const Settings &config, const std::vector<std::string> &extensions);
std::string       download(const std::string &url, const Settings &config);
} // namespace remote
