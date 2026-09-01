#include "gui/hover_reveal.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>

static void check(bool value, const char *message)
{
        if (!value)
                throw std::runtime_error(message);
}

int main()
{
        try {
                ui::HoverReveal panel;
                auto tick = [&](double now, bool trigger = false, bool content = false, bool hold = false,
                                bool activate = false, bool dismiss = false, bool allow = true) {
                        return panel.update({now, trigger, content, hold, activate, dismiss, allow});
                };
                check(!tick(0, true), "Hover does not immediately cover the workspace");
                check(!tick(.20, true), "A quick pointer pass leaves the panel closed");
                check(!tick(.21), "Leaving cancels the initial hover deadline");
                check(!tick(.30, true) && !tick(.51, true), "Re-entry needs its own dwell interval");
                check(tick(.56, true) && !panel.isPinned(), "Sustained hover opens an unpinned preview");
                check(tick(.60) && tick(.80), "Crossing the gap does not immediately hide the preview");
                check(tick(.90, false, true), "Entering the preview cancels its pending close");
                check(tick(1.00) && tick(1.40), "A new leave gets the full close delay");
                check(!tick(1.46), "Leaving both trigger and preview closes it after the delay");

                check(tick(2, false, false, false, true), "A click opens without waiting for hover");
                check(panel.isPinned() && tick(30), "Pinned panels stay open when the pointer leaves");
                check(tick(31, true, false, false, true) && !panel.isPinned(), "A second click restores auto-hide");
                check(tick(32) && !tick(32.46), "Unpinned panels return to delayed closing");

                panel.setPinned(true);
                panel.setPinned(false);
                check(tick(40, false, false, true) && tick(90, false, false, true),
                      "Editing, dragging or an open descendant menu keeps the panel visible");
                check(tick(91) && tick(91.4) && !tick(91.46),
                      "Ending an interaction restarts the full close grace period");
                check(!tick(92, false, false, true), "Interaction hold alone cannot open a hidden panel");

                check(!tick(100, true, false, false, false, false, false) &&
                      !tick(105, true, false, false, false, false, false),
                      "An unrelated popup or disabled trigger can block hover opening");
                check(!tick(106, true) && tick(106.26, true), "Opening dwell starts after the block ends");
                check(!tick(107, true, false, false, false, true), "Escape closes a hovered preview immediately");
                check(!tick(120, true), "Stationary pointer cannot reopen an explicitly closed preview");
                tick(121);
                check(!tick(122, true) && tick(122.26, true), "Leaving and re-entering allows a new hover preview");
                panel.close();
                check(tick(123, true, false, false, true) && panel.isPinned(),
                      "Explicit click can reopen a dismissed panel without a pointer detour");
                check(!tick(124, true, false, false, true, true) && !panel.isPinned(),
                      "Dismissal wins over same-frame activation and pin state");

                panel.reset();
                check(!panel.isOpen() && !panel.isPinned(), "Context reset removes transient open and pin state");
                check(!tick(1, true) && tick(1.26, true), "Context reset also clears dismissal suppression");
                tick(2);
                check(tick(.1) && tick(.5) && !tick(.56), "Backward clock restarts the close deadline");
                panel.reset();
                tick(50, true);
                check(!tick(.1, true) && !tick(.3, true) && tick(.36, true),
                      "Backward clock restarts the opening deadline");
                check(tick(std::numeric_limits<double>::quiet_NaN()), "Invalid time cannot corrupt visibility");

                ui::HoverReveal instant(0, 0);
                check(instant.update({1, true}), "Zero-delay policy can open immediately");
                check(!instant.update({2}), "Zero-delay policy can close immediately");
                std::puts("PASS: hover dwell, gap grace, auto-hide, pinning, active interaction hold, dismissal, context and clock reset");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
