#pragma once

#include <cmath>
#include <string>

namespace looper::engine
{
/**
    Equal-tempered MIDI note number → frequency in Hz (note 69 = A4 = 440 Hz).
    JUCE-free so the tuning math is unit-tested headless.
*/
inline double midiNoteToHertz(int midiNote, double a4Hz = 440.0) noexcept
{
    return a4Hz * std::pow(2.0, (midiNote - 69) / 12.0);
}

/** True for the five black keys of each octave (C#, D#, F#, G#, A#). */
inline bool isBlackKey(int midiNote) noexcept
{
    switch (((midiNote % 12) + 12) % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default:                                 return false;
    }
}

/**
    MIDI note number → scientific pitch notation ("C4", "C#4", ...), with
    middle C (60) as C4 — this project's existing convention (see e.g.
    PianoRoll's demo pattern, rooted at 60 and commented "C4").
*/
inline std::string midiNoteName(int midiNote)
{
    static const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pitchClass = ((midiNote % 12) + 12) % 12;
    const int octave     = midiNote / 12 - 1;
    return std::string(kNames[pitchClass]) + std::to_string(octave);
}

} // namespace looper::engine
