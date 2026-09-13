#include <catch2/catch_test_macros.hpp>

#include <engine/Dither.h>

#include <cmath>
#include <vector>

using looper::engine::TpdfDither;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** What an audio format writer does to a float on the way to a fixed-width
    integer: scale to the word, round, clamp, scale back. */
float quantise(float x, int bits)
{
    const auto steps = (float) ((int64_t) 1 << (bits - 1));
    const float q    = std::round(x * steps);
    return std::clamp(q, -steps, steps - 1.0f) / steps;
}

/** Magnitude at one frequency, by direct correlation. */
double magnitudeAt(const std::vector<float>& signal, double hz, double sampleRate)
{
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < signal.size(); ++n)
    {
        const double phase = 2.0 * kPi * hz * (double) n / sampleRate;
        re += signal[n] * std::cos(phase);
        im -= signal[n] * std::sin(phase);
    }
    return std::sqrt(re * re + im * im) / (double) signal.size();
}
}

TEST_CASE("Dither noise stays within one LSB", "[engine][dither]")
{
    // Bounded, and bounded at the right place: noise larger than an LSB is
    // just hiss, and smaller doesn't decorrelate the error.
    for (int bits : { 16, 24 })
    {
        TpdfDither dither(bits);
        const float lsb = dither.lsb();

        float worst = 0.0f;
        for (int n = 0; n < 200000; ++n)
            worst = std::max(worst, std::abs(dither.processSample(0.0f)));

        INFO("at " << bits << " bits: worst " << worst << " lsb " << lsb);
        CHECK(worst <= lsb);
        CHECK(worst > lsb * 0.8f); // and it does reach most of the way
    }
}

TEST_CASE("Dither has no DC offset", "[engine][dither]")
{
    // A non-zero mean would be a DC offset added to the whole export, eating
    // headroom and moving nothing audible.
    TpdfDither dither(16);

    double sum = 0.0;
    const int samples = 500000;
    for (int n = 0; n < samples; ++n)
        sum += dither.processSample(0.0f);

    const double mean = sum / samples;
    INFO("mean " << mean << " lsb " << dither.lsb());
    CHECK(std::abs(mean) < dither.lsb() * 0.01);
}

TEST_CASE("Dither rescues a tone quieter than one quantisation step", "[engine][dither]")
{
    // The whole reason dither exists, and the thing that separates it from
    // "adding noise". A sine below one quantisation step rounds to nothing at
    // all when truncated - the signal is not merely noisy, it is *gone*.
    // Dithered, the same sine survives in the average: below the noise floor,
    // but recoverable.
    //
    // This is the check that would fail if the dither were bypassed, wrongly
    // scaled, or applied after quantisation instead of before.
    constexpr int    bits       = 16;
    constexpr double sampleRate = 48000.0;
    constexpr int    samples    = 1 << 17;

    // On an exact FFT bin, so the fundamental's own leakage can't be mistaken
    // for a recovered signal.
    constexpr double tone = 48000.0 * 1024.0 / (double) samples;

    TpdfDither  dither(bits);
    const float lsb       = dither.lsb();
    // Just under half a step, so no sample ever reaches the rounding boundary
    // and truncation annihilates the tone completely. At exactly half an LSB
    // the peaks round away from zero and leave a sparse train of impulses -
    // still ruined, but not *silent*, which muddies what this is showing.
    const float amplitude = lsb * 0.4f;

    std::vector<float> plain, dithered;
    plain.reserve(samples);
    dithered.reserve(samples);

    for (int n = 0; n < samples; ++n)
    {
        const auto v = (float) (amplitude * std::sin(2.0 * kPi * tone * n / sampleRate));
        plain.push_back(quantise(v, bits));
        dithered.push_back(quantise(dither.processSample(v), bits));
    }

    const double plainLevel    = magnitudeAt(plain, tone, sampleRate);
    const double ditheredLevel = magnitudeAt(dithered, tone, sampleRate);
    const double expected      = amplitude * 0.5; // a real sine reads A/2 in this measure

    INFO("expected " << expected << "  truncated " << plainLevel
                     << "  dithered " << ditheredLevel);

    // Truncation destroys it outright: every sample rounds to zero, so what
    // comes back is digital silence.
    CHECK(plainLevel == 0.0);

    // Dither keeps it, within a factor of two of where it should be. Not
    // tighter than that: the tone is a fraction of a quantisation step and is
    // being recovered out of noise, so a loose bound here is honest rather
    // than lazy.
    CHECK(ditheredLevel > expected * 0.5);
    CHECK(ditheredLevel < expected * 2.0);
}

TEST_CASE("Dither decorrelates the quantisation error from the signal", "[engine][dither]")
{
    // The other half of the claim. An undithered quantiser's error follows the
    // signal, which is heard as harmonics that were never played; dither
    // replaces that with a steady hiss. Measured as the second and third
    // harmonic of a quiet tone, where truncation's distortion products land.
    constexpr int    bits       = 16;
    constexpr double sampleRate = 48000.0;
    constexpr int    samples    = 1 << 17;
    constexpr double tone       = 48000.0 * 512.0 / (double) samples;

    TpdfDither  dither(bits);
    const float amplitude = dither.lsb() * 4.0f; // quiet, but genuinely present

    std::vector<float> plain, dithered;
    for (int n = 0; n < samples; ++n)
    {
        const auto v = (float) (amplitude * std::sin(2.0 * kPi * tone * n / sampleRate));
        plain.push_back(quantise(v, bits));
        dithered.push_back(quantise(dither.processSample(v), bits));
    }

    const double plainThird    = magnitudeAt(plain, tone * 3.0, sampleRate);
    const double ditheredThird = magnitudeAt(dithered, tone * 3.0, sampleRate);

    INFO("3rd harmonic: truncated " << plainThird << "  dithered " << ditheredThird);

    // The distortion product is markedly reduced. It does not vanish into
    // nothing — it is traded for noise — so this asserts the trade, not a
    // miracle.
    CHECK(ditheredThird < plainThird * 0.5);
}

TEST_CASE("Dither is reproducible from its seed", "[engine][dither]")
{
    // Two exports of the same mix should produce the same file. Random noise
    // that is genuinely random would make every export byte-different, which
    // breaks any check that two renders agree — including this project's own
    // bounce comparisons.
    TpdfDither a(16, 12345u);
    TpdfDither b(16, 12345u);

    for (int n = 0; n < 1000; ++n)
        REQUIRE(a.processSample(0.25f) == b.processSample(0.25f));

    // And a different seed gives a different sequence, or the seed is a lie.
    TpdfDither c(16, 999u);
    a.reset(12345u);

    bool anyDifference = false;
    for (int n = 0; n < 1000 && ! anyDifference; ++n)
        anyDifference = a.processSample(0.25f) != c.processSample(0.25f);

    CHECK(anyDifference);
}

TEST_CASE("Dither scales with the target word length", "[engine][dither]")
{
    // 24-bit's LSB is 256 times smaller than 16-bit's, so its dither must be
    // too — dithering a 24-bit file at 16-bit amplitude would add noise 48dB
    // louder than the format's own floor.
    TpdfDither sixteen(16);
    TpdfDither twentyFour(24);

    CHECK(sixteen.lsb() > twentyFour.lsb() * 200.0f);
    CHECK(sixteen.lsb() < twentyFour.lsb() * 300.0f);
}
