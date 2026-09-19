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

} // namespace soundsplice::engine::hqstretch
