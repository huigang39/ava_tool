#include <winsock2.h>
#include <ws2tcpip.h>
#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "core/local_file_server.hpp"
#include "core/http_win.hpp"
#include "core/remote_files.hpp"
#include "core/updater.hpp"
#include "cJSON.h"
#include <fstream>
#include <future>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static std::string readFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}
static void writeFile(const std::filesystem::path &path, const std::string &body) {
    std::ofstream output(path, std::ios::binary); output.write(body.data(), body.size());
    check(output.good(), "write fixture");
}
static SOCKET connectTo(int port) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(client != INVALID_SOCKET, "create client socket");
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect to file server");
    return client;
}
static std::string raw(int port, const std::string &path, const std::string &method = "GET") {
    SOCKET client = connectTo(port);
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    check(send(client, request.data(), static_cast<int>(request.size()), 0) == request.size(), "send HTTP request");
    std::string response; char buffer[8192]; int count;
    while ((count = recv(client, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, count);
    closesocket(client);
    return response;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    module_init();
    try {
        const auto remoteDefaults = remote::settings();
        check(remoteDefaults.firmwareUrl == "http://127.0.0.1:8090/Firmware/" &&
              remoteDefaults.sessionsUrl == "http://127.0.0.1:8090/Sessions/" &&
              remoteDefaults.toolUrl == "https://github.com/huigang39/ava_tool/releases" && remoteDefaults.username.empty() && remoteDefaults.password.empty(),
              "remote defaults use local shared folders and GitHub updates without old credentials");
        remote::settings() = {"http://10.18.136.120:8090/files/Firmware/NeoFSA/Debug",
                              "http://10.18.136.120:8090/files/Tool/ava_tool/session",
                              "http://10.18.136.120:8090/files/Tool/ava_tool", "admin", "123"};
        auto configuredLegacy = remote::settings(); configuredLegacy.username.clear(); configuredLegacy.password.clear();
        auto *legacy = cJSON_CreateObject(); remote::save(legacy);
        remote::settings() = remoteDefaults;
        remote::load(legacy); cJSON_Delete(legacy);
        check(remote::settings() == configuredLegacy, "custom server addresses are preserved and retired credentials cleared");
        remote::settings() = {"http://custom:8123/fw/", "http://custom:8123/ava/", "http://custom:8123/tools/", "user", "password"};
        auto custom = remote::settings(); custom.username.clear(); custom.password.clear();
        auto *savedCustom = cJSON_CreateObject(); remote::save(savedCustom); remote::load(savedCustom); cJSON_Delete(savedCustom);
        check(remote::settings() == custom, "explicit custom server settings are preserved");
        auto *retired = cJSON_Parse(R"({"remoteFiles":{"firmwareUrl":"http://10.18.136.120:8089/Firmware/","username":"admin","password":"old"}})");
        remote::load(retired); cJSON_Delete(retired);
        check(remote::settings().firmwareUrl == "http://10.18.136.120:8089/Firmware/" && remote::settings().username.empty() && remote::settings().password.empty(),
              "configured firmware URL is retained verbatim without hardcoded migration");
        remote::settings() = remoteDefaults;
        puts("PASS: local defaults, configured old server address and custom server preservation");
        const auto fixture = std::filesystem::path(argv[1]).parent_path() /
            ("server-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto root = fixture / "shared";
        LocalFileServer defaults;
        const auto defaultsPath = fixture / "defaults/local_file_server.json";
        defaults.loadSettings(defaultsPath.string());
        check(!defaults.config.folder.empty() && defaults.config.firmwareFolder == "Firmware" &&
              defaults.config.sessionsFolder == "Sessions" && defaults.config.updatesFolder == "Updates",
              "first run supplies shared folder and role directories without user input");
        for (const char *folder : {"Firmware", "Sessions", "Updates"})
            check(std::filesystem::is_directory(std::filesystem::u8path(defaults.config.folder) / folder),
                  "default role directories are created automatically");
        check(std::filesystem::is_regular_file(defaultsPath), "default configuration is persisted");
        LocalFileServer defaultReload; defaultReload.loadSettings(defaultsPath.string());
        check(defaultReload.config.folder == defaults.config.folder && !defaultReload.config.autoStart,
              "default folder is stable on restart without enabling server automatically");
        defaults.config.port = 0; check(defaults.start(), "default share starts without manual folder setup"); defaults.stop();
        writeFile(defaultsPath, "{\"folder\":\"\",\"firmwareFolder\":\"\"}");
        LocalFileServer migrated; migrated.loadSettings(defaultsPath.string());
        check(migrated.config.folder == defaultReload.config.folder && migrated.config.firmwareFolder == "Firmware",
              "previously empty shared folder configuration receives defaults");
        puts("PASS: default shared folder creation, direct startup, persistence and empty-settings migration");
        for (const char *folder : {"firmware", "sessions", "updates", "temp"})
            std::filesystem::create_directories(root / folder);
        std::string binary("\x7f" "ELF", 4); binary += std::string(3 * 1024 * 1024, '\0'); binary.back() = 'Z';
        writeFile(root / "firmware" / std::filesystem::u8path("中文 & 测试.axf"), binary);
        writeFile(root / "sessions/session.ava", "{\"monitors\":[],\"variables\":[]}");
        writeFile(root / "updates/ava_tool_setup_999.0.0.exe", "MZ-test-installer-never-executed");
        writeFile(root / "updates/ava_tool_setup_0.10.0.exe", "MZ-test-version-0.10.0");
        writeFile(root / "updates/ava_tool_setup_0.2.0.exe", "MZ-test-version-0.2.0");
        writeFile(root / "updates/ava_tool_setup_0.0.1.exe", "MZ-test-old-version");
        writeFile(root / "updates/unrelated.exe", "MZ-not-an-app-installer");
        writeFile(fixture / "secret.txt", "not shared");
        LocalFileServer server;
        server.config.folder = root.string(); server.config.port = 0;
        check(server.start(), server.error().c_str());
        const auto base = server.directoryUrl("127.0.0.1");
        check(base.ends_with('/'), "root URL has trailing slash");
        const auto firmwareUrl = server.directoryUrl("127.0.0.1", "firmware");
        const auto sessionsUrl = server.directoryUrl("127.0.0.1", "sessions");
        const auto updatesUrl = server.directoryUrl("127.0.0.1", "updates");
        remote::Settings cfg{firmwareUrl, sessionsUrl, updatesUrl, "old-user", "old-password"};
        auto files = remote::list(firmwareUrl, cfg, {".axf"});
        check(files.size() == 1 && files[0].name == "中文 & 测试.axf", "existing remote client lists Unicode AXF");
        const auto downloaded = remote::download(files[0].url, cfg);
        check(readFile(downloaded) == binary, "binary AXF download preserves NULs and all bytes");
        std::filesystem::remove(downloaded);
        writeFile(root / "firmware/image.bin", std::string("\0\1\2", 3));
        writeFile(root / "firmware/image.hex", ":00000001FF\n");
        writeFile(root / "firmware/image.ELF", binary);
        auto firmware = remote::list(firmwareUrl, {firmwareUrl, sessionsUrl, updatesUrl, "", ""}, {".axf", ".elf"});
        check(firmware.size() == 2 && std::all_of(firmware.begin(), firmware.end(), [](const auto &file) {
                  return file.name == "中文 & 测试.axf" || file.name == "image.ELF";
              }), "JFlash and variable manager list only AXF/ELF and exclude BIN/HEX");
        auto flashFirmware = remote::list(firmwareUrl, cfg, {".axf", ".elf", ".hex", ".bin"});
        check(flashFirmware.size() == 4, "firmware programmer accepts AXF/ELF/HEX/BIN while variable manager accepts only AXF/ELF");
        std::filesystem::create_directories(root / "other");
        writeFile(root / "other/changed.elf", binary);
        const auto otherUrl = server.directoryUrl("127.0.0.1", "other");
        auto changedConfig = cfg; changedConfig.firmwareUrl = otherUrl;
        auto changedFiles = remote::list(changedConfig.firmwareUrl, changedConfig, {".axf", ".elf"});
        check(changedFiles.size() == 1 && changedFiles[0].name == "changed.elf" && changedFiles[0].url.starts_with(otherUrl),
              "firmware listing follows the configured directory after address changes");
        files = remote::list(sessionsUrl, cfg, {".ava"});
        check(files.size() == 1, "existing client lists AVA sessions");
        const auto session = remote::download(files[0].url, cfg);
        check(readFile(session) == readFile(root / "sessions/session.ava"), "existing client downloads valid AVA session");
        std::filesystem::remove(session);
        auto response = raw(server.port(), "/sessions");
        check(response.starts_with("HTTP/1.1 301") && response.find("Location: /sessions/") != std::string::npos,
              "directory URL redirect preserves relative links");
        response = raw(server.port(), "/sessions/session.ava", "HEAD");
        check(response.starts_with("HTTP/1.1 200") && response.ends_with("\r\n\r\n"), "HEAD omits file body");
        check(raw(server.port(), "/missing").starts_with("HTTP/1.1 404"), "missing files return 404");
        check(raw(server.port(), "/sessions/session.ava", "POST").starts_with("HTTP/1.1 405"), "sharing is read-only");
        for (const char *path : {"/../secret.txt", "/%2e%2e/secret.txt", "/%2e%2e%5csecret.txt", "/%00", "/%zz",
                                 "/C%3A/windows", "//secret.txt", "/CON", "/..%20/secret.txt"})
            check(raw(server.port(), path).starts_with("HTTP/1.1 403"), "outside/malformed/device paths are denied");
        check(raw(server.port(), "/sessions/../updates/ava_tool_setup_999.0.0.exe").starts_with("HTTP/1.1 200"),
              "relative session references inside the share remain valid");
        std::error_code symlinkError;
        std::filesystem::create_directory_symlink(fixture, root / "escape", symlinkError);
        if (!symlinkError) check(raw(server.port(), "/escape/secret.txt").starts_with("HTTP/1.1 403"), "symlink escape denied");
        bool rejected = false;
        try { server.directoryUrl("127.0.0.1", "../"); } catch (...) { rejected = true; }
        check(rejected, "remote address mapping rejects folders outside share");
        std::vector<std::future<std::string>> requests;
        for (int i = 0; i < 8; ++i)
            requests.push_back(std::async(std::launch::async, [&] { return raw(server.port(), "/sessions/session.ava"); }));
        for (auto &request : requests) check(request.get().starts_with("HTTP/1.1 200"), "concurrent clients work");
        LocalFileServer collision; collision.config = server.config; collision.config.port = server.port();
        check(!collision.start() && !collision.error().empty(), "port collision reports error");
        server.config.port = server.port(); server.config.autoStart = true;
        server.config.firmwareFolder = "firmware"; server.config.sessionsFolder = "sessions"; server.config.updatesFolder = "updates";
        server.config.name = "实验台一";
        check(server.saveSettings((fixture / "server.json").string()), "save host-local settings");
        LocalFileServer restored; restored.loadSettings((fixture / "server.json").string());
        check(restored.config.folder == server.config.folder && restored.config.port == server.config.port &&
              restored.config.autoStart && restored.config.updatesFolder == "updates" && restored.config.name == "实验台一", "restore host-local configuration");
        check(server.refreshDiscovery(), server.discoveryError().c_str());
        check(server.discoveryError().empty(), "server discovery responder starts");
        LocalFileServer peer; peer.config = server.config; peer.config.port = 0;
        peer.config.name = "实验台二"; peer.config.firmwareFolder.clear();
        check(peer.start(), peer.error().c_str());
        check(peer.discoveryError().empty(), "multiple servers can listen for discovery on the same computer");
        auto found = LocalFileServer::discover(1200);
        auto first = std::find_if(found.begin(), found.end(), [&](const auto &item) { return item.port == server.port() && item.name == "实验台一"; });
        auto second = std::find_if(found.begin(), found.end(), [&](const auto &item) { return item.port == peer.port() && item.name == "实验台二"; });
        check(first != found.end() && second != found.end(), "one scan discovers multiple named servers on custom HTTP ports");
        check(std::count_if(found.begin(), found.end(), [&](const auto &item) { return item.id == first->id; }) == 1 &&
              std::count_if(found.begin(), found.end(), [&](const auto &item) { return item.id == second->id; }) == 1,
              "repeated replies and network interfaces do not duplicate servers");
        check(first->firmwareUrl == first->baseUrl() + "firmware/" && first->sessionsUrl == first->baseUrl() + "sessions/" &&
              first->updatesUrl == first->baseUrl() + "updates/" && second->firmwareUrl == second->baseUrl(),
              "each server advertises its own exact directory configuration");
        remote::Settings selected{first->firmwareUrl, first->sessionsUrl, first->updatesUrl, "", ""};
        check(remote::list(selected.firmwareUrl, selected, {".axf"}).size() == 1,
              "selected discovered server works with existing remote client");
        const auto peerId = second->id;
        peer.stop();
        server.config.name = "实验台一（新名称）"; check(server.refreshDiscovery(), "republish edited server metadata");
        found = LocalFileServer::discover(800);
        check(std::none_of(found.begin(), found.end(), [&](const auto &item) { return item.id == peerId; }), "rescan removes stopped servers");
        check(std::any_of(found.begin(), found.end(), [&](const auto &item) { return item.name == "实验台一（新名称）" && item.port == server.port(); }),
              "server name edits are visible without restarting HTTP sharing");
        puts("PASS: LAN discovery, multiple named/custom-port servers, deduplication, directory selection, live metadata refresh and stopped peers");
        remote::settings() = cfg;
        {
            Updater updater; updater.checkAsync();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (updater.isChecking() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto info = updater.get();
            check(info.available && info.latestVersion == "999.0.0", "real updater discovers locally hosted installer");
            check(info.installers.size() == 4 && info.installers[0].version == "999.0.0" &&
                  info.installers[1].version == "0.10.0" && info.installers[2].version == "0.2.0" &&
                  info.installers[3].version == "0.0.1", "all application installers sort by numeric version newest first");
            check(info.sourceUrl == updatesUrl && info.assetUrl.starts_with(updatesUrl),
                  "update check and installer use the configured address");
            // Isolate the updater's fixed temporary filename from other running apps
            const char *previous = std::getenv("TMP"); const std::string oldTmp = previous ? previous : "";
            _putenv_s("TMP", (root / "temp").string().c_str());
            updater.downloadAsync(info.assetUrl);
            while (updater.getDownloadState() == Updater::DownloadState::Downloading && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            _putenv_s("TMP", oldTmp.c_str());
            check(updater.getDownloadState() == Updater::DownloadState::Done,
                  "real updater downloads public installer even with old credentials");
            check(readFile(updater.getDownloadedPath()) == "MZ-test-installer-never-executed", "installer download bytes intact");
            updater.resetDownload();
            _putenv_s("TMP", (root / "temp").string().c_str());
            updater.downloadAsync(info.installers.back().url);
            const auto olderDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (updater.getDownloadState() == Updater::DownloadState::Downloading && std::chrono::steady_clock::now() < olderDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            _putenv_s("TMP", oldTmp.c_str());
            check(updater.getDownloadState() == Updater::DownloadState::Done &&
                  readFile(updater.getDownloadedPath()) == "MZ-test-old-version", "selected older installer downloads after a completed latest-version download");
            remote::settings().toolUrl = firmwareUrl;
            updater.checkAsync();
            const auto changedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (updater.isChecking() && std::chrono::steady_clock::now() < changedDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            info = updater.get();
            check(info.checked && !info.available && !info.error.empty() && info.sourceUrl == firmwareUrl,
                  "next check uses the changed address without falling back to the previous update directory");
            remote::settings() = cfg;
        }
        remote::settings().username.clear(); remote::settings().password.clear();
        auto *document = cJSON_CreateObject(); remote::save(document);
        remote::settings().username = "stale"; remote::settings().password = "stale";
        remote::load(document); cJSON_Delete(document);
        check(remote::settings().username.empty() && remote::settings().password.empty(), "empty credentials survive AVA reload");
        remote::settings() = cfg;
        remote::settings().toolUrl = argv[2];
        const char *previousTmp = std::getenv("TMP"); const std::string originalTmp = previousTmp ? previousTmp : "";
        _putenv_s("TMP", (root / "temp").string().c_str());
        for (const char *route : {"/401/ava_tool_setup_999.0.0.exe", "/login-page/ava_tool_setup_999.0.0.exe"}) {
            Updater protectedUpdater;
            protectedUpdater.downloadAsync(std::string(argv[2]) + route);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (protectedUpdater.getDownloadState() == Updater::DownloadState::Downloading && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(protectedUpdater.getDownloadState() == Updater::DownloadState::Done,
                  "existing cookie-auth update servers still download after HTTP 401 or HTTP 200 login page");
            check(readFile(protectedUpdater.getDownloadedPath()) == "MZ-cookie-protected-test-installer", "authenticated installer bytes intact");
        }
        _putenv_s("TMP", originalTmp.c_str());
        puts("PASS: existing authenticated update download (HTTP 401 and HTTP 200 login page)");
        SOCKET slowClient = connectTo(server.port());
        const auto stopped = std::chrono::steady_clock::now();
        server.stop();
        closesocket(slowClient);
        check(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(2), "stop interrupts idle clients promptly");
        check(server.start(), "restart releases and rebinds same port"); server.stop();
        puts("PASS: HTTP listing/download, Unicode/binary AVA/AXF, public updater discovery/download, concurrency, persistence, containment, stop/restart");
    } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return 0;
}
