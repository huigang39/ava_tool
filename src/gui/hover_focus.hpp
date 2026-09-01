#pragma once
#include "gui/design_tokens.hpp"
#include "gui/i18n.hpp"
#include "gui/interaction_history.hpp"
#include "imgui_internal.h"
#include <optional>
#include <string>

namespace ui
{
// Hover temporarily changes the view. The original trigger area only tracks
// pointer intent; it must never paint a second button over the focused content.
template <typename State> class HoverFocus
{
        std::optional<State> before_;
        ImRect               anchor_;
        ImGuiID              trigger_{}, window_{}, root_{};
        double               entered_{};
        int                  frame_{-1};
        float                scroll_{};
        std::string          candidate_;

      public:
        bool active() const { return before_.has_value(); }
        void commit()
        {
                before_.reset();
                candidate_.clear();
        }

        // Call before rendering the owner. Returns true only on restoration.
        bool maintain(State &state)
        {
                if (!before_)
                        return false;
                auto      &g        = *ImGui::GetCurrentContext();
                auto      *hovered  = g.HoveredWindow;
                const bool sameRoot = hovered && hovered->RootWindow->ID == root_;
                const bool escape   = !interaction::escapeConsumed && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                if (frame_ != g.FrameCount - 1 || !anchor_.Contains(g.IO.MousePos) || !sameRoot || g.IO.AppFocusLost ||
                    g.DragDropActive || !g.OpenPopupStack.empty() || escape) {
                        state = *before_;
                        if (auto *window = ImGui::FindWindowByID(window_))
                                ImGui::SetScrollY(window, scroll_);
                        if (escape)
                                interaction::consumeEscape();
                        commit();
                        return true;
                }
                frame_ = g.FrameCount;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        // Consume the original focus click before the new table
                        // can treat it as an edit. A separate owner avoids both
                        // duplicate widget IDs and a click on the Restore action.
                        const ImGuiID owner = ImHashStr("##focusPreviewCommit", 0, trigger_);
                        ImGui::SetKeyOwner(ImGuiKey_MouseLeft, owner, ImGuiInputFlags_LockUntilRelease);
                        commit();
                }
                return false;
        }

        bool consider(const std::string &key, State &state, const State &preview, ImGuiWindow *scrollOwner = nullptr)
        {
                if (before_)
                        return false;
                auto &g = *ImGui::GetCurrentContext();
                if (!ImGui::IsItemHovered() || g.IO.AppFocusLost || g.DragDropActive || ImGui::IsAnyItemActive() ||
                    !g.OpenPopupStack.empty() || ImGui::IsMouseDown(0))
                        return false;
                if (candidate_ != key || frame_ < g.FrameCount - 1) {
                        candidate_ = key;
                        entered_   = ImGui::GetTime();
                }
                frame_ = g.FrameCount;
                if (ImGui::GetTime() - entered_ < .3)
                        return false;
                before_      = state;
                anchor_      = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                trigger_     = g.LastItemData.ID;
                auto *window = ImGui::GetCurrentWindow();
                window_      = scrollOwner ? scrollOwner->ID : window->ID;
                root_        = window->RootWindow->ID;
                scroll_      = scrollOwner ? scrollOwner->Scroll.y : window->Scroll.y;
                state        = preview;
                return true;
        }
};
} // namespace ui
