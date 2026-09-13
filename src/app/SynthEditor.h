#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "engine/SynthTone.h"
#include "model/SynthSettings.h"
#include "model/Track.h"

#include "TrackColours.h"

namespace looper
{
/**
    Editor for the currently selected Instrument track's timbre
    (model::SynthSettings): oscillator waveform, amp envelope, an optional
    per-voice filter, and an output trim — organized into labelled sections
    the way Reason's Objekt panel groups Oscillator/Filter/Amp controls,
    built from this app's existing plain slider/combo-box widgets rather than
    a new rotary-knob look, to stay visually consistent with the rest of the
    app (see the Mixer master panel's filter/delay/reverb controls).

    Shows a centred placeholder instead of the controls when no Instrument
    track is selected (see setNoTrackSelected) — Drum and Audio tracks have
    no synth to edit.
*/
class SynthEditor final : public juce::Component
{
public:
    // Fires with the whole updated settings on any control change — cheap
    // enough to always send the full struct rather than one callback per
    // field (mirrors how MainComponent::syncEngineTracks already pushes every
    // field together).
    std::function<void(const model::SynthSettings&)> onSettingsChanged;

    /** Brackets a change to the settings, so the owner can commit the whole
        drag as one undo step — see EffectChainPanel's identical pair, which
        this mirrors for the same reason: settings are one struct changed as
        a unit, not one field per callback. */
    std::function<void()> onSettingsDragStart;
    std::function<void()> onSettingsDragEnd;

    /** Presets are named elsewhere (MainComponent owns the files); this pane
        only shows names and reports intent, the same separation ArrangementView
        keeps from track colours and EffectChainPanel keeps from the plugin
        list. Index is into whatever list setPresetNames() was last given. */
    std::function<void(int index)> onPresetSelected;
    std::function<void()>          onSavePresetRequested;
    std::function<void(int index)> onDeletePresetRequested;

    /** A one-click starting sound: settings plus an effect chain tuned for
        the picked engine::SynthTone — see model::presetForSynthTone, which
        is what actually knows the values, the same way this pane never has
        document mutation logic of its own. Complements the preset box above
        rather than replacing it: these are always present and can't be
        deleted, where a saved preset is a file the user made. */
    std::function<void(engine::SynthTone)> onSynthToneRequested;

    SynthEditor()
    {
        placeholderLabel_.setText("Select an Instrument track to edit its synth", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        // "(no preset)" occupies id 1 so that loading a project whose synth
        // doesn't match any saved preset has something honest to show,
        // rather than the box silently defaulting to whatever preset
        // happens to be first.
        presetBox_.addItem("(no preset)", 1);
        presetBox_.setSelectedId(1, juce::dontSendNotification);
        presetBox_.onChange = [this]
        {
            const int index = presetBox_.getSelectedId() - 2; // -1 for "(no preset)", -1 for 1-based ids
            if (index >= 0 && onPresetSelected)
                onPresetSelected(index);
        };
        addAndMakeVisible(presetBox_);

        savePresetButton_.setButtonText("Save...");
        savePresetButton_.setTooltip("Save this track's synth and effect chain as a new preset");
        savePresetButton_.onClick = [this] { if (onSavePresetRequested) onSavePresetRequested(); };
        addAndMakeVisible(savePresetButton_);

        deletePresetButton_.setButtonText("Delete");
        deletePresetButton_.setTooltip("Delete the selected preset");
        deletePresetButton_.onClick = [this]
        {
            const int index = presetBox_.getSelectedId() - 2;
            if (index >= 0 && onDeletePresetRequested)
                onDeletePresetRequested(index);
        };
        addAndMakeVisible(deletePresetButton_);

        // Tone templates: one button per engine::SynthTone, each applying a
        // whole SynthSettings + effect chain in one click. Laid out as one
        // row, the same "divide what's actually there" way FretboardPane
        // lays out its guitar tones.
        for (int i = 0; i < engine::kNumSynthTones; ++i)
        {
            const auto tone   = (engine::SynthTone) i;
            auto*      button = toneButtons_.add(new juce::TextButton(engine::synthToneName(tone)));
            button->onClick   = [this, tone] { if (onSynthToneRequested) onSynthToneRequested(tone); };
            addAndMakeVisible(button);
        }

        setupSectionHeader(oscHeader_, "Oscillator");
        waveformBox_.addItem("Sine", 1);
        waveformBox_.addItem("Saw", 2);
        waveformBox_.addItem("Square", 3);
        waveformBox_.addItem("Triangle", 4);
        waveformBox_.setSelectedId(1, juce::dontSendNotification);
        waveformBox_.onChange = [this]
        {
            reportInstantEdit([this] { settings_.waveform = waveformBox_.getSelectedId() - 1; notify(); });
        };
        addAndMakeVisible(waveformBox_);

        setupSectionHeader(ampHeader_, "Amp Envelope");
        setupSlider(attackSlider_, "Attack", 0.0, 2000.0, 1.0, " ms",
                    [this] { settings_.attackMs = (float) attackSlider_.getValue(); notify(); });
        setupSlider(decaySlider_, "Decay", 0.0, 2000.0, 1.0, " ms",
                    [this] { settings_.decayMs = (float) decaySlider_.getValue(); notify(); });
        setupSlider(sustainSlider_, "Sustain", 0.0, 100.0, 1.0, " %",
                    [this] { settings_.sustain = (float) (sustainSlider_.getValue() / 100.0); notify(); });
        setupSlider(releaseSlider_, "Release", 0.0, 4000.0, 1.0, " ms",
                    [this] { settings_.releaseMs = (float) releaseSlider_.getValue(); notify(); });

        setupSectionHeader(filterHeader_, "Filter");
        filterEnabledButton_.setButtonText("On");
        filterEnabledButton_.setClickingTogglesState(true);
        filterEnabledButton_.onClick = [this]
        {
            reportInstantEdit([this]
            {
                settings_.filterEnabled = filterEnabledButton_.getToggleState();
                updateFilterControlsEnabled();
                notify();
            });
        };
        addAndMakeVisible(filterEnabledButton_);

        filterModeBox_.addItem("Low-pass", 1);
        filterModeBox_.addItem("High-pass", 2);
        filterModeBox_.addItem("Band-pass", 3);
        filterModeBox_.setSelectedId(1, juce::dontSendNotification);
        filterModeBox_.onChange = [this]
        {
            reportInstantEdit([this]
            {
                settings_.filterMode = juce::jmax(0, filterModeBox_.getSelectedId() - 1);
                notify();
            });
        };
        addAndMakeVisible(filterModeBox_);

        setupSlider(filterCutoffSlider_, "Cutoff", 20.0, 18000.0, 1.0, " Hz",
                    [this] { settings_.filterCutoff = (float) filterCutoffSlider_.getValue(); notify(); });
        filterCutoffSlider_.setSkewFactorFromMidPoint(1000.0);
        setupSlider(filterResonanceSlider_, "Resonance", 0.1, 5.0, 0.01, " Q",
                    [this] { settings_.filterResonance = (float) filterResonanceSlider_.getValue(); notify(); });

        setupSectionHeader(outputHeader_, "Output");
        setupSlider(gainSlider_, "Gain", -24.0, 24.0, 0.1, " dB",
                    [this] { settings_.gainDb = (float) gainSlider_.getValue(); notify(); });

        updateFilterControlsEnabled();
        setControlsVisible(false);
    }

    /** Reflects @p settings without firing onSettingsChanged, and reveals the
        controls (hiding the placeholder). Called whenever the selected track
        changes, or its synth settings change from elsewhere (undo/redo). */
    void setSettings(const model::SynthSettings& settings)
    {
        settings_ = settings;

        waveformBox_.setSelectedId(settings.waveform + 1, juce::dontSendNotification);
        attackSlider_.setValue(settings.attackMs, juce::dontSendNotification);
        decaySlider_.setValue(settings.decayMs, juce::dontSendNotification);
        sustainSlider_.setValue(settings.sustain * 100.0, juce::dontSendNotification);
        releaseSlider_.setValue(settings.releaseMs, juce::dontSendNotification);
        filterEnabledButton_.setToggleState(settings.filterEnabled, juce::dontSendNotification);
        filterModeBox_.setSelectedId(settings.filterMode + 1, juce::dontSendNotification);
        filterCutoffSlider_.setValue(settings.filterCutoff, juce::dontSendNotification);
        filterResonanceSlider_.setValue(settings.filterResonance, juce::dontSendNotification);
        gainSlider_.setValue(settings.gainDb, juce::dontSendNotification);

        updateFilterControlsEnabled();
        setControlsVisible(true);

        // Selecting a track (or undoing/redoing into different settings)
        // doesn't correspond to any particular saved preset — leaving the
        // box on whatever it last showed would claim otherwise.
        presetBox_.setSelectedId(1, juce::dontSendNotification);
    }

    /** The names of the presets available to load, in the order they should
        be listed — MainComponent owns the actual files, this only shows
        what it's told. */
    void setPresetNames(const juce::StringArray& names)
    {
        presetBox_.clear(juce::dontSendNotification);
        presetBox_.addItem("(no preset)", 1);
        for (int i = 0; i < names.size(); ++i)
            presetBox_.addItem(names[i], i + 2);
        presetBox_.setSelectedId(1, juce::dontSendNotification);
    }

    /** Shows the placeholder instead of the controls — the selected track
        isn't an Instrument track (or none is selected). */
    void setNoTrackSelected()
    {
        setControlsVisible(false);
    }

    /** Which track this is, so the header says so — this tab's title never
        changes per track, so without this there was no on-screen way to
        tell which track's patch was actually open after switching tracks
        while parked here. */
    void setTrackInfo(const juce::String& name, juce::uint32 colour)
    {
        trackName_   = name;
        trackColour_ = colour;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (controlsVisible_)
            paintTrackHeader(g, headerBounds(), trackName_, trackColour_, model::TrackType::Instrument);
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());

        if (! controlsVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(10);

        auto presetRow = area.removeFromTop(kRowHeight);
        deletePresetButton_.setBounds(presetRow.removeFromRight(64).reduced(2, 0));
        savePresetButton_.setBounds(presetRow.removeFromRight(64).reduced(2, 0));
        presetBox_.setBounds(presetRow.reduced(2, 0));

        // Divide what's actually there rather than imposing a minimum width:
        // a minimum overflows the row in a narrow pane and the buttons past
        // the edge get zero width - present, hit-testable at nothing, and
        // indistinguishable from a button that doesn't work. Same loop, and
        // same reason, as FretboardPane's tone row.
        auto toneRow = area.removeFromTop(kToneRowHeight);
        for (int i = 0; i < toneButtons_.size(); ++i)
        {
            const int remaining = toneButtons_.size() - i;
            const int width     = juce::jmax(1, toneRow.getWidth() / remaining);
            toneButtons_[i]->setBounds(toneRow.removeFromLeft(width).reduced(1));
        }
        area.removeFromTop(10);

        layoutHeader(oscHeader_, area);
        layoutRow(area, waveformBox_);
        area.removeFromTop(10);

        layoutHeader(ampHeader_, area);
        layoutRow(area, attackSlider_);
        layoutRow(area, decaySlider_);
        layoutRow(area, sustainSlider_);
        layoutRow(area, releaseSlider_);
        area.removeFromTop(10);

        layoutHeader(filterHeader_, area);
        auto filterToggleRow = area.removeFromTop(kRowHeight);
        filterEnabledButton_.setBounds(filterToggleRow.removeFromLeft(60).reduced(2));
        filterModeBox_.setBounds(filterToggleRow.reduced(2));
        layoutRow(area, filterCutoffSlider_);
        layoutRow(area, filterResonanceSlider_);
        area.removeFromTop(10);

        layoutHeader(outputHeader_, area);
        layoutRow(area, gainSlider_);
    }

private:
    static constexpr int kRowHeight     = 26;
    static constexpr int kHeaderHeight  = 22;
    static constexpr int kToneRowHeight = 22;

    void setupSectionHeader(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.85f));
        addAndMakeVisible(label);
    }

    void setupSlider(juce::Slider& slider, const juce::String& suffix, double lo, double hi, double step,
                     const juce::String& unitSuffix, std::function<void()> onChange)
    {
        slider.setRange(lo, hi, step);
        slider.setTextValueSuffix(unitSuffix);
        slider.setName(suffix);
        slider.onValueChange = std::move(onChange);
        slider.onDragStart = [this] { if (onSettingsDragStart) onSettingsDragStart(); };
        slider.onDragEnd   = [this] { if (onSettingsDragEnd)   onSettingsDragEnd(); };
        addAndMakeVisible(slider);
    }

    /** For a discrete control (toggle, dropdown): a click has no "during" to
        bracket, so both ends fire back to back around the one edit it
        makes — same reasoning as EffectChainPanel::reportInstantEdit. */
    void reportInstantEdit(std::function<void()> apply)
    {
        if (onSettingsDragStart) onSettingsDragStart();
        apply();
        if (onSettingsDragEnd) onSettingsDragEnd();
    }

    void layoutHeader(juce::Label& label, juce::Rectangle<int>& area)
    {
        label.setBounds(area.removeFromTop(kHeaderHeight));
    }

    /** Same top strip resized() carves out before the rest of the layout —
        kept in one place so paint() and resized() can't drift apart. */
    juce::Rectangle<int> headerBounds() const
    {
        return getLocalBounds().removeFromTop(kTrackHeaderHeight);
    }

    void layoutRow(juce::Rectangle<int>& area, juce::Component& control)
    {
        control.setBounds(area.removeFromTop(kRowHeight).reduced(0, 2));
        area.removeFromTop(2);
    }

    void updateFilterControlsEnabled()
    {
        const bool on = filterEnabledButton_.getToggleState();
        filterModeBox_.setEnabled(on);
        filterCutoffSlider_.setEnabled(on);
        filterResonanceSlider_.setEnabled(on);
    }

    /** Every control this pane shows and hides, in one place — see the same
        list in FretboardPane and EffectChainPanel, and the bug that made it
        worth having: a control laid out and shown from one list while being
        parented from another drifts silently, and the failure looks like a
        control that simply doesn't draw. */
    std::vector<juce::Component*> managedControls()
    {
        std::vector<juce::Component*> controls {
            &presetBox_, &savePresetButton_, &deletePresetButton_,
            &oscHeader_, &waveformBox_, &ampHeader_,
            &attackSlider_, &decaySlider_, &sustainSlider_, &releaseSlider_, &filterHeader_,
            &filterEnabledButton_, &filterModeBox_, &filterCutoffSlider_, &filterResonanceSlider_,
            &outputHeader_, &gainSlider_
        };

        for (auto* button : toneButtons_)
            controls.push_back(button);

        return controls;
    }

    void setControlsVisible(bool visible)
    {
        controlsVisible_ = visible;
        placeholderLabel_.setVisible(! visible);
        for (auto* c : managedControls())
            c->setVisible(visible);
        resized();
    }

    void notify()
    {
        if (onSettingsChanged)
            onSettingsChanged(settings_);
    }

    model::SynthSettings settings_;
    bool                  controlsVisible_ = false;
    juce::String          trackName_;
    juce::uint32          trackColour_ = 0;

    juce::Label      placeholderLabel_;

    juce::ComboBox   presetBox_;
    juce::TextButton savePresetButton_, deletePresetButton_;
    juce::OwnedArray<juce::TextButton> toneButtons_;

    juce::Label      oscHeader_;
    juce::ComboBox   waveformBox_;

    juce::Label      ampHeader_;
    juce::Slider      attackSlider_, decaySlider_, sustainSlider_, releaseSlider_;

    juce::Label       filterHeader_;
    juce::TextButton  filterEnabledButton_;
    juce::ComboBox    filterModeBox_;
    juce::Slider      filterCutoffSlider_, filterResonanceSlider_;

    juce::Label       outputHeader_;
    juce::Slider      gainSlider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthEditor)
};

} // namespace looper
