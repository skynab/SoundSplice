#pragma once

#include <algorithm>

#include "model/Clip.h"

namespace soundsplice::app
{
/**
    Where a clip's volume curve is drawn over it, and what a point on it
    means: the two directions of every envelope gesture in the arrangement.

    Height is gain, linear, with silence at the bottom of the clip, unity
    halfway up and kEnvelopeTopGain at the top, so the unity line sits in the
    middle with room to raise and lower a passage by the same distance. Time
    is seconds into the clip's file, the curve's own units (see
    engine::ClipEnvelope).

    JUCE-free, so the mapping is tested on its own.
*/

/** The gain at the top of a clip's box. */
inline constexpr float kEnvelopeTopGain = 2.0f;

/** The gain a height means, in a box starting at @p top, @p height tall. */
inline float gainForY(float y, float top, float height)
{
    if (! (height > 0.0f))
        return 1.0f;

    return std::clamp((top + height - y) / height, 0.0f, 1.0f) * kEnvelopeTopGain;
}

/** The height a gain is drawn at: above kEnvelopeTopGain it's pinned at the
    top, since the box has no room above that. */
inline float yForGain(float gain, float top, float height)
{
    return top + height - std::clamp(gain / kEnvelopeTopGain, 0.0f, 1.0f) * height;
}

/** Seconds into @p clip's file for a beat on the timeline. */
inline double sourceSecondsAtBeat(const model::Clip& clip, double beat, double bpm)
{
    return bpm > 0.0 ? clip.sourceOffsetSeconds + (beat - clip.startBeats) * 60.0 / bpm
                     : clip.sourceOffsetSeconds;
}

/** The beat on the timeline where second @p seconds of @p clip's file plays. */
inline double beatAtSourceSeconds(const model::Clip& clip, double seconds, double bpm)
{
    return clip.startBeats + (seconds - clip.sourceOffsetSeconds) * bpm / 60.0;
}

/** @p seconds kept within the part of the file @p clip plays. */
inline double clampToClipSource(const model::Clip& clip, double seconds, double bpm)
{
    const double end = clip.sourceOffsetSeconds + clip.lengthBeats * 60.0 / std::max(1.0e-9, bpm);
    return std::clamp(seconds, clip.sourceOffsetSeconds, std::max(clip.sourceOffsetSeconds, end));
}

} // namespace soundsplice::app
