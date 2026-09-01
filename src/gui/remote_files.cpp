#include "remote_files.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "platform/native_dlg.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#pragma comment(lib, "shell32.lib")
#endif

static bool
drawRemoteSources()
{
        auto &s       = remote::settings();
        bool  changed = false;
        auto  field   = [&](const char *label, std::string &val) {
                char buf[2048]{};
                snprintf(buf, sizeof(buf), "%s", val.c_str());
                ImGui::SetNextItemWidth(440);
                if (ImGui::InputText(label, buf, sizeof(buf), 0)) {
                        val     = buf;
                        changed = true;
                }
        };
        field(tr("Firmware URL", "固件地址"), s.firmwareUrl);
        field(tr("Session file URL", "会话文件地址"), s.sessionsUrl);
        field(tr("Update URL", "更新地址"), s.toolUrl);

        ImGui::Separator();
        ImGui::TextUnformatted(tr("LAN Servers", "局域网服务器"));
        static std::future<std::vector<LocalFileServer::DiscoveredServer>> scan;
        static std::vector<LocalFileServer::DiscoveredServer>              discovered;
        static std::string                                                 selectedServer, scanError;
        static bool                                                        scanned = false;
        if (scan.valid() && scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                        discovered = scan.get();
                        scanError.clear();
                } catch (const std::exception &e) {
                        scanError = e.what();
                }
                scanned = true;
                if (std::none_of(
                        discovered.begin(), discovered.end(), [&](const auto &item) { return item.id == selectedServer; }))
                        selectedServer.clear();
        }
        ImGui::BeginDisabled(scan.valid());
        if (ImGui::Button(tr("Scan LAN servers", "扫描局域网服务器"))) {
                scanError.clear();
                discovered.clear();
                selectedServer.clear();
                scan = std::async(std::launch::async, [] { return LocalFileServer::discover(); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (scan.valid())
                ImGui::TextDisabled("%s", tr("Scanning...", "正在扫描……"));
        else if (scanned)
                ImGui::TextDisabled("%s: %zu", tr("Servers found", "发现服务器"), discovered.size());
        if (!scanError.empty())
                ui::ErrorTextWrapped("%s", scanError.c_str());
        if (!discovered.empty()) {
                if (ImGui::BeginChild("##lanServers", ImVec2(620, 130), true)) {
                        for (const auto &item : discovered) {
                                ImGui::PushID(item.id.c_str());
                                const auto label = item.name + "  [" + item.host + ":" + std::to_string(item.port) + "]  v" +
                                                   item.version + "###server";
                                if (ImGui::Selectable(label.c_str(), item.id == selectedServer))
                                        selectedServer = item.id;
                                ImGui::PopID();
                        }
                }
                ImGui::EndChild();
                auto selected = std::find_if(
                    discovered.begin(), discovered.end(), [&](const auto &item) { return item.id == selectedServer; });
                if (selected != discovered.end()) {
                        ImGui::Text("%s: %s", tr("Firmware", "固件"), selected->firmwareUrl.c_str());
                        ImGui::Text("%s: %s", tr("Sessions", "会话"), selected->sessionsUrl.c_str());
                        ImGui::Text("%s: %s", tr("Updates", "更新"), selected->updatesUrl.c_str());
                        if (ImGui::Button(tr("Use selected server", "使用此服务器"))) {
                                s.firmwareUrl = selected->firmwareUrl;
                                s.sessionsUrl = selected->sessionsUrl;
                                s.toolUrl     = selected->updatesUrl;
                                s.username.clear();
                                s.password.clear();
                                changed = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Copy server address", "复制服务器地址")))
                                ImGui::SetClipboardText(selected->baseUrl().c_str());
                        if (s.firmwareUrl == selected->firmwareUrl && s.sessionsUrl == selected->sessionsUrl &&
                            s.toolUrl == selected->updatesUrl)
                                ImGui::TextDisabled("%s", tr("Using this server", "当前使用此服务器"));
                }
        } else if (scanned && !scan.valid() && scanError.empty()) {
                ImGui::TextDisabled("%s", tr("No servers found", "未发现服务器"));
        }

        return changed;
}

static bool
drawFileServer(LocalFileServer &server, const std::string &settingsPath)
{
        auto &s       = remote::settings();
        bool  changed = false;
        ImGui::Separator();
        ImGui::TextUnformatted(tr("Local File Server", "本机文件服务器"));
        auto                           &local        = server.config;
        bool                            localChanged = false;
        static std::vector<std::string> addresses;
        static std::string              host, message;
        auto                            refresh = [&] {
                addresses = server.addresses();
                if (std::find(addresses.begin(), addresses.end(), host) == addresses.end())
                        host = addresses.front();
        };
        if (addresses.empty())
                refresh();
        auto localField = [&](const char *label, std::string &val) {
                char buffer[2048]{};
                snprintf(buffer, sizeof(buffer), "%s", val.c_str());
                ImGui::SetNextItemWidth(440);
                if (ImGui::InputText(label, buffer, sizeof(buffer))) {
                        val          = buffer;
                        localChanged = true;
                }
        };
        localField(tr("Server name", "服务器名称"), local.name);
        char sharedFolder[2048]{};
        snprintf(sharedFolder, sizeof(sharedFolder), "%s", local.folder.c_str());
        ImGui::SetNextItemWidth(440);
        ImGui::InputText(tr("Shared folder", "共享文件夹"), sharedFolder, sizeof(sharedFolder), ImGuiInputTextFlags_ReadOnly);
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button(tr("Open shared folder", "打开共享文件夹"))) {
                const auto folder = std::filesystem::u8path(local.folder).wstring();
                if (reinterpret_cast<INT_PTR>(
                        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                        message = tr("Cannot open shared folder.", "无法打开共享文件夹。");
        }
#endif
        ImGui::BeginDisabled(server.running());
        ImGui::SameLine();
        if (ImGui::Button(tr("Change folder...###SharedFolder", "更换目录...###SharedFolder"))) {
                auto path = nativeDlgPickDir(tr("Choose shared folder", "选择共享文件夹"));
                if (!path.empty()) {
                        local.folder = path;
                        localChanged = true;
                }
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt(tr("Server port", "服务器端口"), &local.port))
                localChanged = true;
        ImGui::EndDisabled();
        if (ImGui::Checkbox(tr("Start server on app launch", "启动程序时开启服务器"), &local.autoStart))
                localChanged = true;
        if (server.running()) {
                ImGui::Text("%s (%llu)",
                            tr("Running / requests served", "运行中 / 已处理请求"),
                            static_cast<unsigned long long>(server.requests()));
                if (ImGui::Button(tr("Stop server", "停止服务器"))) {
                        server.stop();
                        message.clear();
                }
        } else {
                ImGui::BeginDisabled(local.folder.empty() || local.port < 1 || local.port > 65535);
                if (ImGui::Button(tr("Start server", "启动服务器"))) {
                        if (server.start()) {
                                refresh();
                                message.clear();
                        }
                }
                ImGui::EndDisabled();
        }
        if (!server.error().empty())
                ui::ErrorTextWrapped("%s", server.error().c_str());
        if (!server.discoveryError().empty())
                ui::ErrorTextWrapped("%s: %s", tr("LAN discovery", "局域网发现"), server.discoveryError().c_str());
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo(tr("LAN address", "局域网地址"), host.c_str())) {
                for (const auto &address : addresses)
                        if (ImGui::Selectable(address.c_str(), address == host))
                                host = address;
                ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Refresh IPs", "刷新 IP")))
                refresh();
        if (server.running()) {
                const auto url = "http://" + host + ":" + std::to_string(server.port()) + "/";
                ImGui::TextUnformatted(url.c_str());
                ImGui::SameLine();
                if (ImGui::Button(tr("Copy address", "复制地址")))
                        ImGui::SetClipboardText(url.c_str());
        }
        localField(tr("Firmware subfolder", "固件子目录"), local.firmwareFolder);
        localField(tr("Session subfolder", "会话子目录"), local.sessionsFolder);
        localField(tr("Update subfolder", "更新包子目录"), local.updatesFolder);
        // Sync when the active shared endpoint changes, without overwriting manual
        // remote server selection on every frame
        static std::string sharedEndpoint;
        if (server.running()) {
                try {
                        const auto firmware = server.directoryUrl(host, local.firmwareFolder);
                        const auto sessions = server.directoryUrl(host, local.sessionsFolder);
                        const auto updates  = server.directoryUrl(host, local.updatesFolder);
                        const auto endpoint = firmware + "\n" + sessions + "\n" + updates;
                        if (sharedEndpoint != endpoint) {
                                sharedEndpoint = endpoint;
                                s.firmwareUrl  = firmware;
                                s.sessionsUrl  = sessions;
                                s.toolUrl      = updates;
                                s.username.clear();
                                s.password.clear();
                                changed = true;
                        }
                } catch (const std::exception &e) {
                        message = e.what();
                }
        } else {
                sharedEndpoint.clear();
        }
        if (localChanged) {
                if (!server.saveSettings(settingsPath))
                        message = tr("Cannot save local server settings.", "无法保存本机服务器设置。");
                server.refreshDiscovery();
        }
        if (!message.empty())
                ui::ErrorTextWrapped("%s", message.c_str());
        ImGui::TextDisabled("%s",
                            tr("Read-only sharing while AvA Tool is running. Files outside this folder are not served.",
                               "AvA Tool 运行期间提供只读共享，仅共享所选文件夹及其子目录。"));
        ImGui::TextDisabled("%s",
                            tr("For Wi-Fi clients, allow AvA Tool through Windows Firewall on the private network.",
                               "Wi-Fi 设备访问时，请允许 AvA Tool 通过 Windows 防火墙的专用网络。"));
        return changed;
}

static void
drawWebsiteServer(LocalFileServer &server, const std::string &settingsPath)
{
        auto                           &config  = server.config;
        bool                            changed = false;
        static std::string              message, host;
        static std::vector<std::string> addresses;
        auto                            refresh = [&] {
                addresses = server.addresses();
                if (std::find(addresses.begin(), addresses.end(), host) == addresses.end())
                        host = addresses.empty() ? "127.0.0.1" : addresses.front();
        };
        if (addresses.empty())
                refresh();
        ImGui::BeginDisabled(server.running());
        char folder[2048]{};
        snprintf(folder, sizeof(folder), "%s", config.folder.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##websiteFolder", tr("Website folder", "网页目录"), folder, sizeof(folder))) {
                config.folder = folder;
                changed       = true;
        }
        if (ImGui::Button(tr("Choose website folder...", "选择网页目录..."))) {
                const auto selected = nativeDlgPickDir(tr("Website folder", "网页目录"));
                if (!selected.empty()) {
                        config.folder = selected;
                        changed       = true;
                }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt(tr("HTTP port", "网页端口"), &config.port))
                changed = true;
        if (ImGui::Checkbox(tr("Single-page app routing", "单页应用路由"), &config.spaFallback))
                changed = true;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Serve index.html for page routes; missing assets still return 404.",
                                     "页面路由回退到 index.html；缺失的资源文件仍返回 404。"));
        ImGui::EndDisabled();
        if (ImGui::Checkbox(tr("Deploy on app launch", "启动程序时部署网页"), &config.autoStart))
                changed = true;
        if (server.running()) {
                ImGui::Text("%s  %llu",
                            tr("Deployed / requests", "已部署 / 请求数"),
                            static_cast<unsigned long long>(server.requests()));
                if (ImGui::Button(tr("Stop website", "停止网页服务")))
                        server.stop();
        } else {
                ImGui::BeginDisabled(config.folder.empty() || config.port < 1 || config.port > 65535);
                if (ImGui::Button(tr("Deploy website", "部署网页")) && server.start()) {
                        refresh();
                        message.clear();
                }
                ImGui::EndDisabled();
        }
        if (!server.error().empty())
                ui::ErrorTextWrapped("%s", server.error().c_str());
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo(tr("LAN address", "局域网地址"), host.c_str())) {
                for (const auto &address : addresses)
                        if (ImGui::Selectable(address.c_str(), host == address))
                                host = address;
                ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Refresh IPs", "刷新 IP")))
                refresh();
        if (server.running()) {
                const auto url = "http://" + host + ":" + std::to_string(server.port()) + "/";
                ImGui::TextUnformatted(url.c_str());
                if (ImGui::Button(tr("Copy website address", "复制网页地址")))
                        ImGui::SetClipboardText(url.c_str());
#ifdef _WIN32
                ImGui::SameLine();
                if (ImGui::Button(tr("Open in browser", "浏览器打开"))) {
                        const auto wide = std::filesystem::u8path(url).wstring();
                        if (reinterpret_cast<INT_PTR>(
                                ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                                message = tr("Cannot open browser.", "无法打开浏览器。");
                }
#endif
        }
        ImGui::TextWrapped("%s",
                           host == "127.0.0.1" ? tr("Select the Wi-Fi / LAN IP to share the website with other devices.",
                                                    "选择 Wi-Fi / 局域网 IP，其他设备即可访问。")
                                               : tr("Devices on the same LAN can open this address while AvA Tool is running.",
                                                    "AvA Tool 运行时，同一局域网内的设备可用浏览器打开此地址。"));
        ImGui::TextDisabled("%s",
                            tr("Static HTML / CSS / JavaScript. Access across subnets or VLANs requires routing and network "
                               "permission for this port.",
                               "支持静态 HTML / CSS / JavaScript；跨子网或 VLAN 访问需路由和网络策略允许此端口。"));
        if (changed && !server.saveSettings(settingsPath))
                message = tr("Cannot save website settings.", "无法保存网页设置。");
        if (!message.empty())
                ui::ErrorTextWrapped("%s", message.c_str());
}

bool
drawServerSettings(LocalFileServer &server, LocalFileServer &website, const std::string &settingsDirectory)
{
        bool         changed  = false;
        const auto  *viewport = ImGui::GetWindowViewport();
        const ImVec2 panelSize(std::min(720.0f, std::max(1.0f, viewport->WorkSize.x - 32.0f)),
                               std::min(500.0f, std::max(1.0f, viewport->WorkSize.y - 64.0f)));
        if (ImGui::BeginChild("##server_settings_fixed", panelSize, false)) {
                if (ImGui::BeginTabBar("##server_settings")) {
                        if (ImGui::BeginTabItem(tr("File sharing", "文件共享"))) {
                                ImGui::PushID("file_server");
                                changed |= drawFileServer(server, settingsDirectory + "/local_file_server.json");
                                ImGui::PopID();
                                ImGui::EndTabItem();
                        }
                        if (ImGui::BeginTabItem(tr("Website", "网页部署"))) {
                                ImGui::PushID("website_server");
                                drawWebsiteServer(website, settingsDirectory + "/website_server.json");
                                ImGui::PopID();
                                ImGui::EndTabItem();
                        }
                        if (ImGui::BeginTabItem(tr("Remote servers", "远程服务器"))) {
                                ImGui::PushID("remote_servers");
                                changed |= drawRemoteSources();
                                ImGui::PopID();
                                ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                }
        }
        ImGui::EndChild();
        return changed;
}

std::string
RemoteFileMenu::draw(const std::string &url, const std::vector<std::string> &extensions)
{
        const auto cfg     = remote::settings();
        const bool changed = !initialized_ || url_ != url || !(config_ == cfg);
        if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                        auto result = pending_.get();
                        if (!changed) {
                                files_ = std::move(result);
                                error_.clear();
                        }
                } catch (const std::exception &e) {
                        if (!changed)
                                error_ = e.what();
                }
        }
        if (changed) {
                files_.clear();
                error_.clear();
        }
        bool refresh =
            ImGui::MenuItem(tr("Refresh remote files", "刷新远程文件"), nullptr, false, !pending_.valid() && !url.empty());
        if ((changed || refresh) && !pending_.valid()) {
                initialized_ = true;
                url_         = url;
                config_      = cfg;
                files_.clear();
                error_.clear();
                if (!url.empty())
                        pending_ = std::async(std::launch::async,
                                              [url, cfg, extensions] { return remote::list(url, cfg, extensions); });
        }
        if (pending_.valid())
                ImGui::TextDisabled("%s", tr("Loading remote files...", "正在读取远程文件……"));
        if (url.empty())
                ImGui::TextDisabled("%s",
                                    tr("Configure the address in Settings > Server > Remote servers.",
                                       "请先在“设置 → 服务器 → 远程服务器”配置地址。"));
        if (!error_.empty())
                ui::ErrorTextWrapped("%s", error_.c_str());
        if (!url.empty() && !pending_.valid() && error_.empty() && files_.empty())
                ImGui::TextDisabled("%s", tr("No matching files in this directory.", "目录中没有匹配的文件。"));
        std::string selected;
        for (const auto &file : files_) {
                ImGui::PushID(file.url.c_str());
                // Text bytes cannot introduce ImGui ID suffixes
                if (ImGui::MenuItem((file.name + "###remoteFile").c_str()))
                        selected = file.url;
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", file.url.c_str());
                ImGui::PopID();
        }
        return selected;
}
