#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Fft.h"
#include "engine/ThirdOctaveEq.h"

namespace soundsplice::engine
{
/**
    Match EQ (Audition's, REAPER's ReaEQ "match"): what one recording's
    average spectrum would need to sound like another's, as the gains of a
    31-band graphic EQ.

    Each recording is measured by SpectrumAverager, a chunk at a time so a
    long one needn't be in memory, into its mean power in each third-octave
    band. The match is the difference, band by band, smoothed across
    neighbours, with its average taken out so the overall level stays where
    it was (loudness is a separate step), and held within the EQ's range.
    JUCE-free, so it's tested headless.
*/
class SpectrumAverager
{
public:
    static constexpr int kSize  = 4096;
    static constexpr int kBands = ThirdOctaveEq::kBands;

    explicit SpectrumAverager(double sampleRate)
        : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0),
          power_((size_t) kSize / 2 + 1, 0.0),
          window_((size_t) kSize),
          re_((size_t) kSize),
          im_((size_t) kSize)
    {
        for (int i = 0; i < kSize; ++i)
            window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / kSize));
        pending_.reserve((size_t) kSize);
    }

    /** The next @p frames of @p channels channels, mixed to one. */
    void append(const std::vector<std::vector<float>>& channels)
    {
        if (channels.empty())
            return;
        const auto frames = channels[0].size();
        for (size_t i = 0; i < frames; ++i)
        {
            float mixed = 0.0f;
            for (const auto& channel : channels)
                mixed += i < channel.size() ? channel[i] : 0.0f;
            pending_.push_back(mixed / (float) channels.size());
            if ((int) pending_.size() == kSize)
            {
                addFrame();
                pending_.clear();
            }
        }
    }

    bool isEmpty() const noexcept { return frames_ == 0; }
    double sampleRate() const noexcept { return sampleRate_; }

    /** Mean power in each third-octave band, in dB. A band too narrow to
        hold a bin takes the nearest one's. */
    std::array<double, kBands> bandLevelsDb() const
    {
        std::array<double, kBands> levels {};
        const double binHz = sampleRate_ / kSize;
        for (int b = 0; b < kBands; ++b)
        {
            const double centre = ThirdOctaveEq::kCentres[(size_t) b];
            const int    low    = (int) std::ceil(centre * std::pow(2.0, -1.0 / 6.0) / binHz);
            const int    high   = (int) std::floor(centre * std::pow(2.0, 1.0 / 6.0) / binHz);
            const int    last   = (int) power_.size() - 1;

            double sum = 0.0;
            int    bins = 0;
            for (int k = std::max(1, low); k <= std::min(high, last); ++k, ++bins)
                sum += power_[(size_t) k];
            if (bins == 0)
            {
                sum  = power_[(size_t) std::clamp((int) std::lround(centre / binHz), 1, last)];
                bins = 1;
            }
            levels[(size_t) b] = 10.0 * std::log10(sum / bins / std::max<std::int64_t>(1, frames_) + 1.0e-20);
        }
        return levels;
    }

private:
    void addFrame()
    {
        for (int i = 0; i < kSize; ++i)
        {
            re_[(size_t) i] = pending_[(size_t) i] * window_[(size_t) i];
            im_[(size_t) i] = 0.0f;
        }
        fft::transform(re_, im_, false);
        for (size_t k = 0; k < power_.size(); ++k)
            power_[k] += (double) re_[k] * re_[k] + (double) im_[k] * im_[k];
        ++frames_;
    }

    double              sampleRate_;
    std::vector<double> power_;
    std::vector<float>  window_, re_, im_;
    std::vector<float>  pending_;
    std::int64_t        frames_ = 0;
};

namespace matcheq
{
    constexpr float kRangeDb = 12.0f;

    /** The 31-band gains that bring @p target's average spectrum towards
        @p reference's. Bands either recording barely has (more than 60 dB
        under its loudest band), or that are past what @p sampleRate carries,
        are left at 0 rather than boosted towards noise. Rounded to half a
        decibel, the graphic EQ's step. */
    inline std::array<float, ThirdOctaveEq::kBands> gains(const std::array<double, ThirdOctaveEq::kBands>& reference,
                                                        const std::array<double, ThirdOctaveEq::kBands>& target,
                                                        double sampleRate)
    {
        constexpr int kBands = ThirdOctaveEq::kBands;
        const double  refTop = *std::max_element(reference.begin(), reference.end());
        const double  tgtTop = *std::max_element(target.begin(), target.end());

        std::array<bool, kBands>   valid {};
        std::array<double, kBands> diff {};
        for (int b = 0; b < kBands; ++b)
        {
            valid[(size_t) b] = ThirdOctaveEq::kCentres[(size_t) b] < sampleRate * 0.45
                             && reference[(size_t) b] > refTop - 60.0 && target[(size_t) b] > tgtTop - 60.0;
            diff[(size_t) b] = reference[(size_t) b] - target[(size_t) b];
        }

        // Smoothed over each band and its neighbours, so one band's accident
        // of measurement doesn't become a spike in the curve.
        std::array<double, kBands> smooth {};
        double                     total = 0.0;
        int                        count = 0;
        for (int b = 0; b < kBands; ++b)
        {
            if (! valid[(size_t) b])
                continue;
            double sum = 0.0, weight = 0.0;
            for (int n = std::max(0, b - 1); n <= std::min(kBands - 1, b + 1); ++n)
                if (valid[(size_t) n])
                {
                    const double w = n == b ? 2.0 : 1.0;
                    sum += diff[(size_t) n] * w;
                    weight += w;
                }
            smooth[(size_t) b] = sum / weight;
            total += smooth[(size_t) b];
            ++count;
        }

        std::array<float, kBands> result {};
        const double              mean = count > 0 ? total / count : 0.0;
        for (int b = 0; b < kBands; ++b)
            if (valid[(size_t) b])
                result[(size_t) b] = std::clamp(std::round((float) (smooth[(size_t) b] - mean) * 2.0f) / 2.0f,
                                                -kRangeDb, kRangeDb);
        return result;
    }
}

} // namespace soundsplice::engine
