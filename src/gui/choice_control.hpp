#pragma once

#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace ui
{
inline float
ChoiceWidth(const char *const items[], int count)
{
        float width = 0;
        for (int i = 0; i < count; ++i)
                width += ImGui::CalcTextSize(items[i], nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
        return width + std::max(0, count - 1) * 2.0f;
}

// Fixed mutually exclusive options use the same compact, keyboard-accessible
// segments everywhere. Wrap only when the containing panel or table cell ends.
inline bool
Choice(const char       *label,
       int              *selected,
       const char *const items[],
       int               count,
       const bool       *enabled       = nullptr,
       bool              allowReselect = false)
{
        if (count <= 0 || ImGui::GetCurrentWindow()->SkipItems)
                return false;
        auto &g                  = *ImGui::GetCurrentContext();
        g.NextItemData.HasFlags &= ~ImGuiNextItemDataFlags_HasWidth;
        const auto &style        = ImGui::GetStyle();
        const float left         = ImGui::GetCursorScreenPos().x;
        float       available    = ImGui::GetContentRegionAvail().x;
        // A newly opened auto-sized popup has not measured its contents yet.
        if (ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_AlwaysAutoResize)
                available = std::max(available, std::min(ChoiceWidth(items, count), ImGui::GetFontSize() * 38));
        available            = std::max(1.0f, available);
        const float right    = left + available;
        const char *labelEnd = ImGui::FindRenderedTextEnd(label);
        bool        changed  = false;
        ImGui::PushID(label);
        ImGui::BeginGroup();
        if (labelEnd != label) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label, labelEnd);
                if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + ChoiceWidth(items, count) <= right)
                        ImGui::SameLine();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, style.ItemSpacing.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, style.FrameRounding);
        for (int i = 0; i < count; ++i) {
                const float natural = ImGui::CalcTextSize(items[i], nullptr, true).x + style.FramePadding.x * 2;
                const float width   = std::min(natural, available);
                if (i && ImGui::GetItemRectMax().x + 2 + width <= right)
                        ImGui::SameLine(0, 2);
                const bool active = *selected == i;
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[active ? ImGuiCol_Header : ImGuiCol_FrameBg]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_HeaderHovered]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_HeaderActive]);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? design::Stroke() : 0.0f);
                ImGui::BeginDisabled(enabled && !enabled[i]);
                if (ImGui::Button(items[i], ImVec2(width, 0)) && (!active || allowReselect)) {
                        *selected = i;
                        changed   = true;
                }
                if (width < natural && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", items[i]);
                ImGui::EndDisabled();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::PopID();
        }
        ImGui::PopStyleVar(2);
        ImGui::EndGroup();
        ImGui::PopID();
        return changed;
}

inline bool
Choice(const char *label, int *selected, const char *items)
{
        std::vector<const char *> labels;
        for (const char *item = items; *item; item += std::strlen(item) + 1)
                labels.push_back(item);
        return Choice(label, selected, labels.data(), static_cast<int>(labels.size()));
}

// Enum-like settings use one compact combo field. A solid accent blend makes
// the current value unambiguous even when the pointer is not over the popup.
inline bool
EnumCombo(const char       *label,
          int              *selected,
          const char *const items[],
          int               count,
          const bool       *enabled       = nullptr,
          bool              allowReselect = false)
{
        if (!selected || !items || count <= 0 || ImGui::GetCurrentWindow()->SkipItems)
                return false;
        const int   current = std::clamp(*selected, 0, count - 1);
        const char *preview = current < count && items[current] ? items[current] : "";
        if (!ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge))
                return false;

        const auto &style        = ImGui::GetStyle();
        const auto  accent       = style.Colors[ImGuiCol_CheckMark];
        const auto  popup        = style.Colors[ImGuiCol_PopupBg];
        const auto  selectedFill = MixSurface(popup, accent, .18f);
        ImGui::PushStyleColor(ImGuiCol_Header, selectedFill);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MixSurface(selectedFill, ImVec4(1, 1, 1, 1), .08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, MixSurface(selectedFill, ImVec4(0, 0, 0, 1), .08f));
        bool changed = false;
        for (int i = 0; i < count; ++i) {
                const bool active = i == current;
                ImGui::PushID(i);
                ImGui::BeginDisabled(enabled && !enabled[i]);
                if (ImGui::Selectable(items[i] ? items[i] : "", active, ImGuiSelectableFlags_SpanAvailWidth) &&
                    (!active || allowReselect)) {
                        *selected = i;
                        changed   = true;
                }
                if (active) {
                        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
                                                            ImGui::GetItemRectMax(),
                                                            ImGui::GetColorU32(accent),
                                                            style.FrameRounding,
                                                            0,
                                                            design::Stroke());
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndCombo();
        return changed;
}

inline bool
EnumCombo(const char *label, int *selected, const char *items)
{
        std::vector<const char *> labels;
        for (const char *item = items; *item; item += std::strlen(item) + 1)
                labels.push_back(item);
        return EnumCombo(label, selected, labels.data(), static_cast<int>(labels.size()));
}

inline bool
AccessChoice(const char *id, bool &writable, bool canWrite = true)
{
        const char *labels[]  = {tr("Read only", "只读"), tr("Read / write", "读写")};
        const bool  enabled[] = {true, canWrite};
        int         value     = writable ? 1 : 0;
        // An explicit access selection also applies to other selected rows or
        // creates a member override when its current inherited value matches.
        if (!Choice(id, &value, labels, 2, enabled, true))
                return false;
        writable = value == 1;
        return true;
}
} // namespace ui
