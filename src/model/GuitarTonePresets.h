#pragma once

#include <vector>

#include "engine/GuitarTone.h"
#include "model/Effects.h"
#include "model/GuitarSettings.h"

namespace looper::model
{
/** A GuitarSettings/effect-chain bundle for one engine::GuitarTone. */
struct GuitarTonePreset
{
    GuitarSettings           guitar;
    std::vector<EffectSlot>  effectChain;
};

/**
    A guitar sound purpose-tuned for @p tone, for FretboardPane's one-click
    tone buttons — built the same in-memory, literal-struct-per-field way
    presetForGenre() builds a genre's SynthPreset (see GenrePresets.h): no
    file I/O, no save/load, just known-good values applied in one undo step.
*/
inline GuitarTonePreset presetForGuitarTone(engine::GuitarTone tone)
{
    GuitarTonePreset p;

    switch (tone)
    {
        case engine::GuitarTone::ModernMetal:
        {
            // Drop C (C2 G2 C3 F3 A3 D4). The growl in this genre is mostly
            // the tuning: dropped low strings put the fundamentals under the
            // cabinet's 90Hz corner, so what's actually heard is the *low-mid
            // harmonics* the distortion generates from them, which is the
            // sound. Standard E through the same chain reads as "distorted
            // rock guitar", not as this.
            p.guitar.tuning = { 36, 43, 48, 53, 57, 62 };

            p.guitar.decaySeconds  = 2.0f;
            // A humbucker is dark *before* its resonance, not overall - the
            // peak at 2-3kHz is the whole reason an electric guitar cuts.
            // These were previously dialled much darker (brightness 0.38) to
            // maximise looper_bounce's old `growl` figure, which measured
            // 90-600Hz over 2-6kHz and so rewarded exactly the wrong thing:
            // it treated the presence band a guitar lives in as a defect.
            // With that metric retired and a cabinet that actually removes
            // fizz, the string can be as bright as a real one.
            p.guitar.brightness    = 0.62f;
            p.guitar.pickPosition  = 0.24f;
            p.guitar.pickHardness  = 0.85f;

            // A hot ceramic humbucker: peak a little higher and sharper than
            // a vintage PAF, which is what makes this style of tone cut
            // rather than just sit there being loud.
            p.guitar.pickupResonanceHz = 2800.0f;
            p.guitar.pickupQ           = 2.0f;
            // Was 0.55: a blunt stand-in for chugging that choked *every* note
            // on release, back when there was no way to mute one note and let
            // the next ring. With a real per-note articulation doing that work
            // the strings can behave like a guitar's and ring until replucked.
            p.guitar.muteOnNoteOff = 0.0f;

            // Tight and dark - a drop-tuned chug is short and has almost no
            // top, which is what makes the open accents around it cut.
            p.guitar.palmMuteDecaySeconds = 0.14f;
            p.guitar.palmMuteBrightness   = 0.18f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -20.0f;
            compressor.compressor.ratio       = 4.0f;
            compressor.compressor.attackMs    = 8.0f;
            compressor.compressor.releaseMs   = 100.0f;
            compressor.compressor.makeUpDb    = 6.0f;

            // Two cascaded gain stages rather than one hard clipper, which is
            // both the real rig (a mid-focused boost pedal into a high-gain
            // amp) and the reason this growls instead of buzzing: a single
            // hard clip at high drive is a square wave, heard as fizz, while
            // stacked soft (tanh) stages saturate progressively and stay
            // harmonically dense in the low-mids.
            //
            // Cabinet is off on the boost and on for the amp, matching the
            // same rig - a boost pedal has no speaker, and cascading two
            // CabinetSims would put four poles of 4.5kHz lowpass in the path
            // and just sound muffled.
            EffectSlot boost;
            boost.kind           = EffectKind::Drive;
            boost.enabled        = true;
            boost.drive.enabled  = true;
            boost.drive.drive    = 10.0f;
            boost.drive.tone     = 0.5f;
            boost.drive.level    = 1.6f;
            boost.drive.hardClip = false;
            boost.drive.cabinet  = false;
            // A boost pedal's job is to be the *first* stage, so its
            // asymmetry is what seeds the even harmonics everything after it
            // then multiplies. Oversampled because this feeds a second
            // clipper with no filtering between them - the one case where the
            // ADAA shaper alone is not enough.
            boost.drive.asymmetry  = 0.30f;
            boost.drive.oversample = true;

            EffectSlot amp;
            amp.kind           = EffectKind::Drive;
            amp.enabled        = true;
            amp.drive.enabled  = true;
            amp.drive.drive    = 22.0f;
            // Was 0.26, pulled down to satisfy the old `growl` figure. The
            // cabinet now has a real 36dB/octave cliff above 5kHz and the
            // boost is a genuine bandpass, so the fizz that tone knob was
            // hiding is no longer generated - and turning it back up buys
            // presence instead of hiss.
            amp.drive.tone     = 0.42f;
            // Pulled down from 2.0 when the amp became a three-stage
            // cascade: make-up gain falls as 1/sqrt(drive), so lowering the
            // drive to suit the cascade *raised* the output and the preset
            // started clipping (the bounce tool caught it as wetPeak > 1).
            amp.drive.level    = 1.05f;
            amp.drive.hardClip = false;
            amp.drive.cabinet  = true;
            // Three gain stages rather than one clipper — the amp half of the
            // chain is where an amp's compression and density should come
            // from, and the interstage bass rolloff is what keeps a drop-tuned
            // low string tight instead of turning the chord to mush. The boost
            // in front stays a single stage, because a boost pedal *is* one.
            amp.drive.stages   = 3;
            // ...and a convolved cabinet rather than a filtered one: the
            // reflections and cone breakup are what make a mic'd 4x12 sound
            // like a box with a microphone in front of it rather than a
            // filter, and this is the tone that most depends on it.
            amp.drive.cabinetIr = true;
            amp.drive.asymmetry  = 0.15f; // less than the boost: this stage is already deep in clip
            amp.drive.oversample = true;

            EffectSlot gate;
            gate.kind             = EffectKind::Gate;
            gate.enabled          = true;
            gate.gate.enabled     = true;
            gate.gate.thresholdDb = -32.0f;
            gate.gate.rangeDb     = 60.0f;
            gate.gate.attackMs    = 0.5f;
            gate.gate.holdMs      = 15.0f;
            gate.gate.releaseMs   = 55.0f;

            // After the amp, because this is the mix decision rather than
            // part of the distortion: a scoop before a clipper changes what
            // gets distorted, and here the intent is to shape what came out.
            // The mid axis had no per-track control at all before this pedal
            // existed, so this preset simply could not express it.
            EffectSlot eq;
            eq.kind                 = EffectKind::Eq;
            eq.enabled              = true;
            eq.eqPedal.enabled      = true;
            eq.eqPedal.lowShelfHz   = 120.0f;
            eq.eqPedal.lowShelfDb   = -1.0f;  // tighten the chug slightly rather than let it bloom
            eq.eqPedal.midHz        = 1100.0f;
            eq.eqPedal.midDb        = 3.5f;   // the band that makes a riff audible in a mix
            eq.eqPedal.midQ         = 0.9f;   // broad - a push, not a resonance
            eq.eqPedal.highShelfHz  = 5000.0f;
            eq.eqPedal.highShelfDb  = 1.5f;

            p.effectChain = { compressor, boost, amp, eq, gate };
            break;
        }
        case engine::GuitarTone::CleanJazz:
        {
            // Warm, long-ringing, no drive - articulate rather than loud.
            p.guitar.decaySeconds  = 3.5f;
            p.guitar.brightness    = 0.35f;
            p.guitar.pickPosition  = 0.35f; // toward the neck - round, warm attack
            p.guitar.pickHardness  = 0.25f; // fingertip-soft
            p.guitar.muteOnNoteOff = 0.0f;  // let it ring

            // A neck humbucker with the tone rolled off: the resonance sits
            // low and gently, which is the whole archetype of this sound.
            p.guitar.pickupResonanceHz = 1800.0f;
            p.guitar.pickupQ           = 0.9f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -24.0f;
            compressor.compressor.ratio       = 2.5f;
            compressor.compressor.attackMs    = 15.0f;
            compressor.compressor.releaseMs   = 150.0f;
            compressor.compressor.makeUpDb    = 2.0f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.3f;
            reverb.reverb.damping  = 0.6f;
            reverb.reverb.mix      = 0.18f;

            p.effectChain = { compressor, reverb };
            break;
        }
        case engine::GuitarTone::ClassicRockCrunch:
        {
            // Warm tube-like overdrive, not a wall of gain - dynamic enough
            // that pick attack still comes through.
            p.guitar.decaySeconds  = 2.6f;
            p.guitar.brightness    = 0.55f;
            p.guitar.pickPosition  = 0.22f;

            // A vintage-output humbucker: lower and broader than Modern
            // Metal's ceramic, which is most of the difference between
            // "crunch" and "high gain" before the amp is even involved.
            p.guitar.pickupResonanceHz = 2400.0f;
            p.guitar.pickupQ           = 1.5f;
            p.guitar.pickHardness  = 0.65f;
            p.guitar.muteOnNoteOff = 0.15f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -22.0f;
            compressor.compressor.ratio       = 3.0f;
            compressor.compressor.attackMs    = 10.0f;
            compressor.compressor.releaseMs   = 120.0f;
            compressor.compressor.makeUpDb    = 3.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 22.0f;
            drive.drive.tone     = 0.5f;
            drive.drive.level    = 1.0f;
            drive.drive.hardClip = false; // soft clip - the tube-y crunch
            // The even harmonics a symmetric curve cannot make. This is the
            // preset that most depends on them: "tube-y" is very largely the
            // 2nd harmonic that an asymmetric stage generates.
            drive.drive.asymmetry = 0.35f;
            drive.drive.cabinet  = true;

            p.effectChain = { compressor, drive };
            break;
        }
        case engine::GuitarTone::AmbientShoegaze:
        {
            // Long, swelling sustain under a heavy chorus/reverb wash.
            p.guitar.decaySeconds  = 6.0f;
            p.guitar.brightness    = 0.38f;
            p.guitar.pickPosition  = 0.3f;

            // Deliberately smooth: a low, broad resonance leaves nothing
            // sharp enough to cut through the wash, which is the point.
            p.guitar.pickupResonanceHz = 2000.0f;
            p.guitar.pickupQ           = 0.8f;
            p.guitar.pickHardness  = 0.4f;
            p.guitar.muteOnNoteOff = 0.0f;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.3f;
            chorus.chorus.depth   = 0.8f;
            chorus.chorus.mix     = 0.6f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.9f;
            reverb.reverb.damping  = 0.3f;
            reverb.reverb.mix      = 0.5f;

            p.effectChain = { chorus, reverb };
            break;
        }
        case engine::GuitarTone::FunkPercussive:
        {
            // Tight, choked, minimal sustain - the chord is a percussion hit.
            p.guitar.decaySeconds  = 0.5f;
            p.guitar.brightness    = 0.65f;
            p.guitar.pickPosition  = 0.18f;
            p.guitar.pickHardness  = 0.7f;
            p.guitar.muteOnNoteOff = 0.85f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -18.0f;
            compressor.compressor.ratio       = 6.0f;
            compressor.compressor.attackMs    = 2.0f;
            compressor.compressor.releaseMs   = 60.0f;
            compressor.compressor.makeUpDb    = 4.0f;

            p.effectChain = { compressor };
            break;
        }
        case engine::GuitarTone::IndustrialCyber:
        {
            // Cold and mechanical rather than heavy: drop C like the metal
            // tone, but where that one is tuned for low-mid growl this is
            // deliberately thinner and more brittle - a hard clip (the fizz
            // Modern Metal avoids is the point here), a bright tone, and a
            // gate set to slam shut so notes end abruptly rather than
            // decaying. Chorus and a short delay do the "cyber" shimmer.
            p.guitar.tuning = { 36, 43, 48, 53, 57, 62 };

            p.guitar.decaySeconds  = 1.2f;  // short - this tone doesn't sustain
            p.guitar.brightness    = 0.8f;  // brittle and present, not warm
            p.guitar.pickPosition  = 0.1f;  // hard at the bridge - thin and glassy
            p.guitar.pickHardness  = 0.95f;
            p.guitar.muteOnNoteOff = 0.9f;  // notes stop dead, machine-like

            // Shorter and less dark than Modern Metal's: this tone is brittle
            // rather than heavy, so a mute here is a click more than a thud.
            p.guitar.palmMuteDecaySeconds = 0.09f;
            p.guitar.palmMuteBrightness   = 0.35f;

            // Single-coil territory: the resonance is high and sharp, which
            // is what makes this read as brittle and glassy rather than
            // merely bright.
            p.guitar.pickupResonanceHz = 5200.0f;
            p.guitar.pickupQ           = 2.6f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 30.0f;
            drive.drive.tone     = 0.8f;  // brighter than Modern Metal's 0.42
            drive.drive.level    = 1.2f;
            drive.drive.hardClip = true;  // square-ish and buzzy on purpose
            drive.drive.cabinet  = true;
            // Hard clipping at drive 30 generates harmonics well past Nyquist
            // in a single stage, and unlike the buzz - which is wanted here -
            // the folded-back aliases are inharmonic and just sound broken.
            drive.drive.oversample = true;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.8f;
            chorus.chorus.depth   = 0.5f;
            chorus.chorus.mix     = 0.3f;

            EffectSlot delay;
            delay.kind           = EffectKind::Delay;
            delay.enabled        = true;
            delay.delay.enabled  = true;
            delay.delay.timeMs   = 250.0f;
            delay.delay.feedback = 0.3f;
            delay.delay.mix      = 0.22f;

            // Tighter and deeper than the metal gate: this tone is about
            // notes stopping, so the gate is part of the sound rather than
            // just noise control.
            EffectSlot gate;
            gate.kind             = EffectKind::Gate;
            gate.enabled          = true;
            gate.gate.enabled     = true;
            gate.gate.thresholdDb = -26.0f;
            gate.gate.rangeDb     = 70.0f;
            gate.gate.attackMs    = 0.3f;
            gate.gate.holdMs      = 8.0f;
            gate.gate.releaseMs   = 25.0f;

            p.effectChain = { drive, gate, chorus, delay };
            break;
        }
    }

    return p;
}

} // namespace looper::model
