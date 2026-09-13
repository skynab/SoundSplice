#include <catch2/catch_test_macros.hpp>

#include <engine/Oscillator.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 44100.0;

    std::vector<float> render(Oscillator::Waveform waveform, double frequency, int numSamples)
    {
        const double increment = frequency / kSampleRate;
        std::vector<float> out((size_t) numSamples);

        double phase = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            out[(size_t) i] = Oscillator::sample(waveform, phase, increment);
            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;
        }
        return out;
    }

    /** Magnitude of one frequency in a signal, by direct correlation — a
        single DFT bin, which is all these tests need and avoids pulling in an
        FFT. */
    double magnitudeAt(const std::vector<float>& signal, double frequency)
    {
        double real = 0.0, imaginary = 0.0;
        for (size_t i = 0; i < signal.size(); ++i)
        {
            const double angle = 2.0 * M_PI * frequency * (double) i / kSampleRate;
            real      += signal[i] * std::cos(angle);
            imaginary += signal[i] * std::sin(angle);
        }
        return std::sqrt(real * real + imaginary * imaginary) / (double) signal.size();
    }
}

TEST_CASE("PolyBLEP is zero away from the discontinuity", "[engine][oscillator]")
{
    // The correction must not touch the middle of the cycle, or it would
    // distort the waveform rather than just band-limiting its edge.
    REQUIRE(Oscillator::polyBlep(0.5, 0.01) == 0.0);
    REQUIRE(Oscillator::polyBlep(0.25, 0.01) == 0.0);
    REQUIRE(Oscillator::polyBlep(0.75, 0.01) == 0.0);
}

TEST_CASE("A saw stays within range", "[engine][oscillator]")
{
    // PolyBLEP overshoots slightly by design; it must not run away.
    const auto saw = render(Oscillator::Waveform::Saw, 440.0, 4410);
    for (float sample : saw)
        REQUIRE(std::abs(sample) < 1.5f);
}

TEST_CASE("A band-limited saw suppresses aliased partials", "[engine][oscillator]")
{
    // At 5kHz on a 44.1kHz rate the 9th harmonic (45kHz) folds back to
    // 45000 - 44100 = 900Hz, which is not a harmonic of 5kHz and so has no
    // business being there. It's a clean, unambiguous aliasing probe.
    constexpr double fundamental = 5000.0;
    constexpr double aliasedBin  = 900.0;

    const auto blepped = render(Oscillator::Waveform::Saw, fundamental, 44100);

    // A naive ramp, generated the way this synth did before PolyBLEP, as the
    // reference for "how bad it was".
    std::vector<float> naive(44100);
    {
        double phase = 0.0;
        const double increment = fundamental / kSampleRate;
        for (size_t i = 0; i < naive.size(); ++i)
        {
            naive[i] = (float) (2.0 * phase - 1.0);
            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;
        }
    }

    const double naiveAlias   = magnitudeAt(naive, aliasedBin);
    const double bleppedAlias = magnitudeAt(blepped, aliasedBin);

    // Measured: naive 0.0354, PolyBLEP 0.00001 — about 68 dB down. The
    // threshold is set far tighter than "some improvement" so that a
    // regression which merely blunts the correction still fails here.
    REQUIRE(bleppedAlias < naiveAlias * 0.02);
}

TEST_CASE("Band-limiting keeps the fundamental intact", "[engine][oscillator]")
{
    // Suppressing aliasing is only worth anything if the note itself survives
    // — a low-pass would also pass the test above.
    constexpr double fundamental = 5000.0;

    const auto blepped = render(Oscillator::Waveform::Saw, fundamental, 44100);
    const double level = magnitudeAt(blepped, fundamental);

    // Measured 0.305 against an ideal saw's 2/pi ~= 0.318 — only 0.4 dB
    // of the note itself is lost, which is what separates band-limiting
    // from simply rolling the top end off.
    REQUIRE(level > 0.29);
}

TEST_CASE("A square is band-limited at both of its edges", "[engine][oscillator]")
{
    // A square steps twice per cycle, so a correction applied at only one of
    // them would leave half the aliasing behind.
    constexpr double fundamental = 5000.0;
    constexpr double aliasedBin  = 900.0;

    const auto blepped = render(Oscillator::Waveform::Square, fundamental, 44100);

    std::vector<float> naive(44100);
    {
        double phase = 0.0;
        const double increment = fundamental / kSampleRate;
        for (size_t i = 0; i < naive.size(); ++i)
        {
            naive[i] = phase < 0.5 ? 1.0f : -1.0f;
            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;
        }
    }

    REQUIRE(magnitudeAt(blepped, aliasedBin) < magnitudeAt(naive, aliasedBin) * 0.05);
}

TEST_CASE("A sine is unchanged by the anti-aliasing work", "[engine][oscillator]")
{
    // The default waveform must be bit-identical: it has no harmonics to
    // alias, and every existing project's sound depends on it.
    const double increment = 440.0 / kSampleRate;
    for (double phase = 0.0; phase < 1.0; phase += 0.01)
    {
        const float expected = (float) std::sin(6.283185307179586 * phase);
        REQUIRE(Oscillator::sample(Oscillator::Waveform::Sine, phase, increment) == expected);
    }
}
