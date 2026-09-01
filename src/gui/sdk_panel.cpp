#include "gui/sdk_panel.hpp"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/flow_toolbar.hpp"
#include "gui/hover_panel.hpp"
#include "gui/search_results.hpp"
#include "gui/sequence_editor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cJSON.h"

#include "app_log.hpp"
#include "core/export_enum.hpp"
#include "core/session_time.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/monitor_types.hpp"
#include "gui/table_island.hpp"
#include "gui/table_text.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/native_dlg.hpp"
#include "timeops.h"

static void
showPathTooltipIfHovered(const char *path)
{
        if (path && path[0] && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path);
}

static std::string
sdkLower(std::string val)
{
        std::transform(
            val.begin(), val.end(), val.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return val;
}

static bool
sdkEndsWith(const std::string &name, const char *suffix)
{
        const std::string lower = sdkLower(name);
        const std::string tail  = suffix;
        return lower.size() >= tail.size() && lower.compare(lower.size() - tail.size(), tail.size(), tail) == 0;
}

static bool
sdkReadName(const std::string &name)
{
        const std::string n = sdkLower(name);
        return n.find("get") != std::string::npos || n.find("read") != std::string::npos ||
               n.find("recv") != std::string::npos || n.find("fetch") != std::string::npos ||
               n.find("pull") != std::string::npos;
}

static bool
sdkWriteName(const std::string &name)
{
        const std::string n = sdkLower(name);
        return n.find("set") != std::string::npos || n.find("write") != std::string::npos ||
               n.find("send") != std::string::npos || n.find("apply") != std::string::npos ||
               n.find("push") != std::string::npos;
}

/**
 * @brief 数据表字段"更新了就闪一下"用的时间基（秒，单调递增）
 */
static double
sdkMonoSeconds()
{
        return static_cast<double>(get_mono_ts_us()) / 1e6;
}

// 数值变化后的高亮持续时间；期间背景按余弦闪烁几下再淡出
static constexpr double kSdkFieldFlashSeconds = 0.7;

// 用户刚编辑过的字段先别闪，免得打字时背景一直在跳
static constexpr double kSdkFieldEditHoldSeconds = 0.6;

static const char *
sdkFieldTypeLabel(CType type)
{
        switch (type) {
                case CType::I8:
                        return "i8";
                case CType::U8:
                        return "u8";
                case CType::I16:
                        return "i16";
                case CType::U16:
                        return "u16";
                case CType::I32:
                        return "i32";
                case CType::U32:
                        return "u32";
                case CType::I64:
                        return "i64";
                case CType::U64:
                        return "u64";
                case CType::F32:
                        return "f32";
                case CType::F64:
                        return "f64";
                default:
                        return ctypeLabel(type);
        }
}

static void
sdkArmFieldWrite(const CStructDecl &table, std::vector<uint8_t> &bytes, const CFieldDecl &field)
{
        uint32_t          mode = field.name == "set_pvct.pos"   ? 3
                                 : field.name == "set_pvct.vel" ? 2
                                 : field.name == "set_pvct.cur" ? 1
                                 : field.name == "set_pvct.tor" ? 4
                                                                : 0;
        const auto        dot  = field.name.find('.');
        const std::string gate = dot == std::string::npos ? "" : "write_" + field.name.substr(0, dot);
        for (const auto &candidate : table.fields) {
                if (candidate.isArray || candidate.offset + candidate.size > bytes.size())
                        continue;
                if (!gate.empty() && candidate.name == gate) {
                        const uint64_t enabled = 1;
                        memcpy(bytes.data() + candidate.offset, &enabled, std::min(candidate.size, sizeof(enabled)));
                }
                if (mode && candidate.name == "control.set_control_mode")
                        memcpy(bytes.data() + candidate.offset, &mode, std::min(candidate.size, sizeof(mode)));
        }
}

/**
 * @brief "操作表"（只读表 + action 枚举）的按钮文案：常见动作给中文，其余直接用枚举名
 */
static std::string
sdkActionLabel(const std::string &enumName)
{
        const std::string n = sdkLower(enumName);
        if (n.find("rescan") != std::string::npos || n.find("search") != std::string::npos ||
            n.find("scan") != std::string::npos)
                return tr("Search", "搜索");
        if (n.find("disconnect") != std::string::npos || n.find("close") != std::string::npos)
                return tr("Disconnect", "断开");
        if (n.find("connect") != std::string::npos || n.find("open") != std::string::npos)
                return tr("Connect", "连接");
        if (n.find("start") != std::string::npos)
                return tr("Start", "启动");
        if (n.find("stop") != std::string::npos)
                return tr("Stop", "停止");
        if (n.find("zero") != std::string::npos)
                return tr("Zero", "清零");
        if (n.find("factory") != std::string::npos)
                return tr("Factory reset", "恢复出厂");
        return enumName;
}

/**
 * @brief 操作表按钮的显示顺序：先搜索、再连接、再断开（符合"先搜后连"的习惯），
 *        其余动作排在后面、保持相对顺序
 */
static int
sdkActionPriority(const std::string &enumName)
{
        const std::string n = sdkLower(enumName);
        if (n.find("rescan") != std::string::npos || n.find("search") != std::string::npos ||
            n.find("scan") != std::string::npos)
                return 0;
        if (n.find("disconnect") != std::string::npos || n.find("close") != std::string::npos)
                return 2;
        if (n.find("connect") != std::string::npos || n.find("open") != std::string::npos)
                return 1;
        if (n.find("start") != std::string::npos)
                return 3;
        if (n.find("stop") != std::string::npos)
                return 4;
        if (n.find("zero") != std::string::npos)
                return 5;
        if (n.find("factory") != std::string::npos)
                return 6;
        return 100;
}

/**
 * @brief 数据表日志的一行：本地时间戳（含毫秒）+ 内容
 */
static std::string
sdkLogLine(const std::string &text)
{
        using namespace std::chrono;
        const auto        now          = system_clock::now();
        const auto        wholeSeconds = floor<seconds>(now);
        const auto        ms           = duration_cast<milliseconds>(now - wholeSeconds);
        const std::time_t t            = system_clock::to_time_t(wholeSeconds);
        std::tm           tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char stamp[32];
        snprintf(stamp,
                 sizeof(stamp),
                 "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                 tmBuf.tm_year + 1900,
                 tmBuf.tm_mon + 1,
                 tmBuf.tm_mday,
                 tmBuf.tm_hour,
                 tmBuf.tm_min,
                 tmBuf.tm_sec,
                 static_cast<int>(ms.count()));
        return std::string(stamp) + "  " + text;
}

/**
 * @brief 从表数据里取一个 char[] 字段的文本（用来把 SDK 的 last_error 一起写进日志）
 */
static std::string
sdkTableText(const CStructDecl &table, const std::vector<uint8_t> &bytes, const char *fieldName)
{
        for (const auto &field : table.fields) {
                if (field.name != fieldName || !field.isArray || field.offset + field.size > bytes.size())
                        continue;
                const char *p = reinterpret_cast<const char *>(bytes.data() + field.offset);
                size_t      n = 0;
                while (n < field.size && p[n] != '\0')
                        ++n;
                return std::string(p, n);
        }
        return std::string();
}

static std::string
sdkSerialDescription(const std::string &text)
{
#ifdef _WIN32
        // Older serial SDKs use SetupAPI's ANSI strings. ImGui expects UTF-8
        if (!text.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0)) {
                const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), nullptr, 0);
                if (length > 0) {
                        std::wstring wide(length, L'\0');
                        MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), wide.data(), length);
                        std::string utf8(WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr),
                                         '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), (int)utf8.size(), nullptr, nullptr);
                        return utf8;
                }
        }
#endif
        return text;
}

/**
 * @brief 读表数据里的一个整数字段（日志里要显示 timeout_ms 之类的参数）；定义在下面
 */
static int64_t sdkIntegerValue(const uint8_t *data, CType type, size_t size);

/**
 * @brief 取表数据里某个整数字段的值
 */
static bool
sdkTableInt(const CStructDecl &table, const std::vector<uint8_t> &bytes, const char *fieldName, int64_t &out)
{
        for (const auto &field : table.fields) {
                if (field.name != fieldName || field.isArray || field.offset + field.size > bytes.size())
                        continue;
                out = sdkIntegerValue(bytes.data() + field.offset, field.type, field.size);
                return true;
        }
        return false;
}

static const char *
sdkChannelType(CType type)
{
        switch (type) {
                case CType::Bool:
                case CType::U8:
                        return "u8";
                case CType::I8:
                        return "i8";
                case CType::U16:
                        return "u16";
                case CType::I16:
                        return "i16";
                case CType::U32:
                        return "u32";
                case CType::I32:
                        return "i32";
                case CType::U64:
                case CType::Ptr:
                        return "u64";
                case CType::I64:
                        return "i64";
                case CType::F32:
                        return "f32";
                case CType::F64:
                        return "f64";
                default:
                        return nullptr;
        }
}

static double
sdkFieldValue(const uint8_t *data, CType type, size_t size)
{
        if (type == CType::F32) {
                float v{};
                memcpy(&v, data, 4);
                return v;
        }
        if (type == CType::F64) {
                double v{};
                memcpy(&v, data, 8);
                return v;
        }
        uint64_t val = 0;
        memcpy(&val, data, std::min(size, sizeof(val)));
        if (type == CType::I8)
                return static_cast<int8_t>(val);
        if (type == CType::I16)
                return static_cast<int16_t>(val);
        if (type == CType::I32)
                return static_cast<int32_t>(val);
        if (type == CType::I64)
                return static_cast<double>(static_cast<int64_t>(val));
        return static_cast<double>(val);
}

static std::string
sdkEnumTypeName(std::string rawType)
{
        for (const char *qualifier : {"const ", "volatile ", "enum "}) {
                const size_t length = strlen(qualifier);
                while (rawType.size() >= length && rawType.compare(0, length, qualifier) == 0)
                        rawType.erase(0, length);
        }
        while (!rawType.empty() && std::isspace(static_cast<unsigned char>(rawType.back())))
                rawType.pop_back();
        return rawType;
}

static int64_t
sdkIntegerValue(const uint8_t *data, CType type, size_t size)
{
        uint64_t raw = 0;
        memcpy(&raw, data, std::min(size, sizeof(raw)));
        switch (type) {
                case CType::I8:
                        return static_cast<int8_t>(raw);
                case CType::I16:
                        return static_cast<int16_t>(raw);
                case CType::I32:
                        return static_cast<int32_t>(raw);
                case CType::I64:
                        return static_cast<int64_t>(raw);
                default:
                        return static_cast<int64_t>(raw);
        }
}

static void
sdkSetIntegerValue(uint8_t *data, size_t size, int64_t val)
{
        memcpy(data, &val, std::min(size, sizeof(val)));
}

static std::string
sdkDefaultArg(const CParam &param)
{
        const std::string name = sdkLower(param.name);
        if (name.find("timeout") != std::string::npos)
                return "100";
        if (name.find("retry") != std::string::npos)
                return "1";
        return "0";
}

static bool
sdkEditValue(const char *id, uint8_t *data, CType type, size_t size, const char *format = nullptr)
{
        if (!data || size == 0)
                return false;
        if (type == CType::Bool) {
                bool val = *data != 0;
                if (!ImGui::Checkbox(id, &val))
                        return false;
                *data = val ? 1 : 0;
                return true;
        }
        ImGuiDataType dataType;
        switch (type) {
                case CType::I8:
                        dataType = ImGuiDataType_S8;
                        break;
                case CType::U8:
                        dataType = ImGuiDataType_U8;
                        break;
                case CType::I16:
                        dataType = ImGuiDataType_S16;
                        break;
                case CType::U16:
                        dataType = ImGuiDataType_U16;
                        break;
                case CType::I32:
                        dataType = ImGuiDataType_S32;
                        break;
                case CType::U32:
                        dataType = ImGuiDataType_U32;
                        break;
                case CType::I64:
                        dataType = ImGuiDataType_S64;
                        break;
                case CType::U64:
                case CType::Ptr:
                        dataType = ImGuiDataType_U64;
                        break;
                case CType::F32:
                        dataType = ImGuiDataType_Float;
                        break;
                case CType::F64:
                        dataType = ImGuiDataType_Double;
                        break;
                default:
                        return false;
        }
        // Note for callers: InputScalar parses its text buffer and writes the result
        // back through `data` on every keystroke, and it rejects
        // ImGuiInputTextFlags_EnterReturnsTrue outright (imgui_widgets.cpp asserts)
        // Anything that must not see a half-typed value has to hand us a scratch
        // buffer and fold it back itself once the user commits
        return ImGui::InputScalar(id, dataType, data, nullptr, nullptr, format);
}

static bool
sdkReservedField(const std::string &name)
{
        return name == "reserved" || name.rfind("reserved.", 0) == 0 || name.find(".reserved") != std::string::npos;
}

static bool
sdkStringArray(const CFieldDecl &field)
{
        return field.isArray && (field.arrayElemType == CType::I8 || field.arrayElemType == CType::U8) &&
               field.rawType.find("char") != std::string::npos;
}

static std::string
sdkBytesToHex(const std::vector<uint8_t> &bytes)
{
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string           text(bytes.size() * 2, '0');
        for (size_t i = 0; i < bytes.size(); ++i) {
                text[i * 2]     = digits[bytes[i] >> 4];
                text[i * 2 + 1] = digits[bytes[i] & 0x0F];
        }
        return text;
}

static bool
sdkHexToBytes(const char *text, std::vector<uint8_t> &bytes)
{
        if (!text)
                return false;
        const size_t length = strlen(text);
        if ((length & 1U) != 0)
                return false;
        auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9')
                        return c - '0';
                if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                return -1;
        };
        const size_t count = std::min(bytes.size(), length / 2);
        for (size_t i = 0; i < count; ++i) {
                const int hi = nibble(text[i * 2]);
                const int lo = nibble(text[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                        return false;
                bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
}

// ─── drawSdkStructFieldRows ───────────────────────────────────────────────────
// Shared struct-field editor used by both the dynamic library caller and the sequence editor

bool
drawSdkStructFieldRows(const CStructDecl &sd, uint8_t *buf, size_t bufSize, int idSalt)
{
        if (!buf || bufSize < sd.totalSize)
                return false;

        bool edited = false;
        for (size_t fi = 0; fi < sd.fields.size(); ++fi) {
                const auto &field = sd.fields[fi];
                if (field.isArray) {
                        size_t esz = ctypeSize(field.arrayElemType);
                        for (size_t ai = 0; ai < field.arrayCount; ++ai) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("  .%s[%zu]  %s", field.name.c_str(), ai, ctypeLabel(field.arrayElemType));
                                ImGui::TableSetColumnIndex(2);
                                char fid[64];
                                snprintf(fid, sizeof(fid), "##af%d_%zu_%zu", idSalt, fi, ai);
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                uint8_t *p2 = buf + field.offset + ai * esz;
                                if (field.arrayElemType == CType::F64) {
                                        double v;
                                        memcpy(&v, p2, 8);
                                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g")) {
                                                memcpy(p2, &v, 8);
                                                edited = true;
                                        }
                                } else if (field.arrayElemType == CType::F32) {
                                        float v;
                                        memcpy(&v, p2, 4);
                                        if (ImGui::InputFloat(fid, &v)) {
                                                memcpy(p2, &v, 4);
                                                edited = true;
                                        }
                                } else {
                                        int iv = 0;
                                        memcpy(&iv, p2, std::min(esz, (size_t)4));
                                        if (ImGui::InputInt(fid, &iv)) {
                                                memcpy(p2, &iv, std::min(esz, (size_t)4));
                                                edited = true;
                                        }
                                }
                        }
                } else if (field.type == CType::F64) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  double", field.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##df%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        double v;
                        memcpy(&v, buf + field.offset, 8);
                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g")) {
                                memcpy(buf + field.offset, &v, 8);
                                edited = true;
                        }
                } else if (field.type == CType::F32) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  float", field.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##ff%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        float v;
                        memcpy(&v, buf + field.offset, 4);
                        if (ImGui::InputFloat(fid, &v)) {
                                memcpy(buf + field.offset, &v, 4);
                                edited = true;
                        }
                } else {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  %s", field.name.c_str(), ctypeLabel(field.type));
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##if%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        int64_t v = 0;
                        memcpy(&v, buf + field.offset, std::min(field.size, (size_t)8));
                        int iv = (int)v;
                        if (ImGui::InputInt(fid, &iv)) {
                                v = iv;
                                memcpy(buf + field.offset, &v, std::min(field.size, (size_t)8));
                                edited = true;
                        }
                }
        }
        return edited;
}

// ─── helpers ──────────────────────────────────────────────────────────────────

void
SdkPanel::setStatus(const std::string &msg, bool isErr)
{
        statusMsg_   = msg;
        statusIsErr_ = isErr;
}

bool
SdkPanel::drawHistoryHeader(const char *id)
{
        ImGui::PushID(id);
        ImGui::SetNextItemOpen(historyExpanded_, ImGuiCond_Always);
        const bool expanded = ImGui::CollapsingHeader(tr("History", "调用历史"));
        ImGui::PopID();
        if (expanded != historyExpanded_) {
                historyExpanded_ = expanded;
                modified_        = true;
        }
        return historyExpanded_;
}

void
SdkPanel::drawHistoryLog(const char *id)
{
        std::vector<HistEntry> snapshot;
        {
                std::lock_guard lock(histMtx_);
                snapshot = history_;
        }
        ui::LogText log;
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
                log.timestamp(it->tsMs);
                log.append(it->call, ImGui::GetColorU32(it->ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f) : ui::ErrorTextColor()));
                log.append(" ");
                log.append(it->result, ImGui::GetColorU32(it->ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ui::ErrorTextColor()));
                log.append("\n");
        }
        ui::CopyLogButton(log.text);
        ui::LogView(id, log, ImVec2(0, std::max(60.0f, ImGui::GetContentRegionAvail().y)));
}

// ─── doLoadDll ────────────────────────────────────────────────────────────────

namespace
{
struct SdkPreset {
        const char *label, *directory, *library, *header, *descriptionEn, *descriptionZh;
};
constexpr SdkPreset sdkPresets[] = {
    {"DYN200", "dyn200", "dyn200.dll", "dyn200.h", "Dynamic torque sensor (serial)", "动态扭矩传感器（串口）"},
    {"KPS6050D", "kps6050d", "kps6050d.dll", "kps6050d.h", "DC power supply (serial)", "直流电源（串口）"},
    {"DSOX2024A", "dsox2024a", "dsox2024a.dll", "dsox2024a.h", "Oscilloscope (USB / LAN)", "示波器（USB / LAN）"},
    {"FSA v3", "fsav3", "fsav3_c.dll", "fsav3_c.h", "FSA v3 actuator (UDP)", "FSA v3 执行器（UDP）"},
};
std::string
sdkPathUtf8(const std::filesystem::path &path)
{
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
}
} // namespace

bool
SdkPanel::loadSdkPreset(size_t index, const std::string &sdkRoot)
{
        if (index >= std::size(sdkPresets))
                return false;
        std::filesystem::path root = std::filesystem::u8path(sdkRoot);
        if (root.empty()) {
#ifdef _WIN32
                wchar_t     executable[32768]{};
                const DWORD length = GetModuleFileNameW(nullptr, executable, (DWORD)std::size(executable));
                if (length && length < std::size(executable))
                        root = std::filesystem::path(executable).parent_path() / "sdk";
#endif
        }
        const auto     &preset    = sdkPresets[index];
        const auto      directory = root / preset.directory;
        const auto      library = directory / preset.library, header = directory / preset.header;
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_regular_file(library, ec) || !std::filesystem::is_regular_file(header, ec)) {
                setStatus(std::string(tr("SDK files missing: ", "SDK 配套文件缺失：")) + sdkPathUtf8(directory), true);
                return false;
        }
        const auto dllText = sdkPathUtf8(library), headerText = sdkPathUtf8(header);
        if (dllText.size() >= sizeof(dllPath_) || headerText.size() >= sizeof(headerPath_)) {
                setStatus(tr("SDK path is too long", "SDK 文件路径过长"), true);
                return false;
        }
        // Validate the pair before replacing the current panel. Loading a library
        // or parsing its header does not execute any device commands
        std::ifstream      input(header);
        std::ostringstream source;
        source << input.rdbuf();
        const auto parsed = parseHeaderFull(source.str());
        if (parsed.functions.empty() && parsed.classes.empty()) {
                setStatus(tr("SDK header contains no callable declarations", "SDK 头文件中没有可调用的声明"), true);
                return false;
        }
        if (seqRunning_) {
                setStatus(tr("Stop the running sequence before switching SDKs", "请先停止运行中的序列，再切换 SDK"), true);
                return false;
        }
        SdkLoader candidate;
        if (!candidate.load(dllText)) {
                setStatus(candidate.prevError(), true);
                return false;
        }
        stopDataUpdate();
        // Dispose objects while their original library and class declarations still exist
        while (!objects_.empty()) {
                selectedClassIdx_ = -1;
                for (int i = 0; i < (int)parseResult_.classes.size(); ++i)
                        if (parseResult_.classes[i].fullName == objects_.back().className ||
                            parseResult_.classes[i].name == objects_.back().className)
                                selectedClassIdx_ = i;
                doDeleteObject((int)objects_.size() - 1);
        }
        {
                std::lock_guard lock(dataTableMtx_);
                dataTables_.clear();
        }
        // A shortcut starts with fresh table settings, including no automatic writes
        snprintf(dllPath_, sizeof(dllPath_), "%s", dllText.c_str());
        snprintf(headerPath_, sizeof(headerPath_), "%s", headerText.c_str());
        doLoadDll();
        if (!loader_.isLoaded())
                return false;
        doParseHeader();
        hasDsoGui_ = showDsoGui_ = !statusIsErr_ && index == 2;
        modified_                = true;
        return !statusIsErr_;
}

void
SdkPanel::doLoadDll()
{
        hasDsoGui_ = showDsoGui_ = false;
        stopDataUpdate();
        bool loaded = false;
        {
                std::lock_guard callLock(sdkCallMtx_);
                if (loader_.isLoaded())
                        loader_.unload();
                loaded = loader_.load(dllPath_);
        }
        if (!loaded) {
                LOG_E("SDK auto-load failed: path=%s error=%s", dllPath_, loader_.prevError().c_str());
                setStatus(loader_.prevError(), true);
                return;
        }

        // On Windows, enumerateExports falls back to GetModuleHandleW(path) when
        // handle is nullptr — works because SdkLoader::load already loaded it
        // On POSIX, it reads the file directly and ignores handle
        auto syms = enumerateExports(nullptr, dllPath_);
        {
                std::lock_guard callLock(sdkCallMtx_);
                loader_.setExports(syms);
        }
        LOG_I("SDK loaded: path=%s exports=%d", dllPath_, (int)syms.size());

        char buf[128];
        snprintf(buf, sizeof(buf), tr("Loaded: %s  (%d exports)", "已加载: %s  (%d 个导出符号)"), dllPath_, (int)syms.size());
        setStatus(buf, false);
}

// ─── doParseHeader ────────────────────────────────────────────────────────────

int64_t
SdkPanel::headerFileMtime() const
{
        if (!headerPath_[0])
                return 0;
        std::error_code ec;
        const auto      stamp = std::filesystem::last_write_time(std::filesystem::u8path(headerPath_), ec);
        if (ec)
                return 0;
        return (int64_t)stamp.time_since_epoch().count();
}

void
SdkPanel::pollHeaderChange()
{
        if (!headerAutoReparse_ || !headerPath_[0])
                return;
        const double now = sessionTimeSec();
        if (now < headerPollAt_)
                return;
        headerPollAt_ = now + 1.0; // stat at most once per second

        const int64_t stamp = headerFileMtime();
        // 0 means the file could not be stat'd — an editor doing a
        // write-to-temp-then-rename save briefly makes the path disappear. Treat it
        // as "no news" instead of a change
        if (stamp == 0 || stamp == headerMtime_)
                return;
        // Commit the stamp first: if the parse itself fails we must not retry it
        // once a second forever
        headerMtime_ = stamp;
        doParseHeader();
        setStatus(tr("Header changed on disk — re-parsed", "头文件已变动，已重新解析"), false);
}

void
SdkPanel::doParseHeader()
{
        stopDataUpdate();
        std::ifstream f(std::filesystem::u8path(headerPath_));
        if (!f) {
                setStatus(tr("Cannot open header file", "无法打开头文件"), true);
                return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        // Re-parsing invalidates every index-keyed cache below, and rebuildDataTables()
        // starts the tables from scratch. Snapshot what the user was looking at (by
        // name) and what they had typed, so an automatic re-parse after a header save
        // does not reset the panel underneath them
        std::string prevFnName, prevClassName, prevMethodName;
        if (selectedFnIdx_ >= 0 && selectedFnIdx_ < (int)parseResult_.functions.size())
                prevFnName = parseResult_.functions[selectedFnIdx_].name;
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size()) {
                const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
                prevClassName         = cls.fullName.empty() ? cls.name : cls.fullName;
                if (selectedMethodIdx_ >= 0 && selectedMethodIdx_ < (int)cls.methods.size())
                        prevMethodName = cls.methods[selectedMethodIdx_].name;
        }
        std::vector<ArgBuf> prevFnArgs   = fnArgBufs_;
        std::vector<ArgBuf> prevMethArgs = methArgBufs_;

        struct TableState {
                std::vector<uint8_t> data;
                bool                 expanded{true};
        };
        std::unordered_map<std::string, TableState> prevTables;
        {
                std::lock_guard lock(dataTableMtx_);
                for (const auto &binding : dataTables_) {
                        TableState st;
                        st.data     = binding.data;
                        st.expanded = binding.expanded;
                        prevTables.emplace(binding.structName, std::move(st));
                }
        }

        parseResult_       = parseHeaderFull(ss.str());
        selectedFnIdx_     = -1;
        selectedClassIdx_  = -1;
        selectedMethodIdx_ = -1;
        selectedObjIdx_    = -1;
        fnArgBufs_.clear();
        methArgBufs_.clear();
        structBufs_.clear();
        fnArgBufsCache_.clear();
        methArgBufsCache_.clear();
        structBufsCache_.clear();
        fnLastResult_.clear();
        rebuildDataTables();

        // ── Restore selection by name ──
        for (int i = 0; !prevFnName.empty() && i < (int)parseResult_.functions.size(); ++i) {
                if (parseResult_.functions[i].name != prevFnName)
                        continue;
                selectFn(i);
                if (prevFnArgs.size() == fnArgBufs_.size())
                        fnArgBufs_ = std::move(prevFnArgs);
                break;
        }
        for (int ci = 0; !prevClassName.empty() && ci < (int)parseResult_.classes.size(); ++ci) {
                const CClassDecl &cls  = parseResult_.classes[ci];
                const std::string full = cls.fullName.empty() ? cls.name : cls.fullName;
                if (full != prevClassName)
                        continue;
                selectedClassIdx_ = ci;
                for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                        if (cls.methods[mi].name != prevMethodName)
                                continue;
                        selectMethod(ci, mi);
                        if (prevMethArgs.size() == methArgBufs_.size())
                                methArgBufs_ = std::move(prevMethArgs);
                        break;
                }
                break;
        }

        // ── Restore data-table state ──
        {
                std::lock_guard lock(dataTableMtx_);
                for (auto &binding : dataTables_) {
                        auto it = prevTables.find(binding.structName);
                        if (it == prevTables.end())
                                continue;
                        const TableState &st = it->second;
                        // Automatic calls are derived from live variable links
                        binding.expanded = st.expanded;
                        // Only carry the raw bytes over when the struct is still exactly
                        // as big as it was — a changed layout would reinterpret them
                        if (st.data.size() == binding.data.size())
                                binding.data = st.data;
                }
        }
        ensureDataTableLocals();

        headerMtime_  = headerFileMtime();
        headerPollAt_ = sessionTimeSec() + 1.0;

        int  totalFn  = (int)parseResult_.functions.size();
        int  totalCls = (int)parseResult_.classes.size();
        char buf[256];
        snprintf(buf, sizeof(buf), tr("Parsed: %d function(s), %d class(es)", "解析: %d 个函数, %d 个类"), totalFn, totalCls);
        setStatus(buf, (totalFn + totalCls) == 0);
}

void
SdkPanel::ensureDataTableLocals(bool preferExisting)
{
        if (!onCreateLocalVar_)
                return;
        struct LocalSnapshot {
                size_t               index;
                std::string          name;
                const CStructDecl   *table;
                std::vector<uint8_t> data;
        };
        std::vector<LocalSnapshot> locals;
        {
                std::lock_guard lock(dataTableMtx_);
                locals.reserve(dataTables_.size());
                for (size_t index = 0; index < dataTables_.size(); ++index) {
                        auto              &binding = dataTables_[index];
                        const CStructDecl *table   = dataTableStruct(binding);
                        if (!table)
                                continue;
                        /*
                         * 导入头文件不再自动往变量管理器里塞表变量：只有用户拖过
                         * 整张表（rootPublished），或者会话里本来就存在同名根变量时，
                         * 才维护整表的 LOCAL 变量；单个字段保持独立绑定。
                         */
                        if (!binding.rootPublished && (!onGetVarBuf_ || !onGetVarBuf_(binding.localName)))
                                continue;
                        binding.rootPublished = true;
                        locals.push_back({index, binding.localName, table, binding.data});
                }
        }
        for (auto &local : locals) {
                // Remember only whether the variable existed. onCreateLocalVar_
                // may enlarge a legacy LOCAL buffer, invalidating its old data()
                // pointer, so never copy through the pointer obtained before it
                const bool existed = onGetVarBuf_ && onGetVarBuf_(local.name) != nullptr;
                onCreateLocalVar_(local.name, CType::Unknown, local.table);
                // Enum definitions come from the header, not from the struct decl, so
                // they need their own hop. Re-publishing on every call is what makes a
                // header re-parse reach already-plotted chs
                if (onPublishLocalEnums_) {
                        const auto memberEnums = collectTableEnums(local.name, *local.table);
                        if (!memberEnums.empty())
                                onPublishLocalEnums_(local.name, memberEnums);
                }
                if (onGetVarBuf_) {
                        if (void *buffer = onGetVarBuf_(local.name)) {
                                if (preferExisting && existed) {
                                        memcpy(local.data.data(), buffer, std::min(local.data.size(), local.table->totalSize));
                                        std::lock_guard lock(dataTableMtx_);
                                        if (local.index < dataTables_.size() &&
                                            dataTables_[local.index].localName == local.name) {
                                                dataTables_[local.index].data = local.data;
                                                ++dataTables_[local.index].dataRevision;
                                        }
                                } else {
                                        memcpy(buffer, local.data.data(), std::min(local.data.size(), local.table->totalSize));
                                        if (onVarWritten_)
                                                onVarWritten_(local.name);
                                }
                        }
                }
        }
}

const CStructDecl *
SdkPanel::dataTableStruct(const DataTableBinding &binding) const
{
        return findStruct(parseResult_, binding.structName);
}

std::vector<std::pair<std::string, std::vector<CEnumValue>>>
SdkPanel::collectTableEnums(const std::string &localName, const CStructDecl &table) const
{
        std::vector<std::pair<std::string, std::vector<CEnumValue>>> out;
        for (const auto &field : table.fields) {
                if (field.isArray || sdkReservedField(field.name))
                        continue;
                const CEnumDecl *ed = findEnum(parseResult_, sdkEnumTypeName(field.rawType));
                if (!ed || ed->values.empty())
                        continue;
                // Same key the variable manager uses for a manual struct field:
                // "<var>.<field>", where <field> already carries the group prefix
                // ("control.set_control_mode")
                out.emplace_back(localName + "." + field.name, ed->values);
        }
        return out;
}

void
SdkPanel::rebuildDataTables()
{
        std::lock_guard lock(dataTableMtx_);
        dataTables_.clear();

        auto matchesStruct = [&](const CParam &param, const CStructDecl &table) {
                const CStructDecl *type = findParamStruct(param);
                return type && (type->fullName == table.fullName || type->name == table.name);
        };
        auto addTable = [&](const CStructDecl &table, int classIdx) {
                const bool isParam = sdkEndsWith(table.name, "param");
                const bool isRt    = sdkEndsWith(table.name, "rt");
                if ((!isParam && !isRt) || !table.isPOD || table.totalSize == 0)
                        return;
                DataTableBinding binding;
                binding.name       = table.name;
                binding.structName = table.fullName.empty() ? table.name : table.fullName;
                binding.kind       = isParam ? DataTableKind::Param : DataTableKind::Realtime;
                binding.isC        = classIdx < 0;
                binding.classIdx   = classIdx;
                binding.localName  = "SDK" + std::to_string(winId_) + "." + table.name;
                binding.data.resize(table.totalSize);
                binding.fieldPublished.assign(table.fields.size(), false);
                binding.actionFieldIdx = -1;
                binding.fieldFlashUntil.assign(table.fields.size(), 0.0);
                binding.fieldEditUntil.assign(table.fields.size(), 0.0);
                binding.prevData.clear(); // 第一次读取只记录，不闪

                auto inspect = [&](const auto &function, int index) {
                        const bool suffix = isParam ? sdkEndsWith(function.name, "param") : sdkEndsWith(function.name, "rt");
                        if (!suffix)
                                return;
                        for (int pi = 0; pi < (int)function.params.size(); ++pi) {
                                if (!matchesStruct(function.params[pi], table))
                                        continue;
                                if (sdkReadName(function.name)) {
                                        binding.readFnIdx       = index;
                                        binding.structParamRead = pi;
                                } else if (sdkWriteName(function.name)) {
                                        binding.writeFnIdx       = index;
                                        binding.structParamWrite = pi;
                                }
                                break;
                        }
                };
                if (classIdx < 0) {
                        for (int i = 0; i < (int)parseResult_.functions.size(); ++i)
                                inspect(parseResult_.functions[i], i);
                } else {
                        const auto &methods = parseResult_.classes[classIdx].methods;
                        for (int i = 0; i < (int)methods.size(); ++i)
                                inspect(methods[i], i);
                }
                /*
                 * 只读表（没有 write 函数）+ 一个名为 action 的枚举字段 = "操作表"：
                 * 面板把枚举值渲染成按钮（搜索/连接/断开…），点一下就用读函数执行
                 * 该动作，因此不提供"自动读取/周期写入"。
                 */
                if (binding.writeFnIdx < 0 && binding.readFnIdx >= 0) {
                        for (size_t fi = 0; fi < table.fields.size(); ++fi) {
                                const auto &field = table.fields[fi];
                                if (field.isArray || field.name != "action")
                                        continue;
                                const CEnumDecl *actionEnum = findEnum(parseResult_, sdkEnumTypeName(field.rawType));
                                if (actionEnum && !actionEnum->values.empty())
                                        binding.actionFieldIdx = static_cast<int>(fi);
                                break;
                        }
                        if (binding.actionFieldIdx >= 0)
                                binding.autoRead = false;
                }
                // Discovery/action tables keep their explicit buttons
                if (!dataTableAutoAllowed(binding)) {
                        binding.autoRead  = false;
                        binding.autoWrite = false;
                }
                // Optional serial-list API used by DYN200 and compatible SDKs. Keep the
                // table ABI unchanged; enumerate through the existing count/name exports
                if (binding.isC && binding.actionFieldIdx >= 0 && sdkEndsWith(table.name, "_port_param")) {
                        const std::string prefix   = table.name.substr(0, table.name.size() - 6);
                        auto              hasField = [&](const char *name, bool text) {
                                return std::any_of(table.fields.begin(), table.fields.end(), [&](const auto &f) {
                                        return f.name == name && f.offset + f.size <= table.totalSize &&
                                               (text ? sdkStringArray(f) && f.size > 1 : !f.isArray && f.type == CType::U32);
                                });
                        };
                        if (hasField("index", false) && hasField("count", false) && hasField("port", true) &&
                            hasField("name", true) && hasField("desc", true)) {
                                for (int i = 0; i < (int)parseResult_.functions.size(); ++i) {
                                        const auto &fn = parseResult_.functions[i];
                                        if (fn.name == prefix + "_count" && fn.retType == CType::U32 && fn.params.empty())
                                                binding.portCountFnIdx = i;
                                        if (fn.name == prefix + "_name" && fn.retType == CType::I32 && fn.params.size() == 5 &&
                                            fn.params[0].type == CType::U32 && fn.params[1].type == CType::Ptr &&
                                            ctypeSize(fn.params[2].type) == sizeof(size_t) &&
                                            ctypeIsInteger(fn.params[2].type) && fn.params[3].type == CType::Ptr &&
                                            ctypeSize(fn.params[4].type) == sizeof(size_t) && ctypeIsInteger(fn.params[4].type))
                                                binding.portNameFnIdx = i;
                                }
                        }
                }
                if (binding.readFnIdx >= 0 || binding.writeFnIdx >= 0)
                        dataTables_.push_back(std::move(binding));
        };

        for (const auto &table : parseResult_.structs)
                addTable(table, -1);
        for (int ci = 0; ci < (int)parseResult_.classes.size(); ++ci)
                for (const auto &table : parseResult_.classes[ci].innerStructs)
                        addTable(table, ci);
}

/**
 * @brief Linked tables are read continuously and write when a linked value changes
 *        Discovery/action tables retain their explicit buttons
 */
bool
SdkPanel::dataTableAutoAllowed(const DataTableBinding &binding) const
{
        return binding.actionFieldIdx < 0;
}

void
SdkPanel::refreshDataTableBindings()
{
        if (!onSnapshotLocalBuffers_)
                return;
        const auto               locals = onSnapshotLocalBuffers_();
        bool                     wake   = false;
        std::vector<std::string> sampleSources;
        {
                std::lock_guard lock(dataTableMtx_);
                for (auto &binding : dataTables_) {
                        const auto *table = dataTableStruct(binding);
                        if (!table)
                                continue;
                        binding.localBindings.clear();
                        binding.rootPublished = false;
                        binding.fieldPublished.assign(table->fields.size(), false);
                        const auto add = [&](const std::string &name, size_t offset, size_t size) {
                                const auto it = locals.find(name);
                                if (it == locals.end() || it->second.size() < size || offset + size > binding.data.size())
                                        return false;
                                binding.localBindings.push_back({name, offset, size});
                                sampleSources.push_back(name);
                                binding.localBaseline.try_emplace(name, it->second);
                                return true;
                        };
                        binding.rootPublished = add(binding.localName, 0, table->totalSize);
                        for (size_t i = 0; i < table->fields.size(); ++i) {
                                const auto &field = table->fields[i];
                                if (sdkReservedField(field.name) || sdkStringArray(field))
                                        continue;
                                const auto name = binding.localName + "." + field.name;
                                if (!field.isArray)
                                        binding.fieldPublished[i] = add(name, field.offset, field.size);
                                else {
                                        const size_t size = ctypeSize(field.arrayElemType);
                                        if (!size)
                                                continue;
                                        for (size_t a = 0; a < field.arrayCount; ++a)
                                                binding.fieldPublished[i] =
                                                    add(name + "[" + std::to_string(a) + "]", field.offset + a * size, size) ||
                                                    binding.fieldPublished[i];
                                }
                        }
                        for (auto it = binding.localBaseline.begin(); it != binding.localBaseline.end();) {
                                if (std::none_of(binding.localBindings.begin(),
                                                 binding.localBindings.end(),
                                                 [&](const auto &link) { return link.name == it->first; }))
                                        it = binding.localBaseline.erase(it);
                                else
                                        ++it;
                        }
                        const bool linked  = !binding.localBindings.empty() && dataTableAutoAllowed(binding);
                        const bool read    = linked && binding.readFnIdx >= 0;
                        const bool write   = linked && binding.writeFnIdx >= 0;
                        wake              |= read != binding.autoRead || write != binding.autoWrite;
                        binding.autoRead   = read;
                        binding.autoWrite  = write;
                }
        }
        if (onLocalSampleSource_)
                for (const auto &name : sampleSources)
                        onLocalSampleSource_(name);
        if (wake)
                dataUpdateCv_.notify_one();
}

void
SdkPanel::stopDataUpdate()
{
        {
                // Pair the stop transition with the condition-variable mutex;
                // otherwise notify can race just before the idle worker waits
                std::lock_guard lock(dataTableMtx_);
                dataUpdateStop_.store(true, std::memory_order_release);
        }
        dataUpdateCv_.notify_all();
        if (dataUpdateThread_.joinable())
                dataUpdateThread_.join();
        {
                std::lock_guard lock(dataTableMtx_);
                dataManualCalls_.clear();
        }
        dataUpdateBusy_.store(false, std::memory_order_release);
        dataUpdateStop_.store(false, std::memory_order_release);
}

void
SdkPanel::refreshSerialPorts(DataTableBinding &binding, const CStructDecl &table)
{
        // Worker thread, with sdkCallMtx_ held. These APIs only enumerate ports;
        // never probe a device by opening it or invoking the table's NONE action
        binding.portsScanned = true;
        binding.serialPorts.clear();
        binding.portScanError.clear();
        const auto count = loader_.call(parseResult_.functions[binding.portCountFnIdx], {});
        if (!count.ok || count.rawU64 > 512) {
                binding.portScanError = count.ok ? "Invalid serial port count" : count.error;
                return;
        }
        for (uint32_t i = 0; i < count.rawU64; ++i) {
                char       name[256]{}, description[512]{};
                const auto result =
                    loader_.call(parseResult_.functions[binding.portNameFnIdx],
                                 {std::to_string(i), "", std::to_string(sizeof(name)), "", std::to_string(sizeof(description))},
                                 {nullptr, name, nullptr, description, nullptr});
                if (!result.ok || static_cast<int32_t>(result.rawU64) != 0) {
                        binding.serialPorts.clear();
                        binding.portScanError = result.ok ? "Serial port list changed; search again" : result.error;
                        return;
                }
                name[sizeof(name) - 1] = description[sizeof(description) - 1] = '\0';
                if (name[0] && std::none_of(binding.serialPorts.begin(), binding.serialPorts.end(), [&](const auto &p) {
                            return p.name == name;
                    }))
                        binding.serialPorts.push_back({i, name, description});
        }
        // Rescan may reorder indices. Preserve the selected port by name, including
        // manually entered names; never silently switch to a different USB adapter
        const auto selected = sdkTableText(table, binding.data, "port");
        for (const auto &port : binding.serialPorts)
                if (port.name == selected) {
                        selectSerialPort(binding, table, port);
                        return;
                }
        // Keep a missing/manual port editable, but clear stale enumeration feedback
        for (const auto &field : table.fields)
                if ((field.name == "name" || field.name == "desc") && field.offset + field.size <= binding.data.size())
                        memset(binding.data.data() + field.offset, 0, field.size);
}

std::vector<size_t>
SdkPanel::selectSerialPort(DataTableBinding &binding, const CStructDecl &table, const DataTableBinding::SerialPortChoice &port)
{
        std::vector<size_t> fields;
        for (size_t i = 0; i < table.fields.size(); ++i) {
                const auto &field = table.fields[i];
                if (field.offset + field.size > binding.data.size())
                        continue;
                auto *data = binding.data.data() + field.offset;
                if (field.name == "index" && !field.isArray)
                        sdkSetIntegerValue(data, field.size, port.index);
                else if (sdkStringArray(field) && (field.name == "port" || field.name == "name" || field.name == "desc")) {
                        memset(data, 0, field.size);
                        const auto &text = field.name == "desc" ? port.description : port.name;
                        if (field.size > 0)
                                memcpy(data, text.data(), std::min(text.size(), field.size - 1));
                } else
                        continue;
                fields.push_back(i);
        }
        return fields;
}

void
SdkPanel::runDataTableCall(size_t index, bool write, bool manual)
{
        const auto       localSnapshot = onSnapshotLocalBuffers_ ? onSnapshotLocalBuffers_() : LocalBuffers{};
        DataTableBinding binding;
        {
                std::lock_guard lock(dataTableMtx_);
                if (index >= dataTables_.size())
                        return;
                // Do not copy UI highlights, history/statistics and publication
                // vectors on every acquisition tick
                const auto &source       = dataTables_[index];
                binding.localName        = source.localName;
                binding.structName       = source.structName;
                binding.isC              = source.isC;
                binding.classIdx         = source.classIdx;
                binding.readFnIdx        = source.readFnIdx;
                binding.writeFnIdx       = source.writeFnIdx;
                binding.structParamRead  = source.structParamRead;
                binding.structParamWrite = source.structParamWrite;
                binding.dataRevision     = source.dataRevision;
                binding.data             = source.data;
                binding.portCountFnIdx   = source.portCountFnIdx;
                binding.portNameFnIdx    = source.portNameFnIdx;
                binding.actionFieldIdx   = source.actionFieldIdx;
                binding.autoWrite        = source.autoWrite;
                binding.localBindings    = source.localBindings;
                binding.localBaseline    = source.localBaseline;
        }
        const CStructDecl *table = dataTableStruct(binding);
        if (!table)
                return;
        const int fnIndex     = write ? binding.writeFnIdx : binding.readFnIdx;
        const int structParam = write ? binding.structParamWrite : binding.structParamRead;
        if (fnIndex < 0 || structParam < 0)
                return;

        std::vector<uint8_t> bytes = binding.data;
        struct LocalEdit {
                size_t               offset, field;
                std::vector<uint8_t> bytes;
        };
        std::vector<LocalEdit> edits;
        for (const auto &link : binding.localBindings) {
                const auto current  = localSnapshot.find(link.name);
                const auto previous = binding.localBaseline.find(link.name);
                if (current == localSnapshot.end() || previous == binding.localBaseline.end() ||
                    current->second.size() < link.size || previous->second.size() < link.size)
                        continue;
                for (size_t i = 0; i < table->fields.size(); ++i) {
                        const auto &field = table->fields[i];
                        if (sdkReservedField(field.name))
                                continue;
                        const size_t begin = std::max(field.offset, link.offset);
                        const size_t end   = std::min(field.offset + field.size, link.offset + link.size);
                        if (end <= begin)
                                continue;
                        const size_t source = begin - link.offset;
                        if (memcmp(current->second.data() + source, previous->second.data() + source, end - begin))
                                edits.push_back(
                                    {begin,
                                     i,
                                     {current->second.begin() + source, current->second.begin() + source + end - begin}});
                }
        }
        // A changed local value must reach the write callback before a read can
        // publish over it. Unchanged linked tables never generate repeated writes
        if (!manual && ((write && edits.empty()) || (!write && binding.autoWrite && !edits.empty())))
                return;
        if (write && !edits.empty()) {
                std::lock_guard lock(dataTableMtx_);
                if (index >= dataTables_.size())
                        return;
                auto &target = dataTables_[index];
                bytes        = target.data;
                for (const auto &edit : edits)
                        if (edit.offset + edit.bytes.size() <= bytes.size())
                                memcpy(bytes.data() + edit.offset, edit.bytes.data(), edit.bytes.size());
                for (const auto &edit : edits)
                        sdkArmFieldWrite(*table, bytes, table->fields[edit.field]);
                target.data          = bytes;
                binding.dataRevision = ++target.dataRevision;
        }
        const auto   inputBytes = bytes;
        CallResult   result;
        CType        callReturnType = CType::Void;
        const double callStartSec   = sdkMonoSeconds();
        {
                std::lock_guard callLock(sdkCallMtx_);
                if (binding.isC) {
                        if (fnIndex >= (int)parseResult_.functions.size())
                                return;
                        const auto &fn = parseResult_.functions[fnIndex];
                        callReturnType = fn.retType;
                        std::vector<std::string> args;
                        std::vector<void *>      pointers(fn.params.size(), nullptr);
                        for (const auto &param : fn.params)
                                args.push_back(sdkDefaultArg(param));
                        pointers[structParam] = bytes.data();
                        result                = loader_.call(fn, args, pointers);
                } else {
                        if (binding.classIdx < 0 || binding.classIdx >= (int)parseResult_.classes.size())
                                return;
                        const auto &cls = parseResult_.classes[binding.classIdx];
                        if (fnIndex >= (int)cls.methods.size())
                                return;
                        const auto &method = cls.methods[fnIndex];
                        callReturnType     = method.retType;
                        void *object       = nullptr;
                        for (const auto &entry : objects_) {
                                if (entry.className == cls.fullName) {
                                        object = entry.ptr;
                                        break;
                                }
                        }
                        if (!method.isStatic && !object) {
                                result.error =
                                    "No SDK object for " + cls.fullName + "; create and initialize one in Advanced Calls";
                        } else {
                                std::vector<std::string> args;
                                std::vector<void *>      pointers(method.params.size(), nullptr);
                                for (const auto &param : method.params)
                                        args.push_back(sdkDefaultArg(param));
                                pointers[structParam] = bytes.data();
                                result                = loader_.callMethod(object, method, args, pointers);
                        }
                }
        }
        // 这次调用花了多久：串口往返 + SDK 处理; 慢的时候就是它
        const float callMs = static_cast<float>((sdkMonoSeconds() - callStartSec) * 1000.0);

        const bool hasStatusReturn = callReturnType == CType::I8 || callReturnType == CType::I16 ||
                                     callReturnType == CType::I32 || callReturnType == CType::I64;
        if (result.ok && hasStatusReturn) {
                int64_t statusCode = 0;
                switch (callReturnType) {
                        case CType::I8:
                                statusCode = static_cast<int8_t>(result.rawU64);
                                break;
                        case CType::I16:
                                statusCode = static_cast<int16_t>(result.rawU64);
                                break;
                        case CType::I32:
                                statusCode = static_cast<int32_t>(result.rawU64);
                                break;
                        case CType::I64:
                                statusCode = static_cast<int64_t>(result.rawU64);
                                break;
                        default:
                                statusCode = static_cast<int64_t>(result.rawU64);
                                break;
                }
                if (statusCode != 0) {
                        result.ok    = false;
                        result.error = "SDK returned error " + std::to_string(statusCode);
                }
        }

        const bool refreshPorts = result.ok && !write && manual && binding.portCountFnIdx >= 0 && binding.portNameFnIdx >= 0;
        if (refreshPorts) {
                std::lock_guard callLock(sdkCallMtx_);
                binding.data = bytes;
                refreshSerialPorts(binding, *table);
                bytes = std::move(binding.data);
        }
        bool         rootPublished = false;
        bool         accepted      = false;
        bool         sampled       = result.ok && !write;
        const double sampleTime    = sessionTimeSec();
        int64_t      fresh         = 1;
        if (sdkTableInt(*table, bytes, "fresh", fresh) && !fresh)
                sampled = false;
        std::vector<bool> fieldsPublished;
        {
                std::lock_guard lock(dataTableMtx_);
                if (index >= dataTables_.size())
                        return;
                auto &target = dataTables_[index];
                if (refreshPorts) {
                        target.serialPorts   = std::move(binding.serialPorts);
                        target.portsScanned  = binding.portsScanned;
                        target.portScanError = std::move(binding.portScanError);
                }
                // Both read and write callbacks may update the table buffer
                // A write callback uses this to acknowledge one-shot command
                // fields (for example by clearing a control word)
                if (result.ok && target.dataRevision == binding.dataRevision) {
                        target.data = bytes;
                        accepted    = true;
                } else if (result.ok && write) {
                        // Preserve newer UI edits while acknowledging unchanged
                        // command fields, so a completed one-shot is not replayed
                        for (const auto &field : table->fields)
                                if (field.offset + field.size <= bytes.size() &&
                                    field.offset + field.size <= target.data.size() &&
                                    !memcmp(target.data.data() + field.offset, inputBytes.data() + field.offset, field.size))
                                        memcpy(target.data.data() + field.offset, bytes.data() + field.offset, field.size);
                }
                /*
                 * 操作表的 action 是一次性的：不管这次调用成功还是失败，都把缓冲里的
                 * action 清掉，免得按钮点完之后下拉框/缓冲一直停在 CONNECT 之类的值上
                 * （SDK 成功时会自己清，失败时它返回的字节会被上面这行丢掉）。
                 */
                if (target.actionFieldIdx >= 0) {
                        const auto &actionField = table->fields[target.actionFieldIdx];
                        if (actionField.offset + actionField.size <= target.data.size())
                                memset(target.data.data() + actionField.offset, 0, actionField.size);
                }
                if (result.ok) {
                        // 逐字段比一次字节：读回来的值变过的字段亮一下，让用户一眼
                        // 看出哪些数据更新了; 写调用同时会把门控字段清零，只更新
                        // 基线不触发高亮，免得刚下发完就闪一片
                        if (!write && target.prevData.size() == bytes.size() &&
                            target.fieldFlashUntil.size() == table->fields.size()) {
                                const double nowSec = sdkMonoSeconds();
                                for (size_t i = 0; i < table->fields.size(); ++i) {
                                        const auto &field = table->fields[i];
                                        if (sdkReservedField(field.name) || field.offset + field.size > bytes.size())
                                                continue;
                                        if (memcmp(target.prevData.data() + field.offset,
                                                   bytes.data() + field.offset,
                                                   field.size) != 0)
                                                target.fieldFlashUntil[i] = nowSec + kSdkFieldFlashSeconds;
                                }
                        }
                        target.prevData = bytes;
                }
                /*
                 * 实测速率：一次串口往返才是一帧，所以"12000 Hz"这种设置不可能达到。
                 * 这里按最近 1 秒完成的调用数算实际 Hz，显示在表头，方便判断瓶颈在
                 * 串口带宽/波特率（而不是面板）。
                 */
                ++target.callCount;
                ++target.rateWindowCalls;
                if (!result.ok)
                        ++target.rateWindowFails;
                target.lastCallMs   = callMs;
                target.avgCallMs    = target.avgCallMs <= 0.0f ? callMs : target.avgCallMs * 0.8f + callMs * 0.2f;
                const double nowSec = sdkMonoSeconds();
                // 表里有 sample_count（收到新帧才 +1）就顺便统计真实数据率
                int64_t sampleNow = 0;
                if (result.ok && sdkTableInt(*table, bytes, "sample_count", sampleNow)) {
                        const uint64_t samples = (uint64_t)sampleNow;
                        if (!samples || (target.sampleCountValid && samples == target.lastSampleCount))
                                sampled = false;
                        if (!target.sampleCountValid) {
                                target.sampleCountValid  = true;
                                target.rateWindowSamples = 0;
                        } else if (samples >= target.lastSampleCount) {
                                target.rateWindowSamples += samples - target.lastSampleCount;
                        }
                        target.lastSampleCount = samples;
                }
                if (target.rateWindowStart <= 0.0)
                        target.rateWindowStart = nowSec;
                const double windowSec = nowSec - target.rateWindowStart;
                if (windowSec >= 1.0) {
                        target.measuredHz              = static_cast<float>(target.rateWindowCalls / windowSec);
                        target.rateWindowStart         = nowSec;
                        const uint64_t callsInWindow   = target.rateWindowCalls;
                        const uint64_t fails           = target.rateWindowFails;
                        const uint64_t samplesInWindow = target.rateWindowSamples;
                        target.rateWindowCalls         = 0;
                        target.rateWindowFails         = 0;
                        target.rateWindowSamples       = 0;
                        if (target.sampleCountValid)
                                target.measuredDataHz = static_cast<float>(samplesInWindow / windowSec);
                        /*
                         * 实际速率远低于设定值时，每 2 秒在日志里写一行"实测/每次耗时"，
                         * 这样不用猜：是串口带宽（比如 9600 下 Modbus 约 26 ms/帧），
                         * 还是驱动/SDK 有额外开销。
                         */
                        if (target.autoRead && target.measuredHz > 0.0f && target.measuredHz < rtUpdateHz_ * 0.5f &&
                            nowSec - target.rateLogAt >= 2.0) {
                                target.rateLogAt = nowSec;
                                /*
                                 * 有 sample_count 的表（例如主动上传）：measuredDataHz 才是传感器
                                 * 真正给数据的速率；measuredHz 只是我们调用它的频率。两个都写出来，
                                 * 一眼就能看出"是数据源慢"还是"我们没在跑"。
                                 */
                                const double dataHz = target.sampleCountValid ? target.measuredDataHz : target.measuredHz;
                                char         line[192];
                                int64_t      timeoutMs  = 0;
                                const bool   hasTimeout = sdkTableInt(*table, bytes, "timeout_ms", timeoutMs);
                                if (hasTimeout && timeoutMs > 0 && fails * 2 >= callsInWindow) {
                                        // 失败率高时，慢的主因是每帧都在等超时，把超时值也写出来
                                        snprintf(line,
                                                 sizeof(line),
                                                 "[rate] %s: %.1f Hz data (%.1f Hz calls), %.0f ms per call, "
                                                 "%llu/%llu failed (timeout %lld ms)",
                                                 target.name.c_str(),
                                                 dataHz,
                                                 target.measuredHz,
                                                 target.avgCallMs,
                                                 (unsigned long long)fails,
                                                 (unsigned long long)callsInWindow,
                                                 (long long)timeoutMs);
                                } else {
                                        snprintf(line,
                                                 sizeof(line),
                                                 "[rate] %s: %.1f Hz data (%.1f Hz calls), %.0f ms per call, "
                                                 "%llu/%llu failed",
                                                 target.name.c_str(),
                                                 dataHz,
                                                 target.measuredHz,
                                                 target.avgCallMs,
                                                 (unsigned long long)fails,
                                                 (unsigned long long)callsInWindow);
                                }
                                if (dataLog_.size() >= 400)
                                        dataLog_.pop_front();
                                dataLog_.push_back({sdkLogLine(line), true});
                        }
                }
                /*
                 * 手动调用和状态变化立即记录。同一种自动调用失败最多每秒一条，
                 * 周期写入成功也不逐帧刷日志，避免日志开销反过来拖慢采集。
                 */
                const bool logWorthy = manual || target.callCount == 1 || result.ok != target.prevOk ||
                                       (!result.ok && (result.error != target.status || nowSec - target.errorLogAt >= 1.0));
                if (logWorthy) {
                        if (!result.ok)
                                target.errorLogAt = nowSec;
                        std::string text = target.name;
                        // 操作表（搜索/连接/断开）把这次点的动作也写进日志
                        if (target.actionFieldIdx >= 0 && target.actionFieldIdx < (int)table->fields.size()) {
                                const auto &actionField = table->fields[target.actionFieldIdx];
                                if (actionField.offset + actionField.size <= binding.data.size()) {
                                        const int64_t action = sdkIntegerValue(
                                            binding.data.data() + actionField.offset, actionField.type, actionField.size);
                                        if (action != 0) {
                                                std::string label = std::to_string(action);
                                                if (const CEnumDecl *ed =
                                                        findEnum(parseResult_, sdkEnumTypeName(actionField.rawType))) {
                                                        for (const auto &val : ed->values)
                                                                if (val.val == action)
                                                                        label = sdkActionLabel(val.name);
                                                }
                                                text += "  " + label;
                                        }
                                }
                        }
                        text += write ? "  write -> " : "  read  -> ";
                        text += result.ok ? (result.display.empty() ? std::string("ok") : result.display) : result.error;
                        if (const std::string detail = sdkTableText(*table, bytes, "last_error");
                            !detail.empty() && detail != "OK")
                                text += "   " + detail;
                        if (dataLog_.size() >= 400)
                                dataLog_.pop_front();
                        dataLog_.push_back({sdkLogLine(text), result.ok});
                }
                target.prevOk   = result.ok;
                target.status   = result.ok ? result.display : result.error;
                rootPublished   = target.rootPublished;
                fieldsPublished = target.fieldPublished;
        }
        if (!result.ok)
                return;
        if (onSnapshotLocalBuffers_ && onCompareExchangeLocalBuffer_) {
                if (!accepted && !write)
                        return;
                for (const auto &link : binding.localBindings) {
                        const auto before = localSnapshot.find(link.name);
                        if (before == localSnapshot.end() || before->second.size() < link.size ||
                            link.offset + link.size > bytes.size())
                                continue;
                        auto desired = before->second;
                        memcpy(desired.data(), bytes.data() + link.offset, link.size);
                        // Leave a concurrent user edit intact; the next write tick
                        // compares it with the last successfully published bytes
                        if (onCompareExchangeLocalBuffer_(link.name, before->second, desired)) {
                                if (accepted && sampled && onLocalSample_)
                                        onLocalSample_(link.name, desired, sampleTime);
                                std::lock_guard lock(dataTableMtx_);
                                if (index < dataTables_.size() && dataTables_[index].localBaseline.contains(link.name))
                                        dataTables_[index].localBaseline[link.name] = std::move(desired);
                        }
                }
                return;
        }
        if (write)
                return;
        if (rootPublished && onGetVarBuf_) {
                if (void *buffer = onGetVarBuf_(binding.localName)) {
                        memcpy(buffer, bytes.data(), std::min(bytes.size(), table->totalSize));
                        if (onVarWritten_)
                                onVarWritten_(binding.localName);
                }
        }
        if (onWriteLocalScalar_) {
                for (size_t i = 0; i < table->fields.size() && i < fieldsPublished.size(); ++i) {
                        const auto &field = table->fields[i];
                        if (!fieldsPublished[i] || sdkReservedField(field.name) || field.offset + field.size > bytes.size())
                                continue;
                        if (field.isArray) {
                                const size_t elemSize = ctypeSize(field.arrayElemType);
                                if (!sdkChannelType(field.arrayElemType) || elemSize == 0 || sdkStringArray(field))
                                        continue;
                                for (size_t ai = 0; ai < field.arrayCount; ++ai)
                                        onWriteLocalScalar_(
                                            binding.localName + "." + field.name + "[" + std::to_string(ai) + "]",
                                            sdkFieldValue(
                                                bytes.data() + field.offset + ai * elemSize, field.arrayElemType, elemSize),
                                            ctypeIsFloat(field.arrayElemType));
                        } else if (sdkChannelType(field.type)) {
                                onWriteLocalScalar_(binding.localName + "." + field.name,
                                                    sdkFieldValue(bytes.data() + field.offset, field.type, field.size),
                                                    ctypeIsFloat(field.type));
                        }
                }
        }
}

void
SdkPanel::scheduleDataUpdate()
{
        refreshDataTableBindings();
        if (dataUpdateThread_.joinable())
                return;
        dataUpdateStop_.store(false, std::memory_order_release);
        dataUpdateThread_ = std::thread([this] {
                uint64_t nextTickUs   = 0;
                uint64_t prevPeriodUs = 0;
                while (!dataUpdateStop_.load(std::memory_order_acquire)) {
                        std::vector<std::pair<size_t, bool>> calls;
                        size_t                               manualCount = 0;
                        {
                                std::unique_lock lock(dataTableMtx_);
                                bool             hasAutomaticCalls = false;
                                for (const auto &table : dataTables_)
                                        hasAutomaticCalls =
                                            hasAutomaticCalls ||
                                            (table.autoRead && table.readFnIdx >= 0 && dataTableAutoAllowed(table)) ||
                                            (table.autoWrite && table.writeFnIdx >= 0 && dataTableAutoAllowed(table));

                                if (dataManualCalls_.empty() && !hasAutomaticCalls) {
                                        nextTickUs   = 0;
                                        prevPeriodUs = 0;
                                        if (dataUpdateStop_.load(std::memory_order_acquire))
                                                break;
                                        dataUpdateCv_.wait(lock);
                                        continue;
                                }

                                calls.swap(dataManualCalls_);
                                manualCount          = calls.size();
                                const uint64_t nowUs = get_mono_ts_us();
                                const float    hz    = std::clamp(rtUpdateHz_, 1.0f, 10000.0f);
                                const uint64_t periodUs =
                                    std::max<uint64_t>(100, static_cast<uint64_t>(1000000.0 / static_cast<double>(hz)));
                                if (periodUs != prevPeriodUs) {
                                        nextTickUs   = nowUs;
                                        prevPeriodUs = periodUs;
                                }
                                if (hasAutomaticCalls && (nextTickUs == 0 || nowUs >= nextTickUs)) {
                                        for (size_t i = 0; i < dataTables_.size(); ++i) {
                                                if (dataTables_[i].autoWrite && dataTables_[i].writeFnIdx >= 0 &&
                                                    dataTableAutoAllowed(dataTables_[i]))
                                                        calls.emplace_back(i, true);
                                                if (dataTables_[i].autoRead && dataTables_[i].readFnIdx >= 0 &&
                                                    dataTableAutoAllowed(dataTables_[i]))
                                                        calls.emplace_back(i, false);
                                        }
                                        // Anchor to the requested cadence rather than accumulating
                                        // OS wakeup jitter. Skip missed ticks instead of queuing a burst
                                        nextTickUs = nextTickUs ? nextTickUs + periodUs : nowUs + periodUs;
                                        if (nextTickUs <= nowUs)
                                                nextTickUs = nowUs + periodUs;
                                }

                                if (calls.empty()) {
                                        if (dataUpdateStop_.load(std::memory_order_acquire))
                                                break;
                                        const uint64_t waitUs = nextTickUs > nowUs ? nextTickUs - nowUs : 100;
                                        dataUpdateCv_.wait_for(lock, std::chrono::microseconds(waitUs));
                                        continue;
                                }
                        }

                        dataUpdateBusy_.store(true, std::memory_order_release);
                        for (size_t i = 0; i < calls.size(); ++i) {
                                if (dataUpdateStop_.load(std::memory_order_acquire))
                                        break;
                                runDataTableCall(calls[i].first, calls[i].second, i < manualCount);
                        }
                        dataUpdateBusy_.store(false, std::memory_order_release);
                }
                dataUpdateBusy_.store(false, std::memory_order_release);
        });
}

void
SdkPanel::mergeDataTableEdits(DataTableBinding           &target,
                              const DataTableBinding     &edited,
                              const std::vector<uint8_t> &before,
                              uint64_t                    beforeRevision,
                              const CStructDecl          &table,
                              const std::vector<size_t>  &committedFields)
{
        // The worker may have published newer samples while ImGui was drawing
        // Merge complete edited fields, never the stale snapshot's whole buffer
        if (target.data.size() == before.size() && edited.data.size() == before.size()) {
                for (size_t i = 0; i < table.fields.size(); ++i) {
                        const auto &field = table.fields[i];
                        if (field.offset + field.size > before.size())
                                continue;
                        const bool committed =
                            std::find(committedFields.begin(), committedFields.end(), i) != committedFields.end();
                        if (committed ||
                            memcmp(before.data() + field.offset, edited.data.data() + field.offset, field.size) != 0)
                                memcpy(target.data.data() + field.offset, edited.data.data() + field.offset, field.size);
                }
        }
        target.dataRevision   += edited.dataRevision - beforeRevision;
        target.expanded        = edited.expanded;
        target.rootPublished   = edited.rootPublished;
        target.fieldPublished  = edited.fieldPublished;
        target.fieldEditUntil  = edited.fieldEditUntil;
}

void
SdkPanel::drawDataTablesTab()
{
        /*
         * 调用日志：表格旁边的绿色返回值改成这里滚动显示，带时间戳；失败用红字。
         * 周期读取成功不刷屏（只在从失败恢复时记），失败/手动读取/下发都会记。
         */
        std::deque<DataLogEntry>      logSnapshot;
        std::vector<DataTableBinding> drawTables;
        float                         drawUpdateHz;
        {
                std::lock_guard lock(dataTableMtx_);
                logSnapshot  = dataLog_;
                drawTables   = dataTables_;
                drawUpdateHz = rtUpdateHz_;
        }
        if (std::none_of(
                drawTables.begin(), drawTables.end(), [&](const auto &table) { return table.name == navigationTable_; })) {
                navigationTable_       = drawTables.empty() ? std::string{} : drawTables.front().name;
                navigationTableSelect_ = true;
        }
        const std::string requestedTable = navigationTable_;
        if (ImGui::BeginTabBar("##sdk_data_pages")) {
                for (const auto &table : drawTables) {
                        const char       *label = table.name == "dyn200_port_param" ? tr("Port parameters", "端口参数")
                                                  : table.name == "dyn200_param"    ? tr("Device parameters", "设备参数")
                                                  : table.name == "dyn200_rt"       ? tr("Live data", "实时数据")
                                                                                    : table.name.c_str();
                        const std::string tabId = std::string(label) + "###" + table.name;
                        if (ImGui::BeginTabItem(tabId.c_str(),
                                                nullptr,
                                                navigationTableSelect_ && table.name == requestedTable
                                                    ? ImGuiTabItemFlags_SetSelected
                                                    : ImGuiTabItemFlags_None)) {
                                navigationTable_ = table.name;
                                ImGui::EndTabItem();
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", table.name.c_str());
                }

                ImGui::PushStyleColor(ImGuiCol_Tab, ImGui::GetStyleColorVec4(ImGuiCol_Button));
                const bool activateLog =
                    ImGui::TabItemButton(tr("Call log##sdkCallLog", "调用日志##sdkCallLog"), ImGuiTabItemFlags_Trailing);
                ImGui::PopStyleColor();
                const std::string hoverId = "SdkCallLog###SdkCallLog_" + std::to_string(ImGui::GetID("sdk_log_hover"));
                if (ui::BeginHoverPanel(hoverId.c_str(),
                                        tr("Call log", "调用日志"),
                                        dataLogHover_,
                                        activateLog,
                                        ImGui::GetFontSize() * 52,
                                        ImGui::GetWindowViewport())) {
                        ui::LogText log;
                        for (const auto &entry : logSnapshot) {
                                log.append(entry.text,
                                           entry.ok ? ImGui::GetColorU32(ImGuiCol_Text)
                                                    : ImGui::GetColorU32(ui::ErrorTextColor()));
                                log.append("\n");
                        }
                        ImGui::Checkbox(tr("Auto scroll", "自动滚动"), &dataLogFollow_);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "%s",
                                    tr("Follows new lines. Scroll up in the log to pause it, scroll back to the bottom "
                                       "(or tick this) to resume.",
                                       "新日志自动滚到底。往上翻日志会暂停自动滚动，滚回最底部（或勾选这里）就会恢复。"));
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Clear log", "清空日志"))) {
                                std::lock_guard lock(dataTableMtx_);
                                dataLog_.clear();
                                log = {};
                        }
                        ImGui::SameLine();
                        ui::CopyLogButton(log.text);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s",
                                            tr("SDK call log (newest at the bottom).", "SDK 调用日志（最新的在最下面）。"));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "%s",
                                    tr("Drag the bottom border to adjust the log height.", "拖拽日志区域的底边框可调整高度。"));
                        {
                                const ImVec2 available = ImGui::GetContentRegionAvail();
                                const float  maxWidth  = std::max(1.0f, available.x);
                                const float  maxHeight = std::max(80.0f, ImGui::GetWindowViewport()->WorkSize.y * .65f);
                                ImGui::SetNextWindowSizeConstraints(ImVec2(maxWidth, 80.0f), ImVec2(maxWidth, maxHeight));
                                if (ImGui::BeginChild("##sdk_data_log",
                                                      ImVec2(0.0f, ImGui::GetFontSize() * 14),
                                                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY,
                                                      ImGuiWindowFlags_HorizontalScrollbar)) {
                                        ui::LogView("##sdk_data_log_text", log, ImVec2(-1.0f, -1.0f), &dataLogFollow_);
                                }
                                ImGui::EndChild();
                        }
                        ImGui::End();
                }
                ImGui::EndTabBar();
        }
        if (navigationTableSelect_)
                navigationTable_ = requestedTable;
        navigationTableSelect_ = false;
        std::vector<std::pair<size_t, bool>> manualCalls;
        auto launch = [&manualCalls](size_t index, bool write) { manualCalls.emplace_back(index, write); };

        for (size_t ti = 0; ti < drawTables.size(); ++ti) {
                auto              &binding = drawTables[ti];
                const CStructDecl *table   = dataTableStruct(binding);
                if (!table)
                        continue;
                if (binding.name != navigationTable_)
                        continue;
                const auto          before         = binding.data;
                const uint64_t      beforeRevision = binding.dataRevision;
                std::vector<size_t> committedFields;
                ImGui::PushID(static_cast<int>(ti));
                binding.expanded = true;

                ui::FlowToolbar dataActions;
                const float     actionWidth = ImGui::GetContentRegionAvail().x;
                const float     nameWidth   = dataActions.next(ui::FlowToolbar::buttonWidth(binding.localName.c_str()));
                ImGui::Selectable(
                    binding.localName.c_str(), false, ImGuiSelectableFlags_None, ImVec2(nameWidth, ImGui::GetFrameHeight()));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Drag the whole table to link automatic reading and writing",
                                             "拖动整个数据表，自动读取并下发修改"));
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        if (onCreateLocalVar_)
                                onCreateLocalVar_(binding.localName, CType::Unknown, table);
                        if (onPublishLocalEnums_) {
                                const auto memberEnums = collectTableEnums(binding.localName, *table);
                                if (!memberEnums.empty())
                                        onPublishLocalEnums_(binding.localName, memberEnums);
                        }
                        binding.rootPublished = true;
                        StructChannelPayload payload{};
                        snprintf(payload.device, sizeof(payload.device), "LOCAL");
                        snprintf(payload.rootName, sizeof(payload.rootName), "%s", binding.localName.c_str());
                        payload.writable = true;
                        for (const auto &field : table->fields) {
                                if (sdkReservedField(field.name) || sdkStringArray(field))
                                        continue;
                                const size_t count    = field.isArray ? field.arrayCount : 1;
                                const CType  type     = field.isArray ? field.arrayElemType : field.type;
                                const size_t elemSize = field.isArray ? ctypeSize(type) : field.size;
                                const char  *chType   = sdkChannelType(type);
                                if (!chType || elemSize == 0)
                                        continue;
                                const CEnumDecl *fieldEnum =
                                    !field.isArray ? findEnum(parseResult_, sdkEnumTypeName(field.rawType)) : nullptr;
                                for (size_t ai = 0; ai < count && payload.count < StructChannelPayload::kMaxEntries; ++ai) {
                                        auto             &entry = payload.entries[payload.count++];
                                        const std::string name  = binding.localName + "." + field.name +
                                                                 (field.isArray ? "[" + std::to_string(ai) + "]" : "");
                                        snprintf(entry.name, sizeof(entry.name), "%s", name.c_str());
                                        snprintf(entry.type, sizeof(entry.type), "%s", chType);
                                        entry.addr     = field.offset + ai * elemSize;
                                        entry.writable = true;
                                        if (fieldEnum) {
                                                entry.numEnums = (u8)std::min((int)fieldEnum->values.size(),
                                                                              StructChannelPayload::kMaxEnums);
                                                for (int k = 0; k < entry.numEnums; ++k) {
                                                        snprintf(entry.enums[k].name,
                                                                 sizeof(entry.enums[k].name),
                                                                 "%s",
                                                                 fieldEnum->values[k].name.c_str());
                                                        entry.enums[k].val = fieldEnum->values[k].val;
                                                }
                                        }
                                }
                        }
                        ImGui::SetDragDropPayload("STRUCT_CHANNEL", &payload, sizeof(payload));
                        ImGui::Text("%s", binding.localName.c_str());
                        ImGui::EndDragDropSource();
                }

                /*
                 * 操作表（只读表 + action 枚举，例如串口表）：把枚举值渲染成按钮
                 * （搜索/连接/断开…），点一下就把该动作写进表格缓冲并调用读函数，
                 * 由 SDK 去搜索/连接。动作是一次性的，所以这张表不提供自动读取。
                 */
                if (binding.actionFieldIdx >= 0 && binding.actionFieldIdx < (int)table->fields.size()) {
                        const CFieldDecl *actionField = &table->fields[binding.actionFieldIdx];
                        const CEnumDecl  *actionEnum  = findEnum(parseResult_, sdkEnumTypeName(actionField->rawType));
                        if (actionEnum) {
                                std::vector<CEnumValue> ordered;
                                for (const auto &val : actionEnum->values)
                                        if (val.val != 0)
                                                ordered.push_back(val);
                                std::stable_sort(ordered.begin(), ordered.end(), [](const CEnumValue &a, const CEnumValue &b) {
                                        return sdkActionPriority(a.name) < sdkActionPriority(b.name);
                                });
                                for (const auto &val : ordered) {
                                        dataActions.next(sdkActionLabel(val.name).c_str());
                                        ImGui::PushID(static_cast<int>(val.val));
                                        if (ImGui::Button(sdkActionLabel(val.name).c_str()) &&
                                            actionField->offset + actionField->size <= binding.data.size()) {
                                                sdkSetIntegerValue(
                                                    binding.data.data() + actionField->offset, actionField->size, val.val);
                                                ++binding.dataRevision;
                                                committedFields.push_back(static_cast<size_t>(binding.actionFieldIdx));
                                                modified_ = true;
                                                launch(ti, false);
                                        }
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s",
                                                                  tr("Runs this action through the table's read function.",
                                                                     "通过这张表的读取函数执行该动作。"));
                                        ImGui::PopID();
                                }
                        }
                }
                if (binding.readFnIdx >= 0 && binding.actionFieldIdx < 0) {
                        dataActions.next(tr("Read", "读取"));
                        if (ImGui::Button(tr("Read", "读取")))
                                launch(ti, false);
                }
                if (binding.writeFnIdx >= 0 && binding.actionFieldIdx < 0) {
                        dataActions.next(tr("Write", "写入"));
                        if (ImGui::Button(tr("Write", "写入")))
                                launch(ti, true);
                }
                if (dataTableAutoAllowed(binding)) {
                        /*
                         * 更新频率滑块（实时表专用）+ 一个数据速率显示。放在自动写入
                         * 右边，样式与变量记录器 scope 的频率一致（淡蓝 "%.0f Hz"）。
                         */
                        float displayedHz = drawUpdateHz;
                        ImGui::SetNextItemWidth(dataActions.next(ImGui::GetFontSize() * 8));
                        if (ImGui::SliderFloat(tr("##sdk_rt_hz", "##sdk_rt_hz"),
                                               &displayedHz,
                                               1.0f,
                                               10000.0f,
                                               tr("%.0f Hz", "%.0f Hz"),
                                               ImGuiSliderFlags_Logarithmic)) {
                                drawUpdateHz = std::clamp(displayedHz, 1.0f, 10000.0f);
                                modified_    = true;
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Update rate for linked variables.", "已拖入变量的同步频率。"));
                        const float dataHz = binding.sampleCountValid ? binding.measuredDataHz : binding.measuredHz;
                        dataActions.next(ImGui::CalcTextSize("99999 Hz").x);
                        if (dataHz > 0.1f)
                                ui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%.0f Hz", dataHz);
                        else
                                ImGui::TextDisabled("-- Hz");
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "%s",
                                    tr("Actual data rate: new samples per second.", "实际数据速率：每秒真正拿到的新数据。"));
                }
                const bool serialPicker = binding.portCountFnIdx >= 0 && binding.portNameFnIdx >= 0;
                if (serialPicker) {
                        const auto  selected = sdkTableText(*table, binding.data, "port");
                        std::string preview  = selected;
                        bool        found    = false;
                        for (const auto &port : binding.serialPorts)
                                if (port.name == selected) {
                                        found = true;
                                        if (!port.description.empty())
                                                preview += " - " + sdkSerialDescription(port.description);
                                        break;
                                }
                        if (preview.empty())
                                preview = binding.portsScanned ? tr("No serial ports found", "未找到串口")
                                                               : tr("Search first", "请先搜索");
                        else if (binding.portsScanned && !found && binding.portScanError.empty())
                                preview += tr(" (not in search results)", "（未在搜索结果中）");
                        const char *portLabel = tr("Serial port##sdk_port_picker", "串口选择##sdk_port_picker");
                        const float portLabelWidth =
                            ImGui::CalcTextSize(portLabel, nullptr, true).x + ImGui::GetStyle().ItemInnerSpacing.x;
                        const float portWidth = std::max(
                            1.0f,
                            std::min(actionWidth - portLabelWidth,
                                     std::clamp(actionWidth * .4f, ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 24)));
                        dataActions.next(portWidth + portLabelWidth);
                        ImGui::SetNextItemWidth(portWidth);
                        ImGui::BeginDisabled(binding.serialPorts.empty());
                        if (ImGui::BeginCombo(portLabel, preview.c_str())) {
                                for (const auto &port : binding.serialPorts) {
                                        ImGui::PushID(static_cast<int>(port.index));
                                        const auto label =
                                            port.name +
                                            (port.description.empty() ? "" : " - " + sdkSerialDescription(port.description));
                                        const bool isSelected = selected == port.name;
                                        if (ImGui::Selectable(label.c_str(), isSelected)) {
                                                const auto edited = selectSerialPort(binding, *table, port);
                                                committedFields.insert(committedFields.end(), edited.begin(), edited.end());
                                                ++binding.dataRevision;
                                                modified_ = true;
                                        }
                                        if (isSelected)
                                                ImGui::SetItemDefaultFocus();
                                        ImGui::PopID();
                                }
                                ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip(
                                    "%s",
                                    tr("Search lists all serial adapters. Select one, then click Connect. You can also edit "
                                       "port below.",
                                       "搜索列出全部串口适配器；选好后点击连接。也可在下面的 port 字段手动填写。"));
                        if (binding.portsScanned) {
                                dataActions.next(ImGui::CalcTextSize("999 个串口").x);
                                ImGui::TextDisabled(tr("%zu ports", "%zu 个串口"), binding.serialPorts.size());
                        }
                        if (!binding.portScanError.empty())
                                ui::ErrorText("%s", binding.portScanError.c_str());
                }
                const ImGuiTableFlags fieldTableFlags = ui::DataTableFlags | ImGuiTableFlags_Resizable |
                                                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings |
                                                        ImGuiTableFlags_ScrollX;
                if (ui::TableIsland tableCard;
                    tableCard.begin("##sdk_data_fields_v4",
                                    3,
                                    fieldTableFlags,
                                    ImVec2(0, 0),
                                    std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 42.0f))) {
                        ImGui::TableSetupColumn(
                            tr("Field", "字段"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0.35f);
                        ImGui::TableSetupColumn(
                            tr("Value", "值"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0.65f);
                        ImGui::TableSetupColumn(tr("Type", "类型"),
                                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                                ui::TableColumnWidth(tr("Type", "类型"), "uint64_t"));
                        ImGui::TableHeadersRow();
                        std::string  activeGroup;
                        bool         activeGroupOpen = true;
                        const double nowSec          = sdkMonoSeconds();
                        for (size_t fi = 0; fi < table->fields.size(); ++fi) {
                                const auto &field = table->fields[fi];
                                if (sdkReservedField(field.name))
                                        continue;
                                // 操作表的 action 已经渲染成按钮了，字段表里不再重复一行
                                if (binding.actionFieldIdx >= 0 && (int)fi == binding.actionFieldIdx)
                                        continue;
                                if (serialPicker && field.name == "index")
                                        continue; // selected by the named port dropdown
                                std::string  displayFieldName = field.name;
                                const size_t groupSeparator   = field.name.find('.');
                                if (groupSeparator != std::string::npos) {
                                        const std::string group = field.name.substr(0, groupSeparator);
                                        displayFieldName        = field.name.substr(groupSeparator + 1);
                                        if (group != activeGroup) {
                                                activeGroup = group;
                                                ImGui::PushID(group.c_str());
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);

                                                activeGroupOpen = ImGui::TreeNodeEx("##nested_group",
                                                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                                        ImGuiTreeNodeFlags_SpanAllColumns,
                                                                                    "%s",
                                                                                    group.c_str());
                                                ImGui::TableSetColumnIndex(2);
                                                ImGui::TextDisabled("struct");
                                                ImGui::PopID();
                                        }
                                        if (!activeGroupOpen)
                                                continue;
                                } else {
                                        activeGroup.clear();
                                        activeGroupOpen = true;
                                }
                                ImGui::PushID(static_cast<int>(fi));
                                const bool   stringArray = sdkStringArray(field);
                                const size_t rowCount    = field.isArray && !stringArray ? field.arrayCount : 1;
                                const CType  rowType     = field.isArray ? field.arrayElemType : field.type;
                                const size_t rowSize     = field.isArray ? ctypeSize(rowType) : field.size;
                                for (size_t ai = 0; ai < rowCount; ++ai) {
                                        ImGui::PushID(static_cast<int>(ai));
                                        ImGui::TableNextRow();
                                        // 这个字段刚读回来的值和上一次不一样：底色闪一下
                                        if (fi < binding.fieldFlashUntil.size() && fi < binding.fieldEditUntil.size()) {
                                                const double flashUntil = binding.fieldFlashUntil[fi];
                                                if (flashUntil > nowSec && binding.fieldEditUntil[fi] <= nowSec) {
                                                        const float decay = static_cast<float>(std::clamp(
                                                            (flashUntil - nowSec) / kSdkFieldFlashSeconds, 0.0, 1.0));
                                                        const float blink =
                                                            0.55f + 0.45f * std::cos(static_cast<float>(nowSec) * 20.0f);
                                                        const float alpha = 0.55f * decay * blink;
                                                        ImGui::TableSetBgColor(
                                                            ImGuiTableBgTarget_RowBg0,
                                                            ImGui::GetColorU32(ImVec4(0.25f, 0.75f, 1.0f, alpha)));
                                                }
                                        }
                                        ImGui::TableSetColumnIndex(0);
                                        const char       *chType = stringArray ? nullptr : sdkChannelType(rowType);
                                        const std::string displayName =
                                            displayFieldName +
                                            (field.isArray && !stringArray ? "[" + std::to_string(ai) + "]" : "");
                                        if (groupSeparator != std::string::npos)
                                                ImGui::Indent();
                                        if (chType)
                                                ImGui::Selectable(displayName.c_str(),
                                                                  false,
                                                                  ImGuiSelectableFlags_AllowOverlap,
                                                                  ImVec2(0, ImGui::GetFrameHeight()));
                                        else
                                                ui::TableText(displayName);
                                        if (groupSeparator != std::string::npos)
                                                ImGui::Unindent();
                                        if (chType && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                                                const std::string suffix = field.isArray ? "[" + std::to_string(ai) + "]" : "";
                                                const std::string localField = binding.localName + "." + field.name + suffix;
                                                if (onCreateLocalVar_)
                                                        onCreateLocalVar_(localField, rowType, nullptr);
                                                binding.fieldPublished[fi] = true;
                                                ChannelDropPayload payload{};
                                                snprintf(payload.name, sizeof(payload.name), "%s", localField.c_str());
                                                snprintf(payload.device, sizeof(payload.device), "LOCAL");
                                                snprintf(payload.type, sizeof(payload.type), "%s", chType);
                                                payload.numBytes = static_cast<u8>(rowSize);
                                                payload.writable = true;
                                                if (const CEnumDecl *dragEnum =
                                                        !field.isArray ? findEnum(parseResult_, sdkEnumTypeName(field.rawType))
                                                                       : nullptr) {
                                                        payload.numEnums = (u8)std::min((int)dragEnum->values.size(),
                                                                                        ChannelDropPayload::kMaxEnums);
                                                        for (int k = 0; k < payload.numEnums; ++k) {
                                                                snprintf(payload.enums[k].name,
                                                                         sizeof(payload.enums[k].name),
                                                                         "%s",
                                                                         dragEnum->values[k].name.c_str());
                                                                payload.enums[k].val = dragEnum->values[k].val;
                                                        }
                                                }
                                                ImGui::SetDragDropPayload("CHANNEL", &payload, sizeof(payload));
                                                ImGui::TextUnformatted(localField.c_str());
                                                ImGui::EndDragDropSource();
                                        }
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::SetNextItemWidth(-FLT_MIN);
                                        if (field.offset + field.size <= binding.data.size()) {
                                                bool             committed  = false;
                                                bool             directEdit = false;
                                                const CEnumDecl *fieldEnum =
                                                    !field.isArray ? findEnum(parseResult_, sdkEnumTypeName(field.rawType))
                                                                   : nullptr;
                                                if (stringArray) {
                                                        auto *text =
                                                            reinterpret_cast<char *>(binding.data.data() + field.offset);
                                                        text[field.size - 1] = '\0';
                                                        committed            = ImGui::InputText(
                                                            "##value", text, field.size, ImGuiInputTextFlags_EnterReturnsTrue);
                                                } else if (fieldEnum && !fieldEnum->values.empty()) {
                                                        uint8_t *valueData = binding.data.data() + field.offset + ai * rowSize;
                                                        const int64_t current = sdkIntegerValue(valueData, rowType, rowSize);
                                                        const CEnumValue *selected = nullptr;
                                                        for (const auto &candidate : fieldEnum->values)
                                                                if (candidate.val == current) {
                                                                        selected = &candidate;
                                                                        break;
                                                                }
                                                        const std::string unknown = std::to_string(current);
                                                        const char       *preview =
                                                            selected ? selected->name.c_str() : unknown.c_str();
                                                        if (ImGui::BeginCombo("##value", preview)) {
                                                                for (const auto &candidate : fieldEnum->values) {
                                                                        const bool isSelected = candidate.val == current;
                                                                        char       label[160];
                                                                        snprintf(label,
                                                                                 sizeof(label),
                                                                                 "%s (%lld)",
                                                                                 candidate.name.c_str(),
                                                                                 (long long)candidate.val);
                                                                        if (ImGui::Selectable(label, isSelected)) {
                                                                                sdkSetIntegerValue(
                                                                                    valueData, rowSize, candidate.val);
                                                                                committed  = true;
                                                                                directEdit = true;
                                                                        }
                                                                        if (isSelected)
                                                                                ImGui::SetItemDefaultFocus();
                                                                }
                                                                ImGui::EndCombo();
                                                        }
                                                } else {
                                                        const char *numberFormat =
                                                            field.name == "control.set_control_word" ? "0x%08X" : nullptr;
                                                        uint8_t *valueData = binding.data.data() + field.offset + ai * rowSize;
                                                        // InputScalar writes the parsed text back through its data
                                                        // pointer on every keystroke, and this byte range is exactly
                                                        // what the auto-write tick ships to the device — typing "25"
                                                        // would send 2 first. Edit a scratch copy instead and fold it
                                                        // back only once the user commits. Every scalar CType handled
                                                        // by sdkEditValue is 8 bytes or fewer
                                                        alignas(8) uint8_t scratch[8]{};
                                                        const size_t copySize = std::min<size_t>(rowSize, sizeof(scratch));
                                                        memcpy(scratch, valueData, copySize);
                                                        const bool valueChanged =
                                                            sdkEditValue("##value", scratch, rowType, copySize, numberFormat);
                                                        const bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                                                                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
                                                        // Bool renders as a checkbox — the toggle itself is the commit
                                                        committed = rowType == CType::Bool
                                                                        ? valueChanged
                                                                        : ((ImGui::IsItemActive() || ImGui::IsItemFocused() ||
                                                                            ImGui::IsItemDeactivatedAfterEdit()) &&
                                                                           enterPressed);
                                                        if (committed)
                                                                memcpy(valueData, scratch, copySize);
                                                }
                                                const bool itemEdited = directEdit || ImGui::IsItemEdited();
                                                // Bump the revision on every keystroke so the auto-read tick
                                                // does not overwrite text mid-edit. Merely having focus (cursor
                                                // parked in the field, nothing typed) must NOT bump this: the
                                                // manual "Read now" call snapshots the revision before the SDK
                                                // call and only publishes its result if the revision still
                                                // matches afterwards, so a field left focused-but-idle would
                                                // otherwise silently block every read for the whole table
                                                if (itemEdited) {
                                                        ++binding.dataRevision;
                                                        // 编辑期间和刚编辑完的一小段时间里不闪，免得打字时背景乱跳
                                                        if (fi < binding.fieldEditUntil.size())
                                                                binding.fieldEditUntil[fi] = nowSec + kSdkFieldEditHoldSeconds;
                                                        modified_ = true;
                                                }
                                                if (committed && binding.writeFnIdx >= 0) {
                                                        committedFields.push_back(fi);
                                                        // Enter can commit an unchanged value, which still must
                                                        // take precedence over an in-flight read/write result
                                                        if (!itemEdited)
                                                                ++binding.dataRevision;
                                                        uint32_t inferredMode = 0;
                                                        if (field.name == "set_pvct.pos")
                                                                inferredMode = 3;
                                                        else if (field.name == "set_pvct.vel")
                                                                inferredMode = 2;
                                                        else if (field.name == "set_pvct.cur")
                                                                inferredMode = 1;
                                                        else if (field.name == "set_pvct.tor")
                                                                inferredMode = 4;
                                                        if (inferredMode != 0) {
                                                                for (const auto &mode : table->fields) {
                                                                        if (mode.name != "control.set_control_mode" ||
                                                                            mode.isArray ||
                                                                            mode.offset + mode.size > binding.data.size())
                                                                                continue;
                                                                        memcpy(binding.data.data() + mode.offset,
                                                                               &inferredMode,
                                                                               std::min(mode.size, sizeof(inferredMode)));
                                                                        committedFields.push_back(
                                                                            static_cast<size_t>(&mode - table->fields.data()));
                                                                        break;
                                                                }
                                                        }
                                                        const size_t separator = field.name.find('.');
                                                        if (separator != std::string::npos) {
                                                                const std::string gateName =
                                                                    "write_" + field.name.substr(0, separator);
                                                                for (const auto &gate : table->fields) {
                                                                        if (gate.name != gateName || gate.isArray ||
                                                                            gate.offset + gate.size > binding.data.size())
                                                                                continue;
                                                                        const uint64_t enabled = 1;
                                                                        memcpy(binding.data.data() + gate.offset,
                                                                               &enabled,
                                                                               std::min(gate.size, sizeof(enabled)));
                                                                        committedFields.push_back(
                                                                            static_cast<size_t>(&gate - table->fields.data()));
                                                                        break;
                                                                }
                                                        }
                                                        launch(ti, true);
                                                }
                                        } else {
                                                ImGui::TextDisabled("-");
                                        }
                                        ImGui::TableSetColumnIndex(2);
                                        const CEnumDecl *typeEnum =
                                            !field.isArray ? findEnum(parseResult_, sdkEnumTypeName(field.rawType)) : nullptr;
                                        ui::TableText(stringArray ? "char[]" : sdkFieldTypeLabel(rowType));
                                        if (typeEnum && ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", typeEnum->name.c_str());
                                        ImGui::PopID();
                                }
                                ImGui::PopID();
                        }
                        tableCard.end();
                }
                ImGui::PopID();
                bool wakeWorker = !manualCalls.empty();
                {
                        std::lock_guard lock(dataTableMtx_);
                        auto           &target = dataTables_[ti];
                        wakeWorker             = wakeWorker || target.autoRead != binding.autoRead ||
                                     target.autoWrite != binding.autoWrite || rtUpdateHz_ != drawUpdateHz;
                        mergeDataTableEdits(target, binding, before, beforeRevision, *table, committedFields);
                        rtUpdateHz_ = drawUpdateHz;
                        dataManualCalls_.insert(dataManualCalls_.end(), manualCalls.begin(), manualCalls.end());
                }
                manualCalls.clear();
                if (wakeWorker)
                        dataUpdateCv_.notify_one();
        }
}

// ─── selectFn / selectMethod ──────────────────────────────────────────────────

void
SdkPanel::selectFn(int idx)
{
        // Save current args before switching
        if (selectedFnIdx_ >= 0 && selectedFnIdx_ < (int)parseResult_.functions.size())
                fnArgBufsCache_[selectedFnIdx_] = fnArgBufs_;

        selectedFnIdx_ = idx;
        fnLastResult_.clear();
        fnLastResultOk_ = false;
        fnResultVar_[0] = '\0'; // result-var name is per-function, not shared
        if (idx < 0 || idx >= (int)parseResult_.functions.size()) {
                fnArgBufs_.clear();
                return;
        }
        const CFuncDecl &fn = parseResult_.functions[idx];
        auto             it = fnArgBufsCache_.find(idx);
        if (it != fnArgBufsCache_.end() && it->second.size() == fn.params.size()) {
                fnArgBufs_ = it->second;
        } else {
                fnArgBufs_.resize(fn.params.size());
                for (auto &b : fnArgBufs_)
                        b = {};
        }
}

void
SdkPanel::selectMethod(int classIdx, int methodIdx)
{
        // Save current args before switching
        if (selectedClassIdx_ >= 0 && selectedMethodIdx_ >= 0) {
                int key                = selectedClassIdx_ * 10000 + selectedMethodIdx_;
                methArgBufsCache_[key] = methArgBufs_;
                structBufsCache_[key]  = structBufs_;
        }

        selectedClassIdx_  = classIdx;
        selectedMethodIdx_ = methodIdx;
        methResultVar_[0]  = '\0'; // result-var name is per-method, not shared

        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size()) {
                methArgBufs_.clear();
                structBufs_.clear();
                return;
        }
        const CClassDecl &cls = parseResult_.classes[classIdx];
        if (methodIdx < 0 || methodIdx >= (int)cls.methods.size()) {
                methArgBufs_.clear();
                structBufs_.clear();
                return;
        }

        const CMethodDecl &meth = cls.methods[methodIdx];
        int                key  = classIdx * 10000 + methodIdx;

        auto ita = methArgBufsCache_.find(key);
        if (ita != methArgBufsCache_.end() && ita->second.size() == meth.params.size()) {
                methArgBufs_ = ita->second;
                auto its     = structBufsCache_.find(key);
                if (its != structBufsCache_.end() && its->second.size() == meth.params.size())
                        structBufs_ = its->second;
                else {
                        structBufs_.resize(meth.params.size());
                        for (size_t i = 0; i < meth.params.size(); ++i) {
                                if (!structBufs_[i].decl) {
                                        const CStructDecl *sd = findParamStruct(meth.params[i]);
                                        if (sd && sd->isPOD && sd->totalSize > 0) {
                                                structBufs_[i].decl = sd;
                                                if (structBufs_[i].data.empty())
                                                        structBufs_[i].data.assign(sd->totalSize, 0);
                                        }
                                }
                        }
                }
        } else {
                methArgBufs_.resize(meth.params.size());
                structBufs_.resize(meth.params.size());
                for (size_t i = 0; i < meth.params.size(); ++i) {
                        methArgBufs_[i]       = {};
                        structBufs_[i]        = {};
                        const CStructDecl *sd = findParamStruct(meth.params[i]);
                        if (sd && sd->isPOD && sd->totalSize > 0) {
                                structBufs_[i].decl = sd;
                                structBufs_[i].data.assign(sd->totalSize, 0);
                        }
                }
        }
}

// ─── doCallC ─────────────────────────────────────────────────────────────────

void
SdkPanel::doCallC()
{
        if (selectedFnIdx_ < 0 || selectedFnIdx_ >= (int)parseResult_.functions.size())
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CFuncDecl         &fn = parseResult_.functions[selectedFnIdx_];
        std::vector<std::string> args(fn.params.size());
        for (size_t i = 0; i < fn.params.size(); ++i)
                args[i] = resolveArgVar(fnArgBufs_[i].text, fn.params[i]);

        CallResult res;
        {
                std::lock_guard callLock(sdkCallMtx_);
                res = loader_.call(fn, args);
        }

        std::string callStr = fn.name + "(";
        for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                        callStr += ", ";
                callStr += args[i];
        }
        callStr += ")";

        fnLastResult_   = res.ok ? res.display : res.error;
        fnLastResultOk_ = res.ok;

        double tsSec = sessionTimeSec();
        pushHistory(callStr, fnLastResult_, res.ok, tsSec);
        // Store the return value only into the user-chosen result variable (created
        // manually via the "+" button); no auto-creation
        if (res.ok)
                writeResultVar(fnResultVar_, res, fn.retType);
}

// ─── doCallMethod ─────────────────────────────────────────────────────────────

void
SdkPanel::doCallMethod()
{
        if (selectedClassIdx_ < 0 || selectedMethodIdx_ < 0)
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CClassDecl  &cls  = parseResult_.classes[selectedClassIdx_];
        const CMethodDecl &meth = cls.methods[selectedMethodIdx_];

        // Determine `this` pointer
        void *thisPtr = nullptr;
        if (!meth.isCtor) {
                if (selectedObjIdx_ < 0 || selectedObjIdx_ >= (int)objects_.size()) {
                        setStatus(tr("Select an object first", "请先选择一个对象"), true);
                        return;
                }
                thisPtr = objects_[selectedObjIdx_].ptr;
        }

        std::vector<std::string> args(meth.params.size());
        std::vector<void *>      structPtrs(meth.params.size(), nullptr);
        // Default struct buffers come from the inline field editor
        for (size_t i = 0; i < structBufs_.size(); ++i) {
                if (structBufs_[i].decl && !structBufs_[i].data.empty())
                        structPtrs[i] = structBufs_[i].data.data();
        }
        for (size_t i = 0; i < meth.params.size(); ++i) {
                const std::string raw = methArgBufs_[i].text;
                // "&var" bound to a struct param → use that LOCAL variable's buffer instead
                // of the inline editor's buffer
                if (raw.size() > 1 && raw[0] == '&' && findParamStruct(meth.params[i]) && onGetVarBuf_) {
                        if (void *buf = onGetVarBuf_(raw.substr(1))) {
                                structPtrs[i] = buf;
                                args[i]       = raw;
                                continue;
                        }
                }
                args[i] = resolveArgVar(raw, meth.params[i]);
        }

        CallResult res;
        {
                std::lock_guard callLock(sdkCallMtx_);
                res = loader_.callMethod(thisPtr, meth, args, structPtrs);
        }

        // Build call string
        std::string callStr = cls.name + "::" + meth.name + "(";
        for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                        callStr += ", ";
                if (structPtrs[i])
                        callStr += "[struct]";
                else
                        callStr += args[i];
        }
        callStr += ")";

        // Look up enum name for integer return values
        std::string display = res.ok ? res.display : res.error;
        if (res.ok && !meth.retRaw.empty()) {
                const CEnumDecl *ed = findEnum(parseResult_, meth.retRaw, cls.fullName);
                if (ed) {
                        for (const auto &ev : ed->values) {
                                if (ev.val == (int64_t)res.rawU64) {
                                        char tmp[64];
                                        snprintf(tmp, sizeof(tmp), "%lld (%s)", (long long)ev.val, ev.name.c_str());
                                        display = tmp;
                                        break;
                                }
                        }
                }
        }

        double tsSec = sessionTimeSec();
        pushHistory(callStr, display, res.ok, tsSec);
        if (!res.ok)
                setStatus(res.error, true);
        // Store the return value only into the user-chosen result variable
        if (res.ok)
                writeResultVar(methResultVar_, res, meth.retType);
}

// ─── doNewObject ──────────────────────────────────────────────────────────────

void
SdkPanel::doNewObject(int classIdx)
{
        stopDataUpdate();
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CClassDecl &cls = parseResult_.classes[classIdx];

        // Allocate a generous buffer for the object
        size_t sz  = cls.instanceSize > 0 ? cls.instanceSize : 4096;
        void  *mem = malloc(sz);
        if (!mem) {
                setStatus("malloc failed", true);
                return;
        }
        memset(mem, 0, sz);

        // Find and call the default constructor
        const CMethodDecl *ctorDecl = nullptr;
        for (const auto &m : cls.methods) {
                if (m.isCtor && m.params.empty()) {
                        ctorDecl = &m;
                        break;
                }
        }
        if (!ctorDecl) {
                // Try any constructor (might take args)
                for (const auto &m : cls.methods) {
                        if (m.isCtor) {
                                ctorDecl = &m;
                                break;
                        }
                }
        }

        int  fieldsSet           = 0;
        bool ctorCalledViaSymbol = false;

        if (ctorDecl) {
                std::vector<std::string> noArgs(ctorDecl->params.size(), "0");
                std::vector<void *>      noPtrs(ctorDecl->params.size(), nullptr);
                CallResult               r;
                {
                        std::lock_guard callLock(sdkCallMtx_);
                        r = loader_.callMethod(mem, *ctorDecl, noArgs, noPtrs);
                }
                if (r.ok) {
                        ctorCalledViaSymbol = true;
                } else {
                        // Symbol not found → constructor is likely inline (defined in the
                        // header, not a standalone export).  Apply inline body assignments
                        // to emulate the constructor
                        fieldsSet = applyInlineBody(mem, sz, cls, *ctorDecl, parseResult_);
                }
        }
        // No ctor decl or inline ctor → zero-initialized buffer is used

        ObjInstance obj;
        snprintf(obj.label, sizeof(obj.label), "obj%d", (int)objects_.size());
        obj.ptr       = mem;
        obj.className = cls.fullName;
        objects_.push_back(std::move(obj));
        selectedObjIdx_ = (int)objects_.size() - 1;

        char buf[256];
        if (ctorCalledViaSymbol) {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (ctor called)", "已创建 %s @ %p (已调用构造函数)"),
                         cls.name.c_str(),
                         mem);
        } else if (fieldsSet > 0) {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (inline ctor: %d fields set)", "已创建 %s @ %p (内联构造: %d 字段已写入)"),
                         cls.name.c_str(),
                         mem,
                         fieldsSet);
        } else {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (zero-init, ctor not found)", "已创建 %s @ %p (零初始化, 未找到构造函数)"),
                         cls.name.c_str(),
                         mem);
        }
        setStatus(buf, fieldsSet == 0 && !ctorCalledViaSymbol);
}

// ─── doDeleteObject ───────────────────────────────────────────────────────────

void
SdkPanel::doDeleteObject(int idx)
{
        stopDataUpdate();
        if (idx < 0 || idx >= (int)objects_.size())
                return;

        // Try to call destructor
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size()) {
                const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
                for (const auto &m : cls.methods) {
                        if (m.isDtor) {
                                std::vector<std::string> noArgs;
                                std::vector<void *>      noPtrs;
                                std::lock_guard          callLock(sdkCallMtx_);
                                loader_.callMethod(objects_[idx].ptr, m, noArgs, noPtrs);
                                break;
                        }
                }
        }
        free(objects_[idx].ptr);
        objects_.erase(objects_.begin() + idx);
        if (selectedObjIdx_ >= (int)objects_.size())
                selectedObjIdx_ = (int)objects_.size() - 1;
}

// ─── lookup helpers ───────────────────────────────────────────────────────────

const CEnumDecl *
SdkPanel::findParamEnum(const CParam &p) const
{
        std::string raw = p.rawType;
        // Strip trailing *, &, const, spaces
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        // Strip leading const
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        std::string classFullName;
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size())
                classFullName = parseResult_.classes[selectedClassIdx_].fullName;
        return findEnum(parseResult_, raw, classFullName);
}

const CEnumDecl *
SdkPanel::getParamEnumDecl(bool isCFunc, int classIdx, int methodIdx, int paramIdx) const
{
        std::string raw;
        std::string classFullName;
        if (isCFunc) {
                if (methodIdx < 0 || methodIdx >= (int)parseResult_.functions.size())
                        return nullptr;
                const CFuncDecl &fn = parseResult_.functions[methodIdx];
                if (paramIdx < 0 || paramIdx >= (int)fn.params.size())
                        return nullptr;
                raw = fn.params[paramIdx].rawType;
        } else {
                if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                        return nullptr;
                const CClassDecl &cls = parseResult_.classes[classIdx];
                classFullName         = cls.fullName;
                if (methodIdx < 0 || methodIdx >= (int)cls.methods.size())
                        return nullptr;
                const CMethodDecl &m = cls.methods[methodIdx];
                if (paramIdx < 0 || paramIdx >= (int)m.params.size())
                        return nullptr;
                raw = m.params[paramIdx].rawType;
        }
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        return findEnum(parseResult_, raw, classFullName);
}

const CStructDecl *
SdkPanel::findParamStruct(const CParam &p) const
{
        std::string raw = p.rawType;
        // Must be a reference/pointer type to be a struct param
        if (raw.find('&') == std::string::npos && raw.find('*') == std::string::npos)
                return nullptr;
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        return findStruct(parseResult_, raw);
}

// ─── resolveArgVar ────────────────────────────────────────────────────────────

std::string
SdkPanel::resolveArgVar(const std::string &raw, const CParam &p) const
{
        if (raw.size() < 2)
                return raw;
        if (raw[0] == '&' && onGetVarBuf_) {
                // Pointer/reference param bound to a LOCAL buffer — pass its address
                if (void *buf = onGetVarBuf_(raw.substr(1))) {
                        char h[32];
                        snprintf(h, sizeof(h), "0x%llx", (unsigned long long)(uintptr_t)buf);
                        return h;
                }
        } else if (raw[0] == '$' && onReadLocalVar_) {
                // Value param bound to a LOCAL variable — substitute its current value
                double v = 0;
                if (onReadLocalVar_(raw.substr(1), v)) {
                        char b[64];
                        if (ctypeIsFloat(p.type))
                                snprintf(b, sizeof(b), "%g", v);
                        else
                                snprintf(b, sizeof(b), "%lld", (long long)(int64_t)v);
                        return b;
                }
        }
        return raw;
}

// ─── drawParamVarButtons ──────────────────────────────────────────────────────

void
SdkPanel::drawParamVarButtons(char *argBuf, size_t argBufSz, const CParam &p, int salt)
{
        std::string rt = p.rawType;
        while (!rt.empty() && (rt.back() == ' ' || rt.back() == '\t'))
                rt.pop_back();
        bool               isPtr = !rt.empty() && (rt.back() == '*' || rt.back() == '&');
        const CStructDecl *sd    = findParamStruct(p);
        std::string        pn    = p.name.empty() ? ("arg" + std::to_string(salt)) : p.name;

        // "+" — create a LOCAL variable for this parameter
        if (onCreateLocalVar_) {
                ImGui::SameLine(0, 2);
                char id[24];
                snprintf(id, sizeof(id), "+##pc%d", salt);
                if (ImGui::SmallButton(id))
                        onCreateLocalVar_(pn, p.type, sd);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Add this parameter as a variable", "把该参数添加为变量"));
        }

        // "v" — pick an existing LOCAL variable and bind it to this argument
        if (onListLocalVars_) {
                ImGui::SameLine(0, 2);
                char btnId[24], popId[24];
                snprintf(btnId, sizeof(btnId), "v##pv%d", salt);
                snprintf(popId, sizeof(popId), "##pvp%d", salt);
                if (ImGui::SmallButton(btnId))
                        ImGui::OpenPopup(popId);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Use a variable for this argument", "用变量作为该参数"));
                if (ImGui::BeginPopup(popId)) {
                        auto vars = onListLocalVars_();
                        if (vars.empty())
                                ImGui::TextDisabled("%s", tr("(no variables)", "(无变量)"));
                        for (const auto &vn : vars) {
                                if (ImGui::Selectable(vn.c_str())) {
                                        std::string bound = (isPtr ? "&" : "$") + vn;
                                        strncpy(argBuf, bound.c_str(), argBufSz - 1);
                                        argBuf[argBufSz - 1] = '\0';
                                }
                        }
                        ImGui::EndPopup();
                }
        }
}

// ─── Python runner script (embedded) ─────────────────────────────────────────

static const char kPyRunnerScript[] =
    "import sys, json, importlib.util, traceback, ast, os\n"
    "_module = None\n"
    "_objs   = {}\n"
    "_nid    = 0\n"
    // Duplicate stdout FD before any user code can change it
    // All RPC replies go through _rpc_fd so subprocess/print from user code
    // cannot pollute the JSON-RPC pipe even if FD 1 is temporarily redirected
    "_rpc_fd = os.dup(sys.stdout.fileno())\n"
    "\n"
    "def _send(obj):\n"
    "    os.write(_rpc_fd, (json.dumps(obj) + '\\n').encode())\n"
    "\n"
    "def _cvt(v):\n"
    "    if not isinstance(v, str): return v\n"
    "    try: return ast.literal_eval(v)\n"
    "    except Exception: return v\n"
    "\n"
    // Redirect FD 1 -> stderr during user-code execution so that user print()
    // calls and subprocess stdout cannot leak into the RPC pipe
    "def _isolated(fn):\n"
    "    saved = os.dup(1)\n"
    "    os.dup2(2, 1)\n"
    "    try:\n"
    "        return fn()\n"
    "    finally:\n"
    "        os.dup2(saved, 1)\n"
    "        os.close(saved)\n"
    "\n"
    "def _run():\n"
    "    global _module, _objs, _nid\n"
    "    for raw in sys.stdin:\n"
    "        raw = raw.strip()\n"
    "        if not raw: continue\n"
    "        try: cmd = json.loads(raw)\n"
    "        except Exception:\n"
    "            _send({'ok': False, 'error': 'bad JSON'})\n"
    "            continue\n"
    "        a = cmd.get('action', '')\n"
    "        try:\n"
    "            if a == 'ping':\n"
    "                _send({'ok': True, 'result': 'pong'})\n"
    "            elif a == 'load':\n"
    "                spec = importlib.util.spec_from_file_location('_ava_user', cmd['path'])\n"
    "                mod  = importlib.util.module_from_spec(spec)\n"
    "                _isolated(lambda: spec.loader.exec_module(mod))\n"
    "                _module = mod\n"
    "                _send({'ok': True, 'result': 'loaded'})\n"
    "            elif a == 'call_func':\n"
    "                fn   = getattr(_module, cmd['name'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                r    = _isolated(lambda: fn(*args))\n"
    "                _send({'ok': True, 'result': str(r) if r is not None else 'None'})\n"
    "            elif a == 'new_obj':\n"
    "                cls  = getattr(_module, cmd['class'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                oid  = _nid; _nid += 1\n"
    "                _objs[oid] = _isolated(lambda: cls(*args))\n"
    "                _send({'ok': True, 'result': '{}#{}'.format(cmd['class'], oid), 'id': oid})\n"
    "            elif a == 'call_meth':\n"
    "                obj = _objs.get(cmd['id'])\n"
    "                if obj is None:\n"
    "                    _send({'ok': False, 'error': 'object not found: id={}'.format(cmd['id'])})\n"
    "                    continue\n"
    "                m    = getattr(obj, cmd['name'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                r    = _isolated(lambda: m(*args))\n"
    "                _send({'ok': True, 'result': str(r) if r is not None else 'None'})\n"
    "            elif a == 'del_obj':\n"
    "                _objs.pop(cmd['id'], None)\n"
    "                _send({'ok': True, 'result': 'deleted'})\n"
    "            else:\n"
    "                _send({'ok': False, 'error': 'unknown: ' + a})\n"
    "        except Exception:\n"
    "            _send({'ok': False, 'error': traceback.format_exc()})\n"
    "\n"
    "_run()\n";

// ─── PyRunner — persistent Python subprocess ──────────────────────────────────

struct SdkPanel::PyRunner {
#ifdef _WIN32
        HANDLE hProc{nullptr};
        HANDLE hStdinW{nullptr};
        HANDLE hStdoutR{nullptr};
#endif
        bool alive_{false};

        ~PyRunner() { shutdown(); }

        bool launch(const std::string &scriptPath, const std::string &pyExe)
        {
#ifdef _WIN32
                SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
                HANDLE              hStdinR = nullptr, hStdoutW = nullptr;
                if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0))
                        return false;
                if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0)) {
                        CloseHandle(hStdinR);
                        CloseHandle(hStdinW);
                        hStdinW = nullptr;
                        return false;
                }
                SetHandleInformation(hStdinW, HANDLE_FLAG_INHERIT, 0);
                SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

                std::string cmd = "\"" + pyExe + "\" -u \"" + scriptPath + "\"";

                STARTUPINFOA si{};
                si.cb         = sizeof(si);
                si.dwFlags    = STARTF_USESTDHANDLES;
                si.hStdInput  = hStdinR;
                si.hStdOutput = hStdoutW;
                si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

                PROCESS_INFORMATION pi{};
                bool                ok = CreateProcessA(
                              nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
                CloseHandle(hStdinR);
                CloseHandle(hStdoutW);

                if (!ok) {
                        CloseHandle(hStdinW);
                        CloseHandle(hStdoutR);
                        hStdinW = hStdoutR = nullptr;
                        return false;
                }
                hProc = pi.hProcess;
                CloseHandle(pi.hThread);
                alive_ = true;
                // Ping to verify
                auto r = xact(R"({"action":"ping"})", 3000);
                if (r.find("pong") == std::string::npos) {
                        shutdown();
                        return false;
                }
                return true;
#else
                (void)scriptPath;
                (void)pyExe;
                return false;
#endif
        }

        void shutdown()
        {
#ifdef _WIN32
                if (hProc) {
                        TerminateProcess(hProc, 0);
                        WaitForSingleObject(hProc, 2000);
                        CloseHandle(hProc);
                        hProc = nullptr;
                }
                if (hStdinW) {
                        CloseHandle(hStdinW);
                        hStdinW = nullptr;
                }
                if (hStdoutR) {
                        CloseHandle(hStdoutR);
                        hStdoutR = nullptr;
                }
#endif
                alive_ = false;
        }

        bool isAlive()
        {
#ifdef _WIN32
                if (!alive_ || !hProc)
                        return false;
                DWORD exit = 0;
                if (GetExitCodeProcess(hProc, &exit) && exit != STILL_ACTIVE)
                        alive_ = false;
#endif
                return alive_;
        }

        std::string xact(const std::string &jsonLine, int timeoutMs = 5000)
        {
                if (!alive_)
                        return R"({"ok":false,"error":"runner not started"})";
#ifdef _WIN32
                std::string msg = jsonLine + "\n";
                DWORD       written;
                if (!WriteFile(hStdinW, msg.c_str(), (DWORD)msg.size(), &written, nullptr) || written != (DWORD)msg.size()) {
                        alive_ = false;
                        return R"({"ok":false,"error":"write failed"})";
                }
                // Read response line with timeout via PeekNamedPipe polling
                std::string result;
                char        c;
                DWORD       rd;
                DWORD       deadline = GetTickCount() + (DWORD)timeoutMs;
                while (true) {
                        DWORD avail = 0;
                        if (!PeekNamedPipe(hStdoutR, nullptr, 0, nullptr, &avail, nullptr)) {
                                alive_ = false;
                                break;
                        }
                        if (avail == 0) {
                                DWORD ex = 0;
                                if (GetExitCodeProcess(hProc, &ex) && ex != STILL_ACTIVE) {
                                        alive_ = false;
                                        break;
                                }
                                if (GetTickCount() >= deadline)
                                        return R"({"ok":false,"error":"timeout"})";
                                Sleep(2);
                                continue;
                        }
                        if (!ReadFile(hStdoutR, &c, 1, &rd, nullptr) || rd == 0) {
                                alive_ = false;
                                break;
                        }
                        if (c == '\n')
                                break;
                        if (c != '\r')
                                result += c;
                }
                return result.empty() ? R"({"ok":false,"error":"no response"})" : result;
#else
                return R"({"ok":false,"error":"not supported"})";
#endif
        }
};

// ─── Python response parser ───────────────────────────────────────────────────

struct PyResp {
        bool        ok{false};
        std::string result;
        std::string error;
        int         id{-1};
};

static PyResp
parsePyResp(const std::string &json)
{
        PyResp r;
        cJSON *obj = cJSON_Parse(json.c_str());
        if (!obj) {
                r.error = "invalid JSON: " + json;
                return r;
        }
        if (const cJSON *v = cJSON_GetObjectItem(obj, "ok"))
                r.ok = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "result"); cJSON_IsString(v))
                r.result = v->valuestring;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "error"); cJSON_IsString(v))
                r.error = v->valuestring;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "id"); cJSON_IsNumber(v))
                r.id = v->valueint;
        cJSON_Delete(obj);
        return r;
}

// ─── Python file parser ───────────────────────────────────────────────────────

static void
parsePyParams(const std::string &raw, std::vector<PyParam> &out, bool skipSelf)
{
        bool   first = skipSelf;
        size_t i     = 0;
        while (i <= raw.size()) {
                // Find next comma at depth 0
                size_t j     = i;
                int    depth = 0;
                while (j < raw.size()) {
                        char ch = raw[j];
                        if (ch == '(' || ch == '[' || ch == '{')
                                ++depth;
                        else if (ch == ')' || ch == ']' || ch == '}')
                                --depth;
                        else if (ch == ',' && depth == 0)
                                break;
                        ++j;
                }
                std::string tok = raw.substr(i, j - i);
                i               = j + 1;

                // Trim
                while (!tok.empty() &&
                       (tok.front() == ' ' || tok.front() == '\t' || tok.front() == '\n' || tok.front() == '\r'))
                        tok.erase(tok.begin());
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\n' || tok.back() == '\r'))
                        tok.pop_back();

                if (tok.empty() || tok[0] == '*')
                        continue;
                if (first && (tok == "self" || tok == "cls")) {
                        first = false;
                        continue;
                }
                first = false;

                PyParam p;
                size_t  eqPos = tok.find('=');
                if (eqPos != std::string::npos) {
                        p.defaultVal = tok.substr(eqPos + 1);
                        tok          = tok.substr(0, eqPos);
                        while (!p.defaultVal.empty() && p.defaultVal.front() == ' ')
                                p.defaultVal.erase(p.defaultVal.begin());
                        while (!tok.empty() && tok.back() == ' ')
                                tok.pop_back();
                }
                size_t colonPos = tok.find(':');
                if (colonPos != std::string::npos)
                        tok = tok.substr(0, colonPos);
                while (!tok.empty() && tok.back() == ' ')
                        tok.pop_back();

                p.name = tok;
                if (!p.name.empty())
                        out.push_back(std::move(p));
        }
}

static PyParseResult
parsePyFile(const std::string &src)
{
        PyParseResult r;
        // Normalize CRLF
        std::string s;
        s.reserve(src.size());
        for (char c : src)
                if (c != '\r')
                        s += c;

        std::istringstream iss(s);
        std::string        line;
        int                classIdx = -1;

        while (std::getline(iss, line)) {
                if (line.empty())
                        continue;

                // Count leading indent (tab = 4 for column tracking)
                int    indent = 0;
                size_t p      = 0;
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
                        if (line[p] == '\t')
                                indent = ((indent / 4) + 1) * 4;
                        else
                                ++indent;
                        ++p;
                }
                std::string trimmed = line.substr(p);
                if (trimmed.empty() || trimmed[0] == '#')
                        continue;

                // Leaving a class body when we see a top-level non-empty line
                if (classIdx >= 0 && indent == 0)
                        classIdx = -1;

                if (indent == 0 && trimmed.size() > 6 && trimmed.substr(0, 6) == "class ") {
                        size_t      ni = 6;
                        std::string name;
                        while (ni < trimmed.size() && (isalnum((unsigned char)trimmed[ni]) || trimmed[ni] == '_'))
                                name += trimmed[ni++];
                        if (!name.empty()) {
                                PyClassDecl cls;
                                cls.name = name;
                                classIdx = (int)r.classes.size();
                                r.classes.push_back(std::move(cls));
                        }
                } else if (trimmed.size() > 4 && trimmed.substr(0, 4) == "def ") {
                        size_t      ni = 4;
                        std::string name;
                        while (ni < trimmed.size() && (isalnum((unsigned char)trimmed[ni]) || trimmed[ni] == '_'))
                                name += trimmed[ni++];
                        if (name.empty())
                                continue;

                        // Extract param string between '(' and matching ')'
                        size_t pOpen  = trimmed.find('(', ni);
                        size_t pClose = std::string::npos;
                        if (pOpen != std::string::npos) {
                                int depth = 0;
                                for (size_t k = pOpen + 1; k < trimmed.size(); ++k) {
                                        if (trimmed[k] == '(')
                                                ++depth;
                                        else if (trimmed[k] == ')') {
                                                if (depth == 0) {
                                                        pClose = k;
                                                        break;
                                                }
                                                --depth;
                                        }
                                }
                        }
                        std::string paramStr;
                        if (pOpen != std::string::npos && pClose != std::string::npos)
                                paramStr = trimmed.substr(pOpen + 1, pClose - pOpen - 1);

                        PyFuncDecl fn;
                        fn.name = name;
                        parsePyParams(paramStr, fn.params, classIdx >= 0);

                        if (classIdx >= 0)
                                r.classes[classIdx].methods.push_back(std::move(fn));
                        else
                                r.functions.push_back(std::move(fn));
                }
        }
        return r;
}

// ─── SdkPanel destructor ─────────────────────────────────────────────────────

SdkPanel::SdkPanel() = default; // PyRunner complete here — unique_ptr can be default-init'd
SdkPanel::~SdkPanel()
{
        stopDataUpdate();
}

// ─── doStartPyRunner ─────────────────────────────────────────────────────────

void
SdkPanel::doStartPyRunner()
{
        if (!pyRunner_)
                pyRunner_ = std::make_unique<PyRunner>();
        if (pyRunner_->isAlive())
                return;

        // Write runner script to temp dir
        std::filesystem::path tmpScript = std::filesystem::temp_directory_path() / "ava_py_runner.py";
        {
                std::ofstream f(tmpScript);
                if (!f) {
                        setStatus(tr("Failed to write Python runner script", "无法写入 Python 运行脚本"), true);
                        return;
                }
                f << kPyRunnerScript;
        }

        for (const char *exe : {"python", "python3"}) {
                if (pyRunner_->launch(tmpScript.string(), exe)) {
                        setStatus(tr("Python runner started", "Python 已启动"), false);
                        return;
                }
        }
        setStatus(tr("Python not found — install Python and add to PATH", "未找到 Python — 请安装 Python 并添加到 PATH"), true);
}

// ─── doLoadPy ────────────────────────────────────────────────────────────────

void
SdkPanel::doLoadPy()
{
        if (!pyPath_[0])
                return;
        std::ifstream f(std::filesystem::u8path(pyPath_));
        if (!f) {
                setStatus(tr("Cannot open Python file", "无法打开 Python 文件"), true);
                return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        pyResult_   = parsePyFile(ss.str());
        pySelFnIdx_ = pySelClsIdx_ = pySelMethIdx_ = pySelObjIdx_ = -1;
        pyFnArgBufs_.clear();
        pyMethArgBufs_.clear();
        pyFnArgBufsCache_.clear();
        pyMethArgBufsCache_.clear();
        pyObjects_.clear();

        if (!pyRunner_ || !pyRunner_->isAlive())
                doStartPyRunner();

        if (pyRunner_ && pyRunner_->isAlive()) {
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "load");
                // Python prefers forward slashes
                std::string fwdPath = pyPath_;
                for (char &c : fwdPath)
                        if (c == '\\')
                                c = '/';
                cJSON_AddStringToObject(cmd, "path", fwdPath.c_str());
                char *s  = cJSON_PrintUnformatted(cmd);
                auto  rr = parsePyResp(pyRunner_->xact(s));
                cJSON_free(s);
                cJSON_Delete(cmd);
                if (!rr.ok) {
                        setStatus(tr("Python load error: ", "Python 加载错误: ") + rr.error, true);
                        return;
                }
        }

        char buf[256];
        snprintf(buf,
                 sizeof(buf),
                 tr("Python: %d function(s), %d class(es)", "Python: %d 个函数, %d 个类"),
                 (int)pyResult_.functions.size(),
                 (int)pyResult_.classes.size());
        setStatus(buf, false);
}

// ─── doCallPyFunc ────────────────────────────────────────────────────────────

void
SdkPanel::doCallPyFunc()
{
        if (pySelFnIdx_ < 0 || pySelFnIdx_ >= (int)pyResult_.functions.size())
                return;
        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }
        const PyFuncDecl &fn = pyResult_.functions[pySelFnIdx_];

        cJSON *args = cJSON_CreateArray();
        for (size_t i = 0; i < fn.params.size(); ++i)
                cJSON_AddItemToArray(args, cJSON_CreateString(i < pyFnArgBufs_.size() ? pyFnArgBufs_[i].text : ""));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_func");
        cJSON_AddStringToObject(cmd, "name", fn.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        pyLastResult_   = rr.ok ? rr.result : rr.error;
        pyLastResultOk_ = rr.ok;

        std::string call = fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < pyFnArgBufs_.size()) ? pyFnArgBufs_[i].text : "";
        }
        call += ")";
        pushHistory(call, pyLastResult_, rr.ok, sessionTimeSec());

        if (rr.ok && pyFnResultVar_[0] != '\0' && onGetVarBuf_) {
                void *buf = onGetVarBuf_(pyFnResultVar_);
                if (buf) {
                        int64_t v = (int64_t)strtoll(rr.result.c_str(), nullptr, 0);
                        memcpy(buf, &v, sizeof(int64_t));
                        if (onVarWritten_)
                                onVarWritten_(pyFnResultVar_);
                }
        }
}

// ─── doNewPyObject / doNewPyObjectWithArgs ────────────────────────────────────

void
SdkPanel::doNewPyObject()
{
        doNewPyObjectWithArgs({});
}

void
SdkPanel::doNewPyObjectWithArgs(const std::vector<std::string> &ctorArgs)
{
        if (pySelClsIdx_ < 0 || pySelClsIdx_ >= (int)pyResult_.classes.size())
                return;
        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }
        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];

        cJSON *args = cJSON_CreateArray();
        for (const auto &a : ctorArgs)
                cJSON_AddItemToArray(args, cJSON_CreateString(a.c_str()));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "new_obj");
        cJSON_AddStringToObject(cmd, "class", cls.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s));
        cJSON_free(s);
        cJSON_Delete(cmd);

        if (!rr.ok) {
                setStatus(tr("Create object failed: ", "创建对象失败: ") + rr.error, true);
                return;
        }
        PyObjEntry e;
        e.pyId      = rr.id;
        e.className = cls.name;
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "obj%d", (int)pyObjects_.size());
        e.label = lbl;
        pyObjects_.push_back(std::move(e));
        pySelObjIdx_ = (int)pyObjects_.size() - 1;

        char buf[128];
        snprintf(buf, sizeof(buf), tr("Created %s#%d", "已创建 %s#%d"), cls.name.c_str(), rr.id);
        setStatus(buf, false);
}

// ─── doCallPyMeth ────────────────────────────────────────────────────────────

void
SdkPanel::doCallPyMeth()
{
        if (pySelClsIdx_ < 0 || pySelMethIdx_ < 0)
                return;
        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
        if (pySelMethIdx_ >= (int)cls.methods.size())
                return;
        const PyFuncDecl &meth   = cls.methods[pySelMethIdx_];
        bool              isInit = (meth.name == "__init__");

        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }

        if (isInit) {
                // Delegate to new-object path passing the __init__ args
                std::vector<std::string> ctorArgs;
                for (size_t i = 0; i < meth.params.size(); ++i)
                        ctorArgs.push_back(i < pyMethArgBufs_.size() ? pyMethArgBufs_[i].text : "");
                doNewPyObjectWithArgs(ctorArgs);
                return;
        }

        if (pySelObjIdx_ < 0 || pySelObjIdx_ >= (int)pyObjects_.size()) {
                setStatus(tr("Select an object first", "请先选择一个对象"), true);
                return;
        }

        cJSON *args = cJSON_CreateArray();
        for (size_t i = 0; i < meth.params.size(); ++i)
                cJSON_AddItemToArray(args, cJSON_CreateString(i < pyMethArgBufs_.size() ? pyMethArgBufs_[i].text : ""));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_meth");
        cJSON_AddNumberToObject(cmd, "id", pyObjects_[pySelObjIdx_].pyId);
        cJSON_AddStringToObject(cmd, "name", meth.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        std::string result = rr.ok ? rr.result : rr.error;
        std::string call   = cls.name + "." + meth.name + "(";
        for (size_t i = 0; i < meth.params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < pyMethArgBufs_.size()) ? pyMethArgBufs_[i].text : "";
        }
        call += ")";
        pushHistory(call, result, rr.ok, sessionTimeSec());
        if (!rr.ok)
                setStatus(rr.error, true);
}

// ─── doDeletePyObject ────────────────────────────────────────────────────────

void
SdkPanel::doDeletePyObject(int idx)
{
        if (idx < 0 || idx >= (int)pyObjects_.size())
                return;
        if (pyRunner_ && pyRunner_->isAlive()) {
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "del_obj");
                cJSON_AddNumberToObject(cmd, "id", pyObjects_[idx].pyId);
                char *s = cJSON_PrintUnformatted(cmd);
                pyRunner_->xact(s);
                cJSON_free(s);
                cJSON_Delete(cmd);
        }
        pyObjects_.erase(pyObjects_.begin() + idx);
        if (pySelObjIdx_ >= (int)pyObjects_.size())
                pySelObjIdx_ = (int)pyObjects_.size() - 1;
}

// ─── drawCFunctionsTab ────────────────────────────────────────────────────────

// ─── Splitter helper ──────────────────────────────────────────────────────────

static void
splitterV(const char *id, float *w, float minW, float maxW)
{
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Separator]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_SeparatorHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);
        ImGui::Button(id, ImVec2(4.0f, -1.0f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
                *w = std::clamp(*w + ImGui::GetIO().MouseDelta.x, minW, maxW);
        ImGui::SameLine(0, 0);
        ImGui::PopStyleColor(3);
}

static void
splitterH(const char *id, float *h, float minH, float maxH)
{
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Separator]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_SeparatorHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);
        ImGui::Button(id, ImVec2(-1.0f, 4.0f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
                *h = std::clamp(*h + ImGui::GetIO().MouseDelta.y, minH, maxH);
        ImGui::PopStyleColor(3);
}

// ─── drawCFunctionsTab ────────────────────────────────────────────────────────

static float sFnSplitW = 220.0f;

void
SdkPanel::drawCFunctionsTab()
{
        float &listWidth = sFnSplitW;
        ImGui::BeginChild("##fnlist", ImVec2(listWidth, 0), true);
        ImGui::TextDisabled("%s (%d)", tr("Functions", "函数列表"), (int)parseResult_.functions.size());
        ImGui::Separator();
        for (int i = 0; i < (int)parseResult_.functions.size(); ++i) {
                const CFuncDecl &fn  = parseResult_.functions[i];
                bool             sel = (i == selectedFnIdx_);
                char             label[256];
                snprintf(label,
                         sizeof(label),
                         "%s  %s",
                         fn.retRaw.empty() ? ctypeLabel(fn.retType) : fn.retRaw.c_str(),
                         fn.name.c_str());
                if (ImGui::Selectable(label, sel) && !sel)
                        selectFn(i);
                // Drag to SequenceEditor to create an SDK call step
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        SdkDragPayload p;
                        p.panelWinId = winId_;
                        p.isCFunc    = true;
                        p.methodIdx  = i;
                        ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                        ImGui::TextUnformatted(fn.name.c_str());
                        ImGui::EndDragDropSource();
                }
        }
        ImGui::EndChild();
        splitterV("##fnsplit", &listWidth, 80.0f, 500.0f);

        ImGui::BeginChild("##callui", ImVec2(0, 0), false);
        if (selectedFnIdx_ < 0 || selectedFnIdx_ >= (int)parseResult_.functions.size()) {
                ImGui::TextDisabled("%s", tr("← Select a function", "← 选择左侧函数"));
        } else {
                const CFuncDecl &fn = parseResult_.functions[selectedFnIdx_];
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ui::ReadableTextColor(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                ImGui::Text("%s  %s", fn.retRaw.c_str(), fn.name.c_str());
                ImGui::PopStyleColor();

                {
                        static ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
                        if (ui::TableIsland tableCard;
                            tableCard.begin("##cfarg_tbl",
                                            3,
                                            tblFlags | ImGuiTableFlags_ScrollX,
                                            ImVec2(0, 0),
                                            std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 44.0f))) {
                                ImGui::TableSetupColumn(tr("Type", "类型"),
                                                        ImGuiTableColumnFlags_WidthFixed,
                                                        ui::TableColumnWidth(tr("Type", "类型"), "const uint64_t *"));
                                ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                ImGui::TableSetupColumn(
                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                ImGui::TableHeadersRow();

                                for (size_t i = 0; i < fn.params.size(); ++i) {
                                        const CParam &p       = fn.params[i];
                                        bool          isCharP = ctypeIsCharPtr(p.type, p.rawType);
                                        char          id[64];
                                        snprintf(id, sizeof(id), "##farg%zu", i);
                                        const char *rawT = p.rawType.empty() ? ctypeLabel(p.type) : p.rawType.c_str();

                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ui::TableText(rawT);
                                        ImGui::TableSetColumnIndex(1);
                                        if (!p.name.empty())
                                                ui::TableText(p.name);
                                        else
                                                ImGui::Text("[%zu]", i);
                                        ImGui::TableSetColumnIndex(2);
                                        if (i < fnArgBufs_.size()) {
                                                bool hasBtns = onCreateLocalVar_ || onListLocalVars_;
                                                ImGui::SetNextItemWidth(hasBtns ? ImGui::GetContentRegionAvail().x - kParamBtnsW
                                                                                : -FLT_MIN);
                                                if (isCharP)
                                                        ImGui::InputText(id, fnArgBufs_[i].text, sizeof(ArgBuf::text));
                                                else if (ctypeIsFloat(p.type))
                                                        ImGui::InputText(id,
                                                                         fnArgBufs_[i].text,
                                                                         sizeof(ArgBuf::text),
                                                                         ImGuiInputTextFlags_CharsDecimal);
                                                else
                                                        ImGui::InputText(id,
                                                                         fnArgBufs_[i].text,
                                                                         sizeof(ArgBuf::text),
                                                                         ImGuiInputTextFlags_CharsHexadecimal |
                                                                             ImGuiInputTextFlags_CharsDecimal);
                                                drawParamVarButtons(fnArgBufs_[i].text, sizeof(ArgBuf::text), p, (int)i);
                                        }
                                }

                                // ── Return value row ──
                                {
                                        std::string retT = fn.retRaw.empty() ? std::string(ctypeLabel(fn.retType)) : fn.retRaw;
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(retT.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextUnformatted(tr("return", "返回值"));
                                        ImGui::TableSetColumnIndex(2);
                                        std::vector<std::string> localVars;
                                        if (onListLocalVars_)
                                                localVars = onListLocalVars_();
                                        bool  hasCreate = onCreateLocalVar_ && !ctypeIsVoid(fn.retType);
                                        bool  hasPick   = !localVars.empty();
                                        float reserve   = (hasCreate ? 24.0f : 0.0f) + (hasPick ? 24.0f : 0.0f);
                                        float rvW = reserve > 0.0f ? ImGui::GetContentRegionAvail().x - reserve : -FLT_MIN;
                                        ImGui::SetNextItemWidth(rvW);
                                        ImGui::InputTextWithHint(
                                            "##fnrv", tr("variable name", "变量名"), fnResultVar_, sizeof(fnResultVar_));
                                        if (hasCreate) {
                                                ImGui::SameLine(0, 2);
                                                if (ImGui::SmallButton("+##fnrvnew")) {
                                                        std::string vn = fnResultVar_[0] ? std::string(fnResultVar_) : fn.name;
                                                        onCreateLocalVar_(vn, fn.retType, nullptr);
                                                        strncpy(fnResultVar_, vn.c_str(), sizeof(fnResultVar_) - 1);
                                                        fnResultVar_[sizeof(fnResultVar_) - 1] = '\0';
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s", tr("Create result variable", "创建结果变量"));
                                        }
                                        if (hasPick) {
                                                ImGui::SameLine(0, 2);
                                                if (ImGui::SmallButton("v##fnrvpick"))
                                                        ImGui::OpenPopup("##fnrvpop");
                                                if (ImGui::BeginPopup("##fnrvpop")) {
                                                        for (const auto &vn : localVars)
                                                                if (ImGui::Selectable(vn.c_str())) {
                                                                        strncpy(
                                                                            fnResultVar_, vn.c_str(), sizeof(fnResultVar_) - 1);
                                                                        fnResultVar_[sizeof(fnResultVar_) - 1] = '\0';
                                                                }
                                                        ImGui::EndPopup();
                                                }
                                        }
                                }

                                tableCard.end();
                        }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                bool canCall = loader_.isLoaded();
                if (!canCall)
                        ImGui::BeginDisabled();
                if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                        doCallC();
                if (!canCall) {
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", tr("(load a DLL first)", "(请先加载 DLL)"));
                }

                if (!fnLastResult_.empty()) {
                        ImGui::Spacing();
                        ImGui::Text("%s", tr("Result:", "返回值:"));
                        ImGui::SameLine();
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            ui::ReadableTextColor(fnLastResultOk_ ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ui::ErrorTextColor(),
                                                  ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                        ImGui::TextUnformatted(fnLastResult_.c_str());
                        ImGui::PopStyleColor();
                }

                ImGui::Spacing();
                ImGui::Separator();
                if (drawHistoryHeader("c_function")) {
                        drawHistoryLog("##fhist");
                }
        }
        ImGui::EndChild();
}

// ─── drawCppClassesTab ────────────────────────────────────────────────────────

void
SdkPanel::drawCppClassesTab()
{
        if (parseResult_.classes.empty()) {
                ImGui::TextDisabled("%s", tr("No classes found in header.", "头文件中未找到类定义。"));
                return;
        }

        float &leftW = clsSplitW_;
        ImGui::BeginChild("##clslist", ImVec2(leftW, 0), true);

        // ── class list ──
        ImGui::TextDisabled("%s (%d)", tr("Classes", "类列表"), (int)parseResult_.classes.size());
        ImGui::Separator();
        for (int ci = 0; ci < (int)parseResult_.classes.size(); ++ci) {
                const CClassDecl &cls    = parseResult_.classes[ci];
                bool              selCls = (ci == selectedClassIdx_);
                char              clsLbl[128];
                snprintf(clsLbl, sizeof(clsLbl), "%s (%d)", cls.name.c_str(), (int)cls.methods.size());
                if (ImGui::Selectable(clsLbl, selCls, ImGuiSelectableFlags_None) && !selCls) {
                        if (selectedClassIdx_ >= 0 && selectedMethodIdx_ >= 0) {
                                int key                = selectedClassIdx_ * 10000 + selectedMethodIdx_;
                                methArgBufsCache_[key] = methArgBufs_;
                                structBufsCache_[key]  = structBufs_;
                        }
                        selectedClassIdx_  = ci;
                        selectedMethodIdx_ = -1;
                        selectedObjIdx_    = -1;
                        methArgBufs_.clear();
                        structBufs_.clear();
                }
        }

        // ── method list for selected class ──
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size()) {
                const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("%s (%d)", tr("Methods", "方法列表"), (int)cls.methods.size());
                ImGui::Separator();
                for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                        const CMethodDecl &m    = cls.methods[mi];
                        bool               selM = (mi == selectedMethodIdx_);
                        char               mLbl[256];
                        const char        *prefix = m.isCtor ? "[ctor]" : m.isDtor ? "[dtor]" : "";
                        if (m.isCtor || m.isDtor)
                                snprintf(mLbl, sizeof(mLbl), "%s %s", prefix, m.name.c_str());
                        else
                                snprintf(mLbl,
                                         sizeof(mLbl),
                                         "%s  %s",
                                         m.retRaw.empty() ? ctypeLabel(m.retType) : m.retRaw.c_str(),
                                         m.name.c_str());
                        if (ImGui::Selectable(mLbl, selM) && !selM)
                                selectMethod(selectedClassIdx_, mi);
                        // Drag to SequenceEditor to create an SDK call step
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                                SdkDragPayload p;
                                p.panelWinId = winId_;
                                p.isCFunc    = false;
                                p.classIdx   = selectedClassIdx_;
                                p.methodIdx  = mi;
                                p.objIdx     = (selectedObjIdx_ >= 0) ? selectedObjIdx_ : 0;
                                ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                                ImGui::Text("%s::%s", cls.name.c_str(), m.name.c_str());
                                ImGui::EndDragDropSource();
                        }
                }
        }
        ImGui::EndChild();
        splitterV("##clssplit", &leftW, 80.0f, 500.0f);

        ImGui::BeginChild("##methcall", ImVec2(0, 0), false);

        if (selectedClassIdx_ < 0 || selectedClassIdx_ >= (int)parseResult_.classes.size()) {
                ImGui::TextDisabled("%s", tr("← Select a class", "← 选择左侧类"));
                ImGui::EndChild();
                return;
        }

        const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
        ImGui::PushStyleColor(
            ImGuiCol_Text, ui::ReadableTextColor(ImVec4(0.9f, 0.7f, 0.3f, 1.f), ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
        ImGui::Text("class %s", cls.fullName.c_str());
        ImGui::PopStyleColor();

        ImGui::Separator();

        if (selectedMethodIdx_ < 0 || selectedMethodIdx_ >= (int)cls.methods.size()) {
                ImGui::TextDisabled("%s", tr("← Select a method", "← 选择左侧方法"));
                ImGui::EndChild();
                return;
        }

        const CMethodDecl &meth = cls.methods[selectedMethodIdx_];

        // ── method signature ──
        ImGui::PushStyleColor(
            ImGuiCol_Text, ui::ReadableTextColor(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
        if (meth.isCtor)
                ImGui::Text("[constructor] %s(...)", meth.name.c_str());
        else if (meth.isDtor)
                ImGui::Text("[destructor] %s()", meth.name.c_str());
        else
                ImGui::Text("%s  %s", meth.retRaw.empty() ? "?" : meth.retRaw.c_str(), meth.name.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // ── parameters (+ object + return value) ──
        {
                const bool             showObjRow = !meth.isCtor && !meth.isDtor;
                const bool             showRetRow = !meth.isDtor;
                static ImGuiTableFlags tblFlags   = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
                if (ui::TableIsland tableCard;
                    tableCard.begin("##mparg_tbl",
                                    3,
                                    tblFlags | ImGuiTableFlags_ScrollX,
                                    ImVec2(0, 0),
                                    std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 44.0f))) {
                        ImGui::TableSetupColumn(tr("Type", "类型"),
                                                ImGuiTableColumnFlags_WidthFixed,
                                                ui::TableColumnWidth(tr("Type", "类型"), "const uint64_t *"));
                        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                        ImGui::TableSetupColumn(tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                        ImGui::TableHeadersRow();

                        // ── Object (this) row ──
                        if (showObjRow) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ui::TableText(cls.name);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(tr("Object", "对象"));
                                ImGui::TableSetColumnIndex(2);
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                const char *objPrev = (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size())
                                                          ? objects_[selectedObjIdx_].label
                                                          : tr("(none)", "(无)");
                                if (ImGui::BeginCombo("##obj_sel", objPrev)) {
                                        if (loader_.isLoaded() &&
                                            ImGui::Selectable(tr("[+ New Object]", "[+ 新建对象]"), false))
                                                doNewObject(selectedClassIdx_);
                                        for (int oi = 0; oi < (int)objects_.size(); ++oi) {
                                                bool sel = (oi == selectedObjIdx_);
                                                char objLbl[128];
                                                snprintf(
                                                    objLbl, sizeof(objLbl), "%s  @%p", objects_[oi].label, objects_[oi].ptr);
                                                if (ImGui::Selectable(objLbl, sel))
                                                        selectedObjIdx_ = oi;
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                        }

                        if (meth.params.empty() && !showObjRow && !showRetRow) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("(%s)", tr("no parameters", "无参数"));
                        }

                        for (size_t pi = 0; pi < meth.params.size(); ++pi) {
                                const CParam &p = meth.params[pi];
                                char          rowId[64];
                                snprintf(rowId, sizeof(rowId), "##mp%zu", pi);

                                // Struct reference parameter → header row + field sub-rows
                                if (pi < structBufs_.size() && structBufs_[pi].decl) {
                                        const CStructDecl    *sd       = structBufs_[pi].decl;
                                        std::vector<uint8_t> &buf      = structBufs_[pi].data;
                                        bool                 &expanded = structBufs_[pi].expanded;

                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(sd->name.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        char hdr[128];
                                        snprintf(
                                            hdr, sizeof(hdr), "%s##shdr%zu", p.name.empty() ? "param" : p.name.c_str(), pi);
                                        ImGui::SetNextItemOpen(expanded, ImGuiCond_Always);
                                        expanded = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanFullWidth);

                                        // col2: show whether this struct uses the inline editor or is bound to a
                                        // LOCAL variable, plus the [+][v] buttons
                                        ImGui::TableSetColumnIndex(2);
                                        if (pi < methArgBufs_.size()) {
                                                const char *bound = methArgBufs_[pi].text;
                                                if (bound[0] == '&')
                                                        ImGui::TextDisabled("%s", bound);
                                                else
                                                        ImGui::TextDisabled("%s", tr("(inline)", "(内联编辑)"));
                                                drawParamVarButtons(methArgBufs_[pi].text, sizeof(ArgBuf::text), p, (int)pi);
                                        }

                                        // Inline field editor is used only when not bound to a "&var"
                                        bool bound = (pi < methArgBufs_.size() && methArgBufs_[pi].text[0] == '&');
                                        if (expanded) {
                                                if (!bound)
                                                        drawSdkStructFieldRows(*sd, buf.data(), buf.size(), (int)pi);
                                                ImGui::TreePop();
                                        }
                                        continue;
                                }

                                // Enum parameter → dropdown row
                                const CEnumDecl *ed = findParamEnum(p);
                                if (ed && !ed->values.empty()) {
                                        const char *boundTxt = (pi < methArgBufs_.size()) ? methArgBufs_[pi].text : "";
                                        bool        isBound  = boundTxt[0] == '$' || boundTxt[0] == '&';
                                        int64_t     curVal   = isBound ? 0 : strtoll(boundTxt, nullptr, 0);
                                        const char *preview2 = isBound ? boundTxt : ed->values[0].name.c_str();
                                        int         curIdx   = 0;
                                        if (!isBound)
                                                for (int ei = 0; ei < (int)ed->values.size(); ++ei) {
                                                        if (ed->values[ei].val == curVal) {
                                                                preview2 = ed->values[ei].name.c_str();
                                                                curIdx   = ei;
                                                                break;
                                                        }
                                                }
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(ed->name.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextUnformatted(p.name.empty() ? "?" : p.name.c_str());
                                        ImGui::TableSetColumnIndex(2);
                                        char cmId[64];
                                        snprintf(cmId, sizeof(cmId), "##ec%zu", pi);
                                        bool hasBtns = onCreateLocalVar_ || onListLocalVars_;
                                        ImGui::SetNextItemWidth(hasBtns ? ImGui::GetContentRegionAvail().x - kParamBtnsW
                                                                        : -FLT_MIN);
                                        if (ImGui::BeginCombo(cmId, preview2)) {
                                                for (int ei = 0; ei < (int)ed->values.size(); ++ei) {
                                                        bool sel2 = (ei == curIdx);
                                                        char evLbl[128];
                                                        snprintf(evLbl,
                                                                 sizeof(evLbl),
                                                                 "%s (%lld)",
                                                                 ed->values[ei].name.c_str(),
                                                                 (long long)ed->values[ei].val);
                                                        if (ImGui::Selectable(evLbl, sel2) && pi < methArgBufs_.size()) {
                                                                snprintf(methArgBufs_[pi].text,
                                                                         sizeof(ArgBuf::text),
                                                                         "%lld",
                                                                         (long long)ed->values[ei].val);
                                                        }
                                                        if (sel2)
                                                                ImGui::SetItemDefaultFocus();
                                                }
                                                ImGui::EndCombo();
                                        }
                                        if (pi < methArgBufs_.size())
                                                drawParamVarButtons(methArgBufs_[pi].text, sizeof(ArgBuf::text), p, (int)pi);
                                        continue;
                                }

                                // Plain parameter → text input row
                                const char *rawT2 = p.rawType.empty() ? ctypeLabel(p.type) : p.rawType.c_str();

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(rawT2);
                                ImGui::TableSetColumnIndex(1);
                                if (!p.name.empty())
                                        ImGui::TextUnformatted(p.name.c_str());
                                else
                                        ImGui::Text("[%zu]", pi);
                                ImGui::TableSetColumnIndex(2);
                                if (pi < methArgBufs_.size()) {
                                        bool isCharP = ctypeIsCharPtr(p.type, p.rawType);
                                        bool hasBtns = onCreateLocalVar_ || onListLocalVars_;
                                        ImGui::SetNextItemWidth(hasBtns ? ImGui::GetContentRegionAvail().x - kParamBtnsW
                                                                        : -FLT_MIN);
                                        if (isCharP)
                                                ImGui::InputText(rowId, methArgBufs_[pi].text, sizeof(ArgBuf::text));
                                        else if (ctypeIsFloat(p.type))
                                                ImGui::InputText(rowId,
                                                                 methArgBufs_[pi].text,
                                                                 sizeof(ArgBuf::text),
                                                                 ImGuiInputTextFlags_CharsDecimal);
                                        else
                                                ImGui::InputText(rowId,
                                                                 methArgBufs_[pi].text,
                                                                 sizeof(ArgBuf::text),
                                                                 ImGuiInputTextFlags_CharsHexadecimal |
                                                                     ImGuiInputTextFlags_CharsDecimal);
                                        drawParamVarButtons(methArgBufs_[pi].text, sizeof(ArgBuf::text), p, (int)pi);
                                }
                        }

                        // ── Return value row ──
                        if (showRetRow) {
                                std::string retT = meth.retRaw.empty() ? std::string(ctypeLabel(meth.retType)) : meth.retRaw;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(retT.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(tr("return", "返回值"));
                                ImGui::TableSetColumnIndex(2);
                                std::vector<std::string> localVars;
                                if (onListLocalVars_)
                                        localVars = onListLocalVars_();
                                bool  hasCreate = onCreateLocalVar_ && !ctypeIsVoid(meth.retType);
                                bool  hasPick   = !localVars.empty();
                                float reserve   = (hasCreate ? 24.0f : 0.0f) + (hasPick ? 24.0f : 0.0f);
                                float rvW       = reserve > 0.0f ? ImGui::GetContentRegionAvail().x - reserve : -FLT_MIN;
                                ImGui::SetNextItemWidth(rvW);
                                ImGui::InputTextWithHint(
                                    "##methrv", tr("variable name", "变量名"), methResultVar_, sizeof(methResultVar_));
                                if (hasCreate) {
                                        ImGui::SameLine(0, 2);
                                        if (ImGui::SmallButton("+##methrvnew")) {
                                                std::string vn = methResultVar_[0] ? std::string(methResultVar_) : meth.name;
                                                onCreateLocalVar_(vn, meth.retType, nullptr);
                                                strncpy(methResultVar_, vn.c_str(), sizeof(methResultVar_) - 1);
                                                methResultVar_[sizeof(methResultVar_) - 1] = '\0';
                                        }
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", tr("Create result variable", "创建结果变量"));
                                }
                                if (hasPick) {
                                        ImGui::SameLine(0, 2);
                                        if (ImGui::SmallButton("v##methrvpick"))
                                                ImGui::OpenPopup("##methrvpop");
                                        if (ImGui::BeginPopup("##methrvpop")) {
                                                for (const auto &vn : localVars)
                                                        if (ImGui::Selectable(vn.c_str())) {
                                                                strncpy(methResultVar_, vn.c_str(), sizeof(methResultVar_) - 1);
                                                                methResultVar_[sizeof(methResultVar_) - 1] = '\0';
                                                        }
                                                ImGui::EndPopup();
                                        }
                                }
                        }

                        tableCard.end();
                }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canCall = loader_.isLoaded() && (meth.isCtor || (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size()));
        if (!canCall)
                ImGui::BeginDisabled();
        if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                doCallMethod();
        if (!canCall)
                ImGui::EndDisabled();

        // ── history ──
        ImGui::Spacing();
        ImGui::Separator();
        if (drawHistoryHeader("cpp_method")) {
                drawHistoryLog("##mhist");
        }

        ImGui::EndChild(); // ##methcall
}

// ─── drawPythonTab ────────────────────────────────────────────────────────────

void
SdkPanel::drawPythonTab()
{
        // ── Python file path row ──────────────────────────────────────────────
        if (ImGui::Button(tr("Browse##py", "浏览##py"))) {
                std::string p =
                    nativeDlgOpen(tr("Select Python script", "选择 Python 脚本"), {{"Python", {"py"}}, {"All Files", {"*"}}});
                if (!p.empty()) {
                        strncpy(pyPath_, p.c_str(), sizeof(pyPath_) - 1);
                        pyPath_[sizeof(pyPath_) - 1] = '\0';
                        doLoadPy();
                        modified_ = true;
                }
        }
        ImGui::SameLine();
        bool runnerOk = pyRunner_ && pyRunner_->isAlive();
        ImGui::TextDisabled(runnerOk ? tr("[py: online]", "[py: 在线]") : tr("[py: offline]", "[py: 离线]"));
        if (pyPath_[0]) {
                ImGui::SameLine();
                ImGui::TextDisabled("— %s", pyPath_);
                showPathTooltipIfHovered(pyPath_);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (pyResult_.functions.empty() && pyResult_.classes.empty()) {
                ImGui::TextDisabled("%s", tr("Load a .py file to begin.", "加载 .py 文件开始使用。"));
                return;
        }

        if (!ImGui::BeginTabBar("##pytabs"))
                return;

        // ── Functions sub-tab ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Functions##pyf", "函数##pyf"))) {
                ImGui::Spacing();
                if (pyResult_.functions.empty()) {
                        ImGui::TextDisabled("%s", tr("No top-level functions found.", "未找到顶层函数。"));
                } else {
                        float &lw = pySplitW_;
                        ImGui::BeginChild("##pyfnlist", ImVec2(lw, 0), true);
                        ImGui::TextDisabled("%s (%d)", tr("Functions", "函数列表"), (int)pyResult_.functions.size());
                        ImGui::Separator();
                        for (int i = 0; i < (int)pyResult_.functions.size(); ++i) {
                                bool sel = (i == pySelFnIdx_);
                                if (ImGui::Selectable(pyResult_.functions[i].name.c_str(), sel) && !sel) {
                                        if (pySelFnIdx_ >= 0 && pySelFnIdx_ < (int)pyResult_.functions.size())
                                                pyFnArgBufsCache_[pySelFnIdx_] = pyFnArgBufs_;
                                        pySelFnIdx_ = i;
                                        pyLastResult_.clear();
                                        const auto &fn2 = pyResult_.functions[i];
                                        auto        cit = pyFnArgBufsCache_.find(i);
                                        if (cit != pyFnArgBufsCache_.end() && cit->second.size() == fn2.params.size()) {
                                                pyFnArgBufs_ = cit->second;
                                        } else {
                                                pyFnArgBufs_.clear();
                                                pyFnArgBufs_.resize(fn2.params.size());
                                                for (size_t j = 0; j < fn2.params.size(); ++j) {
                                                        if (!fn2.params[j].defaultVal.empty())
                                                                strncpy(pyFnArgBufs_[j].text,
                                                                        fn2.params[j].defaultVal.c_str(),
                                                                        sizeof(ArgBuf::text) - 1);
                                                }
                                        }
                                }
                                // Drag to SequenceEditor
                                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                                        SdkDragPayload p;
                                        p.panelWinId = winId_;
                                        p.isPython   = true;
                                        strncpy(p.pyName, pyResult_.functions[i].name.c_str(), sizeof(p.pyName) - 1);
                                        ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                                        ImGui::TextUnformatted(pyResult_.functions[i].name.c_str());
                                        ImGui::EndDragDropSource();
                                }
                        }
                        ImGui::EndChild();
                        splitterV("##pyfnsplit", &lw, 80.0f, 500.0f);

                        ImGui::BeginChild("##pyfncall", ImVec2(0, 0), false);
                        if (pySelFnIdx_ < 0 || pySelFnIdx_ >= (int)pyResult_.functions.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a function", "← 选择左侧函数"));
                        } else {
                                const PyFuncDecl &fn = pyResult_.functions[pySelFnIdx_];
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                      ui::ReadableTextColor(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                                                                            ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                                ImGui::Text("def %s(...)", fn.name.c_str());
                                ImGui::PopStyleColor();

                                if (fn.params.empty()) {
                                        ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
                                } else {
                                        static ImGuiTableFlags pyFTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                             ImGuiTableFlags_SizingStretchProp |
                                                                             ImGuiTableFlags_Resizable;
                                        if (ui::TableIsland tableCard; tableCard.begin(
                                                "##pyfarg_tbl",
                                                3,
                                                pyFTblFlags | ImGuiTableFlags_ScrollX,
                                                ImVec2(0, 0),
                                                std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 34.0f))) {
                                                ImGui::TableSetupColumn(tr("#", "#"),
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        ui::TableColumnWidth("#", "99", 2));
                                                ImGui::TableSetupColumn(
                                                    tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                                ImGui::TableSetupColumn(
                                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                                ImGui::TableHeadersRow();
                                                for (size_t i = 0; i < fn.params.size(); ++i) {
                                                        char id[64];
                                                        snprintf(id, sizeof(id), "##pyfa%zu", i);
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::Text("%d", (int)(i + 1));
                                                        ImGui::TableSetColumnIndex(1);
                                                        ui::TableText(fn.params[i].name);
                                                        ImGui::TableSetColumnIndex(2);
                                                        if (i < pyFnArgBufs_.size()) {
                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                ImGui::InputText(
                                                                    id, pyFnArgBufs_[i].text, sizeof(ArgBuf::text));
                                                        }
                                                }
                                                tableCard.end();
                                        }
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();
                                if (!runnerOk)
                                        ImGui::BeginDisabled();
                                if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                                        doCallPyFunc();
                                if (!runnerOk) {
                                        ImGui::EndDisabled();
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("%s", tr("(Python not running)", "(Python 未运行)"));
                                }
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(120.0f);
                                ImGui::InputTextWithHint(
                                    "##pyrv", tr("variable name", "变量名"), pyFnResultVar_, sizeof(pyFnResultVar_));
                                if (onListLocalVars_) {
                                        auto localVars = onListLocalVars_();
                                        if (!localVars.empty()) {
                                                ImGui::SameLine();
                                                if (ImGui::SmallButton("v##pyrvpick"))
                                                        ImGui::OpenPopup("##pyrvpop");
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s", tr("Pick LOCAL variable", "选择 LOCAL 变量"));
                                                if (ImGui::BeginPopup("##pyrvpop")) {
                                                        for (const auto &vn : localVars)
                                                                if (ImGui::Selectable(vn.c_str()))
                                                                        strncpy(pyFnResultVar_,
                                                                                vn.c_str(),
                                                                                sizeof(pyFnResultVar_) - 1);
                                                        ImGui::EndPopup();
                                                }
                                        }
                                }

                                if (!pyLastResult_.empty()) {
                                        ImGui::Spacing();
                                        ImGui::Text("%s", tr("Result:", "返回值:"));
                                        ImGui::SameLine();
                                        ImGui::PushStyleColor(
                                            ImGuiCol_Text,
                                            ui::ReadableTextColor(pyLastResultOk_ ? ImVec4(0.3f, 1.f, 0.5f, 1.f)
                                                                                  : ui::ErrorTextColor(),
                                                                  ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                                        ImGui::TextUnformatted(pyLastResult_.c_str());
                                        ImGui::PopStyleColor();
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                if (drawHistoryHeader("python_function")) {
                                        drawHistoryLog("##pyfhist");
                                }
                        }
                        ImGui::EndChild();
                }
                ImGui::EndTabItem();
        }

        // ── Classes sub-tab ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Classes##pyc", "类##pyc"))) {
                ImGui::Spacing();
                if (pyResult_.classes.empty()) {
                        ImGui::TextDisabled("%s", tr("No classes found.", "未找到类定义。"));
                } else {
                        float &lw = pySplitW_;
                        ImGui::BeginChild("##pyclslist", ImVec2(lw, 0), true);
                        ImGui::TextDisabled("%s (%d)", tr("Classes", "类列表"), (int)pyResult_.classes.size());
                        ImGui::Separator();
                        for (int ci = 0; ci < (int)pyResult_.classes.size(); ++ci) {
                                const PyClassDecl &cls    = pyResult_.classes[ci];
                                bool               selCls = (ci == pySelClsIdx_);
                                char               lbl[128];
                                snprintf(lbl, sizeof(lbl), "%s (%d)", cls.name.c_str(), (int)cls.methods.size());
                                if (ImGui::Selectable(lbl, selCls) && !selCls) {
                                        if (pySelClsIdx_ >= 0 && pySelMethIdx_ >= 0)
                                                pyMethArgBufsCache_[pySelClsIdx_ * 1000 + pySelMethIdx_] = pyMethArgBufs_;
                                        pySelClsIdx_  = ci;
                                        pySelMethIdx_ = -1;
                                        pyMethArgBufs_.clear();
                                }
                        }
                        if (pySelClsIdx_ >= 0 && pySelClsIdx_ < (int)pyResult_.classes.size()) {
                                const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::TextDisabled("%s (%d)", tr("Methods", "方法列表"), (int)cls.methods.size());
                                ImGui::Separator();
                                for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                                        bool selM = (mi == pySelMethIdx_);
                                        char mlbl[256];
                                        snprintf(mlbl, sizeof(mlbl), "def %s", cls.methods[mi].name.c_str());
                                        if (ImGui::Selectable(mlbl, selM) && !selM) {
                                                if (pySelMethIdx_ >= 0 && pySelClsIdx_ >= 0)
                                                        pyMethArgBufsCache_[pySelClsIdx_ * 1000 + pySelMethIdx_] =
                                                            pyMethArgBufs_;
                                                pySelMethIdx_     = mi;
                                                const auto &meth2 = cls.methods[mi];
                                                int         mkey  = pySelClsIdx_ * 1000 + mi;
                                                auto        mcit  = pyMethArgBufsCache_.find(mkey);
                                                if (mcit != pyMethArgBufsCache_.end() &&
                                                    mcit->second.size() == meth2.params.size()) {
                                                        pyMethArgBufs_ = mcit->second;
                                                } else {
                                                        pyMethArgBufs_.clear();
                                                        pyMethArgBufs_.resize(meth2.params.size());
                                                        for (size_t j = 0; j < meth2.params.size(); ++j) {
                                                                if (!meth2.params[j].defaultVal.empty())
                                                                        strncpy(pyMethArgBufs_[j].text,
                                                                                meth2.params[j].defaultVal.c_str(),
                                                                                sizeof(ArgBuf::text) - 1);
                                                        }
                                                }
                                        }
                                }
                        }
                        ImGui::EndChild();
                        splitterV("##pyclssplit", &lw, 80.0f, 500.0f);

                        ImGui::BeginChild("##pyclscall", ImVec2(0, 0), false);
                        if (pySelClsIdx_ < 0 || pySelClsIdx_ >= (int)pyResult_.classes.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a class", "← 选择左侧类"));
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                                ImGui::EndTabBar();
                                return;
                        }
                        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
                        ImGui::PushStyleColor(
                            ImGuiCol_Text,
                            ui::ReadableTextColor(ImVec4(0.9f, 0.7f, 0.3f, 1.f), ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                        ImGui::Text("class %s", cls.name.c_str());
                        ImGui::PopStyleColor();

                        // Object manager
                        ImGui::Spacing();
                        ImGui::Text("%s:", tr("Objects", "对象管理"));
                        ImGui::SameLine();
                        if (!runnerOk)
                                ImGui::BeginDisabled();
                        if (ImGui::Button(tr("+ New", "+ 新建")))
                                doNewPyObject();
                        if (!runnerOk)
                                ImGui::EndDisabled();
                        if (!pyObjects_.empty()) {
                                ImGui::SameLine();
                                bool canDel = (pySelObjIdx_ >= 0 && pySelObjIdx_ < (int)pyObjects_.size());
                                if (!canDel)
                                        ImGui::BeginDisabled();
                                if (ImGui::Button(tr("Delete", "删除")))
                                        doDeletePyObject(pySelObjIdx_);
                                if (!canDel)
                                        ImGui::EndDisabled();

                                ImGui::BeginChild("##pyobjlist", ImVec2(0, 60.0f), true);
                                for (int oi = 0; oi < (int)pyObjects_.size(); ++oi) {
                                        bool sel = (oi == pySelObjIdx_);
                                        char olbl[128];
                                        snprintf(olbl,
                                                 sizeof(olbl),
                                                 "%s  [id=%d]",
                                                 pyObjects_[oi].label.c_str(),
                                                 pyObjects_[oi].pyId);
                                        if (ImGui::Selectable(olbl, sel))
                                                pySelObjIdx_ = oi;
                                }
                                ImGui::EndChild();
                        }
                        ImGui::Separator();

                        if (pySelMethIdx_ < 0 || pySelMethIdx_ >= (int)cls.methods.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a method", "← 选择左侧方法"));
                        } else {
                                const PyFuncDecl &meth   = cls.methods[pySelMethIdx_];
                                bool              isInit = (meth.name == "__init__");
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                      ui::ReadableTextColor(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                                                                            ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                                ImGui::Text("def %s.%s(...)", cls.name.c_str(), meth.name.c_str());
                                ImGui::PopStyleColor();

                                if (meth.params.empty()) {
                                        ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
                                } else {
                                        static ImGuiTableFlags pyMTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                             ImGuiTableFlags_SizingStretchProp |
                                                                             ImGuiTableFlags_Resizable;
                                        if (ui::TableIsland tableCard; tableCard.begin(
                                                "##pymarg_tbl",
                                                3,
                                                pyMTblFlags | ImGuiTableFlags_ScrollX,
                                                ImVec2(0, 0),
                                                std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 34.0f))) {
                                                ImGui::TableSetupColumn(tr("#", "#"),
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        ui::TableColumnWidth("#", "99", 2));
                                                ImGui::TableSetupColumn(
                                                    tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                                ImGui::TableSetupColumn(
                                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                                ImGui::TableHeadersRow();
                                                for (size_t i = 0; i < meth.params.size(); ++i) {
                                                        char id[64];
                                                        snprintf(id, sizeof(id), "##pyma%zu", i);
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::Text("%d", (int)(i + 1));
                                                        ImGui::TableSetColumnIndex(1);
                                                        ui::TableText(meth.params[i].name);
                                                        ImGui::TableSetColumnIndex(2);
                                                        if (i < pyMethArgBufs_.size()) {
                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                ImGui::InputText(
                                                                    id, pyMethArgBufs_[i].text, sizeof(ArgBuf::text));
                                                        }
                                                }
                                                tableCard.end();
                                        }
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();
                                bool canCall = runnerOk && (isInit || (!pyObjects_.empty() && pySelObjIdx_ >= 0));
                                if (!canCall)
                                        ImGui::BeginDisabled();
                                if (ui::Button(isInit ? tr("  CREATE  ", "  创建  ") : tr("  CALL  ", "  调用  "),
                                               ui::BtnStyle::Success))
                                        doCallPyMeth();
                                if (!canCall)
                                        ImGui::EndDisabled();
                                if (!canCall && runnerOk && !isInit && pyObjects_.empty()) {
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("%s", tr("(create an object first)", "(请先创建对象)"));
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                if (drawHistoryHeader("python_method")) {
                                        drawHistoryLog("##pymhist");
                                }
                        }
                        ImGui::EndChild();
                }
                ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
}

// ─── draw ──────────────────────────────────────────────────────────────────────

void
SdkPanel::draw()
{
        using ui::interaction::Action;
        if (navigationAction_ == Action::Parent || navigationAction_ == Action::Escape) {
                navigationLanguage_ = 0;
                navigationTab_      = 0;
                navigationSelect_   = true;
        }
        navigationAction_ = Action::None;
        if (navigationRoute_) {
                const auto &route = *navigationRoute_;
                if (route.size() >= 4 && route[1] == ':' && route[3] == ':' && route[0] >= '0' && route[0] <= '1' &&
                    route[2] >= '0' && route[2] <= '3') {
                        navigationLanguage_    = route[0] - '0';
                        navigationTab_         = route[2] - '0';
                        navigationTable_       = route.substr(4);
                        navigationTableSelect_ = true;
                        navigationSelect_      = true;
                }
                navigationRoute_.reset();
        }

        const int requestedLanguage = navigationLanguage_, requestedTab = navigationTab_;
        if (loader_.isLoaded())
                scheduleDataUpdate();
        pollHeaderChange();
        if (!open_)
                return;

        ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
        if (!dock_close::begin(titleBuf_, &open_)) {
                if (open_)
                        ui::interaction::registerRegion(
                            std::to_string(navigationLanguage_) + ":" + std::to_string(navigationTab_) + ":" + navigationTable_,
                            false,
                            false,
                            navigationLanguage_ != 0 || navigationTab_ != 0,
                            [this](Action action) { navigationAction_ = action; },
                            [this](const std::string &route) { navigationRoute_ = route; });
                ImGui::End();
                return;
        }

        TutorialGuide::instance().markDock(TutorialGuide::Topic::Library);
        // Double-click the title bar (or dock tab) to rename this window
        {
                ImGuiWindow *win       = ImGui::GetCurrentWindow();
                ImRect       titleRect = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                if (ImGui::IsMouseHoveringRect(titleRect.Min, titleRect.Max, false) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        snprintf(renameBuf_, sizeof(renameBuf_), "%s", userLabel_);
                        ImGui::OpenPopup("Rename SDK");
                }
                if (ImGui::BeginPopup("Rename SDK")) {
                        ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                        ImGui::SetNextItemWidth(220.0f);
                        if (ImGui::IsWindowAppearing())
                                ImGui::SetKeyboardFocusHere();
                        bool commit =
                            ImGui::InputText("##renameSdk",
                                             renameBuf_,
                                             sizeof(renameBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::SameLine();
                        if (ImGui::Button("OK"))
                                commit = true;
                        if (commit) {
                                if (renameBuf_[0] != '\0') {
                                        strncpy(userLabel_, renameBuf_, sizeof(userLabel_) - 1);
                                        userLabel_[sizeof(userLabel_) - 1] = '\0';
                                        snprintf(titleBuf_, sizeof(titleBuf_), "%s###SdkPanel%d", userLabel_, winId_);
                                        modified_ = true;
                                }
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
        }

        // Apply OS-level file drops if this window is hovered
        if (!pendingDropFiles_.empty()) {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_RootWindow)) {
                        for (const auto &f : pendingDropFiles_) {
                                if (f.size() > 4 && f.substr(f.size() - 4) == ".dll") {
                                        strncpy(dllPath_, f.c_str(), sizeof(dllPath_) - 1);
                                        dllPath_[sizeof(dllPath_) - 1] = '\0';
                                        doLoadDll();
                                        modified_ = true;
                                } else if ((f.size() > 2 && f.substr(f.size() - 2) == ".h") ||
                                           (f.size() > 4 && f.substr(f.size() - 4) == ".hpp")) {
                                        strncpy(headerPath_, f.c_str(), sizeof(headerPath_) - 1);
                                        headerPath_[sizeof(headerPath_) - 1] = '\0';
                                        doParseHeader();
                                        modified_ = true;
                                } else if (f.size() > 3 && f.substr(f.size() - 3) == ".py") {
                                        strncpy(pyPath_, f.c_str(), sizeof(pyPath_) - 1);
                                        pyPath_[sizeof(pyPath_) - 1] = '\0';
                                        doLoadPy();
                                        modified_ = true;
                                }
                        }
                }
                pendingDropFiles_.clear();
        }

        // Errors stay visible even when connection settings are collapsed
        if (statusIsErr_ && !statusMsg_.empty())
                ui::ErrorText("%s", statusMsg_.c_str());

        // ── Tabs ──
        ImGuiID configurationId   = 0;
        bool    configurationOpen = false;
        if (ImGui::BeginTabBar("##sdktabs")) {
                // ── C/C++ tab ────────────────────────────────────────────────
                if (ImGui::BeginTabItem(tr("C/C++", "C/C++"),
                                        nullptr,
                                        navigationSelect_ && requestedLanguage == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        navigationLanguage_      = 0;
                        configurationId          = ImGui::GetID("###sdkConfiguration");
                        auto *configurationState = ImGui::GetStateStorage();
                        if (configurationState->GetInt(configurationId, -1) < 0)
                                configurationState->SetBool(configurationId, !loader_.isLoaded());
                        configurationOpen = configurationState->GetBool(configurationId);
                        if (configurationOpen) {
                                ImGui::Spacing();
#ifdef _WIN32
                                ImGui::TextUnformatted(tr("Quick SDK", "快捷 SDK"));
                                for (size_t i = 0; i < std::size(sdkPresets); ++i) {
                                        const auto &preset = sdkPresets[i];
                                        const float width =
                                            ImGui::CalcTextSize(preset.label).x + ImGui::GetStyle().FramePadding.x * 2;
                                        const float nextX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                                        if (nextX + width < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
                                                ImGui::SameLine();
                                        if (ImGui::Button(preset.label))
                                                loadSdkPreset(i);
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip(
                                                    "%s\n%s",
                                                    tr(preset.descriptionEn, preset.descriptionZh),
                                                    tr("Load the library and header together", "自动加载配套动态库和头文件"));
                                }
                                ImGui::Separator();
#endif

                                // DLL browse row
                                if (ImGui::Button(tr("Browse DLL##dll", "浏览动态库##dll"))) {
                                        std::string p = nativeDlgOpen(tr("Select library", "选择动态库"),
#if defined(_WIN32)
                                                                      {{"DLL", {"dll"}}, {"All Files", {"*"}}});
#elif defined(__APPLE__)
                                                                      {{"dylib", {"dylib"}}, {"All Files", {"*"}}});
#else
                                                                      {{"Shared Library", {"so"}}, {"All Files", {"*"}}});
#endif
                                        if (!p.empty()) {
                                                strncpy(dllPath_, p.c_str(), sizeof(dllPath_) - 1);
                                                dllPath_[sizeof(dllPath_) - 1] = '\0';
                                                doLoadDll();
                                                modified_ = true;
                                        }
                                }
                                if (loader_.isLoaded()) {
                                        ImGui::SameLine();
                                        if (ui::Button(tr("Unload", "卸载"), ui::BtnStyle::Danger)) {
                                                stopDataUpdate();
                                                std::lock_guard callLock(sdkCallMtx_);
                                                loader_.unload();
                                                setStatus(tr("Unloaded", "已卸载"), false);
                                        }
                                }
                                if (dllPath_[0]) {
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("— %s", dllPath_);
                                        showPathTooltipIfHovered(dllPath_);
                                }

                                // Header browse row
                                if (ImGui::Button(tr("Browse Header##hdr", "浏览头文件##hdr"))) {
                                        std::string p = nativeDlgOpen(tr("Select header file", "选择头文件"),
                                                                      {{"C/C++ Header", {"h", "hpp"}}, {"All Files", {"*"}}});
                                        if (!p.empty()) {
                                                strncpy(headerPath_, p.c_str(), sizeof(headerPath_) - 1);
                                                headerPath_[sizeof(headerPath_) - 1] = '\0';
                                                doParseHeader();
                                                modified_ = true;
                                        }
                                }
                                if (headerPath_[0]) {
                                        ImGui::SameLine();
                                        if (ImGui::Button(tr("Re-parse##hdr", "重新解析##hdr")))
                                                doParseHeader();
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip(
                                                    "%s",
                                                    tr("Re-read the header from disk (refreshes enums and layouts)",
                                                       "重新读取磁盘上的头文件（刷新枚举与结构体布局）"));
                                        ImGui::SameLine();
                                        if (ImGui::Checkbox(tr("Auto##hdrauto", "自动##hdrauto"), &headerAutoReparse_)) {
                                                headerPollAt_ = 0.0; // check on the next frame
                                                modified_     = true;
                                        }
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip(
                                                    "%s",
                                                    tr("Re-parse automatically when the header file changes on disk",
                                                       "头文件在磁盘上变动时自动重新解析"));
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("— %s", headerPath_);
                                        showPathTooltipIfHovered(headerPath_);
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();

                                if (!statusMsg_.empty() && !statusIsErr_)
                                        ui::SuccessText("%s", statusMsg_.c_str());
                        }

                        if (hasDsoGui_ && onDrawDsoGui_) {
                                const char *viewNames[] = {tr("Oscilloscope GUI", "示波器界面"),
                                                           tr("SDK functions", "SDK 函数调用")};
                                int         view        = showDsoGui_ ? 0 : 1;
                                if (ui::Choice("##sdk_view", &view, viewNames, 2)) {
                                        showDsoGui_ = view == 0;
                                        modified_   = true;
                                }
                        }
                        if (isDsoGuiActive()) {
                                ImGui::BeginChild("dsoSdkGui");
                                onDrawDsoGui_();
                                ImGui::EndChild();
                        } else {
                                bool hasFn      = !parseResult_.functions.empty();
                                bool hasCls     = !parseResult_.classes.empty();
                                bool hasData    = !dataTables_.empty();
                                bool hasSymbols = !loader_.exports().empty();
                                if (!hasFn && !hasCls && !hasData && !hasSymbols) {
                                        ImGui::TextDisabled(
                                            "%s",
                                            tr("Browse a DLL and header file to begin.", "浏览动态库和头文件以开始使用。"));
                                } else if (ImGui::BeginTabBar("##ccpptabs")) {
                                        if (hasData && ImGui::BeginTabItem(tr("Data tables", "数据表"),
                                                                           nullptr,
                                                                           navigationSelect_ && requestedTab == 0
                                                                               ? ImGuiTabItemFlags_SetSelected
                                                                               : 0)) {
                                                navigationTab_ = 0;
                                                drawDataTablesTab();
                                                ImGui::EndTabItem();
                                        }
                                        if (hasFn && ImGui::BeginTabItem(tr("Functions", "C 函数"),
                                                                         nullptr,
                                                                         navigationSelect_ && requestedTab == 1
                                                                             ? ImGuiTabItemFlags_SetSelected
                                                                             : 0)) {
                                                navigationTab_ = 1;
                                                drawCFunctionsTab();
                                                ImGui::EndTabItem();
                                        }
                                        if (hasCls && ImGui::BeginTabItem(tr("Classes", "C++ 类"),
                                                                          nullptr,
                                                                          navigationSelect_ && requestedTab == 2
                                                                              ? ImGuiTabItemFlags_SetSelected
                                                                              : 0)) {
                                                navigationTab_ = 2;
                                                drawCppClassesTab();
                                                ImGui::EndTabItem();
                                        }
                                        if (hasSymbols && ImGui::BeginTabItem(tr("Symbols", "符号"),
                                                                              nullptr,
                                                                              navigationSelect_ && requestedTab == 3
                                                                                  ? ImGuiTabItemFlags_SetSelected
                                                                                  : 0)) {
                                                navigationTab_ = 3;
                                                drawSymbolsTab();
                                                ImGui::EndTabItem();
                                        }
                                        ImGui::EndTabBar();
                                }
                        }
                        ImGui::EndTabItem();
                }
                // ── Python tab ───────────────────────────────────────────────
                if (ImGui::BeginTabItem(tr("Python", "Python"),
                                        nullptr,
                                        navigationSelect_ && requestedLanguage == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        navigationLanguage_ = 1;
                        ImGui::Spacing();
                        drawPythonTab();
                        ImGui::EndTabItem();
                }
                if (configurationId) {
                        std::string configurationLabel = tr("SDK settings", "SDK 配置");
                        if (dllPath_[0] && ImGui::GetWindowWidth() >= ImGui::GetFontSize() * 38) {
                                const std::string path  = dllPath_;
                                configurationLabel     += "  " + path.substr(path.find_last_of("/\\") + 1);
                        }
                        configurationLabel += "###sdkConfigurationAction";
                        ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(configurationLabel.c_str(), nullptr, true).x +
                                                             ImGui::GetStyle().FramePadding.x * 2,
                                                         ImGui::GetWindowWidth() * .5f));
                        ImGui::PushStyleColor(
                            ImGuiCol_Tab, ImGui::GetStyleColorVec4(configurationOpen ? ImGuiCol_TabSelected : ImGuiCol_Button));
                        if (ImGui::TabItemButton(configurationLabel.c_str(), ImGuiTabItemFlags_Trailing))
                                ImGui::GetStateStorage()->SetBool(configurationId, !configurationOpen);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s\n%s\n%s",
                                                  tr("Show / hide SDK settings", "展开 / 收起 SDK 配置"),
                                                  statusMsg_.c_str(),
                                                  dllPath_);
                }
                ImGui::EndTabBar();
        }

        if (navigationSelect_) {
                navigationLanguage_ = requestedLanguage;
                navigationTab_      = requestedTab;
        }
        navigationSelect_ = false;
        if (open_)
                ui::interaction::registerRegion(
                    std::to_string(navigationLanguage_) + ":" + std::to_string(navigationTab_) + ":" + navigationTable_,
                    false,
                    false,
                    navigationLanguage_ != 0 || navigationTab_ != 0,
                    [this](Action action) { navigationAction_ = action; },
                    [this](const std::string &route) { navigationRoute_ = route; });
        ImGui::End();
}

// ─── drawSymbolsTab ──────────────────────────────────────────────────────────

void
SdkPanel::drawSymbolsTab()
{
        const auto &syms = loader_.exports();

        ImGui::Text("%s: %d", tr("Total symbols", "符号总数"), (int)syms.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ui::SearchInput("##symfilter", tr("Filter symbols…", "过滤符号…"), symbolFilter_, sizeof(symbolFilter_));

        // Case-insensitive substring match
        auto containsCI = [](const std::string &hay, const char *needle) -> bool {
                if (!needle || !needle[0])
                        return true;
                std::string h = hay, n = needle;
                std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)tolower(c); });
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
                return h.find(n) != std::string::npos;
        };

        if (ui::SearchOpen("##symfilter")) {
                std::vector<ui::SearchResult> results;
                std::vector<int>              targets;
                for (int i = 0; i < static_cast<int>(syms.size()); ++i)
                        if (containsCI(syms[i].demthetad, symbolFilter_) || containsCI(syms[i].mthetad, symbolFilter_)) {
                                results.push_back({syms[i].demthetad, syms[i].mthetad});
                                targets.push_back(i);
                        }
                const int selected = ui::SearchResults("##symfilter", results);
                if (selected >= 0) {
                        symbolSearchSelected_ = targets[selected];
                        symbolSearchScroll_   = true;
                        symbolFilter_[0]      = 0;
                }
        }
        static ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ui::TableIsland tableCard;
            tableCard.begin("##symtbl",
                            3,
                            tblFlags | ImGuiTableFlags_ScrollX,
                            ImVec2(0, 0),
                            std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 56.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, ui::TableColumnWidth("#", "99999"));
                ImGui::TableSetupColumn(tr("Demangled name", "解析名称"), ImGuiTableColumnFlags_WidthStretch, 0.60f);
                ImGui::TableSetupColumn(tr("Symbol", "符号名"), ImGuiTableColumnFlags_WidthStretch, 0.40f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)syms.size(); ++i) {
                        const ExportedSymbol &s = syms[i];

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", i);
                        ImGui::TableSetColumnIndex(1);
                        if (symbolSearchSelected_ == i) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
                                if (symbolSearchScroll_) {
                                        ImGui::SetScrollHereY(0.5f);
                                        symbolSearchScroll_ = false;
                                }
                        }
                        ui::TableText(s.demthetad);
                        // Click a row's demthetad name to copy it to the clipboard
                        if (ImGui::IsItemClicked())
                                ImGui::SetClipboardText(s.demthetad.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ui::TableText(s.mthetad, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        if (ImGui::IsItemClicked())
                                ImGui::SetClipboardText(s.mthetad.c_str());
                }
                tableCard.end();
        }
}

// ─── pushHistory ─────────────────────────────────────────────────────────────

void
SdkPanel::pushHistory(const std::string &call, const std::string &result, bool ok, double tsSec)
{
        HistEntry e;
        e.call   = call;
        e.result = result;
        e.ok     = ok;
        e.tsMs =
            (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        e.tsSec = tsSec;
        std::lock_guard<std::mutex> lk(histMtx_);
        if (history_.size() >= 400)
                history_.erase(history_.begin());
        history_.push_back(std::move(e));
}

// ─── writeResultVar ──────────────────────────────────────────────────────────

void
SdkPanel::writeResultVar(const std::string &name, const CallResult &res, CType retType)
{
        if (name.empty() || !onWriteLocalScalar_)
                return;
        if (ctypeIsInteger(retType))
                onWriteLocalScalar_(name, (double)(int64_t)res.rawU64, false);
        else if (ctypeIsFloat(retType))
                onWriteLocalScalar_(name, res.rawF64, true);
}

// ─── evalCond ────────────────────────────────────────────────────────────────

bool
SdkPanel::evalCond(const SdkSeqStep &step, const std::unordered_map<std::string, int64_t> &vars)
{
        int64_t lhs = 0;
        {
                auto it = vars.find(step.condVar);
                if (it != vars.end())
                        lhs = it->second;
        }
        int64_t rhs = step.condVal;
        if (strcmp(step.condOp, "==") == 0)
                return lhs == rhs;
        else if (strcmp(step.condOp, "!=") == 0)
                return lhs != rhs;
        else if (strcmp(step.condOp, "<") == 0)
                return lhs < rhs;
        else if (strcmp(step.condOp, ">") == 0)
                return lhs > rhs;
        else if (strcmp(step.condOp, "<=") == 0)
                return lhs <= rhs;
        else if (strcmp(step.condOp, ">=") == 0)
                return lhs >= rhs;
        return false;
}

// ─── execStep / execSteps ─────────────────────────────────────────────────────

SdkPanel::StepResult
SdkPanel::execStep(const SdkSeqStep &step, SeqCtx &ctx)
{
        if (ctx.stopReq)
                return StepResult::Stop;

        switch (step.kind) {

                case SdkStepKind::Call: {
                        // Collect args from step
                        std::vector<std::string> args = step.args;
                        // Expand $var references in args
                        for (auto &a : args) {
                                if (!a.empty() && a[0] == '$') {
                                        auto it = ctx.vars.find(a.substr(1));
                                        if (it != ctx.vars.end())
                                                a = std::to_string(it->second);
                                }
                        }
                        CallResult  res;
                        double      ts = sessionTimeSec();
                        std::string callStr;
                        if (step.isCFunc) {
                                if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.functions.size()) {
                                        const CFuncDecl &fn = parseResult_.functions[step.classIdx];
                                        args.resize(fn.params.size());
                                        {
                                                std::lock_guard callLock(sdkCallMtx_);
                                                res = loader_.call(fn, args);
                                        }
                                        callStr = fn.name + "(";
                                        for (size_t i = 0; i < args.size(); ++i) {
                                                if (i)
                                                        callStr += ",";
                                                callStr += args[i];
                                        }
                                        callStr += ")";
                                }
                        } else {
                                if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size()) {
                                        const CClassDecl &cls = parseResult_.classes[step.classIdx];
                                        if (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size()) {
                                                const CMethodDecl &meth    = cls.methods[step.methodIdx];
                                                void              *thisPtr = nullptr;
                                                if (!meth.isCtor && step.objIdx >= 0 && step.objIdx < (int)objects_.size())
                                                        thisPtr = objects_[step.objIdx].ptr;
                                                args.resize(meth.params.size());
                                                // Build pointer-arg table: &varname args → Variable LOCAL buffers
                                                std::vector<void *>      structPtrs(args.size(), nullptr);
                                                std::vector<std::string> ptrVarNames(args.size());
                                                for (size_t i = 0; i < args.size(); ++i) {
                                                        if (!args[i].empty() && args[i][0] == '&' && onGetVarBuf_) {
                                                                std::string vn  = args[i].substr(1);
                                                                void       *buf = onGetVarBuf_(vn);
                                                                if (buf) {
                                                                        structPtrs[i]  = buf;
                                                                        ptrVarNames[i] = vn;
                                                                        args[i]        = "&" + vn; // keep display text
                                                                }
                                                        }
                                                }
                                                {
                                                        std::lock_guard callLock(sdkCallMtx_);
                                                        res = loader_.callMethod(thisPtr, meth, args, structPtrs);
                                                }
                                                // Notify Variable to refresh display for each pointer output
                                                if (res.ok && onVarWritten_) {
                                                        for (const auto &vn : ptrVarNames)
                                                                if (!vn.empty())
                                                                        onVarWritten_(vn);
                                                }
                                                callStr = cls.name + "::" + meth.name + "(";
                                                for (size_t i = 0; i < args.size(); ++i) {
                                                        if (i)
                                                                callStr += ",";
                                                        callStr += args[i];
                                                }
                                                callStr += ")";
                                        }
                                }
                        }
                        std::string display = res.ok ? res.display : res.error;
                        pushHistory(callStr, display, res.ok, ts);

                        // Store result in variable
                        if (step.resultVar[0] != '\0') {
                                int64_t v = 0;
                                if (res.ok)
                                        memcpy(&v, &res.rawU64, sizeof(int64_t));
                                ctx.vars[step.resultVar] = v;
                        }

                        // Append to seq log
                        {
                                std::lock_guard<std::mutex> lk(seqMtx_);
                                seqLog_.push_back(callStr + " → " + display);
                                if (seqLog_.size() > 200)
                                        seqLog_.erase(seqLog_.begin());
                        }
                        break;
                }

                case SdkStepKind::Sleep: {
                        int remaining = step.sleepMs;
                        while (remaining > 0 && !ctx.stopReq) {
                                int chunk = std::min(remaining, 50);
                                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                                remaining -= chunk;
                        }
                        break;
                }

                case SdkStepKind::If: {
                        if (evalCond(step, ctx.vars))
                                return execSteps(step.body, ctx);
                        else if (step.hasElse)
                                return execSteps(step.elseBody, ctx);
                        break;
                }

                case SdkStepKind::While: {
                        while (!ctx.stopReq && evalCond(step, ctx.vars)) {
                                StepResult r = execSteps(step.body, ctx);
                                if (r == StepResult::Stop)
                                        return StepResult::Stop;
                                if (r == StepResult::Break)
                                        break;
                        }
                        break;
                }

                case SdkStepKind::For: {
                        int64_t &v = ctx.vars[step.forVar];
                        v          = step.forFrom;
                        while (!ctx.stopReq) {
                                if (step.forStep > 0 && v >= step.forTo)
                                        break;
                                if (step.forStep < 0 && v <= step.forTo)
                                        break;
                                StepResult r = execSteps(step.body, ctx);
                                if (r == StepResult::Stop)
                                        return StepResult::Stop;
                                if (r == StepResult::Break)
                                        break;
                                v += step.forStep;
                        }
                        break;
                }

                case SdkStepKind::Break:
                        return StepResult::Break;

                case SdkStepKind::Print: {
                        std::string msg = step.message;
                        // Expand $var references
                        for (auto &[k, v] : ctx.vars) {
                                std::string token = "$" + k;
                                size_t      pos   = 0;
                                while ((pos = msg.find(token, pos)) != std::string::npos) {
                                        msg.replace(pos, token.size(), std::to_string(v));
                                        pos += std::to_string(v).size();
                                }
                        }
                        std::lock_guard<std::mutex> lk(seqMtx_);
                        seqLog_.push_back("[Print] " + msg);
                        if (seqLog_.size() > 200)
                                seqLog_.erase(seqLog_.begin());
                        break;
                }

                default:
                        break;
        }
        return StepResult::Ok;
}

SdkPanel::StepResult
SdkPanel::execSteps(const std::vector<SdkSeqStep> &steps, SeqCtx &ctx)
{
        for (const auto &step : steps) {
                if (ctx.stopReq)
                        return StepResult::Stop;
                StepResult r = execStep(step, ctx);
                if (r != StepResult::Ok)
                        return r;
        }
        return StepResult::Ok;
}

// ─── drawStepEditor ──────────────────────────────────────────────────────────

void
// depth
SdkPanel::drawStepEditor(SdkSeqStep &step, int)
{

        // Kind selector
        const char *kinds[] = {"Call", "Sleep", "If", "While", "For", "Break", "Print"};
        int         kindIdx = (int)step.kind;
        ImGui::SetNextItemWidth(90.0f);
        char kindId[32];
        snprintf(kindId, sizeof(kindId), "##k%p", &step);
        if (ui::Choice(kindId, &kindIdx, kinds, 7))
                step.kind = (SdkStepKind)kindIdx;
        ImGui::SameLine();

        switch (step.kind) {

                case SdkStepKind::Call: {
                        // Class selector
                        if (!parseResult_.classes.empty()) {
                                const char *clsPrev = (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size())
                                                          ? parseResult_.classes[step.classIdx].name.c_str()
                                                          : "---";
                                char        clsId[32];
                                snprintf(clsId, sizeof(clsId), "##cls%p", &step);
                                ImGui::SetNextItemWidth(100.0f);
                                if (ImGui::BeginCombo(clsId, clsPrev)) {
                                        for (int ci = 0; ci < (int)parseResult_.classes.size(); ++ci) {
                                                bool sel = (ci == step.classIdx);
                                                if (ImGui::Selectable(parseResult_.classes[ci].name.c_str(), sel)) {
                                                        step.classIdx  = ci;
                                                        step.methodIdx = 0;
                                                }
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                        }
                        // Method selector
                        if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size()) {
                                const CClassDecl &cls      = parseResult_.classes[step.classIdx];
                                const char       *methPrev = (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size())
                                                                 ? cls.methods[step.methodIdx].name.c_str()
                                                                 : "---";
                                char              mId[32];
                                snprintf(mId, sizeof(mId), "##m%p", &step);
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::BeginCombo(mId, methPrev)) {
                                        for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                                                bool sel = (mi == step.methodIdx);
                                                if (ImGui::Selectable(cls.methods[mi].name.c_str(), sel))
                                                        step.methodIdx = mi;
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                                // Args: ensure correct size
                                int nparams = (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size())
                                                  ? (int)cls.methods[step.methodIdx].params.size()
                                                  : 0;
                                step.args.resize(nparams);
                                for (int ai = 0; ai < nparams; ++ai) {
                                        const CMethodDecl &cm = cls.methods[step.methodIdx];
                                        // Detect pointer (T*) and reference (T& / T&&) params — both are passed as address
                                        bool isPtr = (ai < (int)cm.params.size()) &&
                                                     (!cm.params[ai].rawType.empty() && (cm.params[ai].rawType.back() == '*' ||
                                                                                         cm.params[ai].rawType.back() == '&'));
                                        char argId[32];
                                        snprintf(argId, sizeof(argId), "##a%p_%d", &step, ai);
                                        ImGui::SetNextItemWidth(isPtr ? 100.0f : 80.0f);
                                        if (isPtr)
                                                ImGui::PushStyleColor(
                                                    ImGuiCol_FrameBg,
                                                    ui::MixSurface(ImGui::GetStyle().Colors[ImGuiCol_FrameBg],
                                                                   ImGui::GetStyle().Colors[ImGuiCol_CheckMark],
                                                                   0.10f));
                                        char argBuf[512];
                                        strncpy(argBuf, step.args[ai].c_str(), 511);
                                        argBuf[511] = '\0';
                                        if (ImGui::InputTextWithHint(argId, isPtr ? "&varname" : "value", argBuf, 512))
                                                step.args[ai] = argBuf;
                                        if (isPtr) {
                                                ImGui::PopStyleColor();
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Pointer/reference param — type &varname to pass\n"
                                                                             "a LOCAL Variable buffer (Variable manager).\n"
                                                                             "Or enter a plain value to pass directly.",
                                                                             "指针/引用参数 — 输入 &varname 传入变量管理器的\n"
                                                                             "LOCAL 变量缓冲区，或直接输入数值。"));
                                                // Variable picker: shows all LOCAL variable names as a dropdown
                                                if (onListLocalVars_) {
                                                        char popId[56];
                                                        snprintf(popId, sizeof(popId), "##vp%p_%d", &step, ai);
                                                        char btnId[56];
                                                        snprintf(btnId, sizeof(btnId), "v##vb%p_%d", &step, ai);
                                                        ImGui::SameLine(0, 2);
                                                        if (ImGui::SmallButton(btnId))
                                                                ImGui::OpenPopup(popId);
                                                        if (ImGui::IsItemHovered())
                                                                ImGui::SetTooltip("%s",
                                                                                  tr("Pick LOCAL variable", "选择 LOCAL 变量"));
                                                        if (ImGui::BeginPopup(popId)) {
                                                                auto localVars = onListLocalVars_();
                                                                if (localVars.empty())
                                                                        ImGui::TextDisabled(
                                                                            "%s", tr("No LOCAL variables", "暂无 LOCAL 变量"));
                                                                else
                                                                        for (const auto &vn : localVars)
                                                                                if (ImGui::Selectable(vn.c_str()))
                                                                                        step.args[ai] = "&" + vn;
                                                                ImGui::EndPopup();
                                                        }
                                                }
                                        }
                                        ImGui::SameLine();
                                }
                        }
                        // Result var
                        ImGui::SetNextItemWidth(80.0f);
                        char rvId[32];
                        snprintf(rvId, sizeof(rvId), "##rv%p", &step);
                        ImGui::InputText(rvId, step.resultVar, sizeof(step.resultVar));
                        break;
                }

                case SdkStepKind::Sleep: {
                        char sid[32];
                        snprintf(sid, sizeof(sid), "##sl%p", &step);
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt(sid, &step.sleepMs);
                        ImGui::SameLine();
                        ImGui::TextDisabled("ms");
                        break;
                }

                case SdkStepKind::If:
                case SdkStepKind::While: {
                        char vid[32];
                        snprintf(vid, sizeof(vid), "##cv%p", &step);
                        ImGui::SetNextItemWidth(70.0f);
                        ImGui::InputText(vid, step.condVar, sizeof(step.condVar));
                        ImGui::SameLine();
                        const char *ops[] = {"==", "!=", "<", ">", "<=", ">="};
                        int         opIdx = 0;
                        for (int oi = 0; oi < 6; ++oi)
                                if (strcmp(step.condOp, ops[oi]) == 0) {
                                        opIdx = oi;
                                        break;
                                }
                        char opId[32];
                        snprintf(opId, sizeof(opId), "##op%p", &step);
                        ImGui::SetNextItemWidth(50.0f);
                        if (ui::Choice(opId, &opIdx, ops, 6))
                                strncpy(step.condOp, ops[opIdx], 3);
                        ImGui::SameLine();
                        char vId[32];
                        snprintf(vId, sizeof(vId), "##cval%p", &step);
                        ImGui::SetNextItemWidth(70.0f);
                        int iv = (int)step.condVal;
                        if (ImGui::InputInt(vId, &iv))
                                step.condVal = iv;
                        if (step.kind == SdkStepKind::If) {
                                ImGui::SameLine();
                                char cbId[32];
                                snprintf(cbId, sizeof(cbId), "##else%p", &step);
                                ImGui::Checkbox(tr("else", "否则"), &step.hasElse);
                        }
                        break;
                }

                case SdkStepKind::For: {
                        char fvId[32];
                        snprintf(fvId, sizeof(fvId), "##fv%p", &step);
                        ImGui::SetNextItemWidth(60.0f);
                        ImGui::InputText(fvId, step.forVar, sizeof(step.forVar));
                        ImGui::SameLine();
                        ImGui::TextDisabled("=");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.0f);
                        char ffId[32];
                        snprintf(ffId, sizeof(ffId), "##ff%p", &step);
                        int fi = (int)step.forFrom;
                        if (ImGui::InputInt(ffId, &fi))
                                step.forFrom = fi;
                        ImGui::SameLine();
                        ImGui::TextDisabled(";..<");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.0f);
                        char ftId[32];
                        snprintf(ftId, sizeof(ftId), "##ft%p", &step);
                        int ft = (int)step.forTo;
                        if (ImGui::InputInt(ftId, &ft))
                                step.forTo = ft;
                        ImGui::SameLine();
                        ImGui::TextDisabled(";+=");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(45.0f);
                        char fsId[32];
                        snprintf(fsId, sizeof(fsId), "##fs%p", &step);
                        int fs = (int)step.forStep;
                        if (ImGui::InputInt(fsId, &fs))
                                step.forStep = fs ? fs : 1;
                        break;
                }

                case SdkStepKind::Print: {
                        char msgId[32];
                        snprintf(msgId, sizeof(msgId), "##msg%p", &step);
                        ImGui::SetNextItemWidth(300.0f);
                        ImGui::InputText(msgId, step.message, sizeof(step.message));
                        break;
                }

                default:
                        break;
        }
}

// ─── drawStepList ─────────────────────────────────────────────────────────────

void
SdkPanel::drawStepList(std::vector<SdkSeqStep> &steps, int depth, std::vector<SdkSeqStep> **insertParent, int *insertAfter)
{
        for (int si = 0; si < (int)steps.size(); ++si) {
                SdkSeqStep &step = steps[si];

                // Row: expand arrow + kind badge + editor inline
                ImGui::PushID(&step);
                const char  *kinds[]      = {"CALL", "SLEEP", "IF", "WHILE", "FOR", "BREAK", "PRINT"};
                const ImVec4 kindColors[] = {{0.3f, 0.8f, 1.0f, 1.f},
                                             {0.8f, 0.8f, 0.3f, 1.f},
                                             {1.f, 0.6f, 0.3f, 1.f},
                                             {1.f, 0.4f, 0.8f, 1.f},
                                             {0.4f, 1.f, 0.6f, 1.f},
                                             {1.f, 0.3f, 0.3f, 1.f},
                                             {0.7f, 0.7f, 0.7f, 1.f}};
                int          ki           = (int)step.kind;

                // Delete button
                const float rowIndent = float(depth) * 16.0f;
                if (rowIndent > 0.0f)
                        ImGui::Indent(rowIndent);
                bool del = ui::SmallButton("✕", ui::BtnStyle::Danger);
                ImGui::SameLine();

                // Toggle expand for compound steps
                bool hasBody =
                    (step.kind == SdkStepKind::If || step.kind == SdkStepKind::While || step.kind == SdkStepKind::For);
                if (hasBody) {
                        if (ImGui::SmallButton(step.expanded ? "▼" : "▶"))
                                step.expanded = !step.expanded;
                        ImGui::SameLine();
                }

                // Kind badge
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ui::ReadableTextColor(kindColors[ki], ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
                ImGui::TextUnformatted(kinds[ki]);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (rowIndent > 0.0f)
                        ImGui::Unindent(rowIndent);

                // Inline editor (cursor is already positioned right after the badge via SameLine)
                drawStepEditor(step, 0);

                // Add-after button
                ImGui::Indent(float(depth) * 16.0f + 20.0f);
                char addId[32];
                snprintf(addId, sizeof(addId), "+##add%d", si);
                if (ImGui::SmallButton(addId)) {
                        *insertParent = &steps;
                        *insertAfter  = si;
                }
                ImGui::SameLine();
                ImGui::Unindent(float(depth) * 16.0f + 20.0f);
                ImGui::Separator();

                // Nested body
                if (hasBody && step.expanded) {
                        drawStepList(step.body, depth + 1, insertParent, insertAfter);
                        // Add-to-body button
                        ImGui::Indent(float(depth + 1) * 16.0f);
                        char bodyId[32];
                        snprintf(bodyId, sizeof(bodyId), "+ body##ab%p", &step);
                        if (ImGui::SmallButton(bodyId)) {
                                *insertParent = &step.body;
                                *insertAfter  = (int)step.body.size() - 1;
                        }
                        ImGui::Unindent(float(depth + 1) * 16.0f);

                        // Else body
                        if (step.kind == SdkStepKind::If && step.hasElse) {
                                if (rowIndent > 0.0f)
                                        ImGui::Indent(rowIndent);
                                ImGui::TextDisabled("else:");
                                if (rowIndent > 0.0f)
                                        ImGui::Unindent(rowIndent);
                                drawStepList(step.elseBody, depth + 1, insertParent, insertAfter);
                                ImGui::Indent(float(depth + 1) * 16.0f);
                                char elseId[32];
                                snprintf(elseId, sizeof(elseId), "+ else##ae%p", &step);
                                if (ImGui::SmallButton(elseId)) {
                                        *insertParent = &step.elseBody;
                                        *insertAfter  = (int)step.elseBody.size() - 1;
                                }
                                ImGui::Unindent(float(depth + 1) * 16.0f);
                        }
                }

                ImGui::PopID();

                if (del) {
                        steps.erase(steps.begin() + si);
                        break;
                }
        }
}

// ─── Public info helpers (used by SequenceEditor) ────────────────────────────

std::string
SdkPanel::getClassName(int classIdx) const
{
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return "";
        return parseResult_.classes[classIdx].name;
}

std::string
SdkPanel::getCallLabel(int ci, int mi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "?::?";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return cls.name + "::?";
        return cls.name + "::" + cls.methods[mi].name;
}

std::string
SdkPanel::getCFuncLabel(int fi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "?";
        return parseResult_.functions[fi].name;
}

int
SdkPanel::getParamCount(int ci, int mi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return 0;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return 0;
        return (int)cls.methods[mi].params.size();
}

int
SdkPanel::getCFuncParamCount(int fi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return 0;
        return (int)parseResult_.functions[fi].params.size();
}

bool
SdkPanel::isParamPtrOrRef(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return false;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return false;
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return false;
        const auto &rt = m.params[pi].rawType;
        return !rt.empty() && (rt.back() == '*' || rt.back() == '&');
}

bool
SdkPanel::isCFuncParamPtrOrRef(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return false;
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return false;
        const auto &rt = fn.params[pi].rawType;
        return !rt.empty() && (rt.back() == '*' || rt.back() == '&');
}

// ─── getParamName / getCFuncParamName / getParamRawType / getCFuncParamRawType ─

std::string
SdkPanel::getParamName(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return "";
        return m.params[pi].name;
}

std::string
SdkPanel::getCFuncParamName(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "";
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return "";
        return fn.params[pi].name;
}

std::string
SdkPanel::getParamRawType(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return "";
        return m.params[pi].rawType;
}

std::string
SdkPanel::getCFuncParamRawType(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "";
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return "";
        return fn.params[pi].rawType;
}

std::string
// pyFuncName
SdkPanel::getCallReturnType(bool isCFunc, bool isPython, int classIdx, int methodIdx, const std::string &) const
{
        if (isPython)
                return "";
        if (isCFunc) {
                if (methodIdx < 0 || methodIdx >= (int)parseResult_.functions.size())
                        return "";
                const CFuncDecl &fn = parseResult_.functions[methodIdx];
                return fn.retRaw.empty() ? std::string(ctypeLabel(fn.retType)) : fn.retRaw;
        }
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[classIdx];
        if (methodIdx < 0 || methodIdx >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[methodIdx];
        return m.retRaw.empty() ? std::string(ctypeLabel(m.retType)) : m.retRaw;
}

const CStructDecl *
SdkPanel::getParamStructDecl(bool isCFunc, int ci, int mi, int pi) const
{
        if (isCFunc) {
                // mi is the function index (same convention as getCFuncParamRawType)
                if (mi < 0 || mi >= (int)parseResult_.functions.size())
                        return nullptr;
                const CFuncDecl &fn = parseResult_.functions[mi];
                if (pi < 0 || pi >= (int)fn.params.size())
                        return nullptr;
                return findParamStruct(fn.params[pi]);
        }
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return nullptr;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return nullptr;
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return nullptr;
        return findParamStruct(m.params[pi]);
}

// ─── listObjects / newObject ─────────────────────────────────────────────────

std::vector<SdkPanel::ObjInfo>
SdkPanel::listObjects(int classIdx) const
{
        std::vector<ObjInfo> res;
        std::string          filterClass;
        if (classIdx >= 0 && classIdx < (int)parseResult_.classes.size())
                filterClass = parseResult_.classes[classIdx].fullName;
        for (int i = 0; i < (int)objects_.size(); ++i) {
                const ObjInstance &o = objects_[i];
                if (!filterClass.empty() && o.className != filterClass)
                        continue;
                res.push_back({i, std::string(o.label), o.className});
        }
        return res;
}

int
SdkPanel::newObject(int classIdx)
{
        int prev = (int)objects_.size();
        doNewObject(classIdx);
        return ((int)objects_.size() > prev) ? (int)objects_.size() - 1 : -1;
}

// ─── directCall / directCallC ────────────────────────────────────────────────

SdkPanel::DirectCallResult
SdkPanel::directCall(int ci, int mi, int objIdx, const std::vector<std::string> &args)
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return {false, "invalid class"};
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return {false, "invalid method"};
        const CMethodDecl &meth = cls.methods[mi];

        void *thisPtr = nullptr;
        if (!meth.isCtor && objIdx >= 0 && objIdx < (int)objects_.size())
                thisPtr = objects_[objIdx].ptr;

        if (!meth.isCtor && !meth.isStatic && !thisPtr)
                return {false, "no object — create one first"};

        auto effArgs = args;
        effArgs.resize(meth.params.size());
        std::vector<void *>      structPtrs(effArgs.size(), nullptr);
        std::vector<std::string> ptrVarNames(effArgs.size());
        for (size_t i = 0; i < effArgs.size(); ++i) {
                if (!effArgs[i].empty() && effArgs[i][0] == '&' && onGetVarBuf_) {
                        std::string vn  = effArgs[i].substr(1);
                        void       *buf = onGetVarBuf_(vn);
                        if (buf) {
                                structPtrs[i]  = buf;
                                ptrVarNames[i] = vn;
                        }
                } else if (effArgs[i].size() > 1 && effArgs[i][0] == '$' && onReadLocalVar_) {
                        double v = 0;
                        if (onReadLocalVar_(effArgs[i].substr(1), v)) {
                                char b[64];
                                if (i < meth.params.size() && ctypeIsFloat(meth.params[i].type))
                                        snprintf(b, sizeof(b), "%g", v);
                                else
                                        snprintf(b, sizeof(b), "%lld", (long long)(int64_t)v);
                                effArgs[i] = b;
                        }
                }
        }

        CallResult res;
        {
                std::lock_guard callLock(sdkCallMtx_);
                res = loader_.callMethod(thisPtr, meth, effArgs, structPtrs);
        }
        if (res.ok && onVarWritten_)
                for (const auto &vn : ptrVarNames)
                        if (!vn.empty())
                                onVarWritten_(vn);

        std::string disp = res.ok ? res.display : res.error;
        // Sequence-editor calls keep their own log (seqLog_) and result-var handling

        return {res.ok, disp, res.ok ? (int64_t)res.rawU64 : 0};
}

SdkPanel::DirectCallResult
SdkPanel::directCallC(int fi, const std::vector<std::string> &args)
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return {false, "invalid function"};
        const CFuncDecl &fn = parseResult_.functions[fi];

        auto effArgs = args;
        effArgs.resize(fn.params.size());
        std::vector<std::string> ptrVarNames(effArgs.size());
        for (size_t i = 0; i < effArgs.size(); ++i) {
                if (!effArgs[i].empty() && effArgs[i][0] == '&' && onGetVarBuf_) {
                        std::string vn  = effArgs[i].substr(1);
                        void       *buf = onGetVarBuf_(vn);
                        if (buf) {
                                char addrStr[32];
                                snprintf(addrStr, sizeof(addrStr), "0x%llx", (unsigned long long)(uintptr_t)buf);
                                effArgs[i]     = addrStr;
                                ptrVarNames[i] = vn;
                        }
                } else if (effArgs[i].size() > 1 && effArgs[i][0] == '$' && onReadLocalVar_) {
                        double v = 0;
                        if (onReadLocalVar_(effArgs[i].substr(1), v)) {
                                char b[64];
                                if (i < fn.params.size() && ctypeIsFloat(fn.params[i].type))
                                        snprintf(b, sizeof(b), "%g", v);
                                else
                                        snprintf(b, sizeof(b), "%lld", (long long)(int64_t)v);
                                effArgs[i] = b;
                        }
                }
        }

        CallResult res;
        {
                std::lock_guard callLock(sdkCallMtx_);
                res = loader_.call(fn, effArgs);
        }
        if (res.ok && onVarWritten_)
                for (const auto &vn : ptrVarNames)
                        if (!vn.empty())
                                onVarWritten_(vn);

        std::string disp = res.ok ? res.display : res.error;
        // Sequence-editor calls keep their own log (seqLog_) and result-var handling

        return {res.ok, disp, res.ok ? (int64_t)res.rawU64 : 0};
}

// ─── directCallPy / Python info helpers ──────────────────────────────────────

int
SdkPanel::getPyFuncParamCount(const std::string &funcName) const
{
        for (const auto &fn : pyResult_.functions)
                if (fn.name == funcName)
                        return (int)fn.params.size();
        return 0;
}

std::string
SdkPanel::getPyFuncParamName(const std::string &funcName, int i) const
{
        for (const auto &fn : pyResult_.functions)
                if (fn.name == funcName)
                        return (i >= 0 && i < (int)fn.params.size()) ? fn.params[i].name : "";
        return "";
}

std::string
SdkPanel::getPyFuncLabel(const std::string &funcName) const
{
        return funcName;
}

SdkPanel::DirectCallResult
SdkPanel::directCallPy(const std::string &funcName, const std::vector<std::string> &args)
{
        // Start runner on demand if not running
        if (!pyRunner_ || !pyRunner_->isAlive())
                doStartPyRunner();
        if (!pyRunner_ || !pyRunner_->isAlive())
                return {false, "Python not running"};

        const PyFuncDecl *fn = nullptr;
        for (const auto &f : pyResult_.functions)
                if (f.name == funcName) {
                        fn = &f;
                        break;
                }
        if (!fn)
                return {false, "function not found: " + funcName};

        cJSON *jargs = cJSON_CreateArray();
        for (size_t i = 0; i < fn->params.size(); ++i)
                cJSON_AddItemToArray(jargs, cJSON_CreateString(i < args.size() ? args[i].c_str() : ""));
        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_func");
        cJSON_AddStringToObject(cmd, "name", funcName.c_str());
        cJSON_AddItemToObject(cmd, "args", jargs);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        std::string result = rr.ok ? rr.result : rr.error;
        std::string call   = funcName + "(";
        for (size_t i = 0; i < fn->params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < args.size() ? args[i] : "");
        }
        call += ")";
        // Sequence-editor calls keep their own log (seqLog_); don't mirror into the
        // SDK panel's call history
        // Parse result for variable write-back
        int64_t rawV = 0;
        double  rawD = 0.0;
        if (rr.ok && !rr.result.empty()) {
                rawV = (int64_t)strtoll(rr.result.c_str(), nullptr, 0);
                rawD = strtod(rr.result.c_str(), nullptr);
        }
        return {rr.ok, result, rawV, rawD};
}

// ─── save / load ──────────────────────────────────────────────────────────────

void
// baseDir
SdkPanel::save(void *node, const std::string &) const
{
        cJSON *obj = static_cast<cJSON *>(node);
        cJSON_AddNumberToObject(obj, "winId", winId_);
        cJSON_AddStringToObject(obj, "userLabel", userLabel_);
        cJSON_AddBoolToObject(obj, "hasDsoGui", hasDsoGui_);
        cJSON_AddBoolToObject(obj, "showDsoGui", showDsoGui_);

        cJSON_AddStringToObject(obj, "dllPath", dllPath_);
        cJSON_AddStringToObject(obj, "headerPath", headerPath_);
        cJSON_AddStringToObject(obj, "pyPath", pyPath_);
        cJSON_AddBoolToObject(obj, "historyExpanded", historyExpanded_);
        cJSON_AddBoolToObject(obj, "headerAutoReparse", headerAutoReparse_);
        {
                std::lock_guard lock(dataTableMtx_);
                cJSON_AddNumberToObject(obj, "dataUpdateHz", rtUpdateHz_);
                cJSON *tables = cJSON_AddArrayToObject(obj, "dataTables");
                for (const auto &table : dataTables_) {
                        cJSON *entry = cJSON_CreateObject();
                        cJSON_AddStringToObject(entry, "name", table.structName.c_str());
                        // Realtime tables mix feedback with live actuator commands
                        // (enable/mode/motion). Persisting their raw bytes or an
                        // armed periodic write would silently resend a stale command
                        // to real hardware the moment a future session reloads and
                        // the master reconnects — Param tables are just daemon/adapter
                        // config, so those round-trip normally
                        const bool                 persistState = table.kind == DataTableKind::Param;
                        const std::vector<uint8_t> blank(table.data.size(), 0);
                        const std::string          dataHex = sdkBytesToHex(persistState ? table.data : blank);
                        cJSON_AddStringToObject(entry, "dataHex", dataHex.c_str());
                        /*
                         * 把"哪些表/字段被用户拖进过变量管理器"也存下来：否则会话重载后
                         * 这些标记丢失，数据不会自动刷回监视器，还得重新拖一次 = 手柄。
                         */
                        cJSON_AddBoolToObject(entry, "published", table.rootPublished);
                        std::string publishedFields;
                        publishedFields.reserve(table.fieldPublished.size());
                        for (bool p : table.fieldPublished)
                                publishedFields.push_back(p ? '1' : '0');
                        cJSON_AddStringToObject(entry, "publishedFields", publishedFields.c_str());
                        cJSON_AddItemToArray(tables, entry);
                }
        }
}

void
SdkPanel::load(const void *node, const std::string &baseDir)
{
        const cJSON *obj = static_cast<const cJSON *>(node);

        if (const cJSON *w = cJSON_GetObjectItem(obj, "winId"); cJSON_IsNumber(w))
                setWindowId(w->valueint);
        if (const cJSON *l = cJSON_GetObjectItem(obj, "userLabel"); cJSON_IsString(l) && l->valuestring[0]) {
                std::string label = l->valuestring;
                for (const auto *prefix : {"SDK 调用器 [", "SDK调用器 [", "SDK Caller ["})
                        if (label == std::string(prefix) + std::to_string(winId_) + "]")
                                label = "动态库调用器 [" + std::to_string(winId_) + "]";
                strncpy(userLabel_, label.c_str(), sizeof(userLabel_) - 1);
                userLabel_[sizeof(userLabel_) - 1] = '\0';
                snprintf(titleBuf_, sizeof(titleBuf_), "%s###SdkPanel%d", userLabel_, winId_);
        }
        if (const cJSON *expanded = cJSON_GetObjectItem(obj, "historyExpanded"); cJSON_IsBool(expanded))
                historyExpanded_ = cJSON_IsTrue(expanded);
        if (const cJSON *autoRe = cJSON_GetObjectItem(obj, "headerAutoReparse"); cJSON_IsBool(autoRe))
                headerAutoReparse_ = cJSON_IsTrue(autoRe);
        if (const cJSON *hz = cJSON_GetObjectItem(obj, "dataUpdateHz"); cJSON_IsNumber(hz))
                rtUpdateHz_ = std::clamp(static_cast<float>(hz->valuedouble), 1.0f, 10000.0f);

        auto toAbs = [&](const char *stored) -> std::string {
                if (!stored || !stored[0])
                        return "";
                // If the stored path is already absolute (new format), use it directly
                std::error_code       ec;
                std::filesystem::path fp(stored);
                if (fp.is_absolute()) {
                        auto abs = std::filesystem::canonical(fp, ec);
                        return ec ? stored : abs.generic_string();
                }
                // Legacy: stored as relative path → resolve from baseDir
                if (baseDir.empty())
                        return stored;
                std::filesystem::path joined = std::filesystem::path(baseDir) / fp;
                auto                  abs    = std::filesystem::canonical(joined, ec);
                return ec ? joined.generic_string() : abs.generic_string();
        };

        if (const cJSON *d = cJSON_GetObjectItem(obj, "dllPath"); cJSON_IsString(d) && d->valuestring[0]) {
                std::string abs = toAbs(d->valuestring);
                strncpy(dllPath_, abs.c_str(), sizeof(dllPath_) - 1);
                dllPath_[sizeof(dllPath_) - 1] = '\0';
        }
        if (const cJSON *h = cJSON_GetObjectItem(obj, "headerPath"); cJSON_IsString(h) && h->valuestring[0]) {
                std::string abs = toAbs(h->valuestring);
                strncpy(headerPath_, abs.c_str(), sizeof(headerPath_) - 1);
                headerPath_[sizeof(headerPath_) - 1] = '\0';
                doParseHeader();
        }
        // Load the library prev. Header parsing updates the shared status line;
        // doing it first used to hide an automatic LoadLibrary failure and made
        // a restored panel look parsed while no DLL was actually present
        if (dllPath_[0])
                doLoadDll();
        hasDsoGui_  = cJSON_IsTrue(cJSON_GetObjectItem(obj, "hasDsoGui"));
        showDsoGui_ = hasDsoGui_ && cJSON_IsTrue(cJSON_GetObjectItem(obj, "showDsoGui"));
        if (const cJSON *tables = cJSON_GetObjectItem(obj, "dataTables"); cJSON_IsArray(tables)) {
                std::lock_guard lock(dataTableMtx_);
                const cJSON    *entry = nullptr;
                cJSON_ArrayForEach(entry, tables)
                {
                        const cJSON *name = cJSON_GetObjectItem(entry, "name");
                        if (!cJSON_IsString(name))
                                continue;
                        for (auto &table : dataTables_) {
                                if (table.structName != name->valuestring)
                                        continue;
                                // 恢复"已拖进变量管理器"的状态：表级 + 字段级
                                if (const cJSON *pub = cJSON_GetObjectItem(entry, "published"); cJSON_IsBool(pub))
                                        table.rootPublished = cJSON_IsTrue(pub);
                                if (const cJSON *pf = cJSON_GetObjectItem(entry, "publishedFields"); cJSON_IsString(pf)) {
                                        const char *s = pf->valuestring;
                                        for (size_t i = 0; i < table.fieldPublished.size() && s[i]; ++i)
                                                table.fieldPublished[i] = s[i] == '1';
                                }
                                // Never restore a Realtime table's raw command bytes or
                                // resume its periodic write from a saved session — see
                                // the matching guard in save(). This also protects older
                                // save files written before that guard existed
                                if (table.kind != DataTableKind::Param)
                                        break;
                                if (const cJSON *data = cJSON_GetObjectItem(entry, "dataHex"); cJSON_IsString(data))
                                        sdkHexToBytes(data->valuestring, table.data);
                                break;
                        }
                }
        }
        ensureDataTableLocals(false);
        if (const cJSON *p = cJSON_GetObjectItem(obj, "pyPath"); cJSON_IsString(p) && p->valuestring[0]) {
                std::string abs = toAbs(p->valuestring);
                strncpy(pyPath_, abs.c_str(), sizeof(pyPath_) - 1);
                pyPath_[sizeof(pyPath_) - 1] = '\0';
                doLoadPy();
        }
        modified_ = false;
}
