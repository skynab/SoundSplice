#include <catch2/catch_test_macros.hpp>

#include <engine/DefaultContent.h>

using namespace looper::engine;

namespace
{
    // Restated rather than included from GuitarNode.h: that header pulls in
    // JUCE, and looper_tests deliberately doesn't link it (see
    // tests/CMakeLists.txt). DefaultContent.h is JUCE-free for the same
    // reason, which is exactly why makeDefaultGuitarRiffPattern takes the
    // open note as a parameter instead of reaching for kStandardTuning.
    constexpr int kStandardLowE = 40; // engine::kStandardTuning[0]
    constexpr int kHighestFret  = 24; // engine::kMaxFret

    int countAt(const Pattern& p, double beat, int note)
    {
        int n = 0;
        for (const auto& note_ : p.notes)
            if (note_.startBeats == beat && note_.noteNumber == note)
                ++n;
        return n;
    }
}

TEST_CASE("The default drum loop is one bar", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(pattern.lengthBeats == 4.0);
}

TEST_CASE("The default drum loop puts the kick on 1 and 3", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(countAt(pattern, 0.0, kDefaultKickNote) == 1);
    REQUIRE(countAt(pattern, 2.0, kDefaultKickNote) == 1);
    // Not also sounding where the snare is - a kick on every beat isn't the
    // pattern this claims to be.
    REQUIRE(countAt(pattern, 1.0, kDefaultKickNote) == 0);
    REQUIRE(countAt(pattern, 3.0, kDefaultKickNote) == 0);
}

TEST_CASE("The default drum loop puts the snare on 2 and 4", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(countAt(pattern, 1.0, kDefaultSnareNote) == 1);
    REQUIRE(countAt(pattern, 3.0, kDefaultSnareNote) == 1);
}

TEST_CASE("The default drum loop plays a closed hat on every eighth note", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    int hats = 0;
    for (const auto& note : pattern.notes)
        if (note.noteNumber == kDefaultHatNote)
            ++hats;
    REQUIRE(hats == 8); // eight eighth-notes across one 4/4 bar

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
        REQUIRE(countAt(pattern, beat, kDefaultHatNote) == 1);
}

TEST_CASE("Nothing in the default loop falls outside the bar", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("The default guitar riff is one bar of eighth notes", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultGuitarRiffPattern(kStandardLowE);
    REQUIRE(pattern.lengthBeats == 4.0);
    REQUIRE(pattern.notes.size() == 8);

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
    {
        int atBeat = 0;
        for (const auto& note : pattern.notes)
            if (note.startBeats == beat)
                ++atBeat;
        INFO("beat " << beat);
        REQUIRE(atBeat == 1);
    }
}

TEST_CASE("The default guitar riff transposes with the tuning it's given", "[engine][defaultcontent]")
{
    // The property that makes the riff usable on a dropped tuning at all:
    // pitches are offsets from the low string, so handing it a different
    // open note moves the whole riff rather than leaving it unplayable.
    const auto standard = makeDefaultGuitarRiffPattern(kStandardLowE);
    const auto dropped  = makeDefaultGuitarRiffPattern(kStandardLowE - 4);

    REQUIRE(standard.notes.size() == dropped.notes.size());
    for (size_t i = 0; i < standard.notes.size(); ++i)
    {
        REQUIRE(dropped.notes[i].startBeats == standard.notes[i].startBeats);
        REQUIRE(dropped.notes[i].noteNumber == standard.notes[i].noteNumber - 4);
    }
}

TEST_CASE("Every note in the default guitar riff is reachable on the low string",
          "[engine][defaultcontent]")
{
    // A guitar sounds nothing it can't fret: below the open string there is
    // no fret at all, and past kHighestFret there's no neck. A riff that broke
    // this would simply be silent, which is the failure this catches.
    for (int lowString : { kStandardLowE, 36, 34 }) // standard E, drop C, drop B
    {
        const auto pattern = makeDefaultGuitarRiffPattern(lowString);
        INFO("low string " << lowString);
        for (const auto& note : pattern.notes)
        {
            REQUIRE(note.noteNumber >= lowString);
            REQUIRE(note.noteNumber <= lowString + kHighestFret);
        }
    }
}

TEST_CASE("Nothing in the default guitar riff falls outside the bar, or overlaps itself",
          "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultGuitarRiffPattern(kStandardLowE);
    for (size_t i = 0; i < pattern.notes.size(); ++i)
    {
        const auto& note = pattern.notes[i];
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);

        // Repeated chugs are the same pitch back to back, so this is the one
        // pattern in the app most able to trip the same-pitch overlap that
        // clampNoteLengths exists for - see GenerativeLoop.h.
        for (size_t j = i + 1; j < pattern.notes.size(); ++j)
            if (pattern.notes[j].noteNumber == note.noteNumber)
                REQUIRE(pattern.notes[j].startBeats >= note.startBeats + note.lengthBeats - 1.0e-9);
    }
}

// --- The starter song's material -------------------------------------------
//
// These check the musical claims the starter song makes, which are otherwise
// only checkable by ear: that every part is in one key, that the arrangement's
// pieces tile without holes, and that nothing is unplayable on the instrument
// it was written for.

namespace
{
    constexpr int kDropCLow = 36; // the Modern Metal preset's lowest open string

    bool isInNaturalMinor(int noteNumber, int rootNote)
    {
        int degree = (noteNumber - rootNote) % 12;
        if (degree < 0)
            degree += 12;

        for (int step : kNaturalMinorSteps)
            if (step == degree)
                return true;
        return false;
    }

    int countNotes(const Pattern& p, int noteNumber)
    {
        int n = 0;
        for (const auto& note : p.notes)
            if (note.noteNumber == noteNumber)
                ++n;
        return n;
    }
}

TEST_CASE("The starter progression stays in the natural minor scale", "[engine][defaultcontent]")
{
    // The reason a melody can be written from the scale alone without dodging
    // a chord tone — if a chord left the scale, that would stop being true.
    for (int chord : kStarterProgression)
        REQUIRE(isInNaturalMinor(chord, 0));
}

TEST_CASE("The starter drum intro holds back the backbeat", "[engine][defaultcontent]")
{
    const auto pattern = makeStarterDrumIntroPattern();
    REQUIRE(pattern.lengthBeats == kStarterCycleBeats);

    // No snare at all until the pickup in the final bar — that contrast is the
    // whole point of having an intro section.
    for (const auto& note : pattern.notes)
        if (note.noteNumber == kDefaultSnareNote)
            REQUIRE(note.startBeats >= 3.0 * kStarterBeatsPerBar);

    REQUIRE(countNotes(pattern, kDefaultSnareNote) == 4); // the pickup
}

TEST_CASE("The starter groove is four bars of the default beat", "[engine][defaultcontent]")
{
    const auto pattern = makeStarterDrumGroovePattern(false);
    REQUIRE(pattern.lengthBeats == kStarterCycleBeats);

    // Built by repeating the one-bar loop, so the kick lands on 1 and 3 of
    // every bar — the property that would break first if the repeat were
    // misplaced.
    for (int bar = 0; bar < kStarterCycleBars; ++bar)
    {
        const double barStart = (double) bar * kStarterBeatsPerBar;
        REQUIRE(countAt(pattern, barStart,       kDefaultKickNote) == 1);
        REQUIRE(countAt(pattern, barStart + 2.0, kDefaultKickNote) == 1);
        REQUIRE(countAt(pattern, barStart + 1.0, kDefaultSnareNote) == 1);
        REQUIRE(countAt(pattern, barStart + 3.0, kDefaultSnareNote) == 1);
    }
}

TEST_CASE("The starter groove sounds the kit's fourth pad", "[engine][defaultcontent]")
{
    // Otherwise the clap in the default kit never sounds on first launch, and
    // a pad that is loaded but silent looks broken.
    REQUIRE(countNotes(makeStarterDrumGroovePattern(false), kDefaultClapNote) > 0);
}

TEST_CASE("The starter groove's fill replaces the last half-bar", "[engine][defaultcontent]")
{
    const auto plain  = makeStarterDrumGroovePattern(false);
    const auto filled = makeStarterDrumGroovePattern(true);

    REQUIRE(filled.lengthBeats == plain.lengthBeats);
    REQUIRE(countNotes(filled, kDefaultSnareNote) > countNotes(plain, kDefaultSnareNote));

    const double lastBar = 3.0 * kStarterBeatsPerBar;

    // The fill is sixteenths, i.e. finer than the eighth-note grid everything
    // else sits on.
    bool foundSixteenth = false;
    for (const auto& note : filled.notes)
        if (note.noteNumber == kDefaultSnareNote && note.startBeats > lastBar + 2.0)
        {
            const double intoBeat = note.startBeats - (double) (int) note.startBeats;
            if (intoBeat == 0.25 || intoBeat == 0.75)
                foundSixteenth = true;
        }
    REQUIRE(foundSixteenth);

    // And nothing runs past the cycle it belongs to.
    for (const auto& note : filled.notes)
        REQUIRE(note.startBeats + note.lengthBeats <= filled.lengthBeats + 1.0e-9);
}

TEST_CASE("The starter bass follows the progression", "[engine][defaultcontent]")
{
    const int  root    = kDropCLow + 12;
    const auto pattern = makeStarterBassPattern(root);

    REQUIRE(pattern.lengthBeats == kStarterCycleBeats);

    // Each bar's downbeat is that bar's chord root.
    for (int bar = 0; bar < kStarterCycleBars; ++bar)
    {
        const double barStart = (double) bar * kStarterBeatsPerBar;
        REQUIRE(countAt(pattern, barStart, root + kStarterProgression[bar]) == 1);
    }
}

TEST_CASE("Every starter bass note is in key", "[engine][defaultcontent]")
{
    // Roots, fifths and octaves of diatonic chords are all diatonic — if this
    // fails, the line has left the key the guitar and lead are playing in.
    const int root = kDropCLow + 12;
    for (const auto& note : makeStarterBassPattern(root).notes)
        REQUIRE(isInNaturalMinor(note.noteNumber, root));
}

TEST_CASE("The starter guitar riff follows the progression and is playable", "[engine][defaultcontent]")
{
    const auto pattern = makeStarterGuitarRiffPattern(kDropCLow);
    REQUIRE(pattern.lengthBeats == kStarterCycleBeats);

    for (int bar = 0; bar < kStarterCycleBars; ++bar)
    {
        const double barStart = (double) bar * kStarterBeatsPerBar;
        REQUIRE(countAt(pattern, barStart, kDropCLow + kStarterProgression[bar]) == 1);
    }

    // Nothing below the lowest open string (unreachable — no string can sound
    // it) or past the last fret. The same constraint the one-bar riff has.
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.noteNumber >= kDropCLow);
        REQUIRE(note.noteNumber <= kDropCLow + kHighestFret);
    }
}

TEST_CASE("The starter guitar riff transposes with the tuning", "[engine][defaultcontent]")
{
    // The whole reason the open string is a parameter: a riff hardcoded to one
    // tuning is silent or unplayable in another.
    const auto dropped = makeStarterGuitarRiffPattern(kDropCLow);
    const auto standard = makeStarterGuitarRiffPattern(kStandardLowE);

    REQUIRE(dropped.notes.size() == standard.notes.size());
    for (size_t i = 0; i < dropped.notes.size(); ++i)
        REQUIRE(standard.notes[i].noteNumber - dropped.notes[i].noteNumber
                == kStandardLowE - kDropCLow);
}

TEST_CASE("Both starter lead phrases are two bars and in key", "[engine][defaultcontent]")
{
    const int root = kDropCLow + 24;

    for (int phrase = 0; phrase < 2; ++phrase)
    {
        const auto pattern = makeStarterLeadPattern(root, phrase);

        REQUIRE(pattern.lengthBeats == 2.0 * kStarterBeatsPerBar);
        REQUIRE_FALSE(pattern.notes.empty());

        for (const auto& note : pattern.notes)
        {
            REQUIRE(isInNaturalMinor(note.noteNumber, root));
            REQUIRE(note.startBeats >= 0.0);
            REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
        }
    }
}

TEST_CASE("The two lead phrases are different", "[engine][defaultcontent]")
{
    // A call and an answer that are the same thing is just a repeat, and there
    // would be no reason to split the clip.
    const int root = kDropCLow + 24;
    REQUIRE_FALSE(makeStarterLeadPattern(root, 0).notes == makeStarterLeadPattern(root, 1).notes);
}

TEST_CASE("The answering lead phrase ends on the tonic", "[engine][defaultcontent]")
{
    const int  root    = kDropCLow + 24;
    const auto pattern = makeStarterLeadPattern(root, 1);

    const Note* last = nullptr;
    for (const auto& note : pattern.notes)
        if (last == nullptr || note.startBeats > last->startBeats)
            last = &note;

    REQUIRE(last != nullptr);
    REQUIRE((last->noteNumber - root) % 12 == 0); // resolves, so the cycle loops cleanly
}

TEST_CASE("appendPatternAt shifts notes and grows the length", "[engine][defaultcontent]")
{
    Pattern source;
    source.lengthBeats = 4.0;
    source.notes.push_back({ 0.0, 1.0, 60, 0.8f });

    Pattern destination;
    destination.lengthBeats = 4.0;
    appendPatternAt(destination, source, 4.0, 3);

    REQUIRE(destination.lengthBeats == 8.0);
    REQUIRE(destination.notes.size() == 1);
    REQUIRE(destination.notes[0].startBeats == 4.0);
    REQUIRE(destination.notes[0].noteNumber == 63);
}
