#include "core/http_win.hpp"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{

std::wstring
utf8ToWide(const std::string &s)
{
        if (s.empty())
                return std::wstring();
        int          n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
}

void
setErr(std::string *err, const char *msg)
{
        if (err)
                *err = msg;
}

// Open a request for `url` and read the response. Either appends the body to
// `body` (when body != nullptr) or streams it to `file` (when file != nullptr).
bool
fetch(const std::string                             &url,
      std::string                                   *body,
      FILE                                          *file,
      const std::function<void(uint64_t, uint64_t)> &onProgress,
      std::string                                   *err)
{
        std::wstring wurl = utf8ToWide(url);

        URL_COMPONENTS uc{};
        uc.dwStructSize     = sizeof(uc);
        wchar_t host[256]   = {0};
        wchar_t path[2048]  = {0};
        uc.lpszHostName     = host;
        uc.dwHostNameLength = _countof(host);
        uc.lpszUrlPath      = path;
        uc.dwUrlPathLength  = _countof(path);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
                setErr(err, "bad URL");
                return false;
        }

        const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET hSession = WinHttpOpen(
            L"ava_tool-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
                setErr(err, "WinHttpOpen failed");
                return false;
        }

        bool      ok       = false;
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        HINTERNET hRequest = nullptr;
        if (!hConnect) {
                setErr(err, "WinHttpConnect failed");
                goto cleanup;
        }

        hRequest = WinHttpOpenRequest(hConnect,
                                      L"GET",
                                      path,
                                      nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
                setErr(err, "WinHttpOpenRequest failed");
                goto cleanup;
        }

        // GitHub's REST API requires a User-Agent (set above) and is happy with JSON.
        WinHttpAddRequestHeaders(hRequest, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                setErr(err, "WinHttpSendRequest failed");
                goto cleanup;
        }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
                setErr(err, "WinHttpReceiveResponse failed");
                goto cleanup;
        }

        {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status,
                                    &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status < 200 || status >= 300) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "HTTP %lu", status);
                        setErr(err, buf);
                        goto cleanup;
                }

                uint64_t total = 0;
                DWORD    clen = 0, clenSz = sizeof(clen);
                if (WinHttpQueryHeaders(hRequest,
                                        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX,
                                        &clen,
                                        &clenSz,
                                        WINHTTP_NO_HEADER_INDEX))
                        total = clen;

                uint64_t          received = 0;
                std::vector<char> chunk(64 * 1024);
                for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                                setErr(err, "read failed");
                                goto cleanup;
                        }
                        if (avail == 0)
                                break;
                        if (avail > chunk.size())
                                chunk.resize(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(hRequest, chunk.data(), avail, &got) || got == 0)
                                break;
                        if (body)
                                body->append(chunk.data(), got);
                        if (file)
                                fwrite(chunk.data(), 1, got, file);
                        received += got;
                        if (onProgress)
                                onProgress(received, total);
                }
                ok = true;
        }

cleanup:
        if (hRequest)
                WinHttpCloseHandle(hRequest);
        if (hConnect)
                WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
}

} // namespace

namespace http
{

bool
get(const std::string &url, std::string &out, std::string *err)
{
        out.clear();
        return fetch(url, &out, nullptr, {}, err);
}

bool
download(const std::string                             &url,
         const std::string                             &destPath,
         const std::function<void(uint64_t, uint64_t)> &onProgress,
         std::string                                   *err)
{
        FILE *f = nullptr;
        if (fopen_s(&f, destPath.c_str(), "wb") != 0 || !f) {
                if (err)
                        *err = "cannot open dest file";
                return false;
        }
        bool ok = fetch(url, nullptr, f, onProgress, err);
        fclose(f);
        if (!ok)
                DeleteFileA(destPath.c_str());
        return ok;
}

} // namespace http

#else // !_WIN32

namespace http
{
bool
get(const std::string &, std::string &, std::string *err)
{
        if (err)
                *err = "HTTP not supported on this platform";
        return false;
}
bool
download(const std::string &, const std::string &, const std::function<void(uint64_t, uint64_t)> &, std::string *err)
{
        if (err)
                *err = "HTTP not supported on this platform";
        return false;
}
} // namespace http

#endif // _WIN32
