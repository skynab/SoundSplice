#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/Fft.h"
#include "engine/HqStretch.h"

#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    std::vector<float> sine(double hz, double seconds)
    {
        std::vector<float> out((size_t) (seconds * kRate));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = 0.5f * (float) std::sin(2.0 * kPi * hz * (double) i / kRate);
        return out;
    }

    /** The magnitude spectrum of 16384 samples from @p from. */
    std::vector<double> spectrum(const std::vector<float>& audio, size_t from)
    {
        constexpr int      n = 16384;
        std::vector<float> re(n), im(n, 0.0f);
        for (int i = 0; i < n; ++i)
            re[(size_t) i] = audio[from + (size_t) i] * (float) (0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
        fft::transform(re, im, false);
        std::vector<double> mag(n / 2);
        for (int k = 0; k < n / 2; ++k)
            mag[(size_t) k] = std::hypot(re[(size_t) k], im[(size_t) k]);
        return mag;
    }

    double peakHz(const std::vector<float>& audio, size_t from)
    {
        const auto mag  = spectrum(audio, from);
        const auto best = (size_t) (std::max_element(mag.begin() + 1, mag.end() - 1) - mag.begin());
        const double a = std::log(mag[best - 1] + 1e-12), b = std::log(mag[best] + 1e-12), c = std::log(mag[best + 1] + 1e-12);
        return ((double) best + 0.5 * (a - c) / (a - 2.0 * b + c)) * kRate / 16384.0;
    }

    /** Where the energy between 300 Hz and 3 kHz is centred: moves with a
        voice's formants. */
    double centroidHz(const std::vector<float>& audio, size_t from)
    {
        const auto   mag = spectrum(audio, from);
        double       sum = 0.0, weighted = 0.0;
        for (size_t k = (size_t) (300.0 * 16384 / kRate); k < (size_t) (3000.0 * 16384 / kRate); ++k)
        {
            const double power = mag[k] * mag[k];
            sum += power;
            weighted += power * (double) k * kRate / 16384.0;
        }
        return weighted / sum;
    }
}

TEST_CASE("The stretcher makes exactly the length asked, at the same pitch", "[engine][stretch]")
{
    const auto tone = sine(440.0, 2.0);
    for (double factor : { 0.8, 1.5 })
    {
        hqstretch::Settings settings;
        settings.lengthFactor = factor;
        const auto out        = hqstretch::process({ tone, tone }, kRate, settings);
        REQUIRE(out.size() == 2);
        REQUIRE(out[0].size() == (size_t) std::lround(tone.size() * factor));
        INFO(factor);
        REQUIRE_THAT(peakHz(out[0], out[0].size() / 2 - 8192), WithinAbs(440.0, 2.0));
        // Channels stretched together: the same in, all but the same out.
        double apart = 0.0;
        for (size_t i = 0; i < out[0].size(); ++i)
            apart = std::max(apart, (double) std::abs(out[0][i] - out[1][i]));
        INFO("channels differ by up to " << apart);
        REQUIRE(apart < 1.0e-3);
    }
}

TEST_CASE("The stretcher shifts pitch, keeping the length", "[engine][stretch]")
{
    const auto          tone = sine(440.0, 2.0);
    hqstretch::Settings settings;
    settings.semitones = 7.0;
    const auto out     = hqstretch::process({ tone }, kRate, settings);
    REQUIRE(out[0].size() == tone.size());
    REQUIRE_THAT(peakHz(out[0], 40000), WithinAbs(440.0 * std::pow(2.0, 7.0 / 12.0), 3.0));

    // Too short to take in: nothing, so the caller falls back.
    REQUIRE(hqstretch::process({ std::vector<float>(64, 0.1f) }, kRate, settings).empty());
}

TEST_CASE("Keeping formants holds a voice's character as its pitch moves", "[engine][stretch]")
{
    // A "vowel": a 150 Hz buzz through a resonance at 800 Hz.
    std::vector<float> vowel((size_t) (kRate * 2));
    double             y1 = 0.0, y2 = 0.0;
    const double       r = 0.995, w = 2.0 * kPi * 800.0 / kRate;
    for (size_t i = 0; i < vowel.size(); ++i)
    {
        const double buzz = std::fmod((double) i * 150.0 / kRate, 1.0) < 0.02 ? 1.0 : 0.0;
        const double y    = buzz + 2.0 * r * std::cos(w) * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        vowel[i] = (float) (0.02 * y);
    }
    const double before = centroidHz(vowel, 40000);

    hqstretch::Settings moved;
    moved.semitones = 7.0;
    auto kept          = moved;
    kept.keepFormants  = true;
    const double shifted  = centroidHz(hqstretch::process({ vowel }, kRate, moved)[0], 40000);
    const double held     = centroidHz(hqstretch::process({ vowel }, kRate, kept)[0], 40000);
    INFO("before " << before << ", shifted " << shifted << ", held " << held);
    REQUIRE(shifted > before * 1.25);                                // moved up with the pitch
    REQUIRE(std::abs(held - before) < std::abs(shifted - before) * 0.5); // stayed much closer
}

TEST_CASE("A sliding stretch speeds up and bends pitch across the sound", "[engine][stretch]")
{
    const auto tone = sine(440.0, 4.0);

    // Speeding up from as-is to twice as fast: length L ln 2, pitch held.
    hqstretch::Slide faster;
    faster.endTempoPercent = 100.0;
    const auto sped        = hqstretch::slide({ tone }, kRate, faster);
    REQUIRE_THAT((double) sped[0].size(), WithinAbs(tone.size() * std::log(2.0), 2.0));
    REQUIRE_THAT(peakHz(sped[0], sped[0].size() / 2 - 8192), WithinAbs(440.0, 3.0));

    // Pitch rising an octave at the same tempo: low near the start, high near
    // the end, the length unchanged.
    hqstretch::Slide rising;
    rising.endSemitones = 12.0;
    const auto bent     = hqstretch::slide({ tone }, kRate, rising);
    REQUIRE(bent[0].size() == tone.size());
    const auto expected = [](double fraction) { return 440.0 * std::pow(2.0, fraction); };
    // Measured over 16384 samples starting at 20% and at 70%: their middles
    // sit at about 29% and 79% of the way.
    const double early = peakHz(bent[0], (size_t) (0.2 * bent[0].size()));
    const double late  = peakHz(bent[0], (size_t) (0.7 * bent[0].size()));
    REQUIRE_THAT(early, WithinAbs(expected(0.2 + 8192.0 / bent[0].size()), 20.0));
    REQUIRE_THAT(late, WithinAbs(expected(0.7 + 8192.0 / bent[0].size()), 35.0));
    REQUIRE(late > early * 1.4);

    // Too short to take in: nothing.
    REQUIRE(hqstretch::slide({ std::vector<float>(64, 0.1f) }, kRate, rising).empty());
}
