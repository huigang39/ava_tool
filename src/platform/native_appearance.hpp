#ifndef AVA_NATIVE_APPEARANCE_HPP
#define AVA_NATIVE_APPEARANCE_HPP

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace native_appearance
{
// Apply to the real OS-owned titlebar, including detached ImGui viewports.
// Runtime lookup preserves compatibility with systems that predate these DWM
// attributes. No fake caption buttons, transparency or custom hit-testing.
inline void
applyWindow(void *nativeHandle, bool useDark)
{
#ifdef _WIN32
        HWND         window = static_cast<HWND>(nativeHandle);
        const HANDLE state  = reinterpret_cast<HANDLE>(useDark ? 2 : 1);
        if (!window || GetPropW(window, L"AvA.NativeAppearance") == state)
                return;
        using SetAttribute             = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = []() -> SetAttribute {
                const HMODULE library = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                return library ? reinterpret_cast<SetAttribute>(GetProcAddress(library, "DwmSetWindowAttribute")) : nullptr;
        }();
        if (!setAttribute)
                return;
        // DWMWA_USE_IMMERSIVE_DARK_MODE (20), DWMWA_WINDOW_CORNER_PREFERENCE (33).
        // Older Windows versions safely reject unsupported attributes.
        const BOOL  dark    = useDark ? TRUE : FALSE;
        const DWORD rounded = 2; // DWMWCP_ROUND; DWM handles snapped/maximized windows.
        setAttribute(window, 20, &dark, sizeof(dark));
        setAttribute(window, 33, &rounded, sizeof(rounded));
        SetPropW(window, L"AvA.NativeAppearance", state);
#else
        (void)nativeHandle;
        (void)useDark;
#endif
}
inline void
applyDarkWindow(void *nativeHandle)
{
        applyWindow(nativeHandle, true);
}
} // namespace native_appearance
#endif
