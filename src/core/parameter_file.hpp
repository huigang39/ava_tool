#pragma once
#include "core/parameter_json.hpp"
#include "core/parameter_tree.hpp"
#include <charconv>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace parameter_json
{
// File editing has no device addresses, DWARF types or connection dependency
struct FileEditor {
      private:
        struct NameCache {
                Document                           document;
                std::map<std::string, std::string> defaults, types, names;
        };
        std::shared_ptr<const NameCache> nameCache_;
        mutable TreeCache                treeCache_;

        bool namesUnchanged() const
        {
                if (!nameCache_)
                        return false;
                const auto &previous = nameCache_->document;
                return previous.name == document.name && previous.target == document.target &&
                       previous.components == document.components && previous.fields.size() == document.fields.size() &&
                       nameCache_->defaults == baselinePresetValues && nameCache_->types == baselineTypes &&
                       nameCache_->names == baselineNames &&
                       std::equal(previous.fields.begin(),
                                  previous.fields.end(),
                                  document.fields.begin(),
                                  [](const auto &before, const auto &now) {
                                          return before.row.path == now.row.path && before.row.type == now.row.type &&
                                                 before.row.val == now.row.val && before.role == now.role &&
                                                 before.presetValue == now.presetValue && before.enums == now.enums;
                                  });
        }

      public:
        Document                           document;
        std::string                        source;
        std::string                        baselineSource;
        std::map<std::string, std::string> baselinePresetValues, baselineTypes, baselineNames;
        bool                               dirty = false;

        const TreeNode &fieldTree() const { return treeCache_.get(document); }

        void load(const std::string &text, bool captureDefaults = true, const std::string &fallbackName = "")
        {
                auto next        = parse(text, true);
                auto namedSource = text;
                if (next.name.empty() && !fallbackName.empty()) {
                        next.name         = fallbackName;
                        const char *start = text.c_str() + (text.starts_with("\xEF\xBB\xBF") ? 3 : 0);
                        Json        root(cJSON_Parse(start), cJSON_Delete);
                        writeNames(root.get(), next);
                        char *raw = cJSON_Print(root.get());
                        if (!raw)
                                throw std::runtime_error("Cannot serialize parameter JSON");
                        namedSource = raw;
                        cJSON_free(raw);
                        next = parse(namedSource, true);
                }
                if (captureDefaults)
                        capturePresetValues(next);
                refreshComponentNames(next);
                document       = std::move(next);
                source         = namedSource;
                baselineSource = namedSource;
                baselinePresetValues.clear();
                baselineTypes.clear();
                baselineNames = completeComponents(document) ? componentNames(document) : std::map<std::string, std::string>{};
                for (const auto &field : document.fields)
                        if (field.role == "config") {
                                baselineTypes[field.row.path] = field.row.type;
                                if (field.presetValue)
                                        baselinePresetValues[field.row.path] = *field.presetValue;
                        }
                dirty = false;
        }

        void readValues(const std::unordered_map<std::string, std::string> &values)
        {
                for (auto &field : document.fields) {
                        if (field.role == "derived")
                                continue;
                        const auto found = values.find(field.row.path);
                        if (found == values.end())
                                continue;
                        auto val = found->second;
                        for (const auto &[name, option] : field.enums)
                                if (sameParameterValue(field, val, option)) {
                                        val = option;
                                        break;
                                }
                        if (field.row.val != val) {
                                field.row.val = std::move(val);
                                dirty         = true;
                        }
                }
                refreshNames();
        }

        void restoreDefaults()
        {
                if (source.empty())
                        throw std::runtime_error("Open a parameter JSON first");
                auto next    = document;
                bool changed = false;
                for (auto &field : next.fields) {
                        if (field.role != "config")
                                continue;
                        if (!field.presetValue)
                                throw std::runtime_error(field.row.path +
                                                         ": default value for the current model is unavailable");
                        changed       |= field.row.val != *field.presetValue;
                        field.row.val  = *field.presetValue;
                        validate(field);
                }
                refreshComponentNames(next);
                document  = std::move(next);
                dirty    |= changed;
                refreshNames();
        }

        void reset()
        {
                if (baselineSource.empty())
                        throw std::runtime_error("Open a parameter JSON first");
                auto next = parse(baselineSource, true);
                for (auto &field : next.fields)
                        if (field.role == "config") {
                                if (const auto original = baselinePresetValues.find(field.row.path);
                                    original != baselinePresetValues.end())
                                        field.presetValue = original->second;
                        }
                capturePresetValues(next);
                refreshComponentNames(next);
                document = std::move(next);
                source   = baselineSource;
                dirty    = false;
        }

        std::map<std::string, std::string> defaultsForBaseline(const Document &current) const
        {
                auto       defaults = baselinePresetValues;
                const auto names = completeComponents(current) ? componentNames(current) : std::map<std::string, std::string>{};
                for (const auto &field : current.fields) {
                        const auto original = baselineTypes.find(field.row.path);
                        if (field.role != "config" || !field.presetValue || defaults.contains(field.row.path) ||
                            original == baselineTypes.end() || original->second != field.row.type)
                                continue;
                        // A root-level field belongs to no component, so a renamed
                        // component cannot invalidate it as this document's default
                        if (!isRootField(field.row.path)) {
                                const auto section = field.row.path.substr(0, field.row.path.find('.'));
                                const auto now = names.find(section), before = baselineNames.find(section);
                                if (now != names.end() && before != baselineNames.end() &&
                                    baseName(now->second) != baseName(before->second))
                                        continue;
                        }
                        defaults[field.row.path] = *field.presetValue;
                }
                return defaults;
        }

        void refreshNames()
        {
                // Called before and after drawing every frame. Recompute only
                // when an input affecting names/defaults changed, including
                // direct edits through the public Document. Exact comparisons
                // keep correctness independent of hash collisions
                if (namesUnchanged())
                        return;
                auto next = document;
                refreshComponentNames(next);
                auto defaults        = defaultsForBaseline(next);
                nameCache_           = std::make_shared<NameCache>(NameCache{next, defaults, baselineTypes, baselineNames});
                document             = std::move(next);
                baselinePresetValues = std::move(defaults);
        }

        template <class T> static bool integer(const std::string &text)
        {
                T          val{};
                const auto result = std::from_chars(text.data(), text.data() + text.size(), val, 10);
                return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        static void validate(const Field &field)
        {
                if (field.role == "derived")
                        return;
                const auto &r     = field.row;
                bool        valid = false;
                if (r.type == "u8" || r.type == "uint8_t")
                        valid = integer<uint8_t>(r.val);
                else if (r.type == "u16" || r.type == "uint16_t")
                        valid = integer<uint16_t>(r.val);
                else if (r.type == "u32" || r.type == "uint32_t")
                        valid = integer<uint32_t>(r.val);
                else if (r.type == "u64" || r.type == "uint64_t")
                        valid = integer<uint64_t>(r.val);
                else if (r.type == "i8" || r.type == "int8_t")
                        valid = integer<int8_t>(r.val);
                else if (r.type == "i16" || r.type == "int16_t")
                        valid = integer<int16_t>(r.val);
                else if (r.type == "i32" || r.type == "int32_t")
                        valid = integer<int32_t>(r.val);
                else if (r.type == "i64" || r.type == "int64_t" || r.type == "enum")
                        valid = integer<int64_t>(r.val);
                else if (r.type == "bool")
                        valid = r.val == "0" || r.val == "1" || r.val == "true" || r.val == "false";
                else if (r.type == "f32" || r.type == "f16" || r.type == "float" || r.type == "float32" || r.type == "f64" ||
                         r.type == "double" || r.type == "float64") {
                        const auto val = parameter_csv::number(r.val);
                        valid = std::abs(val) <= (r.type == "f32" || r.type == "f16" || r.type == "float" || r.type == "float32"
                                                      ? std::numeric_limits<float>::max()
                                                      : std::numeric_limits<double>::max());
                }
                if (!valid)
                        throw std::runtime_error(r.path + ": value does not fit " + r.type);
                if (!field.enums.empty()) {
                        bool found = false;
                        for (const auto &[name, val] : field.enums)
                                found |= val == r.val;
                        if (!found)
                                throw std::runtime_error(r.path + ": value is not a declared enum option");
                }
                parameter_csv::bounds(r, r.val == "true" ? "1" : r.val == "false" ? "0" : r.val);
                if (field.presetValue) {
                        auto original    = field;
                        original.row.val = *field.presetValue;
                        original.presetValue.reset();
                        if (const auto option = original.enums.find(original.row.val); option != original.enums.end())
                                original.row.val = option->second;
                        validate(original);
                }
        }

        static void writeNames(cJSON *root, const Document &doc)
        {
                cJSON_DeleteItemFromObjectCaseSensitive(root, "target");
                cJSON_AddStringToObject(root, "target", doc.target.c_str());
                cJSON_DeleteItemFromObjectCaseSensitive(root, "name");
                cJSON_AddStringToObject(root, "name", doc.name.c_str());
                cJSON_DeleteItemFromObjectCaseSensitive(root, "components");
                if (!doc.components.empty()) {
                        if (componentName(doc.components) != doc.name)
                                throw std::runtime_error("Configuration name differs from components");
                        auto *names = cJSON_AddObjectToObject(root, "components");
                        for (const auto *section : componentSections)
                                cJSON_AddStringToObject(names, section, doc.components.at(section).c_str());
                }
        }

        static void writePresetValue(cJSON *item, const Field &field)
        {
                cJSON_DeleteItemFromObjectCaseSensitive(item, "preset_value");
                if (field.role == "config" && field.presetValue)
                        cJSON_AddStringToObject(item, "preset_value", field.presetValue->c_str());
        }

        void replaceComponent(const std::string &section, const std::string &newSource, const std::string &fallbackName = "")
        {
                if (source.empty())
                        throw std::runtime_error("Open a complete parameter JSON first");
                const auto preset = parse(newSource);
                auto       before = document;
                capturePresetValues(before);
                auto       next     = compose(before, preset, section, fallbackName);
                auto       defaults = defaultsForBaseline(before);
                const auto parseRaw = [](const std::string &text) {
                        return Json(cJSON_Parse(text.c_str() + (text.starts_with("\xEF\xBB\xBF") ? 3 : 0)), cJSON_Delete);
                };
                auto                                 root     = parseRaw(source);
                auto                                 incoming = parseRaw(newSource);
                std::map<std::string, const cJSON *> oldFields, newFields;
                for (const auto *item = get(root.get(), "parameters")->child; item; item = item->next)
                        oldFields.emplace(string(item, "path", true), item);
                for (const auto *item = get(incoming.get(), "parameters")->child; item; item = item->next)
                        newFields.emplace(string(item, "path", true), item);
                Json fields(cJSON_CreateArray(), cJSON_Delete);
                for (const auto &field : next.fields) {
                        validate(field);
                        const auto &lookup = field.row.path.starts_with(section + '.') ? newFields : oldFields;
                        const auto  it     = lookup.find(field.row.path);
                        if (it == lookup.end())
                                throw std::runtime_error("Missing JSON field metadata: " + field.row.path);
                        auto *item = cJSON_Duplicate(it->second, true);
                        if (!item || !cJSON_AddItemToArray(fields.get(), item)) {
                                if (item)
                                        cJSON_Delete(item);
                                throw std::runtime_error("Cannot copy component field");
                        }
                        if (field.role != "derived" &&
                            !cJSON_ReplaceItemInObjectCaseSensitive(item, "value", cJSON_CreateString(field.row.val.c_str())))
                                throw std::runtime_error("Cannot update component value");
                        writePresetValue(item, field);
                }
                if (!cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "parameters", fields.get()))
                        throw std::runtime_error("Cannot replace component parameters");
                fields.release();
                writeNames(root.get(), next);
                char *raw = cJSON_Print(root.get());
                if (!raw)
                        throw std::runtime_error("Cannot serialize component JSON");
                std::string serialized(raw);
                cJSON_free(raw);
                // Validate the exact metadata-preserving output as well: the
                // public Document can have been edited independently of source
                auto parsed          = parse(serialized);
                document             = std::move(parsed);
                source               = std::move(serialized);
                baselinePresetValues = std::move(defaults);
                dirty                = true;
        }

        std::string save() const
        {
                if (source.empty())
                        throw std::runtime_error("Open a parameter JSON first");
                auto current = document;
                refreshComponentNames(current);
                // Preserve provenance, notes, and any future metadata that the editor does not interpret
                const char *start = source.c_str() + (source.starts_with("\xEF\xBB\xBF") ? 3 : 0);
                Json        root(cJSON_Parse(start), cJSON_Delete);
                writeNames(root.get(), current);
                auto *fields = cJSON_GetObjectItemCaseSensitive(root.get(), "parameters");
                auto *item   = fields ? fields->child : nullptr;
                // cJSON arrays are linked lists; indexed access here used to
                // walk from the first field for every row (quadratic cost)
                for (const auto &field : current.fields) {
                        if (!item || string(item, "path", true) != field.row.path)
                                throw std::runtime_error("Parameter field metadata no longer matches its path");
                        auto *currentItem = item;
                        item              = item->next;
                        if (field.role == "derived")
                                continue;
                        validate(field);
                        writePresetValue(currentItem, field);
                        if (!cJSON_ReplaceItemInObjectCaseSensitive(
                                currentItem, "value", cJSON_CreateString(field.row.val.c_str())))
                                throw std::runtime_error("Cannot update JSON value");
                }
                if (item)
                        throw std::runtime_error("Parameter field metadata no longer matches its path");
                char *raw = cJSON_Print(root.get());
                if (!raw)
                        throw std::runtime_error("Cannot serialize parameter JSON");
                std::string result(raw);
                cJSON_free(raw);
                parse(result, true);
                return result;
        }
};
} // namespace parameter_json
