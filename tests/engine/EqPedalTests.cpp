#include <catch2/catch_test_macros.hpp>

#include <engine/PedalDsp.h>

#include <cmath>

using looper::engine::ThreeBandEq;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** RMS gain through the EQ at one frequency, over the settled tail. */
float measureGain(const ThreeBandEq::Settings& settings, double hz, double sampleRate = 48000.0)
{
    ThreeBandEq eq;
    eq.prepare(sampleRate);
    eq.setSettings(settings);

    const int numSamples = 16384;

    double sumIn = 0.0, sumOut = 0.0;
    for (int n = 0; n < numSamples; ++n)
    {
        const double in  = std::sin(2.0 * kPi * hz * n / sampleRate);
        const double out = eq.processSample((float) in);

        if (n >= numSamples / 2) // ignore the transient
        {
            sumIn  += in * in;
            sumOut += out * out;
        }
    }
    return (float) std::sqrt(sumOut / sumIn);
}

ThreeBandEq::Settings midCut(float hz, float db, float q)
{
    ThreeBandEq::Settings s;
    s.midHz = hz;
    s.midDb = db;
    s.midQ  = q;
    return s;
}
}

TEST_CASE("The EQ pedal cuts the mids where it is told to", "[engine][eq]")
{
    // The scoop that was unreachable on a track before this pedal existed:
    // EqSettings is master-bus only, and a filter slot picks a cutoff, which
    // is a different thing entirely.
    const auto scooped = midCut(800.0f, -12.0f, 1.0f);

    const float atMid  = measureGain(scooped, 800.0);
    const float atLow  = measureGain(scooped, 80.0);
    const float atHigh = measureGain(scooped, 8000.0);

    INFO("800Hz " << atMid << "  80Hz " << atLow << "  8k " << atHigh);

    REQUIRE(atMid < 0.3f);  // -12dB is about 0.25
    REQUIRE(atLow > 0.9f);  // and the bands either side are left alone
    REQUIRE(atHigh > 0.9f);
}

TEST_CASE("The EQ pedal's mid is sweepable", "[engine][eq]")
{
    // "The mids" is 400Hz on one guitar and 1.2kHz on another. A fixed bell
    // would only ever be right for one of them, so the frequency has to carry
    // the cut with it.
    const auto low  = midCut(400.0f, -12.0f, 2.0f);
    const auto high = midCut(1600.0f, -12.0f, 2.0f);

    REQUIRE(measureGain(low, 400.0) < measureGain(high, 400.0));
    REQUIRE(measureGain(high, 1600.0) < measureGain(low, 1600.0));
}

TEST_CASE("The EQ pedal's Q controls how wide the mid band is", "[engine][eq]")
{
    // A narrow bell has stopped cutting an octave up; a broad one has not.
    const float narrow = measureGain(midCut(800.0f, -12.0f, 4.0f), 1600.0);
    const float broad  = measureGain(midCut(800.0f, -12.0f, 0.5f), 1600.0);

    INFO("an octave up: Q=4 " << narrow << "  Q=0.5 " << broad);
    REQUIRE(narrow > broad);
}

TEST_CASE("The EQ pedal's shelves work at their own ends", "[engine][eq]")
{
    ThreeBandEq::Settings s;
    s.lowShelfHz  = 120.0f;
    s.lowShelfDb  = 9.0f;
    s.highShelfHz = 4000.0f;
    s.highShelfDb = -9.0f;

    const float atLow  = measureGain(s, 50.0);
    const float atMid  = measureGain(s, 800.0);
    const float atHigh = measureGain(s, 10000.0);

    INFO("50Hz " << atLow << "  800Hz " << atMid << "  10k " << atHigh);

    REQUIRE(atLow > 2.0f);   // +9dB is about 2.8
    REQUIRE(atHigh < 0.5f);  // -9dB is about 0.35
    REQUIRE(atMid > 0.8f);   // and the middle is left near flat
    REQUIRE(atMid < 1.25f);
}

TEST_CASE("The EQ pedal is flat by default", "[engine][eq]")
{
    // Adding the pedal must change nothing until it is dialled - otherwise
    // inserting one would shift every chain it is dropped into.
    for (double hz : { 60.0, 250.0, 1000.0, 4000.0, 12000.0 })
    {
        const float gain = measureGain(ThreeBandEq::Settings {}, hz);
        INFO("flat at " << hz << "Hz: " << gain);
        REQUIRE(gain > 0.99f);
        REQUIRE(gain < 1.01f);
    }
}

TEST_CASE("The EQ pedal's reported curve matches what it does to audio", "[engine][eq]")
{
    // magnitudeDbAt reads the live coefficients, so a UI curve drawn from it
    // cannot disagree with what is being heard.
    const auto settings = midCut(1000.0f, -10.0f, 1.5f);

    ThreeBandEq eq;
    eq.prepare(48000.0);
    eq.setSettings(settings);

    for (float hz : { 100.0f, 1000.0f, 5000.0f })
    {
        const float measuredDb = 20.0f * std::log10(measureGain(settings, (double) hz));
        const float reportedDb = eq.magnitudeDbAt(hz);

        INFO(hz << "Hz: measured " << measuredDb << "dB  reported " << reportedDb << "dB");
        REQUIRE(std::abs(measuredDb - reportedDb) < 0.5f);
    }
}

TEST_CASE("The EQ pedal clamps settings into usable ranges", "[engine][eq]")
{
    // A 0Hz corner or a 0 Q is a broken filter, and these come from a
    // document that a future version may write differently.
    ThreeBandEq::Settings absurd;
    absurd.lowShelfHz  = -50.0f;
    absurd.midHz       = 1.0e9f;
    absurd.midQ        = 0.0f;
    absurd.midDb       = 400.0f;
    absurd.highShelfHz = 0.0f;

    ThreeBandEq eq;
    eq.prepare(48000.0);
    eq.setSettings(absurd);

    for (int n = 0; n < 4096; ++n)
    {
        const float y = eq.processSample((float) std::sin(0.05 * n));
        REQUIRE(std::isfinite(y));
    }
}
