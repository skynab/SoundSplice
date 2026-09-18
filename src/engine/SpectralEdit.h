#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine::spectral
{
/**
    Spectral editing: changing the level of one band of frequencies over one
    stretch of time and leaving everything else, as Audacity's Spectral Delete
    and Audition's spectral selection do. A cough over a sustained note, a
    phone's ring behind speech, one whistle in a field recording.

    The audio is taken apart into short overlapping windows, each window's
    bins inside the band are scaled, and the windows are added back
    (weighted overlap-add with a Hann window both ways, at a quarter-window
    hop, which reconstructs the input exactly when nothing is scaled). Only
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

/** Scales the bins between @p lowHz and @p highHz by @p gain over samples
    [@p from, @p to) of @p samples. A band bin is scaled fully; one at the
    band's edge, partly, so the band has no hard edge to ring. Returns false,
    changing nothing, if the selection is too short to hold a window. */
inline bool scaleBand(std::vector<float>& samples, int from, int to, double sampleRate, double lowHz, double highHz,
                      float gain)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);

    const int n   = windowFor(to - from);
    const int hop = n / 4;
    if (to - from < n || sampleRate <= 0.0 || highHz <= lowHz)
        return false;

    std::vector<float> window((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / n)); // periodic Hann

    // Per-bin gain: full inside the band, a one-bin ramp at each edge.
    const int          bins     = n / 2 + 1;
    const double       binHz    = sampleRate / n;
    std::vector<float> binGains((size_t) bins, 1.0f);
    for (int k = 0; k < bins; ++k)
    {
        const double hz     = k * binHz;
        const double inside = std::min(hz - lowHz, highHz - hz) / binHz; // bins inside the band's edge
        const double amount = std::clamp(inside + 0.5, 0.0, 1.0);
        binGains[(size_t) k] = (float) (1.0 + (gain - 1.0) * amount);
    }

    // Windows over the whole of [from, to), starting a window early and
    // ending one late so every sample in the range is covered by the same
    // number of windows; only those wholly inside the range are changed.
    const int first = from - n;
    const int last  = to;

    std::vector<double> output((size_t) (to - from), 0.0);
    std::vector<double> weight((size_t) (to - from), 0.0);
    std::vector<float>  re((size_t) n), im((size_t) n);

    for (int start = first; start <= last; start += hop)
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
            for (int k = 0; k < bins; ++k)
            {
                re[(size_t) k] *= binGains[(size_t) k];
                im[(size_t) k] *= binGains[(size_t) k];
                if (k > 0 && k < n - k)
                {
                    re[(size_t) (n - k)] = re[(size_t) k];
                    im[(size_t) (n - k)] = -im[(size_t) k];
                }
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

} // namespace soundsplice::engine::spectral
