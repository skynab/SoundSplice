#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/AutomationLane.h>

using Catch::Approx;
using looper::model::AutomationLane;

TEST_CASE("AutomationLane returns the fallback when empty", "[model][automation]")
{
    AutomationLane lane;
    REQUIRE(lane.empty());
    REQUIRE(lane.valueAt(5.0, -1.0f) == Approx(-1.0f));
}

TEST_CASE("AutomationLane interpolates linearly and holds the ends", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 8.0f);

    REQUIRE(lane.valueAt(0.0)  == Approx(0.0f));
    REQUIRE(lane.valueAt(1.0)  == Approx(2.0f));
    REQUIRE(lane.valueAt(2.0)  == Approx(4.0f));
    REQUIRE(lane.valueAt(4.0)  == Approx(8.0f));
    REQUIRE(lane.valueAt(-1.0) == Approx(0.0f)); // before first -> first
    REQUIRE(lane.valueAt(9.0)  == Approx(8.0f)); // after last -> last
}

TEST_CASE("AutomationLane keeps points sorted and replaces coincident beats", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 0.5f);

    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.points()[0].beat == Approx(0.0));
    REQUIRE(lane.points()[1].beat == Approx(2.0));
    REQUIRE(lane.points()[2].beat == Approx(4.0));

    lane.addPoint(2.0, 0.9f); // replace, not insert
    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.valueAt(2.0) == Approx(0.9f));
}

// --- Editing -------------------------------------------------------------
//
// Until these existed a lane could only ever gain points: fader automation
// wrote them and nothing could find one again, so a mistake was only fixable
// by clearing the whole lane.

TEST_CASE("indexNear finds the closest point inside the tolerance", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(8.0, 0.5f);

    REQUIRE(lane.indexNear(4.1, 0.5) == 1);
    REQUIRE(lane.indexNear(7.8, 0.5) == 2);
    REQUIRE(lane.indexNear(0.0, 0.5) == 0);
}

TEST_CASE("indexNear reports nothing when the tolerance is missed", "[model][automation]")
{
    // What stops a click on empty lane space from grabbing a distant point
    // instead of adding a new one.
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(8.0, 1.0f);

    REQUIRE(lane.indexNear(4.0, 0.5) == -1);
    REQUIRE(AutomationLane{}.indexNear(0.0, 1.0) == -1); // empty lane
}

TEST_CASE("Removing a point leaves the rest in order", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(8.0, 0.5f);

    lane.removePointAt(1);

    REQUIRE(lane.points().size() == 2);
    REQUIRE(lane.points()[0].beat == 0.0);
    REQUIRE(lane.points()[1].beat == 8.0);
}

TEST_CASE("Removing out of range does nothing", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(0.0, 0.5f);

    lane.removePointAt(-1);
    lane.removePointAt(7);

    REQUIRE(lane.points().size() == 1);
}

TEST_CASE("Moving a point keeps the lane sorted and reports where it went",
          "[model][automation]")
{
    // Dragging a point past its neighbour reorders the lane, and a caller
    // tracking "the point I am dragging" has to be told its new index — the
    // alternative is a drag that silently starts moving a different point.
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(8.0, 0.5f);

    const int moved = lane.movePoint(0, 6.0, 0.25f);

    REQUIRE(moved == 1); // it is now between 4.0 and 8.0
    REQUIRE(lane.points()[0].beat == 4.0);
    REQUIRE(lane.points()[1].beat == 6.0);
    REQUIRE(lane.points()[1].value == 0.25f);
    REQUIRE(lane.points()[2].beat == 8.0);
}

TEST_CASE("A moved point cannot go before the start", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(4.0, 1.0f);

    lane.movePoint(0, -5.0, 0.5f);

    REQUIRE(lane.points()[0].beat == 0.0);
}

TEST_CASE("Sorted order survives editing, so valueAt still interpolates",
          "[model][automation]")
{
    // The property everything else depends on: valueAt binary-searches, so an
    // out-of-order lane doesn't merely look wrong, it reads wrong.
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 1.0f);
    lane.addPoint(4.0, 0.0f);

    lane.movePoint(1, 6.0, 1.0f); // drag the middle point past the end
    lane.removePointAt(0);

    REQUIRE(lane.points().size() == 2);
    REQUIRE(lane.points()[0].beat == 4.0);
    REQUIRE(lane.points()[1].beat == 6.0);
    REQUIRE(lane.valueAt(5.0) == 0.5f); // halfway between them
}
