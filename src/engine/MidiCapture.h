#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Pattern.h"

namespace looper::engine
{
/**
    Turns a captured stream of MIDI note events into a Pattern's notes.

    JUCE-free, and deliberately ignorant of both `juce::MidiMessage` and
    TempoMap: AudioEngine translates JUCE messages into PODs on the way in, and
    the caller converts sample times to beats on the way out (see
    MidiRecorder). What is left is a function whose entire contract is "these
    events in, these notes out" — which is why, unlike AudioRecorder, this can
    be unit-tested headless rather than only through the bounce tool. The
    pairing rules below are subtle enough that this matters.
*/

/** One captured note event, timed in samples on the transport's timeline —
    what the audio thread pushes across the ring buffer, so it is a POD and
    trivially copyable by requirement, not just by habit. */
struct RecordedMidiEvent
{
    int64_t timeSamples = 0;
    int     noteNumber  = 60;
    float   velocity    = 0.8f; // 0..1; 0 on a note-on means note-off (see below)
    bool    noteOn      = false;

    bool operator==(const RecordedMidiEvent&) const = default;
};

/** The same event after the caller has converted its time through the tempo
    map — beats from the start of the take. Separate from RecordedMidiEvent
    rather than a mutated field, so it is impossible to pair a stream that
    still holds sample times. */
struct TimedMidiEvent
{
    double beats      = 0.0;
    int    noteNumber = 60;
    float  velocity   = 0.8f;
    bool   noteOn     = false;

    bool operator==(const TimedMidiEvent&) const = default;
};

struct MidiCapture
{
    /** Shortest note this will emit, in beats — a 64th note.

        A zero- or negative-length note is silent in the sequencer and
        invisible in the piano roll, so emitting one would look to the user
        like a *dropped* note rather than a very short one. Clamping is the
        honest outcome: the note was played, and it is representable. */
    static constexpr double kMinNoteLengthBeats = 1.0 / 16.0;

    /**
        Pairs @p events into notes.

        @p takeEndBeats bounds a note still held when the take ended — someone
        holding the final chord as they hit Stop keeps it, clamped to the end,
        rather than losing it. Pass the beat the take stopped at; anything at
        or below the note's own start falls back to the minimum length.

        The rules, each of which exists because a real controller produces it:
          - A note-on pairs with the *next* note-off of the same pitch.
          - A note-on with velocity 0 **is** a note-off. Running-status
            convention; plenty of hardware sends only this, and treating it as
            an onset would record every note as held forever.
          - A note-off with no open note-on is ignored — that is simply a key
            that was already down when recording started.
          - The same pitch retriggered before its first note-off closes FIFO:
            the first on pairs with the first off, which keeps note lengths in
            the order they were played.
          - Output is sorted by start beat, since capture order is arrival
            order.
    */
    static std::vector<Note> notesFromEvents(std::vector<TimedMidiEvent> events,
                                             double takeEndBeats)
    {
        // Stable, so that events sharing a timestamp keep their arrival order
        // — which is what makes the FIFO retrigger rule below deterministic.
        std::stable_sort(events.begin(), events.end(),
                         [](const TimedMidiEvent& a, const TimedMidiEvent& b)
                         { return a.beats < b.beats; });

        std::vector<Note> notes;
        notes.reserve(events.size() / 2);

        // Indices into `notes` for pitches whose note-off hasn't arrived yet,
        // oldest first per pitch. Held as indices rather than iterators
        // because `notes` reallocates as it grows.
        struct OpenNote
        {
            int    noteNumber = 0;
            size_t index      = 0;
        };
        std::vector<OpenNote> open;

        for (const auto& event : events)
        {
            const bool isNoteOn = event.noteOn && event.velocity > 0.0f;

            if (isNoteOn)
            {
                Note note;
                note.startBeats  = std::max(0.0, event.beats);
                note.lengthBeats = kMinNoteLengthBeats; // until its note-off arrives
                note.noteNumber  = event.noteNumber;
                note.velocity    = std::clamp(event.velocity, 0.0f, 1.0f);

                open.push_back({ event.noteNumber, notes.size() });
                notes.push_back(note);
                continue;
            }

            // A note-off (including a zero-velocity note-on): close the
            // oldest still-open note of this pitch, if there is one.
            const auto match = std::find_if(open.begin(), open.end(),
                                            [&](const OpenNote& o)
                                            { return o.noteNumber == event.noteNumber; });
            if (match == open.end())
                continue; // key was already down when capture began

            auto& note = notes[match->index];
            note.lengthBeats = std::max(kMinNoteLengthBeats, event.beats - note.startBeats);
            open.erase(match);
        }

        // Whatever is still held when the take ends runs to the end of it.
        for (const auto& stillOpen : open)
        {
            auto& note = notes[stillOpen.index];
            note.lengthBeats = std::max(kMinNoteLengthBeats, takeEndBeats - note.startBeats);
        }

        std::stable_sort(notes.begin(), notes.end(),
                         [](const Note& a, const Note& b)
                         { return a.startBeats < b.startBeats; });

        return notes;
    }

    /** Rounds @p beats up to the next whole bar, given @p quartersPerBar.

        A take is a musical phrase: ending its clip on the last note's release
        would make a loop of it jarringly short, and would put the downbeat of
        any repeat in the wrong place. Always at least one bar, so a take
        containing a single short note is still a usable clip. */
    static double clipLengthForTake(double beats, double quartersPerBar)
    {
        if (quartersPerBar <= 0.0)
            return std::max(beats, 0.0);

        const double bars = std::ceil(std::max(beats, 0.0) / quartersPerBar);
        return std::max(1.0, bars) * quartersPerBar;
    }
};

} // namespace looper::engine
