#include "core/jlink_port.hpp"
#include "gui/choice_control.hpp"
#include "gui/flash_tool.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "platform/native_dlg.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

void
FlashTool::startReadFlash(bool reinterpretOnly, const std::string &file)
{
        if (reading_ || flashing_ || readFuture_.valid())
                return;
        try {
                size_t            used        = 0;
                const std::string addressText = readAddress_;
                if (addressText.empty() || addressText.front() == '-')
                        throw std::invalid_argument("Invalid start address.");
                const auto parsed = std::stoull(addressText, &used, 16);
                if (used != addressText.size() || parsed > UINT32_MAX)
                        throw std::invalid_argument("Invalid start address.");
                const auto address = static_cast<uint32_t>(parsed);
                const auto type    = reinterpretOnly ? static_cast<flasharray::Type>(readType_) : flasharray::Type::U8;
                const auto n       = flasharray::width(type);
                if (readBytes_ <= 0 || readBytes_ > 16 * 1024 * 1024 || uint64_t(address) + readBytes_ > 0x100000000ULL)
                        throw std::invalid_argument("Invalid read length or address range (maximum 16 MiB).");
                if (!reinterpretOnly && file.empty() && size_t(readBytes_) % n)
                        throw std::invalid_argument("Byte length must be a multiple of the selected type size.");
                if (!std::isfinite(readStep_) || readStep_ <= 0)
                        throw std::invalid_argument("Sample interval must be finite and positive.");
                const auto prev = readSnapshot_;
                if (reinterpretOnly && !prev)
                        return;
                const auto        count = readBytes_;
                const auto        big   = readBigEndian_;
                const auto        step  = readStep_;
                const std::string name  = arrayName_[0] ? arrayName_ : "FlashArray";
                cancelRead_             = false;
                readProgress_           = 0;
                reading_                = true;
                readError_.clear();
                readFuture_ =
                    std::async(std::launch::async, [this, address, type, count, big, step, name, prev, reinterpretOnly, file] {
                            std::vector<uint8_t> bytes;
                            uint32_t             sourceAddress = address;
                            if (reinterpretOnly) {
                                    bytes         = prev->bytes;
                                    sourceAddress = prev->address;
                            } else if (!file.empty()) {
                                    std::ifstream in(std::filesystem::u8path(file), std::ios::binary | std::ios::ate);
                                    const auto    size = in.tellg();
                                    if (!in || size <= 0 || size > 16 * 1024 * 1024)
                                            throw std::runtime_error("BIN size must be between 1 byte and 16 MiB.");
                                    bytes.resize(static_cast<size_t>(size));
                                    in.seekg(0);
                                    if (!in.read(reinterpret_cast<char *>(bytes.data()), size))
                                            throw std::runtime_error("Cannot read BIN file.");
                            } else {
                                    struct Pause {
                                            bool old = g_jlinkSamplingPaused.exchange(true);
                                            ~Pause()
                                            {
                                                    g_jlinkSamplingPaused.store(old);
                                                    JLinkPort::instance().reqRestart();
                                            }
                                    } pause;
                                    JLinkPort::instance().hssStop();
                                    std::string error;
                                    if (!JLinkPort::instance().readFlash(
                                            address, count, bytes, error, cancelRead_, [this](float p) { readProgress_ = p; }))
                                            throw std::runtime_error(error);
                            }
                            if (cancelRead_)
                                    throw std::runtime_error("Read cancelled.");
                            return flasharray::decode(std::move(bytes), sourceAddress, type, big, step, name);
                    });
        } catch (const std::exception &e) {
                readError_ = e.what();
                reading_   = false;
        }
}

void
FlashTool::drawReadFlash()
{
        ImGui::BeginDisabled(reading_.load() || flashing_.load());
        if (ImGui::BeginTable("readControls", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tr("Start address (hex)", "起始地址（十六进制）"));
                ImGui::SetNextItemWidth(-1);
                modified_ |= ImGui::InputText("##readAddress", readAddress_, sizeof(readAddress_));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tr("Byte length", "读取字节数"));
                ImGui::SetNextItemWidth(-1);
                modified_ |= ImGui::InputInt("##readBytes", &readBytes_);
                ImGui::EndTable();
        }
        ImGui::BeginDisabled(!JLinkPort::instance().isConnected() || JLinkPort::instance().isBusy());
        if (ImGui::Button(tr("Read Flash", "读取 Flash")))
                startReadFlash();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Open BIN...", "打开 BIN…"))) {
                const auto path = nativeDlgOpen("Open Flash BIN", {{"Binary", {"bin"}}});
                if (!path.empty())
                        startReadFlash(false, path);
        }
        ImGui::BeginDisabled(!readSnapshot_);
        ImGui::SameLine();
        if (ImGui::Button(tr("Reinterpret", "重新解析")))
                startReadFlash(true);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (reading_) {
                ImGui::ProgressBar(readProgress_.load(), ImVec2(-1, 0));
                if (ImGui::Button(tr("Cancel", "取消")))
                        cancelRead_ = true;
        }
        if (!readError_.empty())
                ui::ErrorTextWrapped("%s", readError_.c_str());
        if (!readSnapshot_)
                return;
        const auto &s = *readSnapshot_;
        ImGui::Separator();
        ImGui::Text("0x%08X | %zu bytes | %s | %zu",
                    s.address,
                    s.bytes.size(),
                    flasharray::names[static_cast<int>(s.type)],
                    s.values.size());
        if (ImGui::Button(tr("Save BIN...", "保存 BIN…"))) {
                const auto path = nativeDlgSave("Save Flash BIN", {{"Binary", {"bin"}}}, "flash.bin");
                if (!path.empty()) {
                        std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
                        out.write(reinterpret_cast<const char *>(s.bytes.data()), s.bytes.size());
                        out.close();
                        if (!out)
                                readError_ = "Cannot save BIN file.";
                }
        }
        hexSelection_ = std::min(hexSelection_, s.bytes.size() - 1);
        ImGui::SetNextItemWidth(150);
        ImGui::InputText(tr("Jump address", "跳转地址"), hexJump_, sizeof(hexJump_), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        bool jump = ImGui::Button(tr("Go", "跳转"));
        if (jump) {
                try {
                        size_t     used    = 0;
                        const auto address = std::stoull(hexJump_, &used, 16);
                        if (used != std::string(hexJump_).size() || address < s.address ||
                            address - s.address >= s.bytes.size())
                                throw std::out_of_range("Address outside the loaded range.");
                        hexSelection_ = size_t(address - s.address);
                        readError_.clear();
                } catch (const std::exception &e) {
                        readError_ = e.what();
                        jump       = false;
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Copy byte", "复制字节"))) {
                char text[8];
                snprintf(text, sizeof(text), "%02X", s.bytes[hexSelection_]);
                ImGui::SetClipboardText(text);
        }
        const float height = std::max(220.0f, ImGui::GetContentRegionAvail().y - 145.0f);
        if (ImGui::BeginTable("hexWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthStretch, 2.5f);
                ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tr("Hex viewer", "十六进制视图"));
                if (ImGui::BeginChild("hexBytes", ImVec2(0, height), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                        const float cell         = ImGui::CalcTextSize("FF").x + 9;
                        const float addressWidth = ImGui::CalcTextSize("00000000").x + 16;
                        ImGui::TextUnformatted("Address");
                        for (int col = 0; col < 16; ++col) {
                                ImGui::SameLine(addressWidth + col * cell);
                                ImGui::Text("%02X", col);
                        }
                        ImGui::SameLine(addressWidth + 16 * cell + 12);
                        ImGui::TextUnformatted("ASCII");
                        ImGui::Separator();
                        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                        if (jump)
                                ImGui::SetScrollY((hexSelection_ / 16) * rowHeight);
                        ImGuiListClipper clipper;
                        clipper.Begin(int((s.bytes.size() + 15) / 16), rowHeight);
                        while (clipper.Step()) {
                                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                        ImGui::Text("%08X", s.address + uint32_t(row * 16));
                                        for (int col = 0; col < 16; ++col) {
                                                const size_t offset = size_t(row) * 16 + col;
                                                if (offset >= s.bytes.size())
                                                        break;
                                                ImGui::SameLine(addressWidth + col * cell);
                                                ImGui::PushID(int(offset));
                                                char label[4];
                                                snprintf(label, sizeof(label), "%02X", s.bytes[offset]);
                                                if (ImGui::Selectable(label,
                                                                      hexSelection_ == offset,
                                                                      0,
                                                                      ImVec2(cell - 2, ImGui::GetTextLineHeight())))
                                                        hexSelection_ = offset;
                                                ImGui::PopID();
                                        }
                                        for (int col = 0; col < 16; ++col) {
                                                const size_t offset = size_t(row) * 16 + col;
                                                if (offset >= s.bytes.size())
                                                        break;
                                                ImGui::SameLine(addressWidth + 16 * cell + 12 +
                                                                col * ImGui::CalcTextSize("M").x);
                                                ImGui::PushID(int(offset));
                                                ImGui::PushID("ascii");
                                                const auto byte     = s.bytes[offset];
                                                char       label[2] = {char(byte >= 32 && byte <= 126 ? byte : '.'), 0};
                                                if (ImGui::Selectable(
                                                        label,
                                                        hexSelection_ == offset,
                                                        0,
                                                        ImVec2(ImGui::CalcTextSize("M").x, ImGui::GetTextLineHeight())))
                                                        hexSelection_ = offset;
                                                ImGui::PopID();
                                                ImGui::PopID();
                                        }
                                }
                        }
                }
                ImGui::EndChild();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tr("Data Inspector", "数据解析"));
                if (ImGui::BeginChild("hexInspector", ImVec2(0, height), true)) {
                        ImGui::Text("0x%08X | +0x%zX", s.address + uint32_t(hexSelection_), hexSelection_);
                        modified_ |= ImGui::Checkbox(tr("Big endian", "大端字节序"), &readBigEndian_);
                        if (ImGui::BeginTable("inspectorValues", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                                ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 85);
                                ImGui::TableSetupColumn(tr("Value", "数值"));
                                ImGui::TableHeadersRow();
                                for (int type = 0; type < 11; ++type) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::TextUnformatted(flasharray::names[type]);
                                        ImGui::TableNextColumn();
                                        const auto kind = static_cast<flasharray::Type>(type);
                                        if (flasharray::width(kind) <= s.bytes.size() - hexSelection_)
                                                ImGui::TextUnformatted(
                                                    flasharray::format(s.bytes.data() + hexSelection_, kind, readBigEndian_)
                                                        .c_str());
                                        else
                                                ImGui::TextDisabled("--");
                                }
                                ImGui::EndTable();
                        }
                        char binary[9]{};
                        for (int bit = 0; bit < 8; ++bit)
                                binary[bit] = (s.bytes[hexSelection_] & (1 << (7 - bit))) ? '1' : '0';
                        ImGui::Text("Binary: %s", binary);
                        ImGui::Text("ASCII: %c",
                                    s.bytes[hexSelection_] >= 32 && s.bytes[hexSelection_] <= 126 ? s.bytes[hexSelection_]
                                                                                                  : '.');
                }
                ImGui::EndChild();
                ImGui::EndTable();
        }
        ImGui::TextDisabled("%s",
                            tr("Select a byte to inspect. Read-only snapshot.", "点击字节查看解析结果；当前视图为只读快照。"));
        if (ImGui::CollapsingHeader(tr("Array data", "数组数据"))) {
                ImGui::BeginDisabled(reading_.load() || flashing_.load());
                modified_ |= ui::Choice(tr("Array type", "数组类型"), &readType_, flasharray::names, 11);
                modified_ |= ImGui::InputDouble(tr("X interval", "横轴间隔"), &readStep_, 0, 0, "%.9g");
                modified_ |= ImGui::InputText(tr("Array name", "数组名称"), arrayName_, sizeof(arrayName_));
                if (ImGui::Button(tr("Apply array format", "应用数组格式")))
                        startReadFlash(true);
                ImGui::EndDisabled();
                ImGui::Button(tr("Drag array to Variables or Monitor Scope", "将数组拖到变量管理器或监视器 Scope"),
                              ImVec2(-1, 32));
                if (ImGui::BeginDragDropSource()) {
                        auto &drag = flasharray::drag();
                        if (drag.snapshot != readSnapshot_) {
                                ++drag.id;
                                drag.snapshot = readSnapshot_;
                        }
                        ImGui::SetDragDropPayload("FLASH_ARRAY", &drag.id, sizeof(drag.id));
                        ImGui::Text("%s (%zu)", s.name.c_str(), s.values.size());
                        ImGui::EndDragDropSource();
                }
                ImGui::TextDisabled("%s",
                                    tr("Preview retains exact integer values; curves use double precision.",
                                       "预览保留整数原值；曲线使用双精度浮点数。"));
                if (ImGui::BeginTable("flashArrayPreview",
                                      3,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                      ImVec2(0, 240))) {
                        ImGui::TableSetupColumn(tr("Index", "索引"));
                        ImGui::TableSetupColumn(tr("Address", "地址"));
                        ImGui::TableSetupColumn(tr("Value", "数值"));
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(s.values.size()));
                        const auto size = flasharray::width(s.type);
                        while (clipper.Step())
                                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::Text("%d", i);
                                        ImGui::TableNextColumn();
                                        ImGui::Text("0x%08X", s.address + uint32_t(i * size));
                                        ImGui::TableNextColumn();
                                        ImGui::TextUnformatted(
                                            flasharray::format(s.bytes.data() + i * size, s.type, s.bigEndian).c_str());
                                }
                        ImGui::EndTable();
                }
        }
}
