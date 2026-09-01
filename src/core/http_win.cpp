#include "core/http_win.hpp"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
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

std::string
wideToUtf8(const wchar_t *s, int len)
{
        if (!s || len <= 0)
                return std::string();
        int         n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
        return out;
}

void
setErr(std::string *err, const char *msg)
{
        if (err)
                *err = msg;
}

/**
 * @brief Open a request for `url` and read the response. Either appends the body to
 *        `body` (when body != nullptr) or streams it to `file` (when file != nullptr)
 */
bool
fetch(const std::string                             &url,
      const std::string                             &method,
      const std::vector<std::string>                &headers,
      const std::string                             &requestBody,
      std::string                                   *body,
      FILE                                          *file,
      const std::function<void(uint64_t, uint64_t)> &onProgress,
      uint32_t                                      *statusOut,
      std::string                                   *err,
      std::string                                   *responseHeaders = nullptr)
{
        std::wstring wurl = utf8ToWide(url);

        URL_COMPONENTS uc{};
        uc.dwStructSize      = sizeof(uc);
        wchar_t host[256]    = {0};
        wchar_t path[2048]   = {0};
        uc.lpszHostName      = host;
        uc.dwHostNameLength  = _countof(host);
        uc.lpszUrlPath       = path;
        uc.dwUrlPathLength   = _countof(path);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
                setErr(err, "bad URL");
                return false;
        }

        std::wstring requestPath(path, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength) {
                std::wstring extra(uc.lpszExtraInfo, uc.dwExtraInfoLength);
                extra.resize(extra.find(L'#') == std::wstring::npos ? extra.size() : extra.find(L'#'));
                requestPath += extra;
        }
        if (requestPath.empty())
                requestPath = L"/";
        const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET hSession = WinHttpOpen(
            L"ava_tool-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
                setErr(err, "WinHttpOpen failed");
                return false;
        }

        WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);
        bool               ok          = false;
        const std::wstring wmethod     = utf8ToWide(method.empty() ? "GET" : method);
        void              *payload     = requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(requestBody.data());
        const DWORD        payloadSize = static_cast<DWORD>(requestBody.size());
        HINTERNET          hConnect    = WinHttpConnect(hSession, host, uc.nPort, 0);
        HINTERNET          hRequest    = nullptr;
        if (!hConnect) {
                setErr(err, "WinHttpConnect failed");
                goto cleanup;
        }

        hRequest = WinHttpOpenRequest(hConnect,
                                      wmethod.c_str(),
                                      requestPath.c_str(),
                                      nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
                setErr(err, "WinHttpOpenRequest failed");
                goto cleanup;
        }

        // GitHub's REST API requires a User-Agent (set above) and is happy with JSON
        // Only default the Accept header when the caller has not picked one — a plain
        // file server has no reason to be asked for GitHub's media type. Scoped so the
        // `goto cleanup` jumps below do not skip a live initialization
        {
                const bool callerSetAccept = std::any_of(headers.begin(), headers.end(), [](const std::string &h) {
                        return _strnicmp(h.c_str(), "Accept:", 7) == 0;
                });
                if (!callerSetAccept)
                        WinHttpAddRequestHeaders(
                            hRequest, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        for (const std::string &header : headers) {
                const std::wstring wideHeader = utf8ToWide(header + "\r\n");
                WinHttpAddRequestHeaders(
                    hRequest, wideHeader.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, payload, payloadSize, payloadSize, 0)) {
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
                if (statusOut)
                        *statusOut = status;
                const bool statusOk = status >= 200 && status < 300;

                if (responseHeaders) {
                        DWORD size = 0;
                        WinHttpQueryHeaders(hRequest,
                                            WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                            WINHTTP_HEADER_NAME_BY_INDEX,
                                            WINHTTP_NO_OUTPUT_BUFFER,
                                            &size,
                                            WINHTTP_NO_HEADER_INDEX);
                        if (size > sizeof(wchar_t)) {
                                std::wstring raw(size / sizeof(wchar_t), L'\0');
                                if (WinHttpQueryHeaders(hRequest,
                                                        WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                                        WINHTTP_HEADER_NAME_BY_INDEX,
                                                        raw.data(),
                                                        &size,
                                                        WINHTTP_NO_HEADER_INDEX))
                                        *responseHeaders = wideToUtf8(raw.c_str(), (int)(size / sizeof(wchar_t)));
                        }
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
                ok = statusOk;
                if (!statusOk) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "HTTP %lu", status);
                        setErr(err, buf);
                }
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
        return fetch(url, "GET", {}, {}, &out, nullptr, {}, nullptr, err);
}

bool
request(const std::string              &method,
        const std::string              &url,
        const std::vector<std::string> &headers,
        const std::string              &requestBody,
        std::string                    &responseBody,
        uint32_t                       &statusCode,
        std::string                    *err,
        std::string                    *responseHeaders)
{
        responseBody.clear();
        statusCode = 0;
        if (responseHeaders)
                responseHeaders->clear();
        return fetch(url, method, headers, requestBody, &responseBody, nullptr, {}, &statusCode, err, responseHeaders);
}

bool
download(const std::string                             &url,
         const std::string                             &destPath,
         const std::function<void(uint64_t, uint64_t)> &onProgress,
         std::string                                   *err,
         const std::vector<std::string>                &headers)
{
        FILE *f = nullptr;
        if (fopen_s(&f, destPath.c_str(), "wb") != 0 || !f) {
                if (err)
                        *err = "cannot open dest file";
                return false;
        }
        bool ok = fetch(url, "GET", headers, {}, nullptr, f, onProgress, nullptr, err);
        fclose(f);
        if (!ok)
                DeleteFileA(destPath.c_str());
        return ok;
}

std::string
sessionCookie(const std::string &responseHeaders)
{
        // Raw header block is "Name: value\r\n" repeated. Collect every Set-Cookie's
        // "name=value" prefix (dropping Path/HttpOnly/... attributes) so a server that
        // splits its session across two cookies still works
        std::string result;
        size_t      pos = 0;
        while (pos < responseHeaders.size()) {
                const size_t      eol  = responseHeaders.find("\r\n", pos);
                const size_t      end  = eol == std::string::npos ? responseHeaders.size() : eol;
                const std::string line = responseHeaders.substr(pos, end - pos);
                pos                    = end + (eol == std::string::npos ? 0 : 2);
                if (_strnicmp(line.c_str(), "Set-Cookie:", 11) != 0)
                        continue;
                std::string val = line.substr(11);
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                        val.erase(val.begin());
                const size_t semi = val.find(';');
                if (semi != std::string::npos)
                        val.resize(semi);
                if (val.empty())
                        continue;
                if (!result.empty())
                        result += "; ";
                result += val;
        }
        return result;
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
request(const std::string &,
        const std::string &,
        const std::vector<std::string> &,
        const std::string &,
        std::string &,
        uint32_t    &statusCode,
        std::string *err,
        std::string *responseHeaders)
{
        statusCode = 0;
        if (responseHeaders)
                responseHeaders->clear();
        if (err)
                *err = "HTTP not supported on this platform";
        return false;
}
bool
download(const std::string &,
         const std::string &,
         const std::function<void(uint64_t, uint64_t)> &,
         std::string *err,
         const std::vector<std::string> &)
{
        if (err)
                *err = "HTTP not supported on this platform";
        return false;
}
std::string
sessionCookie(const std::string &)
{
        return std::string();
}
} // namespace http

#endif // _WIN32
