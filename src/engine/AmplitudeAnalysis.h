#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "engine/SilenceDetection.h"

namespace soundsplice::engine
{
/**
    The analyzers that read levels: Find Clipping (Audacity's, marking each
    run of samples at full scale) and Amplitude Statistics (Audition's peak,
    RMS, DC offset and dynamic range). Both take audio a chunk at a time, so a
    long recording is scanned without being held, and never allocate per
    sample. JUCE-free and tested headless; positions are frames from the start
    of what was scanned.
*/

/** Runs of consecutive samples at or over a threshold on any channel. A run
    counts once it's @p startRun samples long, and ends once @p stopRun samples
    in a row are back under the threshold, so a clipped stretch with a stray
    sample just under full scale in it is still one run. */
class ClippingDetector
{
public:
    ClippingDetector(float threshold = kDefaultThreshold, int startRun = 3, int stopRun = 3)
        : threshold_(threshold), startRun_(std::max(1, startRun)), stopRun_(std::max(1, stopRun))
    {
    }

    /** -0.01 dBFS: a sample written as full scale by any integer format, and
        the sample clamped just under it by float processing. */
    static constexpr float kDefaultThreshold = 0.9989f;

    void process(const float* const* channels, int numChannels, int frames)
    {
        for (int i = 0; i < frames; ++i, ++frame_)
        {
            bool clipped = false;
            for (int ch = 0; ch < numChannels && ! clipped; ++ch)
                clipped = std::abs(channels[ch][i]) >= threshold_;

            if (clipped)
            {
                if (streak_ == 0)
                    streakStart_ = frame_;
                ++streak_;
                calm_ = 0;
                if (! inRun_ && streak_ >= startRun_)
                {
                    inRun_    = true;
                    runStart_ = streakStart_;
                }
                if (inRun_)
                    lastClipped_ = frame_;
            }
            else
            {
                streak_ = 0;
                if (inRun_ && ++calm_ >= stopRun_)
                    closeRun();
            }
        }
    }

    /** Every run found, the last one closed if the audio ended inside it. */
    const std::vector<silence::FrameRange>& finish()
    {
        if (inRun_)
            closeRun();
        return runs_;
    }

private:
    void closeRun()
    {
        runs_.push_back({ runStart_, lastClipped_ + 1 });
        inRun_ = false;
        calm_  = 0;
    }

    float        threshold_;
    int          startRun_;
    int          stopRun_;
    std::int64_t frame_       = 0;
    int          streak_      = 0;
    std::int64_t streakStart_ = 0;
    bool         inRun_       = false;
    std::int64_t runStart_    = 0;
    std::int64_t lastClipped_ = 0;
    int          calm_        = 0;

    std::vector<silence::FrameRange> runs_;
};

/** Peak, RMS, DC offset and dynamic range of a passage, in dB where a level
    is a level. Dynamic range is the spread between the loudest and quietest
    50 ms windows' RMS, ignoring windows of digital silence (under -90 dB), as
    Audition reports it. */
class AmplitudeStatistics
{
public:
    static constexpr double kSilence = -std::numeric_limits<double>::infinity();

    void prepare(double sampleRate, int channels)
    {
        channels_     = std::clamp(channels, 1, kMaxChannels);
        windowFrames_ = std::max(1, (int) std::lround((sampleRate > 0.0 ? sampleRate : 48000.0) * 0.05));
    }

    void process(const float* const* data, int numChannels, int frames)
    {
        const int channels = std::min(numChannels, channels_);
        for (int i = 0; i < frames; ++i)
        {
            double frameSquares = 0.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                const double x = data[ch][i];
                peak_[ch]       = std::max(peak_[ch], std::abs(x));
                sum_[ch]       += x;
                squares_[ch]   += x * x;
                frameSquares   += x * x;
            }
            windowSquares_ += frameSquares / channels;
            ++frames_;

            if (++windowFill_ == windowFrames_)
                closeWindow();
        }
    }

    struct Report
    {
        std::int64_t frames         = 0;
        double       peakDb         = kSilence;    // the highest sample on any channel
        double       peakLeftDb     = kSilence;
        double       peakRightDb    = kSilence;    // the left's again for one channel
        double       rmsDb          = kSilence;    // over every sample of every channel
        double       dcOffsetPercent = 0.0;        // the channel mean furthest from zero, as % of full scale
        double       loudestWindowDb  = kSilence;
        double       quietestWindowDb = kSilence;
        double       dynamicRangeDb   = 0.0;
    };

    Report report() const
    {
        Report out;
        out.frames = frames_;
        if (frames_ == 0)
            return out;

        double squares = 0.0, peak = 0.0, dc = 0.0;
        for (int ch = 0; ch < channels_; ++ch)
        {
            squares += squares_[ch];
            peak     = std::max(peak, peak_[ch]);
            const double mean = sum_[ch] / (double) frames_;
            if (std::abs(mean) > std::abs(dc))
                dc = mean;
        }

        out.peakDb          = decibels(peak);
        out.peakLeftDb      = decibels(peak_[0]);
        out.peakRightDb     = decibels(peak_[channels_ > 1 ? 1 : 0]);
        out.rmsDb           = decibels(std::sqrt(squares / ((double) frames_ * channels_)));
        out.dcOffsetPercent = dc * 100.0;

        // A last partial window counts only if it's at least half a window.
        double loudest = loudest_, quietest = quietest_;
        if (windowFill_ * 2 >= windowFrames_)
            considerWindow(std::sqrt(windowSquares_ / windowFill_), loudest, quietest);

        out.loudestWindowDb  = decibels(loudest);
        out.quietestWindowDb = std::isfinite(quietest) ? decibels(quietest) : kSilence;
        out.dynamicRangeDb   = std::isfinite(out.loudestWindowDb) && std::isfinite(out.quietestWindowDb)
                                   ? out.loudestWindowDb - out.quietestWindowDb
                                   : 0.0;
        return out;
    }

private:
    static constexpr int    kMaxChannels = 2;
    static constexpr double kSilentWindow = 3.1622776601683795e-5; // -90 dB

    static double decibels(double linear) { return linear > 0.0 ? 20.0 * std::log10(linear) : kSilence; }

    static void considerWindow(double rms, double& loudest, double& quietest)
    {
        loudest = std::max(loudest, rms);
        if (rms >= kSilentWindow)
            quietest = std::min(quietest, rms);
    }

    void closeWindow()
    {
        considerWindow(std::sqrt(windowSquares_ / windowFill_), loudest_, quietest_);
        windowSquares_ = 0.0;
        windowFill_    = 0;
    }

    int          channels_     = 2;
    int          windowFrames_ = 2400;
    std::int64_t frames_       = 0;

    double peak_[kMaxChannels] {};
    double sum_[kMaxChannels] {};
    double squares_[kMaxChannels] {};

    double windowSquares_ = 0.0;
    int    windowFill_    = 0;
    double loudest_       = 0.0;
    double quietest_      = std::numeric_limits<double>::infinity();
};

namespace silence
{
    /** The sounds between @p silences in [0, @p totalFrames): each stretch
        that isn't silent and lasts at least @p minFrames. */
    inline std::vector<FrameRange> soundRuns(const std::vector<FrameRange>& silences, std::int64_t totalFrames,
                                             std::int64_t minFrames)
    {
        std::vector<FrameRange> sounds;
        std::int64_t            at = 0;

        const auto add = [&](std::int64_t from, std::int64_t to)
        {
            if (to - from >= std::max<std::int64_t>(1, minFrames))
                sounds.push_back({ from, to });
        };

        for (const auto& quiet : silences)
        {
            add(at, std::min(quiet.from, totalFrames));
            at = std::max(at, quiet.to);
        }
        add(at, totalFrames);
        return sounds;
    }
}

} // namespace soundsplice::engine
