#pragma once
#include <string>
#include <vector>

struct NativeDlgFilter {
        std::string              name; // e.g. "Session Files"
        std::vector<std::string> exts; // without dot, e.g. {"ava"}
};

// Blocking open-file dialog. Returns selected path, or "" if cancelled.
std::string
nativeDlgOpen(const std::string &title, const std::vector<NativeDlgFilter> &filters, const std::string &defaultDir = ".");

// Blocking save-file dialog. Returns chosen path, or "" if cancelled.
std::string nativeDlgSave(const std::string                  &title,
                          const std::vector<NativeDlgFilter> &filters,
                          const std::string                  &defaultName = "",
                          const std::string                  &defaultDir  = ".");

// Blocking pick-directory dialog. Returns chosen path, or "" if cancelled.
std::string nativeDlgPickDir(const std::string &title);
