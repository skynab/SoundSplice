#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/OnsetDetection.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    /** A kick-like thump every @p beatSeconds, over a steady hum and a little
        noise, starting at @p firstSeconds. */
    std::vector<float> drumLoop(double seconds, double beatSeconds, double firstSeconds)
    {
        std::mt19937                          random(11);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        std::vector<float>                    audio((size_t) (seconds * kRate));
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const double t    = (double) i / kRate;
            double       sample = 0.15 * std::sin(2.0 * 3.14159265358979 * 220.0 * t) + 0.01 * uniform(random);
            if (t >= firstSeconds)
            {
                const double into = std::fmod(t - firstSeconds, beatSeconds);
                if (into < 0.2)
                    sample += 0.7 * std::exp(-into * 25.0)
                            * (std::sin(2.0 * 3.14159265358979 * 70.0 * into) + 0.5 * uniform(random) * std::exp(-into * 80.0));
            }
            audio[i] = (float) sample;
        }
        return audio;
    }

    OnsetDetector detect(const std::vector<float>& audio)
    {
        OnsetDetector detector(kRate);
        for (size_t at = 0; at < audio.size(); at += 7000) // uneven chunks, as the app reads them
        {
            const int    n        = (int) std::min<size_t>(7000, audio.size() - at);
            const float* channels[] { audio.data() + at };
            detector.append(channels, 1, n);
        }
        return detector;
    }
}

TEST_CASE("The beat finder marks each hit, and not the held note under them", "[engine][beats]")
{
    const double beat  = 0.5; // 120 bpm
    const double first = 0.3;
    const auto   audio = drumLoop(8.0, beat, first);
    const auto   found = detect(audio).onsets(0.5, 0.1);

    const int expected = (int) std::floor((8.0 - first) / beat) + 1;
    REQUIRE((int) found.size() >= expected - 1);
    REQUIRE((int) found.size() <= expected + 1);

    // Each within a frame or so of a hit.
    for (auto at : found)
    {
        const double t      = (double) at / kRate;
        const double offset = std::fmod(t - first + beat * 0.5, beat) - beat * 0.5;
        INFO("onset at " << t);
        REQUIRE(std::abs(offset) < 0.03);
    }
}

TEST_CASE("The beat finder hears the tempo, and no pulse in a steady sound", "[engine][beats]")
{
    const auto loop = detect(drumLoop(10.0, 60.0 / 128.0, 0.1));
    REQUIRE_THAT(loop.tempoBpm(), WithinAbs(128.0, 2.0));

    std::vector<float> hum((size_t) kRate * 5);
    for (size_t i = 0; i < hum.size(); ++i)
        hum[i] = 0.3f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * (double) i / kRate);
    const auto steady = detect(hum);
    REQUIRE(steady.onsets(0.5, 0.1).size() <= 1); // perhaps its start, nothing after
    REQUIRE(steady.tempoBpm() == 0.0);
}

TEST_CASE("More sensitivity finds softer onsets; a gap keeps a flam as one", "[engine][beats]")
{
    // Loud hits with soft ones between.
    std::vector<float> audio((size_t) kRate * 6, 0.0f);
    std::mt19937                          random(5);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    for (int hit = 0; hit < 24; ++hit)
    {
        const auto  start = (size_t) ((0.2 + hit * 0.25) * kRate);
        const float level = hit % 2 == 0 ? 0.8f : 0.08f;
        for (size_t i = 0; i < 4000 && start + i < audio.size(); ++i)
            audio[start + i] += level * uniform(random) * std::exp(-(float) i / 800.0f);
    }
    const auto detector = detect(audio);
    const auto strict   = detector.onsets(0.1, 0.05);
    const auto keen     = detector.onsets(0.9, 0.05);
    REQUIRE(keen.size() > strict.size());
    REQUIRE(keen.size() >= 20);

    // With a gap longer than their spacing, every other one merges away.
    REQUIRE(detector.onsets(0.9, 0.3).size() <= 13);
}
