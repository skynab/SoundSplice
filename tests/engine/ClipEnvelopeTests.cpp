#include <catch2/catch_test_macros.hpp>

#include <engine/ClipEnvelope.h>

#include <cmath>
#include <limits>

using soundsplice::engine::ClipEnvelope;
using soundsplice::engine::EnvelopePoint;

TEST_CASE("An empty envelope leaves the clip's level alone", "[engine][envelope]")
{
    const ClipEnvelope envelope;
    REQUIRE(envelope.isEmpty());
    REQUIRE(envelope.gainAt(-1.0) == 1.0f);
    REQUIRE(envelope.gainAt(0.0) == 1.0f);
    REQUIRE(envelope.gainAt(1.0e6) == 1.0f);
}

TEST_CASE("Gain is interpolated between points and held beyond them", "[engine][envelope]")
{
    ClipEnvelope envelope;
    envelope.addPoint(2.0, 1.0f);
    envelope.addPoint(4.0, 0.0f);

    REQUIRE(envelope.gainAt(0.0) == 1.0f);  // before the first: its gain
    REQUIRE(envelope.gainAt(2.0) == 1.0f);
    REQUIRE(std::abs(envelope.gainAt(3.0) - 0.5f) < 1.0e-6f);
    REQUIRE(std::abs(envelope.gainAt(3.5) - 0.25f) < 1.0e-6f);
    REQUIRE(envelope.gainAt(4.0) == 0.0f);
    REQUIRE(envelope.gainAt(10.0) == 0.0f); // after the last: its gain

    // One point is a flat level everywhere.
    ClipEnvelope flat;
    flat.addPoint(5.0, 2.0f);
    REQUIRE(flat.gainAt(0.0) == 2.0f);
    REQUIRE(flat.gainAt(9.0) == 2.0f);
}

TEST_CASE("Points stay in order, and one at the same place replaces its gain", "[engine][envelope]")
{
    ClipEnvelope envelope;
    REQUIRE(envelope.addPoint(3.0, 1.0f) == 0);
    REQUIRE(envelope.addPoint(1.0, 0.5f) == 0);
    REQUIRE(envelope.addPoint(2.0, 0.8f) == 1);
    REQUIRE(envelope.addPoint(2.0, 0.2f) == 1); // same place: replaced, not added

    REQUIRE(envelope.points() == std::vector<EnvelopePoint> { { 1.0, 0.5f }, { 2.0, 0.2f }, { 3.0, 1.0f } });
}

TEST_CASE("Gains and positions are kept in range", "[engine][envelope]")
{
    ClipEnvelope envelope;
    envelope.addPoint(-2.0, 10.0f);
    envelope.addPoint(1.0, -1.0f);
    envelope.addPoint(2.0, std::numeric_limits<float>::quiet_NaN());

    REQUIRE(envelope.points()[0] == EnvelopePoint { 0.0, ClipEnvelope::kMaxGain });
    REQUIRE(envelope.points()[1].gain == 0.0f);
    REQUIRE(envelope.points()[2].gain == 1.0f); // not a number: left at unity
}

TEST_CASE("Points can be found, moved past each other, and removed", "[engine][envelope]")
{
    ClipEnvelope envelope;
    envelope.addPoint(1.0, 1.0f);
    envelope.addPoint(2.0, 1.0f);
    envelope.addPoint(3.0, 1.0f);

    REQUIRE(envelope.indexNear(2.04, 0.05) == 1);
    REQUIRE(envelope.indexNear(2.5, 0.05) == -1);

    // Dragged past the last point, it ends up last.
    REQUIRE(envelope.movePoint(0, 3.5, 0.25f) == 2);
    REQUIRE(envelope.points()[2] == EnvelopePoint { 3.5, 0.25f });
    REQUIRE(envelope.points()[0].seconds == 2.0);

    REQUIRE(envelope.movePoint(9, 1.0, 1.0f) == -1);

    envelope.removePointAt(1);
    REQUIRE(envelope.points().size() == 2);
    envelope.removePointAt(7); // out of range: nothing
    REQUIRE(envelope.points().size() == 2);

    ClipEnvelope copy = envelope;
    REQUIRE(copy == envelope);
    copy.clear();
    REQUIRE(copy != envelope);
}
