#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <app/SnapTargets.h>

using namespace soundsplice::app;
using Catch::Matchers::WithinAbs;

TEST_CASE("A nearby magnet pulls a position onto it, ahead of the grid", "[app][snap]")
{
    const std::vector<double> magnets { 3.3, 7.9 };

    REQUIRE_THAT(snapPosition(3.1, magnets, 0.25, true, 1.0), WithinAbs(3.3, 1e-9));
    REQUIRE_THAT(snapPosition(8.0, magnets, 0.25, true, 1.0), WithinAbs(7.9, 1e-9));
}

TEST_CASE("With no magnet close enough, the grid or nothing decides", "[app][snap]")
{
    const std::vector<double> magnets { 3.3 };

    REQUIRE_THAT(snapPosition(5.4, magnets, 0.25, true, 1.0), WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(snapPosition(5.4, magnets, 0.25, true, 0.5), WithinAbs(5.5, 1e-9));
    REQUIRE_THAT(snapPosition(5.4, magnets, 0.25, false, 1.0), WithinAbs(5.4, 1e-9));
    REQUIRE_THAT(snapPosition(5.4, {}, 0.25, false, 1.0), WithinAbs(5.4, 1e-9));
}

TEST_CASE("The nearest of several magnets wins", "[app][snap]")
{
    REQUIRE_THAT(snapPosition(4.0, { 3.8, 4.1, 4.3 }, 0.5, false, 1.0), WithinAbs(4.1, 1e-9));
}

TEST_CASE("A moved span snaps by whichever end is nearer a magnet", "[app][snap]")
{
    // A 2-beat clip dragged to start at 4.9: its end (6.9) is 0.1 from a
    // magnet at 7, its start 0.4 from one at 4.5, so it's the end that lines up.
    REQUIRE_THAT(snapSpanStart(4.9, 2.0, { 4.5, 7.0 }, 0.5, true, 1.0), WithinAbs(5.0, 1e-9));

    // Start nearer: the start lines up.
    REQUIRE_THAT(snapSpanStart(4.55, 2.0, { 4.5, 7.0 }, 0.5, true, 1.0), WithinAbs(4.5, 1e-9));

    // Neither close: the start goes onto the grid, or stays put.
    REQUIRE_THAT(snapSpanStart(10.3, 2.0, { 4.5 }, 0.25, true, 1.0), WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(snapSpanStart(10.3, 2.0, { 4.5 }, 0.25, false, 1.0), WithinAbs(10.3, 1e-9));
}
