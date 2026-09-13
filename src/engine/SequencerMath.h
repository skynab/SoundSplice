#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/** Where a loop should end, given how far the arranged content reaches.

    Rounded up to a whole bar so the loop lands on a bar line rather than
    mid-phrase, and never shorter than one bar — a zero-length loop region
    would either stall the transport or divide by zero downstream, and an
    empty song is exactly when that would happen. */
inline double loopEndForContent(double contentEndBeats, double beatsPerBar) noexcept
{
    const double bar = beatsPerBar > 0.0 ? beatsPerBar : 4.0;
    if (! (contentEndBeats > 0.0))
        return bar;

    const double bars = std::ceil(contentEndBeats / bar);
    return (bars < 1.0 ? 1.0 : bars) * bar;
}

/** True when pressing play from @p playheadBeats should rewind to the start
    first, because the transport is sitting at the end of what was arranged.

    A play button at the end of a song that resumes into silence and stops
    again immediately reads as a button that does nothing — the same thing a
    media player avoids by restarting.

    The tolerance matters. Playback is stopped at the end by a check on the UI
    timer, and the beat position is reconstructed from a sample count, so the
    stored playhead can land a hair either side of the end. Without a margin,
    landing a hair short means play resumes, hits the end within a millisecond
    and stops again. A thousandth of a beat is half a millisecond at 120bpm —
    far below anything audible, and far above the rounding.

    @p contentEndBeats of zero means nothing is arranged, so there is no end
    to be at and no reason to rewind. */
inline bool shouldRestartFromStart(double playheadBeats, double contentEndBeats) noexcept
{
    constexpr double tolerance = 1.0e-3;
    return contentEndBeats > 0.0 && playheadBeats >= contentEndBeats - tolerance;
}

/** How many beats a duration in seconds occupies at a given tempo.

    One definition because there were two, and they disagreed: importing audio
    sized its clip from the file's real duration while recording used a flat
    four beats whatever had been played. That single difference collapsed the
    loop region to one bar after a take, so playback wrapped seconds in and
    the transport wouldn't run past a recording it had just made. */
inline double beatsForSeconds(double seconds, double bpm) noexcept
{
    if (seconds <= 0.0 || bpm <= 0.0)
        return 0.0;

    return seconds * bpm / 60.0;
}

/** The frequency, in Hz, of something that completes one cycle every
    @p beatsPerCycle beats at @p bpm — turns a tempo-relative rate (a wobble's
    1/16 note, a synced LFO's dotted eighth) into the Hz a DSP object actually
    runs at. A free-running Hz rate drifts out of the groove the moment the
    song's tempo changes; this is what keeps a wobble locked to the bar
    instead.

    Returns 0 for a rate or tempo that doesn't make sense, so a caller can
    treat that as "don't advance the phase" rather than divide by zero. */
inline double hzForBeatDivision(double bpm, double beatsPerCycle) noexcept
{
    if (bpm <= 0.0 || beatsPerCycle <= 0.0)
        return 0.0;

    return bpm / (60.0 * beatsPerCycle);
}

/** Positive floating-point modulo: result is always in [0, length). */
inline double wrapPositive(double x, double length) noexcept
{
    if (length <= 0.0)
        return 0.0;

    const double m = std::fmod(x, length);
    return m < 0.0 ? m + length : m;
}

/**
    Decides whether a note edge (a start or end time, in pattern samples) falls
    within the block that begins at @p blockStartInPattern and spans @p numSamples,
    accounting for the pattern looping with period @p length. If so, writes the
    in-block sample @p offset and returns true.

    Blocks are always shorter than the pattern, so each edge is hit at most once
    per block — which makes the modular comparison unambiguous.
*/
inline bool edgeInBlock(double edgeSample, double blockStartInPattern, double length,
                        int numSamples, int& offset) noexcept
{
    const double delta = wrapPositive(edgeSample - blockStartInPattern, length);
    if (delta < (double) numSamples)
    {
        offset = (int) delta;
        return true;
    }
    return false;
}

/**
    As edgeInBlock, but in **beats**.

    Scheduling moved to beats because a block's length in samples is fixed
    while its length in beats is not - once tempo can change, "how many samples
    is a beat" has no answer without a position, so the modulo that wraps a
    looping pattern has to happen in musical time. The single conversion back
    to samples is here, at the edge.

    @p blockLengthBeats is how much musical time this block covers, and
    @p numSamples how many samples that is.
*/
inline bool edgeInBlockBeats(double edgeBeat, double blockStartBeat, double patternLengthBeats,
                             double blockLengthBeats, int numSamples, int& offset) noexcept
{
    if (blockLengthBeats <= 0.0 || numSamples <= 0 || patternLengthBeats <= 0.0)
        return false;

    const double delta = wrapPositive(edgeBeat - blockStartBeat, patternLengthBeats);
    if (delta < blockLengthBeats)
    {
        const double position = delta / blockLengthBeats * (double) numSamples;
        offset = (int) std::clamp(position, 0.0, (double) (numSamples - 1));
        return true;
    }

    return false;
}

} // namespace looper::engine
