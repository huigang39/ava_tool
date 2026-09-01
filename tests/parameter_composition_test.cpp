#include "core/parameter_file.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace parameter_json;

static std::string fixture(const std::string &name) {
    return std::string(R"({"format":"foc-parameters","version":1,"name":")") + name + R"(","target":"g_foc.cfg","source":{"commit":"preserve-base"},"parameters":[
        {"id":1,"revision":1,"path":"mech.motor.npp","type":"uint32_t","value":"7","future_metadata":{"keep":1}},
        {"id":2,"revision":1,"path":"hw.timer.pwm_freq","type":"uint32_t","value":"20000","future_metadata":{"keep":2}},
        {"id":3,"revision":1,"path":"sw.ctrl.gain","type":"float","value":"1.5","future_metadata":{"keep":3}},
        {"path":"sw.calculated","type":"float","role":"derived","expression":"gain * 2","runtime_path":"g_foc.lo.runtime.gain","future_metadata":{"keep":4}}
    ]})";
}
static std::string read(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open test preset: " + path);
    return {(std::istreambuf_iterator<char>(input)), {}};
}
template<class F> static void rejects(F &&fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception &) { rejected = true; }
    assert(rejected);
}
static std::string printed(cJSON *root) {
    char *raw = cJSON_Print(root);
    assert(raw);
    std::string result(raw);
    cJSON_free(raw);
    return result;
}
static const Field &field(const Document &doc, const std::string &path) {
    const auto it = std::find_if(doc.fields.begin(), doc.fields.end(), [&](const auto &f) { return f.row.path == path; });
    assert(it != doc.fields.end());
    return *it;
}
int main(int argc, char **argv) {
    Field enumOrder;
    enumOrder.enums = {{"DRV8353_IDRIVEP_1000MA", "15"}, {"DRV8353_IDRIVEP_100MA", "2"},
                       {"DRV8353_IDRIVEP_50MA", "0"}, {"negative", "-10"}, {"minus_one", "-1"},
                       {"large", "18446744073709551614"}, {"larger", "18446744073709551615"}};
    const auto sorted = enumOptionsByValue(enumOrder);
    assert(sorted[0].second == "-10" && sorted[1].second == "-1");
    assert(sorted[2].first == "DRV8353_IDRIVEP_50MA");
    assert(sorted[3].first == "DRV8353_IDRIVEP_100MA");
    assert(sorted[4].first == "DRV8353_IDRIVEP_1000MA");
    assert(sorted[5].first == "large" && sorted[6].first == "larger");

    FileEditor custom;
    custom.load(fixture(""), true, "CUSTOM");
    assert(custom.document.name == "CUSTOM");
    assert(parse(custom.save()).name == "CUSTOM");
    Json customMetadata(cJSON_Parse(custom.save().c_str()), cJSON_Delete);
    assert(string(get(customMetadata.get(), "source"), "commit") == "preserve-base");
    custom.document.fields[0].row.val = "8";
    custom.refreshNames();
    assert(custom.document.name == "CUSTOM*/CUSTOM/CUSTOM");
    custom.reset();
    assert(custom.document.name == "CUSTOM" && custom.document.fields[0].row.val == "7");
    custom.load(fixture("Existing"), true, "CUSTOM");
    assert(custom.document.name == "Existing");

    // Device readback fills write controls without replacing JSON defaults
    FileEditor readback;
    auto readFixture = parse(fixture("Readback"));
    readFixture.fields[0].enums = {{"Seven", "7"}, {"Eight", "8"}};
    Field enabled;
    enabled.row.path = "sw.enabled"; enabled.row.type = "bool"; enabled.row.val = "false";
    readFixture.fields.push_back(enabled);
    readback.load(format(readFixture));
    const auto baseline = readback.baselineSource;
    readback.readValues({{"mech.motor.npp", "8"}, {"sw.enabled", "1"},
                        {"sw.calculated", "123"}, {"unknown", "999"}});
    assert(field(readback.document, "mech.motor.npp").row.val == "8");
    assert(field(readback.document, "sw.enabled").row.val == "1");
    assert(field(readback.document, "sw.calculated").row.val.empty());
    assert(field(readback.document, "hw.timer.pwm_freq").row.val == "20000");
    assert(readback.dirty && readback.baselineSource == baseline);
    assert(field(parse(readback.save()), "mech.motor.npp").row.val == "8");
    readback.reset();
    assert(field(readback.document, "mech.motor.npp").row.val == "7");
    assert(field(readback.document, "sw.enabled").row.val == "false");
    assert(!readback.dirty);
    readback.readValues({{"mech.motor.npp", "7"}});
    assert(!readback.dirty);
    readback.readValues({{"mech.motor.npp", "99"}});
    assert(field(readback.document, "mech.motor.npp").row.val == "99");
    rejects([&] { readback.save(); });     // Unknown values are never mapped to an arbitrary enum option

    // The first release resets parameter IDs. The same ID/type must never
    // redirect an old field to another path or silently drop it
    const std::string registry = R"({"format":"foc-parameter-registry","version":1,"parameters":[{"id":1,"revision":1,"path":"control.gain","type":"float"},{"id":2,"retired":true}]})";
    auto registered = parse(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","parameters":[{"id":1,"revision":1,"path":"control.gain","type":"float","value":"3"}]})");
    validateIds(registered, registry);
    const auto registeredJson = format(registered);
    registered.fields[0].row.path = "other.gain";
    const auto staleJson = format(registered);
    rejects([&] { validateIds(registered, registry); });
    assert(format(registered) == staleJson);     // Rejection does not alter the preset
    registered = parse(registeredJson);
    registered.fields[0].id = 2;
    rejects([&] { validateIds(registered, registry); });
    registered.fields[0].id = 3;
    rejects([&] { validateIds(registered, registry); });
    registered = parse(registeredJson);
    registered.fields[0].revision = 2;
    rejects([&] { validateIds(registered, registry); });
    registered = parse(registeredJson);
    registered.fields[0].row.type = "uint32_t";
    rejects([&] { validateIds(registered, registry); });
    registered = parse(registeredJson);
    rejects([&] { validateIds(registered, registry + " trailing"); });
    std::cout << "PASS: reassigned, retired, unknown and mismatched IDs rejected without changing values\n";

    FileEditor editor;
    editor.load(fixture("NEO_45H"));
    assert(editor.document.components.empty());
    assert(parse(editor.save()).name == "NEO_45H");
    editor.document.fields[0].row.val = "17";
    editor.document.fields[2].row.val = "2.5";
    auto source = fixture("NEO_65H");
    const auto position = source.find("20000"); source.replace(position, 5, "25000");
    editor.replaceComponent("hw", source);
    assert(editor.dirty);
    assert(editor.document.name == "NEO_45H*/NEO_65H/NEO_45H*");
    assert(field(editor.document, "mech.motor.npp").row.val == "17");
    assert(field(editor.document, "sw.ctrl.gain").row.val == "2.5");
    assert(field(editor.document, "hw.timer.pwm_freq").row.val == "25000");
    const auto saved = editor.save();
    // Restoring model defaults keeps the selected component models, including
    // after saving/reopening edits; it never returns to the original file
    auto modelDefaults = editor;
    modelDefaults.document.fields[1].row.val = "30000";
    modelDefaults.restoreDefaults();
    assert(modelDefaults.document.name == "NEO_45H/NEO_65H/NEO_45H");
    assert(field(modelDefaults.document, "mech.motor.npp").row.val == "7");
    assert(field(modelDefaults.document, "hw.timer.pwm_freq").row.val == "25000");
    assert(field(modelDefaults.document, "sw.ctrl.gain").row.val == "1.5");
    assert(modelDefaults.dirty && modelDefaults.baselineSource == editor.baselineSource);
    modelDefaults.load(saved);
    modelDefaults.restoreDefaults();
    assert(modelDefaults.document.name == "NEO_45H/NEO_65H/NEO_45H");
    assert(field(modelDefaults.document, "hw.timer.pwm_freq").row.val == "25000");
    FileEditor fromCustom;
    fromCustom.load(fixture("CUSTOM"));
    fromCustom.replaceComponent("mech", fixture("NEO_45H"));
    fromCustom.replaceComponent("hw", source);
    fromCustom.replaceComponent("sw", fixture("NEO_75H"));
    fromCustom.readValues({{"mech.motor.npp", "99"}, {"hw.timer.pwm_freq", "30000"}});
    fromCustom.restoreDefaults();
    assert(fromCustom.document.name == "NEO_45H/NEO_65H/NEO_75H");
    assert(field(fromCustom.document, "hw.timer.pwm_freq").row.val == "25000");
    assert(parse(fromCustom.save()).name == fromCustom.document.name);
    auto unavailable = editor;
    unavailable.document.fields[1].presetValue.reset();
    const auto unmodified = unavailable.save();
    rejects([&] { unavailable.restoreDefaults(); });
    assert(unavailable.save() == unmodified);
    Json raw(cJSON_Parse(saved.c_str()), cJSON_Delete);
    assert(string(get(raw.get(), "source"), "commit") == "preserve-base");
    for (auto *item = get(raw.get(), "parameters")->child; item; item = item->next) assert(get(item, "future_metadata"));
    assert(field(editor.document, "sw.calculated").expression == "gain * 2");
    // Composition must not turn pending edits into the reset baseline
    auto resettable = editor;
    resettable.reset();
    assert(!resettable.dirty && resettable.document.name == "NEO_45H" && resettable.document.components.empty());
    assert(field(resettable.document, "mech.motor.npp").row.val == "7");
    assert(field(resettable.document, "hw.timer.pwm_freq").row.val == "20000");
    assert(field(resettable.document, "sw.ctrl.gain").row.val == "1.5");
    assert(resettable.source == fixture("NEO_45H") && resettable.baselineSource == resettable.source);
    // The UI reloads successfully saved content; reset should then restore it
    resettable.load(saved);
    resettable.document.fields[0].row.val = "21";
    resettable.replaceComponent("sw", fixture("NEO_75H"));
    resettable.reset();
    assert(resettable.save() == saved && !resettable.dirty);
    assert(resettable.document.name == "NEO_45H*/NEO_65H/NEO_45H*");
    FileEditor reopened; reopened.load(saved);
    reopened.replaceComponent("sw", fixture("NEO_75H"));
    assert(reopened.document.name == "NEO_45H*/NEO_65H/NEO_75H");
    assert(field(reopened.document, "mech.motor.npp").row.val == "17");
    assert(field(reopened.document, "sw.ctrl.gain").row.val == "1.5");
    reopened.replaceComponent("mech", saved);
    assert(reopened.document.name == "NEO_45H*/NEO_65H/NEO_75H");
    // Device readback has only the name; its three components are recoverable
    auto device = parse(saved); device.components.clear();
    assert(componentNames(device).at("hw") == "NEO_65H");
    assert(compose(device, parse(source), "sw").name == "NEO_45H*/NEO_65H/NEO_65H");

    const auto before = reopened.save();
    const auto beforeSource = reopened.source;
    const auto beforeBaseline = reopened.baselineSource;
    const auto beforeDirty = reopened.dirty;
    const auto failUnchanged = [&](const std::string &input, const std::string &section = "hw") {
        rejects([&] { reopened.replaceComponent(section, input); });
        assert(reopened.save() == before && reopened.source == beforeSource && reopened.baselineSource == beforeBaseline && reopened.dirty == beforeDirty);
    };
    failUnchanged(source, "other");
    auto bad = parse(source); bad.fields.erase(bad.fields.begin() + 1); failUnchanged(format(bad));
    bad = parse(source); bad.fields[1].row.type = "float"; failUnchanged(format(bad));
    bad = parse(source); bad.fields[1].id = 1; failUnchanged(format(bad));
    bad = parse(source); bad.fields[1].id = 42; failUnchanged(format(bad));
    bad = parse(source); bad.target = "g_other.cfg"; failUnchanged(format(bad));
    bad = parse(source); bad.status = "needs_mapping"; failUnchanged(format(bad));
    bad = parse(source); bad.name = std::string(60, 'x'); failUnchanged(format(bad));
    bad = parse(source); bad.fields[1].row.val = "-1"; failUnchanged(format(bad));
    // Replacing a component can recover from an invalid edit in that component
    reopened.document.fields[1].row.val = "bad number";
    reopened.replaceComponent("hw", source);
    assert(field(reopened.document, "hw.timer.pwm_freq").row.val == "25000");
    std::map<std::string, std::string> limit = {{"mech", std::string(20,'a')}, {"hw",std::string(20,'b')}, {"sw",std::string(21,'c')}};
    assert(componentName(limit).size() == 63);
    limit["sw"] += 'c'; rejects([&] { componentName(limit); });
    limit["sw"] = "bad/name"; rejects([&] { componentName(limit); });
    limit["sw"] = std::string("bad\0name",8); rejects([&] { componentName(limit); });
    limit["sw"] = ""; rejects([&] { componentName(limit); });
    rejects([&] { parse(fixture("bad\\u0000name")); });
    assert(parse(fixture("literal\\\\u0000name")).name == "literal\\u0000name");
    Json wrong(cJSON_Parse(saved.c_str()), cJSON_Delete);
    cJSON_ReplaceItemInObjectCaseSensitive(wrong.get(), "name", cJSON_CreateString("incorrect"));
    rejects([&] { parse(printed(wrong.get())); });
    cJSON_ReplaceItemInObjectCaseSensitive(wrong.get(), "name", cJSON_CreateString(editor.document.name.c_str()));
    cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(wrong.get(), "components"), "unknown", "X");
    rejects([&] { parse(printed(wrong.get())); });
    assert(componentNames(Document{}).empty());

    FileEditor marking;
    marking.load(fixture("NEO_45H"));
    marking.document.fields[2].row.val = "1.5000000000";
    marking.refreshNames();
    assert(marking.document.name == "NEO_45H");
    marking.document.fields[2].row.val = "1.6";
    marking.refreshNames();
    assert(marking.document.name == "NEO_45H/NEO_45H/NEO_45H*");
    marking.document.fields[0].row.val = "8";
    marking.refreshNames();
    assert(marking.document.name == "NEO_45H*/NEO_45H/NEO_45H*");
    const auto customized = marking.save();
    marking.load(customized);     // Saving/reopening does not establish new defaults
    assert(marking.document.name == "NEO_45H*/NEO_45H/NEO_45H*");
    assert(marking.document.fields[0].presetValue == "7");
    assert(!marking.document.fields.back().presetValue);
    marking.document.fields[0].row.val = "7";
    marking.document.fields[2].row.val = "1.5";
    marking.refreshNames();
    assert(marking.document.name == "NEO_45H/NEO_45H/NEO_45H");
    marking.reset();
    assert(marking.document.name == "NEO_45H*/NEO_45H/NEO_45H*");
    FileEditor selectedModified;
    selectedModified.load(fixture("NEO_65H"));
    selectedModified.replaceComponent("sw", customized);
    assert(selectedModified.document.name == "NEO_65H/NEO_65H/NEO_45H*");
    selectedModified.document.fields[2].row.val = "1.5";
    selectedModified.refreshNames();
    assert(selectedModified.document.name == "NEO_65H/NEO_65H/NEO_45H");
    // A marked external file may have lost its reference values. Keep its mark
    auto unknown = parse(fixture("NEO_45H/NEO_65H*/NEO_45H"));
    capturePresetValues(unknown);
    refreshComponentNames(unknown);
    assert(!unknown.fields[1].presetValue && unknown.name == "NEO_45H/NEO_65H*/NEO_45H");
    // Resolve a historical edited file against the official preset before capture
    auto historical = fixture("NEO_45H");
    historical.replace(historical.find("1.5"), 3, "2.5");
    FileEditor resolved;
    resolved.load(historical, false);
    resolved.document.fields[2].presetValue = "1.5";
    capturePresetValues(resolved.document);
    resolved.refreshNames();
    assert(resolved.document.name == "NEO_45H/NEO_45H/NEO_45H*");
    resolved.replaceComponent("sw", fixture("NEO_65H"));
    resolved.reset();
    assert(resolved.document.name == "NEO_45H/NEO_45H/NEO_45H*" && resolved.document.fields[2].presetValue == "1.5");

    Field number;
    number.row.type = "uint64_t";
    assert(sameParameterValue(number, "18446744073709551615", "18446744073709551615"));
    assert(!sameParameterValue(number, "18446744073709551615", "18446744073709551614"));
    number.row.type = "int64_t";
    assert(!sameParameterValue(number, "-9223372036854775808", "-9223372036854775807"));
    number.row.type = "float";
    assert(sameParameterValue(number, "0.1", "0.10000000149011612"));
    assert(sameParameterValue(number, "-0.0", "0.000e+0"));
    assert(!sameParameterValue(number, "0.1", "0.10001"));
    assert(!sameParameterValue(number, "nan", "nan"));
    number.row.type = "double";
    assert(!sameParameterValue(number, "0.1", "0.10000000149011612"));
    number.row.type = "bool";
    assert(sameParameterValue(number, "true", "1"));
    number.row.type = "enum"; number.enums = {{"STOP","0"},{"RUN","1"}};
    assert(sameParameterValue(number, "RUN", "1"));
    Field highlighted;
    highlighted.row.type = "f32";
    highlighted.row.val = "0.1";
    highlighted.presetValue = "0.1";
    std::string readValue = "0.10000000149011612";
    assert(readDifference(highlighted, &readValue) == ReadDifference::None);
    readValue = "0.2";
    assert(readDifference(highlighted, &readValue) == ReadDifference::Write);     // Both differ: red wins
    highlighted.row.val = "0.2";
    assert(readDifference(highlighted, &readValue) == ReadDifference::Default);     // Applied, but non-default: yellow
    highlighted.presetValue = "0.2";
    assert(readDifference(highlighted, &readValue) == ReadDifference::None);
    highlighted.row.val = "0.3";
    assert(readDifference(highlighted, &readValue) == ReadDifference::Write);     // Default read, pending write: red
    assert(readDifference(highlighted, nullptr) == ReadDifference::None);         // Unread/stale device
    highlighted.role = "derived";
    assert(readDifference(highlighted, &readValue) == ReadDifference::None);
    highlighted.role = "config";
    highlighted.presetValue.reset();
    highlighted.row.val = readValue;
    assert(readDifference(highlighted, &readValue) == ReadDifference::None);     // No known baseline
    auto invalidDefault = parse(fixture("NEO_45H"));
    invalidDefault.fields[0].presetValue = "bad";
    rejects([&] { parse(format(invalidDefault)); });
    invalidDefault.fields[0].presetValue = "4294967296";
    rejects([&] { parse(format(invalidDefault)); });
    invalidDefault.fields[0].presetValue = "7";
    invalidDefault.fields[2].presetValue = "inf";
    rejects([&] { parse(format(invalidDefault)); });
    invalidDefault.fields[2].presetValue = "1.5";
    invalidDefault.fields.back().presetValue = "0";
    // Derived baselines never enter exported JSON
    assert(!parse(format(invalidDefault)).fields.back().presetValue);
    auto longName = parse(fixture("A"));
    longName.components = {{"mech", std::string(20,'a')}, {"hw",std::string(20,'b')}, {"sw",std::string(21,'c')}};
    longName.name = componentName(longName.components);
    capturePresetValues(longName);
    longName.fields[0].row.val = "8";
    const auto unchangedName = longName.name;
    const auto unchangedNames = longName.components;
    rejects([&] { refreshComponentNames(longName); });
    assert(longName.name == unchangedName && longName.components == unchangedNames);
    auto partial = parse(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","name":"partial","parameters":[{"path":"gain","type":"float","value":"2","preset_value":"1"}]})");
    refreshComponentNames(partial);
    assert(partial.name == "partial" && partial.components.empty());

    if (argc > 1) {
        const std::string folder = argv[1];
        const auto original = parse(read(folder + "/NEO_45H.json"));
        const auto board = parse(read(folder + "/NEO_65H.json"));
        const auto manifest = read(folder + "/param_list_manifest.json");
        validateIds(original, manifest);
        validateIds(board, manifest);
        FileEditor actual; actual.load(read(folder + "/NEO_45H.json"));
        actual.replaceComponent("hw", read(folder + "/NEO_65H.json"));
        assert(actual.document.name == "NEO_45H/NEO_65H/NEO_45H");
        assert(actual.document.fields.size() == original.fields.size());
        for (const auto &f : actual.document.fields) {
            const auto &expected = field(f.row.path.starts_with("hw.") ? board : original, f.row.path);
            assert(f.row.val == expected.row.val && f.id == expected.id && f.revision == expected.revision && f.role == expected.role);
        }
        const auto result = actual.save();
        assert(parse(result).components == actual.document.components);
        std::cout << "PASS: complete NEO_45H / NEO_65H / NEO_45H preset composition (" << actual.document.fields.size() << " fields)\n";
    }
    std::cout << "PASS: component isolation, pending edits, metadata, name limits, readback, atomic rejection and roundtrip\n";
}
