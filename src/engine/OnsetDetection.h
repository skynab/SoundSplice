#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine
{
/**
    Beat finding (Audacity's Beat Finder, Audition's Beat markers): where each
    new sound starts, found by spectral flux, how much louder the spectrum
    got from one short frame to the next, summed over frequency. A drum hit
    or a plucked note jumps across many bins at once; a held note, however
    loud, doesn't. A frame counts as an onset when its flux is a local peak
    standing clear of the flux around it (so a busy passage doesn't read as
    one long beat), and at least a minimum gap after the last.

    Fed a chunk at a time, so a long recording needn't be in memory, and the
    tempo is estimated from the same flux by autocorrelation. JUCE-free, so
    it's tested headless.
*/
class OnsetDetector
{
public:
    static constexpr int kSize = 1024;
    static constexpr int kHop  = 256;

    explicit OnsetDetector(double sampleRate)
        : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0),
          window_((size_t) kSize),
          re_((size_t) kSize),
          im_((size_t) kSize),
          previous_((size_t) kSize / 2 + 1, 0.0f),
          pending_((size_t) kSize, 0.0f)
    {
        for (int i = 0; i < kSize; ++i)
            window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / kSize));
    }

    /** The next @p frames of @p channels channels, mixed to one. */
    void append(const float* const* channels, int numChannels, int frames)
    {
        const int count = std::max(1, numChannels);
        for (int i = 0; i < frames; ++i)
        {
            float mixed = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                mixed += channels[ch][i];
            pending_[(size_t) filled_++] = mixed / (float) count;

            if (filled_ == kSize)
            {
                addFrame();
                std::copy(pending_.begin() + kHop, pending_.end(), pending_.begin());
                filled_ = kSize - kHop;
            }
        }
    }

    /** Each onset, as a frame counted from the first appended: the middle
        of the analysis frame whose flux peaks as the sound begins.
        @p sensitivity 0..1: at 1 the softest onsets count, at 0 only those
        near the loudest; @p minGapSeconds keeps a flam from reading as two. */
    std::vector<std::int64_t> onsets(double sensitivity = 0.5, double minGapSeconds = 0.1) const
    {
        std::vector<std::int64_t> found;
        const int                 n = (int) flux_.size();
        if (n < 3)
            return found;

        // A frame's flux is compared with the mean and spread of its
        // neighbourhood, half a second either side.
        const int    reach  = std::max(1, (int) (0.5 * sampleRate_ / kHop));
        const double keen   = std::clamp(sensitivity, 0.0, 1.0);
        const double factor = 0.5 + 3.0 * (1.0 - keen);
        // The log scale the flux is measured on brings soft onsets near loud
        // ones, so sensitivity also sets how close to the loudest they must
        // come: within about a quarter by default.
        const double floor  = 0.9 * (1.0 - keen) * (1.0 - keen) + 0.02 * keen;
        const auto   minGap = (std::int64_t) (minGapSeconds * sampleRate_);

        double peakFlux = 0.0;
        for (double f : flux_)
            peakFlux = std::max(peakFlux, f);

        // Running sums, so each frame's neighbourhood is O(1).
        std::vector<double> sum((size_t) n + 1, 0.0), squares((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
        {
            sum[(size_t) i + 1]     = sum[(size_t) i] + flux_[(size_t) i];
            squares[(size_t) i + 1] = squares[(size_t) i] + flux_[(size_t) i] * flux_[(size_t) i];
        }

        for (int i = 1; i < n - 1; ++i)
        {
            const double f = flux_[(size_t) i];
            if (f < flux_[(size_t) i - 1] || f < flux_[(size_t) i + 1] || f < peakFlux * floor)
                continue;

            const int    a      = std::max(0, i - reach), b = std::min(n, i + reach + 1);
            const double count  = (double) (b - a);
            const double mean   = (sum[(size_t) b] - sum[(size_t) a]) / count;
            const double spread = std::sqrt(std::max(0.0, (squares[(size_t) b] - squares[(size_t) a]) / count - mean * mean));
            if (f <= mean + factor * spread)
                continue;

            const auto at = (std::int64_t) i * kHop + kSize / 2; // the frame's middle, where its window weighs most
            if (! found.empty() && at - found.back() < minGap)
                continue;
            found.push_back(at);
        }
        return found;
    }

    /** The tempo the onsets keep, in beats per minute between 60 and 200,
        or 0 when there's no steady pulse: the lag at which the flux best
        matches itself. */
    double tempoBpm() const
    {
        const int n = (int) flux_.size();
        if (n < 16)
            return 0.0;

        double mean = 0.0;
        for (double f : flux_)
            mean += f;
        mean /= n;

        const double framesPerSecond = sampleRate_ / kHop;
        const int    shortest        = std::max(1, (int) std::floor(framesPerSecond * 60.0 / 200.0));
        const int    longest         = std::min(n / 2, (int) std::ceil(framesPerSecond * 60.0 / 60.0));

        double zeroLag = 0.0;
        for (double f : flux_)
            zeroLag += (f - mean) * (f - mean);
        if (zeroLag <= 0.0)
            return 0.0;

        int    bestLag   = 0;
        double bestScore = 0.0;
        for (int lag = shortest; lag <= longest; ++lag)
        {
            double score = 0.0;
            for (int i = lag; i < n; ++i)
                score += (flux_[(size_t) i] - mean) * (flux_[(size_t) i - lag] - mean);
            score /= (double) (n - lag);
            if (score > bestScore)
            {
                bestScore = score;
                bestLag   = lag;
            }
        }

        // Too weak a match, or too few onsets to keep one, is no pulse at all.
        if (bestLag == 0 || bestScore < 0.3 * zeroLag / n || onsets(0.5, 0.1).size() < 4)
            return 0.0;

        // Refined between frames by the parabola through the best lag and
        // its neighbours.
        const auto   at      = [&](int lag)
        {
            double score = 0.0;
            for (int i = lag; i < n; ++i)
                score += (flux_[(size_t) i] - mean) * (flux_[(size_t) i - lag] - mean);
            return score / (double) (n - lag);
        };
        const double a = at(bestLag - 1), b = bestScore, c = at(bestLag + 1);
        const double shift = (a - 2.0 * b + c) < 0.0 ? 0.5 * (a - c) / (a - 2.0 * b + c) : 0.0;
        return 60.0 * framesPerSecond / (bestLag + std::clamp(shift, -0.5, 0.5));
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

        // Log magnitude, so a quiet onset in a quiet passage counts as much
        // as a loud one in a loud passage; only rises count.
        double flux = 0.0;
        for (size_t k = 1; k < previous_.size(); ++k)
        {
            const float magnitude = std::log1p(1000.0f * std::hypot(re_[k], im_[k]));
            flux += std::max(0.0f, magnitude - previous_[k]);
            previous_[k] = magnitude;
        }
        // The first frame has nothing before it: whatever's already sounding
        // when the audio starts isn't a sound starting.
        flux_.push_back(flux_.empty() && ! primed_ ? 0.0 : flux);
        primed_ = true;
    }

    double              sampleRate_;
    std::vector<float>  window_, re_, im_, previous_, pending_;
    int                 filled_ = 0;
    bool                primed_ = false;
    std::vector<double> flux_; // one per hop
};

} // namespace soundsplice::engine
