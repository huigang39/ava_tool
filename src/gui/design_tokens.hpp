#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace ui::design
{
// Logical pixels. ApplyAppStyle scales metrics once; drawing helpers use the
// resulting style so detached viewports and font zoom keep the same geometry.
inline constexpr float body          = 16.0f;
inline constexpr float caption       = 12.0f;
inline constexpr float heading       = 20.0f;
inline constexpr float hairline      = 0.5f;
inline constexpr float windowRadius  = 12.0f;
inline constexpr float panelRadius   = 10.0f;
inline constexpr float controlRadius = 6.0f;
inline constexpr float space         = 4.0f;

inline float
InnerRadius(float outer, float inset)
{
        return std::max(0.0f, outer - std::max(0.0f, inset));
}

inline float
Scale(float scale)
{
        return std::isfinite(scale) && scale > 0 ? scale : 1.0f;
}

inline float
FontSize(float logical, float scale)
{
        return std::max(caption, std::round(logical * Scale(scale)));
}

inline float
Stroke()
{
        return std::max(hairline, ImGui::GetStyle().FrameBorderSize);
}
} // namespace ui::design
