#include "gui/flash_tool.hpp"
#include "cJSON.h"
#include "core/remote_files.hpp"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "imgui.h"

#include "app_log.hpp"
#include "core/flash_erase.hpp"
#include "core/http_win.hpp"
#include "core/jlink_port.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/monitor.hpp"
#include "gui/ui_theme.hpp"
#include "platform/native_dlg.hpp"

namespace
{
std::string
lowerExtension(const std::string &path)
{
        std::string ext = std::filesystem::u8path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
}

} // namespace

void
FlashTool::setFlashResult(const bool ok, const std::string &message)
{
        std::lock_guard lock(statusMtx_);
        flashOk_     = ok;
        flashStatus_ = message;
}

void
FlashTool::setDownloadResult(const bool ok, const std::string &message)
{
        std::lock_guard lock(statusMtx_);
        downloadOk_     = ok;
        downloadStatus_ = message;
}

void
FlashTool::selectFirmware()
{
        const std::string path =
            nativeDlgOpen("Select firmware image", {{"Firmware Images", {"axf", "elf", "hex", "bin"}}, {"All Files", {"*"}}});
        if (path.empty())
                return;
        snprintf(firmwarePath_, sizeof(firmwarePath_), "%s", path.c_str());
        modified_ = true;
        setFlashResult(false, {});
        setDownloadResult(false, {});
}

void
FlashTool::startFlash()
{
        if (reading_ || flashing_.exchange(true))
                return;
        if (flashThread_.joinable())
                flashThread_.join();

        const std::string path    = firmwarePath_;
        const bool        erase   = eraseChip_;
        const bool        reset   = resetAfter_;
        u32               address = 0;
        try {
                const unsigned long long parsed = std::stoull(binAddress_, nullptr, 16);
                if (parsed > std::numeric_limits<u32>::max())
                        throw std::out_of_range("address");
                address = static_cast<u32>(parsed);
        } catch (...) {
                setFlashResult(false, tr("Invalid BIN base address.", "BIN 基地址无效。"));
                flashing_.store(false);
                return;
        }

        flashProgress_.store(0.0f);
        setFlashResult(false, tr("Preparing J-Link flash...", "正在准备 J-Link 烧录……"));
        flashThread_ = std::thread([this, path, address, erase, reset]() {
                const bool prevPause = g_jlinkSamplingPaused.exchange(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                JLinkPort::instance().hssStop();
                std::string error;
                const bool  ok = JLinkPort::instance().programFile(path, address, erase, reset, error, [this](float p) {
                        flashProgress_.store(std::clamp(p, 0.0f, 1.0f), std::memory_order_release);
                });
                g_jlinkSamplingPaused.store(prevPause);
                setFlashResult(ok,
                               ok ? tr("Programming and verification completed.", "烧录和校验完成。")
                                  : (error.empty() ? tr("Programming failed.", "烧录失败。") : error));
                flashing_.store(false, std::memory_order_release);
        });
}

void
FlashTool::startEraseFlash()
{
        if (reading_ || JLinkPort::instance().isBusy() || flashing_.exchange(true))
                return;
        if (flashThread_.joinable())
                flashThread_.join();
        const auto device = eraseDevice_;
        const auto epoch  = eraseEpoch_;
        const bool entire = eraseMode_ == 1;
        const auto mask   = eraseSectors_;
        const int  speed  = eraseSpeed_;
        flashProgress_    = 0;
        setFlashResult(false, tr("Erasing Flash...", "正在擦除 Flash……"));
        flashThread_ = std::thread([this, device, epoch, entire, mask, speed] {
                struct Pause {
                        bool previous = g_jlinkSamplingPaused.exchange(true);
                        ~Pause()
                        {
                                g_jlinkSamplingPaused = previous;
                                JLinkPort::instance().reqRestart();
                        }
                } pause;
                std::string       error, logPath;
                const bool        ok = JLinkPort::instance().eraseFlash(device, epoch, entire, mask, speed, error, logPath);
                std::ifstream     file(std::filesystem::u8path(logPath), std::ios::binary);
                const std::string log((std::istreambuf_iterator<char>(file)), {});
                {
                        std::lock_guard lock(statusMtx_);
                        eraseLog_ = logPath.empty() ? error : logPath + "\n" + log;
                }
                flashProgress_ = ok ? 1.0f : 0.0f;
                setFlashResult(ok,
                               ok ? tr("Flash erased. Target left halted; reconnect J-Link before the next operation.",
                                       "Flash 擦除完成。目标保持暂停；下一步操作前请重新连接 J-Link。")
                                  : error);
                flashing_ = false;
        });
}

void
FlashTool::drawEraseFlash()
{
        auto      &port   = JLinkPort::instance();
        const auto device = port.deviceName();
        const auto epoch  = port.targetEpoch();
        if (eraseDevice_ != device || eraseEpoch_ != epoch) {
                eraseDevice_  = device;
                eraseEpoch_   = epoch;
                eraseSectors_ = 0;
        }
        if (!ImGui::CollapsingHeader(tr("Erase Flash", "擦除 Flash"), ImGuiTreeNodeFlags_DefaultOpen))
                return;
        const auto sectors = flasherase::sectors(device);
        const bool busy    = reading_ || flashing_ || port.isBusy();
        ImGui::BeginDisabled(busy);
        ImGui::Text("%s: %s", tr("Target", "目标芯片"), device.empty() ? "--" : device.c_str());
        const char *eraseChoices[] = {tr("Selected sectors", "指定扇区"), tr("Entire chip", "全片擦除")};
        ui::Choice("##erase_mode", &eraseMode_, eraseChoices, 2);
        ImGui::SetNextItemWidth(140);
        ImGui::InputInt(tr("Erase SWD speed (kHz)", "擦除 SWD 速度（kHz）"), &eraseSpeed_, 100, 1000);
        eraseSpeed_ = std::clamp(eraseSpeed_, 5, 50000);
        if (eraseMode_ == 0) {
                if (sectors.empty()) {
                        ImGui::TextWrapped("%s", tr("No sector map", "无扇区信息"));
                } else {

                        if (ImGui::Button(tr("Select all", "全选")))
                                eraseSectors_ = 0xffff;
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Clear selection", "清空选择")))
                                eraseSectors_ = 0;
                        if (ImGui::BeginTable("eraseSectors", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                                for (unsigned i = 0; i < 8; ++i) {
                                        ImGui::TableNextRow();
                                        for (unsigned bank = 0; bank < 2; ++bank) {
                                                const auto  slot   = bank * 8 + i;
                                                const auto &sector = sectors[slot];
                                                ImGui::TableNextColumn();
                                                ImGui::PushID(int(slot));
                                                bool selected = (eraseSectors_ & (1u << slot)) != 0;
                                                char label[96];
                                                snprintf(label,
                                                         sizeof(label),
                                                         "Bank %u / S%u  0x%08X - 0x%08X",
                                                         sector.bank,
                                                         sector.index,
                                                         sector.address,
                                                         sector.address + sector.size - 1);
                                                if (ImGui::Checkbox(label, &selected)) {
                                                        if (selected)
                                                                eraseSectors_ |= 1u << slot;
                                                        else
                                                                eraseSectors_ &= ~(1u << slot);
                                                }
                                                ImGui::PopID();
                                        }
                                }
                                ImGui::EndTable();
                        }
                }
        } else if (!sectors.empty()) {
                ImGui::TextUnformatted("0x08000000 - 0x081FFFFF  (2 MB)");
        }
        const bool valid = port.isConnected() && (eraseMode_ == 1 || (!sectors.empty() && eraseSectors_));
        ImGui::BeginDisabled(!valid);
        if (ui::Button(tr("ERASE FLASH...", "擦除 Flash…"), ui::BtnStyle::Danger, ImVec2(180, 0)))
                ImGui::OpenPopup(tr("Confirm Flash erase###confirmErase", "确认擦除 Flash###confirmErase"));
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal(tr("Confirm Flash erase###confirmErase", "确认擦除 Flash###confirmErase"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", device.c_str());
                ImGui::TextUnformatted(eraseMode_ == 1 ? tr("Erase the entire chip", "将擦除整个芯片 Flash")
                                                       : tr("Erase these sectors", "将擦除以下扇区"));
                if (eraseMode_ == 1 && !sectors.empty())
                        ImGui::TextUnformatted("0x08000000 - 0x081FFFFF");
                for (size_t i = 0; eraseMode_ == 0 && i < sectors.size(); ++i)
                        if (eraseSectors_ & (1u << i))
                                ImGui::Text("Bank %u S%u: 0x%08X - 0x%08X",
                                            sectors[i].bank,
                                            sectors[i].index,
                                            sectors[i].address,
                                            sectors[i].address + sectors[i].size - 1);
                ImGui::TextUnformatted(tr("Data in this range will be permanently erased.", "上述范围内的数据将被永久擦除。"));
                ImGui::BeginDisabled(busy || !valid);
                if (ImGui::Button(tr("Erase now", "确认擦除"))) {
                        startEraseFlash();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
        std::string log;
        {
                std::lock_guard lock(statusMtx_);
                log = eraseLog_;
        }
        if (!log.empty() && ImGui::TreeNode(tr("Erase log", "擦除日志"))) {
                ui::CopyLogButton(log);
                ui::LogView("eraseLog", log, ImVec2(0, 200));
                ImGui::TreePop();
        }
}

void
FlashTool::refreshRemoteFiles()
{
        if (listing_.exchange(true))
                return;
        if (listThread_.joinable())
                listThread_.join();

        std::string url = remote::settings().firmwareUrl;
        while (!url.empty() && (url.back() == '/' || url.back() == ' '))
                url.pop_back();
        const auto config = remote::settings();

        {
                std::lock_guard lock(statusMtx_);
                listStatus_ = tr("Loading firmware list...", "正在读取固件列表……");
        }
        const auto generation = remoteGeneration_.load();
        listThread_           = std::thread([this, url, config, generation]() {
                auto finish = [this, generation](const std::string &status, std::vector<RemoteFirmware> files = {}) {
                        {
                                std::lock_guard lock(statusMtx_);
                                if (generation == remoteGeneration_.load()) {
                                        listStatus_  = status;
                                        remoteFiles_ = std::move(files);
                                }
                        }
                        listing_.store(false, std::memory_order_release);
                };
                if (url.empty()) {
                        finish(tr("Firmware URL is required.", "固件地址不能为空。"));
                        return;
                }

                std::vector<RemoteFirmware> files;
                try {
                        for (const auto &file : remote::list(url, config, {".axf", ".elf", ".hex", ".bin"}))
                                files.push_back({file.name, file.url});
                } catch (const std::exception &e) {
                        finish(e.what());
                        return;
                }
                std::sort(files.begin(), files.end(), [](const RemoteFirmware &a, const RemoteFirmware &b) {
                        return a.name < b.name;
                });
                const size_t count = files.size();
                LOG_I("Firmware listing: %s -> %d file(s)", url.c_str(), (int)count);
                finish(count == 0 ? tr("No AXF, ELF, HEX or BIN files found.", "未找到 AXF、ELF、HEX 或 BIN 文件。")
                                  : std::to_string(count) + tr(" firmware file(s) found.", " 个固件文件。"),
                       std::move(files));
        });
}

void
FlashTool::startDownload(const RemoteFirmware &file)
{
        if (downloading_.exchange(true))
                return;
        if (downloadThread_.joinable())
                downloadThread_.join();

        const std::string url  = file.path;
        const std::string name = file.name;

        downloadProgress_.store(0.0f);
        setDownloadResult(false, tr("Downloading firmware...", "正在下载固件……"));
        const auto generation = remoteGeneration_.load();
        downloadThread_       = std::thread([this, url, name, generation]() {
                auto finish = [this, generation](bool ok, const std::string &text) {
                        if (generation == remoteGeneration_.load())
                                setDownloadResult(ok, text);
                        downloading_.store(false, std::memory_order_release);
                };
                if (!remote::isUrl(url)) {
                        finish(false, tr("Firmware URL is invalid.", "固件地址无效。"));
                        return;
                }

                std::error_code       ec;
                std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "ava_tool_firmware";
                if (ec) {
                        finish(false, tr("Cannot locate a temporary directory.", "无法定位临时目录。"));
                        return;
                }
                std::filesystem::create_directories(dir, ec);
                const std::filesystem::path dest = dir / std::filesystem::u8path(name);

                std::string error;
                const bool  ok = http::download(
                    url,
                    dest.string(),
                    [this](uint64_t received, uint64_t total) {
                            downloadProgress_.store(total > 0 ? std::clamp((float)received / (float)total, 0.0f, 1.0f) : 0.0f,
                                                    std::memory_order_release);
                    },
                    &error);
                if (!ok) {
                        finish(false, error.empty() ? tr("Firmware download failed.", "固件下载失败。") : error);
                        return;
                }

                const std::string localPath = dest.string();
                {
                        std::lock_guard lock(statusMtx_);
                        if (generation == remoteGeneration_.load())
                                pendingLocalPath_ = localPath;
                }
                if (generation == remoteGeneration_.load())
                        setFlashResult(false, {});
                LOG_I("Firmware downloaded: %s -> %s", url.c_str(), localPath.c_str());
                finish(true, tr("Downloaded and selected for programming.", "已下载并选为待烧录固件。"));
        });
}

void
FlashTool::draw()
{
        if (readFuture_.valid() && readFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                        auto result = readFuture_.get();
                        if (!cancelRead_)
                                readSnapshot_ = std::move(result);
                } catch (const std::exception &e) {
                        if (!cancelRead_)
                                readError_ = e.what();
                }
                reading_ = false;
        }
        if (!(remoteConfig_ == remote::settings()))
                resetRemoteState();
        if (!open_)
                return;
        if (remoteRefreshPending_ && !listing_.load()) {
                remoteRefreshPending_ = false;
                refreshRemoteFiles();
        }
        // Keep the previous tool's docking/geometry when upgrading its visible name.
        const auto renamedId = ImHashStr("J-Flash###JFlash");
        if (!ImGui::FindWindowSettingsByID(renamedId)) {
                if (const auto *legacy = ImGui::FindWindowSettingsByID(ImHashStr("JFlash"))) {
                        const auto previous = *legacy; // Creating an entry may reallocate the settings buffer.
                        auto      *renamed  = ImGui::CreateNewWindowSettings("J-Flash###JFlash");
                        *renamed            = previous;
                        renamed->ID         = renamedId;
                }
        }
        ImGui::SetNextWindowSize(ImVec2(1080, 760), ImGuiCond_FirstUseEver);
        if (!dock_close::begin("J-Flash###JFlash", &open_)) {
                ImGui::End();
                return;
        }

        if (ImGui::BeginTabBar("flashToolTabs")) {
                if (ImGui::BeginTabItem(tr("Program Flash", "固件烧录"))) {
                        // Adopt a finished download here rather than in the worker: firmwarePath_
                        // backs the InputText below and belongs to the UI thread
                        {
                                std::lock_guard lock(statusMtx_);
                                if (!pendingLocalPath_.empty()) {
                                        snprintf(firmwarePath_, sizeof(firmwarePath_), "%s", pendingLocalPath_.c_str());
                                        modified_ = true;
                                        pendingLocalPath_.clear();
                                }
                        }

                        ImGui::TextUnformatted(tr("Firmware image", "固件文件"));
                        ImGui::SetNextItemWidth(-90.0f);
                        modified_ |= ImGui::InputText("##flashFile", firmwarePath_, sizeof(firmwarePath_));
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Browse...", "浏览……")))
                                selectFirmware();
                        ImGui::TextDisabled("AXF / ELF / HEX / BIN");

                        const std::string extension = lowerExtension(firmwarePath_);
                        if (extension == ".bin") {
                                ImGui::SetNextItemWidth(180.0f);
                                modified_ |= ImGui::InputText(
                                    tr("BIN base address (hex)", "BIN 基地址（十六进制）"), binAddress_, sizeof(binAddress_));
                        } else if (extension == ".axf" || extension == ".elf" || extension == ".hex") {
                                ImGui::TextDisabled("%s",
                                                    tr("The image supplies target addresses.", "目标地址由固件文件提供。"));
                        }
                        modified_ |= ImGui::Checkbox(tr("Erase entire chip first", "烧录前全片擦除"), &eraseChip_);
                        ImGui::SameLine();
                        modified_ |= ImGui::Checkbox(tr("Reset and run after programming", "烧录后复位并运行"), &resetAfter_);

                        const bool canFlash = !reading_.load() && !flashing_.load() && !JLinkPort::instance().isBusy() &&
                                              firmwarePath_[0] != '\0' && JLinkPort::instance().isConnected();
                        ImGui::BeginDisabled(!canFlash);
                        if (ui::Button(tr("PROGRAM & VERIFY", "烧录并校验"), ui::BtnStyle::Success, ImVec2(180, 0)))
                                startFlash();
                        ImGui::EndDisabled();
                        if (!JLinkPort::instance().isConnected()) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", tr("Connect J-Link first.", "请先连接 J-Link。"));
                        }
                        if (flashing_.load())
                                ImGui::ProgressBar(flashProgress_.load(), ImVec2(-1, 0));
                        drawEraseFlash();

                        std::string flashStatus;
                        bool        flashOk = false;
                        std::string downloadStatus;
                        bool        downloadOk = false;
                        {
                                std::lock_guard lock(statusMtx_);
                                flashStatus    = flashStatus_;
                                flashOk        = flashOk_;
                                downloadStatus = downloadStatus_;
                                downloadOk     = downloadOk_;
                        }
                        if (!flashStatus.empty())
                                ui::TextColored(flashOk ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f) : ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                                "%s",
                                                flashStatus.c_str());

                        ImGui::SeparatorText(tr("Download from firmware server", "从固件服务器下载"));
                        ImGui::TextWrapped("%s", remote::settings().firmwareUrl.c_str());
                        ImGui::TextDisabled("%s",
                                            tr("Configure the directory address in Settings > Server > Remote servers.",
                                               "目录地址请在“设置 → 服务器 → 远程服务器”中配置。"));

                        ImGui::BeginDisabled(listing_.load());
                        if (ImGui::Button(tr("Refresh firmware list", "刷新固件列表")))
                                refreshRemoteFiles();
                        ImGui::EndDisabled();
                        if (listing_.load()) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", tr("Loading...", "正在读取……"));
                        }
                        std::vector<RemoteFirmware> remoteFiles;
                        std::string                 listStatus;
                        {
                                std::lock_guard lock(statusMtx_);
                                remoteFiles = remoteFiles_;
                                listStatus  = listStatus_;
                        }
                        if (!listStatus.empty())
                                ImGui::TextDisabled("%s", listStatus.c_str());
                        if (!remoteFiles.empty()) {
                                if (ImGui::BeginChild("##remoteFirmwareFiles", ImVec2(0, 140), ImGuiChildFlags_Borders)) {
                                        for (const RemoteFirmware &file : remoteFiles) {
                                                char label[768]{};
                                                snprintf(label, sizeof(label), "%s##%s", file.name.c_str(), file.path.c_str());
                                                // Double-click (or the button below) downloads; a single
                                                // click only selects, so a stray click cannot start a
                                                // transfer
                                                if (ImGui::Selectable(label, file.path == selectedRemotePath_))
                                                        selectedRemotePath_ = file.path;
                                                if (ImGui::IsItemHovered()) {
                                                        ImGui::SetTooltip("%s", file.path.c_str());
                                                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                                                            !downloading_.load())
                                                                startDownload(file);
                                                }
                                        }
                                }
                                ImGui::EndChild();
                        }

                        const auto selected =
                            std::find_if(remoteFiles.begin(), remoteFiles.end(), [this](const RemoteFirmware &f) {
                                    return f.path == selectedRemotePath_;
                            });
                        const bool canDownload = !downloading_.load() && selected != remoteFiles.end();
                        ImGui::BeginDisabled(!canDownload);
                        if (ui::Button(tr("DOWNLOAD & SELECT", "下载并选用"), ui::BtnStyle::Primary, ImVec2(180, 0)))
                                startDownload(*selected);
                        ImGui::EndDisabled();
                        if (downloading_.load())
                                ImGui::ProgressBar(downloadProgress_.load(), ImVec2(-1, 0));
                        if (!downloadStatus.empty())
                                ui::TextColored(downloadOk ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f) : ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                                "%s",
                                                downloadStatus.c_str());
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Flash Read", "Flash 读取"))) {
                        drawReadFlash();
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
        ImGui::End();
}
