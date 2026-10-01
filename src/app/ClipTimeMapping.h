#pragma once

#include <cmath>
#include <cstdint>

#include "model/BeatClock.h"
#include "model/Song.h"

namespace soundsplice::app
{
/**
    Where a beat on the timeline falls in an audio clip's file, and back: what
    anything that works on the audio under a timeline position needs, such as
    moving a time selection's edges to zero crossings.

    JUCE-free, and tested on its own, since an off-by-one here means an edge
    that lands a sample away from where the audio says it should.
*/

/** The audio clip on @p track that plays at @p beat, or nullptr. A clip's
    two ends both count, so an edge sitting on a clip boundary still finds
    audio; where two clips meet, the earlier one wins. */
inline const model::Clip* audioClipAt(const model::Track& track, double beat)
{
    const model::Clip* found = nullptr;

    for (const auto& clip : track.clips)
    {
        if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
            continue;

        if (beat >= clip.startBeats && beat <= clip.startBeats + clip.lengthBeats
            && (found == nullptr || clip.startBeats < found->startBeats))
            found = &clip;
    }

    return found;
}

/** The sample of @p clip's file that plays at @p beat. */
inline std::int64_t fileFrameAt(const model::Clip& clip, double beat, double sampleRate, const model::BeatClock& clock)
{
    if (! clock.valid())
        return 0;

    const double seconds = clip.sourceOffsetSeconds + clock.secondsBetween(clip.startBeats, beat);
    return (std::int64_t) std::llround(seconds * sampleRate);
}

/** The beat at which sample @p frame of @p clip's file plays. */
inline double beatForFileFrame(const model::Clip& clip, std::int64_t frame, double sampleRate,
                               const model::BeatClock& clock)
{
    if (sampleRate <= 0.0 || ! clock.valid())
        return clip.startBeats;

    return clip.startBeats + clock.beatsAfter(clip.startBeats, (double) frame / sampleRate - clip.sourceOffsetSeconds);
}

} // namespace soundsplice::app
