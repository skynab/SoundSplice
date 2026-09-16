#pragma once

#include <algorithm>
#include <cmath>

namespace soundsplice::waveformscale
{
/**
    How high a sample is drawn in the audio editor, on its linear scale or on
    a decibel one.

    Linear is what the samples are, and shows loud material as it is. Quiet
    material all but vanishes on it, though: a -40 dB noise floor or a reverb
    tail is a hair on the centre line. On the dB scale height follows level,
    as Audacity's "dB" waveform view does, so the bottom kDbRange decibels
    share the lane evenly and anything quieter sits on the centre line.

    Heights are signed fractions of half a lane: +1 at the top, -1 at the
    bottom. JUCE-free, so the mapping and its inverse are tested on their own.
*/

/** How many decibels below full scale the dB scale shows. */
inline constexpr float kDbRange = 60.0f;

/** The signed height, in half-lanes, that @p sample is drawn at. Past full
    scale is drawn at full scale. */
inline float heightFor(float sample, bool dbScale)
{
    if (! std::isfinite(sample))
        return 0.0f;

    if (! dbScale)
        return std::clamp(sample, -1.0f, 1.0f);

    const float magnitude = std::abs(sample);
    if (magnitude <= 0.0f)
        return 0.0f;

    const float height = std::clamp(1.0f + 20.0f * std::log10(magnitude) / kDbRange, 0.0f, 1.0f);
    return sample < 0.0f ? -height : height;
}

/** The sample a point at signed height @p height stands for: heightFor's
    inverse, for drawing samples in by hand. Heights past a half-lane give
    samples past full scale, for the caller to clamp. On the dB scale the
    centre line itself is silence. */
inline float sampleFor(float height, bool dbScale)
{
    if (! std::isfinite(height))
        return 0.0f;

    if (! dbScale)
        return height;

    const float magnitude = std::abs(height);
    if (magnitude <= 0.0f)
        return 0.0f;

    const float sample = std::pow(10.0f, (magnitude - 1.0f) * kDbRange / 20.0f);
    return height < 0.0f ? -sample : sample;
}

} // namespace soundsplice::waveformscale
