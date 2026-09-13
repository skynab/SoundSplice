#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/Interpolation.h>

using Catch::Approx;
using looper::engine::sampleLinear;

TEST_CASE("sampleLinear returns exact samples at integer positions", "[engine][interp]")
{
    const float data[] = { 0.0f, 1.0f, 2.0f, 3.0f };
    REQUIRE(sampleLinear(data, 4, 0.0) == Approx(0.0f));
    REQUIRE(sampleLinear(data, 4, 1.0) == Approx(1.0f));
    REQUIRE(sampleLinear(data, 4, 3.0) == Approx(3.0f));
}

TEST_CASE("sampleLinear interpolates between samples", "[engine][interp]")
{
    const float data[] = { 0.0f, 1.0f, 2.0f, 3.0f };
    REQUIRE(sampleLinear(data, 4, 0.5)  == Approx(0.5f));
    REQUIRE(sampleLinear(data, 4, 1.25) == Approx(1.25f));
    REQUIRE(sampleLinear(data, 4, 2.75) == Approx(2.75f));
}

TEST_CASE("sampleLinear holds the last sample and silences out-of-range reads", "[engine][interp]")
{
    const float data[] = { 0.0f, 1.0f, 2.0f, 3.0f };
    REQUIRE(sampleLinear(data, 4, 3.5)  == Approx(3.0f)); // holds final sample, no read past end
    REQUIRE(sampleLinear(data, 4, 4.0)  == Approx(0.0f)); // out of range
    REQUIRE(sampleLinear(data, 4, -1.0) == Approx(0.0f));
    REQUIRE(sampleLinear(nullptr, 0, 0.0) == Approx(0.0f));
}
