#pragma once

#include <algorithm>
#include <cmath>

#include "TimelineGeometry.h"

namespace soundsplice::app
{
/**
    The arithmetic behind Zoom to Selection and Fit Project: what zoom makes a
    span of the timeline fill the visible width, and where to scroll to show
    it. JUCE-free, like TimelineGeometry, so the numbers are tested on their
    own.
*/

/** A little room either side of what's fitted, so its edges aren't hard
    against the view's. */
inline constexpr double kFitMargin = 0.05;

/** The zoom at which @p lengthBeats (plus the margin either side) fills
    @p visibleWidth pixels, clamped to [@p minZoom, @p maxZoom]. */
inline float zoomToFit(double lengthBeats, float visibleWidth, float basePixelsPerBeat, float minZoom, float maxZoom)
{
    if (! (lengthBeats > 0.0) || ! (visibleWidth > 0.0f) || ! (basePixelsPerBeat > 0.0f))
        return std::clamp(1.0f, minZoom, maxZoom);

    const double withMargin = lengthBeats * (1.0 + 2.0 * kFitMargin);
    const double zoom       = (double) visibleWidth / (withMargin * (double) basePixelsPerBeat);
    return (float) std::clamp(zoom, (double) minZoom, (double) maxZoom);
}

/** The horizontal scroll that puts @p beat, less the margin for a span of
    @p lengthBeats, at the left of the view: the timeline scrolls under the
    gutter, so what's to its left starts at the gutter's right edge. */
inline int scrollToShow(double beat, double lengthBeats, const TimelineGeometry& geometry)
{
    const double from = std::max(0.0, beat - std::max(0.0, lengthBeats) * kFitMargin);
    return std::max(0, (int) std::floor(from * (double) geometry.pixelsPerBeat()));
}

/** The lane height at which @p numTracks lanes, under a ruler @p rulerHeight
    tall, fill @p visibleHeight pixels, kept within [@p minLane, @p maxLane]:
    too many tracks still scroll, and a single track doesn't grow to fill a
    whole screen. */
inline float laneHeightToFit(int numTracks, float visibleHeight, float rulerHeight, float minLane, float maxLane)
{
    if (numTracks <= 0 || ! (visibleHeight > rulerHeight))
        return minLane;

    return std::clamp((visibleHeight - rulerHeight) / (float) numTracks, minLane, maxLane);
}

} // namespace soundsplice::app
