#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "engine/GuitarChords.h"
#include "engine/SequencerMath.h"
#include "model/Song.h"

namespace looper
{
/** What a chord stamp would do, or why it can't. */
struct ChordStampPlan
{
    bool                      ok = false;
    std::string               problem;  // empty when ok; ready to show the user
    std::vector<engine::Note> notes;    // already offset to where they land
    double                    atBeats = 0.0;
};

/**
    Works out what stamping a chord would put in the clip, and where — or which
    of the several ways it can decline applies.

    Extracted from MainComponent so it can be tested at all. The reported bug
    that started this ("the guitar chord buttons don't seem to be doing
    anything") took a long time to pin down precisely because this decision
    lived inside a component no test can construct: MainComponent writes to the
    real settings file and saves the dock layout when it is destroyed, so
    building one in a test would overwrite the user's workspace. Everything
    that could be checked without it was — the buttons are children, sized,
    hit-testable, and reach their callback — and the fault was downstream of
    all of it.

    Every refusal carries the sentence to show. Silent refusals are what made
    the original report so hard to place: a button that declines without saying
    so is indistinguishable from one that is broken.

    JUCE-free, so the placement arithmetic and the guards are testable
    headlessly like the rest of the note maths.
*/
inline ChordStampPlan planChordStamp(const model::Song& song, int trackIndex, int clipIndex,
                                     const engine::ChordShape& shape, int fretOffset,
                                     const engine::StrumSettings& strum,
                                     double playheadBeats, double beatsPerBar,
                                     unsigned int seed)
{
    ChordStampPlan plan;

    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        plan.problem = "Select a guitar track first";
        return plan;
    }

    const auto& track = song.tracks[(size_t) trackIndex];

    if (track.type != model::TrackType::Guitar)
    {
        plan.problem = "\"" + (track.name.empty() ? std::string("This track") : track.name)
                     + "\" isn't a guitar track — chords need one";
        return plan;
    }

    if (clipIndex < 0 || clipIndex >= (int) track.clips.size())
    {
        plan.problem = "\"" + (track.name.empty() ? std::string("This track") : track.name)
                     + "\" has no clip selected to put the chord in";
        return plan;
    }

    const auto&  clip = track.clips[(size_t) clipIndex];
    const double bar  = beatsPerBar > 0.0 ? beatsPerBar : 4.0;

    // Land it on the bar the playhead is in, so stamping while stopped puts
    // the chord where the transport is rather than always at the start. The
    // position is wrapped into the pattern because a clip loops.
    const double intoClip = playheadBeats - clip.startBeats;
    const double wrapped  = clip.pattern.lengthBeats > 0.0
                              ? engine::wrapPositive(intoClip, clip.pattern.lengthBeats)
                              : 0.0;
    plan.atBeats = std::floor(wrapped / bar) * bar;

    auto struck = engine::GuitarChords::strumChord(shape, track.guitarSettings.tuning.data(),
                                                   fretOffset, 0.0, bar, song.bpm, strum, seed);

    for (auto note : struck)
    {
        note.startBeats += plan.atBeats;

        // A note past the pattern's end would be invisible in the editor and
        // silent in the sequencer, so it isn't added at all.
        if (note.startBeats < clip.pattern.lengthBeats)
            plan.notes.push_back(note);
    }

    if (plan.notes.empty())
    {
        // Not a "no room" case, though it looks like one. atBeats is derived
        // by wrapping the playhead into the pattern, so it is always less than
        // the pattern's length and the first note of the chord always fits.
        // Reaching here means the shape itself had no strings to play, which
        // the built-in shapes never do but a hand-made one could.
        plan.problem = std::string("A ") + shape.name + " shape has no strings to play";
        return plan;
    }

    plan.ok = true;
    return plan;
}

} // namespace looper
