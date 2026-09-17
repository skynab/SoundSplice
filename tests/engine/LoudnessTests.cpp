#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Loudness.h>

#include <cmath>
#include <tuple>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    /** Feeds @p seconds of a stereo @p hz sine at @p dbfs (both channels) in
        blocks of @p block, carrying the phase on from @p phase. */
    void feedSine(LoudnessMeter& meter, double dbfs, double seconds, double& phase, double hz = 1000.0,
                  int block = 512, double phaseOffset = 0.0)
    {
        const double rate      = meter.sampleRate();
        const double amplitude = std::pow(10.0, dbfs / 20.0);
        auto         remaining = (int) std::lround(seconds * rate);

        std::vector<float> left((size_t) block), right((size_t) block);
        while (remaining > 0)
        {
            const int n = std::min(block, remaining);
            for (int i = 0; i < n; ++i)
            {
                const auto value = (float) (amplitude * std::sin(phase + phaseOffset));
                left[(size_t) i] = right[(size_t) i] = value;
                phase += 2.0 * kPi * hz / rate;
            }
            const float* channels[] { left.data(), right.data() };
            meter.process(channels, 2, n);
            remaining -= n;
        }
    }
}

TEST_CASE("A 1 kHz sine at -23 dBFS measures -23 LUFS, at any rate", "[engine][loudness]")
{
    // EBU Tech 3341 cases 1 and 2.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (double level : { -23.0, -33.0 })
        {
            INFO(rate << " Hz, " << level << " dBFS");
            LoudnessMeter meter;
            meter.prepare(rate, 2);
            double phase = 0.0;
            feedSine(meter, level, 20.0, phase);

            REQUIRE_THAT(meter.momentaryLufs(), WithinAbs(level, 0.1));
            REQUIRE_THAT(meter.shortTermLufs(), WithinAbs(level, 0.1));
            REQUIRE_THAT(meter.integratedLufs(), WithinAbs(level, 0.1));
        }
    }
}

TEST_CASE("Integrated loudness gates out the quiet parts", "[engine][loudness]")
{
    // EBU Tech 3341 case 3: 10 s at -36, 60 s at -23, 10 s at -36 dBFS.
    LoudnessMeter meter;
    meter.prepare(48000.0, 2);
    double phase = 0.0;
    feedSine(meter, -36.0, 10.0, phase);
    feedSine(meter, -23.0, 60.0, phase);
    feedSine(meter, -36.0, 10.0, phase);
    REQUIRE_THAT(meter.integratedLufs(), WithinAbs(-23.0, 0.1));

    // Case 5: 20 s at -26, 20.1 s at -20, 20 s at -26.
    meter.reset();
    feedSine(meter, -26.0, 20.0, phase);
    feedSine(meter, -20.0, 20.1, phase);
    feedSine(meter, -26.0, 20.0, phase);
    REQUIRE_THAT(meter.integratedLufs(), WithinAbs(-23.0, 0.1));

    // Case 4: silence under the absolute gate doesn't count at all.
    meter.reset();
    feedSine(meter, -72.0, 10.0, phase);
    feedSine(meter, -36.0, 10.0, phase);
    feedSine(meter, -23.0, 60.0, phase);
    feedSine(meter, -36.0, 10.0, phase);
    feedSine(meter, -72.0, 10.0, phase);
    REQUIRE_THAT(meter.integratedLufs(), WithinAbs(-23.0, 0.1));
}

TEST_CASE("Loudness range spans the loud and quiet passages", "[engine][loudness]")
{
    // EBU Tech 3342 cases 1 and 2.
    for (auto [quiet, loud, expected] : { std::tuple { -30.0, -20.0, 10.0 }, std::tuple { -20.0, -15.0, 5.0 } })
    {
        INFO(quiet << " then " << loud);
        LoudnessMeter meter;
        meter.prepare(48000.0, 2);
        double phase = 0.0;
        feedSine(meter, loud, 20.0, phase);
        feedSine(meter, quiet, 20.0, phase);
        REQUIRE_THAT(meter.loudnessRangeLu(), WithinAbs(expected, 1.0));
    }
}

TEST_CASE("True peak finds the peak between samples", "[engine][loudness]")
{
    // A quarter-rate sine sampled 45 degrees off its peaks: every sample is at
    // 0.707, but the wave reaches 1.
    LoudnessMeter meter;
    meter.prepare(48000.0, 2);
    double phase = 0.0;
    feedSine(meter, 0.0, 1.0, phase, 12000.0, 512, kPi / 4.0);

    REQUIRE_THAT(meter.samplePeakDb(), WithinAbs(-3.01, 0.05));
    REQUIRE_THAT(meter.truePeakDb(), WithinAbs(0.0, 0.3));
}

TEST_CASE("Silence and short audio have no loudness", "[engine][loudness]")
{
    LoudnessMeter meter;
    meter.prepare(48000.0, 2);
    REQUIRE(meter.integratedLufs() == LoudnessMeter::kSilence);
    REQUIRE(meter.momentaryLufs() == LoudnessMeter::kSilence);

    double phase = 0.0;
    feedSine(meter, -20.0, 0.3, phase);
    REQUIRE(meter.integratedLufs() == LoudnessMeter::kSilence); // under one 400 ms block
    REQUIRE(meter.shortTermLufs() == LoudnessMeter::kSilence);
    REQUIRE(meter.loudnessRangeLu() == 0.0);

    LoudnessGain gain;
    REQUIRE_FALSE(loudnessGainFor(meter.integratedLufs(), meter.truePeakDb(), -16.0, -1.0, true, gain));
}

TEST_CASE("Loudness gain reaches the target unless the ceiling stops it", "[engine][loudness]")
{
    LoudnessGain gain;
    REQUIRE(loudnessGainFor(-23.0, -10.0, -16.0, -1.0, true, gain));
    REQUIRE_THAT(gain.gainDb, WithinAbs(7.0, 1e-9));
    REQUIRE_FALSE(gain.limited);
    REQUIRE_THAT(gain.reachesLufs, WithinAbs(-16.0, 1e-9));

    REQUIRE(loudnessGainFor(-23.0, -3.0, -16.0, -1.0, true, gain));
    REQUIRE_THAT(gain.gainDb, WithinAbs(2.0, 1e-9));
    REQUIRE(gain.limited);
    REQUIRE_THAT(gain.reachesLufs, WithinAbs(-21.0, 1e-9));

    REQUIRE(loudnessGainFor(-23.0, -3.0, -16.0, -1.0, false, gain));
    REQUIRE_THAT(gain.gainDb, WithinAbs(7.0, 1e-9));
}
