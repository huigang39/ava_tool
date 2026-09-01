#include "gui/fault_decoder.hpp"
#include "gui/flow_toolbar.hpp"
#include "gui/i18n.hpp"
#include "gui/search_results.hpp"
#include "gui/ui_theme.hpp"
#include "gui/variable.hpp"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace safety_ui
{
void drawResult(const safety::Result &, bool stale);
}
namespace
{
std::string
joinPath(const std::string &root, const std::string &relative)
{
        if (relative.empty())
                return root;
        return root + (relative.front() == '[' ? "" : ".") + relative;
}
std::string
relativePath(const std::string &root, const std::string &path)
{
        if (path == root)
                return "";
        if (path.starts_with(root + "."))
                return path.substr(root.size() + 1);
        if (path.starts_with(root + "["))
                return path.substr(root.size());
        return path;
}
bool
containsText(std::string text, std::string search)
{
        const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::transform(text.begin(), text.end(), text.begin(), lower);
        std::transform(search.begin(), search.end(), search.begin(), lower);
        return text.find(search) != std::string::npos;
}
} // namespace

bool
FaultDecoder::bindGroup(const Variables &variables, unsigned index, const std::string &manager, const std::string &root)
{
        const auto source = variables.find(manager);
        if (index >= profile_.groups.size() || source == variables.end() || source->second->diagnosticFields(root).empty())
                return false;
        const auto &definition = profile_.groups[index];
        auto        binding    = std::find_if(roots_.begin(), roots_.end(), [&](const auto &item) {
                return item.id == definition.id && item.manager == manager;
        });
        if (binding == roots_.end())
                roots_.push_back({definition.id, manager, root});
        else
                binding->root = root;
        groups_.erase(std::remove_if(groups_.begin(),
                                     groups_.end(),
                                     [&](const auto &item) { return item.id == definition.id && item.manager == manager; }),
                      groups_.end());
        overview_ = true;
        detectGroups(variables);
        return true;
}

void
FaultDecoder::rebuildGroup(Group &group, Variable &source, const safety::Group &definition)
{
        auto discovered = definition.expand ? source.diagnosticFields(group.root) : std::vector<diagnostics::Field>{};
        for (const auto &spec : definition.fields) {
                const auto path = joinPath(group.root, spec.path);
                if (std::any_of(discovered.begin(), discovered.end(), [&](const auto &field) { return field.path == path; }))
                        continue;
                const auto fields = source.diagnosticFields(path);
                const auto exact =
                    std::find_if(fields.begin(), fields.end(), [&](const auto &field) { return field.path == path; });
                if (exact != fields.end())
                        discovered.push_back(*exact);
                else
                        discovered.push_back({path, "", "当前来源缺少此字段或此路径不是标量", false});
        }
        // JSON 指定的常用字段优先, 其余字段保持 DWARF 展开顺序
        std::unordered_map<std::string, size_t> order;
        for (size_t i = 0; i < definition.fields.size(); ++i)
                order.emplace(joinPath(group.root, definition.fields[i].path), i);
        const auto rank = [&](const diagnostics::Field &field) {
                const auto found = order.find(field.path);
                return found == order.end() ? definition.fields.size() : found->second;
        };
        std::stable_sort(discovered.begin(), discovered.end(), [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
        const bool newLayout = group.layoutGeneration != source.diagnosticLayoutGeneration();
        auto       previous  = std::move(group.rows);
        group.rows.clear();
        for (const auto &field : discovered) {
                GroupRow   row;
                const auto old = std::find_if(
                    previous.begin(), previous.end(), [&](const auto &item) { return item.field.path == field.path; });
                if (!newLayout && old != previous.end() && (old->field.type == field.type || !field.supported))
                        row = std::move(*old);
                row.field = field;
                row.title = relativePath(group.root, field.path);
                if (row.title.empty())
                        row.title = group.root;
                row.decoder.clear();
                row.note.clear();
                row.unit.clear();
                if (const auto *spec = safety::findField(definition, relativePath(group.root, field.path))) {
                        if (!spec->title.empty())
                                row.title = spec->title;
                        row.decoder = spec->decoder;
                        row.note    = spec->note;
                        row.unit    = spec->unit;
                }
                if (!field.supported) {
                        row.valid  = false;
                        row.status = field.status;
                }
                group.rows.push_back(std::move(row));
        }
        // 已删除字段保留最后采样并标记失效, 重载配置时清除
        if (!newLayout)
                for (auto &old : previous)
                        if (!old.field.path.empty() && old.hasValue &&
                            !std::any_of(group.rows.begin(), group.rows.end(), [&](const auto &row) {
                                    return row.field.path == old.field.path;
                            })) {
                                old.field.supported = old.valid = false;
                                old.field.status = old.status = "字段已从当前来源移除";
                                group.rows.push_back(std::move(old));
                        }
        group.layoutGeneration = source.diagnosticLayoutGeneration();
}

void
FaultDecoder::detectGroups(const Variables &variables)
{
        for (auto &group : groups_)
                group.available = false;
        for (const auto &[manager, source] : variables)
                for (const auto &definition : profile_.groups) {
                        const auto  binding = std::find_if(roots_.begin(), roots_.end(), [&](const auto &item) {
                                return item.id == definition.id && item.manager == manager;
                        });
                        const auto &root    = binding == roots_.end() ? definition.root : binding->root;
                        const auto  fields  = source->diagnosticFields(root);
                        auto        found   = std::find_if(groups_.begin(), groups_.end(), [&](const auto &item) {
                                return item.id == definition.id && item.manager == manager && item.root == root;
                        });
                        if (fields.empty() && found == groups_.end() && binding == roots_.end())
                                continue;
                        if (found == groups_.end()) {
                                groups_.push_back({});
                                found          = std::prev(groups_.end());
                                found->id      = definition.id;
                                found->manager = manager;
                                found->root    = root;
                                found->title   = definition.title.empty() ? root : definition.title;
                                found->note    = definition.note;
                        }
                        found->available = !fields.empty();
                        rebuildGroup(*found, *source, definition);
                }
        std::stable_sort(groups_.begin(), groups_.end(), [&](const auto &a, const auto &b) {
                if (a.manager != b.manager)
                        return a.manager < b.manager;
                const auto ordinal = [&](const std::string &id) {
                        return std::find_if(profile_.groups.begin(),
                                            profile_.groups.end(),
                                            [&](const auto &item) { return item.id == id; }) -
                               profile_.groups.begin();
                };
                return ordinal(a.id) < ordinal(b.id);
        });
}

void
FaultDecoder::refreshGroups(const Variables &variables)
{
        if (groups_.empty())
                detectGroups(variables);
        for (auto &group : groups_) {
                const auto source     = variables.find(group.manager);
                const auto definition = std::find_if(
                    profile_.groups.begin(), profile_.groups.end(), [&](const auto &item) { return item.id == group.id; });
                auto invalidate = [&](const std::string &status) {
                        group.status = status;
                        for (auto &row : group.rows) {
                                row.valid  = false;
                                row.status = status;
                        }
                };
                if (source == variables.end()) {
                        invalidate("变量管理器已关闭,数据失效");
                        continue;
                }
                if (definition == profile_.groups.end()) {
                        invalidate("当前 JSON 未定义此对象");
                        continue;
                }
                if (group.layoutGeneration != source->second->diagnosticLayoutGeneration())
                        rebuildGroup(group, *source->second, *definition);
                if (std::none_of(group.rows.begin(), group.rows.end(), [](const auto &row) { return row.requested; })) {
                        invalidate("未选中或已筛选, 暂停采样");
                        continue;
                }
                if (!definition->when.path.empty()) {
                        diagnostics::Value condition;
                        std::string        status;
                        if (!source->second->readDiagnosticValue(definition->when.path, condition, status)) {
                                invalidate("等待条件变量 " + definition->when.path + ":" + status);
                                continue;
                        }
                        if (!safety::matches(definition->when, condition)) {
                                invalidate("当前条件不匹配,未读取此对象");
                                continue;
                        }
                }
                unsigned valid     = 0;
                unsigned requested = 0;
                for (auto &row : group.rows) {
                        if (!row.requested) {
                                row.valid  = false;
                                row.status = "已筛选, 暂停采样";
                                continue;
                        }
                        ++requested;
                        if (!row.field.supported) {
                                row.valid  = false;
                                row.status = row.field.status;
                                continue;
                        }
                        diagnostics::Value sample;
                        row.valid = source->second->readDiagnosticValue(row.field.path, sample, row.status);
                        if (row.valid) {
                                row.val      = std::move(sample);
                                row.hasValue = true;
                                ++valid;
                        }
                }
                group.status = "有效 " + std::to_string(valid) + " / " + std::to_string(requested) + " 项";
        }
}

std::string
FaultDecoder::groupReport(const Group &group, const GroupRow *only) const
{
        std::string text = group.title + " / " + group.manager + " / " + group.root + "\n" + group.status + "\n";
        if (!group.note.empty())
                text += group.note + "\n";
        for (const auto &row : group.rows) {
                if (only && &row != only)
                        continue;
                text += "\n" + row.title + " [" + row.field.path + "]\n";
                text += row.valid      ? "当前读取有效\n"
                        : row.hasValue ? "旧值 / 当前数据已失效:" + row.status + "\n"
                                       : "未读取:" + row.status + "\n";
                if (!row.note.empty())
                        text += row.note + "\n";
                if (row.hasValue) {
                        text += "类型:" + row.val.type + (row.unit.empty() ? "" : " / 单位:" + row.unit) + "\n";
                        text += safety::report(row.field.path,
                                               safety::describe(safety::findDecoder(profile_, row.decoder), row.val));
                }
        }
        return text;
}

void
FaultDecoder::drawGroups(const Variables &variables)
{
        if (ImGui::GetTime() >= nextDetection_) {
                detectGroups(variables);
                nextDetection_ = ImGui::GetTime() + 1.0;
        }
        refreshGroups(variables);
        ui::FlowToolbar actions;
        actions.next(tr("Refresh objects", "重新识别对象"));
        if (ImGui::Button(tr("Refresh objects", "重新识别对象"))) {
                detectGroups(variables);
                refreshGroups(variables);
        }
        actions.checkbox(tr("Nonzero / unavailable only", "只看非零 / 读取异常"));
        ImGui::Checkbox(tr("Nonzero / unavailable only", "只看非零 / 读取异常"), &groupActiveOnly_);
        actions.group();
        actions.next(tr("Copy all", "复制全部报告"));
        if (ImGui::Button(tr("Copy all", "复制全部报告"))) {
                std::string report = profile_.name + "\n配置:" + profilePath_ + "\n非零值不等同于故障.\n";
                for (const auto &group : groups_)
                        report += "\n" + groupReport(group);
                ImGui::SetClipboardText(report.c_str());
        }
        actions.next(tr("Copy selected", "复制选中字段"));
        ImGui::BeginDisabled(selectedRow_.empty());
        if (ImGui::Button(tr("Copy selected", "复制选中字段"))) {
                for (const auto &group : groups_)
                        for (const auto &row : group.rows)
                                if (selectedRow_ == group.manager + "\n" + group.id + "\n" + row.field.path)
                                        ImGui::SetClipboardText(groupReport(group, &row).c_str());
        }
        ImGui::EndDisabled();
        actions.group();
        actions.next(tr("Status and errors", "状态与错误"));
        if (ImGui::Button(tr("Status and errors", "状态与错误"))) {
                fieldSearch_[0]  = '\0';
                diagnosticsOnly_ = true;
                objectSearch_[0] = '\0';
                groupActiveOnly_ = false;
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s",
                    tr("Show enum states and error bitmaps defined by the safety profile, including historical records.",
                       "按 JSON 解释规则集中查看状态和错误码, 包括历史记录; 非零状态不等于故障."));
        actions.next(tr("All fields", "全部字段"));
        if (ImGui::Button(tr("All fields", "全部字段"))) {
                fieldSearch_[0] = objectSearch_[0] = '\0';
                diagnosticsOnly_                   = false;
                groupActiveOnly_                   = false;
        }
        ui::FlowToolbar searches;
        const char     *objectHint    = tr("Search object / manager", "搜索对象 / 变量管理器");
        const char     *fieldHint     = tr("Search field / explanation", "搜索字段 / 说明");
        const float     minimumObject = ui::FlowToolbar::buttonWidth(objectHint);
        const float minimumSearches = minimumObject + ui::FlowToolbar::buttonWidth(fieldHint) + ImGui::GetStyle().ItemSpacing.x;
        const bool  inlineSearch    = actions.remaining() >= minimumSearches;
        auto       &searchLayout    = inlineSearch ? actions : searches;
        const float searchWidth     = inlineSearch ? actions.remaining() : ImGui::GetContentRegionAvail().x;
        const bool  sideBySide      = inlineSearch || searchWidth >= std::max(minimumSearches, ImGui::GetFontSize() * 26);
        const float objectWidth =
            sideBySide ? std::max(minimumObject, (searchWidth - ImGui::GetStyle().ItemSpacing.x) * .40f) : searchWidth;
        ImGui::SetNextItemWidth(searchLayout.next(objectWidth));
        ui::SearchInput("##safetyObjectSearch", objectHint, objectSearch_, sizeof(objectSearch_));
        ImGui::SetNextItemWidth(
            searchLayout.next(sideBySide ? searchWidth - objectWidth - ImGui::GetStyle().ItemSpacing.x : searchWidth));
        ui::SearchInput("##safetyFieldSearch", fieldHint, fieldSearch_, sizeof(fieldSearch_));
        for (const char *searchId : {"##safetyObjectSearch", "##safetyFieldSearch"}) {
                if (!ui::SearchOpen(searchId))
                        continue;
                const bool                                       fields = std::string(searchId) == "##safetyFieldSearch";
                std::vector<ui::SearchResult>                    results;
                std::vector<std::pair<std::string, std::string>> targets;
                for (const auto &group : groups_) {
                        const auto id = group.manager + "\n" + group.id + "\n" + group.root;
                        if (!fields && containsText(group.title + " " + group.root + " " + group.manager, objectSearch_)) {
                                results.push_back({group.title + " / " + group.root, group.manager});
                                targets.emplace_back(id, "");
                        }
                        if (fields)
                                for (const auto &row : group.rows)
                                        if (containsText(row.title + " " + row.field.path + " " + row.note + " " + row.decoder,
                                                         fieldSearch_)) {
                                                results.push_back({group.title + " / " + row.title, row.field.path});
                                                targets.emplace_back(id,
                                                                     group.manager + "\n" + group.id + "\n" + row.field.path);
                                        }
                }
                const int selected = ui::SearchResults(searchId, results);
                if (selected >= 0) {
                        groupTab_        = targets[selected].first;
                        selectedRow_     = targets[selected].second;
                        groupTabSelect_  = true;
                        searchScroll_    = !selectedRow_.empty();
                        objectSearch_[0] = fieldSearch_[0] = 0;
                        groupActiveOnly_ = diagnosticsOnly_ = false;
                }
        }
        if (groups_.empty()) {
                ImGui::TextWrapped("%s", "尚未找到 JSON 指定的对象.加载对应 ELF,或在变量管理器创建相同路径的 LOCAL 变量.");
                if (ImGui::TreeNode(tr("Configured roots", "配置中的对象路径"))) {
                        for (const auto &group : profile_.groups)
                                ImGui::BulletText("%s / %s", group.title.c_str(), group.root.c_str());
                        ImGui::TreePop();
                }
                return;
        }
        for (auto &group : groups_)
                for (auto &row : group.rows)
                        row.requested = false;
        if (!ImGui::BeginTabBar("##safety_objects", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
                return;
        unsigned          matchingGroups = 0;
        const std::string requestedGroup = groupTab_;
        bool              restoredGroup  = false;
        for (auto &group : groups_) {

                const auto matchesFilter = [&](const auto &row) {
                        const auto *decoder = safety::findDecoder(profile_, row.decoder);
                        if (diagnosticsOnly_ && (!decoder || decoder->kind == "number"))
                                return false;
                        return true;
                };
                const auto visible = [&](const auto &row) {
                        return matchesFilter(row) && !(groupActiveOnly_ && row.valid && diagnostics::isZero(row.val));
                };
                const auto count = std::count_if(group.rows.begin(), group.rows.end(), visible);
                // 零值只隐藏行, 不能停止采样, 否则有效与暂停状态会逐帧交替
                if (!std::any_of(group.rows.begin(), group.rows.end(), matchesFilter))
                        continue;
                const auto id = group.manager + "\n" + group.id + "\n" + group.root;
                ImGui::PushID(id.c_str());
                ++matchingGroups;
                const auto label     = group.title + "###Object";
                const bool restore   = groupTabSelect_ && id == requestedGroup;
                restoredGroup       |= restore;
                const bool selected  = ImGui::BeginTabItem(label.c_str(), nullptr, restore ? ImGuiTabItemFlags_SetSelected : 0);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s / %s\n%s", group.manager.c_str(), group.root.c_str(), group.note.c_str());
                if (!selected) {
                        ImGui::PopID();
                        continue;
                }
                groupTab_ = id;
                ImGui::BeginChild("safetyObjectFields", ImVec2(0, 0));
                // 非零筛选仍需采样零值, 才能及时发现新故障
                for (auto &row : group.rows)
                        row.requested = matchesFilter(row);
                ImGui::TextWrapped("%s  %s  %s  %zu / %zu 个字段",
                                   group.manager.c_str(),
                                   group.root.c_str(),
                                   group.status.c_str(),
                                   size_t(count),
                                   group.rows.size());
                if (!group.note.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", group.note.c_str());
                const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;
                if (ImGui::BeginTable("fields", 5, flags)) {
                        ImGui::TableSetupColumn("字段", ImGuiTableColumnFlags_WidthStretch, .28f);
                        ImGui::TableSetupColumn("数值 / 原始位", ImGuiTableColumnFlags_WidthStretch, .18f);
                        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, .10f);
                        ImGui::TableSetupColumn("读取状态", ImGuiTableColumnFlags_WidthStretch, .12f);
                        ImGui::TableSetupColumn("含义 / 排查 / 清除条件", ImGuiTableColumnFlags_WidthStretch, .32f);
                        ImGui::TableHeadersRow();
                        for (const auto &row : group.rows) {
                                if (!visible(row))
                                        continue;
                                ImGui::PushID(row.field.path.c_str());
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                const auto key = group.manager + "\n" + group.id + "\n" + row.field.path;
                                if (ImGui::Selectable(row.title.c_str(), selectedRow_ == key))
                                        selectedRow_ = key;
                                if (searchScroll_ && selectedRow_ == key) {
                                        ImGui::SetScrollHereY(0.5f);
                                        searchScroll_ = false;
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", row.field.path.c_str());
                                ImGui::TableNextColumn();
                                if (row.hasValue) {
                                        ImGui::TextWrapped("%s%s%s",
                                                           diagnostics::formatValue(row.val).c_str(),
                                                           row.unit.empty() ? "" : " ",
                                                           row.unit.c_str());
                                        ImGui::TextDisabled("%s", diagnostics::formatBits(row.val).c_str());
                                } else
                                        ImGui::TextDisabled("-");
                                ImGui::TableNextColumn();
                                ImGui::TextWrapped("%s", row.hasValue ? row.val.type.c_str() : row.field.type.c_str());
                                ImGui::TableNextColumn();
                                if (row.valid)
                                        ui::TextColored(ImVec4(.4f, .8f, .6f, 1), "有效");
                                else if (row.status.empty())
                                        ImGui::TextDisabled("%s", "等待刷新");
                                else
                                        ui::ErrorTextWrapped("%s", row.status.c_str());
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", row.status.c_str());
                                ImGui::TableNextColumn();
                                if (!row.note.empty())
                                        ImGui::TextWrapped("%s", row.note.c_str());
                                if (!row.valid)
                                        ui::ErrorTextWrapped("%s", row.status.c_str());
                                if (row.hasValue)
                                        safety_ui::drawResult(
                                            safety::describe(safety::findDecoder(profile_, row.decoder), row.val), !row.valid);
                                ImGui::PopID();
                        }
                        ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
                ImGui::PopID();
        }
        ImGui::EndTabBar();
        if (restoredGroup)
                groupTab_ = requestedGroup;
        groupTabSelect_ = false;
        if (!matchingGroups)
                ImGui::TextDisabled("%s", tr("No matching objects or fields.", "没有匹配的对象或字段。"));
}
