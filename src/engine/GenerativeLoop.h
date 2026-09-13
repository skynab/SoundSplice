#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/DefaultContent.h"
#include "engine/NoteOps.h"
#include "engine/Pattern.h"
#include "engine/Scale.h"

namespace looper::engine
{
/**
    The maximally-even distribution of @p pulses onsets across @p steps
    slots, rotated by @p rotation steps — the partition Bjorklund's algorithm
    produces, computed here via integer floor division rather than its
    recursive bucket merge: slot i gets an onset exactly when
    floor(i*pulses/steps) differs from the same value at slot i-1, which
    spreads onsets as evenly as the fraction pulses/steps allows. This is
    what turns "3 kicks in 8 steps" into a tresillo rather than three kicks
    in a row — the shape that makes a generated rhythm sound intentional
    rather than random.
*/
inline std::vector<bool> euclideanRhythm(int steps, int pulses, int rotation = 0)
{
    if (steps <= 0)
        return {};

    pulses = std::clamp(pulses, 0, steps);

    auto floorDiv = [](int64_t a, int64_t b) -> int
    {
        return (int) (a >= 0 ? a / b : -(((-a) + b - 1) / b));
    };

    std::vector<bool> pattern((size_t) steps, false);
    if (pulses > 0)
    {
        int previous = floorDiv(-(int64_t) pulses, steps);
        for (int i = 0; i < steps; ++i)
        {
            const int current = floorDiv((int64_t) i * pulses, steps);
            pattern[(size_t) i] = current != previous;
            previous = current;
        }
    }

    if (rotation != 0)
    {
        const int r = ((rotation % steps) + steps) % steps;
        std::rotate(pattern.begin(), pattern.begin() + r, pattern.end());
    }

    return pattern;
}

/**
    Clamps every note's length so it never reaches past @p pattern's own end,
    nor into the start of the next note that shares its pitch.

    Swing (NoteOps::quantizeNotes) only ever moves a note's *start* later; it
    never touches length. A note whose length was sized against its
    original, unswung position can therefore end up reaching further than
    intended once swung — past the pattern's loop point (handled here the
    same way it always was), or, just as easily, into the start of a *later*
    note of the same pitch. Overlap between different pitches is normal
    polyphony; overlap between two notes of the same pitch is not something
    MIDI note-on/note-off represents cleanly — PatternPlayback tracks "is
    this pitch sounding" as one flag per note number, so an early note's
    note-off can land after a later same-pitch note has already started,
    turning off the wrong instance and leaving a voice with no note-off
    coming — an audible hang.

    Assumes @p pattern's notes are already sorted ascending by startBeats
    (both generators either build them in that order or sort before calling
    this), so each note only needs to look forward for the nearest
    same-pitch neighbour.
*/
inline void clampNoteLengths(Pattern& pattern)
{
    for (size_t i = 0; i < pattern.notes.size(); ++i)
    {
        auto&  note   = pattern.notes[i];
        double maxEnd = pattern.lengthBeats;

        for (size_t j = i + 1; j < pattern.notes.size(); ++j)
        {
            if (pattern.notes[j].noteNumber == note.noteNumber)
            {
                maxEnd = std::min(maxEnd, pattern.notes[j].startBeats);
                break; // ascending order: this is the nearest same-pitch neighbour
            }
        }

        note.lengthBeats = std::max(0.0, std::min(note.lengthBeats, maxEnd - note.startBeats));
    }
}

/**
    Parameters for a generated drum loop. @p kickNote/@p snareNote/@p hatNote
    default to the same GM-ish numbers DefaultContent.h's starter beat uses,
    but callers should pass the target track's *actual* assigned pad notes
    (see model::DrumKit) so the generated pattern plays the kit that's really
    there rather than assuming the factory default.
*/
struct DrumLoopParams
{
    int      bars        = 1;
    int      stepsPerBar = 16;
    double   density     = 0.5; // 0..1
    double   swing       = 0.0; // 0..0.9, forwarded to NoteOps::quantizeNotes
    unsigned seed        = 1;
    int      kickNote    = kDefaultKickNote;
    int      snareNote   = kDefaultSnareNote;
    int      hatNote     = kDefaultHatNote;
};

/**
    A seeded, density-controlled drum loop: a steady backbeat (snare on 2 and
    4, fixed — it's what makes it read as a beat rather than a fill) under a
    kick and hat whose note counts scale with density and jitter slightly
    with the seed, so clicking "generate" again gives a genuinely different
    take rather than only re-humanized velocities.

    The kick always starts on beat 1 (rotation 0) and the snare's Euclidean
    pattern is rotated by a quarter of a bar, which for pulses=2 places its
    two hits on beats 2 and 4 — the standard backbeat position — rather than
    beats 1 and 3, which euclideanRhythm(2, steps, 0) would otherwise give.
*/
inline Pattern generateDrumLoop(const DrumLoopParams& params)
{
    const int    bars        = std::max(1, params.bars);
    const int    stepsPerBar = std::max(4, params.stepsPerBar);
    const int    steps       = stepsPerBar * bars;
    const double density     = std::clamp(params.density, 0.0, 1.0);

    Pattern pattern;
    pattern.lengthBeats = 4.0 * bars;
    const double stepBeats = pattern.lengthBeats / (double) steps;

    std::minstd_rand                      rng(params.seed);
    std::uniform_real_distribution<float> velocityJitter(-0.08f, 0.08f);
    std::uniform_int_distribution<int>    pulseJitter(-1, 1);

    auto addVoice = [&](int pulses, int rotation, int note, float baseVelocity)
    {
        pulses = std::clamp(pulses, 1, steps);
        const auto onsets = euclideanRhythm(steps, pulses, rotation);
        for (int i = 0; i < steps; ++i)
        {
            if (! onsets[(size_t) i])
                continue;

            Note n;
            n.startBeats  = (double) i * stepBeats;
            n.lengthBeats = stepBeats;
            n.noteNumber  = note;
            n.velocity    = (float) std::clamp((double) baseVelocity + (double) velocityJitter(rng), 0.05, 1.0);
            pattern.notes.push_back(n);
        }
    };

    const int kickPulses  = 2 * bars + (int) std::lround(density * 2.0 * (double) bars) + pulseJitter(rng);
    const int hatPulses   = 4 * bars + (int) std::lround(density * 8.0 * (double) bars) + pulseJitter(rng);
    const int snarePulses = 2 * bars; // a steady backbeat; density/seed vary the kick and hat, not this

    addVoice(kickPulses, 0, params.kickNote, 0.95f);
    addVoice(snarePulses, stepsPerBar / 4, params.snareNote, 0.85f);
    addVoice(hatPulses, 0, params.hatNote, 0.6f);

    // Swing shifts the whole kit's off-beats together (a groove template
    // applies uniformly, not per-drum), so it's applied once across every
    // voice's notes rather than inside addVoice. Sorting has to come after:
    // swinging can reorder one voice's notes relative to another's even
    // though NoteOps::quantizeNotes guarantees a note is never pushed past
    // its own next grid step.
    NoteOps::quantizeNotes(pattern.notes, stepBeats, params.swing);

    std::sort(pattern.notes.begin(), pattern.notes.end(),
              [](const Note& a, const Note& b) { return a.startBeats < b.startBeats; });

    // Swing can shift a voice's note later without shortening it - clamp
    // rather than reject, so a swung loop is still exactly as long as it
    // claims to be and never overlaps a later hit on the same pad. Sorting
    // has to happen first: this assumes ascending order by startBeats.
    clampNoteLengths(pattern);

    return pattern;
}

/** Parameters for a generated melodic loop. */
struct MelodicLoopParams
{
    int      bars        = 1;
    int      stepsPerBar = 16;
    Scale    scale;
    double   density     = 0.6; // 0..1: onset density and, loosely, phrase busyness
    double   swing       = 0.0; // 0..0.9, forwarded to NoteOps::quantizeNotes

    /** 0..1: how often an onset is harmonised rather than played as a single
        note. At 0 every onset is one note, which is what this generator did
        before harmony existed. Non-zero by default so a generated part has
        some vertical interest on its own — see generateMelodicLoop. */
    double   harmony     = 0.3;

    unsigned seed        = 1;
};

/**
    A seeded, scale-constrained melodic loop: a Euclidean onset rhythm (so
    the phrasing has real shape rather than a note on every step) walked over
    scale degrees by a seeded random walk, clamped to roughly an octave
    either side of the root so it wanders without changing register
    entirely. Every note this returns satisfies isInScale(note, params.scale)
    by construction, since pitches only ever come from degreeToNote.

    Some onsets are harmonised (see MelodicLoopParams::harmony) — a third,
    or a third and a fifth, stacked on the walked degree and sounding with
    it. Stacking by *scale degree* rather than by a fixed semitone interval
    is what keeps the harmony in key: degree+2 is a major third over some
    degrees of a major scale and a minor third over others, exactly as
    harmonising within a key does on paper. A fixed +4 semitones would leave
    the scale on half of them.
*/
inline Pattern generateMelodicLoop(const MelodicLoopParams& params)
{
    const int    bars        = std::max(1, params.bars);
    const int    stepsPerBar = std::max(4, params.stepsPerBar);
    const int    steps       = stepsPerBar * bars;
    const double density     = std::clamp(params.density, 0.0, 1.0);

    Pattern pattern;
    pattern.lengthBeats = 4.0 * bars;
    const double stepBeats = pattern.lengthBeats / (double) steps;

    const int minPulses = std::max(1, 2 * bars);
    const int pulses    = std::clamp(minPulses + (int) std::lround(density * (double) (steps - minPulses)),
                                     1, steps);
    const auto onsets = euclideanRhythm(steps, pulses, 0);

    std::vector<int> onsetSteps;
    onsetSteps.reserve((size_t) pulses);
    for (int i = 0; i < steps; ++i)
        if (onsets[(size_t) i])
            onsetSteps.push_back(i);

    std::minstd_rand                       rng(params.seed);
    std::uniform_int_distribution<int>     walkStep(-2, 2);
    std::uniform_real_distribution<float>  velocityJitter(-0.1f, 0.1f);
    std::uniform_real_distribution<double> chance(0.0, 1.0);

    const int    scaleSize = (int) intervalsForScale(params.scale.type).size();
    const double harmony   = std::clamp(params.harmony, 0.0, 1.0);
    int          degree    = 0;

    for (size_t idx = 0; idx < onsetSteps.size(); ++idx)
    {
        degree = std::clamp(degree + walkStep(rng), -scaleSize, scaleSize);

        const int startStep = onsetSteps[idx];
        const int nextStep  = idx + 1 < onsetSteps.size() ? onsetSteps[idx + 1] : steps;

        Note n;
        n.startBeats  = (double) startStep * stepBeats;
        n.lengthBeats = std::max(stepBeats * 0.5, (double) (nextStep - startStep) * stepBeats * 0.9);
        n.noteNumber  = degreeToNote(params.scale, degree);
        n.velocity    = (float) std::clamp(0.75 + (double) velocityJitter(rng), 0.1, 1.0);
        pattern.notes.push_back(n);

        // Harmony is drawn every onset, not only the harmonised ones, so a
        // given seed walks the same melody at every harmony setting - the
        // setting adds notes to a part rather than generating a different
        // one, which is what makes it auditionable against itself.
        const double roll = chance(rng);
        if (roll < harmony)
        {
            // A third alone most of the time, the full triad occasionally:
            // constant triads read as a chord pad rather than a harmonised
            // line, and the ask was for *some* onsets to stack up.
            const bool triad = roll < harmony * 0.4;

            // Appended straight after their melody note, which keeps
            // pattern.notes sorted ascending by startBeats (they share one)
            // - clampNoteLengths relies on that ordering. It also handles
            // the one collision that matters here: a harmony pitch that a
            // later onset's melody lands on too.
            for (int interval : { 2, 4 })
            {
                if (interval == 4 && ! triad)
                    break;

                Note h        = n;
                h.noteNumber  = degreeToNote(params.scale, degree + interval);
                h.velocity    = (float) std::clamp((double) n.velocity * 0.8, 0.1, 1.0);
                pattern.notes.push_back(h);
            }
        }
    }

    NoteOps::quantizeNotes(pattern.notes, stepBeats, params.swing);

    // Same fix as generateDrumLoop: a swung note can otherwise claim to
    // sound past the pattern's own end, or into a later note that lands on
    // the same scale degree (and therefore the same pitch) — which the
    // random walk revisits often enough that this isn't a rare case. Notes
    // are already in ascending-startBeats order (built from onsetSteps in
    // that order; swing can't reorder them - see NoteOps::quantizeNotes).
    clampNoteLengths(pattern);

    return pattern;
}

} // namespace looper::engine
