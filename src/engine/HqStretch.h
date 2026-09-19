#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace soundsplice::engine::hqstretch
{
/**
    Time-stretching and pitch-shifting by Signalsmith Stretch (MIT; see
    cmake/stretch.cmake), in place of the phase vocoder in TimeStretch.h for
    Change Tempo and Change Pitch. It keeps transients sharper and a voice
    less phasey than a plain vocoder, and it can keep a voice's formants where
    they are while its pitch moves, so a shifted voice doesn't turn into a
    chipmunk or a giant.

    Offline and exact: the result is exactly @p lengthFactor times as long,
    lined up with the input from its first sample, every channel stretched
    together so the stereo image holds.
*/
struct Settings
{
    double lengthFactor  = 1.0;   // 2 = twice as long (half the tempo)
    double semitones     = 0.0;   // pitch, keeping the length
    bool   keepFormants  = false; // a voice's character stays put as its pitch moves
};

inline std::vector<std::vector<float>> process(const std::vector<std::vector<float>>& channels, double sampleRate,
                                               const Settings& settings)
{
    if (channels.empty() || channels[0].empty() || sampleRate <= 0.0 || settings.lengthFactor <= 0.0)
        return channels;

    const int inputLength  = (int) channels[0].size();
    const int outputLength = std::max(1, (int) std::lround(inputLength * settings.lengthFactor));
    const int count        = (int) channels.size();

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(count, (float) sampleRate);
    stretch.setTransposeSemitones((float) settings.semitones);
    if (settings.keepFormants)
        stretch.setFormantSemitones(0.0f, true); // formants where they were, whatever the pitch

    std::vector<std::vector<float>> output((size_t) count, std::vector<float>((size_t) outputLength, 0.0f));
    std::vector<const float*>       in;
    std::vector<float*>             out;
    for (int c = 0; c < count; ++c)
    {
        in.push_back(channels[(size_t) c].data());
        out.push_back(output[(size_t) c].data());
    }

    // Too short for the stretcher to take in (under about a block): nothing,
    // for the caller to fall back on TimeStretch.h, near enough at that size.
    if (! stretch.exact(in.data(), inputLength, out.data(), outputLength))
        return {};
    return output;
}

/**
    Sliding stretch (Audacity's): tempo and pitch that change steadily from
    one end of the audio to the other, a ritardando or a tape winding down.
    Tempo is as a percentage change (+100 twice as fast, -50 half as fast),
    pitch in semitones, each moving in a straight line from its start value
    to its end value across the input.
*/
struct Slide
{
    double startTempoPercent = 0.0;
    double endTempoPercent   = 0.0;
    double startSemitones    = 0.0;
    double endSemitones      = 0.0;
    bool   keepFormants      = false;

    /** Input samples per output sample at @p fraction (0 to 1) of the input. */
    double rateAt(double fraction) const noexcept
    {
        const double from = std::max(0.05, 1.0 + startTempoPercent / 100.0);
        const double to   = std::max(0.05, 1.0 + endTempoPercent / 100.0);
        return from + (to - from) * std::clamp(fraction, 0.0, 1.0);
    }

    double semitonesAt(double fraction) const noexcept
    {
        return startSemitones + (endSemitones - startSemitones) * std::clamp(fraction, 0.0, 1.0);
    }

    /** How long @p inputLength samples come out: the sum of 1 / rate across
        them, which for a rate in a straight line is a logarithm. */
    double outputLength(double inputLength) const noexcept
    {
        const double from = rateAt(0.0), to = rateAt(1.0);
        return std::abs(to - from) < 1.0e-9 ? inputLength / from
                                            : inputLength * std::log(to / from) / (to - from);
    }
};

/** @p channels with @p settings applied; empty if too short to take in, as
    process() is. */
inline std::vector<std::vector<float>> slide(const std::vector<std::vector<float>>& channels, double sampleRate,
                                             const Slide& settings)
{
    if (channels.empty() || channels[0].empty() || sampleRate <= 0.0)
        return {};

    const int inputLength  = (int) channels[0].size();
    const int outputLength = std::max(1, (int) std::lround(settings.outputLength(inputLength)));
    const int count        = (int) channels.size();

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(count, (float) sampleRate);
    stretch.setTransposeSemitones((float) settings.semitonesAt(0.0));
    if (settings.keepFormants)
        stretch.setFormantSemitones(0.0f, true);

    // As exact() does it: a seek to line the start up, the body processed,
    // and the tail flushed, but with the rate and pitch moving block by block.
    const double startRate  = settings.rateAt(0.0), endRate = settings.rateAt(1.0);
    const int    seekLength = stretch.outputSeekLength((float) startRate);
    constexpr int kBlock    = 256;
    if (inputLength < seekLength + kBlock)
        return {};

    std::vector<std::vector<float>> output((size_t) count, std::vector<float>((size_t) outputLength, 0.0f));
    std::vector<const float*>       in((size_t) count);
    std::vector<float*>             out((size_t) count);
    const auto point = [&](int inputAt, int outputAt)
    {
        for (int c = 0; c < count; ++c)
        {
            in[(size_t) c]  = channels[(size_t) c].data() + inputAt;
            out[(size_t) c] = output[(size_t) c].data() + outputAt;
        }
    };

    point(0, 0);
    stretch.outputSeek(in.data(), seekLength);

    // The last seekLength of input comes out of the flush, at the end rate;
    // the body's output is shared among its blocks by 1 / rate at each one's
    // place in the sound (which the processing trails the input by seekLength).
    const int flushLength = std::clamp((int) std::lround(seekLength / endRate), 0, outputLength);
    const int bodyLength  = outputLength - flushLength;

    std::vector<double> share;
    double              total = 0.0;
    for (int at = seekLength; at < inputLength; at += kBlock)
    {
        const int    n        = std::min(kBlock, inputLength - at);
        const double fraction = (at - seekLength + n * 0.5) / (double) inputLength;
        share.push_back(n / settings.rateAt(fraction));
        total += share.back();
    }

    double target = 0.0;
    int    written = 0;
    size_t block   = 0;
    for (int at = seekLength; at < inputLength; at += kBlock, ++block)
    {
        const int    n        = std::min(kBlock, inputLength - at);
        const double fraction = (at - seekLength + n * 0.5) / (double) inputLength;
        target += share[block] * bodyLength / std::max(1.0e-9, total);
        const int outCount = std::max(0, std::min(bodyLength, (int) std::lround(target)) - written);

        stretch.setTransposeSemitones((float) settings.semitonesAt(fraction));
        point(at, written);
        stretch.process(in.data(), n, out.data(), outCount);
        written += outCount;
    }

    point(0, written);
    stretch.flush(out.data(), outputLength - written, (float) endRate);
    return output;
}

/** @p channels with their pitch moved by @p semitonesAt(sample) semitones
    around each input sample, the length kept: pitch correction's pass. The
    curve is read at the stretcher's processing position, which trails what
    it has been fed by its input latency, so each shift lands where it was
    measured. Empty if too short to take in. */
template <typename Curve>
std::vector<std::vector<float>> transposeCurve(const std::vector<std::vector<float>>& channels, double sampleRate,
                                               Curve&& semitonesAt, bool keepFormants)
{
    if (channels.empty() || channels[0].empty() || sampleRate <= 0.0)
        return {};

    const int length = (int) channels[0].size();
    const int count  = (int) channels.size();

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(count, (float) sampleRate);
    if (keepFormants)
        stretch.setFormantSemitones(0.0f, true);
    stretch.setTransposeSemitones((float) semitonesAt(0));

    const int     seekLength = stretch.outputSeekLength(1.0f);
    constexpr int kBlock     = 256;
    if (length < seekLength + kBlock)
        return {};

    std::vector<std::vector<float>> output((size_t) count, std::vector<float>((size_t) length, 0.0f));
    std::vector<const float*>       in((size_t) count);
    std::vector<float*>             out((size_t) count);
    const auto point = [&](int inputAt, int outputAt)
    {
        for (int c = 0; c < count; ++c)
        {
            in[(size_t) c]  = channels[(size_t) c].data() + inputAt;
            out[(size_t) c] = output[(size_t) c].data() + outputAt;
        }
    };

    point(0, 0);
    stretch.outputSeek(in.data(), seekLength);

    const int body    = length - seekLength; // the flush gives the rest, at rate 1
    int       written = 0;
    for (int at = seekLength; at < length; at += kBlock)
    {
        const int n        = std::min(kBlock, length - at);
        const int outCount = std::max(0, std::min(n, body - written));
        const int now      = std::clamp(at - stretch.inputLatency() + n / 2, 0, length - 1);
        stretch.setTransposeSemitones((float) semitonesAt(now));
        point(at, written);
        stretch.process(in.data(), n, out.data(), outCount);
        written += outCount;
    }

    point(0, written);
    stretch.flush(out.data(), length - written, 1.0f);
    return output;
}

} // namespace soundsplice::engine::hqstretch
