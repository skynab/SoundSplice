#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"
#include "engine/NoiseReduction.h"

namespace soundsplice::engine::dereverb
{
/**
    De-reverb (Audition's DeReverb): a room's tail taken out of a recording
    made in it. After Lebart, Boucher and Denbigh: a room's late reverb decays
    exponentially, by 60 dB over its reverb time, so the reverb in each
    frequency now is about what was there a moment ago, faded by as much as
    the room fades it in that moment. That estimate is subtracted from each
    frame's power, never taking a bin below @p floorDb, and the early sound,
    the voice itself, is left, since it's louder than what the room returns.

    Weighted overlap-add at a quarter hop, as the noise reduction, so it's a
    transparent round trip at no amount. JUCE-free, so how far it takes a
    tail down, and what it leaves, is tested headless.
*/
struct Settings
{
    double reverbSeconds = 0.8;   // the room's RT60
    // How much of the estimate comes off. A tail's level wanders about the
    // estimate from frame to frame, and every dip under it leaves reverb
    // behind, so taking a tail properly down needs more than 1: twice it,
    // by default, takes a tail to a third and costs the voice a decibel.
    double amount        = 2.0;
    double floorDb       = -18.0; // the most any bin is turned down
    double delaySeconds  = 0.05;  // where "late" reverb starts: after the voice's own sound
};

inline std::vector<float> process(const std::vector<float>& samples, double sampleRate, const Settings& settings)
{
    if (samples.empty() || sampleRate <= 0.0 || settings.reverbSeconds <= 0.0)
        return samples;

    constexpr int kSize = 1024;
    constexpr int kHop  = kSize / 4;
    constexpr int kBins = kSize / 2 + 1;

    const auto   window    = noisereduction::hannWindow(kSize);
    const int    delay     = std::max(1, (int) std::lround(settings.delaySeconds * sampleRate / kHop));
    // Power fades by 60 dB (a factor of 10^6) over the reverb time.
    const double fade      = std::pow(10.0, -6.0 * (delay * kHop / sampleRate) / settings.reverbSeconds);
    const double floorGain = std::pow(10.0, std::min(0.0, settings.floorDb) / 20.0);
    const double amount    = std::max(0.0, settings.amount);

    std::vector<float>               re((size_t) kSize), im((size_t) kSize);
    std::vector<std::vector<double>> history((size_t) delay + 1, std::vector<double>((size_t) kBins, 0.0)); // smoothed power, a ring
    std::vector<double>              smoothed((size_t) kBins, 0.0), gains((size_t) kBins, 1.0);
    std::vector<float>               output(samples.size(), 0.0f), weight(samples.size(), 0.0f);
    int                              frame = 0;

    for (int start = 0; start < (int) samples.size(); start += kHop, ++frame)
    {
        for (int i = 0; i < kSize; ++i)
        {
            const int at   = start + i;
            re[(size_t) i] = (at < (int) samples.size() ? samples[(size_t) at] : 0.0f) * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft::transform(re, im, false);

        auto&       now  = history[(size_t) (frame % (delay + 1))];
        const auto& past = history[(size_t) ((frame + 1) % (delay + 1))]; // delay frames ago
        for (int k = 0; k < kBins; ++k)
        {
            const double power = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
            smoothed[(size_t) k] = 0.6 * smoothed[(size_t) k] + 0.4 * power;

            // Estimate and observation both smoothed, so the comparison is
            // like for like: a noisy tail's power jumps about from frame to
            // frame, and against the raw power too little comes off.
            const double late = frame >= delay ? fade * past[(size_t) k] : 0.0;
            const double level = smoothed[(size_t) k];
            const double gain  = level > 1.0e-20 ? std::sqrt(std::max(0.0, 1.0 - amount * late / level)) : 1.0;
            // Rising at once, falling a little slower, so what's left
            // doesn't flutter.
            gains[(size_t) k] = std::max(floorGain, gain >= gains[(size_t) k] ? gain : 0.5 * (gain + gains[(size_t) k]));

            re[(size_t) k] = (float) (re[(size_t) k] * gains[(size_t) k]);
            im[(size_t) k] = (float) (im[(size_t) k] * gains[(size_t) k]);
            if (k > 0 && k < kSize - k)
            {
                re[(size_t) (kSize - k)] = re[(size_t) k];
                im[(size_t) (kSize - k)] = -im[(size_t) k];
            }
        }
        now = smoothed; // the observed power: what the room will return from

        fft::transform(re, im, true);
        for (int i = 0; i < kSize; ++i)
        {
            const int at = start + i;
            if (at >= (int) samples.size())
                break;
            output[(size_t) at] += re[(size_t) i] * window[(size_t) i];
            weight[(size_t) at] += window[(size_t) i] * window[(size_t) i];
        }
    }

    const float steady     = *std::max_element(weight.begin(), weight.end());
    const float floorValue = std::max(1.0e-6f, steady * 0.1f);
    for (size_t i = 0; i < output.size(); ++i)
        output[i] /= std::max(weight[i], floorValue);
    return output;
}

} // namespace soundsplice::engine::dereverb
