#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LayoutHelpers.h"
#include "LevelMeter.h"

namespace soundsplice
{
/**
    A single channel strip in the mixer view: track name, mute/solo, a vertical
    gain fader, and a level meter. Purely a display/input widget — it owns no
    model or engine state; the owner wires its callbacks to the document and
    engine, and feeds it fresh values via the setters.

    Clicking the strip's background (but not its buttons/fader) selects the
    track, matching the common DAW pattern of "click a channel strip to arm it."
*/
class MixerStrip final : public juce::Component
{
public:
    /** Which fader a drag belongs to. One pair of drag callbacks carrying this
        beats a pair per fader that must each be remembered. */
    enum class Fader { Gain, Pan };

    /** Fired when a fader is grabbed and released. The owner uses them to turn
        a whole drag into one undo step: hundreds of onXChange calls make the
        audio follow the fader, and the pair around them says where the move
        began and ended. */
    std::function<void(Fader)> onFaderDragStart;
    std::function<void(Fader)> onFaderDragEnd;

    std::function<void(float)> onGainChange;
    std::function<void(bool)>  onMuteChange;
    std::function<void(bool)>  onSoloChange;
    std::function<void(float)> onPanChange;
    std::function<void()>      onSelect;

    MixerStrip()
    {
        nameLabel_.setJustificationType(juce::Justification::centred);
        nameLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
        nameLabel_.setInterceptsMouseClicks(false, false); // clicks pass through to select the strip
        addAndMakeVisible(nameLabel_);

        muteButton_.setClickingTogglesState(true);
        muteButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::orangered);
        muteButton_.onClick = [this] { if (onMuteChange) onMuteChange(muteButton_.getToggleState()); };
        muteButton_.setTooltip("Mute this track");
        addAndMakeVisible(muteButton_);

        soloButton_.setClickingTogglesState(true);
        soloButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
        soloButton_.onClick = [this] { if (onSoloChange) onSoloChange(soloButton_.getToggleState()); };
        soloButton_.setTooltip("Solo this track - silences every other track");
        addAndMakeVisible(soloButton_);

        panSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        panSlider_.setRange(-100.0, 100.0, 1.0);
        panSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        // Too narrow for a static readout the way the gain fader's text box
        // has room for — a popup while dragging answers "what value am I at".
        panSlider_.setPopupDisplayEnabled(true, false, this);
        panSlider_.setTextValueSuffix(" pan");
        panSlider_.setDoubleClickReturnValue(true, 0.0); // double-click re-centres
        panSlider_.onValueChange = [this] { if (onPanChange) onPanChange((float) (panSlider_.getValue() / 100.0)); };
        wireDrag(panSlider_, Fader::Pan);
        addAndMakeVisible(panSlider_);

        panLabel_.setText("Pan", juce::dontSendNotification);
        panLabel_.setFont(juce::Font(juce::FontOptions(10.0f)));
        panLabel_.setJustificationType(juce::Justification::centredLeft);
        panLabel_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(panLabel_);

        gainSlider_.setSliderStyle(juce::Slider::LinearVertical);
        gainSlider_.setRange(-60.0, 6.0, 0.1);
        gainSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
        gainSlider_.setTextValueSuffix(" dB");
        gainSlider_.onValueChange = [this] { if (onGainChange) onGainChange((float) gainSlider_.getValue()); };
        wireDrag(gainSlider_, Fader::Gain);
        addAndMakeVisible(gainSlider_);

        addAndMakeVisible(meter_);
    }

    void setTrackName(const juce::String& name) { nameLabel_.setText(name, juce::dontSendNotification); }
    void setGainDb(float db)   { gainSlider_.setValue(db, juce::dontSendNotification); }
    void setMuted(bool muted)  { muteButton_.setToggleState(muted, juce::dontSendNotification); }
    void setSoloed(bool solo)  { soloButton_.setToggleState(solo, juce::dontSendNotification); }
    void setPan(float pan)         { panSlider_.setValue(pan * 100.0, juce::dontSendNotification); }
    void setSelected(bool sel) { if (selected_ != sel) { selected_ = sel; repaint(); } }
    void setLevel(int channel, float linearPeak) { meter_.setLevel(channel, linearPeak); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(selected_ ? juce::Colours::white.withAlpha(0.10f) : juce::Colours::black.withAlpha(0.15f));
        g.fillRoundedRectangle(area, 4.0f);

        if (selected_)
        {
            g.setColour(juce::Colours::orange.withAlpha(0.8f));
            g.drawRoundedRectangle(area.reduced(1.0f), 4.0f, 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onSelect)
            onSelect();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);

        nameLabel_.setBounds(area.removeFromTop(20));
        area.removeFromTop(4);

        auto btnRow = area.removeFromTop(22);
        muteButton_.setBounds(btnRow.removeFromLeft(btnRow.getWidth() / 2).reduced(2));
        soloButton_.setBounds(btnRow.reduced(2));
        area.removeFromTop(4);

        // Strips pack side by side, so a narrow one is the normal case once
        // there are several tracks. A fixed 30px caption used to take the
        // whole row and leave the slider zero wide — the label survived and
        // the control it labels didn't.
        layoutLabelledRow(area.removeFromTop(16), panLabel_, panSlider_, 30);
        area.removeFromTop(6);

        auto meterArea = area.removeFromRight(20);
        setBoundsOrHide(meter_, meterArea);
        area.removeFromRight(4);
        setBoundsOrHide(gainSlider_, area);
    }

private:
    /** Reports a fader's grab and release. Both are needed: the start says
        what to undo back to, and without the end a drag would never commit. */
    void wireDrag(juce::Slider& slider, Fader fader)
    {
        slider.onDragStart = [this, fader] { if (onFaderDragStart) onFaderDragStart(fader); };
        slider.onDragEnd   = [this, fader] { if (onFaderDragEnd)   onFaderDragEnd(fader); };
    }

    juce::Label      nameLabel_;
    juce::TextButton muteButton_ { "M" };
    juce::TextButton soloButton_ { "S" };
    juce::Label      panLabel_;
    juce::Slider     panSlider_;
    juce::Slider     gainSlider_;
    LevelMeter       meter_;
    bool             selected_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerStrip)
};

} // namespace soundsplice
