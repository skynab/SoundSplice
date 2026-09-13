#include <catch2/catch_test_macros.hpp>

#include <engine/MidiCapture.h>

using namespace looper::engine;

namespace
{
    TimedMidiEvent on(double beats, int noteNumber, float velocity = 0.8f)
    {
        return { beats, noteNumber, velocity, true };
    }

    TimedMidiEvent off(double beats, int noteNumber)
    {
        return { beats, noteNumber, 0.0f, false };
    }

    bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }
}

TEST_CASE("A note-on pairs with the following note-off", "[engine][midicapture]")
{
    const auto notes = MidiCapture::notesFromEvents({ on(1.0, 60, 0.5f), off(2.5, 60) }, 4.0);

    REQUIRE(notes.size() == 1);
    REQUIRE(near(notes[0].startBeats, 1.0));
    REQUIRE(near(notes[0].lengthBeats, 1.5));
    REQUIRE(notes[0].noteNumber == 60);
    REQUIRE(notes[0].velocity == 0.5f);
}

TEST_CASE("A note-on with velocity 0 is treated as a note-off", "[engine][midicapture]")
{
    // What a great many controllers actually send instead of a note-off.
    // Treating it as an onset would record every note as held forever.
    std::vector<TimedMidiEvent> events { on(0.0, 64, 0.9f), on(1.0, 64, 0.0f) };

    const auto notes = MidiCapture::notesFromEvents(events, 4.0);

    REQUIRE(notes.size() == 1);
    REQUIRE(near(notes[0].lengthBeats, 1.0));
    REQUIRE(notes[0].velocity == 0.9f);
}

TEST_CASE("A note still held when the take ends runs to the end of it", "[engine][midicapture]")
{
    // Holding the final chord as you hit Stop must not lose it.
    const auto notes = MidiCapture::notesFromEvents({ on(2.0, 67) }, 8.0);

    REQUIRE(notes.size() == 1);
    REQUIRE(near(notes[0].lengthBeats, 6.0));
}

TEST_CASE("A note-off with no matching note-on is ignored", "[engine][midicapture]")
{
    // A key that was already down when capture began.
    const auto notes = MidiCapture::notesFromEvents({ off(0.5, 60), on(1.0, 60), off(2.0, 60) }, 4.0);

    REQUIRE(notes.size() == 1);
    REQUIRE(near(notes[0].startBeats, 1.0));
    REQUIRE(near(notes[0].lengthBeats, 1.0));
}

TEST_CASE("The same pitch retriggered before its note-off closes FIFO", "[engine][midicapture]")
{
    std::vector<TimedMidiEvent> events {
        on(0.0, 60), on(1.0, 60), off(2.0, 60), off(3.0, 60)
    };

    const auto notes = MidiCapture::notesFromEvents(events, 4.0);

    REQUIRE(notes.size() == 2);
    // First on pairs with first off (0->2), second with second (1->3), which
    // keeps the note lengths in the order they were played.
    REQUIRE(near(notes[0].startBeats, 0.0));
    REQUIRE(near(notes[0].lengthBeats, 2.0));
    REQUIRE(near(notes[1].startBeats, 1.0));
    REQUIRE(near(notes[1].lengthBeats, 2.0));
}

TEST_CASE("Different pitches held at once stay independent", "[engine][midicapture]")
{
    std::vector<TimedMidiEvent> events {
        on(0.0, 60), on(0.0, 64), on(0.0, 67), off(1.0, 64), off(2.0, 60), off(3.0, 67)
    };

    const auto notes = MidiCapture::notesFromEvents(events, 4.0);

    REQUIRE(notes.size() == 3);
    auto lengthOf = [&](int noteNumber)
    {
        for (const auto& note : notes)
            if (note.noteNumber == noteNumber)
                return note.lengthBeats;
        return -1.0;
    };
    REQUIRE(near(lengthOf(60), 2.0));
    REQUIRE(near(lengthOf(64), 1.0));
    REQUIRE(near(lengthOf(67), 3.0));
}

TEST_CASE("A zero-length note is clamped rather than emitted at zero", "[engine][midicapture]")
{
    // lengthBeats <= 0 is silent in the sequencer and invisible in the piano
    // roll, so it would read as a dropped note rather than a short one.
    const auto notes = MidiCapture::notesFromEvents({ on(1.0, 60), off(1.0, 60) }, 4.0);

    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].lengthBeats >= MidiCapture::kMinNoteLengthBeats);
}

TEST_CASE("Notes come back sorted by start beat", "[engine][midicapture]")
{
    // Capture order is arrival order, which a slow drain can scramble.
    std::vector<TimedMidiEvent> events {
        on(3.0, 72), off(3.5, 72), on(1.0, 60), off(1.5, 60), on(2.0, 64), off(2.5, 64)
    };

    const auto notes = MidiCapture::notesFromEvents(events, 4.0);

    REQUIRE(notes.size() == 3);
    REQUIRE(notes[0].noteNumber == 60);
    REQUIRE(notes[1].noteNumber == 64);
    REQUIRE(notes[2].noteNumber == 72);
}

TEST_CASE("An empty event stream produces no notes", "[engine][midicapture]")
{
    REQUIRE(MidiCapture::notesFromEvents({}, 4.0).empty());
}

TEST_CASE("Take length rounds up to the next whole bar", "[engine][midicapture]")
{
    REQUIRE(near(MidiCapture::clipLengthForTake(0.5,  4.0), 4.0));
    REQUIRE(near(MidiCapture::clipLengthForTake(4.0,  4.0), 4.0));
    REQUIRE(near(MidiCapture::clipLengthForTake(4.25, 4.0), 8.0));
    REQUIRE(near(MidiCapture::clipLengthForTake(9.0,  3.0), 9.0));
    REQUIRE(near(MidiCapture::clipLengthForTake(9.5,  3.0), 12.0));
}

TEST_CASE("An empty take is still one bar long", "[engine][midicapture]")
{
    REQUIRE(near(MidiCapture::clipLengthForTake(0.0, 4.0), 4.0));
}
