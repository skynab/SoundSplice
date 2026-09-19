#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"
#include "engine/NoiseReduction.h"

namespace soundsplice::engine::noisereduction
{
/**
    Adaptive noise reduction (Audition's, iZotope's "adaptive" mode): noise
    reduction without a noise print. The noise at each frequency is taken to
    be the quietest that frequency has been lately, since speech and music
    come and go but hiss and hum are always there underneath: minimum
    statistics (after Martin), the lowest of each bin's smoothed magnitude
    over the last @p adaptSeconds, scaled up for the minimum's own bias. That
    estimate follows noise that changes, a fan speeding up or a new room, which
    a print taken once can't.

    It's primed on the opening seconds before processing from the start, so
    the beginning is cleaned with an estimate rather than a guess. What's then
    done with the estimate is exactly reduceNoise's: over-subtraction by
    @p reductionDb, never more than @p floorDb, weighted overlap-add.
*/
inline std::vector<float> reduceNoiseAdaptive(const std::vector<float>& samples, double sampleRate,
                                              float reductionDb = 12.0f, float floorDb = -18.0f,
                                              double adaptSeconds = 1.5)
{
    if (samples.empty() || sampleRate <= 0.0)
        return samples;

    constexpr int   kSize        = kDefaultFftSize;
    constexpr int   kHop         = kSize / 4;
    constexpr int   kBins        = kSize / 2 + 1;
    constexpr int   kSubwindows  = 8;
    constexpr float kSmoothing   = 0.7f;  // each bin's magnitude, frame to frame
    constexpr float kBias        = 1.6f;  // the minimum of a noisy magnitude sits below its mean

    const int framesPerSub = std::max(1, (int) std::lround(adaptSeconds * sampleRate / kHop / kSubwindows));

    const auto  window       = hannWindow(kSize);
    const float oversubtract = std::pow(10.0f, std::max(0.0f, reductionDb) / 20.0f);
    const float floorGain    = std::pow(10.0f, std::min(0.0f, floorDb) / 20.0f);

    std::vector<float> re((size_t) kSize), im((size_t) kSize);
    std::vector<float> smoothed((size_t) kBins, 0.0f);
    std::vector<float> subMin((size_t) kSubwindows * kBins, 1.0e30f); // each finished subwindow's minimum
    std::vector<float> current((size_t) kBins, 1.0e30f);             // the one filling now
    std::vector<float> noise((size_t) kBins, 0.0f);
    int                inSub = 0, subIndex = 0;
    bool               first = true;

    const auto analyse = [&](int start)
    {
        for (int i = 0; i < kSize; ++i)
        {
            const int   at     = start + i;
            const float sample = at < (int) samples.size() ? samples[(size_t) at] : 0.0f;
            re[(size_t) i]     = sample * window[(size_t) i];
            im[(size_t) i]     = 0.0f;
        }
        fft::transform(re, im, false);
    };

    // One frame's magnitudes into the tracker, and the estimate it gives.
    const auto track = [&]
    {
        for (int k = 0; k < kBins; ++k)
        {
            const float magnitude = std::hypot(re[(size_t) k], im[(size_t) k]);
            smoothed[(size_t) k]  = first ? magnitude : kSmoothing * smoothed[(size_t) k] + (1.0f - kSmoothing) * magnitude;
            current[(size_t) k]   = std::min(current[(size_t) k], smoothed[(size_t) k]);
        }
        first = false;

        if (++inSub == framesPerSub)
        {
            std::copy(current.begin(), current.end(), subMin.begin() + (long) subIndex * kBins);
            std::fill(current.begin(), current.end(), 1.0e30f);
            subIndex = (subIndex + 1) % kSubwindows;
            inSub    = 0;
        }

        for (int k = 0; k < kBins; ++k)
        {
            float lowest = current[(size_t) k];
            for (int s = 0; s < kSubwindows; ++s)
                lowest = std::min(lowest, subMin[(size_t) s * kBins + (size_t) k]);
            noise[(size_t) k] = lowest < 1.0e29f ? lowest * kBias : 0.0f;
        }
    };

    // Primed on the opening, up to twice the adaptation time.
    const int primeEnd = std::min((int) samples.size(), (int) (2.0 * adaptSeconds * sampleRate));
    for (int start = 0; start < primeEnd; start += kHop)
    {
        analyse(start);
        track();
    }

    std::vector<float> output(samples.size(), 0.0f);
    std::vector<float> windowSum(samples.size(), 0.0f);
    for (int start = 0; start < (int) samples.size(); start += kHop)
    {
        analyse(start);
        track();

        for (int k = 0; k < kBins; ++k)
        {
            const float mag = std::hypot(re[(size_t) k], im[(size_t) k]);
            if (mag <= 0.0f)
                continue;
            const float gain = std::clamp((mag - noise[(size_t) k] * oversubtract) / mag, floorGain, 1.0f);
            re[(size_t) k] *= gain;
            im[(size_t) k] *= gain;
            if (k > 0 && k < kSize - k)
            {
                re[(size_t) (kSize - k)] = re[(size_t) k];
                im[(size_t) (kSize - k)] = -im[(size_t) k];
            }
        }
        fft::transform(re, im, true);

        for (int i = 0; i < kSize; ++i)
        {
            const int at = start + i;
            if (at >= (int) samples.size())
                break;
            const float w = window[(size_t) i];
            output[(size_t) at]    += re[(size_t) i] * w;
            windowSum[(size_t) at] += w * w;
        }
    }

    // As reduceNoise does, and for the same reason: the edges only ever see
    // a window's taper.
    const float steady     = *std::max_element(windowSum.begin(), windowSum.end());
    const float floorValue = std::max(1.0e-6f, steady * 0.1f);
    for (size_t i = 0; i < output.size(); ++i)
        output[i] /= std::max(windowSum[i], floorValue);
    return output;
}

} // namespace soundsplice::engine::noisereduction
