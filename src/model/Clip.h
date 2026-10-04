#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine/ClipChannels.h"
#include "engine/ClipEnvelope.h"
#include "engine/ClipFade.h"
#include "engine/ClipSpectralEdits.h"
#include "model/Effects.h"

namespace soundsplice::model
{
/** What a clip is, for Essential Sound (model/EssentialSound.h). The
    numeric values are written to the project file: append, never renumber. */
enum class SoundRole
{
    None     = 0,
    Dialogue = 1,
    Music    = 2,
    Sfx      = 3,
    Ambience = 4
};

/** A clip's Essential Sound tag and its task amounts (0-10, by task id). */
struct EssentialSettings
{
    SoundRole                    role = SoundRole::None;
    std::map<std::string, float> amounts;

    bool operator==(const EssentialSettings&) const = default;
};

/** One transcribed word: its text, where it is in the clip's file, and how
    sure the transcriber was (0-1). */
struct TranscriptWord
{
    std::string text;
    double      start      = 0.0; // seconds into the file
    double      end        = 0.0;
    float       confidence = 1.0f;

    bool operator==(const TranscriptWord&) const = default;
};

/** One recording of a stretch of an audio clip: the clip can play any of its
    takes (model/Takes.h). @c shiftSeconds places it against the other takes -
    where in this take's file the clip is, minus where in the active take's -
    so each keeps the timing it was recorded with, and trimming or splitting
    the clip, which moves only the clip's own offset, moves every take alike. */
struct ClipTake
{
    std::string audioFile;
    double      shiftSeconds = 0.0;
    std::string name;

    bool operator==(const ClipTake&) const = default;
};

/** An audio file's clip placed on a track's timeline. Times are in
    quarter-note beats. */
struct Clip
{
    int         id          = 0;
    double      startBeats  = 0.0;
    double      lengthBeats = 4.0;

    std::string audioFile;

    /** Trim for this clip alone, on top of the track fader — the "amplify"
        of a mastering workflow, and what Normalize writes. Non-destructive:
        the file on disk is untouched, so it can be undone and re-set freely. */
    float gainDb = 0.0f;

    /** Where in audioFile this clip starts playing, in seconds. Trimming a
        clip's start or splitting it moves this rather than rewriting the
        file, so the audio before it is still there to bring back. Seconds
        rather than beats because it measures real time in a recording.
        app/ClipWindow.h holds the arithmetic built on it. */
    double sourceOffsetSeconds = 0.0;

    /** A fade-in and a fade-out applied over the clip's audible length as it
        plays. Non-destructive like gainDb: the file is untouched, so a fade
        can be redrawn or removed freely. The curves and the rule for fades
        longer than the clip are in engine/ClipFade.h. */
    engine::ClipFades fades;

    /** Whether each fade was made by an automatic crossfade (REAPER's: two
        clips overlapping on a track fade across the overlap) rather than
        drawn: an automatic one goes when the overlap does, a drawn one is
        never touched. See model::arrangeedit::applyAutoCrossfades. */
    bool autoFadeIn  = false;
    bool autoFadeOut = false;

    /** Which of the file's channels the clip plays: both, one of them on
        every output, or the two swapped. Non-destructive like the fades; see
        engine/ClipChannels.h and model/TrackChannels.h. */
    engine::ClipChannels channels = engine::ClipChannels::Both;

    /** A volume curve drawn on the clip, in seconds into its file so trims
        and splits keep it on the audio it was drawn over. Empty is unity.
        Non-destructive, like the fades; see engine/ClipEnvelope.h. */
    engine::ClipEnvelope envelope;

    /** Spectral edits kept on the clip, boxes of time and frequency turned
        up, down or out, in seconds into its file like the volume curve.
        Non-destructive: what plays is the file with them applied, rendered
        to a cache (app/SpectralRender.h), so any can be removed later. */
    engine::SpectralRegions spectralEdits;

    /** Effects on this clip alone, in order, before its track's own chain:
        the take's reverb or EQ, which shouldn't touch the rest of the track.
        Played live, like the track's, so a change is heard at once. Built-in
        effects and hosted plugins alike; the clip is read ahead by their
        latency so it stays on time, and their tails ring on past its end
        (engine/AudioFilePlayerNode.h). */
    std::vector<EffectSlot> effects;

    /** Every take of this stretch, when there's more than the one playing:
        the active one is takes[activeTake], and audioFile and
        sourceOffsetSeconds above are always its, so nothing that plays a clip
        needs to know about takes. Empty for an ordinary clip. See
        model/Takes.h. */
    std::vector<ClipTake> takes;
    int                   activeTake = 0;

    /** Warp (model/Warp.h): the clip follows the song's tempo, its audio
        stretched from the tempo it was played at, sourceBpm (0 when that
        isn't known). */
    bool   warp      = false;
    double sourceBpm = 0.0;

    /** Essential Sound: what the clip is, and its tasks' amounts, which make
        the slots marked EffectSlot::essential in effects above. */
    EssentialSettings essential;

    /** What's said in it, word by word (Analyze > Transcribe), each timed in
        seconds into its file like the volume curve, so trims, moves and
        splits keep every word on the audio it came from. See
        model/Transcript.h. */
    std::vector<TranscriptWord> transcript;

    bool operator==(const Clip&) const = default;
};

} // namespace soundsplice::model
