#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Paulstretch.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine::timestretch;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    std::vector<float> tone(double hz, int frames, double amplitude = 0.5)
    {
        std::vector<float> x((size_t) frames);
        for (int n = 0; n < frames; ++n)
            x[(size_t) n] = (float) (amplitude * std::sin(2.0 * kPi * hz * n / kRate));
        return x;
    }

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt(sum / (double) (to - from));
    }

    double levelAt(const std::vector<float>& x, size_t from, size_t to, double hz)
    {
        const double coeff = 2.0 * std::cos(2.0 * kPi * hz / kRate);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            const double s0 = x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / (double) (to - from);
    }
}

TEST_CASE("Paulstretch makes the audio as much longer as asked, at the same level", "[engine][paulstretch]")
{
    const auto input   = tone(440.0, 48000);
    const auto stretched = paulstretch(input, 8.0, 0.25, kRate);

    REQUIRE(stretched.size() == 8 * input.size());

    // Level holds across the stretch, away from the very ends where the
    // first and last windows fade in and out.
    REQUIRE_THAT(rms(stretched, 48000, stretched.size() - 48000), WithinAbs(rms(input, 0, input.size()), 0.05));
}

TEST_CASE("Paulstretch keeps the spectrum and loses the timing", "[engine][paulstretch]")
{
    // A second of 1 kHz, then a second of 4 kHz.
    auto       input = tone(1000.0, 48000);
    const auto high  = tone(4000.0, 48000);
    input.insert(input.end(), high.begin(), high.end());

    const auto stretched = paulstretch(input, 4.0, 0.25, kRate);
    REQUIRE(stretched.size() == 4 * input.size());

    // Each tone is still in its own half and not the other's. Measured
    // against each other rather than absolutely: random phases spread a pure
    // tone into a narrow band, so reading one frequency alone understates it.
    const size_t half = stretched.size() / 2;

    // Wide enough to clear where the tones meet: the windows that straddle
    // the change hold both, and stretching spreads them over four windows'
    // worth of output either side of it.
    const size_t margin = 80000;

    const double firstAt1k  = levelAt(stretched, margin, half - margin, 1000.0);
    const double firstAt4k  = levelAt(stretched, margin, half - margin, 4000.0);
    const double secondAt4k = levelAt(stretched, half + margin, stretched.size() - margin, 4000.0);
    const double secondAt1k = levelAt(stretched, half + margin, stretched.size() - margin, 1000.0);

    REQUIRE(firstAt1k > firstAt4k * 20.0);
    REQUIRE(secondAt4k > secondAt1k * 20.0);

    // And each half holds the level of the tone it came from.
    REQUIRE_THAT(rms(stretched, margin, half - margin), WithinAbs(0.5 / std::sqrt(2.0), 0.05));
    REQUIRE_THAT(rms(stretched, half + margin, stretched.size() - margin), WithinAbs(0.5 / std::sqrt(2.0), 0.05));
}

TEST_CASE("Paulstretch repeats exactly, and leaves audio it can't stretch alone", "[engine][paulstretch]")
{
    const auto input = tone(300.0, 24000);
    REQUIRE(paulstretch(input, 6.0, 0.1, kRate) == paulstretch(input, 6.0, 0.1, kRate));

    // Shorter than one window, or no stretch at all: unchanged.
    REQUIRE(paulstretch(tone(300.0, 100), 6.0, 0.25, kRate) == tone(300.0, 100));
    REQUIRE(paulstretch(input, 0.0, 0.25, kRate) == input);
}
