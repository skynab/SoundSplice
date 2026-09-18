#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/ParametricEq.h"

#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    /** A sine through @p eq once it has settled, in dB relative to the input. */
    double runLevelDb(ParametricEq& eq, double hz)
    {
        eq.reset();
        double peak = 0.0;
        const int total = (int) kRate;
        for (int n = 0; n < total; ++n)
        {
            const float out = eq.processSample((float) std::sin(2.0 * 3.14159265358979 * hz * n / kRate));
            if (n > total / 2)
                peak = std::max(peak, (double) std::abs(out));
        }
        return 20.0 * std::log10(std::max(peak, 1.0e-9));
    }
}

TEST_CASE("The parametric EQ's bands each do what their type says", "[engine][eq]")
{
    ParametricEq eq;
    eq.prepare(kRate);

    // All off: nothing.
    REQUIRE_THAT(runLevelDb(eq, 1000.0), WithinAbs(0.0, 0.05));

    // A bell: its gain at its centre, nothing far away.
    eq.setBand(0, { ParametricBand::Type::Bell, 1000.0f, 9.0f, 2.0f });
    REQUIRE_THAT(runLevelDb(eq, 1000.0), WithinAbs(9.0, 0.2));
    REQUIRE_THAT(runLevelDb(eq, 100.0), WithinAbs(0.0, 0.3));
    REQUIRE_THAT(eq.magnitudeDbAt(1000.0f), WithinAbs(9.0, 0.05));

    // A notch: a hole at its frequency, whatever the gain says.
    eq.setBand(0, { ParametricBand::Type::Notch, 60.0f, 12.0f, 10.0f });
    REQUIRE(runLevelDb(eq, 60.0) < -30.0);
    REQUIRE_THAT(runLevelDb(eq, 1000.0), WithinAbs(0.0, 0.1));

    // Cuts: -3 dB at the corner (Q 0.707), steeply below or above it.
    eq.setBand(0, { ParametricBand::Type::LowCut, 200.0f, 0.0f, 0.707f });
    REQUIRE_THAT(eq.magnitudeDbAt(200.0f), WithinAbs(-3.0, 0.1));
    REQUIRE(eq.magnitudeDbAt(50.0f) < -20.0f);
    REQUIRE_THAT(eq.magnitudeDbAt(5000.0f), WithinAbs(0.0, 0.1));

    eq.setBand(0, { ParametricBand::Type::HighCut, 2000.0f, 0.0f, 0.707f });
    REQUIRE(eq.magnitudeDbAt(8000.0f) < -20.0f);
    REQUIRE_THAT(eq.magnitudeDbAt(100.0f), WithinAbs(0.0, 0.1));

    // Shelves: their gain well past the corner, none well before it.
    eq.setBand(0, { ParametricBand::Type::LowShelf, 200.0f, -6.0f, 0.707f });
    REQUIRE_THAT(eq.magnitudeDbAt(20.0f), WithinAbs(-6.0, 0.3));
    REQUIRE_THAT(eq.magnitudeDbAt(10000.0f), WithinAbs(0.0, 0.2));

    // Bands add: a shelf and a bell elsewhere.
    eq.setBand(1, { ParametricBand::Type::Bell, 5000.0f, 4.0f, 1.0f });
    REQUIRE_THAT(eq.magnitudeDbAt(5000.0f), WithinAbs(4.0, 0.2));
    REQUIRE_THAT(parametricMagnitudeDb(eq.bands(), 5000.0f), WithinAbs(eq.magnitudeDbAt(5000.0f), 1e-4));
}
