#pragma once

#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DelayEffect.h"
#include "engine/DriveEffect.h"
#include "engine/PedalEffects.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"

namespace looper::engine
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
    Eq         = 10
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

    /** The detector signal for this block, or nullptr for "listen to your own
        input" — pushed once per block before process(), exactly like setBpm,
        and a no-op for every node but the compressor for exactly the same
        reason: a filter has no use for another track's audio, and widening
        process() would make eight call sites thread through a value one of
        them reads.

        The buffer belongs to whoever rendered it and is only valid for the
        duration of this block. Nothing here retains it. */
    virtual void setSidechainInput(const juce::AudioBuffer<float>* /*input*/) {}
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
    void setSidechainInput(const juce::AudioBuffer<float>* input) override
    {
        effect.setSidechainInput(input);
    }
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
}

/**
    A track's insert chain: effects in order, each processing in place.

    Built and prepared on the message thread — where allocating a delay line
    is fine — then handed to the audio thread whole, the same pointer-swap the
    rest of the engine uses (Sequencer::ClipList, DrumPadMap, TrackAutomation).
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

    /** The detector signal for this block, pushed to every node the same way
        setBpm is; only a compressor does anything with it. nullptr (the
        default state) means every compressor here listens to its own input. */
    void setSidechainInput(const juce::AudioBuffer<float>* input)
    {
        for (auto& node : nodes_)
            node->setSidechainInput(input);
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

} // namespace looper::engine
