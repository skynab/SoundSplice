#include <catch2/catch_test_macros.hpp>

#include <app/SampleDetail.h>

using namespace soundsplice;

namespace
{
    /** 100 samples at 1000 Hz starting 2 s into the clip: sample i is i/100. */
    SampleDetail detail()
    {
        SampleDetail d;
        d.startSeconds = 2.0;
        d.sampleRate   = 1000.0;
        d.channels.emplace_back();
        for (int i = 0; i < 100; ++i)
            d.channels[0].push_back((float) i / 100.0f);
        return d;
    }
}

TEST_CASE("Samples are wanted once a pixel covers half a peaks bin or less", "[app][sampledetail]")
{
    REQUIRE(SampleDetail::wanted(32.0 / 48000.0, 48000.0));
    REQUIRE(SampleDetail::wanted(1.0 / 480000.0, 48000.0));
    REQUIRE_FALSE(SampleDetail::wanted(64.0 / 48000.0, 48000.0));
    REQUIRE_FALSE(SampleDetail::wanted(0.001, 0.0));
}

TEST_CASE("Held samples cover a view inside them, clipped to the clip's end", "[app][sampledetail]")
{
    const auto d = detail(); // 2.0 s to 2.1 s

    REQUIRE(d.covers(2.01, 2.09, 60.0));
    REQUIRE(d.covers(2.0, 2.1, 60.0));
    REQUIRE_FALSE(d.covers(1.99, 2.05, 60.0));
    REQUIRE_FALSE(d.covers(2.05, 2.2, 60.0));

    // Past the clip's end there is nothing to need.
    REQUIRE(d.covers(2.05, 2.5, 2.1));

    REQUIRE_FALSE(SampleDetail {}.covers(0.0, 1.0, 10.0));
}

TEST_CASE("A pixel's range is the samples inside it, or the one under it", "[app][sampledetail]")
{
    const auto d = detail();

    // Samples 10..19: edges half a sample in, clear of rounding at a boundary.
    auto bin = d.range(0, 2.0095, 2.0195);
    REQUIRE(bin.minimum == 0.10f);
    REQUIRE(bin.maximum == 0.19f);

    // Narrower than a sample: the one it sits on.
    bin = d.range(0, 2.0152, 2.0154);
    REQUIRE(bin.minimum == 0.15f);
    REQUIRE(bin.maximum == 0.15f);

    // Outside what's held.
    REQUIRE(d.range(0, 3.0, 3.1).isEmpty());

    float s = 0.0f;
    REQUIRE(d.sampleAt(0, d.indexAt(2.0305), s));
    REQUIRE(s == 0.30f);
    REQUIRE(d.sampleAt(1, 99, s)); // a missing channel repeats the last
    REQUIRE_FALSE(d.sampleAt(0, 100, s));
    REQUIRE_FALSE(d.sampleAt(0, -1, s));
}
