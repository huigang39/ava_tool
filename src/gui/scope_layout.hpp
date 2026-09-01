#ifndef AVA_SCOPE_LAYOUT_HPP
#define AVA_SCOPE_LAYOUT_HPP
#include <algorithm>
#include <cmath>

namespace ui
{
// Express pane minimums in font units so saved tiny heights cannot make a
// waveform unreadable after a DPI/font change. The containing child scrolls.
inline float
ScopeMinimumHeight(float fontSize, float headerHeight)
{
        return headerHeight + std::max(160.0f, fontSize * 10.0f);
}
inline int
ScopeGridColumns(float availableWidth, float fontSize)
{
        return availableWidth >= std::max(640.0f, fontSize * 52.0f) ? 2 : 1;
}
inline float
ScopePaneHeight(float availableHeight, int rows, float overhead, float minimumHeight)
{
        return rows > 0 ? std::max(minimumHeight, (availableHeight - overhead) / rows) : minimumHeight;
}
} // namespace ui
#endif
