#ifndef AVA_PLATFORM_FONTS_HPP
#define AVA_PLATFORM_FONTS_HPP

#include "imgui.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace ui
{
struct PlatformFontSelection {
        ImFont     *font = nullptr;
        std::string uiPath;
        std::string chinesePath;
};

inline bool
FontFileExists(const std::string &path)
{
        std::error_code error;
        return !path.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(path), error);
}

// Native Latin typography with a separate CJK fallback. Using a CJK font for
// every glyph makes English labels visibly different from the host platform.
// Probe files first: AddFontFromFileTTF asserts on a nonexistent font.
inline PlatformFontSelection
LoadPlatformFonts(ImFontAtlas &atlas, float pixelSize, const std::string &preferredChinese = {})
{
        if (!std::isfinite(pixelSize) || pixelSize <= 0)
                pixelSize = 18;
        std::vector<std::string> uiFonts, chineseFonts;
#ifdef _WIN32
        const char       *windows = std::getenv("WINDIR");
        const std::string folder  = std::string(windows && *windows ? windows : "C:/Windows") + "/Fonts/";
        uiFonts                   = {folder + "segoeui.ttf", folder + "arial.ttf"};
        chineseFonts              = {folder + "msyh.ttc", folder + "msyh.ttf", folder + "simsun.ttc"};
#elif defined(__APPLE__)
        uiFonts      = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc"};
        chineseFonts = {"/System/Library/Fonts/PingFang.ttc",
                        "/System/Library/Fonts/STHeiti Medium.ttc",
                        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"};
#else
        uiFonts      = {"/usr/share/fonts/cantarell/Cantarell-Regular.otf",
                        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
        chineseFonts = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"};
#endif
        if (!preferredChinese.empty())
                chineseFonts.insert(chineseFonts.begin(), preferredChinese);
        PlatformFontSelection result;
        ImFontConfig          config;
        config.SizePixels = pixelSize;
        for (const auto &path : uiFonts) {
                if (FontFileExists(path)) {
                        result.font = atlas.AddFontFromFileTTF(path.c_str(), pixelSize, &config, atlas.GetGlyphRangesDefault());
                        if (result.font) {
                                result.uiPath = path;
                                break;
                        }
                }
        }
        if (!result.font)
                result.font = atlas.AddFontDefault(&config);
        config.MergeMode = true;
        for (const auto &path : chineseFonts) {
                if (FontFileExists(path) &&
                    atlas.AddFontFromFileTTF(path.c_str(), pixelSize, &config, atlas.GetGlyphRangesChineseSimplifiedCommon())) {
                        result.chinesePath = path;
                        break;
                }
        }
        return result;
}
} // namespace ui
#endif
