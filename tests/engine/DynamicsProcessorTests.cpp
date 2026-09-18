#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/DynamicsProcessor.h"

#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    /** A 1 kHz sine at @p levelDb through @p processor, its settled output level in dB. */
    double settledDb(DynamicsProcessor& processor, double levelDb)
    {
        processor.prepare(kRate);
        const double amplitude = std::pow(10.0, levelDb / 20.0);
        double       peak      = 0.0;
        const int    total     = (int) kRate;
        for (int n = 0; n < total; ++n)
        {
            float frame[1] { (float) (amplitude * std::sin(2.0 * 3.14159265358979 * 1000.0 * n / kRate)) };
            processor.processFrame(frame, 1);
            if (n > total * 3 / 4)
                peak = std::max(peak, (double) std::abs(frame[0]));
        }
        return 20.0 * std::log10(std::max(peak, 1.0e-9));
    }
}

TEST_CASE("A transfer curve joins its points and runs level beyond them", "[engine][dynamics]")
{
    TransferCurve curve;
    curve.count     = 3;
    curve.points[0] = { -20.0f, -20.0f };
    curve.points[1] = { -80.0f, -80.0f }; // stored out of order: sorted when read
    curve.points[2] = { 0.0f, -10.0f };

    REQUIRE_THAT(curve.outputDb(-50.0f), WithinAbs(-50.0, 1e-4));  // on the line below the knee
    REQUIRE_THAT(curve.outputDb(-10.0f), WithinAbs(-15.0, 1e-4));  // 2:1 above it
    REQUIRE_THAT(curve.outputDb(-90.0f), WithinAbs(-90.0, 1e-4));  // below the first point, level
    REQUIRE_THAT(curve.outputDb(6.0f), WithinAbs(-4.0, 1e-4));     // above the last, level

    const TransferCurve unity;
    REQUIRE_THAT(unity.outputDb(-37.0f), WithinAbs(-37.0, 1e-4));
}

TEST_CASE("The dynamics processor follows its curve: compressing, gating, or nothing", "[engine][dynamics]")
{
    DynamicsProcessor processor;
    processor.setAttackMs(1.0f);
    processor.setReleaseMs(50.0f);

    // A straight line changes nothing.
    REQUIRE_THAT(settledDb(processor, -12.0), WithinAbs(-12.0, 0.2));

    // 2:1 above -20 dB: -6 dB in comes out near -13 dB.
    TransferCurve compress;
    compress.count     = 3;
    compress.points[0] = { -80.0f, -80.0f };
    compress.points[1] = { -20.0f, -20.0f };
    compress.points[2] = { 0.0f, -10.0f };
    processor.setCurve(compress);
    REQUIRE_THAT(settledDb(processor, -6.0), WithinAbs(-13.0, 1.0));
    REQUIRE_THAT(settledDb(processor, -30.0), WithinAbs(-30.0, 0.5)); // below the knee, untouched

    // A gate: quiet signals are turned right down, loud ones pass.
    TransferCurve gate;
    gate.count     = 4;
    gate.points[0] = { -100.0f, -100.0f };
    gate.points[1] = { -55.0f, -100.0f };
    gate.points[2] = { -50.0f, -50.0f };
    gate.points[3] = { 0.0f, 0.0f };
    processor.setCurve(gate);
    REQUIRE(settledDb(processor, -65.0) < -90.0);
    REQUIRE_THAT(settledDb(processor, -20.0), WithinAbs(-20.0, 0.5));

    // Make-up gain adds on top.
    processor.setCurve({});
    processor.setMakeUpDb(6.0f);
    REQUIRE_THAT(settledDb(processor, -20.0), WithinAbs(-14.0, 0.3));
}
