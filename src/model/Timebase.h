#pragma once

#include <cmath>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    Keeps audio where it is in time when the tempo changes.

    Clip positions are stored in beats, and at one tempo for the whole song
    that is the same information as seconds. The two only disagree when the
    tempo changes: a MIDI part should follow the beat, but a recording plays
    in real time, so an audio clip that kept its beat position would slide
    away from where it was recorded, taking its automation out of line with
    it. For an audio editor that is simply wrong.

    So a tempo change rescales the beat positions of everything on audio
    tracks (clip starts and lengths, and their automation) by new/old, which
    leaves each one at the same time in seconds. Instrument tracks, the
    session grid and the master lane are musical and stay on their beats. A
    clip's source offset and fades are already in seconds and need nothing.
*/
inline void retimeAudioForTempoChange(Song& song, double oldBpm, double newBpm)
{
    if (! (oldBpm > 0.0) || ! (newBpm > 0.0) || std::abs(newBpm - oldBpm) < 1.0e-12)
        return;

    const double factor = newBpm / oldBpm;

    for (auto& track : song.tracks)
    {
        if (track.type != TrackType::Audio)
            continue;

        for (auto& clip : track.clips)
        {
            clip.startBeats  *= factor;
            clip.lengthBeats *= factor;
        }

        for (auto& entry : track.automation)
            entry.second.scaleBeats(factor);
    }
}

} // namespace soundsplice::model
