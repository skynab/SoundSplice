#include <catch2/catch_test_macros.hpp>

#include <app/WaveformPeaks.h>

#include <cmath>

using namespace looper;

namespace
{
    std::vector<float> ramp(int count, float from, float to)
    {
        std::vector<float> out((size_t) count);
        for (int i = 0; i < count; ++i)
            out[(size_t) i] = from + (to - from) * (float) i / (float) std::max(1, count - 1);
        return out;
    }
}

TEST_CASE("An empty build leaves an empty cache", "[app][waveformpeaks]")
{
    WaveformPeaks peaks;
    REQUIRE(peaks.isEmpty());

    peaks.build({});
    REQUIRE(peaks.isEmpty());
    REQUIRE(peaks.numChannels() == 0);

    // Querying an empty cache is the state the pane is in before a file
    // loads, so it has to be safe rather than merely unlikely.
    REQUIRE(peaks.range(0, 0, 100).isEmpty());
    REQUIRE(peaks.overallMagnitude() == 0.0f);
}

TEST_CASE("Peaks capture the extremes of each bin", "[app][waveformpeaks]")
{
    // A signal whose min and max are both away from zero, so a cache that
    // only tracked magnitude would fail this.
    std::vector<float> samples { 0.5f, -0.9f, 0.2f, 0.1f,
                                 0.3f, 0.4f, -0.1f, 0.0f };

    WaveformPeaks peaks;
    peaks.build({ samples }, 4);

    REQUIRE(peaks.numChannels() == 1);
    REQUIRE(peaks.totalSamples() == 8);

    const auto first = peaks.range(0, 0, 4);
    REQUIRE(first.maximum == 0.5f);
    REQUIRE(first.minimum == -0.9f);

    const auto second = peaks.range(0, 4, 8);
    REQUIRE(second.maximum == 0.4f);
    REQUIRE(second.minimum == -0.1f);
}

TEST_CASE("A range spanning bins combines them", "[app][waveformpeaks]")
{
    std::vector<float> samples { 0.5f, -0.9f, 0.2f, 0.1f,
                                 0.3f, 0.4f, -0.1f, 0.0f };

    WaveformPeaks peaks;
    peaks.build({ samples }, 4);

    const auto whole = peaks.range(0, 0, 8);
    REQUIRE(whole.maximum == 0.5f);
    REQUIRE(whole.minimum == -0.9f);
}

TEST_CASE("A range shorter than a bin still reports that bin", "[app][waveformpeaks]")
{
    // Zoomed in past one bin per pixel, adjacent columns ask for sub-bin
    // ranges. Returning nothing would draw gaps in the waveform, which reads
    // as silence that isn't there.
    std::vector<float> samples { 0.5f, -0.9f, 0.2f, 0.1f };

    WaveformPeaks peaks;
    peaks.build({ samples }, 4);

    const auto tiny = peaks.range(0, 1, 2);
    REQUIRE(tiny.maximum == 0.5f);
    REQUIRE(tiny.minimum == -0.9f);
}

TEST_CASE("Ranges outside the file clamp into it", "[app][waveformpeaks]")
{
    // Scrolling and zooming routinely ask for columns past either end.
    WaveformPeaks peaks;
    peaks.build({ ramp(1000, -1.0f, 1.0f) }, 64);

    REQUIRE_FALSE(peaks.range(0, -5000, 10).isEmpty());
    REQUIRE_FALSE(peaks.range(0, 990, 100000).isEmpty());

    // A wholly out-of-range channel is empty rather than out of bounds.
    REQUIRE(peaks.range(5, 0, 100).isEmpty());
    REQUIRE(peaks.range(-1, 0, 100).isEmpty());
}

TEST_CASE("A trailing partial bin is kept, not dropped", "[app][waveformpeaks]")
{
    // Ten samples at four per bin is two full bins and a remainder. Dropping
    // it would silently truncate the end of every file whose length isn't a
    // multiple of the bin size — which is nearly all of them.
    std::vector<float> samples(10, 0.25f);
    samples[9] = 0.8f;

    WaveformPeaks peaks;
    peaks.build({ samples }, 4);

    REQUIRE(peaks.totalSamples() == 10);
    REQUIRE(peaks.range(0, 8, 10).maximum == 0.8f);
    REQUIRE(peaks.overallMagnitude() == 0.8f);
}

TEST_CASE("Channels are summarised independently", "[app][waveformpeaks]")
{
    // A stereo file's two sides routinely differ, and averaging them would
    // hide exactly the imbalance a stereo waveform is looked at to find.
    std::vector<float> left(8, 0.9f);
    std::vector<float> right(8, 0.1f);

    WaveformPeaks peaks;
    peaks.build({ left, right }, 4);

    REQUIRE(peaks.numChannels() == 2);
    REQUIRE(peaks.range(0, 0, 8).maximum == 0.9f);
    REQUIRE(peaks.range(1, 0, 8).maximum == 0.1f);
}

TEST_CASE("Overall magnitude finds the loudest point anywhere", "[app][waveformpeaks]")
{
    // What a "this clips at the current gain" warning is judged against, so
    // it has to find a single loud sample buried anywhere in the file.
    std::vector<float> samples(5000, 0.1f);
    samples[4321] = -0.95f;

    WaveformPeaks peaks;
    peaks.build({ samples });

    REQUIRE(std::abs(peaks.overallMagnitude() - 0.95f) < 1.0e-6f);
}

TEST_CASE("A zero or negative bin size is corrected, not divided by", "[app][waveformpeaks]")
{
    WaveformPeaks peaks;
    peaks.build({ ramp(100, -1.0f, 1.0f) }, 0);

    REQUIRE(peaks.samplesPerBin() >= 1);
    REQUIRE_FALSE(peaks.isEmpty());
    REQUIRE_FALSE(peaks.range(0, 0, 100).isEmpty());
}
