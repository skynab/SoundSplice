#pragma once

#include <utility>
#include <vector>

#include "engine/Pattern.h"

namespace looper::engine
{
/** The GM-ish note numbers model::makeDefaultDrumKit() assigns its four
    starting pads — kept here, not just there, because the loop below has to
    agree with them exactly or it plays the wrong pad. */
inline constexpr int kDefaultKickNote  = 36;
inline constexpr int kDefaultSnareNote = 38;
inline constexpr int kDefaultHatNote   = 42;

/**
    A one-bar starter beat: kick on 1 and 3, snare on 2 and 4, closed hats on
    every eighth note — about as generic a 4/4 pattern as exists, which is
    the point. Not meant to be interesting; meant to be recognizable enough
    on first launch that a new drum track visibly (and audibly) does
    something, rather than opening on a silent, empty grid.

    JUCE-free, like Pattern itself, so the shape of the beat — which beats
    carry which drum — is a headless, testable claim rather than something
    only checkable by ear.
*/
inline Pattern makeDefaultDrumLoopPattern()
{
    Pattern pattern;
    pattern.lengthBeats = 4.0;

    auto add = [&](double beat, int note, float velocity)
    {
        pattern.notes.push_back({ beat, 0.25, note, velocity });
    };

    add(0.0, kDefaultKickNote, 0.95f);
    add(2.0, kDefaultKickNote, 0.9f);

    add(1.0, kDefaultSnareNote, 0.85f);
    add(3.0, kDefaultSnareNote, 0.85f);

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
        add(beat, kDefaultHatNote, 0.6f);

    return pattern;
}

/**
    A one-bar starter riff: straight eighth-note chugs on the lowest string,
    with the root leaving for a minor third and a fourth at the end of the
    bar so it's a phrase rather than a metronome. The guitar equivalent of
    makeDefaultDrumLoopPattern, and there for the same reason — a fresh
    launch should make the guitar track *audibly* do something.

    @p lowStringNote is the open pitch of the lowest string, and is required
    rather than defaulted on purpose: a guitar can only sound a note some
    string can actually reach, so a riff written for standard tuning is
    silent on a dropped one (it lands below every open string) and vice
    versa. Taking the real tuning is the same call DrumLoopParams makes in
    asking for the target kit's actual pad notes. Every pitch below is an
    offset from it, so the riff transposes with the tuning instead of
    breaking against it.
*/
inline Pattern makeDefaultGuitarRiffPattern(int lowStringNote)
{
    Pattern pattern;
    pattern.lengthBeats = 4.0;

    // Short relative to the eighth-note spacing, which is what makes this
    // read as palm-muted chugging rather than as held notes.
    auto add = [&](double beat, int semitonesAboveOpen, float velocity)
    {
        pattern.notes.push_back({ beat, 0.3, lowStringNote + semitonesAboveOpen, velocity });
    };

    add(0.0, 0, 1.0f);  // the downbeat, hit hardest
    add(0.5, 0, 0.8f);
    add(1.0, 0, 0.85f);
    add(1.5, 0, 0.8f);
    add(2.0, 0, 0.95f);
    add(2.5, 0, 0.8f);
    add(3.0, 3, 0.9f);  // minor third
    add(3.5, 5, 0.9f);  // fourth, leading back to the root

    return pattern;
}

// ---------------------------------------------------------------------------
// The starter song
// ---------------------------------------------------------------------------

/** The fourth pad model::makeDefaultDrumKit() assigns (a clap in the Classic
    style), kept here for the same reason the three above are. */
inline constexpr int kDefaultClapNote = 45;

/** Beats per bar the starter material is written in. Everything below assumes
    4/4, which is also what model::Song defaults to — a starter song in a time
    signature the project isn't set to would be wrong on its first bar. */
inline constexpr double kStarterBeatsPerBar = 4.0;

/** One cycle of the starter song's harmony, in bars. */
inline constexpr int kStarterCycleBars = 4;

inline constexpr double kStarterCycleBeats = kStarterBeatsPerBar * (double) kStarterCycleBars;

/**
    The starter song's chord progression: i - VI - III - VII in a natural minor
    key, one chord per bar, as semitone offsets above the key's root.

    Written as offsets rather than absolute pitches because every part has to
    agree with it from a different register — the guitar from its lowest open
    string, the bass an octave up from there, the lead two octaves up — and the
    only thing that keeps four parts in the same key is deriving all of them
    from one place. The previous starter song did not do this: its guitar riff
    was rooted on a drop-C low string, its synth clip held whatever the piano
    roll happened to default to, and the two had no relationship at all.

    i-VI-III-VII specifically because every chord in it is diatonic to the
    natural minor scale, so a melody can be written from the scale alone
    without needing to dodge a chord tone.
*/
inline constexpr int kStarterProgression[kStarterCycleBars] = {
    0,  // i   - the tonic minor
    8,  // VI  - a sixth above
    3,  // III - the relative major
    10  // VII - leads back to the tonic
};

/** Natural minor, as semitone offsets from the root. What the lead melody is
    written from, so it cannot leave the key by construction. */
inline constexpr int kNaturalMinorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };

/** Copies @p source into @p destination, shifted @p atBeat later and @p
    semitones higher, and grows the destination's length to cover it. The one
    place patterns are assembled from smaller ones, so "where does this go" is
    written once rather than at every call site. */
inline void appendPatternAt(Pattern& destination, const Pattern& source,
                            double atBeat, int semitones = 0)
{
    for (const auto& note : source.notes)
    {
        Note shifted = note;
        shifted.startBeats += atBeat;
        shifted.noteNumber += semitones;
        destination.notes.push_back(shifted);
    }

    const double end = atBeat + source.lengthBeats;
    if (end > destination.lengthBeats)
        destination.lengthBeats = end;
}

/**
    The drum part for the starter song's opening four bars: kick and hats, no
    backbeat, with a snare pickup across the last half-bar.

    Deliberately thinner than the groove it leads into — the whole point of the
    starter song's arrangement is that parts *arrive*, and a first section
    identical to the second would waste the only four bars where that is
    visible on an otherwise empty timeline.
*/
inline Pattern makeStarterDrumIntroPattern()
{
    Pattern pattern;
    pattern.lengthBeats = kStarterCycleBeats;

    auto add = [&](double beat, int note, float velocity)
    {
        pattern.notes.push_back({ beat, 0.25, note, velocity });
    };

    for (int bar = 0; bar < kStarterCycleBars; ++bar)
    {
        const double barStart = (double) bar * kStarterBeatsPerBar;

        add(barStart,       kDefaultKickNote, 0.95f);
        add(barStart + 2.0, kDefaultKickNote, 0.9f);

        for (double offset = 0.0; offset < kStarterBeatsPerBar; offset += 0.5)
            add(barStart + offset, kDefaultHatNote, 0.55f);
    }

    // The pickup: four snares climbing into bar five, which is what makes the
    // groove's entry sound intended rather than abrupt.
    const double lastBar = 3.0 * kStarterBeatsPerBar;
    add(lastBar + 2.0,  kDefaultSnareNote, 0.5f);
    add(lastBar + 2.5,  kDefaultSnareNote, 0.6f);
    add(lastBar + 3.0,  kDefaultSnareNote, 0.75f);
    add(lastBar + 3.5,  kDefaultSnareNote, 0.9f);

    return pattern;
}

/**
    Four bars of the full beat, built by repeating makeDefaultDrumLoopPattern
    rather than restating it — so "the basic 4/4 beat" still has exactly one
    definition, and the two cannot drift apart.

    @p withFill replaces the last bar's second half with a sixteenth-note snare
    fill, for the cycle that ends a section.
*/
inline Pattern makeStarterDrumGroovePattern(bool withFill)
{
    const Pattern bar = makeDefaultDrumLoopPattern();

    Pattern pattern;
    pattern.lengthBeats = kStarterCycleBeats;

    const int lastBar = kStarterCycleBars - 1;

    for (int barIndex = 0; barIndex < kStarterCycleBars; ++barIndex)
    {
        const double barStart = (double) barIndex * kStarterBeatsPerBar;

        // The filled bar takes only its first half from the base beat; the
        // second half is the fill, and layering the two would just be a
        // muddle of snares.
        if (withFill && barIndex == lastBar)
        {
            for (const auto& note : bar.notes)
                if (note.startBeats < 2.0)
                    pattern.notes.push_back({ barStart + note.startBeats, note.lengthBeats,
                                              note.noteNumber, note.velocity });

            // Sixteenths, getting louder — the standard "here comes the next
            // section" gesture, and audible proof the grid is finer than the
            // eighth notes everything else here sits on.
            float velocity = 0.55f;
            for (double offset = 2.0; offset < kStarterBeatsPerBar; offset += 0.25)
            {
                pattern.notes.push_back({ barStart + offset, 0.2, kDefaultSnareNote, velocity });
                velocity = velocity < 0.95f ? velocity + 0.05f : 1.0f;
            }

            continue;
        }

        appendPatternAt(pattern, bar, barStart);

        // A clap doubling the backbeat on alternate bars: the fourth pad of
        // the default kit, which otherwise never sounds on first launch.
        if (barIndex % 2 == 1)
        {
            pattern.notes.push_back({ barStart + 1.0, 0.25, kDefaultClapNote, 0.7f });
            pattern.notes.push_back({ barStart + 3.0, 0.25, kDefaultClapNote, 0.7f });
        }
    }

    pattern.lengthBeats = kStarterCycleBeats;
    return pattern;
}

/**
    A four-bar bass line following the progression: a driving eighth-note root
    with the fifth answering it, and an octave lift at the end of each bar.

    @p rootNote is the key's root in the bass's own register.
*/
inline Pattern makeStarterBassPattern(int rootNote)
{
    Pattern pattern;
    pattern.lengthBeats = kStarterCycleBeats;

    for (int bar = 0; bar < kStarterCycleBars; ++bar)
    {
        const double barStart  = (double) bar * kStarterBeatsPerBar;
        const int    chordRoot = rootNote + kStarterProgression[bar];

        auto add = [&](double offset, int semitones, double length, float velocity)
        {
            pattern.notes.push_back({ barStart + offset, length, chordRoot + semitones, velocity });
        };

        add(0.0, 0,  0.45, 0.95f);
        add(0.5, 0,  0.45, 0.75f);
        add(1.5, 0,  0.45, 0.8f);
        add(2.0, 7,  0.45, 0.85f); // the fifth
        add(2.5, 0,  0.45, 0.75f);
        add(3.0, 0,  0.45, 0.8f);
        add(3.5, 12, 0.45, 0.9f);  // octave lift into the next bar
    }

    return pattern;
}

/**
    A four-bar guitar riff following the progression: palm-mute-length chugs on
    each bar's chord root, with the existing one-bar riff closing the cycle.

    Bar four is makeDefaultGuitarRiffPattern transposed onto the VII chord, and
    that is not an arbitrary reuse — that figure already ends by walking up a
    minor third and a fourth, which from the VII lands back on the tonic
    exactly where bar one begins.

    @p lowStringNote is the open pitch of the lowest string, for the same
    reason makeDefaultGuitarRiffPattern takes it: a riff written for one tuning
    is unplayable in another.
*/
inline Pattern makeStarterGuitarRiffPattern(int lowStringNote)
{
    Pattern pattern;
    pattern.lengthBeats = kStarterCycleBeats;

    // Short relative to the eighth-note spacing, so this reads as chugging
    // rather than as held notes — the same relationship the one-bar riff uses.
    const std::vector<std::pair<double, float>> chugs {
        { 0.0, 1.0f }, { 0.5, 0.8f }, { 1.0, 0.85f }, { 1.5, 0.8f },
        { 2.0, 0.95f }, { 2.5, 0.8f }, { 3.0, 0.85f }, { 3.5, 0.8f }
    };

    for (int bar = 0; bar < kStarterCycleBars - 1; ++bar)
    {
        const double barStart  = (double) bar * kStarterBeatsPerBar;
        const int    chordRoot = lowStringNote + kStarterProgression[bar];

        for (const auto& [offset, velocity] : chugs)
            pattern.notes.push_back({ barStart + offset, 0.3, chordRoot, velocity });
    }

    appendPatternAt(pattern, makeDefaultGuitarRiffPattern(lowStringNote),
                    (double) (kStarterCycleBars - 1) * kStarterBeatsPerBar,
                    kStarterProgression[kStarterCycleBars - 1]);

    pattern.lengthBeats = kStarterCycleBeats;
    return pattern;
}

/**
    One of two two-bar lead phrases, written from the natural minor scale so it
    cannot leave the key.

    Two phrases rather than one four-bar line because a track holding a single
    clip loops it indefinitely, while a track holding more than one gates each
    to its own window — so splitting the lead is what makes the starter song
    demonstrate that gating, as well as being the more musical shape (a phrase
    and its answer).

    @p phrase 0 is the call, 1 the answer; anything else is treated as 0.
*/
inline Pattern makeStarterLeadPattern(int rootNote, int phrase)
{
    Pattern pattern;
    pattern.lengthBeats = 2.0 * kStarterBeatsPerBar;

    // Scale degrees, not semitones: degree 7 is the octave, 9 the third above
    // it, and so on, so the melody stays in key however far it climbs.
    auto add = [&](double beat, int degree, double length, float velocity)
    {
        const int octave = degree >= 0 ? degree / 7 : (degree - 6) / 7;
        const int step   = degree - octave * 7;
        pattern.notes.push_back({ beat, length,
                                  rootNote + kNaturalMinorSteps[step] + 12 * octave,
                                  velocity });
    };

    if (phrase == 1)
    {
        // The answer: starts higher and settles back onto the tonic.
        add(0.0, 9, 1.0,  0.9f);
        add(1.0, 8, 0.5,  0.8f);
        add(1.5, 7, 0.5,  0.8f);
        add(2.0, 5, 1.0,  0.85f);
        add(3.0, 7, 0.5,  0.8f);
        add(3.5, 8, 0.5,  0.8f);
        add(4.0, 9, 1.5,  0.95f);
        add(5.5, 8, 0.5,  0.8f);
        add(6.0, 7, 2.0,  0.9f); // held, resolving the cycle
        return pattern;
    }

    // The call.
    add(0.0, 4, 0.5,  0.85f);
    add(0.5, 5, 0.5,  0.8f);
    add(1.0, 6, 1.0,  0.85f);
    add(2.0, 7, 1.0,  0.95f);
    add(3.0, 6, 0.5,  0.8f);
    add(3.5, 5, 0.5,  0.8f);
    add(4.0, 4, 1.5,  0.9f);
    add(5.5, 3, 0.5,  0.8f);
    add(6.0, 2, 1.0,  0.85f);
    add(7.0, 3, 0.5,  0.8f);
    add(7.5, 4, 0.5,  0.85f);

    return pattern;
}

} // namespace looper::engine
