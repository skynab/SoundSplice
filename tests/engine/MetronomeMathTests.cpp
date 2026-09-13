#include <catch2/catch_test_macros.hpp>

#include <engine/MetronomeMath.h>

#include <vector>

using namespace looper::engine;

namespace
{
    // 120bpm at 48kHz: half a second, 24000 samples, per quarter-note beat.
    constexpr double kSamplesPerBeat = 24000.0;

    /** Mirrors Metronome::process's scan, so the tests exercise the same walk
        the audio thread does rather than a reimplementation of it. */
    std::vector<int> beatOffsetsInBlock(int64_t playhead, int numSamples, double samplesPerBeat)
    {
        std::vector<int> offsets;
        for (int64_t beat = MetronomeMath::firstBeatAtOrAfter(playhead, samplesPerBeat);; ++beat)
        {
            const int64_t offset = MetronomeMath::offsetOfBeat(beat, playhead, samplesPerBeat);
            if (offset >= numSamples)
                break;
            if (offset >= 0)
                offsets.push_back((int) offset);
        }
        return offsets;
    }
}

TEST_CASE("A block starting exactly on a beat ticks at offset zero", "[engine][metronome]")
{
    // The epsilon in firstBeatAtOrAfter exists for this: without it, rounding
    // pushes the boundary to the next beat and the click after a seek to a
    // bar line is silently dropped.
    const auto offsets = beatOffsetsInBlock((int64_t) (2 * kSamplesPerBeat), 512, kSamplesPerBeat);

    REQUIRE(offsets.size() == 1);
    REQUIRE(offsets[0] == 0);
}

TEST_CASE("A block between beats ticks not at all", "[engine][metronome]")
{
    const auto offsets = beatOffsetsInBlock(1000, 512, kSamplesPerBeat);
    REQUIRE(offsets.empty());
}

TEST_CASE("A beat landing mid-block ticks at the right offset", "[engine][metronome]")
{
    // Block starts 100 samples before beat 1.
    const auto offsets = beatOffsetsInBlock((int64_t) kSamplesPerBeat - 100, 512, kSamplesPerBeat);

    REQUIRE(offsets.size() == 1);
    REQUIRE(offsets[0] == 100);
}

TEST_CASE("A block long enough to span several beats ticks for each", "[engine][metronome]")
{
    // A deliberately huge block (2.5 beats' worth) — the scan must not stop
    // after the first beat it finds.
    const auto offsets = beatOffsetsInBlock(0, (int) (kSamplesPerBeat * 2.5), kSamplesPerBeat);

    REQUIRE(offsets.size() == 3); // beats 0, 1 and 2
    REQUIRE(offsets[0] == 0);
    REQUIRE(offsets[1] == (int) kSamplesPerBeat);
    REQUIRE(offsets[2] == (int) (kSamplesPerBeat * 2));
}

TEST_CASE("Consecutive blocks tick exactly once per beat, with no drift", "[engine][metronome]")
{
    // Walk 16 beats in 512-sample blocks and count the ticks. A rounding bug
    // in the boundary math shows up here as a missed or doubled beat, which
    // is the failure that would be hardest to spot by ear.
    constexpr int blockSize = 512;
    const int64_t total     = (int64_t) (kSamplesPerBeat * 16);

    int ticks = 0;
    for (int64_t playhead = 0; playhead < total; playhead += blockSize)
        ticks += (int) beatOffsetsInBlock(playhead, blockSize, kSamplesPerBeat).size();

    REQUIRE(ticks == 16);
}

TEST_CASE("Downbeats are accented once per bar", "[engine][metronome]")
{
    constexpr double quartersPerBar = 4.0; // 4/4

    REQUIRE(MetronomeMath::isDownbeat(0, quartersPerBar));
    REQUIRE_FALSE(MetronomeMath::isDownbeat(1, quartersPerBar));
    REQUIRE_FALSE(MetronomeMath::isDownbeat(3, quartersPerBar));
    REQUIRE(MetronomeMath::isDownbeat(4, quartersPerBar));
    REQUIRE(MetronomeMath::isDownbeat(16, quartersPerBar));
}

TEST_CASE("Downbeats follow the time signature", "[engine][metronome]")
{
    constexpr double threeFour = 3.0; // 3/4 — a bar every three quarters

    REQUIRE(MetronomeMath::isDownbeat(0, threeFour));
    REQUIRE(MetronomeMath::isDownbeat(3, threeFour));
    REQUIRE(MetronomeMath::isDownbeat(6, threeFour));
    REQUIRE_FALSE(MetronomeMath::isDownbeat(4, threeFour));
}
