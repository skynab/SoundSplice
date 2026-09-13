#pragma once

#include "engine/MasteringPreset.h"
#include "model/Effects.h"

namespace looper::model
{
/**
    A mastering rack setting for @p preset, built the same in-memory,
    literal-struct-per-field way presetForGuitarTone/presetForSynthTone are.

    Every preset except Flat turns the rack on, because picking one is an
    explicit request to hear it — a preset that applied its values while
    leaving the rack bypassed would look broken. Flat is the exception on
    purpose: it's how you get back to nothing.
*/
inline MasteringSettings presetForMastering(engine::MasteringPreset preset)
{
    MasteringSettings m; // defaults are already the neutral, no-op rack

    switch (preset)
    {
        case engine::MasteringPreset::Flat:
            // Left exactly at the defaults, and deliberately *not* enabled.
            break;

        case engine::MasteringPreset::Transparent:
            // Do as little as possible while still guaranteeing nothing
            // clips: no tone shaping at all, just a ceiling.
            m.enabled            = true;
            m.maximizerInputDb   = 1.0f;
            m.maximizerCeilingDb = -0.3f;
            m.maximizerReleaseMs = 120.0f;
            break;

        case engine::MasteringPreset::Loud:
            // Driven hard into the limiter. A touch of low and high lift
            // because heavy limiting flattens both ends first, so this is
            // compensating for the limiter rather than decorating.
            m.enabled            = true;
            m.lowShelfHz         = 90.0f;
            m.lowShelfDb         = 1.5f;
            m.highShelfHz        = 9000.0f;
            m.highShelfDb        = 1.5f;
            m.exciterAmount      = 0.2f;
            m.maximizerInputDb   = 8.0f;
            m.maximizerCeilingDb = -0.3f;
            m.maximizerReleaseMs = 60.0f;
            m.outputGainDb       = 0.0f;
            break;

        case engine::MasteringPreset::Warm:
            // Weight at the bottom, harshness pulled out of the upper mids,
            // top softened rather than cut.
            m.enabled            = true;
            m.lowShelfHz         = 150.0f;
            m.lowShelfDb         = 2.5f;
            m.peakHz             = 2500.0f;
            m.peakDb             = -2.0f;
            m.peakQ              = 0.8f;
            m.highShelfHz        = 10000.0f;
            m.highShelfDb        = -1.0f;
            m.reverbAmount       = 0.06f;
            m.reverbRoomSize     = 0.5f;
            m.maximizerInputDb   = 2.0f;
            m.maximizerCeilingDb = -0.5f;
            m.maximizerReleaseMs = 150.0f;
            break;

        case engine::MasteringPreset::Wide:
            // The widest setting here is still only 1.4: past that the mono
            // sum starts losing material even with the widener's mid
            // compensation, and a master that hollows out on a phone is a
            // worse outcome than one that isn't quite as wide.
            m.enabled            = true;
            m.width              = 1.4f;
            m.exciterAmount      = 0.25f;
            m.highShelfHz        = 8000.0f;
            m.highShelfDb        = 1.0f;
            m.reverbAmount       = 0.1f;
            m.reverbRoomSize     = 0.7f;
            m.maximizerInputDb   = 2.0f;
            m.maximizerCeilingDb = -0.3f;
            break;

        case engine::MasteringPreset::Voice:
            // Spoken word: cut the rumble a room puts under a voice, lift
            // presence so consonants read, and keep it narrow — a widened
            // voice sounds phasey and loses its centre.
            m.enabled            = true;
            m.lowShelfHz         = 120.0f;
            m.lowShelfDb         = -4.0f;
            m.peakHz             = 3000.0f;
            m.peakDb             = 2.5f;
            m.peakQ              = 0.7f;
            m.highShelfHz        = 9000.0f;
            m.highShelfDb        = 1.0f;
            m.width              = 1.0f;
            m.maximizerInputDb   = 4.0f;
            m.maximizerCeilingDb = -1.0f;
            m.maximizerReleaseMs = 80.0f;
            break;
    }

    return m;
}

} // namespace looper::model
