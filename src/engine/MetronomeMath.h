#pragma once

#include <cmath>
#include <cstdint>

namespace looper::engine
{
/**
    Which beats begin inside a block, and which of those are downbeats.

    JUCE-free so the boundary math is unit-tested headless — the same reason
    SequencerMath and TempoMap are separated out. Getting this wrong is the
    difference between a click that sits on the grid and one that drifts, and
    it's much easier to prove here than by ear.
*/
struct MetronomeMath
{
    /** The index of the first beat starting at or after @p playheadSamples.
        Beat 0 is the song start, so beat indices are absolute, not
        block-relative. */
    static int64_t firstBeatAtOrAfter(int64_t playheadSamples, double samplesPerBeat) noexcept
    {
        if (samplesPerBeat <= 0.0)
            return 0;

        // The epsilon keeps a playhead sitting exactly on a beat from being
        // pushed to the next one by floating-point error, which would drop
        // the very first click after a seek to a bar line.
        const double beatAtPlayhead = (double) playheadSamples / samplesPerBeat;
        return (int64_t) std::ceil(beatAtPlayhead - 1.0e-9);
    }

    /** Where @p beatIndex falls relative to the block starting at
        @p playheadSamples. May be negative or past the block; the caller
        checks (see ticksInBlock's usage in Metronome::process). */
    static int64_t offsetOfBeat(int64_t beatIndex, int64_t playheadSamples, double samplesPerBeat) noexcept
    {
        return (int64_t) std::llround((double) beatIndex * samplesPerBeat) - playheadSamples;
    }

    /** True if @p beatIndex is the first beat of a bar, given how many
        quarter-note beats a bar holds (TempoMap::quartersPerBar — 4 in 4/4,
        3 in 3/4, 6 in 6/8). Downbeats get the accented click. */
    static bool isDownbeat(int64_t beatIndex, double quartersPerBar) noexcept
    {
        if (quartersPerBar <= 0.0)
            return false;

        // Beats can land off a whole number of quarters per bar (6/8 gives
        // 3.0, but 7/8 gives 3.5), so this is a modulo on doubles rather than
        // integers, with a tolerance for the accumulated rounding.
        const double positionInBar = std::fmod((double) beatIndex, quartersPerBar);
        return positionInBar < 1.0e-6 || (quartersPerBar - positionInBar) < 1.0e-6;
    }
};

} // namespace looper::engine
