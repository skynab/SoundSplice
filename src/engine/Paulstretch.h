#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine::timestretch
{
/**
    Paulstretch: extreme time stretching, as Paul Nasca's tool and Audacity's
    effect do it. Each long window is transformed, its phases thrown away and
    replaced with random ones, and the windows are laid back down further
    apart than they were taken.

    Losing the phase is the point. A phase vocoder holds a sound together so
    that stretching it a little sounds like the same performance slower; past
    a few times over, keeping that structure is what makes it sound
    metallic. Paulstretch keeps only the spectrum over time, which turns any
    material into a smooth pad — useless for a small change, and the only
    thing that works for fifty times over.

    JUCE-free, and deterministic: the same audio and settings give the same
    result, random phases included.
*/
inline std::vector<float> paulstretch(const std::vector<float>& samples, double stretch, double windowSeconds,
                                      double sampleRate, std::uint32_t seed = 0x5EED5EEDu)
{
    if (stretch <= 0.0 || samples.empty() || sampleRate <= 0.0)
        return samples;

    // A power of two at least as long as the window asked for: the transform
    // wants one, and a longer window is a smoother, more washed-out stretch.
    int windowSize = 256;
    while ((double) windowSize < windowSeconds * sampleRate && windowSize < (1 << 17))
        windowSize *= 2;
    if ((int) samples.size() < windowSize)
        return samples;

    const int    bins         = windowSize / 2 + 1;
    const int    synthesisHop = windowSize / 2;
    const double analysisHop  = std::max(1.0, (double) synthesisHop / stretch);

    // Paulstretch's own window, (1 - x^2)^1.25 across the frame: flatter in
    // the middle than a Hann and still zero at both ends.
    std::vector<float> window((size_t) windowSize);
    double             meanSquare = 0.0;
    for (int i = 0; i < windowSize; ++i)
    {
        const double x = 2.0 * (double) i / (double) (windowSize - 1) - 1.0;
        window[(size_t) i] = (float) std::pow(std::max(0.0, 1.0 - x * x), 1.25);
        meanSquare += (double) window[(size_t) i] * window[(size_t) i];
    }
    meanSquare /= (double) windowSize;

    const auto outLength = (std::size_t) std::llround((double) samples.size() * stretch) + (std::size_t) windowSize;
    std::vector<float> output(outLength, 0.0f);
    std::vector<float> windowSum(outLength, 0.0f);
    std::vector<float> re((size_t) windowSize), im((size_t) windowSize);

    std::uint32_t random = seed;
    const auto    nextPhase = [&random]
    {
        random = random * 1664525u + 1013904223u;
        return 2.0 * fft::kPi * (double) (random >> 8) / (double) (1u << 24);
    };

    for (int frame = 0;; ++frame)
    {
        const auto start = (std::int64_t) std::llround((double) frame * analysisHop);
        if (start + windowSize > (std::int64_t) samples.size())
            break;

        for (int i = 0; i < windowSize; ++i)
        {
            re[(size_t) i] = samples[(size_t) (start + i)] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }

        fft::transform(re, im, false);

        for (int k = 0; k < bins; ++k)
        {
            const double magnitude = std::hypot((double) re[(size_t) k], (double) im[(size_t) k]);
            const double phase     = nextPhase();

            re[(size_t) k] = (float) (magnitude * std::cos(phase));
            im[(size_t) k] = (float) (magnitude * std::sin(phase));

            // The mirror half, so the inverse transform is real.
            if (k > 0 && k < windowSize - k)
            {
                re[(size_t) (windowSize - k)] = re[(size_t) k];
                im[(size_t) (windowSize - k)] = -im[(size_t) k];
            }
        }

        fft::transform(re, im, true);

        const auto at = (std::size_t) frame * (std::size_t) synthesisHop;
        for (int i = 0; i < windowSize && at + (std::size_t) i < output.size(); ++i)
        {
            output[at + (size_t) i]    += re[(size_t) i] * window[(size_t) i];
            windowSum[at + (size_t) i] += window[(size_t) i] * window[(size_t) i];
        }
    }

    // Random phases make the frames add as power rather than amplitude, so
    // the level is held by dividing by the square root of the overlap rather
    // than the overlap itself. The floor keeps the fade at each end from
    // being divided back up into a burst.
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] /= (float) std::max(0.05, std::sqrt((double) windowSum[i] * meanSquare));

    output.resize((std::size_t) std::max<std::int64_t>(1, std::llround((double) samples.size() * stretch)));
    return output;
}

} // namespace soundsplice::engine::timestretch
