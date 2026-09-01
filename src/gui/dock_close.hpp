#ifndef AVA_DOCK_CLOSE_HPP
#define AVA_DOCK_CLOSE_HPP

#include "gui/dock_fold.hpp"
#include "gui/i18n.hpp"
#include "gui/tutorial_guide.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>
#include <vector>

// Keep close requests separate from each tool's lifetime/open flag. Never keep
// pointers to those flags: some callers pass a temporary bool to Begin()
namespace dock_close
{
struct Request {
        ImGuiID     id;
        std::string title;
        ImVec2      anchor{};
        ImGuiID     viewport{};
};
inline std::vector<Request> pending;
inline ImGuiID              confirmed{};
inline ImGuiID              presented{}, popupId{};

inline ImGuiWindow *
confirmationWindow()
{
        if (auto *context = ImGui::GetCurrentContext())
                for (const auto &popup : context->OpenPopupStack)
                        if (popup.PopupId == popupId)
                                return popup.Window;
        return nullptr;
}

inline void
reset()
{
        pending.clear();
        confirmed = 0;
        presented = popupId = 0;
}

inline bool
begin(const char *name, bool *open, ImGuiWindowFlags flags = 0)
{
        ImGuiWindowClass windowClass;
        windowClass.TabItemFlagsOverrideSet = ImGuiTabItemFlags_NoAssumedClosure;
        ImGui::SetNextWindowClass(&windowClass);
        bool         keepOpen = *open;
        const bool   visible  = ImGui::Begin(name, &keepOpen, flags);
        ImGuiWindow *window   = ImGui::GetCurrentWindow();
        if (confirmed == window->ID) {
                confirmed = 0;
                *open     = false;
                return false;
        }
        if (!keepOpen) {
                if (std::none_of(pending.begin(), pending.end(), [&](const Request &r) { return r.id == window->ID; }))
                        pending.push_back({window->ID,
                                           std::string(name, ImGui::FindRenderedTextEnd(name)),
                                           ImGui::IsMousePosValid()
                                               ? ImGui::GetMousePos()
                                               : ImVec2(window->Pos.x + window->Size.x - 32, window->Pos.y + 24),
                                           window->Viewport->ID});
                // DockNodeUpdateFlagsAndCollapse otherwise removes the window
                // next frame, even though its tab uses NoAssumedClosure
                if (window->DockNode && window->DockNode->WantCloseTabId == window->TabId)
                        window->DockNode->WantCloseTabId = 0;
        }
        const bool contentVisible = visible && !dock_fold::isFolded(window->DockNode);
        if (contentVisible)
                TutorialGuide::instance().markKnownDock(name);
        return contentVisible;
}

/**
 * @brief Submit once after all tool windows (including hidden dock tabs)
 */
inline void
draw()
{
        dock_fold::draw();
        auto *context = ImGui::GetCurrentContext();
        for (const auto &entry : context->DockContext.Nodes.Data) {
                auto *node = static_cast<ImGuiDockNode *>(entry.val_p);
                if (node && node->WantCloseAll)
                        node->WantCloseAll = false;
        }
        pending.erase(std::remove_if(pending.begin(),
                                     pending.end(),
                                     [&](const Request &r) {
                                             auto *window = ImGui::FindWindowByID(r.id);
                                             return !window || window->LastFrameActive != context->FrameCount;
                                     }),
                      pending.end());
        const char *popup = tr("Close Dock Page###ConfirmDockClose", "关闭 Dock 页面###ConfirmDockClose");
        popupId           = ImGui::GetID(popup);
        // A normal popup is dismissed by outside click/Escape. Consume the
        // request once, rather than immediately reopening it on the next frame.
        if (presented && !ImGui::IsPopupOpen(popup)) {
                std::erase_if(pending, [](const Request &request) { return request.id == presented; });
                presented = 0;
        }
        if (!pending.empty() && presented != pending.front().id) {
                ImGui::OpenPopup(popup);
                presented = pending.front().id;
        }
        if (!pending.empty()) {
                const auto &request  = pending.front();
                auto       *viewport = ImGui::FindViewportByID(request.viewport);
                if (!viewport)
                        viewport = ImGui::GetMainViewport();
                const auto &style = ImGui::GetStyle();
                const float width = std::min(ImGui::GetFontSize() * 19, viewport->WorkSize.x - 8);
                const float textHeight =
                    ImGui::CalcTextSize(request.title.c_str(), nullptr, false, width - style.WindowPadding.x * 2).y;
                const float height = textHeight + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() +
                                     style.WindowPadding.y * 2 + style.ItemSpacing.y * 3;
                ImVec2 position(request.anchor.x + 8, request.anchor.y + 8);
                position.x =
                    std::clamp(position.x,
                               viewport->WorkPos.x + 4,
                               std::max(viewport->WorkPos.x + 4, viewport->WorkPos.x + viewport->WorkSize.x - width - 4));
                position.y =
                    std::clamp(position.y,
                               viewport->WorkPos.y + 4,
                               std::max(viewport->WorkPos.y + 4, viewport->WorkPos.y + viewport->WorkSize.y - height - 4));
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(0, 0), ImVec2(std::max(1.0f, viewport->WorkSize.x - 8), std::max(1.0f, viewport->WorkSize.y - 8)));
                ImGui::SetNextWindowPos(position, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Appearing);
        }
        if (ImGui::BeginPopup(popup, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (pending.empty()) {
                        ImGui::CloseCurrentPopup();
                } else {
                        ImGui::TextUnformatted(tr("Close this page?", "确定关闭此页面吗？"));
                        ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
                        ImGui::TextUnformatted(pending.front().title.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Spacing();
                        const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
                        const bool  accept      = ImGui::Button(tr("Close", "关闭"), ImVec2(buttonWidth, 0));
                        ImGui::SameLine();
                        const bool cancel =
                            ImGui::Button(tr("Cancel", "取消"), ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
                        ImGui::SetItemDefaultFocus();
                        if (accept || cancel) {
                                if (accept && !cancel)
                                        confirmed = pending.front().id;
                                pending.erase(pending.begin());
                                presented = 0;
                                ImGui::CloseCurrentPopup();
                        }
                }
                ImGui::EndPopup();
        }
}
} // namespace dock_close
#endif
