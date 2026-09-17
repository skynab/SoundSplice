#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/ClipFade.h>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr FadeShape kAllShapes[] { FadeShape::Linear, FadeShape::EqualPower, FadeShape::SCurve,
                                       FadeShape::Exponential, FadeShape::Logarithmic };
}

TEST_CASE("Every fade shape runs from silence to full level", "[engine][fade]")
{
    for (const auto shape : kAllShapes)
    {
        REQUIRE_THAT(fadeCurve(shape, 0.0), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(fadeCurve(shape, 1.0), WithinAbs(1.0, 1e-6));

        // Clamped rather than extrapolated: a position a sample past either
        // end must not overshoot or go negative.
        REQUIRE_THAT(fadeCurve(shape, -1.0), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(fadeCurve(shape, 2.0), WithinAbs(1.0, 1e-6));

        // Never dips on the way up, or a fade-in would wobble in level.
        float previous = 0.0f;
        for (int i = 0; i <= 100; ++i)
        {
            const float gain = fadeCurve(shape, i / 100.0);
            REQUIRE(gain >= previous - 1.0e-6f);
            previous = gain;
        }
    }
}

TEST_CASE("The fade shapes differ where they should", "[engine][fade]")
{
    REQUIRE_THAT(fadeCurve(FadeShape::Linear, 0.5), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(fadeCurve(FadeShape::EqualPower, 0.5), WithinAbs(0.70710678, 1e-6));
    REQUIRE_THAT(fadeCurve(FadeShape::SCurve, 0.5), WithinAbs(0.5, 1e-6));

    // An S-curve starts more gently than a straight line, and equal power
    // comes up faster.
    REQUIRE(fadeCurve(FadeShape::SCurve, 0.25) < fadeCurve(FadeShape::Linear, 0.25));
    REQUIRE(fadeCurve(FadeShape::EqualPower, 0.25) > fadeCurve(FadeShape::Linear, 0.25));
}

TEST_CASE("Equal-power fades hold a crossfade's power steady", "[engine][fade]")
{
    for (int i = 0; i <= 20; ++i)
    {
        const double t        = i / 20.0;
        const double incoming = fadeCurve(FadeShape::EqualPower, t);
        const double outgoing = fadeCurve(FadeShape::EqualPower, 1.0 - t);
        REQUIRE_THAT(incoming * incoming + outgoing * outgoing, WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("A clip with no fades plays at full level throughout", "[engine][fade]")
{
    const ClipFades none;
    REQUIRE(none.isNone());
    REQUIRE(clipFadeGain(none, 0.0, 4.0) == 1.0f);
    REQUIRE(clipFadeGain(none, 3.999, 4.0) == 1.0f);
}

TEST_CASE("Fades shape the start and the end of a clip", "[engine][fade]")
{
    ClipFades fades;
    fades.inSeconds  = 1.0;
    fades.outSeconds = 2.0;

    const auto fitted = fittedFades(fades, 10.0);

    REQUIRE_THAT(clipFadeGain(fitted, 0.0, 10.0), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(clipFadeGain(fitted, 0.5, 10.0), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(clipFadeGain(fitted, 1.0, 10.0), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(clipFadeGain(fitted, 5.0, 10.0), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(clipFadeGain(fitted, 9.0, 10.0), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(clipFadeGain(fitted, 10.0, 10.0), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Fades longer than their clip are shortened to meet", "[engine][fade]")
{
    ClipFades fades;
    fades.inSeconds  = 3.0;
    fades.outSeconds = 1.0;

    const auto fitted = fittedFades(fades, 2.0);
    REQUIRE_THAT(fitted.inSeconds, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(fitted.outSeconds, WithinAbs(0.5, 1e-9));

    // Fades that already fit are left exactly as set.
    REQUIRE(fittedFades(fades, 8.0) == fades);

    // A clip with no length has no room for any fade.
    REQUIRE(fittedFades(fades, 0.0).isNone());
}

TEST_CASE("Exponential and logarithmic fades mirror each other", "[engine][fade]")
{
    // Even in decibels: halfway through, -30 dB of the 60, less the floor pulled out.
    REQUIRE_THAT(fadeCurve(FadeShape::Exponential, 0.5), WithinAbs((std::pow(10.0, -1.5) - 0.001) / 0.999, 1e-6));
    REQUIRE(fadeCurve(FadeShape::Exponential, 0.25) < fadeCurve(FadeShape::Linear, 0.25));
    REQUIRE(fadeCurve(FadeShape::Logarithmic, 0.25) > fadeCurve(FadeShape::EqualPower, 0.25));

    for (double t : { 0.1, 0.3, 0.7, 0.9 })
        REQUIRE_THAT(fadeCurve(FadeShape::Logarithmic, t), WithinAbs(1.0 - fadeCurve(FadeShape::Exponential, 1.0 - t), 1e-6));
}
