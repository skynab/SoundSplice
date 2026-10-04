#pragma once

#include <algorithm>
#include <cmath>

namespace soundsplice::engine
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

} // namespace soundsplice::engine
