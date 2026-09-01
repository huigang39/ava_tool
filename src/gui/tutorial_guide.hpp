#pragma once
#include "gui/tutorial_content.hpp"
#include "imgui.h"
#include <array>
#include <string>
#include <unordered_map>

class TutorialGuide
{
      public:
        using Topic = ui::guide::Topic;
        static TutorialGuide &instance();
        void                  mark(const char *id);
        void                  markJLinkEntry();
        void                  markDock(Topic topic);
        void                  markKnownDock(const char *windowName);
        void                  draw();
        bool                  isActive() const { return active_; }
        bool                  catalogOpen() const { return catalogOpen_; }
        Topic                 topic() const { return topic_; }
        int                   currentStep() const { return step_; }
        bool                  completed(Topic topic) const { return completed_[static_cast<int>(topic)]; }
        void                  start(Topic topic = Topic::Overview, bool resume = false);
        void                  openCatalog();
        void                  showForCurrentDock();
        void                  dismiss();
        void                  loadState(const std::string &appDir);
        void                  saveState(const std::string &appDir);

      private:
        struct DockTarget {
                Topic   topic{};
                ImVec2  min{}, max{};
                ImGuiID viewport{};
                int     frame{-1};
        };
        bool                                             active_{}, catalogOpen_{}, targetFound_{};
        int                                              step_{};
        Topic                                            topic_{Topic::Overview};
        std::array<int, static_cast<int>(Topic::Count)>  progress_{};
        std::array<bool, static_cast<int>(Topic::Count)> completed_{};
        std::unordered_map<ImGuiID, DockTarget>          docks_;
        ImGuiID                                          lastDock_{}, owner_{}, targetViewport_{};
        ImVec2                                           targetMin_{}, targetMax_{};
        std::string                                      appDir_;
        char                                             search_[128]{};
        void                                             drawCatalog();
        void                                             moveStep(int step);
};
