#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/AdaptiveNoiseReduction.h"
#include "engine/Repair.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    double rms(const std::vector<float>& audio, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) audio[i] * audio[i];
        return std::sqrt(sum / (double) (to - from));
    }

    double levelAt(const std::vector<float>& audio, double hz, size_t from, size_t to)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            re += audio[i] * std::cos(2.0 * kPi * hz * (double) i / kRate);
            im += audio[i] * std::sin(2.0 * kPi * hz * (double) i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (double) (to - from);
    }
}

TEST_CASE("Adaptive noise reduction takes hiss down with no print, and keeps what's over it", "[engine][noise]")
{
    std::mt19937                          random(21);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);

    // Hiss throughout; a tone that comes and goes (a "voice"), and the hiss
    // gets louder halfway, as a fan speeding up would.
    std::vector<float> audio((size_t) kRate * 8);
    for (size_t i = 0; i < audio.size(); ++i)
    {
        const double t     = (double) i / kRate;
        const float  hiss  = (t < 4.0 ? 0.01f : 0.03f) * uniform(random);
        const bool   voice = std::fmod(t, 2.0) >= 1.0;
        audio[i]           = hiss + (voice ? 0.3f * (float) std::sin(2.0 * kPi * 700.0 * t) : 0.0f);
    }

    const auto clean = noisereduction::reduceNoiseAdaptive(audio, kRate, 12.0f, -24.0f);
    REQUIRE(clean.size() == audio.size());

    // In the gaps, the hiss is well down: before and after it changed.
    const auto gapLevel = [&](const std::vector<float>& a, double from)
    { return rms(a, (size_t) (from * kRate), (size_t) ((from + 0.8) * kRate)); };
    REQUIRE(gapLevel(clean, 2.1) < gapLevel(audio, 2.1) * 0.2);   // quieter hiss
    REQUIRE(gapLevel(clean, 6.1) < gapLevel(audio, 6.1) * 0.2);   // louder hiss, followed

    // The tone keeps its level.
    REQUIRE_THAT(levelAt(clean, 700.0, (size_t) (3.1 * kRate), (size_t) (3.9 * kRate)), WithinAbs(0.3, 0.02));
    REQUIRE_THAT(levelAt(clean, 700.0, (size_t) (7.1 * kRate), (size_t) (7.9 * kRate)), WithinAbs(0.3, 0.02));
}

TEST_CASE("De-crackle mends a crackle of tiny clicks and leaves the music alone", "[engine][repair]")
{
    std::mt19937                       random(8);
    std::uniform_int_distribution<int> where(2000, 94000);
    std::uniform_real_distribution<float> height(-0.6f, 0.6f);

    std::vector<float> music((size_t) kRate * 2);
    for (size_t i = 0; i < music.size(); ++i)
        music[i] = 0.3f * (float) (std::sin(2.0 * kPi * 330.0 * (double) i / kRate)
                                   + 0.5 * std::sin(2.0 * kPi * 1250.0 * (double) i / kRate));

    auto crackled = music;
    for (int n = 0; n < 300; ++n)
    {
        const auto at = (size_t) where(random);
        crackled[at] += height(random);
        if (n % 3 == 0)
            crackled[at + 1] -= height(random) * 0.5f; // some two samples wide
    }

    auto mended = crackled;
    const int found = repair::decrackle(mended, 0, (int) mended.size(), kRate, 0.5);
    REQUIRE(found > 200);

    double before = 0.0, after = 0.0;
    for (size_t i = 0; i < music.size(); ++i)
    {
        before += std::abs(crackled[i] - music[i]);
        after  += std::abs(mended[i] - music[i]);
    }
    REQUIRE(after < before * 0.1);

    // Clean music has nothing to mend.
    auto clean = music;
    REQUIRE(repair::decrackle(clean, 0, (int) clean.size(), kRate, 0.5) == 0);
}

