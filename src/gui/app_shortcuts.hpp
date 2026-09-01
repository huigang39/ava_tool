#ifndef AVA_APP_SHORTCUTS_HPP
#define AVA_APP_SHORTCUTS_HPP

namespace ui
{
enum class AppShortcut { None, Save, SaveAs, Open, Commands, Connect, Disconnect, FreezeDisplay, ClearData };

// ImGui already maps the physical Command modifier to KeyCtrl on macOS.
// Callers pass that normalized value as primary, never swap it a second time.
struct AppShortcutInput {
        bool primary           = false;
        bool shift             = false;
        bool alt               = false;
        bool secondary         = false;
        bool textInput         = false;
        bool activeWidget      = false;
        bool popupOpen         = false;
        bool savePressed       = false;
        bool openPressed       = false;
        bool commandsPressed   = false;
        bool connectPressed    = false;
        bool disconnectPressed = false;
        bool spacePressed      = false;
        bool clearPressed      = false;
};

// Active editors/popups keep their keys. Clear requires the primary modifier
// and Shift; an unmodified Space only freezes presentation, never acquisition.
constexpr AppShortcut
ResolveAppShortcut(const AppShortcutInput &input)
{
        if (input.alt || input.secondary || input.textInput || input.activeWidget || input.popupOpen)
                return AppShortcut::None;
        if (!input.primary)
                return !input.shift && input.spacePressed ? AppShortcut::FreezeDisplay : AppShortcut::None;
        if (input.shift && input.clearPressed)
                return AppShortcut::ClearData;
        if (input.savePressed)
                return input.shift ? AppShortcut::SaveAs : AppShortcut::Save;
        if (input.shift)
                return input.disconnectPressed ? AppShortcut::Disconnect : AppShortcut::None;
        if (input.openPressed)
                return AppShortcut::Open;
        if (input.commandsPressed)
                return AppShortcut::Commands;
        if (input.connectPressed)
                return AppShortcut::Connect;
        return AppShortcut::None;
}
} // namespace ui
#endif
