#pragma once

#include <algorithm>
#include <cmath>

namespace soundsplice::engine
{
/**
    A one-pole DC blocker: y = x - x[-1] + r * y[-1], a high-pass whose corner
    sits a few hertz up, so a recording's constant offset (a cheap interface's,
    or what an asymmetric clipper leaves) goes and everything audible stays.
    JUCE-free, for the tests.
*/
class DcBlocker
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setCutoffHz(cutoffHz_);
        reset();
    }

    void reset() noexcept { previousIn_ = previousOut_ = 0.0; }

    void setCutoffHz(float hz) noexcept
    {
        cutoffHz_ = std::max(0.1f, hz);
        pole_     = std::exp(-2.0 * 3.14159265358979323846 * cutoffHz_ / sampleRate_);
    }

    float process(float x) noexcept
    {
        const double y = (double) x - previousIn_ + pole_ * previousOut_;
        previousIn_    = x;
        previousOut_   = std::abs(y) < 1.0e-20 ? 0.0 : y; // no denormals in the tail of silence
        return (float) y;
    }

private:
    double sampleRate_  = 48000.0;
    float  cutoffHz_    = 5.0f;
    double pole_        = 0.9993;
    double previousIn_  = 0.0;
    double previousOut_ = 0.0;
};

} // namespace soundsplice::engine
