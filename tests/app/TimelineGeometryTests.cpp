#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <app/TimelineGeometry.h>

using Catch::Approx;
using looper::TimelineGeometry;

TEST_CASE("TimelineGeometry converts beats <-> x at zoom 1", "[app][timeline]")
{
    TimelineGeometry g;
    REQUIRE(g.xForBeat(0.0) == Approx(g.gutterWidth));
    REQUIRE(g.xForBeat(1.0) == Approx(g.gutterWidth + g.basePixelsPerBeat));

    REQUIRE(g.beatForX(g.gutterWidth) == Approx(0.0));
    REQUIRE(g.beatForX(g.gutterWidth + g.basePixelsPerBeat) == Approx(1.0));
}

TEST_CASE("TimelineGeometry beatForX clamps to zero left of the gutter", "[app][timeline]")
{
    TimelineGeometry g;
    REQUIRE(g.beatForX(0.0f) == Approx(0.0));
    REQUIRE(g.beatForX(g.gutterWidth - 5.0f) == Approx(0.0));
}

TEST_CASE("TimelineGeometry zoom scales pixels per beat", "[app][timeline]")
{
    TimelineGeometry g;
    g.zoom = 2.0f;
    REQUIRE(g.pixelsPerBeat() == Approx(g.basePixelsPerBeat * 2.0f));
    REQUIRE(g.xForBeat(1.0) == Approx(g.gutterWidth + g.basePixelsPerBeat * 2.0f));

    g.zoom = 0.5f;
    REQUIRE(g.pixelsPerBeat() == Approx(g.basePixelsPerBeat * 0.5f));
}

TEST_CASE("TimelineGeometry xForBeat and beatForX round-trip", "[app][timeline]")
{
    TimelineGeometry g;
    g.zoom = 1.5f;
    for (double beat : { 0.0, 1.0, 3.25, 10.0, 63.75 })
        REQUIRE(g.beatForX(g.xForBeat(beat)) == Approx(beat).margin(1e-4));
}

TEST_CASE("TimelineGeometry computes content size from song span", "[app][timeline]")
{
    TimelineGeometry g;
    REQUIRE(g.contentWidth(0.0) == Approx(g.gutterWidth));
    REQUIRE(g.contentWidth(4.0) == Approx(g.gutterWidth + 4.0f * g.basePixelsPerBeat));

    REQUIRE(g.contentHeight(0) == Approx(g.rulerHeight + g.laneHeight)); // at least one lane
    REQUIRE(g.contentHeight(3) == Approx(g.rulerHeight + g.laneHeight * 3.0f));
}
