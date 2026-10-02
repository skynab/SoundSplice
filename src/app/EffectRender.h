#pragma once

#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/EffectChain.h"
#include "engine/EffectSlotFactory.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "model/Effects.h"

namespace soundsplice
{
/** What rendering a chain came to. On failure nothing was rendered, and
    @c error says which plugin wouldn't load. */
struct EffectRenderResult
{
    bool         ok = true;
    juce::String error;
};

/** Runs @p chain's enabled slots, built-ins and hosted plugins, over
    @p block in place. Shared by Apply and Preview, so a preview is exactly
    what Apply will write.

    Each plugin slot gets its own instance, with the slot's saved state
    restored, separate from any copy playing live on a track. The chain runs
    in fixed-size blocks, because a plugin prepared for a block size can't be
    handed a whole selection in one call. The chain's reported latency is
    rendered past the end and read back into place, so a plugin that delays
    its output doesn't shift the audio late.

    All-or-nothing: if any plugin can't be loaded, nothing is rendered. Half a
    chain applied to the audio would be worse than an honest refusal. */
inline EffectRenderResult renderEffectChain(const std::vector<model::EffectSlot>& chain,
                                            juce::AudioBuffer<float>& block, double sampleRate, double bpm,
                                            engine::PluginHost& plugins)
{
    constexpr int kRenderBlockSize = 512;

    engine::EffectChain built;
    for (const auto& slot : chain)
    {
        if (! slot.enabled)
            continue;

        if (slot.kind == model::EffectKind::Plugin)
        {
            std::string error;
            auto instance = plugins.createInstance(model::pluginFormatName(slot.plugin.format), slot.plugin.identifier,
                                                   sampleRate, kRenderBlockSize, &error);
            if (instance == nullptr)
            {
                const juce::String name = slot.plugin.name.empty() ? juce::String("A plugin")
                                                                   : "\"" + juce::String(slot.plugin.name) + "\"";
                return { false, name + " couldn't be loaded"
                                    + (error.empty() ? juce::String() : ": " + juce::String(error)) };
            }

            auto node = std::make_unique<engine::PluginNode>(std::move(instance));
            node->restoreState(slot.plugin.state);
            node->setEnabled(true);
            built.add(std::move(node));
            continue;
        }

        if (auto node = engine::makeConfiguredNode(slot))
            built.add(std::move(node));
    }

    built.prepare(sampleRate, kRenderBlockSize);
    built.setBpm(bpm); // the wobble pedal is tempo-locked

    // Only known once prepared: a plugin may report a different latency for
    // a different rate or block size. The whole chain's, so a limiter's
    // lookahead is taken out as a plugin's is.
    const int latency = built.latencySamples();

    const int numChannels = block.getNumChannels();
    const int length      = block.getNumSamples();

    juce::AudioBuffer<float> work(numChannels, length + latency);
    work.clear();
    for (int ch = 0; ch < numChannels; ++ch)
        work.copyFrom(ch, 0, block, ch, 0, length);

    for (int start = 0; start < work.getNumSamples(); start += kRenderBlockSize)
    {
        const int count = juce::jmin(kRenderBlockSize, work.getNumSamples() - start);
        juce::AudioBuffer<float> view(work.getArrayOfWritePointers(), numChannels, start, count);
        built.process(view);
    }

    for (int ch = 0; ch < numChannels; ++ch)
        block.copyFrom(ch, 0, work, ch, latency, length);

    return {};
}

} // namespace soundsplice
