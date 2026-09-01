#ifndef AVA_DOCK_SESSION_HPP
#define AVA_DOCK_SESSION_HPP

#include "cJSON.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <vector>

// Dock layout ini contains Selected=, but newly appearing tabs and navigation
// focus can override it. Restore after application windows have been submitted
class DockSession
{
        struct Selection {
                ImGuiID  node{}, tab{};
                unsigned settled{};
        };
        std::vector<Selection> pending_;
        ImGuiID                lastFocus_{}, restoreFocus_{};
        int                    loadedFrame_{}, attempts_{};

        static ImGuiID readId(const cJSON *item)
        {
                if (!cJSON_IsString(item))
                        return 0;
                const char *start = item->valuestring;
                const char *end   = start + std::strlen(start);
                if (end - start >= 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X'))
                        start += 2;
                ImGuiID    id{};
                const auto result = std::from_chars(start, end, id, 16);
                return result.ec == std::errc{} && result.ptr == end ? id : 0;
        }
        static void writeId(cJSON *object, const char *key, ImGuiID id)
        {
                char text[16];
                std::snprintf(text, sizeof(text), "0x%08X", id);
                cJSON_AddStringToObject(object, key, text);
        }
        void trackFocus()
        {
                ImGuiContext *context = ImGui::GetCurrentContext();
                if (context && context->NavWindow) {
                        ImGuiWindow *root = context->NavWindow->RootWindow;
                        if (root && root->DockNode && root->DockNode->IsLeafNode())
                                lastFocus_ = root->ID;
                }
        }

      public:
        void save(cJSON *root)
        {
                trackFocus();
                cJSON        *tabs    = cJSON_AddArrayToObject(root, "activeDockTabs");
                ImGuiContext *context = ImGui::GetCurrentContext();
                if (!context)
                        return;
                for (const auto &entry : context->DockContext.Nodes.Data) {
                        const auto *node = static_cast<const ImGuiDockNode *>(entry.val_p);
                        if (!node || !node->IsLeafNode())
                                continue;
                        ImGuiID selected = node->SelectedTabId;
                        if (node->TabBar) {
                                if (node->TabBar->NextSelectedTabId)
                                        selected = node->TabBar->NextSelectedTabId;
                                else if (node->TabBar->SelectedTabId)
                                        selected = node->TabBar->SelectedTabId;
                        }
                        if (!selected)
                                continue;
                        cJSON *item = cJSON_CreateObject();
                        writeId(item, "node", node->ID);
                        writeId(item, "tab", selected); // TabId is different from Window ID
                        cJSON_AddItemToArray(tabs, item);
                }
                writeId(root, "focusedDockWindow", lastFocus_);
        }

        /**
         * @brief Call after LoadIniSettingsFromMemory so legacy Selected= IDs are available
         */
        void load(const cJSON *root)
        {
                pending_.clear();
                lastFocus_ = restoreFocus_ = readId(cJSON_GetObjectItem(root, "focusedDockWindow"));
                attempts_                  = 0;
                ImGuiContext *context      = ImGui::GetCurrentContext();
                loadedFrame_               = context ? context->FrameCount : 0;
                const cJSON *tabs          = cJSON_GetObjectItem(root, "activeDockTabs");
                if (cJSON_IsArray(tabs)) {
                        const cJSON *item;
                        cJSON_ArrayForEach(item, tabs)
                        {
                                const ImGuiID node = readId(cJSON_GetObjectItem(item, "node"));
                                const ImGuiID tab  = readId(cJSON_GetObjectItem(item, "tab"));
                                if (node && tab && pending_.size() < 256)
                                        pending_.push_back({node, tab});
                        }
                } else if (context) {
                        for (const auto &entry : context->DockContext.Nodes.Data) {
                                const auto *node = static_cast<const ImGuiDockNode *>(entry.val_p);
                                if (node && node->IsLeafNode() && node->SelectedTabId)
                                        pending_.push_back({node->ID, node->SelectedTabId});
                        }
                }
        }

        /**
         * @brief Call once each frame, after all dockable windows, before ImGui::Render()
         */
        void afterWindows()
        {
                ImGuiContext *context = ImGui::GetCurrentContext();
                if (!context)
                        return;
                if (pending_.empty() && !restoreFocus_) {
                        trackFocus();
                        return;
                }
                if (context->FrameCount > loadedFrame_ + 1) {
                        bool clicked = false;
                        for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
                                clicked |= ImGui::IsMouseClicked(button);
                        if (clicked || ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                                pending_.clear();
                                restoreFocus_ = 0;
                                trackFocus();
                                return;
                        }
                }
                if (++attempts_ > 30) {
                        pending_.clear();
                        restoreFocus_ = 0;
                        trackFocus();
                        return;
                }

                for (auto &selection : pending_) {
                        ImGuiDockNode *node = ImGui::DockBuilderGetNode(selection.node);
                        if (!node || !node->TabBar)
                                continue;
                        ImGuiWindow *selected  = nullptr;
                        bool         appearing = false;
                        for (ImGuiWindow *window : node->Windows) {
                                appearing |= window->Appearing;
                                if (window->TabId == selection.tab && window->LastFrameActive == context->FrameCount)
                                        selected = window;
                        }
                        if (!selected)
                                continue; // Missing/closed tabs must not be reopened
                        node->SelectedTabId             = selection.tab;
                        node->TabBar->NextSelectedTabId = selection.tab;
                        selection.settled               = appearing ? 0 : selection.settled + 1;
                }
                // Restore navigation focus too: DockNodeUpdateTabBar otherwise selects
                // whatever dock window happened to appear last during startup
                if (restoreFocus_) {
                        ImGuiWindow *window = ImGui::FindWindowByID(restoreFocus_);
                        if (window && window->DockNode && window->LastFrameActive == context->FrameCount)
                                ImGui::FocusWindow(window);
                } else if (context->NavWindow && context->NavWindow->RootWindow->DockNode) {
                        // Legacy sessions don't have a focused-window ID. Avoid letting an
                        // unrelated appearing window override its dock's restored tab
                        auto *node = context->NavWindow->RootWindow->DockNode;
                        for (const auto &selection : pending_)
                                if (selection.node == node->ID)
                                        for (ImGuiWindow *window : node->Windows)
                                                if (window->TabId == selection.tab &&
                                                    window->LastFrameActive == context->FrameCount)
                                                        ImGui::FocusWindow(window);
                }
                pending_.erase(std::remove_if(pending_.begin(),
                                              pending_.end(),
                                              [](const Selection &selection) { return selection.settled >= 3; }),
                               pending_.end());
                if (pending_.empty())
                        restoreFocus_ = 0;
                trackFocus();
        }
};
#endif
