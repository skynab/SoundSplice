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

namespace
{
    /** One tone per frequency in @p hz, each at 0.2, over a second. */
    std::vector<float> tones(std::initializer_list<double> hz)
    {
        std::vector<float> x(48000, 0.0f);
        for (double f : hz)
            for (int n = 0; n < 48000; ++n)
                x[(size_t) n] += (float) (0.2 * std::sin(2.0 * kPi * f * n / kRate));
        return x;
    }
}

TEST_CASE("Spectral EQ is a bell across the band, deepest at its middle", "[engine][spectral]")
{
    // A band from 2 kHz to 8 kHz, whose middle by octave is 4 kHz.
    auto audio = tones({ 500.0, 4000.0 });
    REQUIRE(spectral::bellBand(audio, 0, 48000, kRate, 2000.0, 8000.0, -12.0f));

    const double cut = 0.2 * std::pow(10.0, -12.0 / 20.0);
    REQUIRE_THAT(levelAt(audio, 12000, 36000, 4000.0), WithinAbs(cut, 0.004)); // the full 12 dB
    REQUIRE_THAT(levelAt(audio, 12000, 36000, 500.0), WithinAbs(0.2, 0.002));  // outside: untouched

    // Halfway between the middle and an edge, half the cut in dB.
    auto half = tones({ 2828.4 }); // half an octave above 2 kHz: a quarter of the way up the band
    REQUIRE(spectral::bellBand(half, 0, 48000, kRate, 2000.0, 8000.0, -12.0f));
    REQUIRE_THAT(20.0 * std::log10(levelAt(half, 12000, 36000, 2828.4) / 0.2), WithinAbs(-6.0, 0.5));
}

TEST_CASE("Spectral shelves ramp across the band and hold beyond it", "[engine][spectral]")
{
    const double cut = 0.2 * std::pow(10.0, -12.0 / 20.0);

    auto high = tones({ 500.0, 12000.0 });
    REQUIRE(spectral::shelfBand(high, 0, 48000, kRate, 2000.0, 4000.0, -12.0f, true));
    REQUIRE_THAT(levelAt(high, 12000, 36000, 12000.0), WithinAbs(cut, 0.004)); // above: the full cut
    REQUIRE_THAT(levelAt(high, 12000, 36000, 500.0), WithinAbs(0.2, 0.002));   // below: untouched

    auto low = tones({ 500.0, 12000.0 });
    REQUIRE(spectral::shelfBand(low, 0, 48000, kRate, 2000.0, 4000.0, -12.0f, false));
    REQUIRE_THAT(levelAt(low, 12000, 36000, 500.0), WithinAbs(cut, 0.004));
    REQUIRE_THAT(levelAt(low, 12000, 36000, 12000.0), WithinAbs(0.2, 0.002));
}

TEST_CASE("The healing brush rebuilds only what's painted", "[engine][spectral]")
{
    // The steady 5 kHz note with a burst on it, as for spectral repair, over
    // two seconds, with the selection much wider than the burst.
    std::vector<float> audio(96000);
    for (int n = 0; n < 96000; ++n)
    {
        const double burst = n >= 44000 && n < 52000 ? 0.8 : 0.2;
        audio[(size_t) n] = (float) (burst * std::sin(2.0 * kPi * 5000.0 * n / kRate)
                                   + 0.3 * std::sin(2.0 * kPi * 1000.0 * n / kRate));
    }
    const auto original = audio;

    // Painted over the burst alone: its time and a band around 5 kHz.
    const auto painted = [](double sample, double hz)
    {
        return sample >= 43000.0 && sample <= 53000.0 && hz >= 4000.0 && hz <= 6000.0 ? 1.0f : 0.0f;
    };

    auto healed = audio;
    REQUIRE(spectral::healMask(healed, 30000, 66000, kRate, 24000, painted));

    REQUIRE_THAT(levelAt(healed, 45000, 51000, 5000.0), WithinAbs(0.2, 0.03));
    REQUIRE_THAT(levelAt(healed, 45000, 51000, 1000.0), WithinAbs(0.3, 0.01));

    // Inside the selection but away from the paint, the audio is as it was.
    float worst = 0.0f;
    for (int i = 30000; i < 40000; ++i)
        worst = std::max(worst, std::abs(healed[(size_t) i] - original[(size_t) i]));
    REQUIRE(worst < 1.0e-5f);

    // Nothing painted, nothing changed.
    auto untouched = audio;
    REQUIRE(spectral::healMask(untouched, 30000, 66000, kRate, 24000, [](double, double) { return 0.0f; }));
    for (size_t i = 0; i < untouched.size(); ++i)
        REQUIRE(std::abs(untouched[i] - original[i]) < 1.0e-5f);
}

TEST_CASE("A masked spectral delete removes only what the mask covers", "[spectral]")
{
    constexpr double kRate = 48000.0;
    std::vector<float> samples(48000);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.3f * (float) std::sin(2.0 * 3.14159265358979 * 1000.0 * (double) i / kRate)
                   + 0.3f * (float) std::sin(2.0 * 3.14159265358979 * 6000.0 * (double) i / kRate);

    const auto levelAt = [&](const std::vector<float>& audio, double hz, int from, int to)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            re += audio[(size_t) i] * std::cos(2.0 * 3.14159265358979 * hz * i / kRate);
            im += audio[(size_t) i] * std::sin(2.0 * 3.14159265358979 * hz * i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (to - from);
    };

    // Everything above 3 kHz, from sample 12000 to 36000.
    auto edited = samples;
    REQUIRE(spectral::scaleMask(edited, 0, 48000, kRate, 0.0f, [](double sample, double hz)
    {
        return sample >= 12000.0 && sample < 36000.0 && hz > 3000.0 ? 1.0f : 0.0f;
    }));

    REQUIRE(levelAt(edited, 6000.0, 16000, 32000) < 0.01);                          // gone where masked
    REQUIRE_THAT(levelAt(edited, 1000.0, 16000, 32000), WithinAbs(0.3, 0.01));      // the rest kept
    REQUIRE_THAT(levelAt(edited, 6000.0, 2000, 8000), WithinAbs(0.3, 0.01));        // and before it

    auto untouched = samples;
    REQUIRE(spectral::scaleMask(untouched, 0, 48000, kRate, 0.0f, [](double, double) { return 0.0f; }));
    for (size_t i = 0; i < samples.size(); i += 97)
        REQUIRE_THAT(untouched[i], WithinAbs(samples[i], 1e-5));
}
