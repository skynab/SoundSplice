#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine::pitch
{
/**
    Pitch detection, for the tuner (Analyze > Detect Pitch) and pitch
    correction: YIN (de Cheveigné and Kawahara, 2002). A frame's period is the
    lag at which it best matches itself, measured by the difference function
    normalised by its running mean, so the first dip under a threshold is
    taken rather than the deepest (which is often an octave too low); then
    refined between samples by a parabola. A frame with no clear dip is
    unvoiced: noise, breath, silence. The difference function comes from an
    autocorrelation by FFT, so a song's worth is quick.

    JUCE-free, so the notes it hears are tested headless.
*/
struct Reading
{
    double hz         = 0.0; // 0 when unvoiced
    double confidence = 0.0; // 1 - the dip's depth: 1 a pure tone, 0 noise

    bool voiced() const noexcept { return hz > 0.0; }
};

constexpr double kMinHz = 60.0;   // below a bass voice's lowest
constexpr double kMaxHz = 1500.0; // above a soprano's highest

/** The pitch of @p frame (@p count samples, at least two of the longest
    period long). */
inline Reading detect(const float* frame, int count, double sampleRate, double threshold = 0.15)
{
    const int minLag = std::max(2, (int) std::floor(sampleRate / kMaxHz));
    const int maxLag = std::min(count / 2, (int) std::ceil(sampleRate / kMinHz));
    if (maxLag <= minLag + 2)
        return {};

    const int window = count - maxLag;

    // Silence has no pitch, and its difference function is all zeros.
    double energy = 0.0;
    for (int i = 0; i < window; ++i)
        energy += (double) frame[i] * frame[i];
    if (energy / window < 1.0e-8)
        return {};

    // The difference at each lag, sum (x[i] - x[i + lag])^2, is the two
    // stretches' energies less twice their correlation: the correlation for
    // every lag at once by FFT, the energies by running sums. Far quicker
    // than summing each lag's differences directly.
    size_t size = 1;
    while (size < (size_t) (count + window))
        size <<= 1;
    std::vector<float> re(size, 0.0f), im(size, 0.0f), wre(size, 0.0f), wim(size, 0.0f);
    for (int i = 0; i < count; ++i)
        re[(size_t) i] = frame[i];
    for (int i = 0; i < window; ++i)
        wre[(size_t) i] = frame[i];
    fft::transform(re, im, false);
    fft::transform(wre, wim, false);
    for (size_t k = 0; k < size; ++k)
    {
        const float r = re[k] * wre[k] + im[k] * wim[k]; // X times the window's conjugate
        const float i = im[k] * wre[k] - re[k] * wim[k];
        re[k] = r;
        im[k] = i;
    }
    fft::transform(re, im, true);

    std::vector<double> squares((size_t) count + 1, 0.0);
    for (int i = 0; i < count; ++i)
        squares[(size_t) i + 1] = squares[(size_t) i] + (double) frame[i] * frame[i];

    std::vector<double> difference((size_t) maxLag + 1, 0.0);
    const double        head = squares[(size_t) window];
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        const double shifted = squares[(size_t) (lag + window)] - squares[(size_t) lag];
        difference[(size_t) lag] = std::max(0.0, head + shifted - 2.0 * re[(size_t) lag]);
    }

    // Normalised by the running mean, so lag 0's trivial zero doesn't count.
    std::vector<double> normalised((size_t) maxLag + 1, 1.0);
    double              running = 0.0;
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        running += difference[(size_t) lag];
        normalised[(size_t) lag] = running > 0.0 ? difference[(size_t) lag] * lag / running : 1.0;
    }

    int best = -1;
    for (int lag = minLag; lag < maxLag; ++lag)
        if (normalised[(size_t) lag] < threshold)
        {
            // Down to the bottom of this dip.
            while (lag + 1 < maxLag && normalised[(size_t) lag + 1] < normalised[(size_t) lag])
                ++lag;
            best = lag;
            break;
        }
    if (best < 0)
        return {};

    const double a = normalised[(size_t) best - 1], b = normalised[(size_t) best], c = normalised[(size_t) best + 1];
    const double shift  = (a - 2.0 * b + c) > 0.0 ? 0.5 * (a - c) / (a - 2.0 * b + c) : 0.0;
    const double period = best + std::clamp(shift, -0.5, 0.5);
    return { sampleRate / period, std::clamp(1.0 - b, 0.0, 1.0) };
}

/** A reading every @p hop samples of @p samples, each from the frame
    centred there. */
inline std::vector<Reading> track(const std::vector<float>& samples, double sampleRate, int hop = 256)
{
    const int          frame = std::max(64, (int) std::ceil(sampleRate / kMinHz) * 2 + 64);
    std::vector<Reading> readings;
    std::vector<float>   padded((size_t) frame, 0.0f);
    for (int centre = 0; centre < (int) samples.size(); centre += hop)
    {
        const int start = centre - frame / 2;
        for (int i = 0; i < frame; ++i)
        {
            const int at        = start + i;
            padded[(size_t) i] = at >= 0 && at < (int) samples.size() ? samples[(size_t) at] : 0.0f;
        }
        readings.push_back(detect(padded.data(), frame, sampleRate));
    }
    return readings;
}

/** A frequency as a MIDI note number, fractional: 69 is A4 at 440 Hz. */
inline double noteOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

/** The name of MIDI note @p note, rounded, with its octave: "A4", "C#3". */
inline std::string nameOf(double note)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = (int) std::lround(note);
    return std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

/** The scales pitch correction snaps to. The values are stored in the app's
    settings. */
enum class Scale
{
    Chromatic = 0,
    Major     = 1,
    Minor     = 2
};

/** The note of @p scale in @p key (0 = C) nearest @p note. */
inline double nearestInScale(double note, int key, Scale scale)
{
    static constexpr std::array<int, 7> major { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr std::array<int, 7> minor { 0, 2, 3, 5, 7, 8, 10 };
    if (scale == Scale::Chromatic)
        return std::round(note);

    const auto& steps = scale == Scale::Major ? major : minor;
    double      best = std::round(note), distance = 1.0e9;
    for (int octave = (int) std::floor(note / 12.0) - 1; octave <= (int) std::floor(note / 12.0) + 1; ++octave)
        for (int step : steps)
        {
            const double candidate = octave * 12.0 + key + step;
            if (std::abs(candidate - note) < distance)
            {
                distance = std::abs(candidate - note);
                best     = candidate;
            }
        }
    return best;
}

/**
    Pitch correction's curve: for each reading, how many semitones to move the
    audio there. Voiced readings are pulled towards the nearest note of the
    scale by @p strength (0 to 1); the result is smoothed over @p speedMs, 0
    snapping at once (the robotic effect), a few tens of milliseconds sounding
    natural; unvoiced stretches ease back to no change, so a breath or a
    consonant is left alone.
*/
inline std::vector<double> correction(const std::vector<Reading>& readings, double sampleRate, int hop, int key,
                                      Scale scale, double strength, double speedMs)
{
    std::vector<double> shift(readings.size(), 0.0);
    const double        follow = speedMs <= 0.0 ? 1.0 : 1.0 - std::exp(-(double) hop / (speedMs * 0.001 * sampleRate));
    double              current = 0.0;
    for (size_t i = 0; i < readings.size(); ++i)
    {
        double wanted = 0.0;
        if (readings[i].voiced())
        {
            const double note = noteOf(readings[i].hz);
            wanted            = (nearestInScale(note, key, scale) - note) * std::clamp(strength, 0.0, 1.0);
        }
        current += follow * (wanted - current);
        shift[i] = current;
    }
    return shift;
}

} // namespace soundsplice::engine::pitch
