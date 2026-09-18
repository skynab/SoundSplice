#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Spectrogram.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    SpectrogramData build(const std::vector<float>& mono, int chunk)
    {
        SpectrogramBuilder builder(kRate, (std::int64_t) mono.size());
        for (size_t at = 0; at < mono.size(); at += (size_t) chunk)
        {
            const float* channels[] { mono.data() + at };
            builder.append(channels, 1, (int) std::min<size_t>((size_t) chunk, mono.size() - at));
        }
        return builder.finish();
    }

    int loudestBin(const SpectrogramData& data, int column)
    {
        int best = 0;
        for (int b = 1; b < data.bins; ++b)
            if (data.levelDb(column, b) > data.levelDb(column, best))
                best = b;
        return best;
    }
}

TEST_CASE("A spectrogram shows a tone at its frequency and level, in the right place in time", "[engine][spectrogram]")
{
    // A second of 1 kHz, then a second of 5 kHz, at half scale.
    std::vector<float> audio(96000);
    for (int n = 0; n < 96000; ++n)
        audio[(size_t) n] = (float) (0.5 * std::sin(2.0 * kPi * (n < 48000 ? 1000.0 : 5000.0) * n / kRate));

    const auto data = build(audio, 4096);
    REQUIRE(data.bins == SpectrogramBuilder::kFftSize / 2 + 1);
    REQUIRE_THAT(data.secondsPerColumn, WithinAbs(512.0 / kRate, 1e-12)); // a quarter-window hop
    REQUIRE(data.columns == (96000 - 2048) / 512 + 1);

    const int early = 20;  // well inside the first second
    const int late  = 150; // well inside the second
    REQUIRE_THAT(data.frequencyOfBin(loudestBin(data, early)), WithinAbs(1000.0, 30.0));
    REQUIRE_THAT(data.frequencyOfBin(loudestBin(data, late)), WithinAbs(5000.0, 30.0));

    // Half scale is -6 dB, give or take where the tone falls between bins.
    REQUIRE_THAT(data.levelDb(early, loudestBin(data, early)), WithinAbs(-6.0, 1.5));
    // And far from the tone, far down.
    REQUIRE(data.levelDb(early, (int) data.binOfFrequency(12000.0)) < -80.0f);
}

TEST_CASE("A spectrogram is the same however the audio arrives, and stays bounded for long clips", "[engine][spectrogram]")
{
    std::vector<float> audio(50000);
    for (size_t n = 0; n < audio.size(); ++n)
        audio[n] = (float) std::sin(0.013 * (double) n * (double) n / 1000.0); // a sweep

    const auto whole   = build(audio, 50000);
    const auto chunked = build(audio, 333);
    REQUIRE(whole.columns == chunked.columns);
    REQUIRE(whole.levelsDb == chunked.levelsDb);

    // An hour at 48 kHz still makes no more than the cap's worth of columns.
    SpectrogramBuilder long_(kRate, (std::int64_t) (kRate * 3600.0));
    std::vector<float> silence(1 << 16, 0.0f);
    const float*       channels[] { silence.data() };
    for (std::int64_t done = 0; done < (std::int64_t) (kRate * 3600.0); done += (std::int64_t) silence.size())
        long_.append(channels, 1, (int) silence.size());
    const auto hour = long_.finish();
    REQUIRE(hour.columns <= SpectrogramBuilder::kMaxColumns);
    REQUIRE(hour.columns > SpectrogramBuilder::kMaxColumns / 2);
    REQUIRE(hour.levelDb(10, 100) == SpectrogramBuilder::kFloorDb);
}

TEST_CASE("A spectrogram's window size and shape are chosen", "[spectrogram]")
{
    constexpr double kRate = 48000.0;

    // Two tones 30 Hz apart: a 2048-point window (23 Hz bins, a Hann main
    // lobe four bins wide) runs them together; 16384 points pulls them apart.
    std::vector<float> mono((size_t) (kRate * 2.0));
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.25f * (float) (std::sin(2.0 * 3.14159265358979 * 1000.0 * (double) i / kRate)
                                   + std::sin(2.0 * 3.14159265358979 * 1030.0 * (double) i / kRate));

    const auto build = [&](SpectrogramSettings settings)
    {
        SpectrogramBuilder builder(kRate, (std::int64_t) mono.size(), settings);
        const float*       channels[] { mono.data() };
        builder.append(channels, 1, (int) mono.size());
        return builder.finish();
    };

    const auto dipBetween = [](const SpectrogramData& data)
    {
        const int column = data.columns / 2;
        const auto at    = [&](double hz) { return data.levelDb(column, (int) std::lround(data.binOfFrequency(hz))); };
        return std::min(at(1000.0), at(1030.0)) - at(1015.0);
    };

    SpectrogramSettings wide;
    wide.fftSize = 16384;
    const auto fine   = build(wide);
    const auto coarse = build({});

    REQUIRE(fine.bins == 16384 / 2 + 1);
    REQUIRE_THAT(fine.windowSeconds, Catch::Matchers::WithinAbs(16384.0 / kRate, 1e-12));
    REQUIRE(dipBetween(fine) > 12.0f);   // two lines
    REQUIRE(dipBetween(coarse) < 6.0f);  // one blur

    // Every shape still reads a full-scale-ish tone near its level: -12 dB
    // for each quarter-scale sine, within the shapes' scalloping.
    for (auto shape : { SpectrogramWindow::Hann, SpectrogramWindow::Hamming, SpectrogramWindow::BlackmanHarris,
                        SpectrogramWindow::Rectangular })
    {
        SpectrogramSettings settings;
        settings.fftSize = 16384;
        settings.window  = shape;
        const auto data  = build(settings);
        const int  column = data.columns / 2;
        float      peak   = -200.0f;
        for (int bin = (int) data.binOfFrequency(990.0); bin <= (int) data.binOfFrequency(1010.0); ++bin)
            peak = std::max(peak, data.levelDb(column, bin));
        INFO((int) shape);
        REQUIRE_THAT(peak, Catch::Matchers::WithinAbs(-12.04, 4.0));
    }

    // Sizes are powers of two, in range.
    REQUIRE(SpectrogramSettings::validFftSize(2048) == 2048);
    REQUIRE(SpectrogramSettings::validFftSize(2800) == 2048);
    REQUIRE(SpectrogramSettings::validFftSize(3100) == 4096);
    REQUIRE(SpectrogramSettings::validFftSize(10) == 256);
    REQUIRE(SpectrogramSettings::validFftSize(1 << 20) == 16384);
}
