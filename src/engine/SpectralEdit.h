#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine::spectral
{
/**
    Spectral editing: changing one band of frequencies over one stretch of
    time and leaving everything else, as Audacity's Spectral Delete and
    Audition's spectral selection and spot healing do. A cough over a
    sustained note, a phone's ring behind speech, one whistle in a field
    recording.

    The audio is taken apart into short overlapping windows, each window's
    bins inside the band are changed, and the windows are added back
    (weighted overlap-add with a Hann window both ways, at a quarter-window
    hop, which reconstructs the input exactly when nothing is changed). Only
    windows lying wholly inside [from, to) are changed, so nothing outside the
    selection moves at all, and the change fades in and out over the first and
    last window's length rather than starting with a click.

    JUCE-free, and tested headless.
*/

/** The window length used for a selection @p frames long: 2048, or shorter
    for a short selection, so a few windows still fit inside it. */
inline int windowFor(int frames)
{
    int size = 2048;
    while (size > 128 && size * 2 > frames)
        size /= 2;
    return size;
}

namespace detail
{
    inline std::vector<float> hann(int n)
    {
        std::vector<float> window((size_t) n);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / n)); // periodic
        return window;
    }

    /** How much of each bin of an @p n-point transform is inside the band:
        1 inside, 0 outside, a one-bin ramp at each edge so the band has no
        hard edge to ring. */
    inline std::vector<float> bandAmounts(int n, double sampleRate, double lowHz, double highHz)
    {
        const int          bins  = n / 2 + 1;
        const double       binHz = sampleRate / n;
        std::vector<float> amounts((size_t) bins);
        for (int k = 0; k < bins; ++k)
        {
            const double hz     = k * binHz;
            const double inside = std::min(hz - lowHz, highHz - hz) / binHz;
            amounts[(size_t) k] = (float) std::clamp(inside + 0.5, 0.0, 1.0);
        }
        return amounts;
    }

    /** The mean magnitude of each bin over the windows lying wholly in
        [@p from, @p to) of @p samples; empty if none fits. */
    inline std::vector<double> meanMagnitudes(const std::vector<float>& samples, int from, int to, int n,
                                              const std::vector<float>& window)
    {
        const int           bins = n / 2 + 1;
        std::vector<double> sum((size_t) bins, 0.0);
        std::vector<float>  re((size_t) n), im((size_t) n);
        int                 count = 0;

        for (int start = from; start + n <= to; start += n / 4, ++count)
        {
            for (int i = 0; i < n; ++i)
            {
                re[(size_t) i] = samples[(size_t) (start + i)] * window[(size_t) i];
                im[(size_t) i] = 0.0f;
            }
            fft::transform(re, im, false);
            for (int k = 0; k < bins; ++k)
                sum[(size_t) k] += std::hypot((double) re[(size_t) k], (double) im[(size_t) k]);
        }

        if (count == 0)
            return {};
        for (auto& value : sum)
            value /= count;
        return sum;
    }
}

/**
    The frame-by-frame machinery the edits share: every window wholly inside
    [@p from, @p to) is transformed and handed to @p edit, which changes bins
    0..n/2 of it in place (the mirror half is filled in after), as
    edit(frame, frames, re, im, bandAmounts). Everything else is reconstructed
    as it was. False, changing nothing, if the selection is too short to hold
    a window.
*/
template <typename EditFrame>
bool editBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
              EditFrame&& edit)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);

    const int n   = windowFor(to - from);
    const int hop = n / 4;
    if (to - from < n || sampleRate <= 0.0 || highHz <= lowHz)
        return false;

    const auto window  = detail::hann(n);
    const auto amounts = detail::bandAmounts(n, sampleRate, lowHz, highHz);
    const int  bins    = n / 2 + 1;
    const int  frames  = (to - from - n) / hop + 1; // those wholly inside

    // Windows over the whole of [from, to), starting a window early and
    // ending one late so every sample in the range is covered by the same
    // number of windows; only those wholly inside the range are changed.
    std::vector<double> output((size_t) (to - from), 0.0);
    std::vector<double> weight((size_t) (to - from), 0.0);
    std::vector<float>  re((size_t) n), im((size_t) n);

    for (int start = from - n; start <= to; start += hop)
    {
        for (int i = 0; i < n; ++i)
        {
            const int at = start + i;
            re[(size_t) i] = at >= 0 && at < size ? samples[(size_t) at] * window[(size_t) i] : 0.0f;
            im[(size_t) i] = 0.0f;
        }

        if (start >= from && start + n <= to)
        {
            fft::transform(re, im, false);
            edit((start - from) / hop, frames, re, im, amounts);
            for (int k = 1; k < bins && k < n - k; ++k)
            {
                re[(size_t) (n - k)] = re[(size_t) k];
                im[(size_t) (n - k)] = -im[(size_t) k];
            }
            fft::transform(re, im, true);
        }

        for (int i = 0; i < n; ++i)
        {
            const int at = start + i;
            if (at < from || at >= to)
                continue;
            output[(size_t) (at - from)] += (double) re[(size_t) i] * window[(size_t) i];
            weight[(size_t) (at - from)] += (double) window[(size_t) i] * window[(size_t) i];
        }
    }

    for (int i = 0; i < to - from; ++i)
        if (weight[(size_t) i] > 1.0e-9)
            samples[(size_t) (from + i)] = (float) (output[(size_t) i] / weight[(size_t) i]);

    return true;
}

/** Scales the bins between @p lowHz and @p highHz by @p gain over samples
    [@p from, @p to) of @p samples: Spectral Delete at 0, Spectral Gain
    otherwise. */
inline bool scaleBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
                      float gain)
{
    return editBand(samples, from, to, sampleRate, lowHz, highHz,
                    [gain](int, int, std::vector<float>& re, std::vector<float>& im, const std::vector<float>& amounts)
                    {
                        for (size_t k = 0; k < amounts.size(); ++k)
                        {
                            const float binGain = 1.0f + (gain - 1.0f) * amounts[k];
                            re[k] *= binGain;
                            im[k] *= binGain;
                        }
                    });
}

/** Scales each bin over samples [@p from, @p to) by @p gain as far as
    @p maskAt(the window's middle as a sample index into @p samples, the bin's
    frequency in Hz) says, from 0 (left alone) to 1 (fully): Spectral Delete
    and Spectral Gain for a painted or lassoed shape. */
template <typename MaskAt>
bool scaleMask(std::vector<float>& samples, int from, int to, double sampleRate, float gain, MaskAt&& maskAt)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);

    const int    n     = windowFor(to - from);
    const double binHz = sampleRate / n;
    const int    hop   = n / 4;

    return editBand(samples, from, to, sampleRate, 0.0, sampleRate,
                    [&](int frame, int, std::vector<float>& re, std::vector<float>& im, const std::vector<float>& bins)
                    {
                        const double centre = (double) from + (double) frame * hop + n * 0.5;
                        for (size_t k = 0; k < bins.size(); ++k)
                        {
                            const float amount = std::min(1.0f, (float) maskAt(centre, (double) k * binHz));
                            if (amount <= 0.0f)
                                continue;
                            const float binGain = 1.0f + (gain - 1.0f) * amount;
                            re[k] *= binGain;
                            im[k] *= binGain;
                        }
                    });
}

/** Scales every bin over samples [@p from, @p to) by @p gainAt(its frequency
    in Hz), a linear gain: the shared step for shaped spectral edits. */
template <typename GainAt>
bool scaleByCurve(std::vector<float>& samples, int from, int to, double sampleRate, GainAt&& gainAt)
{
    std::vector<float> gains;
    return editBand(samples, from, to, sampleRate, 0.0, sampleRate,
                    [&](int, int, std::vector<float>& re, std::vector<float>& im, const std::vector<float>& amounts)
                    {
                        if (gains.empty())
                        {
                            const int n = (int) re.size();
                            gains.resize(amounts.size());
                            for (size_t k = 0; k < gains.size(); ++k)
                                gains[k] = (float) gainAt((double) k * sampleRate / n);
                        }
                        for (size_t k = 0; k < gains.size(); ++k)
                        {
                            re[k] *= gains[k];
                            im[k] *= gains[k];
                        }
                    });
}

/** Where @p hz sits across [@p lowHz, @p highHz] by octave, 0 to 1 (clamped). */
inline double octavePosition(double hz, double lowHz, double highHz)
{
    if (hz <= lowHz || highHz <= lowHz || lowHz <= 0.0)
        return hz <= lowHz ? 0.0 : 1.0;
    return std::clamp(std::log(hz / lowHz) / std::log(highHz / lowHz), 0.0, 1.0);
}

/** Spectral EQ, Audacity's spectral parametric EQ: a bell across the band by
    octave, @p gainDb at its middle (the band's geometric centre) and none at
    its edges. */
inline bool bellBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
                     float gainDb)
{
    if (highHz <= lowHz)
        return false;

    const double low = std::max(lowHz, 1.0);
    return scaleByCurve(samples, from, to, sampleRate, [low, highHz, gainDb](double hz)
    {
        if (hz <= low || hz >= highHz)
            return 1.0;
        const double shape = 0.5 - 0.5 * std::cos(2.0 * fft::kPi * octavePosition(hz, low, highHz));
        return std::pow(10.0, gainDb * shape / 20.0);
    });
}

/** Spectral shelf, Audacity's spectral shelves: @p gainDb reached across the
    band by octave and held beyond it, above the band for a high shelf and
    below it for a low one. */
inline bool shelfBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
                      float gainDb, bool highShelf)
{
    if (highHz <= lowHz)
        return false;

    const double low = std::max(lowHz, 1.0);
    return scaleByCurve(samples, from, to, sampleRate, [low, highHz, gainDb, highShelf](double hz)
    {
        const double across = octavePosition(hz, low, highHz);
        const double shape  = 0.5 - 0.5 * std::cos(fft::kPi * (highShelf ? across : 1.0 - across));
        return std::pow(10.0, gainDb * shape / 20.0);
    });
}

/**
    Spectral repair, the spot-healing of Audition and Audacity's spectral
    tools: the band over [@p from, @p to) is rebuilt from what the same
    frequencies do just before and just after it, so a cough over a held note
    is replaced by the note going on. Each bin's level is drawn across the
    gap from its average over the @p contextFrames before to its average over
    those after (either alone if only one side has audio), and the phase of
    what was there is kept, so the result joins its surroundings.

    @p samples must hold that context either side of the selection. False,
    changing nothing, if the selection is too short or there's no context.
*/
template <typename MaskAt>
bool healMask(std::vector<float>& samples, int from, int to, double sampleRate, int contextFrames, MaskAt&& maskAt);

inline bool healBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
                     int contextFrames)
{
    if (highHz <= lowHz || sampleRate <= 0.0)
        return false;

    // The box: every window, and each bin by how far it's inside the band,
    // with a one-bin ramp at each edge.
    const double binHz = sampleRate / windowFor(std::clamp(to, 0, (int) samples.size()) - std::max(from, 0));
    return healMask(samples, from, to, sampleRate, contextFrames, [lowHz, highHz, binHz](double, double hz)
    {
        return (float) std::clamp(std::min(hz - lowHz, highHz - hz) / binHz + 0.5, 0.0, 1.0);
    });
}

/**
    Spectral repair of any shape, for the healing brush: as healBand, but how
    much of each window's bin is rebuilt is @p maskAt(the window's middle as a
    sample index into @p samples, the bin's frequency in Hz), from 0 (left as
    it is) to 1 (rebuilt). Where the mask is 0 throughout, nothing changes.
*/
template <typename MaskAt>
bool healMask(std::vector<float>& samples, int from, int to, double sampleRate, int contextFrames, MaskAt&& maskAt)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);

    const int  n      = windowFor(to - from);
    const auto window = detail::hann(n);
    const auto before = detail::meanMagnitudes(samples, std::max(0, from - contextFrames), from, n, window);
    const auto after  = detail::meanMagnitudes(samples, to, std::min(size, to + contextFrames), n, window);
    if (before.empty() && after.empty())
        return false;

    const double binHz = sampleRate / n;
    const int    hop   = n / 4;

    return editBand(samples, from, to, sampleRate, 0.0, sampleRate,
                    [&](int frame, int frames, std::vector<float>& re, std::vector<float>& im,
                        const std::vector<float>& bins)
                    {
                        const double t      = frames > 1 ? (double) frame / (double) (frames - 1) : 0.5;
                        const double centre = (double) from + (double) frame * hop + n * 0.5;

                        for (size_t k = 0; k < bins.size(); ++k)
                        {
                            const float amount = maskAt(centre, (double) k * binHz);
                            if (amount <= 0.0f)
                                continue;

                            const double target = before.empty() ? after[k]
                                                : after.empty()  ? before[k]
                                                                 : before[k] * (1.0 - t) + after[k] * t;
                            const double current = std::hypot((double) re[k], (double) im[k]);
                            const double wanted  = current + (target - current) * std::min(1.0f, amount);

                            if (current > 1.0e-12)
                            {
                                re[k] = (float) (re[k] * wanted / current);
                                im[k] = (float) (im[k] * wanted / current);
                            }
                            else
                            {
                                re[k] = (float) wanted;
                                im[k] = 0.0f;
                            }
                        }
                    });
}

} // namespace soundsplice::engine::spectral
