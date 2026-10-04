#pragma once

#include <algorithm>
#include <vector>

#include "model/Song.h"
#include "model/TempoChanges.h"
#include "model/WarpFactor.h"

namespace soundsplice::model
{
/**
    Warp: an audio clip that follows the song's tempo. A warped clip keeps
    its place in beats, like a MIDI part, and its audio is stretched (pitch
    kept) by its source tempo (Clip::sourceBpm, which tempo detection or the
    user gives it) over the song's tempo where the clip starts - so a loop at
    100 bpm plays at 120 in a 120 bpm song, and goes on matching when the
    tempo is changed.

    A warped clip's offset into its file, its volume curve and its takes'
    shifts are in the stretched time, the time it plays in; that's what every
    edit to a clip already measures, so none of them has to know about warp.
    Only what reads the file itself - drawing its waveform, editing its
    samples - maps back to the file's own time, by dividing by the factor.

    One stretch per clip, from the tempo at its start: a clip spanning a tempo
    change follows the first tempo throughout.
*/
namespace warpedit
{
    inline double factorFor(const Song& song, const Clip& clip)
    {
        return factorUnder(tempoedit::mapFor(song), clip);
    }

    inline Clip* findClip(Song& song, int trackId, int clipId)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr)
            return nullptr;
        const auto it = std::find_if(track->clips.begin(), track->clips.end(), [clipId](const Clip& c) { return c.id == clipId; });
        return it != track->clips.end() ? &*it : nullptr;
    }

    inline const Clip* findClip(const Song& song, int trackId, int clipId)
    {
        return findClip(const_cast<Song&>(song), trackId, clipId);
    }

    /**
        Warps clip @p clipId (or stops warping it). Its audio stays the same
        stretch of its file: the window grows or shrinks to the time that
        stretch now plays for, and what's measured in stretched time is
        scaled with it. False if it isn't an audio clip, or warping is asked
        for without a source tempo to warp from.
    */
    inline bool setWarp(Song& song, int trackId, int clipId, bool warp)
    {
        auto* clip = findClip(song, trackId, clipId);
        if (clip == nullptr || (warp && clip->sourceBpm <= 0.0) || clip->warp == warp)
            return false;

        const auto   clock   = clockFor(song);
        const double seconds = clock.secondsBetween(clip->startBeats, clip->startBeats + clip->lengthBeats);
        const double before  = factorFor(song, *clip);
        clip->warp           = warp;
        const double after   = factorFor(song, *clip);

        rescale(*clip, after / before);
        clip->lengthBeats = std::max(0.0, clock.beatsAfter(clip->startBeats, seconds * after / before));
        return true;
    }

    /** Gives clip @p clipId its source tempo. A warped clip is restretched to
        it, keeping its length in beats - the point of warping. */
    inline bool setSourceTempo(Song& song, int trackId, int clipId, double bpm)
    {
        auto* clip = findClip(song, trackId, clipId);
        if (clip == nullptr || ! (bpm > 0.0))
            return false;

        const double before = factorFor(song, *clip);
        clip->sourceBpm     = bpm;
        rescale(*clip, factorFor(song, *clip) / before);
        return true;
    }
}

} // namespace soundsplice::model
