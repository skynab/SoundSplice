#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "model/EffectParams.h"
#include "model/Track.h"

namespace soundsplice
{
/**
    Pure geometry for an automation editor: converts between a parameter's
    value and a y-coordinate, and answers "is the cursor on that point?".

    JUCE-free so the conversion math is unit-tested headless, the same way
    TimelineGeometry is — and for the same reason that exists: the mapping is where an editor like this actually goes wrong, and a
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
    std::string topLabel    = "1";
    std::string bottomLabel = "0";

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
    hear. Pan is its natural full range, which really is the whole control.
*/
inline AutomationRange automationRangeFor(model::TrackParam param)
{
    switch (param)
    {
        case model::TrackParam::Gain:
            return { -60.0f, 6.0f, "+6 dB", "-60 dB", 0.0f };
        case model::TrackParam::Pan:
            return { -1.0f, 1.0f, "R", "L", 0.0f };
    }
    return { 0.0f, 1.0f, "1", "0", 0.0f };
}

/** What an automation lane drives: one of the track's own parameters, or
    one parameter of one effect in its chain. The effect is named by its
    position and kind, so a lane shown for a slot that has since become
    another effect is recognised as stale rather than edited. */
struct AutomationTarget
{
    model::TrackParam trackParam = model::TrackParam::Gain; // when slot < 0
    int               slot       = -1;
    model::EffectKind kind       = model::EffectKind::Filter;
    std::string       paramId;

    bool isEffect() const noexcept { return slot >= 0; }
    bool operator==(const AutomationTarget&) const = default;

    static AutomationTarget track(model::TrackParam param) { return { param, -1, {}, {} }; }
    static AutomationTarget effect(int slot, model::EffectKind kind, std::string paramId)
    {
        return { model::TrackParam::Gain, slot, kind, std::move(paramId) };
    }
};

namespace automationdetail
{
    /** @p value as the effect panel shows it: scaled, trimmed, with its unit. */
    inline std::string displayed(const model::EffectParam& param, double value)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%g", value * param.displayScale);
        return buffer + std::string(param.unit);
    }
}

/**
    The range for an effect parameter: its descriptor's, labelled the way the
    effect panel shows values, with @p staticValue - what the parameter is set
    to when nothing automates it - as where a fresh lane sits.
*/
inline AutomationRange automationRangeFor(const model::EffectParam& param, double staticValue)
{
    AutomationRange range;
    range.minValue     = (float) param.min;
    range.maxValue     = (float) param.max;
    range.defaultValue = (float) staticValue;

    if (param.control == model::ParamControl::Toggle)
    {
        range.topLabel    = "On";
        range.bottomLabel = "Off";
    }
    else if (param.control == model::ParamControl::Choice && ! param.choices.empty())
    {
        range.topLabel    = param.choices.back();
        range.bottomLabel = param.choices.front();
    }
    else
    {
        range.topLabel    = automationdetail::displayed(param, param.max);
        range.bottomLabel = automationdetail::displayed(param, param.min);
    }
    return range;
}

/** A name for @p effect's parameter @p index that stands on its own in a
    list. Parameters shown indented under a heading in the effect panel
    ("Band 2", then "  Gain") take the heading with them: "Band 2 Gain". */
inline std::string automationParamName(const model::EffectDescriptor& effect, size_t index)
{
    const std::string name = effect.params[index].name;
    const auto        text = name.find_first_not_of(' ');
    if (text == 0 || text == std::string::npos)
        return name;

    for (size_t i = index; i-- > 0;)
    {
        const std::string heading = effect.params[i].name;
        if (heading.find_first_not_of(' ') == 0)
            return heading + " " + name.substr(text);
    }
    return name.substr(text);
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

} // namespace soundsplice
