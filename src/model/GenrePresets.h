#pragma once

#include "engine/Genre.h"
#include "model/SynthPreset.h"
#include "model/SynthTonePresets.h"

namespace looper::model
{
/**
    A synth sound purpose-tuned for @p genre, for the Generate Loop dialog's
    Instrument-track genre picker — one `SynthPreset` per engine::Genre, so
    picking "Lo-Fi" swaps in something that actually sounds lo-fi rather than
    just biasing the pattern's rhythm.

    Built the same literal-struct-per-field way
    MainComponent::seedFactoryPresets() builds its four factory presets (same
    field names, same units — Hz/ms/dB), but these are constructed in memory
    on demand rather than written to disk: unlike a user's saved presets,
    there's no reason to clutter the presets directory with six files nobody
    asked to save, and applying one is exactly as cheap as reading a
    `.looperpreset` file, minus the file I/O.
*/
inline SynthPreset presetForGenre(engine::Genre genre)
{
    SynthPreset p;
    p.name = engine::genreName(genre);

    switch (genre)
    {
        case engine::Genre::House:
        {
            // A bright, filtered saw pluck/stab with a touch of chorus - the
            // classic house chord-stab timbre.
            p.synth.waveform   = 1; // saw
            p.synth.attackMs   = 3.0f;
            p.synth.decayMs    = 150.0f;
            p.synth.sustain    = 0.6f;
            p.synth.releaseMs  = 200.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 2500.0f;
            p.synth.filterResonance = 0.9f;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.5f;
            chorus.chorus.depth   = 0.4f;
            chorus.chorus.mix     = 0.35f;
            p.effectChain = { chorus };
            break;
        }
        case engine::Genre::Techno:
        {
            // Fast, resonant, driven - a tight acid-adjacent stab/bass.
            p.synth.waveform   = 1; // saw
            p.synth.attackMs   = 2.0f;
            p.synth.decayMs    = 100.0f;
            p.synth.sustain    = 0.6f;
            p.synth.releaseMs  = 80.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 900.0f;
            p.synth.filterResonance = 1.8f;

            EffectSlot drive;
            drive.kind          = EffectKind::Drive;
            drive.enabled       = true;
            drive.drive.enabled = true;
            drive.drive.drive   = 8.0f;
            drive.drive.tone    = 0.5f;
            drive.drive.level   = 0.7f;
            drive.drive.cabinet = true;
            p.effectChain = { drive };
            break;
        }
        case engine::Genre::HipHop:
        {
            // Warm, mellow, low-pass-filtered keys/bass with a small room -
            // boom-bap rather than anything bright.
            p.synth.waveform   = 3; // triangle
            p.synth.attackMs   = 10.0f;
            p.synth.decayMs    = 300.0f;
            p.synth.sustain    = 0.5f;
            p.synth.releaseMs  = 400.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 1200.0f;
            p.synth.filterResonance = 0.7f;

            EffectSlot reverb;
            reverb.kind           = EffectKind::Reverb;
            reverb.enabled        = true;
            reverb.reverb.enabled = true;
            reverb.reverb.roomSize = 0.3f;
            reverb.reverb.damping  = 0.6f;
            reverb.reverb.mix      = 0.2f;
            p.effectChain = { reverb };
            break;
        }
        case engine::Genre::Trap:
        {
            // A bright, short-decay bell/pluck with a bouncy echo - trap
            // melodic lines are plucky and answered by their own delay.
            p.synth.waveform   = 2; // square
            p.synth.attackMs   = 1.0f;
            p.synth.decayMs    = 180.0f;
            p.synth.sustain    = 0.0f;
            p.synth.releaseMs  = 60.0f;

            EffectSlot delay;
            delay.kind          = EffectKind::Delay;
            delay.enabled       = true;
            delay.delay.enabled = true;
            delay.delay.timeMs   = 180.0f;
            delay.delay.feedback = 0.3f;
            delay.delay.mix      = 0.25f;
            p.effectChain = { delay };
            break;
        }
        case engine::Genre::Ambient:
        {
            // A slow-attack, long-release sine pad washed in a large reverb.
            p.synth.waveform   = 0; // sine
            p.synth.attackMs   = 800.0f;
            p.synth.decayMs    = 800.0f;
            p.synth.sustain    = 0.9f;
            p.synth.releaseMs  = 2000.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 2000.0f;
            p.synth.filterResonance = 0.5f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.85f;
            reverb.reverb.damping  = 0.3f;
            reverb.reverb.mix      = 0.5f;
            p.effectChain = { reverb };
            break;
        }
        case engine::Genre::LoFi:
        {
            // Dark, muffled, and gently overdriven - tape-warmth rather than
            // hi-fi clarity, with a slow chorus for pitch-wobble character.
            p.synth.waveform   = 3; // triangle
            p.synth.attackMs   = 15.0f;
            p.synth.decayMs    = 250.0f;
            p.synth.sustain    = 0.6f;
            p.synth.releaseMs  = 300.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 1000.0f;
            p.synth.filterResonance = 0.6f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 3.0f;
            drive.drive.tone     = 0.3f;
            drive.drive.level    = 0.8f;
            drive.drive.hardClip = false;
            drive.drive.cabinet  = false;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.3f;
            chorus.chorus.depth   = 0.5f;
            chorus.chorus.mix     = 0.4f;

            p.effectChain = { drive, chorus };
            break;
        }
        case engine::Genre::Synthwave:
        {
            // A wide, detuned saw stack swept by a slow filter envelope, in
            // the general cyberpunk/synthwave/retrowave vein - unison is
            // most of what makes a synth lead read as "huge" in this genre,
            // the way a single oscillator never can.
            p.synth.waveform   = 1; // saw
            p.synth.attackMs   = 5.0f;
            p.synth.decayMs    = 400.0f;
            p.synth.sustain    = 0.7f;
            p.synth.releaseMs  = 300.0f;
            p.synth.filterEnabled   = true;
            p.synth.filterMode      = 0;
            p.synth.filterCutoff    = 600.0f;
            p.synth.filterResonance = 1.5f;
            p.synth.filterEnvAmount    = 3000.0f;
            p.synth.filterEnvAttackMs  = 20.0f;
            p.synth.filterEnvDecayMs   = 350.0f;
            p.synth.filterEnvSustain   = 0.4f;
            p.synth.filterEnvReleaseMs = 250.0f;
            p.synth.unisonVoices      = 5;
            p.synth.unisonDetuneCents = 18.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 6.0f;
            drive.drive.tone     = 0.6f;
            drive.drive.level    = 0.7f;
            drive.drive.hardClip = false;
            drive.drive.cabinet  = false;
            p.effectChain = { drive };
            break;
        }
        case engine::Genre::Cyberpunk:
        {
            // Delegated rather than restated: the Cyber Bass tone button and
            // the Cyberpunk genre are meant to be the same sound, and two
            // copies of ~20 hand-tuned fields would drift the first time
            // either was adjusted. Only the display name differs, since a
            // generated loop's preset is named after its genre.
            p = presetForSynthTone(engine::SynthTone::CyberBass);
            p.name = engine::genreName(genre);
            break;
        }
    }

    return p;
}

} // namespace looper::model
