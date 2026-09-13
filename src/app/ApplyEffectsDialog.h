#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Effects.h"

#include "EffectChainPanel.h"

namespace looper
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

    ApplyEffectsDialog()
    {
        addAndMakeVisible(chainPanel_);

        chainPanel_.onBuiltInAdded = [this](model::EffectKind kind)
        {
            model::EffectSlot slot;
            slot.kind    = kind;
            slot.enabled = true;
            setEnabledFlagForKind(slot);
            chain_.push_back(slot);
            refresh();
        };

        chainPanel_.onSlotRemoved = [this](int index)
        {
            if (index >= 0 && index < (int) chain_.size())
                chain_.erase(chain_.begin() + index);
            refresh();
        };

        chainPanel_.onSlotMoved = [this](int index, int delta)
        {
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
        // remembered to wire (see FretboardPane's identical note).
        chainPanel_.onSlotSelected          = [](int) {};
        chainPanel_.onPluginEditorRequested = [](int) {};
        chainPanel_.onScanRequested         = [] {};
        chainPanel_.onSlotParamsDragStart   = [](int) {};
        chainPanel_.onSlotParamsDragEnd     = [](int) {};
        chainPanel_.onPluginAdded           = [](const engine::PluginEntry&) {};

        applyButton_.setButtonText("Apply to Selection");
        applyButton_.onClick = [this] { if (onApply) onApply(chain_); };
        addAndMakeVisible(applyButton_);

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

        hintLabel_.setBounds(area.removeFromBottom(kHintHeight));
        chainPanel_.setBounds(area);
    }

private:
    static constexpr int kButtonRowHeight = 30;
    static constexpr int kHintHeight      = 18;

    /** EffectChainPanel drives `slot.<kind>.enabled` from the per-kind
        struct, not from `slot.enabled` — the two are set together everywhere
        else in this app (see GenrePresets and the tone tables), and a slot
        added with only the outer flag set would render silently as a no-op. */
    static void setEnabledFlagForKind(model::EffectSlot& slot)
    {
        switch (slot.kind)
        {
            case model::EffectKind::Filter:     slot.filter.enabled = true; break;
            case model::EffectKind::Delay:      slot.delay.enabled = true; break;
            case model::EffectKind::Reverb:     slot.reverb.enabled = true; break;
            case model::EffectKind::Drive:      slot.drive.enabled = true; break;
            case model::EffectKind::Compressor: slot.compressor.enabled = true; break;
            case model::EffectKind::Tremolo:    slot.tremolo.enabled = true; break;
            case model::EffectKind::Chorus:     slot.chorus.enabled = true; break;
            case model::EffectKind::Wobble:     slot.wobble.enabled = true; break;
            case model::EffectKind::Gate:       slot.gate.enabled = true; break;
            case model::EffectKind::Eq:         slot.eqPedal.enabled = true; break;
            case model::EffectKind::Plugin:     break; // no flag of its own
        }
    }

    void refresh() { chainPanel_.setChain(chain_); }

    std::vector<model::EffectSlot> chain_;

    EffectChainPanel chainPanel_;
    juce::TextButton applyButton_, cancelButton_;
    juce::Label      hintLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ApplyEffectsDialog)
};

} // namespace looper
