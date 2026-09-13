#include <catch2/catch_test_macros.hpp>

#include <engine/EqCurve.h>

#include <cmath>

using looper::engine::eqMagnitudeDb;
using looper::model::EqSettings;

TEST_CASE("A flat EQ reads 0dB everywhere", "[engine][eq][curve]")
{
    EqSettings eq;
    for (float hz : { 30.0f, 250.0f, 1000.0f, 4000.0f, 16000.0f })
        REQUIRE(std::abs(eqMagnitudeDb(eq, hz)) < 0.05f);
}

TEST_CASE("Bass gain shows up at low frequencies and fades out by the treble band", "[engine][eq][curve]")
{
    EqSettings eq;
    eq.bassDb = 9.0f;

    REQUIRE(eqMagnitudeDb(eq, 40.0f) > 6.0f);
    REQUIRE(std::abs(eqMagnitudeDb(eq, 16000.0f)) < 1.0f);
}

TEST_CASE("Treble gain shows up at high frequencies and fades out by the bass band", "[engine][eq][curve]")
{
    EqSettings eq;
    eq.trebleDb = 9.0f;

    REQUIRE(eqMagnitudeDb(eq, 16000.0f) > 6.0f);
    REQUIRE(std::abs(eqMagnitudeDb(eq, 40.0f)) < 1.0f);
}

TEST_CASE("Mid gain shows up at the mid band's centre and fades at the extremes", "[engine][eq][curve]")
{
    EqSettings eq;
    eq.midDb = 9.0f;

    REQUIRE(eqMagnitudeDb(eq, EqSettings::midHz()) > 7.0f);
    REQUIRE(std::abs(eqMagnitudeDb(eq, 30.0f)) < 1.5f);
    REQUIRE(std::abs(eqMagnitudeDb(eq, 18000.0f)) < 1.5f);
}

TEST_CASE("Bands in series add in dB, so boosting two at once compounds at the extremes", "[engine][eq][curve]")
{
    EqSettings bassOnly;
    bassOnly.bassDb = 6.0f;

    EqSettings both;
    both.bassDb   = 6.0f;
    both.trebleDb = 6.0f;

    // Deep in the bass band, treble's shelf hasn't reached down there, so
    // adding a treble boost shouldn't move the low end at all.
    REQUIRE(std::abs(eqMagnitudeDb(bassOnly, 40.0f) - eqMagnitudeDb(both, 40.0f)) < 0.5f);
}
