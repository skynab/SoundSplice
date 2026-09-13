#pragma once

namespace looper::model
{
/**
    Per-track synth timbre for an Instrument track (model::Track::synthSettings)
    — the model-side counterpart to engine::SynthInstrumentNode's per-voice
    oscillator/filter/envelope, the same way FilterSettings/DelaySettings are
    the model-side counterpart to the master FilterEffect/DelayEffect.

    waveform: 0 = sine, 1 = saw, 2 = square, 3 = triangle (a plain int, not a
    shared enum with the engine, matching how FilterSettings::mode already
    documents its meaning by comment rather than a shared type).

    The filter here shapes this track's own voices before they're mixed in —
    distinct from FilterSettings, which shapes the whole master bus.
*/
struct SynthSettings
{
    int   waveform = 0;

    float attackMs  = 5.0f;
    float decayMs   = 120.0f;
    float sustain   = 0.7f;  // 0..1
    float releaseMs = 250.0f;

    bool  filterEnabled = false;
    int   filterMode    = 0; // 0 = low-pass, 1 = high-pass, 2 = band-pass
    float filterCutoff    = 1000.0f; // Hz
    float filterResonance = 0.707f;

    float gainDb = 0.0f; // additional per-track output trim, on top of the track fader

    // Filter envelope: sweeps filterCutoff over the note by filterEnvAmount
    // (Hz, bipolar) under its own ADSR, independent of the amp envelope
    // above — see engine::SynthVoice. Amount 0 (the default) is a no-op.
    float filterEnvAmount     = 0.0f;
    float filterEnvAttackMs   = 0.0f;
    float filterEnvDecayMs    = 0.0f;
    float filterEnvSustain    = 1.0f;
    float filterEnvReleaseMs  = 0.0f;

    // A sub-oscillator: a fixed sine one octave down, mixed in for low-end
    // weight (see engine::SynthVoice).
    bool  subOscEnabled = false;
    float subOscLevel   = 0.3f; // 0..1

    // unisonVoices copies of the main oscillator, detuned symmetrically
    // across unisonDetuneCents and summed (see engine::SynthVoice). 1 = off.
    int   unisonVoices      = 1;
    float unisonDetuneCents = 12.0f;

    bool operator==(const SynthSettings&) const = default;
};

} // namespace looper::model
