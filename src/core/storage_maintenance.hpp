#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#endif

namespace storage_maintenance
{
enum class Kind { Logs, Sampling, Downloads };
struct Root {
        std::filesystem::path path;
        Kind                  kind;
};
struct Usage {
        uint64_t bytes{}, files{}, released{}, removed{}, skipped{}, errors{};
};

inline bool
isLink(const std::filesystem::path &path, const std::filesystem::file_status &status)
{
#ifdef _WIN32
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
                return true;
#endif
        return std::filesystem::is_symlink(status);
}

inline uint64_t
diskBytes(const std::filesystem::path &path, uint64_t fallback)
{
#ifdef _WIN32
        DWORD high = 0;
        SetLastError(NO_ERROR);
        const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
        if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR)
                return (uint64_t(high) << 32) | low;
#endif
        return fallback;
}

inline bool
ownedFile(const std::filesystem::path &path, Kind kind)
{
        const auto name = path.filename().u8string();
        const auto ext  = path.extension().u8string();
        if (kind == Kind::Logs)
                return ext == u8".log" || ext == u8".csv" || ext == u8".part";
        if (kind == Kind::Sampling)
                return name.starts_with(u8"ava_tool_mmap_") && ext == u8".tmp";
        return true;
}

// Only individual, app-owned regular files are removed. Do not traverse links,
// junctions or user-selected parent directories. Active writer files stay intact.
inline Usage
process(const std::vector<Root> &roots, bool clear = false)
{
        Usage                           result;
        std::set<std::filesystem::path> visited;
        for (const auto &entry : roots) {
                std::error_code ec;
                const auto      rootStatus = std::filesystem::symlink_status(entry.path, ec);
                if (ec || !std::filesystem::is_directory(rootStatus) || isLink(entry.path, rootStatus))
                        continue;
                const auto root = std::filesystem::canonical(entry.path, ec);
                if (ec || root == root.root_path()) {
                        ++result.errors;
                        continue;
                }
                std::filesystem::recursive_directory_iterator it(root, ec), end;
                if (ec) {
                        ++result.errors;
                        continue;
                }
                for (; it != end; it.increment(ec)) {
                        if (ec) {
                                ++result.errors;
                                ec.clear();
                                break;
                        }
                        const auto path   = it->path();
                        const auto status = it->symlink_status(ec);
                        if (ec) {
                                ++result.errors;
                                ec.clear();
                                continue;
                        }
                        if (isLink(path, status)) {
                                it.disable_recursion_pending();
                                continue;
                        }
                        if (!std::filesystem::is_regular_file(status) || !ownedFile(path, entry.kind))
                                continue;
                        const auto canonical = std::filesystem::canonical(path, ec);
                        if (ec) {
                                ++result.errors;
                                ec.clear();
                                continue;
                        }
                        const auto relative = canonical.lexically_relative(root);
                        if (relative.empty() || *relative.begin() == ".." || !visited.insert(canonical).second)
                                continue;
                        const auto size = std::filesystem::file_size(path, ec);
                        if (ec) {
                                ++result.errors;
                                ec.clear();
                                continue;
                        }
                        const auto bytes = diskBytes(path, size);
                        ++result.files;
                        result.bytes += bytes;
                        if (!clear)
                                continue;
#ifndef _WIN32
                        if (entry.kind == Kind::Logs && path.extension() == ".part") {
                                ++result.skipped;
                                continue;
                        }
#endif
#ifdef _WIN32
                        // Sharing=0 refuses deletion of files held by samplers,
                        // download workers, loggers or another application instance.
                        HANDLE handle = CreateFileW(path.c_str(),
                                                    DELETE | FILE_READ_ATTRIBUTES,
                                                    0,
                                                    nullptr,
                                                    OPEN_EXISTING,
                                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                                    nullptr);
                        if (handle == INVALID_HANDLE_VALUE) {
                                ++result.skipped;
                                continue;
                        }
                        BY_HANDLE_FILE_INFORMATION info{};
                        FILE_DISPOSITION_INFO      disposition{TRUE};
                        const bool                 removed =
                            GetFileInformationByHandle(handle, &info) &&
                            !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
                        CloseHandle(handle);
#else
                        // Sampling filenames include the creator PID. Existing
                        // active caches cannot be safely unlinked on POSIX here.
                        if (entry.kind == Kind::Sampling) {
                                ++result.skipped;
                                continue;
                        }
                        const bool removed = std::filesystem::remove(path, ec);
#endif
                        if (removed) {
                                ++result.removed;
                                result.released += bytes;
                        } else
                                ++result.skipped;
                }
                if (ec)
                        ++result.errors;
        }
        return result;
}
} // namespace storage_maintenance
