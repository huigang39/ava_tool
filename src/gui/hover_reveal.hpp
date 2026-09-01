#ifndef AVA_HOVER_REVEAL_HPP
#define AVA_HOVER_REVEAL_HPP

#include <cmath>
#include <optional>

namespace ui
{
// Transient interaction state only. The caller supplies hit testing and holds
// (active editors, drags, child menus), keeping this independent of UI windows.
class HoverReveal
{
      public:
        struct Input {
                double now{};
                bool   triggerHovered{};
                bool   panelHovered{};
                bool   holdOpen{};
                bool   activate{};      // Click: pin a preview, or return a pinned panel to auto-hide.
                bool   dismiss{};       // Escape, close button, or an external popup dismissal.
                bool   allowOpen{true}; // Gates hover opening, never an explicit click.
        };

        explicit HoverReveal(double openDelay = 0.25, double closeDelay = 0.45)
            : openDelay_(validDelay(openDelay, 0.25)), closeDelay_(validDelay(closeDelay, 0.45))
        {
        }

        bool isOpen() const { return open_; }
        bool isPinned() const { return pinned_; }
        bool isDetached() const { return detached_; }

        void setPinned(bool pinned)
        {
                const bool wasPinned = pinned_;
                if (!pinned || !wasPinned)
                        detached_ = false;
                pinned_ = pinned;
                if (pinned) {
                        open_               = true;
                        suppressUntilLeave_ = false;
                }
                hoverSince_.reset();
                awaySince_.reset();
        }
        void markDetached()
        {
                if (pinned_)
                        detached_ = true;
        }

        // Unlike reset(), closing under a stationary pointer must not reopen the
        // panel a quarter second later. A new pointer entry or click is required.
        void close()
        {
                open_ = pinned_ = detached_ = false;
                suppressUntilLeave_         = true;
                hoverSince_.reset();
                awaySince_.reset();
        }

        void reset()
        {
                open_ = pinned_ = detached_ = suppressUntilLeave_ = false;
                hoverSince_.reset();
                awaySince_.reset();
                lastTime_.reset();
        }

        bool update(const Input &input)
        {
                if (input.dismiss) {
                        close();
                        return false;
                }
                if (!input.triggerHovered)
                        suppressUntilLeave_ = false;
                if (input.activate) {
                        const bool pin      = !pinned_;
                        open_               = true;
                        suppressUntilLeave_ = false;
                        setPinned(pin);
                }
                if (!std::isfinite(input.now))
                        return open_;
                // Clock resets must not reuse deadlines from a previous UI
                // context. Keep explicit pin state until the owner resets it.
                if (lastTime_ && input.now < *lastTime_) {
                        hoverSince_.reset();
                        awaySince_.reset();
                }
                lastTime_ = input.now;

                if (!open_) {
                        if (!input.triggerHovered || !input.allowOpen || suppressUntilLeave_) {
                                hoverSince_.reset();
                                return false;
                        }
                        if (!hoverSince_)
                                hoverSince_ = input.now;
                        if (input.now - *hoverSince_ < openDelay_)
                                return false;
                        open_ = true;
                        hoverSince_.reset();
                }

                if (pinned_ || input.triggerHovered || input.panelHovered || input.holdOpen) {
                        awaySince_.reset();
                } else {
                        if (!awaySince_)
                                awaySince_ = input.now;
                        if (input.now - *awaySince_ >= closeDelay_) {
                                open_ = false;
                                awaySince_.reset();
                        }
                }
                return open_;
        }

      private:
        static double validDelay(double value, double fallback)
        {
                return std::isfinite(value) && value >= 0 ? value : fallback;
        }
        double                openDelay_, closeDelay_;
        bool                  open_{}, pinned_{}, detached_{}, suppressUntilLeave_{};
        std::optional<double> hoverSince_, awaySince_, lastTime_;
};
} // namespace ui
#endif
