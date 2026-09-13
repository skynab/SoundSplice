#include <catch2/catch_test_macros.hpp>

#include <engine/SessionMath.h>

using namespace looper::engine;

namespace
{
    // One bar of 4/4 at 120bpm, 48kHz: 2 seconds, 96000 samples.
    constexpr double kBarSamples = 96000.0;
}

TEST_CASE("A launch mid-bar waits for the next bar line", "[engine][session]")
{
    REQUIRE(SessionMath::nextLaunchBoundary(1, kBarSamples) == 96000);
    REQUIRE(SessionMath::nextLaunchBoundary(50000, kBarSamples) == 96000);
    REQUIRE(SessionMath::nextLaunchBoundary(95999, kBarSamples) == 96000);
}

TEST_CASE("A launch exactly on a bar line fires there, not a bar later", "[engine][session]")
{
    // Without the epsilon this rounds up and the clip starts a whole bar late
    // — the same trap the metronome's first-click-after-seek has.
    REQUIRE(SessionMath::nextLaunchBoundary(0, kBarSamples) == 0);
    REQUIRE(SessionMath::nextLaunchBoundary(96000, kBarSamples) == 96000);
    REQUIRE(SessionMath::nextLaunchBoundary(192000, kBarSamples) == 192000);
}

TEST_CASE("A zero quantum launches immediately", "[engine][session]")
{
    REQUIRE(SessionMath::nextLaunchBoundary(12345, 0.0) == 12345);
    REQUIRE(SessionMath::nextLaunchBoundary(12345, -1.0) == 12345);
}

TEST_CASE("A boundary inside the block reports where it lands", "[engine][session]")
{
    int offset = -1;
    REQUIRE(SessionMath::boundaryInBlock(96000, 95900, 512, offset));
    REQUIRE(offset == 100);
}

TEST_CASE("A boundary beyond the block is not reached yet", "[engine][session]")
{
    int offset = -1;
    REQUIRE_FALSE(SessionMath::boundaryInBlock(96000, 90000, 512, offset));
}

TEST_CASE("A boundary at the very start of the block lands at offset zero", "[engine][session]")
{
    int offset = -1;
    REQUIRE(SessionMath::boundaryInBlock(96000, 96000, 512, offset));
    REQUIRE(offset == 0);
}

TEST_CASE("A boundary already passed fires immediately rather than being missed", "[engine][session]")
{
    // This is what makes an unquantized launch take effect on the next block
    // instead of never: its boundary is behind the playhead the moment the
    // audio thread sees the request.
    int offset = -1;
    REQUIRE(SessionMath::boundaryInBlock(50000, 96000, 512, offset));
    REQUIRE(offset == 0);
}

TEST_CASE("Consecutive blocks see each bar line exactly once", "[engine][session]")
{
    // Walk four bars in 512-sample blocks and count the boundaries crossed.
    // A double-count here would relaunch a clip mid-bar; a miss would drop a
    // launch entirely.
    constexpr int blockSize = 512;
    const int64_t total     = (int64_t) (kBarSamples * 4);

    int crossings = 0;
    for (int64_t playhead = 0; playhead < total; playhead += blockSize)
    {
        const int64_t boundary = SessionMath::nextLaunchBoundary(playhead, kBarSamples);
        int           offset   = 0;
        if (SessionMath::boundaryInBlock(boundary, playhead, blockSize, offset))
            ++crossings;
    }

    REQUIRE(crossings == 4); // bars 0, 1, 2 and 3
}
