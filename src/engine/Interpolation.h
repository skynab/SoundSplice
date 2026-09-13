#pragma once

#include <cstddef>

namespace looper::engine
{
/**
    Linearly interpolated read from a mono sample array at a fractional position.
    Out-of-range positions return silence; the final sample is held (no read past
    the end). JUCE-free so the resampling math is unit-tested headless.
*/
inline float sampleLinear(const float* data, int length, double position) noexcept
{
    if (data == nullptr || length <= 0 || position < 0.0 || position >= (double) length)
        return 0.0f;

    const int    i0   = (int) position;
    const double frac = position - (double) i0;
    const float  s0   = data[i0];
    const float  s1   = (i0 + 1 < length) ? data[i0 + 1] : s0;

    return (float) (s0 + (s1 - s0) * frac);
}

} // namespace looper::engine
