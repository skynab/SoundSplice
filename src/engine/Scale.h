#pragma once

#include <algorithm>
#include <vector>

namespace looper::engine
{
/** A small, fixed set of scales — enough to deliver "generate a melody in
    this key/scale" (see docs/PLAN.md §10) without an open-ended music-theory
    taxonomy. Modes/scales not listed here are a pure addition later; nothing
    about this shape assumes there are only six. */
enum class ScaleType
{
    Major,
    NaturalMinor,
    MajorPentatonic,
    MinorPentatonic,
    Dorian,
    Mixolydian,
    Phrygian // the half-step above the root is what gives it its "dark"/unsettled color
};

/** A scale is a type plus a root note. The root is a real MIDI note number
    (middle C = 60 = "C4", this project's existing convention — see
    MidiNote.h) rather than a bare pitch class, so a Scale is directly usable
    without a separate octave parameter. */
struct Scale
{
    ScaleType type     = ScaleType::Major;
    int       rootNote = 60;

    bool operator==(const Scale&) const = default;
};

/** The scale's semitone offsets above its root, one octave, ascending. */
inline std::vector<int> intervalsForScale(ScaleType type)
{
    switch (type)
    {
        case ScaleType::Major:           return { 0, 2, 4, 5, 7, 9, 11 };
        case ScaleType::NaturalMinor:    return { 0, 2, 3, 5, 7, 8, 10 };
        case ScaleType::MajorPentatonic: return { 0, 2, 4, 7, 9 };
        case ScaleType::MinorPentatonic: return { 0, 3, 5, 7, 10 };
        case ScaleType::Dorian:          return { 0, 2, 3, 5, 7, 9, 10 };
        case ScaleType::Mixolydian:      return { 0, 2, 4, 5, 7, 9, 10 };
        case ScaleType::Phrygian:        return { 0, 1, 3, 5, 7, 8, 10 };
    }
    return { 0, 2, 4, 5, 7, 9, 11 };
}

/** True if @p noteNumber's pitch class (relative to the scale's root) is one
    of the scale's intervals, in any octave. */
inline bool isInScale(int noteNumber, const Scale& scale)
{
    const auto intervals = intervalsForScale(scale.type);
    int pitchClass = (noteNumber - scale.rootNote) % 12;
    if (pitchClass < 0)
        pitchClass += 12;
    return std::find(intervals.begin(), intervals.end(), pitchClass) != intervals.end();
}

/** The nearest note that is in @p scale. A note already in the scale is
    returned unchanged. Searches outward one semitone at a time and checks
    up before down at each distance, so an exact tie (equally close either
    way) rounds up rather than down. */
inline int snapToScale(int noteNumber, const Scale& scale)
{
    if (isInScale(noteNumber, scale))
        return noteNumber;

    for (int distance = 1; distance <= 6; ++distance)
    {
        if (isInScale(noteNumber + distance, scale))
            return noteNumber + distance;
        if (isInScale(noteNumber - distance, scale))
            return noteNumber - distance;
    }
    return noteNumber; // unreachable: every scale above has an interval within 6 semitones of any note
}

/** The MIDI note for scale degree @p degree, where degree 0 is the root, 1
    is the next note up the scale, -1 the note below it, and so on without
    bound in either direction — degrees beyond one octave wrap, carrying the
    octave with them, so degreeToNote(scale, degree + scaleSize) is always
    exactly one octave above degreeToNote(scale, degree). */
inline int degreeToNote(const Scale& scale, int degree)
{
    const auto intervals = intervalsForScale(scale.type);
    const int  size      = (int) intervals.size();

    const int octave = degree >= 0 ? degree / size
                                    : -(((-degree) + size - 1) / size);
    const int index   = degree - octave * size;

    return scale.rootNote + octave * 12 + intervals[(size_t) index];
}

} // namespace looper::engine
