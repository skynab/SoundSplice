#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/ThirdOctaveEq.h"

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("The 31-band graphic EQ boosts a third of an octave at each centre", "[engine][eq]")
{
    ThirdOctaveEq eq;
    eq.prepare(48000.0);

    // Flat: nothing anywhere.
    REQUIRE_THAT(eq.magnitudeDbAt(1000.0f), WithinAbs(0.0, 1e-6));

    // One band up: its gain at its centre, little a whole octave away.
    eq.setGainDb(17, 9.0f); // 1 kHz
    REQUIRE_THAT(eq.magnitudeDbAt(1000.0f), WithinAbs(9.0, 0.05));
    REQUIRE(eq.magnitudeDbAt(2000.0f) < 1.5f);
    REQUIRE(eq.magnitudeDbAt(500.0f) < 1.5f);

    // Neighbouring bands set together make a smooth shelf-like rise, not
    // separate peaks with holes between.
    for (int b = 24; b < ThirdOctaveEq::kBands; ++b)
        eq.setGainDb(b, 6.0f);
    const float between = eq.magnitudeDbAt(7100.0f); // between 6.3 and 8 kHz
    REQUIRE(between > 5.0f);
    REQUIRE(between < 12.0f);

    // Bands the sample rate can't carry are left alone.
    ThirdOctaveEq low;
    low.prepare(22050.0);
    low.setGainDb(30, 12.0f); // 20 kHz, above this rate's Nyquist
    REQUIRE_THAT(low.magnitudeDbAt(5000.0f), WithinAbs(0.0, 1e-6));

    // Processing a sample matches too: a flat EQ passes it through.
    ThirdOctaveEq flat;
    flat.prepare(48000.0);
    REQUIRE(flat.processSample(0.25f) == 0.25f);
}
