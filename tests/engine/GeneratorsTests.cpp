#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Generators.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    std::vector<float> renderAll(const GeneratorSpec& spec, int chunk = 4096)
    {
        Generator          generator(spec, kRate);
        std::vector<float> out((size_t) generator.totalFrames());
        for (size_t at = 0; at < out.size(); at += (size_t) chunk)
            generator.render(out.data() + at, (int) std::min<size_t>((size_t) chunk, out.size() - at));
        return out;
    }

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt(sum / (double) (to - from));
    }

    /** Upward zero crossings in [from, to), a frequency estimate's raw count. */
    int crossings(const std::vector<float>& x, size_t from, size_t to)
    {
        int count = 0;
        for (size_t i = from + 1; i < to; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                ++count;
        return count;
    }

    /** The level of @p hz in [from, to) (Goertzel), as the amplitude of a sine there. */
    double levelAt(const std::vector<float>& x, size_t from, size_t to, double hz)
    {
        const double coeff = 2.0 * std::cos(2.0 * kPi * hz / kRate);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            const double s0 = x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt(std::max(0.0, power)) / (double) (to - from);
    }
}

TEST_CASE("A tone is the frequency, level and length asked for", "[engine][generators]")
{
    GeneratorSpec spec;
    spec.kind           = GeneratorKind::Tone;
    spec.startHz        = 1000.0;
    spec.startAmplitude = 0.5;
    spec.seconds        = 2.0;

    const auto tone = renderAll(spec);
    REQUIRE(tone.size() == 96000);
    REQUIRE_THAT(rms(tone, 0, tone.size()), WithinAbs(0.5 / std::sqrt(2.0), 1e-3));
    REQUIRE(std::abs(crossings(tone, 0, 48000) - 1000) <= 1);

    // Square: a PolyBLEP square at 0.5 sits at +/-0.5 away from its edges.
    spec.waveform     = Oscillator::Waveform::Square;
    const auto square = renderAll(spec);
    REQUIRE_THAT(rms(square, 0, square.size()), WithinAbs(0.5, 0.02));
}

TEST_CASE("Rendering in chunks gives what rendering at once does", "[engine][generators]")
{
    for (auto kind : { GeneratorKind::Chirp, GeneratorKind::Noise, GeneratorKind::Dtmf })
    {
        GeneratorSpec spec;
        spec.kind    = kind;
        spec.noise   = NoiseColour::Pink;
        spec.seconds = 1.3;

        REQUIRE(renderAll(spec, 777) == renderAll(spec, 65536));
    }
}

TEST_CASE("A chirp sweeps between its frequencies and levels", "[engine][generators]")
{
    GeneratorSpec spec;
    spec.kind           = GeneratorKind::Chirp;
    spec.startHz        = 200.0;
    spec.endHz          = 2000.0;
    spec.startAmplitude = 0.9;
    spec.endAmplitude   = 0.1;
    spec.seconds        = 10.0;

    const auto linear = renderAll(spec);
    const auto tenth  = (size_t) (kRate * 0.1);
    // Over the first and last 100 ms: ~200 Hz and ~2000 Hz.
    REQUIRE(std::abs(crossings(linear, 0, tenth) - 20) <= 2);
    REQUIRE(std::abs(crossings(linear, linear.size() - tenth, linear.size()) - 200) <= 3);
    REQUIRE(rms(linear, 0, tenth) > 0.6);
    REQUIRE(rms(linear, linear.size() - tenth, linear.size()) < 0.1);

    // Logarithmic: halfway through time is halfway in octaves, sqrt(200 * 2000) = 632 Hz.
    spec.logarithmic = true;
    const auto log   = renderAll(spec);
    const auto mid   = log.size() / 2;
    REQUIRE(std::abs(crossings(log, mid - tenth / 2, mid + tenth / 2) - 63) <= 3);
}

TEST_CASE("Noise colours tilt the spectrum as their names say", "[engine][generators]")
{
    GeneratorSpec spec;
    spec.kind           = GeneratorKind::Noise;
    spec.startAmplitude = 1.0;
    spec.seconds        = 4.0;

    const auto hfShare = [&](NoiseColour colour)
    {
        spec.noise       = colour;
        const auto noise = renderAll(spec);
        // The first difference emphasises high frequencies: its level
        // against the signal's says how much of the energy is up there.
        std::vector<float> diff(noise.size() - 1);
        for (size_t i = 1; i < noise.size(); ++i)
            diff[i - 1] = noise[i] - noise[i - 1];

        for (float sample : noise)
            REQUIRE(std::abs(sample) <= 1.0f);
        return rms(diff, 0, diff.size()) / rms(noise, 0, noise.size());
    };

    const double white = hfShare(NoiseColour::White);
    const double pink  = hfShare(NoiseColour::Pink);
    const double brown = hfShare(NoiseColour::Brown);

    REQUIRE_THAT(white, WithinAbs(std::sqrt(2.0), 0.05)); // uncorrelated samples
    REQUIRE(pink < white * 0.8);
    REQUIRE(brown < pink * 0.5);

    spec.noise = NoiseColour::White;
    REQUIRE_THAT(rms(renderAll(spec), 0, 192000), WithinAbs(1.0 / std::sqrt(3.0), 0.01)); // uniform in [-1, 1)
}

TEST_CASE("Silence is silent", "[engine][generators]")
{
    GeneratorSpec spec;
    spec.kind    = GeneratorKind::Silence;
    spec.seconds = 0.5;

    const auto silence = renderAll(spec);
    REQUIRE(silence.size() == 24000);
    for (float sample : silence)
        REQUIRE(sample == 0.0f);
}

TEST_CASE("DTMF plays each key's two tones, with gaps between them", "[engine][generators]")
{
    REQUIRE(Generator::dtmfKeys("1-2 x*#d") == "12*#D");
    REQUIRE(Generator::dtmfFrequencies('5') == std::pair { 770.0, 1336.0 });

    GeneratorSpec spec;
    spec.kind           = GeneratorKind::Dtmf;
    spec.dtmf           = "19";
    spec.dtmfDuty       = 0.5;
    spec.startAmplitude = 1.0;
    spec.seconds        = 0.3; // two keys, one gap: 100 ms each

    const auto tones = renderAll(spec);
    const auto slot  = (size_t) (kRate * 0.1);

    // Key 1: 697 + 1209 Hz, each at half the amplitude.
    REQUIRE_THAT(levelAt(tones, slot / 10, slot * 9 / 10, 697.0), WithinAbs(0.5, 0.05));
    REQUIRE_THAT(levelAt(tones, slot / 10, slot * 9 / 10, 1209.0), WithinAbs(0.5, 0.05));
    REQUIRE(levelAt(tones, slot / 10, slot * 9 / 10, 852.0) < 0.05);

    // The gap.
    REQUIRE(rms(tones, slot + 10, 2 * slot - 10) == 0.0);

    // Key 9: 852 + 1477 Hz.
    REQUIRE_THAT(levelAt(tones, 2 * slot + slot / 10, 2 * slot + slot * 9 / 10, 852.0), WithinAbs(0.5, 0.05));
    REQUIRE_THAT(levelAt(tones, 2 * slot + slot / 10, 2 * slot + slot * 9 / 10, 1477.0), WithinAbs(0.5, 0.05));

    // Faded in: no click at the very first sample.
    REQUIRE(std::abs(tones[0]) < 1.0e-6f);
}
