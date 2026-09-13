#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Effects.h"

#include "EffectChainPanel.h"

namespace soundsplice
{
/**
    Picks a chain of effects to render into an audio selection.

    Wraps the existing EffectChainPanel rather than building a second effect
    UI. That panel already knows how to add, order, bypass and edit every
    effect kind, and it works against a plain `std::vector<model::EffectSlot>`
    — so pointing it at a scratch chain gets the whole thing for free, and any
    effect added to the app later appears here without being wired up twice.

    Deliberately a scratch chain rather than the selected track's: baking an
    effect into the audio *and* leaving it live on the track would apply it
    twice on playback, which is the confusing trap this design avoids. What's
    chosen here is applied once, to the selected samples, and then it's part
    of the audio.
*/
class ApplyEffectsDialog final : public juce::Component
{
public:
    /** Fired when Apply is pressed, with the chain to render. */
    std::function<void(const std::vector<model::EffectSlot>&)> onApply;
    std::function<void()>                                      onCancel;

    /** Preset saving and deleting, passed through from the chain panel: the
        owner holds the preset library (see EffectChainPanel). */
    std::function<void(const model::EffectSlot& slot)>                        onPresetSaveRequested;
    std::function<void(const std::string& effectId, const std::string& name)> onUserPresetDeleted;

    void setUserPresets(std::vector<model::UserEffectPreset> presets)
    {
        chainPanel_.setUserPresets(std::move(presets));
    }

    /** Fired by a plugin slot's Open Plugin Editor, with the slot as it
        stands. This chain isn't playing anywhere, so the owner opens an
        editor on an instance of its own and hands the edited settings back
        through setPluginState. */
    std::function<void(int slotIndex, const model::EffectSlot& slot)> onPluginEditorRequested;

    /** Fired just before a slot is removed or moved, while slot indexes still
        mean what they did, so the owner can hand back any plugin editor's
        settings and close it. */
    std::function<void()> onChainAboutToChange;

    /** Stores a plugin slot's edited state, as model::PluginRef::state. */
    void setPluginState(int slotIndex, const std::string& state)
    {
        if (slotIndex >= 0 && slotIndex < (int) chain_.size()
            && chain_[(size_t) slotIndex].kind == model::EffectKind::Plugin)
            chain_[(size_t) slotIndex].plugin.state = state;
    }

    /** The scanned plugins its Add menu offers. */
    void setAvailablePlugins(std::vector<engine::PluginEntry> plugins)
    {
        chainPanel_.setAvailablePlugins(std::move(plugins));
    }

    /** Fired by Preview, with the chain to hear. The owner renders and plays
        it, or stops a preview already playing. */
    std::function<void(const std::vector<model::EffectSlot>&)> onPreview;

    /** Fired as the dialog goes away, however it was closed (Apply, Cancel,
        Escape or the window's close button), so a preview can't keep playing
        with nothing on screen to stop it. */
    std::function<void()> onDismissed;

    ~ApplyEffectsDialog() override
    {
        if (onDismissed)
            onDismissed();
    }

    ApplyEffectsDialog()
    {
        addAndMakeVisible(chainPanel_);

        chainPanel_.onBuiltInAdded = [this](model::EffectKind kind)
        {
            chain_.push_back(model::makeEffectSlot(kind));
            refresh();
        };

        chainPanel_.onSlotRemoved = [this](int index)
        {
            if (onChainAboutToChange)
                onChainAboutToChange();

            if (index >= 0 && index < (int) chain_.size())
                chain_.erase(chain_.begin() + index);
            refresh();
        };

        chainPanel_.onSlotMoved = [this](int index, int delta)
        {
            if (onChainAboutToChange)
                onChainAboutToChange();

            const int target = index + delta;
            if (index >= 0 && index < (int) chain_.size()
                && target >= 0 && target < (int) chain_.size())
                std::swap(chain_[(size_t) index], chain_[(size_t) target]);
            refresh();
        };

        chainPanel_.onSlotBypassToggled = [this](int index, bool enabled)
        {
            if (index >= 0 && index < (int) chain_.size())
                chain_[(size_t) index].enabled = enabled;
            refresh();
        };

        chainPanel_.onSlotParamsChanged = [this](const model::EffectSlot& slot, int index)
        {
            if (index >= 0 && index < (int) chain_.size())
                chain_[(size_t) index] = slot;
        };

        // Read on demand rather than reacted to — stated explicitly so a
        // control nobody listens to stays distinguishable from one nobody
        // remembered to wire (see tests/gui/PaneAudit.h).
        chainPanel_.onSlotSelected          = [](int) {};
        chainPanel_.onPluginEditorRequested = [this](int index)
        {
            if (index >= 0 && index < (int) chain_.size() && onPluginEditorRequested)
                onPluginEditorRequested(index, chain_[(size_t) index]);
        };
        chainPanel_.onScanRequested         = [] {};
        chainPanel_.onSlotParamsDragStart   = [](int) {};
        chainPanel_.onSlotParamsDragEnd     = [](int) {};
        chainPanel_.onPluginAdded = [this](const engine::PluginEntry& entry)
        {
            auto slot = model::makeEffectSlot(model::EffectKind::Plugin);
            slot.plugin.format     = entry.format == "VST3"      ? model::PluginFormat::VST3
                                   : entry.format == "AudioUnit" ? model::PluginFormat::AudioUnit
                                                                 : model::PluginFormat::Unknown;
            slot.plugin.identifier = entry.identifier;
            slot.plugin.name       = entry.name;
            chain_.push_back(slot);
            refresh();
        };

        chainPanel_.onPresetSaveRequested = [this](const model::EffectSlot& slot, int)
        {
            if (onPresetSaveRequested)
                onPresetSaveRequested(slot);
        };
        chainPanel_.onUserPresetDeleted = [this](const std::string& effectId, const std::string& name)
        {
            if (onUserPresetDeleted)
                onUserPresetDeleted(effectId, name);
        };

        applyButton_.setButtonText("Apply to Selection");
        applyButton_.onClick = [this] { if (onApply) onApply(chain_); };
        addAndMakeVisible(applyButton_);

        previewButton_.setButtonText("Preview");
        previewButton_.setTooltip("Hear the selection through these effects without changing it - press again to stop");
        previewButton_.onClick = [this] { if (onPreview) onPreview(chain_); };
        addAndMakeVisible(previewButton_);

        cancelButton_.setButtonText("Cancel");
        cancelButton_.onClick = [this] { if (onCancel) onCancel(); };
        addAndMakeVisible(cancelButton_);

        hintLabel_.setText("Effects are rendered into the selected audio, not left on the track.",
                           juce::dontSendNotification);
        hintLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        hintLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        hintLabel_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(hintLabel_);

        refresh();
    }

    const std::vector<model::EffectSlot>& chain() const { return chain_; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        auto buttons = area.removeFromBottom(kButtonRowHeight);
        cancelButton_.setBounds(buttons.removeFromRight(juce::jmax(1, buttons.getWidth() / 3)).reduced(2));
        applyButton_.setBounds(buttons.removeFromRight(juce::jmax(1, buttons.getWidth() / 2)).reduced(2));
        previewButton_.setBounds(buttons.reduced(2));

        hintLabel_.setBounds(area.removeFromBottom(kHintHeight));
        chainPanel_.setBounds(area);
    }

private:
    static constexpr int kButtonRowHeight = 30;
    static constexpr int kHintHeight      = 18;

    void refresh() { chainPanel_.setChain(chain_); }

    std::vector<model::EffectSlot> chain_;

    EffectChainPanel chainPanel_;
    juce::TextButton previewButton_, applyButton_, cancelButton_;
    juce::Label      hintLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ApplyEffectsDialog)
};

} // namespace soundsplice
