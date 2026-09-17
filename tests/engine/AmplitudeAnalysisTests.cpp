#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/AmplitudeAnalysis.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    using Runs = std::vector<silence::FrameRange>;

    /** Feeds a mono signal to @p detector in chunks of @p chunk. */
    Runs clippedRuns(const std::vector<float>& signal, int chunk = 5)
    {
        ClippingDetector detector;
        for (size_t at = 0; at < signal.size(); at += (size_t) chunk)
        {
            const float* channels[] { signal.data() + at };
            detector.process(channels, 1, (int) std::min<size_t>((size_t) chunk, signal.size() - at));
        }
        return detector.finish();
    }
}

TEST_CASE("Find Clipping marks runs of full-scale samples, however the audio is chunked", "[engine][analysis]")
{
    std::vector<float> signal(100, 0.5f);
    for (int i = 10; i < 20; ++i) signal[(size_t) i] = 1.0f;   // a run
    signal[15] = 0.99f;                                         // a dip inside it doesn't split it
    for (int i = 40; i < 42; ++i) signal[(size_t) i] = -1.0f;  // two samples: too short to count
    for (int i = 60; i < 70; ++i) signal[(size_t) i] = -1.0f;  // negative clipping counts
    for (int i = 95; i < 100; ++i) signal[(size_t) i] = 1.0f;  // runs off the end

    const Runs expected { { 10, 20 }, { 60, 70 }, { 95, 100 } };
    REQUIRE(clippedRuns(signal, 1) == expected);
    REQUIRE(clippedRuns(signal, 7) == expected);
    REQUIRE(clippedRuns(signal, 100) == expected);

    // Three samples back under the threshold end a run.
    std::vector<float> twoRuns(30, 1.0f);
    for (int i = 10; i < 13; ++i) twoRuns[(size_t) i] = 0.2f;
    REQUIRE(clippedRuns(twoRuns) == Runs { { 0, 10 }, { 13, 30 } });
}

TEST_CASE("Find Clipping looks at every channel", "[engine][analysis]")
{
    std::vector<float> left(20, 0.1f), right(20, 0.1f);
    for (int i = 5; i < 9; ++i) right[(size_t) i] = -1.0f;

    ClippingDetector detector;
    const float*     channels[] { left.data(), right.data() };
    detector.process(channels, 2, 20);
    REQUIRE(detector.finish() == Runs { { 5, 9 } });
}

TEST_CASE("Amplitude statistics report peak, RMS, DC offset and dynamic range", "[engine][analysis]")
{
    constexpr double rate = 48000.0;
    constexpr double pi   = 3.14159265358979323846;

    // One second of a 0.5 sine riding on +0.1 on the left, and a quieter
    // 0.05 sine on the right; then a second at a tenth of that level.
    std::vector<float> left(96000), right(96000);
    for (int n = 0; n < 96000; ++n)
    {
        const double level = n < 48000 ? 1.0 : 0.1;
        const double s     = std::sin(2.0 * pi * 1000.0 * n / rate);
        left[(size_t) n]   = (float) (level * (0.1 + 0.5 * s));
        right[(size_t) n]  = (float) (level * 0.05 * s);
    }

    AmplitudeStatistics stats;
    stats.prepare(rate, 2);
    for (int at = 0; at < 96000; at += 1000)
    {
        const float* channels[] { left.data() + at, right.data() + at };
        stats.process(channels, 2, 1000);
    }

    const auto report = stats.report();
    REQUIRE(report.frames == 96000);
    REQUIRE_THAT(report.peakDb, WithinAbs(20.0 * std::log10(0.6), 0.01));
    REQUIRE_THAT(report.peakLeftDb, WithinAbs(20.0 * std::log10(0.6), 0.01));
    REQUIRE_THAT(report.peakRightDb, WithinAbs(20.0 * std::log10(0.05), 0.01));
    REQUIRE_THAT(report.dcOffsetPercent, WithinAbs((0.1 + 0.01) / 2.0 * 100.0, 0.05));

    // Loud half vs quiet half: exactly 20 dB apart, window for window.
    REQUIRE_THAT(report.dynamicRangeDb, WithinAbs(20.0, 0.1));

    // RMS over all four channel-seconds.
    const double meanSquares = ((0.01 + 0.125) + 0.00125 + (0.01 + 0.125) * 0.01 + 0.00125 * 0.01) / 4.0;
    REQUIRE_THAT(report.rmsDb, WithinAbs(10.0 * std::log10(meanSquares), 0.02));
}

TEST_CASE("Digital silence doesn't count as the quietest part", "[engine][analysis]")
{
    std::vector<float> signal(48000, 0.0f);
    for (int n = 0; n < 24000; ++n)
        signal[(size_t) n] = n % 2 == 0 ? 0.5f : -0.5f;

    AmplitudeStatistics stats;
    stats.prepare(48000.0, 1);
    const float* channels[] { signal.data() };
    stats.process(channels, 1, 48000);

    const auto report = stats.report();
    REQUIRE_THAT(report.loudestWindowDb, WithinAbs(20.0 * std::log10(0.5), 0.01));
    REQUIRE_THAT(report.dynamicRangeDb, WithinAbs(0.0, 0.01));
    REQUIRE(report.peakRightDb == report.peakLeftDb);

    AmplitudeStatistics empty;
    empty.prepare(48000.0, 2);
    REQUIRE(empty.report().frames == 0);
    REQUIRE(empty.report().peakDb == AmplitudeStatistics::kSilence);
}

TEST_CASE("Sounds are what lies between the silences", "[engine][analysis]")
{
    using silence::FrameRange;
    const std::vector<FrameRange> silences { { 0, 100 }, { 300, 320 }, { 900, 1000 } };

    REQUIRE(silence::soundRuns(silences, 1000, 1) == Runs { { 100, 300 }, { 320, 900 } });
    REQUIRE(silence::soundRuns(silences, 1000, 300) == Runs { { 320, 900 } }); // too short dropped
    REQUIRE(silence::soundRuns({}, 500, 1) == Runs { { 0, 500 } });
    REQUIRE(silence::soundRuns({ { 0, 500 } }, 500, 1).empty());
}
