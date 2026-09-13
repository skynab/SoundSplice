#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace looper::model
{
struct AutomationPoint
{
    double beat  = 0.0;
    float  value = 0.0f;

    bool operator==(const AutomationPoint&) const = default;
};

/**
    A parameter automation lane: breakpoints of (beat, value), kept sorted by
    beat, with linear interpolation between them. Values before the first point
    hold the first value; after the last, the last. JUCE-free and unit-tested.
*/
class AutomationLane
{
public:
    void clear() { points_.clear(); }
    bool empty() const { return points_.empty(); }

    const std::vector<AutomationPoint>& points() const { return points_; }

    /** Adds a point, keeping the lane sorted; replaces the value at a coincident beat. */
    void addPoint(double beat, float value)
    {
        auto it = std::lower_bound(points_.begin(), points_.end(), beat,
                                   [](const AutomationPoint& p, double b) { return p.beat < b; });

        if (it != points_.end() && std::abs(it->beat - beat) < 1.0e-9)
            it->value = value;
        else
            points_.insert(it, AutomationPoint { beat, value });
    }

    /** Interpolated value at @p beat; @p fallback when the lane is empty. */
    float valueAt(double beat, float fallback = 0.0f) const
    {
        if (points_.empty())
            return fallback;
        if (beat <= points_.front().beat)
            return points_.front().value;
        if (beat >= points_.back().beat)
            return points_.back().value;

        auto hi = std::lower_bound(points_.begin(), points_.end(), beat,
                                   [](const AutomationPoint& p, double b) { return p.beat < b; });
        const AutomationPoint& upper = *hi;
        const AutomationPoint& lower = *(hi - 1);

        const double span = upper.beat - lower.beat;
        if (span <= 0.0)
            return lower.value;

        const double t = (beat - lower.beat) / span;
        return (float) (lower.value + (upper.value - lower.value) * t);
    }

    /**
        The index of the point nearest @p beat within @p tolerance, or -1.

        Exists because until now a lane could only ever *gain* points: fader
        automation wrote them and nothing could find one again, so a mistake
        was only fixable by clearing the whole lane. Editing needs to identify
        the point under the cursor first.
    */
    int indexNear(double beat, double tolerance) const
    {
        int    best         = -1;
        double bestDistance = tolerance;

        for (int i = 0; i < (int) points_.size(); ++i)
        {
            const double distance = std::abs(points_[(size_t) i].beat - beat);
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best         = i;
            }
        }

        return best;
    }

    /** Removes the point at @p index; out of range is a no-op. */
    void removePointAt(int index)
    {
        if (index >= 0 && index < (int) points_.size())
            points_.erase(points_.begin() + index);
    }

    /**
        Moves the point at @p index to (@p beat, @p value), keeping the lane
        sorted.

        Returns the point's new index, which is not necessarily the old one:
        dragging a point past its neighbour reorders the lane, and a caller
        tracking "the point I am dragging" has to be told where it went. Sorted
        order is not optional — valueAt binary-searches it.
    */
    int movePoint(int index, double beat, float value)
    {
        if (index < 0 || index >= (int) points_.size())
            return -1;

        const AutomationPoint moved { std::max(0.0, beat), value };

        points_.erase(points_.begin() + index);

        auto it = std::lower_bound(points_.begin(), points_.end(), moved.beat,
                                   [](const AutomationPoint& p, double b) { return p.beat < b; });

        const int newIndex = (int) std::distance(points_.begin(), it);
        points_.insert(it, moved);
        return newIndex;
    }

    bool operator==(const AutomationLane&) const = default;

private:
    std::vector<AutomationPoint> points_; // sorted by beat
};

} // namespace looper::model
