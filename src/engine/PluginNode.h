#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/PluginModule.h"

#include "engine/EffectChain.h"

namespace soundsplice::engine
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

        // Automatable parameters by the plugin's own id, which (unlike the
        // index) is meant to survive a plugin update. Built here so the audio
        // thread only ever searches it.
        if (instance_ != nullptr)
            for (int i = 0; i < instance_->getParameters().size(); ++i)
                if (auto* param = instance_->getHostedParameter(i); param != nullptr && param->isAutomatable())
                    params_.push_back({ param->getParameterID().toStdString(), param, param->getValue() });
    }

    /** One automatable parameter, as the automation pane lists it. */
    struct ParameterInfo
    {
        std::string id;
        std::string name;
        std::string lowest, highest; // the plugin's own text at 0 and 1
        float       value = 0.0f;    // now, 0..1
    };

    /** Message thread. Empty for a plugin that didn't load. */
    std::vector<ParameterInfo> parameters() const
    {
        std::vector<ParameterInfo> infos;
        for (const auto& entry : params_)
            infos.push_back({ entry.id, entry.param->getName(64).toStdString(),
                              entry.param->getText(0.0f, 32).toStdString(),
                              entry.param->getText(1.0f, 32).toStdString(), entry.param->getValue() });
        return infos;
    }

    /** Automation: a parameter's normalised 0..1 value, by the plugin's id.
        Only passed on when it changes, since a plugin may do real work per
        change and a held lane repeats itself every block. */
    bool setParam(std::string_view id, float value) override
    {
        for (auto& entry : params_)
            if (entry.id == id)
            {
                value = juce::jlimit(0.0f, 1.0f, value);
                if (value != entry.lastSet)
                {
                    entry.lastSet = value;
                    entry.param->setValue(value);
                }
                return true;
            }
        return false;
    }

    ~PluginNode() override
    {
        if (instance_ != nullptr)
            instance_->releaseResources();
    }

    EffectKind kind() const noexcept override { return EffectKind::Plugin; }

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
    struct Param
    {
        std::string                    id;
        juce::AudioProcessorParameter* param   = nullptr;
        float                          lastSet = -1.0f;
    };

    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::vector<Param>                         params_;
    juce::MidiBuffer                           midi_;
    juce::AudioBuffer<float>                   scratch_;
    bool                                       prepared_ = false;
    bool                                       bypassed_ = false;
};

} // namespace soundsplice::engine
