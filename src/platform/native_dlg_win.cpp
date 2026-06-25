#include <windows.h>

#include "native_dlg.hpp"
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>

static std::wstring
toWide(const std::string &s)
{
        int          n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
}

static std::string
toUtf8(const wchar_t *w)
{
        int         n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        // strip null terminator that WideCharToMultiByte includes
        if (!s.empty() && s.back() == '\0')
                s.pop_back();
        return s;
}

// Build a double-null–terminated filter string for OPENFILENAMEW
static std::vector<wchar_t>
buildFilter(const std::vector<NativeDlgFilter> &filters)
{
        std::vector<wchar_t> buf;
        auto                 append = [&](const std::wstring &s) {
                buf.insert(buf.end(), s.begin(), s.end());
                buf.push_back(L'\0');
        };
        for (const auto &f : filters) {
                append(toWide(f.name));
                std::wstring patterns;
                for (const auto &e : f.exts) {
                        if (!patterns.empty())
                                patterns += L';';
                        patterns += L"*." + toWide(e);
                }
                append(patterns);
        }
        buf.push_back(L'\0'); // final double-null
        return buf;
}

std::string
nativeDlgOpen(const std::string &title, const std::vector<NativeDlgFilter> &filters, const std::string &defaultDir)
{
        wchar_t       buf[MAX_PATH] = {};
        auto          filter        = buildFilter(filters);
        std::wstring  wTitle        = toWide(title);
        OPENFILENAMEW ofn           = {};
        ofn.lStructSize             = sizeof(ofn);
        ofn.hwndOwner               = GetActiveWindow();
        ofn.lpstrFilter             = filter.data();
        ofn.lpstrFile               = buf;
        ofn.nMaxFile                = MAX_PATH;
        ofn.lpstrTitle              = wTitle.c_str();
        ofn.Flags                   = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        std::wstring wDir = toWide(defaultDir);
        if (!defaultDir.empty() && defaultDir != ".")
                ofn.lpstrInitialDir = wDir.c_str();

        return GetOpenFileNameW(&ofn) ? toUtf8(buf) : "";
}

std::string
nativeDlgSave(const std::string                  &title,
              const std::vector<NativeDlgFilter> &filters,
              const std::string                  &defaultName,
              const std::string                  &defaultDir)
{
        wchar_t      buf[MAX_PATH] = {};
        std::wstring wDef          = toWide(defaultName);
        wcsncpy_s(buf, wDef.c_str(), MAX_PATH - 1);

        auto          filter = buildFilter(filters);
        std::wstring  wTitle = toWide(title);
        OPENFILENAMEW ofn    = {};
        ofn.lStructSize      = sizeof(ofn);
        ofn.hwndOwner        = GetActiveWindow();
        ofn.lpstrFilter      = filter.data();
        ofn.lpstrFile        = buf;
        ofn.nMaxFile         = MAX_PATH;
        ofn.lpstrTitle       = wTitle.c_str();
        ofn.Flags            = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        std::wstring wDir = toWide(defaultDir);
        if (!defaultDir.empty() && defaultDir != ".")
                ofn.lpstrInitialDir = wDir.c_str();

        return GetSaveFileNameW(&ofn) ? toUtf8(buf) : "";
}

std::string
nativeDlgPickDir(const std::string &title)
{
        BROWSEINFOW  bi     = {0};
        std::wstring wTitle = toWide(title);
        bi.lpszTitle        = wTitle.c_str();
        bi.ulFlags          = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        HRESULT hr     = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        bool    coInit = (hr == S_OK || hr == S_FALSE);

        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        std::string      ret  = "";
        if (pidl != 0) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path)) {
                        ret = toUtf8(path);
                }
                CoTaskMemFree(pidl);
        }

        if (coInit) {
                CoUninitialize();
        }
        return ret;
}

FILE *
nativeFopen(const std::string &utf8Path, const char *mode)
{
        std::wstring wPath = toWide(utf8Path);
        std::wstring wMode = toWide(mode);
        FILE        *f     = nullptr;
        _wfopen_s(&f, wPath.c_str(), wMode.c_str());
        return f;
}
