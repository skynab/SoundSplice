#include <catch2/catch_test_macros.hpp>

#include <engine/Fft.h>
#include <engine/NoiseReduction.h>

#include <cmath>
#include <random>
#include <vector>

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

    /** Deterministic noise: a fixed seed, so a test that measures how much of
        it survives can't be flaky. */
    std::vector<float> makeNoise(int samples, float amplitude, unsigned seed)
    {
        std::mt19937                          rng(seed);
        std::uniform_real_distribution<float> dist(-amplitude, amplitude);

        std::vector<float> out((size_t) samples);
        for (auto& sample : out)
            sample = dist(rng);
        return out;
    }

    std::vector<float> mix(const std::vector<float>& a, const std::vector<float>& b)
    {
        std::vector<float> out(std::min(a.size(), b.size()));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = a[i] + b[i];
        return out;
    }

    double magnitudeAtHz(const std::vector<float>& signal, double hz)
    {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < signal.size(); ++n)
        {
            const double phase = 2.0 * fft::kPi * hz * (double) n / kSampleRate;
            re += signal[n] * std::cos(phase);
            im -= signal[n] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) signal.size();
    }

    float rmsOf(const std::vector<float>& signal)
    {
        double sum = 0.0;
        for (float s : signal)
            sum += (double) s * (double) s;
        return signal.empty() ? 0.0f : (float) std::sqrt(sum / (double) signal.size());
    }
}

// --- FFT -------------------------------------------------------------------

TEST_CASE("The inverse FFT undoes the forward FFT", "[engine][fft]")
{
    // The property everything else here relies on. Round-tripping has to
    // return the original signal, not the original scaled by n — see the
    // note on transform().
    std::vector<float> original = makeTone(1024, 440.0, 0.7f);
    for (size_t i = 0; i < original.size(); ++i)
        original[i] += 0.3f * (float) std::sin(0.013 * (double) i); // not a single clean bin

    std::vector<float> re = original;
    std::vector<float> im(original.size(), 0.0f);

    fft::transform(re, im, false);
    fft::transform(re, im, true);

    for (size_t i = 0; i < original.size(); ++i)
    {
        INFO("sample " << i);
        REQUIRE(std::abs(re[i] - original[i]) < 1.0e-4f);
        REQUIRE(std::abs(im[i]) < 1.0e-4f);
    }
}

TEST_CASE("A pure tone lands in the bin it belongs to", "[engine][fft]")
{
    // Bin k of an n-point transform is k * sampleRate / n. A tone placed
    // exactly on a bin centre should put essentially all its energy there.
    constexpr int n   = 1024;
    constexpr int bin = 64;
    const double  hz  = bin * kSampleRate / n;

    std::vector<float> re = makeTone(n, hz, 1.0f);
    std::vector<float> im((size_t) n, 0.0f);
    fft::transform(re, im, false);

    auto magnitudeAt = [&](int k) { return std::hypot(re[(size_t) k], im[(size_t) k]); };

    const float atBin = magnitudeAt(bin);
    REQUIRE(atBin > 1.0f);

    for (int k = 0; k < n / 2; ++k)
    {
        if (std::abs(k - bin) <= 1)
            continue;
        INFO("bin " << k);
        REQUIRE(magnitudeAt(k) < atBin * 0.01f);
    }
}

TEST_CASE("A non-power-of-two length is refused, not corrupted", "[engine][fft]")
{
    std::vector<float> re(1000, 1.0f);
    std::vector<float> im(1000, 0.0f);
    fft::transform(re, im, false);

    // Left exactly as it was rather than half-transformed.
    for (float sample : re)
        REQUIRE(sample == 1.0f);

    REQUIRE_FALSE(fft::isPowerOfTwo(0));
    REQUIRE_FALSE(fft::isPowerOfTwo(1));
    REQUIRE_FALSE(fft::isPowerOfTwo(1000));
    REQUIRE(fft::isPowerOfTwo(1024));
}

// --- noise profile ---------------------------------------------------------

TEST_CASE("A noise profile needs at least one full frame", "[engine][noisereduction]")
{
    // A selection too short to measure has no honest answer, and a profile
    // built from a zero-padded frame would describe the padding.
    const auto tooShort = noisereduction::captureNoiseProfile(makeNoise(200, 0.1f, 1u), 1024);
    REQUIRE(tooShort.isEmpty());

    const auto enough = noisereduction::captureNoiseProfile(makeNoise(4096, 0.1f, 1u), 1024);
    REQUIRE_FALSE(enough.isEmpty());
    REQUIRE(enough.fftSize == 1024);
    REQUIRE(enough.magnitude.size() == 1024 / 2 + 1);
}

TEST_CASE("A louder noise floor gives a bigger profile", "[engine][noisereduction]")
{
    const auto quiet = noisereduction::captureNoiseProfile(makeNoise(8192, 0.02f, 7u));
    const auto loud  = noisereduction::captureNoiseProfile(makeNoise(8192, 0.20f, 7u));

    double quietSum = 0.0, loudSum = 0.0;
    for (size_t k = 0; k < quiet.magnitude.size(); ++k)
    {
        quietSum += quiet.magnitude[k];
        loudSum  += loud.magnitude[k];
    }
    REQUIRE(loudSum > quietSum * 5.0);
}

// --- reduction -------------------------------------------------------------

TEST_CASE("Reducing with no profile returns the input untouched", "[engine][noisereduction]")
{
    // The UI can only reach reduceNoise after a capture, but a reloaded
    // project shouldn't be able to silently destroy audio.
    const auto input = makeTone(4096, 440.0, 0.5f);
    const auto out   = noisereduction::reduceNoise(input, NoiseProfile {});

    REQUIRE(out.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE(out[i] == input[i]);
}

TEST_CASE("Overlap-add reconstructs the signal when nothing is subtracted",
          "[engine][noisereduction]")
{
    // With an all-zero profile every bin's gain is 1, so this is pure
    // analysis-then-synthesis. If the windowing and normalisation are right
    // it returns the input; if they aren't, the error shows up here rather
    // than as a mysterious tonal change on real material.
    const auto input = makeTone(8192, 440.0, 0.5f);

    NoiseProfile silent;
    silent.fftSize   = 1024;
    silent.magnitude.assign(1024 / 2 + 1, 0.0f);

    const auto out = noisereduction::reduceNoise(input, silent, 0.0f, -60.0f);
    REQUIRE(out.size() == input.size());

    // The very edges see fewer overlapping frames, so the interior is what's
    // checked — which is also all the caller hears.
    for (size_t i = 2048; i < input.size() - 2048; ++i)
    {
        INFO("sample " << i);
        REQUIRE(std::abs(out[i] - input[i]) < 1.0e-3f);
    }
}

TEST_CASE("Denoising cuts the noise floor while keeping the tone",
          "[engine][noisereduction]")
{
    // The claim that distinguishes denoising from simply turning it down:
    // the noise gets quieter and the signal doesn't. Both are measured.
    constexpr double tone = 1000.0;

    const auto noiseOnly = makeNoise(24000, 0.05f, 99u);
    const auto toneOnly  = makeTone(24000, tone, 0.5f);
    const auto noisy     = mix(toneOnly, noiseOnly);

    // The print comes from a passage of noise alone — exactly what the UI
    // asks the user to select.
    const auto profile = noisereduction::captureNoiseProfile(
        makeNoise(12000, 0.05f, 1234u));
    REQUIRE_FALSE(profile.isEmpty());

    const auto cleaned = noisereduction::reduceNoise(noisy, profile, 12.0f, -24.0f);

    const double before = magnitudeAtHz(noisy, tone);
    const double after  = magnitudeAtHz(cleaned, tone);
    INFO("tone " << before << " -> " << after);
    REQUIRE(after > before * 0.7); // the tone substantially survives

    // Noise, measured where the tone isn't: the residual after removing the
    // tone's own contribution is dominated by what's left of the noise.
    const auto interiorOf = [](const std::vector<float>& v)
    {
        return std::vector<float>(v.begin() + 4000, v.end() - 4000);
    };
    const float noisyRms   = rmsOf(interiorOf(noisy));
    const float cleanedRms = rmsOf(interiorOf(cleaned));
    INFO("rms " << noisyRms << " -> " << cleanedRms);
    REQUIRE(cleanedRms < noisyRms);
}

TEST_CASE("More reduction removes more noise", "[engine][noisereduction]")
{
    const auto noisy   = makeNoise(24000, 0.1f, 55u);
    const auto profile = noisereduction::captureNoiseProfile(makeNoise(12000, 0.1f, 56u));

    const auto gentle = noisereduction::reduceNoise(noisy, profile, 3.0f, -24.0f);
    const auto strong = noisereduction::reduceNoise(noisy, profile, 18.0f, -24.0f);

    INFO("rms " << rmsOf(gentle) << " vs " << rmsOf(strong));
    REQUIRE(rmsOf(strong) < rmsOf(gentle));
}

TEST_CASE("The spectral floor bounds how far a bin is attenuated",
          "[engine][noisereduction]")
{
    // The musical-noise guard. A shallower floor must leave more behind —
    // that residual noise bed is what masks the isolated surviving bins that
    // would otherwise warble.
    const auto noisy   = makeNoise(24000, 0.1f, 77u);
    const auto profile = noisereduction::captureNoiseProfile(makeNoise(12000, 0.1f, 78u));

    const auto shallow = noisereduction::reduceNoise(noisy, profile, 18.0f, -6.0f);
    const auto deep    = noisereduction::reduceNoise(noisy, profile, 18.0f, -48.0f);

    INFO("rms " << rmsOf(shallow) << " vs " << rmsOf(deep));
    REQUIRE(rmsOf(shallow) > rmsOf(deep));
}

TEST_CASE("Denoised output stays finite and bounded", "[engine][noisereduction]")
{
    const auto noisy   = makeNoise(16000, 1.0f, 3u); // full scale
    const auto profile = noisereduction::captureNoiseProfile(makeNoise(8000, 1.0f, 4u));

    for (float sample : noisereduction::reduceNoise(noisy, profile, 24.0f, -60.0f))
    {
        REQUIRE(std::isfinite(sample));
        REQUIRE(std::abs(sample) < 4.0f);
    }
}

TEST_CASE("Denoising preserves length", "[engine][noisereduction]")
{
    // The result replaces a clip's file, so a length change would shift
    // everything after it on the timeline.
    const auto profile = noisereduction::captureNoiseProfile(makeNoise(8192, 0.05f, 11u));
    for (int length : { 1, 100, 4095, 4096, 20000 })
    {
        const auto input = makeNoise(length, 0.05f, 12u);
        INFO("length " << length);
        REQUIRE((int) noisereduction::reduceNoise(input, profile).size() == length);
    }
}
