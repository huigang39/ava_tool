#include "gui/fault_decoder.hpp"
#include "cJSON.h"
#include "gui/dock_close.hpp"
#include "gui/flow_toolbar.hpp"
#include "gui/hover_panel.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor_types.hpp"
#include "gui/ui_theme.hpp"
#include "gui/variable.hpp"
#include "platform/native_dlg.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

namespace safety_ui
{
void
drawResult(const safety::Result &result, bool stale)
{
        if (result.unknown)
                ui::TextColored(ImVec4(.85f, .65f, 1, 1), "%s", "包含未定义值 / 位或规则不匹配");
        ImGui::TextWrapped("%s", result.summary.c_str());
        if (!result.note.empty())
                ImGui::TextDisabled("%s", result.note.c_str());
        for (size_t i = 0; i < result.findings.size(); ++i) {
                const auto &finding = result.findings[i];
                ImGui::PushID(int(i));
                const auto color = stale                                           ? ImVec4(.6f, .6f, .6f, 1)
                                   : finding.severity == safety::Severity::Error   ? ui::ErrorTextColor()
                                   : finding.severity == safety::Severity::Warning ? ImVec4(1, .73f, .25f, 1)
                                   : finding.severity == safety::Severity::Ok      ? ImVec4(.4f, .8f, .6f, 1)
                                                                                   : ImVec4(.5f, .75f, 1, 1);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ui::ReadableTextColor(color, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), 5.0f));
                const bool expanded = ImGui::TreeNodeEx("finding",
                                                        ImGuiTreeNodeFlags_None,
                                                        "%s",
                                                        finding.title.empty() ? finding.name.c_str() : finding.title.c_str());
                ImGui::PopStyleColor();
                if (expanded) {
                        if (!finding.name.empty())
                                ImGui::TextWrapped("%s", finding.name.c_str());
                        if (!finding.cause.empty())
                                ImGui::TextWrapped("含义:%s", finding.cause.c_str());
                        if (!finding.action.empty())
                                ImGui::TextWrapped("排查:%s", finding.action.c_str());
                        if (!finding.clear.empty())
                                ImGui::TextWrapped("清除条件:%s", finding.clear.c_str());
                        ImGui::TreePop();
                }
                ImGui::PopID();
        }
}
} // namespace safety_ui

void
FaultDecoder::scanProfileDirectory()
{
        profileFiles_.clear();
        directoryError_.clear();
        if (profileDirectory_.empty())
                return;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 std::filesystem::u8path(profileDirectory_), std::filesystem::directory_options::skip_permission_denied, error),
             end;
             !error && it != end;
             it.increment(error)) {
                if (!it->is_regular_file(error))
                        continue;
                auto extension = it->path().extension().string();
                std::transform(
                    extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
                if (extension != ".json")
                        continue;
                const auto utf = it->path().u8string();
                profileFiles_.emplace_back(utf.begin(), utf.end());
        }
        std::sort(profileFiles_.begin(), profileFiles_.end());
        if (error)
                directoryError_ = tr("Cannot read directory: ", "无法读取目录: ") + profileDirectory_;
}

bool
FaultDecoder::loadProfile(const std::string &path)
{
        if (path.empty())
                return false;
        try {
                auto       replacement = safety::load(path);
                const auto file        = std::filesystem::u8path(path);
                const auto relative    = file.lexically_relative(std::filesystem::u8path(profileDirectory_));
                if (profileDirectory_.empty() || relative.empty() || *relative.begin() == "..") {
                        const auto utf = file.parent_path().u8string();
                        profileDirectory_.assign(utf.begin(), utf.end());
                        profileFiles_.clear();
                        directoryError_.clear();
                }
                if (profilePath_ != path) {
                        roots_.clear();
                        bindings_.clear();
                }
                profile_     = std::move(replacement);
                profilePath_ = path;
                std::snprintf(profileInput_, sizeof(profileInput_), "%s", path.c_str());
                profileError_.clear();
                groups_.clear();
                selectedRow_.clear();
                manualInput_[0] = 0;
                nextDetection_  = 0;
                // 旧名称绑定归入新名称, 已有的新名称绑定优先, 已删除规则不再保留
                auto previousBindings = std::move(bindings_);
                bindings_.clear();
                std::vector<std::string> bindingIds;
                for (const auto &[id, binding] : previousBindings)
                        bindingIds.push_back(id);
                std::sort(bindingIds.begin(), bindingIds.end());
                for (const auto &id : bindingIds)
                        if (const auto *decoder = safety::findDecoder(profile_, id))
                                if (!bindings_.contains(decoder->id) || id == decoder->id)
                                        bindings_[decoder->id] = std::move(previousBindings.at(id));
                for (auto &[id, binding] : bindings_) {
                        binding.val   = {};
                        binding.valid = binding.hasValue = false;
                        binding.status                   = "配置已重新加载,等待新采样";
                }
                if (const auto *decoder = safety::findDecoder(profile_, decoderId_))
                        decoderId_ = decoder->id;
                else
                        decoderId_ = profile_.decoders.empty() ? "" : profile_.decoders.front().id;
                return true;
        } catch (const std::exception &error) {
                profileError_ = error.what();
                if (profilePath_.empty())
                        std::snprintf(profileInput_, sizeof(profileInput_), "%s", path.c_str());
                return false;
        }
}

bool
FaultDecoder::bind(const Variables &variables, const std::string &manager, const std::string &name)
{
        if (const auto *decoder = safety::findDecoder(profile_, decoderId_))
                decoderId_ = decoder->id;
        else {
                bindingError_ = tr("Select a safety JSON first", "请先选择功能安全 JSON");
                return false;
        }
        const auto source = variables.find(manager);
        if (source == variables.end()) {
                bindingError_ = "变量管理器不可用";
                return false;
        }
        const auto fields = source->second->diagnosticFields(name);
        if (fields.size() != 1 || fields.front().path != name || !fields.front().supported) {
                bindingError_ = "请选择可读取的标量变量(整数,浮点数,布尔值或枚举)";
                return false;
        }
        bindings_[decoderId_] = {manager, name, "等待首次采样"};
        overview_             = false;
        manualInput_[0]       = 0;
        bindingError_.clear();
        return true;
}

bool
FaultDecoder::refresh(const Variables &variables)
{
        if (const auto *decoder = safety::findDecoder(profile_, decoderId_))
                decoderId_ = decoder->id;
        auto &binding = bindings_[decoderId_];
        if (binding.name.empty())
                return false;
        const auto         source = variables.find(binding.manager);
        diagnostics::Value sample;
        binding.valid = source != variables.end() && source->second->readDiagnosticValue(binding.name, sample, binding.status);
        if (source == variables.end())
                binding.status = "变量管理器已关闭";
        if (binding.valid) {
                binding.val      = std::move(sample);
                binding.hasValue = true;
        }
        return binding.valid;
}

void
FaultDecoder::save(cJSON *root) const
{
        auto *object = cJSON_AddObjectToObject(root, "faultDecoder");
        cJSON_AddBoolToObject(object, "open", open);
        cJSON_AddBoolToObject(object, "overview", overview_);
        cJSON_AddStringToObject(object, "groupTab", groupTab_.c_str());
        cJSON_AddStringToObject(object, "profile", profilePath_.empty() ? profileInput_ : profilePath_.c_str());
        cJSON_AddStringToObject(object, "directory", profileDirectory_.c_str());
        cJSON_AddStringToObject(object, "page", decoderId_.c_str());
        cJSON_AddBoolToObject(object, "nonzero", groupActiveOnly_);
        cJSON_AddBoolToObject(object, "diagnosticsOnly", diagnosticsOnly_);
        cJSON_AddStringToObject(object, "objectSearch", objectSearch_);
        cJSON_AddStringToObject(object, "fieldSearch", fieldSearch_);
        auto *bindings = cJSON_AddArrayToObject(object, "bindings");
        for (const auto &[id, binding] : bindings_) {
                if (binding.name.empty())
                        continue;
                auto *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "page", id.c_str());
                cJSON_AddStringToObject(item, "manager", binding.manager.c_str());
                cJSON_AddStringToObject(item, "name", binding.name.c_str());
                cJSON_AddItemToArray(bindings, item);
        }
        auto *roots = cJSON_AddArrayToObject(object, "roots");
        for (const auto &binding : roots_) {
                auto *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "id", binding.id.c_str());
                cJSON_AddStringToObject(item, "manager", binding.manager.c_str());
                cJSON_AddStringToObject(item, "root", binding.root.c_str());
                cJSON_AddItemToArray(roots, item);
        }
}

void
FaultDecoder::load(const cJSON *root)
{
        *this              = FaultDecoder{};
        const auto *object = cJSON_GetObjectItemCaseSensitive(root, "faultDecoder");
        if (!cJSON_IsObject(object))
                return;
        auto string = [](const cJSON *item, const char *key) -> std::string {
                const auto *val = cJSON_GetObjectItemCaseSensitive(item, key);
                return cJSON_IsString(val) ? val->valuestring : "";
        };
        groupTab_         = string(object, "groupTab");
        profileDirectory_ = string(object, "directory");
        const auto path   = string(object, "profile");
        // Older sessions saved the implicit bundled path even when no file was
        // selected. Only preserve explicit user selections across that migration.
        const bool legacyDefault = !cJSON_GetObjectItemCaseSensitive(object, "directory") && path == "config/safety.json";
        if (!path.empty() && !legacyDefault)
                loadProfile(path);
        open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "open"));
        if (const auto *mode = cJSON_GetObjectItemCaseSensitive(object, "overview"))
                overview_ = cJSON_IsTrue(mode);
        groupActiveOnly_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "nonzero"));
        diagnosticsOnly_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "diagnosticsOnly"));
        std::snprintf(objectSearch_, sizeof(objectSearch_), "%s", string(object, "objectSearch").c_str());
        std::snprintf(fieldSearch_, sizeof(fieldSearch_), "%s", string(object, "fieldSearch").c_str());
        if (!cJSON_GetObjectItemCaseSensitive(object, "diagnosticsOnly") && std::string(fieldSearch_) == "reason") {
                diagnosticsOnly_ = true;
                fieldSearch_[0]  = '\0';
        }
        const auto page = string(object, "page");
        if (const auto *decoder = safety::findDecoder(profile_, page))
                decoderId_ = decoder->id;
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, "bindings"))
        {
                const auto id = string(item, "page"), manager = string(item, "manager"), name = string(item, "name");
                if (!id.empty() && !manager.empty() && !name.empty())
                        if (const auto *decoder = safety::findDecoder(profile_, id))
                                if (!bindings_.contains(decoder->id) || id == decoder->id)
                                        bindings_[decoder->id] = {manager, name, "等待首次采样"};
        }
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, "roots"))
        {
                const auto id = string(item, "id"), manager = string(item, "manager"), name = string(item, "root");
                if (!id.empty() && !manager.empty() && !name.empty())
                        roots_.push_back({id, manager, name});
        }
        // 旧会话的固定对象仅迁移到当前 JSON 中仍存在的根路径
        if (roots_.empty())
                cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, "groups"))
                {
                        const auto manager = string(item, "manager"), name = string(item, "root");
                        if (manager.empty() || name.empty())
                                continue;
                        for (const auto &group : profile_.groups)
                                if (group.root == name)
                                        roots_.push_back({group.id, manager, name});
                }
}

void
FaultDecoder::inspect(unsigned wirePage, uint32_t val, unsigned)
{
        if (profilePath_.empty()) {
                open = focus_ = true;
                return;
        }
        const auto  id      = "ext" + std::to_string(wirePage + 1);
        const auto *decoder = safety::findDecoder(profile_, id);
        if (!decoder) {
                profileError_ = "当前 JSON 未定义解析器 " + id;
                open = focus_ = true;
                return;
        }
        decoderId_            = decoder->id;
        bindings_[decoderId_] = {};
        std::snprintf(manualInput_, sizeof(manualInput_), "0x%08X", val);
        overview_   = false;
        modeSelect_ = true;
        open = focus_ = true;
}

void
FaultDecoder::drawManual(const Variables &variables)
{
        if (profile_.decoders.empty()) {
                ImGui::TextWrapped("%s", tr("No decoders in this JSON.", "当前 JSON 没有解析器定义."));
                return;
        }
        const auto *decoder = safety::findDecoder(profile_, decoderId_);
        if (!decoder) {
                decoder    = &profile_.decoders.front();
                decoderId_ = decoder->id;
        } else
                decoderId_ = decoder->id;
        const auto title = decoder->title + " [" + decoder->id + "]";
        ImGui::SetNextItemWidth(std::min(600.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo(tr("Decoder", "解析规则"), title.c_str())) {
                for (const auto &candidate : profile_.decoders)
                        if (ImGui::Selectable((candidate.title + " [" + candidate.id + "]").c_str(),
                                              candidate.id == decoderId_)) {
                                decoderId_      = candidate.id;
                                manualInput_[0] = 0;
                                bindingError_.clear();
                        }
                ImGui::EndCombo();
        }
        decoder = safety::findDecoder(profile_, decoderId_);
        if (!decoder)
                return;
        auto &binding = bindings_[decoderId_];

        const auto source = variables.find(binding.manager);
        const auto sourceTitle =
            binding.name.empty()
                ? std::string(tr("Select from variable manager", "从变量管理器选择"))
                : (source == variables.end() ? binding.manager : source->second->getTitle()) + " / " + binding.name;
        ImGui::SetNextItemWidth(std::min(700.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo(tr("Variable###SafetySource", "变量###SafetySource"), sourceTitle.c_str())) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##safety_variable_search",
                                         tr("Search variable name", "搜索变量名"),
                                         variableSearch_,
                                         sizeof(variableSearch_));
                bool found = false;
                for (const auto &[key, manager] : variables) {
                        ImGui::PushID(key.c_str());
                        ImGui::SeparatorText(manager->getTitle().c_str());
                        std::vector<std::string> names;
                        for (const auto &variable : manager->vars_)
                                for (const auto &field : manager->diagnosticFields(variable.name))
                                        if (field.supported && field.path.find(variableSearch_) != std::string::npos)
                                                names.push_back(field.path);
                        std::sort(names.begin(), names.end());
                        names.erase(std::unique(names.begin(), names.end()), names.end());
                        for (const auto &name : names) {
                                found = true;
                                if (ImGui::Selectable(name.c_str(), binding.manager == key && binding.name == name))
                                        bind(variables, key, name);
                        }
                        ImGui::PopID();
                }
                if (!found)
                        ImGui::TextWrapped("%s", tr("No matching scalar", "没有匹配的标量变量"));
                ImGui::EndCombo();
        }
        if (!bindingError_.empty())
                ui::ErrorTextWrapped("%s", bindingError_.c_str());
        const bool         live = !binding.name.empty();
        diagnostics::Value val;
        bool               hasValue = false;
        if (live) {
                refresh(variables);
                ImGui::SameLine();
                if (ImGui::Button(tr("Unbind", "解除绑定"))) {
                        binding = {};
                        return;
                }
                ImGui::TextUnformatted(binding.name.c_str());
                ImGui::SameLine();
                if (!binding.valid)
                        ui::ErrorTextWrapped("%s", binding.status.c_str());
                else
                        ImGui::TextWrapped("%s", binding.status.c_str());
                if (!binding.valid && binding.hasValue)
                        ui::ErrorTextWrapped("%s", "以下为最后有效值,当前数据已失效.");
                val      = binding.val;
                hasValue = binding.hasValue;
        } else {
                ImGui::SetNextItemWidth(380);
                ImGui::InputTextWithHint(tr("Value", "数值"), "0x00000000 / -7 / 0.25", manualInput_, sizeof(manualInput_));
                std::string error;
                hasValue = safety::parseValue(manualInput_, *decoder, val, error);
                if (!hasValue) {
                        if (*manualInput_)
                                ui::ErrorTextWrapped("%s", error.c_str());
                        else
                                ImGui::TextWrapped("%s", tr("Enter a value.", "请输入数值."));
                }
        }
        if (!hasValue)
                return;
        const auto result = safety::describe(decoder, val);
        ImGui::Text("%s  |  %s  |  %s", result.val.c_str(), result.raw.c_str(), val.type.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Copy report", "复制解析报告"))) {
                const auto state  = !live ? "离线输入 / 快照\n" : binding.valid ? "当前读取有效\n" : "旧值 / 当前数据已失效\n";
                const auto report = std::string(state) + safety::report(live ? binding.name : decoderId_, result);
                ImGui::SetClipboardText(report.c_str());
        }
        safety_ui::drawResult(result, live && !binding.valid);
}

void
FaultDecoder::drawProfileTools()
{
        const auto        fileName = std::filesystem::u8path(profilePath_).filename().u8string();
        const std::string label    = std::string(tr("Configuration", "配置")) +
                                  (fileName.empty() ? "..." : "  " + std::string(fileName.begin(), fileName.end())) +
                                  "###safety_configuration";
        const float width    = std::min(ui::FlowToolbar::buttonWidth(label.c_str()), ImGui::GetContentRegionAvail().x);
        const bool  activate = ImGui::Button(label.c_str(), ImVec2(width, 0));
        TutorialGuide::instance().mark("safety.config");
        const auto trigger = ImGui::GetCurrentContext()->LastItemData;
        if (ui::BeginHoverPanel("##safety_profile_options",
                                tr("Safety configuration", "功能安全配置"),
                                profileHover_,
                                activate,
                                ImGui::GetFontSize() * 36,
                                ImGui::GetWindowViewport())) {
                ui::FlowToolbar tools;
                tools.next(tr("Open JSON...", "打开 JSON..."));
                if (ImGui::Button(tr("Open JSON...", "打开 JSON..."))) {
                        const auto path = nativeDlgOpen(tr("Safety definition", "功能安全配置"), {{"JSON", {"json"}}});
                        if (!path.empty())
                                loadProfile(path);
                }
                tools.next(tr("Select directory...", "选择目录..."));
                if (ImGui::Button(tr("Select directory...", "选择目录..."))) {
                        const auto path = nativeDlgPickDir(tr("Safety JSON directory", "功能安全 JSON 目录"));
                        if (!path.empty()) {
                                profileDirectory_ = path;
                                scanProfileDirectory();
                        }
                }
                tools.next(tr("Load / Reload", "加载 / 重载"));
                ImGui::BeginDisabled(!*profileInput_);
                if (ImGui::Button(tr("Load / Reload", "加载 / 重载")))
                        loadProfile(profileInput_);
                ImGui::EndDisabled();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##safety_profiles",
                                      fileName.empty() ? tr("Select safety JSON", "选择功能安全 JSON")
                                                       : std::string(fileName.begin(), fileName.end()).c_str())) {
                        if (ImGui::IsWindowAppearing())
                                scanProfileDirectory();
                        std::string chosen;
                        for (const auto &path : profileFiles_) {
                                const auto relative = std::filesystem::u8path(path)
                                                          .lexically_relative(std::filesystem::u8path(profileDirectory_))
                                                          .u8string();
                                if (ImGui::Selectable(std::string(relative.begin(), relative.end()).c_str(),
                                                      path == profilePath_))
                                        chosen = path;
                        }
                        if (profileFiles_.empty())
                                ImGui::TextDisabled("%s",
                                                    profileDirectory_.empty() ? tr("Select a directory", "请选择目录")
                                                                              : tr("No JSON files", "没有 JSON 文件"));
                        ImGui::EndCombo();
                        if (!chosen.empty())
                                loadProfile(chosen);
                }
                if (!directoryError_.empty())
                        ui::ErrorTextWrapped("%s", directoryError_.c_str());
                if (!profileDirectory_.empty())
                        ImGui::TextWrapped("%s", profileDirectory_.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##safetyProfile",
                                             tr("JSON file path", "JSON 文件路径"),
                                             profileInput_,
                                             sizeof(profileInput_),
                                             ImGuiInputTextFlags_EnterReturnsTrue))
                        loadProfile(profileInput_);
                ImGui::Separator();
                ImGui::TextUnformatted(profile_.name.c_str());

                ImGui::End();
        }
        ImGui::GetCurrentContext()->LastItemData = trigger;
}

void
FaultDecoder::draw(const Variables &variables)
{
        if (!open)
                return;
        ImGui::SetNextWindowSize(ImVec2(1160, 780), ImGuiCond_FirstUseEver);
        if (focus_) {
                ImGui::SetNextWindowFocus();
                focus_ = false;
        }
        if (!dock_close::begin(tr("Safety###FaultDecoder", "功能安全###FaultDecoder"), &open)) {
                ImGui::End();
                return;
        }
        drawProfileTools();
        ImGui::Separator();
        if (!profileError_.empty()) {
                ui::ErrorTextWrapped("%s", profileError_.c_str());
                if (!profilePath_.empty())
                        ImGui::TextWrapped("继续使用上一次有效配置:%s", profilePath_.c_str());
        }
        if (profilePath_.empty()) {
                ImGui::TextDisabled("%s", tr("Select a safety JSON", "请选择功能安全 JSON"));
                ImGui::End();
                return;
        }
        if (ImGui::BeginTabBar("##safety_mode")) {
                const bool selectOverview = overview_;
                if (ImGui::BeginTabItem(tr("System variables", "系统变量"),
                                        nullptr,
                                        modeSelect_ && selectOverview ? ImGuiTabItemFlags_SetSelected : 0)) {
                        overview_ = true;
                        drawGroups(variables);
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Single value / offline", "单值 / 离线解析"),
                                        nullptr,
                                        modeSelect_ && !selectOverview ? ImGuiTabItemFlags_SetSelected : 0)) {
                        overview_ = false;
                        drawManual(variables);
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
                modeSelect_ = false;
        }
        ImGui::End();
}
