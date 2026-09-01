#pragma once
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>
#include <unordered_map>

namespace ui
{
inline void
TableCellHighlight(float amount, float contentHeight = 0.0f)
{
        auto *table = ImGui::GetCurrentTable();
        if (amount <= 0 || !table || table->CurrentColumn < 0)
                return;

        const auto  &style   = ImGui::GetStyle();
        const ImRect cell    = ImGui::TableGetCellBgRect(table, table->CurrentColumn);
        const float  insetX  = std::min(4.0f, style.FramePadding.x * .35f);
        const float  content = contentHeight > 0.0f ? contentHeight : ImGui::GetTextLineHeight();
        const float  row     = content + table->RowCellPaddingY * 2.0f;
        const ImVec2 minimum(cell.Min.x + insetX, cell.Min.y + 1.0f);
        const ImVec2 maximum(cell.Max.x - insetX, cell.Min.y + row - 1.0f);
        if (maximum.x <= minimum.x || maximum.y <= minimum.y)
                return;

        ImVec4 fill = style.Colors[ImGuiCol_CheckMark];
        fill.w      = .16f + .28f * amount;
        ImGui::GetWindowDrawList()->AddRectFilled(minimum, maximum, ImGui::GetColorU32(fill), style.FrameRounding);

        ImVec4 edge = fill;
        edge.w      = .24f + .36f * amount;
        ImGui::GetWindowDrawList()->AddRect(minimum, maximum, ImGui::GetColorU32(edge), style.FrameRounding, 0, 1.0f);
}

class ValueFlash
{
        struct Entry {
                std::string value;
                double      changed{-1}, seen{};
        };
        std::unordered_map<std::string, Entry> values_;
        double                                 cleanup_{};

      public:
        float update(const std::string &key, const std::string &value)
        {
                const double now = ImGui::GetTime();
                if (now < cleanup_ || now - cleanup_ > 10) {
                        std::erase_if(values_,
                                      [&](const auto &item) { return now < item.second.seen || now - item.second.seen > 30; });
                        cleanup_ = now;
                }
                const auto [it, inserted] = values_.try_emplace(key, Entry{value, -1, now});
                auto      &entry          = it->second;
                const bool valid = !value.empty() && value != "..." && value != "ERR" && value != "UNKNOWN" && value != "—";
                if (!inserted && entry.value != value) {
                        if (valid && entry.value != "..." && entry.value != "ERR" && entry.value != "—")
                                entry.changed = now;
                        entry.value = value;
                }
                entry.seen       = now;
                const double age = now - entry.changed;
                if (!valid || entry.changed < 0 || age < 0 || age >= 1.0)
                        return 0;
                return static_cast<float>((1.0 - age) * (.55 + .45 * (.5 + .5 * std::cos(now * 12.5663706))));
        }
        void cell(const std::string &key, const std::string &value) { TableCellHighlight(update(key, value)); }
};
} // namespace ui
