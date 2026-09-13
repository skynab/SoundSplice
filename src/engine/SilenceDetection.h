#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace soundsplice::engine::silence
{
/**
    Finding where audio falls silent, for Detach at Silences (and later a
    silence finder and Truncate Silence).

    Audio is summarised as the loudest sample in each short window, across all
    channels, so a long recording is read a chunk at a time and never needs to
    be in memory whole; then runs of windows below a threshold that last long
    enough count as silence. A window rather than single samples, because a
    waveform crosses zero hundreds of times a second and every one of those
    would otherwise be a "silence".

    JUCE-free, so the window and run boundaries are tested on their own.
    Positions are frames, half-open [from, to).
*/

struct FrameRange
{
    std::int64_t from = 0;
    std::int64_t to   = 0;

    bool operator==(const FrameRange&) const = default;
};

/** The loudest sample in each window of @p windowFrames frames, built from
    audio handed over a chunk at a time. */
class PeakEnvelope
{
public:
    explicit PeakEnvelope(int windowFrames) : windowFrames_(std::max(1, windowFrames)) {}

    /** Adds @p frames frames of @p numChannels channels. */
    void append(const float* const* channels, int numChannels, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                if (channels[ch] != nullptr)
                    current_ = std::max(current_, std::abs(channels[ch][i]));

            if (++filled_ == windowFrames_)
            {
                peaks_.push_back(current_);
                current_ = 0.0f;
                filled_  = 0;
            }
        }

        totalFrames_ += std::max(0, frames);
    }

    /** The envelope, with a partly filled last window counted as a window. */
    const std::vector<float>& finish()
    {
        if (filled_ > 0)
        {
            peaks_.push_back(current_);
            current_ = 0.0f;
            filled_  = 0;
        }
        return peaks_;
    }

    int          windowFrames() const { return windowFrames_; }
    std::int64_t totalFrames() const { return totalFrames_; }

private:
    int                windowFrames_;
    std::vector<float> peaks_;
    float              current_     = 0.0f;
    int                filled_      = 0;
    std::int64_t       totalFrames_ = 0;
};

inline float gainForDecibels(float decibels)
{
    return std::pow(10.0f, decibels / 20.0f);
}

/** The stretches where every window of @p windowPeaks stays below
    @p threshold (a linear gain) for at least @p minFrames frames, clamped to
    @p totalFrames. */
inline std::vector<FrameRange> silentRuns(const std::vector<float>& windowPeaks, int windowFrames,
                                          std::int64_t totalFrames, float threshold, std::int64_t minFrames)
{
    std::vector<FrameRange> runs;
    const auto window = (std::int64_t) std::max(1, windowFrames);

    const auto close = [&runs, totalFrames, minFrames](std::int64_t from, std::int64_t to)
    {
        to = std::min(to, totalFrames);
        if (to - from >= std::max<std::int64_t>(1, minFrames))
            runs.push_back({ from, to });
    };

    std::int64_t runStart = -1;
    for (size_t w = 0; w < windowPeaks.size(); ++w)
    {
        const bool silent = windowPeaks[w] < threshold;

        if (silent && runStart < 0)
            runStart = (std::int64_t) w * window;
        else if (! silent && runStart >= 0)
        {
            close(runStart, (std::int64_t) w * window);
            runStart = -1;
        }
    }

    if (runStart >= 0)
        close(runStart, totalFrames);

    return runs;
}

} // namespace soundsplice::engine::silence
