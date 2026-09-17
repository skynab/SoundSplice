#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/ToneDsp.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    float sine(double hz, int n, float amplitude = 0.5f)
    {
        return amplitude * (float) std::sin(2.0 * kPi * hz * n / kRate);
    }

    /** RMS of @p process over a second of a @p hz sine, skipping the first half second. */
    template <typename Process>
    double settledRms(double hz, Process&& process)
    {
        double sum = 0.0;
        int    count = 0;
        for (int n = 0; n < (int) kRate; ++n)
        {
            const float out = process(sine(hz, n));
            if (n >= (int) kRate / 2)
            {
                sum += (double) out * out;
                ++count;
            }
        }
        return std::sqrt(sum / count);
    }
}

TEST_CASE("A phaser's allpass chain keeps the level; the mix makes the notches", "[engine][tone]")
{
    const double dryRms = 0.5 / std::sqrt(2.0);

    // Fully wet with no feedback, an allpass chain changes phase, never level.
    Phaser wet;
    wet.prepare(kRate);
    wet.setStages(8);
    wet.setFeedback(0.0f);
    wet.setMix(1.0f);
    REQUIRE_THAT(settledRms(1000.0, [&](float x) { return wet.processSample(x); }), WithinAbs(dryRms, 0.01));

    // Dry, it's the input exactly.
    Phaser dry;
    dry.prepare(kRate);
    dry.setMix(0.0f);
    for (int n = 0; n < 1000; ++n)
        REQUIRE(dry.processSample(sine(440.0, n)) == sine(440.0, n));

    // Half wet with the sweep held still at its lowest point, a tone at a
    // notch is cancelled and one far from it isn't.
    const auto heldAt150Hz = [](double hz)
    {
        Phaser phaser;
        phaser.prepare(kRate);
        phaser.setDepth(0.0f); // stays at 150 Hz
        phaser.setStages(2);   // one notch, where the chain's shift is 180 degrees: at 150 Hz
        phaser.setFeedback(0.0f);
        phaser.setMix(0.5f);
        return settledRms(hz, [&](float x) { return phaser.processSample(x); });
    };
    REQUIRE(heldAt150Hz(150.0) < dryRms * 0.05);
    REQUIRE(heldAt150Hz(5000.0) > dryRms * 0.9);
}

TEST_CASE("A flanger with the sweep off is a comb: dry plus the input a fixed delay later", "[engine][tone]")
{
    Flanger flanger;
    flanger.prepare(kRate);
    flanger.setDepth(0.0f);
    flanger.setDelayMs(1.0f); // 48 samples
    flanger.setFeedback(0.0f);
    flanger.setMix(0.5f);

    std::vector<float> in(2000), out(2000);
    for (int n = 0; n < 2000; ++n)
    {
        in[(size_t) n]  = sine(700.0, n);
        out[(size_t) n] = flanger.processSample(in[(size_t) n]);
    }

    for (int n = 100; n < 2000; ++n)
        REQUIRE_THAT(out[(size_t) n], WithinAbs(0.5 * in[(size_t) n] + 0.5 * in[(size_t) n - 48], 1.0e-4));
}

TEST_CASE("A flanger's output stays bounded at full feedback and sweep", "[engine][tone]")
{
    Flanger flanger;
    flanger.prepare(kRate);
    flanger.setRateHz(2.0f);
    flanger.setDepth(1.0f);
    flanger.setFeedback(0.95f);
    flanger.setMix(1.0f);

    float peak = 0.0f;
    for (int n = 0; n < (int) kRate * 5; ++n)
        peak = std::max(peak, std::abs(flanger.processSample(sine(300.0, n))));
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak < 20.0f);
}

TEST_CASE("Bass and treble shelve their ends and leave the middle", "[engine][tone]")
{
    BassTreble tone;
    tone.prepare(kRate);
    tone.setBassDb(9.0f);
    tone.setTrebleDb(-6.0f);
    tone.setVolumeDb(-2.0f);

    REQUIRE_THAT(tone.magnitudeDbAt(20.0f), WithinAbs(9.0 - 2.0, 0.5));
    REQUIRE_THAT(tone.magnitudeDbAt(20000.0f), WithinAbs(-6.0 - 2.0, 0.6));
    REQUIRE_THAT(tone.magnitudeDbAt(1000.0f), WithinAbs(-2.0, 0.5));

    // And the running filter does what its curve says.
    const double rms = settledRms(40.0, [&](float x) { return tone.processSample(x); });
    REQUIRE_THAT(20.0 * std::log10(rms / (0.5 / std::sqrt(2.0))), WithinAbs(tone.magnitudeDbAt(40.0f), 0.3));
}

TEST_CASE("Stereo tools swap, fold, widen and balance", "[engine][tone]")
{
    const auto apply = [](StereoTool tool, float left, float right)
    {
        tool.processFrame(left, right);
        return std::pair { left, right };
    };

    StereoTool untouched;
    REQUIRE(apply(untouched, 0.3f, -0.7f) == std::pair { 0.3f, -0.7f });

    StereoTool swap;
    swap.swap = true;
    REQUIRE(apply(swap, 0.3f, -0.7f) == std::pair { -0.7f, 0.3f });

    StereoTool mono;
    mono.mono = true;
    auto [ml, mr] = apply(mono, 0.3f, -0.7f);
    REQUIRE_THAT(ml, WithinAbs(-0.2, 1e-6));
    REQUIRE(ml == mr);

    StereoTool wide;
    wide.width = 2.0f; // mid -0.2, side 0.5 doubled
    auto [wl, wr] = apply(wide, 0.3f, -0.7f);
    REQUIRE_THAT(wl, WithinAbs(0.8, 1e-6));
    REQUIRE_THAT(wr, WithinAbs(-1.2, 1e-6));

    StereoTool rightOnly;
    rightOnly.balance = 1.0f;
    REQUIRE(apply(rightOnly, 0.3f, -0.7f) == std::pair { 0.0f, -0.7f });

    StereoTool leanLeft;
    leanLeft.balance = -0.5f;
    auto [bl, br] = apply(leanLeft, 0.3f, -0.7f);
    REQUIRE(bl == 0.3f);
    REQUIRE_THAT(br, WithinAbs(-0.35, 1e-6));
}
