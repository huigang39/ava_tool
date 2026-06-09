#ifndef TUTORIAL_GUIDE_HPP
#define TUTORIAL_GUIDE_HPP

#include "imgui.h"
#include <string>
#include <vector>

// Lightweight first-run tutorial overlay for ImGui applications.
// Usage:
//   1. Call TutorialGuide::instance().draw() once per frame (after all widgets).
//   2. After drawing each landmark widget, call TutorialGuide::instance().mark("id").
//   3. On first launch the guide walks the user through each step.
class TutorialGuide
{
      public:
        static TutorialGuide &instance();

        struct Step {
                std::string id; // matches mark() calls
        };

        // Register the screen-rect of a widget that was just drawn.
        // Call immediately after the ImGui widget call (uses GetItemRectMin/Max).
        void mark(const char *id);

        // Draw the overlay (highlight + tooltip). Call once at the end of the frame.
        void draw();

        // Is the tutorial currently active?
        bool isActive() const { return active_ && step_ < (int)steps_.size(); }

        // Start / restart the tutorial.
        void start();

        // Check & load persisted "done" flag from disk.
        void loadState(const std::string &appDir);
        void saveState(const std::string &appDir);

      private:
        TutorialGuide();

        bool              active_ = false;
        int               step_   = 0;
        std::vector<Step> steps_;
        std::string       appDir_;

        // Per-frame transient: screen rect of the current step's target widget.
        bool   targetFound_ = false;
        ImVec2 targetMin_{};
        ImVec2 targetMax_{};
        float  pulseTime_ = 0.0f;
};

#endif // TUTORIAL_GUIDE_HPP
