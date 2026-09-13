#pragma once

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace soundsplice::app
{
/**
    How time is shown: bars and beats for music, minutes and seconds for
    everything an audio editor does (a podcast has no bars).

    An app preference rather than project data, like the dock layout: it
    changes what the ruler, grid, snapping and position readout count in, not
    anything about the song. JUCE-free so the formatting and the choice of grid
    step, which is where the off-by-one and rounding mistakes live, are tested
    headless.
*/
enum class TimeFormat
{
    BarsBeats      = 0, // the numeric values are stored in the app settings
    MinutesSeconds = 1
};

/** @p seconds as a clock: "1:05.250", or "1:02:03.250" past an hour, with
    @p decimals digits after the point (none for 0). Rounded before being split
    into fields, so 59.9996 reads "1:00.000" rather than "0:60.000". Negative
    or non-numeric times read as zero. */
inline std::string formatClockTime(double seconds, int decimals = 3)
{
    if (! (seconds > 0.0))
        seconds = 0.0;

    decimals = decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals);

    long long unitsPerSecond = 1;
    for (int i = 0; i < decimals; ++i)
        unitsPerSecond *= 10;

    const long long totalUnits   = std::llround(seconds * (double) unitsPerSecond);
    const long long wholeSeconds = totalUnits / unitsPerSecond;
    const long long fraction     = totalUnits % unitsPerSecond;

    const long long hours   = wholeSeconds / 3600;
    const long long minutes = (wholeSeconds / 60) % 60;
    const long long secs    = wholeSeconds % 60;

    char text[64];
    if (hours > 0)
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", hours, minutes, secs);
    else
        std::snprintf(text, sizeof(text), "%lld:%02lld", minutes, secs);

    std::string result(text);
    if (decimals > 0)
    {
        std::snprintf(text, sizeof(text), ".%0*lld", decimals, fraction);
        result += text;
    }
    return result;
}

namespace timeformatdetail
{
    // 1-2-5 steps, then the ones a clock is read in. The seconds grid is
    // drawn at these.
    inline constexpr double kSecondsSteps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0,
                                                30.0, 60.0, 300.0, 600.0, 1800.0, 3600.0 };

    inline bool divides(double small, double large)
    {
        const double ratio = large / small;
        return std::abs(ratio - std::round(ratio)) < 1.0e-6;
    }
}

/** The smallest step, in seconds, whose grid lines are at least
    @p minSpacingPixels apart at @p pixelsPerSecond: what the ruler labels. */
inline double secondsGridStep(double pixelsPerSecond, double minSpacingPixels)
{
    const auto& steps = timeformatdetail::kSecondsSteps;
    if (! (pixelsPerSecond > 0.0))
        return steps[std::size(steps) - 1];

    for (double step : steps)
        if (step * pixelsPerSecond >= minSpacingPixels)
            return step;

    return steps[std::size(steps) - 1];
}

/** The finer step for the unlabelled lines between @p majorStep's labelled
    ones, and what clips snap to: the smallest step at least
    @p minSpacingPixels apart that divides @p majorStep exactly, so every
    labelled line is also a minor line and snapping lands on the labels too.
    @p majorStep itself if nothing finer fits. */
inline double secondsMinorStep(double majorStep, double pixelsPerSecond, double minSpacingPixels)
{
    if (pixelsPerSecond > 0.0)
        for (double step : timeformatdetail::kSecondsSteps)
            if (step <= majorStep && step * pixelsPerSecond >= minSpacingPixels
                && timeformatdetail::divides(step, majorStep))
                return step;

    return majorStep;
}

/** How many decimals a ruler label needs to tell lines @p step seconds apart. */
inline int clockDecimalsForStep(double step)
{
    if (step >= 1.0)
        return 0;
    return step >= 0.1 ? 1 : 2;
}

} // namespace soundsplice::app
