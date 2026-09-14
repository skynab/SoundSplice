#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace soundsplice::engine
{
/**
    A volume curve drawn on one audio clip: Audacity's envelope tool, REAPER's
    take volume envelope.

    Points are placed in seconds into the clip's *source file*, not seconds
    from the clip's start. Trimming, splitting and slipping a clip only move
    its window over the file (see app/ClipWindow.h), so a curve kept in file
    time stays on the audio it was drawn over through every one of them: each
    half of a split keeps its own part of the curve with nothing to copy or
    shift.

    Gains are linear (1 is unchanged), interpolated linearly between points,
    and held flat before the first point and after the last, like
    model::AutomationLane. An empty envelope is unity everywhere, so a clip
    that has never had one drawn plays exactly as before.

    JUCE-free and allocation-free to read, so the player can call gainAt on
    the audio thread.
*/
struct EnvelopePoint
{
    double seconds = 0.0; // into the source file
    float  gain    = 1.0f;

    bool operator==(const EnvelopePoint&) const = default;
};

class ClipEnvelope
{
public:
    /** How far a point can raise the level: +12 dB. Silence is the floor. */
    static constexpr float kMaxGain = 4.0f;

    bool isEmpty() const noexcept { return points_.empty(); }
    const std::vector<EnvelopePoint>& points() const noexcept { return points_; }
    void clear() { points_.clear(); }

    /** Adds a point, keeping the envelope in order; a point already at the
        same place takes the new gain instead. Gain is clamped to [0, kMaxGain]
        and position to the start of the file. Returns the point's index. */
    int addPoint(double seconds, float gain)
    {
        const EnvelopePoint point { std::max(0.0, seconds), clampGain(gain) };

        auto it = lowerBound(point.seconds);
        if (it != points_.end() && std::abs(it->seconds - point.seconds) < kSamePlace)
        {
            it->gain = point.gain;
            return (int) (it - points_.begin());
        }

        return (int) (points_.insert(it, point) - points_.begin());
    }

    /** The gain at @p seconds into the source file: 1 with no points. */
    float gainAt(double seconds) const noexcept
    {
        if (points_.empty())
            return 1.0f;
        if (seconds <= points_.front().seconds)
            return points_.front().gain;
        if (seconds >= points_.back().seconds)
            return points_.back().gain;

        const auto upper = std::lower_bound(points_.begin(), points_.end(), seconds,
                                            [](const EnvelopePoint& p, double s) { return p.seconds < s; });
        const auto lower = upper - 1;

        const double span = upper->seconds - lower->seconds;
        if (span <= 0.0)
            return upper->gain;

        const double t = (seconds - lower->seconds) / span;
        return (float) (lower->gain + (upper->gain - lower->gain) * t);
    }

    /** The index of the point nearest @p seconds within @p tolerance, or -1. */
    int indexNear(double seconds, double tolerance) const noexcept
    {
        int    best         = -1;
        double bestDistance = tolerance;

        for (int i = 0; i < (int) points_.size(); ++i)
        {
            const double distance = std::abs(points_[(size_t) i].seconds - seconds);
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best         = i;
            }
        }

        return best;
    }

    void removePointAt(int index)
    {
        if (index >= 0 && index < (int) points_.size())
            points_.erase(points_.begin() + index);
    }

    /** Moves point @p index to (@p seconds, @p gain), keeping the envelope in
        order. Returns its new index, which changes when it's dragged past a
        neighbour, or -1 if there's no such point. */
    int movePoint(int index, double seconds, float gain)
    {
        if (index < 0 || index >= (int) points_.size())
            return -1;

        points_.erase(points_.begin() + index);
        const EnvelopePoint moved { std::max(0.0, seconds), clampGain(gain) };
        return (int) (points_.insert(lowerBound(moved.seconds), moved) - points_.begin());
    }

    bool operator==(const ClipEnvelope&) const = default;

private:
    /** Closer than a tenth of a sample at 192 kHz counts as the same place. */
    static constexpr double kSamePlace = 1.0e-6;

    static float clampGain(float gain)
    {
        return std::isfinite(gain) ? std::clamp(gain, 0.0f, kMaxGain) : 1.0f;
    }

    std::vector<EnvelopePoint>::iterator lowerBound(double seconds)
    {
        return std::lower_bound(points_.begin(), points_.end(), seconds,
                                [](const EnvelopePoint& p, double s) { return p.seconds < s; });
    }

    std::vector<EnvelopePoint> points_; // in order of seconds
};

} // namespace soundsplice::engine
