#include <catch2/catch_test_macros.hpp>

#include <engine/Spectrum.h>

#include <cmath>
#include <random>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    std::vector<float> makeTone(int samples, double hz, float amplitude)
    {
        std::vector<float> out((size_t) samples);
        for (int n = 0; n < samples; ++n)
            out[(size_t) n] = amplitude * (float) std::sin(2.0 * fft::kPi * hz * n / kSampleRate);
        return out;
    }

    /** A frequency that lands exactly on a bin centre, so the test isn't
        measuring spectral leakage instead of what it means to. */
    double binCentre(int bin, int fftSize) { return (double) bin * kSampleRate / (double) fftSize; }
}

TEST_CASE("A passage too short to analyse gives an empty spectrum", "[engine][spectrum]")
{
    // The honest answer for a short selection: a spectrum built from mostly
    // zero padding would describe the padding.
    REQUIRE(spectrum::analyse(makeTone(100, 440.0, 0.5f), kSampleRate).isEmpty());
    REQUIRE(spectrum::analyse({}, kSampleRate).isEmpty());
    REQUIRE(spectrum::analyse(makeTone(8192, 440.0, 0.5f), 0.0).isEmpty());
}

TEST_CASE("A tone lands at its own frequency", "[engine][spectrum]")
{
    const double hz     = binCentre(64, 2048);
    const auto   result = spectrum::analyse(makeTone(48000, hz, 0.8f), kSampleRate);

    REQUIRE_FALSE(result.isEmpty());
    INFO("expected " << hz << "Hz, dominant " << result.dominantFrequency() << "Hz");
    REQUIRE(std::abs(result.dominantFrequency() - hz) < 30.0);
}

TEST_CASE("A full-scale sine reads about 0dBFS", "[engine][spectrum]")
{
    // The calibration that makes the axis mean something. Without it the
    // numbers drift with the frame size and can't be compared to anything.
    const double hz     = binCentre(100, 2048);
    const auto   result = spectrum::analyse(makeTone(48000, hz, 1.0f), kSampleRate);

    const float peak = result.magnitudeDbAtHz(hz);
    INFO("full-scale sine read " << peak << " dBFS");
    REQUIRE(peak > -2.0f);
    REQUIRE(peak < 1.0f);
}

TEST_CASE("Halving the amplitude drops the level by about 6dB", "[engine][spectrum]")
{
    const double hz = binCentre(100, 2048);

    const auto loud  = spectrum::analyse(makeTone(48000, hz, 1.0f), kSampleRate);
    const auto quiet = spectrum::analyse(makeTone(48000, hz, 0.5f), kSampleRate);

    const float difference = loud.magnitudeDbAtHz(hz) - quiet.magnitudeDbAtHz(hz);
    INFO("difference " << difference << " dB");
    REQUIRE(std::abs(difference - 6.0f) < 1.0f);
}

TEST_CASE("Nothing much shows up away from the tone", "[engine][spectrum]")
{
    // A spectrum that reported energy everywhere would be useless for
    // finding a resonance, which is the whole point of looking at one.
    const double hz     = binCentre(100, 2048);
    const auto   result = spectrum::analyse(makeTone(48000, hz, 1.0f), kSampleRate);

    const float atTone = result.magnitudeDbAtHz(hz);
    const float faraway = result.magnitudeDbAtHz(hz * 4.0);

    INFO("tone " << atTone << " dB, four octaves up " << faraway << " dB");
    REQUIRE(faraway < atTone - 40.0f);
}

TEST_CASE("Silence reads at the floor rather than negative infinity",
          "[engine][spectrum]")
{
    // log(0) is -inf, which would poison any drawing code that scaled by it.
    const std::vector<float> silence(48000, 0.0f);
    const auto               result = spectrum::analyse(silence, kSampleRate);

    REQUIRE_FALSE(result.isEmpty());
    for (float db : result.magnitudesDb)
    {
        REQUIRE(std::isfinite(db));
        REQUIRE(db <= Spectrum::kFloorDb + 0.001f);
    }
}

TEST_CASE("Bin frequencies span zero to Nyquist", "[engine][spectrum]")
{
    const auto result = spectrum::analyse(makeTone(48000, 1000.0, 0.5f), kSampleRate);

    REQUIRE(result.frequencyForBin(0) == 0.0);
    REQUIRE(std::abs(result.frequencyForBin((int) result.magnitudesDb.size() - 1)
                     - kSampleRate * 0.5) < 1.0);
}

TEST_CASE("Querying outside the spectrum returns the floor", "[engine][spectrum]")
{
    // Drawing code sweeps a fixed 20Hz-20kHz axis regardless of sample rate,
    // so it will ask past Nyquist on a low-rate file.
    const auto result = spectrum::analyse(makeTone(48000, 1000.0, 0.5f), kSampleRate);

    REQUIRE(result.magnitudeDbAtHz(-100.0) == Spectrum::kFloorDb);
    REQUIRE(result.magnitudeDbAtHz(1.0e9) == Spectrum::kFloorDb);
    REQUIRE(Spectrum {}.magnitudeDbAtHz(440.0) == Spectrum::kFloorDb);
}

TEST_CASE("Two tones both show up", "[engine][spectrum]")
{
    // Real material has more than one thing in it, and averaging must not
    // smear them into one broad lump.
    const double lowHz  = binCentre(50, 2048);
    const double highHz = binCentre(300, 2048);

    auto mixed = makeTone(48000, lowHz, 0.5f);
    const auto second = makeTone(48000, highHz, 0.5f);
    for (size_t i = 0; i < mixed.size(); ++i)
        mixed[i] += second[i];

    const auto result = spectrum::analyse(mixed, kSampleRate);

    const float atLow  = result.magnitudeDbAtHz(lowHz);
    const float atHigh = result.magnitudeDbAtHz(highHz);
    const float between = result.magnitudeDbAtHz((lowHz + highHz) * 0.5);

    INFO("low " << atLow << ", high " << atHigh << ", between " << between);
    REQUIRE(atLow > between + 25.0f);
    REQUIRE(atHigh > between + 25.0f);
}

TEST_CASE("A noisy signal has no single dominant peak but stays finite",
          "[engine][spectrum]")
{
    std::mt19937                          rng(42);
    std::uniform_real_distribution<float> dist(-0.3f, 0.3f);

    std::vector<float> noise(48000);
    for (auto& sample : noise)
        sample = dist(rng);

    const auto result = spectrum::analyse(noise, kSampleRate);
    REQUIRE_FALSE(result.isEmpty());

    for (float db : result.magnitudesDb)
    {
        REQUIRE(std::isfinite(db));
        REQUIRE(db < 6.0f);
    }
}
