#include <catch2/catch_test_macros.hpp>

#include <app/AutomationGeometry.h>

using namespace looper;

namespace
{
    constexpr float kLaneTop    = 40.0f;
    constexpr float kLaneHeight = 100.0f;

    bool near(float a, float b, float tolerance = 1.0e-4f)
    {
        return std::abs(a - b) < tolerance;
    }
}

TEST_CASE("Higher values sit higher on screen", "[app][automationgeometry]")
{
    // The one property nobody reads the lane without assuming.
    AutomationGeometry geometry;
    const auto range = automationRangeFor(model::TrackParam::SendLevel);

    const float loud  = geometry.yForValue(1.0f, range, kLaneTop, kLaneHeight);
    const float quiet = geometry.yForValue(0.0f, range, kLaneTop, kLaneHeight);

    REQUIRE(loud < quiet);
}

TEST_CASE("A value round-trips through y and back", "[app][automationgeometry]")
{
    AutomationGeometry geometry;

    for (auto param : { model::TrackParam::Gain, model::TrackParam::Pan,
                        model::TrackParam::SendLevel })
    {
        const auto range = automationRangeFor(param);

        for (float t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const float value = range.minValue + t * (range.maxValue - range.minValue);
            const float y     = geometry.yForValue(value, range, kLaneTop, kLaneHeight);

            REQUIRE(near(geometry.valueForY(y, range, kLaneTop, kLaneHeight), value, 1.0e-3f));
        }
    }
}

TEST_CASE("The extremes sit inside the lane, not on its edges", "[app][automationgeometry]")
{
    // The margin's whole purpose: a point at full scale must still be drawn
    // and still be grabbable rather than half off the edge.
    AutomationGeometry geometry;
    const auto range = automationRangeFor(model::TrackParam::Pan);

    const float top    = geometry.yForValue(range.maxValue, range, kLaneTop, kLaneHeight);
    const float bottom = geometry.yForValue(range.minValue, range, kLaneTop, kLaneHeight);

    REQUIRE(top > kLaneTop);
    REQUIRE(bottom < kLaneTop + kLaneHeight);
    REQUIRE(near(top, kLaneTop + geometry.verticalMargin));
}

TEST_CASE("Dragging past the lane pins the value at its limit", "[app][automationgeometry]")
{
    // Rather than running off the range, which would write a gain of +40dB
    // from a drag that merely overshot.
    AutomationGeometry geometry;
    const auto range = automationRangeFor(model::TrackParam::Gain);

    REQUIRE(near(geometry.valueForY(kLaneTop - 200.0f, range, kLaneTop, kLaneHeight),
                 range.maxValue));
    REQUIRE(near(geometry.valueForY(kLaneTop + 500.0f, range, kLaneTop, kLaneHeight),
                 range.minValue));
}

TEST_CASE("A value outside the range is clamped on the way in too",
          "[app][automationgeometry]")
{
    AutomationGeometry geometry;
    const auto range = automationRangeFor(model::TrackParam::Gain);

    const float above = geometry.yForValue(100.0f, range, kLaneTop, kLaneHeight);
    REQUIRE(near(above, geometry.yForValue(range.maxValue, range, kLaneTop, kLaneHeight)));
}

TEST_CASE("Gain's centre is unity, not the middle of its range",
          "[app][automationgeometry]")
{
    // 0 dB is what an unautomated track already plays at, so a fresh lane has
    // to start there — and it is deliberately *not* the midpoint of -60..+6.
    const auto range = automationRangeFor(model::TrackParam::Gain);
    REQUIRE(range.defaultValue == 0.0f);
    REQUIRE(range.defaultValue != (range.minValue + range.maxValue) * 0.5f);
}

TEST_CASE("Pan and send default to where the control already sits",
          "[app][automationgeometry]")
{
    REQUIRE(automationRangeFor(model::TrackParam::Pan).defaultValue == 0.0f);  // centre
    REQUIRE(automationRangeFor(model::TrackParam::SendLevel).defaultValue == 0.0f); // no send
}

TEST_CASE("Hit-testing is a radius, not a rectangle", "[app][automationgeometry]")
{
    AutomationGeometry geometry;
    geometry.grabRadius = 10.0f;

    REQUIRE(geometry.hitsPoint(100.0f, 100.0f, 100.0f, 100.0f));
    REQUIRE(geometry.hitsPoint(106.0f, 106.0f, 100.0f, 100.0f));  // ~8.5 away
    REQUIRE_FALSE(geometry.hitsPoint(108.0f, 108.0f, 100.0f, 100.0f)); // ~11.3 away
}

TEST_CASE("The grab radius in beats follows the zoom", "[app][automationgeometry]")
{
    // Zoomed in, the same pixel tolerance covers less musical time — otherwise
    // a click at high zoom would grab a point several bars away.
    AutomationGeometry geometry;
    geometry.grabRadius = 8.0f;

    REQUIRE(near((float) geometry.grabRadiusInBeats(16.0f), 0.5f));
    REQUIRE(near((float) geometry.grabRadiusInBeats(64.0f), 0.125f));
    REQUIRE(geometry.grabRadiusInBeats(0.0f) == 0.0);
}

TEST_CASE("A zero-height lane doesn't divide by zero", "[app][automationgeometry]")
{
    AutomationGeometry geometry;
    const auto range = automationRangeFor(model::TrackParam::Pan);

    const float y = geometry.yForValue(0.0f, range, kLaneTop, 0.0f);
    REQUIRE(std::isfinite(y));
    REQUIRE(std::isfinite(geometry.valueForY(y, range, kLaneTop, 0.0f)));
}
