#pragma once

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace soundsplice::app
{
/**
    How time is shown: bars and beats for music; a clock, a sample count or
    timecode for everything an audio editor does (a podcast has no bars).

    An app preference rather than project data, like the dock layout: it
    changes what the ruler, grid, snapping and position readout count in, not
    anything about the song. JUCE-free so the formatting and the choice of grid
    step, which is where the off-by-one and rounding mistakes live, are tested
    headless.
*/
enum class TimeFormat
{
    // The numeric values are stored in the app settings.
    BarsBeats      = 0,
    MinutesSeconds = 1,
    Samples        = 2,
    Timecode       = 3
};

/** Everything needed to show a time in the chosen format. */
struct TimeDisplay
{
    TimeFormat format     = TimeFormat::BarsBeats;
    double     sampleRate = 48000.0; // Samples count at the rate the project plays at
    int        fps        = 30;      // Timecode's frame rate

    bool operator==(const TimeDisplay&) const = default;
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

/** @p seconds as timecode, "01:02:03:12": hours, minutes, seconds and frames
    at @p fps. A frame is counted once it has begun, as a timecode display
    does, with a hair of tolerance so 0.1s at 30fps is frame 3, not 2. */
inline std::string formatTimecode(double seconds, int fps)
{
    if (! (seconds > 0.0))
        seconds = 0.0;
    if (fps <= 0)
        fps = 30;

    const long long frames       = (long long) std::floor(seconds * fps + 1.0e-6);
    const long long wholeSeconds = frames / fps;

    char text[64];
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld:%02lld", wholeSeconds / 3600,
                  (wholeSeconds / 60) % 60, wholeSeconds % 60, frames % fps);
    return text;
}

/** A sample count with its thousands separated: "2,880,000". */
inline std::string formatSampleCount(long long samples)
{
    const std::string digits = std::to_string(samples > 0 ? samples : 0);

    std::string grouped;
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            grouped += ',';
        grouped += digits[i];
    }
    return grouped;
}

namespace timeformatdetail
{
    // 1-2-5 steps, then the ones a clock is read in.
    inline constexpr double kSecondsSteps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0,
                                                30.0, 60.0, 300.0, 600.0, 1800.0, 3600.0 };

    inline bool divides(double small, double large)
    {
        const double ratio = large / small;
        return std::abs(ratio - std::round(ratio)) < 1.0e-6;
    }
}

/** The steps, in seconds and finest first, that @p display's ruler can be
    drawn at: clock steps, 1-2-5 sample counts, or frames and then seconds. */
inline std::vector<double> gridStepsFor(const TimeDisplay& display)
{
    std::vector<double> steps;

    switch (display.format)
    {
        case TimeFormat::Samples:
        {
            const double rate = display.sampleRate > 0.0 ? display.sampleRate : 48000.0;
            for (double decade = 1.0; decade <= 1.0e10; decade *= 10.0)
                for (double multiple : { 1.0, 2.0, 5.0 })
                    steps.push_back(multiple * decade / rate);
            break;
        }

        case TimeFormat::Timecode:
        {
            const int fps = display.fps > 0 ? display.fps : 30;
            for (double frames : { 1.0, 2.0, 5.0, 10.0 })
                if (frames < fps)
                    steps.push_back(frames / fps);
            for (double seconds : { 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0, 600.0, 1800.0, 3600.0 })
                steps.push_back(seconds);
            break;
        }

        case TimeFormat::BarsBeats:
        case TimeFormat::MinutesSeconds:
        default:
            steps.assign(std::begin(timeformatdetail::kSecondsSteps), std::end(timeformatdetail::kSecondsSteps));
            break;
    }

    return steps;
}

/** The finest of @p steps whose lines are at least @p minSpacingPixels apart
    at @p pixelsPerSecond: what the ruler labels. The coarsest if none are. */
inline double gridStep(const std::vector<double>& steps, double pixelsPerSecond, double minSpacingPixels)
{
    if (steps.empty())
        return 1.0;

    if (pixelsPerSecond > 0.0)
        for (double step : steps)
            if (step * pixelsPerSecond >= minSpacingPixels)
                return step;

    return steps.back();
}

/** The finer step for the unlabelled lines between @p majorStep's labelled
    ones, and what clips snap to: the finest of @p steps at least
    @p minSpacingPixels apart that divides @p majorStep exactly, so every
    labelled line is also a minor line and snapping lands on the labels too.
    @p majorStep itself if nothing finer fits. */
inline double minorStep(const std::vector<double>& steps, double majorStep, double pixelsPerSecond,
                        double minSpacingPixels)
{
    if (pixelsPerSecond > 0.0)
        for (double step : steps)
            if (step <= majorStep && step * pixelsPerSecond >= minSpacingPixels
                && timeformatdetail::divides(step, majorStep))
                return step;

    return majorStep;
}

/** gridStep over the Minutes:Seconds steps. */
inline double secondsGridStep(double pixelsPerSecond, double minSpacingPixels)
{
    return gridStep(gridStepsFor({ TimeFormat::MinutesSeconds }), pixelsPerSecond, minSpacingPixels);
}

/** minorStep over the Minutes:Seconds steps. */
inline double secondsMinorStep(double majorStep, double pixelsPerSecond, double minSpacingPixels)
{
    return minorStep(gridStepsFor({ TimeFormat::MinutesSeconds }), majorStep, pixelsPerSecond, minSpacingPixels);
}

/** How many decimals a clock label needs to tell lines @p step seconds apart. */
inline int clockDecimalsForStep(double step)
{
    if (step >= 1.0)
        return 0;
    return step >= 0.1 ? 1 : 2;
}

/** How far apart a format's ruler labels have to be to fit: a sample count
    or a timecode is wider than a short clock reading. */
inline float minLabelSpacing(TimeFormat format)
{
    switch (format)
    {
        case TimeFormat::Samples:  return 96.0f;
        case TimeFormat::Timecode: return 88.0f;
        default:                   return 64.0f;
    }
}

/** A ruler label for a line at @p seconds, on a grid labelled every @p majorStep. */
inline std::string gridLabel(const TimeDisplay& display, double seconds, double majorStep)
{
    switch (display.format)
    {
        case TimeFormat::Samples:
            return formatSampleCount(std::llround(seconds * (display.sampleRate > 0.0 ? display.sampleRate : 48000.0)));
        case TimeFormat::Timecode:
            return formatTimecode(seconds, display.fps);
        default:
            return formatClockTime(seconds, clockDecimalsForStep(majorStep));
    }
}

/** A playhead position in @p display's format. Bars and beats need the tempo
    map rather than seconds, so that format reads as a clock here. */
inline std::string formatPosition(const TimeDisplay& display, double seconds)
{
    switch (display.format)
    {
        case TimeFormat::Samples:
            return formatSampleCount(std::llround(seconds * (display.sampleRate > 0.0 ? display.sampleRate : 48000.0)));
        case TimeFormat::Timecode:
            return formatTimecode(seconds, display.fps);
        default:
            return formatClockTime(seconds, 3);
    }
}

} // namespace soundsplice::app
