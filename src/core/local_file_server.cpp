#ifdef _WIN32
// Winsock must precede IP Helper's Windows declarations
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on
#endif
#include "local_file_server.hpp"
#include "cJSON.h"
#include "version.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>

namespace
{
constexpr unsigned short discoveryPort  = 45990;
constexpr const char    *discoveryGroup = "239.255.42.99";
constexpr const char    *discoveryQuery = "AVA_TOOL_DISCOVER/1 ";
constexpr const char    *discoveryReply = "AVA_TOOL_REPLY/1 ";
std::string
token()
{
        std::random_device random;
        std::ostringstream text;
        text << std::hex << random() << random() << random() << random();
        return text.str();
}
std::string
utf8(const std::filesystem::path &path)
{
        auto text = path.generic_u8string();
        return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
std::string
encode(const std::string &text)
{
        const char *hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : text) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '/')
                        out += char(c);
                else {
                        out += '%';
                        out += hex[c >> 4];
                        out += hex[c & 15];
                }
        }
        return out;
}
std::string
html(const std::string &text)
{
        std::string out;
        for (char c : text) {
                switch (c) {
                        case '&':
                                out += "&amp;";
                                break;
                        case '<':
                                out += "&lt;";
                                break;
                        case '>':
                                out += "&gt;";
                                break;
                        case '"':
                                out += "&quot;";
                                break;
                        case '\'':
                                out += "&#39;";
                                break;
                        default:
                                out += c;
                }
        }
        return out;
}
std::string
decode(const std::string &text)
{
        auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                        return c - '0';
                if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                return -1;
        };
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
                unsigned char c = text[i];
                if (c == '%') {
                        if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0)
                                throw std::runtime_error("Invalid URL encoding");
                        c  = static_cast<unsigned char>((hex(text[i + 1]) << 4) | hex(text[i + 2]));
                        i += 2;
                }
                if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '*')
                        throw std::runtime_error("Invalid path");
                out += char(c);
        }
        return out;
}
bool
inside(const std::filesystem::path &root, const std::filesystem::path &path)
{
        auto r = root.begin(), p = path.begin();
        for (; r != root.end(); ++r, ++p)
                if (p == path.end() || *r != *p)
                        return false;
        return true;
}
std::filesystem::path
resolvePath(const std::filesystem::path &root, const std::string &relative)
{
        const auto path = std::filesystem::u8path(relative);
        if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
                throw std::runtime_error("Path must be relative to the shared folder");
        for (const auto &part : path) {
                auto name = utf8(part);
                if (name.empty() || name == "." || name == "..")
                        continue;
                if (name.back() == '.' || name.back() == ' ' || name.find_first_of(":\\\"<>|?*") != std::string::npos)
                        throw std::runtime_error("Invalid file name");
                auto stem = name.substr(0, name.find('.'));
                std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                    (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
                     stem[3] <= '9'))
                        throw std::runtime_error("Device names are not shared");
        }
        const auto resolved = std::filesystem::weakly_canonical(root / path);
        if (!inside(root, resolved))
                throw std::runtime_error("Path is outside the shared folder");
        return resolved;
}

const char *
webContentType(const std::filesystem::path &path)
{
        auto ext = utf8(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".html" || ext == ".htm")
                return "text/html; charset=utf-8";
        if (ext == ".css")
                return "text/css; charset=utf-8";
        if (ext == ".js" || ext == ".mjs")
                return "text/javascript; charset=utf-8";
        if (ext == ".json" || ext == ".map")
                return "application/json; charset=utf-8";
        if (ext == ".webmanifest")
                return "application/manifest+json";
        if (ext == ".svg")
                return "image/svg+xml";
        if (ext == ".png")
                return "image/png";
        if (ext == ".jpg" || ext == ".jpeg")
                return "image/jpeg";
        if (ext == ".gif")
                return "image/gif";
        if (ext == ".webp")
                return "image/webp";
        if (ext == ".avif")
                return "image/avif";
        if (ext == ".ico")
                return "image/x-icon";
        if (ext == ".woff")
                return "font/woff";
        if (ext == ".woff2")
                return "font/woff2";
        if (ext == ".ttf")
                return "font/ttf";
        if (ext == ".wasm")
                return "application/wasm";
        if (ext == ".txt" || ext == ".csv")
                return "text/plain; charset=utf-8";
        if (ext == ".pdf")
                return "application/pdf";
        if (ext == ".mp4")
                return "video/mp4";
        if (ext == ".mp3")
                return "audio/mpeg";
        return "application/octet-stream";
}
} // namespace

LocalFileServer::~LocalFileServer()
{
        stop();
}
void
LocalFileServer::stop()
{
        running_.store(false);
        if (discoveryThread_.joinable())
                discoveryThread_.join();
        for (auto &thread : workers_)
                if (thread.joinable())
                        thread.join();
        workers_.clear();
#ifdef _WIN32
        if (discoverySocket_ != ~std::uintptr_t{0})
                closesocket(static_cast<SOCKET>(discoverySocket_));
        if (listener_ != ~std::uintptr_t{0})
                closesocket(static_cast<SOCKET>(listener_));
        if (socketsInitialized_)
                WSACleanup();
#endif
        listener_           = ~std::uintptr_t{0};
        discoverySocket_    = ~std::uintptr_t{0};
        socketsInitialized_ = false;
}
bool
LocalFileServer::start()
{
        stop();
        error_.clear();
        try {
                if (config.folder.empty())
                        throw std::runtime_error("Choose a shared folder first");
                root_ = std::filesystem::canonical(std::filesystem::u8path(config.folder));
                if (!std::filesystem::is_directory(root_))
                        throw std::runtime_error("Shared path is not a directory");
                if (mode_ == Mode::Website && !std::filesystem::is_regular_file(resolvePath(root_, "index.html")))
                        throw std::runtime_error("Website folder must contain index.html");
                spaFallback_ = config.spaFallback;
                if (config.port < 0 || config.port > 65535)
                        throw std::runtime_error("Invalid port");
#ifdef _WIN32
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data))
                        throw std::runtime_error("WSAStartup failed");
                socketsInitialized_ = true;
                SOCKET listener     = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (listener == INVALID_SOCKET)
                        throw std::runtime_error("Cannot create server socket");
                listener_      = static_cast<std::uintptr_t>(listener);
                BOOL exclusive = TRUE;
                setsockopt(
                    listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_ANY);
                address.sin_port        = htons(static_cast<short>(config.port));
                if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR ||
                    listen(listener, SOMAXCONN) == SOCKET_ERROR)
                        throw std::runtime_error("Cannot listen on port (in use or blocked), Winsock " +
                                                 std::to_string(WSAGetLastError()));
                int length = sizeof(address);
                if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == SOCKET_ERROR)
                        throw std::runtime_error("Cannot obtain server port");
                boundPort_         = ntohs(address.sin_port);
                u_long nonblocking = 1;
                if (ioctlsocket(listener, FIONBIO, &nonblocking) == SOCKET_ERROR)
                        throw std::runtime_error("Cannot configure server socket");
                requests_.store(0);
                running_.store(true);
                for (int i = 0; i < 4; ++i)
                        workers_.emplace_back([this] { serve(); });
                instanceId_ = token();
                if (mode_ == Mode::Files)
                        startDiscovery();
                return true;
#else
                throw std::runtime_error("Local file server is currently supported on Windows");
#endif
        } catch (const std::exception &e) {
                error_ = e.what();
                stop();
                return false;
        }
}

void
LocalFileServer::serve()
{
#ifdef _WIN32
        const SOCKET listener = static_cast<SOCKET>(listener_);
        while (running()) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(listener, &readable);
                timeval timeout{0, 100000};
                if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
                        continue;
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                        continue;
                u_long nonblocking = 1;
                if (ioctlsocket(client, FIONBIO, &nonblocking) == 0) {
                        try {
                                handle(static_cast<std::uintptr_t>(client));
                        } catch (...) {
                        }
                }
                shutdown(client, SD_SEND);
                closesocket(client);
        }
#endif
}
bool
LocalFileServer::sendAll(std::uintptr_t client, const char *data, size_t size)
{
#ifdef _WIN32
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (size && running()) {
                int count = send(static_cast<SOCKET>(client), data, static_cast<int>(std::min<size_t>(size, 65536)), 0);
                if (count > 0) {
                        data     += count;
                        size     -= count;
                        deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                } else {
                        if (WSAGetLastError() != WSAEWOULDBLOCK || std::chrono::steady_clock::now() >= deadline)
                                return false;
                        fd_set writable;
                        FD_ZERO(&writable);
                        FD_SET(static_cast<SOCKET>(client), &writable);
                        timeval timeout{0, 100000};
                        select(0, nullptr, &writable, nullptr, &timeout);
                }
        }
        return size == 0;
#else
        (void)client;
        (void)data;
        (void)size;
        return false;
#endif
}
void
LocalFileServer::handle(std::uintptr_t client)
{
#ifdef _WIN32
        std::string            request;
        const auto             deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::array<char, 4096> buffer{};
        while (running() && request.find("\r\n\r\n") == std::string::npos) {
                int count = recv(static_cast<SOCKET>(client), buffer.data(), static_cast<int>(buffer.size()), 0);
                if (count > 0) {
                        request.append(buffer.data(), count);
                        if (request.size() > 16384)
                                return;
                } else {
                        if (!count || WSAGetLastError() != WSAEWOULDBLOCK || std::chrono::steady_clock::now() >= deadline)
                                return;
                        fd_set readable;
                        FD_ZERO(&readable);
                        FD_SET(static_cast<SOCKET>(client), &readable);
                        timeval timeout{0, 100000};
                        select(0, &readable, nullptr, nullptr, &timeout);
                }
        }
        if (!running())
                return;
        ++requests_;
        std::istringstream line(request.substr(0, request.find("\r\n")));
        std::string        method, target, version;
        line >> method >> target >> version;
        const bool head = method == "HEAD";
        auto header = [&](const std::string &status, uint64_t size, const std::string &type, const std::string &extra = "") {
                std::string text = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(size) +
                                   "\r\nContent-Type: " + type +
                                   "\r\nConnection: close\r\nCache-Control: no-store\r\n"
                                   "X-Content-Type-Options: nosniff\r\n" +
                                   extra + "\r\n";
                return sendAll(client, text.data(), text.size());
        };
        auto response = [&](const std::string &status, const std::string &body, const std::string &extra = "") {
                if (header(status, body.size(), "text/plain; charset=utf-8", extra) && !head)
                        sendAll(client, body.data(), body.size());
        };
        if (method != "GET" && !head) {
                response("405 Method Not Allowed", "Read-only file server", "Allow: GET, HEAD\r\n");
                return;
        }
        if (!target.starts_with('/') || !version.starts_with("HTTP/1.")) {
                response("400 Bad Request", "Invalid request");
                return;
        }
        std::string           path;
        std::filesystem::path file;
        try {
                path = decode(target.substr(0, target.find_first_of("?#")));
                if (path.starts_with("//"))
                        throw std::runtime_error("Invalid path");
                file = resolvePath(root_, path.substr(1));
                if (mode_ == Mode::Website) {
                        for (const auto &part : std::filesystem::u8path(path.substr(1))) {
                                const auto name = utf8(part);
                                if (name.size() > 1 && name.front() == '.' && name != "..")
                                        throw std::runtime_error("Hidden files are not web content");
                        }
                }
        } catch (...) {
                response("403 Forbidden", "Path is not shared");
                return;
        }
        try {
                if (mode_ == Mode::Website) {
                        if (std::filesystem::is_directory(file)) {
                                if (!path.ends_with('/')) {
                                        response("301 Moved Permanently", "", "Location: " + encode(path) + "/\r\n");
                                        return;
                                }
                                file = resolvePath(root_, path.substr(1) + "index.html");
                        } else if (!std::filesystem::exists(file) && spaFallback_ && file.extension().empty()) {
                                std::string headers = request.substr(request.find("\r\n") + 2);
                                std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
                                        return char(std::tolower(c));
                                });
                                if (headers.find("text/html") != std::string::npos)
                                        file = resolvePath(root_, "index.html");
                        }
                }
                if (!std::filesystem::exists(file) || (mode_ == Mode::Website && !std::filesystem::is_regular_file(file))) {
                        response("404 Not Found", "File not found");
                        return;
                }
                if (std::filesystem::is_directory(file)) {
                        if (!path.ends_with('/')) {
                                response("301 Moved Permanently", "", "Location: " + encode(path) + "/\r\n");
                                return;
                        }
                        std::string body = "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
                                           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                                           "<title>AvA Tool files</title><style>body{font:16px "
                                           "system-ui;max-width:900px;margin:40px auto;padding:0 20px}"
                                           "li{margin:12px 0}a{color:#1769aa}</style><h1>AvA Tool</h1><h2>" +
                                           html(path) + "</h2><ul>";
                        if (path != "/")
                                body += "<li><a href=\"../\">../</a></li>";
                        std::vector<std::filesystem::directory_entry> entries;
                        for (const auto &entry : std::filesystem::directory_iterator(file))
                                entries.push_back(entry);
                        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
                                return a.path().filename() < b.path().filename();
                        });
                        for (const auto &entry : entries) {
                                if (!inside(root_, std::filesystem::weakly_canonical(entry.path())))
                                        continue;
                                bool directory = entry.is_directory();
                                if (!directory && !entry.is_regular_file())
                                        continue;
                                const std::string name  = utf8(entry.path().filename()) + (directory ? "/" : "");
                                body                   += "<li><a href=\"" + encode(name) + "\">" + html(name) + "</a></li>";
                        }
                        body += "</ul><p>Read-only shared files / 只读文件共享</p></html>";
                        if (header("200 OK",
                                   body.size(),
                                   "text/html; charset=utf-8",
                                   "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n") &&
                            !head)
                                sendAll(client, body.data(), body.size());
                } else if (std::filesystem::is_regular_file(file)) {
                        std::ifstream input(file, std::ios::binary);
                        if (!input) {
                                response("403 Forbidden", "Cannot read file");
                                return;
                        }
                        const auto size    = std::filesystem::file_size(file);
                        const bool website = mode_ == Mode::Website;
                        if (!header("200 OK",
                                    size,
                                    website ? webContentType(file) : "application/octet-stream",
                                    website ? "" : "Content-Disposition: attachment\r\n") ||
                            head)
                                return;
                        std::array<char, 65536> chunk{};
                        uint64_t                remaining = size;
                        while (remaining && running()) {
                                input.read(chunk.data(),
                                           static_cast<std::streamsize>(std::min<uint64_t>(remaining, chunk.size())));
                                const auto count = input.gcount();
                                if (count <= 0 || !sendAll(client, chunk.data(), static_cast<size_t>(count)))
                                        return;
                                remaining -= static_cast<uint64_t>(count);
                        }
                } else
                        response("403 Forbidden", "Not a regular file");
        } catch (...) {
                response("500 Internal Server Error", "Cannot read shared path");
        }
#else
        (void)client;
#endif
}

std::vector<std::string>
LocalFileServer::addresses() const
{
        std::vector<std::string> result;
#ifdef _WIN32
        ULONG                      size = 16384;
        std::vector<unsigned char> storage(size);
        auto                      *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
        ULONG                      status   = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
        if (status == ERROR_BUFFER_OVERFLOW) {
                storage.resize(size);
                adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
                status   = GetAdaptersAddresses(AF_INET,
                                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                              nullptr,
                                              adapters,
                                              &size);
        }
        if (status == NO_ERROR)
                for (auto *adapter = adapters; adapter; adapter = adapter->Next) {
                        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                                continue;
                        for (auto *entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
                                if (entry->Address.lpSockaddr->sa_family != AF_INET)
                                        continue;
                                auto *address = reinterpret_cast<sockaddr_in *>(entry->Address.lpSockaddr);
                                char  text[INET_ADDRSTRLEN]{};
                                if (InetNtopA(AF_INET, &address->sin_addr, text, sizeof(text)))
                                        result.emplace_back(text);
                        }
                }
#endif
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        result.emplace_back("127.0.0.1");
        return result;
}
std::string
LocalFileServer::directoryUrl(const std::string &host, const std::string &relative) const
{
        if (!running())
                throw std::runtime_error("Start the local file server first");
        const auto path = resolvePath(root_, relative);
        if (!std::filesystem::is_directory(path))
                throw std::runtime_error("Shared subdirectory does not exist");
        auto suffix = utf8(path.lexically_relative(root_));
        if (suffix == ".")
                suffix.clear();
        return "http://" + host + ":" + std::to_string(boundPort_) + "/" + encode(suffix) + (suffix.empty() ? "" : "/");
}
bool
LocalFileServer::refreshDiscovery()
{
        discoveryError_ = discoverySocketError_;
        std::string payload;
        try {
                if (!running())
                        return true;
                std::string name = config.name;
#ifdef _WIN32
                if (name.empty()) {
                        wchar_t computer[256]{};
                        DWORD   length = 256;
                        if (GetComputerNameW(computer, &length))
                                name = utf8(std::filesystem::path(computer));
                }
#endif
                if (name.empty())
                        name = "AvA Tool";
                auto path = [&](const std::string &folder) {
                        const std::string base = "http://localhost:" + std::to_string(port());
                        return directoryUrl("localhost", folder).substr(base.size());
                };
                const auto firmware = path(config.firmwareFolder), sessions = path(config.sessionsFolder),
                           updates = path(config.updatesFolder);
                auto *root         = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "product", "ava_tool");
                cJSON_AddNumberToObject(root, "protocol", 1);
                cJSON_AddStringToObject(root, "id", instanceId_.c_str());
                cJSON_AddStringToObject(root, "name", name.substr(0, 256).c_str());
                cJSON_AddStringToObject(root, "version", AVA_VERSION);
                cJSON_AddNumberToObject(root, "port", port());
                cJSON_AddStringToObject(root, "firmware", firmware.c_str());
                cJSON_AddStringToObject(root, "sessions", sessions.c_str());
                cJSON_AddStringToObject(root, "updates", updates.c_str());
                char *text = cJSON_PrintUnformatted(root);
                if (text)
                        payload = text;
                cJSON_free(text);
                cJSON_Delete(root);
                if (payload.empty() || payload.size() > 8000)
                        throw std::runtime_error("Discovery metadata is too large");
        } catch (const std::exception &e) {
                discoveryError_ = e.what();
        }
        std::lock_guard lock(discoveryMutex_);
        discoveryPayload_ = std::move(payload);
        return discoveryError_.empty();
}

void
LocalFileServer::startDiscovery()
{
        discoverySocketError_.clear();
        refreshDiscovery();
#ifdef _WIN32
        auto   failure = [&](const std::string &message) { discoverySocketError_ = discoveryError_ = message; };
        SOCKET udp     = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == INVALID_SOCKET) {
                failure("Cannot create LAN discovery socket");
                return;
        }
        BOOL reuse = TRUE;
        setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = htons(discoveryPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(udp, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
                failure("Cannot listen for LAN discovery, Winsock " + std::to_string(WSAGetLastError()));
                closesocket(udp);
                return;
        }
        bool joined = false;
        for (const auto &host : addresses()) {
                ip_mreq group{};
                InetPtonA(AF_INET, discoveryGroup, &group.imr_multiaddr);
                InetPtonA(AF_INET, host.c_str(), &group.imr_interface);
                joined |=
                    setsockopt(udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&group), sizeof(group)) == 0;
        }
        if (!joined)
                failure("Cannot join LAN discovery multicast group");
        u_long nonblocking = 1;
        if (ioctlsocket(udp, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                failure("Cannot configure LAN discovery socket");
                closesocket(udp);
                return;
        }
        discoverySocket_ = static_cast<std::uintptr_t>(udp);
        discoveryThread_ = std::thread([this] { answerDiscovery(); });
#endif
}

void
LocalFileServer::answerDiscovery()
{
#ifdef _WIN32
        const auto socket = static_cast<SOCKET>(discoverySocket_);
        while (running()) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(socket, &readable);
                timeval timeout{0, 100000};
                if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
                        continue;
                char        buffer[128];
                sockaddr_in source{};
                int         length = sizeof(source);
                int         count = recvfrom(socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&source), &length);
                if (count <= 0)
                        continue;
                const std::string query(buffer, count);
                if (!query.starts_with(discoveryQuery))
                        continue;
                const auto nonce = query.substr(std::strlen(discoveryQuery));
                if (nonce.empty() || nonce.size() > 64 ||
                    !std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) { return std::isxdigit(c); }))
                        continue;
                std::string payload;
                {
                        std::lock_guard lock(discoveryMutex_);
                        payload = discoveryPayload_;
                }
                if (payload.empty())
                        continue;
                const auto reply = std::string(discoveryReply) + nonce + "\n" + payload;
                sendto(socket, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr *>(&source), length);
        }
#endif
}

std::vector<LocalFileServer::DiscoveredServer>
LocalFileServer::discover(int milliseconds)
{
        std::vector<DiscoveredServer> result;
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
                throw std::runtime_error("Cannot initialize LAN scanner");
        struct SocketScope {
                SOCKET val{INVALID_SOCKET};
                ~SocketScope()
                {
                        if (val != INVALID_SOCKET)
                                closesocket(val);
                        WSACleanup();
                }
        } scope;
        scope.val = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (scope.val == INVALID_SOCKET)
                throw std::runtime_error("Cannot create LAN scanner socket");
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(scope.val, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR)
                throw std::runtime_error("Cannot bind LAN scanner socket");
        u_long nonblocking = 1;
        if (ioctlsocket(scope.val, FIONBIO, &nonblocking) == SOCKET_ERROR)
                throw std::runtime_error("Cannot configure LAN scanner socket");
        DWORD ttl = 1, loop = 1;
        setsockopt(scope.val, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof(ttl));
        setsockopt(scope.val, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char *>(&loop), sizeof(loop));
        BOOL broadcast = TRUE;
        setsockopt(scope.val, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&broadcast), sizeof(broadcast));
        const std::string nonce = token(), query = std::string(discoveryQuery) + nonce;
        const std::string expected   = std::string(discoveryReply) + nonce + "\n";
        const auto        interfaces = LocalFileServer{}.addresses();
        auto              sendQuery  = [&] {
                sockaddr_in target{};
                target.sin_family = AF_INET;
                target.sin_port   = htons(discoveryPort);
                for (const auto &host : interfaces) {
                        in_addr address{};
                        InetPtonA(AF_INET, host.c_str(), &address);
                        if (setsockopt(scope.val,
                                       IPPROTO_IP,
                                       IP_MULTICAST_IF,
                                       reinterpret_cast<const char *>(&address),
                                       sizeof(address)) == 0) {
                                InetPtonA(AF_INET, discoveryGroup, &target.sin_addr);
                                sendto(scope.val,
                                       query.data(),
                                       static_cast<int>(query.size()),
                                       0,
                                       reinterpret_cast<sockaddr *>(&target),
                                       sizeof(target));
                        }
                        // Also query this machine directly (works without multicast)
                        target.sin_addr = address;
                        sendto(scope.val,
                               query.data(),
                               static_cast<int>(query.size()),
                               0,
                               reinterpret_cast<sockaddr *>(&target),
                               sizeof(target));
                }
                target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                sendto(scope.val,
                       query.data(),
                       static_cast<int>(query.size()),
                       0,
                       reinterpret_cast<sockaddr *>(&target),
                       sizeof(target));
        };
        const auto started  = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::milliseconds(std::clamp(milliseconds, 200, 5000));
        sendQuery();
        bool repeated = false;
        while (std::chrono::steady_clock::now() < deadline) {
                if (!repeated && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(400)) {
                        sendQuery();
                        repeated = true;
                }
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(scope.val, &readable);
                timeval timeout{0, 100000};
                if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
                        continue;
                char        buffer[8192];
                sockaddr_in source{};
                int         length = sizeof(source);
                const int   count =
                    recvfrom(scope.val, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&source), &length);
                if (count <= 0)
                        continue;
                const std::string reply(buffer, count);
                if (!reply.starts_with(expected))
                        continue;
                auto *root = cJSON_ParseWithLength(reply.data() + expected.size(), reply.size() - expected.size());
                if (!root)
                        continue;
                auto field = [&](const char *key) -> std::string {
                        auto *item = cJSON_GetObjectItemCaseSensitive(root, key);
                        return cJSON_IsString(item) ? item->valuestring : "";
                };
                auto            *port     = cJSON_GetObjectItemCaseSensitive(root, "port");
                auto            *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
                DiscoveredServer server;
                server.id      = field("id");
                server.name    = field("name");
                server.version = field("version");
                server.port    = cJSON_IsNumber(port) ? port->valueint : 0;
                char host[INET_ADDRSTRLEN]{};
                InetNtopA(AF_INET, &source.sin_addr, host, sizeof(host));
                server.host         = host;
                const auto firmware = field("firmware"), sessions = field("sessions"), updates = field("updates");
                auto       validPath = [](const std::string &path) {
                        return path.starts_with('/') && !path.starts_with("//") && path.ends_with('/') &&
                               path.find_first_of("\r\n\\?#") == std::string::npos;
                };
                const bool valid = field("product") == "ava_tool" && cJSON_IsNumber(protocol) && protocol->valueint == 1 &&
                                   !server.id.empty() && server.id.size() <= 128 && !server.name.empty() &&
                                   server.name.size() <= 256 && server.version.size() <= 64 && server.port >= 1 &&
                                   server.port <= 65535 && validPath(firmware) && validPath(sessions) && validPath(updates);
                cJSON_Delete(root);
                if (!valid)
                        continue;
                const auto base    = server.baseUrl();
                server.firmwareUrl = base + firmware.substr(1);
                server.sessionsUrl = base + sessions.substr(1);
                server.updatesUrl  = base + updates.substr(1);
                auto existing =
                    std::find_if(result.begin(), result.end(), [&](const auto &item) { return item.id == server.id; });
                if (existing == result.end()) {
                        if (result.size() < 256)
                                result.push_back(std::move(server));
                } else if (existing->host == "127.0.0.1" && server.host != "127.0.0.1")
                        *existing = std::move(server);
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
                if (a.name != b.name)
                        return a.name < b.name;
                if (a.host != b.host)
                        return a.host < b.host;
                return a.port < b.port;
        });
#else
        (void)milliseconds;
        throw std::runtime_error("LAN server discovery is currently supported on Windows");
#endif
        return result;
}

void
LocalFileServer::loadSettings(const std::string &path)
{
        std::ifstream file(std::filesystem::u8path(path));
        std::string   text((std::istreambuf_iterator<char>(file)), {});
        auto         *root  = cJSON_Parse(text.c_str());
        auto          field = [&](const char *key, std::string &val) {
                auto *item = cJSON_GetObjectItemCaseSensitive(root, key);
                if (cJSON_IsString(item))
                        val = item->valuestring;
        };
        field("folder", config.folder);
        field("firmwareFolder", config.firmwareFolder);
        field("name", config.name);
        field("sessionsFolder", config.sessionsFolder);
        field("updatesFolder", config.updatesFolder);
        auto *port = cJSON_GetObjectItemCaseSensitive(root, "port");
        if (cJSON_IsNumber(port) && port->valueint >= 1 && port->valueint <= 65535)
                config.port = port->valueint;
        config.autoStart   = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "autoStart"));
        config.spaFallback = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "spaFallback"));
        cJSON_Delete(root);
        // Initialize new installs and migrate previously saved empty folders
        // Existing user-selected folders (including empty role paths) stay intact
        if (config.folder.empty() && mode_ == Mode::Files) {
                try {
                        const auto shared = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path() / "shared";
                        config.folder     = utf8(shared);
                        config.firmwareFolder = "Firmware";
                        config.sessionsFolder = "Sessions";
                        config.updatesFolder  = "Updates";
                        for (const char *folder : {"Firmware", "Sessions", "Updates"})
                                std::filesystem::create_directories(shared / folder);
                        if (!saveSettings(path))
                                error_ = "Cannot save default shared folder settings";
                } catch (const std::exception &e) {
                        error_ = "Cannot create default shared folder: " + std::string(e.what());
                }
        }
}
bool
LocalFileServer::saveSettings(const std::string &path) const
{
        auto *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "folder", config.folder.c_str());
        cJSON_AddStringToObject(root, "name", config.name.c_str());
        cJSON_AddStringToObject(root, "firmwareFolder", config.firmwareFolder.c_str());
        cJSON_AddStringToObject(root, "sessionsFolder", config.sessionsFolder.c_str());
        cJSON_AddStringToObject(root, "updatesFolder", config.updatesFolder.c_str());
        cJSON_AddNumberToObject(root, "port", config.port);
        cJSON_AddBoolToObject(root, "autoStart", config.autoStart);
        cJSON_AddBoolToObject(root, "spaFallback", config.spaFallback);
        char         *text = cJSON_Print(root);
        std::ofstream file(std::filesystem::u8path(path), std::ios::trunc);
        if (text)
                file << text;
        file.flush();
        bool ok = text && file.good();
        cJSON_free(text);
        cJSON_Delete(root);
        return ok;
}
