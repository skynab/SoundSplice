#include <catch2/catch_test_macros.hpp>

#include <engine/SilenceDetection.h>

using namespace soundsplice::engine::silence;

namespace
{
    std::vector<float> peaksOf(const std::vector<std::vector<float>>& channels, int windowFrames,
                               const std::vector<int>& chunkSizes)
    {
        PeakEnvelope envelope(windowFrames);
        int at = 0;
        for (const int size : chunkSizes)
        {
            std::vector<const float*> pointers;
            for (const auto& channel : channels)
                pointers.push_back(channel.data() + at);
            envelope.append(pointers.data(), (int) pointers.size(), size);
            at += size;
        }
        return envelope.finish();
    }
}

TEST_CASE("The envelope keeps the loudest sample per window across channels", "[engine][silence]")
{
    const std::vector<float> left  { 0.1f, -0.5f, 0.2f,   0.0f, 0.0f, 0.0f,   0.3f };
    const std::vector<float> right { 0.0f,  0.1f, 0.0f,  -0.4f, 0.0f, 0.1f,   0.0f };

    const std::vector<float> expected { 0.5f, 0.4f, 0.3f }; // the last window is partly filled

    REQUIRE(peaksOf({ left, right }, 3, { 7 }) == expected);

    // However the audio arrives, in chunks that don't line up with windows.
    REQUIRE(peaksOf({ left, right }, 3, { 1, 4, 2 }) == expected);
    REQUIRE(peaksOf({ left, right }, 3, { 2, 2, 2, 1 }) == expected);
}

TEST_CASE("Silent runs are found at the start, in the middle and at the end", "[engine][silence]")
{
    const float quiet = 0.001f, loud = 0.5f;
    //                                     0      10     20     30     40     50     60
    const std::vector<float> peaks { quiet, quiet, loud, quiet, quiet, quiet, loud, quiet };

    const auto runs = silentRuns(peaks, 10, 75, 0.01f, 1);
    REQUIRE(runs == std::vector<FrameRange> { { 0, 20 }, { 30, 60 }, { 70, 75 } });

    // Shorter than the minimum: not silence.
    REQUIRE(silentRuns(peaks, 10, 75, 0.01f, 25) == std::vector<FrameRange> { { 30, 60 } });

    // The threshold is strict: exactly at it is sound.
    REQUIRE(silentRuns({ 0.01f, 0.01f }, 10, 20, 0.01f, 1).empty());

    REQUIRE(silentRuns({}, 10, 0, 0.01f, 1).empty());
}

TEST_CASE("Decibels convert to gain", "[engine][silence]")
{
    REQUIRE(gainForDecibels(0.0f) == 1.0f);
    REQUIRE(std::abs(gainForDecibels(-20.0f) - 0.1f) < 1.0e-6f);
    REQUIRE(std::abs(gainForDecibels(-60.0f) - 0.001f) < 1.0e-7f);
}
