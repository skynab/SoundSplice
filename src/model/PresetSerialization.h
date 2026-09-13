#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include "model/SynthPreset.h"

namespace looper::model
{
/**
    A small, line-based text format for one SynthPreset — deliberately its
    own format rather than a fragment carved out of Serialization.h's whole-
    project one.

    The two formats currently agree field-for-field (SYNTH and FXSLOT below
    mirror the project format's records exactly), but they serve different
    documents with different lifetimes: a project file's format is versioned
    against the whole document and every field ever added to any part of it;
    a preset is just a synth and its chain, shared or reused independently
    of any project. Sharing the writer would mean either format's next
    change has to consider the other's constraints. JUCE-free for the same
    reason Serialization.h is: headless round-trip tests.
*/
inline constexpr int kPresetFormatVersion = 4; // v2: SYNTH gains the filter-envelope,
                                                // sub-oscillator, and unison fields
                                                // (see Serialization.h's matching v26)
                                                // v3: FXSLOT gains the gate pedal
                                                // (see Serialization.h's matching v27)
                                                // v4: FXSLOT gains drive asymmetry
                                                // and oversampling, plus the EQ
                                                // pedal, appended at the end of
                                                // the line
                                                // (see Serialization.h's matching v30)

namespace preset_detail
{
    inline std::string num(double v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
    }

    inline std::string trimLeadingSpace(std::string s)
    {
        if (! s.empty() && s.front() == ' ')
            s.erase(0, 1);
        return s;
    }
}

inline std::string serializePreset(const SynthPreset& preset)
{
    using namespace preset_detail;
    std::ostringstream out;

    out << "LOOPERPRESET " << kPresetFormatVersion << "\n";
    out << "NAME " << preset.name << "\n"; // rest-of-line: a name may contain spaces

    const auto& synth = preset.synth;
    out << "SYNTH " << synth.waveform << " "
        << num((double) synth.attackMs) << " "
        << num((double) synth.decayMs) << " "
        << num((double) synth.sustain) << " "
        << num((double) synth.releaseMs) << " "
        << (synth.filterEnabled ? 1 : 0) << " "
        << synth.filterMode << " "
        << num((double) synth.filterCutoff) << " "
        << num((double) synth.filterResonance) << " "
        << num((double) synth.gainDb) << " "
        << num((double) synth.filterEnvAmount) << " "
        << num((double) synth.filterEnvAttackMs) << " "
        << num((double) synth.filterEnvDecayMs) << " "
        << num((double) synth.filterEnvSustain) << " "
        << num((double) synth.filterEnvReleaseMs) << " "
        << (synth.subOscEnabled ? 1 : 0) << " "
        << num((double) synth.subOscLevel) << " "
        << synth.unisonVoices << " "
        << num((double) synth.unisonDetuneCents) << "\n";

    out << "FXCHAIN " << preset.effectChain.size() << "\n";
    for (const auto& slot : preset.effectChain)
    {
        out << "FXSLOT " << (int) slot.kind << " " << (slot.enabled ? 1 : 0) << " "
            << slot.filter.mode << " "
            << num((double) slot.filter.cutoff) << " "
            << num((double) slot.filter.resonance) << " "
            << num((double) slot.delay.timeMs) << " "
            << num((double) slot.delay.feedback) << " "
            << num((double) slot.delay.mix) << " "
            << num((double) slot.reverb.roomSize) << " "
            << num((double) slot.reverb.damping) << " "
            << num((double) slot.reverb.mix) << " "
            << num((double) slot.drive.drive) << " "
            << num((double) slot.drive.tone) << " "
            << num((double) slot.drive.level) << " "
            << (slot.drive.hardClip ? 1 : 0) << " "
            << (slot.drive.cabinet ? 1 : 0) << " "
            << num((double) slot.compressor.thresholdDb) << " "
            << num((double) slot.compressor.ratio) << " "
            << num((double) slot.compressor.attackMs) << " "
            << num((double) slot.compressor.releaseMs) << " "
            << num((double) slot.compressor.makeUpDb) << " "
            << num((double) slot.tremolo.rateHz) << " "
            << num((double) slot.tremolo.depth) << " "
            << num((double) slot.chorus.rateHz) << " "
            << num((double) slot.chorus.depth) << " "
            << num((double) slot.chorus.mix) << " "
            << num((double) slot.wobble.rateBeats) << " "
            << num((double) slot.wobble.depth) << " "
            << num((double) slot.wobble.baseCutoffHz) << " "
            << num((double) slot.wobble.resonance) << " "
            << num((double) slot.wobble.mix) << " "
            << num((double) slot.gate.thresholdDb) << " "
            << num((double) slot.gate.rangeDb) << " "
            << num((double) slot.gate.attackMs) << " "
            << num((double) slot.gate.holdMs) << " "
            << num((double) slot.gate.releaseMs) << " "
            // Appended, not placed with the other drive fields: the line is
            // positional, so a mid-line insert would make every older preset
            // read its values into the wrong slots.
            << num((double) slot.drive.asymmetry) << " "
            << (slot.drive.oversample ? 1 : 0) << " "
            << num((double) slot.eqPedal.lowShelfHz) << " "
            << num((double) slot.eqPedal.lowShelfDb) << " "
            << num((double) slot.eqPedal.midHz) << " "
            << num((double) slot.eqPedal.midDb) << " "
            << num((double) slot.eqPedal.midQ) << " "
            << num((double) slot.eqPedal.highShelfHz) << " "
            << num((double) slot.eqPedal.highShelfDb) << "\n";

        if (slot.kind == EffectKind::Plugin)
        {
            out << "FXPLUGFMT " << (int) slot.plugin.format << "\n";
            out << "FXPLUGID " << slot.plugin.identifier << "\n";
            out << "FXPLUGNAME " << slot.plugin.name << "\n";
            out << "FXPLUGSTATE " << slot.plugin.state << "\n";
        }
    }

    return out.str();
}

/** Tolerant the same way Serialization.h's deserialize is: a preset written
    by an older build simply leaves fields it didn't have at their struct
    defaults. Returns false (and, if given, an @p error) on anything that
    isn't at least a well-formed preset of a version this build understands. */
inline bool deserializePreset(const std::string& text, SynthPreset& result, std::string* error = nullptr)
{
    using namespace preset_detail;

    auto fail = [&](const char* msg)
    {
        if (error != nullptr) *error = msg;
        return false;
    };

    std::istringstream lines(text);
    std::string line;

    if (! std::getline(lines, line))
        return fail("empty preset");

    std::istringstream header(line);
    std::string tag;
    int version = 0;
    header >> tag >> version;
    if (tag != "LOOPERPRESET")
        return fail("not a Looper-Audio preset");
    if (version > kPresetFormatVersion)
        return fail("preset was saved by a newer version of the app");

    auto readTagged = [&](const char* expectedTag, std::string& rest) -> bool
    {
        std::string nextLine;
        if (! std::getline(lines, nextLine))
            return false;
        const auto space = nextLine.find(' ');
        const auto tagPart = space == std::string::npos ? nextLine : nextLine.substr(0, space);
        if (tagPart != expectedTag)
            return false;
        rest = space == std::string::npos ? std::string {} : trimLeadingSpace(nextLine.substr(space));
        return true;
    };

    SynthPreset preset;

    std::string rest;
    if (! readTagged("NAME", rest))
        return fail("truncated preset (no name)");
    preset.name = rest;

    if (! readTagged("SYNTH", rest))
        return fail("truncated preset (no synth settings)");
    {
        std::istringstream ss(rest);
        int    waveform = 0, filterEnabled = 0, filterMode = 0;
        double attackMs = 5.0, decayMs = 120.0, sustain = 0.7, releaseMs = 250.0;
        double filterCutoff = 1000.0, filterResonance = 0.707, gainDb = 0.0;
        // A preset saved before v2 has no tokens for these; pre-set to
        // model::SynthSettings' real defaults so the tolerant >> chain
        // leaves them there rather than at 0.
        double filterEnvAmount = 0.0, filterEnvAttackMs = 0.0, filterEnvDecayMs = 0.0;
        double filterEnvSustain = 1.0, filterEnvReleaseMs = 0.0;
        int    subOscEnabled = 0;
        double subOscLevel = 0.3;
        int    unisonVoices = 1;
        double unisonDetuneCents = 12.0;

        ss >> waveform >> attackMs >> decayMs >> sustain >> releaseMs
           >> filterEnabled >> filterMode >> filterCutoff >> filterResonance >> gainDb
           >> filterEnvAmount >> filterEnvAttackMs >> filterEnvDecayMs >> filterEnvSustain >> filterEnvReleaseMs
           >> subOscEnabled >> subOscLevel >> unisonVoices >> unisonDetuneCents;

        preset.synth.waveform        = waveform;
        preset.synth.attackMs        = (float) attackMs;
        preset.synth.decayMs         = (float) decayMs;
        preset.synth.sustain         = (float) sustain;
        preset.synth.releaseMs       = (float) releaseMs;
        preset.synth.filterEnabled   = filterEnabled != 0;
        preset.synth.filterMode      = filterMode;
        preset.synth.filterCutoff    = (float) filterCutoff;
        preset.synth.filterResonance = (float) filterResonance;
        preset.synth.gainDb          = (float) gainDb;
        preset.synth.filterEnvAmount    = (float) filterEnvAmount;
        preset.synth.filterEnvAttackMs  = (float) filterEnvAttackMs;
        preset.synth.filterEnvDecayMs   = (float) filterEnvDecayMs;
        preset.synth.filterEnvSustain   = (float) filterEnvSustain;
        preset.synth.filterEnvReleaseMs = (float) filterEnvReleaseMs;
        preset.synth.subOscEnabled      = subOscEnabled != 0;
        preset.synth.subOscLevel        = (float) subOscLevel;
        preset.synth.unisonVoices       = unisonVoices;
        preset.synth.unisonDetuneCents  = (float) unisonDetuneCents;
    }

    if (! readTagged("FXCHAIN", rest))
        return fail("truncated preset (no effect chain)");
    const int slotCount = std::atoi(rest.c_str());

    for (int s = 0; s < slotCount; ++s)
    {
        if (! readTagged("FXSLOT", rest))
            return fail("truncated preset (effect chain cut short)");

        std::istringstream ss(rest);
        int    kind = 0, enabled = 0, filterMode = 0;
        double cutoff = 0.0, resonance = 0.0;
        double delayTime = 0.0, delayFeedback = 0.0, delayMix = 0.0;
        double room = 0.0, damping = 0.0, reverbMix = 0.0;
        double driveAmount = 4.0, driveTone = 0.5, driveLevel = 0.7;
        int    driveHard = 0, driveCab = 1;
        double compThreshold = -18.0, compRatio = 4.0, compAttack = 10.0;
        double compRelease = 120.0, compMakeUp = 0.0;
        double tremRate = 5.0, tremDepth = 0.5;
        double chorusRate = 0.6, chorusDepth = 0.5, chorusMix = 0.5;
        double wobbleRateBeats = 0.25, wobbleDepth = 0.7, wobbleBaseCutoffHz = 200.0;
        double wobbleResonance = 0.9, wobbleMix = 1.0;
        double gateThreshold = -40.0, gateRange = 60.0, gateAttack = 2.0;
        double gateHold = 20.0, gateRelease = 150.0;
        double driveAsymmetry = 0.0;
        int    driveOversample = 0;
        double eqLowHz = 100.0, eqLowDb = 0.0;
        double eqMidHz = 800.0, eqMidDb = 0.0, eqMidQ = 1.0;
        double eqHighHz = 4000.0, eqHighDb = 0.0;

        ss >> kind >> enabled >> filterMode >> cutoff >> resonance
           >> delayTime >> delayFeedback >> delayMix >> room >> damping >> reverbMix
           >> driveAmount >> driveTone >> driveLevel >> driveHard >> driveCab
           >> compThreshold >> compRatio >> compAttack >> compRelease >> compMakeUp
           >> tremRate >> tremDepth
           >> chorusRate >> chorusDepth >> chorusMix
           >> wobbleRateBeats >> wobbleDepth >> wobbleBaseCutoffHz >> wobbleResonance >> wobbleMix
           >> gateThreshold >> gateRange >> gateAttack >> gateHold >> gateRelease
           >> driveAsymmetry >> driveOversample
           >> eqLowHz >> eqLowDb >> eqMidHz >> eqMidDb >> eqMidQ >> eqHighHz >> eqHighDb;

        EffectSlot slot;
        slot.kind              = (EffectKind) kind;
        slot.enabled           = enabled != 0;
        slot.filter.enabled    = slot.enabled && slot.kind == EffectKind::Filter;
        slot.filter.mode       = filterMode;
        slot.filter.cutoff     = (float) cutoff;
        slot.filter.resonance  = (float) resonance;
        slot.delay.enabled     = slot.enabled && slot.kind == EffectKind::Delay;
        slot.delay.timeMs      = (float) delayTime;
        slot.delay.feedback    = (float) delayFeedback;
        slot.delay.mix         = (float) delayMix;
        slot.reverb.enabled    = slot.enabled && slot.kind == EffectKind::Reverb;
        slot.reverb.roomSize   = (float) room;
        slot.reverb.damping    = (float) damping;
        slot.reverb.mix        = (float) reverbMix;
        slot.drive.enabled     = slot.enabled && slot.kind == EffectKind::Drive;
        slot.drive.drive       = (float) driveAmount;
        slot.drive.tone        = (float) driveTone;
        slot.drive.level       = (float) driveLevel;
        slot.drive.hardClip    = driveHard != 0;
        slot.drive.cabinet     = driveCab != 0;
        slot.drive.asymmetry   = (float) driveAsymmetry;
        slot.drive.oversample  = driveOversample != 0;
        slot.compressor.enabled     = slot.enabled && slot.kind == EffectKind::Compressor;
        slot.compressor.thresholdDb = (float) compThreshold;
        slot.compressor.ratio       = (float) compRatio;
        slot.compressor.attackMs    = (float) compAttack;
        slot.compressor.releaseMs   = (float) compRelease;
        slot.compressor.makeUpDb    = (float) compMakeUp;
        slot.tremolo.enabled        = slot.enabled && slot.kind == EffectKind::Tremolo;
        slot.tremolo.rateHz         = (float) tremRate;
        slot.tremolo.depth          = (float) tremDepth;
        slot.chorus.enabled         = slot.enabled && slot.kind == EffectKind::Chorus;
        slot.chorus.rateHz          = (float) chorusRate;
        slot.chorus.depth           = (float) chorusDepth;
        slot.chorus.mix             = (float) chorusMix;
        slot.wobble.enabled         = slot.enabled && slot.kind == EffectKind::Wobble;
        slot.wobble.rateBeats       = (float) wobbleRateBeats;
        slot.wobble.depth           = (float) wobbleDepth;
        slot.wobble.baseCutoffHz    = (float) wobbleBaseCutoffHz;
        slot.wobble.resonance       = (float) wobbleResonance;
        slot.wobble.mix             = (float) wobbleMix;
        slot.eqPedal.enabled     = slot.enabled && slot.kind == EffectKind::Eq;
        slot.eqPedal.lowShelfHz  = (float) eqLowHz;
        slot.eqPedal.lowShelfDb  = (float) eqLowDb;
        slot.eqPedal.midHz       = (float) eqMidHz;
        slot.eqPedal.midDb       = (float) eqMidDb;
        slot.eqPedal.midQ        = (float) eqMidQ;
        slot.eqPedal.highShelfHz = (float) eqHighHz;
        slot.eqPedal.highShelfDb = (float) eqHighDb;
        slot.gate.enabled           = slot.enabled && slot.kind == EffectKind::Gate;
        slot.gate.thresholdDb       = (float) gateThreshold;
        slot.gate.rangeDb           = (float) gateRange;
        slot.gate.attackMs          = (float) gateAttack;
        slot.gate.holdMs            = (float) gateHold;
        slot.gate.releaseMs         = (float) gateRelease;

        if (slot.kind == EffectKind::Plugin)
        {
            if (! readTagged("FXPLUGFMT", rest))   return fail("truncated plugin slot");
            slot.plugin.format = (PluginFormat) std::atoi(rest.c_str());
            if (! readTagged("FXPLUGID", rest))    return fail("truncated plugin slot");
            slot.plugin.identifier = rest;
            if (! readTagged("FXPLUGNAME", rest))  return fail("truncated plugin slot");
            slot.plugin.name = rest;
            if (! readTagged("FXPLUGSTATE", rest)) return fail("truncated plugin slot");
            slot.plugin.state = rest;
        }

        preset.effectChain.push_back(std::move(slot));
    }

    result = std::move(preset);
    return true;
}

} // namespace looper::model
