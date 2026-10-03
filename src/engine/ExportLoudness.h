#pragma once

#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Loudness.h"
#include "engine/Maximizer.h"

namespace soundsplice::engine
{
/**
    Loudness-normalize on export: a rendered mix brought to a LUFS target
    with its true peak held under a ceiling, the way a podcast host or a
    streaming service asks for (-16 LUFS, -1 dBTP, say).

    Unlike Match Loudness, which only turns a clip down when the target
    would push it over the ceiling, this reaches the target: the gain goes
    on, and whatever then crosses the ceiling is limited. The limiter
    (Maximizer) is a sample-peak brickwall, and a true peak - between the
    samples - can still land above its ceiling, so it's run again lower by
    however much the true peak went over, until the true peak fits. Offline,
    a few passes cost nothing, and the result is checked rather than hoped.
*/
struct ExportLoudnessResult
{
    bool   measured       = false; // false: silent or too short to measure, left alone
    double gainDb         = 0.0;
    double integratedLufs = LoudnessMeter::kSilence; // after
    double truePeakDb     = LoudnessMeter::kSilence; // after
    bool   limited        = false;
};

namespace exportloudness
{
    /** One channel is measured as dual mono: on both speakers, which is
        how everything here plays it (a mono reference in A/B is heard on
        both sides), so the reading is what's heard. BS.1770 would read a
        lone channel 3 LU quieter, the convention behind "-19 LUFS mono"
        beside "-16 LUFS stereo" - the same loudness out of two speakers.
        Exports are always rendered in stereo, so a target never meets this. */
    inline LoudnessReport measure(const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        LoudnessMeter meter;
        meter.prepare(sampleRate, 2);
        const int    last = audio.getNumChannels() - 1;
        const float* both[2] { audio.getReadPointer(0), audio.getReadPointer(std::min(1, last)) };
        meter.process(both, 2, audio.getNumSamples());
        return LoudnessReport::of(meter, audio.getNumSamples() / sampleRate);
    }

    /** @p audio through a lookahead limiter at @p ceilingDb, lined back up. */
    inline void limit(juce::AudioBuffer<float>& audio, double sampleRate, float ceilingDb)
    {
        Maximizer limiter;
        const int channels = std::min(2, audio.getNumChannels());
        limiter.prepare(sampleRate, channels);
        limiter.setInputGainDb(0.0f);
        limiter.setCeilingDb(ceilingDb);
        limiter.setReleaseMs(80.0f);

        const int latency = limiter.latencySamples();
        const int total   = audio.getNumSamples();
        float     frame[2] {};
        for (int i = 0; i < total + latency; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
                frame[ch] = i < total ? audio.getSample(ch, i) : 0.0f;
            limiter.processFrame(frame, channels);
            if (i >= latency)
                for (int ch = 0; ch < channels; ++ch)
                    audio.setSample(ch, i - latency, frame[ch]);
        }
        // A third channel and up (not that a mix has one) gets the same
        // gain as neither - leave it as it was rather than guess.
    }
}

/** Brings @p audio to @p targetLufs with its true peak at or under
    @p ceilingDbtp. */
inline ExportLoudnessResult normalizeForExport(juce::AudioBuffer<float>& audio, double sampleRate, double targetLufs,
                                               double ceilingDbtp = -1.0)
{
    ExportLoudnessResult result;
    if (audio.getNumSamples() == 0 || audio.getNumChannels() == 0 || sampleRate <= 0.0)
        return result;

    const auto before = exportloudness::measure(audio, sampleRate);
    if (! std::isfinite(before.integratedLufs))
        return result;

    result.measured = true;
    result.gainDb   = targetLufs - before.integratedLufs;
    audio.applyGain(juce::Decibels::decibelsToGain((float) result.gainDb));

    auto after = before.withGain(result.gainDb);
    float ceiling = (float) ceilingDbtp;
    for (int pass = 0; pass < 6 && std::isfinite(after.truePeakDb) && after.truePeakDb > ceilingDbtp + 0.01; ++pass)
    {
        // Lower by the overshoot of the last pass (none on the first).
        if (pass > 0)
            ceiling -= (float) (after.truePeakDb - ceilingDbtp) + 0.05f;
        exportloudness::limit(audio, sampleRate, std::max(ceiling, -24.0f));
        after          = exportloudness::measure(audio, sampleRate);
        result.limited = true;
    }

    result.integratedLufs = after.integratedLufs;
    result.truePeakDb     = after.truePeakDb;
    return result;
}

} // namespace soundsplice::engine
