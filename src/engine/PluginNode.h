#pragma once

#include <memory>

#include "engine/PluginModule.h"

#include "engine/EffectChain.h"

namespace looper::engine
{
/**
    A hosted plugin as one node of a track's effect chain.

    The instance is created on the message thread (see PluginHost) and reaches
    the audio thread inside a chain that is swapped in whole — so instantiation,
    which allocates and loads a binary, never happens under the audio thread.
    Destruction likewise: a retired chain is deleted by the message thread in
    AudioEngine::pump.

    **This is where the engine's no-allocation rule stops being enforceable.**
    Every other node here is code this project controls; a hosted plugin is
    not. Plugins allocate in prepareToPlay (fine, message thread) and some
    allocate or take locks in processBlock (not fine, and not something a host
    can prevent). Stating it is more useful than pretending the discipline
    extends into third-party code.
*/
class PluginNode final : public EffectProcessor
{
public:
    explicit PluginNode(std::unique_ptr<juce::AudioPluginInstance> instance)
        : instance_(std::move(instance))
    {
        // Pre-allocated so processBlock never has to grow it. Insert effects
        // get no MIDI, but processBlock requires a buffer regardless.
        midi_.ensureSize(256);
    }

    ~PluginNode() override
    {
        if (instance_ != nullptr)
            instance_->releaseResources();
    }

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Plugin; }

    void prepare(double sampleRate, int blockSize) override
    {
        if (instance_ == nullptr)
            return;

        // Ask for stereo in/out. A plugin that refuses keeps whatever layout
        // it has; process() below copes with a mismatch rather than assuming.
        instance_->setPlayConfigDetails(2, 2, sampleRate, blockSize);
        instance_->prepareToPlay(sampleRate, blockSize);
        prepared_ = true;
    }

    void process(juce::AudioBuffer<float>& buffer) override
    {
        if (instance_ == nullptr || ! prepared_ || bypassed_)
            return;

        midi_.clear();

        const int pluginChannels = juce::jmax(instance_->getTotalNumInputChannels(),
                                              instance_->getTotalNumOutputChannels());

        // A plugin may want more channels than the track's stereo buffer has.
        // Handing it a buffer that's too small would have it write past the
        // end, so widen into scratch space and copy back what fits.
        if (pluginChannels > buffer.getNumChannels())
        {
            scratch_.setSize(pluginChannels, buffer.getNumSamples(), false, false, true);
            scratch_.clear();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                scratch_.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

            instance_->processBlock(scratch_, midi_);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.copyFrom(ch, 0, scratch_, ch, 0, buffer.getNumSamples());
            return;
        }

        instance_->processBlock(buffer, midi_);
    }

    void setEnabled(bool enabled) override { bypassed_ = ! enabled; }
    void setBypassed(bool shouldBypass) noexcept { bypassed_ = shouldBypass; }

    /** The plugin's own opaque state, for saving into the document. Message
        thread — call it while this node isn't live, or accept that a plugin
        may be mid-block. */
    std::string saveState() const
    {
        if (instance_ == nullptr)
            return {};

        juce::MemoryBlock block;
        instance_->getStateInformation(block);
        return block.toBase64Encoding().toStdString();
    }

    /** Restores state saved by saveState(). Silently ignores a blob the plugin
        rejects — a plugin that changed its own format across versions is its
        own problem, and losing a preset is better than refusing to load the
        project. */
    void restoreState(const std::string& base64)
    {
        if (instance_ == nullptr || base64.empty())
            return;

        juce::MemoryBlock block;
        if (block.fromBase64Encoding(juce::String(base64)) && block.getSize() > 0)
            instance_->setStateInformation(block.getData(), (int) block.getSize());
    }

    juce::AudioPluginInstance* instance() const noexcept { return instance_.get(); }

private:
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    juce::MidiBuffer                           midi_;
    juce::AudioBuffer<float>                   scratch_;
    bool                                       prepared_ = false;
    bool                                       bypassed_ = false;
};

} // namespace looper::engine
