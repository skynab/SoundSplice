#include <catch2/catch_test_macros.hpp>

#include <engine/AutomationCurve.h>
#include <model/AutomationLane.h>

using namespace looper;

namespace
{
    bool near(float a, float b) { return std::abs(a - b) < 1.0e-6f; }
}

TEST_CASE("An empty curve returns the fallback", "[engine][automation]")
{
    const engine::AutomationCurve curve;
    REQUIRE(near(curve.valueAt(4.0, -6.0f), -6.0f));
}

TEST_CASE("A curve holds its end values outside the range", "[engine][automation]")
{
    engine::AutomationCurve curve;
    curve.addPoint(2.0, -20.0f);
    curve.addPoint(6.0, 0.0f);

    REQUIRE(near(curve.valueAt(0.0, 99.0f), -20.0f));  // before the first point
    REQUIRE(near(curve.valueAt(10.0, 99.0f), 0.0f));   // after the last
}

TEST_CASE("A curve interpolates linearly between points", "[engine][automation]")
{
    engine::AutomationCurve curve;
    curve.addPoint(0.0, 0.0f);
    curve.addPoint(4.0, 8.0f);

    REQUIRE(near(curve.valueAt(1.0, 0.0f), 2.0f));
    REQUIRE(near(curve.valueAt(2.0, 0.0f), 4.0f));
    REQUIRE(near(curve.valueAt(3.0, 0.0f), 6.0f));
}

TEST_CASE("sortPoints fixes out-of-order input", "[engine][automation]")
{
    // valueAt binary-searches, so unsorted points would silently return
    // nonsense rather than failing loudly.
    engine::AutomationCurve curve;
    curve.addPoint(8.0, 10.0f);
    curve.addPoint(0.0, 0.0f);
    curve.addPoint(4.0, 5.0f);
    curve.sortPoints();

    REQUIRE(near(curve.valueAt(0.0, 0.0f), 0.0f));
    REQUIRE(near(curve.valueAt(4.0, 0.0f), 5.0f));
    REQUIRE(near(curve.valueAt(8.0, 0.0f), 10.0f));
    REQUIRE(near(curve.valueAt(2.0, 0.0f), 2.5f));
}

TEST_CASE("The engine curve agrees with the model lane it is converted from", "[engine][automation]")
{
    // These are two representations of the same thing, kept apart only
    // because `model` already depends on `engine` and the dependency can't
    // run both ways. If they ever disagreed, a project would sound different
    // played than it does exported — so pin them together directly.
    model::AutomationLane   lane;
    engine::AutomationCurve curve;

    const std::vector<std::pair<double, float>> points {
        { 0.0, -40.0f }, { 1.5, -12.0f }, { 3.25, 0.0f }, { 8.0, -6.0f }
    };
    for (const auto& [beat, value] : points)
    {
        lane.addPoint(beat, value);
        curve.addPoint(beat, value);
    }
    curve.sortPoints();

    for (double beat = -1.0; beat <= 10.0; beat += 0.125)
        REQUIRE(near(curve.valueAt(beat, 99.0f), lane.valueAt(beat, 99.0f)));
}

TEST_CASE("TrackAutomation reports whether anything is automated", "[engine][automation]")
{
    engine::TrackAutomation automation;
    REQUIRE_FALSE(automation.any());

    automation.pan.addPoint(0.0, -1.0f);
    REQUIRE(automation.any());
}
