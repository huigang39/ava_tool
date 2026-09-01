#include "core/jlink_port.hpp"
#include "core/remote_device.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include <chrono>
#include <cstdio>
#include <future>

void
JLinkPort::startDeviceSharing(const std::string &name, int port, const std::string &key, bool writes)
{
        rttStop();
        swoStop();
        ava_peer::Backend backend;
        backend.device = [this] {
                return ava_peer::Device{deviceName(), deviceEpoch_.load(), isConnected_.load() && isOpen_.load(), true, true};
        };
        backend.read = [this](uint64_t epoch, uint32_t address, uint32_t size, void *data) {
                auto target = targetEpoch_.load();
                return epoch == deviceEpoch_.load() && readMemAtEpoch(target, address, size, data);
        };
        backend.write = [this](uint64_t epoch, uint32_t address, uint32_t size, const void *data) {
                auto target = targetEpoch_.load();
                return epoch == deviceEpoch_.load() && writeMemAtEpoch(target, address, size, data);
        };
        backend.reset = [this](uint64_t epoch) {
                auto target = targetEpoch_.load();
                return epoch == deviceEpoch_.load() && resetTarget(target);
        };
        ava_peer::server().start(std::move(backend), name, uint16_t(port), key, writes);
}
void
JLinkPort::connectRemoteDevice(const std::string &host, int port, const std::string &key)
{
        auto route = lockRoute();
        targetEpoch_.fetch_add(1);
        hssStop();
        rttStop();
        swoStop();
        remoteSampling_ = false;
        ava_peer::client().connect(host, uint16_t(port), key);
        remoteSelected_ = true;
        targetEpoch_.fetch_add(1);
}
void
JLinkPort::selectLocalDevice()
{
        auto route = lockRoute();
        targetEpoch_.fetch_add(1);
        hssStop();
        ava_peer::client().close();
        remoteSelected_ = false;
        remoteSampling_ = false;
        targetEpoch_.fetch_add(1);
}
void
JLinkPort::drawRemoteDevices()
{
        if (!remoteWindowOpen_)
                return;
        ImGui::SetNextWindowSize(ImVec2(620, 490), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Remote devices###RemoteDevices", "远程设备###RemoteDevices"), &remoteWindowOpen_)) {
                ImGui::End();
                return;
        }
        using Result                               = RemoteUiResult;
        auto                              &pending = remoteJob_;
        static std::vector<ava_peer::Peer> peers;
        static std::string                 message, key;
        static char                        host[64] = "", pairing[40] = "", name[128] = "AvA Tool";
        static int                         port = 45991, sharePort = 45991;
        static bool                        allowWrites  = false;
        static bool                        messageError = false;
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result  = pending.get();
                message      = result.message;
                messageError = result.error;
                if (!result.peers.empty())
                        peers = std::move(result.peers);
        }
        const bool working = pending.valid();
        auto       job     = [&](auto operation) {
                pending = std::async(std::launch::async, [operation] {
                        try {
                                return operation();
                        } catch (const std::exception &e) {
                                return Result{e.what(), {}, true};
                        }
                });
        };
        ImGui::TextWrapped("%s",
                           tr("Share a local device, or use a device connected to another AvA Tool instance.",
                              "共享本机设备，或使用另一台 ava_tool 实例连接的设备。"));
        if (ImGui::BeginTabBar("PeerPages")) {
                if (ImGui::BeginTabItem(tr("Connect to a peer", "连接远端"))) {
                        ImGui::BeginDisabled(working);
                        if (ImGui::Button(tr("Discover LAN devices", "发现局域网设备"))) {
                                peers.clear();
                                job([] {
                                        auto found = ava_peer::discover();
                                        return Result{found.empty() ? "未发现设备，可手动输入 IP" : "已发现共享设备",
                                                      std::move(found)};
                                });
                        }
                        for (const auto &peer : peers) {
                                std::string label = peer.name + "  " + peer.host + ":" + std::to_string(peer.port);
                                if (ImGui::Selectable(label.c_str())) {
                                        snprintf(host, sizeof(host), "%s", peer.host.c_str());
                                        port = peer.port;
                                }
                        }
                        ImGui::InputText(tr("IP address", "IP 地址"), host, sizeof(host));
                        ImGui::InputInt(tr("Port", "端口"), &port);
                        ImGui::InputText(tr("Pairing key", "配对码"), pairing, sizeof(pairing), ImGuiInputTextFlags_Password);
                        if (ui::Button(tr("Connect", "连接"), ui::BtnStyle::Primary)) {
                                std::string address = host, code = pairing;
                                int         selectedPort = port;
                                job([this, address, code, selectedPort] {
                                        if (selectedPort < 1 || selectedPort > 65535)
                                                throw std::runtime_error("端口范围 1–65535");
                                        connectRemoteDevice(address, selectedPort, code);
                                        return Result{
                                            "远端已连接：可在现有变量和示波器中使用；采样为设备端轮询（最高 50 Hz）。", {}};
                                });
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Use local device", "切回本机")))
                                job([this] {
                                        selectLocalDevice();
                                        return Result{"已切回本机设备", {}};
                                });
                        if (usingRemote()) {
                                ImGui::Separator();
                                ImGui::TextUnformatted(ava_peer::client().connected()
                                                           ? tr("Remote connected", "远端已连接")
                                                           : tr("Remote disconnected", "远端已断开"));
                                bool controlled = ava_peer::client().ownsControl();
                                if (ImGui::Button(controlled ? tr("Release control", "释放控制权")
                                                             : tr("Acquire control", "取得控制权")))
                                        job([controlled] {
                                                ava_peer::client().claim(!controlled);
                                                return Result{controlled ? "已释放控制权"
                                                                         : "已取得控制权；本机用户和其他远端暂不能写入",
                                                              {}};
                                        });
                                ImGui::BeginDisabled(!ava_peer::client().connected() || !controlled);
                                if (ImGui::Button(tr("Reset remote target…", "复位远端 MCU…")))
                                        ImGui::OpenPopup("ResetRemoteTarget");
                                ImGui::EndDisabled();
                                if (ImGui::BeginPopup("ResetRemoteTarget")) {
                                        ImGui::TextUnformatted(tr("Reset the remote MCU? Reconnection will be required.",
                                                                  "复位远端 MCU？操作后需要重新连接。"));
                                        if (ImGui::Button(tr("Reset", "复位"))) {
                                                job([] {
                                                        if (!ava_peer::client().reset())
                                                                throw std::runtime_error(ava_peer::client().error());
                                                        return Result{"远端已复位，请重新连接", {}};
                                                });
                                                ImGui::CloseCurrentPopup();
                                        }
                                        ImGui::SameLine();
                                        if (ImGui::Button(tr("Cancel", "取消")))
                                                ImGui::CloseCurrentPopup();
                                        ImGui::EndPopup();
                                }
                                ImGui::TextWrapped(
                                    "%s",
                                    tr("Supported: variable reads/writes, timestamped sampling, reset. Flash, RTT, SWO and "
                                       "bitfield writes are unavailable.",
                                       "支持变量读写、带时间戳采样与复位。暂不提供烧录、RTT、SWO 和位字段写入。"));
                                ImGui::Text("%s: %llu",
                                            tr("Dropped buffered frames", "缓冲覆盖帧"),
                                            (unsigned long long)ava_peer::client().dropped());
                        }
                        ImGui::EndDisabled();
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Share this device", "共享本机"))) {
                        if (key.empty())
                                key = ava_peer::randomKey();
                        bool sharing = ava_peer::server().running();
                        ImGui::BeginDisabled(working || sharing);
                        ImGui::InputText(tr("Instance name", "实例名称"), name, sizeof(name));
                        ImGui::InputInt(tr("Listening port", "共享端口"), &sharePort);
                        ImGui::Checkbox(tr("Allow remote control requests", "允许远端申请写入控制权"), &allowWrites);
                        ImGui::EndDisabled();
                        ImGui::TextWrapped("%s: %s", tr("Pairing key", "配对码"), key.c_str());
                        if (ImGui::Button(tr("Copy key", "复制配对码")))
                                ImGui::SetClipboardText(key.c_str());
                        ImGui::SameLine();
                        ImGui::BeginDisabled(working || sharing);
                        if (ImGui::Button(tr("New key", "更换配对码")))
                                key = ava_peer::randomKey();
                        ImGui::EndDisabled();
                        ImGui::BeginDisabled(working);
                        if (ui::Button(sharing ? tr("Stop sharing", "停止共享") : tr("Start sharing", "开启共享"),
                                       sharing ? ui::BtnStyle::Danger : ui::BtnStyle::Primary)) {
                                std::string title = name, code = key;
                                int         chosenPort = sharePort;
                                bool        writes     = allowWrites;
                                job([this, sharing, title, code, chosenPort, writes] {
                                        if (sharing) {
                                                ava_peer::server().stop();
                                                return Result{"共享已关闭，连接与控制权已释放", {}};
                                        }
                                        if (chosenPort < 1 || chosenPort > 65535)
                                                throw std::runtime_error("端口范围 1–65535");
                                        if (!isOpen_ || !isConnected_)
                                                throw std::runtime_error("请先连接本机 J-Link，再开启共享");
                                        startDeviceSharing(title, chosenPort, code, writes);
                                        return Result{"共享已开启。对方可通过局域网发现或输入本机 IP 连接。", {}};
                                });
                        }
                        ImGui::EndDisabled();
                        if (sharing)
                                ImGui::Text("%s: %d   %s: %d",
                                            tr("Port", "端口"),
                                            ava_peer::server().port(),
                                            tr("Clients", "连接数"),
                                            ava_peer::server().viewers());
                        if (ava_peer::remoteOwnsWrites())
                                ImGui::TextUnformatted(tr("Remote controller holds write access", "远端正在持有写入控制权"));
                        ImGui::TextWrapped("%s",
                                           tr("Shares the local J-Link only. Remote connections are never re-exported.",
                                              "仅共享本机 J-Link，连接远端时仍可保留本机共享；不会转发远端设备。"));
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
        if (working)
                ImGui::TextUnformatted(tr("Working…", "处理中…"));
        if (!message.empty()) {
                if (messageError)
                        ui::ErrorTextWrapped("%s", message.c_str());
                else
                        ui::SuccessTextWrapped("%s", message.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("LAN  authenticated messages  data not encrypted", "局域网  消息身份校验  数据未加密"));
        ImGui::End();
}
