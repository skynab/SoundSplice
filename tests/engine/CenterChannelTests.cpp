#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/CenterChannel.h"

#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    /** A "mix": a 1 kHz voice in the centre, a 3 kHz guitar hard left and a
        5 kHz keyboard hard right. */
    void mix(std::vector<float>& left, std::vector<float>& right)
    {
        left.assign((size_t) kRate * 2, 0.0f);
        right.assign(left.size(), 0.0f);
        for (size_t i = 0; i < left.size(); ++i)
        {
            const double t     = (double) i / kRate;
            const float  voice = 0.3f * (float) std::sin(2.0 * kPi * 1000.0 * t);
            left[i]  = voice + 0.2f * (float) std::sin(2.0 * kPi * 3000.0 * t);
            right[i] = voice + 0.2f * (float) std::sin(2.0 * kPi * 5000.0 * t + 0.4);
        }
    }

    double levelAt(const std::vector<float>& audio, double hz)
    {
        double re = 0.0, im = 0.0;
        const size_t from = 24000, to = 72000;
        for (size_t i = from; i < to; ++i)
        {
            re += audio[i] * std::cos(2.0 * kPi * hz * (double) i / kRate);
            im += audio[i] * std::sin(2.0 * kPi * hz * (double) i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (double) (to - from);
    }
}

TEST_CASE("Vocal reduction takes out the centre and leaves the sides", "[engine][centre]")
{
    std::vector<float> left, right;
    mix(left, right);
    centre::process(left, right, kRate, {});

    REQUIRE(levelAt(left, 1000.0) < 0.3 * 0.05);   // the voice down by over 25 dB
    REQUIRE(levelAt(right, 1000.0) < 0.3 * 0.05);
    REQUIRE_THAT(levelAt(left, 3000.0), WithinAbs(0.2, 0.01));  // the guitar stays, on its side
    REQUIRE_THAT(levelAt(right, 5000.0), WithinAbs(0.2, 0.01));
    REQUIRE(levelAt(right, 3000.0) < 0.005);
}

TEST_CASE("Vocal isolation keeps the centre alone, in both channels", "[engine][centre]")
{
    std::vector<float> left, right;
    mix(left, right);
    centre::Settings settings;
    settings.mode = centre::Mode::Isolate;
    centre::process(left, right, kRate, settings);

    REQUIRE_THAT(levelAt(left, 1000.0), WithinAbs(0.3, 0.015));
    REQUIRE_THAT(levelAt(right, 1000.0), WithinAbs(0.3, 0.015));
    REQUIRE(levelAt(left, 3000.0) < 0.01);
    REQUIRE(levelAt(right, 5000.0) < 0.01);
}

TEST_CASE("Outside its band, and at no strength, the centre is left alone", "[engine][centre]")
{
    std::vector<float> left, right;
    mix(left, right);
    const auto originalLeft = left;

    centre::Settings band;
    band.lowHz  = 2000.0; // the voice at 1 kHz is below the band
    band.highHz = 9000.0;
    centre::process(left, right, kRate, band);
    REQUIRE_THAT(levelAt(left, 1000.0), WithinAbs(0.3, 0.01));

    // At zero strength it's a transparent round trip.
    std::vector<float> l2, r2;
    mix(l2, r2);
    centre::Settings none;
    none.strength = 0.0;
    centre::process(l2, r2, kRate, none);
    for (size_t i = 0; i < l2.size(); i += 97)
        REQUIRE_THAT(l2[i], WithinAbs(originalLeft[i], 1.0e-4));
}
