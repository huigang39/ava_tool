#ifndef AVA_APPEARANCE_HPP
#define AVA_APPEARANCE_HPP

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace appearance
{
enum class AppearanceMode { System, Schedule, Light, Dark };
using Mode = AppearanceMode;

// The daytime interval is [dayStart, nightStart). It may span midnight.
// Equal/invalid schedule bounds fall back to system appearance rather than
// arbitrarily keeping one theme forever. Explicit Light/Dark always wins.
constexpr bool
ResolveDark(Mode mode, bool systemDark, int localHour, int dayStart = 7, int nightStart = 19)
{
        if (mode == Mode::Light)
                return false;
        if (mode == Mode::Dark)
                return true;
        if (mode != Mode::Schedule || localHour < 0 || localHour > 23 || dayStart < 0 || dayStart > 23 || nightStart < 0 ||
            nightStart > 23 || dayStart == nightStart)
                return systemDark;
        const bool daytime = dayStart < nightStart ? localHour >= dayStart && localHour < nightStart
                                                   : localHour >= dayStart || localHour < nightStart;
        return !daytime;
}

inline std::string
Lower(std::string value)
{
        for (auto &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
}

#ifndef _WIN32
namespace detail
{
struct CommandResult {
        int         status = -1;
        std::string output;
};

// Read-only, fixed executables/arguments, no shell. A missing desktop service
// must never leave a theme query worker blocked indefinitely.
inline CommandResult
ReadPreference(const char *executable, std::initializer_list<const char *> arguments)
{
        CommandResult result;
        int           outputPipe[2];
        if (pipe(outputPipe) != 0)
                return result;
        fcntl(outputPipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(outputPipe[1], F_SETFD, FD_CLOEXEC);
        fcntl(outputPipe[0], F_SETFL, O_NONBLOCK);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(executable));
        for (auto argument : arguments)
                argv.push_back(const_cast<char *>(argument));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
        posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pid_t     child   = 0;
        const int spawned = posix_spawn(&child, executable, &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(outputPipe[1]);
        if (spawned != 0) {
                close(outputPipe[0]);
                return result;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        bool       done     = false;
        while (std::chrono::steady_clock::now() < deadline) {
                pollfd descriptor{outputPipe[0], POLLIN | POLLHUP, 0};
                poll(&descriptor, 1, 40);
                char    buffer[256];
                ssize_t count;
                while ((count = read(outputPipe[0], buffer, sizeof(buffer))) > 0) {
                        result.output.append(buffer, static_cast<size_t>(count));
                        if (result.output.size() > 4096)
                                break;
                }
                if (result.output.size() > 4096)
                        break;
                int        status = 0;
                const auto waited = waitpid(child, &status, WNOHANG);
                if (waited == child) {
                        // The process can exit between the first read and
                        // waitpid. Drain bytes written in that interval.
                        while ((count = read(outputPipe[0], buffer, sizeof(buffer))) > 0 && result.output.size() <= 4096)
                                result.output.append(buffer, static_cast<size_t>(count));
                        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                        if (result.output.size() > 4096) {
                                result.output.clear();
                                result.status = -1;
                        }
                        done = true;
                        break;
                }
                if (waited < 0 && errno != EINTR) {
                        done = true;
                        break;
                }
        }
        if (!done) {
                kill(child, SIGKILL);
                while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                }
                result.output.clear();
        }
        close(outputPipe[0]);
        return result;
}

inline int
KdePreference()
{
        const char       *configDir = std::getenv("XDG_CONFIG_HOME");
        const char       *userHome  = std::getenv("HOME");
        const std::string directory = configDir && *configDir ? configDir : userHome ? std::string(userHome) + "/.config" : "";
        if (directory.empty())
                return -1;
        std::ifstream file(directory + "/kdeglobals");
        std::string   line, section, scheme;
        size_t        bytes = 0;
        while (std::getline(file, line) && (bytes += line.size()) <= 65536) {
                if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                if (!line.empty() && line[0] == '[')
                        section = line;
                if (section == "[Colors:Window]" && line.rfind("BackgroundNormal=", 0) == 0) {
                        int r, g, b;
                        if (std::sscanf(line.c_str() + 17, "%d,%d,%d", &r, &g, &b) == 3 && r >= 0 && r <= 255 && g >= 0 &&
                            g <= 255 && b >= 0 && b <= 255)
                                return 0.2126f * r + 0.7152f * g + 0.0722f * b < 128 ? 1 : 0;
                }
                if (section == "[General]" && line.rfind("ColorScheme=", 0) == 0)
                        scheme = Lower(line.substr(12));
        }
        if (scheme.find("dark") != std::string::npos)
                return 1;
        if (scheme == "breeze" || scheme.find("light") != std::string::npos)
                return 0;
        return -1;
}
} // namespace detail
#endif

// Call off the render thread. A failed or unavailable platform preference
// leaves the caller's current appearance intact.
inline bool
QuerySystemDark(bool fallback = true)
{
#ifdef _WIN32
        using ReadValue             = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
        static const auto readValue = []() -> ReadValue {
                const HMODULE library = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                return library ? reinterpret_cast<ReadValue>(GetProcAddress(library, "RegGetValueW")) : nullptr;
        }();
        DWORD value = 1, size = sizeof(value);
        if (readValue && readValue(HKEY_CURRENT_USER,
                                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                   L"AppsUseLightTheme",
                                   RRF_RT_REG_DWORD,
                                   nullptr,
                                   &value,
                                   &size) == ERROR_SUCCESS)
                return value == 0;
        return fallback;
#elif defined(__APPLE__)
        const auto result = detail::ReadPreference("/usr/bin/defaults", {"read", "-g", "AppleInterfaceStyle"});
        if (result.status == 0)
                return Lower(result.output).find("dark") != std::string::npos;
        // Light mode normally removes this preference; defaults returns 1.
        return result.status == 1 ? false : fallback;
#else
        const char       *desktop     = std::getenv("XDG_CURRENT_DESKTOP");
        const char       *session     = std::getenv("DESKTOP_SESSION");
        const std::string environment = Lower(desktop && *desktop ? desktop : session ? session : "");
        if (environment.find("kde") != std::string::npos || environment.find("plasma") != std::string::npos) {
                const int kde = detail::KdePreference();
                if (kde >= 0)
                        return kde != 0;
        }
        const auto scheme =
            detail::ReadPreference("/usr/bin/gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
        if (scheme.status == 0) {
                if (scheme.output.find("prefer-dark") != std::string::npos)
                        return true;
                if (scheme.output.find("prefer-light") != std::string::npos)
                        return false;
        }
        const auto gtk = detail::ReadPreference("/usr/bin/gsettings", {"get", "org.gnome.desktop.interface", "gtk-theme"});
        if (gtk.status == 0 && !gtk.output.empty())
                return Lower(gtk.output).find("dark") != std::string::npos;
        return fallback;
#endif
}
} // namespace appearance
#endif
