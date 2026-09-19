#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DelayEffect.h"
#include "engine/DriveEffect.h"
#include "engine/PedalEffects.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"
#include "engine/DynamicsEffects.h"
#include "engine/ParametricEqEffect.h"
#include "engine/DynamicsProcessorEffect.h"
#include "engine/ThirdOctaveEqEffect.h"
#include "engine/ConvolutionEffect.h"
#include "engine/VocoderEffect.h"
#include "engine/ChannelMixer.h"
#include "engine/ToneEffects.h"
#include "engine/UtilityEffects.h"

namespace soundsplice::engine
{
/** What one node of a chain is. Mirrors model::EffectKind, which `engine`
    can't reference: `model` already depends on `engine` (a Clip owns an
    engine::Pattern), so the dependency can't run both ways. The owner
    converts at the boundary, as it does for automation curves. */
enum class EffectNodeKind
{
    Filter = 0,
    Delay  = 1,
    Reverb = 2,
    Plugin = 3,
    Drive      = 4,
    Compressor = 5,
    Tremolo    = 6,
    Chorus     = 7,
    Wobble     = 8,
    Gate       = 9,
    Eq         = 10,
    Amplify    = 11,
    Invert     = 12,
    DcOffset   = 13,
    Limiter    = 14,
    Phaser     = 15,
    Flanger    = 16,
    BassTreble = 17,
    StereoTool = 18,
    GraphicEq  = 19,
    DeEsser    = 20,
    Expander   = 21,
    RingMod    = 22,
    Wah        = 23,
    Echo       = 24,
    Multiband  = 25,
    ParametricEq = 26,
    Dynamics     = 27,
    GraphicEq31  = 28,
    Convolution  = 29,
    Vocoder      = 30,
    ChannelMixer = 31
};

/** What a chain slot should be. Carries plugin identity as plain strings —
    the engine can't reference model::PluginRef, and this is the same boundary
    the rest of the engine keeps. */
struct EffectSlotSpec
{
    EffectNodeKind kind = EffectNodeKind::Filter;
    std::string    pluginFormat;
    std::string    pluginIdentifier;
    std::string    pluginState; // base64, applied after instantiation

    /** Only identity matters for deciding whether to rebuild — a changed
        preset is restored onto the existing instance, not a new chain. */
    bool sameShapeAs(const EffectSlotSpec& other) const
    {
        return kind == other.kind
            && pluginFormat == other.pluginFormat
            && pluginIdentifier == other.pluginIdentifier;
    }
};

/** Every built-in's parameters for one slot, pushed by index. All of them
    travel together because only the ones matching the slot's kind are read —
    the same trade model::EffectSlot makes, so switching a slot's kind doesn't
    lose the settings of the others. */
struct EffectSlotParams
{
    bool  enabled = false;

    int   filterMode      = 0;
    float filterCutoff    = 1000.0f;
    float filterResonance = 0.707f;

    float delayTimeMs   = 300.0f;
    float delayFeedback = 0.35f;
    float delayMix      = 0.3f;

    float reverbRoomSize = 0.5f;
    float reverbDamping  = 0.5f;
    float reverbMix      = 0.3f;

    float driveAmount   = 4.0f;
    float driveTone     = 0.5f;
    float driveLevel    = 0.7f;
    bool  driveHardClip = false;
    bool  driveCabinet  = true;
    float driveAsymmetry = 0.0f;
    bool  driveOversample = false;
    int   driveStages     = 1;
    bool  driveCabinetIr  = false;

    float compThresholdDb = -18.0f;
    float compRatio       = 4.0f;
    float compAttackMs    = 10.0f;
    float compReleaseMs   = 120.0f;
    float compMakeUpDb    = 0.0f;

    float tremoloRateHz = 5.0f;
    float tremoloDepth  = 0.5f;

    float chorusRateHz = 0.6f;
    float chorusDepth  = 0.5f;
    float chorusMix    = 0.5f;

    float wobbleRateBeats    = 0.25f;
    float wobbleDepth        = 0.7f;
    float wobbleBaseCutoffHz = 200.0f;
    float wobbleResonance    = 0.9f;
    float wobbleMix          = 1.0f;

    float gateThresholdDb = -40.0f;
    float gateRangeDb     = 60.0f;
    float gateAttackMs    = 2.0f;
    float gateHoldMs      = 20.0f;
    float gateReleaseMs   = 150.0f;

    float eqLowShelfHz  = 100.0f;
    float eqLowShelfDb  = 0.0f;
    float eqMidHz       = 800.0f;
    float eqMidDb       = 0.0f;
    float eqMidQ        = 1.0f;
    float eqHighShelfHz = 4000.0f;
    float eqHighShelfDb = 0.0f;

    float amplifyGainDb    = 0.0f;
    bool  invertLeft       = true;
    bool  invertRight      = true;
    float dcCutoffHz       = 5.0f;
    float limiterInputDb   = 0.0f;
    float limiterCeilingDb = -1.0f;
    float limiterReleaseMs = 100.0f;

    float phaserRateHz     = 0.5f;
    float phaserDepth      = 0.7f;
    float phaserFeedback   = 0.5f;
    int   phaserStagePairs = 3;
    float phaserMix        = 0.5f;
    float flangerRateHz    = 0.25f;
    float flangerDepth     = 0.7f;
    float flangerDelayMs   = 1.0f;
    float flangerFeedback  = 0.5f;
    float flangerMix       = 0.5f;
    float bassDb           = 0.0f;
    float trebleDb         = 0.0f;
    float toneVolumeDb     = 0.0f;
    float stereoWidth      = 1.0f;
    float stereoBalance    = 0.0f;
    bool  stereoMono       = false;
    bool  stereoSwap       = false;

    float graphicEqDb[10] {};
    float deEssFrequencyHz = 5500.0f;
    float deEssThresholdDb = -30.0f;
    float deEssReductionDb = 12.0f;
    float expThresholdDb   = -40.0f;
    float expRatio         = 2.0f;
    float expRangeDb       = 40.0f;
    float expAttackMs      = 5.0f;
    float expReleaseMs     = 100.0f;
    float ringFrequencyHz  = 440.0f;
    float ringMix          = 1.0f;
    float wahRateHz        = 1.5f;
    float wahDepth         = 0.8f;
    float wahResonance     = 4.0f;
    float wahMix           = 1.0f;
    float echoTimeMs       = 250.0f;
    int   echoTaps         = 3;
    float echoDecay        = 0.5f;
    float echoMix          = 0.35f;
    bool  echoPingPong     = false;

    float mbLowHz          = 200.0f;
    float mbHighHz         = 3000.0f;
    float mbThresholdDb[3] { -20.0f, -20.0f, -20.0f };
    float mbRatio[3]       { 3.0f, 3.0f, 3.0f };
    float mbMakeUpDb[3]    {};
    float mbAttackMs       = 10.0f;
    float mbReleaseMs      = 150.0f;

    std::array<ParametricBand, ParametricEq::kBands> peqBands {};

    TransferCurve dynCurve;
    int           dynDetector  = 0;
    float         dynAttackMs  = 5.0f;
    float         dynReleaseMs = 150.0f;
    float         dynMakeUpDb  = 0.0f;

    std::array<float, ThirdOctaveEq::kBands> geq31Db {};

    std::string convIrFile;
    float       convMix        = 0.3f;
    float       convPreDelayMs = 0.0f;
    float       convGainDb     = 0.0f;

    int   vocCarrier    = 1;
    float vocPitchHz    = 110.0f;
    int   vocBands      = 16;
    float vocResponseMs = 30.0f;
    float vocMix        = 1.0f;
    float vocGainDb     = 0.0f;

    channelmixer::Matrix mixerMatrix;
};

/** One effect in a track's chain. Virtual dispatch costs one indirect call
    per node per block, which is nothing against the work inside — and it's
    what lets a node hold only the state its own kind needs, instead of every
    node carrying a delay line it may never use. */
struct EffectProcessor
{
    virtual ~EffectProcessor() = default;

    virtual EffectNodeKind kind() const noexcept = 0;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void process(juce::AudioBuffer<float>& buffer) = 0;

    /** Bypass. Means the same thing for a hosted plugin as for a built-in: the
        node stays in the chain and passes audio through untouched. */
    virtual void setEnabled(bool enabled) = 0;

    /** Tempo, pushed once per block before process() (see EffectChain::setBpm).
        A no-op for every node except Wobble: bpm has no meaning to a filter, a
        delay in milliseconds, or a plugin, so only the one node that actually
        needs it overrides this. A default here rather than widening
        process()'s signature, so the other seven nodes' call sites don't have
        to thread through a value none of them read. */
    virtual void setBpm(double /*bpm*/) {}

};

struct FilterNode final : EffectProcessor
{
    FilterEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DelayNode final : EffectProcessor
{
    DelayEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Delay; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DriveNode final : EffectProcessor
{
    DriveEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Drive; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct CompressorNode final : EffectProcessor
{
    CompressorEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Compressor; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct TremoloNode final : EffectProcessor
{
    TremoloEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Tremolo; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ChorusNode final : EffectProcessor
{
    ChorusEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Chorus; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct WobbleNode final : EffectProcessor
{
    WobbleEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Wobble; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
    void setBpm(double bpm) override { effect.setBpm(bpm); }
};

struct GateNode final : EffectProcessor
{
    GateEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Gate; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct EqNode final : EffectProcessor
{
    EqPedalEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Eq; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct AmplifyNode final : EffectProcessor
{
    AmplifyEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Amplify; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct InvertNode final : EffectProcessor
{
    InvertEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Invert; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DcOffsetNode final : EffectProcessor
{
    DcOffsetEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::DcOffset; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct LimiterNode final : EffectProcessor
{
    LimiterEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Limiter; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct PhaserNode final : EffectProcessor
{
    PhaserEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Phaser; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct FlangerNode final : EffectProcessor
{
    FlangerEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Flanger; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct BassTrebleNode final : EffectProcessor
{
    BassTrebleEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::BassTreble; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct StereoToolNode final : EffectProcessor
{
    StereoToolEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::StereoTool; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct GraphicEqNode final : EffectProcessor
{
    GraphicEqEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::GraphicEq; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DeEsserNode final : EffectProcessor
{
    DeEsserEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::DeEsser; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ExpanderNode final : EffectProcessor
{
    ExpanderEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Expander; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct RingModNode final : EffectProcessor
{
    RingModulatorEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::RingMod; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct WahNode final : EffectProcessor
{
    WahEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Wah; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct EchoNode final : EffectProcessor
{
    EchoEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Echo; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ChannelMixerNode final : EffectProcessor
{
    ChannelMixerEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::ChannelMixer; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct VocoderNode final : EffectProcessor
{
    VocoderEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Vocoder; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ConvolutionNode final : EffectProcessor
{
    ConvolutionReverbEffect effect;
    std::string             loadedFile; // message thread only
    bool                    loaded = false;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Convolution; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct GraphicEq31Node final : EffectProcessor
{
    ThirdOctaveEqEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::GraphicEq31; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DynamicsNode final : EffectProcessor
{
    DynamicsProcessorEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Dynamics; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ParametricEqNode final : EffectProcessor
{
    ParametricEqEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::ParametricEq; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct MultibandNode final : EffectProcessor
{
    MultibandEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Multiband; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ReverbNode final : EffectProcessor
{
    ReverbEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Reverb; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

/** Applies one slot's parameters to a node, by dynamic kind.

    A free function rather than only an EffectChain member because a node is
    also configured outside a chain - the offline "apply effects to a
    selection" path and the bounce tool both build a single node and need it
    set up exactly as the live chain would. Having the dispatch in one place
    is what stops those from drifting, which they had. */
inline void applyParams(EffectProcessor& node, const EffectSlotParams& params)
{
        node.setEnabled(params.enabled);

        if (auto* filter = dynamic_cast<FilterNode*>(&node))
        {
            filter->effect.setMode(params.filterMode);
            filter->effect.setCutoff(params.filterCutoff);
            filter->effect.setResonance(params.filterResonance);
        }
        else if (auto* delay = dynamic_cast<DelayNode*>(&node))
        {
            delay->effect.setTimeMs(params.delayTimeMs);
            delay->effect.setFeedback(params.delayFeedback);
            delay->effect.setMix(params.delayMix);
        }
        else if (auto* reverb = dynamic_cast<ReverbNode*>(&node))
        {
            reverb->effect.setRoomSize(params.reverbRoomSize);
            reverb->effect.setDamping(params.reverbDamping);
            reverb->effect.setMix(params.reverbMix);
        }
        else if (auto* comp = dynamic_cast<CompressorNode*>(&node))
        {
            comp->effect.setThresholdDb(params.compThresholdDb);
            comp->effect.setRatio(params.compRatio);
            comp->effect.setAttackMs(params.compAttackMs);
            comp->effect.setReleaseMs(params.compReleaseMs);
            comp->effect.setMakeUpDb(params.compMakeUpDb);
        }
        else if (auto* chorus = dynamic_cast<ChorusNode*>(&node))
        {
            chorus->effect.setRateHz(params.chorusRateHz);
            chorus->effect.setDepth(params.chorusDepth);
            chorus->effect.setMix(params.chorusMix);
        }
        else if (auto* trem = dynamic_cast<TremoloNode*>(&node))
        {
            trem->effect.setRateHz(params.tremoloRateHz);
            trem->effect.setDepth(params.tremoloDepth);
        }
        else if (auto* wobble = dynamic_cast<WobbleNode*>(&node))
        {
            wobble->effect.setRateInBeats(params.wobbleRateBeats);
            wobble->effect.setDepth(params.wobbleDepth);
            wobble->effect.setBaseCutoffHz(params.wobbleBaseCutoffHz);
            wobble->effect.setResonance(params.wobbleResonance);
            wobble->effect.setMix(params.wobbleMix);
        }
        else if (auto* drive = dynamic_cast<DriveNode*>(&node))
        {
            drive->effect.setDrive(params.driveAmount);
            drive->effect.setTone(params.driveTone);
            drive->effect.setLevel(params.driveLevel);
            drive->effect.setHardClip(params.driveHardClip);
            drive->effect.setCabinet(params.driveCabinet);
            drive->effect.setAsymmetry(params.driveAsymmetry);
            drive->effect.setOversample(params.driveOversample);
            drive->effect.setStages(params.driveStages);
            drive->effect.setCabinetIr(params.driveCabinetIr);
        }
        else if (auto* eq = dynamic_cast<EqNode*>(&node))
        {
            eq->effect.setLowShelfHz(params.eqLowShelfHz);
            eq->effect.setLowShelfDb(params.eqLowShelfDb);
            eq->effect.setMidHz(params.eqMidHz);
            eq->effect.setMidDb(params.eqMidDb);
            eq->effect.setMidQ(params.eqMidQ);
            eq->effect.setHighShelfHz(params.eqHighShelfHz);
            eq->effect.setHighShelfDb(params.eqHighShelfDb);
        }
        else if (auto* gate = dynamic_cast<GateNode*>(&node))
        {
            gate->effect.setThresholdDb(params.gateThresholdDb);
            gate->effect.setRangeDb(params.gateRangeDb);
            gate->effect.setAttackMs(params.gateAttackMs);
            gate->effect.setHoldMs(params.gateHoldMs);
            gate->effect.setReleaseMs(params.gateReleaseMs);
        }
        else if (auto* amplify = dynamic_cast<AmplifyNode*>(&node))
        {
            amplify->effect.setGainDb(params.amplifyGainDb);
        }
        else if (auto* invert = dynamic_cast<InvertNode*>(&node))
        {
            invert->effect.setLeft(params.invertLeft);
            invert->effect.setRight(params.invertRight);
        }
        else if (auto* dc = dynamic_cast<DcOffsetNode*>(&node))
        {
            dc->effect.setCutoffHz(params.dcCutoffHz);
        }
        else if (auto* limiter = dynamic_cast<LimiterNode*>(&node))
        {
            limiter->effect.setInputGainDb(params.limiterInputDb);
            limiter->effect.setCeilingDb(params.limiterCeilingDb);
            limiter->effect.setReleaseMs(params.limiterReleaseMs);
        }
        else if (auto* phaser = dynamic_cast<PhaserNode*>(&node))
        {
            phaser->effect.setRateHz(params.phaserRateHz);
            phaser->effect.setDepth(params.phaserDepth);
            phaser->effect.setFeedback(params.phaserFeedback);
            phaser->effect.setStages(params.phaserStagePairs * 2);
            phaser->effect.setMix(params.phaserMix);
        }
        else if (auto* flanger = dynamic_cast<FlangerNode*>(&node))
        {
            flanger->effect.setRateHz(params.flangerRateHz);
            flanger->effect.setDepth(params.flangerDepth);
            flanger->effect.setDelayMs(params.flangerDelayMs);
            flanger->effect.setFeedback(params.flangerFeedback);
            flanger->effect.setMix(params.flangerMix);
        }
        else if (auto* tone = dynamic_cast<BassTrebleNode*>(&node))
        {
            tone->effect.setBassDb(params.bassDb);
            tone->effect.setTrebleDb(params.trebleDb);
            tone->effect.setVolumeDb(params.toneVolumeDb);
        }
        else if (auto* stereo = dynamic_cast<StereoToolNode*>(&node))
        {
            stereo->effect.setWidth(params.stereoWidth);
            stereo->effect.setBalance(params.stereoBalance);
            stereo->effect.setMono(params.stereoMono);
            stereo->effect.setSwap(params.stereoSwap);
        }
        else if (auto* graphic = dynamic_cast<GraphicEqNode*>(&node))
        {
            for (int band = 0; band < 10; ++band)
                graphic->effect.setBandDb(band, params.graphicEqDb[band]);
        }
        else if (auto* deEss = dynamic_cast<DeEsserNode*>(&node))
        {
            deEss->effect.setFrequencyHz(params.deEssFrequencyHz);
            deEss->effect.setThresholdDb(params.deEssThresholdDb);
            deEss->effect.setMaxReductionDb(params.deEssReductionDb);
        }
        else if (auto* expander = dynamic_cast<ExpanderNode*>(&node))
        {
            expander->effect.setThresholdDb(params.expThresholdDb);
            expander->effect.setRatio(params.expRatio);
            expander->effect.setRangeDb(params.expRangeDb);
            expander->effect.setAttackMs(params.expAttackMs);
            expander->effect.setReleaseMs(params.expReleaseMs);
        }
        else if (auto* ring = dynamic_cast<RingModNode*>(&node))
        {
            ring->effect.setFrequencyHz(params.ringFrequencyHz);
            ring->effect.setMix(params.ringMix);
        }
        else if (auto* wah = dynamic_cast<WahNode*>(&node))
        {
            wah->effect.setRateHz(params.wahRateHz);
            wah->effect.setDepth(params.wahDepth);
            wah->effect.setResonance(params.wahResonance);
            wah->effect.setMix(params.wahMix);
        }
        else if (auto* echo = dynamic_cast<EchoNode*>(&node))
        {
            echo->effect.setTimeMs(params.echoTimeMs);
            echo->effect.setTaps(params.echoTaps);
            echo->effect.setDecay(params.echoDecay);
            echo->effect.setMix(params.echoMix);
            echo->effect.setPingPong(params.echoPingPong);
        }
        else if (auto* multiband = dynamic_cast<MultibandNode*>(&node))
        {
            multiband->effect.setLowHz(params.mbLowHz);
            multiband->effect.setHighHz(params.mbHighHz);
            multiband->effect.setAttackMs(params.mbAttackMs);
            multiband->effect.setReleaseMs(params.mbReleaseMs);
            for (int band = 0; band < 3; ++band)
                multiband->effect.setBand(band, params.mbThresholdDb[band], params.mbRatio[band],
                                          params.mbMakeUpDb[band]);
        }
        else if (auto* parametric = dynamic_cast<ParametricEqNode*>(&node))
        {
            for (int band = 0; band < ParametricEq::kBands; ++band)
                parametric->effect.setBand(band, params.peqBands[(size_t) band]);
        }
        else if (auto* dynamics = dynamic_cast<DynamicsNode*>(&node))
        {
            dynamics->effect.setCurve(params.dynCurve);
            dynamics->effect.setDetector((DynamicsProcessor::Detector) params.dynDetector);
            dynamics->effect.setAttackMs(params.dynAttackMs);
            dynamics->effect.setReleaseMs(params.dynReleaseMs);
            dynamics->effect.setMakeUpDb(params.dynMakeUpDb);
        }
        else if (auto* mixer = dynamic_cast<ChannelMixerNode*>(&node))
        {
            mixer->effect.setMatrix(params.mixerMatrix);
        }
        else if (auto* vocoder = dynamic_cast<VocoderNode*>(&node))
        {
            vocoder->effect.setCarrier(params.vocCarrier);
            vocoder->effect.setPitchHz(params.vocPitchHz);
            vocoder->effect.setBands(params.vocBands);
            vocoder->effect.setResponseMs(params.vocResponseMs);
            vocoder->effect.setMix(params.vocMix);
            vocoder->effect.setGainDb(params.vocGainDb);
        }
        else if (auto* convolution = dynamic_cast<ConvolutionNode*>(&node))
        {
            convolution->effect.setMix(params.convMix);
            convolution->effect.setPreDelayMs(params.convPreDelayMs);
            convolution->effect.setGainDb(params.convGainDb);
            convolution->effect.collectRetired();

            // Read only when the file changes, not on every parameter move. A
            // file that can't be read leaves the built-in hall.
            if (! convolution->loaded || params.convIrFile != convolution->loadedFile)
            {
                convolution->loaded     = true;
                convolution->loadedFile = params.convIrFile;
                std::vector<std::vector<float>> channels;
                double                          rate = 0.0;
                loadImpulseFile(params.convIrFile, channels, rate);
                convolution->effect.setImpulse(std::move(channels), rate);
            }
        }
        else if (auto* graphic31 = dynamic_cast<GraphicEq31Node*>(&node))
        {
            for (int band = 0; band < ThirdOctaveEq::kBands; ++band)
                graphic31->effect.setBandDb(band, params.geq31Db[(size_t) band]);
        }
}

/**
    A track's insert chain: effects in order, each processing in place.

    Built and prepared on the message thread — where allocating a delay line
    is fine — then handed to the audio thread whole, the same pointer-swap the
    rest of the engine uses (Sequencer::ClipList, TrackAutomation).
    A swap can't be observed half-applied, which matters more here than
    usual: half a chain is a very different sound from all of it.

    Only *structural* changes rebuild: adding, removing, reordering, or
    changing a node's kind. Parameter changes go straight to the live nodes'
    atomics (see AudioEngine::trackChainFilter and friends), because
    rebuilding on every knob turn would reallocate delay lines and reset every
    tail in the chain — an audible glitch from turning a knob.
*/
class EffectChain
{
public:
    void add(std::unique_ptr<EffectProcessor> node) { nodes_.push_back(std::move(node)); }

    void prepare(double sampleRate, int blockSize)
    {
        for (auto& node : nodes_)
            node->prepare(sampleRate, blockSize);
    }

    /** Audio thread: run every node in order, in place. */
    void process(juce::AudioBuffer<float>& buffer)
    {
        for (auto& node : nodes_)
            node->process(buffer);
    }

    bool   empty() const noexcept { return nodes_.empty(); }
    size_t size() const noexcept  { return nodes_.size(); }

    /** Tempo, forwarded to every node once per block — see
        EffectProcessor::setBpm for why this exists instead of widening
        process()'s signature. */
    void setBpm(double bpm)
    {
        for (auto& node : nodes_)
            node->setBpm(bpm);
    }

    /** Applies one slot's parameters, addressed by *position*. By index rather
        than by kind because a chain may hold two filters, and "the filter"
        stops meaning anything the moment it does. An out-of-range index is
        ignored: the live chain can be one rebuild behind the document. */
    void applyParams(size_t index, const EffectSlotParams& params)
    {
        if (index >= nodes_.size())
            return;

        engine::applyParams(*nodes_[index], params);
    }

    EffectProcessor* nodeAt(size_t index) { return index < nodes_.size() ? nodes_[index].get() : nullptr; }

private:
    std::vector<std::unique_ptr<EffectProcessor>> nodes_;
};

} // namespace soundsplice::engine
