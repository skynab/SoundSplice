#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/EffectKind.h"
#include "engine/EffectParamValues.h"
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
/** What a chain slot should be. Carries plugin identity as plain strings —
    the engine can't reference model::PluginRef, and this is the same boundary
    the rest of the engine keeps. */
struct EffectSlotSpec
{
    EffectKind  kind = EffectKind::Filter;
    std::string pluginFormat;
    std::string pluginIdentifier;
    std::string pluginState; // base64, applied after instantiation

    /** Only identity matters for deciding whether to rebuild — a changed
        preset is restored onto the existing instance, not a new chain. */
    bool sameShapeAs(const EffectSlotSpec& other) const
    {
        return kind == other.kind
            && pluginFormat == other.pluginFormat
            && pluginIdentifier == other.pluginIdentifier;
    }
};

/** One effect in a track's chain. Virtual dispatch costs one indirect call
    per node per block, which is nothing against the work inside — and it's
    what lets a node hold only the state its own kind needs, instead of every
    node carrying a delay line it may never use. */
struct EffectProcessor
{
    virtual ~EffectProcessor() = default;

    virtual EffectKind kind() const noexcept = 0;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void process(juce::AudioBuffer<float>& buffer) = 0;

    /** Bypass. Means the same thing for a hosted plugin as for a built-in: the
        node stays in the chain and passes audio through untouched. */
    virtual void setEnabled(bool enabled) = 0;

    /** Tempo, pushed once per block before process() (see EffectChain::setBpm).
        A no-op for every node except Wobble: bpm has no meaning to a filter, a
        delay in milliseconds, or a plugin, so only the one node that actually
        needs it overrides this. A default here rather than widening
        process()'s signature, so the other nodes' call sites don't have
        to thread through a value none of them read. */
    virtual void setBpm(double /*bpm*/) {}

    /** Bypass and every parameter, from one slot's settings. Message thread:
        the setters behind it store atomics the audio thread reads.

        The live chain, the offline "apply effects to a selection" path and
        the bounce tool all configure nodes through this, so they can't set
        one up differently from each other - which they once did. */
    void applyParams(const EffectParamValues& values)
    {
        setEnabled(values.enabled);
        apply(values);
    }

protected:
    /** Reads this node's parameters out of @p values, by the ids its
        model::EffectDescriptor gives them. A hosted plugin keeps its own. */
    virtual void apply(const EffectParamValues& /*values*/) {}
};

/** What every built-in node is: one effect object, prepared, run and
    bypassed. Each node below adds only how it reads its parameters. */
template <typename Effect, EffectKind Kind>
struct BuiltInNode : EffectProcessor
{
    Effect effect;

    EffectKind kind() const noexcept override { return Kind; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct FilterNode final : BuiltInNode<FilterEffect, EffectKind::Filter>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setMode(p.getInt("mode"));
        effect.setCutoff(p.getFloat("cutoff"));
        effect.setResonance(p.getFloat("resonance"));
    }
};

struct DelayNode final : BuiltInNode<DelayEffect, EffectKind::Delay>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setTimeMs(p.getFloat("time"));
        effect.setFeedback(p.getFloat("feedback"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct ReverbNode final : BuiltInNode<ReverbEffect, EffectKind::Reverb>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRoomSize(p.getFloat("room"));
        effect.setDamping(p.getFloat("damping"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct DriveNode final : BuiltInNode<DriveEffect, EffectKind::Drive>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setDrive(p.getFloat("drive"));
        effect.setTone(p.getFloat("tone"));
        effect.setLevel(p.getFloat("level"));
        effect.setHardClip(p.getBool("hardClip"));
        effect.setCabinet(p.getBool("cabinet"));
        effect.setAsymmetry(p.getFloat("asymmetry"));
        effect.setOversample(p.getBool("oversample"));
        effect.setStages(p.getInt("stages"));
        effect.setCabinetIr(p.getBool("cabinetIr"));
    }
};

struct CompressorNode final : BuiltInNode<CompressorEffect, EffectKind::Compressor>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setThresholdDb(p.getFloat("threshold"));
        effect.setRatio(p.getFloat("ratio"));
        effect.setAttackMs(p.getFloat("attack"));
        effect.setReleaseMs(p.getFloat("release"));
        effect.setMakeUpDb(p.getFloat("makeUp"));
    }
};

struct TremoloNode final : BuiltInNode<TremoloEffect, EffectKind::Tremolo>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRateHz(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
    }
};

struct ChorusNode final : BuiltInNode<ChorusEffect, EffectKind::Chorus>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRateHz(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct WobbleNode final : BuiltInNode<WobbleEffect, EffectKind::Wobble>
{
    void setBpm(double bpm) override { effect.setBpm(bpm); }

    void apply(const EffectParamValues& p) override
    {
        effect.setRateInBeats(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
        effect.setBaseCutoffHz(p.getFloat("cutoff"));
        effect.setResonance(p.getFloat("resonance"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct GateNode final : BuiltInNode<GateEffect, EffectKind::Gate>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setThresholdDb(p.getFloat("threshold"));
        effect.setRangeDb(p.getFloat("range"));
        effect.setAttackMs(p.getFloat("attack"));
        effect.setHoldMs(p.getFloat("hold"));
        effect.setReleaseMs(p.getFloat("release"));
    }
};

struct EqNode final : BuiltInNode<EqPedalEffect, EffectKind::Eq>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setLowShelfHz(p.getFloat("lowFreq"));
        effect.setLowShelfDb(p.getFloat("low"));
        effect.setMidHz(p.getFloat("midFreq"));
        effect.setMidDb(p.getFloat("mid"));
        effect.setMidQ(p.getFloat("midQ"));
        effect.setHighShelfHz(p.getFloat("highFreq"));
        effect.setHighShelfDb(p.getFloat("high"));
    }
};

struct AmplifyNode final : BuiltInNode<AmplifyEffect, EffectKind::Amplify>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setGainDb(p.getFloat("gain"));
    }
};

struct InvertNode final : BuiltInNode<InvertEffect, EffectKind::Invert>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setLeft(p.getBool("left"));
        effect.setRight(p.getBool("right"));
    }
};

struct DcOffsetNode final : BuiltInNode<DcOffsetEffect, EffectKind::DcOffset>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setCutoffHz(p.getFloat("cutoff"));
    }
};

struct LimiterNode final : BuiltInNode<LimiterEffect, EffectKind::Limiter>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setInputGainDb(p.getFloat("input"));
        effect.setCeilingDb(p.getFloat("ceiling"));
        effect.setReleaseMs(p.getFloat("release"));
    }
};

struct PhaserNode final : BuiltInNode<PhaserEffect, EffectKind::Phaser>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRateHz(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
        effect.setFeedback(p.getFloat("feedback"));
        effect.setStages(p.getInt("stages") * 2); // stored as pairs, so every setting makes whole notches
        effect.setMix(p.getFloat("mix"));
    }
};

struct FlangerNode final : BuiltInNode<FlangerEffect, EffectKind::Flanger>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRateHz(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
        effect.setDelayMs(p.getFloat("delay"));
        effect.setFeedback(p.getFloat("feedback"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct BassTrebleNode final : BuiltInNode<BassTrebleEffect, EffectKind::BassTreble>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setBassDb(p.getFloat("bass"));
        effect.setTrebleDb(p.getFloat("treble"));
        effect.setVolumeDb(p.getFloat("volume"));
    }
};

struct StereoToolNode final : BuiltInNode<StereoToolEffect, EffectKind::StereoTool>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setWidth(p.getFloat("width"));
        effect.setBalance(p.getFloat("balance"));
        effect.setMono(p.getBool("mono"));
        effect.setSwap(p.getBool("swap"));
    }
};

struct GraphicEqNode final : BuiltInNode<GraphicEqEffect, EffectKind::GraphicEq>
{
    void apply(const EffectParamValues& p) override
    {
        static constexpr const char* kIds[] { "band31", "band62", "band125", "band250", "band500",
                                             "band1k",  "band2k",  "band4k",  "band8k",  "band16k" };
        for (int band = 0; band < 10; ++band)
            effect.setBandDb(band, p.getFloat(kIds[band]));
    }
};

struct DeEsserNode final : BuiltInNode<DeEsserEffect, EffectKind::DeEsser>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setFrequencyHz(p.getFloat("frequency"));
        effect.setThresholdDb(p.getFloat("threshold"));
        effect.setMaxReductionDb(p.getFloat("reduction"));
    }
};

struct ExpanderNode final : BuiltInNode<ExpanderEffect, EffectKind::Expander>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setThresholdDb(p.getFloat("threshold"));
        effect.setRatio(p.getFloat("ratio"));
        effect.setRangeDb(p.getFloat("range"));
        effect.setAttackMs(p.getFloat("attack"));
        effect.setReleaseMs(p.getFloat("release"));
    }
};

struct RingModNode final : BuiltInNode<RingModulatorEffect, EffectKind::RingMod>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setFrequencyHz(p.getFloat("frequency"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct WahNode final : BuiltInNode<WahEffect, EffectKind::Wah>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setRateHz(p.getFloat("rate"));
        effect.setDepth(p.getFloat("depth"));
        effect.setResonance(p.getFloat("resonance"));
        effect.setMix(p.getFloat("mix"));
    }
};

struct EchoNode final : BuiltInNode<EchoEffect, EffectKind::Echo>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setTimeMs(p.getFloat("time"));
        effect.setTaps(p.getInt("taps"));
        effect.setDecay(p.getFloat("decay"));
        effect.setMix(p.getFloat("mix"));
        effect.setPingPong(p.getBool("pingPong"));
    }
};

struct MultibandNode final : BuiltInNode<MultibandEffect, EffectKind::Multiband>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setLowHz(p.getFloat("lowCrossover"));
        effect.setHighHz(p.getFloat("highCrossover"));
        effect.setAttackMs(p.getFloat("attack"));
        effect.setReleaseMs(p.getFloat("release"));

        static constexpr const char* kBands[] { "low", "mid", "high" };
        for (int band = 0; band < 3; ++band)
        {
            const std::string name = kBands[band];
            effect.setBand(band, p.getFloat(name + "Threshold"), p.getFloat(name + "Ratio"), p.getFloat(name + "MakeUp"));
        }
    }
};

struct ParametricEqNode final : BuiltInNode<ParametricEqEffect, EffectKind::ParametricEq>
{
    void apply(const EffectParamValues& p) override
    {
        for (int band = 0; band < ParametricEq::kBands; ++band)
        {
            const auto     name = "band" + std::to_string(band + 1);
            ParametricBand settings;
            settings.type   = (ParametricBand::Type) std::clamp(p.getInt(name + "Type"), 0, 6);
            settings.hz     = p.getFloat(name + "Hz");
            settings.gainDb = p.getFloat(name + "Gain");
            settings.q      = p.getFloat(name + "Q");
            effect.setBand(band, settings);
        }
    }
};

struct DynamicsNode final : BuiltInNode<DynamicsProcessorEffect, EffectKind::Dynamics>
{
    void apply(const EffectParamValues& p) override
    {
        TransferCurve curve;
        curve.count = std::clamp(p.getInt("points"), 2, TransferCurve::kMaxPoints);
        for (int point = 0; point < TransferCurve::kMaxPoints; ++point)
        {
            const auto name = "point" + std::to_string(point + 1);
            curve.points[(size_t) point] = { p.getFloat(name + "In"), p.getFloat(name + "Out") };
        }

        effect.setCurve(curve);
        effect.setDetector((DynamicsProcessor::Detector) p.getInt("detector"));
        effect.setAttackMs(p.getFloat("attack"));
        effect.setReleaseMs(p.getFloat("release"));
        effect.setMakeUpDb(p.getFloat("makeUp"));
    }
};

struct GraphicEq31Node final : BuiltInNode<ThirdOctaveEqEffect, EffectKind::GraphicEq31>
{
    void apply(const EffectParamValues& p) override
    {
        for (int band = 0; band < ThirdOctaveEq::kBands; ++band)
            effect.setBandDb(band, p.getFloat("band" + std::to_string(band + 1)));
    }
};

struct ConvolutionNode final : BuiltInNode<ConvolutionReverbEffect, EffectKind::Convolution>
{
    std::string loadedFile; // message thread only
    bool        loaded = false;

    void apply(const EffectParamValues& p) override
    {
        effect.setMix(p.getFloat("mix"));
        effect.setPreDelayMs(p.getFloat("preDelay"));
        effect.setGainDb(p.getFloat("gain"));
        effect.collectRetired();

        // Read only when the file changes, not on every parameter move. A
        // file that can't be read leaves the built-in hall.
        const auto& file = p.text("irFile");
        if (! loaded || file != loadedFile)
        {
            loaded     = true;
            loadedFile = file;
            std::vector<std::vector<float>> channels;
            double                          rate = 0.0;
            loadImpulseFile(file, channels, rate);
            effect.setImpulse(std::move(channels), rate);
        }
    }
};

struct VocoderNode final : BuiltInNode<VocoderEffect, EffectKind::Vocoder>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setCarrier(p.getInt("carrier"));
        effect.setPitchHz(p.getFloat("pitch"));
        effect.setBands(p.getInt("bands"));
        effect.setResponseMs(p.getFloat("response"));
        effect.setMix(p.getFloat("mix"));
        effect.setGainDb(p.getFloat("gain"));
    }
};

struct ChannelMixerNode final : BuiltInNode<ChannelMixerEffect, EffectKind::ChannelMixer>
{
    void apply(const EffectParamValues& p) override
    {
        effect.setMatrix({ p.getFloat("leftToLeft"), p.getFloat("rightToLeft"), p.getFloat("leftToRight"),
                          p.getFloat("rightToRight"), (channelmixer::MidSide) std::clamp(p.getInt("midSide"), 0, 3) });
    }
};

/** A new node for a built-in effect kind, or nullptr for a kind that can't
    be built here (Plugin, which needs the plugin host). The one list of
    which node realises which kind: the live chain, the offline render and
    the bounce tool all build through it. */
inline std::unique_ptr<EffectProcessor> makeBuiltInNode(EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::Filter:        return std::make_unique<FilterNode>();
        case EffectKind::Delay:         return std::make_unique<DelayNode>();
        case EffectKind::Reverb:        return std::make_unique<ReverbNode>();
        case EffectKind::Drive:         return std::make_unique<DriveNode>();
        case EffectKind::Compressor:    return std::make_unique<CompressorNode>();
        case EffectKind::Tremolo:       return std::make_unique<TremoloNode>();
        case EffectKind::Chorus:        return std::make_unique<ChorusNode>();
        case EffectKind::Wobble:        return std::make_unique<WobbleNode>();
        case EffectKind::Gate:          return std::make_unique<GateNode>();
        case EffectKind::Eq:            return std::make_unique<EqNode>();
        case EffectKind::Amplify:       return std::make_unique<AmplifyNode>();
        case EffectKind::Invert:        return std::make_unique<InvertNode>();
        case EffectKind::DcOffset:      return std::make_unique<DcOffsetNode>();
        case EffectKind::Limiter:       return std::make_unique<LimiterNode>();
        case EffectKind::Phaser:        return std::make_unique<PhaserNode>();
        case EffectKind::Flanger:       return std::make_unique<FlangerNode>();
        case EffectKind::BassTreble:    return std::make_unique<BassTrebleNode>();
        case EffectKind::StereoTool:    return std::make_unique<StereoToolNode>();
        case EffectKind::GraphicEq:     return std::make_unique<GraphicEqNode>();
        case EffectKind::DeEsser:       return std::make_unique<DeEsserNode>();
        case EffectKind::Expander:      return std::make_unique<ExpanderNode>();
        case EffectKind::RingMod:       return std::make_unique<RingModNode>();
        case EffectKind::Wah:           return std::make_unique<WahNode>();
        case EffectKind::Echo:          return std::make_unique<EchoNode>();
        case EffectKind::Multiband:     return std::make_unique<MultibandNode>();
        case EffectKind::ParametricEq:  return std::make_unique<ParametricEqNode>();
        case EffectKind::Dynamics:      return std::make_unique<DynamicsNode>();
        case EffectKind::GraphicEq31:   return std::make_unique<GraphicEq31Node>();
        case EffectKind::Convolution:   return std::make_unique<ConvolutionNode>();
        case EffectKind::Vocoder:       return std::make_unique<VocoderNode>();
        case EffectKind::ChannelMixer:  return std::make_unique<ChannelMixerNode>();
        case EffectKind::Plugin:        return nullptr;
    }
    return nullptr;
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
        stops meaning anything the moment it does. An out-of-range index, or a
        node of another kind, is ignored: the live chain can be one rebuild
        behind the document, and a node reading another effect's values by
        its own ids would find none of them. */
    void applyParams(size_t index, const EffectParamValues& values)
    {
        if (index >= nodes_.size() || nodes_[index]->kind() != values.kind)
            return;

        nodes_[index]->applyParams(values);
    }

    EffectProcessor* nodeAt(size_t index) { return index < nodes_.size() ? nodes_[index].get() : nullptr; }

private:
    std::vector<std::unique_ptr<EffectProcessor>> nodes_;
};

} // namespace soundsplice::engine
