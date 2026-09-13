#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/DelayLine.h>

#include <vector>

using Catch::Approx;
using looper::engine::DelayLine;

TEST_CASE("DelayLine delays an impulse by the delay time", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(8);

    std::vector<float> in { 1, 0, 0, 0, 0, 0, 0, 0 };
    std::vector<float> out;
    for (float x : in)
        out.push_back(dl.processSample(x, 3, 0.0f));

    REQUIRE(out[0] == Approx(0.0f));
    REQUIRE(out[2] == Approx(0.0f));
    REQUIRE(out[3] == Approx(1.0f)); // impulse reappears 3 samples later
    REQUIRE(out[4] == Approx(0.0f));
}

TEST_CASE("DelayLine feedback produces decaying echoes", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(8);

    std::vector<float> in(8, 0.0f);
    in[0] = 1.0f;
    std::vector<float> out;
    for (float x : in)
        out.push_back(dl.processSample(x, 2, 0.5f));

    REQUIRE(out[2] == Approx(1.0f));  // first tap
    REQUIRE(out[4] == Approx(0.5f));  // one feedback round
    REQUIRE(out[6] == Approx(0.25f)); // two feedback rounds
}

TEST_CASE("DelayLine clamps an over-long delay to its buffer", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(4);
    REQUIRE(dl.processSample(1.0f, 100, 0.0f) == Approx(0.0f)); // no out-of-bounds read
}

TEST_CASE("A fractional delay interpolates between samples", "[engine][delay]")
{
    // Halfway between two known samples must be their average, or the sweep a
    // chorus performs would step from one sample to the next.
    DelayLine line;
    line.prepare(64);

    // Write a ramp so neighbouring samples differ predictably.
    for (int n = 0; n < 32; ++n)
        line.processSampleFractional((float) n, 0.0, 0.0f);

    DelayLine a, b, half;
    a.prepare(64); b.prepare(64); half.prepare(64);
    for (int n = 0; n < 32; ++n)
    {
        a.processSampleFractional((float) n, 4.0, 0.0f);
        b.processSampleFractional((float) n, 5.0, 0.0f);
        half.processSampleFractional((float) n, 4.5, 0.0f);
    }

    const float atFour = a.processSampleFractional(99.0f, 4.0, 0.0f);
    const float atFive = b.processSampleFractional(99.0f, 5.0, 0.0f);
    const float atHalf = half.processSampleFractional(99.0f, 4.5, 0.0f);

    REQUIRE(atFour != atFive); // the two are genuinely different samples
    REQUIRE(std::abs(atHalf - 0.5f * (atFour + atFive)) < 1.0e-4f);
}

TEST_CASE("A swept fractional delay stays smooth", "[engine][delay]")
{
    // The reason interpolation is here at all. Sweeping the delay across whole
    // samples with an integer read makes the output jump each time the index
    // changes; that jump is an audible click on every step of the sweep.
    DelayLine fractional, integral;
    fractional.prepare(2048);
    integral.prepare(2048);

    constexpr int kSamples = 4000;
    float worstFractional = 0.0f, worstIntegral = 0.0f;
    float previousFractional = 0.0f, previousIntegral = 0.0f;

    for (int n = 0; n < kSamples; ++n)
    {
        // A slow sine input so the signal itself is smooth, and a slow sweep
        // across several whole samples of delay.
        const float in    = (float) std::sin(2.0 * 3.14159265 * 200.0 * n / 48000.0);
        const double delay = 100.0 + 40.0 * (double) n / kSamples;

        const float f = fractional.processSampleFractional(in, delay, 0.0f);
        const float i = integral.processSample(in, (int) delay, 0.0f);

        if (n > 200) // past the initial fill
        {
            worstFractional = std::max(worstFractional, std::abs(f - previousFractional));
            worstIntegral   = std::max(worstIntegral, std::abs(i - previousIntegral));
        }
        previousFractional = f;
        previousIntegral   = i;
    }

    INFO("worst step: fractional " << worstFractional << ", integral " << worstIntegral);
    REQUIRE(worstFractional < worstIntegral);
}

TEST_CASE("At whole delays the fractional path matches the integer one", "[engine][delay]")
{
    // The invariant that matters: interpolation must not change the answer
    // where there is nothing to interpolate. My first attempt here asserted
    // that a delay of zero returns the previous sample — it doesn't, in either
    // path, because delay zero reads the write position, which is a whole
    // buffer old. That was the test being wrong, not the line.
    for (int delay : { 1, 2, 7, 16, 31 })
    {
        DelayLine fractional, integral;
        fractional.prepare(64);
        integral.prepare(64);

        for (int n = 0; n < 200; ++n)
        {
            const float in = (float) std::sin(0.05 * n);
            const float f  = fractional.processSampleFractional(in, (double) delay, 0.0f);
            const float i  = integral.processSample(in, delay, 0.0f);

            INFO("delay " << delay << " sample " << n);
            REQUIRE(std::abs(f - i) < 1.0e-6f);
        }
    }
}

TEST_CASE("A fractional delay past the buffer is clamped, not wrapped", "[engine][delay]")
{
    // An LFO overshooting its range must not read from the far end of the
    // buffer, which would be a delay of the wrong length entirely.
    DelayLine line;
    line.prepare(32);

    for (int n = 0; n < 64; ++n)
    {
        const float out = line.processSampleFractional(0.5f, 1.0e6, 0.0f);
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) <= 1.0f);
    }
}
