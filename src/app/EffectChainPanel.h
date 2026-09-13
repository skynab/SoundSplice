#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LayoutHelpers.h"
#include "engine/PluginHost.h"
#include "model/Effects.h"

namespace looper
{
/**
    The selected track's effect chain: an ordered list of slots, each a
    built-in (filter, delay, reverb) or a hosted plugin, with add, remove,
    reorder and bypass — and the selected slot's parameters below.

    Replaces the fixed filter/delay/reverb panel that came before it. That one
    could only ever edit one of each, which stopped being true the moment a
    chain could hold two filters or a plugin.

    Owns no document state: it draws from a snapshot and reports intent through
    the callbacks, like ArrangementView and SessionView.
*/
class EffectChainPanel final : public juce::Component
{
public:
    std::function<void(int slotIndex)>                              onSlotSelected;
    std::function<void(int slotIndex, bool enabled)>                onSlotBypassToggled;
    std::function<void(int slotIndex)>                              onSlotRemoved;
    std::function<void(int slotIndex, int delta)>                   onSlotMoved;   // -1 up, +1 down
    std::function<void(model::EffectKind kind)>                     onBuiltInAdded;
    std::function<void(const engine::PluginEntry&)>                 onPluginAdded;
    std::function<void(int slotIndex)>                              onPluginEditorRequested;
    std::function<void()>                                           onScanRequested;
    std::function<void(const model::EffectSlot& slot, int slotIndex)> onSlotParamsChanged;

    /** Brackets a change to the selected slot's parameters, so the owner can
        commit the whole thing as one undo step (see MainComponent's
        beginEffectSlotParamsDrag/endEffectSlotParamsDrag) rather than one
        step per notch — the same problem the mixer faders solve, but for a
        whole struct of fields rather than one number. A slider spans a real
        drag; a toggle or dropdown fires both back to back, since a click has
        no "during" to span. */
    std::function<void(int slotIndex)> onSlotParamsDragStart;
    std::function<void(int slotIndex)> onSlotParamsDragEnd;

    EffectChainPanel()
    {
        placeholder_.setText("Select a track to edit its effects", juce::dontSendNotification);
        placeholder_.setJustificationType(juce::Justification::centred);
        placeholder_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholder_);

        addButton_.setButtonText("+ Add");
        addButton_.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(addButton_);

        removeButton_.setButtonText("Remove");
        removeButton_.onClick = [this]
        {
            if (onSlotRemoved && isValidSlot(selected_))
                onSlotRemoved(selected_);
        };
        addAndMakeVisible(removeButton_);

        upButton_.setButtonText("Up");
        upButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, -1); };
        addAndMakeVisible(upButton_);

        downButton_.setButtonText("Down");
        downButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, +1); };
        addAndMakeVisible(downButton_);

        editorButton_.setButtonText("Open Plugin Editor");
        editorButton_.onClick = [this]
        {
            if (onPluginEditorRequested && isValidSlot(selected_))
                onPluginEditorRequested(selected_);
        };
        addChildComponent(editorButton_);

        setupSlider(cutoff_, 20.0, 18000.0, 1.0, " Hz", [this] { pushParams(); });
        cutoff_.setSkewFactorFromMidPoint(1000.0);
        setupSlider(resonance_, 0.1, 5.0, 0.01, " Q", [this] { pushParams(); });
        setupSlider(timeMs_, 20.0, 1000.0, 1.0, " ms", [this] { pushParams(); });
        setupSlider(feedback_, 0.0, 95.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(mix_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(roomSize_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(damping_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });

        setupSlider(driveAmount_, 1.0, 40.0, 0.1, " x", [this] { pushParams(); });
        setupSlider(driveTone_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(driveLevel_, 0.0, 150.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(driveAsymmetry_, -100.0, 100.0, 1.0, " %", [this] { pushParams(); });
        driveAsymmetry_.setTooltip("Bias into the clipper - adds the even harmonics a symmetric curve can't make");

        driveHardClip_.setButtonText("Fuzz (hard clip)");
        driveHardClip_.onClick = [this] { reportInstantEdit(); };
        addChildComponent(driveHardClip_);

        driveCabinet_.setButtonText("Cabinet");
        driveCabinet_.setTooltip("Speaker simulation - without it, distortion is heard as fizz");
        driveCabinet_.onClick = [this] { reportInstantEdit(); };
        addChildComponent(driveCabinet_);

        driveOversample_.setButtonText("Oversample (4x)");
        driveOversample_.setTooltip("Runs the clipper at 4x - costs CPU, removes the aliasing grit of high gain");
        driveOversample_.onClick = [this] { reportInstantEdit(); };
        addChildComponent(driveOversample_);

        setupSlider(eqLowShelfHz_, 20.0, 1000.0, 1.0, " Hz", [this] { pushParams(); });
        setupSlider(eqLowShelfDb_, -24.0, 24.0, 0.5, " dB", [this] { pushParams(); });
        setupSlider(eqMidHz_, 100.0, 8000.0, 1.0, " Hz", [this] { pushParams(); });
        setupSlider(eqMidDb_, -24.0, 24.0, 0.5, " dB", [this] { pushParams(); });
        eqMidDb_.setTooltip("The mid scoop or push - the EQ decision a rock or metal tone turns on");
        setupSlider(eqMidQ_, 0.2, 8.0, 0.05, "", [this] { pushParams(); });
        setupSlider(eqHighShelfHz_, 1000.0, 16000.0, 10.0, " Hz", [this] { pushParams(); });
        setupSlider(eqHighShelfDb_, -24.0, 24.0, 0.5, " dB", [this] { pushParams(); });

        setupSlider(compThreshold_, -60.0, 0.0, 0.5, " dB", [this] { pushParams(); });
        setupSlider(compRatio_, 1.0, 20.0, 0.1, " :1", [this] { pushParams(); });
        setupSlider(compAttack_, 0.5, 200.0, 0.5, " ms", [this] { pushParams(); });
        setupSlider(compRelease_, 10.0, 1000.0, 1.0, " ms", [this] { pushParams(); });
        setupSlider(compMakeUp_, -12.0, 24.0, 0.5, " dB", [this] { pushParams(); });

        setupSlider(tremRate_, 0.1, 20.0, 0.1, " Hz", [this] { pushParams(); });
        setupSlider(tremDepth_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });

        setupSlider(chorusRate_, 0.05, 8.0, 0.05, " Hz", [this] { pushParams(); });
        setupSlider(chorusDepth_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(chorusMix_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });

        // Rate is beats-per-cycle, not Hz: 0.25 is a sixteenth note, 1.0 a
        // quarter — the values a wobble is actually dialled in as, so it
        // stays locked to the bar as the song's tempo changes.
        setupSlider(wobbleRate_, 0.0625, 4.0, 0.0625, " beats", [this] { pushParams(); });
        setupSlider(wobbleDepth_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(wobbleCutoff_, 40.0, 4000.0, 1.0, " Hz", [this] { pushParams(); });
        wobbleCutoff_.setSkewFactorFromMidPoint(400.0);
        setupSlider(wobbleResonance_, 0.1, 5.0, 0.01, " Q", [this] { pushParams(); });
        setupSlider(wobbleMix_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });

        setupSlider(gateThreshold_, -80.0, 0.0, 0.5, " dB", [this] { pushParams(); });
        setupSlider(gateRange_, 0.0, 90.0, 1.0, " dB", [this] { pushParams(); });
        setupSlider(gateAttack_, 0.1, 50.0, 0.1, " ms", [this] { pushParams(); });
        setupSlider(gateHold_, 0.0, 500.0, 1.0, " ms", [this] { pushParams(); });
        setupSlider(gateRelease_, 1.0, 2000.0, 1.0, " ms", [this] { pushParams(); });

        filterMode_.addItem("Low-pass", 1);
        filterMode_.addItem("High-pass", 2);
        filterMode_.addItem("Band-pass", 3);
        filterMode_.setSelectedId(1, juce::dontSendNotification);
        filterMode_.onChange = [this] { reportInstantEdit(); };
        addAndMakeVisible(filterMode_);

        compSidechain_.onChange = [this] { reportInstantEdit(); };
        addAndMakeVisible(compSidechain_);

        // Laying out, showing and hiding a component that was never parented
        // all succeed silently and draw nothing — which is exactly how the
        // drive pedal shipped invisible. So parenting is taken from the same
        // list the show/hide code uses, rather than trusting each control's
        // own setup to have done it: addChildComponent is a no-op for a
        // component that is already a child, so this can only ever fix the
        // case that was broken.
        //
        // The assertion is still worth having in a debug build. It fires
        // earlier and says something happened, where the loop alone silently
        // repairs it — and a control that reaches here unparented is also a
        // control whose range and callback were never set, which this cannot
        // fix. It would at least be visible rather than absent.
        for (auto* control : paramControls())
        {
            jassert(getIndexOfChildComponent(control) >= 0);
            addChildComponent(control);
        }

        setContentVisible(false);
    }

    /** The scanned plugins offered by the Add menu. */
    void setAvailablePlugins(std::vector<engine::PluginEntry> plugins) { plugins_ = std::move(plugins); }

    /**
        The tracks a compressor here can take its detector from: (id, name)
        pairs, excluding the track this chain belongs to — a track ducking
        itself is just an ordinary compressor, and offering it would be
        offering a no-op.

        Rebuilt whenever the selection or the track list changes, because a
        track that has been renamed, added or deleted has to be reflected here;
        the *selected* value is preserved across the rebuild, so repopulating
        the list can't silently unset someone's routing.
    */
    void setSidechainSources(const std::vector<std::pair<int, juce::String>>& tracks)
    {
        const int previous = compSidechain_.getSelectedId();

        compSidechain_.clear(juce::dontSendNotification);
        compSidechain_.addItem("This track (no sidechain)", 1);

        for (const auto& [id, name] : tracks)
            compSidechain_.addItem(name, id + 2); // +2: see the member's docs

        // Restores the routing if that track still exists, and falls back to
        // "no sidechain" if it was the one deleted.
        compSidechain_.setSelectedId(previous > 0 ? previous : 1, juce::dontSendNotification);
        if (compSidechain_.getSelectedId() == 0)
            compSidechain_.setSelectedId(1, juce::dontSendNotification);
    }

    void setChain(const std::vector<model::EffectSlot>& chain)
    {
        chain_ = chain;
        selected_ = chain_.empty() ? -1 : juce::jlimit(0, (int) chain_.size() - 1, juce::jmax(0, selected_));
        refreshParamControls();
        setContentVisible(true);
    }

    void setNoTrackSelected() { setContentVisible(false); }

    /** Selects a slot, so a test can walk every effect kind's controls. The
        app selects by clicking the list, which a headless test can't do. */
    void selectSlotForTesting(int index)
    {
        selected_ = index;
        refreshParamControls();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int slot = slotAtY(e.position.y);
        if (slot < 0)
            return;

        // The left-hand strip of a row is its bypass button; the rest selects.
        if (e.position.x < (float) kBypassWidth)
        {
            if (onSlotBypassToggled)
                onSlotBypassToggled(slot, ! chain_[(size_t) slot].enabled);
            return;
        }

        selected_ = slot;
        refreshParamControls();
        repaint();
        resized();
        if (onSlotSelected)
            onSlotSelected(slot);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
        if (! contentVisible_)
            return;

        g.setFont(juce::FontOptions(12.0f));
        for (int i = 0; i < (int) chain_.size(); ++i)
        {
            const auto  row  = rowBounds(i);
            const auto& slot = chain_[(size_t) i];

            g.setColour(i == selected_ ? juce::Colours::white.withAlpha(0.10f)
                                       : juce::Colours::white.withAlpha(0.04f));
            g.fillRect(row);

            if (i == selected_)
            {
                g.setColour(juce::Colours::orange.withAlpha(0.7f));
                g.drawRect(row, 1);
            }

            // Bypass indicator: filled when active, hollow when bypassed —
            // the same meaning for a plugin as for a built-in.
            const auto dot = juce::Rectangle<int>(row.getX() + 8, row.getCentreY() - 5, 10, 10).toFloat();
            g.setColour(slot.enabled ? juce::Colours::limegreen : juce::Colours::white.withAlpha(0.25f));
            slot.enabled ? g.fillEllipse(dot) : g.drawEllipse(dot, 1.2f);

            g.setColour(juce::Colours::white.withAlpha(slot.enabled ? 0.9f : 0.45f));
            g.drawText(slotLabel(slot), row.getX() + kBypassWidth, row.getY(),
                       row.getWidth() - kBypassWidth - 6, row.getHeight(),
                       juce::Justification::centredLeft);
        }

        if (chain_.empty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText("No effects — use + Add", listArea(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        placeholder_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds().reduced(6);

        // Four fixed-width buttons in a row: in a narrow pane the later ones
        // run past the right edge and used to end up zero wide. Dropping the
        // ones that don't fit keeps the rest usable, and they return when the
        // pane is widened. Add is first because it's the one that has to work
        // for the panel to be worth anything.
        auto toolbar = area.removeFromTop(kToolbarHeight);
        setBoundsOrHide(addButton_, toolbar.removeFromLeft(64).reduced(2));
        setBoundsOrHide(removeButton_, toolbar.removeFromLeft(70).reduced(2));
        setBoundsOrHide(upButton_, toolbar.removeFromLeft(44).reduced(2));
        setBoundsOrHide(downButton_, toolbar.removeFromLeft(56).reduced(2));

        area.removeFromTop((int) chain_.size() * kRowHeight + 6);

        // Whatever the selected slot needs: a plugin gets an editor button, a
        // built-in gets its own parameters.
        if (! isValidSlot(selected_))
            return;

        const auto kind = chain_[(size_t) selected_].kind;
        if (kind == model::EffectKind::Plugin)
        {
            setBoundsOrHide(editorButton_, area.removeFromTop(kRowHeight).reduced(2));
            return;
        }

        // A kind with five rows in a short pane ran the last of them off the
        // bottom, leaving them zero-high rather than absent. Only the controls
        // this kind uses reach here — the rest were hidden by
        // refreshParamControls — so setting visibility both ways is safe.
        auto row = [&area](juce::Component& c)
        {
            setBoundsOrHide(c, area.removeFromTop(kRowHeight).reduced(2));
        };
        if (kind == model::EffectKind::Filter) { row(filterMode_); row(cutoff_); row(resonance_); }
        else if (kind == model::EffectKind::Delay) { row(timeMs_); row(feedback_); row(mix_); }
        else if (kind == model::EffectKind::Reverb) { row(roomSize_); row(damping_); row(mix_); }
        else if (kind == model::EffectKind::Drive)
        {
            row(driveAmount_); row(driveTone_); row(driveLevel_); row(driveAsymmetry_);
            row(driveHardClip_); row(driveCabinet_); row(driveOversample_);
        }
        else if (kind == model::EffectKind::Compressor)
        {
            row(compThreshold_); row(compRatio_); row(compAttack_); row(compRelease_); row(compMakeUp_);
            row(compSidechain_);
        }
        else if (kind == model::EffectKind::Tremolo) { row(tremRate_); row(tremDepth_); }
        else if (kind == model::EffectKind::Chorus)
        {
            row(chorusRate_); row(chorusDepth_); row(chorusMix_);
        }
        else if (kind == model::EffectKind::Wobble)
        {
            row(wobbleRate_); row(wobbleDepth_); row(wobbleCutoff_); row(wobbleResonance_); row(wobbleMix_);
        }
        else if (kind == model::EffectKind::Gate)
        {
            row(gateThreshold_); row(gateRange_); row(gateAttack_); row(gateHold_); row(gateRelease_);
        }
        else if (kind == model::EffectKind::Eq)
        {
            row(eqLowShelfHz_); row(eqLowShelfDb_);
            row(eqMidHz_); row(eqMidDb_); row(eqMidQ_);
            row(eqHighShelfHz_); row(eqHighShelfDb_);
        }
    }

private:
    static constexpr int kRowHeight     = 26;
    static constexpr int kToolbarHeight = 26;
    static constexpr int kBypassWidth   = 26;

    juce::Rectangle<int> listArea() const
    {
        auto area = getLocalBounds().reduced(6);
        area.removeFromTop(kToolbarHeight);
        return area;
    }

    juce::Rectangle<int> rowBounds(int index) const
    {
        auto area = listArea();
        return { area.getX(), area.getY() + index * kRowHeight, area.getWidth(), kRowHeight };
    }

    int slotAtY(float y) const
    {
        const auto area = listArea();
        const int  idx  = (int) ((y - (float) area.getY()) / (float) kRowHeight);
        return (y >= (float) area.getY() && idx >= 0 && idx < (int) chain_.size()) ? idx : -1;
    }

    bool isValidSlot(int index) const { return index >= 0 && index < (int) chain_.size(); }

    static juce::String slotLabel(const model::EffectSlot& slot)
    {
        switch (slot.kind)
        {
            case model::EffectKind::Filter: return "Filter";
            case model::EffectKind::Delay:  return "Delay";
            case model::EffectKind::Reverb: return "Reverb";
            case model::EffectKind::Drive:      return "Drive";
            case model::EffectKind::Compressor: return "Compressor";
            case model::EffectKind::Tremolo:    return "Tremolo";
            case model::EffectKind::Chorus:     return "Chorus";
            case model::EffectKind::Wobble:     return "Wobble";
            case model::EffectKind::Gate:       return "Gate";
            case model::EffectKind::Eq:         return "EQ";
            case model::EffectKind::Plugin:
                // A plugin the machine no longer has still names itself, which
                // is the whole reason the document stores the name.
                return slot.plugin.name.empty() ? juce::String("(missing plugin)")
                                                : juce::String(slot.plugin.name);
        }
        return {};
    }

    void showAddMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Filter");
        menu.addItem(2, "Delay");
        menu.addItem(3, "Reverb");

        // Grouped separately from Filter/Delay/Reverb because these read as
        // stompbox-style effects rather than studio processing — not because
        // they're guitar-only. Wobble is the clearest case: it's a bass/synth
        // effect that's essentially never used on a guitar. Presentation
        // only — they all run in the same chain, in whatever order they are
        // put in, on whatever track type.
        juce::PopupMenu pedals;
        pedals.addItem(5, "Drive");
        pedals.addItem(6, "Compressor");
        pedals.addItem(7, "Tremolo");
        pedals.addItem(8, "Chorus");
        pedals.addItem(9, "Wobble");
        pedals.addItem(10, "Gate");
        pedals.addItem(11, "EQ");
        menu.addSubMenu("Pedals", pedals);
        menu.addSeparator();

        if (plugins_.empty())
        {
            menu.addItem(4, "Scan for plugins...");
        }
        else
        {
            juce::PopupMenu pluginMenu;
            for (int i = 0; i < (int) plugins_.size(); ++i)
                pluginMenu.addItem(100 + i, plugins_[(size_t) i].name + "  (" + plugins_[(size_t) i].format + ")");
            pluginMenu.addSeparator();
            pluginMenu.addItem(4, "Rescan...");
            menu.addSubMenu("Plugins", pluginMenu);
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_),
                           [this](int result)
        {
            if (result == 0)
                return;
            if (result == 1 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Filter);
            else if (result == 2 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Delay);
            else if (result == 3 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Reverb);
            else if (result == 5 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Drive);
            else if (result == 6 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Compressor);
            else if (result == 7 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Tremolo);
            else if (result == 8 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Chorus);
            else if (result == 9 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Wobble);
            else if (result == 10 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Gate);
            else if (result == 11 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Eq);
            else if (result == 4 && onScanRequested) onScanRequested();
            else if (result >= 100)
            {
                const size_t index = (size_t) (result - 100);
                if (index < plugins_.size() && onPluginAdded)
                    onPluginAdded(plugins_[index]);
            }
        });
    }

    /** Every parameter control, in one place.

        This list existed twice, verbatim, and that is how a bug shipped: the
        drive controls were added to both copies and to the layout, but
        setupSlider — which is what actually parents them — was never called
        on them. They were laid out, shown and hidden all correctly while
        never being child components at all, so none of it drew anything.

        One definition, plus the assertion in the constructor that checks the
        list against reality, is what makes that unrepresentable rather than
        merely unlikely. */
    std::vector<juce::Component*> paramControls()
    {
        return { &filterMode_, &compSidechain_, &cutoff_, &resonance_, &timeMs_,
                 &feedback_, &mix_, &roomSize_, &damping_, &editorButton_,
                 &driveAmount_, &driveTone_, &driveLevel_,
                 &driveAsymmetry_, &driveHardClip_, &driveCabinet_, &driveOversample_,
                 &compThreshold_, &compRatio_, &compAttack_,
                 &compRelease_, &compMakeUp_, &tremRate_, &tremDepth_,
                 &chorusRate_, &chorusDepth_, &chorusMix_,
                 &wobbleRate_, &wobbleDepth_, &wobbleCutoff_, &wobbleResonance_, &wobbleMix_,
                 &gateThreshold_, &gateRange_, &gateAttack_, &gateHold_, &gateRelease_,
                 &eqLowShelfHz_, &eqLowShelfDb_, &eqMidHz_, &eqMidDb_, &eqMidQ_,
                 &eqHighShelfHz_, &eqHighShelfDb_ };
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        slider.onDragStart = [this] { if (onSlotParamsDragStart) onSlotParamsDragStart(selected_); };
        slider.onDragEnd   = [this] { if (onSlotParamsDragEnd)   onSlotParamsDragEnd(selected_); };
        addChildComponent(slider);
    }

    /** For a discrete control (toggle, dropdown) rather than a slider: a
        click has no "during" to bracket, so both ends of the drag report
        fire back to back around the one edit it makes. */
    void reportInstantEdit()
    {
        if (onSlotParamsDragStart) onSlotParamsDragStart(selected_);
        pushParams();
        if (onSlotParamsDragEnd) onSlotParamsDragEnd(selected_);
    }

    /** Mirrors the selected slot into the parameter controls, and shows only
        the ones that slot's kind actually uses. */
    void refreshParamControls()
    {
        for (auto* c : paramControls())
            c->setVisible(false);

        if (! isValidSlot(selected_))
            return;

        const auto& slot = chain_[(size_t) selected_];
        updating_ = true;

        switch (slot.kind)
        {
            case model::EffectKind::Filter:
                filterMode_.setSelectedId(slot.filter.mode + 1, juce::dontSendNotification);
                cutoff_.setValue(slot.filter.cutoff, juce::dontSendNotification);
                resonance_.setValue(slot.filter.resonance, juce::dontSendNotification);
                filterMode_.setVisible(true); cutoff_.setVisible(true); resonance_.setVisible(true);
                break;

            case model::EffectKind::Delay:
                timeMs_.setValue(slot.delay.timeMs, juce::dontSendNotification);
                feedback_.setValue(slot.delay.feedback * 100.0, juce::dontSendNotification);
                mix_.setValue(slot.delay.mix * 100.0, juce::dontSendNotification);
                timeMs_.setVisible(true); feedback_.setVisible(true); mix_.setVisible(true);
                break;

            case model::EffectKind::Reverb:
                roomSize_.setValue(slot.reverb.roomSize * 100.0, juce::dontSendNotification);
                damping_.setValue(slot.reverb.damping * 100.0, juce::dontSendNotification);
                mix_.setValue(slot.reverb.mix * 100.0, juce::dontSendNotification);
                roomSize_.setVisible(true); damping_.setVisible(true); mix_.setVisible(true);
                break;

            case model::EffectKind::Drive:
                driveAmount_.setValue(slot.drive.drive, juce::dontSendNotification);
                driveTone_.setValue(slot.drive.tone * 100.0, juce::dontSendNotification);
                driveLevel_.setValue(slot.drive.level * 100.0, juce::dontSendNotification);
                driveHardClip_.setToggleState(slot.drive.hardClip, juce::dontSendNotification);
                driveCabinet_.setToggleState(slot.drive.cabinet, juce::dontSendNotification);
                driveAsymmetry_.setValue(slot.drive.asymmetry * 100.0, juce::dontSendNotification);
                driveOversample_.setToggleState(slot.drive.oversample, juce::dontSendNotification);
                driveAmount_.setVisible(true); driveTone_.setVisible(true);
                driveLevel_.setVisible(true); driveHardClip_.setVisible(true);
                driveCabinet_.setVisible(true); driveAsymmetry_.setVisible(true);
                driveOversample_.setVisible(true);
                break;

            case model::EffectKind::Compressor:
                compThreshold_.setValue(slot.compressor.thresholdDb, juce::dontSendNotification);
                compRatio_.setValue(slot.compressor.ratio, juce::dontSendNotification);
                compAttack_.setValue(slot.compressor.attackMs, juce::dontSendNotification);
                compRelease_.setValue(slot.compressor.releaseMs, juce::dontSendNotification);
                compMakeUp_.setValue(slot.compressor.makeUpDb, juce::dontSendNotification);
                compSidechain_.setSelectedId(slot.compressor.sidechainTrackId >= 0
                                                 ? slot.compressor.sidechainTrackId + 2 : 1,
                                             juce::dontSendNotification);
                compThreshold_.setVisible(true); compRatio_.setVisible(true);
                compAttack_.setVisible(true); compRelease_.setVisible(true);
                compMakeUp_.setVisible(true); compSidechain_.setVisible(true);
                break;

            case model::EffectKind::Tremolo:
                tremRate_.setValue(slot.tremolo.rateHz, juce::dontSendNotification);
                tremDepth_.setValue(slot.tremolo.depth * 100.0, juce::dontSendNotification);
                tremRate_.setVisible(true); tremDepth_.setVisible(true);
                break;

            case model::EffectKind::Chorus:
                chorusRate_.setValue(slot.chorus.rateHz, juce::dontSendNotification);
                chorusDepth_.setValue(slot.chorus.depth * 100.0, juce::dontSendNotification);
                chorusMix_.setValue(slot.chorus.mix * 100.0, juce::dontSendNotification);
                chorusRate_.setVisible(true); chorusDepth_.setVisible(true);
                chorusMix_.setVisible(true);
                break;

            case model::EffectKind::Wobble:
                wobbleRate_.setValue(slot.wobble.rateBeats, juce::dontSendNotification);
                wobbleDepth_.setValue(slot.wobble.depth * 100.0, juce::dontSendNotification);
                wobbleCutoff_.setValue(slot.wobble.baseCutoffHz, juce::dontSendNotification);
                wobbleResonance_.setValue(slot.wobble.resonance, juce::dontSendNotification);
                wobbleMix_.setValue(slot.wobble.mix * 100.0, juce::dontSendNotification);
                wobbleRate_.setVisible(true); wobbleDepth_.setVisible(true);
                wobbleCutoff_.setVisible(true); wobbleResonance_.setVisible(true);
                wobbleMix_.setVisible(true);
                break;

            case model::EffectKind::Gate:
                gateThreshold_.setValue(slot.gate.thresholdDb, juce::dontSendNotification);
                gateRange_.setValue(slot.gate.rangeDb, juce::dontSendNotification);
                gateAttack_.setValue(slot.gate.attackMs, juce::dontSendNotification);
                gateHold_.setValue(slot.gate.holdMs, juce::dontSendNotification);
                gateRelease_.setValue(slot.gate.releaseMs, juce::dontSendNotification);
                gateThreshold_.setVisible(true); gateRange_.setVisible(true);
                gateAttack_.setVisible(true); gateHold_.setVisible(true);
                gateRelease_.setVisible(true);
                break;

            case model::EffectKind::Eq:
                eqLowShelfHz_.setValue(slot.eqPedal.lowShelfHz, juce::dontSendNotification);
                eqLowShelfDb_.setValue(slot.eqPedal.lowShelfDb, juce::dontSendNotification);
                eqMidHz_.setValue(slot.eqPedal.midHz, juce::dontSendNotification);
                eqMidDb_.setValue(slot.eqPedal.midDb, juce::dontSendNotification);
                eqMidQ_.setValue(slot.eqPedal.midQ, juce::dontSendNotification);
                eqHighShelfHz_.setValue(slot.eqPedal.highShelfHz, juce::dontSendNotification);
                eqHighShelfDb_.setValue(slot.eqPedal.highShelfDb, juce::dontSendNotification);
                eqLowShelfHz_.setVisible(true); eqLowShelfDb_.setVisible(true);
                eqMidHz_.setVisible(true); eqMidDb_.setVisible(true); eqMidQ_.setVisible(true);
                eqHighShelfHz_.setVisible(true); eqHighShelfDb_.setVisible(true);
                break;

            case model::EffectKind::Plugin:
                editorButton_.setVisible(true);
                break;
        }

        updating_ = false;
        resized();
    }

    /** Reads the controls back into the selected slot and reports it. Guarded
        against the setValue calls in refreshParamControls, which would
        otherwise echo straight back as a user edit. */
    void pushParams()
    {
        if (updating_ || ! isValidSlot(selected_) || ! onSlotParamsChanged)
            return;

        auto slot = chain_[(size_t) selected_];
        switch (slot.kind)
        {
            case model::EffectKind::Filter:
                slot.filter.mode      = juce::jmax(0, filterMode_.getSelectedId() - 1);
                slot.filter.cutoff    = (float) cutoff_.getValue();
                slot.filter.resonance = (float) resonance_.getValue();
                break;
            case model::EffectKind::Delay:
                slot.delay.timeMs   = (float) timeMs_.getValue();
                slot.delay.feedback = (float) (feedback_.getValue() / 100.0);
                slot.delay.mix      = (float) (mix_.getValue() / 100.0);
                break;
            case model::EffectKind::Reverb:
                slot.reverb.roomSize = (float) (roomSize_.getValue() / 100.0);
                slot.reverb.damping  = (float) (damping_.getValue() / 100.0);
                slot.reverb.mix      = (float) (mix_.getValue() / 100.0);
                break;
            case model::EffectKind::Drive:
                slot.drive.drive    = (float) driveAmount_.getValue();
                slot.drive.tone     = (float) (driveTone_.getValue() / 100.0);
                slot.drive.level    = (float) (driveLevel_.getValue() / 100.0);
                slot.drive.hardClip = driveHardClip_.getToggleState();
                slot.drive.cabinet  = driveCabinet_.getToggleState();
                slot.drive.asymmetry  = (float) (driveAsymmetry_.getValue() / 100.0);
                slot.drive.oversample = driveOversample_.getToggleState();
                break;
            case model::EffectKind::Compressor:
                slot.compressor.thresholdDb = (float) compThreshold_.getValue();
                slot.compressor.ratio       = (float) compRatio_.getValue();
                slot.compressor.attackMs    = (float) compAttack_.getValue();
                slot.compressor.releaseMs   = (float) compRelease_.getValue();
                slot.compressor.makeUpDb    = (float) compMakeUp_.getValue();
                slot.compressor.sidechainTrackId = compSidechain_.getSelectedId() > 1
                                                     ? compSidechain_.getSelectedId() - 2 : -1;
                break;
            case model::EffectKind::Tremolo:
                slot.tremolo.rateHz = (float) tremRate_.getValue();
                slot.tremolo.depth  = (float) (tremDepth_.getValue() / 100.0);
                break;
            case model::EffectKind::Chorus:
                slot.chorus.rateHz = (float) chorusRate_.getValue();
                slot.chorus.depth  = (float) (chorusDepth_.getValue() / 100.0);
                slot.chorus.mix    = (float) (chorusMix_.getValue() / 100.0);
                break;
            case model::EffectKind::Wobble:
                slot.wobble.rateBeats    = (float) wobbleRate_.getValue();
                slot.wobble.depth        = (float) (wobbleDepth_.getValue() / 100.0);
                slot.wobble.baseCutoffHz = (float) wobbleCutoff_.getValue();
                slot.wobble.resonance    = (float) wobbleResonance_.getValue();
                slot.wobble.mix          = (float) (wobbleMix_.getValue() / 100.0);
                break;
            case model::EffectKind::Gate:
                slot.gate.thresholdDb = (float) gateThreshold_.getValue();
                slot.gate.rangeDb     = (float) gateRange_.getValue();
                slot.gate.attackMs    = (float) gateAttack_.getValue();
                slot.gate.holdMs      = (float) gateHold_.getValue();
                slot.gate.releaseMs   = (float) gateRelease_.getValue();
                break;
            case model::EffectKind::Eq:
                slot.eqPedal.lowShelfHz  = (float) eqLowShelfHz_.getValue();
                slot.eqPedal.lowShelfDb  = (float) eqLowShelfDb_.getValue();
                slot.eqPedal.midHz       = (float) eqMidHz_.getValue();
                slot.eqPedal.midDb       = (float) eqMidDb_.getValue();
                slot.eqPedal.midQ        = (float) eqMidQ_.getValue();
                slot.eqPedal.highShelfHz = (float) eqHighShelfHz_.getValue();
                slot.eqPedal.highShelfDb = (float) eqHighShelfDb_.getValue();
                break;
            case model::EffectKind::Plugin:
                return; // a plugin's parameters live in its own editor
        }

        chain_[(size_t) selected_] = slot;
        onSlotParamsChanged(slot, selected_);
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholder_.setVisible(! visible);

        juce::Component* toolbar[] = { &addButton_, &removeButton_, &upButton_, &downButton_ };
        for (auto* c : toolbar)
            c->setVisible(visible);

        if (! visible)
        {
            for (auto* c : paramControls())
                c->setVisible(false);
        }
        resized();
        repaint();
    }

    std::vector<model::EffectSlot>   chain_;
    std::vector<engine::PluginEntry> plugins_;
    int                              selected_       = 0;
    bool                             contentVisible_ = false;
    bool                             updating_       = false;

    juce::Label      placeholder_;
    juce::TextButton addButton_, removeButton_, upButton_, downButton_, editorButton_;
    juce::ComboBox   filterMode_;

    /** Which track's signal drives a compressor's detector. Item id 1 is
        "this track" (an ordinary compressor); every other id is a track id + 2,
        so ids stay positive and distinct from JUCE's "nothing selected" 0. */
    juce::ComboBox   compSidechain_;
    juce::Slider     cutoff_, resonance_, timeMs_, feedback_, mix_, roomSize_, damping_;
    juce::Slider       driveAmount_, driveTone_, driveLevel_;
    juce::Slider       compThreshold_, compRatio_, compAttack_, compRelease_, compMakeUp_;
    juce::Slider       tremRate_, tremDepth_;
    juce::Slider       chorusRate_, chorusDepth_, chorusMix_;
    juce::Slider       wobbleRate_, wobbleDepth_, wobbleCutoff_, wobbleResonance_, wobbleMix_;
    juce::Slider       gateThreshold_, gateRange_, gateAttack_, gateHold_, gateRelease_;
    juce::Slider       eqLowShelfHz_, eqLowShelfDb_, eqMidHz_, eqMidDb_, eqMidQ_,
                       eqHighShelfHz_, eqHighShelfDb_;
    juce::ToggleButton driveHardClip_, driveCabinet_, driveOversample_;
    juce::Slider       driveAsymmetry_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectChainPanel)
};

} // namespace looper
