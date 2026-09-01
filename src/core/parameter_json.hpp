#pragma once
#include "cJSON.h"
#include "core/parameter_csv.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace parameter_json
{
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
struct Field {
        parameter_csv::Row                 row;
        std::map<std::string, std::string> enums;
        std::string                        role = "config", expression, runtimePath, autoWhenZero, display;
        unsigned                           id = 0, revision = 1;
        std::optional<std::string>         presetValue;
};
struct Document {
        std::string                        name, target, status = "ready";
        std::vector<Field>                 fields;
        std::map<std::string, std::string> components;
};
inline std::vector<std::pair<std::string, std::string>>
enumOptionsByValue(const Field &field)
{
        std::vector<std::pair<std::string, std::string>> options(field.enums.begin(), field.enums.end());
        // Compare decimal magnitudes directly to retain exact 64-bit ordering
        const auto magnitude = [](std::string_view val) {
                if (val.starts_with('-'))
                        val.remove_prefix(1);
                const auto first = val.find_first_not_of('0');
                return first == std::string_view::npos ? std::string_view("0") : val.substr(first);
        };
        std::sort(options.begin(), options.end(), [&](const auto &a, const auto &b) {
                const auto left = magnitude(a.second), right = magnitude(b.second);
                const bool leftNegative  = a.second.starts_with('-') && left != "0";
                const bool rightNegative = b.second.starts_with('-') && right != "0";
                if (leftNegative != rightNegative)
                        return leftNegative;
                if (left == right)
                        return a.first < b.first;
                const bool less = left.size() != right.size() ? left.size() < right.size() : left < right;
                return leftNegative ? !less : less;
        });
        return options;
}
inline constexpr std::array<const char *, 3> componentSections = {"mech", "hw", "sw"};
inline bool
isComponent(const std::string &section)
{
        return std::find(componentSections.begin(), componentSections.end(), section) != componentSections.end();
}
/**
 * @brief A root-level field sits directly under the selected variable and belongs to no component:
 *        component presets never supply it, so it is always carried over from the
 *        opened document instead of being swapped or dropped with a component
 */
inline bool
isRootField(const std::string &path)
{
        return path.find('.') == std::string::npos;
}
inline std::string
baseName(const std::string &name)
{
        const auto end = name.find_last_not_of('*');
        return end == std::string::npos ? std::string{} : name.substr(0, end + 1);
}
inline std::string
componentName(const std::map<std::string, std::string> &names)
{
        if (names.size() != componentSections.size())
                throw std::runtime_error("Expected mechanical, hardware and software component names");
        std::string result;
        for (const auto *section : componentSections) {
                const auto it = names.find(section);
                if (it == names.end() || it->second.empty() || it->second.find_first_of("/\0", 0, 2) != std::string::npos)
                        throw std::runtime_error(std::string(section) +
                                                 ": component name must be nonempty and contain no slash or NUL");
                if (!result.empty())
                        result += '/';
                result += it->second;
        }
        if (result.size() > 63)
                throw std::runtime_error("Combined configuration name exceeds 63 UTF-8 bytes");
        return result;
}
inline std::map<std::string, std::string>
componentNames(const Document &doc, const std::string &fallbackName = "")
{
        if (!doc.components.empty()) {
                if (componentName(doc.components) != doc.name)
                        throw std::runtime_error("Configuration name differs from components");
                return doc.components;
        }
        const auto &name = doc.name.empty() ? fallbackName : doc.name;
        if (name.empty())
                return {};
        std::map<std::string, std::string> result;
        const auto                         first = name.find('/');
        if (first == std::string::npos) {
                for (const auto *section : componentSections)
                        result[section] = name;
        } else {
                const auto second = name.find('/', first + 1);
                if (second == std::string::npos || name.find('/', second + 1) != std::string::npos)
                        throw std::runtime_error("Combined configuration name needs three slash-separated names");
                result["mech"] = name.substr(0, first);
                result["hw"]   = name.substr(first + 1, second - first - 1);
                result["sw"]   = name.substr(second + 1);
        }
        // An old single name may still be displayed even if repeating it three times
        // would exceed the MCU limit. Composition validates the final combination
        for (const auto &[section, val] : result)
                if (val.empty() || val.find('\0') != std::string::npos)
                        throw std::runtime_error(section + ": invalid component name");
        return result;
}
inline bool
completeComponents(const Document &doc)
{
        return !doc.target.empty() && std::all_of(componentSections.begin(), componentSections.end(), [&](const auto *section) {
                const std::string prefix = std::string(section) + '.';
                return std::any_of(doc.fields.begin(), doc.fields.end(), [&](const auto &field) {
                        return field.role == "config" && field.row.path.starts_with(prefix);
                });
        });
}
inline bool
sameParameterValue(const Field &field, const std::string &a, const std::string &b)
{
        const auto numeric = [&](const std::string &val) {
                const auto found = field.enums.find(val);
                return found == field.enums.end() ? val : found->second;
        };
        const auto left = numeric(a), right = numeric(b);
        const auto integer = []<class T>(const std::string &text, T &val) {
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), val, 10);
                return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
        };
        const auto &type = field.row.type;
        if (type == "bool") {
                const auto boolean = [](const std::string &val) {
                        return val == "true" || val == "1" ? 1 : val == "false" || val == "0" ? 0 : -1;
                };
                const auto val = boolean(left);
                return val >= 0 && val == boolean(right);
        }
        const auto equalIntegers = [&]<class T>() {
                T x{}, y{};
                return integer(left, x) && integer(right, y) && x == y;
        };
        if (type == "u8" || type == "uint8_t")
                return equalIntegers.template operator()<uint8_t>();
        if (type == "u16" || type == "uint16_t")
                return equalIntegers.template operator()<uint16_t>();
        if (type == "u32" || type == "uint32_t")
                return equalIntegers.template operator()<uint32_t>();
        if (type == "u64" || type == "uint64_t")
                return equalIntegers.template operator()<uint64_t>();
        if (type == "i8" || type == "int8_t")
                return equalIntegers.template operator()<int8_t>();
        if (type == "i16" || type == "int16_t")
                return equalIntegers.template operator()<int16_t>();
        if (type == "i32" || type == "int32_t")
                return equalIntegers.template operator()<int32_t>();
        if (type == "i64" || type == "int64_t" || type == "enum")
                return equalIntegers.template operator()<int64_t>();
        if (type == "f32" || type == "f16" || type == "float" || type == "float32" || type == "f64" || type == "double" ||
            type == "float64") {
                try {
                        const auto x = parameter_csv::number(left), y = parameter_csv::number(right);
                        if (type == "f32" || type == "f16" || type == "float" || type == "float32") {
                                const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
                                return std::isfinite(fx) && std::isfinite(fy) && fx == fy;
                        }
                        const auto dx = static_cast<double>(x), dy = static_cast<double>(y);
                        return std::isfinite(dx) && std::isfinite(dy) && dx == dy;
                } catch (const std::exception &) {
                        return false;
                }
        }
        return left == right;
}
// Ordered by priority so a group can inherit its most important difference
enum class ReadDifference { None, Default, Write };
inline ReadDifference
readDifference(const Field &field, const std::string *actual)
{
        if (!actual || field.role == "derived")
                return ReadDifference::None;
        if (!sameParameterValue(field, field.row.val, *actual))
                return ReadDifference::Write;
        if (field.presetValue && !sameParameterValue(field, *field.presetValue, *actual))
                return ReadDifference::Default;
        return ReadDifference::None;
}
inline void
capturePresetValues(Document &doc)
{
        const auto names = completeComponents(doc) ? componentNames(doc) : std::map<std::string, std::string>{};
        for (auto &field : doc.fields) {
                if (field.role != "config" || field.presetValue)
                        continue;
                // No component name can mark a root-level field, so its value is
                // always this document's own baseline
                if (isRootField(field.row.path)) {
                        field.presetValue = field.row.val;
                        continue;
                }
                const auto section = field.row.path.substr(0, field.row.path.find('.'));
                // A marked file without its original reference is not a new default
                if (const auto name = names.find(section); name != names.end() && name->second.ends_with('*'))
                        continue;
                field.presetValue = field.row.val;
        }
}
inline void
refreshComponentNames(Document &doc)
{
        if (!completeComponents(doc))
                return;
        auto names = componentNames(doc);
        if (names.empty())
                return;
        bool anyModified = false;
        for (const auto *section : componentSections) {
                bool              changed = false, unknown = false;
                const std::string prefix = std::string(section) + '.';
                for (const auto &field : doc.fields)
                        if (field.role == "config" && field.row.path.starts_with(prefix)) {
                                if (!field.presetValue)
                                        unknown = true;
                                else if (!sameParameterValue(field, field.row.val, *field.presetValue))
                                        changed = true;
                        }
                changed   |= unknown && names.at(section).ends_with('*');
                auto name  = baseName(names.at(section));
                if (name.empty())
                        throw std::runtime_error(std::string(section) + ": empty preset name");
                names[section]  = name + (changed ? "*" : "");
                anyModified    |= changed;
        }
        // Keep legacy whole-profile names until they first become a composition
        if (!anyModified && doc.components.empty() && doc.name.find('/') == std::string::npos)
                return;
        const auto name = componentName(names); // validate before modifying either field
        doc.components  = std::move(names);
        doc.name        = name;
}
inline std::string
scalar(const cJSON *x)
{
        if (cJSON_IsString(x))
                return x->valuestring;
        if (cJSON_IsBool(x))
                return cJSON_IsTrue(x) ? "1" : "0";
        if (cJSON_IsNumber(x)) {
                if (!std::isfinite(x->valuedouble) || std::abs(x->valuedouble) > 9007199254740991.0)
                        throw std::runtime_error("Use a decimal string for integers beyond 2^53");
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%.17g", x->valuedouble);
                return buffer;
        }
        throw std::runtime_error("Expected string, boolean or number");
}
inline const cJSON *
get(const cJSON *x, const char *key)
{
        return cJSON_GetObjectItemCaseSensitive(x, key);
}
inline std::string
string(const cJSON *x, const char *key, bool required = false)
{
        const auto *v = get(x, key);
        if (!v && !required)
                return {};
        if (!cJSON_IsString(v))
                throw std::runtime_error(std::string("Expected string: ") + key);
        return v->valuestring;
}
inline void
uniqueKeys(const cJSON *node, int depth = 0)
{
        if (depth > 32)
                throw std::runtime_error("JSON nesting exceeds 32");
        if (cJSON_IsObject(node)) {
                std::set<std::string> names;
                for (const auto *x = node->child; x; x = x->next) {
                        if (!x->string || !names.insert(x->string).second)
                                throw std::runtime_error("Duplicate JSON key");
                        uniqueKeys(x, depth + 1);
                }
        } else if (cJSON_IsArray(node))
                for (auto *x = node->child; x; x = x->next)
                        uniqueKeys(x, depth + 1);
}
inline Document
parse(const std::string &content, bool allowIncomplete = false)
{
        if (content.size() > 16 * 1024 * 1024 || content.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid JSON size/content");
        // cJSON exposes strings as C strings, so reject escaped NUL before it can
        // silently truncate a name (an escaped backslash is skipped as one pair)
        for (size_t i = 0; i + 1 < content.size(); ++i)
                if (content[i] == '\\') {
                        if (content.compare(i, 6, "\\u0000") == 0)
                                throw std::runtime_error("JSON strings must not contain NUL");
                        ++i;
                }
        const char *start = content.c_str() + (content.starts_with("\xEF\xBB\xBF") ? 3 : 0);
        Json        root(cJSON_ParseWithOpts(start, nullptr, true), cJSON_Delete);
        if (!root || !cJSON_IsObject(root.get()))
                throw std::runtime_error("Invalid parameter JSON");
        uniqueKeys(root.get());
        if (string(root.get(), "format", true) != "foc-parameters" || !cJSON_IsNumber(get(root.get(), "version")) ||
            get(root.get(), "version")->valuedouble != 1)
                throw std::runtime_error("Expected foc-parameters version 1");
        Document doc;
        doc.name   = string(root.get(), "name");
        doc.target = string(root.get(), "target", true);
        if (const auto *components = get(root.get(), "components")) {
                if (!cJSON_IsObject(components))
                        throw std::runtime_error("components requires an object");
                for (const auto *entry = components->child; entry; entry = entry->next) {
                        if (!isComponent(entry->string) || !cJSON_IsString(entry))
                                throw std::runtime_error("Invalid component name/section");
                        doc.components.emplace(entry->string, entry->valuestring);
                }
                if (componentName(doc.components) != doc.name)
                        throw std::runtime_error("Configuration name differs from components");
        }
        if (auto *s = get(root.get(), "status"))
                doc.status = scalar(s);
        if (doc.status != "ready" && !(allowIncomplete && doc.status == "needs_mapping"))
                throw std::runtime_error("Historical profile needs hardware mapping review; see its notes");
        const auto *fields = get(root.get(), "parameters");
        if (!cJSON_IsArray(fields) || (cJSON_GetArraySize(fields) == 0 && doc.status == "ready") ||
            cJSON_GetArraySize(fields) > 65536)
                throw std::runtime_error("Expected 1..65536 parameters");
        std::set<std::string> paths;
        std::set<unsigned>    ids;
        for (const auto *x = fields->child; x; x = x->next) {
                Field f;
                if (auto *id = get(x, "id")) {
                        if (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 65535 ||
                            std::floor(id->valuedouble) != id->valuedouble)
                                throw std::runtime_error("Invalid permanent parameter ID");
                        f.id = static_cast<unsigned>(id->valuedouble);
                        if (!ids.insert(f.id).second)
                                throw std::runtime_error("Duplicate parameter ID");
                        const auto *rev = get(x, "revision");
                        if (!cJSON_IsNumber(rev) || rev->valuedouble < 1 || rev->valuedouble > 255 ||
                            std::floor(rev->valuedouble) != rev->valuedouble)
                                throw std::runtime_error("Invalid parameter revision");
                        f.revision = static_cast<unsigned>(rev->valuedouble);
                }
                f.row.path = string(x, "path", true);
                if (f.row.path.size() > 4096 || std::count(f.row.path.begin(), f.row.path.end(), '.') +
                                                        std::count(f.row.path.begin(), f.row.path.end(), '[') >
                                                    32)
                        throw std::runtime_error("Parameter path exceeds maximum depth/length");
                if (!paths.insert(f.row.path).second)
                        throw std::runtime_error("Duplicate parameter path: " + f.row.path);
                try {
                        f.row.type = string(x, "type", true);
                        if (get(x, "role"))
                                f.role = string(x, "role", true);
                        if (f.role != "config" && f.role != "derived")
                                throw std::runtime_error("Invalid parameter role");
                        f.expression   = string(x, "expression");
                        f.runtimePath  = string(x, "runtime_path");
                        f.autoWhenZero = string(x, "auto_when_zero");
                        f.display      = string(x, "display");
                        if (!f.display.empty() && f.display != "checkbox")
                                throw std::runtime_error("Invalid display widget");
                        if (f.role == "derived") {
                                if (get(x, "value"))
                                        throw std::runtime_error("Derived fields must not contain a configured value");
                                if (get(x, "preset_value"))
                                        throw std::runtime_error("Derived fields must not contain a preset value");
                        } else
                                f.row.val = scalar(get(x, "value"));
                        if (get(x, "preset_value"))
                                f.presetValue = string(x, "preset_value", true);
                        f.row.unit        = string(x, "unit");
                        f.row.description = string(x, "description");
                        if (auto *m = get(x, "min"))
                                f.row.min = scalar(m);
                        if (auto *m = get(x, "max"))
                                f.row.max = scalar(m);
                        if (auto *e = get(x, "enum")) {
                                if (!cJSON_IsObject(e) || !e->child)
                                        throw std::runtime_error("enum must be a nonempty name/value object");
                                std::set<std::string> values;
                                for (auto *v = e->child; v; v = v->next) {
                                        auto val = scalar(v);
                                        if (val.find_first_not_of("-0123456789") != std::string::npos || val.empty() ||
                                            val == "-")
                                                throw std::runtime_error("enum values must be decimal integers");
                                        if (!values.insert(val).second)
                                                throw std::runtime_error("Duplicate enum value");
                                        f.enums.emplace(v->string, val);
                                }
                                auto selected = f.enums.find(f.row.val);
                                if (selected != f.enums.end())
                                        f.row.val = selected->second;
                                else if (!values.contains(f.row.val))
                                        throw std::runtime_error("Value is not a declared enum option");
                                if (f.presetValue) {
                                        const auto original = f.enums.find(*f.presetValue);
                                        if (original != f.enums.end())
                                                f.presetValue = original->second;
                                        else if (!values.contains(*f.presetValue))
                                                throw std::runtime_error("Preset value is not a declared enum option");
                                }
                        }
                        if (f.display == "checkbox" && f.role != "derived" && f.row.val != "0" && f.row.val != "1" &&
                            f.row.val != "true" && f.row.val != "false")
                                throw std::runtime_error("Checkbox requires 0/1 or true/false");
                        if (f.presetValue && !sameParameterValue(f, *f.presetValue, *f.presetValue))
                                throw std::runtime_error("Preset value does not fit the declared type");
                        doc.fields.push_back(std::move(f));
                } catch (const std::exception &e) {
                        throw std::runtime_error(f.row.path + ": " + e.what());
                }
        }
        if (!doc.components.empty())
                for (const auto *section : componentSections) {
                        const std::string prefix = std::string(section) + '.';
                        if (std::none_of(doc.fields.begin(), doc.fields.end(), [&](const auto &f) {
                                    return f.role == "config" && f.row.path.starts_with(prefix);
                            }))
                                throw std::runtime_error(std::string("Missing configuration component: ") + section);
                }
        refreshComponentNames(doc);
        return doc;
}
/**
 * @brief The registry must accompany the firmware/AXF. An ID is only valid for the
 *        same path, type and revision; a first-release ID reset must never redirect an
 *        older preset to another field that happens to have the same scalar type
 */
inline void
validateIds(const Document &document, const std::string &registryText)
{
        Json root(cJSON_ParseWithOpts(registryText.c_str(), nullptr, true), cJSON_Delete);
        if (!root || !cJSON_IsObject(root.get()) || string(root.get(), "format") != "foc-parameter-registry" ||
            !cJSON_IsNumber(get(root.get(), "version")) || get(root.get(), "version")->valuedouble != 1)
                throw std::runtime_error("Expected foc-parameter-registry version 1");
        uniqueKeys(root.get());
        const auto *entries = get(root.get(), "parameters");
        if (!cJSON_IsArray(entries))
                throw std::runtime_error("Missing registry entries");
        std::map<unsigned, const cJSON *> entriesById;
        for (const auto *entry = entries->child; entry; entry = entry->next) {
                const auto *identifier = get(entry, "id");
                if (!cJSON_IsNumber(identifier) || identifier->valuedouble < 1 || identifier->valuedouble > 65535 ||
                    std::floor(identifier->valuedouble) != identifier->valuedouble ||
                    !entriesById.emplace(static_cast<unsigned>(identifier->valuedouble), entry).second)
                        throw std::runtime_error("Invalid registry ID");
        }
        for (const auto &field : document.fields) {
                if (!field.id || field.role == "derived")
                        continue;
                const auto found = entriesById.find(field.id);
                if (found == entriesById.end() || cJSON_IsTrue(get(found->second, "retired")))
                        throw std::runtime_error("Parameter ID " + std::to_string(field.id) +
                                                 ": not present in the current firmware schema");
                const auto *entry    = found->second;
                const auto *revision = get(entry, "revision");
                if (!cJSON_IsNumber(revision) || revision->valuedouble != field.revision ||
                    string(entry, "type", true) != field.row.type || string(entry, "path", true) != field.row.path)
                        throw std::runtime_error("Parameter ID " + std::to_string(field.id) +
                                                 ": path/type/revision differs from the current firmware schema");
        }
}
inline std::string
format(const Document &input)
{
        auto doc = input;
        refreshComponentNames(doc);
        Json root(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(root.get(), "format", "foc-parameters");
        cJSON_AddNumberToObject(root.get(), "version", 1);
        cJSON_AddStringToObject(root.get(), "name", doc.name.c_str());
        cJSON_AddStringToObject(root.get(), "target", doc.target.c_str());
        cJSON_AddStringToObject(root.get(), "status", doc.status.c_str());
        if (!doc.components.empty()) {
                if (componentName(doc.components) != doc.name)
                        throw std::runtime_error("Configuration name differs from components");
                auto *components = cJSON_AddObjectToObject(root.get(), "components");
                for (const auto *section : componentSections)
                        cJSON_AddStringToObject(components, section, doc.components.at(section).c_str());
        }
        auto *fields = cJSON_AddArrayToObject(root.get(), "parameters");
        for (const auto &f : doc.fields) {
                const auto &r = f.row;
                auto       *x = cJSON_CreateObject();
                cJSON_AddItemToArray(fields, x);
                if (f.id) {
                        cJSON_AddNumberToObject(x, "id", f.id);
                        cJSON_AddNumberToObject(x, "revision", f.revision);
                }
                cJSON_AddStringToObject(x, "path", r.path.c_str());
                cJSON_AddStringToObject(x, "type", r.type.c_str());
                if (f.role == "derived") {
                        cJSON_AddStringToObject(x, "role", "derived");
                        cJSON_AddStringToObject(x, "expression", f.expression.c_str());
                        cJSON_AddStringToObject(x, "runtime_path", f.runtimePath.c_str());
                } else
                        cJSON_AddStringToObject(x, "value", r.val.c_str());
                if (f.role != "derived" && f.presetValue)
                        cJSON_AddStringToObject(x, "preset_value", f.presetValue->c_str());
                if (!f.display.empty())
                        cJSON_AddStringToObject(x, "display", f.display.c_str());
                if (!f.autoWhenZero.empty())
                        cJSON_AddStringToObject(x, "auto_when_zero", f.autoWhenZero.c_str());
                cJSON_AddStringToObject(x, "unit", r.unit.c_str());
                cJSON_AddStringToObject(x, "description", r.description.c_str());
                if (!r.min.empty())
                        cJSON_AddStringToObject(x, "min", r.min.c_str());
                if (!r.max.empty())
                        cJSON_AddStringToObject(x, "max", r.max.c_str());
                if (!f.enums.empty()) {
                        auto *e = cJSON_AddObjectToObject(x, "enum");
                        for (const auto &[name, val] : f.enums)
                                cJSON_AddStringToObject(e, name.c_str(), val.c_str());
                }
        }
        char *raw = cJSON_Print(root.get());
        if (!raw)
                throw std::runtime_error("JSON allocation failed");
        std::string result = raw;
        cJSON_free(raw);
        return result;
}
/**
 * @brief A preset remains an ordinary complete parameter JSON. Only the chosen prefix
 *        supplies values; names carry provenance without changing the MCU layout
 */
inline Document
compose(const Document    &baseInput,
        const Document    &presetInput,
        const std::string &section,
        const std::string &fallbackName = "")
{
        auto base = baseInput, preset = presetInput;
        capturePresetValues(base);
        capturePresetValues(preset);
        if (!isComponent(section))
                throw std::runtime_error("Unknown configuration component: " + section);
        if (base.target.empty() || preset.target != base.target)
                throw std::runtime_error("Component target must match the selected variable");
        if (base.status != "ready" || preset.status != "ready")
                throw std::runtime_error("Component profiles must be ready");
        for (const auto *required : componentSections) {
                const std::string prefix = std::string(required) + '.';
                if (std::none_of(base.fields.begin(), base.fields.end(), [&](const auto &f) {
                            return f.role == "config" && f.row.path.starts_with(prefix);
                    }))
                        throw std::runtime_error(std::string("Open a complete configuration first; missing ") + required);
        }
        const auto                           selected = [&](const Field &f) { return f.row.path.starts_with(section + '.'); };
        std::map<std::string, const Field *> expected, incoming;
        for (const auto &field : base.fields)
                if (selected(field) && field.role == "config")
                        expected.emplace(field.row.path, &field);
        for (const auto &field : preset.fields)
                if (selected(field) && field.role == "config")
                        incoming.emplace(field.row.path, &field);
        if (incoming.size() != expected.size() || incoming.empty())
                throw std::runtime_error(section + ": preset must contain every field in this component");
        for (const auto &[path, field] : expected) {
                const auto it = incoming.find(path);
                if (it == incoming.end() || it->second->row.type != field->row.type ||
                    (field->id && it->second->id && (field->id != it->second->id || field->revision != it->second->revision)))
                        throw std::runtime_error(path + ": component schema/ID differs from opened configuration");
        }
        auto next       = base;
        next.components = componentNames(base);
        if (next.components.empty())
                throw std::runtime_error("Opened configuration needs a name before composing components");
        const auto names = componentNames(preset, fallbackName);
        if (names.empty())
                throw std::runtime_error("Selected component needs a name");
        next.components[section] = names.at(section);
        next.name                = componentName(next.components);
        next.fields.clear();
        bool inserted = false;
        // Only the selected component is swapped; root-level fields belong to no
        // component and always stay as the opened configuration had them
        for (const auto &field : base.fields) {
                if (!selected(field))
                        next.fields.push_back(field);
                else if (!inserted) {
                        for (const auto &replacement : preset.fields)
                                if (selected(replacement))
                                        next.fields.push_back(replacement);
                        inserted = true;
                }
        }
        // Also checks cross-component duplicate IDs/paths after the replacement
        return parse(format(next));
}
} // namespace parameter_json
