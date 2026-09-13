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

TEST_CASE("Timecode counts hours, minutes, seconds and frames", "[app][timeformat]")
{
    REQUIRE(formatTimecode(0.0, 30) == "00:00:00:00");
    REQUIRE(formatTimecode(65.5, 30) == "00:01:05:15");
    REQUIRE(formatTimecode(3661.04, 25) == "01:01:01:01");

    // A frame counts once it has begun, without floating point pushing
    // 0.1 s at 30 fps back into frame 2.
    REQUIRE(formatTimecode(0.1, 30) == "00:00:00:03");
    REQUIRE(formatTimecode(1.0 - 1.0e-9, 24) == "00:00:01:00");
    REQUIRE(formatTimecode(-2.0, 24) == "00:00:00:00");
}

TEST_CASE("Sample counts are grouped in thousands", "[app][timeformat]")
{
    REQUIRE(formatSampleCount(0) == "0");
    REQUIRE(formatSampleCount(999) == "999");
    REQUIRE(formatSampleCount(1000) == "1,000");
    REQUIRE(formatSampleCount(2880000) == "2,880,000");
    REQUIRE(formatSampleCount(-5) == "0");
}

TEST_CASE("Every format's minor grid divides its labelled grid", "[app][timeformat]")
{
    const TimeDisplay formats[] {
        { TimeFormat::MinutesSeconds, 48000.0, 30 },
        { TimeFormat::Samples, 48000.0, 30 },
        { TimeFormat::Samples, 44100.0, 30 },
        { TimeFormat::Timecode, 48000.0, 24 },
        { TimeFormat::Timecode, 48000.0, 25 },
        { TimeFormat::Timecode, 48000.0, 30 },
    };

    for (const auto& display : formats)
    {
        const auto steps = gridStepsFor(display);
        REQUIRE_FALSE(steps.empty());
        for (size_t i = 1; i < steps.size(); ++i)
            REQUIRE(steps[i] > steps[i - 1]); // finest first

        for (double pps : { 0.05, 0.5, 3.0, 48.0, 400.0, 5000.0, 100000.0 })
        {
            const double major = gridStep(steps, pps, minLabelSpacing(display.format));
            const double minor = minorStep(steps, major, pps, 12.0);
            const double ratio = major / minor;

            INFO("format " << (int) display.format << " at " << display.fps << " fps / "
                 << display.sampleRate << " Hz, " << pps << " px/s: major " << major << ", minor " << minor);
            REQUIRE(minor <= major);
            REQUIRE(std::abs(ratio - std::round(ratio)) < 1.0e-6);
        }
    }
}

TEST_CASE("Ruler labels and the readout speak the chosen format", "[app][timeformat]")
{
    const TimeDisplay samples { TimeFormat::Samples, 48000.0, 30 };
    const TimeDisplay timecode { TimeFormat::Timecode, 48000.0, 25 };
    const TimeDisplay clock { TimeFormat::MinutesSeconds, 48000.0, 30 };

    REQUIRE(gridLabel(samples, 1.0, 1.0) == "48,000");
    REQUIRE(gridLabel(timecode, 2.2, 1.0) == "00:00:02:05");
    REQUIRE(gridLabel(clock, 90.0, 10.0) == "1:30");
    REQUIRE(gridLabel(clock, 1.5, 0.5) == "0:01.5");

    REQUIRE(formatPosition(samples, 0.5) == "24,000");
    REQUIRE(formatPosition(timecode, 0.5) == "00:00:00:12");
    REQUIRE(formatPosition(clock, 0.5) == "0:00.500");

    // The steps a sample grid is drawn at are whole numbers of samples.
    for (double step : gridStepsFor(samples))
    {
        const double inSamples = step * samples.sampleRate;
        REQUIRE(std::abs(inSamples - std::round(inSamples)) < 1.0e-6);
    }
}
