#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/SpectralEdit.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    /** 1 kHz and 5 kHz together, each at 0.3. */
    std::vector<float> twoTones(int frames)
    {
        std::vector<float> x((size_t) frames);
        for (int n = 0; n < frames; ++n)
            x[(size_t) n] = (float) (0.3 * std::sin(2.0 * kPi * 1000.0 * n / kRate)
                                   + 0.3 * std::sin(2.0 * kPi * 5000.0 * n / kRate));
        return x;
    }

    double levelAt(const std::vector<float>& x, int from, int to, double hz)
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

TEST_CASE("Spectral delete takes out one band over one stretch and leaves the rest", "[engine][spectral]")
{
    const auto original = twoTones(48000);
    auto       edited   = original;

    // The 5 kHz tone, from 0.25 s to 0.75 s.
    REQUIRE(spectral::scaleBand(edited, 12000, 36000, kRate, 4000.0, 6000.0, 0.0f));

    // In the middle of the selection: 5 kHz gone, 1 kHz as it was.
    REQUIRE(levelAt(edited, 18000, 30000, 5000.0) < 0.003);
    REQUIRE_THAT(levelAt(edited, 18000, 30000, 1000.0), WithinAbs(0.3, 0.005));

    // Outside the selection, every sample exactly as it was.
    for (int i = 0; i < 12000; ++i)
        REQUIRE(edited[(size_t) i] == original[(size_t) i]);
    for (int i = 36000; i < 48000; ++i)
        REQUIRE(edited[(size_t) i] == original[(size_t) i]);
}

TEST_CASE("Spectral editing at unity gain gives back the audio", "[engine][spectral]")
{
    const auto original = twoTones(20000);
    auto       edited   = original;
    REQUIRE(spectral::scaleBand(edited, 3000, 17000, kRate, 100.0, 20000.0, 1.0f));

    float worst = 0.0f;
    for (size_t i = 0; i < edited.size(); ++i)
        worst = std::max(worst, std::abs(edited[i] - original[i]));
    REQUIRE(worst < 1.0e-5f);
}

TEST_CASE("Spectral gain can raise a band, and refuses a selection too short to hold a window", "[engine][spectral]")
{
    auto edited = twoTones(48000);
    REQUIRE(spectral::scaleBand(edited, 0, 48000, kRate, 800.0, 1200.0, 2.0f));
    REQUIRE_THAT(levelAt(edited, 12000, 36000, 1000.0), WithinAbs(0.6, 0.01));
    REQUIRE_THAT(levelAt(edited, 12000, 36000, 5000.0), WithinAbs(0.3, 0.005));

    auto tiny = twoTones(1000);
    REQUIRE_FALSE(spectral::scaleBand(tiny, 100, 150, kRate, 4000.0, 6000.0, 0.0f));
    REQUIRE(tiny == twoTones(1000));
    REQUIRE_FALSE(spectral::scaleBand(tiny, 0, 1000, kRate, 6000.0, 4000.0, 0.0f)); // no band
}

TEST_CASE("Spectral repair rebuilds a band from what surrounds it", "[engine][spectral]")
{
    // A steady 5 kHz note at 0.2 over a second, with a loud 1 kHz tone as the
    // background, and a burst in the middle where the 5 kHz jumps to 0.8, as
    // a cough or a clunk would.
    std::vector<float> audio(48000);
    for (int n = 0; n < 48000; ++n)
    {
        const double burst = n >= 20000 && n < 28000 ? 0.8 : 0.2;
        audio[(size_t) n] = (float) (burst * std::sin(2.0 * kPi * 5000.0 * n / kRate)
                                   + 0.3 * std::sin(2.0 * kPi * 1000.0 * n / kRate));
    }
    const auto original = audio;

    REQUIRE(spectral::healBand(audio, 19000, 29000, kRate, 4000.0, 6000.0, 8192));

    // The note goes on through the gap at its level either side, and the
    // background is untouched.
    REQUIRE_THAT(levelAt(audio, 21000, 27000, 5000.0), WithinAbs(0.2, 0.03));
    REQUIRE_THAT(levelAt(audio, 21000, 27000, 1000.0), WithinAbs(0.3, 0.01));

    // Nothing outside the selection moves.
    for (int i = 0; i < 19000; ++i)
        REQUIRE(audio[(size_t) i] == original[(size_t) i]);
    for (int i = 29000; i < 48000; ++i)
        REQUIRE(audio[(size_t) i] == original[(size_t) i]);

    // Without any audio either side to learn from, it refuses.
    auto alone = twoTones(4096);
    REQUIRE_FALSE(spectral::healBand(alone, 0, 4096, kRate, 4000.0, 6000.0, 8192));
}
