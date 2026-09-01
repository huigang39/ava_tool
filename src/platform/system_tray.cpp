#include "system_tray.hpp"
#include "gui/i18n.hpp"
#include <string>
#include <utility>
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#pragma comment(lib, "shell32.lib")
#endif

struct SystemTray::Impl {
        Action action{Action::None};
#ifdef _WIN32
        static constexpr UINT callbackMessage = WM_APP + 21;
        HWND                  window{};
        NOTIFYICONDATAW       icon{};
        bool                  added{}, version4{};
        UINT                  taskbarCreated{RegisterWindowMessageW(L"TaskbarCreated")};
        bool                  addIcon()
        {
                icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
                added       = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
                if (added) {
                        icon.uVersion = NOTIFYICON_VERSION_4;
                        version4      = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
                }
                return added;
        }
        void command(UINT command)
        {
                if (command == 1 && action != Action::Exit)
                        action = Action::Restore;
                if (command == 2)
                        action = Action::Exit;
        }
        static std::wstring wide(const char *text)
        {
                int          size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
                std::wstring result(size, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
                return result;
        }
        void menu()
        {
                HMENU menu = CreatePopupMenu();
                if (!menu)
                        return;
                auto open = wide(tr("Open AvA Tool", "打开 AvA Tool"));
                auto exit = wide(tr("Exit application", "退出程序"));
                AppendMenuW(menu, MF_STRING, 1, open.c_str());
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 2, exit.c_str());
                SetMenuDefaultItem(menu, 1, FALSE);
                POINT position{};
                GetCursorPos(&position);
                SetForegroundWindow(window);
                UINT selected = TrackPopupMenuEx(
                    menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, position.x, position.y, window, nullptr);
                if (selected)
                        SendMessageW(window, WM_COMMAND, selected, 0);
                PostMessageW(window, WM_NULL, 0, 0);
                DestroyMenu(menu);
        }
        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
                auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (message == WM_NCCREATE) {
                        self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
                        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (self) {
                        if (message == self->taskbarCreated && self->added) {
                                self->added = false;
                                if (!self->addIcon())
                                        self->command(1); // Never strand a hidden application
                                return 0;
                        }
                        if (message == WM_COMMAND) {
                                self->command(LOWORD(wParam));
                                return 0;
                        }
                        if (message == callbackMessage) {
                                const UINT event = self->version4 ? LOWORD(lParam) : static_cast<UINT>(lParam);
                                if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK)
                                        self->command(1);
                                else if (event == WM_CONTEXTMENU || (!self->version4 && event == WM_RBUTTONUP))
                                        self->menu();
                                return 0;
                        }
                }
                return DefWindowProcW(window, message, wParam, lParam);
        }
#endif
};
SystemTray::SystemTray() : impl_(std::make_unique<Impl>()) {}
SystemTray::~SystemTray()
{
        remove();
#ifdef _WIN32
        if (impl_->window)
                DestroyWindow(impl_->window);
#endif
}
bool
SystemTray::show()
{
#ifdef _WIN32
        if (impl_->added)
                return true;
        if (!impl_->window) {
                WNDCLASSW type{};
                type.lpfnWndProc   = Impl::procedure;
                type.hInstance     = GetModuleHandleW(nullptr);
                type.lpszClassName = L"AvA.Tool.SystemTray";
                if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                        return false;
                // A hidden top-level window also receives Explorer restart broadcasts
                impl_->window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                                type.lpszClassName,
                                                L"AvA Tool Tray",
                                                WS_OVERLAPPED,
                                                0,
                                                0,
                                                0,
                                                0,
                                                nullptr,
                                                nullptr,
                                                type.hInstance,
                                                impl_.get());
                if (!impl_->window)
                        return false;
                impl_->icon.cbSize           = sizeof(impl_->icon);
                impl_->icon.hWnd             = impl_->window;
                impl_->icon.uID              = 1;
                impl_->icon.uCallbackMessage = Impl::callbackMessage;
                impl_->icon.hIcon            = LoadIconW(type.hInstance, L"GLFW_ICON");
                if (!impl_->icon.hIcon)
                        impl_->icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
                wcscpy_s(impl_->icon.szTip, L"AvA Tool");
        }
        return impl_->addIcon();
#else
        return false;
#endif
}
void
SystemTray::remove()
{
#ifdef _WIN32
        if (impl_->added)
                Shell_NotifyIconW(NIM_DELETE, &impl_->icon);
        impl_->added = false;
#endif
        impl_->action = Action::None;
}
SystemTray::Action
SystemTray::takeAction()
{
        return std::exchange(impl_->action, Action::None);
}
