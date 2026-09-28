#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/EffectSlotFactory.h>
#include <engine/InstrumentTrack.h>
#include <engine/PluginNode.h>
#include <model/EffectParams.h>

#include <algorithm>
#include <cmath>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr int kBlock  = 256;
    constexpr int kFrames = kBlock * 188; // about a second, in whole blocks

    /** Sample @p at of the test signal: a 220 Hz sine on the left, inverted on the right. */
    float signal(int channel, int at, float amplitude)
    {
        const auto value = amplitude * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * at / 48000.0);
        return channel == 0 ? value : -value;
    }

    /** @p slot's node, run over kFrames of the test signal in kBlock blocks.
        Returns the last block out. */
    juce::AudioBuffer<float> run(const model::EffectSlot& slot, float amplitude)
    {
        auto node = engine::makeConfiguredNode(slot);
        REQUIRE(node != nullptr);
        node->prepare(48000.0, kBlock);

        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int start = 0; start < kFrames; start += kBlock)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < kBlock; ++n)
                    buffer.setSample(ch, n, signal(ch, start + n, amplitude));
            node->process(buffer);
        }
        return buffer;
    }

    /** What run() fed the node in its last block. */
    float lastInput(int channel, int n, float amplitude)
    {
        return signal(channel, kFrames - kBlock + n, amplitude);
    }
}

TEST_CASE("Every built-in effect kind builds a node of that kind", "[gui][effects]")
{
    for (const auto& effect : model::builtInEffects())
    {
        INFO(effect.name);
        const auto node = engine::makeConfiguredNode(model::makeEffectSlot(effect.kind));
        REQUIRE(node != nullptr);
        REQUIRE(node->kind() == effect.kind);
    }
}

TEST_CASE("Every node reads exactly the parameters its descriptor names", "[gui][effects]")
{
    // What stands in for a struct field the compiler would check: a parameter
    // the node never reads is a control that does nothing, and a read by an id
    // the descriptor doesn't have is a setting stuck at zero.
    for (const auto& effect : model::builtInEffects())
    {
        INFO(effect.name);
        const auto values = model::effectParamValues(model::makeEffectSlot(effect.kind));
        const auto node   = engine::makeBuiltInNode(effect.kind);
        REQUIRE(node != nullptr);
        node->applyParams(values);

        for (const auto& id : values.unreadIds())
            FAIL_CHECK("never read: " << id);
        for (const auto& id : values.missingIds())
            FAIL_CHECK("no such parameter: " << id);
    }
}

TEST_CASE("Every parameter can be set on its own, as automation sets it", "[gui][effects]")
{
    for (const auto& effect : model::builtInEffects())
    {
        INFO(effect.name);
        const auto node = engine::makeBuiltInNode(effect.kind);
        for (const auto& param : effect.params)
        {
            INFO(param.id);
            REQUIRE(node->setParam(param.id, (float) param.max));
        }
        REQUIRE_FALSE(node->setParam("noSuchParameter", 1.0f));
        REQUIRE_FALSE(node->setParam("band0", 1.0f));
        REQUIRE_FALSE(node->setParam("band99", 1.0f));
    }
}

TEST_CASE("Setting one band of the parametric EQ leaves the others alone", "[gui][effects]")
{
    // A flat EQ, then one band's gain automated up: only that band may move,
    // which a whole-band setter fed stale values would get wrong.
    auto slot = model::makeEffectSlot(model::EffectKind::ParametricEq);
    auto node = engine::makeConfiguredNode(slot);
    node->prepare(48000.0, kBlock);
    REQUIRE(node->setParam("band3Gain", 12.0f)); // the 400 Hz bell

    juce::AudioBuffer<float> buffer(2, kBlock);
    float                    peak = 0.0f;
    for (int start = 0; start < kFrames; start += kBlock)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < kBlock; ++n)
                buffer.setSample(ch, n, 0.1f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 400.0
                                                                   * (start + n) / 48000.0));
        node->process(buffer);
        if (start > kFrames / 2)
            peak = std::max(peak, buffer.getMagnitude(0, 0, kBlock));
    }
    REQUIRE_THAT(peak, WithinAbs(0.1f * juce::Decibels::decibelsToGain(12.0f), 0.02f));
}

TEST_CASE("Amplify and Invert change level and polarity exactly", "[gui][effects]")
{
    auto amplify           = model::makeEffectSlot(model::EffectKind::Amplify);
    amplify.amplify.gainDb = 6.0f;

    const auto louder = run(amplify, 0.25f);
    const auto gain   = juce::Decibels::decibelsToGain(6.0f);
    for (int n = 0; n < kBlock; ++n)
        REQUIRE_THAT(louder.getSample(0, n), WithinAbs(lastInput(0, n, 0.25f) * gain, 1.0e-6));

    auto invert         = model::makeEffectSlot(model::EffectKind::Invert);
    invert.invert.right = false;

    const auto flipped = run(invert, 0.25f);
    for (int n = 0; n < kBlock; ++n)
    {
        REQUIRE(flipped.getSample(0, n) == -lastInput(0, n, 0.25f));
        REQUIRE(flipped.getSample(1, n) == lastInput(1, n, 0.25f));
    }
}

TEST_CASE("The limiter holds its ceiling however hard it's driven", "[gui][effects]")
{
    auto limiter                = model::makeEffectSlot(model::EffectKind::Limiter);
    limiter.limiter.inputGainDb = 18.0f;
    limiter.limiter.ceilingDb   = -3.0f;

    const auto out     = run(limiter, 0.9f);
    const auto ceiling = juce::Decibels::decibelsToGain(-3.0f);
    const auto peak    = std::max(out.getMagnitude(0, 0, kBlock), out.getMagnitude(1, 0, kBlock));

    REQUIRE(peak <= ceiling + 1.0e-6f);
    REQUIRE(peak > ceiling * 0.9f); // driven up to it, not just turned down
}

TEST_CASE("A bypassed utility effect leaves the audio alone", "[gui][effects]")
{
    for (auto kind : { model::EffectKind::Amplify, model::EffectKind::Invert, model::EffectKind::DcOffset,
                       model::EffectKind::Limiter })
    {
        auto slot           = model::makeEffectSlot(kind);
        slot.enabled        = false;
        slot.amplify.gainDb = 12.0f;

        const auto out = run(slot, 0.5f);
        for (int n = 0; n < kBlock; ++n)
            REQUIRE(out.getSample(0, n) == lastInput(0, n, 0.5f));
    }
}

TEST_CASE("The convolution reverb adds its tail a block late and swaps responses safely", "[gui][effects][convolution]")
{
    using namespace soundsplice::engine;

    ConvolutionNode node;
    node.prepare(48000.0, 512);

    EffectParamValues params;
    params.enabled = true;
    params.set("mix", 0.5);
    params.set("preDelay", 0.0);
    params.set("gain", 0.0);
    params.setText("irFile", "");
    node.applyParams(params); // no file: the built-in hall

    // A click, then silence: half of it dry at once, the wet tail only from a
    // block on, and still going a good while after.
    juce::AudioBuffer<float> buffer(2, 48000);
    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);
    for (int start = 0; start < buffer.getNumSamples(); start += 512)
    {
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, start,
                                       std::min(512, buffer.getNumSamples() - start));
        node.process(block);
    }

    REQUIRE(buffer.getSample(0, 0) == 0.5f);
    for (int n = 1; n < Convolver::kBlock; ++n)
        REQUIRE(buffer.getSample(0, n) == 0.0f);
    float tail = 0.0f;
    for (int n = 24000; n < 26000; ++n)
        tail = std::max(tail, std::abs(buffer.getSample(0, n)));
    REQUIRE(tail > 0.0f);
    REQUIRE(buffer.getSample(0, 3000) != buffer.getSample(1, 3000)); // wide

    // A file that doesn't exist falls back to the hall rather than silence,
    // and the swap happens on the next block without the audio thread waiting.
    params.setText("irFile", "/nonexistent/impulse.wav");
    node.applyParams(params);
    juce::AudioBuffer<float> next(2, 512);
    next.clear();
    node.process(next);
    node.applyParams(params); // frees the response the audio thread let go of
    SUCCEED();
}


TEST_CASE("The channel mixer routes, folds and flips channels, and works in mid/side", "[gui][effects]")
{
    using namespace soundsplice::engine::channelmixer;
    const auto run = [](Matrix m, float l, float r)
    {
        process(l, r, m);
        return std::pair { l, r };
    };

    REQUIRE(run({}, 0.3f, -0.2f) == std::pair { 0.3f, -0.2f });                       // unchanged
    REQUIRE(run({ 0, 1, 1, 0 }, 0.3f, -0.2f) == std::pair { -0.2f, 0.3f });            // swapped
    const auto mono = run({ 0.5f, 0.5f, 0.5f, 0.5f }, 0.3f, -0.1f);
    REQUIRE_THAT(mono.first, WithinAbs(0.1, 1e-6));
    REQUIRE(mono.first == mono.second);
    REQUIRE(run({ 1, 0, 0, -1 }, 0.3f, 0.2f) == std::pair { 0.3f, -0.2f });            // right inverted

    // Encode then decode is a round trip; mixing as M/S with the side up
    // widens, the mid kept.
    const auto encoded = run({ 1, 0, 0, 1, MidSide::Encode }, 0.5f, 0.1f);
    REQUIRE_THAT(encoded.first, WithinAbs(0.3, 1e-6));  // mid
    REQUIRE_THAT(encoded.second, WithinAbs(0.2, 1e-6)); // side
    const auto decoded = run({ 1, 0, 0, 1, MidSide::Decode }, encoded.first, encoded.second);
    REQUIRE_THAT(decoded.first, WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(decoded.second, WithinAbs(0.1, 1e-6));

    const auto wider = run({ 1, 0, 0, 1.5f, MidSide::Around }, 0.5f, 0.1f);
    REQUIRE_THAT((wider.first + wider.second) * 0.5, WithinAbs(0.3, 1e-6));  // mid kept
    REQUIRE_THAT((wider.first - wider.second) * 0.5, WithinAbs(0.3, 1e-6));  // side 0.2 x 1.5
}

TEST_CASE("Effect automation sets its parameter each block, and only on its own kind", "[gui][effects]")
{
    using namespace soundsplice::engine;

    EffectChain chain;
    chain.add(makeBuiltInNode(EffectKind::Filter));
    chain.add(makeBuiltInNode(EffectKind::Amplify));
    chain.applyParams(1, model::effectParamValues(model::makeEffectSlot(model::EffectKind::Amplify), true));
    chain.prepare(48000.0, kBlock);

    // Amplify's gain ramps from 0 to 12 dB over four beats.
    TrackAutomation automation;
    EffectParamCurve gain { 1, EffectKind::Amplify, "gain", {} };
    gain.curve.addPoint(0.0, 0.0f);
    gain.curve.addPoint(4.0, 12.0f);
    automation.effects.push_back(gain);

    // A curve meant for another kind at the same slot is ignored.
    EffectParamCurve stale { 1, EffectKind::Filter, "cutoff", {} };
    stale.curve.addPoint(0.0, 20.0f);
    automation.effects.push_back(stale);

    const auto levelAt = [&](double beat)
    {
        applyEffectAutomation(chain, automation, beat);
        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < kBlock; ++n)
                buffer.setSample(ch, n, 0.25f);
        chain.process(buffer);
        return buffer.getSample(0, kBlock - 1);
    };

    // The filter at slot 0 was never enabled, so it passes the signal through.
    // Amplify glides to a new gain, so each level is read once it's settled.
    const auto settledAt = [&](double beat)
    {
        for (int i = 0; i < 50; ++i)
            levelAt(beat);
        return levelAt(beat);
    };

    REQUIRE_THAT(settledAt(0.0), WithinAbs(0.25f, 1.0e-3));
    REQUIRE_THAT(settledAt(2.0), WithinAbs(0.25f * juce::Decibels::decibelsToGain(6.0f), 1.0e-3));
    REQUIRE_THAT(settledAt(4.0), WithinAbs(0.25f * juce::Decibels::decibelsToGain(12.0f), 1.0e-3));
}

namespace
{
    /** Just enough of a hosted plugin: one gain parameter, applied. */
    struct FakePlugin final : juce::AudioPluginInstance
    {
        juce::AudioParameterFloat* gain = nullptr;

        FakePlugin()
        {
            auto parameter = std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 0.5f);
            gain           = parameter.get();
            addHostedParameter(std::move(parameter));
        }

        void fillInPluginDescription(juce::PluginDescription&) const override {}
        const juce::String getName() const override { return "Fake"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain(gain->get()); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}
    };
}

TEST_CASE("A plugin's parameters are listed by its own id and automated through it", "[gui][effects]")
{
    using namespace soundsplice::engine;

    auto  plugin = std::make_unique<FakePlugin>();
    auto* gain   = plugin->gain;
    PluginNode node(std::move(plugin));

    const auto params = node.parameters();
    REQUIRE(params.size() == 1);
    REQUIRE(params[0].id == "gain");
    REQUIRE(params[0].name == "Gain");
    REQUIRE(params[0].value == 0.5f);

    REQUIRE(node.setParam("gain", 0.25f));
    REQUIRE(gain->get() == 0.25f);
    REQUIRE(node.setParam("gain", 3.0f)); // held to the normalised range
    REQUIRE(gain->get() == 1.0f);
    REQUIRE_FALSE(node.setParam("volume", 0.5f));

    // Through the chain, as the track does it each block.
    EffectChain chain;
    chain.add(std::make_unique<PluginNode>(nullptr)); // a plugin that didn't load keeps its place
    chain.add(std::make_unique<PluginNode>(std::make_unique<FakePlugin>()));
    TrackAutomation automation;
    EffectParamCurve curve { 1, EffectKind::Plugin, "gain", {} };
    curve.curve.addPoint(0.0, 0.0f);
    curve.curve.addPoint(4.0, 1.0f);
    automation.effects.push_back(curve);

    applyEffectAutomation(chain, automation, 1.0);
    auto* hosted = dynamic_cast<PluginNode*>(chain.nodeAt(1));
    REQUIRE(hosted->parameters()[0].value == 0.25f);
    REQUIRE(dynamic_cast<PluginNode*>(chain.nodeAt(0))->parameters().empty());
}
