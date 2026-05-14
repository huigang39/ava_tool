#include "native_dlg.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string runCmd(const std::string &cmd)
{
    FILE *f = popen(cmd.c_str(), "r");
    if (!f) return {};
    char        buf[4096];
    std::string result;
    while (fgets(buf, sizeof(buf), f))
        result += buf;
    pclose(f);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// Single-quote escape for shell arguments
static std::string sq(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

static bool cmdExists(const char *name)
{
    std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
    return system(check.c_str()) == 0;
}

static std::string filterArgZenity(const std::vector<NativeDlgFilter> &filters)
{
    std::string args;
    for (const auto &f : filters) {
        // zenity: --file-filter="Name | *.ext1 *.ext2"
        std::string pat = f.name + " |";
        for (const auto &e : f.exts) pat += " *." + e;
        args += " --file-filter=" + sq(pat);
    }
    return args;
}

std::string
nativeDlgOpen(const std::string &title, const std::vector<NativeDlgFilter> &filters,
              const std::string &defaultDir)
{
    if (cmdExists("zenity")) {
        std::string cmd = "zenity --file-selection --title=" + sq(title);
        cmd += filterArgZenity(filters);
        if (!defaultDir.empty() && defaultDir != ".")
            cmd += " --filename=" + sq(defaultDir + "/");
        cmd += " 2>/dev/null";
        return runCmd(cmd);
    }
    if (cmdExists("kdialog")) {
        std::string base = (defaultDir.empty() || defaultDir == ".") ? "." : defaultDir;
        std::string cmd  = "kdialog --getopenfilename " + sq(base);
        if (!filters.empty()) {
            std::string pat;
            for (const auto &f : filters)
                for (const auto &e : f.exts)
                    pat += "*." + e + " ";
            cmd += " " + sq(pat);
        }
        cmd += " --title " + sq(title) + " 2>/dev/null";
        return runCmd(cmd);
    }
    return {};
}

std::string
nativeDlgSave(const std::string &title, const std::vector<NativeDlgFilter> &filters,
              const std::string &defaultName, const std::string &defaultDir)
{
    if (cmdExists("zenity")) {
        std::string base = (defaultDir.empty() || defaultDir == ".") ? "." : defaultDir;
        if (!defaultName.empty()) base += "/" + defaultName;
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title=" + sq(title);
        cmd += " --filename=" + sq(base);
        cmd += " 2>/dev/null";
        return runCmd(cmd);
    }
    if (cmdExists("kdialog")) {
        std::string base = (defaultDir.empty() || defaultDir == ".") ? "." : defaultDir;
        if (!defaultName.empty()) base += "/" + defaultName;
        std::string cmd = "kdialog --getsavefilename " + sq(base);
        if (!filters.empty()) {
            std::string pat;
            for (const auto &f : filters)
                for (const auto &e : f.exts)
                    pat += "*." + e + " ";
            cmd += " " + sq(pat);
        }
        cmd += " --title " + sq(title) + " 2>/dev/null";
        return runCmd(cmd);
    }
    return {};
}
