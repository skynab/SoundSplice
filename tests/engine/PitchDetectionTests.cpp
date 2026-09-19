#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/HqStretch.h"
#include "engine/PitchDetection.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    /** A voice-like tone: a fundamental and a few harmonics. */
    std::vector<float> voice(double hz, double seconds)
    {
        std::vector<float> out((size_t) (seconds * kRate));
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / kRate;
            out[i]         = (float) (0.3 * std::sin(2.0 * kPi * hz * t) + 0.15 * std::sin(4.0 * kPi * hz * t)
                                      + 0.08 * std::sin(6.0 * kPi * hz * t));
        }
        return out;
    }

    /** The median of the voiced readings over the middle of @p audio. */
    double medianPitch(const std::vector<float>& audio)
    {
        const auto          readings = pitch::track(audio, kRate, 512);
        std::vector<double> voiced;
        for (size_t i = readings.size() / 4; i < readings.size() * 3 / 4; ++i)
            if (readings[i].voiced())
                voiced.push_back(readings[i].hz);
        if (voiced.empty())
            return 0.0;
        std::nth_element(voiced.begin(), voiced.begin() + (long) voiced.size() / 2, voiced.end());
        return voiced[voiced.size() / 2];
    }

    double cents(double hz, double reference) { return 1200.0 * std::log2(hz / reference); }

    std::vector<std::vector<float>> corrected(const std::vector<float>& sung, double strength)
    {
        const auto readings = pitch::track(sung, kRate, 256);
        const auto shift    = pitch::correction(readings, kRate, 256, 0, pitch::Scale::Chromatic, strength, 20.0);
        return hqstretch::transposeCurve({ sung }, kRate,
                                         [&](int sample) { return shift[std::min(shift.size() - 1, (size_t) sample / 256)]; },
                                         true);
    }
}

TEST_CASE("The tuner hears a note's pitch, and none in noise or silence", "[engine][pitch]")
{
    for (double hz : { 82.41, 220.0, 440.0, 987.77 })
    {
        INFO(hz << " Hz");
        REQUIRE_THAT(cents(medianPitch(voice(hz, 0.5)), hz), WithinAbs(0.0, 5.0));
    }

    std::mt19937                          random(2);
    std::uniform_real_distribution<float> uniform(-0.3f, 0.3f);
    std::vector<float>                    noise((size_t) kRate / 2);
    for (auto& s : noise)
        s = uniform(random);
    int voiced = 0;
    for (const auto& reading : pitch::track(noise, kRate, 512))
        voiced += reading.voiced() ? 1 : 0;
    REQUIRE(voiced < 3);

    REQUIRE_FALSE(pitch::detect(std::vector<float>(2000, 0.0f).data(), 2000, kRate).voiced());

    REQUIRE(pitch::nameOf(pitch::noteOf(440.0)) == "A4");
    REQUIRE(pitch::nameOf(pitch::noteOf(261.63)) == "C4");
    REQUIRE(pitch::nameOf(pitch::noteOf(277.18)) == "C#4");
}

TEST_CASE("Notes snap to the nearest in the key's scale", "[engine][pitch]")
{
    // C major has no C#: a sharp C# goes up to D, a flat one down to C.
    REQUIRE(pitch::nearestInScale(61.6, 0, pitch::Scale::Major) == 62.0);
    REQUIRE(pitch::nearestInScale(60.9, 0, pitch::Scale::Major) == 60.0);
    REQUIRE(pitch::nearestInScale(61.4, 0, pitch::Scale::Chromatic) == 61.0);
    // A minor: A B C D E F G.
    REQUIRE(pitch::nearestInScale(68.6, 9, pitch::Scale::Minor) == 69.0); // G# is out: up to A
    REQUIRE(pitch::nearestInScale(66.6, 9, pitch::Scale::Minor) == 67.0); // F#: nearer G than F
}

TEST_CASE("Pitch correction brings an out-of-tune voice onto the note", "[engine][pitch]")
{
    // A voice 35 cents sharp of A3.
    const auto sung = voice(220.0 * std::pow(2.0, 35.0 / 1200.0), 2.0);

    const auto fixed = corrected(sung, 1.0);
    REQUIRE(fixed[0].size() == sung.size());
    REQUIRE_THAT(cents(medianPitch(fixed[0]), 220.0), WithinAbs(0.0, 8.0));

    // Half strength goes half way.
    REQUIRE_THAT(cents(medianPitch(corrected(sung, 0.5)[0]), 220.0), WithinAbs(17.5, 8.0));
}
