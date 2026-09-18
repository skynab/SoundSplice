#pragma once

#include <string>

#include "engine/ClipChannels.h"
#include "engine/ClipEnvelope.h"
#include "engine/ClipFade.h"
#include "engine/ClipSpectralEdits.h"
#include "engine/Pattern.h"

namespace soundsplice::model
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

    /** Where in audioFile this clip starts playing, in seconds. Trimming a
        clip's start or splitting it moves this rather than rewriting the
        file, so the audio before it is still there to bring back. Seconds
        rather than beats because it measures real time in a recording. Audio
        clips only; app/ClipWindow.h holds the arithmetic built on it. */
    double sourceOffsetSeconds = 0.0;

    /** A fade-in and a fade-out applied over the clip's audible length as it
        plays. Non-destructive like gainDb: the file is untouched, so a fade
        can be redrawn or removed freely. Audio clips only; the curves and the
        rule for fades longer than the clip are in engine/ClipFade.h. */
    engine::ClipFades fades;

    /** Which of the file's channels the clip plays: both, one of them on
        every output, or the two swapped. Non-destructive like the fades; see
        engine/ClipChannels.h and model/TrackChannels.h. Audio clips only. */
    engine::ClipChannels channels = engine::ClipChannels::Both;

    /** A volume curve drawn on the clip, in seconds into its file so trims
        and splits keep it on the audio it was drawn over. Empty is unity.
        Non-destructive, like the fades; see engine/ClipEnvelope.h. Audio
        clips only. */
    engine::ClipEnvelope envelope;

    /** Spectral edits kept on the clip, boxes of time and frequency turned
        up, down or out, in seconds into its file like the volume curve.
        Non-destructive: what plays is the file with them applied, rendered
        to a cache (app/SpectralRender.h), so any can be removed later.
        Audio clips only. */
    engine::SpectralRegions spectralEdits;

    bool operator==(const Clip&) const = default;
};

} // namespace soundsplice::model
