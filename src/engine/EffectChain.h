#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
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

    /** One parameter, by its descriptor id: what automation sets each block.
        Audio-thread safe - a table lookup and an atomic store, nothing
        allocated. False if this node has no such parameter. */
    virtual bool setParam(std::string_view /*id*/, float /*value*/) { return false; }

    /** How late this node's output is against its input, in samples, as
        things stand (a bypassed node adds none). Most add none at all; a
        lookahead limiter and many plugins do. The track compensates for it -
        see InstrumentTrack's delay compensation. Audio thread safe. */
    virtual int latencySamples() const noexcept { return 0; }

    /** The detector signal for this block, for an effect that can listen to
        another track (model::canBeKeyed), or nullptr for its own input. Set
        before process() each block; a no-op for everything else. */
    virtual void setSidechainInput(const juce::AudioBuffer<float>* /*input*/) {}

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

/** One parameter of a built-in node: the id its model::EffectDescriptor
    gives it, and how to set it. A node's table of these is its whole
    parameter surface - applying a slot's settings and automating one
    parameter both go through it, so the two can't disagree.

    A family of numbered parameters (a parametric EQ's six band gains) is one
    entry: ids id1..idN, each followed by suffix, with the number, from 0,
    passed to the setter. */
template <typename Effect>
struct NodeParam
{
    const char* id;
    void (*set)(Effect&, int index, float value);
    int         count  = 0; // 0 for a single parameter
    const char* suffix = "";

    /** Whether @p name is this parameter, or one of its family, and which.
        Allocates nothing, so it's safe on the audio thread. */
    bool matches(std::string_view name, int& index) const noexcept
    {
        const std::string_view prefix = id, tail = suffix;
        if (count == 0)
        {
            index = 0;
            return name == prefix;
        }

        if (name.size() <= prefix.size() + tail.size() || name.substr(0, prefix.size()) != prefix
            || name.substr(name.size() - tail.size()) != tail)
            return false;

        int number = 0;
        for (const char c : name.substr(prefix.size(), name.size() - prefix.size() - tail.size()))
        {
            if (c < '0' || c > '9' || number > count)
                return false;
            number = number * 10 + (c - '0');
        }
        index = number - 1;
        return number >= 1 && number <= count;
    }

    /** The id of member @p index of this entry. Message thread: it allocates. */
    std::string idAt(int index) const
    {
        return count == 0 ? std::string(id) : id + std::to_string(index + 1) + suffix;
    }
};

/** What every built-in node is: one effect object, prepared, run and
    bypassed, with its parameters set through Derived::kParams. Each node
    below adds only that table. */
template <typename Derived, typename Effect, EffectKind Kind>
struct BuiltInNode : EffectProcessor
{
    using Param = NodeParam<Effect>;

    Effect effect;

    EffectKind kind() const noexcept override { return Kind; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }

    bool setParam(std::string_view id, float value) override
    {
        for (const auto& param : Derived::kParams)
        {
            int index = 0;
            if (param.matches(id, index))
            {
                param.set(effect, index, value);
                return true;
            }
        }
        return false;
    }

protected:
    /** Every parameter in the table that @p values has. One it lacks is left
        as it is: that's how an automated parameter is kept out of the static
        settings, so the two don't take turns. */
    void apply(const EffectParamValues& values) override
    {
        for (const auto& param : Derived::kParams)
            for (int index = 0; index < std::max(1, param.count); ++index)
                if (const auto value = values.find(param.idAt(index)))
                    param.set(effect, index, (float) *value);
    }
};

struct FilterNode final : BuiltInNode<FilterNode, FilterEffect, EffectKind::Filter>
{
    static constexpr Param kParams[] {
        { "mode", [](FilterEffect& e, int, float v) { e.setMode((int) std::lround(v)); } },
        { "cutoff", [](FilterEffect& e, int, float v) { e.setCutoff(v); } },
        { "resonance", [](FilterEffect& e, int, float v) { e.setResonance(v); } },
    };
};

struct DelayNode final : BuiltInNode<DelayNode, DelayEffect, EffectKind::Delay>
{
    static constexpr Param kParams[] {
        { "time", [](DelayEffect& e, int, float v) { e.setTimeMs(v); } },
        { "feedback", [](DelayEffect& e, int, float v) { e.setFeedback(v); } },
        { "mix", [](DelayEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct ReverbNode final : BuiltInNode<ReverbNode, ReverbEffect, EffectKind::Reverb>
{
    static constexpr Param kParams[] {
        { "room", [](ReverbEffect& e, int, float v) { e.setRoomSize(v); } },
        { "damping", [](ReverbEffect& e, int, float v) { e.setDamping(v); } },
        { "mix", [](ReverbEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct DriveNode final : BuiltInNode<DriveNode, DriveEffect, EffectKind::Drive>
{
    static constexpr Param kParams[] {
        { "drive", [](DriveEffect& e, int, float v) { e.setDrive(v); } },
        { "tone", [](DriveEffect& e, int, float v) { e.setTone(v); } },
        { "level", [](DriveEffect& e, int, float v) { e.setLevel(v); } },
        { "hardClip", [](DriveEffect& e, int, float v) { e.setHardClip(v >= 0.5f); } },
        { "cabinet", [](DriveEffect& e, int, float v) { e.setCabinet(v >= 0.5f); } },
        { "asymmetry", [](DriveEffect& e, int, float v) { e.setAsymmetry(v); } },
        { "oversample", [](DriveEffect& e, int, float v) { e.setOversample(v >= 0.5f); } },
        { "stages", [](DriveEffect& e, int, float v) { e.setStages((int) std::lround(v)); } },
        { "cabinetIr", [](DriveEffect& e, int, float v) { e.setCabinetIr(v >= 0.5f); } },
    };
};

struct CompressorNode final : BuiltInNode<CompressorNode, CompressorEffect, EffectKind::Compressor>
{
    static constexpr Param kParams[] {
        { "threshold", [](CompressorEffect& e, int, float v) { e.setThresholdDb(v); } },
        { "ratio", [](CompressorEffect& e, int, float v) { e.setRatio(v); } },
        { "attack", [](CompressorEffect& e, int, float v) { e.setAttackMs(v); } },
        { "release", [](CompressorEffect& e, int, float v) { e.setReleaseMs(v); } },
        { "makeUp", [](CompressorEffect& e, int, float v) { e.setMakeUpDb(v); } },
    };

    void setSidechainInput(const juce::AudioBuffer<float>* input) override { effect.setSidechainInput(input); }
};

struct TremoloNode final : BuiltInNode<TremoloNode, TremoloEffect, EffectKind::Tremolo>
{
    static constexpr Param kParams[] {
        { "rate", [](TremoloEffect& e, int, float v) { e.setRateHz(v); } },
        { "depth", [](TremoloEffect& e, int, float v) { e.setDepth(v); } },
    };
};

struct ChorusNode final : BuiltInNode<ChorusNode, ChorusEffect, EffectKind::Chorus>
{
    static constexpr Param kParams[] {
        { "rate", [](ChorusEffect& e, int, float v) { e.setRateHz(v); } },
        { "depth", [](ChorusEffect& e, int, float v) { e.setDepth(v); } },
        { "mix", [](ChorusEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct WobbleNode final : BuiltInNode<WobbleNode, WobbleEffect, EffectKind::Wobble>
{
    void setBpm(double bpm) override { effect.setBpm(bpm); }

    static constexpr Param kParams[] {
        { "rate", [](WobbleEffect& e, int, float v) { e.setRateInBeats(v); } },
        { "depth", [](WobbleEffect& e, int, float v) { e.setDepth(v); } },
        { "cutoff", [](WobbleEffect& e, int, float v) { e.setBaseCutoffHz(v); } },
        { "resonance", [](WobbleEffect& e, int, float v) { e.setResonance(v); } },
        { "mix", [](WobbleEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct GateNode final : BuiltInNode<GateNode, GateEffect, EffectKind::Gate>
{
    static constexpr Param kParams[] {
        { "threshold", [](GateEffect& e, int, float v) { e.setThresholdDb(v); } },
        { "range", [](GateEffect& e, int, float v) { e.setRangeDb(v); } },
        { "attack", [](GateEffect& e, int, float v) { e.setAttackMs(v); } },
        { "hold", [](GateEffect& e, int, float v) { e.setHoldMs(v); } },
        { "release", [](GateEffect& e, int, float v) { e.setReleaseMs(v); } },
    };

    void setSidechainInput(const juce::AudioBuffer<float>* input) override { effect.setSidechainInput(input); }
};

struct EqNode final : BuiltInNode<EqNode, EqPedalEffect, EffectKind::Eq>
{
    static constexpr Param kParams[] {
        { "lowFreq", [](EqPedalEffect& e, int, float v) { e.setLowShelfHz(v); } },
        { "low", [](EqPedalEffect& e, int, float v) { e.setLowShelfDb(v); } },
        { "midFreq", [](EqPedalEffect& e, int, float v) { e.setMidHz(v); } },
        { "mid", [](EqPedalEffect& e, int, float v) { e.setMidDb(v); } },
        { "midQ", [](EqPedalEffect& e, int, float v) { e.setMidQ(v); } },
        { "highFreq", [](EqPedalEffect& e, int, float v) { e.setHighShelfHz(v); } },
        { "high", [](EqPedalEffect& e, int, float v) { e.setHighShelfDb(v); } },
    };
};

struct AmplifyNode final : BuiltInNode<AmplifyNode, AmplifyEffect, EffectKind::Amplify>
{
    static constexpr Param kParams[] {
        { "gain", [](AmplifyEffect& e, int, float v) { e.setGainDb(v); } },
    };
};

struct InvertNode final : BuiltInNode<InvertNode, InvertEffect, EffectKind::Invert>
{
    static constexpr Param kParams[] {
        { "left", [](InvertEffect& e, int, float v) { e.setLeft(v >= 0.5f); } },
        { "right", [](InvertEffect& e, int, float v) { e.setRight(v >= 0.5f); } },
    };
};

struct DcOffsetNode final : BuiltInNode<DcOffsetNode, DcOffsetEffect, EffectKind::DcOffset>
{
    static constexpr Param kParams[] {
        { "cutoff", [](DcOffsetEffect& e, int, float v) { e.setCutoffHz(v); } },
    };
};

struct LimiterNode final : BuiltInNode<LimiterNode, LimiterEffect, EffectKind::Limiter>
{
    int latencySamples() const noexcept override { return effect.latencySamples(); }

    static constexpr Param kParams[] {
        { "input", [](LimiterEffect& e, int, float v) { e.setInputGainDb(v); } },
        { "ceiling", [](LimiterEffect& e, int, float v) { e.setCeilingDb(v); } },
        { "release", [](LimiterEffect& e, int, float v) { e.setReleaseMs(v); } },
    };
};

struct PhaserNode final : BuiltInNode<PhaserNode, PhaserEffect, EffectKind::Phaser>
{
    static constexpr Param kParams[] {
        { "rate", [](PhaserEffect& e, int, float v) { e.setRateHz(v); } },
        { "depth", [](PhaserEffect& e, int, float v) { e.setDepth(v); } },
        { "feedback", [](PhaserEffect& e, int, float v) { e.setFeedback(v); } },
        { "stages", [](PhaserEffect& e, int, float v) { e.setStages((int) std::lround(v) * 2); } },
        { "mix", [](PhaserEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct FlangerNode final : BuiltInNode<FlangerNode, FlangerEffect, EffectKind::Flanger>
{
    static constexpr Param kParams[] {
        { "rate", [](FlangerEffect& e, int, float v) { e.setRateHz(v); } },
        { "depth", [](FlangerEffect& e, int, float v) { e.setDepth(v); } },
        { "delay", [](FlangerEffect& e, int, float v) { e.setDelayMs(v); } },
        { "feedback", [](FlangerEffect& e, int, float v) { e.setFeedback(v); } },
        { "mix", [](FlangerEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct BassTrebleNode final : BuiltInNode<BassTrebleNode, BassTrebleEffect, EffectKind::BassTreble>
{
    static constexpr Param kParams[] {
        { "bass", [](BassTrebleEffect& e, int, float v) { e.setBassDb(v); } },
        { "treble", [](BassTrebleEffect& e, int, float v) { e.setTrebleDb(v); } },
        { "volume", [](BassTrebleEffect& e, int, float v) { e.setVolumeDb(v); } },
    };
};

struct StereoToolNode final : BuiltInNode<StereoToolNode, StereoToolEffect, EffectKind::StereoTool>
{
    static constexpr Param kParams[] {
        { "width", [](StereoToolEffect& e, int, float v) { e.setWidth(v); } },
        { "balance", [](StereoToolEffect& e, int, float v) { e.setBalance(v); } },
        { "mono", [](StereoToolEffect& e, int, float v) { e.setMono(v >= 0.5f); } },
        { "swap", [](StereoToolEffect& e, int, float v) { e.setSwap(v >= 0.5f); } },
    };
};

struct GraphicEqNode final : BuiltInNode<GraphicEqNode, GraphicEqEffect, EffectKind::GraphicEq>
{
    static constexpr Param kParams[] {
        { "band31", [](GraphicEqEffect& e, int, float v) { e.setBandDb(0, v); } },
        { "band62", [](GraphicEqEffect& e, int, float v) { e.setBandDb(1, v); } },
        { "band125", [](GraphicEqEffect& e, int, float v) { e.setBandDb(2, v); } },
        { "band250", [](GraphicEqEffect& e, int, float v) { e.setBandDb(3, v); } },
        { "band500", [](GraphicEqEffect& e, int, float v) { e.setBandDb(4, v); } },
        { "band1k", [](GraphicEqEffect& e, int, float v) { e.setBandDb(5, v); } },
        { "band2k", [](GraphicEqEffect& e, int, float v) { e.setBandDb(6, v); } },
        { "band4k", [](GraphicEqEffect& e, int, float v) { e.setBandDb(7, v); } },
        { "band8k", [](GraphicEqEffect& e, int, float v) { e.setBandDb(8, v); } },
        { "band16k", [](GraphicEqEffect& e, int, float v) { e.setBandDb(9, v); } },
    };
};

struct DeEsserNode final : BuiltInNode<DeEsserNode, DeEsserEffect, EffectKind::DeEsser>
{
    static constexpr Param kParams[] {
        { "frequency", [](DeEsserEffect& e, int, float v) { e.setFrequencyHz(v); } },
        { "threshold", [](DeEsserEffect& e, int, float v) { e.setThresholdDb(v); } },
        { "reduction", [](DeEsserEffect& e, int, float v) { e.setMaxReductionDb(v); } },
    };
};

struct ExpanderNode final : BuiltInNode<ExpanderNode, ExpanderEffect, EffectKind::Expander>
{
    static constexpr Param kParams[] {
        { "threshold", [](ExpanderEffect& e, int, float v) { e.setThresholdDb(v); } },
        { "ratio", [](ExpanderEffect& e, int, float v) { e.setRatio(v); } },
        { "range", [](ExpanderEffect& e, int, float v) { e.setRangeDb(v); } },
        { "attack", [](ExpanderEffect& e, int, float v) { e.setAttackMs(v); } },
        { "release", [](ExpanderEffect& e, int, float v) { e.setReleaseMs(v); } },
    };
};

struct RingModNode final : BuiltInNode<RingModNode, RingModulatorEffect, EffectKind::RingMod>
{
    static constexpr Param kParams[] {
        { "frequency", [](RingModulatorEffect& e, int, float v) { e.setFrequencyHz(v); } },
        { "mix", [](RingModulatorEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct WahNode final : BuiltInNode<WahNode, WahEffect, EffectKind::Wah>
{
    static constexpr Param kParams[] {
        { "rate", [](WahEffect& e, int, float v) { e.setRateHz(v); } },
        { "depth", [](WahEffect& e, int, float v) { e.setDepth(v); } },
        { "resonance", [](WahEffect& e, int, float v) { e.setResonance(v); } },
        { "mix", [](WahEffect& e, int, float v) { e.setMix(v); } },
    };
};

struct EchoNode final : BuiltInNode<EchoNode, EchoEffect, EffectKind::Echo>
{
    static constexpr Param kParams[] {
        { "time", [](EchoEffect& e, int, float v) { e.setTimeMs(v); } },
        { "taps", [](EchoEffect& e, int, float v) { e.setTaps((int) std::lround(v)); } },
        { "decay", [](EchoEffect& e, int, float v) { e.setDecay(v); } },
        { "mix", [](EchoEffect& e, int, float v) { e.setMix(v); } },
        { "pingPong", [](EchoEffect& e, int, float v) { e.setPingPong(v >= 0.5f); } },
    };
};

struct MultibandNode final : BuiltInNode<MultibandNode, MultibandEffect, EffectKind::Multiband>
{
    static constexpr Param kParams[] {
        { "lowCrossover", [](MultibandEffect& e, int, float v) { e.setLowHz(v); } },
        { "highCrossover", [](MultibandEffect& e, int, float v) { e.setHighHz(v); } },
        { "attack", [](MultibandEffect& e, int, float v) { e.setAttackMs(v); } },
        { "release", [](MultibandEffect& e, int, float v) { e.setReleaseMs(v); } },
        { "lowThreshold", [](MultibandEffect& e, int, float v) { e.setBandThresholdDb(0, v); } },
        { "lowRatio", [](MultibandEffect& e, int, float v) { e.setBandRatio(0, v); } },
        { "lowMakeUp", [](MultibandEffect& e, int, float v) { e.setBandMakeUpDb(0, v); } },
        { "midThreshold", [](MultibandEffect& e, int, float v) { e.setBandThresholdDb(1, v); } },
        { "midRatio", [](MultibandEffect& e, int, float v) { e.setBandRatio(1, v); } },
        { "midMakeUp", [](MultibandEffect& e, int, float v) { e.setBandMakeUpDb(1, v); } },
        { "highThreshold", [](MultibandEffect& e, int, float v) { e.setBandThresholdDb(2, v); } },
        { "highRatio", [](MultibandEffect& e, int, float v) { e.setBandRatio(2, v); } },
        { "highMakeUp", [](MultibandEffect& e, int, float v) { e.setBandMakeUpDb(2, v); } },
    };
};

struct ParametricEqNode final : BuiltInNode<ParametricEqNode, ParametricEqEffect, EffectKind::ParametricEq>
{
    static constexpr Param kParams[] {
        { "band", [](ParametricEqEffect& e, int i, float v) { e.setBandType(i, std::clamp((int) std::lround(v), 0, 6)); }, 6, "Type" },
        { "band", [](ParametricEqEffect& e, int i, float v) { e.setBandHz(i, v); }, 6, "Hz" },
        { "band", [](ParametricEqEffect& e, int i, float v) { e.setBandGainDb(i, v); }, 6, "Gain" },
        { "band", [](ParametricEqEffect& e, int i, float v) { e.setBandQ(i, v); }, 6, "Q" },
    };
};

struct DynamicsNode final : BuiltInNode<DynamicsNode, DynamicsProcessorEffect, EffectKind::Dynamics>
{
    static constexpr Param kParams[] {
        { "points", [](DynamicsProcessorEffect& e, int, float v) { e.setPointCount(std::clamp((int) std::lround(v), 2, TransferCurve::kMaxPoints)); } },
        { "point", [](DynamicsProcessorEffect& e, int i, float v) { e.setPointInDb(i, v); }, 6, "In" },
        { "point", [](DynamicsProcessorEffect& e, int i, float v) { e.setPointOutDb(i, v); }, 6, "Out" },
        { "detector", [](DynamicsProcessorEffect& e, int, float v) { e.setDetector((DynamicsProcessor::Detector) (int) std::lround(v)); } },
        { "attack", [](DynamicsProcessorEffect& e, int, float v) { e.setAttackMs(v); } },
        { "release", [](DynamicsProcessorEffect& e, int, float v) { e.setReleaseMs(v); } },
        { "makeUp", [](DynamicsProcessorEffect& e, int, float v) { e.setMakeUpDb(v); } },
    };
};

struct GraphicEq31Node final : BuiltInNode<GraphicEq31Node, ThirdOctaveEqEffect, EffectKind::GraphicEq31>
{
    static constexpr Param kParams[] {
        { "band", [](ThirdOctaveEqEffect& e, int i, float v) { e.setBandDb(i, v); }, 31, "" },
    };
};

struct ConvolutionNode final : BuiltInNode<ConvolutionNode, ConvolutionReverbEffect, EffectKind::Convolution>
{
    std::string loadedFile; // message thread only
    bool        loaded = false;

    // The impulse response is a file, not a number, so it isn't in the table
    // and can't be automated: it's read here, on the message thread.
    void apply(const EffectParamValues& values) override
    {
        BuiltInNode::apply(values);
        effect.collectRetired();

        // Read only when the file changes, not on every parameter move. A
        // file that can't be read leaves the built-in hall.
        const auto& file = values.text("irFile");
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

    static constexpr Param kParams[] {
        { "mix", [](ConvolutionReverbEffect& e, int, float v) { e.setMix(v); } },
        { "preDelay", [](ConvolutionReverbEffect& e, int, float v) { e.setPreDelayMs(v); } },
        { "gain", [](ConvolutionReverbEffect& e, int, float v) { e.setGainDb(v); } },
    };
};

struct VocoderNode final : BuiltInNode<VocoderNode, VocoderEffect, EffectKind::Vocoder>
{
    static constexpr Param kParams[] {
        { "carrier", [](VocoderEffect& e, int, float v) { e.setCarrier((int) std::lround(v)); } },
        { "pitch", [](VocoderEffect& e, int, float v) { e.setPitchHz(v); } },
        { "bands", [](VocoderEffect& e, int, float v) { e.setBands((int) std::lround(v)); } },
        { "response", [](VocoderEffect& e, int, float v) { e.setResponseMs(v); } },
        { "mix", [](VocoderEffect& e, int, float v) { e.setMix(v); } },
        { "gain", [](VocoderEffect& e, int, float v) { e.setGainDb(v); } },
    };
};

struct ChannelMixerNode final : BuiltInNode<ChannelMixerNode, ChannelMixerEffect, EffectKind::ChannelMixer>
{
    static constexpr Param kParams[] {
        { "leftToLeft", [](ChannelMixerEffect& e, int, float v) { e.setLeftToLeft(v); } },
        { "rightToLeft", [](ChannelMixerEffect& e, int, float v) { e.setRightToLeft(v); } },
        { "leftToRight", [](ChannelMixerEffect& e, int, float v) { e.setLeftToRight(v); } },
        { "rightToRight", [](ChannelMixerEffect& e, int, float v) { e.setRightToRight(v); } },
        { "midSide", [](ChannelMixerEffect& e, int, float v) { e.setMidSide(std::clamp((int) std::lround(v), 0, 3)); } },
    };
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

    /** How late the whole chain's output is: its nodes' latencies added up. */
    int latencySamples() const noexcept
    {
        int total = 0;
        for (const auto& node : nodes_)
            total += node->latencySamples();
        return total;
    }

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

    /** Points slot @p index's detector at @p key for this block (see
        EffectProcessor::setSidechainInput); clearSidechains first takes
        every slot back to its own input. Audio thread. */
    void setSidechainInput(size_t index, const juce::AudioBuffer<float>* key)
    {
        if (index < nodes_.size())
            nodes_[index]->setSidechainInput(key);
    }

    void clearSidechains()
    {
        for (auto& node : nodes_)
            node->setSidechainInput(nullptr);
    }

private:
    std::vector<std::unique_ptr<EffectProcessor>> nodes_;
};

} // namespace soundsplice::engine
