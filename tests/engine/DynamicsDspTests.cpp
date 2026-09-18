#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/DynamicsDsp.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979323846;

    float sine(double hz, int n, double amplitude)
    {
        return (float) (amplitude * std::sin(2.0 * kPi * hz * n / kRate));
    }

    /** The level in dB of what @p process makes of a @p hz sine at @p amplitude,
        over its second half second (after one and a half of settling). */
    template <typename Process>
    double settledLevelDb(double hz, double amplitude, Process&& process)
    {
        double sum = 0.0, input = 0.0;
        int    count = 0;
        for (int n = 0; n < (int) (kRate * 2.0); ++n)
        {
            const float x = sine(hz, n, amplitude);
            const float y = process(x);
            if (n >= (int) (kRate * 1.5))
            {
                sum   += (double) y * y;
                input += (double) x * x;
                ++count;
            }
        }
        return 10.0 * std::log10(sum / input);
    }
}

TEST_CASE("The graphic EQ raises and cuts its own octave", "[engine][dynamics]")
{
    GraphicEq eq;
    eq.prepare(kRate);
    eq.setGainDb(5, 6.0f);   // 1 kHz
    eq.setGainDb(9, -9.0f);  // 16 kHz

    REQUIRE_THAT(eq.magnitudeDbAt(1000.0f), WithinAbs(6.0, 0.2));
    REQUIRE_THAT(eq.magnitudeDbAt(16000.0f), WithinAbs(-9.0, 0.5));
    REQUIRE_THAT(eq.magnitudeDbAt(100.0f), WithinAbs(0.0, 0.3));

    REQUIRE_THAT(settledLevelDb(1000.0, 0.3, [&](float x) { return eq.processSample(x); }), WithinAbs(6.0, 0.2));

    // Flat, it leaves the audio exactly alone.
    GraphicEq flat;
    flat.prepare(kRate);
    for (int n = 0; n < 1000; ++n)
        REQUIRE(flat.processSample(sine(440.0, n, 0.5)) == sine(440.0, n, 0.5));
}

TEST_CASE("The de-esser turns down loud highs and leaves the rest", "[engine][dynamics]")
{
    const auto deEssed = [](double hz, double amplitude)
    {
        DeEsser deEsser;
        deEsser.prepare(kRate);
        deEsser.setFrequencyHz(5000.0f);
        deEsser.setThresholdDb(-30.0f);
        deEsser.setMaxReductionDb(12.0f);
        return settledLevelDb(hz, amplitude, [&](float x)
        {
            float frame[1] { x };
            deEsser.processFrame(frame, 1);
            return frame[0];
        });
    };

    // Well over: close to the full reduction, less what the crossover's slope
    // leaves of a 9 kHz tone in the band below it.
    REQUIRE_THAT(deEssed(9000.0, 0.5), WithinAbs(-12.0, 2.0));
    REQUIRE_THAT(deEssed(200.0, 0.5), WithinAbs(0.0, 0.1));    // below the split: untouched
    REQUIRE_THAT(deEssed(9000.0, 0.01), WithinAbs(0.0, 0.2));  // under the threshold: untouched
}

TEST_CASE("The expander pushes quiet audio further down and leaves loud audio alone", "[engine][dynamics]")
{
    const auto expanded = [](double amplitude)
    {
        Expander expander;
        expander.prepare(kRate);
        expander.setThresholdDb(-40.0f);
        expander.setRatio(2.0f);
        expander.setRangeDb(40.0f);
        return settledLevelDb(300.0, amplitude, [&](float x)
        {
            float frame[1] { x };
            expander.processFrame(frame, 1);
            return frame[0];
        });
    };

    REQUIRE_THAT(expanded(0.3), WithinAbs(0.0, 0.2));
    // A -50 dB peak is 10 dB under: at 2:1, 10 dB more.
    REQUIRE_THAT(expanded(std::pow(10.0, -50.0 / 20.0)), WithinAbs(-10.0, 1.0));
    // Far under: held to the range.
    REQUIRE_THAT(expanded(std::pow(10.0, -100.0 / 20.0)), WithinAbs(-40.0, 1.0));
}

TEST_CASE("Ring modulation multiplies by the carrier", "[engine][dynamics]")
{
    RingModulator ring;
    ring.prepare(kRate);
    ring.setFrequencyHz(1000.0f);
    ring.setMix(1.0f);

    for (int n = 0; n < 480; ++n)
    {
        const float carrier = ring.nextCarrier();
        REQUIRE_THAT(carrier, WithinAbs(std::sin(2.0 * kPi * 1000.0 * n / kRate), 1e-4));
        REQUIRE_THAT(ring.apply(0.5f, carrier), WithinAbs(0.5 * carrier, 1e-6));
    }

    ring.setMix(0.0f);
    REQUIRE(ring.apply(0.25f, ring.nextCarrier()) == 0.25f);
}

TEST_CASE("The wah passes its band and cuts far from it", "[engine][dynamics]")
{
    const auto held = [](double hz)
    {
        Wah wah;
        wah.prepare(kRate);
        wah.setDepth(0.0f); // held at 350 Hz
        wah.setResonance(4.0f);
        wah.setMix(1.0f);
        return settledLevelDb(hz, 0.5, [&](float x) { return wah.processSample(x); });
    };

    REQUIRE_THAT(held(350.0), WithinAbs(0.0, 0.3));
    REQUIRE(held(5000.0) < -20.0);
}

TEST_CASE("The multiband compressor squashes the band it's set for and leaves the others", "[engine][dynamics]")
{
    // A loud bass tone and a quiet high one, together.
    const auto level = [](double hz, bool compressBass)
    {
        MultibandCompressor multiband;
        multiband.prepare(kRate);
        multiband.setCrossovers(300.0f, 3000.0f);
        multiband.setAttackMs(5.0f);
        multiband.setReleaseMs(80.0f);
        multiband.setBand(0, compressBass ? -30.0f : 0.0f, compressBass ? 8.0f : 1.0f, 0.0f);
        multiband.setBand(1, 0.0f, 1.0f, 0.0f);
        multiband.setBand(2, 0.0f, 1.0f, 0.0f);

        return settledLevelDb(hz, 0.5, [&](float x)
        {
            float frame[1] { x };
            multiband.processFrame(frame, 1);
            return frame[0];
        });
    };

    // 100 Hz sits in the low band: over its threshold, it's pulled down.
    REQUIRE(level(100.0, true) < -12.0);
    // 8 kHz sits in the high band, whose threshold nothing reaches.
    REQUIRE_THAT(level(8000.0, true), WithinAbs(0.0, 0.5));
    // With every band at 1:1 the compressor is (to the ear) not there.
    REQUIRE_THAT(level(100.0, false), WithinAbs(0.0, 0.5));
    REQUIRE_THAT(level(1000.0, false), WithinAbs(0.0, 0.5));
}
