#include <catch2/catch_test_macros.hpp>

#include <engine/Pickup.h>

#include <cmath>

using looper::engine::Pickup;

namespace
{
// RMS gain at one frequency, measured over the settled tail. Same technique as
// StateVariableFilterTests, which is what this is built on.
float measureGain(Pickup& pickup, float freq, float sampleRate, int numSamples = 16000)
{
    constexpr double twoPi = 6.283185307179586;
    double       phase = 0.0;
    const double inc   = twoPi * freq / sampleRate;

    double sumIn = 0.0, sumOut = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = (float) std::sin(phase);
        phase += inc;
        const float y = pickup.processSample(x);

        if (i >= numSamples / 2)
        {
            sumIn  += (double) x * x;
            sumOut += (double) y * y;
        }
    }
    return sumIn > 0.0 ? (float) std::sqrt(sumOut / sumIn) : 0.0f;
}
}

TEST_CASE("Pickup peaks at its resonance", "[engine][pickup]")
{
    const float sr = 48000.0f;
    Pickup      pickup;
    pickup.prepare(sr);
    pickup.setResonanceHz(3000.0f);
    pickup.setQ(2.0f);

    pickup.reset();
    const float belowGain = measureGain(pickup, 400.0f, sr);
    pickup.reset();
    const float peakGain = measureGain(pickup, 3000.0f, sr);
    pickup.reset();
    const float aboveGain = measureGain(pickup, 12000.0f, sr);

    // The peak is the whole point: it must be a genuine lift over the flat
    // passband, not merely "less attenuated than the stopband". Nothing in
    // this signal path could produce one before - every filter between string
    // and speaker was a first-order real pole.
    REQUIRE(peakGain > belowGain * 1.5f);
    REQUIRE(belowGain > 0.9f); // still flat well under the corner
    REQUIRE(aboveGain < 0.2f); // and 12dB/oct away above it
}

TEST_CASE("Pickup Q controls how pronounced the peak is", "[engine][pickup]")
{
    const float sr = 48000.0f;

    Pickup gentle;
    gentle.prepare(sr);
    gentle.setResonanceHz(3000.0f);
    gentle.setQ(0.7f);
    const float gentlePeak = measureGain(gentle, 3000.0f, sr);

    Pickup spiky;
    spiky.prepare(sr);
    spiky.setResonanceHz(3000.0f);
    spiky.setQ(3.0f);
    const float spikyPeak = measureGain(spiky, 3000.0f, sr);

    REQUIRE(spikyPeak > gentlePeak * 1.5f);
}

TEST_CASE("Pickup resonance frequency moves the peak", "[engine][pickup]")
{
    const float sr = 48000.0f;

    // A humbucker peaks low and dark; a single coil peaks high and bright.
    // That difference really is mostly this one number, so each has to be
    // louder than the other at its own resonance.
    Pickup humbucker;
    humbucker.prepare(sr);
    humbucker.setResonanceHz(2200.0f);
    humbucker.setQ(1.6f);

    Pickup singleCoil;
    singleCoil.prepare(sr);
    singleCoil.setResonanceHz(5000.0f);
    singleCoil.setQ(1.6f);

    humbucker.reset();
    const float humbuckerAtLow = measureGain(humbucker, 2200.0f, sr);
    humbucker.reset();
    const float humbuckerAtHigh = measureGain(humbucker, 5000.0f, sr);

    singleCoil.reset();
    const float singleAtLow = measureGain(singleCoil, 2200.0f, sr);
    singleCoil.reset();
    const float singleAtHigh = measureGain(singleCoil, 5000.0f, sr);

    REQUIRE(humbuckerAtLow > singleAtLow);
    REQUIRE(singleAtHigh > humbuckerAtHigh);
}

TEST_CASE("Pickup disabled passes the signal through untouched", "[engine][pickup]")
{
    const float sr = 48000.0f;
    Pickup      pickup;
    pickup.prepare(sr);
    pickup.setEnabled(false);

    // Bit-identical, not merely close: off has to mean the old behaviour
    // exactly, so this change can be measured against it rather than assumed.
    for (int i = 0; i < 512; ++i)
    {
        const float x = (float) std::sin(0.05 * i) * 0.8f;
        REQUIRE(pickup.processSample(x) == x);
    }
}
