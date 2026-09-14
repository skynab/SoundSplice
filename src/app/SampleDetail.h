#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "WaveformPeaks.h"

namespace soundsplice
{
/**
    The actual samples of a short stretch of a clip, for drawing the audio
    editor zoomed in past what its peaks can show.

    WaveformPeaks summarises every 64 samples as one bin, which is exactly
    right until a pixel covers fewer samples than that; zoomed in further, the
    waveform turns into flat-topped blocks, and the one thing a deep zoom is
    for (finding a click, placing an edge between two samples) becomes
    impossible. So once the view is that close, the editor asks for the
    visible samples themselves, which are few enough at that zoom to read on
    the spot.

    JUCE-free, so which samples a pixel covers is tested on its own.
*/
struct SampleDetail
{
    /** Pixels covering this many samples or fewer are drawn from samples
        rather than peaks: half a peaks bin, so the switch happens before the
        blocks can be seen. */
    static constexpr double kMaxSamplesPerPixel = 32.0;

    double                          startSeconds = 0.0; // where channels[.][0] is, into the clip
    double                          sampleRate   = 0.0;
    std::vector<std::vector<float>> channels;

    bool isEmpty() const { return channels.empty() || channels[0].empty() || sampleRate <= 0.0; }

    /** Whether a view with @p secondsPerPixel at @p sampleRate is close
        enough to want samples. */
    static bool wanted(double secondsPerPixel, double rate)
    {
        return rate > 0.0 && secondsPerPixel > 0.0 && secondsPerPixel * rate <= kMaxSamplesPerPixel;
    }

    double endSeconds() const
    {
        return isEmpty() ? startSeconds : startSeconds + (double) channels[0].size() / sampleRate;
    }

    /** Whether these samples cover all of [@p fromSeconds, @p toSeconds),
        clamped to @p clipSeconds: the part of a view past the end of the clip
        has no samples to need. */
    bool covers(double fromSeconds, double toSeconds, double clipSeconds) const
    {
        if (isEmpty())
            return false;

        const double from = std::max(0.0, fromSeconds);
        const double to   = std::min(toSeconds, clipSeconds);
        const double tolerance = 1.0 / sampleRate;
        return to <= from || (from >= startSeconds - tolerance && to <= endSeconds() + tolerance);
    }

    /** Index into channels of the sample at @p seconds into the clip (not
        clamped). */
    long indexAt(double seconds) const
    {
        return (long) std::floor((seconds - startSeconds) * sampleRate);
    }

    /** The sample @p index of @p channel (a missing channel repeats the last
        one), or nothing outside what's held. */
    bool sampleAt(int channel, long index, float& out) const
    {
        if (isEmpty() || index < 0)
            return false;

        const auto& samples = channels[(size_t) std::clamp(channel, 0, (int) channels.size() - 1)];
        if (index >= (long) samples.size())
            return false;

        out = samples[(size_t) index];
        return true;
    }

    /** The extremes of @p channel over [@p fromSeconds, @p toSeconds): every
        sample whose position falls in it, or the one sample under it if none
        does. Empty outside what's held. */
    PeakBin range(int channel, double fromSeconds, double toSeconds) const
    {
        if (isEmpty())
            return {};

        const auto& samples = channels[(size_t) std::clamp(channel, 0, (int) channels.size() - 1)];
        const long  size    = (long) samples.size();

        long first = (long) std::ceil((fromSeconds - startSeconds) * sampleRate);
        long last  = (long) std::ceil((toSeconds - startSeconds) * sampleRate) - 1;
        if (last < first)
            first = last = indexAt(fromSeconds);

        first = std::max(0L, first);
        last  = std::min(size - 1, last);
        if (last < first)
            return {};

        PeakBin bin { samples[(size_t) first], samples[(size_t) first] };
        for (long i = first + 1; i <= last; ++i)
        {
            bin.minimum = std::min(bin.minimum, samples[(size_t) i]);
            bin.maximum = std::max(bin.maximum, samples[(size_t) i]);
        }
        return bin;
    }
};

} // namespace soundsplice
