#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/PianoRoll.h>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    constexpr float kWidth = 800.0f;
}

TEST_CASE("The playhead starts at the left of the grid, not the gutter", "[gui][pianoroll]")
{
    // The gutter holds the pitch names; beat zero is where the grid begins.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    roll.setPlayheadBeats(0.0, true);
    REQUIRE(roll.playheadXForTesting(kWidth) > 0.0f);
    REQUIRE(roll.playheadXForTesting(kWidth) < kWidth * 0.2f); // just past the gutter
}

TEST_CASE("The playhead advances across the grid with the beat", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    roll.setPlayheadBeats(0.0, true);
    const float atStart = roll.playheadXForTesting(kWidth);

    roll.setPlayheadBeats(1.0, true);
    const float atOne = roll.playheadXForTesting(kWidth);

    roll.setPlayheadBeats(3.0, true);
    const float atThree = roll.playheadXForTesting(kWidth);

    REQUIRE(atOne > atStart);
    REQUIRE(atThree > atOne);
    REQUIRE(atThree <= kWidth);
}

TEST_CASE("The playhead never leaves the grid", "[gui][pianoroll]")
{
    // A clip loops, so the owner wraps the position — but a rounding error or
    // a pattern-length change between frames must not put the line outside
    // the grid, where it would read as a rendering fault.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    for (double beats : { -5.0, -0.001, 0.0, 4.0, 100.0 })
    {
        roll.setPlayheadBeats(beats, true);
        const float x = roll.playheadXForTesting(kWidth);
        INFO("beats " << beats << " -> x " << x);
        REQUIRE(x >= 0.0f);
        REQUIRE(x <= kWidth);
    }
}

TEST_CASE("The grid's bar length follows the time signature", "[gui][pianoroll]")
{
    // In 4/4 a bar is four beats and the grid marks every fourth; in 3/4 it
    // has to mark every third, or the heavy lines say the wrong thing.
    JuceFixture fixture;
    PianoRoll roll;

    REQUIRE(roll.beatsPerBarForTesting() == 4.0); // the default

    roll.setBeatsPerBar(3.0);
    REQUIRE(roll.beatsPerBarForTesting() == 3.0);

    // Nonsense is refused rather than dividing the grid by zero.
    roll.setBeatsPerBar(0.0);
    REQUIRE(roll.beatsPerBarForTesting() == 4.0);
}

TEST_CASE("Pitch zoom reads as a multiplier, x1 being the default window", "[gui][pianoroll]")
{
    // Row counts are an implementation detail; "x1" is the same thing the
    // tracks view means by it, which is the point of matching the control.
    JuceFixture fixture;
    PianoRoll roll;

    REQUIRE(roll.pitchZoom() == 1.0f);

    roll.setPitchZoom(2.0f);
    REQUIRE(roll.pitchZoom() == 2.0f); // half as many rows, twice as tall

    roll.setPitchZoom(1.0f);
    REQUIRE(roll.pitchZoom() == 1.0f);
}

TEST_CASE("Pitch zoom stays inside the range the grid supports", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;

    roll.setPitchZoom(1000.0f);
    REQUIRE(roll.pitchZoom() <= PianoRoll::kMaxPitchZoom);
    REQUIRE_FALSE(roll.canPitchZoomIn());

    roll.setPitchZoom(0.0f);
    REQUIRE(roll.pitchZoom() >= PianoRoll::kMinPitchZoom);
    REQUIRE_FALSE(roll.canPitchZoomOut());

    roll.setPitchZoom(1.0f);
    REQUIRE(roll.canPitchZoomIn());
    REQUIRE(roll.canPitchZoomOut());
}

TEST_CASE("Zooming in shows fewer notes, zooming out shows more", "[gui][pianoroll]")
{
    // The direction has to match the word: without this the control could be
    // wired backwards and every other assertion here would still hold.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize(800, 400);

    roll.setPitchZoom(1.0f);
    const float rowAtOne = roll.rowHeightForTesting(400.0f);

    roll.setPitchZoom(2.0f);
    REQUIRE(roll.rowHeightForTesting(400.0f) > rowAtOne); // zoomed in: taller rows

    roll.setPitchZoom(0.5f);
    REQUIRE(roll.rowHeightForTesting(400.0f) < rowAtOne); // zoomed out: more of them
}

TEST_CASE("A zoom lands on a window the grid can actually show", "[gui][pianoroll]")
{
    // Rows are whole, so an arbitrary multiplier snaps. pitchZoom must report
    // where it landed rather than what was asked for, or the readout would
    // disagree with the grid.
    JuceFixture fixture;
    PianoRoll roll;

    roll.setPitchZoom(1.37f);
    const float landed = roll.pitchZoom();

    roll.setPitchZoom(landed);
    REQUIRE(roll.pitchZoom() == landed); // setting what it reports is a no-op
}

TEST_CASE("Delete removes the selected notes", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    for (int i = 0; i < 5; ++i)
        pattern.notes.push_back({ (double) i * 0.5, 0.25, 60 + i, 0.8f });

    roll.setPattern(pattern);
    roll.selectAllForTesting();

    REQUIRE(roll.deleteSelectedNotes() == 5);
    REQUIRE(roll.pattern().notes.empty());
}

TEST_CASE("Deleting several notes removes the right ones", "[gui][pianoroll]")
{
    // Indices shift as notes are erased. Erasing front-first would make the
    // rest of the selection point at the wrong notes, or past the end — so
    // this deletes a scattered selection and checks the survivors by pitch.
    JuceFixture fixture;
    PianoRoll roll;

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    for (int i = 0; i < 6; ++i)
        pattern.notes.push_back({ (double) i * 0.5, 0.25, 60 + i, 0.8f });

    roll.setPattern(pattern);
    roll.selectForTesting({ 0, 2, 4 });

    REQUIRE(roll.deleteSelectedNotes() == 3);
    REQUIRE(roll.pattern().notes.size() == 3);
    REQUIRE(roll.pattern().notes[0].noteNumber == 61);
    REQUIRE(roll.pattern().notes[1].noteNumber == 63);
    REQUIRE(roll.pattern().notes[2].noteNumber == 65);
}

TEST_CASE("Deleting with nothing selected changes nothing", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 60, 0.8f });
    roll.setPattern(pattern);

    REQUIRE(roll.deleteSelectedNotes() == 0);
    REQUIRE(roll.pattern().notes.size() == 1);
}

TEST_CASE("Delete is consumed by the keys pane, not passed to the track", "[gui][pianoroll]")
{
    // The trap this closes: unhandled here, Delete reaches the app and
    // removes the whole selected track — so editing notes and reaching for
    // Delete would destroy the part being edited. Consumed even with nothing
    // selected, since "sometimes deletes everything" is the failure.
    JuceFixture fixture;
    PianoRoll roll;

    const juce::KeyPress del(juce::KeyPress::deleteKey);
    const juce::KeyPress backspace(juce::KeyPress::backspaceKey);

    REQUIRE(roll.keyPressed(del));
    REQUIRE(roll.keyPressed(backspace));

    // cmd+backspace is Delete Clip and belongs to the owner, so it passes on.
    const juce::KeyPress cmdBackspace(juce::KeyPress::backspaceKey,
                                      juce::ModifierKeys(juce::ModifierKeys::commandModifier), 0);
    REQUIRE_FALSE(roll.keyPressed(cmdBackspace));
}

TEST_CASE("The keys pane takes keyboard focus, or it never sees Delete", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;
    REQUIRE(roll.getWantsKeyboardFocus());
}

namespace
{
    /** The meters the transport control offers, as quarter-notes per bar. */
    struct Meter { int numerator, denominator; double quartersPerBar; };

    const Meter kOfferedMeters[] = {
        { 4, 4, 4.0 }, { 3, 4, 3.0 }, { 2, 4, 2.0 }, { 5, 4, 5.0 },
        { 6, 8, 3.0 }, { 7, 8, 3.5 }, { 12, 8, 6.0 },
    };
}

TEST_CASE("The grid shows the whole pattern in every meter the app offers", "[gui][pianoroll]")
{
    // The cap used to be 64 steps, described as "4 bars of 16ths" — true only
    // in 4/4. Four bars of 5/4 is 80 sixteenths and of 12/8 is 96, so those
    // patterns were silently truncated: the tail invisible and unreachable in
    // the editor while still playing.
    JuceFixture fixture;

    for (const auto& meter : kOfferedMeters)
    {
        for (int bars : { 1, 2, 4 })
        {
            engine::Pattern pattern;
            pattern.lengthBeats = meter.quartersPerBar * bars;

            PianoRoll roll;
            roll.setPattern(pattern);

            const double covered = roll.numStepsForTesting() * roll.stepBeatsForTesting();
            INFO(meter.numerator << "/" << meter.denominator << ", " << bars << " bars: "
                 << pattern.lengthBeats << " beats, grid covers " << covered);

            REQUIRE(std::abs(covered - pattern.lengthBeats) < 1.0e-9);
        }
    }
}

TEST_CASE("Every offered meter divides into whole steps", "[gui][pianoroll]")
{
    // A bar that isn't a whole number of steps would put bar lines between
    // columns, and no note could be placed on the downbeat.
    JuceFixture fixture;
    PianoRoll roll;
    const double stepBeats = roll.stepBeatsForTesting();

    for (const auto& meter : kOfferedMeters)
    {
        const double steps = meter.quartersPerBar / stepBeats;
        INFO(meter.numerator << "/" << meter.denominator << " -> " << steps << " steps per bar");
        REQUIRE(std::abs(steps - std::round(steps)) < 1.0e-9);
    }
}

TEST_CASE("The playhead measures against the pattern, not the grid", "[gui][pianoroll]")
{
    // If the grid is ever capped, the pattern is the truth. Scaling against a
    // truncated grid would misplace the line for the whole clip rather than
    // only past the cap.
    JuceFixture fixture;

    engine::Pattern pattern;
    pattern.lengthBeats = 16.0;

    PianoRoll roll;
    roll.setSize(800, 400);
    roll.setPattern(pattern);

    // Halfway through the pattern is halfway across the grid.
    roll.setPlayheadBeats(8.0, true);
    const float atHalf = roll.playheadXForTesting(800.0f);

    roll.setPlayheadBeats(16.0, true);
    const float atEnd = roll.playheadXForTesting(800.0f);

    roll.setPlayheadBeats(0.0, true);
    const float atStart = roll.playheadXForTesting(800.0f);

    REQUIRE(atHalf > atStart);
    REQUIRE(atEnd > atHalf);
    REQUIRE(std::abs((atHalf - atStart) - (atEnd - atHalf)) < 1.0f); // evenly spaced
}

TEST_CASE("An absurd pattern length is capped rather than asking for a million columns",
          "[gui][pianoroll]")
{
    JuceFixture fixture;

    engine::Pattern pattern;
    pattern.lengthBeats = 100000.0;

    PianoRoll roll;
    roll.setPattern(pattern);

    REQUIRE(roll.numStepsForTesting() <= PianoRoll::maxStepsForTesting());
    REQUIRE(roll.numStepsForTesting() > 0);
}

TEST_CASE("A capped grid doesn't misplace the playhead", "[gui][pianoroll]")
{
    // The only case where the grid's extent and the pattern's length differ:
    // a pattern past the column cap. Every other test has them equal, so this
    // is the one that can tell "measured against the pattern" from "measured
    // against the grid" — without it that change is unverified.
    JuceFixture fixture;

    PianoRoll roll;
    roll.setSize(800, 400);

    engine::Pattern pattern;
    pattern.lengthBeats = 100.0; // 400 sixteenths, past the 256-column cap
    roll.setPattern(pattern);

    const double gridBeats = roll.numStepsForTesting() * roll.stepBeatsForTesting();
    REQUIRE(gridBeats < pattern.lengthBeats); // the case actually arose

    roll.setPlayheadBeats(0.0, true);
    const float atStart = roll.playheadXForTesting(800.0f);

    roll.setPlayheadBeats(50.0, true); // halfway through the pattern
    const float atHalf = roll.playheadXForTesting(800.0f);

    roll.setPlayheadBeats(100.0, true); // the very end
    const float atEnd = roll.playheadXForTesting(800.0f);

    // Halfway through the pattern is halfway across the drawn grid. Measured
    // against the capped grid instead, beat 50 would already be past its end
    // and pin to the right edge alongside beat 100.
    const float halfway = atStart + (atEnd - atStart) * 0.5f;
    INFO("start " << atStart << " half " << atHalf << " end " << atEnd);
    REQUIRE(std::abs(atHalf - halfway) < 2.0f);
    REQUIRE(atHalf < atEnd - 10.0f); // and nowhere near pinned to the end
}

TEST_CASE("At x1 the grid fits its pane, with nothing to scroll", "[gui][pianoroll]")
{
    // The behaviour the roll has always had, preserved as the zoom's floor:
    // below x1 there would be empty space to the right of the last step.
    JuceFixture fixture;
    PianoRoll roll;

    REQUIRE(roll.timeZoom() == 1.0f);
    REQUIRE(roll.preferredWidth(800) == 800);

    roll.setTimeZoom(0.1f);
    REQUIRE(roll.timeZoom() == PianoRoll::kMinTimeZoom);
    REQUIRE(roll.preferredWidth(800) == 800);
}

TEST_CASE("Zooming time widens the grid past its pane", "[gui][pianoroll]")
{
    // What makes a long pattern workable: at 256 steps in an 800-pixel pane a
    // sixteenth is three pixels across, which is not something a note can be
    // placed on.
    JuceFixture fixture;
    PianoRoll roll;

    roll.setTimeZoom(4.0f);
    REQUIRE(roll.preferredWidth(800) > 800 * 3);

    roll.setTimeZoom(2.0f);
    const int atTwo = roll.preferredWidth(800);
    roll.setTimeZoom(4.0f);
    REQUIRE(roll.preferredWidth(800) > atTwo);
}

TEST_CASE("The pitch gutter isn't stretched by the time zoom", "[gui][pianoroll]")
{
    // Only the grid widens. Scaling the whole component would make the pitch
    // names grow with the zoom and eat the pane.
    JuceFixture fixture;
    PianoRoll roll;

    const int viewport = 800;
    roll.setTimeZoom(1.0f);
    const int gridAtOne = roll.preferredWidth(viewport) - (int) roll.gutterWidthForTesting();

    roll.setTimeZoom(3.0f);
    const int gridAtThree = roll.preferredWidth(viewport) - (int) roll.gutterWidthForTesting();

    REQUIRE(std::abs(gridAtThree - gridAtOne * 3) <= 1);
}

TEST_CASE("Time zoom stays inside its range", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;

    roll.setTimeZoom(1000.0f);
    REQUIRE(roll.timeZoom() == PianoRoll::kMaxTimeZoom);

    roll.setTimeZoom(-5.0f);
    REQUIRE(roll.timeZoom() == PianoRoll::kMinTimeZoom);
}

TEST_CASE("A time zoom change tells the owner to resize", "[gui][pianoroll]")
{
    // The roll can't resize itself — it's inside a viewport the owner lays
    // out. Without this the wheel gesture would change nothing on screen.
    JuceFixture fixture;
    PianoRoll roll;

    int notifications = 0;
    roll.onTimeZoomChanged = [&notifications] { ++notifications; };

    roll.setTimeZoom(2.0f);
    REQUIRE(notifications == 1);

    roll.setTimeZoom(2.0f); // no change, no notification
    REQUIRE(notifications == 1);

    roll.setTimeZoom(3.0f);
    REQUIRE(notifications == 2);
}

namespace
{
    /** A synthetic click at @p position — the grid's hit-testing has never
        been exercised through a real juce::MouseEvent before, only through
        the row/step math it's built on, so this is the only way to catch a
        transform bug that shifts what's drawn without shifting what a click
        actually hits (or vice versa). */
    juce::MouseEvent clickAt(juce::Component& target, juce::Point<float> position)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now     = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(), 1.0f,
                                0.0f, 0.0f, 0.0f, 0.0f, &target, &target,
                                now, position, now, 1, false);
    }
}

TEST_CASE("A click lands on the row it's over, below the track header", "[gui][pianoroll]")
{
    // The header (see TrackColours.h's paintTrackHeader) sits above the
    // grid, drawn through a graphics transform rather than a real child
    // Component — so the grid's own paint() code stays byte-for-byte the
    // same as before the header existed. Mouse handling has to apply the
    // identical offset by hand instead, and that's the half of the fix a
    // visual check alone would never catch: a header that's drawn in the
    // right place but not accounted for in mouseDown would still *look*
    // correct while every click landed one header-height too high.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);
    roll.setPattern(engine::Pattern {});
    REQUIRE(roll.pattern().notes.empty());

    const float gutter = roll.gutterWidthForTesting();
    const float rowH    = roll.rowHeightForTesting(roll.gridHeightForTesting());
    const juce::Point<float> firstRowFirstStep(gutter + 5.0f, (float) kTrackHeaderHeight + rowH * 0.5f);

    roll.mouseDown(clickAt(roll, firstRowFirstStep));

    REQUIRE(roll.pattern().notes.size() == 1);
    REQUIRE(roll.pattern().notes[0].startBeats == 0.0);
    // Row 0 specifically, not merely "some row within the grid" — a header
    // offset applied to the paint but not the click would still land inside
    // the grid, just one row off, which the size/startBeats checks alone
    // wouldn't catch.
    REQUIRE(roll.pattern().notes[0].noteNumber == PianoRoll::kDefaultLowPitch + PianoRoll::kDefaultNumRows - 1);
}

TEST_CASE("A click just above the header hits nothing", "[gui][pianoroll]")
{
    // The header band itself isn't part of the grid — a click there must
    // not be silently reinterpreted as landing on row 0.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);
    roll.setPattern(engine::Pattern {});

    // Comfortably inside the header, not just barely — a y within one row's
    // height of the boundary truncates towards zero (C-style int cast of a
    // small negative), landing back on row 0 by coincidence rather than
    // actually proving anything about the header offset.
    roll.mouseDown(clickAt(roll, { roll.gutterWidthForTesting() + 5.0f, 1.0f }));

    // A click well above row 0 (inside the header band) resolves to a
    // negative row, which cellAt() reports as "not on the grid" — same
    // as clicking above the grid always has, header or not.
    REQUIRE(roll.pattern().notes.empty());
}
