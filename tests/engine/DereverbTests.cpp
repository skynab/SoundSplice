#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/Dereverb.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    double rms(const std::vector<float>& audio, double from, double to)
    {
        double sum = 0.0;
        const auto a = (size_t) (from * kRate), b = (size_t) (to * kRate);
        for (size_t i = a; i < b; ++i)
            sum += (double) audio[i] * audio[i];
        return std::sqrt(sum / (double) (b - a));
    }
}

TEST_CASE("De-reverb takes a room's tail down and leaves the sound that made it", "[engine][dereverb]")
{
    // "Syllables": 80 ms tone bursts every 600 ms, dry.
    std::vector<float> dry((size_t) (kRate * 4.0), 0.0f);
    for (int burst = 0; burst < 6; ++burst)
    {
        const auto start = (size_t) ((0.2 + burst * 0.6) * kRate);
        for (size_t i = 0; i < (size_t) (0.08 * kRate); ++i)
            dry[start + i] = 0.4f * (float) std::sin(2.0 * kPi * (300.0 + 150.0 * burst) * (double) i / kRate);
    }

    // A room: exponentially dying noise, 60 dB over 0.8 s, from 20 ms on.
    std::mt19937                          random(6);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    std::vector<float>                    room((size_t) (kRate * 0.8));
    for (size_t i = (size_t) (0.02 * kRate); i < room.size(); ++i)
        room[i] = 0.012f * uniform(random) * (float) std::pow(10.0, -3.0 * ((double) i / kRate) / 0.8);

    std::vector<float> wet = dry;
    for (size_t i = 0; i < dry.size(); ++i)
        if (dry[i] != 0.0f)
            for (size_t k = 0; k < room.size() && i + k < wet.size(); ++k)
                wet[i + k] += dry[i] * room[k];

    dereverb::Settings settings;
    settings.reverbSeconds = 0.8;
    const auto clean = dereverb::process(wet, kRate, settings);
    REQUIRE(clean.size() == wet.size());

    // In the gaps (the tails), the reverb is well down.
    double tailBefore = 0.0, tailAfter = 0.0;
    for (int burst = 1; burst < 5; ++burst)
    {
        const double from = 0.2 + burst * 0.6 + 0.15, to = from + 0.4;
        tailBefore += rms(wet, from, to);
        tailAfter += rms(clean, from, to);
    }
    REQUIRE(tailAfter < tailBefore * 0.5);

    // The bursts themselves are kept, within a couple of decibels.
    for (int burst = 1; burst < 5; ++burst)
    {
        const double from = 0.2 + burst * 0.6 + 0.01, to = from + 0.06;
        INFO("burst " << burst);
        REQUIRE_THAT(20.0 * std::log10(rms(clean, from, to) / rms(wet, from, to)), WithinAbs(0.0, 2.0));
    }

    // At no amount it's a transparent round trip.
    settings.amount = 0.0;
    const auto same = dereverb::process(wet, kRate, settings);
    for (size_t i = 2048; i < wet.size() - 2048; i += 101)
        REQUIRE_THAT(same[i], WithinAbs(wet[i], 1.0e-4));
}

