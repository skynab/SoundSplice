#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace soundsplice::app::sampledraw
{
/**
    The draw tool's arithmetic: redrawing samples by hand when the audio
    editor is zoomed in far enough to see them, to take out a click or a pop
    that no filter removes cleanly.

    A stroke arrives as a series of mouse positions, each a sample index and
    a value. Between two positions every sample is set on the straight line
    joining them, so a fast drag that skips samples still leaves a continuous
    line rather than a comb of isolated points. Values are clamped to full
    scale: a drawn sample past it would only clip.

    JUCE-free, so which samples a stroke changes is tested on its own.
*/

/** The span of sample indices a stroke has changed, [first, last], or empty. */
struct Touched
{
    long first = -1;
    long last  = -1;

    bool isEmpty() const { return first < 0; }

    void include(long from, long to)
    {
        const long low  = std::min(from, to);
        const long high = std::max(from, to);
        if (isEmpty())
        {
            first = low;
            last  = high;
            return;
        }
        first = std::min(first, low);
        last  = std::max(last, high);
    }
};

inline float clampSample(float value)
{
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

/** Sets every sample from index @p from to index @p to (inclusive, in either
    order) on the straight line from @p fromValue to @p toValue, within
    @p samples. Returns the indices actually written. */
inline Touched drawLine(std::vector<float>& samples, long from, float fromValue, long to, float toValue)
{
    Touched touched;
    const long size = (long) samples.size();
    if (size == 0)
        return touched;

    if (to < from)
    {
        std::swap(from, to);
        std::swap(fromValue, toValue);
    }

    const long first = std::max(0L, from);
    const long last  = std::min(size - 1, to);
    if (last < first)
        return touched;

    const long span = to - from;
    for (long i = first; i <= last; ++i)
    {
        const float t = span > 0 ? (float) (i - from) / (float) span : 0.0f;
        samples[(size_t) i] = clampSample(fromValue + (toValue - fromValue) * t);
    }

    touched.include(first, last);
    return touched;
}

} // namespace soundsplice::app::sampledraw
