#include <catch2/catch_test_macros.hpp>

#include <app/TimeFormat.h>

using namespace soundsplice::app;

TEST_CASE("Times read as a clock", "[app][timeformat]")
{
    REQUIRE(formatClockTime(0.0) == "0:00.000");
    REQUIRE(formatClockTime(65.25) == "1:05.250");
    REQUIRE(formatClockTime(3723.5, 1) == "1:02:03.5");
    REQUIRE(formatClockTime(5.4, 0) == "0:05");
    REQUIRE(formatClockTime(0.05, 2) == "0:00.05");
}

TEST_CASE("A clock rounds before carrying, so it never shows 60 seconds", "[app][timeformat]")
{
    REQUIRE(formatClockTime(59.9996) == "1:00.000");
    REQUIRE(formatClockTime(3599.9999, 2) == "1:00:00.00");
}

TEST_CASE("A time before zero, or not a number, reads as zero", "[app][timeformat]")
{
    REQUIRE(formatClockTime(-3.0) == "0:00.000");
    REQUIRE(formatClockTime(std::nan("")) == "0:00.000");
}

TEST_CASE("The ruler labels the smallest step that keeps labels apart", "[app][timeformat]")
{
    // 48 px per second: a label needs 60 px, so every 2 seconds.
    REQUIRE(secondsGridStep(48.0, 60.0) == 2.0);

    // Zoomed right in, tenths of a second fit.
    REQUIRE(secondsGridStep(1000.0, 60.0) == 0.1);

    // Zoomed right out, or no scale at all, the coarsest step.
    REQUIRE(secondsGridStep(0.001, 60.0) == 3600.0);
    REQUIRE(secondsGridStep(0.0, 60.0) == 3600.0);
}

TEST_CASE("Minor grid lines always divide the labelled ones", "[app][timeformat]")
{
    // Otherwise a minor line would fall between two labels and snapping would
    // miss the labelled times.
    REQUIRE(secondsMinorStep(2.0, 48.0, 12.0) == 0.5);  // the finest step that fits and divides 2
    REQUIRE(secondsMinorStep(10.0, 6.0, 12.0) == 2.0);
    REQUIRE(secondsMinorStep(30.0, 6.0, 12.0) == 2.0);
    REQUIRE(secondsMinorStep(0.5, 100.0, 12.0) == 0.5); // 0.2 fits but doesn't divide 0.5; 0.1 doesn't fit
    REQUIRE(secondsMinorStep(5.0, 1.0, 12.0) == 5.0);   // nothing finer fits: the labelled step itself

    for (double pps : { 0.05, 0.5, 3.0, 7.0, 48.0, 120.0, 900.0, 5000.0 })
    {
        const double major = secondsGridStep(pps, 64.0);
        const double minor = secondsMinorStep(major, pps, 12.0);
        const double ratio = major / minor;

        INFO("pixels per second " << pps << ": major " << major << ", minor " << minor);
        REQUIRE(minor <= major);
        REQUIRE(std::abs(ratio - std::round(ratio)) < 1.0e-6);
    }
}

TEST_CASE("Ruler labels carry as many decimals as their step needs", "[app][timeformat]")
{
    REQUIRE(clockDecimalsForStep(5.0) == 0);
    REQUIRE(clockDecimalsForStep(1.0) == 0);
    REQUIRE(clockDecimalsForStep(0.5) == 1);
    REQUIRE(clockDecimalsForStep(0.1) == 1);
    REQUIRE(clockDecimalsForStep(0.05) == 2);
}
