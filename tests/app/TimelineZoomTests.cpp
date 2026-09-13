#include <catch2/catch_test_macros.hpp>

#include <app/TimelineZoom.h>

#include <cmath>

using namespace soundsplice;

TEST_CASE("Fitting a span makes it, with its margins, fill the width", "[app][timelinezoom]")
{
    // 100 beats at 24 px a beat is 2400 px; with 5% either side, 2640 px.
    const float zoom = app::zoomToFit(100.0, 1320.0f, 24.0f, 0.01f, 100.0f);
    REQUIRE(std::abs(zoom - 0.5f) < 1.0e-6f);

    // Clamped to the view's zoom range.
    REQUIRE(app::zoomToFit(100000.0, 800.0f, 24.0f, 0.25f, 4.0f) == 0.25f);
    REQUIRE(app::zoomToFit(0.01, 800.0f, 24.0f, 0.25f, 4.0f) == 4.0f);

    // Nothing to fit leaves the zoom at 1, inside the range.
    REQUIRE(app::zoomToFit(0.0, 800.0f, 24.0f, 0.25f, 4.0f) == 1.0f);
    REQUIRE(app::zoomToFit(10.0, 0.0f, 24.0f, 0.25f, 4.0f) == 1.0f);
}

TEST_CASE("Fitting tracks vertically shares the height between the lanes", "[app][timelinezoom]")
{
    // 400 px under a 22 px ruler is 378 px; six tracks get 63 px each.
    REQUIRE(app::laneHeightToFit(6, 400.0f, 22.0f, 24.0f, 160.0f) == 63.0f);

    // Too many tracks to fit still get a usable lane, and scroll.
    REQUIRE(app::laneHeightToFit(100, 400.0f, 22.0f, 24.0f, 160.0f) == 24.0f);

    // One track doesn't grow to fill the screen.
    REQUIRE(app::laneHeightToFit(1, 1000.0f, 22.0f, 24.0f, 160.0f) == 160.0f);

    REQUIRE(app::laneHeightToFit(0, 400.0f, 22.0f, 24.0f, 160.0f) == 24.0f);
    REQUIRE(app::laneHeightToFit(4, 10.0f, 22.0f, 24.0f, 160.0f) == 24.0f);
}

TEST_CASE("Scrolling to a span puts its start, less the margin, at the left", "[app][timelinezoom]")
{
    TimelineGeometry geometry;
    geometry.basePixelsPerBeat = 24.0f;
    geometry.zoom              = 2.0f; // 48 px a beat

    // 20 beats from beat 40: the margin is one beat, so beat 39 at the left.
    REQUIRE(app::scrollToShow(40.0, 20.0, geometry) == 39 * 48);

    // Never before the start of the timeline.
    REQUIRE(app::scrollToShow(0.5, 20.0, geometry) == 0);
}
