#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Pattern.h"

namespace looper::engine
{
/**
    Edit-time operations on a pattern's notes.

    JUCE-free so the timing math is unit-tested headless, like SequencerMath
    and MetronomeMath. These rewrite real note positions rather than acting as
    a playback-time "feel" parameter, which keeps the engine unchanged and the
    result visible and editable in the grid — consistent with this project's
    rule that generated/derived musical data stays editable rather than hiding
    in the signal path.
*/
struct NoteOps
{
    /** The grid position of step @p step, given a swing amount.

        Swing delays every *odd* step by a fraction of a step, which is what
        turns a straight 16th grid into a shuffled one. Expressing the grid as
        a function of the step index (rather than nudging notes after
        quantizing them) is what makes quantizeNotes idempotent at any swing
        amount: a note already sitting on a swung grid position quantizes to
        exactly where it already is. */
    static double gridPosition(int64_t step, double stepBeats, double swingAmount) noexcept
    {
        const double base   = (double) step * stepBeats;
        const bool   offBeat = (step % 2) != 0;
        return offBeat ? base + swingAmount * stepBeats : base;
    }

    /** Snaps @p notes onto the (optionally swung) grid.

        @p swingAmount is the fraction of a step that off-beats are delayed by:
        0 is a straight grid, ~0.25 a light shuffle, ~0.66 a hard triplet
        feel. Clamped below one step so notes can never be pushed past the
        step that follows them, which would reorder the part.

        @p selection lists indices to affect; an empty selection means the
        whole pattern, so the operation is useful before anyone has learned
        the selection gesture. */
    static void quantizeNotes(std::vector<Note>& notes, double stepBeats, double swingAmount,
                              const std::vector<int>& selection = {})
    {
        if (stepBeats <= 0.0 || notes.empty())
            return;

        const double swing = std::clamp(swingAmount, 0.0, 0.9);

        auto quantizeOne = [&](Note& note)
        {
            // Search the steps either side of where the note sits rather than
            // rounding directly: with swing the grid isn't evenly spaced, so
            // the nearest position isn't always the nearest step index.
            const int64_t nearestStep = (int64_t) std::floor(note.startBeats / stepBeats);

            double best         = gridPosition(nearestStep, stepBeats, swing);
            double bestDistance = std::abs(note.startBeats - best);

            for (int64_t step = nearestStep - 1; step <= nearestStep + 2; ++step)
            {
                if (step < 0)
                    continue;
                const double candidate = gridPosition(step, stepBeats, swing);
                const double distance  = std::abs(note.startBeats - candidate);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best         = candidate;
                }
            }

            note.startBeats = std::max(0.0, best);
        };

        if (selection.empty())
        {
            for (auto& note : notes)
                quantizeOne(note);
            return;
        }

        for (int index : selection)
            if (index >= 0 && index < (int) notes.size())
                quantizeOne(notes[(size_t) index]);
    }
};

} // namespace looper::engine
