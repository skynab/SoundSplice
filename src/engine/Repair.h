#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace soundsplice::engine::repair
{
/**
    Restoration, as Audacity's Repair, Click Removal and Clip Fix and
    Audition's DeHummer: filling a short damaged stretch by predicting it from
    the audio either side, finding clicks and filling them the same way,
    rebuilding clipped peaks, and notching out mains hum and its harmonics.

    Each works on one channel in memory: the selection with some audio either
    side of it for context. JUCE-free and tested headless.
*/

/** Burg's method: the coefficients a[0..order] (a[0] = 1) of an
    autoregressive model of @p x, so that x[n] + a[1] x[n-1] + ... +
    a[order] x[n-order] is as small as it can be. Order is cut to fit short
    input; silence gives a model predicting silence. */
inline std::vector<double> burg(const std::vector<double>& x, int order)
{
    const int n = (int) x.size();
    order       = std::clamp(order, 0, std::max(0, n - 1));

    std::vector<double> a((size_t) order + 1, 0.0);
    a[0] = 1.0;
    if (order == 0)
        return a;

    std::vector<double> f = x, b = x;

    double denominator = 0.0;
    for (double v : x)
        denominator += 2.0 * v * v;
    denominator -= x.front() * x.front() + x.back() * x.back();
    const double energy = denominator;

    for (int k = 0; k < order; ++k)
    {
        // Stop once what's left to model is rounding: a perfectly predictable
        // signal (a few pure tones) otherwise drives the next reflection to or
        // past 1, which is an unstable model that runs away when predicting.
        if (denominator <= energy * 1.0e-10 || denominator <= 1.0e-30)
            break;

        double numerator = 0.0;
        for (int i = 0; i <= n - k - 2; ++i)
            numerator += f[(size_t) (i + k + 1)] * b[(size_t) i];
        const double mu = -2.0 * numerator / denominator;
        if (! (std::abs(mu) < 0.9999))
            break;

        for (int i = 0; i <= (k + 1) / 2; ++i)
        {
            const double t1 = a[(size_t) i] + mu * a[(size_t) (k + 1 - i)];
            const double t2 = a[(size_t) (k + 1 - i)] + mu * a[(size_t) i];
            a[(size_t) i]         = t1;
            a[(size_t) (k + 1 - i)] = t2;
        }

        for (int i = 0; i <= n - k - 2; ++i)
        {
            const double t1 = f[(size_t) (i + k + 1)] + mu * b[(size_t) i];
            const double t2 = b[(size_t) i] + mu * f[(size_t) (i + k + 1)];
            f[(size_t) (i + k + 1)] = t1;
            b[(size_t) i]           = t2;
        }

        denominator = (1.0 - mu * mu) * denominator - f[(size_t) (k + 1)] * f[(size_t) (k + 1)]
                    - b[(size_t) (n - k - 2)] * b[(size_t) (n - k - 2)];
    }

    return a;
}

/** The next sample a model predicts from the @p order samples before
    @p at in @p x. */
inline double predict(const std::vector<double>& a, const std::vector<double>& x, int at)
{
    double sum = 0.0;
    for (int k = 1; k < (int) a.size(); ++k)
        sum -= a[(size_t) k] * x[(size_t) (at - k)];
    return sum;
}

/**
    Replaces samples [@p from, @p to) of @p samples with a prediction from
    what's around them: a model fitted to up to @p contextFrames before the gap
    runs forward into it, one fitted to up to that many after it runs
    backward, and the two cross-fade across it. How Audacity's Repair and most
    click removers fill a gap. False if there's no audio either side to learn
    from.
*/
inline bool interpolate(std::vector<float>& samples, int from, int to, int contextFrames = 2048, int order = 32)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);
    const int gap = to - from;
    if (gap == 0)
        return true;

    const int before = std::min(from, contextFrames);
    const int after  = std::min(size - to, contextFrames);
    if (before < 2 && after < 2)
        return false;

    const auto modelOf = [order](std::vector<double> context)
    {
        return burg(context, std::min(order, (int) context.size() / 2));
    };

    // Forward, from the left.
    std::vector<double> forward;
    if (before >= 2)
    {
        std::vector<double> context(samples.begin() + (from - before), samples.begin() + from);
        const auto          a = modelOf(context);
        forward               = context;
        forward.resize((size_t) (before + gap));
        const int p = (int) a.size() - 1;
        for (int i = before; i < before + gap; ++i)
            forward[(size_t) i] = i >= p ? predict(a, forward, i) : 0.0;
        forward.erase(forward.begin(), forward.begin() + before);
    }

    // Backward, from the right: the context reversed, predicted "forward".
    std::vector<double> backward;
    if (after >= 2)
    {
        std::vector<double> context(samples.rbegin() + (size - to - after), samples.rbegin() + (size - to));
        const auto          a = modelOf(context);
        backward              = context;
        backward.resize((size_t) (after + gap));
        const int p = (int) a.size() - 1;
        for (int i = after; i < after + gap; ++i)
            backward[(size_t) i] = i >= p ? predict(a, backward, i) : 0.0;
        backward.erase(backward.begin(), backward.begin() + after);
        std::reverse(backward.begin(), backward.end());
    }

    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < gap; ++i)
    {
        double value;
        if (forward.empty())
            value = backward[(size_t) i];
        else if (backward.empty())
            value = forward[(size_t) i];
        else
        {
            const double w = gap > 1 ? 0.5 - 0.5 * std::cos(pi * (double) i / (double) (gap - 1)) : 0.5;
            value          = forward[(size_t) i] * (1.0 - w) + backward[(size_t) i] * w;
        }
        samples[(size_t) (from + i)] = (float) std::clamp(value, -4.0, 4.0);
    }
    return true;
}

/**
    The clicks in [@p from, @p to) of @p samples, each as the samples to
    replace [first, last), for interpolate to fill. A click is where the audio departs from what a short-term
    model of it predicts by more than @p sensitivity times that model's usual
    error; runs of such samples close together are one click, and one wider
    than @p maxWidth samples is left alone, being more likely a transient in
    the music than damage.
*/
inline std::vector<std::pair<int, int>> findClicks(const std::vector<float>& samples, int from, int to,
                                                   double sensitivity, int maxWidth)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);

    constexpr int kBlock   = 2048;
    constexpr int kOrder   = 20;
    constexpr int kMerge   = 16; // samples apart that still count as one click
    constexpr int kMargin  = 2;  // samples either side of a click also replaced

    std::vector<int> flagged;

    // Blocks overlap by the model's order, so every sample has a full
    // history in some block.
    for (int start = std::max(0, from - kOrder); start < to; start += kBlock - kOrder)
    {
        const int end = std::min(size, start + kBlock);
        if (end - start <= kOrder * 2)
            break;

        std::vector<double> block(samples.begin() + start, samples.begin() + end);
        const auto          a = burg(block, kOrder);

        std::vector<double> residual((size_t) (end - start), 0.0);
        std::vector<double> magnitudes;
        magnitudes.reserve(residual.size());
        for (int i = kOrder; i < end - start; ++i)
        {
            residual[(size_t) i] = block[(size_t) i] - predict(a, block, i);
            magnitudes.push_back(std::abs(residual[(size_t) i]));
        }

        // The median rather than the RMS, so the clicks themselves don't
        // raise the bar they're measured against.
        auto middle = magnitudes.begin() + (long) magnitudes.size() / 2;
        std::nth_element(magnitudes.begin(), middle, magnitudes.end());
        // Never under -80 dB: a departure that small isn't a click, and a
        // perfectly predictable passage would otherwise flag its rounding.
        const double threshold = std::max(1.0e-4, *middle * 1.4826 * sensitivity);

        for (int i = kOrder; i < end - start; ++i)
            if (start + i >= from && start + i < to && std::abs(residual[(size_t) i]) > threshold)
                flagged.push_back(start + i);
    }

    std::sort(flagged.begin(), flagged.end());
    flagged.erase(std::unique(flagged.begin(), flagged.end()), flagged.end());

    std::vector<std::pair<int, int>> clicks;
    for (size_t i = 0; i < flagged.size();)
    {
        size_t j = i;
        while (j + 1 < flagged.size() && flagged[j + 1] - flagged[j] <= kMerge)
            ++j;

        const int clickFrom = std::max(from, flagged[i] - kMargin);
        const int clickTo   = std::min(to, flagged[j] + 1 + kMargin);
        if (clickTo - clickFrom <= maxWidth)
            clicks.emplace_back(clickFrom, clickTo);

        i = j + 1;
    }
    return clicks;
}

/** Each click findClicks finds, filled by interpolate. Returns how many were
    repaired. */
inline int removeClicks(std::vector<float>& samples, int from, int to, double sensitivity, int maxWidth)
{
    int repaired = 0;
    for (const auto& [clickFrom, clickTo] : findClicks(samples, from, to, sensitivity, maxWidth))
        if (interpolate(samples, clickFrom, clickTo, 1024, 24))
            ++repaired;
    return repaired;
}

/**
    De-crackle (Audition's DeCrackle): the dense crackle of a worn record or a
    bad cable, hundreds of clicks a second each a sample or two wide, mended
    by removeClicks tuned for them: nothing wider than a millisecond is
    touched, so no transient in the music is, and the threshold drops as
    @p amount (0 to 1) rises. Crackle is dense enough that each click's
    neighbourhood holds others, which would throw off the model a repair is
    built from: so every click is found first, all are filled, then all are
    filled again, each now from a neighbourhood already mended. Returns how
    many were mended.
*/
inline int decrackle(std::vector<float>& samples, int from, int to, double sampleRate, double amount)
{
    // In the model's usual errors, as findClicks counts them: lower flags
    // quieter clicks, but under about 4 the music itself starts to count.
    const double sensitivity = 8.0 - 4.0 * std::clamp(amount, 0.0, 1.0);
    // A spike throws the model's predictions off for its order's length
    // after it, so even a one-sample click is flagged about that wide.
    const int    maxWidth    = std::max(32, (int) std::lround(0.001 * sampleRate));

    const auto clicks = findClicks(samples, from, to, sensitivity, maxWidth);
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [clickFrom, clickTo] : clicks)
            interpolate(samples, clickFrom, clickTo, 1024, 24);
    return (int) clicks.size();
}

/**
    Clip Fix, as Audacity's: every run of samples in [@p from, @p to) at or
    over @p level (a magnitude, usually just under the channel's peak) is
    redrawn as the cubic through the two good samples either side, which
    carries the waveform on past the flat top it was cut to. Runs at the very
    edge of the audio, with no good samples on a side, stay as they are.
    Returns how many runs were rebuilt.
*/
inline int fixClipping(std::vector<float>& samples, int from, int to, float level)
{
    const int size = (int) samples.size();
    from = std::clamp(from, 0, size);
    to   = std::clamp(to, from, size);
    if (level <= 0.0f)
        return 0;

    int fixed = 0;
    for (int i = from; i < to;)
    {
        if (std::abs(samples[(size_t) i]) < level)
        {
            ++i;
            continue;
        }

        int end = i;
        while (end < to && std::abs(samples[(size_t) end]) >= level)
            ++end;

        if (i >= 2 && end + 1 < size)
        {
            // Lagrange cubic through t = -2, -1 (before) and n, n + 1 (after).
            const int    n  = end - i;
            const double t0 = -2.0, t1 = -1.0, t2 = n, t3 = n + 1.0;
            const double y0 = samples[(size_t) (i - 2)], y1 = samples[(size_t) (i - 1)];
            const double y2 = samples[(size_t) end], y3 = samples[(size_t) (end + 1)];

            for (int k = 0; k < n; ++k)
            {
                const double t = k;
                const double v = y0 * (t - t1) * (t - t2) * (t - t3) / ((t0 - t1) * (t0 - t2) * (t0 - t3))
                               + y1 * (t - t0) * (t - t2) * (t - t3) / ((t1 - t0) * (t1 - t2) * (t1 - t3))
                               + y2 * (t - t0) * (t - t1) * (t - t3) / ((t2 - t0) * (t2 - t1) * (t2 - t3))
                               + y3 * (t - t0) * (t - t1) * (t - t2) / ((t3 - t0) * (t3 - t1) * (t3 - t2));

                // Never pulled back inside what was recorded: a clipped
                // sample was at least as loud as the level it was cut to.
                const float recorded = samples[(size_t) (i + k)];
                samples[(size_t) (i + k)] = std::abs(v) >= std::abs(recorded) && v * recorded > 0.0
                                              ? (float) std::clamp(v, -4.0, 4.0)
                                              : recorded;
            }
            ++fixed;
        }
        i = end;
    }
    return fixed;
}

/** One RBJ notch. */
struct Notch
{
    double b0 = 1, b1 = 0, b2 = 1, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    Notch(double hz, double q, double sampleRate)
    {
        constexpr double pi    = 3.14159265358979323846;
        const double     w     = 2.0 * pi * hz / sampleRate;
        const double     alpha = std::sin(w) / (2.0 * q);
        const double     a0    = 1.0 + alpha;
        b0 = 1.0 / a0;
        b1 = -2.0 * std::cos(w) / a0;
        b2 = 1.0 / a0;
        a1 = -2.0 * std::cos(w) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double process(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1             = b1 * x - a1 * y + z2;
        z2             = b2 * x - a2 * y;
        return y;
    }
};

/**
    Hum removal: a narrow notch at @p fundamentalHz (50 or 60, for mains) and
    at each of its first @p harmonics - 1 multiples below Nyquist, run over the
    whole of @p samples so the filters have settled by the part that's kept.
    @p q sets how narrow: higher takes less of the music around each one.
*/
inline void removeHum(std::vector<float>& samples, double sampleRate, double fundamentalHz, int harmonics, double q)
{
    if (sampleRate <= 0.0 || fundamentalHz <= 0.0)
        return;

    std::vector<Notch> notches;
    for (int h = 1; h <= std::max(1, harmonics); ++h)
        if (fundamentalHz * h < sampleRate * 0.45)
            notches.emplace_back(fundamentalHz * h, std::max(1.0, q), sampleRate);

    for (auto& sample : samples)
    {
        double x = sample;
        for (auto& notch : notches)
            x = notch.process(x);
        sample = (float) x;
    }
}

} // namespace soundsplice::engine::repair
