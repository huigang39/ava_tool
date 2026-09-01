#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/flow_toolbar.hpp"
#include "gui/hover_panel.hpp"
#include "gui/search_match.hpp"
#include "gui/search_results.hpp"
#include "gui/table_hierarchy.hpp"
#include "gui/table_island.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_set>

#include "ImGuiNotify.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/native_dlg.hpp"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "core/parameter_codec.hpp"
#include "core/parameter_presets.hpp"
#include "core/variable_rewrite.hpp"
#include "gui/audio_input.hpp"
#include "gui/fault_decoder.hpp"
#include "gui/firmware_status.hpp"
#include "gui/gui.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/numeric_format.hpp"
#include "gui/parameter_errors.hpp"
#include "gui/table_text.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_icons.hpp"
#include "gui/ui_theme.hpp"
#include "gui/unit_conversion_json.hpp"
#include "gui/unit_conversion_ui.hpp"
#include "gui/value_clipboard.hpp"
#include "gui/variable.hpp"

// Port headers
#include "jlink_port.h"
#include "net.h"
#include "shm.h"

static std::string shmShadowKey(const VarEntry &v);

// Decode the stored number, never its formatted enum/radix label. Conversion is
// a read-only view and does not pass through the variable write path.
static std::optional<double>
unitConversionNumber(const u8 *raw, DataType type, u32 bitOffset = 0, u32 bitSize = 0)
{
        if (!raw)
                return std::nullopt;
        double value = 0;
        if (type == DataType::F16) {
                u16 bits{};
                std::memcpy(&bits, raw, sizeof(bits));
                value = fast_f16_to_f32(bits);
        } else if (type == DataType::F32) {
                float number{};
                std::memcpy(&number, raw, sizeof(number));
                value = number;
        } else if (type == DataType::F64) {
                std::memcpy(&value, raw, sizeof(value));
        } else {
                const bool signedValue =
                    type == DataType::I8 || type == DataType::I16 || type == DataType::I32 || type == DataType::I64;
                const bool unsignedValue =
                    type == DataType::U8 || type == DataType::U16 || type == DataType::U32 || type == DataType::U64;
                if (!signedValue && !unsignedValue)
                        return std::nullopt;
                const u32 storageBits = Parser::typeBytes(type) * 8;
                const u32 width       = bitSize ? bitSize : storageBits;
                if (!width || width > storageBits || bitOffset >= storageBits || (bitSize && width > storageBits - bitOffset))
                        return std::nullopt;
                u64 bits{};
                std::memcpy(&bits, raw, storageBits / 8);
                if (bitSize)
                        bits >>= bitOffset;
                if (width < 64)
                        bits &= (u64{1} << width) - 1;
                if (signedValue) {
                        if (width < 64 && (bits & (u64{1} << (width - 1))))
                                bits |= ~((u64{1} << width) - 1);
                        i64 signedBits{};
                        std::memcpy(&signedBits, &bits, sizeof(bits));
                        value = static_cast<double>(signedBits);
                } else {
                        value = static_cast<double>(bits);
                }
        }
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

static void
saveVariableDisplaySettings(cJSON *object, const VarEntry &entry)
{
        if (!entry.memberDisplayRadices.empty()) {
                auto *radices = cJSON_AddObjectToObject(object, "memberDisplayRadices");
                for (const auto &[path, radix] : entry.memberDisplayRadices)
                        cJSON_AddNumberToObject(radices, path.c_str(), normalizeDisplayRadix(radix));
        }
        unit_conversion::SaveSettings(object, "unitConversion", entry.unitConversion);
        if (!entry.memberUnitConversions.empty()) {
                cJSON *members = cJSON_CreateObject();
                for (const auto &[path, settings] : entry.memberUnitConversions)
                        unit_conversion::SaveSettings(members, path.c_str(), settings);
                cJSON_AddItemToObject(object, "memberUnitConversions", members);
        }
}

static void
loadVariableDisplaySettings(const cJSON *object, VarEntry &entry)
{
        entry.memberDisplayRadices.clear();
        const auto *radices = cJSON_GetObjectItemCaseSensitive(object, "memberDisplayRadices");
        if (cJSON_IsObject(radices))
                for (const auto *item = radices->child; item; item = item->next)
                        if (item->string && cJSON_IsNumber(item))
                                entry.memberDisplayRadices[item->string] = normalizeDisplayRadix(item->valueint);
        entry.unitConversion = unit_conversion::LoadSettings(object, "unitConversion");
        entry.memberUnitConversions.clear();
        const cJSON *members = cJSON_GetObjectItemCaseSensitive(object, "memberUnitConversions");
        if (cJSON_IsObject(members))
                for (const cJSON *member = members->child; member; member = member->next)
                        if (member->string && cJSON_IsObject(member))
                                entry.memberUnitConversions.emplace(member->string,
                                                                    unit_conversion::LoadSettings(members, member->string));
}

std::optional<double>
Variable::unitConversionMemberValue(const VarEntry &owner, u64 address, DataType type, u32 bitOffset, u32 bitSize) const
{
        const u32 size = Parser::typeBytes(type);
        if (owner.addrUnknown || size == 0 || size > 8)
                return std::nullopt;
        u8 bytes[8]{};
        if (owner.port == PortType::JLINK) {
                const auto found = syncReadCache_.find(address);
                if (found == syncReadCache_.end() || !found->second.ok || found->second.sz < size)
                        return std::nullopt;
                std::memcpy(bytes, found->second.buf, size);
        } else {
                if (address < owner.addr)
                        return std::nullopt;
                const u64 offset = address - owner.addr;
                auto      copy   = [&](const std::vector<uint8_t> &buffer) {
                        if (offset > buffer.size() || size > buffer.size() - offset)
                                return false;
                        std::memcpy(bytes, buffer.data() + static_cast<size_t>(offset), size);
                        return true;
                };
                if (owner.port == PortType::LOCAL) {
                        std::lock_guard lock(mtxLocal_);
                        const auto      found = localBufs_.find(owner.name);
                        if (found == localBufs_.end() || !copy(found->second))
                                return std::nullopt;
                } else if (owner.port == PortType::SHM) {
                        const auto found = shmShadowBufs_.find(shmShadowKey(owner));
                        if (found == shmShadowBufs_.end() || !copy(found->second))
                                return std::nullopt;
                } else {
                        return std::nullopt;
                }
        }
        return unitConversionNumber(bytes, type, bitOffset, bitSize);
}

void
Variable::drawUnitConversionEditor()
{
        const char *popup = tr("Unit conversion###UnitConversionEditor", "单位换算###UnitConversionEditor");
        if (unitConversionEditRequested_) {
                ImGui::OpenPopup(popup);
                unitConversionEditRequested_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;
        const auto owner = std::find_if(
            vars_.begin(), vars_.end(), [&](const VarEntry &variable) { return variable.name == unitConversionEditOwner_; });
        if (owner == vars_.end()) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
        }
        ImGui::TextWrapped("%s", unitConversionEditPath_.c_str());
        ImGui::Separator();
        const bool valid = ui::UnitConversionFields(unitConversionEditDraft_);
        ImGui::BeginDisabled(!valid);
        if (ui::Button(tr("Apply", "应用"), ui::BtnStyle::Primary)) {
                auto settings    = unitConversionEditDraft_;
                settings.enabled = true;
                if (unitConversionEditPath_ == owner->name)
                        owner->unitConversion = settings;
                else
                        owner->memberUnitConversions[unitConversionEditPath_] = settings;
                Monitor::updateUnitConversion(unitConversionEditPath_, std::nullopt, settings);
                isModified_ = true;
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel", "取消")))
                ImGui::CloseCurrentPopup();
        unit_conversion::Settings current{};
        if (unitConversionEditPath_ == owner->name) {
                current = owner->unitConversion;
        } else if (const auto found = owner->memberUnitConversions.find(unitConversionEditPath_);
                   found != owner->memberUnitConversions.end()) {
                current = found->second;
        }
        if (current.enabled) {
                ImGui::SameLine();
                if (ui::Button(tr("Disable", "关闭换算"), ui::BtnStyle::Danger)) {
                        auto settings    = current;
                        settings.enabled = false;
                        if (unitConversionEditPath_ == owner->name)
                                owner->unitConversion = settings;
                        else
                                owner->memberUnitConversions[unitConversionEditPath_] = settings;
                        Monitor::updateUnitConversion(unitConversionEditPath_, std::nullopt, settings);
                        isModified_ = true;
                        ImGui::CloseCurrentPopup();
                }
        }
        ImGui::EndPopup();
}

// A stable drag/select hit target without repeating an '=' button on every row.
static void
watchAddress(u64 address)
{
        char label[32];
        snprintf(label, sizeof(label), "0x%08llX", static_cast<unsigned long long>(address));
        ui::TableText(label);
}

/**
 * @brief Helper to decode raw bytes to string based on DataType
 */
static std::string
decodeValue(const u8 *raw, DataType type, u32 bitOffset = 0, u32 bitSize = 0, int radix = 10)
{
        if (!raw)
                return "...";
        auto decodeInteger = [&](u32 storageBits, bool signedValue) {
                u64 val = 0;
                std::memcpy(&val, raw, storageBits / 8);
                const u32 width = bitSize > 0 ? std::min(bitSize, storageBits) : storageBits;
                if (bitSize > 0)
                        val = bitOffset < storageBits ? val >> bitOffset : 0;
                return formatIntegerValue(val, signedValue, width, radix);
        };
        char buf[64];
        if (radix != 10 && (type == DataType::F16 || type == DataType::F32 || type == DataType::F64))
                return decodeInteger(Parser::typeBytes(type) * 8, false);
        switch (type) {
                case DataType::U8:
                        return decodeInteger(8, false);
                case DataType::I8:
                        return decodeInteger(8, true);
                case DataType::U16:
                        return decodeInteger(16, false);
                case DataType::I16:
                        return decodeInteger(16, true);
                case DataType::F16: {
                        u16 bits;
                        std::memcpy(&bits, raw, sizeof(bits));
                        snprintf(buf, sizeof(buf), "%.6f", fast_f16_to_f32(bits));
                        break;
                }
                case DataType::U32:
                        return decodeInteger(32, false);
                case DataType::I32:
                        return decodeInteger(32, true);
                case DataType::F32:
                        snprintf(buf, sizeof(buf), "%.6f", *(f32 *)raw);
                        break;
                case DataType::U64:
                        return decodeInteger(64, false);
                case DataType::I64:
                        return decodeInteger(64, true);
                case DataType::F64:
                        snprintf(buf, sizeof(buf), "%.8f", *(f64 *)raw);
                        break;
                default:
                        snprintf(buf, sizeof(buf), "Unknown");
                        break;
        }
        return buf;
}

static std::string
rewriteValue(const u8 *data, DataType type, u32 bitOffset = 0, u32 bitSize = 0)
{
        char text[64];
        if (type == DataType::F16) {
                u16 v;
                std::memcpy(&v, data, sizeof(v));
                snprintf(text, sizeof(text), "%.9g", fast_f16_to_f32(v));
                return text;
        }
        if (type == DataType::F32) {
                float v;
                std::memcpy(&v, data, sizeof(v));
                snprintf(text, sizeof(text), "%.9g", v);
                return text;
        }
        if (type == DataType::F64) {
                double v;
                std::memcpy(&v, data, sizeof(v));
                snprintf(text, sizeof(text), "%.17g", v);
                return text;
        }
        return decodeValue(data, type, bitOffset, bitSize);
}

// A flattened scalar leaf of the rewrite editor's buffer, keyed by its
// variable-relative path (e.g. "type_cfg.actuator" or "arr[2].gain")
struct RewriteLeaf {
        size_t   offset{};
        DataType type{DataType::UNKNOWN};
        u32      bitOffset{0};
        u32      bitSize{0};
        u64      typeOff{};
};

static void
mergeManifestLeaves(std::unordered_map<std::string, RewriteLeaf> &leaves, const std::string &manifestText)
{
        if (manifestText.empty())
                return;
        parameter_json::Json manifest(cJSON_Parse(manifestText.c_str()), cJSON_Delete);
        const auto          *entries = parameter_json::get(manifest.get(), "parameters");
        if (!cJSON_IsArray(entries))
                return;
        for (auto *e = entries->child; e; e = e->next) {
                if (cJSON_IsTrue(parameter_json::get(e, "retired")))
                        continue;
                const std::string path   = parameter_json::string(e, "path", true);
                const auto       *offset = parameter_json::get(e, "offset");
                const auto       *shift  = parameter_json::get(e, "shift");
                const auto       *bits   = parameter_json::get(e, "bits");
                const auto       *type   = parameter_json::get(e, "type");
                if (!cJSON_IsNumber(offset) || !cJSON_IsNumber(shift) || !cJSON_IsNumber(bits) || !cJSON_IsString(type))
                        throw std::runtime_error("Invalid parameter manifest layout: " + path);
                for (const auto *number : {offset, shift, bits})
                        if (!std::isfinite(number->valuedouble) || number->valuedouble < 0 ||
                            number->valuedouble > UINT32_MAX || std::floor(number->valuedouble) != number->valuedouble)
                                throw std::runtime_error("Invalid manifest field range: " + path);
                if (bits->valuedouble == 0 || bits->valuedouble > 32 || shift->valuedouble + bits->valuedouble > 32)
                        throw std::runtime_error("Invalid manifest bit layout: " + path);
                if (const auto existing = leaves.find(path); existing != leaves.end()) {
                        const auto &leaf = existing->second;
                        // DWARF 将位偏移折入字节地址, 清单则保留相对存储字的位偏移
                        const auto manifestOffset = static_cast<size_t>(offset->valuedouble);
                        const auto manifestShift  = static_cast<u32>(shift->valuedouble);
                        if (leaf.offset != manifestOffset + manifestShift / 8 || leaf.bitOffset != manifestShift % 8 ||
                            (leaf.bitSize ? leaf.bitSize : 32) != static_cast<u32>(bits->valuedouble))
                                throw std::runtime_error("Manifest/DWARF field layout differs: " + path);
                        continue;
                }
                leaves[path] = RewriteLeaf{static_cast<size_t>(offset->valuedouble),
                                           Parser::strToDataType(type->valuestring),
                                           static_cast<u32>(shift->valuedouble),
                                           static_cast<u32>(bits->valuedouble),
                                           0};
        }
}

/**
 * @brief Defined later in this file; forward-declared so preset (de)serialization can
 *        reuse the same DWARF helpers as the editor tree
 */
static const dwarf::Type *resolveAlias(const dwarf::Info &info, u64 typeOff);
static const char        *scalarPayloadType(const dwarf::Info &info, u64 typeOff);
static u64                typeSize(const dwarf::Info &info, u64 typeOff);

static void
collectRewriteLeaves(const dwarf::Info                            &info,
                     u64                                           typeOff,
                     size_t                                        offset,
                     const std::string                            &path,
                     std::unordered_map<std::string, RewriteLeaf> &out,
                     int                                           depth,
                     u32                                           bitOffset = 0,
                     u32                                           bitSize   = 0,
                     bool                                          canonical = false)
{
        if (depth > 24 || out.size() >= 65536)
                return;
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;
        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                const dwarf::Type::Member *selected = nullptr;
                if (canonical && t->kind == dwarf::TypeKind::UNION && !t->members.empty()) {
                        selected = &t->members.front();
                }
                for (const auto &m : t->members) {
                        if (selected && selected != &m)
                                continue;
                        const std::string name  = m.name.empty() ? "<anon>" : m.name;
                        const std::string child = path.empty() ? name : path + "." + name;
                        collectRewriteLeaves(
                            info, m.type, offset + m.offset, child, out, depth + 1, m.bitOffset, m.bitSize, canonical);
                }
                return;
        }
        if (t->kind == dwarf::TypeKind::ARRAY) {
                const u64 elemSize = typeSize(info, t->inner);
                const u64 count    = elemSize ? std::min<u64>(typeSize(info, typeOff) / elemSize, 65536) : 0;
                for (u64 i = 0; i < count; ++i)
                        collectRewriteLeaves(info,
                                             t->inner,
                                             offset + static_cast<size_t>(i * elemSize),
                                             path + "[" + std::to_string(i) + "]",
                                             out,
                                             depth + 1,
                                             0,
                                             0,
                                             canonical);
                return;
        }
        if (const char *sType = scalarPayloadType(info, typeOff))
                out[path] = RewriteLeaf{offset,
                                        t->kind == dwarf::TypeKind::POINTER && t->size == 8 ? DataType::U64
                                                                                            : Parser::strToDataType(sType),
                                        bitOffset,
                                        bitSize,
                                        typeOff};
}

/**
 * @brief Parse a scalar text value into a raw buffer of the given type. Returns false
 *        on a malformed value or an unsupported type
 */
static bool
encodeScalar(const std::string &text, DataType type, u8 *buf)
{
        try {
                switch (type) {
                        case DataType::U8:
                                return variable_rewrite::integer<u8>(text, buf);
                        case DataType::I8:
                                return variable_rewrite::integer<i8>(text, buf);
                        case DataType::U16:
                                return variable_rewrite::integer<u16>(text, buf);
                        case DataType::I16:
                                return variable_rewrite::integer<i16>(text, buf);
                        case DataType::U32:
                                return variable_rewrite::integer<u32>(text, buf);
                        case DataType::I32:
                                return variable_rewrite::integer<i32>(text, buf);
                        case DataType::U64:
                                return variable_rewrite::integer<u64>(text, buf);
                        case DataType::I64:
                                return variable_rewrite::integer<i64>(text, buf);
                        case DataType::F16:
                        case DataType::F32:
                        case DataType::F64: {
                                size_t       end = 0;
                                const double v   = std::stod(text, &end);
                                if (text.find_first_not_of(" \t\r\n", end) != std::string::npos || !std::isfinite(v))
                                        return false;
                                if (type == DataType::F64)
                                        std::memcpy(buf, &v, sizeof(v));
                                else {
                                        const float f = static_cast<float>(v);
                                        if (!std::isfinite(f))
                                                return false;
                                        if (type == DataType::F16) {
                                                const u16 half = fast_f32_to_f16(f);
                                                if (!std::isfinite(fast_f16_to_f32(half)))
                                                        return false;
                                                std::memcpy(buf, &half, sizeof(half));
                                        } else
                                                std::memcpy(buf, &f, sizeof(f));
                                }
                                return true;
                        }
                        default:
                                return false;
                }
        } catch (...) {
                return false;
        }
}

static std::string
absoluteDisplayPath(const std::string &path)
{
        if (path.empty())
                return {};
        if (remote::isUrl(path))
                return path;
        std::error_code       ec;
        std::filesystem::path p(path);
        if (!p.is_absolute())
                p = std::filesystem::absolute(p, ec);
        if (ec)
                return path;
        return p.lexically_normal().string();
}

static const char *
portName(PortType port)
{
        switch (port) {
                case PortType::JLINK:
                        return "JLINK";
                case PortType::UDP:
                        return "UDP";
                case PortType::SHM:
                        return "SHM";
                case PortType::LOCAL:
                        return "LOCAL";
                case PortType::AUDIO:
                        return "AUDIO";
                default:
                        return "JLINK";
        }
}

static PortType
portFromJson(const cJSON *object)
{
        if (!object)
                return PortType::JLINK;
        if (const cJSON *s = cJSON_GetObjectItem(object, "portStr"); cJSON_IsString(s)) {
                const std::string name = s->valuestring;
                if (name == "LOCAL" || name == "MANUAL")
                        return PortType::LOCAL;
                if (name == "SHM")
                        return PortType::SHM;
                if (name == "AUDIO")
                        return PortType::AUDIO;
                if (name == "UDP")
                        return PortType::UDP;
                return PortType::JLINK;
        }
        const cJSON *n = cJSON_GetObjectItem(object, "port");
        if (!cJSON_IsNumber(n))
                return PortType::JLINK;
        // Historical values: MANUAL=3, LOCAL=4, AUDIO=5
        switch (n->valueint) {
                case 1:
                        return PortType::UDP;
                case 2:
                        return PortType::SHM;
                case 3:
                case 4:
                        return PortType::LOCAL;
                case 5:
                        return PortType::AUDIO;
                default:
                        return PortType::JLINK;
        }
}

static CType
dataTypeToCType(DataType type)
{
        switch (type) {
                case DataType::U8:
                        return CType::U8;
                case DataType::U16:
                        return CType::U16;
                case DataType::U32:
                        return CType::U32;
                case DataType::U64:
                        return CType::U64;
                case DataType::I8:
                        return CType::I8;
                case DataType::I16:
                        return CType::I16;
                case DataType::I32:
                        return CType::I32;
                case DataType::I64:
                        return CType::I64;
                case DataType::F32:
                        return CType::F32;
                case DataType::F64:
                        return CType::F64;
                default:
                        return CType::Unknown;
        }
}

static std::string
functionSignature(const CFuncDecl &fn)
{
        std::string out  = fn.retRaw.empty() ? ctypeLabel(fn.retType) : fn.retRaw;
        out             += " " + fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i)
                        out += ", ";
                out += fn.params[i].rawType.empty() ? ctypeLabel(fn.params[i].type) : fn.params[i].rawType;
        }
        return out + ")";
}

static std::string
methodSignature(const CMethodDecl &method)
{
        std::string out  = method.retRaw.empty() ? ctypeLabel(method.retType) : method.retRaw;
        out             += " " + method.fullClassName + "::" + method.name + "(";
        for (size_t i = 0; i < method.params.size(); ++i) {
                if (i)
                        out += ", ";
                out += method.params[i].rawType.empty() ? ctypeLabel(method.params[i].type) : method.params[i].rawType;
        }
        return out + ")";
}

static bool
pointerMatchesDataType(const CParam &param, DataType type)
{
        if (param.type != CType::Ptr)
                return false;
        std::string raw;
        raw.reserve(param.rawType.size());
        for (unsigned char c : param.rawType)
                if (!std::isspace(c) && c != '*' && c != '&')
                        raw.push_back(static_cast<char>(std::tolower(c)));
        auto eraseWord = [&](const char *word) {
                for (size_t pos = raw.find(word); pos != std::string::npos; pos = raw.find(word))
                        raw.erase(pos, std::strlen(word));
        };
        eraseWord("const");
        eraseWord("volatile");
        switch (type) {
                case DataType::U8:
                        return raw == "uint8_t" || raw == "unsignedchar";
                case DataType::I8:
                        return raw == "int8_t" || raw == "signedchar";
                case DataType::U16:
                        return raw == "uint16_t" || raw == "unsignedshort";
                case DataType::I16:
                        return raw == "int16_t" || raw == "short" || raw == "signedshort";
                case DataType::U32:
                        return raw == "uint32_t" || raw == "unsignedint";
                case DataType::I32:
                        return raw == "int32_t" || raw == "int" || raw == "signedint";
                case DataType::U64:
                        return raw == "uint64_t" || raw == "unsignedlonglong";
                case DataType::I64:
                        return raw == "int64_t" || raw == "longlong" || raw == "signedlonglong";
                case DataType::F32:
                        return raw == "float";
                case DataType::F64:
                        return raw == "double";
                default:
                        return false;
        }
}

static bool
isReadAccessor(const CFuncDecl &fn, DataType type)
{
        if (fn.isVariadic || dataTypeToCType(type) == CType::Unknown)
                return false;
        if (fn.params.empty())
                return fn.retType == dataTypeToCType(type);
        return fn.params.size() == 1 && pointerMatchesDataType(fn.params[0], type) &&
               (fn.retType == CType::Void || ctypeIsInteger(fn.retType));
}

static bool
isWriteAccessor(const CFuncDecl &fn, DataType type)
{
        if (fn.isVariadic || fn.params.size() != 1 || dataTypeToCType(type) == CType::Unknown)
                return false;
        const bool parameterMatches = pointerMatchesDataType(fn.params[0], type) || fn.params[0].type == dataTypeToCType(type);
        return parameterMatches && (fn.retType == CType::Void || ctypeIsInteger(fn.retType));
}

static bool
callStatusOk(const CallResult &result, CType returnType, std::string &error)
{
        if (!result.ok) {
                error = result.error;
                return false;
        }
        if ((returnType == CType::F32 || returnType == CType::F64) && !std::isfinite(result.rawF64)) {
                error = "function returned a non-finite value";
                return false;
        }
        if ((returnType == CType::I8 || returnType == CType::I16 || returnType == CType::I32 || returnType == CType::I64) &&
            static_cast<int64_t>(result.rawU64) < 0) {
                error = "function returned failure status " + std::to_string(static_cast<int64_t>(result.rawU64));
                return false;
        }
        return true;
}

static void
storeCallValue(const CallResult &result, DataType type, std::array<uint8_t, 8> &dst)
{
        switch (type) {
                case DataType::F32: {
                        const float val = static_cast<float>(result.rawF64);
                        std::memcpy(dst.data(), &val, sizeof(val));
                        break;
                }
                case DataType::F64:
                        std::memcpy(dst.data(), &result.rawF64, sizeof(result.rawF64));
                        break;
                default: {
                        const u32 size = Parser::typeBytes(type);
                        std::memcpy(dst.data(), &result.rawU64, std::min<u32>(size, sizeof(result.rawU64)));
                        break;
                }
        }
}

static void
drawAudioDeviceCombo(int &deviceIndex, char *deviceName, size_t deviceNameSize)
{
        auto &audio = AudioInput::instance();
        if (audio.devices().empty())
                audio.refreshDevices();
        if (deviceIndex < 0)
                deviceIndex = audio.defaultDeviceIndex();
        std::string cur = audio.deviceName(deviceIndex);
        if (deviceName && deviceNameSize > 0)
                snprintf(deviceName, deviceNameSize, "%s", cur.c_str());

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##audioDevice", cur.c_str())) {
                for (const auto &dev : audio.devices()) {
                        const bool selected = dev.index == deviceIndex;
                        if (ImGui::Selectable(dev.name.c_str(), selected)) {
                                deviceIndex = dev.index;
                                if (deviceName && deviceNameSize > 0)
                                        snprintf(deviceName, deviceNameSize, "%s", dev.name.c_str());
                        }
                        if (selected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Audio input device", "音频输入设备"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Refresh##auddev", "刷新##auddev"))) {
                audio.refreshDevices();
                if (deviceIndex < 0)
                        deviceIndex = audio.defaultDeviceIndex();
                std::string refreshed = audio.deviceName(deviceIndex);
                if (deviceName && deviceNameSize > 0)
                        snprintf(deviceName, deviceNameSize, "%s", refreshed.c_str());
        }
}

void
Variable::completeElfReload()
{
        JLinkPort::instance().setFirmwareSource(reinterpret_cast<uintptr_t>(this), firmwareBuildImage_);
}

void
Variable::unregisterFirmwareSource()
{
        JLinkPort::instance().removeFirmwareSource(reinterpret_cast<uintptr_t>(this));
}

static std::vector<std::string>
findMatchingFirmware(const std::string &selected, const std::string &id)
{
        std::vector<std::string> matches;
        if (selected.empty() || id.empty() || remote::isUrl(selected))
                return matches;
        std::set<std::filesystem::path> archives;
        auto                            folder = std::filesystem::u8path(selected).parent_path();
        for (int level = 0; level < 3 && !folder.empty(); ++level, folder = folder.parent_path()) {
                std::error_code ec;
                if (std::filesystem::is_directory(folder / "archive", ec))
                        archives.insert(folder / "archive");
                for (std::filesystem::directory_iterator
                         it(folder, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     !ec && it != end;
                     it.increment(ec)) {
                        if (it->is_directory(ec) && std::filesystem::is_directory(it->path() / "archive", ec))
                                archives.insert(it->path() / "archive");
                }
        }
        size_t visited = 0;
        for (const auto &archive : archives) {
                std::error_code ec;
                for (std::filesystem::recursive_directory_iterator
                         it(archive, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                     !ec && it != end && visited++ < 4000;
                     it.increment(ec)) {
                        if (it->path().filename() != "build_manifest.json")
                                continue;
                        std::ifstream                                   input(it->path(), std::ios::binary);
                        std::string                                     text((std::istreambuf_iterator<char>(input)), {});
                        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> manifest(cJSON_Parse(text.c_str()), cJSON_Delete);
                        const auto *build = cJSON_GetObjectItemCaseSensitive(manifest.get(), "firmware_build");
                        const auto *val   = cJSON_GetObjectItemCaseSensitive(build, "build_id");
                        if (!cJSON_IsString(val) || id != val->valuestring)
                                continue;
                        for (std::filesystem::directory_iterator file(it->path().parent_path(), ec), last; !ec && file != last;
                             file.increment(ec)) {
                                if (file->path().extension() == ".axf" || file->path().extension() == ".elf")
                                        matches.push_back(file->path().generic_string());
                        }
                        if (matches.size() >= 8)
                                return matches;
                }
        }
        return matches;
}

void
Variable::drawFirmwareStatus()
{
        if (elfPath_.empty() || !JLinkPort::instance().isConnected())
                return;
        const auto status = JLinkPort::instance().firmwareStatus();
        if (status.state == firmware_match::State::Matched && firmwareBuildImage_)
                return;
        if (status.state == firmware_match::State::Unverified || status.state == firmware_match::State::Matched) {
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f),
                                   "%s",
                                   tr("AXF unverified: legacy debugging remains available", "AXF 未校验: 旧固件仍可正常调试"));
                if (!status.detail.empty())
                        ImGui::TextWrapped("%s", firmwareStatusDetail(status));
                return;
        }
        ImGui::TextColored(status.forced ? ImVec4(0.95f, 0.72f, 0.25f, 1.0f) : ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                           "%s",
                           firmwareStatusLabel(status));
        if (!status.detail.empty())
                ImGui::TextWrapped("%s", firmwareStatusDetail(status));
        if (status.state != firmware_match::State::Mismatch)
                return;
        ImGui::Text("Device: %.16s...   AXF: %.16s...", status.deviceId.c_str(), status.expectedId.c_str());
        ImGui::BeginDisabled(JLinkPort::instance().isBusy());
        if (ImGui::SmallButton(status.forced ? tr("Cancel forced use", "取消强制使用")
                                             : tr("Force use AXF/ELF", "强制使用 AXF/ELF")))
                JLinkPort::instance().setFirmwareMismatchOverride(!status.forced);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    "%s",
                    tr("Read, write and sample using the loaded file addresses, which may differ from the device. "
                       "Applies only to the current connection and files.",
                       "按已加载文件中的地址读写和采样, 地址可能与设备不一致. 仅对当前连接和文件有效."));
        if (firmwareSearchId_ != status.deviceId) {
                firmwareSearchId_ = status.deviceId;
                firmwareMatches_  = findMatchingFirmware(elfPath_, status.deviceId);
        }
        for (const auto &path : firmwareMatches_) {
                const auto label = std::string(tr("Load matching AXF: ", "加载匹配的 AXF: ")) + path;
                if (ImGui::SmallButton(label.c_str())) {
                        loadElf(path);
                        break;
                }
        }
}

bool
Variable::loadCfg(const std::string &cfgPath)
{
        JsonParser jp;
        if (!jp.parse(cfgPath))
                return false;
        dataTree_ = jp.getDataTree();
        cfgPath_  = cfgPath;
        elfPath_.clear();
        binPath_.clear();
        firmwareBuildImage_.reset();
        unregisterFirmwareSource();
        rebuildSearchPool();
        return true;
}

bool
Variable::loadBin(const std::string &binPath)
{
        BinParser bp;
        bp.setTemplate(dataTree_);
        if (!bp.parse(binPath))
                return false;
        dataTree_ = bp.getDataTree();
        binPath_  = binPath;
        elfPath_.clear();
        firmwareBuildImage_.reset();
        unregisterFirmwareSource();
        rebuildSearchPool();
        return true;
}

bool
Variable::loadElf(const std::string &elfPath)
{
        if (isElfLoading_)
                return false;

        if (elfLoadThread_.joinable())
                elfLoadThread_.join();
        {
                std::lock_guard lk(mtxElf_);
                pendingElf_.reset();
                elfLoadError_.clear();
        }
        elfPath_      = elfPath;
        isElfLoading_ = true;
        JLinkPort::instance().setFirmwareSource(reinterpret_cast<uintptr_t>(this), firmwareBuildImage_, true);

        auto task           = std::make_shared<ElfLoadingTask>();
        currentLoadingTask_ = task;

        const auto remoteConfig = remote::settings();
        elfLoadThread_          = std::thread([this, elfPath, task, remoteConfig]() {
                auto      result = std::make_unique<ElfLoadResult>();
                const u64 start  = get_mono_ts_ms();
                try {
                        LOG_I("Async Load ELF start: %s", elfPath.c_str());
                        result->localPath         = remote::isUrl(elfPath) ? remote::download(elfPath, remoteConfig) : elfPath;
                        const auto checkCancelled = [&] {
                                if (task->aborted)
                                        throw std::runtime_error("ELF loading cancelled");
                        };
                        checkCancelled();
                        // Record the version being parsed, not a later write that
                        // happens while parsing (which must trigger another reload)
                        std::error_code ec;
                        result->writeTime = std::filesystem::last_write_time(result->localPath, ec);
                        if (ec)
                                result->writeTime = {};
                        firmware_match::FileSnapshot snapshot(std::filesystem::u8path(result->localPath));
                        ElfParser                    ep;
                        if (!ep.parse(snapshot.path.string()))
                                throw std::runtime_error("Failed to parse ELF/AXF file.");
                        checkCancelled();
                        result->info = ep.getElfInfo();
                        const auto buildSymbol =
                            std::find_if(result->info.symbols.begin(), result->info.symbols.end(), [](const auto &symbol) {
                                    return symbol.name == "g_firmware_build_info";
                            });
                        if (buildSymbol != result->info.symbols.end()) {
                                if (buildSymbol->size != firmware_match::infoSize || buildSymbol->addr > UINT32_MAX)
                                        throw std::runtime_error("Invalid AXF build information symbol");
                                result->buildImage =
                                    firmware_match::Image::load(snapshot.path, static_cast<uint32_t>(buildSymbol->addr));
                                result->buildImage->path = result->localPath;
                        }
                        dwarf::parse(result->info, result->dwarf);
                        checkCancelled();
                        if (result->dwarf.present)
                                for (const auto &v : result->dwarf.variables)
                                        flattenDwarfType(result->pool, result->dwarf, v.name, v.addr, v.type, 0);
                } catch (const std::exception &e) {
                        result->error = e.what();
                }
                {
                        std::lock_guard lk(mtxElf_);
                        if (!task->aborted)
                                pendingElf_ = std::move(result);
                }
                isElfLoading_ = false;
                LOG_I("Async Load ELF finished: %llu ms", get_mono_ts_ms() - start);
        });

        return true;
}

void
Variable::applyPendingElf()
{
        // Never replace DWARF maps from the worker: updateVariables(), the
        // symbol browser and other GUI tools may be holding references to them
        if (isElfLoading_.load(std::memory_order_acquire))
                return;
        std::unique_ptr<ElfLoadResult> result;
        {
                std::lock_guard lk(mtxElf_);
                result = std::move(pendingElf_);
        }
        if (!result || (currentLoadingTask_ && currentLoadingTask_->aborted))
                return;
        if (!result->error.empty()) {
                std::lock_guard lk(mtxElf_);
                elfLoadError_ = std::move(result->error);
                JLinkPort::instance().setFirmwareSource(reinterpret_cast<uintptr_t>(this), firmwareBuildImage_);
                return; // Keep the last complete symbol table on a failed reload
        }
        std::lock_guard lk(mtxElf_);
        elfLocalPath_ = std::move(result->localPath);
        elfInfo_      = std::move(result->info);
        dwarfInfo_    = std::move(result->dwarf);
        searchPool_   = std::move(result->pool);
        ++searchPoolGeneration_;
        elfLastWriteTime_ = result->writeTime;
        ++elfLayoutGeneration_;
        firmwareBuildImage_ = std::move(result->buildImage);
        firmwareSearchId_.clear();
        firmwareMatches_.clear();
        JLinkPort::instance().setFirmwareSource(reinterpret_cast<uintptr_t>(this), firmwareBuildImage_, true);
        focusWatch(-1, {});
        searchResults_.clear();
        memberPollReqs_.clear();
        diagnosticPollReqs_.clear();
        syncReadCache_.clear();
        memberNumericValueCache_.clear();
        pendingCopyOwner_.clear();
        pendingCopyRequests_.clear();
        cfgPath_.clear();
        binPath_.clear();
        i32 syncCount = 0;
        for (auto &v : vars_) {
                if (v.port != PortType::JLINK)
                        continue;
                for (const auto &se : searchPool_)
                        if (se.path == v.name) {
                                v.addr    = se.addr;
                                v.typeOff = se.typeOff;
                                ++syncCount;
                                break;
                        }
        }
        if (syncCount > 0)
                LOG_I("Synced %d addresses in %s", syncCount, name_.c_str());
        elfReloaded_ = true; // Notify other tools only after committing the result
}

void
Variable::handleDroppedFile(const std::string &path)
{
        const auto  dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return std::tolower(c); });

        if (ext == ".elf" || ext == ".axf" || ext == ".out") {
                if (loadElf(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded ELF: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load ELF"});
        } else if (ext == ".json") {
                if (loadParameterFile(path))
                        return;
                if (loadCfg(path))
                        ImGui::InsertNotification(
                            {ImGuiToastType::Success, toastDismissTime_, "Loaded JSON: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load JSON"});
        } else if (ext == ".bin") {
                if (loadBin(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded BIN: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load BIN"});
        } else if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx") {
                parseFunctionHeader(path);
        } else if (ext == ".dll" || ext == ".so" || ext == ".dylib" || ext.find(".so.") == 0) {
                loadFunctionLibrary(path);
        }
}

bool
Variable::parseFunctionHeader(const std::string &path)
{
        FunctionSource *target = sourceForHeader(path);
        target->headerPath     = path;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
                target->status        = tr("Cannot open function header.", "无法打开函数头文件。");
                target->statusError   = true;
                target->statusWarning = false;
                return false;
        }
        const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        target->declarations = parseHeaderFull(source);
        functionPanelOpen_   = true;
        size_t methodCount   = 0;
        for (const auto &cls : target->declarations.classes)
                methodCount += cls.methods.size();
        if (!target->declarations.functions.empty()) {
                target->status = std::to_string(target->declarations.functions.size()) +
                                 tr(" top-level C functions parsed.", " 个顶层 C 函数已解析。");
                target->statusError   = false;
                target->statusWarning = false;
        } else if (!target->declarations.classes.empty()) {
                target->status = tr("No top-level C functions. Found ", "未找到顶层 C 函数；发现 ") +
                                 std::to_string(target->declarations.classes.size()) + tr(" C++ class(es), ", " 个 C++ 类、") +
                                 std::to_string(methodCount) +
                                 tr(" method(s). Variable binding requires an extern \"C\" wrapper.",
                                    " 个成员方法。变量绑定需要 extern \"C\" 包装函数。");
                target->statusError   = false;
                target->statusWarning = true;
        } else {
                target->status        = tr("No callable declarations were found in the header.", "头文件中未找到可调用声明。");
                target->statusError   = true;
                target->statusWarning = false;
        }
        isModified_ = true;
        return !target->declarations.functions.empty() || !target->declarations.classes.empty();
}

bool
Variable::loadFunctionLibrary(const std::string &path)
{
        FunctionSource *target = sourceForLibrary(path);
        target->libraryPath    = path;
        std::lock_guard<std::mutex> callLock(sdkCallMtx_);
        if (!target->loader.load(path)) {
                target->status        = target->loader.prevError();
                target->statusError   = true;
                target->statusWarning = false;
                return false;
        }
        functionPanelOpen_    = true;
        target->status        = tr("Dynamic library loaded.", "动态库已加载。");
        target->statusError   = false;
        target->statusWarning = false;
        isModified_           = true;
        return true;
}

std::string
Variable::functionSourceKey(const FunctionSource &source)
{
        return !source.libraryPath.empty() ? source.libraryPath : source.headerPath;
}

static std::string
lowerPathStem(const std::string &path)
{
        std::string stem = std::filesystem::path(path).stem().string();
        std::ranges::transform(stem, stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return stem;
}

Variable::FunctionSource *
Variable::sourceForLibrary(const std::string &path)
{
        for (auto &source : functionSources_)
                if (source->libraryPath == path)
                        return source.get();
        const std::string stem = lowerPathStem(path);
        for (auto &source : functionSources_)
                if (source->libraryPath.empty() && !source->headerPath.empty() && lowerPathStem(source->headerPath) == stem)
                        return source.get();
        for (auto &source : functionSources_)
                if (source->libraryPath.empty())
                        return source.get();
        functionSources_.push_back(std::make_unique<FunctionSource>());
        return functionSources_.back().get();
}

Variable::FunctionSource *
Variable::sourceForHeader(const std::string &path)
{
        for (auto &source : functionSources_)
                if (source->headerPath == path)
                        return source.get();
        const std::string stem = lowerPathStem(path);
        for (auto &source : functionSources_)
                if (source->headerPath.empty() && !source->libraryPath.empty() && lowerPathStem(source->libraryPath) == stem)
                        return source.get();
        for (auto &source : functionSources_)
                if (source->headerPath.empty())
                        return source.get();
        functionSources_.push_back(std::make_unique<FunctionSource>());
        return functionSources_.back().get();
}

const CFuncDecl *
Variable::findFunction(const VarEntry::FunctionBinding &binding, FunctionSource **resolvedSource) const
{
        if (binding.empty())
                return nullptr;
        for (const auto &source : functionSources_) {
                if (!binding.sourcePath.empty() && binding.sourcePath != functionSourceKey(*source))
                        continue;
                for (const auto &fn : source->declarations.functions)
                        if (fn.name == binding.functionName &&
                            (binding.signature.empty() || binding.signature == functionSignature(fn))) {
                                if (resolvedSource)
                                        *resolvedSource = source.get();
                                return &fn;
                        }
        }
        return nullptr;
}

void
Variable::startAccessorThread()
{
        if (accessor_)
                return;
        accessor_       = std::make_shared<AccessState>();
        auto state      = accessor_;
        accessorThread_ = std::thread([this, state]() {
                LOG_I("Variable[%s] function accessor thread started", name_.c_str());
                std::vector<AccessReadReq> reads;
                while (true) {
                        std::deque<AccessWriteReq> writes;
                        {
                                std::unique_lock<std::mutex> lk(state->mtx);
                                const auto                   ready = [&]() {
                                        return !state->running || state->readsPending || !state->writes.empty();
                                };
                                if (reads.empty())
                                        state->cv.wait(lk, ready);
                                else
                                        state->cv.wait_for(lk, std::chrono::microseconds(state->intervalUs.load()), ready);
                                if (!state->running)
                                        break;
                                if (state->readsPending) {
                                        reads               = state->reads;
                                        state->readsPending = false;
                                }
                                writes.swap(state->writes);
                        }

                        for (auto &request : writes) {
                                AccessResult             result;
                                std::vector<std::string> args(1, request.valueText);
                                std::vector<void *>      pointers;
                                if (request.function.params[0].type == CType::Ptr)
                                        pointers = {request.val.data()};
                                CallResult called;
                                if (!request.source) {
                                        called.error = "function source is unavailable";
                                } else {
                                        std::lock_guard<std::mutex> callLock(sdkCallMtx_);
                                        called = request.source->loader.call(request.function, args, pointers);
                                }
                                result.ok = callStatusOk(called, request.function.retType, result.error);
                                std::lock_guard<std::mutex> lk(state->mtx);
                                state->writeResults[request.name] = std::move(result);
                        }

                        for (const auto &request : reads) {
                                AccessResult result;
                                result.size = Parser::typeBytes(request.type);
                                CallResult called;
                                if (request.function.params.empty()) {
                                        if (request.source) {
                                                std::lock_guard<std::mutex> callLock(sdkCallMtx_);
                                                called = request.source->loader.call(request.function, {});
                                        } else {
                                                called.error = "function source is unavailable";
                                        }
                                        result.ok = callStatusOk(called, request.function.retType, result.error);
                                        if (result.ok) {
                                                storeCallValue(called, request.type, result.val);
                                                result.hasValue = true;
                                        }
                                } else {
                                        std::vector<std::string> args(1);
                                        std::vector<void *>      pointers{result.val.data()};
                                        if (request.source) {
                                                std::lock_guard<std::mutex> callLock(sdkCallMtx_);
                                                called = request.source->loader.call(request.function, args, pointers);
                                        } else {
                                                called.error = "function source is unavailable";
                                        }
                                        result.ok       = callStatusOk(called, request.function.retType, result.error);
                                        result.hasValue = result.ok;
                                }
                                if (result.ok && result.hasValue && result.size <= result.val.size())
                                        publishLocalSample(request.name,
                                                           {result.val.begin(), result.val.begin() + result.size},
                                                           sessionTimeSec());
                                std::lock_guard<std::mutex> lk(state->mtx);
                                state->results[request.name] = std::move(result);
                        }
                }
                LOG_I("Variable[%s] function accessor thread stopped", name_.c_str());
        });
}

void
Variable::stopAccessorThread()
{
        if (accessor_) {
                {
                        std::lock_guard<std::mutex> lk(accessor_->mtx);
                        accessor_->running = false;
                }
                accessor_->cv.notify_all();
        }
        if (accessorThread_.joinable())
                accessorThread_.join();
        accessor_.reset();
}

static const dwarf::Type *
resolveAlias(const dwarf::Info &info, u64 typeOff)
{
        for (int guard = 0; guard < 32; ++guard) {
                const auto it = info.types.find(typeOff);
                if (it == info.types.end())
                        return nullptr;
                const dwarf::Type &t = it->second;
                if (t.kind == dwarf::TypeKind::TYPEDEF || t.kind == dwarf::TypeKind::MODIFIER) {
                        if (t.inner == 0)
                                return &t;
                        typeOff = t.inner;
                        continue;
                }
                return &t;
        }
        return nullptr;
}

static std::string
prettyType(const dwarf::Info &info, u64 typeOff, int depth = 0)
{
        if (depth > 16 || typeOff == 0)
                return "void";
        const auto it = info.types.find(typeOff);
        if (it == info.types.end())
                return "?";
        const dwarf::Type &t = it->second;

        switch (t.kind) {
                case dwarf::TypeKind::BASE:
                case dwarf::TypeKind::TYPEDEF:
                        return t.name.empty() ? "?" : t.name;
                case dwarf::TypeKind::POINTER:
                        return prettyType(info, t.inner, depth + 1) + " *";
                case dwarf::TypeKind::MODIFIER:
                        return prettyType(info, t.inner, depth + 1);
                case dwarf::TypeKind::ARRAY: {
                        std::string s = prettyType(info, t.inner, depth + 1);
                        for (const u64 d : t.dims) {
                                s += "[";
                                if (d > 0)
                                        s += std::to_string(d);
                                s += "]";
                        }
                        return s;
                }
                case dwarf::TypeKind::STRUCT:
                        return "struct " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::UNION:
                        return "union " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::ENUM:
                        return "enum " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::SUBROUTINE:
                        return "func";
                default:
                        return "?";
        }
}

static const char *
scalarPayloadType(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return nullptr;
        if (t->kind == dwarf::TypeKind::BASE) {
                const u64 sz = t->size;
                switch (t->encoding) {
                        case 0x04: // DW_ATE_float
                                if (sz == 2)
                                        return "f16";
                                return (sz == 8) ? "f64" : "f32";
                        case 0x05: // DW_ATE_signed
                        case 0x06: // DW_ATE_signed_char
                                if (sz == 1)
                                        return "i8";
                                if (sz == 2)
                                        return "i16";
                                if (sz == 8)
                                        return "i64";
                                return "i32";
                        case 0x02: // DW_ATE_boolean
                        case 0x07: // DW_ATE_unsigned
                        case 0x08: // DW_ATE_unsigned_char
                        default:
                                if (sz == 1)
                                        return "u8";
                                if (sz == 2)
                                        return "u16";
                                if (sz == 8)
                                        return "u64";
                                return "u32";
                }
        }
        if (t->kind == dwarf::TypeKind::ENUM) {
                if (t->inner != 0)
                        if (const char *underlying = scalarPayloadType(info, t->inner))
                                return underlying;
                const u64 sz = t->size ? t->size : 4;
                if (sz == 1)
                        return "i8";
                if (sz == 2)
                        return "i16";
                if (sz == 8)
                        return "i64";
                return "i32";
        }
        if (t->kind == dwarf::TypeKind::POINTER)
                return t->size == 8 ? "u64" : (t->size == 2 ? "u16" : "u32");
        return nullptr;
}

static void
fillEnumPayload(const dwarf::Info                    &info,
                u64                                   typeOff,
                ChannelDropPayload                   &p,
                const std::vector<VarEntry::EnumDef> *overrideDefs = nullptr)
{
        // User-defined overrides win over DWARF — they reflect the latest edits
        // in the Variable window
        if (overrideDefs && !overrideDefs->empty()) {
                p.numEnums = (u8)std::min((int)overrideDefs->size(), ChannelDropPayload::kMaxEnums);
                for (int i = 0; i < p.numEnums; ++i) {
                        snprintf(p.enums[i].name, sizeof(p.enums[i].name), "%s", (*overrideDefs)[i].name.c_str());
                        p.enums[i].val = (*overrideDefs)[i].val;
                }
                return;
        }
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t || t->kind != dwarf::TypeKind::ENUM) {
                p.numEnums = 0;
                return;
        }
        p.numEnums = (u8)std::min((int)t->enums.size(), ChannelDropPayload::kMaxEnums);
        for (int i = 0; i < p.numEnums; ++i) {
                snprintf(p.enums[i].name, sizeof(p.enums[i].name), "%s", t->enums[i].name.c_str());
                p.enums[i].val = t->enums[i].val;
        }
}

static u64
typeSize(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return 0;
        if (t->kind == dwarf::TypeKind::ARRAY) {
                if (t->size > 0)
                        return t->size;
                u64 elem  = typeSize(info, t->inner);
                u64 total = elem ? elem : 1;
                for (const u64 d : t->dims)
                        total *= (d ? d : 1);
                return total;
        }
        return t->size;
}

void
Variable::rebuildSearchPool()
{
        std::vector<SearchEntry> newPool;
        if (dwarfInfo_.present) {
                for (const auto &v : dwarfInfo_.variables) {
                        flattenDwarfType(newPool, dwarfInfo_, v.name, v.addr, v.type, 0);
                }
        }
        if (!dataTree_.children.empty()) {
                for (const auto &n : dataTree_.children) {
                        flattenDataTree(newPool, n.name, n);
                }
        }
        searchPool_ = std::move(newPool);
        ++searchPoolGeneration_;
}

bool
Variable::refreshChannelEnums(MonitorChannel *ch)
{
        if (!ch)
                return false;
        const std::string &chPath = ch->getSymbolName();
        if (chPath.empty())
                return false;

        // 1. Locate the leaf typeOff via searchPool_ — established truth for
        // every flattened path under this Variable
        u64  leafTypeOff = 0;
        bool foundInPool = false;
        for (const auto &se : searchPool_) {
                if (se.path == chPath) {
                        leafTypeOff = se.typeOff;
                        foundInPool = true;
                        break;
                }
        }
        if (!foundInPool)
                return false; // Not this Variable's symbol — let caller try the next

        // 2. Find user override (exact match first, then parent member match)
        const std::vector<VarEntry::EnumDef> *override_ = nullptr;
        for (const auto &v : vars_) {
                if (v.name == chPath) {
                        if (!v.enumDefs.empty())
                                override_ = &v.enumDefs;
                        break;
                }
                if (chPath.size() > v.name.size() + 1 && chPath.compare(0, v.name.size(), v.name) == 0 &&
                    (chPath[v.name.size()] == '.' || chPath[v.name.size()] == '[')) {
                        auto it = v.memberEnumDefs.find(chPath);
                        if (it != v.memberEnumDefs.end() && !it->second.empty()) {
                                override_ = &it->second;
                                break;
                        }
                        // No override for this leaf — fall through to DWARF below
                }
        }

        // 3. Build new enum entries: override wins; otherwise DWARF at the leaf
        // type. If DWARF says not-an-enum and there's no override, clear —
        // the type may have changed in the reloaded ELF
        std::vector<MonitorChannel::EnumEntry> ents;
        if (override_) {
                ents.reserve(override_->size());
                for (const auto &e : *override_)
                        ents.push_back({e.name, e.val});
        } else {
                std::lock_guard    lk(mtxElf_);
                const dwarf::Type *t = resolveAlias(dwarfInfo_, leafTypeOff);
                if (t && t->kind == dwarf::TypeKind::ENUM) {
                        ents.reserve(t->enums.size());
                        for (const auto &e : t->enums)
                                ents.push_back({e.name, e.val});
                }
        }
        ch->setEnums(std::move(ents));
        return true;
}

bool
Variable::setLocalMemberEnums(const std::string                                                     &varName,
                              const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> &memberEnums)
{
        auto sameDefs = [](const std::vector<VarEntry::EnumDef> &a, const std::vector<VarEntry::EnumDef> &b) {
                if (a.size() != b.size())
                        return false;
                for (size_t i = 0; i < a.size(); ++i)
                        if (a[i].val != b[i].val || a[i].name != b[i].name)
                                return false;
                return true;
        };

        for (auto &v : vars_) {
                if (v.name != varName || v.port != PortType::LOCAL)
                        continue;
                bool changed = false;
                for (const auto &[path, defs] : memberEnums) {
                        auto it = v.memberEnumDefs.find(path);
                        if (it != v.memberEnumDefs.end() && sameDefs(it->second, defs))
                                continue;
                        v.memberEnumDefs[path] = defs;
                        changed                = true;
                }
                if (changed) {
                        isModified_        = true;
                        propertiesChanged_ = true;
                }
                return changed;
        }
        return false;
}

bool
Variable::refreshLocalChannelEnums(const std::string &varName, MonitorChannel *ch)
{
        if (!ch)
                return false;
        const std::string &chPath = ch->getSymbolName();
        // Must be a member path under varName ("<var>.<member>" / "<var>[i]")
        if (chPath.size() <= varName.size() + 1 || chPath.compare(0, varName.size(), varName) != 0 ||
            (chPath[varName.size()] != '.' && chPath[varName.size()] != '['))
                return false;

        for (const auto &v : vars_) {
                if (v.name != varName || v.port != PortType::LOCAL)
                        continue;
                auto it = v.memberEnumDefs.find(chPath);
                if (it == v.memberEnumDefs.end())
                        return false;
                std::vector<MonitorChannel::EnumEntry> ents;
                ents.reserve(it->second.size());
                for (const auto &e : it->second)
                        ents.push_back({e.name, e.val});
                ch->setEnums(std::move(ents));
                return true;
        }
        return false;
}

void
Variable::flattenDwarfType(std::vector<SearchEntry> &pool,
                           const dwarf::Info        &info,
                           const std::string        &parentPath,
                           u64                       parentAddr,
                           u64                       typeOff,
                           int                       depth,
                           u32                       bitOffset,
                           u32                       bitSize)
{
        if (depth > 8)
                return;
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        pool.push_back(e);
                }
                for (const auto &m : t->members) {
                        std::string path = parentPath + "." + (m.name.empty() ? "<anon>" : m.name);
                        flattenDwarfType(pool, info, path, parentAddr + m.offset, m.type, depth + 1, m.bitOffset, m.bitSize);
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        pool.push_back(e);
                }
                u64 elemSize  = typeSize(info, t->inner);
                u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                u64 displayed = (dim == 0) ? 0 : (dim < 8 ? dim : 8);
                for (u64 i = 0; i < displayed; ++i) {
                        std::string path = parentPath + "[" + std::to_string(i) + "]";
                        flattenDwarfType(pool, info, path, parentAddr + i * elemSize, t->inner, depth + 1, 0, 0);
                }
        } else {
                const char *sType = scalarPayloadType(info, typeOff);
                if (sType) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = Parser::strToDataType(sType);
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        e.bitOffset   = bitOffset;
                        e.bitSize     = bitSize;
                        pool.push_back(e);
                } else if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        pool.push_back(e);
                }
        }
}

void
Variable::flattenDataTree(std::vector<SearchEntry> &pool, const std::string &parentPath, const DataTree &node)
{
        if (node.type == DataType::ARRAY) {
                for (size_t i = 0; i < node.children.size() && i < 32; ++i) {
                        flattenDataTree(pool, parentPath + "[" + std::to_string(i) + "]", node.children[i]);
                }
        } else {
                SearchEntry e;
                e.path        = parentPath;
                e.addr        = 0;
                e.type        = node.type;
                e.defaultPort = PortType::LOCAL;
                e.typeOff     = 0;
                pool.push_back(e);
        }
}

/**
 * @brief Defined later in this file; forward-declared so drawSymbolLeaf can flatten a
 *        dragged struct/array into per-member chs (same as the watch list does)
 */
static void flattenForStructPayload(const dwarf::Info                                                     &info,
                                    const std::string                                                     &prefix,
                                    u64                                                                    baseAddr,
                                    u64                                                                    typeOff,
                                    StructChannelPayload                                                  &sp,
                                    const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memberOverrides,
                                    int                                                                    maxDepth,
                                    const std::unordered_map<std::string, unit_conversion::Settings> *conversions = nullptr);

void
Variable::restoreTableWidths(TableUiState &state, int columnCount)
{
        if (!state.restoreWidths)
                return;
        ImGuiTable *table = ImGui::GetCurrentTable();
        if (!table || table->IsLayoutLocked)
                return;
        for (int i = 0; i < columnCount && i < table->ColumnsCount && i < static_cast<int>(state.widths.size()); ++i) {
                const float width = state.widths[i];
                if (width <= 1.0f)
                        continue;
                ImGuiTableColumn &column = table->Columns[i];
                if (column.Flags & ImGuiTableColumnFlags_WidthStretch)
                        column.StretchWeight = width;
                else
                        column.WidthRequest =
                            std::max(width, ImGui::GetFontSize() * 3.0f + ImGui::GetStyle().CellPadding.x * 2);
                column.AutoFitQueue = 0;
        }
        state.restoreWidths = false;
}

void
Variable::restoreTableSort(TableUiState &state)
{
        if (!state.restoreSort)
                return;
        ImGui::TableSetColumnSortDirection(state.sortColumn, static_cast<ImGuiSortDirection>(state.sortDirection), false);
        state.restoreSort = false;
}

void
Variable::captureTableWidths(TableUiState &state, int columnCount)
{
        const ImGuiTable *table        = ImGui::GetCurrentTable();
        const bool        userResizing = table && table->ResizedColumn >= 0;
        for (int i = 0; i < columnCount && i < static_cast<int>(state.widths.size()); ++i) {
                const float width = table && i < table->ColumnsCount ? table->Columns[i].WidthGiven : 0.0f;
                if (width <= 1.0f)
                        continue;
                if (state.initialized && userResizing && std::abs(state.widths[i] - width) > 0.5f)
                        isModified_ = true;
                state.widths[i] = width;
        }
        state.initialized = true;
}

void
Variable::captureTableSort(TableUiState &state, int column, int dir)
{
        if (state.sortColumn == column && state.sortDirection == dir)
                return;
        state.sortColumn    = column;
        state.sortDirection = dir;
        isModified_         = true;
}

void
Variable::drawSymbolTree()
{
        std::lock_guard lk(mtxElf_);
        if (dwarfInfo_.present && !dwarfInfo_.variables.empty()) {
                if (ui::TableIsland tableCard;
                    tableCard.begin("SymbolTreeTableV2",
                                    4,
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                    ImVec2(0, 0))) {
                        auto symbolSortFlags = [&](int column) {
                                ImGuiTableColumnFlags result = ImGuiTableColumnFlags_None;
                                if (symbolTableUi_.sortColumn == column) {
                                        result |= ImGuiTableColumnFlags_DefaultSort;
                                        result |= symbolTableUi_.sortDirection == ImGuiSortDirection_Descending
                                                      ? ImGuiTableColumnFlags_PreferSortDescending
                                                      : ImGuiTableColumnFlags_PreferSortAscending;
                                }
                                return result;
                        };
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_WidthStretch | symbolSortFlags(0));
                        ImGui::TableSetupColumn(tr("Address###col_addr", "地址###col_addr"),
                                                ImGuiTableColumnFlags_WidthFixed | symbolSortFlags(1),
                                                120.0f);
                        ImGui::TableSetupColumn(tr("Size###col_size", "大小###col_size"),
                                                ImGuiTableColumnFlags_WidthFixed | symbolSortFlags(2),
                                                60.0f);
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"),
                                                ImGuiTableColumnFlags_WidthStretch | symbolSortFlags(3));
                        restoreTableWidths(symbolTableUi_, 4);
                        restoreTableSort(symbolTableUi_);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();

                        std::vector<const dwarf::Variable *> vars;
                        vars.reserve(dwarfInfo_.variables.size());
                        for (const auto &v : dwarfInfo_.variables)
                                vars.push_back(&v);

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        captureTableSort(symbolTableUi_, spec->ColumnIndex, spec->SortDirection);
                                        std::sort(vars.begin(),
                                                  vars.end(),
                                                  [&](const dwarf::Variable *a, const dwarf::Variable *b) -> bool {
                                                          int cmp = 0;
                                                          switch (spec->ColumnIndex) {
                                                                  case 0: // Name
                                                                          cmp = a->name.compare(b->name);
                                                                          break;
                                                                  case 1: // Address
                                                                          cmp = (a->addr < b->addr)
                                                                                    ? -1
                                                                                    : (a->addr > b->addr ? 1 : 0);
                                                                          break;
                                                                  case 2: // Size
                                                                          cmp = ((int)typeSize(dwarfInfo_, a->type) -
                                                                                 (int)typeSize(dwarfInfo_, b->type));
                                                                          break;
                                                                  case 3: // Type
                                                                          cmp = prettyType(dwarfInfo_, a->type)
                                                                                    .compare(prettyType(dwarfInfo_, b->type));
                                                                          break;
                                                          }
                                                          if (cmp == 0)
                                                                  cmp = a->name.compare(b->name);
                                                          return spec->SortDirection == ImGuiSortDirection_Ascending
                                                                     ? (cmp < 0)
                                                                     : (cmp > 0);
                                                  });
                                }
                        }

                        for (const auto *v : vars) {
                                drawSymbolLeaf(v->name, v->name, v->addr, v->type, 0);
                        }
                        captureTableWidths(symbolTableUi_, 4);
                        tableCard.end();
                }
        } else if (!dataTree_.children.empty()) {
                if (ui::TableIsland tableCard;
                    tableCard.begin("BinTreeTableV2",
                                    3,
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                    ImVec2(0, 0))) {
                        auto binSortFlags = [&](int column) {
                                ImGuiTableColumnFlags result = ImGuiTableColumnFlags_None;
                                if (binTableUi_.sortColumn == column) {
                                        result |= ImGuiTableColumnFlags_DefaultSort;
                                        result |= binTableUi_.sortDirection == ImGuiSortDirection_Descending
                                                      ? ImGuiTableColumnFlags_PreferSortDescending
                                                      : ImGuiTableColumnFlags_PreferSortAscending;
                                }
                                return result;
                        };
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_WidthStretch | binSortFlags(0));
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"),
                                                ImGuiTableColumnFlags_WidthStretch | binSortFlags(1));
                        ImGui::TableSetupColumn(tr("Value###col_value", "数值###col_value"),
                                                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthStretch);
                        restoreTableWidths(binTableUi_, 3);
                        restoreTableSort(binTableUi_);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();

                        std::vector<DataTree *> nodes;
                        nodes.reserve(dataTree_.children.size());
                        for (auto &n : dataTree_.children)
                                nodes.push_back(&n);

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        captureTableSort(binTableUi_, spec->ColumnIndex, spec->SortDirection);
                                        std::sort(
                                            nodes.begin(), nodes.end(), [&](const DataTree *a, const DataTree *b) -> bool {
                                                    int cmp = 0;
                                                    switch (spec->ColumnIndex) {
                                                            case 0:
                                                                    cmp = a->name.compare(b->name);
                                                                    break;
                                                            case 1:
                                                                    cmp = std::string(Parser::dataTypeToStr(a->type))
                                                                              .compare(Parser::dataTypeToStr(b->type));
                                                                    break;
                                                    }
                                                    if (cmp == 0)
                                                            cmp = a->name.compare(b->name);
                                                    return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0)
                                                                                                               : (cmp > 0);
                                            });
                                }
                        }

                        for (auto *n : nodes)
                                drawDataTreeLeaf(*n);
                        captureTableWidths(binTableUi_, 3);
                        tableCard.end();
                }
        } else {
                ImGui::TextDisabled("%s", tr("Drop an ELF or BIN file to browse symbols.", "拖入 ELF 或 BIN 文件以浏览符号。"));
        }
}

void
Variable::drawSymbolLeaf(
    const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff, i32 depth, u32 bitOffset, u32 bitSize)
{
        // Note: mtxElf_ is already held by drawSymbolTree()
        if (depth > 16)
                return;
        const dwarf::Type *t          = resolveAlias(dwarfInfo_, typeOff);
        const bool         isStruct   = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION);
        const bool         isArray    = t && t->kind == dwarf::TypeKind::ARRAY;
        const bool         isEnum     = t && t->kind == dwarf::TypeKind::ENUM;
        const bool         expandable = (isStruct && !t->members.empty()) || (isArray && !t->dims.empty()) || isEnum;
        const char        *scalarKind = scalarPayloadType(dwarfInfo_, typeOff);

        ImGui::PushID((int)addr ^ (depth << 16) ^ (int)typeOff);
        ui::HierarchyNextRow();
        ImGui::TableSetColumnIndex(0);

        bool open = false;
        if (expandable) {
                open = ui::HierarchyNode(displayName.c_str(), ImGuiTreeNodeFlags_OpenOnArrow);
        } else {
                ui::HierarchyNode(displayName.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        }

        // Drag support
        if (ImGui::BeginDragDropSource()) {
                if (!scalarKind && (isStruct || isArray)) {
                        // Flatten the struct/array into per-member scalar chs so the
                        // monitor plots each leaf, matching the watch-list behaviour
                        // Dropping a raw struct as a single CHANNEL produced one
                        // un-plottable ch (the abnormal display the user hit)
                        auto sp = std::make_unique<StructChannelPayload>();
                        snprintf(sp->device, sizeof(sp->device), "JLINK");
                        snprintf(sp->rootName, sizeof(sp->rootName), "%s", fullPath.c_str());
                        sp->rootAddr    = addr;
                        sp->rootTypeOff = typeOff;
                        sp->srcWatch    = this; // mirror back into this window's watch list on a monitor drop
                        flattenForStructPayload(dwarfInfo_, fullPath, addr, typeOff, *sp, nullptr, 8);
                        ImGui::SetDragDropPayload("STRUCT_CHANNEL", sp.get(), sizeof(*sp));
                } else {
                        ChannelDropPayload p{};
                        snprintf(p.name, sizeof(p.name), "%s", fullPath.c_str());
                        p.addr = addr;
                        if (scalarKind)
                                snprintf(p.type, sizeof(p.type), "%s", scalarKind);
                        snprintf(p.device, sizeof(p.device), "JLINK");
                        p.numBytes  = (u8)typeSize(dwarfInfo_, typeOff);
                        p.typeOff   = typeOff;
                        p.bitOffset = bitOffset;
                        p.bitSize   = bitSize;
                        p.srcWatch  = this; // mirror back into this window's watch list on a monitor drop
                        fillEnumPayload(dwarfInfo_, typeOff, p);
                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                }
                ImGui::Text(tr("Dragging %s", "拖拽 %s"), fullPath.c_str());
                ImGui::EndDragDropSource();
        }
        // Double click to add
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !ImGui::IsItemToggledOpen())
                addSymbolToWatch(fullPath, addr, typeOff, PortType::JLINK, bitOffset, bitSize);

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("0x%08llX", (unsigned long long)addr);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, typeOff));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(prettyType(dwarfInfo_, typeOff).c_str());

        if (open) {
                if (isStruct) {
                        for (const auto &m : t->members) {
                                drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name,
                                               fullPath + "." + (m.name.empty() ? "<anon>" : m.name),
                                               addr + m.offset,
                                               m.type,
                                               depth + 1,
                                               m.bitOffset,
                                               m.bitSize);
                        }
                } else if (isArray) {
                        u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                        u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                        u64 displayed = (dim == 0) ? 0 : (dim < elfArrayMaxElems_ ? dim : elfArrayMaxElems_);
                        for (u64 i = 0; i < displayed; ++i) {
                                std::string idx = "[" + std::to_string(i) + "]";
                                drawSymbolLeaf(idx, fullPath + idx, addr + i * elemSize, t->inner, depth + 1);
                        }
                } else if (isEnum) {
                        for (const auto &e : t->enums) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ui::HierarchyNode(e.name.c_str(),
                                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                      ImGuiTreeNodeFlags_SpanFullWidth);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%lld", (long long)e.val);
                        }
                }
                ImGui::TreePop();
        }
        ImGui::PopID();
}

void
Variable::drawDataTreeLeaf(DataTree &node, const int indentLevel)
{
        // Unique per-node ID: template names repeat across sibling structs/arrays,
        // and without this the TreeNodeEx IDs collide — ImGui then can't tell the
        // rows apart, which silently breaks drag-and-drop / hover for the dupes
        // (drawSymbolLeaf and the watch table push IDs for the same reason.)
        ImGui::PushID(static_cast<const void *>(&node));

        ui::HierarchyNextRow();
        ImGui::TableSetColumnIndex(0);

        bool isArray = (node.type == DataType::ARRAY);
        bool open    = false;
        if (isArray) {
                open = ui::HierarchyNode(node.name.c_str(), ImGuiTreeNodeFlags_None);
        } else {
                ui::HierarchyNode(node.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        if (watchHasIdentity(node.name, 0, node.type)) {
                                ImGui::InsertNotification({ImGuiToastType::Info,
                                                           1800,
                                                           tr("%s is already in the variable list", "%s 已在变量列表中"),
                                                           node.name.c_str()});
                        } else {
                                VarEntry v;
                                v.name     = node.name;
                                v.type     = node.type;
                                v.port     = PortType::LOCAL;
                                v.addr     = 0;
                                v.writable = false;
                                {
                                        std::lock_guard<std::mutex> lk(mtxLocal_);
                                        localBufs_.emplace(
                                            v.name, std::vector<uint8_t>(std::max<u32>(1, Parser::typeBytes(v.type)), 0));
                                }
                                vars_.push_back(v);
                                isModified_ = true;
                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                           2000,
                                                           tr("Added %s to variable list", "已将 %s 加入变量列表"),
                                                           node.name.c_str()});
                        }
                }

                if (ImGui::BeginDragDropSource()) {
                        ChannelDropPayload p{};
                        snprintf(p.name, sizeof(p.name), "%s", node.name.c_str());
                        p.addr = 0;
                        snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(node.type));
                        snprintf(p.device, sizeof(p.device), "LOCAL");
                        p.numBytes = (u8)Parser::typeBytes(node.type);
                        p.typeOff  = 0;
                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), node.name.c_str());
                        ImGui::EndDragDropSource();
                }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(Parser::dataTypeToStr(node.type));
        ImGui::TableSetColumnIndex(2);
        if (std::holds_alternative<f32>(node.val))
                ImGui::Text("%f", std::get<f32>(node.val));
        else if (std::holds_alternative<u32>(node.val))
                ImGui::Text("%u", std::get<u32>(node.val));
        else if (std::holds_alternative<i32>(node.val))
                ImGui::Text("%d", std::get<i32>(node.val));

        if (open && isArray) {
                for (auto &child : node.children)
                        drawDataTreeLeaf(child, indentLevel + 1);
                ImGui::TreePop();
        }

        ImGui::PopID();
}

static void
flattenForStructPayload(const dwarf::Info                                                     &info,
                        const std::string                                                     &prefix,
                        u64                                                                    baseAddr,
                        u64                                                                    typeOff,
                        StructChannelPayload                                                  &sp,
                        const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memberOverrides,
                        int                                                                    maxDepth,
                        const std::unordered_map<std::string, unit_conversion::Settings>      *conversions)
{
        if (sp.count >= StructChannelPayload::kMaxEntries || maxDepth <= 0)
                return;
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;

        // Fill a leaf entry's enum array: user override (if any) wins over DWARF
        auto fillEntryEnums = [&info,
                               memberOverrides](StructChannelPayload::Entry &e, u64 leafTypeOff, const std::string &leafPath) {
                if (memberOverrides) {
                        auto it = memberOverrides->find(leafPath);
                        if (it != memberOverrides->end() && !it->second.empty()) {
                                e.numEnums = (u8)std::min((int)it->second.size(), StructChannelPayload::kMaxEnums);
                                for (int i = 0; i < e.numEnums; ++i) {
                                        snprintf(e.enums[i].name, sizeof(e.enums[i].name), "%s", it->second[i].name.c_str());
                                        e.enums[i].val = it->second[i].val;
                                }
                                return;
                        }
                }
                const dwarf::Type *lt = resolveAlias(info, leafTypeOff);
                if (!lt || lt->kind != dwarf::TypeKind::ENUM) {
                        e.numEnums = 0;
                        return;
                }
                e.numEnums = (u8)std::min((int)lt->enums.size(), StructChannelPayload::kMaxEnums);
                for (int i = 0; i < e.numEnums; ++i) {
                        snprintf(e.enums[i].name, sizeof(e.enums[i].name), "%s", lt->enums[i].name.c_str());
                        e.enums[i].val = lt->enums[i].val;
                }
        };

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        if (sp.count >= StructChannelPayload::kMaxEntries)
                                break;
                        std::string mPath = prefix + "." + (m.name.empty() ? "<anon>" : m.name);
                        u64         mAddr = baseAddr + m.offset;
                        const char *sType = scalarPayloadType(info, m.type);
                        if (sType) {
                                auto &e = sp.entries[sp.count++];
                                snprintf(e.name, sizeof(e.name), "%s", mPath.c_str());
                                e.addr = mAddr;
                                snprintf(e.type, sizeof(e.type), "%s", sType);
                                e.bitOffset = m.bitOffset;
                                e.bitSize   = m.bitSize;
                                e.writable  = sp.writable;
                                if (conversions) {
                                        if (const auto found = conversions->find(mPath); found != conversions->end())
                                                e.unitConversion = found->second;
                                }
                                fillEntryEnums(e, m.type, mPath);
                        } else {
                                flattenForStructPayload(
                                    info, mPath, mAddr, m.type, sp, memberOverrides, maxDepth - 1, conversions);
                        }
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                u64         elemSize = typeSize(info, t->inner);
                u64         dim      = t->dims.empty() ? 0 : t->dims.front();
                const char *sType    = scalarPayloadType(info, t->inner);
                for (u64 i = 0; i < dim && sp.count < StructChannelPayload::kMaxEntries; ++i) {
                        std::string idxPath = prefix + "[" + std::to_string(i) + "]";
                        u64         eAddr   = baseAddr + i * elemSize;
                        if (sType) {
                                auto &e = sp.entries[sp.count++];
                                snprintf(e.name, sizeof(e.name), "%s", idxPath.c_str());
                                e.addr = eAddr;
                                snprintf(e.type, sizeof(e.type), "%s", sType);
                                e.writable = sp.writable;
                                if (conversions) {
                                        if (const auto found = conversions->find(idxPath); found != conversions->end())
                                                e.unitConversion = found->second;
                                }
                                fillEntryEnums(e, t->inner, idxPath);
                        } else {
                                flattenForStructPayload(
                                    info, idxPath, eAddr, t->inner, sp, memberOverrides, maxDepth - 1, conversions);
                        }
                }
        }
}

bool
Variable::refreshSymbolSearch()
{
        if (searchAppliedGeneration_ == searchPoolGeneration_ && searchAppliedQuery_ == searchBuf_)
                return false;
        std::lock_guard lock(mtxElf_);
        searchAppliedGeneration_ = searchPoolGeneration_;
        searchAppliedQuery_      = searchBuf_;
        searchResults_.clear();
        const std::string query = ui::search::LowerCopy(searchAppliedQuery_);
        if (query.empty())
                return true;
        std::unordered_set<std::string> matchedPaths;
        for (const auto &entry : searchPool_) {
                std::string path = entry.path;
                std::ranges::transform(path, path.begin(), [](unsigned char c) { return std::tolower(c); });
                if (path.find(query) == std::string::npos && !ui::search::MatchesAddress(entry.addr, query))
                        continue;
                bool hasParent = false;
                for (size_t i = 0; i < entry.path.size(); ++i)
                        if ((entry.path[i] == '.' || entry.path[i] == '[') && matchedPaths.contains(entry.path.substr(0, i))) {
                                hasParent = true;
                                break;
                        }
                if (!hasParent) {
                        searchResults_.push_back(entry);
                        matchedPaths.insert(entry.path);
                }
        }
        return true;
}

void
Variable::drawSymbolBrowserPanel(bool activated)
{
        const bool wasPinned = symbolBrowserHover_.isPinned();
        const auto viewport  = ImGui::GetWindowViewport();
        const auto panelId   = "SymbolBrowser###SymbolBrowser_" + std::to_string(ImGui::GetID("symbol_browser"));
        const auto path      = absoluteDisplayPath(!elfPath_.empty() ? elfPath_ : (!binPath_.empty() ? binPath_ : cfgPath_));
        if (ui::BeginHoverPanel(panelId.c_str(),
                                tr("Symbol File", "符号文件"),
                                symbolBrowserHover_,
                                activated,
                                ImGui::GetFontSize() * 64,
                                viewport)) {
                if (!path.empty()) {
                        ImGui::TextDisabled("%s", std::filesystem::u8path(path).filename().string().c_str());
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", path.c_str());
                } else
                        ImGui::TextDisabled("%s", tr("No symbol file loaded", "尚未加载符号文件"));
                if (ui::Button(tr("Load / replace...", "加载 / 替换..."), ui::BtnStyle::Neutral))
                        state_ = WindowState::LoadElf;
                ImGui::SameLine();
                if (ImGui::Button(tr("Remote...", "远程...")))
                        ImGui::OpenPopup("remoteSymbols");
                if (ImGui::BeginPopup("remoteSymbols")) {
                        ImGui::BeginDisabled(isElfLoading_.load());
                        const auto selected = remoteSymbols_.draw(remote::settings().firmwareUrl, {".axf", ".elf"});
                        if (!selected.empty()) {
                                if (loadElf(selected))
                                        isModified_ = true;
                                ImGui::CloseCurrentPopup();
                        }
                        if (remote::isUrl(elfPath_) &&
                            ImGui::MenuItem(tr("Reload current remote file", "重新加载当前远程文件"))) {
                                loadElf(elfPath_);
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndDisabled();
                        ImGui::EndPopup();
                }
                ImGui::Separator();
                const float height = std::min(viewport->WorkSize.y * .65f, ImGui::GetFontSize() * 40);
                if (ImGui::BeginChild("SymbolBrowserSection",
                                      ImVec2(0, std::max(1.0f, height)),
                                      false,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                        drawSymbolBrowser();
                ImGui::EndChild();
                ImGui::End();
        }
        if (wasPinned != symbolBrowserHover_.isPinned())
                isModified_ = true;
}

void
Variable::drawSymbolBrowser()
{
        TutorialGuide::instance().mark("variable_browser");
        if (isElfLoading_) {
                ImGui::Text("%s", tr("Loading symbols...", "正在加载符号..."));
                ImGui::SameLine();
                static float ang  = 0.0f;
                ang              += 0.1f;
                ImGui::Text("%c", "|/-\\"[(int)(ang) % 4]);
                return;
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint(
            "##symbol_search",
            tr("Search name or address (e.g. motor.position / 0x20000000)", "搜索名称或地址，例如 motor.position / 0x20000000"),
            searchBuf_,
            sizeof(searchBuf_));
        const bool searchChanged = refreshSymbolSearch();

        if (searchBuf_[0] == '\0') {
                drawSymbolTree();
        } else {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
                if (ui::TableIsland tableCard; tableCard.begin("SymbolSearchTableV2", 4, flags, ImVec2(0, 0))) {
                        auto symbolSortFlags = [&](int column) {
                                ImGuiTableColumnFlags result = ImGuiTableColumnFlags_None;
                                if (symbolTableUi_.sortColumn == column) {
                                        result |= ImGuiTableColumnFlags_DefaultSort;
                                        result |= symbolTableUi_.sortDirection == ImGuiSortDirection_Descending
                                                      ? ImGuiTableColumnFlags_PreferSortDescending
                                                      : ImGuiTableColumnFlags_PreferSortAscending;
                                }
                                return result;
                        };
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_WidthStretch | symbolSortFlags(0));
                        ImGui::TableSetupColumn(tr("Address###col_addr", "地址###col_addr"),
                                                ImGuiTableColumnFlags_WidthFixed | symbolSortFlags(1),
                                                120.0f);
                        ImGui::TableSetupColumn(tr("Size###col_size", "大小###col_size"),
                                                ImGuiTableColumnFlags_WidthFixed | symbolSortFlags(2),
                                                60.0f);
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"),
                                                ImGuiTableColumnFlags_WidthStretch | symbolSortFlags(3));
                        restoreTableWidths(symbolTableUi_, 4);
                        restoreTableSort(symbolTableUi_);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();
                        if (searchChanged)
                                ImGui::SetScrollY(0);

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0 && (sorts_specs->SpecsDirty || searchChanged)) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        captureTableSort(symbolTableUi_, spec->ColumnIndex, spec->SortDirection);
                                        std::sort(searchResults_.begin(),
                                                  searchResults_.end(),
                                                  [&](const SearchEntry &a, const SearchEntry &b) -> bool {
                                                          int cmp = 0;
                                                          switch (spec->ColumnIndex) {
                                                                  case 0:
                                                                          cmp = a.path.compare(b.path);
                                                                          break;
                                                                  case 1:
                                                                          cmp = (a.addr < b.addr) ? -1
                                                                                                  : (a.addr > b.addr ? 1 : 0);
                                                                          break;
                                                                  case 2:
                                                                          cmp = ((int)typeSize(dwarfInfo_, a.typeOff) -
                                                                                 (int)typeSize(dwarfInfo_, b.typeOff));
                                                                          break;
                                                                  case 3:
                                                                          cmp = prettyType(dwarfInfo_, a.typeOff)
                                                                                    .compare(prettyType(dwarfInfo_, b.typeOff));
                                                                          break;
                                                          }
                                                          if (cmp == 0)
                                                                  cmp = a.path.compare(b.path);
                                                          return spec->SortDirection == ImGuiSortDirection_Ascending
                                                                     ? (cmp < 0)
                                                                     : (cmp > 0);
                                                  });
                                        sorts_specs->SpecsDirty = false;
                                }
                        }

                        for (int i = 0; i < (int)searchResults_.size(); ++i) {
                                const auto &e = searchResults_[i];
                                ImGui::PushID(i);
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);

                                const dwarf::Type *t = resolveAlias(dwarfInfo_, e.typeOff);
                                const bool         isComplex =
                                    t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                          t->kind == dwarf::TypeKind::ARRAY);

                                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
                                if (!isComplex)
                                        nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                                bool open = ui::HierarchyNode(e.path.c_str(), nodeFlags);

                                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !ImGui::IsItemToggledOpen())
                                        addSymbolToWatch(e.path, e.addr, e.typeOff, e.defaultPort, e.bitOffset, e.bitSize);

                                if (ImGui::BeginDragDropSource()) {
                                        if (isComplex) {
                                                // Flatten struct/array into per-member chs so the
                                                // monitor plots each leaf instead of one bad ch
                                                auto sp = std::make_unique<StructChannelPayload>();
                                                snprintf(sp->device,
                                                         sizeof(sp->device),
                                                         e.defaultPort == PortType::JLINK ? "JLINK" : "SHM");
                                                snprintf(sp->rootName, sizeof(sp->rootName), "%s", e.path.c_str());
                                                sp->rootAddr    = e.addr;
                                                sp->rootTypeOff = e.typeOff;
                                                flattenForStructPayload(dwarfInfo_, e.path, e.addr, e.typeOff, *sp, nullptr, 8);
                                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", sp.get(), sizeof(*sp));
                                        } else {
                                                ChannelDropPayload p{};
                                                snprintf(p.name, sizeof(p.name), "%s", e.path.c_str());
                                                p.addr = e.addr;
                                                snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(e.type));
                                                snprintf(p.device,
                                                         sizeof(p.device),
                                                         e.defaultPort == PortType::JLINK ? "JLINK" : "SHM");
                                                p.numBytes  = (u8)Parser::typeBytes(e.type);
                                                p.typeOff   = e.typeOff;
                                                p.bitOffset = e.bitOffset;
                                                p.bitSize   = e.bitSize;
                                                fillEnumPayload(dwarfInfo_, e.typeOff, p);
                                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                        }
                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), e.path.c_str());
                                        ImGui::EndDragDropSource();
                                }

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("0x%08llX", (unsigned long long)e.addr);
                                ImGui::TableSetColumnIndex(2);
                                ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, e.typeOff));
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextUnformatted(prettyType(dwarfInfo_, e.typeOff).c_str());

                                if (open && isComplex) {
                                        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                                                for (const auto &m : t->members)
                                                        drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name,
                                                                       e.path + "." + m.name,
                                                                       e.addr + m.offset,
                                                                       m.type,
                                                                       1,
                                                                       m.bitOffset,
                                                                       m.bitSize);
                                        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                                                u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                                                u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                                                u64 displayed = dim < (u64)elfArrayMaxElems_ ? dim : (u64)elfArrayMaxElems_;
                                                for (u64 j = 0; j < displayed; ++j) {
                                                        std::string idx = "[" + std::to_string(j) + "]";
                                                        drawSymbolLeaf(idx, e.path + idx, e.addr + j * elemSize, t->inner, 1);
                                                }
                                        }
                                        ImGui::TreePop();
                                }

                                ImGui::PopID();
                        }
                        captureTableWidths(symbolTableUi_, 4);
                        tableCard.end();
                }
        }
}

void
Variable::drawFunctionPanel()
{
        if (ImGui::Button(tr("Library...##varfn", "动态库...##varfn"))) {
                std::string path = nativeDlgOpen(tr("Select library", "选择动态库"), {{"Libraries", {"dll", "so", "dylib"}}});
                if (!path.empty())
                        loadFunctionLibrary(path);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(functionSources_.empty());
        if (ImGui::Button(tr("Load All##varfn", "全部加载##varfn")))
                for (auto &source : functionSources_)
                        if (!source->libraryPath.empty())
                                loadFunctionLibrary(source->libraryPath);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Header...##varfn", "头文件...##varfn"))) {
                std::string path = nativeDlgOpen(tr("Select header", "选择头文件"), {{"Headers", {"h", "hpp", "hh", "hxx"}}});
                if (!path.empty())
                        parseFunctionHeader(path);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(functionSources_.empty());
        if (ImGui::Button(tr("Reparse All##varfn", "全部重解析##varfn")))
                for (auto &source : functionSources_)
                        if (!source->headerPath.empty())
                                parseFunctionHeader(source->headerPath);
        ImGui::EndDisabled();

        for (size_t i = 0; i < functionSources_.size(); ++i) {
                const auto &source      = *functionSources_[i];
                size_t      methodCount = 0;
                for (const auto &cls : source.declarations.classes)
                        methodCount += cls.methods.size();
                const std::string key = functionSourceKey(source);
                const std::string label =
                    key.empty() ? tr("Unconfigured source", "未配置源") : std::filesystem::path(key).filename().string();
                ImGui::TextDisabled("[%d] %s  C=%d C++=%d/%d",
                                    (int)i + 1,
                                    label.c_str(),
                                    (int)source.declarations.functions.size(),
                                    (int)source.declarations.classes.size(),
                                    (int)methodCount);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("DLL: %s\nHeader: %s", source.libraryPath.c_str(), source.headerPath.c_str());
                if (!source.status.empty()) {
                        ImGui::SameLine();
                        const ImVec4 color = source.statusError     ? ui::ErrorTextColor()
                                             : source.statusWarning ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f)
                                                                    : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
                        ImGui::TextColored(color, "%s", source.status.c_str());
                }
        }
        constexpr ImGuiTableFlags flags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ui::TableIsland tableCard; tableCard.begin("FunctionBrowserTable", 4, flags, ImVec2(0, 0))) {
                ImGui::TableSetupColumn(tr("Source", "来源"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn(tr("Kind", "类别"), ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn(tr("Signature", "签名"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const auto &sourcePtr : functionSources_) {
                        const auto       &source     = *sourcePtr;
                        const std::string sourceName = std::filesystem::path(functionSourceKey(source)).filename().string();
                        for (const auto &fn : source.declarations.functions) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(sourceName.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "C");
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextUnformatted(fn.name.c_str());
                                ImGui::TableSetColumnIndex(3);
                                const std::string sig = functionSignature(fn);
                                ImGui::TextUnformatted(sig.c_str());
                        }
                        for (const auto &cls : source.declarations.classes) {
                                for (const auto &method : cls.methods) {
                                        ui::HierarchyNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(sourceName.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextDisabled("C++");
                                        ImGui::TableSetColumnIndex(2);
                                        const std::string name = cls.fullName + "::" + method.name;
                                        ImGui::TextUnformatted(name.c_str());
                                        ImGui::TableSetColumnIndex(3);
                                        const std::string sig = methodSignature(method);
                                        ImGui::TextDisabled("%s", sig.c_str());
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip(
                                                    "%s",
                                                    tr("C++ member method: visible for inspection, but LOCAL bindings require "
                                                       "a C ABI wrapper.",
                                                       "C++ 成员方法：可浏览，但 LOCAL 绑定需要 C ABI 包装函数。"));
                                }
                        }
                }
                tableCard.end();
        }
}

static bool
isIntegerDataType(DataType type)
{
        return type == DataType::U8 || type == DataType::U16 || type == DataType::U32 || type == DataType::U64 ||
               type == DataType::I8 || type == DataType::I16 || type == DataType::I32 || type == DataType::I64;
}

void
Variable::drawFunctionBindingPopup()
{
        if (functionBindIdx_ < 0 || functionBindIdx_ >= (int)vars_.size())
                return;
        ImGui::OpenPopup("###BindVariableFunctions");
        if (!ImGui::BeginPopupModal(tr("Bind Functions###BindVariableFunctions", "绑定读写函数###BindVariableFunctions"),
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
                return;

        VarEntry &v = vars_[functionBindIdx_];
        ImGui::Text("%s: %s (%s)", tr("Variable", "变量"), v.name.c_str(), Parser::dataTypeToStr(v.type));
        auto drawBinding = [&](const char *label, VarEntry::FunctionBinding &binding, bool read) {
                FunctionSource   *resolvedSource = nullptr;
                const CFuncDecl  *resolved       = findFunction(binding, &resolvedSource);
                const std::string preview =
                    binding.empty()
                        ? tr("(none)", "（无）")
                        : (resolved ? std::filesystem::path(functionSourceKey(*resolvedSource)).filename().string() +
                                          " :: " + functionSignature(*resolved)
                                    : binding.functionName);
                ImGui::SetNextItemWidth(440.0f);
                if (ImGui::BeginCombo(label, preview.c_str())) {
                        if (ImGui::Selectable(tr("(none)", "（无）"), binding.empty())) {
                                binding = {};
                                if (read)
                                        v.readFunctionError.clear();
                                else
                                        v.writeFunctionError.clear();
                                v.functionError = !v.writeFunctionError.empty() ? v.writeFunctionError : v.readFunctionError;
                                isModified_     = true;
                        }
                        for (const auto &source : functionSources_) {
                                const std::string sourceName =
                                    std::filesystem::path(functionSourceKey(*source)).filename().string();
                                for (const auto &fn : source->declarations.functions) {
                                        if (!(read ? isReadAccessor(fn, v.type) : isWriteAccessor(fn, v.type)))
                                                continue;
                                        const std::string sig  = functionSignature(fn);
                                        const std::string item = sourceName + " :: " + sig;
                                        if (ImGui::Selectable(item.c_str(), resolved == &fn)) {
                                                binding.sourcePath   = functionSourceKey(*source);
                                                binding.functionName = fn.name;
                                                binding.signature    = sig;
                                                v.functionError.clear();
                                                v.readFunctionError.clear();
                                                v.writeFunctionError.clear();
                                                isModified_ = true;
                                        }
                                }
                        }
                        ImGui::EndCombo();
                }
                if (!binding.empty() && !resolved)
                        ui::ErrorText("%s", tr("Saved function/signature is unresolved.", "保存的函数/签名已失效。"));
        };
        drawBinding(tr("Read function", "读取函数"), v.readFunction, true);
        drawBinding(tr("Write function", "写入函数"), v.writeFunction, false);
        if (!v.functionError.empty())
                ui::ErrorTextWrapped("%s: %s", tr("Last error", "最近错误"), v.functionError.c_str());
        ImGui::TextDisabled("%s",
                            tr("Read: T fn(void) or status fn(T*). Write: status fn(T) or status fn(T*).",
                               "读取：T fn(void) 或 status fn(T*)；写入：status fn(T) 或 status fn(T*)。"));
        if (ImGui::Button(tr("Done", "完成"), ImVec2(120, 0))) {
                functionBindIdx_ = -1;
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
}

static bool
isWatchAggregate(const dwarf::Type *type)
{
        return type && (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION ||
                        type->kind == dwarf::TypeKind::ARRAY);
}

void
Variable::focusWatch(int parent, const std::string &path)
{
        watchFocusPreview_.commit();
        if (parent < 0 || parent >= static_cast<int>(vars_.size()))
                watchFocus_ = {};
        else {
                const auto &owner = vars_[parent];
                watchFocus_       = {owner.name, owner.port, path, owner.addr};
        }
        watchFocusScrollReset_ = true;
        rowDragSrc_ = fieldDragParent_ = fieldDragSrc_ = -1;
}

int
Variable::resolveWatchFocus(std::vector<WatchFocusNode> &nodes) const
{
        nodes.clear();
        if (watchFocus_.owner.empty())
                return -1;
        std::lock_guard lock(mtxElf_);
        int             ownerIndex = -1, matches = 0, addressMatches = 0, addressIndex = -1;
        for (int i = 0; i < static_cast<int>(vars_.size()); ++i) {
                const auto &v = vars_[i];
                if (v.name != watchFocus_.owner || v.port != watchFocus_.port)
                        continue;
                ownerIndex = i;
                ++matches;
                if (v.addr == watchFocus_.ownerAddress) {
                        addressIndex = i;
                        ++addressMatches;
                }
        }
        if (matches != 1)
                ownerIndex = addressMatches == 1 ? addressIndex : -1;
        if (ownerIndex < 0)
                return -1;
        const auto &owner  = vars_[ownerIndex];
        const auto &target = watchFocus_.path;
        if (owner.flashArray || owner.addrUnknown || !target.starts_with(owner.name))
                return -1;
        WatchFocusNode              node{owner.name, owner.name, owner.addr, owner.typeOff};
        std::vector<WatchFocusNode> resolved;
        for (;;) {
                const auto *type = resolveAlias(dwarfInfo_, node.typeOff);
                if (!isWatchAggregate(type))
                        return -1;
                resolved.push_back(node);
                if (node.path == target) {
                        nodes = std::move(resolved);
                        return ownerIndex;
                }
                const size_t pos = node.path.size();
                if (pos >= target.size())
                        return -1;
                if (type->kind == dwarf::TypeKind::ARRAY) {
                        if (target[pos] != '[' || type->dims.empty())
                                return -1;
                        const auto end = target.find(']', pos + 1);
                        if (end == std::string::npos)
                                return -1;
                        u64        index{};
                        const auto parsed = std::from_chars(target.data() + pos + 1, target.data() + end, index);
                        if (parsed.ec != std::errc{} || parsed.ptr != target.data() + end || index >= type->dims.front())
                                return -1;
                        node.label    = target.substr(pos, end - pos + 1);
                        node.path    += node.label;
                        node.addr    += index * typeSize(dwarfInfo_, type->inner);
                        node.typeOff  = type->inner;
                } else {
                        if (target[pos] != '.')
                                return -1;
                        bool found = false;
                        for (const auto &member : type->members) {
                                const std::string label = member.name.empty() ? "<anon>" : member.name;
                                const std::string path  = node.path + "." + label;
                                if (!target.starts_with(path) ||
                                    (target.size() > path.size() && target[path.size()] != '.' && target[path.size()] != '['))
                                        continue;
                                node  = {path, label, node.addr + member.offset, member.type};
                                found = true;
                                break;
                        }
                        if (!found)
                                return -1;
                }
                if (owner.hiddenMembers.contains(node.path))
                        return -1;
                if (const auto it = owner.memberOverrides.find(node.path); it != owner.memberOverrides.end()) {
                        const auto &override = it->second;
                        if (override.hasType)
                                return -1; // An aggregate overridden as a scalar has no visible subtree
                        if (override.hasAddress)
                                node.addr = override.address;
                        if (!override.alias.empty())
                                node.label = override.alias;
                }
        }
}

void
Variable::drawWatchFocusNavigation()
{
        if (watchFocus_.owner.empty())
                return;
        std::vector<WatchFocusNode> nodes;
        const int                   ownerIndex = resolveWatchFocus(nodes);
        if (ownerIndex < 0) {
                focusWatch(-1, {});
                return;
        }
        if (ImGui::SmallButton(tr("< Back###watch_focus_back", "< 返回上级###watch_focus_back"))) {
                if (nodes.size() > 1)
                        focusWatch(ownerIndex, nodes[nodes.size() - 2].path);
                else
                        focusWatch(-1, {});
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("All variables###watch_focus_all", "全部变量###watch_focus_all")))
                focusWatch(-1, {});
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Focused view", "聚焦视图"));

        // Each crumb has a canonical ID (repeated names such as "lo" are common)
        // Wrap between segments; cap a long individual label and show its full path on hover
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < nodes.size(); ++i) {
                const auto &node  = nodes[i];
                const float width = ImGui::CalcTextSize(node.label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
                if (i > 0 &&
                    ImGui::GetItemRectMax().x + width + ImGui::CalcTextSize(">").x + ImGui::GetStyle().ItemSpacing.x * 3 <=
                        right) {
                        ImGui::SameLine();
                        ImGui::TextUnformatted(">");
                        ImGui::SameLine();
                }
                ImGui::PushID(node.path.c_str());
                if (ImGui::Button((node.label + "###crumb").c_str(),
                                  ImVec2(std::max(1.0f, std::min(width, ImGui::GetContentRegionAvail().x)), 0)))
                        focusWatch(ownerIndex, node.path);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", node.path.c_str());
                if (ImGui::BeginPopupContextItem("focus_crumb_actions")) {
                        drawWatchValueActions(ownerIndex, node.path);
                        if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                openRewriteMember(ownerIndex, node.addr, node.typeOff, node.path);
                        ImGui::EndPopup();
                }
                ImGui::PopID();
        }
}

bool
Variable::drawWatchFocusTable()
{
        // Resolve after the table has sorted vars_; never retain an owner index
        // across sorting or an address/type offset across a symbol reload
        std::vector<WatchFocusNode> nodes;
        const int                   ownerIndex = resolveWatchFocus(nodes);
        if (ownerIndex < 0)
                return false;
        const auto      node  = nodes.back();
        const auto     &owner = vars_[ownerIndex];
        std::lock_guard lock(mtxElf_);
        ImGui::PushID("watch_focus");
        ImGui::PushID(node.path.c_str());
        ui::HierarchyGap();
        ui::HierarchyCard focusedCard;
        focusedCard.begin(0, true);
        const int firstRow = ImGui::TableGetRowIndex();
        drawVarVarTreeRow(node.path, node.addr, node.typeOff, 0, owner.port, owner.shm.name, ownerIndex);
        if (ImGui::TableGetRowIndex() == firstRow) {
                ui::HierarchyNextRow();
                ui::HierarchyRow(0);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", tr("No visible members", "没有可显示的成员"));
        }
        ImGui::PopID();
        ImGui::PopID();
        return true;
}

Variable::PointerRead
Variable::readPointerMemory(const VarEntry &owner, u64 address, u32 size, u8 *bytes, u64 notBefore)
{
        if (owner.addrUnknown || size == 0 || size > 8 || address > UINT64_MAX - (size - 1))
                return PointerRead::Error;
        if (owner.port == PortType::JLINK) {
                if (!JLinkPort::instance().isConnected())
                        return PointerRead::Disconnected;
                if (address > UINT32_MAX - (size - 1))
                        return PointerRead::Error; // This J-Link transport has a 32-bit address space.
                auto &requested  = memberPollReqs_[address];
                requested        = std::max(requested, size);
                const auto found = syncReadCache_.find(address);
                if (found == syncReadCache_.end() || found->second.sz < size || found->second.timestamp < notBefore ||
                    found->second.epoch != JLinkPort::instance().targetEpoch() ||
                    get_mono_ts_ms() - found->second.timestamp > std::max<u64>(1000, u64(updateIntervalMs_) * 5))
                        return PointerRead::Waiting;
                if (!found->second.ok)
                        return PointerRead::Error;
                std::memcpy(bytes, found->second.buf, size);
                return PointerRead::Ready;
        }
        if (owner.port != PortType::LOCAL && owner.port != PortType::SHM)
                return PointerRead::Unsupported;
        const auto type = size == 1 ? DataType::U8 : size == 2 ? DataType::U16 : size == 4 ? DataType::U32 : DataType::U64;
        if (Parser::typeBytes(type) != size)
                return PointerRead::Error;
        // LOCAL/SHM reads are restricted to registered snapshots. Never dereference
        // an arbitrary native pointer supplied by firmware, a file, or a plug-in.
        if (readWatchBytes(owner, address, type, bytes))
                return PointerRead::Ready;
        for (const auto &candidate : vars_) {
                if (&candidate == &owner || candidate.port != owner.port || candidate.addrUnknown)
                        continue;
                if (owner.port == PortType::SHM && std::strcmp(owner.shm.name, candidate.shm.name) != 0)
                        continue;
                if (readWatchBytes(candidate, address, type, bytes))
                        return PointerRead::Ready;
        }
        return PointerRead::Error;
}

void
Variable::drawPointerButton(int index, u64 address, u64 typeOff, const std::string &path)
{
        if (index < 0 || index >= int(vars_.size()))
                return;
        const auto *type = resolveAlias(dwarfInfo_, typeOff);
        if (!type || type->kind != dwarf::TypeKind::POINTER)
                return;
        const u32  size = type->size == 8 ? 8 : type->size == 2 ? 2 : 4;
        u64        target{};
        const auto status = readPointerMemory(vars_[index], address, size, reinterpret_cast<u8 *>(&target));
        char       label[80];
        if (status == PointerRead::Ready)
                snprintf(label,
                         sizeof(label),
                         target ? "0x%0*llX >" : "nullptr >",
                         int(size * 2),
                         static_cast<unsigned long long>(target));
        else
                snprintf(label,
                         sizeof(label),
                         "%s >",
                         status == PointerRead::Error          ? "ERR"
                         : status == PointerRead::Disconnected ? tr("Disconnected", "未连接")
                         : status == PointerRead::Unsupported  ? tr("Unavailable", "不支持")
                                                               : tr("Reading...", "读取中..."));
        ImGui::PushID(path.c_str());
        if (status == PointerRead::Ready)
                valueFlash_.cell("pointer:" + path, label);
        if (status == PointerRead::Error)
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
        const bool selected = ImGui::SmallButton((std::string(label) + "###PointerTarget").c_str());
        if (status == PointerRead::Error)
                ImGui::PopStyleColor();
        if (selected) {
                const auto &owner = vars_[index];
                pointerView_ = {true, owner.name, path, owner.port, owner.addr, address, typeOff, elfLayoutGeneration_, {}};
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s\n%s",
                                  prettyType(dwarfInfo_, typeOff).c_str(),
                                  tr("Inspect pointer target (read only)", "查看指针指向的内容（只读）"));
        ImGui::PopID();
}

void
Variable::drawPointerInspector()
{
        if (!pointerView_.open)
                return;
        auto &view     = pointerView_;
        auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f, viewport->WorkPos.y + viewport->WorkSize.y * .5f),
            ImGuiCond_Appearing,
            ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(viewport->WorkSize.x, ImGui::GetFontSize() * 48),
                                        std::min(viewport->WorkSize.y, ImGui::GetFontSize() * 20)),
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(320.f, viewport->WorkSize.x), std::min(180.f, viewport->WorkSize.y)), viewport->WorkSize);
        const std::string title = std::string(tr("Pointer", "指针查看")) + "###PointerInspector" + name_;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetFontSize() * .65f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
        const bool visible = ImGui::Begin(title.c_str(), &view.open, ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar(2);
        if (!visible) {
                ImGui::End();
                return;
        }
        auto found = std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) {
                return v.name == view.owner && v.port == view.port && v.addr == view.ownerAddress;
        });
        if (found == vars_.end() || view.generation != elfLayoutGeneration_) {
                ui::ErrorText(
                    tr("Variable or symbols changed. Select the pointer again.", "变量或符号已改变，请重新选择指针。"));
                ImGui::End();
                return;
        }
        const auto &owner = *found;
        ImGui::TextWrapped("%s", view.path.c_str());
        auto statusText = [](PointerRead state) {
                switch (state) {
                        case PointerRead::Waiting:
                                return tr("Reading...", "读取中...");
                        case PointerRead::Disconnected:
                                return tr("Disconnected", "未连接");
                        case PointerRead::Unsupported:
                                return tr("This port does not support pointer reads", "当前端口不支持指针读取");
                        default:
                                return tr("Read failed / inaccessible address", "读取失败 / 地址不可访问");
                }
        };
        int                                                                                         rows      = 0;
        bool                                                                                        truncated = false;
        std::vector<std::pair<u64, u64>>                                                            ancestors;
        std::function<void(u64, u64, const std::string &, const std::string &, int, u64, u32, u32)> drawNode;
        drawNode = [&](u64                offset,
                       u64                address,
                       const std::string &label,
                       const std::string &path,
                       int                depth,
                       u64                barrier,
                       u32                bitOffset,
                       u32                bitSize) {
                if (rows >= 256 || depth > 12) {
                        truncated = true;
                        return;
                }
                ++rows;
                const auto *type      = resolveAlias(dwarfInfo_, offset);
                const bool  pointer   = type && type->kind == dwarf::TypeKind::POINTER;
                const bool  aggregate = isWatchAggregate(type);
                const bool  cycle =
                    std::find(ancestors.begin(), ancestors.end(), std::pair<u64, u64>{address, offset}) != ancestors.end();
                u64         target{};
                u8          bytes[8]{};
                const char *scalar   = scalarPayloadType(dwarfInfo_, offset);
                const auto  dataType = scalar ? Parser::strToDataType(scalar) : DataType::UNKNOWN;
                PointerRead state    = PointerRead::Ready;
                std::string value;
                if (cycle)
                        value = tr("Circular reference", "循环引用");
                else if (scalar) {
                        const auto size = Parser::typeBytes(dataType);
                        state           = readPointerMemory(owner, address, size, bytes, barrier);
                        if (state == PointerRead::Ready) {
                                if (pointer) {
                                        std::memcpy(&target, bytes, size);
                                        char text[32];
                                        snprintf(text,
                                                 sizeof(text),
                                                 "0x%0*llX",
                                                 int(size * 2),
                                                 static_cast<unsigned long long>(target));
                                        value = target ? text : "nullptr";
                                } else
                                        value = decodeValue(bytes, dataType, bitOffset, bitSize, owner.radixForPath(view.path));
                        } else
                                value = statusText(state);
                } else if (!aggregate)
                        value = tr("No readable type information", "无可读取的类型信息");
                const auto *pointee = pointer ? resolveAlias(dwarfInfo_, type->inner) : nullptr;
                const bool  follow =
                    pointer && target && state == PointerRead::Ready && pointee && pointee->kind != dwarf::TypeKind::SUBROUTINE;
                if (pointer && target && !pointee)
                        value += tr(" (void / incomplete type)", "（void / 不完整类型）");
                if (pointee && pointee->kind == dwarf::TypeKind::SUBROUTINE)
                        value += tr(" (function)", "（函数）");
                ImGui::PushID(path.c_str());
                ImGui::PushID(static_cast<int>(address >> 32));
                ImGui::PushID(static_cast<int>(address));
                ui::HierarchyNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool hasChildren = !cycle && (aggregate || follow);
                auto       flags       = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (!hasChildren)
                        flags |= ImGuiTreeNodeFlags_Leaf;
                if (depth <= 1)
                        flags |= ImGuiTreeNodeFlags_DefaultOpen;
                const bool expanded = ui::HierarchyNode(label.c_str(), flags) && hasChildren;
                ui::HierarchyRow(expanded ? depth : std::max(0, depth - 1));
                ui::HierarchyCard card;
                if (expanded)
                        card.begin(depth);
                ImGui::TableSetColumnIndex(1);
                if (state == PointerRead::Ready && scalar && !cycle)
                        valueFlash_.cell("pointer-preview:" + path + "@" + std::to_string(address), value);
                if (state == PointerRead::Error)
                        ui::ErrorText("%s", value.c_str());
                else
                        ui::TableText(value);
                ImGui::TableSetColumnIndex(2);
                ui::TableText(prettyType(dwarfInfo_, offset));
                ImGui::TableSetColumnIndex(3);
                watchAddress(address);
                if (expanded) {
                        ancestors.emplace_back(address, offset);
                        if (follow) {
                                auto [it, inserted] = view.targets.try_emplace(path, target, get_mono_ts_ms());
                                if (!inserted && it->second.first != target)
                                        it->second = {target, get_mono_ts_ms()};
                                // A moved pointer must not show a cached value from an earlier target visit.
                                const u64 nextBarrier = std::max(barrier, it->second.second);
                                drawNode(type->inner, target, "*" + label, path + "*", depth + 1, nextBarrier, 0, 0);
                        } else if (type->kind == dwarf::TypeKind::ARRAY) {
                                const u64 stride = typeSize(dwarfInfo_, type->inner);
                                const u64 count  = stride ? typeSize(dwarfInfo_, offset) / stride : 0;
                                for (u64 i = 0; i < std::min<u64>(count, 64) && rows < 256; ++i) {
                                        if (i > (UINT64_MAX - address) / stride) {
                                                truncated = true;
                                                break;
                                        }
                                        const auto item = "[" + std::to_string(i) + "]";
                                        drawNode(
                                            type->inner, address + i * stride, item, path + item, depth + 1, barrier, 0, 0);
                                }
                                truncated |= count > 64;
                        } else {
                                for (const auto &member : type->members) {
                                        if (rows >= 256) {
                                                truncated = true;
                                                break;
                                        }
                                        if (member.offset > UINT64_MAX - address) {
                                                truncated = true;
                                                continue;
                                        }
                                        drawNode(member.type,
                                                 address + member.offset,
                                                 member.name,
                                                 path + "." + member.name,
                                                 depth + 1,
                                                 barrier,
                                                 member.bitOffset,
                                                 member.bitSize);
                                }
                        }
                        ancestors.pop_back();
                }
                card.end();
                ImGui::PopID();
                ImGui::PopID();
                ImGui::PopID();
        };
        const float        footer = ImGui::GetTextLineHeightWithSpacing() * 2;
        ui::HierarchyTable table;
        if (table.begin("##PointerContents",
                        4,
                        ui::HierarchyTableFlags | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                        ImVec2(0, std::max(1.0f, ImGui::GetContentRegionAvail().y - footer)))) {
                ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn(tr("Value", "数值"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
                ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(tr("Address", "地址"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ui::HierarchyHeadersRow();
                drawNode(view.typeOff, view.address, view.path, view.path, 0, 0, 0, 0);
                table.end();
        }
        if (truncated)
                ImGui::TextWrapped(
                    "%s",
                    tr("Preview limit: 64 array elements, 256 rows, 12 levels.", "预览上限：数组 64 项、256 行、12 层。"));
        ImGui::End();
}

bool
Variable::readWatchBytes(const VarEntry &owner, u64 address, DataType type, u8 *bytes) const
{
        const auto size = Parser::typeBytes(type);
        if (owner.addrUnknown || !size || size > 8)
                return false;
        if (owner.port == PortType::JLINK) {
                const auto found = syncReadCache_.find(address);
                if (found != syncReadCache_.end() && found->second.ok && found->second.sz >= size &&
                    (!JLinkPort::instance().isConnected() || found->second.epoch == JLinkPort::instance().targetEpoch())) {
                        std::memcpy(bytes, found->second.buf, size);
                        return true;
                }
        } else if (address >= owner.addr) {
                const u64 offset = address - owner.addr;
                auto      copy   = [&](const auto &buffer) {
                        if (offset > buffer.size() || size > buffer.size() - offset)
                                return false;
                        std::memcpy(bytes, buffer.data() + offset, size);
                        return true;
                };
                if (owner.port == PortType::LOCAL) {
                        std::lock_guard lock(mtxLocal_);
                        const auto      found = localBufs_.find(owner.name);
                        if (found != localBufs_.end() && owner.readFunctionError.empty() && copy(found->second))
                                return true;
                } else if (owner.port == PortType::SHM) {
                        const auto found = shmShadowBufs_.find(shmShadowKey(owner));
                        if (found != shmShadowBufs_.end() && copy(found->second))
                                return true;
                }
        }
        if (address == owner.addr && !owner.isStruct && owner.displayByteSize >= size && owner.valueStr != "ERR" &&
            (owner.port != PortType::JLINK || !JLinkPort::instance().isConnected())) {
                std::memcpy(bytes, owner.displayBytes.data(), size);
                return true;
        }
        return false;
}

bool
Variable::readLocalChannelBytes(const std::string &name, RawSample &out) const
{
        out = {};
        for (const auto &owner : vars_) {
                if (owner.port != PortType::LOCAL)
                        continue;
                if (owner.name == name && !owner.isStruct) {
                        out.size = Parser::typeBytes(owner.type);
                        return readWatchBytes(owner, owner.addr, owner.type, out.bytes.data());
                }
                for (const auto &field : owner.structFields)
                        if (name == owner.name + "." + field.name) {
                                out.size = Parser::typeBytes(field.type);
                                return readWatchBytes(owner, owner.addr + field.byteOffset, field.type, out.bytes.data());
                        }
        }
        return false;
}

void
Variable::drawWatchValueActions(int index, const std::string &path)
{
        if (index < 0 || index >= static_cast<int>(vars_.size()))
                return;
        auto &owner = vars_[index];
        if (ImGui::MenuItem(tr("Copy", "复制")))
                requestWatchCopy(index, path);
        if (ImGui::BeginMenu(tr("Value display format", "数值显示格式"))) {
                const int   radices[] = {10, 16, 8, 2};
                const char *labels[]  = {
                    tr("Decimal", "十进制"), tr("Hexadecimal", "十六进制"), tr("Octal", "八进制"), tr("Binary", "二进制")};
                int selected = 0;
                for (int i = 0; i < 4; ++i)
                        if (owner.radixForPath(path) == radices[i])
                                selected = i;
                if (ui::Choice("##value_format", &selected, labels, 4)) {
                        const int i = selected;
                        if (path == owner.name)
                                owner.displayRadix = radices[i];
                        else
                                owner.memberDisplayRadices[path] = radices[i];
                        if (path == owner.name && owner.displayByteSize) {
                                owner.valueStr = decodeValue(
                                    owner.displayBytes.data(), owner.type, owner.bitOffset, owner.bitSize, owner.displayRadix);
                                owner.prevGoodValueStr = owner.valueStr;
                        }
                        memberValueCache_.clear();
                        prevUpdateTs_ = 0;
                        isModified_   = true;
                }
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Floating point: raw IEEE 754 bits", "浮点数：显示 IEEE 754 原始位模式"));
                ImGui::EndMenu();
        }
        const auto conversion = path == owner.name ? owner.unitConversion : [&] {
                const auto found = owner.memberUnitConversions.find(path);
                return found == owner.memberUnitConversions.end() ? unit_conversion::Settings{} : found->second;
        }();
        if (ImGui::BeginMenu(tr("Unit conversion", "单位换算"))) {
                const std::string conversionLabel = conversion.enabled
                                                        ? std::string(unit_conversion::UnitLabel(conversion.source)) + " → " +
                                                              unit_conversion::UnitLabel(conversion.target)
                                                        : tr("Not configured", "未设置");
                ImGui::TextDisabled("%s", conversionLabel.c_str());
                if (ImGui::MenuItem(conversion.enabled ? tr("Edit...", "编辑...") : tr("Set...", "设置..."))) {
                        unitConversionEditOwner_         = owner.name;
                        unitConversionEditPath_          = path;
                        unitConversionEditDraft_         = conversion;
                        unitConversionEditDraft_.enabled = true;
                        unitConversionEditRequested_     = true;
                }
                if (conversion.enabled && ImGui::MenuItem(tr("Disable", "关闭"))) {
                        auto disabled    = conversion;
                        disabled.enabled = false;
                        if (path == owner.name)
                                owner.unitConversion = disabled;
                        else
                                owner.memberUnitConversions[path] = disabled;
                        Monitor::updateUnitConversion(path, std::nullopt, disabled);
                        isModified_ = true;
                }
                ImGui::EndMenu();
        }
        ImGui::Separator();
}

std::string
Variable::watchClipboardText(int index, const std::string &selectedPath, bool *waiting)
{
        if (waiting)
                *waiting = false;
        if (index < 0 || index >= static_cast<int>(vars_.size()))
                return {};
        const auto &owner = vars_[index];
        const auto  path  = selectedPath.empty() ? owner.name : selectedPath;
        std::string text;
        ui::appendClipboardRow(text, tr("Name", "变量名"), tr("Value", "变量值"), tr("Address", "地址"));
        auto contains = [](const std::string &parent, const std::string &child) {
                return child == parent || (child.size() > parent.size() && child.starts_with(parent) &&
                                           (child[parent.size()] == '.' || child[parent.size()] == '['));
        };
        size_t           rows    = 0;
        constexpr size_t maxRows = 100000;
        auto             append  = [&](const std::string &name, const std::string &value, u64 address) {
                if (!contains(path, name) || rows >= maxRows)
                        return;
                char addressText[32]{};
                if (!owner.addrUnknown && (owner.port == PortType::JLINK || owner.flashArray))
                        snprintf(addressText, sizeof(addressText), "0x%08llX", static_cast<unsigned long long>(address));
                ui::appendClipboardRow(text, name, value, addressText);
                ++rows;
        };
        auto scalar = [&](const std::string &name, u64 address, DataType type, u32 bitOffset, u32 bitSize) {
                if (!contains(path, name))
                        return;
                u8          bytes[8]{};
                std::string value;
                if (readWatchBytes(owner, address, type, bytes)) {
                        value = decodeValue(bytes, type, bitOffset, bitSize, owner.radixForPath(name));
                } else {
                        const auto cached = memberValueCache_.find(name);
                        value             = name == owner.name                  ? owner.valueStr
                                            : cached != memberValueCache_.end() ? cached->second
                                                                                : "...";
                        if (value.empty())
                                value = "...";
                        const auto found = syncReadCache_.find(address);
                        if (found != syncReadCache_.end() && !found->second.ok)
                                value = "ERR";
                        if (owner.addrUnknown)
                                value = "UNKNOWN";
                        const auto size = Parser::typeBytes(type);
                        if (waiting && !owner.addrUnknown && owner.port == PortType::JLINK && size && size <= 8) {
                                pendingCopyRequests_[address] = size;
                                const bool missing            = found == syncReadCache_.end() || found->second.sz < size ||
                                                     (JLinkPort::instance().isConnected() &&
                                                      found->second.epoch != JLinkPort::instance().targetEpoch());
                                *waiting = missing || *waiting;
                                if (missing)
                                        value = "...";
                        }
                        if (value == "...")
                                value = tr("Not sampled", "未采样");
                }
                append(name, value, address);
        };
        if (owner.flashArray) {
                const auto &array = *owner.flashArray;
                const auto  width = flasharray::width(array.type);
                append(owner.name, "[" + std::to_string(array.values.size()) + "]", owner.addr);
                for (size_t i = 0; i < array.values.size() && rows < maxRows; ++i) {
                        const auto  name  = owner.name + "[" + std::to_string(i) + "]";
                        const int   radix = owner.radixForPath(name);
                        const auto *bytes = array.bytes.data() + i * width;
                        const auto  value = radix == 10 ? flasharray::format(bytes, array.type, array.bigEndian)
                                                        : formatIntegerValue(flasharray::bits(bytes, width, array.bigEndian),
                                                                            false,
                                                                            static_cast<u32>(width * 8),
                                                                            radix);
                        append(name, value, owner.addr + i * width);
                }
                if (rows >= maxRows)
                        ui::appendClipboardRow(text, "...", tr("Copy limited to 100000 rows", "复制内容达到 100000 行上限"));
                return text;
        }
        if (!owner.structFields.empty()) {
                append(owner.name, "{...}", owner.addr);
                for (const auto &field : owner.structFields)
                        scalar(owner.name + "." + field.name, owner.addr + field.byteOffset, field.type, 0, 0);
        } else {
                std::lock_guard                                                        lock(mtxElf_);
                std::function<void(const std::string &, u64, u64, unsigned, u32, u32)> visit;
                visit = [&](const std::string &name, u64 address, u64 typeId, unsigned depth, u32 bitOffset, u32 bitSize) {
                        if (rows >= maxRows || (!contains(path, name) && !contains(name, path)))
                                return;
                        if (depth > 32) {
                                append(name, tr("Nesting limit reached", "达到嵌套深度上限"), address);
                                return;
                        }
                        const auto override = owner.memberOverrides.find(name);
                        if (override != owner.memberOverrides.end() && override->second.hasAddress)
                                address = override->second.address;
                        const auto *type  = resolveAlias(dwarfInfo_, typeId);
                        const char *token = scalarPayloadType(dwarfInfo_, typeId);
                        if (override != owner.memberOverrides.end() && override->second.hasType) {
                                scalar(name, address, override->second.type, bitOffset, bitSize);
                        } else if (token || !type) {
                                scalar(name, address, token ? Parser::strToDataType(token) : owner.type, bitOffset, bitSize);
                        } else if (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION) {
                                append(name, "{...}", address);
                                for (const auto &member : type->members)
                                        visit(name + "." + (member.name.empty() ? "<anon>" : member.name),
                                              address + member.offset,
                                              member.type,
                                              depth + 1,
                                              member.bitOffset,
                                              member.bitSize);
                        } else if (type->kind == dwarf::TypeKind::ARRAY) {
                                if (type->dims.empty()) {
                                        append(name, "[?]", address);
                                        return;
                                }
                                const auto elementSize = typeSize(dwarfInfo_, type->inner);
                                if (!elementSize) {
                                        append(name, "...", address);
                                        return;
                                }
                                std::function<void(const std::string &, u64, size_t)> dimension;
                                dimension = [&](const std::string &prefix, u64 base, size_t dim) {
                                        if (!contains(path, prefix) && !contains(prefix, path))
                                                return;
                                        if (dim >= type->dims.size()) {
                                                visit(prefix, base, type->inner, depth + 1, 0, 0);
                                                return;
                                        }
                                        append(prefix, "[" + std::to_string(type->dims[dim]) + "]", base);
                                        u64 stride = elementSize;
                                        for (size_t d = dim + 1; d < type->dims.size(); ++d) {
                                                if (type->dims[d] && stride > UINT64_MAX / type->dims[d])
                                                        return;
                                                stride *= type->dims[d];
                                        }
                                        for (u64 i = 0; i < type->dims[dim] && rows < maxRows; ++i) {
                                                if (stride && i > (UINT64_MAX - base) / stride)
                                                        break;
                                                dimension(prefix + "[" + std::to_string(i) + "]", base + i * stride, dim + 1);
                                        }
                                };
                                dimension(name, address, 0);
                        } else {
                                append(name, "...", address);
                        }
                };
                visit(owner.name, owner.addr, owner.typeOff, 0, owner.bitOffset, owner.bitSize);
        }
        if (rows >= maxRows)
                ui::appendClipboardRow(text, "...", tr("Copy limited to 100000 rows", "复制内容达到 100000 行上限"));
        return text;
}

void
Variable::requestWatchCopy(int index, const std::string &path)
{
        pendingCopyOwner_    = vars_[index].name;
        pendingCopyPath_     = path;
        pendingCopyDeadline_ = get_mono_ts_ms() + 5000;
        pendingCopyNextPoll_ = 0;
        pendingCopyRequests_.clear();
}

void
Variable::updateWatchCopy()
{
        if (pendingCopyOwner_.empty() || get_mono_ts_ms() < pendingCopyNextPoll_)
                return;
        const auto found = std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) { return v.name == pendingCopyOwner_; });
        if (found != vars_.end()) {
                bool waiting = false;
                pendingCopyRequests_.clear();
                const auto text = watchClipboardText(static_cast<int>(found - vars_.begin()), pendingCopyPath_, &waiting);
                if (waiting && JLinkPort::instance().isConnected() && get_mono_ts_ms() < pendingCopyDeadline_) {
                        pendingCopyNextPoll_ = get_mono_ts_ms() + std::max<u32>(20, updateIntervalMs_);
                        return;
                }
                ImGui::SetClipboardText(text.c_str());
                ImGui::InsertNotification({ImGuiToastType::Success, 2500, "%s", tr("Variable data copied", "已复制变量数据")});
        }
        pendingCopyOwner_.clear();
        pendingCopyRequests_.clear();
}

void
Variable::drawWatchFocusButton(int parent, const std::string &path)
{
        if (ImGui::SmallButton(tr("Focus###focus_group", "聚焦###focus_group")))
                focusWatch(parent, path);
}

void
Variable::markWatchSearchRow(int owner, const std::string &path)
{
        if (owner < 0 || owner >= static_cast<int>(vars_.size()) || vars_[owner].name != watchSearchSelectedOwner_ ||
            path != watchSearchSelectedPath_)
                return;
        ui::HierarchySelection();
        if (watchSearchScroll_) {
                ImGui::SetScrollHereY(.35f);
                watchSearchScroll_ = false;
        }
}

void
Variable::drawWatchSearch(bool activated)
{
        (void)activated;
        if (!watchSearchHover_.isOpen())
                return;
        const std::string query     = ui::search::LowerCopy(watchSearchText_);
        size_t            signature = searchPoolGeneration_;
        const auto        hash      = [&](const auto &value) {
                signature ^=
                    std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (signature << 6) + (signature >> 2);
        };
        for (const auto &v : vars_) {
                hash(v.name);
                hash(v.addr);
                hash(v.typeOff);
                hash(static_cast<int>(v.port));
                for (const auto &member : v.structFields) {
                        hash(std::string(member.name));
                        hash(member.byteOffset);
                        hash(static_cast<int>(member.type));
                }
                for (const auto &member : v.hiddenMembers)
                        hash(member);
                for (const auto &[path, member] : v.memberOverrides) {
                        hash(path);
                        hash(member.alias);
                        hash(member.hasType);
                        hash(static_cast<int>(member.type));
                }
        }
        if (query != watchSearchQuery_ || signature != watchSearchSignature_) {
                watchSearchQuery_     = query;
                watchSearchSignature_ = signature;
                watchSearchResults_.clear();
                watchSearchLimited_     = false;
                size_t          visited = 0;
                std::lock_guard lock(mtxElf_);
                for (const auto &v : vars_) {
                        std::vector<std::string> ancestors;
                        const auto               add = [&](const std::string &path, u64 address) {
                                const auto alias = v.memberOverrides.find(path);
                                if (query.empty() || ui::search::LowerCopy(path).find(query) != std::string::npos ||
                                    (alias != v.memberOverrides.end() &&
                                     ui::search::LowerCopy(alias->second.alias).find(query) != std::string::npos) ||
                                    (!v.addrUnknown && ui::search::MatchesAddress(address, query)))
                                        watchSearchResults_.push_back({v.name, path, v.port, address, ancestors});
                        };
                        std::function<void(const std::string &, u64, u64, int)> visit;
                        visit = [&](const std::string &path, u64 typeOff, u64 address, int depth) {
                                if (v.hiddenMembers.contains(path))
                                        return;
                                if (++visited > 20000 || watchSearchResults_.size() >= 500 || depth > 32) {
                                        watchSearchLimited_ = true;
                                        return;
                                }
                                add(path, address);
                                if (query.empty())
                                        return;
                                if (const auto override = v.memberOverrides.find(path);
                                    override != v.memberOverrides.end() && override->second.hasType)
                                        return;
                                const auto *type = resolveAlias(dwarfInfo_, typeOff);
                                if (!type)
                                        return;
                                ancestors.push_back(path);
                                if (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION) {
                                        for (const auto &member : type->members) {
                                                if (watchSearchLimited_)
                                                        break;
                                                visit(path + "." + (member.name.empty() ? "<anon>" : member.name),
                                                      member.type,
                                                      address + member.offset,
                                                      depth + 1);
                                        }
                                } else if (type->kind == dwarf::TypeKind::ARRAY && !type->dims.empty()) {
                                        u64 first = 0, count = type->dims.front();
                                        // A specific array index can be found without walking every preceding element.
                                        const auto leaf = path.substr(
                                            path.find_last_of('.') == std::string::npos ? 0 : path.find_last_of('.') + 1);
                                        const auto prefix = ui::search::LowerCopy(leaf) + "[";
                                        const auto at     = query.find(prefix);
                                        if (at != std::string::npos) {
                                                const auto *begin  = query.data() + at + prefix.size();
                                                u64         index  = 0;
                                                const auto  parsed = std::from_chars(begin, query.data() + query.size(), index);
                                                if (parsed.ec == std::errc{} && parsed.ptr < query.data() + query.size() &&
                                                    *parsed.ptr == ']' && index < count) {
                                                        first = index;
                                                        count = index + 1;
                                                }
                                        }
                                        const u64 elementSize = typeSize(dwarfInfo_, type->inner);
                                        for (u64 i = first; i < count && !watchSearchLimited_; ++i)
                                                visit(path + "[" + std::to_string(i) + "]",
                                                      type->inner,
                                                      address + i * elementSize,
                                                      depth + 1);
                                }
                                ancestors.pop_back();
                        };
                        visit(v.name, v.typeOff, v.addr, 0);
                        if (!query.empty() && !v.structFields.empty()) {
                                ancestors.push_back(v.name);
                                for (const auto &member : v.structFields)
                                        if (!v.hiddenMembers.contains(v.name + (v.isArray ? "" : ".") + member.name))
                                                add(v.name + (v.isArray ? "" : ".") + member.name, v.addr + member.byteOffset);
                        }
                        if (watchSearchLimited_)
                                break;
                }
        }
        if (watchSearchLimited_)
                ImGui::TextDisabled("%s",
                                    tr("Results limited; refine the name or array index", "结果较多，请补充名称或数组下标"));
        if (watchSearchResults_.empty())
                ImGui::TextDisabled("%s", tr("No matching watched variables", "没有匹配的变量"));
        if (ImGui::BeginChild("##watch_search_results", ImVec2(0, ImGui::GetFontSize() * 10), ImGuiChildFlags_Borders)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(watchSearchResults_.size()));
                while (clipper.Step()) {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                                const auto &result = watchSearchResults_[i];
                                ImGui::PushID(i);
                                if (ImGui::Selectable(result.path.c_str(),
                                                      watchSearchSelectedPath_ == result.path &&
                                                          watchSearchSelectedOwner_ == result.owner,
                                                      ImGuiSelectableFlags_SelectOnClick)) {
                                        const auto owner = std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) {
                                                return v.name == result.owner && v.port == result.port;
                                        });
                                        if (owner != vars_.end()) {
                                                focusWatch(-1, {});
                                                for (const auto &ancestor : result.ancestors)
                                                        owner->expandedMembers.insert(ancestor);
                                                watchSearchSelectedOwner_ = result.owner;
                                                watchSearchSelectedPath_  = result.path;
                                                watchSearchScroll_        = true;
                                                isModified_               = true;
                                        }
                                        watchSearchHover_.close();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s\n%s\n%s",
                                                          result.path.c_str(),
                                                          ui::search::AddressText(result.address).c_str(),
                                                          portName(result.port));
                                ImGui::PopID();
                        }
                }
        }
        ImGui::EndChild();
}

void
Variable::drawVariableList()
{
        if (watchFocusPreview_.maintain(watchFocus_))
                watchFocusScrollReset_ = false;
        const float toolbarLeft      = ImGui::GetCursorScreenPos().x;
        const float toolbarTop       = ImGui::GetCursorScreenPos().y;
        const float toolbarRight     = toolbarLeft + ImGui::GetContentRegionAvail().x;
        const float refreshWidth     = std::min(ImGui::GetFontSize() * 9, (toolbarRight - toolbarLeft) * .35f);
        const float actionRight      = toolbarRight - refreshWidth - ImGui::GetStyle().ItemSpacing.x * 2;
        float       lastControlRight = 0;
        bool        toolbarGroup     = false;
        auto        toolbarButton    = [&](const char *label, bool addIcon = false) {
                const float width = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2 +
                                    (addIcon ? ImGui::GetFontSize() * 1.1f + ImGui::GetStyle().ItemInnerSpacing.x + 1 : 0);
                const float spacing = ImGui::GetStyle().ItemSpacing.x * (toolbarGroup ? 2.5f : 1.0f);
                if (lastControlRight > 0 && lastControlRight + spacing + width <= actionRight)
                        ImGui::SameLine(0, spacing);
                toolbarGroup = false;
                const bool clicked = addIcon ? ui::IconButton(label, ui::Icon::Add) : ui::Button(label, ui::BtnStyle::Neutral);
                lastControlRight = ImGui::GetItemRectMax().x;
                return clicked;
        };
        if (toolbarButton(tr("Add Variable", "添加变量"), true))
                state_ = WindowState::AddVariable;
        const float minSearchWidth = ImGui::GetFontSize() * 10;
        const float spacing        = ImGui::GetStyle().ItemSpacing.x;
        if (lastControlRight > 0 && lastControlRight + spacing + minSearchWidth <= actionRight)
                ImGui::SameLine();
        const float searchWidth = std::clamp(actionRight - ImGui::GetCursorScreenPos().x, 1.0f, ImGui::GetFontSize() * 13);
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::InputTextWithHint("##watch_search",
                                 tr("Search name, address or collapsed member", "搜索名称、地址或折叠成员"),
                                 watchSearchText_,
                                 sizeof(watchSearchText_));
        TutorialGuide::instance().mark("variables.search");
        const bool searchRequested = ImGui::IsItemActivated() || ImGui::IsItemEdited();
        if (searchRequested)
                watchSearchHover_.setPinned(true);
        if (ImGui::IsItemDeactivated() && watchSearchText_[0] == '\0')
                watchSearchHover_.close();
        lastControlRight = ImGui::GetItemRectMax().x;
        toolbarGroup     = false;
        if (toolbarButton(tr("Variable list...", "变量列表...")))
                ImGui::OpenPopup("watch_list_menu");
        bool requestClear = false;
        if (ImGui::BeginPopup("watch_list_menu")) {
                if (ImGui::MenuItem(tr("Import .var...", "导入 .var..."))) {
                        std::string path = nativeDlgOpen("Import Variables", {{"Variable Files", {"var", "json"}}});
                        if (!path.empty())
                                importVarFile(path);
                }
                if (ImGui::MenuItem(tr("Export .var...", "导出 .var..."), nullptr, false, !vars_.empty())) {
                        std::string path =
                            nativeDlgSave("Export Variables", {{"Variable Files", {"var"}}}, getTitle() + ".var");
                        if (!path.empty())
                                exportVarFile(path);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(tr("Clear variable list...", "清空变量列表..."), nullptr, false, !vars_.empty()))
                        requestClear = true;
                ImGui::EndPopup();
        }
        const char *clearPopup = tr("Clear variable list###clear_watch_list", "清空变量列表###clear_watch_list");
        if (requestClear)
                ImGui::OpenPopup(clearPopup);
        if (ImGui::BeginPopupModal(clearPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(tr("Clear the variable list?", "清空变量列表？"));
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
                ImGui::TextWrapped(
                    "%s",
                    tr("This removes the variable list only. Device memory and existing monitor channels are unchanged.",
                       "此操作仅移除变量列表。设备内存和已有监视器通道保持不变。"));
                ImGui::PopTextWrapPos();
                if (ui::Button(tr("Clear list", "清空列表"), ui::BtnStyle::Danger)) {
                        vars_.clear();
                        focusWatch(-1, {});
                        prevSelectedIndex_ = -1;
                        isModified_        = true;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }
        toolbarGroup = true;
        drawSymbolBrowserPanel(toolbarButton(tr("Symbol File###symbol_file_toggle", "符号文件###symbol_file_toggle")));
        toolbarGroup = true;
        if (toolbarButton(tr("Tools...", "工具...")))
                ImGui::OpenPopup("watch_tools_menu");
        if (ImGui::BeginPopup("watch_tools_menu")) {
                if (ImGui::MenuItem(tr("Parameter configuration...", "参数配置...")))
                        openParameterEditor();
                if (ImGui::MenuItem(tr("Safety...", "功能安全...")))
                        faultDecoder().openOverview();
                ImGui::EndPopup();
        }
        toolbarGroup = true;
        if (toolbarButton(tr("Collapse all", "全部折叠"))) {
                for (auto &variable : vars_)
                        variable.expandedMembers.clear();
                isModified_ = true;
        }
        const auto afterActions = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(toolbarRight - refreshWidth, toolbarTop));
        ImGui::SetNextItemWidth(refreshWidth);
        int refresh = static_cast<int>(updateIntervalMs_);
        if (ImGui::SliderInt("##RefreshMs", &refresh, 10, 2000, "%d ms", ImGuiSliderFlags_Logarithmic)) {
                updateIntervalMs_ = static_cast<u32>(refresh);
                isModified_       = true;
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Refresh interval; Ctrl+click to enter a value", "刷新间隔；Ctrl+点击可输入数值"));
        ImGui::SetCursorScreenPos(ImVec2(toolbarLeft, std::max(afterActions.y, ImGui::GetCursorScreenPos().y)));
        drawWatchSearch(false);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(tr("%d variables", "%d 个变量"), static_cast<int>(vars_.size()));
        ImGui::PopStyleColor();
        drawWatchFocusNavigation();

        constexpr ImGuiTableFlags flags = ui::HierarchyTableFlags | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable;
        if (ui::HierarchyTable tableCard; tableCard.begin("VarMonitorTableV2", 6, flags, ImVec2(0, 0))) {
                if (watchFocusScrollReset_) {
                        ImGui::SetScrollY(0.0f);
                        watchFocusScrollReset_ = false;
                }
                auto watchSortFlags = [&](int column) {
                        ImGuiTableColumnFlags result = ImGuiTableColumnFlags_None;
                        if (watchTableUi_.sortColumn == column) {
                                result |= ImGuiTableColumnFlags_DefaultSort;
                                result |= watchTableUi_.sortDirection == ImGuiSortDirection_Descending
                                              ? ImGuiTableColumnFlags_PreferSortDescending
                                              : ImGuiTableColumnFlags_PreferSortAscending;
                        }
                        return result;
                };
                ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                        ImGuiTableColumnFlags_WidthFixed | watchSortFlags(0),
                                        ui::TableColumnWidth(tr("Name", "名称"), "", 16));
                ImGui::TableSetupColumn(tr("Value###col_value", "数值###col_value"),
                                        ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                                        ui::TableColumnWidth(tr("Value", "数值"), "-123456.789012", 12));
                ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"),
                                        ImGuiTableColumnFlags_WidthFixed | watchSortFlags(2),
                                        ui::TableColumnWidth(tr("Type", "类型"), "enum (u32)", 6));
                ImGui::TableSetupColumn(tr("Address###col_addr", "地址###col_addr"),
                                        ImGuiTableColumnFlags_WidthFixed | watchSortFlags(3),
                                        ui::TableColumnWidth(tr("Address", "地址"), "0xFFFFFFFF", 8));
                ImGui::TableSetupColumn(tr("Port###col_port", "端口###col_port"),
                                        ImGuiTableColumnFlags_WidthFixed | watchSortFlags(4),
                                        ui::TableColumnWidth(tr("Port", "端口"), "JLINK", 4));
                ImGui::TableSetupColumn(tr("R/W###col_rw", "读写###col_rw"),
                                        ImGuiTableColumnFlags_WidthFixed | watchSortFlags(5),
                                        ui::TableColumnWidth(tr("R/W", "读写"), "RW", 3));
                restoreTableWidths(watchTableUi_, 6);
                const float readableColumns = ui::TableColumnWidth(tr("Name", "名称"), "", 16) +
                                              ui::TableColumnWidth(tr("Value", "数值"), "-123456.789012", 12) +
                                              ImGui::GetFontSize() * 4;
                ImGui::TableSetupScrollFreeze(ImGui::GetContentRegionAvail().x >= readableColumns ? 1 : 0, 1);
                restoreTableSort(watchTableUi_);
                ui::HierarchyHeadersRow();

                if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                        if (sorts_specs->SpecsCount > 0 && sorts_specs->SpecsDirty) {
                                const auto *spec = &sorts_specs->Specs[0];
                                captureTableSort(watchTableUi_, spec->ColumnIndex, spec->SortDirection);
                                std::sort(vars_.begin(), vars_.end(), [&](const VarEntry &a, const VarEntry &b) -> bool {
                                        int cmp = 0;
                                        switch (spec->ColumnIndex) {
                                                case 0:
                                                        cmp = a.name.compare(b.name);
                                                        break;
                                                case 2:
                                                        cmp = prettyType(dwarfInfo_, a.typeOff)
                                                                  .compare(prettyType(dwarfInfo_, b.typeOff));
                                                        break;
                                                case 3:
                                                        cmp = (a.addr < b.addr) ? -1 : (a.addr > b.addr ? 1 : 0);
                                                        break;
                                                case 4:
                                                        cmp = (int)a.port - (int)b.port;
                                                        break;
                                                case 5:
                                                        cmp = (int)a.writable - (int)b.writable;
                                                        break;
                                        }
                                        if (cmp == 0)
                                                cmp = a.name.compare(b.name);
                                        return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0) : (cmp > 0);
                                });
                                sorts_specs->SpecsDirty = false;
                        }
                }

                // Deferred reorder (drag grip): applied after the loop so we never
                // mutate vars_ mid-iteration
                int varMoveSrc = -1, varMoveDst = -1;

                // Drop the grip drag state once the drag ends
                if (!ImGui::IsDragDropActive()) {
                        rowDragSrc_      = -1;
                        fieldDragParent_ = -1;
                        fieldDragSrc_    = -1;
                }

                // Build the monitor drag payload (CHANNEL / STRUCT_CHANNEL) for a row. This is
                // carried by the "=" grip so the same handle both reorders the row (in-window)
                // and drops onto a monitor to add chs
                auto setMonitorPayload = [&](const VarEntry &v, bool isComplex, bool hasManualAggregate) {
                        if (isComplex) {
                                auto sp      = std::make_unique<StructChannelPayload>();
                                sp->writable = v.writable;
                                snprintf(sp->device, sizeof(sp->device), "%s", portName(v.port));
                                if (v.port == PortType::SHM)
                                        snprintf(sp->shmName, sizeof(sp->shmName), "%s", v.shm.name);
                                if (v.port == PortType::AUDIO)
                                        snprintf(sp->shmName, sizeof(sp->shmName), "%s", v.audio.deviceName);
                                snprintf(sp->rootName, sizeof(sp->rootName), "%s", v.name.c_str());
                                sp->rootAddr    = v.addr;
                                sp->rootTypeOff = v.typeOff;
                                if (hasManualAggregate) {
                                        for (const auto &sf : v.structFields) {
                                                if (sp->count >= StructChannelPayload::kMaxEntries)
                                                        break;
                                                auto &e = sp->entries[sp->count++];
                                                snprintf(e.name,
                                                         sizeof(e.name),
                                                         v.isArray ? "%s%s" : "%s.%s",
                                                         v.name.c_str(),
                                                         sf.name);
                                                e.addr = v.addr + sf.byteOffset;
                                                snprintf(e.type, sizeof(e.type), "%s", Parser::dataTypeToStr(sf.type));
                                                e.writable = v.writable;
                                                e.numEnums = 0;
                                                auto eit   = v.memberEnumDefs.find(e.name);
                                                if (eit != v.memberEnumDefs.end() && !eit->second.empty()) {
                                                        e.numEnums = (u8)std::min((int)eit->second.size(),
                                                                                  StructChannelPayload::kMaxEnums);
                                                        for (int k = 0; k < e.numEnums; ++k) {
                                                                snprintf(e.enums[k].name,
                                                                         sizeof(e.enums[k].name),
                                                                         "%s",
                                                                         eit->second[k].name.c_str());
                                                                e.enums[k].val = eit->second[k].val;
                                                        }
                                                }
                                        }
                                } else {
                                        std::lock_guard lk(mtxElf_);
                                        flattenForStructPayload(dwarfInfo_,
                                                                v.name,
                                                                v.addr,
                                                                v.typeOff,
                                                                *sp,
                                                                &v.memberEnumDefs,
                                                                8,
                                                                &v.memberUnitConversions);
                                }
                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", sp.get(), sizeof(*sp));
                        } else {
                                ChannelDropPayload p{};
                                p.writable = v.writable;
                                snprintf(p.name, sizeof(p.name), "%s", v.name.c_str());
                                p.addr = v.addr;
                                snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(v.type));
                                snprintf(p.device, sizeof(p.device), "%s", portName(v.port));
                                if (v.port == PortType::SHM)
                                        snprintf(p.shmName, sizeof(p.shmName), "%s", v.shm.name);
                                if (v.port == PortType::AUDIO)
                                        snprintf(p.shmName, sizeof(p.shmName), "%s", v.audio.deviceName);
                                p.numBytes       = (u8)Parser::typeBytes(v.type);
                                p.typeOff        = v.typeOff;
                                p.bitOffset      = v.bitOffset;
                                p.bitSize        = v.bitSize;
                                p.unitConversion = v.unitConversion;
                                {
                                        std::lock_guard lk(mtxElf_);
                                        fillEnumPayload(dwarfInfo_, v.typeOff, p, &v.enumDefs);
                                }
                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                        }
                };

                // A row reorder target: reorders when the active drag is an internal "=" grip
                // drag (rowDragSrc_ >= 0). Returns nothing; updates varMoveSrc/Dst
                auto acceptRowReorder = [&](int dstRow) {
                        if (rowDragSrc_ < 0)
                                return;
                        const ImGuiPayload *pl = ImGui::GetDragDropPayload();
                        if (!pl ||
                            !(pl->IsDataType("CHANNEL") || pl->IsDataType("STRUCT_CHANNEL") || pl->IsDataType("FLASH_ARRAY")))
                                return;
                        if (ImGui::AcceptDragDropPayload(pl->DataType)) {
                                varMoveSrc = rowDragSrc_;
                                varMoveDst = dstRow;
                        }
                };

                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
                const bool focused = drawWatchFocusTable();
                for (int i = 0; !focused && i < (int)vars_.size(); ++i) {
                        auto &v = vars_[i];
                        if (v.flashArray) {
                                if (drawFlashArrayRow(i, varMoveSrc, varMoveDst)) {
                                        vars_.erase(vars_.begin() + i);
                                        --i;
                                        prevSelectedIndex_ = -1;
                                        isModified_        = true;
                                }
                                continue;
                        }
                        ImGui::PushID(i);

                        // Detect a struct/array type up front — the "=" grip needs it to build
                        // the monitor drag payload
                        const dwarf::Type *t = nullptr;
                        {
                                std::lock_guard lk(mtxElf_);
                                t = resolveAlias(dwarfInfo_, v.typeOff);
                        }
                        const bool hasManualAggregate = v.isStruct && v.typeOff == 0;
                        const bool isComplex          = hasManualAggregate ||
                                               (t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                                      t->kind == dwarf::TypeKind::ARRAY));

                        if (isComplex)
                                ui::HierarchyGap();
                        ui::HierarchyNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ui::HierarchyCard structureCard;
                        if (isComplex)
                                structureCard.begin(0);

                        const ImGuiID rowContextId  = ImGui::GetID("##var_row_ctx");
                        bool          pendingDelete = false;
                        if (ImGui::BeginPopup("##var_row_ctx")) {
                                // Auto-select on right-click when nothing is selected
                                if (!v.selected) {
                                        for (auto &var : vars_)
                                                var.selected = false;
                                        v.selected = true;
                                }
                                drawWatchValueActions(i, v.name);
                                if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                        pendingDelete = true;
                                }
                                if (ImGui::MenuItem(tr("Edit Properties...", "编辑属性..."))) {
                                        editPropIdx_ = i;
                                        snprintf(editPropBuf_.name, sizeof(editPropBuf_.name), "%s", v.name.c_str());
                                        snprintf(editPropBuf_.addrBuf,
                                                 sizeof(editPropBuf_.addrBuf),
                                                 "%llX",
                                                 (unsigned long long)v.addr);
                                        editPropBuf_.type        = v.type;
                                        editPropBuf_.port        = v.port;
                                        editPropBuf_.writable    = v.writable;
                                        editPropBuf_.structMode  = v.isStruct;
                                        editPropBuf_.unionMode   = v.isUnion;
                                        editPropBuf_.arrayMode   = v.isArray;
                                        editPropBuf_.typeChanged = false;
                                        if (t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                                  t->kind == dwarf::TypeKind::ARRAY)) {
                                                editPropBuf_.structMode = true;
                                                editPropBuf_.unionMode  = t->kind == dwarf::TypeKind::UNION;
                                                editPropBuf_.arrayMode  = t->kind == dwarf::TypeKind::ARRAY;
                                        }
                                        editPropBuf_.structFields = v.structFields;
                                        if (v.port == PortType::SHM) {
                                                snprintf(editPropBuf_.shmName, sizeof(editPropBuf_.shmName), "%s", v.shm.name);
                                        }
                                        if (v.port == PortType::AUDIO) {
                                                editPropBuf_.audioDeviceIndex = v.audio.deviceIndex;
                                                snprintf(editPropBuf_.audioDeviceName,
                                                         sizeof(editPropBuf_.audioDeviceName),
                                                         "%s",
                                                         v.audio.deviceName);
                                        }
                                }
                                if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                        openRewrite(v);
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("Access mode", "读写模式"));
                                auto applyWritable = [&](bool writable) {
                                        for (auto &selected : vars_) {
                                                if (!selected.selected || (writable && selected.port == PortType::AUDIO))
                                                        continue;
                                                selected.writable = writable;
                                        }
                                        isModified_        = true;
                                        propertiesChanged_ = true;
                                };
                                bool accessWritable = v.writable;
                                if (ui::AccessChoice("##access", accessWritable, v.port != PortType::AUDIO))
                                        applyWritable(accessWritable);
                                ImGui::Separator();
                                const u32  traceSize = Parser::typeBytes(v.type);
                                const bool canTrace  = v.port == PortType::JLINK && !isComplex && traceSize > 0;
                                ImGui::BeginDisabled(!canTrace);
                                if (ImGui::MenuItem(canTrace
                                                        ? tr("Trace writes with DWT", "使用 DWT 跟踪写入")
                                                        : tr("DWT trace (JLINK scalar only)", "DWT 跟踪（仅 JLINK 标量）"))) {
                                        JLinkPort::instance().configureDwtWriteTrace(
                                            static_cast<u32>(v.addr), traceSize, v.name);
                                }
                                ImGui::EndDisabled();
                                ImGui::EndPopup();
                        }

                        // Popup is handled before TreeNodeEx so the ID stack matches
                        // OpenPopupOnItemClick (expanded structs push to the ID stack)
                        if (pendingDelete) {
                                ImGui::PopID();
                                for (int j = (int)vars_.size() - 1; j >= 0; --j)
                                        if (vars_[j].selected)
                                                vars_.erase(vars_.begin() + j);
                                isModified_        = true;
                                prevSelectedIndex_ = -1;
                                break;
                        }
                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                        if (isComplex)
                                ui::HierarchyRow(0);
                        if (ImGui::IsPopupOpen(rowContextId, ImGuiPopupFlags_None))
                                ui::HierarchySelection(true);
                        const bool wasOpenTop = isComplex && v.expandedMembers.count(v.name) > 0;
                        if (isComplex)
                                ImGui::SetNextItemOpen(wasOpenTop);
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        ui::HierarchyName();
                        const float labelWidth = ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing();
                        const float labelX     = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
                        bool        open       = ui::HierarchyNode(v.name.c_str(), nodeFlags);
                        markWatchSearchRow(i, v.name);
                        ImGui::PopStyleColor(2);
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                rowDragSrc_ = i;
                                setMonitorPayload(v, isComplex, hasManualAggregate);
                                ImGui::Text(tr("Dragging %s", "拖拽 %s"), v.name.c_str());
                                ImGui::EndDragDropSource();
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                                ImGui::OpenPopupEx(rowContextId, ImGuiPopupFlags_None);

                        ui::ClippedTextTooltip(v.name.c_str(), labelWidth);
                        if (isWatchAggregate(t) && ImGui::IsItemHovered()) {
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() &&
                                    ImGui::GetIO().MousePos.x >= labelX &&
                                    ImGui::GetIO().MousePos.x < labelX + ImGui::CalcTextSize(v.name.c_str(), nullptr, true).x)
                                        focusWatch(i, v.name);
                                ImGui::SetTooltip("%s\n%s",
                                                  v.name.c_str(),
                                                  tr("Double-click name to focus; click arrow to expand/collapse",
                                                     "双击名称聚焦；单击箭头展开/折叠"));
                        }
                        // The name cell is both a drag source and a reorder target.
                        if (ImGui::BeginDragDropTarget()) {
                                acceptRowReorder(i);
                                ImGui::EndDragDropTarget();
                        }
                        if (isComplex && open != wasOpenTop) {
                                if (open)
                                        v.expandedMembers.insert(v.name);
                                else
                                        v.expandedMembers.erase(v.name);
                                isModified_ = true;
                        }

                        // Left click never selects a row. Selection remains available
                        // only as the target context for right-click bulk actions.
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                                for (auto &var : vars_)
                                        var.selected = false;
                                prevSelectedIndex_ = -1;
                        }

                        // Dragging the name above also adds this row to a monitor.

                        // Value
                        ImGui::TableSetColumnIndex(1);
                        const bool disconnectedJLink = v.port == PortType::JLINK && !JLinkPort::instance().isConnected();
                        const std::string &displayValue =
                            disconnectedJLink && !v.prevGoodValueStr.empty() ? v.prevGoodValueStr : v.valueStr;
                        if (isWatchAggregate(t)) {
                                drawWatchFocusButton(i, v.name);
                        } else if (t && t->kind == dwarf::TypeKind::POINTER) {
                                drawPointerButton(i, v.addr, v.typeOff, v.name);
                        } else {
                                valueFlash_.cell(v.name + "#" + std::to_string(variableRadix(v)), displayValue);
                                ui::TableText(displayValue);
                        }

                        // Type
                        ImGui::TableSetColumnIndex(2);
                        if (hasManualAggregate) {
                                ui::TableText(v.isArray ? "array" : (v.isUnion ? "union" : "struct"));
                        } else if (isComplex) {
                                ui::TableText(t->kind == dwarf::TypeKind::ARRAY
                                                  ? "array"
                                                  : (t->kind == dwarf::TypeKind::UNION ? "union" : "struct"));
                        } else if ((t && t->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty()) {
                                const char *storage =
                                    t ? scalarPayloadType(dwarfInfo_, v.typeOff) : Parser::dataTypeToStr(v.type);
                                ui::TableText(std::string("enum (") + (storage ? storage : Parser::dataTypeToStr(v.type)) +
                                              ")");
                        } else if (t && t->kind == dwarf::TypeKind::POINTER) {
                                ui::TableText(prettyType(dwarfInfo_, v.typeOff));
                        } else {
                                ui::TableText(Parser::dataTypeToStr(v.type));
                        }

                        // Address
                        ImGui::TableSetColumnIndex(3);
                        if (v.port == PortType::LOCAL) {
                                ImGui::TextDisabled("(local)");
                        } else if (v.port == PortType::SHM) {
                                ui::TableText(v.shm.name);
                        } else if (v.port == PortType::AUDIO) {
                                ui::TableText(v.audio.deviceName[0]
                                                  ? v.audio.deviceName
                                                  : AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        } else if (v.addrUnknown) {
                                ui::ErrorText("unknown");
                        } else {
                                watchAddress(v.addr);
                        }

                        // Port
                        ImGui::TableSetColumnIndex(4);
                        ui::TableText(portName(v.port));

                        // R/W
                        ImGui::TableSetColumnIndex(5);
                        const bool hasFunction = !v.readFunction.empty() || !v.writeFunction.empty();
                        if (hasFunction)
                                ImGui::TextColored(
                                    v.functionError.empty() ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f) : ui::ErrorTextColor(), "FN");
                        else if (v.writable)
                                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                        else
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");
                        if (hasFunction && ImGui::IsItemHovered()) {
                                if (!v.functionError.empty())
                                        ImGui::SetTooltip("%s", v.functionError.c_str());
                                else
                                        ImGui::SetTooltip("R: %s\nW: %s",
                                                          v.readFunction.empty() ? "-" : v.readFunction.functionName.c_str(),
                                                          v.writeFunction.empty() ? "-" : v.writeFunction.functionName.c_str());
                        }

                        if (open && isComplex) {
                                if (hasManualAggregate) {
                                        // Render manually defined struct fields from LOCAL buffer
                                        int                          fieldMoveSrc = -1, fieldMoveDst = -1;
                                        std::unique_lock<std::mutex> lk(mtxLocal_);
                                        auto                         it = localBufs_.find(v.name);
                                        for (int fi = 0; fi < (int)v.structFields.size(); ++fi) {
                                                const auto &sf = v.structFields[fi];
                                                ImGui::PushID(fi);
                                                ui::HierarchyNextRow();
                                                ui::HierarchyRow(0);
                                                ImGui::TableSetColumnIndex(0);
                                                // Suppress the hover/active highlight on struct sub-items
                                                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                                                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                                                ui::HierarchyNode(sf.name,
                                                                  ImGuiTreeNodeFlags_Leaf |
                                                                      ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                      ImGuiTreeNodeFlags_SpanFullWidth);
                                                ImGui::PopStyleColor(2);
                                                markWatchSearchRow(i, v.name + (v.isArray ? "" : ".") + sf.name);
                                                if (ImGui::BeginDragDropSource()) {
                                                        fieldDragParent_ = i;
                                                        fieldDragSrc_    = fi;
                                                        ChannelDropPayload p{};
                                                        p.writable = v.writable;
                                                        snprintf(p.name,
                                                                 sizeof(p.name),
                                                                 v.isArray ? "%s%s" : "%s.%s",
                                                                 v.name.c_str(),
                                                                 sf.name);
                                                        p.addr = v.addr + sf.byteOffset;
                                                        snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(sf.type));
                                                        snprintf(p.device, sizeof(p.device), "%s", portName(v.port));
                                                        if (v.port == PortType::SHM)
                                                                snprintf(p.shmName, sizeof(p.shmName), "%s", v.shm.name);
                                                        p.numBytes = (u8)Parser::typeBytes(sf.type);
                                                        if (const auto conversion = v.memberUnitConversions.find(p.name);
                                                            conversion != v.memberUnitConversions.end())
                                                                p.unitConversion = conversion->second;
                                                        auto eit = v.memberEnumDefs.find(p.name);
                                                        if (eit != v.memberEnumDefs.end() && !eit->second.empty()) {
                                                                p.numEnums = (u8)std::min((int)eit->second.size(),
                                                                                          ChannelDropPayload::kMaxEnums);
                                                                for (int k = 0; k < p.numEnums; ++k) {
                                                                        snprintf(p.enums[k].name,
                                                                                 sizeof(p.enums[k].name),
                                                                                 "%s",
                                                                                 eit->second[k].name.c_str());
                                                                        p.enums[k].val = eit->second[k].val;
                                                                }
                                                        }
                                                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), p.name);
                                                        ImGui::EndDragDropSource();
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Drag: reorder / add to monitor",
                                                                             "拖动：调整顺序 / 添加到监视器"));
                                                if (ImGui::BeginDragDropTarget()) {
                                                        if (fieldDragParent_ == i && fieldDragSrc_ >= 0) {
                                                                const ImGuiPayload *pl = ImGui::GetDragDropPayload();
                                                                if (pl && pl->IsDataType("CHANNEL") &&
                                                                    ImGui::AcceptDragDropPayload("CHANNEL")) {
                                                                        fieldMoveSrc = fieldDragSrc_;
                                                                        fieldMoveDst = fi;
                                                                }
                                                        }
                                                        ImGui::EndDragDropTarget();
                                                }

                                                if (ui::HierarchyContextItem()) {
                                                        drawWatchValueActions(i, v.name + "." + sf.name);
                                                        if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                                                openRewriteMember(i,
                                                                                  v.addr + sf.byteOffset,
                                                                                  0,
                                                                                  v.name + "." + sf.name,
                                                                                  sf.type);
                                                        ImGui::EndPopup();
                                                }
                                                ImGui::TableSetColumnIndex(1);
                                                uint8_t   tmp[8]{};
                                                const u32 fsz        = Parser::typeBytes(sf.type);
                                                bool      valueReady = false;
                                                if (v.port == PortType::LOCAL && it != localBufs_.end() &&
                                                    sf.byteOffset + fsz <= (u32)it->second.size()) {
                                                        std::memcpy(tmp, it->second.data() + sf.byteOffset, fsz);
                                                        valueReady = true;
                                                } else if (v.port == PortType::JLINK) {
                                                        auto pit = syncReadCache_.find(v.addr + sf.byteOffset);
                                                        if (pit != syncReadCache_.end() && pit->second.ok) {
                                                                std::memcpy(tmp, pit->second.buf, fsz);
                                                                valueReady = true;
                                                        }
                                                } else if (v.port == PortType::SHM) {
                                                        auto sit = shmShadowBufs_.find(shmShadowKey(v));
                                                        if (sit != shmShadowBufs_.end() &&
                                                            sf.byteOffset + fsz <= (u32)sit->second.size()) {
                                                                std::memcpy(tmp, sit->second.data() + sf.byteOffset, fsz);
                                                                valueReady = true;
                                                        }
                                                }
                                                if (valueReady)
                                                        ui::TableText(
                                                            decodeValue(
                                                                tmp, sf.type, 0, 0, v.radixForPath(v.name + "." + sf.name))
                                                                .c_str());
                                                else
                                                        ImGui::TextDisabled("---");
                                                ImGui::TableSetColumnIndex(2);
                                                ui::TableText(Parser::dataTypeToStr(sf.type));
                                                ImGui::TableSetColumnIndex(3);
                                                ImGui::Text("+0x%X", sf.byteOffset);
                                                ImGui::PopID();
                                        }
                                        lk.unlock();
                                        // Apply a deferred field reorder (struct sub-item grip). Reorders the
                                        // display order; byte offsets are unchanged
                                        if (fieldMoveSrc >= 0 && fieldMoveDst >= 0 && fieldMoveSrc != fieldMoveDst &&
                                            fieldMoveSrc < (int)v.structFields.size() &&
                                            fieldMoveDst < (int)v.structFields.size()) {
                                                auto moved = v.structFields[fieldMoveSrc];
                                                v.structFields.erase(v.structFields.begin() + fieldMoveSrc);
                                                // Keep the original destination index after erase. For a downward
                                                // move this inserts after the drop row, allowing adjacent rows to swap
                                                int dst = fieldMoveDst;
                                                v.structFields.insert(v.structFields.begin() + dst, moved);
                                                isModified_ = true;
                                        }
                                } else {
                                        std::lock_guard lk(mtxElf_);
                                        drawVarVarTreeRow(v.name, v.addr, v.typeOff, 1, v.port, v.shm.name, i);
                                }
                                ImGui::TreePop();
                        }

                        ImGui::PopID();
                }

                // Apply a deferred row reorder (drag grip). Move the dragged entry so
                // it lands at the drop row's position
                if (varMoveSrc >= 0 && varMoveDst >= 0 && varMoveSrc != varMoveDst && varMoveSrc < (int)vars_.size() &&
                    varMoveDst < (int)vars_.size()) {
                        VarEntry moved = std::move(vars_[varMoveSrc]);
                        vars_.erase(vars_.begin() + varMoveSrc);
                        // As above, the original destination index is the correct
                        // insertion point for an adjacent downward move
                        int dst = varMoveDst;
                        vars_.insert(vars_.begin() + dst, std::move(moved));
                        prevSelectedIndex_ = dst;
                        isModified_        = true;
                }

                ImGui::PopStyleVar();
                captureTableWidths(watchTableUi_, 6);
                tableCard.end();
        }

        // Only accept "add to watch list" drops from outside (symbol browser). An internal
        // "=" grip drag (row or struct field) is a reorder/monitor drag — never re-add it here
        if (rowDragSrc_ < 0 && fieldDragParent_ < 0 && ImGui::BeginDragDropTarget()) {
                if (const auto *payload = ImGui::AcceptDragDropPayload("FLASH_ARRAY")) {
                        if (payload->DataSize == sizeof(uint64_t)) {
                                uint64_t id;
                                memcpy(&id, payload->Data, sizeof(id));
                                const auto &drag = flasharray::drag();
                                if (id == drag.id)
                                        addFlashArray(drag.snapshot);
                        }
                }
                // A struct/array dragged from the symbol browser arrives as a
                // STRUCT_CHANNEL (flattened for the monitor); here we re-add it as a
                // single expandable watch-list entry using the carried root metadata
                if (const ImGuiPayload *sPayload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                        if (sPayload->DataSize == sizeof(StructChannelPayload))
                                addStructToWatch(*static_cast<const StructChannelPayload *>(sPayload->Data));
                }

                const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
                if (!payload)
                        payload = ImGui::AcceptDragDropPayload("CHANNEL");
                if (payload)
                        addScalarToWatch(*static_cast<const ChannelDropPayload *>(payload->Data));

                ImGui::EndDragDropTarget();
        }
}

bool
Variable::watchHasName(const std::string &name) const
{
        for (const auto &v : vars_)
                if (v.name == name)
                        return true;
        return false;
}

bool
Variable::watchHasIdentity(const std::string &name, u64 address, DataType type) const
{
        return std::any_of(vars_.begin(), vars_.end(), [&](const VarEntry &variable) {
                return variable.name == name && variable.addr == address && variable.type == type;
        });
}

void
Variable::addScalarToWatch(const ChannelDropPayload &p)
{
        const DataType type = (strcmp(p.type, "struct") == 0 || strcmp(p.type, "union") == 0 || strcmp(p.type, "array") == 0)
                                  ? DataType::U32
                                  : Parser::strToDataType(p.type);
        if (watchHasIdentity(p.name, p.addr, type))
                return;
        VarEntry v;
        v.name      = p.name;
        v.addr      = p.addr;
        v.writable  = true;
        v.typeOff   = p.typeOff;
        v.bitOffset = p.bitOffset;
        v.bitSize   = p.bitSize;

        if (strcmp(p.device, "SHM") == 0) {
                v.port = PortType::SHM;
                snprintf(v.shm.name, sizeof(v.shm.name), "%s", p.name);
        } else if (strcmp(p.device, "AUDIO") == 0) {
                v.port              = PortType::AUDIO;
                v.writable          = false;
                v.audio.deviceIndex = static_cast<int>(p.addr);
                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", p.shmName);
        } else if (strcmp(p.device, "LOCAL") == 0 || strcmp(p.device, "MANUAL") == 0) {
                v.port     = PortType::LOCAL;
                v.writable = false; // Manual/Bin data is usually readonly in this context
        } else {
                v.port = PortType::JLINK;
        }

        if (strcmp(p.type, "struct") == 0 || strcmp(p.type, "union") == 0 || strcmp(p.type, "array") == 0) {
                v.type     = DataType::U32; // Placeholder; tree uses typeOff
                v.isStruct = true;
                v.isUnion  = strcmp(p.type, "union") == 0;
                v.isArray  = strcmp(p.type, "array") == 0;
        } else
                v.type = Parser::strToDataType(p.type);

        vars_.push_back(v);
        if (v.port == PortType::LOCAL) {
                std::lock_guard<std::mutex> lk(mtxLocal_);
                localBufs_.emplace(v.name, std::vector<uint8_t>(std::max<u32>(1, Parser::typeBytes(v.type)), 0));
        }
        isModified_ = true;
}

void
Variable::addStructToWatch(const StructChannelPayload &s)
{
        if (s.rootName[0] == '\0' || watchHasIdentity(s.rootName, s.rootAddr, DataType::U32))
                return;
        VarEntry v;
        v.name     = s.rootName;
        v.addr     = s.rootAddr;
        v.typeOff  = s.rootTypeOff;
        v.writable = true;
        v.type     = DataType::U32; // placeholder; tree uses typeOff
        if (const dwarf::Type *type = resolveAlias(dwarfInfo_, v.typeOff)) {
                v.isStruct = type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION ||
                             type->kind == dwarf::TypeKind::ARRAY;
                v.isUnion = type->kind == dwarf::TypeKind::UNION;
                v.isArray = type->kind == dwarf::TypeKind::ARRAY;
        }
        if (strcmp(s.device, "SHM") == 0) {
                v.port = PortType::SHM;
                snprintf(v.shm.name, sizeof(v.shm.name), "%s", s.shmName[0] != '\0' ? s.shmName : s.rootName);
        } else if (strcmp(s.device, "AUDIO") == 0) {
                v.port              = PortType::AUDIO;
                v.writable          = false;
                v.audio.deviceIndex = static_cast<int>(s.rootAddr);
                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", s.shmName);
        } else if (strcmp(s.device, "LOCAL") == 0 || strcmp(s.device, "MANUAL") == 0) {
                v.port     = PortType::LOCAL;
                v.writable = false;
        } else {
                v.port = PortType::JLINK;
        }
        vars_.push_back(v);
        isModified_ = true;
}

void
Variable::mirrorFromMonitorDrop(const WatchMirrorRequest &req)
{
        if (req.isStruct)
                addStructToWatch(req.group);
        else
                addScalarToWatch(req.scalar);
}

void
Variable::drawAddVariableDialog()
{
        if (state_ != WindowState::AddVariable)
                return;
        ImGui::OpenPopup("###AddNewVariable");
        if (ImGui::BeginPopupModal(tr("Add New Variable###AddNewVariable", "添加新变量###AddNewVariable"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("##newVarName", newVar_.name, sizeof(newVar_.name));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Name", "名称"));
                static const char    *ports[]   = {"JLINK", "SHM", "LOCAL", "AUDIO"};
                static const PortType portMap[] = {PortType::JLINK, PortType::SHM, PortType::LOCAL, PortType::AUDIO};
                static int            portIdx   = 0;
                if (ui::Choice("##newVarPort", &portIdx, ports, IM_ARRAYSIZE(ports))) {
                        newVar_.port = portMap[portIdx];
                        if (newVar_.port == PortType::AUDIO) {
                                newVar_.type             = DataType::F32;
                                newVar_.writable         = false;
                                newVar_.addr             = (u64)AudioInput::instance().defaultDeviceIndex();
                                newVar_.audioDeviceIndex = static_cast<int>(newVar_.addr);
                                newVar_.structMode       = false;
                                newVar_.unionMode        = false;
                                newVar_.arrayMode        = false;
                                std::string devName      = AudioInput::instance().deviceName(newVar_.audioDeviceIndex);
                                snprintf(newVar_.audioDeviceName, sizeof(newVar_.audioDeviceName), "%s", devName.c_str());
                        }
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Port", "端口"));

                if (newVar_.port == PortType::AUDIO) {
                        newVar_.type = DataType::F32;
                        ImGui::TextDisabled("%s", tr("F32 normalized PCM sample (-1..1)", "F32 归一化 PCM 采样值 (-1..1)"));
                } else {
                        static const char    *types[]          = {"u8",
                                                                  "u16",
                                                                  "u32",
                                                                  "u64",
                                                                  "i8",
                                                                  "i16",
                                                                  "i32",
                                                                  "i64",
                                                                  "f16",
                                                                  "f32",
                                                                  "f64",
                                                                  "struct",
                                                                  "union",
                                                                  "array"};
                        static const DataType typeVals[]       = {DataType::U8,
                                                                  DataType::U16,
                                                                  DataType::U32,
                                                                  DataType::U64,
                                                                  DataType::I8,
                                                                  DataType::I16,
                                                                  DataType::I32,
                                                                  DataType::I64,
                                                                  DataType::F16,
                                                                  DataType::F32,
                                                                  DataType::F64};
                        constexpr int         kScalarTypeCount = IM_ARRAYSIZE(typeVals);
                        int                   typeIdx          = 2;
                        if (newVar_.structMode)
                                typeIdx = newVar_.arrayMode ? 13 : (newVar_.unionMode ? 12 : 11);
                        else {
                                const char *curName = Parser::dataTypeToStr(newVar_.type);
                                for (int k = 0; k < kScalarTypeCount; ++k)
                                        if (strcmp(types[k], curName) == 0) {
                                                typeIdx = k;
                                                break;
                                        }
                        }
                        if (ui::Choice("##newVarType", &typeIdx, types, IM_ARRAYSIZE(types))) {
                                if (typeIdx < kScalarTypeCount) {
                                        newVar_.type       = typeVals[typeIdx];
                                        newVar_.structMode = false;
                                        newVar_.unionMode  = false;
                                        newVar_.arrayMode  = false;
                                } else {
                                        newVar_.structMode = true;
                                        newVar_.unionMode  = typeIdx == 12;
                                        newVar_.arrayMode  = typeIdx == 13;
                                }
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Type", "类型"));
                }

                if (newVar_.port == PortType::JLINK) {
                        ImGui::InputText("##newVarAddress", newVar_.addrBuf, sizeof(newVar_.addrBuf));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Address (Hex)", "地址（十六进制）"));
                        try {
                                newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16);
                        } catch (...) {
                                newVar_.addr = 0;
                        }
                } else if (newVar_.port == PortType::SHM) {
                        ImGui::InputText("##newVarShmName", newVar_.shmName, sizeof(newVar_.shmName));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("SHM Name", "共享内存名称"));
                        ImGui::InputText("##newVarOffset", newVar_.addrBuf, sizeof(newVar_.addrBuf));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Offset", "偏移"));
                        try {
                                newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16);
                        } catch (...) {
                                newVar_.addr = 0;
                        }
                } else if (newVar_.port == PortType::AUDIO) {
                        drawAudioDeviceCombo(
                            newVar_.audioDeviceIndex, newVar_.audioDeviceName, sizeof(newVar_.audioDeviceName));
                        newVar_.addr = (u64)std::max(0, newVar_.audioDeviceIndex);
                } else if (newVar_.port == PortType::LOCAL) {
                        if (!newVar_.structMode) {
                                int localSz = (newVar_.addr > 0) ? (int)newVar_.addr : 8;
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::InputInt(tr("Size (bytes)##lsz", "大小(字节)##lsz"), &localSz)) {
                                        if (localSz < 1)
                                                localSz = 1;
                                        if (localSz > 4096)
                                                localSz = 4096;
                                        newVar_.addr = (u64)localSz;
                                }
                        } else {
                                // ── Manual aggregate member/element editor ──
                                static const char *sfTypes[] = {
                                    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f16", "f32", "f64"};
                                static const DataType sfTypeVals[] = {DataType::U8,
                                                                      DataType::U16,
                                                                      DataType::U32,
                                                                      DataType::U64,
                                                                      DataType::I8,
                                                                      DataType::I16,
                                                                      DataType::I32,
                                                                      DataType::I64,
                                                                      DataType::F16,
                                                                      DataType::F32,
                                                                      DataType::F64};
                                constexpr int         kSfTypeCount = IM_ARRAYSIZE(sfTypeVals);

                                constexpr ImGuiTableFlags tfl =
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
                                if (ui::TableIsland tableCard; tableCard.begin("##sftbl", 4, tfl, ImVec2(0, 120))) {
                                        ImGui::TableSetupColumn(newVar_.arrayMode ? tr("Element", "元素") : tr("Name", "名称"),
                                                                ImGuiTableColumnFlags_WidthStretch);
                                        ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                        ImGui::TableSetupColumn(tr("Offset", "偏移"), ImGuiTableColumnFlags_WidthFixed, 48.0f);
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
                                        ImGui::TableHeadersRow();

                                        int toDelete = -1;
                                        for (int fi = 0; fi < (int)newVar_.structFields.size(); ++fi) {
                                                auto &sf = newVar_.structFields[fi];
                                                ImGui::PushID(fi);
                                                ui::HierarchyNextRow();

                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                ImGui::InputText("##fn", sf.name, sizeof(sf.name));

                                                ImGui::TableSetColumnIndex(1);
                                                int sfIdx = 2;
                                                for (int k = 0; k < kSfTypeCount; ++k)
                                                        if (sfTypeVals[k] == sf.type) {
                                                                sfIdx = k;
                                                                break;
                                                        }
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ui::Choice("##ft", &sfIdx, sfTypes, kSfTypeCount))
                                                        sf.type = sfTypeVals[sfIdx];

                                                ImGui::TableSetColumnIndex(2);
                                                int off = (int)sf.byteOffset;
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::InputInt("##fo", &off, 0, 0)) {
                                                        if (off < 0)
                                                                off = 0;
                                                        sf.byteOffset = (u32)off;
                                                }

                                                ImGui::TableSetColumnIndex(3);
                                                if (ImGui::SmallButton("X"))
                                                        toDelete = fi;

                                                ImGui::PopID();
                                        }
                                        if (toDelete >= 0)
                                                newVar_.structFields.erase(newVar_.structFields.begin() + toDelete);
                                        tableCard.end();
                                }

                                if (ImGui::Button(newVar_.arrayMode ? tr("+ Element", "+ 添加元素")
                                                                    : tr("+ Field", "+ 添加字段"))) {
                                        VarEntry::StructField sf{};
                                        if (!newVar_.unionMode && !newVar_.structFields.empty()) {
                                                const auto &prev = newVar_.structFields.back();
                                                sf.byteOffset    = prev.byteOffset + Parser::typeBytes(prev.type);
                                        }
                                        if (newVar_.arrayMode)
                                                snprintf(sf.name, sizeof(sf.name), "[%d]", (int)newVar_.structFields.size());
                                        else
                                                snprintf(sf.name, sizeof(sf.name), "field%d", (int)newVar_.structFields.size());
                                        newVar_.structFields.push_back(sf);
                                }
                        }
                }

                if (newVar_.port == PortType::AUDIO) {
                        newVar_.writable = false;
                        ImGui::TextDisabled("%s", tr("Read-only", "只读"));
                } else {
                        ui::AccessChoice(tr("Access mode", "读写模式"), newVar_.writable);
                }

                if (ImGui::Button("OK", ImVec2(120, 0))) {
                        VarEntry v;
                        v.name     = newVar_.name;
                        v.type     = newVar_.type;
                        v.port     = newVar_.port;
                        v.addr     = newVar_.addr;
                        v.writable = newVar_.writable;
                        if (newVar_.structMode) {
                                v.structFields = newVar_.structFields;
                                v.isStruct     = true;
                                v.isUnion      = newVar_.unionMode;
                                v.isArray      = newVar_.arrayMode;
                        }
                        if (v.port == PortType::SHM) {
                                snprintf(v.shm.name, sizeof(v.shm.name), "%s", newVar_.shmName);
                                v.shm.inited = false;
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type              = DataType::F32;
                                v.addr              = (u64)std::max(0, newVar_.audioDeviceIndex);
                                v.audio.deviceIndex = newVar_.audioDeviceIndex;
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", newVar_.audioDeviceName);
                        }
                        if (watchHasIdentity(v.name, v.addr, v.type)) {
                                ImGui::InsertNotification({ImGuiToastType::Info,
                                                           1800,
                                                           tr("%s is already in the variable list", "%s 已在变量列表中"),
                                                           v.name.c_str()});
                        } else {
                                if (v.port == PortType::LOCAL) {
                                        size_t bsz;
                                        if (newVar_.structMode) {
                                                bsz = 0;
                                                for (const auto &sf : v.structFields)
                                                        bsz =
                                                            std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
                                                if (bsz == 0)
                                                        bsz = 8;
                                        } else {
                                                bsz = (v.addr > 0) ? (size_t)v.addr : 8;
                                        }
                                        std::lock_guard<std::mutex> lk(mtxLocal_);
                                        localBufs_.emplace(v.name, std::vector<uint8_t>(bsz, 0));
                                }
                                vars_.push_back(v);
                                isModified_        = true;
                                propertiesChanged_ = true;
                        }
                        state_             = WindowState::None;
                        newVar_.structMode = false;
                        newVar_.unionMode  = false;
                        newVar_.arrayMode  = false;
                        newVar_.structFields.clear();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(120, 0))) {
                        state_             = WindowState::None;
                        newVar_.structMode = false;
                        newVar_.unionMode  = false;
                        newVar_.arrayMode  = false;
                        newVar_.structFields.clear();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::drawEnumEditPopup()
{
        if (enumEditIdx_ < 0 || enumEditIdx_ >= (int)vars_.size()) {
                enumEditIdx_ = -1;
                return;
        }
        ImGui::OpenPopup("###EditEnumDef");
        if (ImGui::BeginPopupModal(tr("Edit Enum Definition###EditEnumDef", "编辑枚举定义###EditEnumDef"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                VarEntry &v = vars_[enumEditIdx_];

                // Pre-populate from DWARF on first open (when user hasn't set anything yet)
                if (v.enumDefs.empty()) {
                        std::lock_guard    lk(mtxElf_);
                        const dwarf::Type *et = resolveAlias(dwarfInfo_, v.typeOff);
                        if (et && et->kind == dwarf::TypeKind::ENUM) {
                                for (const auto &e : et->enums)
                                        v.enumDefs.push_back({e.name, e.val});
                        }
                }

                ImGui::Text(tr("Variable: %s", "变量: %s"), v.name.c_str());
                ImGui::Separator();

                constexpr ImGuiTableFlags tfl =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ui::TableIsland tableCard; tableCard.begin("EnumDefTable", 3, tfl, ImVec2(400, 200))) {
                        ImGui::TableSetupColumn(tr("Label###col_label", "标签###col_label"),
                                                ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Value###col_enumval", "数值###col_enumval"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                        ImGui::TableHeadersRow();

                        int deleteIdx = -1;
                        for (int j = 0; j < (int)v.enumDefs.size(); ++j) {
                                ImGui::PushID(j);
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::SetNextItemWidth(-1);
                                char nameBuf[64];
                                snprintf(nameBuf, sizeof(nameBuf), "%s", v.enumDefs[j].name.c_str());
                                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                                        v.enumDefs[j].name = nameBuf;
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1);
                                int ival = (int)v.enumDefs[j].val;
                                if (ImGui::InputInt("##val", &ival, 0, 0))
                                        v.enumDefs[j].val = ival;
                                ImGui::TableSetColumnIndex(2);
                                if (ui::SmallButton("x", ui::BtnStyle::Danger))
                                        deleteIdx = j;
                                ImGui::PopID();
                        }
                        if (deleteIdx >= 0)
                                v.enumDefs.erase(v.enumDefs.begin() + deleteIdx);
                        tableCard.end();
                }

                if (ImGui::Button(tr("+ Add", "+ 添加")))
                        v.enumDefs.push_back({"", 0});
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear All", "全部清空")))
                        v.enumDefs.clear();
                ImGui::SameLine(0, 40);
                if (ImGui::Button(tr("Close", "关闭"), ImVec2(80, 0))) {
                        isModified_  = true;
                        enumEditIdx_ = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::drawSubEnumEditPopup()
{
        if (enumSubEditParentIdx_ < 0 || enumSubEditParentIdx_ >= (int)vars_.size()) {
                enumSubEditParentIdx_ = -1;
                return;
        }
        ImGui::OpenPopup("###EditMemberEnumDef");
        if (ImGui::BeginPopupModal(
                tr("Edit Member Enum Definition###EditMemberEnumDef", "编辑成员枚举定义###EditMemberEnumDef"),
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
                VarEntry &parent = vars_[enumSubEditParentIdx_];
                auto     &defs   = parent.memberEnumDefs[enumSubEditMemberPath_];

                if (defs.empty()) {
                        std::lock_guard    lk(mtxElf_);
                        const dwarf::Type *et = resolveAlias(dwarfInfo_, enumSubEditMemberTypeOff_);
                        if (et && et->kind == dwarf::TypeKind::ENUM)
                                for (const auto &e : et->enums)
                                        defs.push_back({e.name, e.val});
                }

                ImGui::Text(tr("Member: %s", "成员: %s"), enumSubEditMemberPath_.c_str());
                ImGui::Separator();

                constexpr ImGuiTableFlags tfl =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ui::TableIsland tableCard; tableCard.begin("SubEnumDefTable", 3, tfl, ImVec2(400, 200))) {
                        ImGui::TableSetupColumn(tr("Label###col_label", "标签###col_label"),
                                                ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Value###col_enumval", "数值###col_enumval"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                        ImGui::TableHeadersRow();

                        int deleteIdx = -1;
                        for (int j = 0; j < (int)defs.size(); ++j) {
                                ImGui::PushID(j);
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::SetNextItemWidth(-1);
                                char nameBuf[64];
                                snprintf(nameBuf, sizeof(nameBuf), "%s", defs[j].name.c_str());
                                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                                        defs[j].name = nameBuf;
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1);
                                int ival = (int)defs[j].val;
                                if (ImGui::InputInt("##val", &ival, 0, 0))
                                        defs[j].val = ival;
                                ImGui::TableSetColumnIndex(2);
                                if (ui::SmallButton("x", ui::BtnStyle::Danger))
                                        deleteIdx = j;
                                ImGui::PopID();
                        }
                        if (deleteIdx >= 0)
                                defs.erase(defs.begin() + deleteIdx);
                        tableCard.end();
                }

                if (ImGui::Button(tr("+ Add", "+ 添加")))
                        defs.push_back({"", 0});
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear All", "全部清空")))
                        defs.clear();
                ImGui::SameLine(0, 40);
                if (ImGui::Button(tr("Close", "关闭"), ImVec2(80, 0))) {
                        isModified_           = true;
                        enumSubEditParentIdx_ = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::beginMemberProperties(i32                parentIdx,
                                const std::string &path,
                                u64                defaultAddr,
                                DataType           defaultType,
                                bool               scalar,
                                bool               defaultWritable,
                                dwarf::TypeKind    kind)
{
        if (parentIdx < 0 || parentIdx >= (int)vars_.size())
                return;
        memberPropEdit_               = {};
        memberPropEdit_.parentIdx     = parentIdx;
        memberPropEdit_.path          = path;
        memberPropEdit_.defaultAddr   = defaultAddr;
        memberPropEdit_.aggregateKind = scalar ? dwarf::TypeKind::UNKNOWN : kind;
        editPropIdx_                  = parentIdx;
        editPropBuf_                  = {};
        auto &parent                  = vars_[parentIdx];
        snprintf(editPropBuf_.name, sizeof(editPropBuf_.name), "%s", path.c_str());
        editPropBuf_.type       = defaultType;
        editPropBuf_.port       = parent.port;
        editPropBuf_.writable   = defaultWritable;
        editPropBuf_.structMode = !scalar;
        editPropBuf_.unionMode  = kind == dwarf::TypeKind::UNION;
        editPropBuf_.arrayMode  = kind == dwarf::TypeKind::ARRAY;
        snprintf(editPropBuf_.shmName, sizeof(editPropBuf_.shmName), "%s", parent.shm.name);
        editPropBuf_.audioDeviceIndex = parent.audio.deviceIndex;
        snprintf(editPropBuf_.audioDeviceName, sizeof(editPropBuf_.audioDeviceName), "%s", parent.audio.deviceName);
        auto it = parent.memberOverrides.find(path);
        if (it != parent.memberOverrides.end()) {
                const auto &ov = it->second;
                if (!ov.alias.empty())
                        snprintf(editPropBuf_.name, sizeof(editPropBuf_.name), "%s", ov.alias.c_str());
                if (ov.hasAddress)
                        defaultAddr = ov.address;
                if (ov.hasType) {
                        editPropBuf_.type       = ov.type;
                        editPropBuf_.structMode = editPropBuf_.unionMode = editPropBuf_.arrayMode = false;
                }
                if (ov.hasWritable)
                        editPropBuf_.writable = ov.writable;
        }
        snprintf(editPropBuf_.addrBuf, sizeof(editPropBuf_.addrBuf), "%llX", (unsigned long long)defaultAddr);
}

void
Variable::drawEditPropertiesPopup()
{
        if (editPropIdx_ < 0 || editPropIdx_ >= (int)vars_.size()) {
                editPropIdx_              = -1;
                memberPropEdit_.parentIdx = -1;
                return;
        }
        const bool member = memberPropEdit_.parentIdx >= 0;
        ImGui::OpenPopup("###EditVarProperties");
        if (ImGui::BeginPopupModal(tr("Edit Variable Properties###EditVarProperties", "编辑变量属性###EditVarProperties"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {

                ImGui::Text("%s", tr("Name", "名称"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("##editPropName", editPropBuf_.name, sizeof(editPropBuf_.name));

                ImGui::Text("%s", tr("Type", "类型"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                static const char *types[] = {
                    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f16", "f32", "f64", "struct", "union", "array"};
                static const DataType typeVals[]      = {DataType::U8,
                                                         DataType::U16,
                                                         DataType::U32,
                                                         DataType::U64,
                                                         DataType::I8,
                                                         DataType::I16,
                                                         DataType::I32,
                                                         DataType::I64,
                                                         DataType::F16,
                                                         DataType::F32,
                                                         DataType::F64};
                constexpr int         scalarTypeCount = IM_ARRAYSIZE(typeVals);
                int typeIdx = editPropBuf_.structMode ? (editPropBuf_.arrayMode ? 13 : (editPropBuf_.unionMode ? 12 : 11)) : 2;
                if (!editPropBuf_.structMode) {
                        const char *curName = Parser::dataTypeToStr(editPropBuf_.type);
                        for (int k = 0; k < scalarTypeCount; ++k)
                                if (strcmp(types[k], curName) == 0) {
                                        typeIdx = k;
                                        break;
                                }
                }
                bool allowedTypes[IM_ARRAYSIZE(types)];
                for (int k = 0; k < IM_ARRAYSIZE(types); ++k) {
                        const auto kind = memberPropEdit_.aggregateKind;
                        allowedTypes[k] = !member || k < scalarTypeCount || (k == 11 && kind == dwarf::TypeKind::STRUCT) ||
                                          (k == 12 && kind == dwarf::TypeKind::UNION) ||
                                          (k == 13 && kind == dwarf::TypeKind::ARRAY);
                }
                const bool typeChanged = ui::Choice("##editPropType", &typeIdx, types, IM_ARRAYSIZE(types), allowedTypes);
                if (typeChanged) {
                        editPropBuf_.typeChanged = true;
                        if (typeIdx >= scalarTypeCount) {
                                editPropBuf_.structMode = true;
                                editPropBuf_.unionMode  = typeIdx == 12;
                                editPropBuf_.arrayMode  = typeIdx == 13;
                        } else {
                                editPropBuf_.structMode = false;
                                editPropBuf_.unionMode  = false;
                                editPropBuf_.arrayMode  = false;
                                editPropBuf_.type       = typeVals[typeIdx];
                        }
                }

                ImGui::Text("%s", tr("Address", "地址"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("##editPropAddr", editPropBuf_.addrBuf, sizeof(editPropBuf_.addrBuf));

                ImGui::Text("%s", tr("Port", "端口"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                static const char    *ports[]   = {"JLINK", "SHM", "LOCAL", "AUDIO"};
                static const PortType portMap[] = {PortType::JLINK, PortType::SHM, PortType::LOCAL, PortType::AUDIO};
                int                   portIdx   = 0;
                for (int i = 0; i < IM_ARRAYSIZE(portMap); ++i)
                        if (portMap[i] == editPropBuf_.port) {
                                portIdx = i;
                                break;
                        }
                ImGui::BeginDisabled(member);
                if (ui::Choice("##editPropPort", &portIdx, ports, IM_ARRAYSIZE(ports)))
                        editPropBuf_.port = portMap[portIdx];
                ImGui::EndDisabled();
                if (member)
                        ImGui::TextDisabled("%s", tr("Port is inherited from the parent variable.", "端口继承父变量。"));

                if (editPropBuf_.port == PortType::AUDIO) {
                        editPropBuf_.type       = DataType::F32;
                        editPropBuf_.writable   = false;
                        editPropBuf_.structMode = false;
                        editPropBuf_.unionMode  = false;
                        editPropBuf_.arrayMode  = false;
                        ImGui::TextDisabled("%s", tr("F32 normalized PCM sample (-1..1)", "F32 归一化 PCM 采样值 (-1..1)"));
                } else {
                        if (editPropBuf_.port == PortType::LOCAL)
                                ImGui::TextDisabled("%s",
                                                    tr("In-process buffer — use &varname in SDK sequence.",
                                                       "进程内缓冲区，在 SDK 序列参数中用 &varname 引用。"));
                        if (editPropBuf_.structMode && !member) {
                                static const char *sfTypes[] = {
                                    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f16", "f32", "f64"};
                                static const DataType     sfTypeVals[] = {DataType::U8,
                                                                          DataType::U16,
                                                                          DataType::U32,
                                                                          DataType::U64,
                                                                          DataType::I8,
                                                                          DataType::I16,
                                                                          DataType::I32,
                                                                          DataType::I64,
                                                                          DataType::F16,
                                                                          DataType::F32,
                                                                          DataType::F64};
                                constexpr int             kSfTypeCount = IM_ARRAYSIZE(sfTypeVals);
                                constexpr ImGuiTableFlags tfl =
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
                                if (ui::TableIsland tableCard; tableCard.begin("##epSfTbl", 4, tfl, ImVec2(0, 120))) {
                                        ImGui::TableSetupColumn(editPropBuf_.arrayMode ? tr("Element", "元素")
                                                                                       : tr("Name", "名称"),
                                                                ImGuiTableColumnFlags_WidthStretch);
                                        ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                        ImGui::TableSetupColumn(tr("Offset", "偏移"), ImGuiTableColumnFlags_WidthFixed, 48.0f);
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
                                        ImGui::TableHeadersRow();
                                        int toDelete = -1;
                                        for (int fi = 0; fi < (int)editPropBuf_.structFields.size(); ++fi) {
                                                auto &sf = editPropBuf_.structFields[fi];
                                                ImGui::PushID(fi);
                                                ui::HierarchyNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                ImGui::InputText("##fn", sf.name, sizeof(sf.name));
                                                ImGui::TableSetColumnIndex(1);
                                                int sfIdx = 2;
                                                for (int k = 0; k < kSfTypeCount; ++k)
                                                        if (sfTypeVals[k] == sf.type) {
                                                                sfIdx = k;
                                                                break;
                                                        }
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ui::Choice("##ft", &sfIdx, sfTypes, kSfTypeCount))
                                                        sf.type = sfTypeVals[sfIdx];
                                                ImGui::TableSetColumnIndex(2);
                                                int off = (int)sf.byteOffset;
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::InputInt("##fo", &off, 0, 0)) {
                                                        if (off < 0)
                                                                off = 0;
                                                        sf.byteOffset = (u32)off;
                                                }
                                                ImGui::TableSetColumnIndex(3);
                                                if (ImGui::SmallButton("X"))
                                                        toDelete = fi;
                                                ImGui::PopID();
                                        }
                                        if (toDelete >= 0)
                                                editPropBuf_.structFields.erase(editPropBuf_.structFields.begin() + toDelete);
                                        tableCard.end();
                                }
                                if (ImGui::Button(editPropBuf_.arrayMode ? tr("+ Element", "+ 添加元素")
                                                                         : tr("+ Field", "+ 添加字段"))) {
                                        VarEntry::StructField sf{};
                                        if (!editPropBuf_.unionMode && !editPropBuf_.structFields.empty()) {
                                                const auto &prev = editPropBuf_.structFields.back();
                                                sf.byteOffset    = prev.byteOffset + Parser::typeBytes(prev.type);
                                        }
                                        if (editPropBuf_.arrayMode)
                                                snprintf(
                                                    sf.name, sizeof(sf.name), "[%d]", (int)editPropBuf_.structFields.size());
                                        else
                                                snprintf(
                                                    sf.name, sizeof(sf.name), "field%d", (int)editPropBuf_.structFields.size());
                                        editPropBuf_.structFields.push_back(sf);
                                }
                        }
                }
                if (member && editPropBuf_.structMode)
                        ImGui::TextDisabled("%s", tr("Aggregate layout follows the firmware type.", "结构布局沿用固件类型。"));
                ImGui::BeginDisabled(member);
                if (editPropBuf_.port == PortType::SHM) {
                        ImGui::Text("%s", tr("SHM Name", "SHM 名称"));
                        ImGui::SameLine(100);
                        ImGui::SetNextItemWidth(260);
                        ImGui::InputText("##editPropShm", editPropBuf_.shmName, sizeof(editPropBuf_.shmName));
                } else if (editPropBuf_.port == PortType::AUDIO) {
                        ImGui::Text("%s", tr("Audio Device", "音频设备"));
                        ImGui::SameLine(100);
                        drawAudioDeviceCombo(
                            editPropBuf_.audioDeviceIndex, editPropBuf_.audioDeviceName, sizeof(editPropBuf_.audioDeviceName));
                }

                ImGui::EndDisabled();
                if (editPropBuf_.port != PortType::AUDIO)
                        ui::AccessChoice(tr("Access mode", "读写模式"), editPropBuf_.writable);
                else
                        ImGui::TextDisabled("%s", tr("Read-only", "只读"));

                ImGui::Separator();
                if (ui::Button(tr("Apply", "应用"), ui::BtnStyle::Success, ImVec2(100, 0))) {
                        if (member) {
                                auto &ov      = vars_[editPropIdx_].memberOverrides[memberPropEdit_.path];
                                ov.alias      = editPropBuf_.name == memberPropEdit_.path ? "" : editPropBuf_.name;
                                ov.hasAddress = true;
                                try {
                                        ov.address = std::stoull(editPropBuf_.addrBuf, nullptr, 16);
                                } catch (...) {
                                        ov.address = memberPropEdit_.defaultAddr;
                                }
                                ov.hasType     = !editPropBuf_.structMode;
                                ov.type        = editPropBuf_.type;
                                ov.hasWritable = true;
                                ov.writable    = editPropBuf_.writable;
                                isModified_ = propertiesChanged_ = true;
                                editPropIdx_ = memberPropEdit_.parentIdx = -1;
                                ImGui::CloseCurrentPopup();
                                ImGui::EndPopup();
                                return;
                        }
                        VarEntry                      &v        = vars_[editPropIdx_];
                        dwarf::TypeKind                prevKind = dwarf::TypeKind::UNKNOWN;
                        std::vector<VarEntry::EnumDef> prevEnumDefs;
                        {
                                std::lock_guard lk(mtxElf_);
                                if (const dwarf::Type *current = resolveAlias(dwarfInfo_, v.typeOff)) {
                                        prevKind = current->kind;
                                        if (current->kind == dwarf::TypeKind::ENUM)
                                                for (const auto &entry : current->enums)
                                                        prevEnumDefs.push_back({entry.name, entry.val});
                                }
                        }
                        const std::string oldName = v.name;
                        v.name                    = editPropBuf_.name;
                        if (v.name != oldName && !v.memberUnitConversions.empty()) {
                                std::unordered_map<std::string, unit_conversion::Settings> renamed;
                                for (const auto &[path, settings] : v.memberUnitConversions) {
                                        const bool member = path.starts_with(oldName) && path.size() > oldName.size() &&
                                                            (path[oldName.size()] == '.' || path[oldName.size()] == '[');
                                        renamed[member ? v.name + path.substr(oldName.size()) : path] = settings;
                                }
                                v.memberUnitConversions = std::move(renamed);
                        }
                        v.displayNumericValue.reset();
                        v.type     = editPropBuf_.type;
                        v.port     = editPropBuf_.port;
                        v.writable = editPropBuf_.writable;
                        try {
                                v.addr = std::stoull(editPropBuf_.addrBuf, nullptr, 16);
                        } catch (...) {
                                v.addr = 0;
                        }
                        if (v.port == PortType::SHM) {
                                snprintf(v.shm.name, sizeof(v.shm.name), "%s", editPropBuf_.shmName);
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type     = DataType::F32;
                                v.typeOff  = 0;
                                v.writable = false;
                                v.isStruct = false;
                                v.isUnion  = false;
                                v.isArray  = false;
                                v.structFields.clear();
                                v.enumDefs.clear();
                                v.addr              = (u64)std::max(0, editPropBuf_.audioDeviceIndex);
                                v.audio.deviceIndex = editPropBuf_.audioDeviceIndex;
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", editPropBuf_.audioDeviceName);
                        }
                        if (editPropBuf_.structMode) {
                                v.isStruct     = true;
                                v.isUnion      = editPropBuf_.unionMode;
                                v.isArray      = editPropBuf_.arrayMode;
                                v.structFields = editPropBuf_.structFields;
                                const bool sameDwarfAggregate =
                                    (prevKind == dwarf::TypeKind::STRUCT && !v.isUnion && !v.isArray) ||
                                    (prevKind == dwarf::TypeKind::UNION && v.isUnion) ||
                                    (prevKind == dwarf::TypeKind::ARRAY && v.isArray);
                                if (!sameDwarfAggregate)
                                        v.typeOff = 0;
                        } else {
                                const bool wasDwarfAggregate = prevKind == dwarf::TypeKind::STRUCT ||
                                                               prevKind == dwarf::TypeKind::UNION ||
                                                               prevKind == dwarf::TypeKind::ARRAY;
                                if (wasDwarfAggregate || editPropBuf_.typeChanged)
                                        v.typeOff = 0;
                                if (prevKind == dwarf::TypeKind::ENUM && v.enumDefs.empty())
                                        v.enumDefs = std::move(prevEnumDefs);
                                v.isStruct = false;
                                v.isUnion  = false;
                                v.isArray  = false;
                                v.structFields.clear();
                        }
                        if (v.port == PortType::LOCAL && editPropBuf_.structMode) {
                                size_t totalSz = 0;
                                for (const auto &sf : v.structFields)
                                        totalSz = std::max<size_t>(totalSz, sf.byteOffset + Parser::typeBytes(sf.type));
                                if (totalSz == 0)
                                        totalSz = 8;
                                std::lock_guard lk(mtxLocal_);
                                localBufs_[v.name].assign(totalSz, 0);
                        } else if (v.port == PortType::LOCAL && !editPropBuf_.structMode) {
                                std::lock_guard lk(mtxLocal_);
                                auto            it = localBufs_.find(v.name);
                                if (it != localBufs_.end())
                                        it->second.assign(Parser::typeBytes(v.type), 0);
                        }
                        isModified_        = true;
                        propertiesChanged_ = true;
                        editPropIdx_       = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (member) {
                        if (ImGui::Button(tr("Reset", "重置"), ImVec2(100, 0))) {
                                vars_[editPropIdx_].memberOverrides.erase(memberPropEdit_.path);
                                isModified_ = propertiesChanged_ = true;
                                editPropIdx_ = memberPropEdit_.parentIdx = -1;
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                }
                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(100, 0))) {
                        memberPropEdit_.parentIdx = -1;
                        editPropIdx_              = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::exportVarFile(const std::string &path)
{
        cJSON *root = cJSON_CreateObject();
        cJSON *vArr = cJSON_CreateArray();
        for (const auto &v : vars_) {
                cJSON *vObj = cJSON_CreateObject();
                cJSON_AddStringToObject(vObj, "name", v.name.c_str());
                flasharray::save(vObj, v.flashArray);
                if (v.flashArray)
                        cJSON_AddBoolToObject(vObj, "flashArrayExpanded", v.expandedMembers.contains(v.name));
                cJSON_AddNumberToObject(vObj, "type", (int)v.type);
                cJSON_AddNumberToObject(vObj, "displayRadix", variableRadix(v));
                saveVariableDisplaySettings(vObj, v);
                cJSON_AddStringToObject(vObj, "typeStr", Parser::dataTypeToStr(v.type));
                cJSON_AddNumberToObject(vObj, "port", (int)v.port);
                cJSON_AddStringToObject(vObj, "portStr", portName(v.port));
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "0x%08llX", (unsigned long long)v.addr);
                cJSON_AddStringToObject(vObj, "addr", addrBuf);
                cJSON_AddBoolToObject(vObj, "writable", v.writable);
                cJSON_AddBoolToObject(vObj, "isStruct", v.isStruct);
                cJSON_AddBoolToObject(vObj, "isUnion", v.isUnion);
                cJSON_AddBoolToObject(vObj, "isArray", v.isArray);
                cJSON_AddNumberToObject(vObj, "typeOff", (double)v.typeOff);
                if (v.port == PortType::LOCAL) {
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        if (const auto it = localBufs_.find(v.name); it != localBufs_.end())
                                cJSON_AddNumberToObject(vObj, "localBufSize", static_cast<double>(it->second.size()));
                }
                cJSON_AddNumberToObject(vObj, "bitOffset", v.bitOffset);
                cJSON_AddNumberToObject(vObj, "bitSize", v.bitSize);
                auto saveBinding = [vObj](const char *key, const VarEntry::FunctionBinding &binding) {
                        if (binding.empty())
                                return;
                        cJSON *object = cJSON_CreateObject();
                        cJSON_AddStringToObject(object, "source", binding.sourcePath.c_str());
                        cJSON_AddStringToObject(object, "name", binding.functionName.c_str());
                        cJSON_AddStringToObject(object, "signature", binding.signature.c_str());
                        cJSON_AddItemToObject(vObj, key, object);
                };
                saveBinding("readFunction", v.readFunction);
                saveBinding("writeFunction", v.writeFunction);
                if (v.port == PortType::SHM) {
                        cJSON_AddStringToObject(vObj, "shmName", v.shm.name);
                }
                if (v.port == PortType::AUDIO) {
                        cJSON_AddNumberToObject(vObj, "audioDeviceIndex", v.audio.deviceIndex);
                        cJSON_AddStringToObject(vObj, "audioDeviceName", v.audio.deviceName);
                }
                if (!v.enumDefs.empty()) {
                        cJSON *eArr = cJSON_CreateArray();
                        for (const auto &e : v.enumDefs) {
                                cJSON *eObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(eObj, "name", e.name.c_str());
                                cJSON_AddNumberToObject(eObj, "value", (double)e.val);
                                cJSON_AddItemToArray(eArr, eObj);
                        }
                        cJSON_AddItemToObject(vObj, "enumDefs", eArr);
                }
                if (!v.structFields.empty()) {
                        cJSON *sfArr = cJSON_CreateArray();
                        for (const auto &sf : v.structFields) {
                                cJSON *field = cJSON_CreateObject();
                                cJSON_AddStringToObject(field, "name", sf.name);
                                cJSON_AddNumberToObject(field, "type", static_cast<int>(sf.type));
                                cJSON_AddNumberToObject(field, "offset", sf.byteOffset);
                                cJSON_AddItemToArray(sfArr, field);
                        }
                        cJSON_AddItemToObject(vObj, "structFields", sfArr);
                }
                cJSON_AddItemToArray(vArr, vObj);
        }
        cJSON_AddItemToObject(root, "variables", vArr);
        if (!functionSources_.empty()) {
                cJSON *sources = cJSON_CreateArray();
                for (const auto &source : functionSources_) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "libraryPath", source->libraryPath.c_str());
                        cJSON_AddStringToObject(item, "headerPath", source->headerPath.c_str());
                        cJSON_AddItemToArray(sources, item);
                }
                cJSON_AddItemToObject(root, "functionSources", sources);
        }

        // Remember the ELF/AXF this list was built from, stored RELATIVE to the .var
        // file so the pair stays portable when moved together (resolved on import)
        if (!elfPath_.empty()) {
                std::error_code             ec;
                const std::filesystem::path baseDir = std::filesystem::path(path).parent_path();
                const std::filesystem::path rel     = (baseDir.empty() || remote::isUrl(elfPath_))
                                                          ? std::filesystem::path(elfPath_)
                                                          : std::filesystem::relative(elfPath_, baseDir, ec);
                const std::string elfStored = (remote::isUrl(elfPath_) || ec || rel.empty()) ? elfPath_ : rel.generic_string();
                cJSON_AddStringToObject(root, "elfPath", elfStored.c_str());
        }

        char *jsonStr = cJSON_Print(root);
        cJSON_Delete(root);
        if (jsonStr) {
                std::ofstream ofs(path, std::ios::trunc);
                if (ofs)
                        ofs << jsonStr;
                free(jsonStr);
                ImGui::InsertNotification(
                    {ImGuiToastType::Success, 3000, tr("Exported %d variables", "已导出 %d 个变量"), (int)vars_.size()});
        }
}

void
Variable::importVarFile(const std::string &path)
{
        std::ifstream ifs(path);
        if (!ifs) {
                ImGui::InsertNotification({ImGuiToastType::Error, 3000, tr("Failed to open file", "无法打开文件")});
                return;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        cJSON      *root = cJSON_Parse(content.c_str());
        if (!root) {
                ImGui::InsertNotification({ImGuiToastType::Error, 3000, tr("Invalid JSON", "JSON 格式错误")});
                return;
        }

        cJSON *vArr = cJSON_GetObjectItem(root, "variables");
        if (!cJSON_IsArray(vArr)) {
                cJSON_Delete(root);
                ImGui::InsertNotification(
                    {ImGuiToastType::Error, 3000, tr("No 'variables' array found", "未找到 'variables' 数组")});
                return;
        }

        int imported = 0;
        if (const cJSON *sources = cJSON_GetObjectItem(root, "functionSources"); cJSON_IsArray(sources)) {
                for (const cJSON *item = sources->child; item; item = item->next) {
                        auto source = std::make_unique<FunctionSource>();
                        if (const cJSON *p = cJSON_GetObjectItem(item, "libraryPath"); cJSON_IsString(p))
                                source->libraryPath = p->valuestring;
                        if (const cJSON *p = cJSON_GetObjectItem(item, "headerPath"); cJSON_IsString(p))
                                source->headerPath = p->valuestring;
                        if (!source->libraryPath.empty() || !source->headerPath.empty())
                                functionSources_.push_back(std::move(source));
                }
        } else {
                auto source = std::make_unique<FunctionSource>();
                if (const cJSON *p = cJSON_GetObjectItem(root, "functionLibraryPath"); cJSON_IsString(p))
                        source->libraryPath = p->valuestring;
                if (const cJSON *p = cJSON_GetObjectItem(root, "functionHeaderPath"); cJSON_IsString(p))
                        source->headerPath = p->valuestring;
                if (!source->libraryPath.empty() || !source->headerPath.empty())
                        functionSources_.push_back(std::move(source));
        }
        if (!functionSources_.empty())
                functionPanelOpen_ = true;
        for (int i = 0; i < cJSON_GetArraySize(vArr); ++i) {
                cJSON *vObj  = cJSON_GetArrayItem(vArr, i);
                cJSON *nItem = cJSON_GetObjectItem(vObj, "name");
                if (!cJSON_IsString(nItem))
                        continue;

                VarEntry v;
                v.name = nItem->valuestring;
                loadVariableDisplaySettings(vObj, v);
                if (const auto *radix = cJSON_GetObjectItem(vObj, "displayRadix"); cJSON_IsNumber(radix)) {
                        const int r    = radix->valueint;
                        v.displayRadix = r == 0 ? normalizeDisplayRadix(displayRadix_) : normalizeDisplayRadix(r);
                }

                if (cJSON_GetObjectItemCaseSensitive(vObj, "flashArray")) {
                        try {
                                restoreFlashArray(v, vObj);
                                if (!watchHasIdentity(v.name, v.addr, v.type)) {
                                        vars_.push_back(std::move(v));
                                        ++imported;
                                }
                        } catch (const std::exception &e) {
                                ImGui::InsertNotification(
                                    {ImGuiToastType::Error, 7000, "%s", parameterError(e.what()).c_str()});
                        }
                        continue;
                }
                if (cJSON_GetObjectItem(vObj, "type"))
                        v.type = (DataType)cJSON_GetObjectItem(vObj, "type")->valueint;
                v.port = portFromJson(vObj);

                // Address: prefer string "0x..." format, fall back to numeric
                cJSON *addrItem = cJSON_GetObjectItem(vObj, "addr");
                if (cJSON_IsString(addrItem)) {
                        try {
                                v.addr = std::stoull(addrItem->valuestring, nullptr, 16);
                        } catch (...) {
                                v.addr = 0;
                        }
                } else if (cJSON_IsNumber(addrItem)) {
                        v.addr = (u64)addrItem->valuedouble;
                }

                if (cJSON_GetObjectItem(vObj, "writable"))
                        v.writable = cJSON_IsTrue(cJSON_GetObjectItem(vObj, "writable"));
                if (const cJSON *item = cJSON_GetObjectItem(vObj, "isStruct"); cJSON_IsBool(item))
                        v.isStruct = cJSON_IsTrue(item);
                if (const cJSON *item = cJSON_GetObjectItem(vObj, "isUnion"); cJSON_IsBool(item))
                        v.isUnion = cJSON_IsTrue(item);
                if (const cJSON *item = cJSON_GetObjectItem(vObj, "isArray"); cJSON_IsBool(item))
                        v.isArray = cJSON_IsTrue(item);
                if (cJSON_GetObjectItem(vObj, "typeOff"))
                        v.typeOff = (u64)cJSON_GetObjectItem(vObj, "typeOff")->valuedouble;
                if (cJSON_GetObjectItem(vObj, "bitOffset"))
                        v.bitOffset = (u32)cJSON_GetObjectItem(vObj, "bitOffset")->valueint;
                if (cJSON_GetObjectItem(vObj, "bitSize"))
                        v.bitSize = (u32)cJSON_GetObjectItem(vObj, "bitSize")->valueint;
                auto loadBinding = [vObj](const char *key, VarEntry::FunctionBinding &binding) {
                        const cJSON *object = cJSON_GetObjectItem(vObj, key);
                        if (!cJSON_IsObject(object))
                                return;
                        if (const cJSON *source = cJSON_GetObjectItem(object, "source"); cJSON_IsString(source))
                                binding.sourcePath = source->valuestring;
                        if (const cJSON *name = cJSON_GetObjectItem(object, "name"); cJSON_IsString(name))
                                binding.functionName = name->valuestring;
                        if (const cJSON *sig = cJSON_GetObjectItem(object, "signature"); cJSON_IsString(sig))
                                binding.signature = sig->valuestring;
                };
                loadBinding("readFunction", v.readFunction);
                loadBinding("writeFunction", v.writeFunction);

                if (v.port == PortType::UDP) {
                        if (cJSON_IsString(cJSON_GetObjectItem(vObj, "udpIp")))
                                snprintf(v.udp.ip, sizeof(v.udp.ip), "%s", cJSON_GetObjectItem(vObj, "udpIp")->valuestring);
                        if (cJSON_GetObjectItem(vObj, "udpPort"))
                                v.udp.port = (u16)cJSON_GetObjectItem(vObj, "udpPort")->valueint;
                } else if (v.port == PortType::SHM) {
                        if (cJSON_IsString(cJSON_GetObjectItem(vObj, "shmName")))
                                snprintf(
                                    v.shm.name, sizeof(v.shm.name), "%s", cJSON_GetObjectItem(vObj, "shmName")->valuestring);
                } else if (v.port == PortType::AUDIO) {
                        v.type     = DataType::F32;
                        v.writable = false;
                        if (auto *adi = cJSON_GetObjectItem(vObj, "audioDeviceIndex"); cJSON_IsNumber(adi))
                                v.audio.deviceIndex = adi->valueint;
                        else
                                v.audio.deviceIndex = static_cast<int>(v.addr);
                        if (auto *adn = cJSON_GetObjectItem(vObj, "audioDeviceName"); cJSON_IsString(adn))
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", adn->valuestring);
                        else
                                snprintf(v.audio.deviceName,
                                         sizeof(v.audio.deviceName),
                                         "%s",
                                         AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        v.addr = (u64)std::max(0, v.audio.deviceIndex);
                }

                cJSON *eArr = cJSON_GetObjectItem(vObj, "enumDefs");
                if (cJSON_IsArray(eArr)) {
                        for (int j = 0; j < cJSON_GetArraySize(eArr); ++j) {
                                cJSON            *eObj = cJSON_GetArrayItem(eArr, j);
                                VarEntry::EnumDef d;
                                if (cJSON_IsString(cJSON_GetObjectItem(eObj, "name")))
                                        d.name = cJSON_GetObjectItem(eObj, "name")->valuestring;
                                if (cJSON_GetObjectItem(eObj, "value"))
                                        d.val = (i64)cJSON_GetObjectItem(eObj, "value")->valuedouble;
                                v.enumDefs.push_back(d);
                        }
                }

                if (const cJSON *fields = cJSON_GetObjectItem(vObj, "structFields"); cJSON_IsArray(fields)) {
                        for (const cJSON *item = fields->child; item; item = item->next) {
                                VarEntry::StructField field;
                                if (const cJSON *name = cJSON_GetObjectItem(item, "name"); cJSON_IsString(name))
                                        snprintf(field.name, sizeof(field.name), "%s", name->valuestring);
                                if (const cJSON *type = cJSON_GetObjectItem(item, "type"); cJSON_IsNumber(type))
                                        field.type = static_cast<DataType>(type->valueint);
                                if (const cJSON *offset = cJSON_GetObjectItem(item, "offset"); cJSON_IsNumber(offset))
                                        field.byteOffset = static_cast<u32>(offset->valuedouble);
                                v.structFields.push_back(field);
                        }
                        v.isStruct = v.isStruct || !v.structFields.empty();
                }
                if (watchHasIdentity(v.name, v.addr, v.type))
                        continue;
                if (v.port == PortType::LOCAL) {
                        size_t size = std::max<u32>(1, Parser::typeBytes(v.type));
                        for (const auto &field : v.structFields)
                                size = std::max(size, static_cast<size_t>(field.byteOffset + Parser::typeBytes(field.type)));
                        if (const cJSON *savedSize = cJSON_GetObjectItem(vObj, "localBufSize");
                            cJSON_IsNumber(savedSize) && savedSize->valuedouble > 0.0)
                                size = std::max(size, static_cast<size_t>(savedSize->valuedouble));
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        localBufs_.emplace(v.name, std::vector<uint8_t>(size, 0));
                }
                vars_.push_back(v);
                imported++;
        }

        // Resolve the ELF/AXF path stored relative to this .var file. Loading it
        // re-syncs the imported variables' addresses/types against the symbol file
        std::string elfToLoad;
        if (const cJSON *elfItem = cJSON_GetObjectItem(root, "elfPath");
            cJSON_IsString(elfItem) && elfItem->valuestring[0] != '\0') {
                const std::filesystem::path stored = elfItem->valuestring;
                std::error_code             ec;
                const std::filesystem::path resolved =
                    stored.is_absolute() ? stored : (std::filesystem::path(path).parent_path() / stored).lexically_normal();
                if (remote::isUrl(elfItem->valuestring))
                        elfToLoad = elfItem->valuestring;
                else if (std::filesystem::exists(resolved, ec))
                        elfToLoad = resolved.string();
                else if (std::filesystem::exists(stored, ec)) // fall back to CWD-relative
                        elfToLoad = stored.string();
        }

        cJSON_Delete(root);
        isModified_        = true;
        propertiesChanged_ = true;
        ImGui::InsertNotification({ImGuiToastType::Success, 3000, tr("Imported %d variables", "已导入 %d 个变量"), imported});

        if (!elfToLoad.empty() && elfToLoad != elfPath_)
                loadElf(elfToLoad);
}

static void
populateShmMemberCache(const dwarf::Info                            &info,
                       const std::string                            &prefix,
                       u64                                           baseAddr,
                       u64                                           typeOff,
                       const u8                                     *blob,
                       usize                                         blobSize,
                       std::unordered_map<std::string, std::string> &cache,
                       std::unordered_map<std::string, double>      &numericCache,
                       const VarEntry                               &owner)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;
        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        if (m.offset >= blobSize)
                                continue;
                        std::string memberPath = prefix + "." + (m.name.empty() ? "<anon>" : m.name);
                        u64         memberAddr = baseAddr + m.offset;
                        const char *sType      = scalarPayloadType(info, m.type);
                        if (sType) {
                                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                                if (m.offset + sz <= blobSize) {
                                        if (const auto number = unitConversionNumber(
                                                blob + m.offset, Parser::strToDataType(sType), m.bitOffset, m.bitSize))
                                                numericCache[memberPath] = *number;
                                        cache[memberPath] = decodeValue(blob + m.offset,
                                                                        Parser::strToDataType(sType),
                                                                        m.bitOffset,
                                                                        m.bitSize,
                                                                        owner.radixForPath(memberPath));
                                }
                        } else {
                                populateShmMemberCache(info,
                                                       memberPath,
                                                       memberAddr,
                                                       m.type,
                                                       blob + m.offset,
                                                       blobSize - m.offset,
                                                       cache,
                                                       numericCache,
                                                       owner);
                        }
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                u64 elemSize = typeSize(info, t->inner);
                if (elemSize == 0)
                        return;
                u64         dim   = t->dims.empty() ? 0 : t->dims.front();
                const char *sType = scalarPayloadType(info, t->inner);
                for (u64 i = 0; i < dim; ++i) {
                        u64 offset = i * elemSize;
                        if (offset >= blobSize)
                                break;
                        std::string memberPath = prefix + "[" + std::to_string(i) + "]";
                        u64         memberAddr = baseAddr + offset;
                        if (sType) {
                                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                                if (offset + sz <= blobSize) {
                                        if (const auto number =
                                                unitConversionNumber(blob + offset, Parser::strToDataType(sType)))
                                                numericCache[memberPath] = *number;
                                        cache[memberPath] = decodeValue(
                                            blob + offset, Parser::strToDataType(sType), 0, 0, owner.radixForPath(memberPath));
                                }
                        } else {
                                populateShmMemberCache(info,
                                                       memberPath,
                                                       memberAddr,
                                                       t->inner,
                                                       blob + offset,
                                                       blobSize - offset,
                                                       cache,
                                                       numericCache,
                                                       owner);
                        }
                }
        }
}

static std::string
shmShadowKey(const VarEntry &v)
{
        return std::string(v.shm.name) + "\n" + v.name;
}

void
Variable::startPollThread()
{
        if (poll_)
                return; // already running
        poll_              = std::make_shared<PollState>();
        poll_->intervalMs  = updateIntervalMs_;
        auto        ps     = poll_; // worker holds its own ref — never touches `this`
        std::string nameCp = name_;
        pollThread_        = std::thread([ps, nameCp]() {
                LOG_I("Variable[%s] poll thread started", nameCp.c_str());
                try {
                        while (ps->running.load()) {
                                u32 interval = ps->intervalMs.load();
                                if (interval < 5)
                                        interval = 5;

                                if (!JLinkPort::instance().isConnected()) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                        continue;
                                }

                                // Snapshot the read list published by the GUI thread
                                std::vector<PollReq> reqs;
                                {
                                        std::lock_guard lk(ps->mtx);
                                        reqs = ps->reqs;
                                }

                                const auto sweepStart = get_mono_ts_ms();
                                // 合并相邻或重叠的请求, 不读取空隙, 不跨越 4 KB 边界
                                std::sort(reqs.begin(), reqs.end(), [](const auto &a, const auto &b) {
                                        return a.epoch != b.epoch ? a.epoch < b.epoch : a.addr < b.addr;
                                });
                                for (size_t first = 0; first < reqs.size() && ps->running.load();) {
                                        const auto &head = reqs[first];
                                        size_t      last = first + 1;
                                        u64         end  = head.addr + head.sz;
                                        while (last < reqs.size()) {
                                                const auto &next    = reqs[last];
                                                const u64   nextEnd = next.addr + next.sz;
                                                if (next.epoch != head.epoch || !next.sz || next.sz > 8 ||
                                                    next.addr > UINT32_MAX - (next.sz - 1) || next.addr > end ||
                                                    nextEnd > head.addr + 256 || (head.addr >> 12) != ((nextEnd - 1) >> 12))
                                                        break;
                                                end = std::max(end, nextEnd);
                                                ++last;
                                        }
                                        std::array<u8, 256> buffer{};
                                        const bool          ok = head.sz > 0 && head.sz <= 8 &&
                                                        head.addr <= UINT32_MAX - (head.sz - 1) && end <= u64(UINT32_MAX) + 1 &&
                                                        JLinkPort::instance().readMemAtEpoch(head.epoch,
                                                                                             static_cast<u32>(head.addr),
                                                                                             static_cast<u32>(end - head.addr),
                                                                                             buffer.data());
                                        const auto timestamp = get_mono_ts_ms();
                                        for (size_t i = first; i < last && ps->running.load(); ++i) {
                                                const auto &r = reqs[i];
                                                PollVal     pv{};
                                                pv.sz        = r.sz;
                                                pv.epoch     = r.epoch;
                                                pv.timestamp = timestamp;
                                                pv.ok        = ok;
                                                if (ok)
                                                        std::copy_n(buffer.data() + r.addr - head.addr, r.sz, pv.buf);
                                                else if (r.sz > 0 && r.sz <= 8 && r.addr <= UINT32_MAX - (r.sz - 1)) {
                                                        // 失败后逐项重试一次, 隔离坏地址并确认短暂读取失败
                                                        pv.ok = JLinkPort::instance().readMemAtEpoch(
                                                            r.epoch, static_cast<u32>(r.addr), r.sz, pv.buf);
                                                        pv.timestamp = get_mono_ts_ms();
                                                }
                                                std::lock_guard lk(ps->mtx);
                                                ps->vals[r.addr] = pv;
                                        }
                                        first = last;
                                }

                                ps->sweepMs.store(get_mono_ts_ms() - sweepStart);

                                // Sleep in small slices so a stop request (e.g. the
                                // Variable being destroyed during a session import) is
                                // honored promptly instead of blocking the join for up
                                // to a full refresh interval
                                for (u32 slept = 0; slept < interval && ps->running.load(); slept += 20)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20u, interval - slept)));
                        }
                } catch (...) {
                        // Never let an exception escape — that would call
                        // std::terminate and take the whole app down
                        LOG_E("Variable[%s] poll thread caught exception", nameCp.c_str());
                }
                LOG_I("Variable[%s] poll thread stopped", nameCp.c_str());
        });
}

void
Variable::stopPollThread()
{
        if (poll_)
                poll_->running.store(false);
        if (pollThread_.joinable())
                pollThread_.join();
        poll_.reset();
}

void
Variable::updateVariables()
{
        // Keep the worker's poll cadence in sync with the UI slider and make sure
        // the background reader is running (lazy start on first display)
        startPollThread();
        poll_->intervalMs.store(updateIntervalMs_);

        // Publish LOCAL function reads to their own worker and consume the prev
        // completed batch on every GUI pass. The worker cadence comes from the
        // fastest Variable Recorder that references a bound LOCAL scalar
        std::vector<AccessReadReq> functionReads;
        for (const auto &v : vars_) {
                if (v.flashArray)
                        continue;
                if (v.port != PortType::LOCAL || v.isStruct || v.readFunction.empty())
                        continue;
                FunctionSource *source = nullptr;
                if (const CFuncDecl *fn = findFunction(v.readFunction, &source); fn && isReadAccessor(*fn, v.type))
                        functionReads.push_back({v.name, v.type, *fn, source});
        }
        if (!functionReads.empty() || accessor_) {
                startAccessorThread();
                const u32 monitorHz = monitorFunctionHz_.load(std::memory_order_acquire);
                const u32 intervalUs =
                    monitorHz > 0 ? std::max(1u, 1000000u / monitorHz) : std::max(1u, updateIntervalMs_) * 1000u;
                accessor_->intervalUs.store(intervalUs, std::memory_order_release);
                std::unordered_map<std::string, AccessResult> completed;
                std::unordered_map<std::string, AccessResult> completedWrites;
                bool                                          readsChanged = false;
                {
                        std::lock_guard<std::mutex> lk(accessor_->mtx);
                        completed.swap(accessor_->results);
                        completedWrites.swap(accessor_->writeResults);
                        readsChanged = accessor_->reads.size() != functionReads.size();
                        if (!readsChanged) {
                                for (size_t i = 0; i < functionReads.size(); ++i) {
                                        const auto &oldReq = accessor_->reads[i];
                                        const auto &newReq = functionReads[i];
                                        if (oldReq.name != newReq.name || oldReq.type != newReq.type ||
                                            oldReq.source != newReq.source || oldReq.function.name != newReq.function.name ||
                                            functionSignature(oldReq.function) != functionSignature(newReq.function)) {
                                                readsChanged = true;
                                                break;
                                        }
                                }
                        }
                        if (readsChanged) {
                                accessor_->reads        = functionReads;
                                accessor_->readsPending = true;
                        }
                }
                if (readsChanged)
                        accessor_->cv.notify_one();
                for (auto &v : vars_) {
                        if (v.flashArray)
                                continue;
                        auto result = completed.find(v.name);
                        if (result == completed.end())
                                continue;
                        v.readFunctionError = result->second.ok ? std::string{} : result->second.error;
                        v.functionError     = !v.writeFunctionError.empty() ? v.writeFunctionError : v.readFunctionError;
                        if (result->second.ok && result->second.hasValue && v.port == PortType::LOCAL) {
                                std::lock_guard<std::mutex> lk(mtxLocal_);
                                auto                       &buffer   = localBufs_[v.name];
                                const size_t                required = std::max<size_t>(1, Parser::typeBytes(v.type));
                                if (buffer.size() >= required)
                                        std::memcpy(buffer.data(),
                                                    result->second.val.data(),
                                                    std::min<size_t>(required, result->second.size));
                        }
                }
                for (auto &v : vars_) {
                        if (v.flashArray)
                                continue;
                        auto result = completedWrites.find(v.name);
                        if (result != completedWrites.end()) {
                                v.writeFunctionError = result->second.ok ? std::string{} : result->second.error;
                                v.functionError = !v.writeFunctionError.empty() ? v.writeFunctionError : v.readFunctionError;
                        }
                }
        }

        u64 now = get_mono_ts_ms();
        if (now - prevUpdateTs_ < updateIntervalMs_)
                return;
        prevUpdateTs_ = now;

        // Pause freezes the displayed values, while both target polling and LOCAL
        // function accessors continue sampling in their background threads
        if (g_monitorPaused.load())
                return;

        // Publish the current set of JLINK scalar reads for the worker thread, and
        // take a snapshot of the latest values it has produced
        std::unordered_map<u64, PollVal> polledVals;
        {
                std::unordered_map<u64, u32> reqByAddr;
                std::vector<PollReq>         reqs;
                for (const auto &v : vars_) {
                        if (v.flashArray)
                                continue;
                        if (v.port != PortType::JLINK || v.is_editing)
                                continue;
                        if (v.isStruct && v.typeOff == 0) {
                                for (const auto &field : v.structFields)
                                        reqByAddr[v.addr + field.byteOffset] = Parser::typeBytes(field.type);
                                continue;
                        }
                        const dwarf::Type *t = resolveAlias(dwarfInfo_, v.typeOff);
                        if (t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                  t->kind == dwarf::TypeKind::ARRAY))
                                continue; // complex types aren't scalar-read here

                        reqByAddr[v.addr] = Parser::typeBytes(v.type);
                }
                for (const auto &[addr, sz] : memberPollReqs_) {
                        auto &requestedSize = reqByAddr[addr];
                        requestedSize       = std::max(requestedSize, sz);
                }
                reqs.reserve(reqByAddr.size());
                for (auto it = diagnosticPollReqs_.begin(); it != diagnosticPollReqs_.end();) {
                        if (now - it->second.touched > 1000) {
                                it = diagnosticPollReqs_.erase(it);
                                continue;
                        }
                        auto &size = reqByAddr[it->first];
                        size       = std::max(size, it->second.size);
                        ++it;
                }
                for (const auto &[addr, size] : pendingCopyRequests_)
                        reqByAddr[addr] = std::max(reqByAddr[addr], size);
                for (const auto &[addr, sz] : reqByAddr)
                        reqs.push_back({JLinkPort::instance().targetEpoch(), addr, sz});
                std::lock_guard lk(poll_->mtx);
                poll_->reqs = std::move(reqs);
                polledVals  = poll_->vals;
        }
        syncReadCache_ = polledVals;

        for (auto &v : vars_) {
                if (v.flashArray)
                        continue;
                if (v.is_editing)
                        continue;

                const dwarf::Type *t         = resolveAlias(dwarfInfo_, v.typeOff);
                const bool         isComplex = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                             t->kind == dwarf::TypeKind::ARRAY);
                if (v.isStruct && v.typeOff == 0) {
                        v.valueStr = std::string(v.isArray ? "array (" : (v.isUnion ? "union (" : "struct (")) +
                                     std::to_string(v.structFields.size()) + " fields)";
                        if (v.port == PortType::SHM) {
                                size_t totalSz = 0;
                                for (const auto &field : v.structFields)
                                        totalSz = std::max<size_t>(totalSz, field.byteOffset + Parser::typeBytes(field.type));
                                if (!v.shm.inited) {
                                        shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                                        if (shm_init(&v.shm.handle, cfg) == 0)
                                                v.shm.inited = true;
                                }
                                if (v.shm.inited && totalSz > 0 && totalSz <= 4096) {
                                        auto &shadow = shmShadowBufs_[shmShadowKey(v)];
                                        shadow.resize(totalSz);
                                        shm_read(&v.shm.handle, shadow.data(), totalSz);
                                }
                        }
                        continue;
                }
                if (isComplex) {
                        v.valueStr = "...";
                        // For SHM structs: read the full blob and populate the member cache so the
                        // struct browser can display live values without requiring JLink
                        if (v.port == PortType::SHM) {
                                if (!v.shm.inited) {
                                        shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                                        if (shm_init(&v.shm.handle, cfg) == 0)
                                                v.shm.inited = true;
                                }
                                if (v.shm.inited) {
                                        usize sz = typeSize(dwarfInfo_, v.typeOff);
                                        if (sz > 0 && sz <= 4096) {
                                                auto &shadow = shmShadowBufs_[shmShadowKey(v)];
                                                if (shadow.size() != sz)
                                                        shadow.assign(sz, 0);
                                                std::vector<u8> tmp(sz);
                                                usize           nr = shm_read(&v.shm.handle, tmp.data(), sz);
                                                if (nr > 0) {
                                                        std::memcpy(shadow.data(), tmp.data(), std::min(nr, sz));
                                                        populateShmMemberCache(dwarfInfo_,
                                                                               v.name,
                                                                               v.addr,
                                                                               v.typeOff,
                                                                               shadow.data(),
                                                                               sz,
                                                                               memberValueCache_,
                                                                               memberNumericValueCache_,
                                                                               v);
                                                } else if (!shadow.empty()) {
                                                        populateShmMemberCache(dwarfInfo_,
                                                                               v.name,
                                                                               v.addr,
                                                                               v.typeOff,
                                                                               shadow.data(),
                                                                               sz,
                                                                               memberValueCache_,
                                                                               memberNumericValueCache_,
                                                                               v);
                                                }
                                        }
                                }
                        }
                        continue;
                }

                if (v.port == PortType::AUDIO) {
                        v.type     = DataType::F32;
                        v.writable = false;
                        if (v.audio.deviceName[0] == '\0') {
                                snprintf(v.audio.deviceName,
                                         sizeof(v.audio.deviceName),
                                         "%s",
                                         AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        }
                        AudioSample latest;
                        if (AudioInput::instance().latestSample(v.audio.deviceIndex, latest)) {
                                v.displayByteSize = sizeof(latest.val);
                                std::memcpy(v.displayBytes.data(), &latest.val, sizeof(latest.val));
                                v.valueStr = decodeValue(v.displayBytes.data(), DataType::F32, 0, 0, variableRadix(v));
                                v.displayNumericValue =
                                    std::isfinite(latest.val) ? std::optional<double>(latest.val) : std::nullopt;
                        } else {
                                v.valueStr = "...";
                                v.displayNumericValue.reset();
                        }
                        continue;
                }

                u8  buf[8] = {0};
                int ret    = -2; // -2: No update, -1: Error, 0: Success
                u32 sz     = Parser::typeBytes(v.type);
                if (v.port == PortType::JLINK && JLinkPort::instance().isConnected()) {
                        // Value is read by the background poll thread; consume the
                        // cached bytes so the render thread never blocks on USB I/O
                        auto it = polledVals.find(v.addr);
                        if (it != polledVals.end() && it->second.ok) {
                                std::memcpy(buf, it->second.buf, sizeof(buf));
                                ret = 0;
                        } else if (it != polledVals.end()) {
                                ret = -1;
                        }
                        // Not yet polled (it == end): leave ret = -2 (no update)
                } else if (v.port == PortType::SHM) {
                        if (!v.shm.inited) {
                                shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                                if (shm_init(&v.shm.handle, cfg) == 0)
                                        v.shm.inited = true;
                                else
                                        ret = -1;
                        }
                        if (v.shm.inited) {
                                usize nr = shm_read(&v.shm.handle, buf, sz);
                                if (nr == sz) {
                                        ret = 0;
                                } else if (nr > 0) {
                                        // SHM is a byte stream, so a producer may have written a
                                        // narrower scalar than this row expects. Keep the bytes we
                                        // did receive instead of consuming them and showing "..."
                                        ret = 0;
                                } else {
                                        ret = -2;
                                }
                        }
                } else if (v.port == PortType::LOCAL) {
                        if (!v.structFields.empty()) {
                                // Manual aggregate: show field count summary
                                v.valueStr = std::string(v.isArray ? "array (" : (v.isUnion ? "union (" : "struct (")) +
                                             std::to_string(v.structFields.size()) + " fields)";
                                continue;
                        }
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        auto                        it = localBufs_.find(v.name);
                        if (it != localBufs_.end() && !it->second.empty()) {
                                const auto &vd = it->second;
                                if (vd.size() > sizeof(double)) {
                                        // Struct-sized buffer: show hex dump, skip decodeValue
                                        char   hs[80] = {};
                                        size_t showN  = std::min(vd.size(), (size_t)16);
                                        size_t pos    = 0;
                                        for (size_t b = 0; b < showN && pos + 3 < sizeof(hs); ++b)
                                                pos += (size_t)snprintf(hs + pos, sizeof(hs) - pos, "%02X ", vd[b]);
                                        if (vd.size() > 16 && pos > 0) {
                                                hs[pos - 1] = '\0';
                                                strncat(hs, "...", 4);
                                        }
                                        v.valueStr = hs;
                                        continue; // outer for(auto& v : vars_)
                                }
                                std::memcpy(buf, vd.data(), std::min<size_t>(sz, vd.size()));
                                ret = 0;
                        }
                }

                if (ret == 0) {
                        v.displayByteSize = std::min<u32>(sz, static_cast<u32>(v.displayBytes.size()));
                        std::copy_n(buf, v.displayByteSize, v.displayBytes.begin());
                        v.displayNumericValue     = unitConversionNumber(buf, v.type, v.bitOffset, v.bitSize);
                        const dwarf::Type *et     = resolveAlias(dwarfInfo_, v.typeOff);
                        const bool         isEnum = (et && et->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty();
                        if (isEnum && variableRadix(v) == 10) {
                                i64 ival = 0;
                                std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                                v.valueStr = decodeValue(buf, v.type, v.bitOffset, v.bitSize, variableRadix(v));
                                // User-defined defs take priority over DWARF
                                bool found = false;
                                for (const auto &e : v.enumDefs)
                                        if (e.val == ival) {
                                                v.valueStr = e.name;
                                                found      = true;
                                                break;
                                        }
                                if (!found && et && et->kind == dwarf::TypeKind::ENUM)
                                        for (const auto &e : et->enums)
                                                if (e.val == ival) {
                                                        v.valueStr = e.name;
                                                        break;
                                                }
                        } else {
                                v.valueStr = decodeValue(buf, v.type, v.bitOffset, v.bitSize, variableRadix(v));
                        }
                        v.prevGoodValueStr = v.valueStr;
                } else if (ret == -1) {
                        // A failed J-Link poll often races with disconnect. Keep
                        // the prev complete frame in that case instead of
                        // replacing the cached display value with "ERR"
                        if (v.port != PortType::JLINK || JLinkPort::instance().isConnected()) {
                                v.valueStr = "ERR";
                                v.displayNumericValue.reset();
                        }
                } else if (v.port == PortType::JLINK && !JLinkPort::instance().isConnected() && !v.prevGoodValueStr.empty())
                        v.valueStr = v.prevGoodValueStr;
                else if (v.valueStr.empty())
                        v.valueStr = "...";
        }
}

void
Variable::writeVariable(const VarEntry &v, const std::string &newVal)
{
        if (v.flashArray)
                return;
        u8  buf[8]{};
        u32 sz = Parser::typeBytes(v.type);
        try {
                if (v.port == PortType::JLINK && parameterDevice_.guardedFoc && v.addr >= parameterDevice_.address &&
                    v.addr - parameterDevice_.address < parameterDevice_.bytes.size())
                        throw std::runtime_error("Open the complete parameter editor to write configuration");
                if (v.type == DataType::F16)
                        *(u16 *)buf = fast_f32_to_f16(std::stof(newVal));
                else if (v.type == DataType::F32)
                        *(f32 *)buf = std::stof(newVal);
                else if (v.type == DataType::U32)
                        *(u32 *)buf = (u32)std::stoul(newVal);
                else if (v.type == DataType::I32)
                        *(i32 *)buf = (i32)std::stol(newVal);
                else if (v.type == DataType::U16)
                        *(u16 *)buf = (u16)std::stoul(newVal);
                else if (v.type == DataType::I16)
                        *(i16 *)buf = (i16)std::stol(newVal);
                else if (v.type == DataType::U8)
                        *(u8 *)buf = (u8)std::stoul(newVal);
                else if (v.type == DataType::I8)
                        *(i8 *)buf = (i8)std::stol(newVal);
                else if (v.type == DataType::F64)
                        *(f64 *)buf = std::stod(newVal);
                else if (v.type == DataType::U64)
                        *(u64 *)buf = std::stoull(newVal);
                else if (v.type == DataType::I64)
                        *(i64 *)buf = std::stoll(newVal);

                if (v.port == PortType::JLINK && JLinkPort::instance().isConnected()) {
                        if (v.bitSize > 0)
                                JLinkPort::instance().writeMemBitfield(
                                    static_cast<u32>(v.addr), sz, buf, v.bitOffset, v.bitSize);
                        else
                                JLinkPort::instance().writeMem(static_cast<u32>(v.addr), sz, buf);
                } else if (v.port == PortType::SHM && v.shm.inited) {
                        shm_write(const_cast<shm_t *>(&v.shm.handle), buf, sz);
                } else if (v.port == PortType::LOCAL) {
                        {
                                std::lock_guard<std::mutex> lk(mtxLocal_);
                                auto                       &vec = localBufs_[v.name];
                                if (vec.empty())
                                        vec.resize(8, 0); // fallback
                                std::memcpy(vec.data(), buf, std::min<size_t>(sz, vec.size()));
                        }
                        FunctionSource *source = nullptr;
                        if (const CFuncDecl *fn = findFunction(v.writeFunction, &source); fn && isWriteAccessor(*fn, v.type)) {
                                startAccessorThread();
                                AccessWriteReq request;
                                request.name      = v.name;
                                request.type      = v.type;
                                request.function  = *fn;
                                request.source    = source;
                                request.size      = std::min<u32>(sz, static_cast<u32>(request.val.size()));
                                request.valueText = newVal;
                                std::memcpy(request.val.data(), buf, request.size);
                                {
                                        std::lock_guard<std::mutex> lk(accessor_->mtx);
                                        accessor_->writes.push_back(std::move(request));
                                }
                                accessor_->cv.notify_one();
                        }
                }
        } catch (...) {
                ImGui::InsertNotification({ImGuiToastType::Error, 2000, "Invalid input for %s", v.name.c_str()});
        }
}

void
Variable::openRewrite(const VarEntry &variable)
{
        // Called from menus that may already hold mtxElf_ or mtxLocal_. No locking or I/O here
        rewrite_              = {};
        rewrite_.source       = variable;
        rewrite_.ownerName    = variable.name;
        rewrite_.ownerAddress = variable.addr;
        rewrite_.pending      = true;
}

void
Variable::openRewriteMember(
    int parent, u64 addr, u64 typeOff, const std::string &name, DataType overrideType, u32 bitOffset, u32 bitSize)
{
        if (parent < 0 || parent >= static_cast<int>(vars_.size()))
                return;
        openRewrite(vars_[parent]);
        auto &v     = rewrite_.source;
        v.name      = name;
        v.addr      = addr;
        v.typeOff   = overrideType == DataType::UNKNOWN ? typeOff : 0;
        v.type      = overrideType;
        v.bitOffset = bitOffset;
        v.bitSize   = bitSize;
        v.structFields.clear();
        v.enumDefs.clear();
        if (auto it = vars_[parent].memberEnumDefs.find(name); it != vars_[parent].memberEnumDefs.end())
                v.enumDefs = it->second;
}

void
Variable::prepareRewrite()
{
        auto &d   = rewrite_;
        d.pending = false;
        d.open = d.requestOpen = true;
        d.address              = d.source.addr;
        d.variableName         = d.source.name;
        d.typeOff              = d.source.typeOff;
        try {
                {
                        std::lock_guard lk(mtxElf_);
                        d.layoutGeneration = elfLayoutGeneration_;
                        // Copy only reachable types; ELF reloads cannot invalidate an open editor
                        std::function<void(u64, int)> capture = [&](u64 off, int depth) {
                                if (!off || depth > 24 || d.info.types.contains(off))
                                        return;
                                auto it = dwarfInfo_.types.find(off);
                                if (it == dwarfInfo_.types.end())
                                        return;
                                const auto type = it->second;
                                d.info.types.emplace(off, type);
                                capture(type.inner, depth + 1);
                                for (const auto &m : type.members)
                                        capture(m.type, depth + 1);
                        };
                        capture(d.typeOff, 0);
                }
                u64  nextType = UINT64_MAX;
                auto scalar   = [&](DataType dt, const std::vector<VarEntry::EnumDef> &enums) {
                        dwarf::Type t;
                        t.size     = Parser::typeBytes(dt);
                        t.name     = Parser::dataTypeToStr(dt);
                        t.kind     = enums.empty() ? dwarf::TypeKind::BASE : dwarf::TypeKind::ENUM;
                        t.encoding = dt == DataType::F16 || dt == DataType::F32 || dt == DataType::F64 ? 4
                                       : dt == DataType::I8 || dt == DataType::I16 || dt == DataType::I32 || dt == DataType::I64
                                           ? 5
                                           : 7;
                        for (const auto &e : enums)
                                t.enums.push_back({e.name, e.val});
                        const auto off    = nextType--;
                        d.info.types[off] = std::move(t);
                        return off;
                };
                if (!d.typeOff) {
                        if (d.source.flashArray) {
                                static constexpr DataType types[] = {DataType::F16,
                                                                     DataType::F32,
                                                                     DataType::F64,
                                                                     DataType::I8,
                                                                     DataType::U8,
                                                                     DataType::I16,
                                                                     DataType::U16,
                                                                     DataType::I32,
                                                                     DataType::U32,
                                                                     DataType::I64,
                                                                     DataType::U64};
                                const auto                dt      = types[static_cast<int>(d.source.flashArray->type)];
                                const auto                inner   = scalar(dt, {});
                                if (d.variableName == d.ownerName) {
                                        dwarf::Type t;
                                        t.kind                  = dwarf::TypeKind::ARRAY;
                                        t.inner                 = inner;
                                        t.size                  = d.source.flashArray->bytes.size();
                                        d.typeOff               = nextType--;
                                        d.info.types[d.typeOff] = std::move(t);
                                } else
                                        d.typeOff = inner;
                        } else if (!d.source.structFields.empty()) {
                                dwarf::Type t;
                                t.kind = d.source.isUnion ? dwarf::TypeKind::UNION : dwarf::TypeKind::STRUCT;
                                t.name = d.source.typeStr;
                                for (const auto &f : d.source.structFields) {
                                        const auto off = scalar(f.type, {});
                                        t.members.push_back({f.name, f.byteOffset, off, 0, 0});
                                        t.size = std::max<u64>(t.size, u64(f.byteOffset) + Parser::typeBytes(f.type));
                                }
                                d.typeOff               = nextType--;
                                d.info.types[d.typeOff] = std::move(t);
                        } else {
                                d.typeOff = scalar(d.source.type, d.source.enumDefs);
                        }
                }
                const auto *type = resolveAlias(d.info, d.typeOff);
                const u64   size = type ? type->size : 0;
                d.isFocCfg = d.guardedFoc = false;
                d.stateAddress            = 0;
                d.stateSize               = 0;
                // 参数描述块按地址关联配置和状态变量, 不依赖符号名称
                try {
                        const auto &image = parameterFirmwareIdentity();
                        d.guardedFoc      = true;
                        d.isFocCfg        = d.address == image.field(60) && size == image.field(12);
                        d.stateAddress    = image.field(64);
                        d.stateSize       = 4;
                } catch (const std::runtime_error &error) {
                        if (std::string_view(error.what()) != "No parameter descriptor for selected variable")
                                throw;
                }
                // 无描述块的本地配置由所选 JSON 模板识别, 不按结构体或变量名称判断
                if (!d.isFocCfg && type && type->kind == dwarf::TypeKind::STRUCT) {
                        const auto name = std::find_if(type->members.begin(), type->members.end(), [](const auto &member) {
                                return member.name == "name";
                        });
                        if (name != type->members.end()) {
                                const auto *header = resolveAlias(d.info, name->type);
                                const auto *schema = parameterPreset("param_list");
                                if (header && header->kind == dwarf::TypeKind::ARRAY && header->size == 64 && schema &&
                                    parameter_json::completeComponents(*schema) &&
                                    (schema->target == d.variableName || parameterTargetOverride_ == d.variableName))
                                        d.isFocCfg = true;
                        }
                }
                if (!size || size > variable_rewrite::maxBytes ||
                    (d.source.port == PortType::JLINK && (d.address > UINT32_MAX || size - 1 > UINT32_MAX - d.address)) ||
                    (d.source.bitSize &&
                     !variable_rewrite::mask(static_cast<unsigned>(size), d.source.bitOffset, d.source.bitSize))) {
                        d.status = tr("Invalid or oversized variable layout (limit: 1 MiB)",
                                      "变量类型或地址无效，或超过 1 MiB 编辑上限");
                        return;
                }
                d.bytes.resize(static_cast<size_t>(size));
                reloadRewrite();
        } catch (const std::exception &e) {
                d.status   = std::string(tr("Cannot open editor: ", "无法打开编辑器：")) + parameterError(e.what());
                d.statusOk = d.targetLoaded = false;
        }
}

const firmware_identity::Image &
Variable::parameterFirmwareIdentity()
{
        std::lock_guard lock(mtxElf_);
        const auto      matches = [&](const firmware_identity::Image &image) {
                if (!rewrite_.typeOff)
                        return true;
                const auto size = typeSize(rewrite_.info, rewrite_.typeOff);
                return size && rewrite_.address >= image.field(60) &&
                       variable_rewrite::range(rewrite_.address - image.field(60), size, image.field(12));
        };
        if (parameterFirmwareImage_ && parameterFirmwareGeneration_ == elfLayoutGeneration_ &&
            matches(*parameterFirmwareImage_))
                return *parameterFirmwareImage_;
        const auto path = std::filesystem::u8path(remote::isUrl(elfPath_) ? elfLocalPath_ : elfPath_);
        for (const auto &symbol : elfInfo_.symbols) {
                if (symbol.type != 1 || symbol.addr > UINT32_MAX || symbol.size != 92)
                        continue;
                std::optional<firmware_identity::Image> candidate;
                try {
                        candidate = firmware_identity::Image::load(path, static_cast<uint32_t>(symbol.addr));
                } catch (const std::runtime_error &error) {
                        if (std::string_view(error.what()) != "AXF has no supported FOC parameter identity; rebuild firmware")
                                throw;
                        continue;
                }
                if (!matches(*candidate))
                        continue;
                parameterFirmwareImage_      = std::move(candidate);
                parameterFirmwareGeneration_ = elfLayoutGeneration_;
                return *parameterFirmwareImage_;
        }
        throw std::runtime_error("No parameter descriptor for selected variable");
}

void
Variable::validateParameterManifest()
{
        if (parameterManifestCache_.empty())
                return;
        parameter_json::Json manifest(cJSON_Parse(parameterManifestCache_.c_str()), cJSON_Delete);
        auto                 number = [&](const char *key) -> u32 {
                const auto *val = parameter_json::get(manifest.get(), key);
                if (!cJSON_IsNumber(val) || !std::isfinite(val->valuedouble) || val->valuedouble < 0 ||
                    val->valuedouble > UINT32_MAX || val->valuedouble != std::floor(val->valuedouble))
                        throw std::runtime_error("Invalid parameter identity metadata");
                return static_cast<u32>(val->valuedouble);
        };
        const auto *entries = parameter_json::get(manifest.get(), "parameters");
        if (!cJSON_IsArray(entries))
                throw std::runtime_error("Invalid parameter manifest");
        u32 count = 0;
        for (auto *entry = entries->child; entry; entry = entry->next)
                if (!cJSON_IsTrue(parameter_json::get(entry, "retired")))
                        ++count;
        parameterFirmwareIdentity().validateManifest(
            number("parameter_schema_version"), number("cfg_size"), number("layout_fingerprint"), count);
}

void
Variable::verifyRewriteFirmware(bool forWrite)
{
        if (!rewrite_.guardedFoc || rewrite_.source.port != PortType::JLINK)
                return;
        if (rewrite_.layoutGeneration != elfLayoutGeneration_)
                throw std::runtime_error("AXF changed; reopen the parameter editor");
        const auto &image = parameterFirmwareIdentity();
        if (rewrite_.address < image.field(60) ||
            !variable_rewrite::range(rewrite_.address - image.field(60), rewrite_.bytes.size(), image.field(12)) ||
            rewrite_.stateAddress != image.field(64) || rewrite_.stateSize != 4)
                throw std::runtime_error("FOC parameter/state address does not match AXF identity");
        validateParameterManifest();
        const auto epoch = JLinkPort::instance().targetEpoch();
        if (forWrite && epoch != rewrite_.targetEpoch)
                throw std::runtime_error("Device connection changed; read again");
        image.verify([](uint32_t address, uint32_t size, uint8_t *bytes) {
                return JLinkPort::instance().readMem(address, size, bytes);
        });
        if (epoch != JLinkPort::instance().targetEpoch() || rewrite_.layoutGeneration != elfLayoutGeneration_)
                throw std::runtime_error("Device connection or AXF changed during firmware verification");
        if (forWrite) {
                uint32_t state = UINT32_MAX;
                if (!JLinkPort::instance().readMem(image.field(64), sizeof(state), &state) || (state != 0 && state != 3))
                        throw std::runtime_error("FOC must be DISABLE or FAULT before writing parameters");
        }
}

bool
Variable::readRewrite(std::vector<u8> &bytes)
{
        verifyRewriteFirmware(false);
        auto &d = rewrite_;
        if (bytes.empty())
                return false;
        if (d.source.flashArray) {
                const auto owner =
                    std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) { return v.name == d.ownerName; });
                if (owner == vars_.end() || !owner->flashArray || d.address < d.ownerAddress)
                        return false;
                const auto &snapshot = *owner->flashArray;
                if (!variable_rewrite::range(d.address - d.ownerAddress, bytes.size(), snapshot.bytes.size()))
                        return false;
                std::memcpy(bytes.data(), snapshot.bytes.data() + d.address - d.ownerAddress, bytes.size());
                const auto width = flasharray::width(snapshot.type);
                if (snapshot.bigEndian)
                        for (size_t i = 0; i < bytes.size(); i += width)
                                std::reverse(bytes.begin() + i, bytes.begin() + i + width);
                return true;
        }
        if (d.source.port == PortType::JLINK)
                return !d.source.addrUnknown && JLinkPort::instance().isConnected() &&
                       JLinkPort::instance().readMem(static_cast<u32>(d.address), static_cast<u32>(bytes.size()), bytes.data());
        if (d.source.port == PortType::LOCAL && !d.source.flashArray) {
                if (d.address < d.ownerAddress)
                        return false;
                std::lock_guard lk(mtxLocal_);
                auto            it     = localBufs_.find(d.ownerName);
                const auto      offset = d.address - d.ownerAddress;
                if (it == localBufs_.end() || !variable_rewrite::range(offset, bytes.size(), it->second.size()))
                        return false;
                std::memcpy(bytes.data(), it->second.data() + offset, bytes.size());
                return true;
        }
        // UDP/audio are sample streams; SHM is a FIFO, not addressable memory
        // Never consume a sample or pretend to verify writes against these sources
        return false;
}

void
Variable::reloadRewrite(bool preserveEdits)
{
        auto &d = rewrite_;
        try {
                if (d.source.port == PortType::JLINK && d.layoutGeneration != elfLayoutGeneration_)
                        throw std::runtime_error(
                            tr("AXF changed; reopen this variable before reading", "AXF 已变更，请重新打开此变量后读取"));
                std::vector<u8> bytes(d.bytes.size());
                const auto      epoch = JLinkPort::instance().targetEpoch();
                const bool      ok =
                    readRewrite(bytes) && (d.source.port != PortType::JLINK || epoch == JLinkPort::instance().targetEpoch());
                d.targetLoaded = ok;
                d.statusOk     = ok;
                if (ok) {
                        std::vector<u8> preserved;
                        const bool      keep = preserveEdits && d.original.size() == d.bytes.size() && d.bytes != d.original;
                        if (keep) {
                                std::unordered_map<std::string, RewriteLeaf> leaves;
                                collectRewriteLeaves(d.info, d.typeOff, 0, "", leaves, 0, d.source.bitOffset, d.source.bitSize);
                                if (d.isFocCfg)
                                        mergeManifestLeaves(leaves, parameterManifestCache_);
                                if (leaves.size() >= 65536)
                                        throw std::runtime_error(tr("Too many fields; export your edits before reading again",
                                                                    "字段过多，请先导出编辑内容再重新读取"));
                                std::vector<u8> masks(d.bytes.size(), 0);
                                for (const auto &[path, leaf] : leaves) {
                                        const auto size = Parser::typeBytes(leaf.type);
                                        if (!size || size > 8 || !variable_rewrite::range(leaf.offset, size, masks.size()))
                                                continue;
                                        u64 before = 0, after = 0;
                                        std::memcpy(&before, d.original.data() + leaf.offset, size);
                                        std::memcpy(&after, d.bytes.data() + leaf.offset, size);
                                        const auto mask = variable_rewrite::mask(
                                            size, leaf.bitSize ? leaf.bitOffset : 0, leaf.bitSize ? leaf.bitSize : size * 8);
                                        if (((before ^ after) & mask) != 0)
                                                for (u32 i = 0; i < size; ++i)
                                                        masks[leaf.offset + i] |= static_cast<u8>(mask >> (8 * i));
                                }
                                for (size_t i = 0; i < masks.size(); ++i)
                                        if (((d.original[i] ^ d.bytes[i]) & ~masks[i]) != 0)
                                                throw std::runtime_error(
                                                    tr("An edited byte has no field layout; preview kept. Export it before "
                                                       "discarding edits.",
                                                       "部分改动缺少字段布局，预览已保留。请先导出，再放弃改动重新读取。"));
                                if (!variable_rewrite::merge(d.original, d.bytes, bytes, masks, preserved)) {
                                        d.statusOk = false;
                                        d.status =
                                            tr("An edited field also changed on the target. Preview kept; export it or discard "
                                               "edits and read again.",
                                               "编辑字段在目标上也发生了变化。预览已保留；可先导出，或放弃改动后重新读取。");
                                        return;
                                }
                        }
                        if (d.isFocCfg) {
                                parameter_json::Document prev, current;
                                prev.name                                               = d.profileName;
                                const auto                                  &namedBytes = keep ? preserved : bytes;
                                std::unordered_map<std::string, RewriteLeaf> leaves;
                                collectRewriteLeaves(d.info, d.typeOff, 0, "", leaves, 0, 0, 0, true);
                                for (size_t i = 0; i < 64; ++i) {
                                        const auto leaf = leaves.find("name[" + std::to_string(i) + "]");
                                        if (leaf == leaves.end() || leaf->second.offset >= namedBytes.size())
                                                throw std::runtime_error("Missing configuration name");
                                        const auto val = namedBytes[leaf->second.offset];
                                        if (!val)
                                                break;
                                        current.name.push_back(static_cast<char>(val));
                                }
                                auto       before = parameter_json::componentNames(prev);
                                const auto after  = parameter_json::componentNames(current);
                                if (!keep && d.source.port == PortType::JLINK && epoch != d.targetEpoch)
                                        before.clear();
                                std::erase_if(d.jsonFields, [&](const auto &entry) {
                                        // Root-level fields belong to no component, so a
                                        // component rename never invalidates them
                                        if (parameter_json::isRootField(entry.first))
                                                return false;
                                        const auto section = entry.first.substr(0, entry.first.find('.'));
                                        const auto oldName = before.find(section);
                                        const auto newName = after.find(section);
                                        return oldName == before.end() || newName == after.end() ||
                                               parameter_json::baseName(oldName->second) !=
                                                   parameter_json::baseName(newName->second);
                                });
                                d.profileName = current.name;
                        }
                        d.original = std::move(bytes);
                        d.bytes    = keep ? std::move(preserved) : d.original;
                        d.nameComparedBytes.clear();
                        d.targetEpoch = epoch;
                        d.preset      = -1;
                        d.status = d.source.flashArray ? tr("Editing a local array snapshot; this does not write device Flash",
                                                            "正在编辑本地数组快照，不会写入设备 Flash")
                                                       : tr("Edit values below, then click Write changes",
                                                            "修改下方数值，完成后点击“写入改动”");
                        const bool nameReady = !d.isFocCfg || syncRewriteName();
                        if (keep && nameReady)
                                d.status = tr("Current values refreshed; your edits are preserved and have not been written",
                                              "当前值已刷新，编辑内容已保留，尚未写入设备");
                } else {
                        d.status =
                            d.source.port == PortType::JLINK
                                ? tr("Read failed. Connect the target and check the address, then retry.",
                                     "读取失败，请连接目标并确认地址后重试。")
                            : d.source.port == PortType::LOCAL && !d.source.flashArray
                                ? tr("Local variable no longer exists or its layout changed", "本地变量已不存在或布局已变化")
                                : tr("This source has no random-access write interface. Use a J-Link or LOCAL variable.",
                                     "此数据源没有按地址重写接口，请使用 J-Link 或 LOCAL 变量。");
                }
        } catch (const std::exception &e) {
                d.targetLoaded = d.statusOk = false;
                d.status                    = std::string(tr("Read failed: ", "读取失败：")) + parameterError(e.what());
        }
}

bool
Variable::writeRewrite(const std::vector<u8> &bytes, const std::vector<u8> &masks)
{
        verifyRewriteFirmware(true);
        auto &d = rewrite_;
        if (d.source.flashArray) {
                auto owner = std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) { return v.name == d.ownerName; });
                if (owner == vars_.end() || !owner->flashArray || d.address < d.ownerAddress)
                        return false;
                const auto &snapshot = *owner->flashArray;
                if (!variable_rewrite::range(d.address - d.ownerAddress, bytes.size(), snapshot.bytes.size()))
                        return false;
                auto       raw    = snapshot.bytes;
                auto       edited = bytes;
                const auto width  = flasharray::width(snapshot.type);
                if (snapshot.bigEndian)
                        for (size_t i = 0; i < edited.size(); i += width)
                                std::reverse(edited.begin() + i, edited.begin() + i + width);
                std::memcpy(raw.data() + d.address - d.ownerAddress, edited.data(), edited.size());
                owner->flashArray = flasharray::decode(
                    std::move(raw), snapshot.address, snapshot.type, snapshot.bigEndian, snapshot.step, snapshot.name);
                isModified_ = true;
                return static_cast<bool>(owner->flashArray);
        }
        if (d.source.port == PortType::JLINK) {
                for (size_t i = 0; i < bytes.size();) {
                        if (!masks[i]) {
                                ++i;
                                continue;
                        }
                        const size_t start = i++;
                        while (i < bytes.size() && masks[i])
                                ++i;
                        if (d.targetEpoch != JLinkPort::instance().targetEpoch() || d.layoutGeneration != elfLayoutGeneration_)
                                return false;
                        if (!JLinkPort::instance().writeMem(
                                static_cast<u32>(d.address + start), static_cast<u32>(i - start), bytes.data() + start))
                                return false;
                }
                return true;
        }
        if (d.source.port != PortType::LOCAL)
                return false;
        auto owner = std::find_if(
            vars_.begin(), vars_.end(), [&](const auto &v) { return v.name == d.ownerName && v.port == PortType::LOCAL; });
        if (owner == vars_.end())
                return false;
        {
                std::lock_guard lk(mtxLocal_);
                auto            it = localBufs_.find(d.ownerName);
                if (d.address < d.ownerAddress || it == localBufs_.end() ||
                    !variable_rewrite::range(d.address - d.ownerAddress, bytes.size(), it->second.size()))
                        return false;
                // Recheck under the local lock so concurrent SDK writes cannot be overwritten
                std::vector<u8> current(bytes.size()), merged;
                std::memcpy(current.data(), it->second.data() + d.address - d.ownerAddress, current.size());
                if (!variable_rewrite::merge(d.original, bytes, current, masks, merged))
                        return false;
                std::memcpy(it->second.data() + d.address - d.ownerAddress, merged.data(), merged.size());
        }
        if (d.ownerName == d.variableName) {
                FunctionSource *source = nullptr;
                if (const auto *fn = findFunction(owner->writeFunction, &source); fn && isWriteAccessor(*fn, owner->type)) {
                        startAccessorThread();
                        AccessWriteReq request;
                        request.name     = owner->name;
                        request.type     = owner->type;
                        request.function = *fn;
                        request.source   = source;
                        request.size     = static_cast<u32>(std::min(bytes.size(), request.val.size()));
                        std::memcpy(request.val.data(), bytes.data(), request.size);
                        request.valueText = decodeValue(bytes.data(), owner->type);
                        {
                                std::lock_guard lk(accessor_->mtx);
                                accessor_->writes.push_back(std::move(request));
                        }
                        accessor_->cv.notify_one();
                }
        }
        notifyLocalWrite(d.ownerName);
        return true;
}

void
Variable::drawRewriteNode(
    const u64 typeOff, const size_t offset, const std::string &path, const int depth, const u32 bitOffset, const u32 bitSize)
{
        auto              &dialog = rewrite_;
        const dwarf::Type *type   = resolveAlias(rewrite_.info, typeOff);
        if (!type || depth > 24 || !variable_rewrite::range(offset, type->size, dialog.bytes.size()))
                return;

        ui::HierarchyRow(ImMax(0, depth - 1));
        ui::HierarchyCard card;
        if (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION) {
                ImGui::PushID(path.c_str());
                if (dialog.expandMode >= 0)
                        ImGui::SetNextItemOpen(dialog.expandMode != 0);
                else if (depth == 0)
                        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
                const bool open = ui::HierarchyNode(path.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                ui::HierarchyRow(open ? depth : ImMax(0, depth - 1));
                if (depth == 0 || open)
                        card.begin(depth);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", prettyType(rewrite_.info, typeOff).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu B", static_cast<unsigned long long>(type->size));
                if (open) {
                        for (const auto &member : type->members) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawRewriteNode(member.type,
                                                offset + member.offset,
                                                member.name,
                                                depth + 1,
                                                member.bitOffset,
                                                member.bitSize);
                        }
                        ImGui::TreePop();
                }
                ImGui::PopID();
                return;
        }
        if (type->kind == dwarf::TypeKind::ARRAY) {
                const u64 elemSize = typeSize(rewrite_.info, type->inner);
                const u64 count    = elemSize ? typeSize(dialog.info, typeOff) / elemSize : 0;
                ImGui::PushID(path.c_str());
                if (dialog.expandMode >= 0)
                        ImGui::SetNextItemOpen(dialog.expandMode != 0);
                else if (depth == 0)
                        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
                const bool open = ui::HierarchyNode(path.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                ui::HierarchyRow(open ? depth : ImMax(0, depth - 1));
                if (depth == 0 || open)
                        card.begin(depth);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", prettyType(rewrite_.info, typeOff).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", static_cast<unsigned long long>(count));
                if (open) {
                        int      &page  = dialog.arrayPages[path + "@" + std::to_string(offset)];
                        const int pages = static_cast<int>((count + 127) / 128);
                        if (pages > 1) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::SetNextItemWidth(140);
                                ImGui::SliderInt("Page", &page, 0, pages - 1);
                        }
                        page = std::clamp(page, 0, std::max(0, pages - 1));
                        for (u64 i = static_cast<u64>(page) * 128; i < std::min(count, static_cast<u64>(page + 1) * 128); ++i) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawRewriteNode(type->inner,
                                                offset + static_cast<size_t>(i * elemSize),
                                                "[" + std::to_string(i) + "]",
                                                depth + 1);
                        }
                        ImGui::TreePop();
                }
                ImGui::PopID();
                return;
        }

        std::string needle   = dialog.search;
        std::string haystack = path;
        std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return std::tolower(c); });
        std::ranges::transform(haystack, haystack.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!needle.empty() && haystack.find(needle) == std::string::npos)
                return;

        std::string jsonPath = path;
        if (jsonPath == dialog.variableName)
                jsonPath.clear();
        else if (jsonPath.starts_with(dialog.variableName + "."))
                jsonPath.erase(0, dialog.variableName.size() + 1);
        else if (jsonPath.starts_with(dialog.variableName + "["))
                jsonPath.erase(0, dialog.variableName.size());
        if (auto field = dialog.jsonFields.find(jsonPath);
            field != dialog.jsonFields.end() && field->second.role == "derived") {
                ImGui::TextUnformatted(path.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(tr("See runtime state", "见运行态"));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(tr("Calculated at runtime", "运行时计算"));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s", field->second.expression.c_str(), field->second.runtimePath.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(prettyType(rewrite_.info, typeOff).c_str());
                return;
        }
        ImGui::TextUnformatted(path.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%s", prettyType(rewrite_.info, typeOff).c_str());
        ImGui::TableSetColumnIndex(1);
        if (const char *st = scalarPayloadType(dialog.info, typeOff);
            st && variable_rewrite::range(offset, type->size, dialog.original.size())) {
                const auto dt =
                    type->kind == dwarf::TypeKind::POINTER && type->size == 8 ? DataType::U64 : Parser::strToDataType(st);
                if (Parser::typeBytes(dt) == type->size) {
                        const auto  field    = dialog.jsonFields.find(jsonPath);
                        std::string original = rewriteValue(dialog.original.data() + offset, dt, bitOffset, bitSize);
                        if (bitSize == 1 || type->encoding == 2 ||
                            (field != dialog.jsonFields.end() &&
                             (field->second.display == "checkbox" || field->second.row.type == "bool"))) {
                                bool checked = original != "0" && original != "false";
                                ImGui::PushID(path.c_str());
                                ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
                                ImGui::BeginDisabled();
                                ImGui::Checkbox("##original", &checked);
                                ImGui::EndDisabled();
                                ImGui::PopStyleVar();
                                ImGui::PopID();
                        } else {
                                if (field != dialog.jsonFields.end())
                                        for (const auto &[name, val] : field->second.enums)
                                                if (val == original) {
                                                        original = name;
                                                        break;
                                                }
                                ImGui::TextUnformatted(original.c_str());
                        }
                }
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::PushID(path.c_str());
        ImGui::PushID(static_cast<int>(offset));
        void      *val      = dialog.bytes.data() + offset;
        const auto metadata = dialog.jsonFields.find(jsonPath);
        const bool checkbox = bitSize == 1 || type->encoding == 2 ||
                              (metadata != dialog.jsonFields.end() &&
                               (metadata->second.display == "checkbox" || metadata->second.row.type == "bool"));
        if (checkbox) {
                const char *payload = scalarPayloadType(dialog.info, typeOff);
                const auto  dt      = payload ? Parser::strToDataType(payload) : DataType::UNKNOWN;
                bool        checked = rewriteValue(static_cast<u8 *>(val), dt, bitOffset, bitSize) != "0";
                if (ImGui::Checkbox("##value", &checked)) {
                        if (bitSize)
                                variable_rewrite::storeBits(
                                    static_cast<u8 *>(val), type->size, bitOffset, bitSize, checked ? 1 : 0);
                        else {
                                u8 raw[8]{};
                                if (encodeScalar(checked ? "1" : "0", dt, raw))
                                        std::memcpy(val, raw, type->size);
                        }
                }
                ImGui::PopID();
                ImGui::PopID();
                return;
        }
        if (auto field = dialog.jsonFields.find(jsonPath); field != dialog.jsonFields.end() && !field->second.enums.empty()) {
                const auto *payload = scalarPayloadType(dialog.info, typeOff);
                const auto  dt      = payload ? Parser::strToDataType(payload) : DataType::UNKNOWN;
                std::string current = rewriteValue(static_cast<u8 *>(val), dt, bitOffset, bitSize), label = current;
                for (const auto &[name, v] : field->second.enums)
                        if (v == current)
                                label = name;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##value", label.c_str())) {
                        for (const auto &[name, v] : parameter_json::enumOptionsByValue(field->second))
                                if (ImGui::Selectable(name.c_str(), v == current)) {
                                        u8 raw[8]{};
                                        if (encodeScalar(v, dt, raw)) {
                                                if (!bitSize)
                                                        std::memcpy(val, raw, type->size);
                                                else if (dt == DataType::I8 || dt == DataType::I16 || dt == DataType::I32 ||
                                                         dt == DataType::I64) {
                                                        i64 n{};
                                                        if (variable_rewrite::integer<i64>(v, reinterpret_cast<u8 *>(&n)))
                                                                variable_rewrite::storeSignedBits(
                                                                    static_cast<u8 *>(val), type->size, bitOffset, bitSize, n);
                                                } else {
                                                        u64 n{};
                                                        if (variable_rewrite::integer<u64>(v, reinterpret_cast<u8 *>(&n)))
                                                                variable_rewrite::storeBits(
                                                                    static_cast<u8 *>(val), type->size, bitOffset, bitSize, n);
                                                }
                                        }
                                }
                        ImGui::EndCombo();
                }
                ImGui::PopID();
                ImGui::PopID();
                return;
        }
        if (bitSize > 0 && (type->kind == dwarf::TypeKind::BASE || type->kind == dwarf::TypeKind::ENUM)) {
                // Bitfield: expose only this field's bits, read-modify-write the storage unit
                // so sibling fields sharing the same word are preserved
                const u32 storageBits = static_cast<u32>(std::min<size_t>(type->size, sizeof(u64)) * 8);
                if (!variable_rewrite::mask(static_cast<unsigned>(type->size), bitOffset, bitSize)) {
                        ImGui::Text("Invalid bitfield");
                        ImGui::PopID();
                        ImGui::PopID();
                        return;
                }
                const u32 width     = bitSize;
                const u64 fieldMask = (width >= 64) ? ~u64{0} : ((u64{1} << width) - 1);
                const u32 shift     = std::min(bitOffset, storageBits);
                u64       storage   = 0;
                std::memcpy(&storage, val, std::min<size_t>(type->size, sizeof(storage)));
                u64 field = (shift < storageBits) ? ((storage >> shift) & fieldMask) : 0;
                ImGui::PushID(static_cast<int>(bitOffset));
                bool changed = false;
                if ((type->encoding == 5 || type->encoding == 6) && width > 1) {
                        i64 signedField =
                            width == 64 ? static_cast<i64>(field) : static_cast<i64>(field << (64 - width)) >> (64 - width);
                        changed           = ImGui::InputScalar("##value", ImGuiDataType_S64, &signedField);
                        const i64 minimum = width == 64 ? INT64_MIN : -(i64{1} << (width - 1));
                        const i64 maximum = width == 64 ? INT64_MAX : (i64{1} << (width - 1)) - 1;
                        field             = static_cast<u64>(std::clamp(signedField, minimum, maximum)) & fieldMask;
                } else if (width == 1) {
                        bool b  = field != 0;
                        changed = ImGui::Checkbox("##value", &b);
                        field   = b ? 1 : 0;
                } else if (width <= 8) {
                        u8 v    = static_cast<u8>(field);
                        changed = ImGui::InputScalar("##value", ImGuiDataType_U8, &v);
                        field   = v;
                } else if (width <= 16) {
                        u16 v   = static_cast<u16>(field);
                        changed = ImGui::InputScalar("##value", ImGuiDataType_U16, &v);
                        field   = v;
                } else if (width <= 32) {
                        u32 v   = static_cast<u32>(field);
                        changed = ImGui::InputScalar("##value", ImGuiDataType_U32, &v);
                        field   = v;
                } else {
                        u64 v   = field;
                        changed = ImGui::InputScalar("##value", ImGuiDataType_U64, &v);
                        field   = v;
                }
                if (changed) {
                        field = std::min(field, fieldMask);
                        variable_rewrite::storeBits(static_cast<u8 *>(val), type->size, bitOffset, bitSize, field);
                }
                ImGui::PopID();
                ImGui::PopID();
                ImGui::PopID();
                return;
        }
        if (type->kind == dwarf::TypeKind::ENUM) {
                i64 current = 0;
                std::memcpy(&current, val, std::min<size_t>(type->size, sizeof(current)));
                if (type->size < sizeof(current) && type->size > 0 &&
                    std::any_of(type->enums.begin(), type->enums.end(), [](const auto &e) { return e.val < 0; })) {
                        const unsigned shift = 64 - static_cast<unsigned>(type->size * 8);
                        current              = static_cast<i64>(static_cast<u64>(current) << shift) >> shift;
                }
                const char *preview = nullptr;
                for (const auto &entry : type->enums)
                        if (entry.val == current)
                                preview = entry.name.c_str();
                const std::string fallback = std::to_string(current);
                if (ImGui::BeginCombo("##value", preview ? preview : fallback.c_str())) {
                        for (const auto &entry : type->enums) {
                                const bool selected = entry.val == current;
                                if (ImGui::Selectable(entry.name.c_str(), selected))
                                        std::memcpy(val, &entry.val, std::min<size_t>(type->size, sizeof(entry.val)));
                                if (selected)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
        } else if (type->kind == dwarf::TypeKind::BASE && type->encoding == 0x02 && type->size == 1) {
                bool checked = *static_cast<u8 *>(val) != 0;
                if (ImGui::Checkbox("##value", &checked))
                        *static_cast<u8 *>(val) = checked ? 1U : 0U;
        } else if (type->kind == dwarf::TypeKind::BASE && type->encoding == 0x04 && type->size == 2) {
                u16 raw;
                std::memcpy(&raw, val, sizeof(raw));
                float v = fast_f16_to_f32(raw);
                if (ImGui::InputFloat("##value", &v)) {
                        raw = fast_f32_to_f16(v);
                        std::memcpy(val, &raw, sizeof(raw));
                }
        } else if ((type->kind == dwarf::TypeKind::BASE || type->kind == dwarf::TypeKind::POINTER) &&
                   (type->size == 1 || type->size == 2 || type->size == 4 || type->size == 8) &&
                   (type->encoding != 0x04 || type->size == 4 || type->size == 8)) {
                const bool    floating    = type->encoding == 0x04;
                const bool    signedValue = type->encoding == 0x05 || type->encoding == 0x06;
                ImGuiDataType dataType    = ImGuiDataType_U32;
                if (floating)
                        dataType = type->size == 8 ? ImGuiDataType_Double : ImGuiDataType_Float;
                else if (signedValue)
                        dataType = type->size == 1   ? ImGuiDataType_S8
                                   : type->size == 2 ? ImGuiDataType_S16
                                   : type->size == 8 ? ImGuiDataType_S64
                                                     : ImGuiDataType_S32;
                else
                        dataType = type->size == 1   ? ImGuiDataType_U8
                                   : type->size == 2 ? ImGuiDataType_U16
                                   : type->size == 8 ? ImGuiDataType_U64
                                                     : ImGuiDataType_U32;
                ImGui::SetNextItemWidth(-1.0f);
                alignas(8) u8 aligned[8]{};
                std::memcpy(aligned, val, type->size);
                if (ImGui::InputScalar("##value", dataType, aligned))
                        std::memcpy(val, aligned, type->size);
        } else {
                ImGui::Text("-");
        }
        ImGui::PopID();
        ImGui::PopID();
}

static std::string
csvLeafType(const dwarf::Info &info, const RewriteLeaf &leaf)
{
        const auto *type = resolveAlias(info, leaf.typeOff);
        if (type && type->kind == dwarf::TypeKind::ENUM)
                return "enum";
        if (type && type->encoding == 2)
                return "bool";
        switch (leaf.type) {
                case DataType::F16:
                        return "f16";
                case DataType::F32:
                        return "f32";
                case DataType::F64:
                        return "f64";
                case DataType::I8:
                        return "i8";
                case DataType::U8:
                        return "u8";
                case DataType::I16:
                        return "i16";
                case DataType::U16:
                        return "u16";
                case DataType::I32:
                        return "i32";
                case DataType::U32:
                        return "u32";
                case DataType::I64:
                        return "i64";
                case DataType::U64:
                        return "u64";
                default:
                        throw std::runtime_error("unsupported CSV type");
        }
}

std::string
Variable::exportRewriteCsv()
{
        auto                                        &d = rewrite_;
        std::unordered_map<std::string, RewriteLeaf> leaves;
        collectRewriteLeaves(d.info, d.typeOff, 0, "", leaves, 0, d.source.bitOffset, d.source.bitSize, true);
        std::vector<parameter_csv::Row> rows;
        for (const auto &[path, leaf] : leaves) {
                if (!variable_rewrite::range(leaf.offset, Parser::typeBytes(leaf.type), d.bytes.size()))
                        throw std::runtime_error("CSV field out of bounds: " + path);
                parameter_csv::Row row;
                if (auto it = d.csvRows.find(path); it != d.csvRows.end())
                        row = it->second;
                row.path         = path;
                row.type         = csvLeafType(d.info, leaf);
                row.val          = rewriteValue(d.bytes.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize);
                const auto *type = resolveAlias(d.info, leaf.typeOff);
                if (row.type == "enum") {
                        for (const auto &entry : type->enums)
                                if (row.val == std::to_string(entry.val)) {
                                        row.val = entry.name;
                                        break;
                                }
                } else if (row.type == "bool")
                        row.val = row.val == "0" ? "false" : "true";
                rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
        return parameter_csv::format(rows);
}

struct EncodedRewriteRow {
        const RewriteLeaf *leaf;
        std::array<u8, 8>  raw{};
        std::string        val;
};

static EncodedRewriteRow
encodeRewriteRow(const parameter_csv::Row                           &row,
                 const dwarf::Info                                  &info,
                 const std::unordered_map<std::string, RewriteLeaf> &leaves,
                 size_t                                              configSize)
{
        try {
                const auto it = leaves.find(row.path);
                if (it == leaves.end())
                        throw std::runtime_error("unknown field");
                const auto &leaf = it->second;
                if (row.type != csvLeafType(info, leaf))
                        throw std::runtime_error("type differs from AXF");
                const auto size = Parser::typeBytes(leaf.type);
                if (!variable_rewrite::range(leaf.offset, size, configSize))
                        throw std::runtime_error("invalid field range");
                auto        val  = row.val;
                const auto *type = resolveAlias(info, leaf.typeOff);
                if (row.type == "enum") {
                        auto e = std::find_if(type->enums.begin(), type->enums.end(), [&](const auto &x) {
                                return x.name == val || std::to_string(x.val) == val;
                        });
                        if (e == type->enums.end())
                                throw std::runtime_error("unknown enum name/value");
                        val = std::to_string(e->val);
                } else if (row.type == "bool") {
                        if (val == "true")
                                val = "1";
                        if (val == "false")
                                val = "0";
                        if (val != "0" && val != "1")
                                throw std::runtime_error("expected true/false or 0/1");
                }
                parameter_csv::bounds(row, val);
                EncodedRewriteRow encoded{&leaf, {}, val};
                if (!encodeScalar(val, leaf.type, encoded.raw.data()))
                        throw std::runtime_error("invalid value or numeric overflow");
                return encoded;
        } catch (const std::exception &e) {
                throw std::runtime_error("Parameter #" + std::to_string(row.line) + " [" + row.path + "]: " + e.what());
        }
}

static void
applyRewriteRow(const parameter_csv::Row                           &row,
                const dwarf::Info                                  &info,
                const std::unordered_map<std::string, RewriteLeaf> &leaves,
                std::vector<u8>                                    &edited)
{
        const auto  encoded = encodeRewriteRow(row, info, leaves, edited.size());
        const auto &leaf    = *encoded.leaf;
        const auto  size    = Parser::typeBytes(leaf.type);
        try {
                bool ok = true;
                if (leaf.bitSize) {
                        if (leaf.type == DataType::I8 || leaf.type == DataType::I16 || leaf.type == DataType::I32 ||
                            leaf.type == DataType::I64) {
                                i64 v{};
                                ok = variable_rewrite::integer<i64>(encoded.val, reinterpret_cast<u8 *>(&v)) &&
                                     variable_rewrite::storeSignedBits(
                                         edited.data() + leaf.offset, size, leaf.bitOffset, leaf.bitSize, v);
                        } else {
                                u64 v{};
                                ok = variable_rewrite::integer<u64>(encoded.val, reinterpret_cast<u8 *>(&v)) &&
                                     variable_rewrite::storeBits(
                                         edited.data() + leaf.offset, size, leaf.bitOffset, leaf.bitSize, v);
                        }
                } else
                        std::memcpy(edited.data() + leaf.offset, encoded.raw.data(), size);
                if (!ok)
                        throw std::runtime_error("bitfield overflow");
        } catch (const std::exception &e) {
                throw std::runtime_error("Parameter #" + std::to_string(row.line) + " [" + row.path + "]: " + e.what());
        }
}

static std::optional<parameter_codec::Field>
moduleParameterField(const parameter_json::Field &field, const RewriteLeaf &leaf)
{
        if (!field.id || field.role == "derived" || Parser::typeBytes(leaf.type) != 4 || leaf.offset > UINT16_MAX)
                return std::nullopt;
        const auto &type = field.row.type;
        uint8_t     storageType;
        if (type == "f32" || type == "float" || type == "float32_t")
                storageType = PARAM_LIST_F32;
        else if (type == "u32" || type == "uint32_t" || type == "bool")
                storageType = PARAM_LIST_U32;
        else if (type == "enum" && (!leaf.bitSize || leaf.bitSize == 32 || leaf.type != DataType::I32))
                storageType = PARAM_LIST_ENUM32;
        else
                return std::nullopt;
        if (leaf.bitOffset >= 32 || leaf.bitSize > 32 || (leaf.bitSize ? leaf.bitSize : 32) > 32 - leaf.bitOffset)
                throw std::runtime_error("Invalid parameter bit layout: " + field.row.path);
        return parameter_codec::Field{{static_cast<uint16_t>(field.id),
                                       static_cast<uint16_t>(leaf.offset),
                                       storageType,
                                       static_cast<uint8_t>(field.revision),
                                       static_cast<uint8_t>(leaf.bitOffset),
                                       static_cast<uint8_t>(leaf.bitSize ? leaf.bitSize : 32)},
                                      field.row.path};
}

static void
applyParameterRows(const std::vector<parameter_csv::Row>                        &rows,
                   const std::unordered_map<std::string, parameter_json::Field> &fields,
                   const dwarf::Info                                            &info,
                   const std::unordered_map<std::string, RewriteLeaf>           &leaves,
                   std::vector<u8>                                              &edited)
{
        std::vector<parameter_codec::Field> moduleFields;
        std::vector<uint32_t>               values;
        for (const auto &row : rows) {
                const auto field = fields.find(row.path);
                const auto leaf  = leaves.find(row.path);
                if (field != fields.end() && leaf != leaves.end())
                        if (auto descriptor = moduleParameterField(field->second, leaf->second)) {
                                const auto encoded = encodeRewriteRow(row, info, leaves, edited.size());
                                uint32_t   val;
                                std::memcpy(&val, encoded.raw.data(), sizeof(val));
                                moduleFields.push_back(std::move(*descriptor));
                                values.push_back(val);
                                continue;
                        }
                // 通用变量、字符串和较宽整数沿用通用编辑器支持的类型
                applyRewriteRow(row, info, leaves, edited);
        }
        if (!moduleFields.empty()) {
                const parameter_codec::Layout layout(edited.size(), std::move(moduleFields));
                layout.decode(edited, layout.encode_values(values));
        }
}

bool
Variable::importRewriteCsv(const std::string &content)
{
        auto &d = rewrite_;
        try {
                const auto                                   rows = parameter_csv::parse(content);
                std::unordered_map<std::string, RewriteLeaf> leaves;
                collectRewriteLeaves(d.info, d.typeOff, 0, "", leaves, 0, d.source.bitOffset, d.source.bitSize);
                if (d.isFocCfg) {
                        validateParameterManifest();
                        mergeManifestLeaves(leaves, parameterManifestCache_);
                }
                auto edited   = d.bytes;
                auto metadata = d.csvRows;
                applyParameterRows(rows, d.jsonFields, d.info, leaves, edited);
                for (const auto &row : rows) {
                        metadata[row.path] = row;
                }
                d.bytes       = std::move(edited);
                d.csvRows     = std::move(metadata);
                d.changedOnly = true;
                d.status   = std::to_string(rows.size()) + tr(" parameters imported into preview; click Write changes to apply",
                                                            " 项已导入预览；点击“写入改动”后生效");
                d.statusOk = true;
                return true;
        } catch (const std::exception &e) {
                d.status   = parameterError(e.what());
                d.statusOk = false;
                return false;
        }
}

bool
Variable::validateRewriteCsv()
{
        try {
                std::unordered_map<std::string, RewriteLeaf> leaves;
                collectRewriteLeaves(
                    rewrite_.info, rewrite_.typeOff, 0, "", leaves, 0, rewrite_.source.bitOffset, rewrite_.source.bitSize);
                for (const auto &[path, row] : rewrite_.csvRows) {
                        const auto &leaf = leaves.at(path);
                        parameter_csv::bounds(
                            row, rewriteValue(rewrite_.bytes.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize));
                }
                return true;
        } catch (const std::exception &e) {
                rewrite_.status   = std::string(tr("Parameter limits: ", "参数范围校验失败：")) + parameterError(e.what());
                rewrite_.statusOk = false;
                return false;
        }
}

static std::string readParameterFile(const std::string &path);

std::filesystem::path
Variable::parameterSchemaDirectory() const
{
        // A firmware bundle owns its schema; a model picker must not replace it
        if (!elfPath_.empty() && !remote::isUrl(elfPath_)) {
                const auto      bundled = std::filesystem::u8path(elfPath_).parent_path() / "parameters";
                std::error_code ec;
                if (std::filesystem::is_directory(bundled, ec))
                        return bundled;
        }
        return parameterDirectory_.empty() ? std::filesystem::current_path() / "parameters" : parameterDirectory_;
}

void
Variable::ensureParameterCache()
{
        const auto schema    = parameterSchemaDirectory();
        const auto directory = parameterDirectory_.empty() ? schema : parameterDirectory_;
        const auto stamp     = [](const std::filesystem::path &path) {
                std::error_code ec;
                return std::filesystem::last_write_time(path, ec);
        };
        const auto schemaTime    = stamp(schema / "param_list.json");
        const auto manifestTime  = stamp(schema / "param_list_manifest.json");
        const auto directoryTime = stamp(directory), schemaDirectoryTime = stamp(schema);
        const bool presetsChanged =
            std::any_of(parameterPresetTimes_.begin(), parameterPresetTimes_.end(), [&](const auto &entry) {
                    return stamp(std::filesystem::u8path(entry.first)) != entry.second;
            });
        if (directory == parameterCacheDirectory_ && schema == parameterCacheSchemaDirectory_ &&
            schemaTime == parameterCacheSchemaTime_ && manifestTime == parameterCacheManifestTime_ &&
            directoryTime == parameterCacheDirectoryTime_ && schemaDirectoryTime == parameterCacheSchemaDirectoryTime_ &&
            !presetsChanged)
                return;
        parameterCacheDirectory_           = directory;
        parameterCacheSchemaDirectory_     = schema;
        parameterCacheSchemaTime_          = schemaTime;
        parameterCacheManifestTime_        = manifestTime;
        parameterCacheDirectoryTime_       = directoryTime;
        parameterCacheSchemaDirectoryTime_ = schemaDirectoryTime;
        parameterPresetCache_.clear();
        parameterPresetTimes_.clear();
        parameterMissingPresets_.clear();
        parameterManifestCache_.clear();
        rewrite_.nameComparedBytes.clear();
}

const parameter_json::Document *
Variable::parameterPreset(const std::string &name)
{
        ensureParameterCache();
        if (const auto found = parameterPresetCache_.find(name); found != parameterPresetCache_.end())
                return &found->second;
        if (name.empty() || name.find_first_of("/\\") != std::string::npos)
                return nullptr;
        const auto primary = name == "param_list" ? parameterCacheSchemaDirectory_ : parameterCacheDirectory_;
        for (const auto &directory : {primary, parameterCacheSchemaDirectory_}) {
                const auto path = parameter_json::findPreset(directory, name);
                if (path.empty())
                        continue;
                const auto utf      = path.u8string();
                auto       document = parameter_json::parse(readParameterFile(std::string(utf.begin(), utf.end())));
                if (document.target.empty())
                        continue;
                if ((name == "param_list" ||
                     std::filesystem::is_regular_file(parameterCacheSchemaDirectory_ / "param_list_manifest.json")) &&
                    std::any_of(
                        document.fields.begin(), document.fields.end(), [](const auto &field) { return field.id != 0; })) {
                        if (parameterManifestCache_.empty()) {
                                const auto manifest = (parameterCacheSchemaDirectory_ / "param_list_manifest.json").u8string();
                                parameterManifestCache_ = readParameterFile(std::string(manifest.begin(), manifest.end()));
                        }
                        parameter_json::validateIds(document, parameterManifestCache_);
                }
                std::error_code ec;
                parameterPresetTimes_[std::string(utf.begin(), utf.end())] = std::filesystem::last_write_time(path, ec);
                return &parameterPresetCache_.emplace(name, std::move(document)).first->second;
        }
        parameterMissingPresets_.insert(name);
        return nullptr;
}

void
Variable::addParameterBaselines(parameter_json::Document &document)
{
        if (!parameter_json::completeComponents(document) || document.name.empty())
                return;
        const auto names = parameter_json::componentNames(document);
        for (const auto &[section, selected] : names) {
                const auto *preset = parameterPreset(parameter_json::baseName(selected));
                if (!preset)
                        continue;
                std::unordered_map<std::string, const parameter_json::Field *> fields;
                fields.reserve(preset->fields.size());
                for (const auto &candidate : preset->fields)
                        if (candidate.role != "derived")
                                fields.emplace(candidate.row.path, &candidate);
                for (auto &field : document.fields) {
                        if (field.role == "derived" || field.presetValue || !field.row.path.starts_with(section + '.'))
                                continue;
                        const auto source = fields.find(field.row.path);
                        if (source == fields.end())
                                continue;
                        const auto &candidate = *source->second;
                        if (candidate.row.type == field.row.type &&
                            (!candidate.id || !field.id || (candidate.id == field.id && candidate.revision == field.revision)))
                                field.presetValue = candidate.presetValue ? candidate.presetValue
                                                                          : std::optional<std::string>{candidate.row.val};
                }
        }
        parameter_json::capturePresetValues(document);
}

bool
Variable::syncRewriteName()
{
        if (!rewrite_.isFocCfg || !rewrite_.targetLoaded)
                return true;
        try {
                if (rewrite_.nameComparedBytes == rewrite_.bytes)
                        return true;
                ensureParameterCache();
                exportRewriteJson();
                rewrite_.nameComparedBytes = rewrite_.bytes;
                return true;
        } catch (const std::exception &e) {
                rewrite_.status   = parameterError(e.what());
                rewrite_.statusOk = false;
                return false;
        }
}

bool
Variable::importRewriteJson(const std::string &content)
{
        try {
                auto doc = parameter_json::parse(content);
                ensureParameterCache();
                if (rewrite_.isFocCfg &&
                    std::any_of(doc.fields.begin(), doc.fields.end(), [](const auto &f) { return f.id != 0; })) {
                        const auto manifestPath = parameterSchemaDirectory() / "param_list_manifest.json";
                        const auto path         = manifestPath.u8string();
                        const auto manifest     = readParameterFile(std::string(path.begin(), path.end()));
                        parameter_json::validateIds(doc, manifest);
                        parameterManifestCache_ = manifest;
                }
                addParameterBaselines(doc);
                parameter_json::refreshComponentNames(doc);
                if (doc.target != rewrite_.variableName)
                        throw std::runtime_error("JSON target differs from opened variable: " + doc.target);
                std::vector<parameter_csv::Row>                        rows;
                std::unordered_map<std::string, parameter_json::Field> fields;
                for (const auto &f : doc.fields) {
                        if (f.role != "derived")
                                rows.push_back(f.row);
                        fields.emplace(f.row.path, f);
                }
                if (rewrite_.isFocCfg && !rows.empty()) {
                        if (doc.name.empty() || doc.name.size() > 63 || doc.name.find('\0') != std::string::npos)
                                throw std::runtime_error("Configuration name requires 1..63 UTF-8 bytes");
                        const auto currentRows = parameter_csv::parse(exportRewriteCsv());
                        for (size_t i = 0; i < 64; ++i) {
                                const auto path = "name[" + std::to_string(i) + "]";
                                if (fields.contains(path))
                                        throw std::runtime_error("Configuration name must use JSON top-level name");
                                const auto it = std::find_if(
                                    currentRows.begin(), currentRows.end(), [&](const auto &r) { return r.path == path; });
                                if (it == currentRows.end())
                                        throw std::runtime_error("AXF has no 64-byte foc.cfg.name");
                                auto           row  = *it;
                                const unsigned byte = i < doc.name.size() ? static_cast<unsigned char>(doc.name[i]) : 0;
                                row.val             = std::to_string(byte);
                                row.min.clear();
                                row.max.clear();
                                rows.push_back(std::move(row));
                        }
                }
                std::unordered_map<std::string, RewriteLeaf> leaves;
                collectRewriteLeaves(
                    rewrite_.info, rewrite_.typeOff, 0, "", leaves, 0, rewrite_.source.bitOffset, rewrite_.source.bitSize);
                if (rewrite_.isFocCfg) {
                        validateParameterManifest();
                        mergeManifestLeaves(leaves, parameterManifestCache_);
                }
                auto edited   = rewrite_.bytes;
                auto metadata = rewrite_.csvRows;
                for (const auto &f : doc.fields) {
                        if (f.role == "derived")
                                continue;
                        for (const auto &[name, val] : f.enums) {
                                auto option = f.row;
                                option.val  = val;
                                option.min.clear();
                                option.max.clear();
                                applyRewriteRow(option, rewrite_.info, leaves, edited);
                        }
                }
                // Validate all choices before committing anything, including metadata
                edited = rewrite_.bytes;
                applyParameterRows(rows, fields, rewrite_.info, leaves, edited);
                for (const auto &row : rows) {
                        metadata[row.path] = row;
                }
                rewrite_.bytes       = std::move(edited);
                rewrite_.csvRows     = std::move(metadata);
                rewrite_.changedOnly = !rows.empty();
                rewrite_.jsonFields  = std::move(fields);
                rewrite_.profileName = doc.name;
                rewrite_.nameComparedBytes.clear();
                rewrite_.status   = tr("JSON loaded; review changes then write", "JSON 已载入；检查改动后点击写入");
                rewrite_.statusOk = true;
                return true;
        } catch (const std::exception &e) {
                rewrite_.status   = parameterError(e.what());
                rewrite_.statusOk = false;
                return false;
        }
}

std::string
Variable::exportRewriteJson(std::vector<u8> *candidate)
{
        auto                    &configuration = candidate ? *candidate : rewrite_.bytes;
        parameter_json::Document doc;
        doc.name   = rewrite_.profileName.empty() ? rewrite_.variableName : rewrite_.profileName;
        doc.target = rewrite_.variableName;
        std::unordered_map<std::string, RewriteLeaf> leaves;
        collectRewriteLeaves(
            rewrite_.info, rewrite_.typeOff, 0, "", leaves, 0, rewrite_.source.bitOffset, rewrite_.source.bitSize);
        // The matching firmware manifest supplies canonical fields absent from
        // DWARF (for example, aliases of an anonymous union)
        if (rewrite_.isFocCfg) {
                validateParameterManifest();
                mergeManifestLeaves(leaves, parameterManifestCache_);
        }
        if (rewrite_.isFocCfg) {
                doc.name.clear();
                for (size_t i = 0; i < 64; ++i) {
                        const auto it = leaves.find("name[" + std::to_string(i) + "]");
                        if (it == leaves.end() || it->second.offset >= configuration.size())
                                throw std::runtime_error("Missing configuration name");
                        const char c = static_cast<char>(configuration[it->second.offset]);
                        if (!c)
                                break;
                        doc.name.push_back(c);
                }
                if (std::count(doc.name.begin(), doc.name.end(), '/') == 2)
                        doc.components = parameter_json::componentNames(doc);
                // Runtime-only derived fields cannot be discovered by walking
                // cfg in DWARF. Use the matching schema even on a fresh read
                const auto *schema = parameterPreset("param_list");
                if (!schema)
                        throw std::runtime_error(
                            "Choose the matching parameter preset directory before exporting FOC configuration");
                std::vector<parameter_codec::Field> moduleFields;
                for (const auto &field : schema->fields)
                        if (const auto leaf = leaves.find(field.row.path); leaf != leaves.end())
                                if (auto descriptor = moduleParameterField(field, leaf->second))
                                        moduleFields.push_back(std::move(*descriptor));
                const parameter_codec::Layout             layout(configuration.size(), std::move(moduleFields));
                const auto                                values = layout.read_values(configuration);
                std::unordered_map<std::string, uint32_t> moduleValues;
                for (size_t i = 0; i < values.size(); ++i)
                        moduleValues.emplace(layout.fields()[i].path, values[i]);
                for (auto field : schema->fields) {
                        // The template's own model is not the selected component
                        field.presetValue.reset();
                        if (const auto metadata = rewrite_.jsonFields.find(field.row.path);
                            metadata != rewrite_.jsonFields.end() && metadata->second.role == field.role &&
                            metadata->second.row.type == field.row.type &&
                            (!metadata->second.id ||
                             (metadata->second.id == field.id && metadata->second.revision == field.revision))) {
                                const auto id = field.id, revision = field.revision;
                                field          = metadata->second;
                                field.id       = id;
                                field.revision = revision;
                        }
                        if (field.role == "derived")
                                field.row.val.clear();
                        else {
                                const auto leaf = leaves.find(field.row.path);
                                const bool typeMatches =
                                    leaf == leaves.end()
                                        ? false
                                        : (leaf->second.typeOff
                                               ? csvLeafType(rewrite_.info, leaf->second) == field.row.type
                                               : std::string(Parser::dataTypeToStr(leaf->second.type)) == field.row.type);
                                if (!typeMatches)
                                        throw std::runtime_error(
                                            "Parameter schema does not match current AXF: " + field.row.path +
                                            " (schema: " + parameterSchemaDirectory().string() + ")");
                                const auto &location = leaf->second;
                                if (!variable_rewrite::range(
                                        location.offset, Parser::typeBytes(location.type), configuration.size()))
                                        throw std::runtime_error("Configuration field is out of bounds: " + field.row.path);
                                if (const auto val = moduleValues.find(field.row.path); val != moduleValues.end())
                                        field.row.val = rewriteValue(reinterpret_cast<const u8 *>(&val->second), location.type);
                                else
                                        field.row.val = rewriteValue(configuration.data() + location.offset,
                                                                     location.type,
                                                                     location.bitOffset,
                                                                     location.bitSize);
                        }
                        doc.fields.push_back(std::move(field));
                }
                addParameterBaselines(doc);
                parameter_json::refreshComponentNames(doc);
                const auto output  = parameter_json::format(doc);
                auto       renamed = configuration;
                if (doc.name.size() > 63)
                        throw std::runtime_error("Configuration name exceeds 63 UTF-8 bytes");
                for (size_t i = 0; i < 64; ++i) {
                        const auto leaf = leaves.find("name[" + std::to_string(i) + "]");
                        if (leaf == leaves.end() || leaf->second.offset >= renamed.size())
                                throw std::runtime_error("Missing configuration name");
                        renamed[leaf->second.offset] = i < doc.name.size() ? static_cast<u8>(doc.name[i]) : 0;
                }
                configuration = std::move(renamed);
                if (!candidate) {
                        for (const auto &field : doc.fields)
                                rewrite_.jsonFields[field.row.path] = field;
                        rewrite_.profileName = doc.name;
                }
                return output;
        }
        std::vector<parameter_csv::Row> rows;
        if (rewrite_.jsonFields.empty()) {
                rows = parameter_csv::parse(exportRewriteCsv());
        } else {
                for (const auto &[path, field] : rewrite_.jsonFields) {
                        if (field.role == "derived") {
                                doc.fields.push_back(field);
                                continue;
                        }
                        const auto leaf = leaves.find(path);
                        if (leaf == leaves.end())
                                throw std::runtime_error("Unknown parameter path: " + path);
                        auto row = field.row;
                        row.val  = rewriteValue(configuration.data() + leaf->second.offset,
                                               leaf->second.type,
                                               leaf->second.bitOffset,
                                               leaf->second.bitSize);
                        rows.push_back(std::move(row));
                }
        }
        for (const auto &row : rows) {
                parameter_json::Field f;
                f.row = row;
                if (auto it = rewrite_.jsonFields.find(row.path); it != rewrite_.jsonFields.end()) {
                        f     = it->second;
                        f.row = row;
                }
                const auto &leaf = leaves.at(row.path);
                if (f.enums.empty()) {
                        const auto *t = resolveAlias(rewrite_.info, leaf.typeOff);
                        if (t && t->kind == dwarf::TypeKind::ENUM)
                                for (const auto &e : t->enums)
                                        f.enums[e.name] = std::to_string(e.val);
                }
                const auto val = rewriteValue(rewrite_.bytes.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize);
                if (!f.enums.empty()) {
                        f.row.val = val;
                        for (const auto &[name, v] : f.enums)
                                if (v == val) {
                                        f.row.val = name;
                                        break;
                                }
                }
                doc.fields.push_back(std::move(f));
        }
        return parameter_json::format(doc);
}

static std::string
readParameterFile(const std::string &path)
{
        FILE *f = nativeFopen(path, "rb");
        if (!f)
                throw std::runtime_error("Cannot open parameter JSON");
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        rewind(f);
        std::string content;
        if (size > 0 && size <= 16 * 1024 * 1024) {
                content.resize(static_cast<size_t>(size));
                if (fread(content.data(), 1, content.size(), f) != content.size())
                        content.clear();
        }
        fclose(f);
        if (content.empty())
                throw std::runtime_error("Empty, oversized or unreadable parameter JSON");
        return content;
}

bool
Variable::loadParameterFile(const std::string &path)
{
        clearParameterHistory();
        try {
                const auto           content = readParameterFile(path);
                parameter_json::Json root(cJSON_Parse(content.c_str()), cJSON_Delete);
                if (!root || parameter_json::string(root.get(), "format") != "foc-parameters")
                        return false;
                ensureParameterCache();
                parameter_json::FileEditor next;
                const auto                 stem = std::filesystem::u8path(path).stem().u8string();
                next.load(content, false, std::string(stem.begin(), stem.end()));
                if (!parameterTargetOverride_.empty())
                        next.document.target = parameterTargetOverride_;
                addParameterBaselines(next.document);
                next.refreshNames();
                isModified_               = true;
                parameterFile_            = std::move(next);
                parameterFilePath_        = path;
                parameterFileCombination_ = false;
                setParameterFileStatus({});
                parameterFileOpen_        = true;
                parameterFileRequestOpen_ = true;
                if (parameterDevice_.variableName != parameterFile_.document.target) {
                        parameterDeviceValues_.clear();
                        parameterDevice_ = {};
                }
                parameterReadPending_ = false;
                return true;
        } catch (const std::exception &e) {
                setParameterFileStatus(parameterError(e.what()), true);
                ImGui::InsertNotification({ImGuiToastType::Error, 5000, "%s", parameterError(e.what()).c_str()});
                return true;
        }
}

bool
Variable::selectParameterComponent(const std::string &section, const std::string &path)
{
        clearParameterHistory();
        try {
                const auto content = readParameterFile(path);
                ensureParameterCache();
                parameter_json::FileEditor incoming;
                const auto                 stem = std::filesystem::u8path(path).stem().u8string();
                incoming.load(content, false, std::string(stem.begin(), stem.end()));
                addParameterBaselines(incoming.document);
                incoming.refreshNames();
                if (!parameterTargetOverride_.empty())
                        incoming.document.target = parameterTargetOverride_;
                auto next = parameterFile_;
                if (next.source.empty())
                        next.load(incoming.save());
                addParameterBaselines(next.document);
                next.replaceComponent(section, incoming.save(), std::string(stem.begin(), stem.end()));
                isModified_    = true;
                parameterFile_ = std::move(next);
                if (!parameterFileCombination_)
                        parameterFilePath_.clear();
                parameterFileCombination_ = true;
                setParameterFileStatus(
                    tr("Component selected; other component edits are preserved", "已更新所选配置，其他两组的修改已保留"),
                    false,
                    true);
                parameterFileOpen_ = parameterFileRequestOpen_ = true;
                parameterReadPending_                          = false;
                return true;
        } catch (const std::exception &e) {
                setParameterFileStatus(parameterError(e.what()), true);
                return false;
        }
}

void
Variable::openParameterComposition()
{
        try {
                parameterReadPending_ = !parameterFileOpen_;
                // Keep any unsaved offline edits when this window is already in use
                if (!parameterFile_.dirty && rewrite_.isFocCfg && rewrite_.targetLoaded) {
                        parameter_json::FileEditor snapshot;
                        snapshot.load(exportRewriteJson());
                        parameterFile_ = std::move(snapshot);
                        parameterFilePath_.clear();
                        parameterFileCombination_ = true;
                        isModified_               = true;
                        setParameterFileStatus(tr("Editing the current configuration preview", "正在编辑当前配置预览"));
                }
                if (!parameterFileOpen_)
                        isModified_ = true;
                parameterFileOpen_ = parameterFileRequestOpen_ = true;
        } catch (const std::exception &e) {
                rewrite_.status   = parameterError(e.what());
                rewrite_.statusOk = false;
        }
}

const std::vector<std::filesystem::path> &
Variable::parameterPresetFiles()
{
        if (parameterDirectory_.empty())
                parameterDirectory_ = parameterSchemaDirectory();
        const auto now = std::chrono::steady_clock::now();
        if (parameterFileListDirectory_ == parameterDirectory_ && now - parameterFileListChecked_ < std::chrono::seconds(1))
                return parameterFileList_;
        parameterFileList_          = parameter_json::presetFiles(parameterDirectory_);
        parameterFileListDirectory_ = parameterDirectory_;
        parameterFileListChecked_   = now;
        return parameterFileList_;
}

struct ParameterPresetMenu {
        std::map<std::filesystem::path, ParameterPresetMenu> folders;
        std::vector<const std::filesystem::path *>           files;
};

/**
 * @brief 显示实际目录对应的子菜单, 返回所选文件路径
 */
static std::filesystem::path
drawParameterPresetEntries(const ParameterPresetMenu                                &menu,
                           const std::function<bool(const std::filesystem::path &)> &isSelected,
                           bool                                                      showExtension)
{
        std::filesystem::path chosen;
        for (const auto &[folder, child] : menu.folders) {
                const auto        utf = folder.u8string();
                const std::string label(utf.begin(), utf.end());
                if (!ImGui::BeginMenu(label.c_str()))
                        continue;
                const auto selected = drawParameterPresetEntries(child, isSelected, showExtension);
                if (!selected.empty())
                        chosen = selected;
                ImGui::EndMenu();
        }
        for (const auto *file : menu.files) {
                const auto        utf = (showExtension ? file->filename() : file->stem()).u8string();
                const std::string label(utf.begin(), utf.end());
                const auto        path = file->u8string();
                const std::string id(path.begin(), path.end());
                ImGui::PushID(id.c_str());
                if (ImGui::MenuItem(label.c_str(), nullptr, isSelected(*file)))
                        chosen = *file;
                ImGui::PopID();
        }
        return chosen;
}

/**
 * @brief 将预设的相对目录转换为分级菜单, 文件名不参与分类
 */
static std::filesystem::path
drawParameterPresetMenu(const std::vector<std::filesystem::path>                 &files,
                        const std::filesystem::path                              &directory,
                        const std::function<bool(const std::filesystem::path &)> &isSelected,
                        bool                                                      showExtension)
{
        ParameterPresetMenu menu;
        for (const auto &file : files) {
                const auto relative = file.lexically_relative(directory);
                if (relative.empty() || *relative.begin() == "..")
                        continue;
                auto *node = &menu;
                for (const auto &folder : relative.parent_path())
                        node = &node->folders[folder];
                node->files.push_back(&file);
        }
        const auto chosen = drawParameterPresetEntries(menu, isSelected, showExtension);
        if (files.empty())
                ImGui::TextUnformatted(tr("No JSON files; choose a directory", "目录中没有 JSON, 请选择预设目录"));
        if (!chosen.empty())
                ImGui::CloseCurrentPopup();
        return chosen;
}

void
Variable::drawParameterComponents()
{
        if (!parameterFile_.source.empty() && parameterFile_.document.target.empty())
                return;
        try {
                if (!parameterFile_.source.empty())
                        parameterFile_.refreshNames();
        } catch (const std::exception &e) {
                setParameterFileStatus(parameterError(e.what()), true);
        }
        if (parameterDirectory_.empty())
                parameterDirectory_ = parameterSchemaDirectory();
        const std::array<const char *, 3>  sections{"mech", "hw", "sw"};
        const std::array<const char *, 3>  labels{tr("Mechanical (mechanical)", "机械配置（mechanical）"),
                                                 tr("Hardware (hardware)", "硬件配置（hardware）"),
                                                 tr("Software (software)", "软件配置（software）")};
        std::map<std::string, std::string> names;
        try {
                names = parameter_json::componentNames(parameterFile_.document);
        } catch (const std::exception &e) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
                ImGui::TextWrapped("%s", parameterError(e.what()).c_str());
                ImGui::PopStyleColor();
        }
        ImGui::Separator();
        for (size_t i = 0; i < sections.size(); ++i) {
                ImGui::PushID(sections[i]);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(labels[i]);
                ImGui::SameLine(220.0f);
                ImGui::SetNextItemWidth(-130.0f);
                const auto  selected = names.find(sections[i]);
                const auto *preview  = selected == names.end() ? tr("Select a preset", "选择型号") : selected->second.c_str();
                if (ImGui::BeginCombo("##component_preset", preview)) {
                        const auto chosen = drawParameterPresetMenu(
                            parameterPresetFiles(),
                            parameterDirectory_,
                            [&](const std::filesystem::path &file) {
                                    const auto utf = file.stem().u8string();
                                    return selected != names.end() &&
                                           parameter_json::baseName(selected->second) == std::string(utf.begin(), utf.end());
                            },
                            false);
                        ImGui::EndCombo();
                        if (!chosen.empty()) {
                                const auto utf = chosen.u8string();
                                selectParameterComponent(sections[i], std::string(utf.begin(), utf.end()));
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Open JSON...", "打开 JSON..."))) {
                        const auto path = nativeDlgOpen(labels[i], {{"JSON", {"json"}}});
                        if (!path.empty())
                                selectParameterComponent(sections[i], path);
                }
                ImGui::PopID();
        }
        if (!parameterFile_.document.name.empty())
                ImGui::TextWrapped("%s: %s", tr("Configuration name", "配置名称"), parameterFile_.document.name.c_str());
        ImGui::Separator();
}

Variable::ParameterDraft
Variable::captureParameterDraft() const
{
        ParameterDraft draft;
        for (const auto &field : parameterFile_.document.fields)
                if (field.role != "derived")
                        draft.emplace_back(field.row.path, field.row.val);
        return draft;
}

void
Variable::restoreParameterDraft(const ParameterDraft &draft)
{
        std::unordered_map<std::string, std::string> values(draft.begin(), draft.end());
        for (auto &field : parameterFile_.document.fields)
                if (field.role != "derived")
                        if (const auto value = values.find(field.row.path); value != values.end())
                                field.row.val = value->second;
        parameterFile_.dirty = true;
        isModified_          = true;
        try {
                parameterFile_.refreshNames();
        } catch (const std::exception &error) {
                setParameterFileStatus(parameterError(error.what()), true);
        }
}

void
Variable::clearParameterHistory()
{
        parameterHistory_.clear();
        parameterEditBefore_.reset();
        parameterEditActive_ = 0;
        parameterAction_     = ui::interaction::Action::None;
}

void
Variable::drawParameterTargetPicker()
{
        const std::string label =
            std::string(tr("Target  ", "目标  ")) +
            (parameterFile_.document.target.empty() ? tr("Select variable", "选择变量") : parameterFile_.document.target) +
            "###parameter_target_picker";
        const float width    = std::min(ui::FlowToolbar::buttonWidth(label.c_str()), ImGui::GetFontSize() * 18);
        const bool  activate = ImGui::Button(label.c_str(), ImVec2(width, 0));
        TutorialGuide::instance().mark("parameters.target");
        const auto        trigger = ImGui::GetCurrentContext()->LastItemData;
        const std::string id      = "##ParameterTarget_" + std::to_string(ImGui::GetID("parameter_target"));
        if (ui::BeginHoverPanel(id.c_str(),
                                tr("Select target variable", "选择目标变量"),
                                parameterTargetHover_,
                                activate,
                                ImGui::GetFontSize() * 34,
                                ImGui::GetWindowViewport())) {
                ImGui::TextDisabled(tr("Variable manager: %s", "变量管理器：%s"), getTitle().c_str());
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##target_search",
                                         tr("Search variable name", "搜索变量名"),
                                         parameterTargetSearch_,
                                         sizeof(parameterTargetSearch_));
                std::vector<std::string> candidates;
                for (const auto &entry : vars_)
                        candidates.push_back(entry.name);
                for (const auto &entry : searchPool_)
                        candidates.push_back(entry.path);
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
                const auto lower = [](std::string value) {
                        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                                return static_cast<char>(std::tolower(ch));
                        });
                        return value;
                };
                const std::string query = lower(parameterTargetSearch_);
                if (!query.empty())
                        std::erase_if(candidates,
                                      [&](const auto &name) { return lower(name).find(query) == std::string::npos; });
                if (candidates.empty())
                        ImGui::TextWrapped("%s",
                                           tr("No matching variable. Load symbols or add variables in this manager first.",
                                              "没有匹配变量。请先在此变量管理器加载符号文件或添加变量。"));
                if (ImGui::BeginChild("##target_list", ImVec2(0, ImGui::GetFontSize() * 15), false)) {
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(candidates.size()));
                        while (clipper.Step())
                                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                                        if (ImGui::Selectable(candidates[i].c_str(),
                                                              candidates[i] == parameterFile_.document.target)) {
                                                setParameterTarget(candidates[i]);
                                                parameterTargetHover_.close();
                                        }
                }
                ImGui::EndChild();
                ImGui::End();
        }
        ImGui::GetCurrentContext()->LastItemData = trigger;
}

void
Variable::drawParameterFileEditor()
{
        using ui::interaction::Action;
        if (parameterAction_ == Action::Undo) {
                if (const auto *draft = parameterHistory_.undo())
                        restoreParameterDraft(*draft);
        } else if (parameterAction_ == Action::Redo) {
                if (const auto *draft = parameterHistory_.redo())
                        restoreParameterDraft(*draft);
        } else if (parameterAction_ == Action::Parent || parameterAction_ == Action::Escape) {
                parameterFileOpen_ = false;
                if (auto *parent = ImGui::FindWindowByName(("###" + name_).c_str()))
                        ImGui::FocusWindow(parent);
        }
        parameterAction_ = Action::None;

        if (!parameterFileOpen_) {
                parameterFileVisible_ = false;
                return;
        }
        const std::string title =
            std::string(tr("Parameter Configuration###ParameterFileEditor", "参数配置###ParameterFileEditor")) + name_;
        if (parameterFileRequestOpen_) {
                ImGui::SetNextWindowFocus();
                parameterFileRequestOpen_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(1040, 720), ImGuiCond_FirstUseEver);
        if (!dock_close::begin(title.c_str(), &parameterFileOpen_)) {
                if (open_ && parameterFileOpen_)
                        ui::interaction::registerRegion(
                            std::to_string(parameterComponentTab_),
                            parameterHistory_.canUndo(),
                            parameterHistory_.canRedo(),
                            true,
                            [this](Action action) { parameterAction_ = action; },
                            [this](const std::string &route) {
                                    if (route.size() == 1 && route[0] >= '0' && route[0] <= '3')
                                            parameterNavigateTab_ = route[0] - '0';
                            });
                parameterFileVisible_ = false;
                ImGui::End();
                return;
        }
        if (!parameterFileVisible_ && !parameterFile_.dirty)
                parameterReadPending_ = true;
        parameterFileVisible_ = true;

        if (parameterReadPending_ && JLinkPort::instance().isConnected() && !JLinkPort::instance().isBusy()) {
                parameterReadPending_ = false;
                try {
                        if (parameterFile_.source.empty()) {
                                if (const auto *schema = parameterPreset("param_list"))
                                        parameterFile_.load(parameter_json::format(*schema));
                        }
                        if (!parameterFile_.source.empty())
                                updateParameterDevice(false);
                        else
                                setParameterFileStatus(
                                    tr("Open a parameter JSON before reading", "请先打开参数 JSON 再读取参数"), true);
                } catch (const std::exception &e) {
                        setParameterFileStatus(parameterError(e.what()), true);
                }
        }
        if (!parameterFile_.source.empty()) {
                try {
                        parameterFile_.refreshNames();
                } catch (const std::exception &e) {
                        setParameterFileStatus(parameterError(e.what()), true);
                }
        }
        ui::FlowToolbar parameterActions;
        parameterActions.next(ImGui::GetFontSize() * 18);
        drawParameterTargetPicker();
        parameterActions.group();
        std::string configurationHeader = tr("Presets and configuration", "预设与组合配置");
        if (!parameterFile_.document.name.empty())
                configurationHeader += "  " + parameterFile_.document.name;
        configurationHeader += "###parameter_configuration";
        const float presetWidth =
            std::min(ui::FlowToolbar::buttonWidth(configurationHeader.c_str()), ImGui::GetFontSize() * 24);
        const bool activatePresets = ImGui::Button(configurationHeader.c_str(), ImVec2(parameterActions.next(presetWidth), 0));
        TutorialGuide::instance().mark("parameters.presets");
        const auto        presetTrigger = ImGui::GetCurrentContext()->LastItemData;
        const std::string hoverId = "ParameterPresets###ParameterPresets_" + std::to_string(ImGui::GetID("parameter_presets"));
        if (ui::BeginHoverPanel(hoverId.c_str(),
                                tr("Presets and configuration", "预设与组合配置"),
                                parameterPresetHover_,
                                activatePresets,
                                ImGui::GetFontSize() * 49,
                                ImGui::GetWindowViewport())) {
                if (ImGui::Button(tr("Open JSON...", "打开 JSON..."))) {
                        const auto path = nativeDlgOpen(tr("Parameter JSON", "参数 JSON"), {{"JSON", {"json"}}});
                        if (!path.empty())
                                loadParameterFile(path);
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Preset directory...", "选择预设目录..."))) {
                        const auto path = nativeDlgPickDir(tr("Preset directory", "选择预设目录"));
                        if (!path.empty()) {
                                parameterDirectory_ = std::filesystem::u8path(path);
                                isModified_         = true;
                                parameterFileListDirectory_.clear();
                        }
                }
                ImGui::SameLine();
                std::string selectedJson;
                if (!parameterFile_.source.empty()) {
                        if ((parameterFileCombination_ || parameterFile_.dirty) && !parameterFile_.document.name.empty())
                                selectedJson = parameterFile_.document.name + ".json";
                        else {
                                const auto filename = std::filesystem::u8path(parameterFilePath_).filename().u8string();
                                selectedJson.assign(filename.begin(), filename.end());
                        }
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##offline_presets",
                                      selectedJson.empty() ? tr("Select parameter JSON", "选择参数 JSON")
                                                           : selectedJson.c_str())) {
                        if (parameterDirectory_.empty())
                                parameterDirectory_ = parameterSchemaDirectory();
                        const auto chosen = drawParameterPresetMenu(
                            parameterPresetFiles(),
                            parameterDirectory_,
                            [&](const std::filesystem::path &file) {
                                    return file == std::filesystem::u8path(parameterFilePath_);
                            },
                            true);
                        ImGui::EndCombo();
                        if (!chosen.empty()) {
                                const auto utf = chosen.u8string();
                                loadParameterFile(std::string(utf.begin(), utf.end()));
                        }
                }
                drawParameterComponents();
                const bool  loaded    = !parameterFile_.source.empty();
                const auto *fileLabel = parameterFilePath_.empty() && loaded ? tr("New combined configuration", "新组合配置")
                                                                             : parameterFilePath_.c_str();
                ImGui::TextWrapped("%s%s", fileLabel, parameterFile_.dirty ? " *" : "");
                if (loaded)
                        ImGui::Text("%s | %s", parameterFile_.document.target.c_str(), parameterFile_.document.status.c_str());
                ImGui::End();
        }
        ImGui::GetCurrentContext()->LastItemData = presetTrigger;
        parameterActions.group();
        const bool loaded = !parameterFile_.source.empty();
        ImGui::BeginDisabled(!loaded);
        parameterActions.next(tr("Save", "保存"));
        const bool saveFile = ImGui::Button(tr("Save", "保存"));
        parameterActions.next(tr("Save as...", "另存为..."));
        const bool saveAsFile = ImGui::Button(tr("Save as...", "另存为..."));
        if (saveFile || saveAsFile) {
                try {
                        const auto content = parameterFile_.save();
                        const auto path =
                            saveFile && !parameterFilePath_.empty()
                                ? parameterFilePath_
                                : nativeDlgSave(tr("Parameter JSON", "参数 JSON"), {{"JSON", {"json"}}}, "param_list.json");
                        if (!path.empty()) {
                                FILE *file = nativeFopen(path, "wb");
                                bool  ok   = false;
                                if (file) {
                                        ok = fwrite(content.data(), 1, content.size(), file) == content.size();
                                        ok = fclose(file) == 0 && ok;
                                }
                                if (!ok)
                                        throw std::runtime_error("Cannot save JSON");
                                parameterFile_.load(content);
                                parameterFilePath_        = path;
                                parameterFileCombination_ = false;
                                isModified_               = true;
                                setParameterFileStatus(tr("JSON saved", "JSON 已保存"), false, true);
                        }
                } catch (const std::exception &e) {
                        setParameterFileStatus(parameterError(e.what()), true);
                }
        }
        parameterActions.next(tr("Restore default parameters", "恢复默认参数"));
        if (ImGui::Button(tr("Restore default parameters", "恢复默认参数"))) {
                try {
                        auto next = parameterFile_;
                        addParameterBaselines(next.document);
                        next.restoreDefaults();
                        isModified_           = true;
                        parameterFile_        = std::move(next);
                        parameterReadPending_ = false;
                        setParameterFileStatus(tr("Current model defaults restored; click Write parameters to apply",
                                                  "已恢复当前型号默认参数，点击写入参数应用到设备"),
                                               false,
                                               true);
                } catch (const std::exception &e) {
                        setParameterFileStatus(parameterError(e.what()), true);
                }
        }
        parameterActions.group();
        parameterActions.next(tr("Write parameters", "写入参数"));
        ImGui::BeginDisabled(parameterFile_.document.status != "ready");
        if (ImGui::Button(tr("Write parameters", "写入参数")))
                updateParameterDevice(true);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        parameterActions.next(tr("Read parameters", "读取参数"));
        ImGui::BeginDisabled(!loaded || !JLinkPort::instance().isConnected() || JLinkPort::instance().isBusy());
        if (ImGui::Button(tr("Read parameters", "读取参数")))
                updateParameterDevice(false);
        ImGui::EndDisabled();
        const bool deviceValuesCurrent =
            parameterDevice_.targetLoaded && parameterDevice_.variableName == parameterFile_.document.target &&
            parameterDevice_.layoutGeneration == elfLayoutGeneration_ &&
            (parameterDevice_.source.port != PortType::JLINK ||
             (JLinkPort::instance().isConnected() && parameterDevice_.targetEpoch == JLinkPort::instance().targetEpoch()));
        if (!parameterFileStatus_.empty()) {
                if (parameterFileStatusError_ || parameterFileStatusSuccess_)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              parameterFileStatusError_ ? ui::ErrorTextColor() : ui::SuccessTextColor());
                const auto   position = ImGui::GetCursorScreenPos();
                const float  width    = ImGui::GetContentRegionAvail().x;
                const ImVec2 end(position.x + width, position.y + ImGui::GetTextLineHeight());
                const auto   lineEnd = parameterFileStatus_.find_first_of("\r\n");
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
                                          position,
                                          end,
                                          end.x,
                                          parameterFileStatus_.c_str(),
                                          parameterFileStatus_.c_str() +
                                              (lineEnd == std::string::npos ? parameterFileStatus_.size() : lineEnd),
                                          nullptr);
                ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                        ImGui::TextUnformatted(parameterFileStatus_.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                }
                if (parameterFileStatusError_ || parameterFileStatusSuccess_)
                        ImGui::PopStyleColor();
        }
        if (loaded && ImGui::BeginTabBar("##parameter_components")) {
                const char *labels[] = {tr("Mechanical###mech", "机械###mech"),
                                        tr("Hardware###hw", "硬件###hw"),
                                        tr("Software###sw", "软件###sw"),
                                        tr("Common###common", "公共###common")};
                for (const int i : {3, 0, 1, 2}) {
                        if (ImGui::BeginTabItem(
                                labels[i], nullptr, parameterNavigateTab_ == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                                if (parameterNavigateTab_ < 0 && parameterComponentTab_ != i)
                                        isModified_ = true;
                                parameterComponentTab_ = i;
                                ImGui::EndTabItem();
                        }
                }
                ImGui::EndTabBar();
        }
        if (parameterNavigateTab_ >= 0)
                parameterComponentTab_ = parameterNavigateTab_;
        parameterNavigateTab_     = -1;
        const auto &style         = ImGui::GetStyle();
        const char *expandLabel   = tr("Expand all", "展开全部");
        const char *collapseLabel = tr("Collapse all", "折叠全部");
        const float controlsWidth = ImGui::CalcTextSize(expandLabel).x + ImGui::CalcTextSize(collapseLabel).x +
                                    style.FramePadding.x * 4.0f + ImGui::CalcTextSize("(?)").x + style.ItemSpacing.x * 3.0f;
        const bool inlineSearch = ImGui::GetContentRegionAvail().x >= controlsWidth + 160.0f;
        ImGui::SetNextItemWidth(inlineSearch ? ImGui::GetContentRegionAvail().x - controlsWidth : -1.0f);
        ui::SearchInput("##file_search",
                        tr("Search field or description", "搜索字段或说明"),
                        parameterFileSearch_,
                        sizeof(parameterFileSearch_));
        if (inlineSearch)
                ImGui::SameLine();
        if (ImGui::Button(expandLabel))
                parameterTreeExpand_ = 1;
        ImGui::SameLine();
        if (ImGui::Button(collapseLabel))
                parameterTreeExpand_ = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (deviceValuesCurrent) {
                        ui::ErrorText("%s", tr("Red: read differs from write", "红色：读取与写入不一致"));
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.20f, 1.0f),
                                           "%s",
                                           tr("Yellow: read differs from defaults", "黄色：读取与默认参数不一致"));
                }
                ImGui::EndTooltip();
        }
        if (ui::SearchOpen("##file_search")) {
                const auto                    query = ui::search::LowerCopy(parameterFileSearch_);
                std::vector<ui::SearchResult> results;
                std::vector<std::string>      paths;
                for (const auto &field : parameterFile_.document.fields) {
                        if (field.role == "derived")
                                continue;
                        if (ui::search::LowerCopy(field.row.path + " " + field.row.description).find(query) ==
                            std::string::npos)
                                continue;
                        results.push_back({field.row.path, field.row.description});
                        paths.push_back(field.row.path);
                }
                const int selected = ui::SearchResults("##file_search", results);
                if (selected >= 0) {
                        parameterSearchSelected_ = paths[selected];
                        parameterSearchScroll_   = true;
                        parameterComponentTab_   = 3;
                        const auto section       = parameterSearchSelected_.substr(0, parameterSearchSelected_.find('.'));
                        for (int i = 0; i < 3; ++i)
                                if (section == parameter_json::componentSections[i])
                                        parameterComponentTab_ = i;
                        parameterNavigateTab_   = parameterComponentTab_;
                        parameterFileSearch_[0] = 0;
                }
        }
        if (parameterEditBefore_ && parameterEditActive_ != ImGui::GetActiveID()) {
                parameterHistory_.record(*parameterEditBefore_, captureParameterDraft());
                parameterEditBefore_.reset();
        }
        const auto beforeFields = captureParameterDraft();
        ImGui::PushID(parameterComponentTab_);
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
        if (ui::HierarchyTable tableCard;
            loaded && tableCard.begin("##parameter_file",
                                      5,
                                      ui::HierarchyTableFlags | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                      ImVec2(0, 0))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("Field", "字段"), ImGuiTableColumnFlags_WidthStretch, 0.48f);
                ImGui::TableSetupColumn(tr("Write", "写入"), ImGuiTableColumnFlags_WidthStretch, 0.27f);
                ImGui::TableSetupColumn(tr("Read", "读取"), ImGuiTableColumnFlags_WidthStretch, 0.27f);
                ImGui::TableSetupColumn(tr("Unit", "单位"), ImGuiTableColumnFlags_WidthStretch, 0.10f);
                ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthStretch, 0.15f);
                ui::HierarchyHeadersRow();
                const auto &root           = parameterFile_.fieldTree();
                using Difference           = parameter_json::ReadDifference;
                const auto differenceColor = [](Difference difference) {
                        return difference == Difference::Write ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                                                               : ImVec4(1.0f, 0.85f, 0.20f, 1.0f);
                };
                const auto differenceText = [](Difference difference) {
                        return difference == Difference::Write
                                   ? tr("Read and write values differ", "读取与写入值不一致")
                                   : tr("Read and default parameter values differ", "读取与默认参数值不一致");
                };
                const auto differs = [&](size_t index) {
                        const auto &field  = parameterFile_.document.fields[index];
                        const auto  actual = parameterDeviceValues_.find(field.row.path);
                        return parameter_json::readDifference(
                            field, deviceValuesCurrent && actual != parameterDeviceValues_.end() ? &actual->second : nullptr);
                };
                std::unordered_map<const parameter_json::TreeNode *, Difference> differenceGroups;
                std::function<Difference(const parameter_json::TreeNode &)>      collectDifferences = [&](const auto &node) {
                        auto difference =
                            node.field != std::numeric_limits<size_t>::max() ? differs(node.field) : Difference::None;
                        for (const auto &child : node.children)
                                difference = std::max(collectDifferences(child), difference);
                        if (difference != Difference::None && !node.children.empty())
                                differenceGroups.emplace(&node, difference);
                        return difference;
                };
                if (deviceValuesCurrent)
                        collectDifferences(root);
                const auto visibleField = [&](size_t index) {
                        const auto &field = parameterFile_.document.fields[index];
                        if (field.role == "derived")
                                return false;
                        return true;
                };
                std::function<bool(const parameter_json::TreeNode &)> visible = [&](const auto &node) {
                        if (node.field != std::numeric_limits<size_t>::max() && visibleField(node.field))
                                return true;
                        return std::any_of(
                            node.children.begin(), node.children.end(), [&](const auto &child) { return visible(child); });
                };
                std::function<void(const parameter_json::TreeNode &, int)> drawNode = [&](const auto &node, int depth) {
                        if (!visible(node))
                                return;
                        ImGui::PushID(node.path.c_str());
                        ui::HierarchyCard card;
                        if (!node.children.empty()) {
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                const bool revealSearch =
                                    parameterSearchScroll_ && parameterSearchSelected_.starts_with(node.path);
                                if (revealSearch || parameterTreeExpand_ >= 0)
                                        ImGui::SetNextItemOpen(revealSearch || parameterTreeExpand_ == 1);
                                const bool different = differenceGroups.contains(&node);
                                if (different)
                                        ImGui::PushStyleColor(ImGuiCol_Text, differenceColor(differenceGroups.at(&node)));
                                const bool expanded = ui::HierarchyNode(node.name.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
                                ui::HierarchyRow(expanded ? depth : ImMax(0, depth - 1));
                                if (depth == 0 || expanded)
                                        card.begin(depth);
                                if (different) {
                                        ImGui::PopStyleColor();
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", differenceText(differenceGroups.at(&node)));
                                }
                                if (expanded) {
                                        for (const auto &child : node.children)
                                                drawNode(child, depth + 1);
                                        ImGui::TreePop();
                                }
                        } else if (node.field != std::numeric_limits<size_t>::max()) {
                                auto      &field            = parameterFile_.document.fields[node.field];
                                auto      &row              = field.row;
                                const auto beforeDifference = differs(node.field);
                                const bool wasDifferent     = beforeDifference != Difference::None;
                                ui::HierarchyNextRow();
                                if (row.path == parameterSearchSelected_) {
                                        ui::HierarchySelection();
                                        if (parameterSearchScroll_) {
                                                ImGui::SetScrollHereY(0.5f);
                                                parameterSearchScroll_ = false;
                                        }
                                }
                                ui::HierarchyRow(ImMax(0, depth - 1));
                                if (depth == 0)
                                        card.begin(depth);
                                ImGui::TableSetColumnIndex(0);
                                if (wasDifferent)
                                        ImGui::PushStyleColor(ImGuiCol_Text, differenceColor(beforeDifference));
                                ui::HierarchyNode(node.name.c_str(),
                                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                      ImGuiTreeNodeFlags_SpanAvailWidth);
                                if (wasDifferent)
                                        ImGui::PopStyleColor();
                                if (ImGui::IsItemHovered() && (wasDifferent || !row.description.empty()))
                                        ImGui::SetTooltip("%s%s%s",
                                                          wasDifferent ? differenceText(beforeDifference) : "",
                                                          wasDifferent && !row.description.empty() ? "\n" : "",
                                                          row.description.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1);
                                if (wasDifferent) {
                                        const bool red = beforeDifference == Difference::Write;
                                        ImGui::PushStyleColor(ImGuiCol_FrameBg,
                                                              red ? ImVec4(0.55f, 0.08f, 0.08f, 0.70f)
                                                                  : ImVec4(0.50f, 0.40f, 0.03f, 0.70f));
                                        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                                                              red ? ImVec4(0.65f, 0.12f, 0.12f, 0.80f)
                                                                  : ImVec4(0.60f, 0.48f, 0.05f, 0.80f));
                                        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                                                              red ? ImVec4(0.75f, 0.16f, 0.16f, 0.90f)
                                                                  : ImVec4(0.70f, 0.56f, 0.07f, 0.90f));
                                }
                                if (field.display == "checkbox" || row.type == "bool") {
                                        bool checked = row.val == "1" || row.val == "true";
                                        if (ImGui::Checkbox("##value", &checked)) {
                                                row.val              = checked ? "1" : "0";
                                                parameterFile_.dirty = true;
                                        }
                                } else if (!field.enums.empty()) {
                                        std::string label = row.val;
                                        for (const auto &[name, val] : field.enums)
                                                if (val == row.val)
                                                        label = name;
                                        if (ImGui::BeginCombo("##value", label.c_str())) {
                                                for (const auto &[name, val] : parameter_json::enumOptionsByValue(field))
                                                        if (ImGui::Selectable(name.c_str(), val == row.val)) {
                                                                row.val              = val;
                                                                parameterFile_.dirty = true;
                                                        }
                                                ImGui::EndCombo();
                                        }
                                } else {
                                        if (!field.autoWhenZero.empty()) {
                                                try {
                                                        if (parameter_csv::number(row.val) == 0)
                                                                ImGui::TextUnformatted(tr("Automatic (0)", "自动（0）"));
                                                } catch (...) {
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s", field.autoWhenZero.c_str());
                                        }
                                        std::vector<char> buffer(std::max<size_t>(256, row.val.size() + 64));
                                        std::copy(row.val.begin(), row.val.end(), buffer.begin());
                                        if (ImGui::InputText("##value", buffer.data(), buffer.size())) {
                                                row.val              = buffer.data();
                                                parameterFile_.dirty = true;
                                        }
                                }
                                if (wasDifferent)
                                        ImGui::PopStyleColor(3);
                                const auto difference = differs(node.field);
                                const bool different  = difference != Difference::None;
                                if (different) {
                                        auto background = differenceColor(difference);
                                        background.w    = 0.14f;
                                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(background));
                                }
                                ImGui::TableSetColumnIndex(2);
                                const auto actual = parameterDeviceValues_.find(row.path);
                                if (deviceValuesCurrent && actual != parameterDeviceValues_.end()) {
                                        if (field.display == "checkbox" || row.type == "bool") {
                                                bool checked = actual->second == "1" || actual->second == "true";
                                                if (different)
                                                        ImGui::PushStyleColor(ImGuiCol_CheckMark, differenceColor(difference));
                                                ImGui::BeginDisabled();
                                                ImGui::Checkbox("##read_value", &checked);
                                                ImGui::EndDisabled();
                                                if (different)
                                                        ImGui::PopStyleColor();
                                        } else {
                                                std::string val = actual->second;
                                                for (const auto &[name, number] : field.enums)
                                                        if (number == val) {
                                                                val = name;
                                                                break;
                                                        }
                                                if (different)
                                                        ImGui::TextColored(differenceColor(difference), "%s", val.c_str());
                                                else
                                                        ImGui::TextUnformatted(val.c_str());
                                        }
                                } else
                                        ImGui::TextDisabled("%s", tr("Not read", "未读取"));
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextUnformatted(row.unit.c_str());
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted(row.type.c_str());
                                if (depth == 0)
                                        card.end(false);
                        }
                        ImGui::PopID();
                };
                // Shared fields retain their canonical paths and appear only on the Common page.
                for (const auto &node : root.children) {
                        if (parameterComponentTab_ == 3) {
                                if (!parameter_json::isComponent(node.name))
                                        drawNode(node, 0);
                        } else if (node.name == parameter_json::componentSections[parameterComponentTab_]) {
                                if (node.children.empty())
                                        drawNode(node, 0);
                                else
                                        for (const auto &child : node.children)
                                                drawNode(child, 0);
                        }
                }
                parameterTreeExpand_ = -1;
                tableCard.end();
                try {
                        parameterFile_.refreshNames();
                } catch (const std::exception &e) {
                        setParameterFileStatus(parameterError(e.what()), true);
                }
        }
        ImGui::PopStyleVar();
        if (parameterEditBefore_ && parameterEditActive_ && parameterEditActive_ != ImGui::GetActiveID()) {
                parameterHistory_.record(*parameterEditBefore_, beforeFields);
                parameterEditBefore_.reset();
        }
        const auto afterFields = captureParameterDraft();
        if (beforeFields != afterFields)
                isModified_ = true;
        if (beforeFields != afterFields && !parameterEditBefore_)
                parameterEditBefore_ = beforeFields;
        parameterEditActive_ = ImGui::GetActiveID();
        if (parameterEditBefore_ && !ImGui::IsAnyItemActive()) {
                parameterHistory_.record(*parameterEditBefore_, afterFields);
                parameterEditBefore_.reset();
        }
        if (open_ && parameterFileOpen_)
                ui::interaction::registerRegion(
                    std::to_string(parameterComponentTab_),
                    parameterHistory_.canUndo(),
                    parameterHistory_.canRedo(),
                    true,
                    [this](Action action) { parameterAction_ = action; },
                    [this](const std::string &route) {
                            if (route.size() == 1 && route[0] >= '0' && route[0] <= '3')
                                    parameterNavigateTab_ = route[0] - '0';
                    });
        ImGui::PopID();
        ImGui::End();
}

bool
Variable::updateParameterDevice(bool apply)
{
        clearParameterHistory();
        if (parameterDevice_.variableName != parameterFile_.document.target &&
            rewrite_.variableName == parameterFile_.document.target) {
                parameterDevice_.source       = rewrite_.source;
                parameterDevice_.variableName = rewrite_.variableName;
        }
        std::swap(rewrite_, parameterDevice_);
        bool ok = false;
        try {
                const auto content = apply ? parameterFile_.save() : std::string{};
                ok                 = prepareParameterDevice(parameterFile_.document.target);
                if (ok && apply)
                        ok = importRewriteJson(content);
                if (ok && apply)
                        ok = commitRewrite();
                if (ok) {
                        parameterDeviceValues_.clear();
                        std::unordered_map<std::string, RewriteLeaf> leaves;
                        collectRewriteLeaves(rewrite_.info,
                                             rewrite_.typeOff,
                                             0,
                                             "",
                                             leaves,
                                             0,
                                             rewrite_.source.bitOffset,
                                             rewrite_.source.bitSize);
                        if (rewrite_.isFocCfg)
                                mergeManifestLeaves(leaves, parameterManifestCache_);
                        for (const auto &[path, leaf] : leaves) {
                                const auto size = Parser::typeBytes(leaf.type);
                                if (variable_rewrite::range(leaf.offset, size, rewrite_.original.size()))
                                        parameterDeviceValues_[path] = rewriteValue(
                                            rewrite_.original.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize);
                        }
                        if (rewrite_.source.port == PortType::JLINK &&
                            JLinkPort::instance().targetEpoch() != rewrite_.targetEpoch)
                                throw std::runtime_error("Device connection changed; read again");
                        if (!apply) {
                                auto next         = parameterFile_;
                                bool modelChanged = false;
                                if (rewrite_.isFocCfg) {
                                        std::string deviceName;
                                        for (size_t i = 0; i < 64; ++i) {
                                                const auto leaf = leaves.find("name[" + std::to_string(i) + "]");
                                                if (leaf == leaves.end() || leaf->second.offset >= rewrite_.original.size())
                                                        throw std::runtime_error("Missing configuration name");
                                                const char byte = static_cast<char>(rewrite_.original[leaf->second.offset]);
                                                if (!byte)
                                                        break;
                                                deviceName.push_back(byte);
                                        }
                                        if (deviceName.size() > 63)
                                                throw std::runtime_error("Configuration name exceeds 63 UTF-8 bytes");
                                        if (deviceName.empty())
                                                deviceName = "CUSTOM";
                                        const auto previous = parameter_json::componentNames(next.document);
                                        next.document.name  = deviceName;
                                        next.document.components.clear();
                                        const auto current = parameter_json::componentNames(next.document);
                                        for (auto &field : next.document.fields) {
                                                const auto section = field.row.path.substr(0, field.row.path.find('.'));
                                                if (!parameter_json::isComponent(section))
                                                        continue;
                                                const auto old = previous.find(section);
                                                if (old == previous.end() || parameter_json::baseName(old->second) !=
                                                                                 parameter_json::baseName(current.at(section)))
                                                        field.presetValue.reset();
                                        }
                                        addParameterBaselines(next.document);
                                        if (previous != current) {
                                                next.dirty   = true;
                                                modelChanged = true;
                                        }
                                }
                                next.readValues(parameterDeviceValues_);
                                isModified_    = true;
                                parameterFile_ = std::move(next);
                                if (modelChanged) {
                                        parameterFilePath_.clear();
                                        parameterFileCombination_ = true;
                                }
                        }
                        setParameterFileStatus(
                            apply ? rewrite_.status : tr("Parameters read", "已读取参数并填入写入控件"), false, true);
                } else {
                        parameterDeviceValues_.clear();
                        setParameterFileStatus(rewrite_.status.empty()
                                                   ? tr("Cannot read device parameters; check connection and AXF",
                                                        "无法读取设备参数，请检查连接和 AXF")
                                                   : rewrite_.status,
                                               true);
                }
        } catch (const std::exception &e) {
                ok = false;
                parameterDeviceValues_.clear();
                setParameterFileStatus(parameterError(e.what()), true);
        }
        std::swap(rewrite_, parameterDevice_);
        return ok;
}

void
Variable::setParameterTarget(const std::string &target)
{
        clearParameterHistory();
        if (target.empty())
                throw std::runtime_error("Specify a target variable first");
        if (parameterFile_.source.empty()) {
                auto document   = parameterFile_.document;
                document.target = target;
                parameterFile_.load(parameter_json::format(document));
        } else {
                parameterFile_.document.target = target;
        }
        parameterFile_.dirty     = true;
        parameterTargetOverride_ = target;
        isModified_              = true;
        parameterDevice_         = {};
        parameterDeviceValues_.clear();
        parameterReadPending_ = false;
        setParameterFileStatus(tr("Target updated; click Read parameters", "目标变量已更新，请点击读取参数"), false, true);
}

bool
Variable::prepareParameterDevice(const std::string &target)
{
        try {
                if (target.empty())
                        throw std::runtime_error("Specify a target variable first");
                // 已配置的本地变量使用其端口和缓冲区, ELF 变量按完整路径匹配
                const auto watch =
                    std::find_if(vars_.begin(), vars_.end(), [&](const auto &entry) { return entry.name == target; });
                VarEntry entry{};
                if (watch != vars_.end()) {
                        entry = *watch;
                } else {
                        std::lock_guard lock(mtxElf_);
                        const auto      symbol = std::find_if(
                            searchPool_.begin(), searchPool_.end(), [&](const auto &item) { return item.path == target; });
                        if (symbol == searchPool_.end())
                                throw std::runtime_error("Target variable was not found in the loaded ELF: " + target);
                        const auto *type = resolveAlias(dwarfInfo_, symbol->typeOff);
                        if (!type)
                                throw std::runtime_error("Target variable has no DWARF type: " + target);
                        entry.name      = target;
                        entry.addr      = symbol->addr;
                        entry.typeOff   = symbol->typeOff;
                        entry.type      = symbol->type;
                        entry.isStruct  = type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION;
                        entry.isUnion   = type->kind == dwarf::TypeKind::UNION;
                        entry.isArray   = type->kind == dwarf::TypeKind::ARRAY;
                        entry.bitOffset = symbol->bitOffset;
                        entry.bitSize   = symbol->bitSize;
                        entry.port      = PortType::JLINK;
                        entry.writable  = true;
                }
                openRewrite(entry);
                prepareRewrite();
                return rewrite_.targetLoaded;
        } catch (const std::exception &e) {
                rewrite_.status   = parameterError(e.what());
                rewrite_.statusOk = rewrite_.targetLoaded = false;
                return false;
        }
}

bool
Variable::commitRewrite()
{
        auto &dialog = rewrite_;
        if (!dialog.targetLoaded || !syncRewriteName() || !validateRewriteCsv())
                return false;
        if (dialog.source.port == PortType::JLINK && JLinkPort::instance().isBusy()) {
                dialog.status   = tr("Device is busy", "设备正忙");
                dialog.statusOk = false;
                return false;
        }
        bool writeAttempted = false;
        try {
                bool ok = dialog.source.port != PortType::JLINK ||
                          (JLinkPort::instance().isConnected() && JLinkPort::instance().targetEpoch() == dialog.targetEpoch);
                std::string error;
                {
                        std::lock_guard lk(mtxElf_);
                        if (dialog.source.typeOff && dialog.layoutGeneration != elfLayoutGeneration_) {
                                ok    = false;
                                error = tr("Symbols were reloaded; close and reopen this variable",
                                           "符号已重新加载，请关闭后重新打开此变量");
                        }
                }
                if (!ok && error.empty())
                        error = tr("Target changed or disconnected; read current value again",
                                   "目标已变更或断开，请重新读取当前值");
                if (ok && dialog.guardedFoc && dialog.source.port == PortType::JLINK) {
                        verifyRewriteFirmware(false);
                        u64 state = 0;
                        ok        = dialog.stateAddress && dialog.stateAddress <= UINT32_MAX && dialog.stateSize &&
                             dialog.stateSize <= sizeof(state) &&
                             JLinkPort::instance().readMem(static_cast<u32>(dialog.stateAddress), dialog.stateSize, &state) &&
                             (state == 0 || state == 3);
                        if (!ok)
                                error = tr("FOC must be DISABLE or FAULT; matching AXF state symbol is required",
                                           "FOC 必须处于 DISABLE 或 FAULT，且 AXF 中需包含状态变量");
                }
                std::unordered_map<std::string, RewriteLeaf> leaves;
                collectRewriteLeaves(
                    dialog.info, dialog.typeOff, 0, "", leaves, 0, dialog.source.bitOffset, dialog.source.bitSize);
                if (leaves.size() >= 65536) {
                        ok    = false;
                        error = tr("Too many fields; open a smaller sub-variable", "字段过多，请选择较小的子变量编辑");
                }
                std::vector<u8> masks(dialog.bytes.size(), 0);
                for (const auto &[path, leaf] : leaves) {
                        const auto size = Parser::typeBytes(leaf.type);
                        if (!size || size > 8 || !variable_rewrite::range(leaf.offset, size, masks.size()))
                                continue;
                        u64 before = 0, after = 0;
                        std::memcpy(&before, dialog.original.data() + leaf.offset, size);
                        std::memcpy(&after, dialog.bytes.data() + leaf.offset, size);
                        const auto mask = variable_rewrite::mask(
                            size, leaf.bitSize ? leaf.bitOffset : 0, leaf.bitSize ? leaf.bitSize : size * 8);
                        if (((before ^ after) & mask) == 0)
                                continue;
                        for (u32 i = 0; i < size; ++i)
                                masks[leaf.offset + i] |= static_cast<u8>(mask >> (i * 8));
                }
                std::vector<u8> current(dialog.bytes.size()), merged;
                if (ok)
                        ok = readRewrite(current);
                if (ok && !variable_rewrite::merge(dialog.original, dialog.bytes, current, masks, merged)) {
                        ok    = false;
                        error = tr("An edited field changed on the target. Read it again before writing.",
                                   "正在编辑的字段已被目标修改，请重新读取后再写入。");
                }
                if (ok && dialog.isFocCfg) {
                        // Unedited fields may have changed on the target
                        // Recompute the label from the merged configuration,
                        // then include it in the same conflict-checked write
                        exportRewriteJson(&merged);
                        auto namedEdits = dialog.bytes;
                        for (const auto &[path, leaf] : leaves) {
                                if (!path.starts_with("name["))
                                        continue;
                                namedEdits[leaf.offset] = merged[leaf.offset];
                                masks[leaf.offset]      = 0xff;
                        }
                        if (!variable_rewrite::merge(dialog.original, namedEdits, current, masks, merged)) {
                                ok    = false;
                                error = tr("Configuration name changed on the target; read it again",
                                           "配置名称已被目标修改，请重新读取后再写入");
                        }
                }
                if (ok && dialog.source.port == PortType::JLINK && JLinkPort::instance().targetEpoch() != dialog.targetEpoch) {
                        ok    = false;
                        error = tr("Target changed; read it again", "目标已变更，请重新读取");
                }
                if (ok) {
                        writeAttempted = true;
                        ok             = writeRewrite(merged, masks);
                }
                std::vector<u8> verify(dialog.bytes.size());
                // A failed write may have partially reached the device; always
                // attempt readback after a write, and never report it as success
                if (writeAttempted) {
                        const bool readBack = readRewrite(verify);
                        ok                  = readBack && ok;
                }
                if (ok && dialog.source.port == PortType::JLINK)
                        ok = JLinkPort::instance().targetEpoch() == dialog.targetEpoch;
                if (ok)
                        for (size_t i = 0; i < verify.size(); ++i)
                                if (((verify[i] ^ merged[i]) & masks[i]) != 0) {
                                        ok = false;
                                        break;
                                }
                if (ok) {
                        dialog.original = dialog.bytes = std::move(verify);
                        dialog.status                  = dialog.source.port == PortType::LOCAL
                                                             ? tr("Local value updated; bound write accessor queued if configured",
                                                 "本地值已更新；如配置了写函数，已提交调用")
                                                             : tr("Changes written and read back successfully", "改动已写入并回读校验成功");
                } else
                        dialog.status = error.empty() ? tr("Write/readback failed; read current values before retrying",
                                                           "写入或回读失败，请重新读取后重试")
                                                      : error;
                dialog.statusOk = ok;
                if (!ok && writeAttempted)
                        dialog.targetLoaded = false;
        } catch (const std::exception &e) {
                dialog.statusOk = false;
                if (writeAttempted)
                        dialog.targetLoaded = false;
                dialog.status = std::string(tr("Write failed: ", "写入失败：")) + parameterError(e.what());
        }
        return dialog.statusOk;
}

void
Variable::drawRewriteDialog()
{
        if (rewrite_.pending)
                prepareRewrite();
        auto &dialog = rewrite_;
        if (!dialog.open)
                return;
        const std::string title = std::string(tr("Modify ", "修改 ")) + dialog.variableName + "###RewriteVariable" + name_;
        if (dialog.requestOpen) {
                ImGui::SetNextWindowFocus();
                dialog.requestOpen = false;
        }
        ImGui::SetNextWindowSize(ImVec2(980.0f, 720.0f), ImGuiCond_FirstUseEver);
        bool keepOpen = dialog.open;
        if (!dock_close::begin(title.c_str(), &keepOpen)) {
                ImGui::End();
                dialog.open = keepOpen;
                return;
        }
        dialog.open = keepOpen;

        const bool jlink         = dialog.source.port == PortType::JLINK;
        const bool connected     = !jlink || JLinkPort::instance().isConnected();
        const bool layoutCurrent = !jlink || dialog.layoutGeneration == elfLayoutGeneration_;
        const bool epochCurrent  = !jlink || dialog.targetEpoch == JLinkPort::instance().targetEpoch();
        const bool deviceBusy    = jlink && JLinkPort::instance().isBusy();
        if (!layoutCurrent)
                ImGui::TextWrapped("%s",
                                   tr("AXF changed. Reopen this variable to use the new layout.",
                                      "AXF 已变更，请重新打开此变量以使用新布局。"));
        else if (!connected)
                ImGui::TextWrapped("%s",
                                   tr("J-Link is disconnected. Reconnect and reopen this variable to read current values.",
                                      "J-Link 未连接，请连接后重新打开此变量读取当前值。"));
        else if (!epochCurrent)
                ImGui::TextWrapped("%s",
                                   tr("MCU reset or connection changed. Reopen this variable before writing.",
                                      "MCU 已复位或连接已变化，请重新打开此变量后再写入。"));
        if (!connected && jlink) {
                ImGui::BeginDisabled(deviceBusy);
                if (ImGui::Button(tr("Connect J-Link", "连接 J-Link")))
                        JLinkPort::instance().connectAsync();
                ImGui::EndDisabled();
                ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##rewrite_search", tr("Search field", "搜索字段"), dialog.search, sizeof(dialog.search));

        if (!dialog.status.empty()) {
                const ImVec4 color = dialog.statusOk ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f) : ui::ErrorTextColor();
                ImGui::TextColored(color, "%s", dialog.status.c_str());
        }
        if (ImGui::Button(tr("Expand all", "展开全部")))
                dialog.expandMode = 1;
        ImGui::SameLine();
        if (ImGui::Button(tr("Collapse all", "折叠全部")))
                dialog.expandMode = 0;
        ImGui::SameLine();
        ImGui::Checkbox(tr("Changed only", "只看改动"), &dialog.changedOnly);
        ImGui::SameLine();
        ImGui::BeginDisabled(!dialog.targetLoaded || dialog.bytes == dialog.original);
        if (ImGui::Button(tr("Reset edits", "恢复原值")))
                dialog.bytes = dialog.original;
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!dialog.targetLoaded);
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
        if (ui::HierarchyTable tableCard;
            tableCard.begin("##rewrite_editor",
                            4,
                            ui::HierarchyTableFlags | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                            ImVec2(0.0f, -42.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("Field", "字段"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn(tr("Original", "原值"), ImGuiTableColumnFlags_WidthStretch, 0.20f);
                ImGui::TableSetupColumn(tr("New value", "修改值"), ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ui::HierarchyHeadersRow();
                ui::HierarchyNextRow();
                ImGui::TableSetColumnIndex(0);
                if (dialog.search[0] || dialog.changedOnly || !dialog.csvRows.empty()) {
                        std::unordered_map<std::string, RewriteLeaf> leaves;
                        collectRewriteLeaves(
                            dialog.info, dialog.typeOff, 0, "", leaves, 0, dialog.source.bitOffset, dialog.source.bitSize);
                        std::vector<std::string> paths;
                        for (const auto &[path, leaf] : leaves)
                                paths.push_back(path);
                        std::sort(paths.begin(), paths.end());
                        for (const auto &path : paths) {
                                const auto       &leaf  = leaves.at(path);
                                const std::string label = path.empty()
                                                              ? dialog.variableName
                                                              : dialog.variableName + (path[0] == '[' ? "" : ".") + path;
                                std::string       query = dialog.search, searchable = label;
                                std::ranges::transform(query, query.begin(), [](unsigned char c) { return std::tolower(c); });
                                std::ranges::transform(
                                    searchable, searchable.begin(), [](unsigned char c) { return std::tolower(c); });
                                if (!query.empty() && searchable.find(query) == std::string::npos)
                                        continue;
                                const size_t size = Parser::typeBytes(leaf.type);
                                if (!variable_rewrite::range(leaf.offset, size, dialog.bytes.size()))
                                        continue;
                                if (dialog.changedOnly && dialog.original.size() == dialog.bytes.size() &&
                                    rewriteValue(
                                        dialog.original.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize) ==
                                        rewriteValue(
                                            dialog.bytes.data() + leaf.offset, leaf.type, leaf.bitOffset, leaf.bitSize))
                                        continue;
                                ui::HierarchyNextRow();
                                ImGui::TableSetColumnIndex(0);
                                if (auto it = dialog.csvRows.find(path); it != dialog.csvRows.end()) {
                                        const auto &r = it->second;
                                        ImGui::TextDisabled("%s  %s  [%s, %s]",
                                                            r.description.c_str(),
                                                            r.unit.c_str(),
                                                            r.min.c_str(),
                                                            r.max.c_str());
                                }
                                drawRewriteNode(leaf.typeOff, leaf.offset, label, 1, leaf.bitOffset, leaf.bitSize);
                        }
                } else {
                        drawRewriteNode(
                            dialog.typeOff, 0, dialog.variableName, 0, dialog.source.bitOffset, dialog.source.bitSize);
                }
                dialog.expandMode = -1;
                tableCard.end();
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();

        const bool nameReady = syncRewriteName();
        const bool dirty     = dialog.targetLoaded && dialog.bytes != dialog.original;
        ImGui::BeginDisabled(!dirty || !connected || !epochCurrent || !layoutCurrent || deviceBusy);
        if (ImGui::Button(tr("Write changes", "写入改动")) && nameReady)
                commitRewrite();
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(tr("Close", "关闭"))) {
                dialog.open = false;
        }
        ImGui::End();
}

void
Variable::draw()
{
        drawFirmwareStatus();
        {
                std::lock_guard lk(mtxElf_);
                if (!elfLoadError_.empty()) {
                        ImGui::InsertNotification({ImGuiToastType::Error, 7000, "%s", parameterError(elfLoadError_).c_str()});
                        elfLoadError_.clear();
                }
        }
        // Rebuilt while drawing the currently expanded tree. updateVariables()
        // publishes this set to the background reader on the next refresh tick
        memberPollReqs_.clear();
        updateWatchCopy();

        if (ImGui::BeginChild("TopSection", ImVec2(0, 0), false))
                drawVariableList();
        ImGui::EndChild();

        drawPointerInspector();
        drawAddVariableDialog();
        drawEnumEditPopup();
        drawSubEnumEditPopup();
        drawEditPropertiesPopup();
        drawFunctionBindingPopup();
        drawUnitConversionEditor();
        if (state_ == WindowState::LoadElf) {
                state_        = WindowState::None;
                std::string p = nativeDlgOpen("Choose Symbol File", {{"Symbol Files", {"elf", "axf", "out", "json", "bin"}}});
                if (!p.empty())
                        handleDroppedFile(p);
        }
        for (auto &path : Gui::getDroppedFiles())
                handleDroppedFile(path);
}

void
Variable::updateDisplay()
{
        applyPendingElf();
        if (!elfPath_.empty() && !remote::isUrl(elfPath_)) {
                try {
                        if (std::filesystem::exists(elfPath_)) {
                                auto currentWriteTime = std::filesystem::last_write_time(elfPath_);
                                if (elfLastWriteTime_ != std::filesystem::file_time_type{} &&
                                    currentWriteTime > elfLastWriteTime_) {
                                        if (loadElf(elfPath_)) {
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Info, 3000, "ELF Reloading: %s", elfPath_.c_str()});
                                        }
                                }
                                elfLastWriteTime_ = currentWriteTime;
                        }
                } catch (...) {
                }
        }
        updateVariables();
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        // "VisibleTitle###name_": the visible label tracks the user-editable title
        // while the trailing id keeps the ImGui window id (and dock layout) stable
        const std::string winLabel = getTitle() + "###" + name_;
        if (dock_close::begin(winLabel.c_str(), &open_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                TutorialGuide::instance().markDock(TutorialGuide::Topic::Variables);
                // Double-click or F2 renames dock tabs; only empty title-bar space folds panes
                ImGuiWindow *win       = ImGui::GetCurrentWindow();
                ImRect       titleRect = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                if ((win->DockIsActive && dock_fold::isTabLabelHovered(win) &&
                     (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_F2, false))) ||
                    (!win->DockIsActive && ImGui::IsMouseHoveringRect(titleRect.Min, titleRect.Max, false) &&
                     ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                        snprintf(renameBuf_, sizeof(renameBuf_), "%s", getTitle().c_str());
                        ImGui::OpenPopup("Rename Variable");
                }
                if (ImGui::BeginPopup("Rename Variable")) {
                        ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::IsWindowAppearing())
                                ImGui::SetKeyboardFocusHere();
                        bool commit =
                            ImGui::InputText("##renameVar",
                                             renameBuf_,
                                             sizeof(renameBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::SameLine();
                        if (ImGui::Button("OK"))
                                commit = true;
                        if (commit) {
                                if (renameBuf_[0] != '\0')
                                        setTitle(renameBuf_);
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
                draw();
        }
        ImGui::End();
        drawRewriteDialog();
        const bool parameterWasOpen = parameterFileOpen_;
        drawParameterFileEditor();
        if (parameterWasOpen != parameterFileOpen_)
                isModified_ = true;
}

void
Variable::save(void *node) const
{
        cJSON *root = static_cast<cJSON *>(node);
        {
                std::lock_guard lock(mtxLocal_);
                auto           *sources = cJSON_AddArrayToObject(root, "sampledLocalSources");
                for (const auto &v : vars_)
                        if (v.port == PortType::LOCAL && localSampleSources_.contains(v.name))
                                cJSON_AddItemToArray(sources, cJSON_CreateString(v.name.c_str()));
        }
        auto *editor = cJSON_AddObjectToObject(root, "parameterEditor");
        cJSON_AddBoolToObject(editor, "open", parameterFileOpen_);
        cJSON_AddBoolToObject(editor, "combination", parameterFileCombination_);
        cJSON_AddBoolToObject(editor, "dirty", parameterFile_.dirty);
        cJSON_AddNumberToObject(editor, "tab", parameterComponentTab_);
        cJSON_AddStringToObject(editor, "path", parameterFilePath_.c_str());
        const auto directory = parameterDirectory_.u8string();
        cJSON_AddStringToObject(editor, "directory", std::string(directory.begin(), directory.end()).c_str());
        cJSON_AddStringToObject(editor, "target", parameterTargetOverride_.c_str());
        cJSON_AddStringToObject(editor, "source", parameterFile_.source.c_str());
        auto *draft = cJSON_AddObjectToObject(editor, "draft");
        for (const auto &[path, value] : captureParameterDraft())
                cJSON_AddStringToObject(draft, path.c_str(), value.c_str());
        cJSON_AddNumberToObject(root, "displayRadix", displayRadix_);
        cJSON_AddNumberToObject(root, "updateIntervalMs", updateIntervalMs_);
        cJSON_AddBoolToObject(root, "symbolBrowserPinned", symbolBrowserHover_.isPinned());
        cJSON_AddBoolToObject(root, "elfFilterObjectsOnly", elfFilterObjectsOnly_);
        if (!functionSources_.empty()) {
                cJSON *sources = cJSON_CreateArray();
                for (const auto &source : functionSources_) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "libraryPath", source->libraryPath.c_str());
                        cJSON_AddStringToObject(item, "headerPath", source->headerPath.c_str());
                        cJSON_AddItemToArray(sources, item);
                }
                cJSON_AddItemToObject(root, "functionSources", sources);
        }
        auto saveTableUi = [root](const char *name, const TableUiState &state, int columnCount) {
                cJSON *table = cJSON_CreateObject();
                cJSON_AddNumberToObject(table, "sortColumn", state.sortColumn);
                cJSON_AddNumberToObject(table, "sortDirection", state.sortDirection);
                cJSON *widths = cJSON_CreateArray();
                for (int i = 0; i < columnCount && i < static_cast<int>(state.widths.size()); ++i)
                        cJSON_AddItemToArray(widths, cJSON_CreateNumber(state.widths[i]));
                cJSON_AddItemToObject(table, "columnWidths", widths);
                cJSON_AddItemToObject(root, name, table);
        };
        saveTableUi("watchTableUi", watchTableUi_, 6);
        saveTableUi("symbolTableUi", symbolTableUi_, 4);
        saveTableUi("binTableUi", binTableUi_, 3);
        cJSON *vArr = cJSON_CreateArray();
        for (const auto &v : vars_) {
                cJSON *vObj = cJSON_CreateObject();
                cJSON_AddStringToObject(vObj, "name", v.name.c_str());
                flasharray::save(vObj, v.flashArray);
                if (v.flashArray)
                        cJSON_AddBoolToObject(vObj, "flashArrayExpanded", v.expandedMembers.contains(v.name));
                cJSON_AddNumberToObject(vObj, "type", (int)v.type);
                cJSON_AddNumberToObject(vObj, "displayRadix", variableRadix(v));
                saveVariableDisplaySettings(vObj, v);
                cJSON_AddNumberToObject(vObj, "port", (int)v.port);
                cJSON_AddStringToObject(vObj, "portStr", portName(v.port));
                cJSON_AddNumberToObject(vObj, "addr", (double)v.addr);
                cJSON_AddBoolToObject(vObj, "writable", v.writable);
                cJSON_AddBoolToObject(vObj, "isStruct", v.isStruct);
                cJSON_AddBoolToObject(vObj, "isUnion", v.isUnion);
                cJSON_AddBoolToObject(vObj, "isArray", v.isArray);
                cJSON_AddNumberToObject(vObj, "typeOff", (double)v.typeOff);
                if (v.port == PortType::LOCAL) {
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        if (const auto it = localBufs_.find(v.name); it != localBufs_.end())
                                cJSON_AddNumberToObject(vObj, "localBufSize", static_cast<double>(it->second.size()));
                }
                auto saveBinding = [vObj](const char *key, const VarEntry::FunctionBinding &binding) {
                        if (binding.empty())
                                return;
                        cJSON *object = cJSON_CreateObject();
                        cJSON_AddStringToObject(object, "source", binding.sourcePath.c_str());
                        cJSON_AddStringToObject(object, "name", binding.functionName.c_str());
                        cJSON_AddStringToObject(object, "signature", binding.signature.c_str());
                        cJSON_AddItemToObject(vObj, key, object);
                };
                saveBinding("readFunction", v.readFunction);
                saveBinding("writeFunction", v.writeFunction);
                if (v.port == PortType::SHM) {
                        cJSON_AddStringToObject(vObj, "shmName", v.shm.name);
                }
                if (!v.structFields.empty()) {
                        cJSON *sfArr = cJSON_CreateArray();
                        for (const auto &sf : v.structFields) {
                                cJSON *fObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(fObj, "name", sf.name);
                                cJSON_AddNumberToObject(fObj, "type", (int)sf.type);
                                cJSON_AddNumberToObject(fObj, "offset", (double)sf.byteOffset);
                                cJSON_AddItemToArray(sfArr, fObj);
                        }
                        cJSON_AddItemToObject(vObj, "structFields", sfArr);
                }
                if (!v.hiddenMembers.empty()) {
                        cJSON *hmArr = cJSON_CreateArray();
                        for (const auto &p : v.hiddenMembers)
                                cJSON_AddItemToArray(hmArr, cJSON_CreateString(p.c_str()));
                        cJSON_AddItemToObject(vObj, "hiddenMembers", hmArr);
                }
                if (!v.expandedMembers.empty()) {
                        cJSON *emArr = cJSON_CreateArray();
                        for (const auto &p : v.expandedMembers)
                                cJSON_AddItemToArray(emArr, cJSON_CreateString(p.c_str()));
                        cJSON_AddItemToObject(vObj, "expandedMembers", emArr);
                }
                if (!v.memberEnumDefs.empty()) {
                        cJSON *medObj = cJSON_CreateObject();
                        for (const auto &[mpath, mdefs] : v.memberEnumDefs) {
                                cJSON *defArr = cJSON_CreateArray();
                                for (const auto &d : mdefs) {
                                        cJSON *dObj = cJSON_CreateObject();
                                        cJSON_AddStringToObject(dObj, "name", d.name.c_str());
                                        cJSON_AddNumberToObject(dObj, "value", (double)d.val);
                                        cJSON_AddItemToArray(defArr, dObj);
                                }
                                cJSON_AddItemToObject(medObj, mpath.c_str(), defArr);
                        }
                        cJSON_AddItemToObject(vObj, "memberEnumDefs", medObj);
                }
                if (!v.memberOverrides.empty()) {
                        cJSON *overridesObj = cJSON_CreateObject();
                        for (const auto &[path, ov] : v.memberOverrides) {
                                cJSON *ovObj = cJSON_CreateObject();
                                if (!ov.alias.empty())
                                        cJSON_AddStringToObject(ovObj, "alias", ov.alias.c_str());
                                if (ov.hasAddress)
                                        cJSON_AddNumberToObject(ovObj, "address", (double)ov.address);
                                if (ov.hasType)
                                        cJSON_AddNumberToObject(ovObj, "type", (int)ov.type);
                                if (ov.hasWritable)
                                        cJSON_AddBoolToObject(ovObj, "writable", ov.writable);
                                cJSON_AddItemToObject(overridesObj, path.c_str(), ovObj);
                        }
                        cJSON_AddItemToObject(vObj, "memberOverrides", overridesObj);
                }
                cJSON_AddItemToArray(vArr, vObj);
        }
        cJSON_AddItemToObject(root, "vars", vArr);
}

void
Variable::load(const void *node)
{
        focusWatch(-1, {});
        const cJSON *root = static_cast<const cJSON *>(node);
        {
                std::lock_guard lock(mtxLocal_);
                localSampleSources_.clear();
                localSamples_.clear();
                localSampleBytes_ = 0;
                if (const auto *sources = cJSON_GetObjectItem(root, "sampledLocalSources"); cJSON_IsArray(sources))
                        for (const auto *item = sources->child; item; item = item->next)
                                if (cJSON_IsString(item))
                                        localSampleSources_.insert(item->valuestring);
        }
        parameterFile_     = {};
        parameterFileOpen_ = parameterFileRequestOpen_ = parameterReadPending_ = false;
        parameterFilePath_.clear();
        parameterDirectory_.clear();
        clearParameterHistory();
        if (const auto *editor = cJSON_GetObjectItem(root, "parameterEditor"); cJSON_IsObject(editor)) {
                auto text = [&](const char *key) -> std::string {
                        const auto *value = cJSON_GetObjectItem(editor, key);
                        return cJSON_IsString(value) ? value->valuestring : "";
                };
                parameterFilePath_        = text("path");
                parameterDirectory_       = std::filesystem::u8path(text("directory"));
                parameterTargetOverride_  = text("target");
                parameterFileOpen_        = cJSON_IsTrue(cJSON_GetObjectItem(editor, "open"));
                parameterFileCombination_ = cJSON_IsTrue(cJSON_GetObjectItem(editor, "combination"));
                if (const auto *tab = cJSON_GetObjectItem(editor, "tab"); cJSON_IsNumber(tab))
                        parameterComponentTab_ = std::clamp(tab->valueint, 0, 3);
                parameterNavigateTab_ = parameterComponentTab_;
                try {
                        const auto source = text("source");
                        if (!source.empty()) {
                                parameterFile_.load(source);
                                ParameterDraft draft;
                                if (const auto *values = cJSON_GetObjectItem(editor, "draft"); cJSON_IsObject(values))
                                        for (auto *value = values->child; value; value = value->next)
                                                if (value->string && cJSON_IsString(value))
                                                        draft.emplace_back(value->string, value->valuestring);
                                restoreParameterDraft(draft);
                                if (!parameterTargetOverride_.empty())
                                        parameterFile_.document.target = parameterTargetOverride_;
                                parameterFile_.dirty = cJSON_IsTrue(cJSON_GetObjectItem(editor, "dirty"));
                        }
                } catch (const std::exception &error) {
                        setParameterFileStatus(parameterError(error.what()), true);
                }
        }

        stopAccessorThread();
        functionSources_.clear();
        if (const cJSON *sources = cJSON_GetObjectItem(root, "functionSources"); cJSON_IsArray(sources)) {
                for (const cJSON *item = sources->child; item; item = item->next) {
                        auto source = std::make_unique<FunctionSource>();
                        if (const cJSON *path = cJSON_GetObjectItem(item, "libraryPath"); cJSON_IsString(path))
                                source->libraryPath = path->valuestring;
                        if (const cJSON *path = cJSON_GetObjectItem(item, "headerPath"); cJSON_IsString(path))
                                source->headerPath = path->valuestring;
                        source->status = tr("Use Load/Reparse to activate this source.", "请使用加载/重新解析来启用此函数源。");
                        if (!source->libraryPath.empty() || !source->headerPath.empty())
                                functionSources_.push_back(std::move(source));
                }
        } else {
                auto source = std::make_unique<FunctionSource>();
                if (const cJSON *path = cJSON_GetObjectItem(root, "functionLibraryPath"); cJSON_IsString(path))
                        source->libraryPath = path->valuestring;
                if (const cJSON *path = cJSON_GetObjectItem(root, "functionHeaderPath"); cJSON_IsString(path))
                        source->headerPath = path->valuestring;
                source->status = tr("Use Load/Reparse to activate this source.", "请使用加载/重新解析来启用此函数源。");
                if (!source->libraryPath.empty() || !source->headerPath.empty())
                        functionSources_.push_back(std::move(source));
        }
        if (!functionSources_.empty()) {
                functionPanelOpen_ = true;
        }
        if (const cJSON *radix = cJSON_GetObjectItem(root, "displayRadix"); cJSON_IsNumber(radix))
                displayRadix_ = normalizeDisplayRadix(radix->valueint);
        if (const cJSON *interval = cJSON_GetObjectItem(root, "updateIntervalMs"); cJSON_IsNumber(interval))
                updateIntervalMs_ = static_cast<u32>(std::clamp(interval->valueint, 10, 2000));
        if (const cJSON *pinned = cJSON_GetObjectItem(root, "symbolBrowserPinned"); cJSON_IsBool(pinned))
                symbolBrowserHover_.setPinned(cJSON_IsTrue(pinned));
        functionPanelOpen_ = false;
        if (const cJSON *objectsOnly = cJSON_GetObjectItem(root, "elfFilterObjectsOnly"); cJSON_IsBool(objectsOnly))
                elfFilterObjectsOnly_ = cJSON_IsTrue(objectsOnly);
        watchTableUi_                = TableUiState{};
        watchTableUi_.widths         = {250.0f, 250.0f, 60.0f, 150.0f, 80.0f, 50.0f};
        watchTableUi_.restoreWidths  = true;
        watchTableUi_.restoreSort    = true;
        symbolTableUi_               = TableUiState{};
        symbolTableUi_.widths        = {300.0f, 120.0f, 60.0f, 240.0f, 0.0f, 0.0f};
        symbolTableUi_.restoreWidths = true;
        symbolTableUi_.restoreSort   = true;
        binTableUi_                  = TableUiState{};
        binTableUi_.widths           = {300.0f, 180.0f, 240.0f, 0.0f, 0.0f, 0.0f};
        binTableUi_.restoreWidths    = true;
        binTableUi_.restoreSort      = true;
        auto loadTableUi =
            [root](const char *name, TableUiState &state, int columnCount, const std::vector<int> &sortableColumns) {
                    const cJSON *table = cJSON_GetObjectItem(root, name);
                    if (!cJSON_IsObject(table))
                            return;
                    if (const cJSON *column = cJSON_GetObjectItem(table, "sortColumn"); cJSON_IsNumber(column)) {
                            const int candidate = column->valueint;
                            if (std::find(sortableColumns.begin(), sortableColumns.end(), candidate) != sortableColumns.end())
                                    state.sortColumn = candidate;
                    }
                    if (const cJSON *dir = cJSON_GetObjectItem(table, "sortDirection"); cJSON_IsNumber(dir)) {
                            if (dir->valueint == ImGuiSortDirection_Ascending || dir->valueint == ImGuiSortDirection_Descending)
                                    state.sortDirection = dir->valueint;
                    }
                    bool widthsLoaded = false;
                    if (const cJSON *widths = cJSON_GetObjectItem(table, "columnWidths"); cJSON_IsArray(widths)) {
                            const int count = std::min(columnCount, cJSON_GetArraySize(widths));
                            for (int i = 0; i < count; ++i) {
                                    const cJSON *width = cJSON_GetArrayItem(widths, i);
                                    if (cJSON_IsNumber(width) && width->valuedouble > 1.0) {
                                            state.widths[i] = static_cast<float>(width->valuedouble);
                                            widthsLoaded    = true;
                                    }
                            }
                    }
                    if (widthsLoaded)
                            state.restoreWidths = true;
                    state.restoreSort = true;
                    state.initialized = false;
            };
        loadTableUi("watchTableUi", watchTableUi_, 6, {0, 2, 3, 4, 5});
        loadTableUi("symbolTableUi", symbolTableUi_, 4, {0, 1, 2, 3});
        loadTableUi("binTableUi", binTableUi_, 3, {0, 1});
        const auto isUnsignedInteger = [](const cJSON *item) {
                return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && item->valuedouble >= 0.0 &&
                       item->valuedouble < std::ldexp(1.0, 64) && std::floor(item->valuedouble) == item->valuedouble;
        };
        size_t invalidEntries = 0;
        cJSON *vArr           = cJSON_GetObjectItem(root, "vars");
        if (cJSON_IsArray(vArr)) {
                vars_.clear();
                for (int i = 0; i < cJSON_GetArraySize(vArr); ++i) {
                        cJSON       *vObj = cJSON_GetArrayItem(vArr, i);
                        VarEntry     v{};
                        const cJSON *name = cJSON_GetObjectItem(vObj, "name");
                        if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
                                ++invalidEntries;
                                continue;
                        }
                        v.name = name->valuestring;
                        loadVariableDisplaySettings(vObj, v);
                        if (cJSON_GetObjectItemCaseSensitive(vObj, "flashArray")) {
                                try {
                                        restoreFlashArray(v, vObj);
                                        if (!watchHasIdentity(v.name, v.addr, v.type))
                                                vars_.push_back(std::move(v));
                                } catch (const std::exception &e) {
                                        ImGui::InsertNotification(
                                            {ImGuiToastType::Error, 7000, "%s", parameterError(e.what()).c_str()});
                                }
                                continue;
                        }
                        const cJSON *type    = cJSON_GetObjectItem(vObj, "type");
                        const cJSON *address = cJSON_GetObjectItem(vObj, "addr");
                        // These are session-format keys, independent of firmware symbol names
                        // Skip incomplete records instead of crashing or inventing a target address
                        if (!cJSON_IsNumber(type) || type->valuedouble != type->valueint ||
                            type->valueint < static_cast<int>(DataType::UNKNOWN) ||
                            type->valueint > static_cast<int>(DataType::F16) || !isUnsignedInteger(address)) {
                                ++invalidEntries;
                                continue;
                        }
                        if (const auto *radix = cJSON_GetObjectItem(vObj, "displayRadix"); cJSON_IsNumber(radix)) {
                                const int r    = radix->valueint;
                                v.displayRadix = r == 0 ? normalizeDisplayRadix(displayRadix_) : normalizeDisplayRadix(r);
                        }
                        v.type     = static_cast<DataType>(type->valueint);
                        v.port     = portFromJson(vObj);
                        v.addr     = static_cast<u64>(address->valuedouble);
                        v.writable = cJSON_IsTrue(cJSON_GetObjectItem(vObj, "writable"));
                        if (const cJSON *item = cJSON_GetObjectItem(vObj, "isStruct"); cJSON_IsBool(item))
                                v.isStruct = cJSON_IsTrue(item);
                        if (const cJSON *item = cJSON_GetObjectItem(vObj, "isUnion"); cJSON_IsBool(item))
                                v.isUnion = cJSON_IsTrue(item);
                        if (const cJSON *item = cJSON_GetObjectItem(vObj, "isArray"); cJSON_IsBool(item))
                                v.isArray = cJSON_IsTrue(item);
                        if (const cJSON *offset = cJSON_GetObjectItem(vObj, "typeOff"); isUnsignedInteger(offset))
                                v.typeOff = static_cast<u64>(offset->valuedouble);
                        auto loadBinding = [vObj](const char *key, VarEntry::FunctionBinding &binding) {
                                const cJSON *object = cJSON_GetObjectItem(vObj, key);
                                if (!cJSON_IsObject(object))
                                        return;
                                if (const cJSON *source = cJSON_GetObjectItem(object, "source"); cJSON_IsString(source))
                                        binding.sourcePath = source->valuestring;
                                if (const cJSON *name = cJSON_GetObjectItem(object, "name"); cJSON_IsString(name))
                                        binding.functionName = name->valuestring;
                                if (const cJSON *sig = cJSON_GetObjectItem(object, "signature"); cJSON_IsString(sig))
                                        binding.signature = sig->valuestring;
                        };
                        loadBinding("readFunction", v.readFunction);
                        loadBinding("writeFunction", v.writeFunction);
                        if (v.port == PortType::SHM) {
                                if (const cJSON *name = cJSON_GetObjectItem(vObj, "shmName"); cJSON_IsString(name))
                                        snprintf(v.shm.name, sizeof(v.shm.name), "%s", name->valuestring);
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type     = DataType::F32;
                                v.writable = false;
                                if (auto *adi = cJSON_GetObjectItem(vObj, "audioDeviceIndex"); cJSON_IsNumber(adi))
                                        v.audio.deviceIndex = adi->valueint;
                                else
                                        v.audio.deviceIndex = static_cast<int>(v.addr);
                                if (auto *adn = cJSON_GetObjectItem(vObj, "audioDeviceName"); cJSON_IsString(adn))
                                        snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", adn->valuestring);
                                else
                                        snprintf(v.audio.deviceName,
                                                 sizeof(v.audio.deviceName),
                                                 "%s",
                                                 AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                                v.addr = (u64)std::max(0, v.audio.deviceIndex);
                        }
                        if (cJSON *sfArr = cJSON_GetObjectItem(vObj, "structFields"); cJSON_IsArray(sfArr)) {
                                for (int si = 0; si < cJSON_GetArraySize(sfArr); ++si) {
                                        cJSON                *fObj = cJSON_GetArrayItem(sfArr, si);
                                        VarEntry::StructField sf;
                                        if (auto *n = cJSON_GetObjectItem(fObj, "name"); cJSON_IsString(n))
                                                strncpy(sf.name, n->valuestring, sizeof(sf.name) - 1);
                                        if (auto *ty = cJSON_GetObjectItem(fObj, "type"); cJSON_IsNumber(ty))
                                                sf.type = (DataType)(int)ty->valuedouble;
                                        if (auto *off = cJSON_GetObjectItem(fObj, "offset"); cJSON_IsNumber(off))
                                                sf.byteOffset = (u32)(int)off->valuedouble;
                                        v.structFields.push_back(sf);
                                }
                                v.isStruct = v.isStruct || !v.structFields.empty();
                        }
                        if (watchHasIdentity(v.name, v.addr, v.type))
                                continue;
                        if (v.port == PortType::LOCAL) {
                                size_t bsz = (v.addr > 0) ? (size_t)v.addr : 8;
                                if (!v.structFields.empty()) {
                                        bsz = 0;
                                        for (const auto &sf : v.structFields)
                                                bsz = std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
                                        if (bsz == 0)
                                                bsz = 8;
                                }
                                if (const cJSON *savedSize = cJSON_GetObjectItem(vObj, "localBufSize");
                                    cJSON_IsNumber(savedSize) && savedSize->valuedouble > 0.0)
                                        bsz = std::max(bsz, static_cast<size_t>(savedSize->valuedouble));
                                std::lock_guard<std::mutex> lk(mtxLocal_);
                                localBufs_.emplace(v.name, std::vector<uint8_t>(bsz, 0));
                        }
                        cJSON *hmArr = cJSON_GetObjectItem(vObj, "hiddenMembers");
                        if (cJSON_IsArray(hmArr))
                                for (int k = 0; k < cJSON_GetArraySize(hmArr); ++k)
                                        if (cJSON_IsString(cJSON_GetArrayItem(hmArr, k)))
                                                v.hiddenMembers.insert(cJSON_GetArrayItem(hmArr, k)->valuestring);
                        cJSON *emArr = cJSON_GetObjectItem(vObj, "expandedMembers");
                        if (cJSON_IsArray(emArr))
                                for (int k = 0; k < cJSON_GetArraySize(emArr); ++k)
                                        if (cJSON_IsString(cJSON_GetArrayItem(emArr, k)))
                                                v.expandedMembers.insert(cJSON_GetArrayItem(emArr, k)->valuestring);
                        cJSON *medObj = cJSON_GetObjectItem(vObj, "memberEnumDefs");
                        if (cJSON_IsObject(medObj)) {
                                for (cJSON *child = medObj->child; child; child = child->next) {
                                        if (!cJSON_IsArray(child))
                                                continue;
                                        std::vector<VarEntry::EnumDef> mdefs;
                                        for (int k = 0; k < cJSON_GetArraySize(child); ++k) {
                                                cJSON       *dObj = cJSON_GetArrayItem(child, k);
                                                const cJSON *name = cJSON_GetObjectItem(dObj, "name");
                                                const cJSON *val  = cJSON_GetObjectItem(dObj, "value");
                                                if (!cJSON_IsString(name) || !cJSON_IsNumber(val))
                                                        continue;
                                                VarEntry::EnumDef d{};
                                                d.name = name->valuestring;
                                                d.val  = (i64)val->valuedouble;
                                                mdefs.push_back(d);
                                        }
                                        v.memberEnumDefs[child->string] = std::move(mdefs);
                                }
                        }
                        cJSON *overridesObj = cJSON_GetObjectItem(vObj, "memberOverrides");
                        if (cJSON_IsObject(overridesObj)) {
                                for (cJSON *child = overridesObj->child; child; child = child->next) {
                                        if (!cJSON_IsObject(child) || !child->string)
                                                continue;
                                        VarEntry::MemberOverride ov;
                                        if (auto *alias = cJSON_GetObjectItem(child, "alias"); cJSON_IsString(alias))
                                                ov.alias = alias->valuestring;
                                        if (auto *address = cJSON_GetObjectItem(child, "address"); cJSON_IsNumber(address)) {
                                                ov.hasAddress = true;
                                                ov.address    = (u64)address->valuedouble;
                                        }
                                        if (auto *type = cJSON_GetObjectItem(child, "type"); cJSON_IsNumber(type)) {
                                                ov.hasType = true;
                                                ov.type    = (DataType)type->valueint;
                                        }
                                        if (auto *writable = cJSON_GetObjectItem(child, "writable"); cJSON_IsBool(writable)) {
                                                ov.hasWritable = true;
                                                ov.writable    = cJSON_IsTrue(writable);
                                        }
                                        v.memberOverrides[child->string] = std::move(ov);
                                }
                        }
                        vars_.push_back(v);
                }
        }
        if (invalidEntries > 0) {
                LOG_W("Variable[%s]: skipped %zu invalid session entries", name_.c_str(), invalidEntries);
                ImGui::InsertNotification({ImGuiToastType::Warning,
                                           7000,
                                           tr("Skipped %zu variables with invalid names, types or addresses.",
                                              "已跳过 %zu 个名称、类型或地址无效的变量。"),
                                           invalidEntries});
        }
        isModified_ = false;
}

bool
Variable::findElfSymbolAddress(const std::string &name, u32 &address) const
{
        std::lock_guard lk(mtxElf_);
        for (const auto &symbol : elfInfo_.symbols) {
                if (symbol.name == name && symbol.addr <= 0xffffffffull) {
                        address = static_cast<u32>(symbol.addr);
                        return true;
                }
        }
        // DWARF-flattened members allow protocol fields to be located by name
        // when debug information is present; the top-level symbols above remain
        // sufficient for release ELFs that only retain the symbol table
        for (const auto &entry : searchPool_) {
                if (entry.path == name && entry.addr <= 0xffffffffull) {
                        address = static_cast<u32>(entry.addr);
                        return true;
                }
        }
        return false;
}

void
Variable::addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port, u32 bitOffset, u32 bitSize)
{
        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t)
                return;
        const bool aggregateType =
            t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY;
        const char    *scalarType = scalarPayloadType(dwarfInfo_, typeOff);
        const DataType type =
            aggregateType ? DataType::U32 : (scalarType ? Parser::strToDataType(scalarType) : DataType::UNKNOWN);
        if (type == DataType::UNKNOWN || watchHasIdentity(fullPath, addr, type))
                return;

        if (aggregateType) {
                VarEntry v;
                v.name      = fullPath;
                v.type      = DataType::U32;
                v.port      = port;
                v.addr      = addr;
                v.writable  = true;
                v.typeOff   = typeOff;
                v.bitOffset = bitOffset;
                v.bitSize   = bitSize;
                v.isStruct  = t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                             t->kind == dwarf::TypeKind::ARRAY;
                v.isUnion = t->kind == dwarf::TypeKind::UNION;
                v.isArray = t->kind == dwarf::TypeKind::ARRAY;
                vars_.push_back(v);
                isModified_ = true;
        } else {
                if (scalarType) {
                        VarEntry v;
                        v.name      = fullPath;
                        v.type      = type;
                        v.port      = port;
                        v.addr      = addr;
                        v.writable  = true;
                        v.typeOff   = typeOff;
                        v.bitOffset = bitOffset;
                        v.bitSize   = bitSize;
                        vars_.push_back(v);
                        isModified_ = true;
                }
        }
}

bool
Variable::addSymbolToWatch(const std::string &fullPath, u64 address, u64 typeOffset, PortType port, u32 bitOffset, u32 bitSize)
{
        if (fullPath.empty())
                return false;
        const dwarf::Type *type      = resolveAlias(dwarfInfo_, typeOffset);
        const bool     aggregateType = type && (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION ||
                                            type->kind == dwarf::TypeKind::ARRAY);
        const char    *scalarType    = scalarPayloadType(dwarfInfo_, typeOffset);
        const DataType identityType =
            aggregateType ? DataType::U32 : (scalarType ? Parser::strToDataType(scalarType) : DataType::UNKNOWN);
        if (identityType == DataType::UNKNOWN) {
                ImGui::InsertNotification({ImGuiToastType::Warning,
                                           2200,
                                           tr("Cannot add %s: unsupported symbol type", "无法添加 %s：不支持的符号类型"),
                                           fullPath.c_str()});
                return false;
        }
        if (watchHasIdentity(fullPath, address, identityType)) {
                ImGui::InsertNotification({ImGuiToastType::Info,
                                           1800,
                                           tr("%s is already in the variable list", "%s 已在变量列表中"),
                                           fullPath.c_str()});
                return false;
        }
        const size_t previous = vars_.size();
        addRecursive(fullPath, address, typeOffset, port, bitOffset, bitSize);
        if (vars_.size() == previous) {
                ImGui::InsertNotification({ImGuiToastType::Warning,
                                           2200,
                                           tr("Cannot add %s: unsupported symbol type", "无法添加 %s：不支持的符号类型"),
                                           fullPath.c_str()});
                return false;
        }
        isModified_ = true;
        ImGui::InsertNotification(
            {ImGuiToastType::Success, 2000, tr("Added %s to variable list", "已将 %s 加入变量列表"), fullPath.c_str()});
        return true;
}

void
Variable::drawVarVarTreeRow(const std::string &fullPath,
                            u64                addr,
                            u64                typeOff,
                            i32                depth,
                            PortType           port,
                            const std::string &shmRegionName,
                            i32                parentVarIdx,
                            i32                colorDepth)
{
        if (depth > 16)
                return;
        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t)
                return;
        struct ScopedIndent {
                ScopedIndent(int) { ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f); }
                ~ScopedIndent() { ImGui::PopStyleVar(); }
        } indent(depth);

        const char *devLabel = (port == PortType::SHM)     ? "SHM"
                               : (port == PortType::LOCAL) ? "LOCAL"
                               : (port == PortType::UDP)   ? "UDP"
                                                           : "JLINK";

        auto drawValueCell =
            [&](u64 memberAddr, const char *sType, u64 memberTypeOff, const std::string &mPath, u32 bitOffset, u32 bitSize) {
                    const int radix =
                        parentVarIdx >= 0 && parentVarIdx < (int)vars_.size() ? vars_[parentVarIdx].radixForPath(mPath) : 10;
                    auto               it             = memberValueCache_.find(mPath);
                    const dwarf::Type *et             = resolveAlias(dwarfInfo_, memberTypeOff);
                    const bool         isEnum         = et && et->kind == dwarf::TypeKind::ENUM;
                    auto               decodeWithEnum = [&](const u8 *buf, u32 sz) -> std::string {
                            if (const auto number = unitConversionNumber(buf, Parser::strToDataType(sType), bitOffset, bitSize))
                                    memberNumericValueCache_[mPath] = *number;
                            if (!isEnum || radix != 10)
                                    return decodeValue(buf, Parser::strToDataType(sType), bitOffset, bitSize, radix);
                            // User-defined overrides for this member take priority
                            if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                    const auto &medMap = vars_[parentVarIdx].memberEnumDefs;
                                    auto        medIt  = medMap.find(mPath);
                                    if (medIt != medMap.end() && !medIt->second.empty()) {
                                            i64 ival = 0;
                                            std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                                            for (const auto &e : medIt->second)
                                                    if (e.val == ival)
                                                            return e.name;
                                    }
                            }
                            i64 ival = 0;
                            std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                            for (const auto &e : et->enums)
                                    if (e.val == ival)
                                            return e.name;
                            return decodeValue(buf, Parser::strToDataType(sType), bitOffset, bitSize, radix);
                    };
                    if (port != PortType::JLINK && parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                            u8         bytes[8]{};
                            const auto type = Parser::strToDataType(sType);
                            if (readWatchBytes(vars_[parentVarIdx], memberAddr, type, bytes)) {
                                    auto value               = decodeWithEnum(bytes, Parser::typeBytes(type));
                                    memberValueCache_[mPath] = value;
                                    return value;
                            }
                    }
                    if (port == PortType::JLINK) {
                            u32 sz                      = Parser::typeBytes(Parser::strToDataType(sType));
                            memberPollReqs_[memberAddr] = sz;

                            auto itSync = syncReadCache_.find(memberAddr);
                            if (itSync != syncReadCache_.end()) {
                                    if (itSync->second.ok) {
                                            std::string val          = decodeWithEnum(itSync->second.buf, sz);
                                            memberValueCache_[mPath] = val;
                                            return val;
                                    }
                                    if (!JLinkPort::instance().isConnected() && it != memberValueCache_.end())
                                            return it->second;
                                    return std::string("ERR");
                            }
                            if (it != memberValueCache_.end()) {
                                    return it->second;
                            } else {
                                    return std::string("...");
                            }
                    } else {
                            // SHM/UDP: cache is populated by updateVariables
                            if (it != memberValueCache_.end())
                                    return it->second;
                            return std::string("...");
                    }
            };

        // Character arrays retain their expandable bytes, but show text on the parent row
        auto drawStringValue = [&](u64 base, size_t capacity, const std::string &path) {
                std::string val;
                bool        complete = true;
                for (size_t i = 0; i < capacity; ++i) {
                        unsigned char byte = 0;
                        if (port == PortType::JLINK) {
                                memberPollReqs_[base + i] = std::max<u32>(memberPollReqs_[base + i], 1);
                                const auto it             = syncReadCache_.find(base + i);
                                if (it == syncReadCache_.end() || !it->second.ok) {
                                        complete = false;
                                        continue;
                                }
                                byte = it->second.buf[0];
                        } else if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                const auto &owner  = vars_[parentVarIdx];
                                const auto  offset = base + i - owner.addr;
                                if (port == PortType::LOCAL) {
                                        std::lock_guard<std::mutex> lock(mtxLocal_);
                                        const auto                  it = localBufs_.find(owner.name);
                                        if (it == localBufs_.end() || offset >= it->second.size()) {
                                                complete = false;
                                                break;
                                        }
                                        byte = it->second[offset];
                                } else if (port == PortType::SHM) {
                                        const auto it = shmShadowBufs_.find(shmShadowKey(owner));
                                        if (it == shmShadowBufs_.end() || offset >= it->second.size()) {
                                                complete = false;
                                                break;
                                        }
                                        byte = it->second[offset];
                                } else {
                                        complete = false;
                                        break;
                                }
                        } else {
                                complete = false;
                                break;
                        }
                        if (!byte)
                                break;
                        val.push_back(byte < 32 || byte == 127 ? '?' : static_cast<char>(byte));
                }
                if (complete)
                        memberValueCache_[path] = val;
                const auto cached = memberValueCache_.find(path);
                return cached == memberValueCache_.end() ? std::string("...") : cached->second;
        };

        const std::set<std::string> *hidden =
            (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) ? &vars_[parentVarIdx].hiddenMembers : nullptr;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        std::string memberPath = fullPath + "." + (m.name.empty() ? "<anon>" : m.name);
                        if (hidden && hidden->count(memberPath))
                                continue;
                        const u64   defaultMemberAddr = addr + m.offset;
                        const char *baseSType         = scalarPayloadType(dwarfInfo_, m.type);
                        bool        parentWritable =
                            parentVarIdx >= 0 && parentVarIdx < (int)vars_.size() ? vars_[parentVarIdx].writable : true;
                        const VarEntry::MemberOverride *memberOverride = nullptr;
                        if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                auto oit = vars_[parentVarIdx].memberOverrides.find(memberPath);
                                if (oit != vars_[parentVarIdx].memberOverrides.end())
                                        memberOverride = &oit->second;
                        }
                        u64 memberAddr =
                            memberOverride && memberOverride->hasAddress ? memberOverride->address : defaultMemberAddr;
                        DataType effectiveType = baseSType ? Parser::strToDataType(baseSType) : DataType::UNKNOWN;
                        if (memberOverride && memberOverride->hasType)
                                effectiveType = memberOverride->type;
                        std::string effectiveTypeName =
                            effectiveType != DataType::UNKNOWN ? Parser::dataTypeToStr(effectiveType) : "";
                        const char        *sType     = effectiveType != DataType::UNKNOWN ? effectiveTypeName.c_str() : nullptr;
                        const bool         isComplex = !sType;
                        const dwarf::Type *memberType    = resolveAlias(dwarfInfo_, m.type);
                        const dwarf::Type *characterType = memberType && memberType->kind == dwarf::TypeKind::ARRAY
                                                               ? resolveAlias(dwarfInfo_, memberType->inner)
                                                               : nullptr;
                        const bool isString = characterType && characterType->name == "char" && memberType->dims.size() == 1 &&
                                              memberType->dims.front() > 0 && memberType->dims.front() <= 256;
                        const char *complexTypeName =
                            memberType && memberType->kind == dwarf::TypeKind::UNION
                                ? "union"
                                : (memberType && memberType->kind == dwarf::TypeKind::ARRAY ? "array" : "struct");
                        const bool memberWritable =
                            memberOverride && memberOverride->hasWritable ? memberOverride->writable : parentWritable;
                        const std::string displayName = memberOverride && !memberOverride->alias.empty()
                                                            ? memberOverride->alias
                                                            : (m.name.empty() ? "<anon>" : m.name);

                        ui::HierarchyNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        std::set<std::string> *expSetM  = (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                              ? &vars_[parentVarIdx].expandedMembers
                                                              : nullptr;
                        const bool             wasOpenM = isComplex && expSetM && expSetM->count(memberPath) > 0;
                        if (isComplex && expSetM)
                                ImGui::SetNextItemOpen(wasOpenM);
                        // Suppress the hover/active highlight on struct sub-items
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        ui::HierarchyName();
                        const std::string memberLabel  = displayName + "###member_" + memberPath;
                        const float       memberLabelX = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
                        bool              open         = ui::HierarchyNode(memberLabel.c_str(), nodeFlags);
                        ImGui::PopStyleColor(2);
                        ui::HierarchyRow(colorDepth + (isComplex && open ? 1 : 0));
                        markWatchSearchRow(parentVarIdx, memberPath);
                        ui::HierarchyCard memberCard;
                        if (isComplex && open)
                                memberCard.begin(colorDepth + 1);
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memOvr =
                                    (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                        ? &vars_[parentVarIdx].memberEnumDefs
                                        : nullptr;
                                if (!sType) {
                                        auto sp      = std::make_unique<StructChannelPayload>();
                                        sp->writable = memberWritable;
                                        snprintf(sp->device, sizeof(sp->device), "%s", devLabel);
                                        if (!shmRegionName.empty())
                                                snprintf(sp->shmName, sizeof(sp->shmName), "%s", shmRegionName.c_str());
                                        flattenForStructPayload(dwarfInfo_,
                                                                memberPath,
                                                                memberAddr,
                                                                m.type,
                                                                *sp,
                                                                memOvr,
                                                                8,
                                                                parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()
                                                                    ? &vars_[parentVarIdx].memberUnitConversions
                                                                    : nullptr);
                                        ImGui::SetDragDropPayload("STRUCT_CHANNEL", sp.get(), sizeof(*sp));
                                } else {
                                        ChannelDropPayload p{};
                                        p.writable = memberWritable;
                                        snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                                        p.addr = memberAddr;
                                        snprintf(p.type, sizeof(p.type), "%s", sType);
                                        snprintf(p.device, sizeof(p.device), "%s", devLabel);
                                        if (!shmRegionName.empty())
                                                snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                                        p.numBytes  = (u8)Parser::typeBytes(effectiveType);
                                        p.typeOff   = m.type;
                                        p.bitOffset = m.bitOffset;
                                        p.bitSize   = m.bitSize;
                                        if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                const auto conversion =
                                                    vars_[parentVarIdx].memberUnitConversions.find(memberPath);
                                                if (conversion != vars_[parentVarIdx].memberUnitConversions.end())
                                                        p.unitConversion = conversion->second;
                                        }
                                        const std::vector<VarEntry::EnumDef> *leafOvr = nullptr;
                                        if (memOvr) {
                                                auto it = memOvr->find(memberPath);
                                                if (it != memOvr->end() && !it->second.empty())
                                                        leafOvr = &it->second;
                                        }
                                        fillEnumPayload(dwarfInfo_, m.type, p, leafOvr);
                                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                }
                                ImGui::Text(tr("Dragging %s", "拖拽 %s"), memberPath.c_str());
                                ImGui::EndDragDropSource();
                        }

                        if (isComplex && expSetM && open != wasOpenM) {
                                if (open)
                                        expSetM->insert(memberPath);
                                else
                                        expSetM->erase(memberPath);
                                isModified_ = true;
                        }

                        {
                                const dwarf::Type *mt            = resolveAlias(dwarfInfo_, m.type);
                                const bool         hasCustomEnum = parentVarIdx >= 0 && parentVarIdx < (int)vars_.size() &&
                                                           vars_[parentVarIdx].memberEnumDefs.contains(memberPath) &&
                                                           !vars_[parentVarIdx].memberEnumDefs.at(memberPath).empty();
                                const bool isMemberEnum = hasCustomEnum || (mt && mt->kind == dwarf::TypeKind::ENUM &&
                                                                            !(memberOverride && memberOverride->hasType));
                                if (isComplex && isWatchAggregate(mt) && ImGui::IsItemHovered() &&
                                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() &&
                                    ImGui::GetIO().MousePos.x >= memberLabelX &&
                                    ImGui::GetIO().MousePos.x <
                                        memberLabelX + ImGui::CalcTextSize(displayName.c_str(), nullptr, true).x)
                                        focusWatch(parentVarIdx, memberPath);
                                if (ui::HierarchyContextItem()) {
                                        drawWatchValueActions(parentVarIdx, memberPath);
                                        if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                        vars_[parentVarIdx].hiddenMembers.insert(memberPath);
                                                        isModified_ = true;
                                                }
                                        }
                                        if (ImGui::MenuItem(tr("Edit Properties...", "编辑属性...")))
                                                beginMemberProperties(parentVarIdx,
                                                                      memberPath,
                                                                      defaultMemberAddr,
                                                                      baseSType ? Parser::strToDataType(baseSType)
                                                                                : DataType::UNKNOWN,
                                                                      baseSType != nullptr,
                                                                      parentWritable,
                                                                      mt ? mt->kind : dwarf::TypeKind::STRUCT);
                                        if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                                openRewriteMember(parentVarIdx,
                                                                  memberAddr,
                                                                  m.type,
                                                                  memberPath,
                                                                  memberOverride && memberOverride->hasType ? effectiveType
                                                                                                            : DataType::UNKNOWN,
                                                                  m.bitOffset,
                                                                  m.bitSize);
                                        ImGui::Separator();
                                        ImGui::TextDisabled("%s", tr("Access mode", "读写模式"));
                                        auto setMemberWritable = [&](bool writable) {
                                                if (parentVarIdx < 0 || parentVarIdx >= (int)vars_.size())
                                                        return;
                                                auto &memberOv       = vars_[parentVarIdx].memberOverrides[memberPath];
                                                memberOv.hasWritable = true;
                                                memberOv.writable    = writable;
                                                isModified_          = true;
                                                propertiesChanged_   = true;
                                        };
                                        bool accessWritable = memberWritable;
                                        if (ui::AccessChoice("##access", accessWritable, port != PortType::AUDIO))
                                                setMemberWritable(accessWritable);
                                        const u32 traceSize =
                                            baseSType ? Parser::typeBytes(Parser::strToDataType(baseSType)) : 0;
                                        ImGui::BeginDisabled(port != PortType::JLINK || traceSize == 0);
                                        if (ImGui::MenuItem(tr("Trace writes with DWT", "使用 DWT 跟踪写入"))) {
                                                JLinkPort::instance().configureDwtWriteTrace(
                                                    static_cast<u32>(memberAddr), traceSize, memberPath);
                                        }
                                        ImGui::EndDisabled();
                                        ImGui::EndPopup();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip(
                                            "%s\n0x%08llX%s",
                                            memberPath.c_str(),
                                            (unsigned long long)memberAddr,
                                            isComplex && isWatchAggregate(mt)
                                                ? tr("\nDouble-click name to focus; click arrow to expand/collapse",
                                                     "\n双击名称聚焦；单击箭头展开/折叠")
                                                : "");
                                // The name above is the monitor drag source.
                                ImGui::TableSetColumnIndex(1);
                                if (mt && mt->kind == dwarf::TypeKind::POINTER) {
                                        drawPointerButton(parentVarIdx, memberAddr, m.type, memberPath);
                                } else if (sType) {
                                        std::string valStr =
                                            drawValueCell(memberAddr, sType, m.type, memberPath, m.bitOffset, m.bitSize);
                                        valueFlash_.cell(memberPath, valStr);
                                        ui::TableText(valStr);
                                } else if (isString) {
                                        const auto text = drawStringValue(memberAddr, memberType->dims.front(), memberPath);
                                        ui::TableText(text);
                                } else {
                                        ImGui::PushID(memberPath.c_str());
                                        drawWatchFocusButton(parentVarIdx, memberPath);
                                        ImGui::PopID();
                                }
                                ImGui::TableSetColumnIndex(2);
                                if (isString)
                                        ImGui::TextUnformatted("str");
                                else if (isMemberEnum)
                                        ui::TableText(std::string("enum (") + (baseSType ? baseSType : "i32") + ")");
                                else if (mt && mt->kind == dwarf::TypeKind::POINTER)
                                        ui::TableText(prettyType(dwarfInfo_, m.type));
                                else
                                        ui::TableText(sType ? sType : complexTypeName);
                                ImGui::TableSetColumnIndex(3);
                                watchAddress(memberAddr);
                                ImGui::TableSetColumnIndex(4);
                                ui::TableText(devLabel);

                                ImGui::TableSetColumnIndex(5);
                                if (memberWritable)
                                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                                else
                                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");
                        }

                        if (open && isComplex) {
                                drawVarVarTreeRow(memberPath,
                                                  memberAddr,
                                                  m.type,
                                                  depth + 1,
                                                  port,
                                                  shmRegionName,
                                                  parentVarIdx,
                                                  colorDepth + 1);
                                ImGui::TreePop();
                        }
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                u64 displayed = (dim == 0) ? 0 : (dim < (u64)elfArrayMaxElems_ ? dim : (u64)elfArrayMaxElems_);
                for (u64 i = 0; i < displayed; ++i) {
                        std::string idxStr     = "[" + std::to_string(i) + "]";
                        std::string memberPath = fullPath + idxStr;
                        if (hidden && hidden->count(memberPath))
                                continue;
                        const u64   defaultMemberAddr = addr + i * elemSize;
                        const char *baseSType         = scalarPayloadType(dwarfInfo_, t->inner);
                        bool        parentWritable =
                            parentVarIdx >= 0 && parentVarIdx < (int)vars_.size() ? vars_[parentVarIdx].writable : true;
                        const VarEntry::MemberOverride *memberOverride = nullptr;
                        if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                auto oit = vars_[parentVarIdx].memberOverrides.find(memberPath);
                                if (oit != vars_[parentVarIdx].memberOverrides.end())
                                        memberOverride = &oit->second;
                        }
                        u64 memberAddr =
                            memberOverride && memberOverride->hasAddress ? memberOverride->address : defaultMemberAddr;
                        DataType effectiveType = baseSType ? Parser::strToDataType(baseSType) : DataType::UNKNOWN;
                        if (memberOverride && memberOverride->hasType)
                                effectiveType = memberOverride->type;
                        std::string effectiveTypeName =
                            effectiveType != DataType::UNKNOWN ? Parser::dataTypeToStr(effectiveType) : "";
                        const char *sType     = effectiveType != DataType::UNKNOWN ? effectiveTypeName.c_str() : nullptr;
                        const bool  isComplex = !sType;
                        const bool  memberWritable =
                            memberOverride && memberOverride->hasWritable ? memberOverride->writable : parentWritable;
                        const std::string displayName =
                            memberOverride && !memberOverride->alias.empty() ? memberOverride->alias : idxStr;

                        ui::HierarchyNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        std::set<std::string> *expSetA  = (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                              ? &vars_[parentVarIdx].expandedMembers
                                                              : nullptr;
                        const bool             wasOpenA = isComplex && expSetA && expSetA->count(memberPath) > 0;
                        if (isComplex && expSetA)
                                ImGui::SetNextItemOpen(wasOpenA);
                        // Suppress the hover/active highlight on struct/array sub-items
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        ui::HierarchyName();
                        const std::string memberLabel  = displayName + "###member_" + memberPath;
                        const float       memberLabelX = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
                        bool              open         = ui::HierarchyNode(memberLabel.c_str(), nodeFlags);
                        ImGui::PopStyleColor(2);
                        ui::HierarchyRow(colorDepth + (isComplex && open ? 1 : 0));
                        markWatchSearchRow(parentVarIdx, memberPath);
                        ui::HierarchyCard memberCard;
                        if (isComplex && open)
                                memberCard.begin(colorDepth + 1);
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memOvr =
                                    (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                        ? &vars_[parentVarIdx].memberEnumDefs
                                        : nullptr;
                                if (!sType) {
                                        auto sp      = std::make_unique<StructChannelPayload>();
                                        sp->writable = memberWritable;
                                        snprintf(sp->device, sizeof(sp->device), "%s", devLabel);
                                        if (!shmRegionName.empty())
                                                snprintf(sp->shmName, sizeof(sp->shmName), "%s", shmRegionName.c_str());
                                        flattenForStructPayload(dwarfInfo_,
                                                                memberPath,
                                                                memberAddr,
                                                                t->inner,
                                                                *sp,
                                                                memOvr,
                                                                8,
                                                                parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()
                                                                    ? &vars_[parentVarIdx].memberUnitConversions
                                                                    : nullptr);
                                        ImGui::SetDragDropPayload("STRUCT_CHANNEL", sp.get(), sizeof(*sp));
                                } else {
                                        ChannelDropPayload p{};
                                        p.writable = memberWritable;
                                        snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                                        p.addr = memberAddr;
                                        snprintf(p.type, sizeof(p.type), "%s", sType);
                                        snprintf(p.device, sizeof(p.device), "%s", devLabel);
                                        if (!shmRegionName.empty())
                                                snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                                        p.numBytes = (u8)Parser::typeBytes(effectiveType);
                                        p.typeOff  = t->inner;
                                        if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                const auto conversion =
                                                    vars_[parentVarIdx].memberUnitConversions.find(memberPath);
                                                if (conversion != vars_[parentVarIdx].memberUnitConversions.end())
                                                        p.unitConversion = conversion->second;
                                        }
                                        const std::vector<VarEntry::EnumDef> *leafOvr = nullptr;
                                        if (memOvr) {
                                                auto it = memOvr->find(memberPath);
                                                if (it != memOvr->end() && !it->second.empty())
                                                        leafOvr = &it->second;
                                        }
                                        fillEnumPayload(dwarfInfo_, t->inner, p, leafOvr);
                                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                }
                                ImGui::Text(tr("Dragging %s", "拖拽 %s"), memberPath.c_str());
                                ImGui::EndDragDropSource();
                        }

                        if (isComplex && expSetA && open != wasOpenA) {
                                if (open)
                                        expSetA->insert(memberPath);
                                else
                                        expSetA->erase(memberPath);
                                isModified_ = true;
                        }

                        {
                                const dwarf::Type *et2           = resolveAlias(dwarfInfo_, t->inner);
                                const bool         hasCustomEnum = parentVarIdx >= 0 && parentVarIdx < (int)vars_.size() &&
                                                           vars_[parentVarIdx].memberEnumDefs.contains(memberPath) &&
                                                           !vars_[parentVarIdx].memberEnumDefs.at(memberPath).empty();
                                const bool isElemEnum = hasCustomEnum || (et2 && et2->kind == dwarf::TypeKind::ENUM &&
                                                                          !(memberOverride && memberOverride->hasType));
                                if (isComplex && isWatchAggregate(et2) && ImGui::IsItemHovered() &&
                                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() &&
                                    ImGui::GetIO().MousePos.x >= memberLabelX &&
                                    ImGui::GetIO().MousePos.x <
                                        memberLabelX + ImGui::CalcTextSize(displayName.c_str(), nullptr, true).x)
                                        focusWatch(parentVarIdx, memberPath);
                                if (ui::HierarchyContextItem()) {
                                        drawWatchValueActions(parentVarIdx, memberPath);
                                        if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                        vars_[parentVarIdx].hiddenMembers.insert(memberPath);
                                                        isModified_ = true;
                                                }
                                        }
                                        if (ImGui::MenuItem(tr("Edit Properties...", "编辑属性...")))
                                                beginMemberProperties(parentVarIdx,
                                                                      memberPath,
                                                                      defaultMemberAddr,
                                                                      baseSType ? Parser::strToDataType(baseSType)
                                                                                : DataType::UNKNOWN,
                                                                      baseSType != nullptr,
                                                                      parentWritable,
                                                                      et2 ? et2->kind : dwarf::TypeKind::STRUCT);
                                        if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                                openRewriteMember(parentVarIdx,
                                                                  memberAddr,
                                                                  t->inner,
                                                                  memberPath,
                                                                  memberOverride && memberOverride->hasType
                                                                      ? effectiveType
                                                                      : DataType::UNKNOWN);
                                        ImGui::Separator();
                                        ImGui::TextDisabled("%s", tr("Access mode", "读写模式"));
                                        auto setElementWritable = [&](bool writable) {
                                                if (parentVarIdx < 0 || parentVarIdx >= (int)vars_.size())
                                                        return;
                                                auto &memberOv       = vars_[parentVarIdx].memberOverrides[memberPath];
                                                memberOv.hasWritable = true;
                                                memberOv.writable    = writable;
                                                isModified_          = true;
                                                propertiesChanged_   = true;
                                        };
                                        bool accessWritable = memberWritable;
                                        if (ui::AccessChoice("##access", accessWritable, port != PortType::AUDIO))
                                                setElementWritable(accessWritable);
                                        const u32 traceSize =
                                            baseSType ? Parser::typeBytes(Parser::strToDataType(baseSType)) : 0;
                                        ImGui::BeginDisabled(port != PortType::JLINK || traceSize == 0);
                                        if (ImGui::MenuItem(tr("Trace writes with DWT", "使用 DWT 跟踪写入"))) {
                                                JLinkPort::instance().configureDwtWriteTrace(
                                                    static_cast<u32>(memberAddr), traceSize, memberPath);
                                        }
                                        ImGui::EndDisabled();
                                        ImGui::EndPopup();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip(
                                            "%s\n0x%08llX%s",
                                            memberPath.c_str(),
                                            (unsigned long long)memberAddr,
                                            isComplex && isWatchAggregate(et2)
                                                ? tr("\nDouble-click name to focus; click arrow to expand/collapse",
                                                     "\n双击名称聚焦；单击箭头展开/折叠")
                                                : "");
                                // The name above is the monitor drag source.
                                ImGui::TableSetColumnIndex(1);
                                if (et2 && et2->kind == dwarf::TypeKind::POINTER) {
                                        drawPointerButton(parentVarIdx, memberAddr, t->inner, memberPath);
                                } else if (sType) {
                                        std::string valStr = drawValueCell(memberAddr, sType, t->inner, memberPath, 0, 0);
                                        valueFlash_.cell(memberPath, valStr);
                                        ui::TableText(valStr);
                                } else {
                                        ImGui::PushID(memberPath.c_str());
                                        drawWatchFocusButton(parentVarIdx, memberPath);
                                        ImGui::PopID();
                                }
                                ImGui::TableSetColumnIndex(2);
                                if (isElemEnum)
                                        ui::TableText(std::string("enum (") + (baseSType ? baseSType : "i32") + ")");
                                else if (et2 && et2->kind == dwarf::TypeKind::POINTER)
                                        ui::TableText(prettyType(dwarfInfo_, t->inner));
                                else
                                        ImGui::TextUnformatted(
                                            sType ? sType
                                                  : (et2 && et2->kind == dwarf::TypeKind::UNION
                                                         ? "union"
                                                         : (et2 && et2->kind == dwarf::TypeKind::STRUCT ? "struct" : "array")));
                                ImGui::TableSetColumnIndex(3);
                                watchAddress(memberAddr);
                                ImGui::TableSetColumnIndex(4);
                                ui::TableText(devLabel);

                                ImGui::TableSetColumnIndex(5);
                                if (memberWritable)
                                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                                else
                                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");
                        }

                        if (open && isComplex) {
                                drawVarVarTreeRow(memberPath,
                                                  memberAddr,
                                                  t->inner,
                                                  depth + 1,
                                                  port,
                                                  shmRegionName,
                                                  parentVarIdx,
                                                  colorDepth + 1);
                                ImGui::TreePop();
                        }
                }
        }
}

void *
Variable::getLocalBuf(const std::string &name)
{
        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                       &buf = localBufs_[name];
        if (buf.empty())
                buf.resize(8, 0); // fallback for entries not yet pre-allocated
        return buf.data();
}

void
Variable::notifyLocalWrite(const std::string &name)
{
        std::vector<uint8_t> bytes;
        {
                std::lock_guard lock(mtxLocal_);
                const auto      it = localBufs_.find(name);
                if (!localSampleSources_.contains(name) || it == localBufs_.end())
                        return;
                bytes = it->second;
        }
        publishLocalSample(name, bytes, sessionTimeSec());
}

std::unordered_map<std::string, std::vector<uint8_t>>
Variable::snapshotLocalBuffers() const
{
        std::unordered_map<std::string, std::vector<uint8_t>> result;
        std::lock_guard                                       lock(mtxLocal_);
        for (const auto &variable : vars_)
                if (variable.port == PortType::LOCAL)
                        if (const auto it = localBufs_.find(variable.name); it != localBufs_.end())
                                result.emplace(it->first, it->second);
        return result;
}

bool
Variable::compareExchangeLocalBuffer(const std::string          &name,
                                     const std::vector<uint8_t> &expected,
                                     const std::vector<uint8_t> &desired)
{
        if (expected.size() != desired.size())
                return false;
        std::lock_guard lock(mtxLocal_);
        const auto      it = localBufs_.find(name);
        if (it == localBufs_.end() || it->second != expected)
                return false;
        it->second = desired;
        return true;
}

void
Variable::addLocalVar(const std::string &name, DataType type, size_t bufSize)
{
        if (watchHasName(name))
                return;
        VarEntry v;
        v.name     = name;
        v.type     = type;
        v.port     = PortType::LOCAL;
        v.addr     = 0;
        v.writable = true;
        {
                std::lock_guard<std::mutex> lk(mtxLocal_);
                localBufs_.emplace(name, std::vector<uint8_t>(bufSize > 0 ? bufSize : 8, 0));
        }
        vars_.push_back(v);
        isModified_        = true;
        propertiesChanged_ = true;
}

void
Variable::setLocalScalar(const std::string &name, double val)
{
        // Only LOCAL scalar entries are writable here; a user-configured JLINK/UDP/SHM
        // variable of the same name is intentionally left untouched
        DataType type = DataType::UNKNOWN;
        for (const auto &v : vars_)
                if (v.name == name && v.port == PortType::LOCAL && !v.isStruct) {
                        type = v.type;
                        break;
                }
        if (type == DataType::UNKNOWN)
                return;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(name);
        if (it == localBufs_.end())
                return;
        auto &buf = it->second;
        auto  put = [&](const void *src, size_t n) {
                if (buf.size() < n)
                        buf.resize(n, 0);
                std::memcpy(buf.data(), src, n);
        };
        switch (type) {
                case DataType::F16: {
                        uint16_t bits = fast_f32_to_f16((float)val);
                        put(&bits, 2);
                        break;
                }
                case DataType::F32: {
                        float f = (float)val;
                        put(&f, 4);
                        break;
                }
                case DataType::F64: {
                        double d = val;
                        put(&d, 8);
                        break;
                }
                case DataType::U8: {
                        uint8_t v = (uint8_t)(int64_t)val;
                        put(&v, 1);
                        break;
                }
                case DataType::I8: {
                        int8_t v = (int8_t)(int64_t)val;
                        put(&v, 1);
                        break;
                }
                case DataType::U16: {
                        uint16_t v = (uint16_t)(int64_t)val;
                        put(&v, 2);
                        break;
                }
                case DataType::I16: {
                        int16_t v = (int16_t)(int64_t)val;
                        put(&v, 2);
                        break;
                }
                case DataType::U32: {
                        uint32_t v = (uint32_t)(int64_t)val;
                        put(&v, 4);
                        break;
                }
                case DataType::I32: {
                        int32_t v = (int32_t)(int64_t)val;
                        put(&v, 4);
                        break;
                }
                case DataType::U64: {
                        uint64_t v = (uint64_t)(int64_t)val;
                        put(&v, 8);
                        break;
                }
                case DataType::I64: {
                        int64_t v = (int64_t)val;
                        put(&v, 8);
                        break;
                }
                default:
                        return;
        }
        isModified_ = true;
}

bool
Variable::writeLocalScalarFromMonitor(const std::string &name, double val)
{
        const VarEntry              *target = nullptr;
        const VarEntry::StructField *member = nullptr;
        for (const auto &v : vars_)
                if (v.name == name && v.port == PortType::LOCAL && !v.isStruct) {
                        target = &v;
                        break;
                }
        if (!target) {
                for (const auto &v : vars_) {
                        if (v.port != PortType::LOCAL || !v.isStruct || name.rfind(v.name + ".", 0) != 0)
                                continue;
                        const auto suffix = name.substr(v.name.size() + 1);
                        for (const auto &field : v.structFields)
                                if (suffix == field.name) {
                                        target = &v;
                                        member = &field;
                                        break;
                                }
                        if (target)
                                break;
                }
                if (!target)
                        return false;
        }

        char       text[64]{};
        const auto type = member ? member->type : target->type;
        switch (type) {
                case DataType::F16:
                case DataType::F32:
                case DataType::F64:
                        snprintf(text, sizeof(text), "%.17g", val);
                        break;
                case DataType::U8:
                case DataType::U16:
                case DataType::U32:
                case DataType::U64:
                        snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(val));
                        break;
                case DataType::I8:
                case DataType::I16:
                case DataType::I32:
                case DataType::I64:
                        snprintf(text, sizeof(text), "%lld", static_cast<long long>(val));
                        break;
                default:
                        return false;
        }
        if (member) {
                uint8_t    encoded[8]{};
                const auto size = Parser::typeBytes(type);
                if (!size || size > sizeof(encoded) || !encodeScalar(text, type, encoded))
                        return false;
                std::lock_guard lock(mtxLocal_);
                const auto      it = localBufs_.find(target->name);
                if (it == localBufs_.end() || member->byteOffset + size > it->second.size())
                        return false;
                memcpy(it->second.data() + member->byteOffset, encoded, size);
                isModified_ = true;
                return true;
        }
        writeVariable(*target, text);
        return true;
}

bool
Variable::openFunctionBindingFor(const std::string &name)
{
        for (int i = 0; i < static_cast<int>(vars_.size()); ++i) {
                auto &variable = vars_[i];
                if (variable.name != name || variable.port != PortType::LOCAL || variable.isStruct)
                        continue;
                const dwarf::Type *type = nullptr;
                {
                        std::lock_guard lk(mtxElf_);
                        type = resolveAlias(dwarfInfo_, variable.typeOff);
                }
                if (type && (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION ||
                             type->kind == dwarf::TypeKind::ARRAY))
                        return false;
                functionBindIdx_   = i;
                functionPanelOpen_ = true;
                open_              = true;
                return true;
        }
        return false;
}

void
Variable::addLocalStructVar(const std::string &name, const std::vector<VarEntry::StructField> &fields, size_t totalSize)
{
        // Compute buffer size from fields if totalSize was not supplied
        size_t bsz = totalSize;
        if (bsz == 0) {
                for (const auto &sf : fields)
                        bsz = std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
        }
        if (bsz == 0)
                bsz = 8;

        // A session saved before localBufSize existed reconstructs an SDK
        // structure only up to its prev visible field. Hidden reserved tails
        // therefore produce an undersized buffer (fsav3_rt: 380 vs 1112 bytes)
        // Upgrade an existing LOCAL aggregate before the SDK copies into it
        for (auto &existing : vars_) {
                if (existing.name != name)
                        continue;
                if (existing.port == PortType::LOCAL) {
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        auto                       &buffer = localBufs_[name];
                        if (buffer.size() < bsz)
                                buffer.resize(bsz, 0);
                        existing.type         = DataType::U8;
                        existing.writable     = true;
                        existing.isStruct     = true;
                        existing.structFields = fields;
                }
                return;
        }

        VarEntry v;
        v.name         = name;
        v.type         = DataType::U8; // per-byte access; display driven by structFields
        v.port         = PortType::LOCAL;
        v.addr         = 0;
        v.writable     = true;
        v.isStruct     = true;
        v.structFields = fields;
        {
                std::lock_guard<std::mutex> lk(mtxLocal_);
                localBufs_.emplace(name, std::vector<uint8_t>(bsz, 0));
        }
        vars_.push_back(v);
        isModified_        = true;
        propertiesChanged_ = true;
}

std::vector<Variable::PopupMember>
Variable::getPopupMembers(const std::string &varName, const std::string &pathPrefix, u64 typeOff, u64 baseAddr) const
{
        std::vector<PopupMember> result;
        const VarEntry          *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve)
                return result;
        if (typeOff == 0)
                typeOff = ve->typeOff;
        if (baseAddr == 0)
                baseAddr = ve->addr;

        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t || (t->kind != dwarf::TypeKind::STRUCT && t->kind != dwarf::TypeKind::UNION))
                return result;

        for (const auto &m : t->members) {
                const std::string  mPath = pathPrefix + "." + m.name;
                const dwarf::Type *mt    = resolveAlias(dwarfInfo_, m.type);
                PopupMember        pm;
                pm.name     = m.name;
                pm.typeOff  = m.type;
                pm.addr     = baseAddr + m.offset;
                pm.isStruct = mt && (mt->kind == dwarf::TypeKind::STRUCT || mt->kind == dwarf::TypeKind::UNION);
                if (!pm.isStruct) {
                        auto it   = memberValueCache_.find(mPath);
                        pm.valStr = (it != memberValueCache_.end()) ? it->second : "...";
                }
                result.push_back(std::move(pm));
        }
        return result;
}

bool
Variable::getDwarfMemberAsFloat(const std::string &memberPath, float &out) const
{
        const auto it = memberNumericValueCache_.find(memberPath);
        if (it == memberNumericValueCache_.end())
                return false;
        out = static_cast<float>(it->second);
        return true;
}

void
Variable::refreshDwarfMember(const std::string &memberPath)
{
        // memberPath e.g. "motorState.speed" or "motorState.control.reference"
        auto dot = memberPath.find('.');
        if (dot == std::string::npos)
                return;
        const std::string varName = memberPath.substr(0, dot);
        std::string       subPath = memberPath.substr(dot + 1); // "speed" or "control.reference"

        // Find parent VarEntry
        const VarEntry *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve || ve->port != PortType::JLINK)
                return;
        if (!JLinkPort::instance().isConnected())
                return;

        // Walk DWARF type tree along subPath to get final address + DataType
        u64 addr    = ve->addr;
        u64 typeOff = ve->typeOff;

        while (!subPath.empty()) {
                const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
                if (!t || (t->kind != dwarf::TypeKind::STRUCT && t->kind != dwarf::TypeKind::UNION))
                        return;

                std::string elem;
                auto        nd = subPath.find('.');
                if (nd == std::string::npos) {
                        elem = subPath;
                        subPath.clear();
                } else {
                        elem    = subPath.substr(0, nd);
                        subPath = subPath.substr(nd + 1);
                }
                bool found = false;
                for (const auto &m : t->members) {
                        if (m.name == elem) {
                                addr    += m.offset;
                                typeOff  = m.type;
                                found    = true;
                                break;
                        }
                }
                if (!found)
                        return;
        }

        // Resolve leaf type → type string via existing scalarPayloadType helper
        const char *sType = scalarPayloadType(dwarfInfo_, typeOff);
        if (!sType)
                return; // nested struct / pointer — not a numeric leaf
        DataType dt = Parser::strToDataType(sType);

        u32 sz = Parser::typeBytes(dt);
        if (sz == 0 || sz > 8)
                return;

        // Rate-limit to ~100 ms per address
        u64   now    = get_mono_ts_ms();
        auto &prevTs = memberRefreshTs_[addr];
        if (now - prevTs < 100)
                return;
        prevTs = now;

        u8   buf[8]{};
        bool ok = JLinkPort::instance().readMem((u32)addr, sz, buf);
        if (ok) {
                memberValueCache_[memberPath] = decodeValue(buf, dt, 0, 0, ve->radixForPath(memberPath));
                if (const auto number = unitConversionNumber(buf, dt))
                        memberNumericValueCache_[memberPath] = *number;
        }
}

std::string
Variable::resolveFunctionAddress(const u32 address) const
{
        std::lock_guard  lock(mtxElf_);
        const ElfSymbol *best = nullptr;
        for (const ElfSymbol &symbol : elfInfo_.symbols) {
                if (symbol.type != 2 || symbol.addr > address)
                        continue; // STT_FUNC
                if (symbol.size > 0 && static_cast<u64>(address) >= symbol.addr + symbol.size)
                        continue;
                if (!best || symbol.addr > best->addr)
                        best = &symbol;
        }
        if (!best)
                return {};
        char offset[32]{};
        if (static_cast<u64>(address) > best->addr)
                snprintf(
                    offset, sizeof(offset), "+0x%llX", static_cast<unsigned long long>(static_cast<u64>(address) - best->addr));
        return best->name + offset;
}

/**
 * @brief Decode `fsz` bytes at `tmp` as the given scalar type into a float
 *        Returns false for non-scalar/unsupported types
 */
static bool
decodeLocalScalar(const uint8_t *tmp, DataType type, float &out)
{
        switch (type) {
                case DataType::F16: {
                        uint16_t bits;
                        std::memcpy(&bits, tmp, 2);
                        out = fast_f16_to_f32(bits);
                        return true;
                }
                case DataType::F32: {
                        float v;
                        std::memcpy(&v, tmp, 4);
                        out = v;
                        return true;
                }
                case DataType::F64: {
                        double v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                case DataType::U8:
                        out = (float)tmp[0];
                        return true;
                case DataType::I8:
                        out = (float)(int8_t)tmp[0];
                        return true;
                case DataType::U16: {
                        uint16_t v;
                        std::memcpy(&v, tmp, 2);
                        out = (float)v;
                        return true;
                }
                case DataType::I16: {
                        int16_t v;
                        std::memcpy(&v, tmp, 2);
                        out = (float)v;
                        return true;
                }
                case DataType::U32: {
                        uint32_t v;
                        std::memcpy(&v, tmp, 4);
                        out = (float)v;
                        return true;
                }
                case DataType::I32: {
                        int32_t v;
                        std::memcpy(&v, tmp, 4);
                        out = (float)v;
                        return true;
                }
                case DataType::U64: {
                        uint64_t v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                case DataType::I64: {
                        int64_t v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                default:
                        return false;
        }
}

/**
 * @brief Snapshot all LOCAL variable values as (chName, value) pairs. Channel
 *        names match what the watch-list drag produces: "<var>" for a scalar LOCAL
 *        variable and "<var>.<field>" for each field of a manual struct. The GUI feeds
 *        these into matching monitor scope chs each frame
 */
std::vector<std::pair<std::string, float>>
Variable::collectLocalChannelValues() const
{
        std::vector<std::pair<std::string, float>> outVals;
        std::lock_guard<std::mutex>                lk(mtxLocal_);
        for (const auto &v : vars_) {
                if (v.port != PortType::LOCAL || localSampleSources_.contains(v.name))
                        continue;
                auto it = localBufs_.find(v.name);
                if (it == localBufs_.end() || it->second.empty())
                        continue;
                const auto &buf = it->second;

                if (!v.structFields.empty()) {
                        for (const auto &sf : v.structFields) {
                                u32 fsz = Parser::typeBytes(sf.type);
                                if (fsz == 0 || sf.byteOffset + fsz > (u32)buf.size())
                                        continue;
                                uint8_t tmp[8]{};
                                std::memcpy(tmp, buf.data() + sf.byteOffset, fsz);
                                float val;
                                if (decodeLocalScalar(tmp, sf.type, val))
                                        outVals.emplace_back(v.name + "." + sf.name, val);
                        }
                } else {
                        u32 sz = Parser::typeBytes(v.type);
                        if (sz == 0 || sz > buf.size())
                                continue;
                        uint8_t tmp[8]{};
                        std::memcpy(tmp, buf.data(), sz);
                        float val;
                        if (decodeLocalScalar(tmp, v.type, val))
                                outVals.emplace_back(v.name, val);
                }
        }
        return outVals;
}

void
Variable::markLocalSampleSource(const std::string &name)
{
        std::lock_guard lock(mtxLocal_);
        localSampleSources_.insert(name);
}

void
Variable::publishLocalSample(const std::string &name, const std::vector<uint8_t> &bytes, double time)
{
        std::lock_guard lock(mtxLocal_);
        if (!localSampleSources_.contains(name) || bytes.empty() || !std::isfinite(time) || time > sessionTimeSec())
                return;
        // Bound pending data if the UI is stopped in a dialog or a debugger.
        constexpr size_t maxBytes = 16 * 1024 * 1024;
        if (bytes.size() > maxBytes)
                return;
        while (!localSamples_.empty() && (localSamples_.size() >= 8192 || localSampleBytes_ + bytes.size() > maxBytes)) {
                localSampleBytes_ -= localSamples_.front().bytes.size();
                localSamples_.pop_front();
        }
        localSamples_.push_back({name, bytes, time, getSessionStartUs()});
        localSampleBytes_ += bytes.size();
}

std::unordered_map<std::string, Variable::LocalSampleBatch>
Variable::drainLocalSamples()
{
        std::deque<LocalSample> samples;
        {
                std::lock_guard lock(mtxLocal_);
                samples.swap(localSamples_);
                localSampleBytes_ = 0;
        }
        std::unordered_map<std::string, LocalSampleBatch> result;
        for (const auto &sample : samples) {
                if (sample.epoch != getSessionStartUs())
                        continue;
                const auto variable = std::find_if(vars_.begin(), vars_.end(), [&](const auto &v) {
                        return v.port == PortType::LOCAL && v.name == sample.name;
                });
                if (variable == vars_.end())
                        continue;
                auto append = [&](const std::string &name, DataType type, size_t offset) {
                        const auto size = Parser::typeBytes(type);
                        if (!size || size > 8 || offset + size > sample.bytes.size())
                                return;
                        uint8_t bytes[8]{};
                        std::memcpy(bytes, sample.bytes.data() + offset, size);
                        float value;
                        if (!decodeLocalScalar(bytes, type, value))
                                return;
                        auto &batch    = result[name];
                        batch.raw.size = size;
                        std::copy_n(bytes, size, batch.raw.bytes.begin());
                        // A published root and its separately published scalar can
                        // refer to the same sample; count it only once per channel.
                        if (!batch.times.empty() && batch.times.back() == sample.time) {
                                batch.values.back() = value;
                                return;
                        }
                        batch.values.push_back(value);
                        batch.times.push_back(sample.time);
                };
                if (variable->structFields.empty())
                        append(sample.name, variable->type, 0);
                else
                        for (const auto &field : variable->structFields)
                                append(sample.name + "." + field.name, field.type, field.byteOffset);
        }
        return result;
}

void
Variable::setLocalMonitorSampleRates(const std::unordered_map<std::string, u32> &rates)
{
        u32 fastestHz = 0;
        for (const auto &v : vars_) {
                if (v.port != PortType::LOCAL || v.isStruct || v.readFunction.empty())
                        continue;
                auto it = rates.find(v.name);
                if (it != rates.end())
                        fastestHz = std::max(fastestHz, it->second);
        }
        monitorFunctionHz_.store(fastestHz, std::memory_order_release);
}

bool
Variable::readLocalFieldAsFloat(const std::string &varName, const std::string &fieldName, float &out) const
{
        const VarEntry *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve || ve->structFields.empty())
                return false;

        const VarEntry::StructField *sf = nullptr;
        for (const auto &f : ve->structFields)
                if (fieldName == f.name) {
                        sf = &f;
                        break;
                }
        if (!sf)
                return false;

        u32 fsz = Parser::typeBytes(sf->type);
        if (fsz == 0)
                return false;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(varName);
        if (it == localBufs_.end())
                return false;
        const auto &buf = it->second;
        if (sf->byteOffset + fsz > (u32)buf.size())
                return false;

        uint8_t tmp[8]{};
        std::memcpy(tmp, buf.data() + sf->byteOffset, fsz);
        return decodeLocalScalar(tmp, sf->type, out);
}

bool
Variable::readLocalScalar(const std::string &name, double &out) const
{
        DataType type = DataType::UNKNOWN;
        for (const auto &v : vars_)
                if (v.name == name && v.port == PortType::LOCAL && !v.isStruct) {
                        type = v.type;
                        break;
                }
        if (type == DataType::UNKNOWN)
                return false;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(name);
        if (it == localBufs_.end() || it->second.empty())
                return false;
        u32 sz = Parser::typeBytes(type);
        if (sz == 0 || sz > it->second.size())
                return false;
        uint8_t tmp[8]{};
        std::memcpy(tmp, it->second.data(), sz);
        float f;
        if (!decodeLocalScalar(tmp, type, f))
                return false;
        out = f;
        return true;
}
