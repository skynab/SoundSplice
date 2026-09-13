#pragma once

#include "engine/SynthTone.h"
#include "model/SynthPreset.h"

namespace looper::model
{
/**
    A synth sound purpose-tuned for @p tone, for the synth editor's one-click
    tone buttons — built the same in-memory, literal-struct-per-field way
    presetForGenre() and presetForGuitarTone() are (see GenrePresets.h,
    GuitarTonePresets.h): no file I/O, no save/load, just known-good values
    applied in one undo step.

    Returns a full SynthPreset, so applying one is exactly the same operation
    as loading a saved preset — the handler overwrites synthSettings and
    effectChain together, and neither the engine nor the serializers need to
    know these exist.
*/
inline SynthPreset presetForSynthTone(engine::SynthTone tone)
{
    SynthPreset p;
    p.name = engine::synthToneName(tone);

    switch (tone)
    {
        case engine::SynthTone::CyberBass:
        {
            // The genre's signature sound. Two things make it: several
            // detuned saws beating against each other (the "reese"), and a
            // filter low enough that what's left is mostly the growl of
            // those beats rather than the saws' own top end - then driven,
            // which turns the beating into harmonic movement instead of
            // just chorusing.
            p.synth.waveform  = 1; // saw
            p.synth.attackMs  = 1.0f;
            p.synth.decayMs   = 220.0f;
            p.synth.sustain   = 0.8f;
            p.synth.releaseMs = 120.0f;

            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0; // low-pass
            p.synth.filterCutoff    = 350.0f;
            p.synth.filterResonance = 1.6f;

            // The envelope is what stops it being a static pad: each note
            // opens the filter and closes it again, which reads as the
            // note being *played* rather than held.
            p.synth.filterEnvAmount    = 1800.0f;
            p.synth.filterEnvAttackMs  = 2.0f;
            p.synth.filterEnvDecayMs   = 180.0f;
            p.synth.filterEnvSustain   = 0.25f;
            p.synth.filterEnvReleaseMs = 120.0f;

            p.synth.subOscEnabled = true;
            p.synth.subOscLevel   = 0.55f; // the weight under all the detune

            p.synth.unisonVoices      = 4;
            p.synth.unisonDetuneCents = 18.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 14.0f;
            drive.drive.tone     = 0.4f;
            drive.drive.level    = 1.0f;
            drive.drive.hardClip = false;
            // No cabinet: that's a guitar speaker's rolloff, and it would
            // take the top off a synth bass that needs the grit to stay
            // audible in a mix.
            drive.drive.cabinet  = false;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -18.0f;
            compressor.compressor.ratio       = 4.0f;
            compressor.compressor.attackMs    = 5.0f;
            compressor.compressor.releaseMs   = 90.0f;
            compressor.compressor.makeUpDb    = 3.0f;

            p.effectChain = { drive, compressor };
            break;
        }
        case engine::SynthTone::CyberLead:
        {
            // The same detuned-saw family an octave up in character: bright
            // enough to cut, short enough to sit over a busy low end, with
            // a delay doing the work an arpeggio would otherwise have to.
            p.synth.waveform  = 1; // saw
            p.synth.attackMs  = 3.0f;
            p.synth.decayMs   = 160.0f;
            p.synth.sustain   = 0.5f;
            p.synth.releaseMs = 240.0f;

            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 2600.0f;
            p.synth.filterResonance = 1.2f;

            p.synth.filterEnvAmount    = 2500.0f;
            p.synth.filterEnvAttackMs  = 1.0f;
            p.synth.filterEnvDecayMs   = 120.0f;
            p.synth.filterEnvSustain   = 0.3f;
            p.synth.filterEnvReleaseMs = 200.0f;

            p.synth.unisonVoices      = 3;
            p.synth.unisonDetuneCents = 14.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 6.0f;
            drive.drive.tone     = 0.6f;
            drive.drive.level    = 0.9f;
            drive.drive.hardClip = false;
            drive.drive.cabinet  = false;

            EffectSlot delay;
            delay.kind           = EffectKind::Delay;
            delay.enabled        = true;
            delay.delay.enabled  = true;
            delay.delay.timeMs   = 375.0f; // a dotted eighth at 120bpm
            delay.delay.feedback = 0.35f;
            delay.delay.mix      = 0.28f;

            p.effectChain = { drive, delay };
            break;
        }
        case engine::SynthTone::DarkPad:
        {
            // Slow, wide and dark - the bed the other two sit on top of.
            // No drive at all: a pad's job here is space, and saturation
            // would put it in the same frequency fight as the bass.
            p.synth.waveform  = 3; // triangle
            p.synth.attackMs  = 600.0f;
            p.synth.decayMs   = 800.0f;
            p.synth.sustain   = 0.75f;
            p.synth.releaseMs = 1400.0f;

            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 900.0f;
            p.synth.filterResonance = 0.8f;

            p.synth.filterEnvAmount    = 700.0f;
            p.synth.filterEnvAttackMs  = 900.0f;
            p.synth.filterEnvDecayMs   = 1200.0f;
            p.synth.filterEnvSustain   = 0.5f;
            p.synth.filterEnvReleaseMs = 1400.0f;

            p.synth.unisonVoices      = 4;
            p.synth.unisonDetuneCents = 22.0f;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.25f;
            chorus.chorus.depth   = 0.7f;
            chorus.chorus.mix     = 0.5f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.85f;
            reverb.reverb.damping  = 0.45f;
            reverb.reverb.mix      = 0.45f;

            p.effectChain = { chorus, reverb };
            break;
        }
    }

    return p;
}

} // namespace looper::model
