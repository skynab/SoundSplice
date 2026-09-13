#include <catch2/catch_test_macros.hpp>

#include <engine/Exciter.h>
#include <engine/Maximizer.h>
#include <engine/StereoWidener.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    /** Magnitude at one frequency by direct correlation — the same technique
        the chorus and drive tests use, and cheaper than an FFT for a single
        known bin. */
    double magnitudeAtHz(const std::vector<float>& signal, double hz)
    {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < signal.size(); ++n)
        {
            const double phase = 2.0 * 3.14159265358979 * hz * (double) n / kSampleRate;
            re += signal[n] * std::cos(phase);
            im -= signal[n] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) signal.size();
    }
}

// --- Exciter ---------------------------------------------------------------

TEST_CASE("An exciter at zero amount is bit-identical to its input", "[engine][mastering]")
{
    // A parallel effect on a master bus has to be *exactly* nothing when
    // turned down. "Almost unchanged" is what makes an effect untrustworthy
    // to leave inserted.
    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.setAmount(0.0f);

    for (int n = 0; n < 4000; ++n)
    {
        const float in = (float) std::sin(0.05 * n);
        REQUIRE(exciter.processSample(0, in) == in);
    }
}

TEST_CASE("An exciter adds harmonics above the tone it's given", "[engine][mastering]")
{
    // The entire claim of an exciter: energy appears at harmonics that
    // weren't in the input. A tone above the crossover, so it's the band
    // being saturated.
    constexpr double tone = 4000.0;

    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.setCrossoverHz(2000.0f);
    exciter.setAmount(1.0f);

    std::vector<float> out;
    out.reserve(16384);
    for (int n = 0; n < 16384; ++n)
        out.push_back(exciter.processSample(0, 0.5f * (float) std::sin(2.0 * 3.14159265358979 * tone * n / kSampleRate)));

    const std::vector<float> steady(out.begin() + 4096, out.end());
    const double fundamental = magnitudeAtHz(steady, tone);
    const double third       = magnitudeAtHz(steady, tone * 3.0);

    INFO("third harmonic at " << (100.0 * third / fundamental) << "% of the fundamental");
    REQUIRE(fundamental > 1.0e-3);
    REQUIRE(third > fundamental * 0.001);
}

TEST_CASE("An exciter leaves the low band alone", "[engine][mastering]")
{
    // It's meant to be a treble effect. A tone well under the crossover
    // should come through essentially untouched — otherwise it's just a
    // distortion pedal on the master bus.
    constexpr double tone = 100.0;

    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.setCrossoverHz(3000.0f);
    exciter.setAmount(1.0f);

    double worst = 0.0;
    for (int n = 2000; n < 12000; ++n)
    {
        const float in  = 0.5f * (float) std::sin(2.0 * 3.14159265358979 * tone * n / kSampleRate);
        const float out = exciter.processSample(0, in);
        if (n > 4000)
            worst = std::max(worst, (double) std::abs(out - in));
    }

    INFO("largest change to a 100Hz tone: " << worst);
    REQUIRE(worst < 0.05);
}

TEST_CASE("An exciter stays finite and bounded", "[engine][mastering]")
{
    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.setAmount(1.0f);

    for (int n = 0; n < 100000; ++n)
    {
        const float out = exciter.processSample(0, n % 2 == 0 ? 1.0f : -1.0f);
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) < 4.0f);
    }
}

// --- StereoWidener ---------------------------------------------------------

TEST_CASE("A widener at unity width is bit-identical", "[engine][mastering]")
{
    StereoWidener widener;
    widener.setWidth(1.0f);

    for (int n = 0; n < 1000; ++n)
    {
        float l = (float) std::sin(0.03 * n);
        float r = (float) std::cos(0.07 * n);
        const float origL = l, origR = r;
        widener.processFrame(l, r);
        REQUIRE(l == origL);
        REQUIRE(r == origR);
    }
}

TEST_CASE("Zero width collapses to mono", "[engine][mastering]")
{
    StereoWidener widener;
    widener.setWidth(0.0f);

    float l = 0.8f, r = -0.2f;
    widener.processFrame(l, r);
    REQUIRE(std::abs(l - r) < 1.0e-6f);
    REQUIRE(std::abs(l - 0.3f) < 1.0e-6f); // the mid: (0.8 + -0.2) / 2
}

TEST_CASE("Widening actually widens", "[engine][mastering]")
{
    // The side component must grow — otherwise the control is decoration.
    const float l0 = 0.6f, r0 = 0.2f;
    const float side0 = 0.5f * (l0 - r0);

    StereoWidener widener;
    widener.setWidth(1.8f);

    float l = l0, r = r0;
    widener.processFrame(l, r);
    const float side = 0.5f * (l - r);

    REQUIRE(side > side0);
}

TEST_CASE("Widening stays mono-compatible", "[engine][mastering]")
{
    // The failure this guards against is the one that matters: a master that
    // sounds impressive on headphones and hollow on a phone, because summing
    // to mono cancels the sides. The mono sum must hold its level as width
    // rises, not fall away.
    for (float width : { 1.0f, 1.2f, 1.4f, 1.6f, 2.0f })
    {
        StereoWidener widener;
        widener.setWidth(width);

        float l = 0.5f, r = 0.3f;
        const float monoBefore = 0.5f * (l + r);
        widener.processFrame(l, r);
        const float monoAfter = 0.5f * (l + r);

        INFO("width " << width << ": mono " << monoBefore << " -> " << monoAfter);
        REQUIRE(monoAfter >= monoBefore);          // never quieter in mono
        REQUIRE(monoAfter <= monoBefore * 1.6f);   // and not absurdly louder either
        REQUIRE(widener.monoCompatibility() >= 1.0f);
    }
}

// --- Maximizer -------------------------------------------------------------

namespace
{
    /** Runs @p input through a maximizer and returns the output. */
    std::vector<float> throughMaximizer(Maximizer& maximizer, const std::vector<float>& input)
    {
        std::vector<float> out;
        out.reserve(input.size());
        for (float sample : input)
        {
            float frame[2] { sample, sample };
            maximizer.processFrame(frame, 2);
            out.push_back(frame[0]);
        }
        return out;
    }

    float peakOf(const std::vector<float>& signal)
    {
        float peak = 0.0f;
        for (float s : signal)
            peak = std::max(peak, std::abs(s));
        return peak;
    }

    float rmsOf(const std::vector<float>& signal)
    {
        double sum = 0.0;
        for (float s : signal)
            sum += (double) s * (double) s;
        return signal.empty() ? 0.0f : (float) std::sqrt(sum / (double) signal.size());
    }
}

TEST_CASE("A maximizer never exceeds its ceiling", "[engine][mastering]")
{
    // The one guarantee a brickwall makes. Tested against the nastiest
    // inputs available: a full-scale square (every sample a transient) and
    // heavy drive on top of it.
    for (float ceilingDb : { -0.3f, -1.0f, -6.0f })
    {
        for (float driveDb : { 0.0f, 6.0f, 18.0f })
        {
            Maximizer maximizer;
            maximizer.prepare(kSampleRate);
            maximizer.setCeilingDb(ceilingDb);
            maximizer.setInputGainDb(driveDb);
            maximizer.setReleaseMs(50.0f);

            std::vector<float> square;
            square.reserve(20000);
            for (int n = 0; n < 20000; ++n)
                square.push_back(n % 100 < 50 ? 1.0f : -1.0f);

            const auto  out     = throughMaximizer(maximizer, square);
            const float ceiling = dbToLinear(ceilingDb);

            INFO("ceiling " << ceilingDb << "dB, drive " << driveDb << "dB, peak " << peakOf(out));
            REQUIRE(peakOf(out) <= ceiling + 1.0e-5f);
        }
    }
}

TEST_CASE("Lookahead means a transient doesn't overshoot", "[engine][mastering]")
{
    // Without lookahead the leading edge of a sudden peak passes through
    // before the gain has moved — which for a limiter isn't "a bit soft",
    // it's a breach of the ceiling. Silence then instantly full scale is the
    // worst case.
    Maximizer maximizer;
    maximizer.prepare(kSampleRate);
    maximizer.setCeilingDb(-1.0f);
    maximizer.setInputGainDb(0.0f);

    std::vector<float> step(8000, 0.0f);
    for (size_t n = 2000; n < step.size(); ++n)
        step[n] = 1.0f;

    const auto out = throughMaximizer(maximizer, step);
    REQUIRE(peakOf(out) <= dbToLinear(-1.0f) + 1.0e-5f);
    REQUIRE(maximizer.latencySamples() > 0);
}

TEST_CASE("Driving the maximizer raises average level", "[engine][mastering]")
{
    // The whole point: peak pinned, average up. If RMS didn't rise, the
    // "loudness" control would only be making things quieter.
    auto makeTone = []
    {
        std::vector<float> tone;
        tone.reserve(24000);
        for (int n = 0; n < 24000; ++n)
            tone.push_back(0.5f * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * n / kSampleRate));
        return tone;
    };

    Maximizer gentle, driven;
    gentle.prepare(kSampleRate);
    driven.prepare(kSampleRate);
    for (auto* m : { &gentle, &driven })
    {
        m->setCeilingDb(-0.3f);
        m->setReleaseMs(80.0f);
    }
    gentle.setInputGainDb(0.0f);
    driven.setInputGainDb(12.0f);

    const auto quiet = throughMaximizer(gentle, makeTone());
    const auto loud  = throughMaximizer(driven, makeTone());

    INFO("rms " << rmsOf(quiet) << " -> " << rmsOf(loud));
    REQUIRE(rmsOf(loud) > rmsOf(quiet));
    REQUIRE(peakOf(loud) <= dbToLinear(-0.3f) + 1.0e-5f);
}

TEST_CASE("A maximizer below its ceiling is close to transparent", "[engine][mastering]")
{
    // Nothing to limit means nothing should happen beyond the lookahead
    // delay — a limiter that ducked quiet material would be a compressor
    // nobody asked for.
    Maximizer maximizer;
    maximizer.prepare(kSampleRate);
    maximizer.setCeilingDb(-0.3f);
    maximizer.setInputGainDb(0.0f);

    std::vector<float> quiet;
    quiet.reserve(12000);
    for (int n = 0; n < 12000; ++n)
        quiet.push_back(0.05f * (float) std::sin(0.02 * n));

    const auto out = throughMaximizer(maximizer, quiet);

    REQUIRE(std::abs(maximizer.currentReductionDb()) < 0.01f);
    REQUIRE(std::abs(rmsOf(out) - rmsOf(quiet)) < 0.005f);
}

TEST_CASE("A maximizer reports the reduction it's applying", "[engine][mastering]")
{
    Maximizer maximizer;
    maximizer.prepare(kSampleRate);
    maximizer.setCeilingDb(-6.0f);
    maximizer.setInputGainDb(12.0f);

    std::vector<float> loud(8000, 0.9f);
    throughMaximizer(maximizer, loud);

    INFO("reduction " << maximizer.currentReductionDb() << " dB");
    REQUIRE(maximizer.currentReductionDb() < -1.0f);
}

TEST_CASE("A maximizer stays finite on silence and on nonsense", "[engine][mastering]")
{
    // log(0) is -inf; the detector floor is what stops that reaching the
    // gain, and without it the first silent sample poisons everything after.
    Maximizer maximizer;
    maximizer.prepare(kSampleRate);
    maximizer.setInputGainDb(24.0f);

    std::vector<float> input(8000, 0.0f);
    for (size_t n = 4000; n < input.size(); ++n)
        input[n] = (n % 2 == 0) ? 8.0f : -8.0f; // well past full scale

    for (float sample : throughMaximizer(maximizer, input))
        REQUIRE(std::isfinite(sample));
}
