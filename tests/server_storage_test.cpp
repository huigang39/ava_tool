#include "core/storage_maintenance.hpp"
#include "core/local_file_server.hpp"
#include <ws2tcpip.h>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void write(const std::filesystem::path &path, const std::string &value) { std::ofstream(path, std::ios::binary) << value; }
static std::string request(int port, const std::string &path, const char *method = "GET", const char *accept = "*/*")
{
        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(connect(socketHandle, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "HTTP connect");
        const std::string text = std::string(method) + " " + path + " HTTP/1.1\r\nHost: localhost\r\nAccept: " + accept + "\r\nConnection: close\r\n\r\n";
        send(socketHandle, text.data(), int(text.size()), 0);
        std::string reply; char buf[4096]; int size;
        while ((size = recv(socketHandle, buf, sizeof(buf), 0)) > 0) reply.append(buf, size);
        closesocket(socketHandle);
        return reply;
}
int main(int argc, char **argv)
{
        try {
                check(argc == 2, "isolated fixture directory required");
                const auto base = std::filesystem::absolute(argv[1]);
                const auto site = base / "site", logs = base / "log", cache = base / "ava_tool_mmap_cache";
                for (auto dir : {site, logs, cache, site / "docs", site / "invalid/index.html"}) std::filesystem::create_directories(dir);
                write(site / "index.html", "<!doctype html><script src='/app.js'></script>website");
                write(site / "app.js", "document.title = 'works';");
                write(site / "style.css", "body { color: red; }");
                write(site / "docs/index.html", "nested page");
                write(site / ".env", "hidden-secret");
                write(base / "outside.txt", "private");
                LocalFileServer website(LocalFileServer::Mode::Website);
                website.config.folder = site.string(); website.config.port = 0;
                check(website.start(), website.error().c_str());
                const auto html = request(website.port(), "/");
                check(html.starts_with("HTTP/1.1 200") && html.find("Content-Type: text/html") != std::string::npos && html.ends_with("website"), "Root serves index.html inline");
                check(html.find("Content-Disposition") == std::string::npos, "Website renders instead of downloading");
                check(request(website.port(), "/app.js?v=1").find("Content-Type: text/javascript") != std::string::npos, "Absolute JS assets and query strings work");
                check(request(website.port(), "/style.css").find("Content-Type: text/css") != std::string::npos, "CSS MIME type");
                check(request(website.port(), "/docs").starts_with("HTTP/1.1 301"), "Nested directories redirect");
                check(request(website.port(), "/docs/").ends_with("nested page"), "Nested index pages");
                check(request(website.port(), "/invalid/").starts_with("HTTP/1.1 404"), "Website never lists a directory named index.html");
                check(request(website.port(), "/", "HEAD").ends_with("\r\n\r\n"), "HEAD has no body");
                for (const char *path : {"/../outside.txt", "/%2e%2e/outside.txt", "/.env", "/%00", "/C%3a/Windows"})
                        check(request(website.port(), path).starts_with("HTTP/1.1 403"), "Web paths cannot escape root or expose hidden files");
                check(request(website.port(), "/missing.js").starts_with("HTTP/1.1 404"), "Missing asset is 404");
                check(request(website.port(), "/", "POST").starts_with("HTTP/1.1 405"), "Website is read-only");
                check(request(website.port(), "/route", "GET", "text/html").starts_with("HTTP/1.1 404"), "SPA routing is opt-in");
                website.stop(); website.config.spaFallback = true;
                check(website.start(), "Restart with SPA enabled");
                check(request(website.port(), "/route", "GET", "text/html").ends_with("website"), "SPA page route falls back to index");
                check(request(website.port(), "/missing.js", "GET", "text/html").starts_with("HTTP/1.1 404"), "SPA does not swallow missing assets");
                check(request(website.port(), "/api", "GET", "application/json").starts_with("HTTP/1.1 404"), "SPA does not swallow API requests");
                check(website.saveSettings((base / "website.json").string()), "Save website settings");
                LocalFileServer restored(LocalFileServer::Mode::Website); restored.loadSettings((base / "website.json").string());
                check(restored.config.folder == website.config.folder && restored.config.spaFallback, "Website settings restore");
                LocalFileServer files; files.config.folder = site.string(); files.config.port = 0;
                check(files.start(), "File share can run alongside website");
                check(request(files.port(), "/").find("<a href=") != std::string::npos, "File sharing still lists directories");
                check(request(files.port(), "/app.js").find("Content-Disposition: attachment") != std::string::npos, "File sharing still downloads attachments");
                files.stop(); website.stop();

                namespace sm = storage_maintenance;
                write(logs / "old.log", std::string(65536, 'L'));
                write(logs / "live.log.part", "active log");
                write(logs / "user.json", "keep");
                write(cache / "ava_tool_mmap_1_1.tmp", std::string(65536, 'C'));
                write(cache / "ava_tool_mmap_1_2.tmp", "busy");
                write(cache / "unrelated.tmp", "keep");
                HANDLE active = CreateFileW((cache / "ava_tool_mmap_1_2.tmp").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                check(active != INVALID_HANDLE_VALUE, "Open active cache");
                HANDLE logWriter = CreateFileW((logs / "live.log.part").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                check(logWriter != INVALID_HANDLE_VALUE, "Open active log");
                const std::vector<sm::Root> roots{{logs, sm::Kind::Logs}, {cache, sm::Kind::Sampling}, {logs, sm::Kind::Logs}};
                const auto usage = sm::process(roots);
                check(usage.files == 4 && usage.bytes >= 131072, "Usage totals bytes and deduplicates roots");
                std::error_code ec;
                std::filesystem::create_directory_symlink(base, cache / "escape", ec);
                const auto cleared = sm::process(roots, true);
                CloseHandle(active);
                CloseHandle(logWriter);
                check(cleared.removed == 2 && cleared.skipped == 2 && cleared.released >= 131072, "Cleanup reports released bytes and skips active files");
                check(std::filesystem::exists(base / "outside.txt") && std::filesystem::exists(logs / "user.json") && std::filesystem::exists(cache / "unrelated.tmp"), "Cleanup preserves unrelated files and does not follow links");
                check(sm::process(roots).files == 2, "Usage refreshes after cleanup");
                std::cout << "PASS: website routing/MIME/HEAD/SPA/settings, file sharing, cache sizes and safe cleanup\n";
        } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
