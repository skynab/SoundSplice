#include <catch2/catch_test_macros.hpp>

#include <engine/GuitarChords.h>

#include <stdexcept>
#include <string>

using namespace looper::engine;

namespace
{
    constexpr int kStandard[6] = { 40, 45, 50, 55, 59, 64 }; // E2 A2 D3 G3 B3 E4

    const ChordShape& shapeNamed(const char* name)
    {
        for (const auto& shape : kChordShapes)
            if (std::string(shape.name) == name)
                return shape;
        throw std::runtime_error("no such shape");
    }
}

TEST_CASE("An open E is the notes a guitarist would play", "[engine][chords]")
{
    // 0-2-2-1-0-0 in standard tuning: E B E G# B E. Getting this wrong would
    // be a chord that's *nearly* right, which is harder to notice than one
    // that's obviously broken.
    const auto notes = GuitarChords::notesForShape(shapeNamed("E"), kStandard);
    REQUIRE(notes == std::vector<int> { 40, 47, 52, 56, 59, 64 });
}

TEST_CASE("Unplayed strings are skipped, not silently sounded", "[engine][chords]")
{
    // A major is x-0-2-2-2-0: the low E isn't struck at all. A chord voicing
    // that quietly added it would be a different chord.
    const auto notes = GuitarChords::notesForShape(shapeNamed("A"), kStandard);
    REQUIRE(notes.size() == 5);
    REQUIRE(notes.front() == 45); // starts on the open A, not the low E

    const auto d = GuitarChords::notesForShape(shapeNamed("D"), kStandard);
    REQUIRE(d == std::vector<int> { 50, 57, 62, 66 });
}

TEST_CASE("Every shape produces at least a triad", "[engine][chords]")
{
    for (const auto& shape : kChordShapes)
    {
        const auto notes = GuitarChords::notesForShape(shape, kStandard);
        INFO("shape " << shape.name);
        REQUIRE(notes.size() >= 3);
        for (int note : notes)
            REQUIRE((note >= 0 && note <= 127));
    }
}

TEST_CASE("A fret offset moves the shape up the neck", "[engine][chords]")
{
    // The E shape barred at fret 3 is a G — every fretted note up three
    // semitones. Open strings move too, since a barre replaces the nut.
    const auto open   = GuitarChords::notesForShape(shapeNamed("E"), kStandard, 0);
    const auto barred = GuitarChords::notesForShape(shapeNamed("E"), kStandard, 3);

    REQUIRE(open.size() == barred.size());
    for (size_t i = 0; i < open.size(); ++i)
        REQUIRE(barred[i] == open[i] + 3);
}

TEST_CASE("A downstroke strikes low to high, an upstroke high to low", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 20.0;

    strum.downstroke = true;
    const auto down = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(down.front().noteNumber == 40); // low E first
    REQUIRE(down.back().noteNumber == 64);

    strum.downstroke = false;
    const auto up = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(up.front().noteNumber == 64); // high E first
    REQUIRE(up.back().noteNumber == 40);
}

TEST_CASE("Strummed notes are staggered, not simultaneous", "[engine][chords]")
{
    // The whole point: six notes at the same instant read as an organ.
    StrumSettings strum;
    strum.spreadMs = 24.0;

    const auto notes = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(notes.size() == 6);

    for (size_t i = 1; i < notes.size(); ++i)
        REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);

    // 24ms at 120bpm is 0.048 beats from first string to last.
    const double span = notes.back().startBeats - notes.front().startBeats;
    REQUIRE(std::abs(span - 0.048) < 0.002);
}

TEST_CASE("A zero spread is a simultaneous chord", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 0.0;

    const auto notes = GuitarChords::strumChord(shapeNamed("G"), kStandard, 0, 2.0, 1.0, 120.0, strum);
    for (const auto& note : notes)
        REQUIRE(std::abs(note.startBeats - 2.0) < 1.0e-9);
}

TEST_CASE("Humanising never reorders the strokes", "[engine][chords]")
{
    // Jitter is capped below half the gap between strings precisely so this
    // holds: a hand moving across the strings cannot hit the fourth before
    // the third, however loose its timing.
    StrumSettings strum;
    strum.spreadMs = 15.0;
    strum.humanise = 1.0;

    for (uint32_t seed = 1; seed <= 50; ++seed)
    {
        const auto notes = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 1.0, 1.0, 140.0, strum, seed);
        INFO("seed " << seed);
        for (size_t i = 1; i < notes.size(); ++i)
            REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);
    }
}

TEST_CASE("Humanising varies the timing it is given", "[engine][chords]")
{
    // ...but it must actually do something, or the control is decorative.
    StrumSettings straight;
    straight.spreadMs = 15.0;
    straight.humanise = 0.0;

    StrumSettings loose = straight;
    loose.humanise = 1.0;

    const auto exact    = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 0.0, 1.0, 120.0, straight, 7);
    const auto humanised = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 0.0, 1.0, 120.0, loose, 7);

    bool anyDifferent = false;
    for (size_t i = 0; i < exact.size(); ++i)
        if (std::abs(exact[i].startBeats - humanised[i].startBeats) > 1.0e-9)
            anyDifferent = true;

    REQUIRE(anyDifferent);
}

TEST_CASE("A strum never places a note before the bar it was asked for", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 30.0;
    strum.humanise = 1.0;

    for (uint32_t seed = 1; seed <= 20; ++seed)
    {
        const auto notes = GuitarChords::strumChord(shapeNamed("Am"), kStandard, 0, 0.0, 1.0, 120.0, strum, seed);
        for (const auto& note : notes)
            REQUIRE(note.startBeats >= 0.0);
    }
}

TEST_CASE("A power chord on the low E is root, fifth and octave", "[engine][chords]")
{
    // 3rd fret of the low E is G. G5 is G, D, G.
    const auto notes = GuitarChords::notesForRoot(MovableShape::Power, kStandard, 0, 3);
    REQUIRE(notes == std::vector<int> { 43, 50, 55 });
}

TEST_CASE("A power chord stays a fifth across the G-B pair", "[engine][chords]")
{
    // Rooted on the G string, the familiar "two frets up on each of the next
    // two strings" fingering is WRONG: G to B is a major third, not a fourth,
    // so that shape gives a sixth and a minor seventh rather than a fifth and
    // an octave. Solving each interval against the real tuning can't make
    // that mistake.
    const auto notes = GuitarChords::notesForRoot(MovableShape::Power, kStandard, 3, 5);
    REQUIRE(notes.size() == 3);
    REQUIRE(notes[1] - notes[0] == 7);  // a fifth, whatever the strings are tuned to
    REQUIRE(notes[2] - notes[0] == 12); // an octave
}

TEST_CASE("A stacked shape keeps its intervals wherever it is rooted", "[engine][chords]")
{
    // The whole promise of solving against the tuning: the same click gives
    // the same chord anywhere on the neck, on any string, in any tuning.
    for (auto shape : { MovableShape::Power, MovableShape::Octave })
    {
        const auto wanted = intervalsForShape(shape);

        for (int rootString = 0; rootString + (int) wanted.size() <= 6; ++rootString)
        {
            for (int fret = 0; fret <= 10; ++fret)
            {
                const auto notes = GuitarChords::notesForRoot(shape, kStandard, rootString, fret);
                INFO("shape " << movableShapeName(shape) << " string " << rootString << " fret " << fret);
                REQUIRE(notes.size() == wanted.size());

                for (size_t i = 0; i < notes.size(); ++i)
                    REQUIRE(notes[i] - notes[0] == wanted[i]);
            }
        }
    }
}

TEST_CASE("Major and minor come out as barre chords, not stacks", "[engine][chords]")
{
    // An E-shape barre at the 3rd fret is G major: the same notes as the open
    // E shape, every one of them three semitones up.
    const auto open   = GuitarChords::notesForShape(kChordShapes[0], kStandard, 0); // "E"
    const auto barred = GuitarChords::notesForRoot(MovableShape::Major, kStandard, 0, 3);

    REQUIRE(barred.size() == open.size());
    for (size_t i = 0; i < barred.size(); ++i)
        REQUIRE(barred[i] == open[i] + 3);

    // Rooted on the A string it's the A shape instead — five strings, not six.
    const auto aShape = GuitarChords::notesForRoot(MovableShape::Major, kStandard, 1, 3);
    REQUIRE(aShape.size() == 5);
    REQUIRE(aShape.front() == kStandard[1] + 3); // rooted where it was clicked
}

TEST_CASE("A minor barre differs from the major by its third", "[engine][chords]")
{
    // The one note that distinguishes them; getting it wrong is a chord
    // that's nearly right, which is harder to notice than one that isn't.
    const auto major = GuitarChords::notesForRoot(MovableShape::Major, kStandard, 0, 5);
    const auto minor = GuitarChords::notesForRoot(MovableShape::Minor, kStandard, 0, 5);

    REQUIRE(major.size() == minor.size());

    int differences = 0;
    for (size_t i = 0; i < major.size(); ++i)
        if (major[i] != minor[i])
        {
            ++differences;
            REQUIRE(major[i] - minor[i] == 1); // flattened by a semitone
        }

    REQUIRE(differences == 1);
}

TEST_CASE("A power chord follows the tuning into drop D", "[engine][chords]")
{
    // Drop D lowers the sixth string a whole tone, so the same fret is a
    // different chord — and the shape above it has to move with it.
    constexpr int dropD[6] = { 38, 45, 50, 55, 59, 64 };

    const auto standard = GuitarChords::notesForRoot(MovableShape::Power, kStandard, 0, 3);
    const auto dropped  = GuitarChords::notesForRoot(MovableShape::Power, dropD, 0, 3);

    REQUIRE(dropped[0] == standard[0] - 2); // the root moved down a tone
    REQUIRE(dropped[1] - dropped[0] == 7);  // and it's still a power chord
    REQUIRE(dropped[2] - dropped[0] == 12);
}

TEST_CASE("A shape rooted too high up the neck drops what won't fit", "[engine][chords]")
{
    // At the top fret the octave would need a fret past the end of the neck.
    // Dropping it beats inventing a fret that doesn't exist.
    const auto notes = GuitarChords::notesForRoot(MovableShape::Power, kStandard, 0, 22, 22);
    REQUIRE(notes.size() < 3);
    for (int note : notes)
        REQUIRE(note <= kStandard[0] + 22 + 12);
}

TEST_CASE("A shape rooted on the top string still gives its root", "[engine][chords]")
{
    // There are no strings above it to carry the rest of the shape.
    const auto notes = GuitarChords::notesForRoot(MovableShape::Power, kStandard, 5, 5);
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0] == kStandard[5] + 5);
}

TEST_CASE("Rooting off the neck produces nothing, not a guess", "[engine][chords]")
{
    REQUIRE(GuitarChords::notesForRoot(MovableShape::Power, kStandard, -1, 3).empty());
    REQUIRE(GuitarChords::notesForRoot(MovableShape::Power, kStandard, 6, 3).empty());
    REQUIRE(GuitarChords::notesForRoot(MovableShape::Power, kStandard, 0, -1).empty());
    REQUIRE(GuitarChords::notesForRoot(MovableShape::Power, kStandard, 0, 99).empty());
}

TEST_CASE("A rooted chord is strummed like any other", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 20.0;

    const auto notes = GuitarChords::strumRootedChord(MovableShape::Power, kStandard, 0, 5,
                                                      0.0, 1.0, 120.0, strum);
    REQUIRE(notes.size() == 3);
    for (size_t i = 1; i < notes.size(); ++i)
        REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);
}
