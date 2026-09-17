#pragma once

#include <memory>

#include "engine/EffectChain.h"
#include "model/Effects.h"

namespace soundsplice::engine
{
/**
    Turns a document effect slot into the engine node and parameter set that
    realise it.

    Shared rather than duplicated because it had already been duplicated three
    times - MainComponent, AudioEngine's rebuild switch, and the bounce tool -
    and the bounce tool's copy drifted. A verification tool that builds the
    chain differently from the app verifies the wrong thing, and it does so
    silently: everything still runs, the numbers just describe a signal path
    nobody hears.

    The two functions stay free rather than becoming members of EffectChain
    because they map *model* types, and EffectChain is deliberately reachable
    from code that has no model at all.
*/
/** One engine node for a built-in effect kind, or nullptr for a kind that
    can't be built here (Plugin, which needs the plugin host). Mirrors
    AudioEngine::rebuildTrackEffectChain's switch. */
inline std::unique_ptr<EffectProcessor> makeEffectNode(model::EffectKind kind)
{
    switch (kind)
    {
        case model::EffectKind::Filter:     return std::make_unique<FilterNode>();
        case model::EffectKind::Delay:      return std::make_unique<DelayNode>();
        case model::EffectKind::Reverb:     return std::make_unique<ReverbNode>();
        case model::EffectKind::Drive:      return std::make_unique<DriveNode>();
        case model::EffectKind::Compressor: return std::make_unique<CompressorNode>();
        case model::EffectKind::Tremolo:    return std::make_unique<TremoloNode>();
        case model::EffectKind::Chorus:     return std::make_unique<ChorusNode>();
        case model::EffectKind::Wobble:     return std::make_unique<WobbleNode>();
        case model::EffectKind::Gate:       return std::make_unique<GateNode>();
        case model::EffectKind::Eq:         return std::make_unique<EqNode>();
        case model::EffectKind::Amplify:    return std::make_unique<AmplifyNode>();
        case model::EffectKind::Invert:     return std::make_unique<InvertNode>();
        case model::EffectKind::DcOffset:   return std::make_unique<DcOffsetNode>();
        case model::EffectKind::Limiter:    return std::make_unique<LimiterNode>();
        case model::EffectKind::Phaser:     return std::make_unique<PhaserNode>();
        case model::EffectKind::Flanger:    return std::make_unique<FlangerNode>();
        case model::EffectKind::BassTreble: return std::make_unique<BassTrebleNode>();
        case model::EffectKind::StereoTool: return std::make_unique<StereoToolNode>();
        case model::EffectKind::GraphicEq:  return std::make_unique<GraphicEqNode>();
        case model::EffectKind::DeEsser:    return std::make_unique<DeEsserNode>();
        case model::EffectKind::Expander:   return std::make_unique<ExpanderNode>();
        case model::EffectKind::RingMod:    return std::make_unique<RingModNode>();
        case model::EffectKind::Wah:        return std::make_unique<WahNode>();
        case model::EffectKind::Echo:       return std::make_unique<EchoNode>();
        case model::EffectKind::Plugin:     return nullptr;
    }
    return nullptr;
}

inline EffectSlotParams toSlotParams(const model::EffectSlot& slot)
{
    EffectSlotParams params;
    params.enabled         = slot.enabled;
    params.filterMode      = slot.filter.mode;
    params.filterCutoff    = slot.filter.cutoff;
    params.filterResonance = slot.filter.resonance;
    params.delayTimeMs     = slot.delay.timeMs;
    params.delayFeedback   = slot.delay.feedback;
    params.delayMix        = slot.delay.mix;
    params.reverbRoomSize  = slot.reverb.roomSize;
    params.reverbDamping   = slot.reverb.damping;
    params.reverbMix       = slot.reverb.mix;
    params.driveAmount     = slot.drive.drive;
    params.driveTone       = slot.drive.tone;
    params.driveLevel      = slot.drive.level;
    params.driveHardClip   = slot.drive.hardClip;
    params.driveCabinet    = slot.drive.cabinet;
    params.driveAsymmetry  = slot.drive.asymmetry;
    params.driveOversample = slot.drive.oversample;
    params.driveStages     = slot.drive.stages;
    params.driveCabinetIr  = slot.drive.cabinetIr;
    params.eqLowShelfHz    = slot.eqPedal.lowShelfHz;
    params.eqLowShelfDb    = slot.eqPedal.lowShelfDb;
    params.eqMidHz         = slot.eqPedal.midHz;
    params.eqMidDb         = slot.eqPedal.midDb;
    params.eqMidQ          = slot.eqPedal.midQ;
    params.eqHighShelfHz   = slot.eqPedal.highShelfHz;
    params.eqHighShelfDb   = slot.eqPedal.highShelfDb;
    params.compThresholdDb = slot.compressor.thresholdDb;
    params.compRatio       = slot.compressor.ratio;
    params.compAttackMs    = slot.compressor.attackMs;
    params.compReleaseMs   = slot.compressor.releaseMs;
    params.compMakeUpDb    = slot.compressor.makeUpDb;
    params.tremoloRateHz   = slot.tremolo.rateHz;
    params.tremoloDepth    = slot.tremolo.depth;
    params.chorusRateHz    = slot.chorus.rateHz;
    params.chorusDepth     = slot.chorus.depth;
    params.chorusMix       = slot.chorus.mix;
    params.wobbleRateBeats    = slot.wobble.rateBeats;
    params.wobbleDepth        = slot.wobble.depth;
    params.wobbleBaseCutoffHz = slot.wobble.baseCutoffHz;
    params.wobbleResonance    = slot.wobble.resonance;
    params.wobbleMix          = slot.wobble.mix;
    params.gateThresholdDb = slot.gate.thresholdDb;
    params.gateRangeDb     = slot.gate.rangeDb;
    params.gateAttackMs    = slot.gate.attackMs;
    params.gateHoldMs      = slot.gate.holdMs;
    params.gateReleaseMs   = slot.gate.releaseMs;
    params.amplifyGainDb    = slot.amplify.gainDb;
    params.invertLeft       = slot.invert.left;
    params.invertRight      = slot.invert.right;
    params.dcCutoffHz       = slot.dcOffset.cutoffHz;
    params.limiterInputDb   = slot.limiter.inputGainDb;
    params.limiterCeilingDb = slot.limiter.ceilingDb;
    params.limiterReleaseMs = slot.limiter.releaseMs;
    params.phaserRateHz     = slot.phaser.rateHz;
    params.phaserDepth      = slot.phaser.depth;
    params.phaserFeedback   = slot.phaser.feedback;
    params.phaserStagePairs = slot.phaser.stagePairs;
    params.phaserMix        = slot.phaser.mix;
    params.flangerRateHz    = slot.flanger.rateHz;
    params.flangerDepth     = slot.flanger.depth;
    params.flangerDelayMs   = slot.flanger.delayMs;
    params.flangerFeedback  = slot.flanger.feedback;
    params.flangerMix       = slot.flanger.mix;
    params.bassDb           = slot.bassTreble.bassDb;
    params.trebleDb         = slot.bassTreble.trebleDb;
    params.toneVolumeDb     = slot.bassTreble.volumeDb;
    params.stereoWidth      = slot.stereoTool.width;
    params.stereoBalance    = slot.stereoTool.balance;
    params.stereoMono       = slot.stereoTool.mono;
    params.stereoSwap       = slot.stereoTool.swap;
    params.graphicEqDb[0]   = slot.graphicEq.band31;
    params.graphicEqDb[1]   = slot.graphicEq.band62;
    params.graphicEqDb[2]   = slot.graphicEq.band125;
    params.graphicEqDb[3]   = slot.graphicEq.band250;
    params.graphicEqDb[4]   = slot.graphicEq.band500;
    params.graphicEqDb[5]   = slot.graphicEq.band1k;
    params.graphicEqDb[6]   = slot.graphicEq.band2k;
    params.graphicEqDb[7]   = slot.graphicEq.band4k;
    params.graphicEqDb[8]   = slot.graphicEq.band8k;
    params.graphicEqDb[9]   = slot.graphicEq.band16k;
    params.deEssFrequencyHz = slot.deEsser.frequencyHz;
    params.deEssThresholdDb = slot.deEsser.thresholdDb;
    params.deEssReductionDb = slot.deEsser.maxReductionDb;
    params.expThresholdDb   = slot.expander.thresholdDb;
    params.expRatio         = slot.expander.ratio;
    params.expRangeDb       = slot.expander.rangeDb;
    params.expAttackMs      = slot.expander.attackMs;
    params.expReleaseMs     = slot.expander.releaseMs;
    params.ringFrequencyHz  = slot.ringMod.frequencyHz;
    params.ringMix          = slot.ringMod.mix;
    params.wahRateHz        = slot.wah.rateHz;
    params.wahDepth         = slot.wah.depth;
    params.wahResonance     = slot.wah.resonance;
    params.wahMix           = slot.wah.mix;
    params.echoTimeMs       = slot.echo.timeMs;
    params.echoTaps         = slot.echo.taps;
    params.echoDecay        = slot.echo.decay;
    params.echoMix          = slot.echo.mix;
    params.echoPingPong     = slot.echo.pingPong;
    return params;
}

/** The node for @p slot, already configured - which is what every caller
    actually wants. Returns null for a Plugin slot, which needs a host. */
inline std::unique_ptr<EffectProcessor> makeConfiguredNode(const model::EffectSlot& slot)
{
    auto node = makeEffectNode(slot.kind);
    if (node != nullptr)
        applyParams(*node, toSlotParams(slot));
    return node;
}

} // namespace soundsplice::engine
