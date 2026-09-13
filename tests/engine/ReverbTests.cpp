#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/Reverb.h>

using Catch::Approx;
using looper::engine::Reverb;

TEST_CASE("Reverb produces a decaying tail from an impulse", "[engine][reverb]")
{
    Reverb r;
    r.prepare(44100.0);
    r.setDry(0.0f);
    r.setWet(1.0f);
    r.setRoomSize(0.5f);
    r.setDamping(0.5f);
    r.setWidth(1.0f);

    // Feed a single impulse, then silence.
    float l = 1.0f, rr = 1.0f;
    r.processStereo(l, rr);

    double early = 0.0, late = 0.0;
    const int n = 44100;
    for (int i = 0; i < n; ++i)
    {
        float a = 0.0f, b = 0.0f;
        r.processStereo(a, b);
        const double e = (double) a * a + (double) b * b;
        if (i < 5000)
            early += e;
        else if (i >= n - 5000)
            late += e;
    }

    REQUIRE(early > 0.0);  // a tail was produced
    REQUIRE(late < early);  // and it decays over one second
}

TEST_CASE("Reverb dry path passes audio through untouched", "[engine][reverb]")
{
    Reverb r;
    r.prepare(44100.0);
    r.setDry(1.0f);
    r.setWet(0.0f);

    float l = 0.5f, rr = -0.3f;
    r.processStereo(l, rr);

    REQUIRE(l == Approx(0.5f));
    REQUIRE(rr == Approx(-0.3f));
}
