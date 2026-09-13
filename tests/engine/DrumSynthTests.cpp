#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/DrumSynth.h>

#include <cmath>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    bool allFiniteAndBounded(const std::vector<float>& buffer, float limit = 2.0f)
    {
        for (float s : buffer)
        {
            if (! std::isfinite(s) || std::abs(s) > limit)
                return false;
        }
        return true;
    }

    float rms(const std::vector<float>& buffer, size_t from, size_t count)
    {
        double sum = 0.0;
        const size_t to = std::min(buffer.size(), from + count);
        for (size_t i = from; i < to; ++i)
            sum += (double) buffer[i] * buffer[i];
        const size_t n = to - from;
        return n == 0 ? 0.0f : (float) std::sqrt(sum / (double) n);
    }

    /** A crude proxy for pitch: how many times the signal crosses zero in a
        window, which rises with frequency. Good enough to tell "this window
        is higher-pitched than that one" without needing an FFT. */
    int zeroCrossings(const std::vector<float>& buffer, size_t from, size_t count)
    {
        const size_t to = std::min(buffer.size(), from + count);
        int crossings = 0;
        for (size_t i = from + 1; i < to; ++i)
            if ((buffer[i - 1] < 0.0f) != (buffer[i] < 0.0f))
                ++crossings;
        return crossings;
    }
}

TEST_CASE("A kick is finite, bounded, and decays", "[engine][drumsynth]")
{
    const auto kick = synthesizeKick(kSampleRate, 160.0f, 45.0f, 25.0f, 250.0f, 3.0f);
    REQUIRE(kick.size() > 100);
    REQUIRE(allFiniteAndBounded(kick));

    const float early = rms(kick, 0, 200);
    const float late  = rms(kick, kick.size() - 200, 200);
    INFO("early rms " << early << " late rms " << late);
    REQUIRE(late < early);
}

TEST_CASE("A kick's pitch actually falls, not just its level", "[engine][drumsynth]")
{
    // The defining feature of a kick over a plain decaying tone: it starts
    // higher and sweeps down. Without the sweep this would just be a
    // sine-wave blip, indistinguishable from any other pitched decay.
    const auto kick = synthesizeKick(kSampleRate, 160.0f, 45.0f, 25.0f, 250.0f, 1.0f);

    const int earlyCrossings = zeroCrossings(kick, 0, 400);
    const int lateCrossings  = zeroCrossings(kick, kick.size() / 2, 400);
    INFO("early crossings " << earlyCrossings << " late crossings " << lateCrossings);
    REQUIRE(earlyCrossings > lateCrossings);
}

TEST_CASE("A snare is finite, bounded, and decays", "[engine][drumsynth]")
{
    const auto snare = synthesizeSnare(kSampleRate, 200.0f, 0.3f, 180.0f, 2000.0f, 1u);
    REQUIRE(snare.size() > 100);
    REQUIRE(allFiniteAndBounded(snare));

    const float early = rms(snare, 0, 200);
    const float late  = rms(snare, snare.size() - 200, 200);
    REQUIRE(late < early);
}

TEST_CASE("Two snares with different seeds don't sound identical", "[engine][drumsynth]")
{
    // The noise component is the whole point of a snare — if the seed did
    // nothing, every "variant" would be the same sound wearing a different
    // name.
    const auto a = synthesizeSnare(kSampleRate, 200.0f, 0.3f, 180.0f, 2000.0f, 1u);
    const auto b = synthesizeSnare(kSampleRate, 200.0f, 0.3f, 180.0f, 2000.0f, 2u);

    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    REQUIRE(worst > 0.01f);
}

TEST_CASE("A hat is finite, bounded, and decays quickly", "[engine][drumsynth]")
{
    const auto hat = synthesizeHat(kSampleRate, 8000.0f, 60.0f, 3u);
    REQUIRE(hat.size() > 50);
    REQUIRE(allFiniteAndBounded(hat));

    const float early = rms(hat, 0, 100);
    const float late  = rms(hat, hat.size() - 100, 100);
    REQUIRE(late < early);
}

TEST_CASE("A clap is finite, bounded, and has more than one burst", "[engine][drumsynth]")
{
    const auto clap = synthesizeClap(kSampleRate, 1500.0f, 120.0f, 4u);
    REQUIRE(clap.size() > 100);
    REQUIRE(allFiniteAndBounded(clap));

    // Between the first burst's decay and the second burst's onset, level
    // should dip — a single unbroken decay would mean the layering did
    // nothing.
    const size_t betweenBurst1And2 = (size_t) (0.008 * kSampleRate); // ~8ms, before burst 2 at 12ms
    const size_t atBurst1Peak      = (size_t) (0.001 * kSampleRate);
    REQUIRE(rms(clap, betweenBurst1And2, 50) < rms(clap, atBurst1Peak, 50));
}

TEST_CASE("normalizePeak scales to the target and leaves silence alone", "[engine][drumsynth]")
{
    std::vector<float> buffer { 0.1f, -0.4f, 0.2f, -0.1f };
    normalizePeak(buffer, 0.8f);

    float peak = 0.0f;
    for (float s : buffer)
        peak = std::max(peak, std::abs(s));
    REQUIRE(peak == Catch::Approx(0.8f).margin(1.0e-5f));

    std::vector<float> silence(100, 0.0f);
    normalizePeak(silence, 0.8f);
    for (float s : silence)
        REQUIRE(s == 0.0f);
}
