#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace looper::app
{
/**
    Lays fixed-width controls left to right, wrapping onto a new row when the
    next one would not fit.

    Exists because the obvious way to build a toolbar — a chain of
    `area.removeFromLeft(n)` — fails silently when the pane is narrower than
    the sum of the widths. `juce::Rectangle::removeFromLeft` on an exhausted
    rectangle returns an *empty* rectangle rather than clipping or complaining,
    so every control past the edge is laid out zero-wide and simply never
    appears. The transport's count-in box was invisible at the default window
    size for exactly this reason: the Transport pane is around 320px wide and
    its button row wanted over 500.

    Wrapping rather than hiding, because these are all controls the user needs
    to reach; a pane that is merely narrow should get taller, not lose its
    buttons. (Where hiding *is* right — a row that would push the pane's real
    content off the bottom — see `setBoundsOrHide` in EffectChainPanel.)

    Free-standing and JUCE-geometry-only so the wrap is a headless, testable
    claim rather than something you have to resize a window to check.
*/
struct RowItem
{
    int              width     = 0;
    int              gapBefore = 0;  // ignored when the item starts a row
    juce::Point<int> shrink    { 0, 0 }; // applied as reduced(x, y)
};

/**
    Rectangles for @p items inside @p area, wrapping at @p area's width.

    @p rowHeight is each row's height and @p rowGap the space between rows.
    Rows are taken from the top of @p area; an item wider than the whole area
    is given the full width rather than nothing, since a clipped control is
    still usable and an invisible one is not.
*/
inline std::vector<juce::Rectangle<int>> wrapRow(juce::Rectangle<int> area,
                                                 int rowHeight, int rowGap,
                                                 const std::vector<RowItem>& items)
{
    std::vector<juce::Rectangle<int>> bounds;
    bounds.reserve(items.size());

    if (area.getWidth() <= 0 || rowHeight <= 0)
    {
        bounds.assign(items.size(), {});
        return bounds;
    }

    const int rowWidth = area.getWidth();
    auto      row      = area.removeFromTop(rowHeight);
    int       used     = 0;

    for (const auto& item : items)
    {
        const int width  = juce::jmin(juce::jmax(1, item.width), rowWidth);
        const int gap    = used > 0 ? item.gapBefore : 0;
        const int needed = gap + width;

        if (used > 0 && used + needed > rowWidth)
        {
            area.removeFromTop(rowGap);
            row  = area.removeFromTop(rowHeight);
            used = 0;
            bounds.push_back(row.removeFromLeft(width).reduced(item.shrink.x, item.shrink.y));
            used = width;
            continue;
        }

        if (gap > 0)
            row.removeFromLeft(gap);

        bounds.push_back(row.removeFromLeft(width).reduced(item.shrink.x, item.shrink.y));
        used += needed;
    }

    return bounds;
}

/** Total height @p items need at @p width — so a caller can reserve room
    before laying anything out. */
inline int wrappedRowHeight(int width, int rowHeight, int rowGap, const std::vector<RowItem>& items)
{
    if (width <= 0 || items.empty())
        return 0;

    int rows = 1;
    int used = 0;

    for (const auto& item : items)
    {
        const int itemWidth = juce::jmin(juce::jmax(1, item.width), width);
        const int needed    = (used > 0 ? item.gapBefore : 0) + itemWidth;

        if (used > 0 && used + needed > width)
        {
            ++rows;
            used = itemWidth;
        }
        else
        {
            used += needed;
        }
    }

    return rows * rowHeight + (rows - 1) * rowGap;
}

} // namespace looper::app
