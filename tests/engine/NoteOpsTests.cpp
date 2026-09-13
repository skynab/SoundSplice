#include <catch2/catch_test_macros.hpp>

#include <engine/NoteOps.h>

using namespace looper::engine;

namespace
{
    constexpr double kStep = 0.25; // a 16th note

    Note noteAt(double startBeats, int noteNumber = 60)
    {
        return { startBeats, kStep, noteNumber, 0.8f };
    }

    bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }
}

TEST_CASE("Quantize snaps notes to the nearest step", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.03), noteAt(0.27), noteAt(0.71) };

    NoteOps::quantizeNotes(notes, kStep, 0.0);

    REQUIRE(near(notes[0].startBeats, 0.0));
    REQUIRE(near(notes[1].startBeats, 0.25));
    REQUIRE(near(notes[2].startBeats, 0.75));
}

TEST_CASE("Quantize leaves an already-quantized pattern alone", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.0), noteAt(0.25), noteAt(0.5) };
    const auto        before = notes;

    NoteOps::quantizeNotes(notes, kStep, 0.0);

    for (size_t i = 0; i < notes.size(); ++i)
        REQUIRE(near(notes[i].startBeats, before[i].startBeats));
}

TEST_CASE("Swing delays off-beats and leaves down-beats alone", "[engine][noteops]")
{
    // Steps 0 and 2 are on the beat grid; 1 and 3 are the off-beats swing moves.
    std::vector<Note> notes { noteAt(0.0), noteAt(0.25), noteAt(0.5), noteAt(0.75) };

    NoteOps::quantizeNotes(notes, kStep, 0.5);

    REQUIRE(near(notes[0].startBeats, 0.0));
    REQUIRE(near(notes[1].startBeats, 0.25 + 0.5 * kStep));
    REQUIRE(near(notes[2].startBeats, 0.5));
    REQUIRE(near(notes[3].startBeats, 0.75 + 0.5 * kStep));
}

TEST_CASE("Swing is idempotent, even when heavy", "[engine][noteops]")
{
    // The failure this guards: quantizing to the nearest *step* and then
    // nudging would push a heavily swung note onto the following step the
    // second time it's applied, so repeating the command would walk the part
    // forwards. Quantizing to the swung grid itself can't do that.
    std::vector<Note> notes { noteAt(0.0), noteAt(0.25), noteAt(0.5), noteAt(0.75) };

    NoteOps::quantizeNotes(notes, kStep, 0.66);
    const auto afterFirst = notes;

    NoteOps::quantizeNotes(notes, kStep, 0.66);
    NoteOps::quantizeNotes(notes, kStep, 0.66);

    for (size_t i = 0; i < notes.size(); ++i)
        REQUIRE(near(notes[i].startBeats, afterFirst[i].startBeats));
}

TEST_CASE("Swing never reorders notes", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.0), noteAt(0.25), noteAt(0.5), noteAt(0.75) };

    NoteOps::quantizeNotes(notes, kStep, 5.0); // absurd amount, clamped

    for (size_t i = 1; i < notes.size(); ++i)
        REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);
}

TEST_CASE("Quantize applies only to the selection when there is one", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.03), noteAt(0.27), noteAt(0.71) };

    NoteOps::quantizeNotes(notes, kStep, 0.0, { 1 });

    REQUIRE(near(notes[0].startBeats, 0.03)); // untouched
    REQUIRE(near(notes[1].startBeats, 0.25)); // quantized
    REQUIRE(near(notes[2].startBeats, 0.71)); // untouched
}

TEST_CASE("An out-of-range selection index is ignored", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.03) };

    NoteOps::quantizeNotes(notes, kStep, 0.0, { 7, -1 });

    REQUIRE(near(notes[0].startBeats, 0.03));
}

TEST_CASE("Quantize never moves a note before the start", "[engine][noteops]")
{
    std::vector<Note> notes { noteAt(0.01) };

    NoteOps::quantizeNotes(notes, kStep, 0.5);

    REQUIRE(notes[0].startBeats >= 0.0);
}
