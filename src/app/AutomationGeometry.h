#pragma once

#include <algorithm>
#include <cmath>

#include "model/Track.h"

namespace looper
{
/**
    Pure geometry for an automation editor: converts between a parameter's
    value and a y-coordinate, and answers "is the cursor on that point?".

    JUCE-free so the conversion math is unit-tested headless, the same way
    TimelineGeometry and PianoRollGeometry are — and for the same reason those
    exist: the mapping is where an editor like this actually goes wrong, and a
    mapping that only fails visibly on screen is a mapping nobody can check.

    The x axis is deliberately absent: beats-to-pixels already has one correct
    implementation in TimelineGeometry, and an automation lane must scroll and
    zoom in step with the arrangement rather than approximately like it.
*/

/** The value range an automatable parameter is edited over. */
struct AutomationRange
{
    float minValue = 0.0f;
    float maxValue = 1.0f;

    /** Label for the top of the lane, then the bottom. */
    const char* topLabel    = "1";
    const char* bottomLabel = "0";

    /** What an empty lane sits at — the value the parameter has when nothing
        is automating it, so a fresh lane starts flat where the sound already
        is rather than jumping somewhere on the first click. */
    float defaultValue = 0.0f;
};

/**
    The range for @p param.

    Gain is in decibels over a range chosen to be useful rather than complete:
    a fader's full travel goes to -inf, but an automation lane spending half
    its height between -60 and -inf dB would waste it on differences nobody can
    hear. Pan and send are their natural full ranges, which really are the
    whole control.
*/
inline AutomationRange automationRangeFor(model::TrackParam param)
{
    switch (param)
    {
        case model::TrackParam::Gain:
            return { -60.0f, 6.0f, "+6 dB", "-60 dB", 0.0f };
        case model::TrackParam::Pan:
            return { -1.0f, 1.0f, "R", "L", 0.0f };
        case model::TrackParam::SendLevel:
            return { 0.0f, 1.0f, "100%", "0%", 0.0f };
    }
    return { 0.0f, 1.0f, "1", "0", 0.0f };
}

struct AutomationGeometry
{
    /** Vertical padding inside the lane, so a point at the very top or bottom
        is still fully drawn and still grabbable rather than half off the edge. */
    float verticalMargin = 6.0f;

    /** How close, in pixels, the cursor has to be to grab a point. Generous on
        purpose: a breakpoint is a few pixels across, and an editor that makes
        you hit it exactly is one where every miss silently adds a new point
        instead of moving the one you meant. */
    float grabRadius = 7.0f;

    /** y for @p value within a lane occupying [laneTop, laneTop + laneHeight).
        Higher values are higher on screen, which is the only arrangement
        anyone reads without thinking about it. */
    float yForValue(float value, const AutomationRange& range, float laneTop, float laneHeight) const
    {
        const float usable = std::max(1.0f, laneHeight - 2.0f * verticalMargin);
        const float span   = range.maxValue - range.minValue;

        if (span <= 0.0f)
            return laneTop + verticalMargin;

        const float t = std::clamp((value - range.minValue) / span, 0.0f, 1.0f);
        return laneTop + verticalMargin + (1.0f - t) * usable;
    }

    /** The inverse, clamped to the range — so dragging past the top of the
        lane pins the value at its maximum instead of running off it. */
    float valueForY(float y, const AutomationRange& range, float laneTop, float laneHeight) const
    {
        const float usable = std::max(1.0f, laneHeight - 2.0f * verticalMargin);
        const float t      = std::clamp(1.0f - (y - laneTop - verticalMargin) / usable, 0.0f, 1.0f);

        return range.minValue + t * (range.maxValue - range.minValue);
    }

    /** True if (@p pointX, @p pointY) is within grabRadius of (@p x, @p y). */
    bool hitsPoint(float x, float y, float pointX, float pointY) const
    {
        const float dx = x - pointX;
        const float dy = y - pointY;
        return dx * dx + dy * dy <= grabRadius * grabRadius;
    }

    /** The grab radius expressed in beats, for finding a point by time alone
        (see model::AutomationLane::indexNear). Depends on zoom, which is why
        it is asked for rather than stored. */
    double grabRadiusInBeats(float pixelsPerBeat) const
    {
        return pixelsPerBeat > 0.0f ? (double) (grabRadius / pixelsPerBeat) : 0.0;
    }
};

} // namespace looper
