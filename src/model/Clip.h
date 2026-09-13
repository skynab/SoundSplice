#pragma once

#include <string>

#include "engine/Pattern.h"

namespace looper::model
{
enum class ClipType
{
    Instrument, // holds a MIDI Pattern
    Audio       // references an audio file
};

/** A clip placed on a track's timeline. Times are in quarter-note beats. */
struct Clip
{
    int         id          = 0;
    ClipType    type        = ClipType::Instrument;
    double      startBeats  = 0.0;
    double      lengthBeats = 4.0;

    engine::Pattern pattern;   // used when type == Instrument
    std::string     audioFile; // used when type == Audio

    /** Trim for this clip alone, on top of the track fader — the "amplify"
        of a mastering workflow, and what Normalize writes. Non-destructive:
        the file on disk is untouched, so it can be undone and re-set freely.
        Currently applied to Audio clips only; an Instrument clip's level is
        already expressible per note via velocity. */
    float gainDb = 0.0f;

    /** The tempo this clip's audio was recorded/rendered at, in BPM, or 0 if
        it isn't known. Detected on import (see engine::detectTempo) and
        overridable by hand, because a detector is a guess and the user may
        simply know better.

        Stored separately from warpEnabled so that switching warp off and on
        again doesn't throw away the answer — and so a clip can carry a known
        tempo without being warped, which is what "set the project tempo from
        this clip" needs. Audio clips only; an Instrument clip's notes are
        already expressed in beats and so follow the tempo by construction. */
    double sourceBpm = 0.0;

    /** Whether this clip stretches to follow the project tempo.

        Off by default, which is what keeps every project made before warping
        existed sounding exactly as it did: an unwarped audio clip plays at its
        own rate, the behaviour AudioFilePlayerNode has always had. */
    bool warpEnabled = false;

    bool operator==(const Clip&) const = default;
};

} // namespace looper::model
