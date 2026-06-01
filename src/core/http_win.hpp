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

namespace http
{

// GET `url`, storing the response body in `out`. Follows redirects (e.g. GitHub
// release asset URLs redirect to a CDN). Returns true on HTTP 2xx.
bool get(const std::string &url, std::string &out, std::string *err = nullptr);

// Download `url` to the local file `destPath`. `onProgress(received, total)` is
// called periodically (total may be 0 if the server omits Content-Length).
bool download(const std::string                             &url,
              const std::string                             &destPath,
              const std::function<void(uint64_t, uint64_t)> &onProgress = {},
              std::string                                   *err        = nullptr);

} // namespace http

#endif // !HTTP_WIN_HPP
