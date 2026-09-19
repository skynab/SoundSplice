#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MatchEq.h"
#include "engine/ShelfPeakFilter.h"

#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    std::vector<std::vector<float>> noise(int frames, unsigned seed)
    {
        std::mt19937                          random(seed);
        std::uniform_real_distribution<float> uniform(-0.3f, 0.3f);
        std::vector<float>                    samples((size_t) frames);
        for (auto& s : samples)
            s = uniform(random);
        return { samples };
    }

    std::array<double, SpectrumAverager::kBands> measure(const std::vector<std::vector<float>>& audio)
    {
        SpectrumAverager averager(kRate);
        // In uneven chunks, as the app reads them.
        for (size_t at = 0; at < audio[0].size(); at += 10007)
        {
            const auto end = std::min(audio[0].size(), at + 10007);
            averager.append({ std::vector<float>(audio[0].begin() + (long) at, audio[0].begin() + (long) end) });
        }
        return averager.bandLevelsDb();
    }
}

TEST_CASE("Match EQ finds what a recording's tone needs to match another's", "[engine][eq]")
{
    const int frames = (int) kRate * 8;

    // The target: white noise. The reference: other white noise, with a
    // broad 6 dB bump at 2 kHz.
    const auto target = noise(frames, 1);
    auto       reference = noise(frames, 2);
    ShelfPeakFilter bump;
    bump.prepare(kRate);
    bump.setShape(ShelfPeakFilter::Shape::Peaking);
    bump.setFrequency(2000.0f);
    bump.setQ(0.7f);
    bump.setGainDb(6.0f);
    for (auto& s : reference[0])
        s = bump.processSample(s);

    const auto gains = matcheq::gains(measure(reference), measure(target), kRate);

    // 2 kHz is band 20; 200 Hz is band 10.
    REQUIRE_THAT(gains[20] - gains[10], WithinAbs(6.0, 1.5));
    REQUIRE(gains[20] > 2.0f);
    for (float gain : gains)
    {
        REQUIRE(gain >= -matcheq::kRangeDb);
        REQUIRE(gain <= matcheq::kRangeDb);
    }

    // Two recordings of the same sound need nothing.
    const auto same = matcheq::gains(measure(target), measure(noise(frames, 3)), kRate);
    for (float gain : same)
        REQUIRE_THAT(gain, WithinAbs(0.0, 1.0));

    // Nothing measured is nothing to match.
    SpectrumAverager empty(kRate);
    REQUIRE(empty.isEmpty());
}
