#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Interpolation.h"

namespace soundsplice::engine::audioedits
{
/**
    The destructive edit operations, as plain sample-vector transformations.

    JUCE-free and returning new vectors rather than mutating, because the
    interesting bugs here are all off-by-ones at range boundaries — a cut that
    drops one sample too many, a fade that never quite reaches zero, a paste
    that lands one sample late — and none of those are visible by ear or
    catchable anywhere but a headless test.

    Ranges are always half-open sample indices `[from, to)`, matching how
    every other range in this codebase reads, and are clamped rather than
    asserted: a selection dragged past the end of a file is normal, not an
    error.

    **Channels are transformed independently, with the same indices.** The
    caller loops. That's deliberate — the alternative, taking all channels at
    once, would make it easy to hand different ranges per channel and shear a
    stereo file apart. The one thing that genuinely must be decided once for
    all channels is where a cut lands, which is why zero-crossing search is
    its own function the caller applies before looping.
*/

/** A range clamped into @p length and ordered, so `from <= to` always. */
inline void clampRange(int length, int& from, int& to)
{
    from = std::clamp(from, 0, length);
    to   = std::clamp(to, 0, length);
    if (to < from)
        std::swap(from, to);
}

/** @p samples with `[from, to)` removed — Cut and Delete. */
inline std::vector<float> removeRange(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out;
    out.reserve(samples.size() - (size_t) (last - first));
    out.insert(out.end(), samples.begin(), samples.begin() + first);
    out.insert(out.end(), samples.begin() + last, samples.end());
    return out;
}

/** Only `[from, to)` — Trim to selection. */
inline std::vector<float> keepRange(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);
    return { samples.begin() + first, samples.begin() + last };
}

/** A copy of `[from, to)`, leaving @p samples alone — Copy. */
inline std::vector<float> extractRange(const std::vector<float>& samples, int from, int to)
{
    return keepRange(samples, from, to);
}

/** @p source spliced into @p destination at @p at — Paste. An @p at past the
    end appends, which is what pasting with the cursor at the end means. */
inline std::vector<float> insertAt(const std::vector<float>& destination,
                                   const std::vector<float>& source, int at)
{
    const int position = std::clamp(at, 0, (int) destination.size());

    std::vector<float> out;
    out.reserve(destination.size() + source.size());
    out.insert(out.end(), destination.begin(), destination.begin() + position);
    out.insert(out.end(), source.begin(), source.end());
    out.insert(out.end(), destination.begin() + position, destination.end());
    return out;
}

/** `[from, to)` zeroed, the rest untouched and the length unchanged —
    Silence. Distinct from Delete on purpose: silencing keeps everything
    after it where it was, deleting pulls it earlier. */
inline std::vector<float> silenceRange(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out = samples;
    std::fill(out.begin() + first, out.begin() + last, 0.0f);
    return out;
}

/** `[from, to)` played backwards in place — Reverse. */
inline std::vector<float> reverseRange(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out = samples;
    std::reverse(out.begin() + first, out.begin() + last);
    return out;
}

/** A linear ramp from silence to unity across `[from, to)` — Fade In.

    Linear in amplitude, which is what a fade control means everywhere else
    in this app (and what Audacity's plain Fade In does). The first sample is
    exactly zero and the last is very close to unity; a ramp that reached
    unity one sample early would leave a step at the join. */
inline std::vector<float> fadeIn(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out   = samples;
    const int          count = last - first;
    if (count <= 0)
        return out;

    for (int i = 0; i < count; ++i)
        out[(size_t) (first + i)] *= (float) i / (float) count;
    return out;
}

/** A linear ramp from unity to silence across `[from, to)` — Fade Out. The
    last sample is exactly zero, so a fade to the end of a file really ends
    in silence. */
inline std::vector<float> fadeOut(const std::vector<float>& samples, int from, int to)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out   = samples;
    const int          count = last - first;
    if (count <= 0)
        return out;

    for (int i = 0; i < count; ++i)
        out[(size_t) (first + i)] *= (float) (count - 1 - i) / (float) count;
    return out;
}

/** Studio Fade Out, as Audacity's: the level eases down (a raised-cosine
    curve) while a two-pole low-pass sweeps from the top of the audio band to
    250 Hz, so the sound darkens as it goes, the way a mixed fade on a
    console sounds rather than a turned-down fader. Ends in exact silence. */
inline std::vector<float> studioFadeOut(const std::vector<float>& samples, int from, int to, double sampleRate)
{
    int first = from, last = to;
    clampRange((int) samples.size(), first, last);

    std::vector<float> out   = samples;
    const int          count = last - first;
    if (count <= 0 || sampleRate <= 0.0)
        return out;

    constexpr double pi      = 3.14159265358979323846;
    const double     topHz   = std::min(20000.0, sampleRate * 0.45);
    constexpr double lowHz   = 250.0;
    double           stage1  = 0.0, stage2 = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const double x      = count > 1 ? (double) i / (double) (count - 1) : 1.0;
        const double gain   = 0.5 + 0.5 * std::cos(pi * x);
        const double hz     = topHz * std::pow(lowHz / topHz, x);
        const double coeff  = 1.0 - std::exp(-2.0 * pi * hz / sampleRate);

        stage1 += coeff * ((double) samples[(size_t) (first + i)] - stage1);
        stage2 += coeff * (stage1 - stage2);
        out[(size_t) (first + i)] = (float) (stage2 * gain);
    }
    return out;
}

/**
    The nearest point to @p index where the waveform crosses zero, within
    @p searchRadius samples.

    Cutting mid-waveform leaves a step discontinuity, which is heard as a
    click — the single most common way a destructive edit sounds wrong. Moving
    the boundary a few samples to a zero crossing removes it, and a few
    samples is far below anything audible as a timing change.

    Returns @p index unchanged when nothing better is within reach, so a
    caller can always use the result.
*/
inline int nearestZeroCrossing(const std::vector<float>& samples, int index, int searchRadius = 512)
{
    const int length = (int) samples.size();
    if (length < 2)
        return std::clamp(index, 0, length);

    const int centre = std::clamp(index, 0, length - 1);

    for (int offset = 0; offset <= searchRadius; ++offset)
    {
        for (const int candidate : { centre - offset, centre + offset })
        {
            if (candidate <= 0 || candidate >= length)
                continue;

            // A sign change between consecutive samples: the crossing is
            // between them, and `candidate` is the first sample after it.
            const float previous = samples[(size_t) (candidate - 1)];
            const float current  = samples[(size_t) candidate];
            if ((previous <= 0.0f && current >= 0.0f) || (previous >= 0.0f && current <= 0.0f))
                return candidate;
        }
    }

    return centre;
}

/**
    @p samples resampled by @p ratio (output length = input length / ratio).

    For pasting audio recorded at one rate into a file at another. Linear
    interpolation via the same sampleLinear the players use — good enough for
    a rate conversion between the usual 44.1/48k neighbours, and consistent
    with what playback would have done anyway.
*/
inline std::vector<float> resample(const std::vector<float>& samples, double ratio)
{
    if (samples.empty() || ratio <= 0.0)
        return samples;

    const int length    = (int) samples.size();
    const int outLength = std::max(1, (int) std::llround((double) length / ratio));

    std::vector<float> out((size_t) outLength);
    double             position = 0.0;

    for (int i = 0; i < outLength; ++i)
    {
        out[(size_t) i] = sampleLinear(samples.data(), length, position);
        position += ratio;
    }
    return out;
}

/**
    Normalize, as Audacity's: each channel's DC offset (its average, which
    should be zero) taken out if @p removeDc, then the audio scaled so its
    loudest sample reaches @p targetPeak, either all channels by the one gain
    (keeping their balance) or, if @p independently, each by its own. The DC
    comes out first so the peak it's measured by is the true swing. False,
    changing nothing, if the audio is silent.
*/
inline bool normalize(std::vector<std::vector<float>>& channels, float targetPeak, bool removeDc, bool independently)
{
    std::vector<double> offsets(channels.size(), 0.0);
    std::vector<float>  peaks(channels.size(), 0.0f);
    for (size_t c = 0; c < channels.size(); ++c)
    {
        auto& channel = channels[c];
        if (removeDc && ! channel.empty())
        {
            double sum = 0.0;
            for (float s : channel)
                sum += s;
            offsets[c] = sum / (double) channel.size();
        }
        for (float s : channel)
            peaks[c] = std::max(peaks[c], std::abs((float) (s - offsets[c])));
    }

    const float loudest = peaks.empty() ? 0.0f : *std::max_element(peaks.begin(), peaks.end());
    if (loudest <= 0.0f)
        return false;

    for (size_t c = 0; c < channels.size(); ++c)
    {
        const float peak = independently ? peaks[c] : loudest;
        const float gain = peak > 0.0f ? targetPeak / peak : 1.0f; // a silent channel stays silent
        for (auto& s : channels[c])
            s = (float) (s - offsets[c]) * gain;
    }
    return true;
}

} // namespace soundsplice::engine::audioedits
