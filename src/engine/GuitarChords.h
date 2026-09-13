#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/Pattern.h"

namespace looper::engine
{
inline constexpr int kChordStrings = 6;

/**
    A chord as a guitarist holds it: one fret per string, or -1 for a string
    that isn't played.

    Shapes rather than note sets, because that's the difference between a
    chord a guitar can play and six notes that happen to spell one. The same
    triad has many voicings on a neck and only some are reachable by a hand;
    storing the shape keeps the reachable one.
*/
struct ChordShape
{
    const char* name;
    int         frets[kChordStrings]; // -1 = string not played
};

/** Open-position shapes, the ones most guitar parts are actually built from.
    Movable barre chords come from the same table with a fret offset, which is
    exactly how a player thinks of them. */
inline constexpr ChordShape kChordShapes[] = {
    { "E",  {  0,  2,  2,  1,  0,  0 } },
    { "Em", {  0,  2,  2,  0,  0,  0 } },
    { "A",  { -1,  0,  2,  2,  2,  0 } },
    { "Am", { -1,  0,  2,  2,  1,  0 } },
    { "D",  { -1, -1,  0,  2,  3,  2 } },
    { "Dm", { -1, -1,  0,  2,  3,  1 } },
    { "G",  {  3,  2,  0,  0,  0,  3 } },
    { "C",  { -1,  3,  2,  0,  1,  0 } },
};

inline constexpr int kNumChordShapes = (int) (sizeof(kChordShapes) / sizeof(kChordShapes[0]));

/** A shape a player can put anywhere on the neck, as intervals above the root
    rather than as a fingering. See notesForRoot. */
enum class MovableShape
{
    Power = 0, // root, fifth, octave — most of rock rhythm guitar
    Octave,    // root and octave alone, for riffing
    Major,     // a barre chord, not a stack: see notesForRoot
    Minor
};

inline constexpr int kNumMovableShapes = 4;

inline const char* movableShapeName(MovableShape shape)
{
    switch (shape)
    {
        case MovableShape::Power:  return "Power";
        case MovableShape::Octave: return "Octave";
        case MovableShape::Major:  return "Major";
        case MovableShape::Minor:  return "Minor";
    }
    return "Power";
}

/** True for the shapes that really are a stack of intervals on consecutive
    strings. A power chord is: root, the fifth on the next string, the octave
    on the one after, which is exactly how it's fingered. A major chord is
    not — it's a barre across all six strings, and pretending otherwise
    produces the right pitches in a voicing no hand would play. */
inline bool isIntervalStack(MovableShape shape)
{
    return shape == MovableShape::Power || shape == MovableShape::Octave;
}

/** The intervals, in semitones above the root, for the stacked shapes. */
inline std::vector<int> intervalsForShape(MovableShape shape)
{
    return shape == MovableShape::Octave ? std::vector<int> { 0, 12 }
                                         : std::vector<int> { 0, 7, 12 };
}

/** How a chord is struck. */
struct StrumSettings
{
    bool   downstroke = true; // low string first; an upstroke starts high
    double spreadMs   = 18.0; // time between the first string and the last
    double humanise   = 0.0;  // 0..1, jitter on timing and velocity
};

/**
    Turning chord shapes into notes.

    JUCE-free so the timing can be measured headlessly, and deliberately
    producing *real notes at real times* rather than a playback-time "strum
    feel" — the same choice §18's swing made. A strum that exists in the
    pattern stays visible and editable, and the engine needs to know nothing
    about it.
*/
struct GuitarChords
{
    /** The MIDI notes a shape produces on a given tuning, low string to high,
        skipping strings that aren't played. @p fretOffset slides the shape up
        the neck, which is what makes a barre chord out of an open one. */
    static std::vector<int> notesForShape(const ChordShape& shape, const int* tuning, int fretOffset = 0)
    {
        std::vector<int> notes;
        notes.reserve(kChordStrings);

        for (int s = 0; s < kChordStrings; ++s)
        {
            if (shape.frets[s] < 0)
                continue; // muted or simply not struck

            // An open string stays open when the shape moves: you can't slide
            // a nut up the neck. Fretted notes move with the offset.
            const int fret = shape.frets[s] == 0 && fretOffset == 0 ? 0 : shape.frets[s] + fretOffset;
            const int note = tuning[s] + fret;
            if (note >= 0 && note <= 127)
                notes.push_back(note);
        }
        return notes;
    }

    /**
        The notes of a movable shape rooted on @p rootString at @p fret —
        what clicking a fret with a chord mode selected produces.

        Each interval is placed on the next string up and the fret solved
        against that string's *actual* tuning, rather than reproducing the
        familiar fingering (root, then two frets up on each of the next two
        strings). That fingering is only right because those strings happen to
        be a fourth apart: it produces a wrong chord across the G–B pair,
        which is a major third, and in any tuning that isn't standard. §21
        shipped drop tunings, so both cases are expected rather than
        hypothetical.

        Solving from the tuning is also what a player actually does when
        working a shape out on an unfamiliar neck.

        A note whose fret falls off the neck is dropped rather than invented,
        so a shape rooted near the top doesn't reach frets that don't exist.
        Strings are numbered as the rest of the pane numbers them: 0 is the
        lowest.
    */
    static std::vector<int> notesForRoot(MovableShape shape, const int* tuning,
                                         int rootString, int fret, int highestFret = 22)
    {
        std::vector<int> notes;
        if (rootString < 0 || rootString >= kChordStrings || fret < 0 || fret > highestFret)
            return notes;

        // Major and minor are barre chords: the open E and A shapes slid up
        // the neck, which is how a player thinks of them and what the shape
        // table above already encodes. Deriving them as an interval stack
        // instead would give correct pitches in a voicing no hand would play.
        if (! isIntervalStack(shape))
        {
            const bool minor  = (shape == MovableShape::Minor);
            const bool aShape = (rootString >= 1); // rooted on the A string or above

            const std::string_view wanted = aShape ? (minor ? "Am" : "A") : (minor ? "Em" : "E");
            for (const auto& candidate : kChordShapes)
                if (std::string_view(candidate.name) == wanted)
                    return notesForShape(candidate, tuning, fret);

            return notes;
        }

        const int rootNote  = tuning[rootString] + fret;
        const auto intervals = intervalsForShape(shape);

        for (size_t i = 0; i < intervals.size(); ++i)
        {
            // One interval per string, moving up the neck from the root.
            const int stringIndex = rootString + (int) i;
            if (stringIndex >= kChordStrings)
                break; // ran out of strings: play the part of the shape that fits

            const int wanted    = rootNote + intervals[i];
            const int fretHere  = wanted - tuning[stringIndex];

            // The interval simply isn't reachable on this string in this
            // tuning — skipping it beats fretting a note that isn't in the
            // chord.
            if (fretHere < 0 || fretHere > highestFret)
                continue;

            if (wanted >= 0 && wanted <= 127)
                notes.push_back(wanted);
        }

        return notes;
    }

    /** As strumChord, for a shape rooted at a point on the neck. Shares the
        stagger and humanising, since a power chord is struck by the same hand
        as any other. */
    static std::vector<Note> strumRootedChord(MovableShape shape, const int* tuning,
                                              int rootString, int fret,
                                              double startBeats, double lengthBeats, double bpm,
                                              const StrumSettings& strum, uint32_t seed = 1,
                                              int highestFret = 22)
    {
        auto pitches = notesForRoot(shape, tuning, rootString, fret, highestFret);
        return staggered(std::move(pitches), startBeats, lengthBeats, bpm, strum, seed);
    }

    /**
        Stamps a strummed chord into notes with staggered start times.

        Strings are struck in sequence, not together — roughly 10–30ms apart —
        and that stagger is most of what makes a strum sound like a hand rather
        than an organ. A downstroke starts on the lowest string, an upstroke on
        the highest.

        Jitter is bounded to less than half the gap between strings, so a
        humanised strum can never reorder itself: the strings must be struck in
        the order the hand moves, however loose the timing.
    */
    static std::vector<Note> strumChord(const ChordShape& shape, const int* tuning, int fretOffset,
                                        double startBeats, double lengthBeats, double bpm,
                                        const StrumSettings& strum, uint32_t seed = 1)
    {
        return staggered(notesForShape(shape, tuning, fretOffset),
                         startBeats, lengthBeats, bpm, strum, seed);
    }

private:
    /** Turns a set of pitches, lowest first, into struck notes. Shared by the
        open-shape palette and the movable shapes rooted on the neck: the
        difference between them is which strings are held down, not how the
        hand crosses them. */
    static std::vector<Note> staggered(std::vector<int> notes, double startBeats, double lengthBeats,
                                       double bpm, const StrumSettings& strum, uint32_t seed)
    {
        if (notes.empty())
            return {};

        if (! strum.downstroke)
            std::reverse(notes.begin(), notes.end()); // an upstroke hits the high strings first

        const double beatsPerMs = bpm > 0.0 ? bpm / 60000.0 : 0.0;
        const double spread     = std::max(0.0, strum.spreadMs) * beatsPerMs;
        const double step       = notes.size() > 1 ? spread / (double) (notes.size() - 1) : 0.0;
        const double humanise   = std::clamp(strum.humanise, 0.0, 1.0);

        uint32_t random = seed == 0 ? 1 : seed;
        auto nextUnit = [&random]
        {
            random = random * 1664525u + 1013904223u;
            return (double) ((int32_t) random) / 2147483648.0; // -1..1
        };

        std::vector<Note> out;
        out.reserve(notes.size());

        for (size_t i = 0; i < notes.size(); ++i)
        {
            // Capped below half a step, which is what guarantees the strokes
            // stay in order no matter how much humanising is asked for.
            const double jitter = nextUnit() * humanise * step * 0.4;
            const double start  = startBeats + (double) i * step + jitter;

            Note note;
            note.startBeats  = std::max(0.0, start);
            note.lengthBeats = std::max(0.05, lengthBeats);
            note.noteNumber  = notes[i];
            note.velocity    = (float) std::clamp(0.85 + nextUnit() * humanise * 0.15, 0.15, 1.0);
            out.push_back(note);
        }
        return out;
    }
};

} // namespace looper::engine
