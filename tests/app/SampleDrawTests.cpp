#include <catch2/catch_test_macros.hpp>

#include <app/SampleDraw.h>

#include <cmath>
#include <limits>

using namespace soundsplice::app::sampledraw;

TEST_CASE("A stroke sets every sample on the line between its ends", "[app][sampledraw]")
{
    std::vector<float> samples(10, 0.9f);

    const auto touched = drawLine(samples, 2, 0.0f, 6, 0.8f);
    REQUIRE(touched.first == 2);
    REQUIRE(touched.last == 6);

    REQUIRE(samples[1] == 0.9f); // untouched either side
    REQUIRE(samples[2] == 0.0f);
    REQUIRE(std::abs(samples[3] - 0.2f) < 1.0e-6f);
    REQUIRE(std::abs(samples[4] - 0.4f) < 1.0e-6f);
    REQUIRE(std::abs(samples[5] - 0.6f) < 1.0e-6f);
    REQUIRE(std::abs(samples[6] - 0.8f) < 1.0e-6f);
    REQUIRE(samples[7] == 0.9f);
}

TEST_CASE("A stroke drawn right to left is the same line", "[app][sampledraw]")
{
    std::vector<float> forwards(10, 0.0f), backwards(10, 0.0f);
    drawLine(forwards, 1, -0.5f, 5, 0.5f);
    drawLine(backwards, 5, 0.5f, 1, -0.5f);
    REQUIRE(forwards == backwards);
}

TEST_CASE("A single position sets one sample", "[app][sampledraw]")
{
    std::vector<float> samples(4, 0.0f);
    const auto touched = drawLine(samples, 2, 0.3f, 2, 0.3f);
    REQUIRE(touched.first == 2);
    REQUIRE(touched.last == 2);
    REQUIRE(samples == std::vector<float> { 0.0f, 0.0f, 0.3f, 0.0f });
}

TEST_CASE("Values are kept to full scale, and the line to the samples held", "[app][sampledraw]")
{
    std::vector<float> samples(5, 0.0f);

    // Past the ends of what's held: only what's inside is written, still on
    // the line through the positions given.
    const auto touched = drawLine(samples, -2, -1.0f, 2, 1.0f);
    REQUIRE(touched.first == 0);
    REQUIRE(touched.last == 2);
    REQUIRE(std::abs(samples[0] - 0.0f) < 1.0e-6f);
    REQUIRE(std::abs(samples[1] - 0.5f) < 1.0e-6f);
    REQUIRE(samples[2] == 1.0f);

    // Past full scale: clamped. Not a number: silence.
    drawLine(samples, 3, 4.0f, 3, 4.0f);
    REQUIRE(samples[3] == 1.0f);
    drawLine(samples, 4, std::numeric_limits<float>::quiet_NaN(), 4, 0.0f);
    REQUIRE(samples[4] == 0.0f);

    // Nothing held, or entirely outside it: nothing touched.
    std::vector<float> empty;
    REQUIRE(drawLine(empty, 0, 0.5f, 3, 0.5f).isEmpty());
    REQUIRE(drawLine(samples, 7, 0.5f, 9, 0.5f).isEmpty());
}

TEST_CASE("A whole stroke's touched span covers every segment", "[app][sampledraw]")
{
    Touched stroke;
    stroke.include(10, 14);
    stroke.include(14, 8);
    stroke.include(20, 20);
    REQUIRE(stroke.first == 8);
    REQUIRE(stroke.last == 20);
}
