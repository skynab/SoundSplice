#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/Vocoder.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    double levelAt(const std::vector<float>& audio, double hz, size_t from, size_t to)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            re += audio[i] * std::cos(2.0 * kPi * hz * (double) i / kRate);
            im += audio[i] * std::sin(2.0 * kPi * hz * (double) i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (double) (to - from);
    }

    double rms(const std::vector<float>& audio, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) audio[i] * audio[i];
        return std::sqrt(sum / (double) (to - from));
    }
}

TEST_CASE("The vocoder puts the carrier where the voice is, and nothing where it's silent", "[engine][vocoder]")
{
    Vocoder vocoder;
    vocoder.prepare(kRate);
    vocoder.setCarrier(Vocoder::Carrier::Sawtooth);
    vocoder.setPitchHz(110.0);

    // A "voice" with energy only around 1 kHz, for a second, then silence.
    std::vector<float> out((size_t) kRate * 2);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float voice = i < (size_t) kRate ? 0.5f * (float) std::sin(2.0 * kPi * 1000.0 * (double) i / kRate) : 0.0f;
        out[i]            = vocoder.process(voice, 0.0f);
    }

    // The saw's harmonics near the voice (990 Hz) come through; far ones don't.
    const double near = levelAt(out, 990.0, 12000, 48000);
    const double far  = levelAt(out, 4950.0, 12000, 48000);
    REQUIRE(near > far * 10.0);
    REQUIRE(near > 0.01);

    // Once the voice stops, so does the output, within the response time.
    REQUIRE(rms(out, 60000, 96000) < 1.0e-3);
}

TEST_CASE("The vocoder follows its modulator from the right channel too, at a sensible level", "[engine][vocoder]")
{
    std::mt19937                          random(9);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);

    for (int bands : { 8, 16, 32 })
    {
        Vocoder vocoder;
        vocoder.prepare(kRate);
        vocoder.setBands(bands);
        vocoder.setCarrier(Vocoder::Carrier::RightChannel);

        // Noise speaking through noise: the output is as loud as the part of
        // the voice the bands cover, whatever their number. 80 Hz to 10 kHz
        // holds 41% of white noise's power at this rate: -3.9 dB.
        std::vector<float> voice((size_t) kRate), out((size_t) kRate);
        for (size_t i = 0; i < out.size(); ++i)
        {
            voice[i] = 0.3f * uniform(random);
            out[i]   = vocoder.process(voice[i], 0.3f * uniform(random));
        }
        const double ratioDb = 20.0 * std::log10(rms(out, 12000, out.size()) / rms(voice, 12000, voice.size()));
        INFO(bands << " bands: " << ratioDb << " dB");
        const double covered = 10.0 * std::log10((Vocoder::kHighestHz - Vocoder::kLowestHz) / (kRate * 0.5));
        REQUIRE_THAT(ratioDb, WithinAbs(covered, 1.5));
    }
}
