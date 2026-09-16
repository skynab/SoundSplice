#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <app/WaveformScale.h>

using namespace soundsplice::waveformscale;
using Catch::Matchers::WithinAbs;

TEST_CASE("On the linear scale a sample is drawn at its value", "[app][waveformscale]")
{
    REQUIRE_THAT(heightFor(0.5f, false), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(heightFor(-0.25f, false), WithinAbs(-0.25, 1e-6));
    REQUIRE_THAT(heightFor(1.5f, false), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(sampleFor(0.3f, false), WithinAbs(0.3, 1e-6));
}

TEST_CASE("On the dB scale height follows level", "[app][waveformscale]")
{
    REQUIRE_THAT(heightFor(1.0f, true), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(heightFor(0.031622777f, true), WithinAbs(0.5, 1e-5)); // -30 dB, halfway down a 60 dB range
    REQUIRE_THAT(heightFor(-0.031622777f, true), WithinAbs(-0.5, 1e-5));
    REQUIRE_THAT(heightFor(0.001f, true), WithinAbs(0.0, 1e-5));        // -60 dB, the bottom of it
    REQUIRE_THAT(heightFor(0.0f, true), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(heightFor(1.0e-5f, true), WithinAbs(0.0, 1e-6)); // below the range: the centre line
    REQUIRE_THAT(heightFor(2.0f, true), WithinAbs(1.0, 1e-6));

    // Quiet material stands well clear of the centre line.
    REQUIRE(heightFor(0.01f, true) > 10.0f * heightFor(0.01f, false));
}

TEST_CASE("A height maps back to the sample drawn there", "[app][waveformscale]")
{
    for (float sample : { 0.9f, 0.1f, -0.02f, 0.003f })
        for (bool db : { false, true })
            REQUIRE_THAT(sampleFor(heightFor(sample, db), db), WithinAbs(sample, 1e-5));

    REQUIRE_THAT(sampleFor(0.0f, true), WithinAbs(0.0, 1e-6));
}
