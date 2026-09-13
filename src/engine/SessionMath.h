#pragma once

#include <cmath>
#include <cstdint>

namespace looper::engine
{
/**
    When a launched session clip actually starts.

    Clicking a clip mid-bar shouldn't start it mid-bar — it waits for the next
    musical boundary, which is what makes launching a musical act rather than a
    test of reflexes. That decision has to happen on the audio thread, because
    only it knows the sample-accurate playhead; the message thread just records
    which slot the user wants.

    JUCE-free so the boundary arithmetic is unit-tested headless, like
    SequencerMath and MetronomeMath. An off-by-one here is a clip that starts a
    bar late or a hair early, which is easy to ship and hard to hear
    deliberately.
*/
struct SessionMath
{
    /** The first quantization boundary at or after @p playheadSamples.

        A quantum of zero (or less) means "no quantization" — the launch takes
        effect immediately, so the boundary *is* the playhead. A playhead
        sitting exactly on a boundary launches there rather than waiting a
        whole extra bar. */
    static int64_t nextLaunchBoundary(int64_t playheadSamples, double quantumSamples) noexcept
    {
        if (quantumSamples <= 0.0)
            return playheadSamples;

        const double position = (double) playheadSamples / quantumSamples;

        // The epsilon keeps a playhead already on a boundary from being pushed
        // to the following one by floating-point error — the same guard
        // MetronomeMath needs for the first click after a seek.
        const double boundary = std::ceil(position - 1.0e-9);
        return (int64_t) std::llround(boundary * quantumSamples);
    }

    /** True if @p boundary falls inside the block starting at @p playheadSamples,
        writing where in the block it lands. A boundary already behind the
        playhead counts as "now" (offset 0) rather than being missed — which is
        what makes a launch requested with no quantization take effect on the
        very next block instead of never. */
    static bool boundaryInBlock(int64_t boundary, int64_t playheadSamples, int numSamples,
                                int& offsetOut) noexcept
    {
        if (boundary >= playheadSamples + (int64_t) numSamples)
            return false;

        offsetOut = (int) (boundary <= playheadSamples ? 0 : boundary - playheadSamples);
        return true;
    }
};

} // namespace looper::engine
