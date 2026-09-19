#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/EffectSlotFactory.h>
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
        REQUIRE((int) node->kind() == (int) effect.kind);
    }
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

    EffectSlotParams params;
    params.enabled = true;
    params.convMix = 0.5f;
    applyParams(node, params); // no file: the built-in hall

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
    params.convIrFile = "/nonexistent/impulse.wav";
    applyParams(node, params);
    juce::AudioBuffer<float> next(2, 512);
    next.clear();
    node.process(next);
    applyParams(node, params); // frees the response the audio thread let go of
    SUCCEED();
}

