#pragma once

#include "imgui.h"
#include <algorithm>

namespace ui
{
// Place controls on the same row while they fit, without adding an ID scope or
// changing the controls' native keyboard/focus behavior.
class FlowToolbar
{
        float left_, right_;
        bool  started_{};
        bool  group_{};

      public:
        FlowToolbar() : left_(ImGui::GetCursorScreenPos().x), right_(left_ + ImGui::GetContentRegionAvail().x) {}

        static float buttonWidth(const char *label)
        {
                return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
        }

        float next(float width)
        {
                width               = std::max(1.0f, std::min(width, right_ - left_));
                const float spacing = ImGui::GetStyle().ItemSpacing.x * (group_ ? 2.5f : 1.0f);
                if (started_ && ImGui::GetItemRectMax().x + spacing + width <= right_)
                        ImGui::SameLine(0, spacing);
                group_   = false;
                started_ = true;
                return width;
        }

        void next(const char *label) { next(buttonWidth(label)); }
        void group() { group_ = true; }

        float remaining() const
        {
                const float nextX = started_ ? ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x : left_;
                return std::max(0.0f, right_ - nextX);
        }

        void checkbox(const char *label)
        {
                next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x);
        }
};
} // namespace ui
