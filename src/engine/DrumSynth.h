#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "engine/StateVariableFilter.h"

namespace looper::engine
{
namespace drum_synth_detail
{
    constexpr double kTwoPi = 6.283185307179586;
}

/**
    Procedural one-shot drum sounds — kick, snare, closed hat, clap — so the
    app can ship a starter kit without bundling (or licensing) anyone else's
    recordings. Every sound here is a handful of well-worn synthesis tricks
    that predate sampling entirely: a pitch-swept sine for a kick, filtered
    noise for a hat, noise plus a short tone for a snare. Nothing here claims
    to sound like a real drum kit — it claims to be an unmistakable "kick",
    "snare", or "hat" on first listen, which is all a default needs to be.

    JUCE-free, like the rest of this engine's DSP, so the shape of each sound
    — that it decays, that it doesn't clip, that a kick actually falls in
    pitch — is a headless, testable claim. Writing the result to a .wav file
    is someone else's job (see the JUCE-side caller); this only produces the
    samples.

    A fixed seed rather than real randomness: the same variant must sound
    the same on every run, including in a test that checks it's finite and
    bounded — nondeterministic noise would make that check nondeterministic
    too.
*/
inline std::vector<float> synthesizeKick(double sampleRate, float startHz, float endHz,
                                         float pitchDecayMs, float ampDecayMs, float drive)
{
    const int totalSamples = std::max(1, (int) (sampleRate * (ampDecayMs * 0.001 + 0.05)));
    std::vector<float> out((size_t) totalSamples, 0.0f);

    const double pitchTau = std::max(0.001, (double) pitchDecayMs * 0.001);
    const double ampTau   = std::max(0.001, (double) ampDecayMs * 0.001);
    const double driveAmount = std::max(1.0, (double) drive);
    const double driveNorm   = std::tanh(driveAmount); // so drive doesn't also change loudness

    double phase = 0.0;
    for (int i = 0; i < totalSamples; ++i)
    {
        const double t    = (double) i / sampleRate;
        const double freq = (double) endHz + ((double) startHz - (double) endHz) * std::exp(-t / pitchTau);
        phase += freq / sampleRate;
        if (phase >= 1.0)
            phase -= 1.0;

        const double raw = std::sin(drum_synth_detail::kTwoPi * phase);
        const double shaped = std::tanh(raw * driveAmount) / driveNorm;
        const double amp = std::exp(-t / ampTau);
        out[(size_t) i] = (float) (shaped * amp);
    }
    return out;
}

inline std::vector<float> synthesizeSnare(double sampleRate, float toneHz, float toneMix,
                                          float ampDecayMs, float noiseCentreHz, unsigned int seed)
{
    const int totalSamples = std::max(1, (int) (sampleRate * (ampDecayMs * 0.001 + 0.03)));
    std::vector<float> out((size_t) totalSamples, 0.0f);

    StateVariableFilter noiseFilter;
    noiseFilter.prepare(sampleRate);
    noiseFilter.setMode(StateVariableFilter::Mode::BandPass);
    noiseFilter.setCutoff(noiseCentreHz);
    noiseFilter.setResonance(0.8f);

    std::minstd_rand rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    const double ampTau = std::max(0.001, (double) ampDecayMs * 0.001);
    const float  mix    = std::clamp(toneMix, 0.0f, 1.0f);
    double tonePhase = 0.0;

    for (int i = 0; i < totalSamples; ++i)
    {
        const double t = (double) i / sampleRate;

        const float noise         = dist(rng);
        const float filteredNoise = noiseFilter.processSample(noise);

        tonePhase += (double) toneHz / sampleRate;
        if (tonePhase >= 1.0)
            tonePhase -= 1.0;
        const float tone = (float) std::sin(drum_synth_detail::kTwoPi * tonePhase);

        const float amp = (float) std::exp(-t / ampTau);
        out[(size_t) i] = (filteredNoise * (1.0f - mix) + tone * mix) * amp;
    }
    return out;
}

inline std::vector<float> synthesizeHat(double sampleRate, float cutoffHz, float ampDecayMs, unsigned int seed)
{
    const int totalSamples = std::max(1, (int) (sampleRate * (ampDecayMs * 0.001 + 0.01)));
    std::vector<float> out((size_t) totalSamples, 0.0f);

    StateVariableFilter filter;
    filter.prepare(sampleRate);
    filter.setMode(StateVariableFilter::Mode::HighPass);
    filter.setCutoff(cutoffHz);
    filter.setResonance(0.5f);

    std::minstd_rand rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    const double ampTau = std::max(0.001, (double) ampDecayMs * 0.001);
    for (int i = 0; i < totalSamples; ++i)
    {
        const double t = (double) i / sampleRate;
        const float filtered = filter.processSample(dist(rng));
        const float amp = (float) std::exp(-t / ampTau);
        out[(size_t) i] = filtered * amp;
    }
    return out;
}

inline std::vector<float> synthesizeClap(double sampleRate, float cutoffHz, float ampDecayMs, unsigned int seed)
{
    // Three quick, tight bursts (the "clap" transient) followed by one
    // longer one (the room tail) — a clap is a handful of hands, not one.
    const double burstStartsMs[] = { 0.0, 12.0, 24.0, 36.0 };
    const int    totalSamples    = std::max(1, (int) (sampleRate * ((ampDecayMs + 40.0) * 0.001)));
    std::vector<float> out((size_t) totalSamples, 0.0f);

    StateVariableFilter filter;
    filter.prepare(sampleRate);
    filter.setMode(StateVariableFilter::Mode::BandPass);
    filter.setCutoff(cutoffHz);
    filter.setResonance(0.9f);

    std::minstd_rand rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    for (size_t b = 0; b < 4; ++b)
    {
        const bool   isTail       = (b == 3);
        const double burstDecayMs = isTail ? (double) ampDecayMs : 8.0;
        const double ampTau       = std::max(0.001, burstDecayMs * 0.001);
        const int    startSample  = (int) (burstStartsMs[b] * 0.001 * sampleRate);

        for (int i = startSample; i < totalSamples; ++i)
        {
            const double t        = (double) (i - startSample) / sampleRate;
            const float  filtered = filter.processSample(dist(rng));
            const float  amp      = (float) std::exp(-t / ampTau);
            out[(size_t) i] += filtered * amp * 0.6f;
        }
    }
    return out;
}

/** Scales @p buffer so its loudest sample sits at @p targetPeak, leaving
    silence alone — variants built from different parameters would otherwise
    land at wildly different loudnesses, which reads as broken more than as
    character. */
inline void normalizePeak(std::vector<float>& buffer, float targetPeak = 0.9f)
{
    float peak = 0.0f;
    for (float s : buffer)
        peak = std::max(peak, std::abs(s));

    if (peak <= 1.0e-6f)
        return; // silence stays silence rather than amplifying noise floor to targetPeak

    const float scale = targetPeak / peak;
    for (float& s : buffer)
        s *= scale;
}

} // namespace looper::engine
