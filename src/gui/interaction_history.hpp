#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace ui::interaction
{
enum class Action { None, Back, Forward, Parent, Escape, Undo, Redo };

template <class State> class UndoHistory
{
        struct Entry {
                State before, after;
        };
        std::vector<Entry> entries_;
        size_t             cursor_{};

      public:
        void clear()
        {
                entries_.clear();
                cursor_ = 0;
        }
        bool canUndo() const { return cursor_ > 0; }
        bool canRedo() const { return cursor_ < entries_.size(); }
        void record(State before, State after)
        {
                if (before == after)
                        return;
                entries_.erase(entries_.begin() + cursor_, entries_.end());
                entries_.push_back({std::move(before), std::move(after)});
                if (entries_.size() > 32)
                        entries_.erase(entries_.begin());
                cursor_ = entries_.size();
        }
        const State *undo() { return canUndo() ? &entries_[--cursor_].before : nullptr; }
        const State *redo() { return canRedo() ? &entries_[cursor_++].after : nullptr; }
};

struct Region {
        ImGuiID                                  id{};
        std::string                              route;
        bool                                     undo{}, redo{}, parent{};
        std::function<void(Action)>              command;
        std::function<void(const std::string &)> navigate;
};
struct Location {
        ImGuiID                                 window{}, region{};
        std::string                             route;
        std::vector<std::pair<ImGuiID, ImVec2>> scroll;
        bool                                    same(const Location &other) const
        {
                return window == other.window && region == other.region && route == other.route;
        }
};
inline std::vector<Region>   regions;
inline std::vector<Location> visits;
inline size_t                visitIndex{};
inline ImGuiID               context{};
inline Action                requested = Action::None;
inline bool                  popupAtFrameStart{}, activeAtFrameStart{}, undoAvailable{}, redoAvailable{}, parentAvailable{};
inline bool                  restoring{};
inline bool                  escapeConsumed{};
inline int                   restoreFrames{};
inline Location              restore;

// Called before NewFrame, so Esc closing an editor/menu cannot also leave a view.
inline void
beginFrame()
{
        escapeConsumed     = false;
        popupAtFrameStart  = ImGui::GetCurrentContext()->OpenPopupStack.Size != 0;
        activeAtFrameStart = ImGui::IsAnyItemActive();
        regions.clear();
}
inline void
consumeEscape()
{
        escapeConsumed = true;
}
inline void
reset()
{
        regions.clear();
        visits.clear();
        visitIndex    = 0;
        context       = 0;
        requested     = Action::None;
        restoring     = false;
        restoreFrames = 0;
        undoAvailable = redoAvailable = parentAvailable = false;
}
inline void
registerRegion(std::string                              route,
               bool                                     undo,
               bool                                     redo,
               bool                                     parent,
               std::function<void(Action)>              command,
               std::function<void(const std::string &)> navigate)
{
        Region entry{
            ImGui::GetCurrentWindow()->ID, std::move(route), undo, redo, parent, std::move(command), std::move(navigate)};
        for (auto &region : regions)
                if (region.id == entry.id) {
                        region = std::move(entry);
                        return;
                }
        regions.push_back(std::move(entry));
}
inline bool
canBack()
{
        return !visits.empty() && visitIndex > 0;
}
inline bool
canForward()
{
        return visitIndex + 1 < visits.size();
}
inline Region *
findRegion(ImGuiID id)
{
        for (auto &region : regions)
                if (region.id == id)
                        return &region;
        return nullptr;
}
inline bool
navigable(ImGuiWindow *window)
{
        return window && window->Active &&
               !(window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_DockNodeHost)) &&
               (!(window->Flags & ImGuiWindowFlags_ChildWindow) || window->DockIsActive) && window->Name[0] != '#';
}
inline void
process()
{
        auto      &g                  = *ImGui::GetCurrentContext();
        const bool restoringThisFrame = restoring;
        if (restoring) {
                if (auto *window = ImGui::FindWindowByID(restore.window); window && window->Active) {
                        ImGui::FocusWindow(window);
                        bool ready = !window->Hidden && !window->SkipItems;
                        for (const auto &[id, scroll] : restore.scroll) {
                                auto *child  = ImGui::FindWindowByID(id);
                                ready       &= child && child->Active && !child->Hidden;
                        }
                        // Dock tabs and nested tab bars become visible on the
                        // next frame; restore child scroll after their layout.
                        if (ready || ++restoreFrames >= 4) {
                                for (const auto &[id, scroll] : restore.scroll)
                                        if (auto *child = ImGui::FindWindowByID(id); child && child->Active) {
                                                ImGui::SetScrollX(child, scroll.x);
                                                ImGui::SetScrollY(child, scroll.y);
                                        }
                                restoring = false;
                        }
                } else
                        restoring = false;
        }
        auto   *focused = g.NavWindow;
        auto   *root    = focused ? focused->RootWindow : nullptr;
        Region *region  = nullptr;
        for (auto it = regions.rbegin(); it != regions.rend(); ++it)
                if (auto *window = ImGui::FindWindowByID(it->id);
                    focused && window && (focused == window || ImGui::IsWindowChildOf(focused, window, false, false))) {
                        region = &*it;
                        break;
                }
        if (navigable(root))
                context = region ? region->id : 0;
        if (navigable(root) && !restoringThisFrame) {
                Location now{root->ID, context, region ? region->route : std::string{}};
                for (auto *window : g.Windows)
                        if (window->Active && window->RootWindow == root)
                                now.scroll.push_back({window->ID, window->Scroll});
                if (visits.empty()) {
                        visits.push_back(now);
                        visitIndex = 0;
                } else if (visits[visitIndex].same(now))
                        visits[visitIndex] = now;
                else {
                        visits.erase(visits.begin() + visitIndex + 1, visits.end());
                        visits.push_back(now);
                        if (visits.size() > 100)
                                visits.erase(visits.begin());
                        visitIndex = visits.size() - 1;
                }
        }
        region          = findRegion(context);
        undoAvailable   = region && region->undo;
        redoAvailable   = region && region->redo;
        parentAvailable = region && region->parent;
        Action action   = requested;
        requested       = Action::None;
        const auto &io  = g.IO;
        const bool  blocked =
            popupAtFrameStart || g.OpenPopupStack.Size || activeAtFrameStart || ImGui::IsAnyItemActive() || io.WantTextInput;
        if (action == Action::None && !blocked) {
                if (io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
                        if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
                                action = io.KeyShift ? Action::Redo : Action::Undo;
                        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                                action = Action::Redo;
                } else if (io.KeyAlt && !io.KeyCtrl && !io.KeySuper && !io.KeyShift) {
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                                action = Action::Back;
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                                action = Action::Forward;
                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
                                action = Action::Parent;
                } else if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift) {
                        if (ImGui::IsMouseClicked(3))
                                action = Action::Back;
                        if (ImGui::IsMouseClicked(4))
                                action = Action::Forward;
                        if (!escapeConsumed && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                                action = Action::Escape;
                }
        }
        if (action == Action::Back || action == Action::Forward) {
                const int direction = action == Action::Back ? -1 : 1;
                for (int i  = static_cast<int>(visitIndex) + direction; i >= 0 && i < static_cast<int>(visits.size());
                     i     += direction) {
                        const auto &target = visits[i];
                        auto       *window = ImGui::FindWindowByID(target.window);
                        if (!window || !window->Active)
                                continue;
                        auto *destination = findRegion(target.region);
                        if (target.region && !destination)
                                continue;
                        if (destination)
                                destination->navigate(target.route);
                        visitIndex    = i;
                        restore       = target;
                        restoring     = true;
                        restoreFrames = 0;
                        break;
                }
        } else if (region && action != Action::None &&
                   ((action == Action::Undo && region->undo) || (action == Action::Redo && region->redo) ||
                    ((action == Action::Parent || action == Action::Escape) && region->parent))) {
                region->command(action); // panel applies on its next frame, under its normal locking
        }
}
} // namespace ui::interaction
