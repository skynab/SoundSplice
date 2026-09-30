#include <catch2/catch_test_macros.hpp>

#include <engine/RetroRecorder.h>

#include <vector>

using namespace soundsplice::engine;

namespace
{
    /** Feeds @p blocks blocks of 100 samples, each sample its own index from
        @p firstValue on, played from @p playhead on. Mono, so it's kept on
        both sides. */
    void feed(RetroRecorder& retro, int blocks, int64_t playhead, float firstValue, bool playing = true)
    {
        std::vector<float> block(100);
        for (int b = 0; b < blocks; ++b)
        {
            for (int n = 0; n < 100; ++n)
                block[(size_t) n] = firstValue + (float) (b * 100 + n);
            const float* channels[] { block.data() };
            retro.process(channels, 1, 100, playing, playhead + b * 100);
        }
    }
}

TEST_CASE("Retroactive recording keeps the latest run, placed where it was played", "[gui][recording]")
{
    RetroRecorder retro;
    retro.prepare(1000.0, 1.0); // room for 1000 samples
    REQUIRE(retro.isOn());

    juce::AudioBuffer<float> out;
    int64_t                  start = 0;
    REQUIRE_FALSE(retro.copyLatestRun(out, start)); // nothing played yet

    feed(retro, 5, 5000, 0.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 5000);
    REQUIRE(out.getNumSamples() == 500);
    REQUIRE(out.getSample(0, 0) == 0.0f);
    REQUIRE(out.getSample(1, 499) == 499.0f); // the mono input on both sides

    // Stopping keeps it; playing again from somewhere else starts a new run.
    feed(retro, 2, 0, 0.0f, false);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 5000);
    feed(retro, 3, 20000, 1000.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 20000);
    REQUIRE(out.getNumSamples() == 300);
    REQUIRE(out.getSample(0, 0) == 1000.0f);

    // A run longer than the room keeps its end, and says where that starts.
    feed(retro, 15, 40000, 0.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(out.getNumSamples() == 1000);
    REQUIRE(start == 40500);
    REQUIRE(out.getSample(0, 0) == 500.0f);
    REQUIRE(out.getSample(0, 999) == 1499.0f);

    // While it may still be written over, the oldest end is left out.
    REQUIRE(retro.copyLatestRun(out, start, 200));
    REQUIRE(out.getNumSamples() == 800);
    REQUIRE(start == 40700);
    REQUIRE(out.getSample(0, 0) == 700.0f);

    retro.prepare(1000.0, 0.0);
    REQUIRE_FALSE(retro.isOn());
    REQUIRE_FALSE(retro.copyLatestRun(out, start));
}
