#pragma once

namespace soundsplice::app
{
/**
    Where a clip's take lanes sit inside it (View > Show Take Lanes), as
    REAPER draws them: a header band along the top, which still moves the clip
    and holds its fade handles, and below it one row per take, each showing
    that take's audio across the clip. Click a row to play that take; drag
    along a row to comp the stretch dragged over to that take.

    One layout for both painting and hit-testing, so a row is drawn where it
    responds. JUCE-free, in the clip box's own y coordinates.
*/
struct TakeLaneLayout
{
    float headerTop = 0.0f;
    float rowsTop   = 0.0f; // the first row's top, just under the header
    float rowHeight = 0.0f;
    int   rowCount  = 0;    // 0: the lanes don't fit, so the clip is drawn as usual

    bool shown() const noexcept { return rowCount > 0; }

    float rowTop(int row) const noexcept { return rowsTop + rowHeight * (float) row; }

    /** The take whose row holds @p y, or -1 for the header or outside. */
    int rowAt(float y) const noexcept
    {
        if (! shown() || y < rowsTop)
            return -1;
        const int row = (int) ((y - rowsTop) / rowHeight);
        return row < rowCount ? row : -1;
    }
};

inline constexpr float kTakeLaneHeaderHeight = 12.0f;
inline constexpr float kMinTakeLaneHeight    = 8.0f;

/** The lanes of a clip whose box runs from @p top, @p height tall, holding
    @p takeCount takes. A clip with fewer than two, or too short for a row of
    kMinTakeLaneHeight each under the header, gets none. */
inline TakeLaneLayout takeLaneLayout(float top, float height, int takeCount)
{
    TakeLaneLayout layout;
    if (takeCount < 2 || height < kTakeLaneHeaderHeight + kMinTakeLaneHeight * (float) takeCount)
        return layout;

    layout.headerTop = top;
    layout.rowsTop   = top + kTakeLaneHeaderHeight;
    layout.rowHeight = (height - kTakeLaneHeaderHeight) / (float) takeCount;
    layout.rowCount  = takeCount;
    return layout;
}

/** A track lane tall enough to give each of @p maxTakes takes a row of
    @p rowHeight, with the header and the clip's 3-pixel inset above and below. */
inline float laneHeightForTakes(int maxTakes, float rowHeight = 18.0f)
{
    return 6.0f + kTakeLaneHeaderHeight + rowHeight * (float) maxTakes;
}

} // namespace soundsplice::app
