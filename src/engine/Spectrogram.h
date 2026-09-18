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
    whatever the clip's length, and each is a windowed transform's level per
    bin in dB (Hann, 2048 points, unless SpectrogramSettings says otherwise). JUCE-free, so its frequency and time placement are tested
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

/** The window a spectrogram's columns are measured through. The values are
    stored in the app's settings, so they are part of that format. */
enum class SpectrogramWindow
{
    Hann           = 0,
    Hamming        = 1,
    BlackmanHarris = 2,
    Rectangular    = 3
};

/** How a spectrogram is measured, as Audacity's spectrogram settings offer
    it: a longer window resolves nearby frequencies (a bass line's notes) at
    the cost of blurring quick events (a drum hit) in time, and the window's
    shape trades how sharp a pure tone's line is against how far it leaks. */
struct SpectrogramSettings
{
    static constexpr int kSmallestFft = 256;
    static constexpr int kLargestFft  = 16384;

    int               fftSize = 2048;
    SpectrogramWindow window  = SpectrogramWindow::Hann;

    /** @p size rounded to the nearest power of two in range. */
    static int validFftSize(int size)
    {
        int best = kSmallestFft;
        for (int candidate = kSmallestFft; candidate <= kLargestFft; candidate *= 2)
            if (std::abs(std::log2((double) std::max(1, size) / candidate))
                < std::abs(std::log2((double) std::max(1, size) / best)))
                best = candidate;
        return best;
    }

    /** The window's @p size coefficients, symmetric. */
    static std::vector<float> coefficients(SpectrogramWindow shape, int size)
    {
        std::vector<float> w((size_t) std::max(1, size), 1.0f);
        const double       n = std::max(1, size - 1);
        for (int i = 0; i < size; ++i)
        {
            const double x = 2.0 * fft::kPi * i / n;
            switch (shape)
            {
                case SpectrogramWindow::Hamming: w[(size_t) i] = (float) (0.54 - 0.46 * std::cos(x)); break;
                case SpectrogramWindow::BlackmanHarris:
                    w[(size_t) i] = (float) (0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x)
                                             - 0.01168 * std::cos(3.0 * x));
                    break;
                case SpectrogramWindow::Rectangular: w[(size_t) i] = 1.0f; break;
                case SpectrogramWindow::Hann:
                default: w[(size_t) i] = (float) (0.5 - 0.5 * std::cos(x)); break;
            }
        }
        return w;
    }
};

class SpectrogramBuilder
{
public:
    static constexpr int    kFftSize    = 2048; // the default window
    static constexpr int    kMaxColumns = 4096;
    static constexpr float  kFloorDb    = -120.0f;

    /** For @p totalFrames frames at @p sampleRate, to be appended in order. */
    SpectrogramBuilder(double sampleRate, std::int64_t totalFrames, SpectrogramSettings settings = {})
        : fftSize_(SpectrogramSettings::validFftSize(settings.fftSize))
    {
        data_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        data_.bins       = fftSize_ / 2 + 1;

        // A quarter-window hop, widened for a long clip so the column count
        // stays bounded: past a few thousand, a column is narrower than a
        // pixel on any screen.
        hop_ = std::max<std::int64_t>(fftSize_ / 4, totalFrames / kMaxColumns + 1);
        data_.secondsPerColumn = (double) hop_ / data_.sampleRate;
        data_.windowSeconds    = (double) fftSize_ / data_.sampleRate;

        window_    = SpectrogramSettings::coefficients(settings.window, fftSize_);
        windowSum_ = 0.0;
        for (float w : window_)
            windowSum_ += w;

        re_.resize((size_t) fftSize_);
        im_.resize((size_t) fftSize_);
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
            if ((std::int64_t) pending_.size() >= fftSize_ && position_ - fftSize_ == nextColumnStart_
                && data_.columns < kMaxColumns)
            {
                addColumn(pending_.data() + pending_.size() - fftSize_);
                nextColumnStart_ += hop_;
            }

            // Only what a later window can still need is kept: everything
            // from where the next one starts. Trimmed in batches, not a frame
            // at a time. (With a hop longer than a window, the frames between
            // windows are never needed at all.)
            const auto firstHeld = position_ - (std::int64_t) pending_.size();
            const auto unneeded  = std::min<std::int64_t>(nextColumnStart_ - firstHeld, (std::int64_t) pending_.size());
            if (unneeded > (std::int64_t) fftSize_ * 4)
                pending_.erase(pending_.begin(), pending_.begin() + unneeded);
        }
    }

    SpectrogramData finish() { return std::move(data_); }

private:
    void addColumn(const float* samples)
    {
        for (int i = 0; i < fftSize_; ++i)
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
    int                fftSize_         = kFftSize;
    std::int64_t       hop_             = kFftSize / 4;
    std::int64_t       position_        = 0;  // frames appended so far
    std::int64_t       nextColumnStart_ = 0;  // the frame the next window starts at
    std::vector<float> pending_;              // the latest frames, ending at position_
    std::vector<float> window_;
    double             windowSum_ = 1.0;
    std::vector<float> re_, im_;
};

} // namespace soundsplice::engine
