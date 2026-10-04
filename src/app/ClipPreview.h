#pragma once

#include <algorithm>

namespace soundsplice
{
/**
    How much of an audio clip's width its waveform actually covers, as a
    fraction of that width.

    An audio clip does not loop — AudioClipSlot plays the file once from the
    clip's start and goes silent when the file runs out or the clip's window
    ends, whichever comes first. So a waveform stretched to fill the clip
    would be a picture of something the clip doesn't do: a two-second file in
    an eight-beat clip at 120bpm is heard for the first half and silent for
    the rest, and the drawing has to say so.

    The reverse case is just as real. A file longer than its clip is audible
    only up to the clip's end, so the waveform is drawn up to there and the
    rest of the file isn't shown at all.

    Returns 0 when there is nothing to draw, which callers use to skip the
    work entirely.
*/
inline double audioClipDrawnFraction(double fileSeconds, double clipLengthBeats, double secondsPerBeat)
{
    if (fileSeconds <= 0.0 || clipLengthBeats <= 0.0 || secondsPerBeat <= 0.0)
        return 0.0;

    const double clipSeconds = clipLengthBeats * secondsPerBeat;
    if (clipSeconds <= 0.0)
        return 0.0;

    return std::min(1.0, fileSeconds / clipSeconds);
}

/** The seconds of a file an audio clip actually plays — the same rule as
    audioClipDrawnFraction, expressed as a duration so a thumbnail can be
    asked for exactly that range. */
inline double audioClipAudibleSeconds(double fileSeconds, double clipLengthBeats, double secondsPerBeat)
{
    if (fileSeconds <= 0.0 || clipLengthBeats <= 0.0 || secondsPerBeat <= 0.0)
        return 0.0;

    return std::min(fileSeconds, clipLengthBeats * secondsPerBeat);
}

} // namespace soundsplice
