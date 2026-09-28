#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/AutomationLane.h>
#include <model/AutomationWriter.h>

using Catch::Approx;
using soundsplice::model::AutomationLane;

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

TEST_CASE("A segment follows the shape of the point it starts at", "[model][automation]")
{
    using soundsplice::model::CurveShape;

    const auto at = [](CurveShape shape, double beat)
    {
        AutomationLane lane;
        lane.addPoint(0.0, 0.0f, shape);
        lane.addPoint(4.0, 1.0f);
        return lane.valueAt(beat);
    };

    REQUIRE(at(CurveShape::Linear, 1.0) == 0.25f);
    REQUIRE(at(CurveShape::Hold, 3.99) == 0.0f);
    REQUIRE(at(CurveShape::Hold, 4.0) == 1.0f);
    REQUIRE(at(CurveShape::FastStart, 1.0) > 0.5f);  // most of the way already
    REQUIRE(at(CurveShape::SlowStart, 3.0) < 0.5f);  // most of it still to come
    REQUIRE(at(CurveShape::SCurve, 2.0) == 0.5f);    // symmetric about the middle
    REQUIRE(at(CurveShape::SCurve, 0.4) < 0.1f);     // and gentle at the ends

    // Every shape still starts and ends on the points' values.
    for (int shape = 0; shape <= (int) CurveShape::SCurve; ++shape)
    {
        REQUIRE(at((CurveShape) shape, 0.0) == 0.0f);
        REQUIRE(at((CurveShape) shape, 4.0) == 1.0f);
    }
}

TEST_CASE("Moving a point keeps its shape, and setShape changes only it", "[model][automation]")
{
    using soundsplice::model::CurveShape;

    AutomationLane lane;
    lane.addPoint(0.0, 0.0f, CurveShape::SCurve);
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(8.0, 0.0f);

    const int moved = lane.movePoint(0, 6.0, 0.5f); // past its neighbour
    REQUIRE(lane.points()[(size_t) moved].shape == CurveShape::SCurve);

    lane.setShape(0, CurveShape::Hold);
    REQUIRE(lane.points()[0].shape == CurveShape::Hold);
    REQUIRE(lane.points()[2].shape == CurveShape::Linear);
    lane.setShape(9, CurveShape::Hold); // out of range: nothing
}

TEST_CASE("Writing over a lane replaces only the stretch that was written", "[model][automation]")
{
    using soundsplice::model::LaneWriter;

    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(8.0, 1.0f);

    LaneWriter writer;
    writer.begin(lane, 2.0, 0.9f); // grabbed at 0.9 where the curve was at 0.25
    writer.advance(lane, 3.0, 0.9f);
    writer.advance(lane, 4.0, 0.5f);
    writer.end(lane, 5.0);
    REQUIRE_FALSE(writer.active());

    REQUIRE(lane.valueAt(1.0) == Approx(0.125f));  // before: untouched
    REQUIRE(lane.valueAt(2.0) == Approx(0.25f));   // starts from the curve
    REQUIRE(lane.valueAt(2.5) == Approx(0.9f));    // then jumps to the control
    REQUIRE(lane.valueAt(3.0) == Approx(0.9f));    // held flat, not ramped
    REQUIRE(lane.valueAt(3.5) == Approx(0.7f));
    REQUIRE(lane.valueAt(4.5) == Approx(0.5f));    // held to where it was let go
    REQUIRE(lane.valueAt(6.5) == Approx(0.75f));   // after: back into the old curve
    REQUIRE(lane.valueAt(8.0) == Approx(1.0f));
}

TEST_CASE("A held value writes two points, however long it's held", "[model][automation]")
{
    using soundsplice::model::LaneWriter;

    AutomationLane lane;
    LaneWriter     writer;
    writer.begin(lane, 0.0, -6.0f);
    for (double beat = 0.25; beat <= 16.0; beat += 0.25)
        writer.advance(lane, beat, -6.0f);
    writer.end(lane, 16.0);

    REQUIRE(lane.points().size() == 2);
    REQUIRE(lane.valueAt(9.0) == -6.0f);
}

TEST_CASE("A jump back while writing starts a new stretch there", "[model][automation]")
{
    using soundsplice::model::LaneWriter;

    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(16.0, 0.0f);

    LaneWriter writer;
    writer.begin(lane, 4.0, 1.0f);
    writer.advance(lane, 8.0, 1.0f);
    writer.advance(lane, 2.0, 0.5f); // the loop wrapped
    writer.advance(lane, 3.0, 0.5f);
    writer.end(lane, 3.0);

    REQUIRE(lane.valueAt(6.0) == Approx(1.0f)); // the first stretch survives
    REQUIRE(lane.valueAt(2.5) == Approx(0.5f));
    REQUIRE(lane.valueAt(12.0) == Approx(0.5f)); // from where it ended back to the old curve
}
