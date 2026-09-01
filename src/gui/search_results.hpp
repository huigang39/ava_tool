#pragma once
#include "gui/i18n.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <vector>

namespace ui
{
// Shared inline result list: typing opens it, selecting a result returns to the content.
struct SearchResult {
        std::string label, detail;
};
inline bool
SearchInput(const char *id, const char *hint, char *text, size_t size)
{
        const bool changed = ImGui::InputTextWithHint(id, hint, text, size);
        auto      *storage = ImGui::GetStateStorage();
        const auto key     = ImGui::GetID(id);
        if (ImGui::IsItemActivated() || ImGui::IsItemEdited())
                storage->SetBool(key, true);
        if (ImGui::IsItemDeactivated() && !text[0])
                storage->SetBool(key, false);
        return changed;
}
inline bool
SearchOpen(const char *id)
{
        return ImGui::GetStateStorage()->GetBool(ImGui::GetID(id));
}
inline int
SearchResults(const char *id, const std::vector<SearchResult> &items)
{
        if (!SearchOpen(id))
                return -1;
        auto      *storage  = ImGui::GetStateStorage();
        const auto key      = ImGui::GetID(id);
        int        selected = -1;
        ImGui::PushID(id);
        if (ImGui::BeginChild("results", ImVec2(0, ImGui::GetFontSize() * 10), ImGuiChildFlags_Borders)) {
                if (items.empty())
                        ImGui::TextDisabled("%s", tr("No matching results", "没有匹配结果"));
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(items.size()));
                while (clipper.Step())
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                                ImGui::PushID(i);
                                if (ImGui::Selectable(items[i].label.c_str(), false, ImGuiSelectableFlags_SelectOnClick))
                                        selected = i;
                                if (ImGui::IsItemHovered() && !items[i].detail.empty())
                                        ImGui::SetTooltip("%s", items[i].detail.c_str());
                                ImGui::PopID();
                        }
        }
        ImGui::EndChild();
        ImGui::PopID();
        if (selected >= 0)
                storage->SetBool(key, false);
        return selected;
}
} // namespace ui
