#include "core/safety.hpp"
#include "cJSON.h"
#include "safety.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace safety
{
namespace
{
using diagnostics::Kind;
using diagnostics::Value;
uint64_t
maskFor(unsigned width)
{
        return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
}
std::string_view
trim(std::string_view text)
{
        constexpr std::string_view spaces = " \t\r\n\v\f";
        while (!text.empty() && spaces.find(text.front()) != std::string_view::npos)
                text.remove_prefix(1);
        while (!text.empty() && spaces.find(text.back()) != std::string_view::npos)
                text.remove_suffix(1);
        return text;
}
/**
 * @brief 用公共 C 核心解析整数, 保留 64 位完整精度
 */
bool
integer(std::string_view text, unsigned width, uint64_t &out, bool allowNegative = true)
{
        if (text.find('\0') != std::string_view::npos) {
                out = 0;
                return false;
        }
        const std::string input(text);
        return safety_parse_integer(input.c_str(), width, allowNegative, &out) == SAFETY_OK;
}
enum safety_value_type
nativeType(std::string_view type)
{
        if (type == "bool")
                return SAFETY_BOOLEAN;
        if (type.starts_with('i'))
                return SAFETY_SIGNED;
        if (type.starts_with('u'))
                return SAFETY_UNSIGNED;
        if (type.starts_with('f'))
                return SAFETY_FLOATING;
        return SAFETY_AUTO;
}
struct safety_value
nativeValue(const Value &val)
{
        const auto type = val.kind == Kind::Signed     ? SAFETY_SIGNED
                          : val.kind == Kind::Boolean  ? SAFETY_BOOLEAN
                          : val.kind == Kind::Floating ? SAFETY_FLOATING
                                                       : SAFETY_UNSIGNED;
        return {val.bits, val.real, val.width, type};
}
const cJSON *
get(const cJSON *node, const char *key)
{
        return cJSON_GetObjectItemCaseSensitive(node, key);
}
std::string
str(const cJSON *node, const char *key, bool required = false)
{
        const auto *item = get(node, key);
        if (!item && !required)
                return {};
        if (!cJSON_IsString(item) || (required && !*item->valuestring))
                throw std::runtime_error(std::string("Expected string: ") + key);
        return item->valuestring;
}
void
object(const cJSON *node)
{
        if (!cJSON_IsObject(node))
                throw std::runtime_error("Expected JSON object");
}
const cJSON *
array(const cJSON *node, const char *key, unsigned limit, bool required = true)
{
        auto *item = get(node, key);
        if (!item && !required)
                return nullptr;
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > int(limit))
                throw std::runtime_error(std::string("Invalid array: ") + key);
        return item->child;
}
void
uniqueKeys(const cJSON *node, unsigned depth = 0)
{
        if (depth > 24)
                throw std::runtime_error("JSON nesting exceeds 24");
        std::set<std::string> names;
        for (auto *child = node->child; child; child = child->next) {
                if (cJSON_IsObject(node) && (!child->string || !names.insert(child->string).second))
                        throw std::runtime_error("Duplicate JSON key");
                uniqueKeys(child, depth + 1);
        }
}
bool
pathValid(std::string_view path, bool relative)
{
        if (path.empty())
                return relative;
        // An array root may address its children as [0].member
        if (relative && path.front() == '[')
                return pathValid(std::string("root") + std::string(path), false);
        auto   first = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        auto   digit = [](char c) { return c >= '0' && c <= '9'; };
        size_t i     = 0;
        while (i < path.size()) {
                if (!first(path[i]))
                        return false;
                while (++i < path.size() && (first(path[i]) || digit(path[i]))) {
                }
                while (i < path.size() && path[i] == '[') {
                        const size_t start = ++i;
                        while (i < path.size() && digit(path[i]))
                                ++i;
                        if (start == i || i >= path.size() || path[i++] != ']')
                                return false;
                        uint64_t index = 0;
                        if (!integer(path.substr(start, i - start - 1), 32, index, false))
                                return false;
                }
                if (i == path.size())
                        return true;
                if (path[i++] != '.' || i == path.size())
                        return false;
        }
        return false;
}
unsigned
typeWidth(std::string_view type)
{
        if (type == "auto")
                return 0;
        if (type == "bool")
                return 1;
        for (auto prefix : {'u', 'i', 'f'}) {
                if (type.size() < 2 || type.front() != prefix)
                        continue;
                uint64_t width = 0;
                if (!integer(type.substr(1), 8, width, false))
                        break;
                if ((prefix == 'f' && (width == 32 || width == 64)) ||
                    (prefix != 'f' && (width == 8 || width == 16 || width == 32 || width == 64)))
                        return unsigned(width);
        }
        throw std::runtime_error("Unsupported safety decoder type: " + std::string(type));
}
Severity
severity(std::string_view text)
{
        if (text.empty() || text == "info")
                return Severity::Info;
        if (text == "ok")
                return Severity::Ok;
        if (text == "warning")
                return Severity::Warning;
        if (text == "error")
                return Severity::Error;
        throw std::runtime_error("Unknown severity: " + std::string(text));
}
std::string
severityName(Severity val)
{
        switch (val) {
                case Severity::Ok:
                        return "正常";
                case Severity::Warning:
                        return "警告";
                case Severity::Error:
                        return "错误";
                default:
                        return "信息";
        }
}
} // namespace

Profile
parse(std::string_view json)
{
        if (json.size() > 8 * 1024 * 1024 || json.find('\0') != std::string_view::npos)
                throw std::runtime_error("Invalid safety JSON size/content");
        for (size_t i = 0; i + 1 < json.size(); ++i) {
                if (json[i] != '\\')
                        continue;
                if (json.substr(i, 6) == "\\u0000")
                        throw std::runtime_error("JSON strings must not contain NUL");
                ++i;
        }
        if (json.starts_with("\xEF\xBB\xBF"))
                json.remove_prefix(3);
        const std::string                               content(json);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithOpts(content.c_str(), nullptr, true), cJSON_Delete);
        object(root.get());
        uniqueKeys(root.get());
        auto *version = get(root.get(), "version");
        if (str(root.get(), "format", true) != "safety" || !cJSON_IsNumber(version) || version->valuedouble != 1)
                throw std::runtime_error("Expected safety JSON version 1");
        Profile profile;
        profile.name        = str(root.get(), "name", true);
        profile.description = str(root.get(), "description");
        std::set<std::string> ids;
        for (auto *node = array(root.get(), "decoders", 1024); node; node = node->next) {
                object(node);
                Decoder decoder;
                decoder.id = str(node, "id", true);
                if (!ids.insert(decoder.id).second)
                        throw std::runtime_error("Duplicate decoder: " + decoder.id);
                for (auto *alias = array(node, "aliases", 128, false); alias; alias = alias->next) {
                        if (!cJSON_IsString(alias) || !*alias->valuestring || !ids.insert(alias->valuestring).second)
                                throw std::runtime_error("Invalid/duplicate decoder alias: " + decoder.id);
                        decoder.aliases.emplace_back(alias->valuestring);
                }
                decoder.title = str(node, "title");
                decoder.note  = str(node, "note");
                decoder.unit  = str(node, "unit");
                if (get(node, "kind"))
                        decoder.kind = str(node, "kind", true);
                if (decoder.kind != "bits" && decoder.kind != "enum" && decoder.kind != "number")
                        throw std::runtime_error("Unknown decoder kind: " + decoder.kind);
                if (get(node, "type"))
                        decoder.type = str(node, "type", true);
                const unsigned typedWidth = typeWidth(decoder.type);
                if (typedWidth)
                        decoder.width = typedWidth;
                if (const auto *width = get(node, "width")) {
                        if (!cJSON_IsNumber(width) || width->valuedouble < 1 || width->valuedouble > 64 ||
                            width->valuedouble != std::floor(width->valuedouble))
                                throw std::runtime_error("Invalid decoder width");
                        decoder.width = unsigned(width->valuedouble);
                        if (typedWidth && typedWidth != decoder.width)
                                throw std::runtime_error("Decoder type/width mismatch");
                }
                if (decoder.type.starts_with('f') && decoder.kind != "number")
                        throw std::runtime_error("Floating point decoders must use number kind");
                std::set<uint64_t> values;
                for (auto *entry = array(node, "entries", 4096, false); entry; entry = entry->next) {
                        object(entry);
                        Entry item;
                        item.name     = str(entry, "name");
                        item.title    = str(entry, "title");
                        item.cause    = str(entry, "cause");
                        item.action   = str(entry, "action");
                        item.clear    = str(entry, "clear");
                        item.severity = severity(str(entry, "severity"));
                        item.val      = str(entry, "value");
                        item.mask     = str(entry, "mask");
                        uint64_t val  = 0;
                        if (decoder.kind == "number")
                                throw std::runtime_error("Number decoder cannot have entries");
                        if (decoder.kind == "bits") {
                                if (!item.val.empty() || !integer(item.mask, decoder.width, val, false) || !val)
                                        throw std::runtime_error("Invalid bit mask in " + decoder.id);
                        } else if (!item.mask.empty() || !integer(item.val, decoder.width, val))
                                throw std::runtime_error("Invalid enum value in " + decoder.id);
                        if (decoder.kind == "enum") {
                                Value       checked;
                                std::string error;
                                if (!parseValue(item.val, decoder, checked, error))
                                        throw std::runtime_error("Invalid enum value in " + decoder.id + ": " + error);
                        }
                        if (!values.insert(val).second)
                                throw std::runtime_error("Duplicate value/mask in " + decoder.id);
                        decoder.entries.push_back(std::move(item));
                }
                profile.decoders.push_back(std::move(decoder));
        }
        ids.clear();
        for (auto *node = array(root.get(), "groups", 256); node; node = node->next) {
                object(node);
                Group group;
                group.id    = str(node, "id", true);
                group.title = str(node, "title");
                if (!ids.insert(group.id).second)
                        throw std::runtime_error("Duplicate group: " + group.id);
                group.root = str(node, "root", true);
                group.note = str(node, "note");
                if (!pathValid(group.root, false))
                        throw std::runtime_error("Invalid root path: " + group.root);
                if (const auto *expand = get(node, "expand")) {
                        if (!cJSON_IsBool(expand))
                                throw std::runtime_error("expand must be boolean");
                        group.expand = cJSON_IsTrue(expand);
                }
                if (const auto *when = get(node, "when")) {
                        object(when);
                        group.when.path = str(when, "path", true);
                        group.when.val  = str(when, "value", true);
                        group.when.mask = str(when, "mask");
                        uint64_t val = 0, mask = UINT64_MAX;
                        if (!pathValid(group.when.path, false) || !integer(group.when.val, 64, val) ||
                            (!group.when.mask.empty() && (!integer(group.when.mask, 64, mask, false) || !mask)) ||
                            (val & ~mask))
                                throw std::runtime_error("Invalid group condition: " + group.id);
                }
                std::set<std::string> paths;
                for (auto *field = array(node, "fields", 16384, false); field; field = field->next) {
                        object(field);
                        Field item;
                        if (!get(field, "path"))
                                throw std::runtime_error("Field requires path (empty for root value)");
                        item.path    = str(field, "path");
                        item.title   = str(field, "title");
                        item.note    = str(field, "note");
                        item.decoder = str(field, "decoder");
                        item.unit    = str(field, "unit");
                        if (!pathValid(item.path, true) || !paths.insert(item.path).second)
                                throw std::runtime_error("Invalid/duplicate field path in " + group.id + ": " + item.path);
                        if (!item.decoder.empty() && !findDecoder(profile, item.decoder))
                                throw std::runtime_error("Unknown decoder: " + item.decoder);
                        group.fields.push_back(std::move(item));
                }
                if (!group.expand && group.fields.empty())
                        throw std::runtime_error("Group has neither fields nor expansion: " + group.id);
                profile.groups.push_back(std::move(group));
        }
        return profile;
}

Profile
load(const std::string &path)
{
        const auto    utf8 = std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size());
        std::ifstream stream(std::filesystem::path(utf8), std::ios::binary | std::ios::ate);
        if (!stream)
                throw std::runtime_error("Cannot open safety JSON: " + path);
        const auto size = stream.tellg();
        if (size < 0 || size > 8 * 1024 * 1024)
                throw std::runtime_error("Safety JSON exceeds 8 MB");
        std::string content(size_t(size), '\0');
        stream.seekg(0);
        if (!stream.read(content.data(), std::streamsize(content.size())))
                throw std::runtime_error("Cannot read safety JSON");
        return parse(content);
}
const Decoder *
findDecoder(const Profile &profile, std::string_view id)
{
        for (const auto &decoder : profile.decoders) {
                if (decoder.id == id || std::find(decoder.aliases.begin(), decoder.aliases.end(), id) != decoder.aliases.end())
                        return &decoder;
        }
        return nullptr;
}
const Field *
findField(const Group &group, std::string_view path)
{
        for (const auto &field : group.fields)
                if (field.path == path)
                        return &field;
        return nullptr;
}
bool
parseValue(std::string_view text, const Decoder &decoder, Value &out, std::string &error)
{
        out = {};
        error.clear();
        text = trim(text);
        Value val;
        val.width = decoder.width;
        val.type  = decoder.type;
        if (!val.width || val.width > 64 || text.empty()) {
                error = "数值为空或位宽无效";
                return false;
        }
        if (decoder.type.starts_with('f')) {
                val.kind    = Kind::Floating;
                auto result = std::from_chars(text.data(), text.data() + text.size(), val.real, std::chars_format::general);
                if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(val.real)) {
                        error = "请输入有限的小数";
                        return false;
                }
                if (val.width == 32) {
                        float number = static_cast<float>(val.real);
                        if (!std::isfinite(number)) {
                                error = "数值超出 f32 范围";
                                return false;
                        }
                        val.real = number;
                        val.bits = std::bit_cast<uint32_t>(number);
                } else
                        val.bits = std::bit_cast<uint64_t>(val.real);
        } else {
                if (decoder.type == "bool") {
                        if (text == "true")
                                text = "1";
                        if (text == "false")
                                text = "0";
                }
                const bool negative   = text.front() == '-';
                const bool signedType = decoder.type.starts_with('i') || (decoder.type == "auto" && negative);
                if (!integer(text, decoder.width, val.bits, signedType)) {
                        error = "请输入位宽范围内的十进制整数或 0x 十六进制值";
                        return false;
                }
                if (signedType && !negative && !text.starts_with("0x") && !text.starts_with("0X") &&
                    val.bits > (maskFor(val.width) >> 1)) {
                        error = "数值超出有符号整数范围(原始位模式请用 0x)";
                        return false;
                }
                val.kind = decoder.type == "bool" ? Kind::Boolean : signedType ? Kind::Signed : Kind::Unsigned;
        }
        out = std::move(val);
        return true;
}
Result
describe(const Decoder *decoder, const Value &val)
{
        Result result;
        result.val = diagnostics::formatValue(val);
        result.raw = diagnostics::formatBits(val);
        if (!decoder) {
                result.summary = result.val;
                for (const auto &[number, name] : val.enums)
                        if ((val.kind == Kind::Signed && diagnostics::signValue(val) == number) ||
                            (val.kind != Kind::Floating && val.bits == uint64_t(number))) {
                                result.summary = name;
                                break;
                        }
                return result;
        }
        result.note = decoder->note;
        if (!decoder->unit.empty())
                result.val += " " + decoder->unit;
        std::vector<safety_entry> entries;
        entries.reserve(decoder->entries.size());
        for (const auto &entry : decoder->entries) {
                uint64_t expected = 0;
                if (!integer(decoder->kind == "bits" ? entry.mask : entry.val, decoder->width, expected)) {
                        result.unknown = true;
                        result.summary = "解析定义无效";
                        return result;
                }
                entries.push_back({expected});
        }
        const safety_decoder definition{decoder->kind == "bits"   ? SAFETY_BITS
                                        : decoder->kind == "enum" ? SAFETY_ENUM
                                                                  : SAFETY_NUMBER,
                                        nativeType(decoder->type),
                                        decoder->width,
                                        entries.data(),
                                        entries.size()};
        const auto          input = nativeValue(val);
        std::vector<size_t> indices(entries.size());
        safety_result       decoded{};
        const int           status = safety_decode(&definition, &input, indices.data(), indices.size(), &decoded);
        if (status != SAFETY_OK) {
                result.unknown = true;
                result.summary = status == SAFETY_MISMATCH ? "变量类型或位宽与解析定义不匹配" : "解析定义或原始数值无效";
                return result;
        }
        if (decoder->kind == "number") {
                result.summary = result.val;
                return result;
        }
        for (size_t i = 0; i < decoded.matched; ++i) {
                const auto &entry = decoder->entries[indices[i]];
                result.findings.push_back({entry.name, entry.title, entry.cause, entry.action, entry.clear, entry.severity});
        }
        if (decoder->kind == "bits") {
                const auto unknown = decoded.unknown_mask;
                result.unknown     = unknown != 0;
                if (unknown) {
                        Value raw;
                        raw.bits       = unknown;
                        raw.width      = decoder->width;
                        result.summary = "未定义位: " + diagnostics::formatBits(raw);
                } else if (diagnostics::isZero(val))
                        result.summary = "无置位项";
        } else if (decoded.unknown_value) {
                result.unknown = true;
                result.summary = "未定义枚举值: " + result.val;
        }
        for (const auto &item : result.findings) {
                if (!result.summary.empty())
                        result.summary += "; ";
                result.summary += item.title.empty() ? item.name : item.title;
        }
        return result;
}
bool
matches(const Condition &condition, const Value &val)
{
        if (condition.path.empty())
                return true;
        if (val.kind == Kind::Floating || !val.width || val.width > 64)
                return false;
        uint64_t expected = 0, mask = maskFor(val.width);
        if (!integer(condition.val, val.width, expected) ||
            (!condition.mask.empty() && (!integer(condition.mask, val.width, mask, false) || !mask)))
                return false;
        const auto input = nativeValue(val);
        return safety_matches(&input, expected, mask) == 1;
}
std::string
report(std::string_view path, const Result &result)
{
        std::string text(path);
        text += " = " + result.val + " (" + result.raw + ")\n";
        if (!result.summary.empty())
                text += result.summary + "\n";
        if (!result.note.empty())
                text += result.note + "\n";
        for (const auto &item : result.findings) {
                text += "[" + severityName(item.severity) + "] " + item.name + " " + item.title + "\n";
                if (!item.cause.empty())
                        text += "原因: " + item.cause + "\n";
                if (!item.action.empty())
                        text += "处理: " + item.action + "\n";
                if (!item.clear.empty())
                        text += "消除条件: " + item.clear + "\n";
        }
        return text;
}
} // namespace safety
