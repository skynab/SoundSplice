#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/DcBlocker.h>

#include <algorithm>
#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("The DC blocker removes an offset and leaves audio alone", "[engine][dcblocker]")
{
    constexpr double rate = 48000.0;
    constexpr double pi   = 3.14159265358979323846;

    DcBlocker blocker;
    blocker.prepare(rate);
    blocker.setCutoffHz(5.0f);

    // Two seconds of a 100 Hz tone riding on a 0.25 offset.
    constexpr int frames = 96000;
    float         peak   = 0.0f;
    double        sum    = 0.0;
    for (int n = 0; n < frames; ++n)
    {
        const auto tone = (float) (0.5 * std::sin(2.0 * pi * 100.0 * n / rate));
        const auto out  = blocker.process(0.25f + tone);
        if (n >= frames - 4800) // the last 100 ms, well after it settles
        {
            peak = std::max(peak, std::abs(out));
            sum += out;
        }
    }

    REQUIRE_THAT(sum / 4800.0, WithinAbs(0.0, 0.002)); // the offset is gone
    REQUIRE_THAT(peak, WithinAbs(0.5, 0.01));          // the tone isn't
}

TEST_CASE("The DC blocker settles to silence without denormals", "[engine][dcblocker]")
{
    DcBlocker blocker;
    blocker.prepare(44100.0);
    blocker.process(1.0f);

    float last = 1.0f;
    for (int n = 0; n < 441000; ++n)
        last = blocker.process(0.0f);
    REQUIRE(last == 0.0f);
}
