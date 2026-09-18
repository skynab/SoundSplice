#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine
{
/**
    A spectrogram of a clip: its level at each frequency over time, for the
    audio editor's spectrogram view (the start of spectral editing, as
    Audacity, Audition and REAPER all show it).

    Built a chunk at a time as the clip is read, as the waveform's peaks are,
    so a long recording never has to be in memory whole. The channels are
    mixed to one, the columns spaced so that there are at most a few thousand
    whatever the clip's length, and each is a Hann-windowed transform's level
    per bin in dB. JUCE-free, so its frequency and time placement are tested
    headless.
*/
struct SpectrogramData
{
    int                columns          = 0;
    int                bins             = 0;       // fftSize / 2 + 1, DC first
    double             sampleRate       = 0.0;
    double             secondsPerColumn = 0.0;     // column c is centred on c * secondsPerColumn + window / 2
    double             windowSeconds    = 0.0;
    std::vector<float> levelsDb;                   // columns * bins, column by column

    bool isEmpty() const noexcept { return columns == 0 || bins == 0; }

    float levelDb(int column, int bin) const noexcept
    {
        return levelsDb[(size_t) column * (size_t) bins + (size_t) bin];
    }

    double frequencyOfBin(double bin) const noexcept
    {
        return bins > 1 ? bin * sampleRate * 0.5 / (double) (bins - 1) : 0.0;
    }

    /** The (fractional) bin at @p hz. */
    double binOfFrequency(double hz) const noexcept
    {
        return sampleRate > 0.0 ? hz * (double) (bins - 1) / (sampleRate * 0.5) : 0.0;
    }
};

class SpectrogramBuilder
{
public:
    static constexpr int    kFftSize    = 2048;
    static constexpr int    kMaxColumns = 4096;
    static constexpr float  kFloorDb    = -120.0f;

    /** For @p totalFrames frames at @p sampleRate, to be appended in order. */
    SpectrogramBuilder(double sampleRate, std::int64_t totalFrames)
    {
        data_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        data_.bins       = kFftSize / 2 + 1;

        // A quarter-window hop, widened for a long clip so the column count
        // stays bounded: past a few thousand, a column is narrower than a
        // pixel on any screen.
        hop_ = std::max<std::int64_t>(kFftSize / 4, totalFrames / kMaxColumns + 1);
        data_.secondsPerColumn = (double) hop_ / data_.sampleRate;
        data_.windowSeconds    = (double) kFftSize / data_.sampleRate;

        window_.resize(kFftSize);
        windowSum_ = 0.0;
        for (int i = 0; i < kFftSize; ++i)
        {
            window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / (kFftSize - 1)));
            windowSum_ += window_[(size_t) i];
        }

        re_.resize(kFftSize);
        im_.resize(kFftSize);
    }

    /** The next @p frames of @p numChannels channels, mixed to one. */
    void append(const float* const* channels, int numChannels, int frames)
    {
        const int channelCount = std::max(1, numChannels);
        for (int i = 0; i < frames; ++i)
        {
            float mixed = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                mixed += channels[ch][i];
            pending_.push_back(mixed / (float) channelCount);
            ++position_;

            // A window is ready when the audio reaches its end.
            // More audio than was promised adds nothing past the cap.
            if ((std::int64_t) pending_.size() >= kFftSize && position_ - kFftSize == nextColumnStart_
                && data_.columns < kMaxColumns)
            {
                addColumn(pending_.data() + pending_.size() - kFftSize);
                nextColumnStart_ += hop_;
            }

            // Only what a later window can still need is kept: everything
            // from where the next one starts. Trimmed in batches, not a frame
            // at a time. (With a hop longer than a window, the frames between
            // windows are never needed at all.)
            const auto firstHeld = position_ - (std::int64_t) pending_.size();
            const auto unneeded  = std::min<std::int64_t>(nextColumnStart_ - firstHeld, (std::int64_t) pending_.size());
            if (unneeded > (std::int64_t) kFftSize * 4)
                pending_.erase(pending_.begin(), pending_.begin() + unneeded);
        }
    }

    SpectrogramData finish() { return std::move(data_); }

private:
    void addColumn(const float* samples)
    {
        for (int i = 0; i < kFftSize; ++i)
        {
            re_[(size_t) i] = samples[i] * window_[(size_t) i];
            im_[(size_t) i] = 0.0f;
        }
        fft::transform(re_, im_, false);

        // Scaled so a full-scale sine reads about 0 dB in its bin.
        const double scale = 2.0 / windowSum_;
        for (int k = 0; k < data_.bins; ++k)
        {
            const double magnitude = std::hypot((double) re_[(size_t) k], (double) im_[(size_t) k]) * scale;
            data_.levelsDb.push_back(magnitude > 1.0e-6 ? (float) (20.0 * std::log10(magnitude)) : kFloorDb);
        }
        ++data_.columns;
    }

    SpectrogramData    data_;
    std::int64_t       hop_             = kFftSize / 4;
    std::int64_t       position_        = 0;  // frames appended so far
    std::int64_t       nextColumnStart_ = 0;  // the frame the next window starts at
    std::vector<float> pending_;              // the latest frames, ending at position_
    std::vector<float> window_;
    double             windowSum_ = 1.0;
    std::vector<float> re_, im_;
};

} // namespace soundsplice::engine
