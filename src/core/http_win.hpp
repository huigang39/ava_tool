/**
 * @file  http_win.hpp
 * @brief Minimal HTTPS client (WinHTTP) shared by the app's update checker and
 *        the standalone updater.exe. Windows-only; the functions return false on
 *        other platforms so the rest of the code can compile unconditionally.
 */
#ifndef HTTP_WIN_HPP
#define HTTP_WIN_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace http
{

/**
 * @brief GET `url`, storing the response body in `out`. Follows redirects (e.g. GitHub
 *        release asset URLs redirect to a CDN). Returns true on HTTP 2xx
 */
bool get(const std::string &url, std::string &out, std::string *err = nullptr);

/**
 * @brief Generic HTTPS request used by authenticated APIs. `headers` entries are full
 *        header lines without CRLF. The response body and numeric HTTP status are
 *        returned even for non-2xx responses. When `responseHeaders` is non-null it
 *        receives the raw CRLF-separated response header block — that is how callers
 *        pick up Set-Cookie from a form-login endpoint, since every call opens its own
 *        WinHTTP session and therefore keeps no cookie jar of its own
 */
bool request(const std::string              &method,
             const std::string              &url,
             const std::vector<std::string> &headers,
             const std::string              &requestBody,
             std::string                    &responseBody,
             uint32_t                       &statusCode,
             std::string                    *err             = nullptr,
             std::string                    *responseHeaders = nullptr);

// Download `url` to the local file `destPath`. `onProgress(received, total)` is
// called periodically (total may be 0 if the server omits Content-Length)
// `headers` lets callers attach a session cookie or other auth
bool download(const std::string                             &url,
              const std::string                             &destPath,
              const std::function<void(uint64_t, uint64_t)> &onProgress = {},
              std::string                                   *err        = nullptr,
              const std::vector<std::string>                &headers    = {});

/**
 * @brief Extract a "name=value" cookie pair from a raw response header block, suitable
 *        for sending straight back as a "Cookie: " header value. Returns an empty
 *        string when the block carries no Set-Cookie
 */
std::string sessionCookie(const std::string &responseHeaders);

} // namespace http

#endif // !HTTP_WIN_HPP
