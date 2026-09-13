#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/DrumKitStyle.h"
#include "engine/Pattern.h"
#include "model/DrumKit.h"
#include "model/Track.h"

#include "DrumKitEditor.h"
#include "DrumStepGrid.h"
#include "TrackColours.h"

namespace looper
{
/**
    The Drums pane: the kit's sounds on the left (DrumKitEditor — load/replace
    samples, add pads, per-pad mute/solo/gain/pan/pitch) and the rhythm on the
    right (DrumStepGrid — one row per pad, click to toggle steps), with a
    draggable divider between them.

    It owns no state of its own beyond what's needed to keep the two halves
    agreeing on the selected pad; everything else is forwarded straight
    through to whichever half needs it, and edits are reported to the owner
    via the callbacks below (which are simply the two children's callbacks
    re-exposed, so MainComponent wires the document/engine in one place).

    Shows a placeholder instead of both halves when the selected track isn't
    a Drum track — the same is-it-this-track-type gating SynthEditor uses for
    Instrument tracks.
*/
class DrumsPane final : public juce::Component
{
public:
    DrumsPane()
    {
        placeholderLabel_.setText("Select a Drum track to edit its kit", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        addChildComponent(kitEditor_);
        addChildComponent(stepGrid_);
        addChildComponent(divider_);

        // Selecting a pad on either side selects it on the other, so the
        // mix strip always describes the row you last touched in the grid.
        kitEditor_.onPadSelected = [this](int padIndex) { stepGrid_.setSelectedPad(padIndex); };
        stepGrid_.onPadSelected  = [this](int padIndex)
        {
            kitEditor_.setSelectedPad(padIndex);
            stepGrid_.setSelectedPad(padIndex);
        };

        // Kit styles: one button per engine::DrumKitStyle, each swapping
        // every pad's sample and trim in one click. Same shape as the tone
        // rows on FretboardPane and SynthEditor.
        for (int i = 0; i < engine::kNumDrumKitStyles; ++i)
        {
            const auto style  = (engine::DrumKitStyle) i;
            auto*      button = kitStyleButtons_.add(new juce::TextButton(engine::drumKitStyleName(style)));
            button->onClick   = [this, style] { if (onKitStyleRequested) onKitStyleRequested(style); };
            addChildComponent(button);
        }

        layout_.setItemLayout(0, 220, 520, 320); // kit editor: min/max/preferred
        layout_.setItemLayout(1, 8, 8, 8);       // divider
        layout_.setItemLayout(2, 200, -1.0, -1.0); // step grid: takes the rest
    }

    // Straight pass-throughs to the two halves — see DrumKitEditor/DrumStepGrid.
    std::function<void(int padIndex, const juce::File&)>     onSampleAssigned;
    std::function<void(int padIndex, const model::DrumPad&)> onPadMixChanged;
    std::function<void()>                                    onPadAdded;
    std::function<void(int padIndex)>                        onPadRemoved;
    std::function<void(const engine::Pattern&)>              onPatternChanged;
    std::function<void(int noteNumber)>                      onNotePreview;

    /** A one-click kit character: every pad's sample and trim replaced at
        once — see engine::padsForDrumKitStyle, which is what knows the
        values, and MainComponent::applyDrumKitStyle, which resolves them to
        real files. Not a pass-through like the callbacks above; this pane
        owns the buttons that fire it. */
    std::function<void(engine::DrumKitStyle)>                onKitStyleRequested;

    /** Shows the kit and its pattern. Both halves get the same pad list, so
        the grid's rows and the editor's rows always describe the same kit. */
    void setKit(const std::vector<model::DrumPad>& pads, const engine::Pattern& pattern)
    {
        kitEditor_.setPads(pads);
        stepGrid_.setPads(pads);
        stepGrid_.setPattern(pattern);
        stepGrid_.setSelectedPad(kitEditor_.selectedPad());
        setContentVisible(true);
    }

    /** Which track this is, so the header says so — this tab's title never
        changes per track, so without this there was no on-screen way to
        tell which track's kit was actually open after switching tracks
        while parked here. */
    void setTrackInfo(const juce::String& name, juce::uint32 colour)
    {
        trackName_    = name;
        trackColour_  = colour;
        repaint();
    }

    /** Refreshes only what a live mix tweak affects — the grid's dimming of
        muted pads — without rebuilding the kit editor's row widgets underneath
        the mouse mid-drag (see DrumKitEditor::setPads). */
    void refreshPadsForMixChange(const std::vector<model::DrumPad>& pads)
    {
        stepGrid_.setPads(pads);
    }

    void setPattern(const engine::Pattern& pattern) { stepGrid_.setPattern(pattern); }

    void setPlayheadBeats(double patternLocalBeats, bool visible)
    {
        if (contentVisible_)
            stepGrid_.setPlayheadBeats(patternLocalBeats, visible);
    }

    /** Shows the placeholder instead of the kit — the selected track isn't a
        Drum track (or none is selected). */
    void setNoDrumTrackSelected() { setContentVisible(false); }

    /** Wires the two halves' callbacks through to this pane's own. Called once
        by the owner after construction. */
    void connectCallbacks()
    {
        kitEditor_.onSampleAssigned = [this](int padIndex, const juce::File& file)
        {
            if (onSampleAssigned) onSampleAssigned(padIndex, file);
        };
        kitEditor_.onPadMixChanged = [this](int padIndex, const model::DrumPad& pad)
        {
            if (onPadMixChanged) onPadMixChanged(padIndex, pad);
        };
        kitEditor_.onPadAdded   = [this] { if (onPadAdded) onPadAdded(); };
        kitEditor_.onPadRemoved = [this](int padIndex) { if (onPadRemoved) onPadRemoved(padIndex); };

        stepGrid_.onChange = [this](const engine::Pattern& pattern)
        {
            if (onPatternChanged) onPatternChanged(pattern);
        };
        stepGrid_.onNotePreview = [this](int noteNumber)
        {
            if (onNotePreview) onNotePreview(noteNumber);
        };
    }

    void paint(juce::Graphics& g) override
    {
        if (contentVisible_)
            paintTrackHeader(g, headerBounds(), trackName_, trackColour_, model::TrackType::Drum);
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);

        // Divide what's actually there rather than imposing a minimum width
        // — a minimum overflows the row in a narrow pane and leaves the
        // buttons past the edge with zero width, which is indistinguishable
        // from a button that doesn't work. Same loop as FretboardPane's.
        auto styleRow = area.removeFromTop(kKitStyleRowHeight);
        for (int i = 0; i < kitStyleButtons_.size(); ++i)
        {
            const int remaining = kitStyleButtons_.size() - i;
            const int width     = juce::jmax(1, styleRow.getWidth() / remaining);
            kitStyleButtons_[i]->setBounds(styleRow.removeFromLeft(width).reduced(1));
        }

        juce::Component* items[] = { &kitEditor_, &divider_, &stepGrid_ };
        layout_.layOutComponents(items, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                 false, // side by side
                                 true); // full height
    }

private:
    /** Same top strip resized() carves out before laying out the two halves
        — kept in one place so paint() and resized() can't drift apart. */
    juce::Rectangle<int> headerBounds() const
    {
        return getLocalBounds().removeFromTop(kTrackHeaderHeight);
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholderLabel_.setVisible(! visible);
        kitEditor_.setVisible(visible);
        stepGrid_.setVisible(visible);
        divider_.setVisible(visible);
        for (auto* button : kitStyleButtons_)
            button->setVisible(visible);
        resized();
    }

    static constexpr int kKitStyleRowHeight = 22;

    DrumKitEditor kitEditor_;
    DrumStepGrid  stepGrid_;
    bool          contentVisible_ = false;
    juce::String  trackName_;
    juce::uint32  trackColour_ = 0;

    juce::Label                       placeholderLabel_;
    juce::StretchableLayoutManager    layout_;
    juce::StretchableLayoutResizerBar divider_ { &layout_, 1, true };
    juce::OwnedArray<juce::TextButton> kitStyleButtons_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumsPane)
};

} // namespace looper
