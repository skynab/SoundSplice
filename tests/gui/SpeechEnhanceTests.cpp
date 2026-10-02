#include <catch2/catch_test_macros.hpp>

#include <engine/SpeechEnhance.h>

#include <cmath>
#include <random>
#include <vector>

using namespace soundsplice::engine;

namespace
{
    /** Four seconds: a voiced, pitched "syllable" every half second (a
        150 Hz buzz with harmonics, swelling and fading), over steady noise. */
    std::vector<float> voiceInNoise(double rate, std::vector<float>* cleanOut = nullptr)
    {
        std::mt19937                          random(3);
        std::normal_distribution<float>       noise(0.0f, 0.02f);
        const int                             n = (int) (rate * 4.0);
        std::vector<float>                    out((size_t) n), clean((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t        = i / rate;
            const double phase    = std::fmod(t, 0.5);
            const double envelope = phase < 0.3 ? std::sin(3.14159265358979 * phase / 0.3) : 0.0;
            double       voice    = 0.0;
            for (int h = 1; h <= 12; ++h)
                voice += std::sin(2.0 * 3.14159265358979 * 150.0 * h * t) / h;
            clean[(size_t) i] = (float) (0.15 * envelope * voice);
            out[(size_t) i]   = clean[(size_t) i] + noise(random);
        }
        if (cleanOut != nullptr)
            *cleanOut = clean;
        return out;
    }

    /** RMS over [from, to) seconds. */
    double rms(const std::vector<float>& x, double rate, double from, double to)
    {
        double sum = 0.0;
        int    n   = 0;
        for (int i = (int) (from * rate); i < (int) (to * rate) && i < (int) x.size(); ++i, ++n)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return std::sqrt(sum / std::max(1, n));
    }

    /** The lag (in samples, -range..range) at which @p b best matches @p a. */
    int bestLag(const std::vector<float>& a, const std::vector<float>& b, int range)
    {
        int    best = 0;
        double top  = -1e300;
        for (int lag = -range; lag <= range; ++lag)
        {
            double sum = 0.0;
            for (size_t i = (size_t) range; i + (size_t) range < a.size(); ++i)
                sum += (double) a[i] * b[(size_t) ((long long) i + lag)];
            if (sum > top)
            {
                top  = sum;
                best = lag;
            }
        }
        return best;
    }
}

TEST_CASE("Speech enhancement takes the noise out between words, and keeps the words where they were", "[gui][speechenhance]")
{
    for (const double rate : { 48000.0, 44100.0 })
    {
        INFO(rate);
        std::vector<float> clean;
        const auto noisy    = voiceInNoise(rate, &clean);
        const auto enhanced = speechenhance::enhance(noisy, rate);
        REQUIRE(enhanced.size() == noisy.size());

        // The gaps between syllables (0.3-0.5 s of each half second) much quieter.
        const double before = rms(noisy, rate, 1.32, 1.48);
        const double after  = rms(enhanced, rate, 1.32, 1.48);
        INFO("gap before " << before << " after " << after);
        REQUIRE(after < before * 0.3); // over 10 dB down

        // The voice still there.
        REQUIRE(rms(enhanced, rate, 1.05, 1.25) > rms(clean, rate, 1.05, 1.25) * 0.4);

        // And not moved in time: the frame of delay is taken off.
        REQUIRE(std::abs(bestLag(clean, enhanced, 600)) <= 2);
    }
}

TEST_CASE("Speech enhancement's amount blends with the original", "[gui][speechenhance]")
{
    const auto noisy = voiceInNoise(48000.0);
    const auto none  = speechenhance::enhance(noisy, 48000.0, 0.0f);
    REQUIRE(none == noisy);

    const auto full = speechenhance::enhance(noisy, 48000.0, 1.0f);
    const auto half = speechenhance::enhance(noisy, 48000.0, 0.5f);
    for (size_t i = 1000; i < noisy.size(); i += 4999)
        REQUIRE(std::abs(half[i] - 0.5f * (full[i] + noisy[i])) < 1e-5f);

    REQUIRE(speechenhance::enhance({}, 48000.0).empty());
}
