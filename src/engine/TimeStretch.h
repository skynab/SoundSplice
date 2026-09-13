#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/AudioEdits.h"
#include "engine/Fft.h"
#include "engine/NoiseReduction.h"

namespace looper::engine::timestretch
{
/**
    Speed and pitch, as offline sample-vector transformations.

    Two genuinely different operations, kept separate because conflating them
    is the usual source of confusion:

      - **changeSpeed** is varispeed — playing the audio faster or slower, so
        pitch moves with it. That's what a tape machine does, and what the
        drum sampler already does per pad (see DrumKitNode's pitchRatio).
        It's a resample and nothing more.

      - **pitchShift** moves pitch while *keeping the length*, which no amount
        of resampling can do on its own. It needs the signal taken apart into
        frequency bins and put back together with a different hop — a phase
        vocoder.

    JUCE-free, so the pitch ratio the vocoder actually achieves is measured in
    a headless test rather than asserted here.
*/

/** Plays @p samples at @p speedFactor times the rate: 2.0 is twice as fast,
    half as long and an octave up. Speed and pitch move together, on purpose. */
inline std::vector<float> changeSpeed(const std::vector<float>& samples, double speedFactor)
{
    if (speedFactor <= 0.0)
        return samples;
    return audioedits::resample(samples, speedFactor);
}

namespace detail
{
    inline constexpr int kFftSize     = 2048; // finer bins than the denoiser: pitch work needs resolution
    inline constexpr int kAnalysisHop = kFftSize / 4;

    /** Wraps a phase difference into [-pi, pi].

        The whole phase vocoder rests on this. A bin's phase advance between
        frames is only known modulo 2pi, and the *wrapped* remainder is what
        reveals how far the true frequency sits from the bin centre. Skip it
        and every partial is quantised to its bin, which is exactly the
        metallic, detuned artefact a bad pitch shifter has. */
    inline double wrapPhase(double phase)
    {
        return phase - 2.0 * fft::kPi * std::round(phase / (2.0 * fft::kPi));
    }
}

/**
    Stretches @p samples in time by @p factor without changing pitch.

    2.0 is twice as long; 0.5 half. Implemented as a phase vocoder: analyse at
    a fixed hop, resynthesise at `hop * factor`, and advance each bin's phase
    by its *measured* frequency rather than its bin centre so partials stay
    coherent across frames.
*/
inline std::vector<float> timeStretch(const std::vector<float>& samples, double factor)
{
    using namespace detail;

    if (factor <= 0.0 || std::abs(factor - 1.0) < 1.0e-9 || (int) samples.size() < kFftSize)
        return samples;

    const int    bins         = kFftSize / 2 + 1;
    const int    synthesisHop = std::max(1, (int) std::llround((double) kAnalysisHop * factor));
    const auto   window       = noisereduction::hannWindow(kFftSize);

    const int outLength = (int) std::llround((double) samples.size() * factor) + kFftSize;

    std::vector<float> output((size_t) outLength, 0.0f);
    std::vector<float> windowSum((size_t) outLength, 0.0f);

    std::vector<double> lastPhase((size_t) bins, 0.0);
    std::vector<double> sumPhase((size_t) bins, 0.0);
    std::vector<float>  re((size_t) kFftSize), im((size_t) kFftSize);

    int writePosition = 0;

    for (int start = 0; start + kFftSize <= (int) samples.size(); start += kAnalysisHop)
    {
        for (int i = 0; i < kFftSize; ++i)
        {
            re[(size_t) i] = samples[(size_t) (start + i)] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }

        fft::transform(re, im, false);

        for (int k = 0; k < bins; ++k)
        {
            const double real      = re[(size_t) k];
            const double imaginary = im[(size_t) k];
            const double magnitude = std::hypot(real, imaginary);
            const double phase     = std::atan2(imaginary, real);

            // How far this bin's phase would advance in one analysis hop if
            // the partial sat exactly at the bin centre.
            const double expected = 2.0 * fft::kPi * (double) kAnalysisHop * (double) k / (double) kFftSize;

            // The remainder after removing that is the partial's true
            // deviation from the bin centre — see wrapPhase.
            const double deviation = wrapPhase(phase - lastPhase[(size_t) k] - expected);
            lastPhase[(size_t) k]  = phase;

            // Advance by the measured frequency, scaled to the synthesis hop.
            const double trueAdvance = expected + deviation;
            sumPhase[(size_t) k] += trueAdvance * (double) synthesisHop / (double) kAnalysisHop;

            re[(size_t) k] = (float) (magnitude * std::cos(sumPhase[(size_t) k]));
            im[(size_t) k] = (float) (magnitude * std::sin(sumPhase[(size_t) k]));

            // Keep the spectrum conjugate-symmetric so the inverse comes back
            // real; letting the halves disagree leaves an imaginary residue
            // heard as a harsh edge.
            if (k > 0 && k < kFftSize - k)
            {
                re[(size_t) (kFftSize - k)] =  re[(size_t) k];
                im[(size_t) (kFftSize - k)] = -im[(size_t) k];
            }
        }

        fft::transform(re, im, true);

        for (int i = 0; i < kFftSize; ++i)
        {
            const int index = writePosition + i;
            if (index < 0 || index >= outLength)
                continue;

            const float w = window[(size_t) i];
            output[(size_t) index]    += re[(size_t) i] * w;
            windowSum[(size_t) index] += w * w;
        }

        writePosition += synthesisHop;
    }

    // Normalising by the accumulated squared window is what makes the
    // overlap-add reconstruct at a steady level. The floor matters as much as
    // the division: at the very start and end only a window's taper
    // contributes, so windowSum there is near zero, and dividing by it
    // amplifies enormously — measured, a downward pitch shift peaked at 65x
    // full scale before this. Flooring at a fraction of the steady-state sum
    // caps the gain and lets the edges taper, which is honest: there really
    // is less data there.
    const float steadyState = *std::max_element(windowSum.begin(), windowSum.end());
    const float floorValue  = std::max(1.0e-6f, steadyState * 0.1f);

    for (size_t i = 0; i < output.size(); ++i)
        output[i] /= std::max(windowSum[i], floorValue);

    // Trimmed to the length actually asked for: the buffer was oversized by a
    // frame so the last hop had somewhere to land.
    const int wanted = std::max(1, (int) std::llround((double) samples.size() * factor));
    output.resize((size_t) std::min(outLength, wanted));
    return output;
}

/**
    Shifts @p samples by @p semitones, keeping the length.

    Stretch then resample: stretching by the pitch ratio makes it longer and
    leaves pitch alone, and resampling by the same ratio brings the length
    back while carrying pitch up with it. Doing only the second half would be
    varispeed — see changeSpeed, which is exactly that and is a different
    thing to want.
*/
inline std::vector<float> pitchShift(const std::vector<float>& samples, double semitones)
{
    if (std::abs(semitones) < 1.0e-9 || samples.empty())
        return samples;

    const double ratio     = std::pow(2.0, semitones / 12.0);
    const auto   stretched = timeStretch(samples, ratio);
    auto         shifted   = audioedits::resample(stretched, ratio);

    // Resampling rounds, so nudge the length back to exactly what came in —
    // a clip whose length drifted by a sample per edit would slowly slip
    // against everything else on the timeline.
    shifted.resize(samples.size(), 0.0f);
    return shifted;
}

} // namespace looper::engine::timestretch
