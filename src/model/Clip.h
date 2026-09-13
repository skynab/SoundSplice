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

    bool operator==(const Clip&) const = default;
};

} // namespace looper::model
