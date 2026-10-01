#include <catch2/catch_test_macros.hpp>

#include <app/TakeLanes.h>

using namespace soundsplice;

TEST_CASE("Take lanes split a clip under its header, one row per take", "[app][takelanes]")
{
    // A box from y=10, 72 tall, with three takes: a 12px header, then 20px rows.
    const auto lanes = app::takeLaneLayout(10.0f, 72.0f, 3);
    REQUIRE(lanes.shown());
    REQUIRE(lanes.headerTop == 10.0f);
    REQUIRE(lanes.rowsTop == 22.0f);
    REQUIRE(lanes.rowHeight == 20.0f);
    REQUIRE(lanes.rowTop(2) == 62.0f);

    REQUIRE(lanes.rowAt(15.0f) == -1); // the header
    REQUIRE(lanes.rowAt(22.0f) == 0);
    REQUIRE(lanes.rowAt(41.9f) == 0);
    REQUIRE(lanes.rowAt(42.0f) == 1);
    REQUIRE(lanes.rowAt(81.9f) == 2);
    REQUIRE(lanes.rowAt(82.0f) == -1); // below the clip
}

TEST_CASE("A clip with one take, or too short for its rows, has no take lanes", "[app][takelanes]")
{
    REQUIRE_FALSE(app::takeLaneLayout(0.0f, 200.0f, 1).shown());
    REQUIRE_FALSE(app::takeLaneLayout(0.0f, 200.0f, 0).shown());

    // Four takes need 12 + 4 * 8 = 44 pixels.
    REQUIRE_FALSE(app::takeLaneLayout(0.0f, 43.0f, 4).shown());
    REQUIRE(app::takeLaneLayout(0.0f, 44.0f, 4).shown());
    REQUIRE_FALSE(app::takeLaneLayout(0.0f, 44.0f, 4).rowAt(100.0f) >= 0);
}

TEST_CASE("The lane height for takes fits each one its row", "[app][takelanes]")
{
    const float lane  = app::laneHeightForTakes(4);
    const auto  lanes = app::takeLaneLayout(3.0f, lane - 6.0f, 4);
    REQUIRE(lanes.shown());
    REQUIRE(lanes.rowHeight == 18.0f);
}
