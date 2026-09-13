#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <app/PianoRollGeometry.h>

using Catch::Approx;
using looper::PianoRollGeometry;

TEST_CASE("PianoRollGeometry pitchForRow and rowForPitch round-trip", "[app][pianoroll]")
{
    PianoRollGeometry g;
    for (int row = 0; row < g.numRows; ++row)
        REQUIRE(g.rowForPitch(g.pitchForRow(row)) == row);

    // Row 0 is the highest pitch, the last row the lowest.
    REQUIRE(g.pitchForRow(0) == g.lowPitch + g.numRows - 1);
    REQUIRE(g.pitchForRow(g.numRows - 1) == g.lowPitch);
}

TEST_CASE("PianoRollGeometry cellAt rejects the gutter", "[app][pianoroll]")
{
    PianoRollGeometry g;
    const float width = 400.0f, height = 240.0f;
    int row = -1, step = -1;

    REQUIRE_FALSE(g.cellAt(0.0f, 10.0f, width, height, row, step));
    REQUIRE_FALSE(g.cellAt(g.gutterWidth - 1.0f, 10.0f, width, height, row, step));
}

TEST_CASE("PianoRollGeometry cellAt locates the correct row and step", "[app][pianoroll]")
{
    PianoRollGeometry g;
    const float width = 440.0f, height = 240.0f; // gutter 40 + 400 grid -> 25px/step
    int row = -1, step = -1;

    REQUIRE(g.cellAt(g.gutterWidth + 1.0f, 1.0f, width, height, row, step));
    REQUIRE(row == 0);
    REQUIRE(step == 0);

    // Last step, last row: just inside the bottom-right corner.
    REQUIRE(g.cellAt(width - 1.0f, height - 1.0f, width, height, row, step));
    REQUIRE(row == g.numRows - 1);
    REQUIRE(step == g.numSteps - 1);
}

TEST_CASE("PianoRollGeometry cellAt rejects points beyond the grid", "[app][pianoroll]")
{
    PianoRollGeometry g;
    const float width = 440.0f, height = 240.0f;
    int row = -1, step = -1;

    REQUIRE_FALSE(g.cellAt(width + 5.0f, 1.0f, width, height, row, step));
    REQUIRE_FALSE(g.cellAt(g.gutterWidth + 1.0f, height + 5.0f, width, height, row, step));
}

TEST_CASE("PianoRollGeometry xForStep and yForRow agree with cellAt", "[app][pianoroll]")
{
    PianoRollGeometry g;
    const float width = 440.0f, height = 240.0f;

    for (int step = 0; step < g.numSteps; ++step)
    {
        for (int row = 0; row < g.numRows; ++row)
        {
            const float x = g.xForStep(step, width) + 1.0f;
            const float y = g.yForRow(row, height) + 1.0f;
            int foundRow = -1, foundStep = -1;
            REQUIRE(g.cellAt(x, y, width, height, foundRow, foundStep));
            REQUIRE(foundRow == row);
            REQUIRE(foundStep == step);
        }
    }
}

TEST_CASE("setPitchRange keeps the window inside the MIDI range", "[app][pianoroll]")
{
    PianoRollGeometry g;

    g.setPitchRange(-20, 24);
    REQUIRE(g.lowPitch == 0);

    // The top row must never exceed 127, so lowPitch is pulled down to fit.
    g.setPitchRange(120, 24);
    REQUIRE(g.highestPitch() == 127);
    REQUIRE(g.lowPitch == 128 - 24);
}

TEST_CASE("Zoom is clamped to a usable number of rows", "[app][pianoroll]")
{
    PianoRollGeometry g;

    g.zoomBy(-1000);
    REQUIRE(g.numRows == PianoRollGeometry::kMinRows);

    g.zoomBy(1000);
    REQUIRE(g.numRows == PianoRollGeometry::kMaxRows);
}

TEST_CASE("Zooming out near the top of the range stays in bounds", "[app][pianoroll]")
{
    PianoRollGeometry g;
    g.setPitchRange(127, 12); // clamped to sit against the ceiling
    REQUIRE(g.highestPitch() == 127);

    g.zoomBy(24); // more rows have to come from below, not above 127
    REQUIRE(g.highestPitch() == 127);
    REQUIRE(g.lowPitch >= 0);
}

TEST_CASE("Scrolling the pitch window round-trips and clamps at the ends", "[app][pianoroll]")
{
    PianoRollGeometry g;
    const int start = g.lowPitch;

    g.scrollPitchBy(12);
    REQUIRE(g.lowPitch == start + 12);
    g.scrollPitchBy(-12);
    REQUIRE(g.lowPitch == start);

    g.scrollPitchBy(-1000);
    REQUIRE(g.lowPitch == 0);
    g.scrollPitchBy(1000);
    REQUIRE(g.highestPitch() == 127);
}

TEST_CASE("pitchForRow and rowForPitch still agree after scroll and zoom", "[app][pianoroll]")
{
    PianoRollGeometry g;
    g.setPitchRange(36, 18);

    for (int row = 0; row < g.numRows; ++row)
        REQUIRE(g.rowForPitch(g.pitchForRow(row)) == row);
}
