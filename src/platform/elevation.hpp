#pragma once

#include <string>
#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace elevation
{
/**
 * @brief Windows command-line quoting, including embedded quotes and trailing slashes
 */
inline std::wstring
quote(const std::wstring &argument)
{
        std::wstring result  = L"\"";
        size_t       slashes = 0;
        for (wchar_t ch : argument) {
                if (ch == L'\\') {
                        ++slashes;
                        continue;
                }
                result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                result  += ch;
                slashes  = 0;
        }
        result.append(slashes * 2, L'\\');
        return result + L'"';
}

inline constexpr const char    *marker     = "--ava-elevation-relaunch";
inline constexpr const wchar_t *wideMarker = L"--ava-elevation-relaunch";

inline bool
isAdministrator()
{
#ifdef _WIN32
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return false;
        TOKEN_ELEVATION elevation{};
        DWORD           size    = 0;
        const bool      success = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
        CloseHandle(token);
        return success && elevation.TokenIsElevated != 0;
#else
        return false;
#endif
}

enum class Result { Ready, Relaunched, Failed };

/**
 * @brief Called only after the user accepts a device permission prompt and saves the session
 */
inline Result
relaunchAsAdministrator(unsigned long &error, const std::wstring &session)
{
        error = 0;
#ifdef _WIN32
        if (isAdministrator())
                return Result::Ready;
        const std::wstring parameters =
            quote(session) + L" " + wideMarker + L" --ava-wait-parent=" + std::to_wstring(GetCurrentProcessId());
        wchar_t executable[32768]{};
        DWORD   length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) {
                error = ERROR_INSUFFICIENT_BUFFER;
                return Result::Failed;
        }
        const std::wstring path(executable);
        const auto         folder = path.substr(0, path.find_last_of(L"\\/"));
        SHELLEXECUTEINFOW  request{};
        request.cbSize       = sizeof(request);
        request.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        request.lpVerb       = L"runas";
        request.lpFile       = executable;
        request.lpParameters = parameters.c_str();
        request.lpDirectory  = folder.c_str();
        request.nShow        = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&request)) {
                error = GetLastError();
                return Result::Failed;
        }
        if (request.hProcess)
                CloseHandle(request.hProcess);
        return Result::Relaunched;
#else
        return Result::Ready;
#endif
}

/**
 * @brief The elevated instance must not open the saved session until the old one has
 *        released devices and flushed settings. This path never requests elevation
 */
inline bool
waitForPreviousInstance(unsigned long processId)
{
#ifdef _WIN32
        if (!processId || processId == GetCurrentProcessId())
                return false;
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process)
                return GetLastError() == ERROR_INVALID_PARAMETER; // Already exited
        const DWORD result = WaitForSingleObject(process, 60000);
        CloseHandle(process);
        return result == WAIT_OBJECT_0;
#else
        return true;
#endif
}
} // namespace elevation
