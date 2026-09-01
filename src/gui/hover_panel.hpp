#ifndef AVA_HOVER_PANEL_HPP
#define AVA_HOVER_PANEL_HPP

#include "gui/design_tokens.hpp"
#include "gui/hover_reveal.hpp"
#include "gui/i18n.hpp"
#include "gui/interaction_history.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace ui
{
// Click-open utility windows. Recorder statistics and focus own their explicit
// hover previews separately; settings never open merely because the pointer passes.
inline bool
BeginHoverPanel(const char *id, const char *title, HoverReveal &state, bool activated, float width, ImGuiViewport *viewport)
{
        auto        &g = *ImGui::GetCurrentContext();
        const ImRect trigger(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        auto        *panel = ImGui::FindWindowByName(id);
        auto belongs = [&](ImGuiWindow *window) { return window && panel && ImGui::IsWindowWithinBeginStackOf(window, panel); };
        const bool overTrigger = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool overPanel   = panel && panel->WasActive && belongs(g.HoveredWindow);
        bool       childPopup = false, otherPopup = false;
        for (const auto &popup : g.OpenPopupStack) {
                if (belongs(popup.Window))
                        childPopup = true;
                else
                        otherPopup = true;
        }
        const bool activeEditor = belongs(g.ActiveIdWindow) && g.ActiveId != 0;
        const bool holding      = activeEditor || childPopup;
        // Inputs and child menus receive the first Escape. Only a plain panel
        // dismissal consumes Escape before application parent navigation.
        const bool dismiss = state.isOpen() && !holding && !interaction::activeAtFrameStart &&
                             !interaction::popupAtFrameStart && !otherPopup && !interaction::escapeConsumed &&
                             ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                             (!state.isPinned() || overTrigger || overPanel || belongs(g.NavWindow));
        if (dismiss)
                interaction::consumeEscape();
        // Include child windows and menus in the panel's hit region. An active
        // editor may hold Escape, but must not prevent an intentional outside click.
        const bool clickedOutside =
            state.isOpen() && panel && panel->WasActive && !overTrigger && !overPanel && !ImGui::GetTopMostPopupModal() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right));
        if (activated) {
                if (state.isOpen())
                        state.close();
                else
                        state.setPinned(true);
        }
        HoverReveal::Input input;
        input.now            = ImGui::GetTime();
        input.triggerHovered = overTrigger;
        input.panelHovered   = overPanel;
        input.holdOpen       = holding;
        input.activate       = false;
        input.dismiss        = dismiss;
        input.allowOpen      = false;
        if (!state.update(input) || g.IO.AppFocusLost || (ImGui::GetTopMostPopupModal() && !childPopup))
                return false;
        if (viewport->WorkSize.x <= 0 || viewport->WorkSize.y <= 0)
                return false;
        const auto &style     = ImGui::GetStyle();
        const float maxWidth  = std::max(1.0f, viewport->WorkSize.x - 12.0f);
        const float maxHeight = std::max(1.0f, viewport->WorkSize.y - 12.0f);
        width                 = std::clamp(width, std::min(maxWidth, ImGui::GetFontSize() * 12), maxWidth);
        if (state.isPinned() && trigger.GetWidth() > 0 && trigger.GetHeight() > 0) {
                auto  *draw = ImGui::GetForegroundDrawList(viewport);
                ImVec4 fill = style.Colors[ImGuiCol_CheckMark];
                fill.w      = .10f;
                draw->AddRectFilled(trigger.Min, trigger.Max, ImGui::GetColorU32(fill), style.FrameRounding);
                ImVec4 edge = style.Colors[ImGuiCol_CheckMark];
                edge.w      = .60f;
                draw->AddRect(trigger.Min, trigger.Max, ImGui::GetColorU32(edge), style.FrameRounding, 0, design::Stroke());
        }
        const float  height      = panel ? std::min(panel->Size.y, viewport->WorkSize.y) : 0.0f;
        const float  anchorWidth = panel ? std::min(panel->Size.x, maxWidth) : width;
        const ImVec2 anchor(
            std::clamp(trigger.Min.x, viewport->WorkPos.x, viewport->WorkPos.x + viewport->WorkSize.x - anchorWidth),
            std::clamp(
                trigger.Max.y + style.ItemSpacing.y, viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - height));
        ImGui::SetNextWindowViewport(viewport->ID);
        if (!state.isPinned() || !state.isDetached() || !panel || !panel->WasActive)
                ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
        const float headerMinimum = style.WindowPadding.x * 2 + ImGui::GetFontSize() * 8;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(maxWidth, headerMinimum), std::min(maxHeight, ImGui::GetFrameHeight() * 5)),
            ImVec2(maxWidth, maxHeight));
        ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_FirstUseEver);
        if (activated && state.isPinned())
                ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_PopupBg]);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style.PopupRounding);
        const ImVec2 beforeSize = panel ? panel->SizeFull : ImVec2(width, 0);
        const ImVec2 beforePos  = panel ? panel->Pos : anchor;
        const bool   visible =
            ImGui::Begin(id,
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (!visible) {
                ImGui::End();
                return false;
        }
        if (!otherPopup && !childPopup && (state.isPinned() || overTrigger || overPanel || activated))
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        // Native borders and the corner grip resize click-open panels.
        // A resize is deliberate: keep the panel open and preserve the resized position.
        auto *current  = ImGui::GetCurrentWindow();
        bool  resizing = current->ResizeBorderHeld >= 0;
        for (int corner = 0; corner < 4; ++corner)
                resizing |= g.ActiveId == ImGui::GetWindowResizeCornerID(current, corner);
        auto      &storage     = current->StateStorage;
        const auto resizeState = ImGui::GetID("##resizeBaseline");
        if (resizing) {
                if (!storage.GetBool(resizeState)) {
                        storage.SetBool(resizeState, true);
                        storage.SetFloat(resizeState + 1, beforePos.x);
                        storage.SetFloat(resizeState + 2, beforePos.y);
                        storage.SetFloat(resizeState + 3, beforeSize.x);
                        storage.SetFloat(resizeState + 4, beforeSize.y);
                }
                if (!state.isPinned())
                        state.setPinned(true);
                state.markDetached();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        ImGui::SetWindowPos(ImVec2(storage.GetFloat(resizeState + 1), storage.GetFloat(resizeState + 2)));
                        ImGui::SetWindowSize(ImVec2(storage.GetFloat(resizeState + 3), storage.GetFloat(resizeState + 4)));
                        ImGui::ClearActiveID();
                        interaction::consumeEscape();
                        storage.SetBool(resizeState, false);
                }
        } else
                storage.SetBool(resizeState, false);
        {
                const float  titleWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                const ImVec2 titleMin   = ImGui::GetCursorScreenPos();
                const ImVec2 titleMax(titleMin.x + titleWidth, titleMin.y + ImGui::GetFrameHeight());
                ImGui::InvisibleButton("##hoverPanelDrag", ImVec2(titleWidth, ImGui::GetFrameHeight()));
                if (ImGui::IsItemActive() && state.isPinned()) {
                        ImGuiWindow *window = ImGui::GetCurrentWindow();
                        state.markDetached();
                        const ImVec2 next(std::clamp(window->Pos.x + ImGui::GetIO().MouseDelta.x,
                                                     viewport->WorkPos.x - window->Size.x + 48.0f,
                                                     viewport->WorkPos.x + viewport->WorkSize.x - 48.0f),
                                          std::clamp(window->Pos.y + ImGui::GetIO().MouseDelta.y,
                                                     viewport->WorkPos.y,
                                                     viewport->WorkPos.y + viewport->WorkSize.y - 32.0f));
                        window->Pos = next;
                }
                if (ImGui::IsItemHovered() && state.isPinned())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                ImGui::RenderTextClipped(titleMin, titleMax, title, nullptr, nullptr, ImVec2(0.0f, 0.5f));
        }
        ImGui::Separator();
        // Draw this last frame so inputs can finish deactivation and apply edits
        // before disappearing. The click remains available to the outside control.
        if (clickedOutside)
                state.close();
        return true; // caller ends with End()
}
} // namespace ui
#endif
