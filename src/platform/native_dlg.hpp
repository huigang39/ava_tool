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

// Platform-correct fopen for a UTF-8 encoded path (needed on Windows where
// the C runtime uses ANSI codepage). Equivalent to fopen(path, mode) on POSIX.
FILE *nativeFopen(const std::string &utf8Path, const char *mode);
