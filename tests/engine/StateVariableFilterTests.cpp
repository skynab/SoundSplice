#include <catch2/catch_test_macros.hpp>

#include <engine/StateVariableFilter.h>

#include <cmath>

using looper::engine::StateVariableFilter;

namespace
{
// RMS gain of the filter at a given frequency, measured over the settled tail.
float measureGain(StateVariableFilter& filter, float freq, float sampleRate, int numSamples = 8000)
{
    constexpr double twoPi = 6.283185307179586;
    double phase = 0.0;
    const double inc = twoPi * freq / sampleRate;

    double sumIn = 0.0, sumOut = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = (float) std::sin(phase);
        phase += inc;
        const float y = filter.processSample(x);

        if (i >= numSamples / 2) // ignore the transient
        {
            sumIn  += (double) x * x;
            sumOut += (double) y * y;
        }
    }
    return sumIn > 0.0 ? (float) std::sqrt(sumOut / sumIn) : 0.0f;
}
}

TEST_CASE("SVF low-pass passes lows and attenuates highs", "[engine][svf]")
{
    const float sr = 48000.0f;
    StateVariableFilter f;
    f.prepare(sr);
    f.setMode(StateVariableFilter::Mode::LowPass);
    f.setCutoff(1000.0f);
    f.setResonance(0.707f);

    f.reset();
    const float low = measureGain(f, 100.0f, sr);
    f.reset();
    const float high = measureGain(f, 12000.0f, sr);

    REQUIRE(low > 0.9f);   // ~unity well below cutoff
    REQUIRE(high < 0.15f); // strongly attenuated well above cutoff
}

TEST_CASE("SVF high-pass passes highs and attenuates lows", "[engine][svf]")
{
    const float sr = 48000.0f;
    StateVariableFilter f;
    f.prepare(sr);
    f.setMode(StateVariableFilter::Mode::HighPass);
    f.setCutoff(1000.0f);
    f.setResonance(0.707f);

    f.reset();
    const float high = measureGain(f, 12000.0f, sr);
    f.reset();
    const float low = measureGain(f, 100.0f, sr);

    REQUIRE(high > 0.9f);
    REQUIRE(low < 0.15f);
}

TEST_CASE("SVF band-pass peaks near the cutoff", "[engine][svf]")
{
    const float sr = 48000.0f;
    StateVariableFilter f;
    f.prepare(sr);
    f.setMode(StateVariableFilter::Mode::BandPass);
    f.setCutoff(1000.0f);
    f.setResonance(0.707f);

    f.reset();
    const float atCutoff = measureGain(f, 1000.0f, sr);
    f.reset();
    const float low = measureGain(f, 60.0f, sr);
    f.reset();
    const float high = measureGain(f, 16000.0f, sr);

    REQUIRE(atCutoff > low);
    REQUIRE(atCutoff > high);
}
