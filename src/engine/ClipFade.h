#pragma once

#include <algorithm>
#include <cmath>

namespace soundsplice::engine
{
/**
    Clip fades: a fade-in and a fade-out applied to an audio clip as it plays,
    without touching its file.

    JUCE-free, and the one place the curves live. Playback multiplies by
    clipFadeGain, and the timeline draws its fade overlay from fadeCurve, so
    what is drawn can't drift from what is heard.
*/

/** The curve a fade follows. The numeric values are written to the project
    file, so they are part of the format: append, never renumber. */
enum class FadeShape
{
    Linear     = 0, // straight in amplitude: predictable, though the quiet end sounds abrupt
    EqualPower = 1, // quarter sine: two of these crossfading hold the combined loudness steady
    SCurve     = 2  // eases in and out: the smoothest start and finish
};

/** Gain along a fade-in of @p shape at @p t, from 0 (the start, silent) to 1
    (the end, full level). @p t is clamped to that range. A fade-out is the
    same curve read backwards. */
inline float fadeCurve(FadeShape shape, double t) noexcept
{
    constexpr double kPi = 3.141592653589793;
    const double x = std::clamp(t, 0.0, 1.0);

    switch (shape)
    {
        case FadeShape::EqualPower: return (float) std::sin(x * kPi * 0.5);
        case FadeShape::SCurve:     return (float) (0.5 - 0.5 * std::cos(x * kPi));
        case FadeShape::Linear:
        default:                    return (float) x;
    }
}

/** A clip's two fades. Lengths are in seconds because a fade covers real
    time in the recording, like the clip's source offset. */
struct ClipFades
{
    double    inSeconds  = 0.0;
    double    outSeconds = 0.0;
    FadeShape inShape    = FadeShape::Linear;
    FadeShape outShape   = FadeShape::Linear;

    bool isNone() const noexcept { return inSeconds <= 0.0 && outSeconds <= 0.0; }

    bool operator==(const ClipFades&) const = default;
};

/** @p fades as they apply to a clip @p clipSeconds long. Fades that add up to
    more than the clip are shortened in proportion so they meet rather than
    overlap: a 3s fade-in and a 1s fade-out on a 2s clip become 1.5s and 0.5s.
    The stored lengths stay as set, so lengthening the clip again brings the
    full fades back. */
inline ClipFades fittedFades(const ClipFades& fades, double clipSeconds) noexcept
{
    ClipFades fitted  = fades;
    fitted.inSeconds  = std::max(0.0, fades.inSeconds);
    fitted.outSeconds = std::max(0.0, fades.outSeconds);

    if (clipSeconds <= 0.0)
    {
        fitted.inSeconds  = 0.0;
        fitted.outSeconds = 0.0;
        return fitted;
    }

    const double total = fitted.inSeconds + fitted.outSeconds;
    if (total > clipSeconds)
    {
        const double scale = clipSeconds / total;
        fitted.inSeconds  *= scale;
        fitted.outSeconds *= scale;
    }

    return fitted;
}

/** The gain @p fitted applies @p secondsIntoClip into a clip @p clipSeconds
    long: 1 outside both fades. @p fitted should already have been through
    fittedFades, which is what keeps the two fades from overlapping. */
inline float clipFadeGain(const ClipFades& fitted, double secondsIntoClip, double clipSeconds) noexcept
{
    float gain = 1.0f;

    if (fitted.inSeconds > 0.0 && secondsIntoClip < fitted.inSeconds)
        gain *= fadeCurve(fitted.inShape, secondsIntoClip / fitted.inSeconds);

    const double fromEnd = clipSeconds - secondsIntoClip;
    if (fitted.outSeconds > 0.0 && fromEnd < fitted.outSeconds)
        gain *= fadeCurve(fitted.outShape, fromEnd / fitted.outSeconds);

    return gain;
}

} // namespace soundsplice::engine
