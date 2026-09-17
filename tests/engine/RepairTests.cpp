#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Repair.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace soundsplice::engine::repair;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    /** Two tones over a noise floor about 60 dB down, as a recording has:
        pure tones alone are a degenerate case for a predictive model, with
        nothing left to model once they're found. */
    std::vector<float> music(int frames)
    {
        std::vector<float> x((size_t) frames);
        std::uint32_t      seed = 2024;
        for (int n = 0; n < frames; ++n)
        {
            seed = seed * 1664525u + 1013904223u;
            const double noise = ((double) (seed >> 8) / (double) (1u << 24) - 0.5) * 0.002;
            x[(size_t) n] = (float) (0.4 * std::sin(2.0 * kPi * 440.0 * n / kRate)
                                   + 0.2 * std::sin(2.0 * kPi * 1250.0 * n / kRate + 0.7) + noise);
        }
        return x;
    }

    double maxError(const std::vector<float>& a, const std::vector<float>& b, int from, int to)
    {
        double worst = 0.0;
        for (int i = from; i < to; ++i)
            worst = std::max(worst, (double) std::abs(a[(size_t) i] - b[(size_t) i]));
        return worst;
    }

    double goertzel(const std::vector<float>& x, int from, int to, double hz)
    {
        const double coeff = 2.0 * std::cos(2.0 * kPi * hz / kRate);
        double s1 = 0.0, s2 = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double s0 = x[(size_t) i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / (double) (to - from);
    }
}

TEST_CASE("Burg's method recovers an autoregressive process", "[engine][repair]")
{
    // x[n] = 1.6 x[n-1] - 0.8 x[n-2] + noise.
    std::vector<double> x(4000, 0.0);
    std::uint32_t       seed = 12345;
    for (size_t n = 2; n < x.size(); ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        const double noise = ((double) (seed >> 8) / (double) (1u << 24) - 0.5) * 0.01;
        x[n] = 1.6 * x[n - 1] - 0.8 * x[n - 2] + noise;
    }

    const auto a = burg(x, 2);
    REQUIRE_THAT(a[1], WithinAbs(-1.6, 0.05));
    REQUIRE_THAT(a[2], WithinAbs(0.8, 0.05));
}

TEST_CASE("Repair fills a gap with what the audio around it predicts", "[engine][repair]")
{
    const auto clean   = music(8000);
    auto       damaged = clean;
    for (int i = 4000; i < 4100; ++i) // 100 samples of dropout
        damaged[(size_t) i] = 0.0f;

    REQUIRE(interpolate(damaged, 4000, 4100));
    REQUIRE(maxError(damaged, clean, 4000, 4100) < 0.02);
    REQUIRE(maxError(damaged, clean, 0, 4000) == 0.0);    // nothing else touched
    REQUIRE(maxError(damaged, clean, 4100, 8000) == 0.0);

    // At the very start there's only the right side to learn from.
    auto start = clean;
    std::fill(start.begin(), start.begin() + 30, 0.0f);
    REQUIRE(interpolate(start, 0, 30));
    REQUIRE(maxError(start, clean, 0, 30) < 0.05);
}

TEST_CASE("Click removal repairs clicks and leaves clean audio alone", "[engine][repair]")
{
    const auto clean = music(48000);

    auto untouched = clean;
    REQUIRE(removeClicks(untouched, 0, 48000, 8.0, 96) == 0);
    REQUIRE(maxError(untouched, clean, 0, 48000) == 0.0);

    auto clicked = clean;
    for (int at : { 5000, 17000, 30011, 41000 })
    {
        clicked[(size_t) at] += 0.8f;
        clicked[(size_t) at + 1] -= 0.5f;
        clicked[(size_t) at + 2] += 0.3f;
    }

    REQUIRE(removeClicks(clicked, 0, 48000, 8.0, 96) == 4);
    REQUIRE(maxError(clicked, clean, 0, 48000) < 0.02);
}

TEST_CASE("Clip Fix carries clipped peaks on past the level they were cut to", "[engine][repair]")
{
    std::vector<float> clean(4800);
    for (int n = 0; n < 4800; ++n)
        clean[(size_t) n] = (float) std::sin(2.0 * kPi * 100.0 * n / kRate);

    auto clipped = clean;
    for (auto& sample : clipped)
        sample = std::clamp(sample, -0.7f, 0.7f);

    const double before = maxError(clipped, clean, 0, 4800);
    auto         fixed  = clipped;
    REQUIRE(fixClipping(fixed, 0, 4800, 0.7f) > 0);

    REQUIRE(maxError(fixed, clean, 0, 4800) < before * 0.5);
    REQUIRE(*std::max_element(fixed.begin(), fixed.end()) > 0.9f);

    // Nothing at the level: nothing changes.
    auto quiet = clean;
    REQUIRE(fixClipping(quiet, 0, 4800, 1.5f) == 0);
    REQUIRE(quiet == clean);
}

TEST_CASE("Hum removal takes out the mains and its harmonics, and leaves the music", "[engine][repair]")
{
    std::vector<float> signal(96000);
    for (int n = 0; n < 96000; ++n)
    {
        double hum = 0.0;
        for (int h = 1; h <= 4; ++h)
            hum += 0.1 / h * std::sin(2.0 * kPi * 60.0 * h * n / kRate + h);
        signal[(size_t) n] = (float) (hum + 0.3 * std::sin(2.0 * kPi * 1000.0 * n / kRate));
    }

    removeHum(signal, kRate, 60.0, 8, 30.0);

    // Measured over the second second, once the notches have settled.
    for (int h = 1; h <= 4; ++h)
        REQUIRE(goertzel(signal, 48000, 96000, 60.0 * h) < 0.005);
    REQUIRE_THAT(goertzel(signal, 48000, 96000, 1000.0), WithinAbs(0.3, 0.01));
}
