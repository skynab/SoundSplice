#include <catch2/catch_test_macros.hpp>

#include <engine/Oversampler.h>
#include <engine/Waveshaper.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
constexpr double kPi = 3.14159265358979323846;

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

TEST_CASE("Oversampling passes a clean signal through essentially unchanged", "[engine][drive]")
{
    // Before anything is claimed about aliasing: wrapping an identity in the
    // rate converter must give back the identity, or every later measurement
    // is measuring the converter rather than the shaper.
    constexpr double sampleRate = 48000.0;
    constexpr double tone       = 1000.0;

    Oversampler4x      os;
    std::vector<float> in, out;
    for (int n = 0; n < 16384; ++n)
    {
        const float x = 0.5f * (float) std::sin(2.0 * kPi * tone * n / sampleRate);
        in.push_back(x);
        out.push_back(os.process(x, [](float v) { return v; }));
    }

    // Compared over the settled tail: the FIR has a fixed group delay, which
    // shifts phase but not magnitude, so magnitude is what this asserts.
    const std::vector<float> steadyIn(in.begin() + 4096, in.end());
    const std::vector<float> steadyOut(out.begin() + 4096, out.end());

    const double before = magnitudeAt(steadyIn, tone, sampleRate);
    const double after  = magnitudeAt(steadyOut, tone, sampleRate);

    INFO("in " << before << " out " << after);
    REQUIRE(after > before * 0.98);
    REQUIRE(after < before * 1.02);
}

TEST_CASE("Oversampling reduces the aliasing of a hard-driven shaper", "[engine][drive]")
{
    // The claim the feature exists for. A 5kHz tone into a lot of gain puts
    // its 5th harmonic at 25kHz, which folds back to 23kHz at 48k - and
    // successive odd harmonics fold to bins all over the audible band.
    constexpr double sampleRate = 48000.0;
    constexpr double tone       = 5000.0;

    // 7th harmonic of 5kHz is 35kHz, folding to 48k - 35k = 13kHz. Nothing
    // the tone itself produces lands there, so energy in that bin is alias.
    constexpr double aliasBin = 13000.0;

    auto run = [](bool oversampled)
    {
        Waveshaper    shaper;
        Oversampler4x os;
        shaper.setDrive(25.0f);
        shaper.setKind(Waveshaper::Kind::Hard);
        shaper.reset();
        os.reset();

        std::vector<float> out;
        out.reserve(16384);
        for (int n = 0; n < 16384; ++n)
        {
            const float x = 0.9f * (float) std::sin(2.0 * kPi * tone * n / sampleRate);
            out.push_back(oversampled ? os.process(x, [&shaper](float v) { return shaper.processSample(v); })
                                      : shaper.processSample(x));
        }
        return out;
    };

    const double plain = magnitudeAt(run(false), aliasBin, sampleRate);
    const double over  = magnitudeAt(run(true), aliasBin, sampleRate);

    INFO("alias at " << aliasBin << "Hz: plain " << plain << " oversampled " << over);

    // Measured at roughly a tenth. The bound is set well above that so this
    // fails if oversampling is bypassed without chasing the exact figure.
    REQUIRE(over < plain * 0.5);
}

TEST_CASE("Oversampling stays stable and settles", "[engine][drive]")
{
    Oversampler4x os;
    Waveshaper    shaper;
    shaper.setDrive(50.0f);

    for (int n = 0; n < 100000; ++n)
    {
        const float y = os.process(n % 2 == 0 ? 1.0f : -1.0f,
                                   [&shaper](float v) { return shaper.processSample(v); });
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 10.0f);
    }
}
