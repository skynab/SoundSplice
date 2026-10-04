#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/AutomationShape.h"
#include "engine/EffectKind.h"

namespace soundsplice::engine
{
/**
    An audio-thread-readable parameter curve: breakpoints of (beat, value),
    sorted, linearly interpolated, holding the end values outside the range.

    Semantically identical to model::AutomationLane, and deliberately a
    separate type rather than a reuse of it: `model` already depends on
    `engine` (a Clip owns engine::ClipFades and the like), so the dependency can't run
    both ways. The owner converts one into the other when handing automation
    to the engine — see AudioEngine::setTrackAutomation.

    JUCE-free, so the interpolation is unit-tested headless alongside the
    model's version and the two can be checked for agreement.
*/
struct AutomationCurvePoint
{
    double     beat  = 0.0;
    float      value = 0.0f;
    CurveShape shape = CurveShape::Linear; // of the segment from here to the next point
};

class AutomationCurve
{
public:
    void addPoint(double beat, float value, CurveShape shape = CurveShape::Linear)
    {
        points_.push_back({ beat, value, shape });
    }

    /** Points must be sorted for valueAt's binary search; the converter feeds
        them in order, but sorting here makes that impossible to get wrong. */
    void sortPoints()
    {
        std::sort(points_.begin(), points_.end(),
                  [](const AutomationCurvePoint& a, const AutomationCurvePoint& b) { return a.beat < b.beat; });
    }

    bool   empty() const noexcept { return points_.empty(); }
    size_t size() const noexcept  { return points_.size(); }

    /** Interpolated value at @p beat; @p fallback when the curve is empty.
        Audio-thread safe: reads only, no allocation. */
    float valueAt(double beat, float fallback) const noexcept
    {
        if (points_.empty())
            return fallback;
        if (beat <= points_.front().beat)
            return points_.front().value;
        if (beat >= points_.back().beat)
            return points_.back().value;

        const auto hi = std::lower_bound(points_.begin(), points_.end(), beat,
                                         [](const AutomationCurvePoint& p, double b) { return p.beat < b; });
        const auto& upper = *hi;
        const auto& lower = *(hi - 1);

        const double span = upper.beat - lower.beat;
        if (span <= 0.0)
            return lower.value;

        return (float) (lower.value + (upper.value - lower.value) * shapedFraction(lower.shape, (beat - lower.beat) / span));
    }

private:
    std::vector<AutomationCurvePoint> points_;
};

/**
    Every automated parameter of one track, swapped onto the audio thread as a
    single unit — the same whole-object hand-off shape as AudioFilePlayerNode::ClipList,
    for the same reason: one pointer swap can't
    be observed half-applied.

    An empty curve means "not automated", and the track falls back to its
    static value.
*/
/** One automated parameter of one effect in a track's chain. Addressed by
    the slot's position and kind (checked against the node before it's set,
    since the chain can be one rebuild behind), and by the parameter's
    descriptor id. */
struct EffectParamCurve
{
    int             slot = 0;
    EffectKind      kind = EffectKind::Filter;
    std::string     paramId;
    AutomationCurve curve; // in the parameter's own units
};

struct TrackAutomation
{
    AutomationCurve gain;      // dB
    AutomationCurve pan;       // -1..+1

    std::vector<EffectParamCurve> effects;

    bool any() const noexcept { return ! gain.empty() || ! pan.empty() || ! effects.empty(); }
};

} // namespace soundsplice::engine
