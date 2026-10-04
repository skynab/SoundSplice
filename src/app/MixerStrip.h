#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LayoutHelpers.h"
#include "LevelMeter.h"
#include "Theme.h"

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
    std::function<void(bool)>  onArmChange;       // the R button
    std::function<void()>      onInputMenuRequested; // right-click on it
    std::function<void()>      onAutomationModeMenuRequested; // the automation mode button

    // Routing (model/Routing.h): the output button, the sends button, and
    // each send's level slider, by its position in the track's sends.
    std::function<void()>                onOutputMenuRequested;
    std::function<void()>                onSendsMenuRequested;
    std::function<void(int send, float)> onSendLevelChange;
    std::function<void(int send)>        onSendDragStart;
    std::function<void(int send)>        onSendDragEnd;

    /** One send as the strip shows it. */
    struct SendView
    {
        juce::String busName;
        float        levelDb  = 0.0f;
        bool         preFader = false;
    };

    MixerStrip()
    {
        nameLabel_.setJustificationType(juce::Justification::centred);
        nameLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
        nameLabel_.setInterceptsMouseClicks(false, false); // clicks pass through to select the strip
        addAndMakeVisible(nameLabel_);
        applyThemeColours();

        muteButton_.setClickingTogglesState(true);
        muteButton_.onClick = [this] { if (onMuteChange) onMuteChange(muteButton_.getToggleState()); };
        muteButton_.setTooltip("Mute this track");
        addAndMakeVisible(muteButton_);

        soloButton_.setClickingTogglesState(true);
        soloButton_.onClick = [this] { if (onSoloChange) onSoloChange(soloButton_.getToggleState()); };
        soloButton_.setTooltip("Solo this track - silences every other track");
        addAndMakeVisible(soloButton_);

        armButton_.setClickingTogglesState(true);
        armButton_.onClick = [this]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu())
            {
                armButton_.setToggleState(! armButton_.getToggleState(), juce::dontSendNotification);
                if (onInputMenuRequested)
                    onInputMenuRequested();
                return;
            }
            if (onArmChange)
                onArmChange(armButton_.getToggleState());
        };
        armButton_.setTooltip("Arm this track to record - several can be armed at once. "
                              "Right-click to choose its input");
        addAndMakeVisible(armButton_);

        autoModeButton_.onClick = [this] { if (onAutomationModeMenuRequested) onAutomationModeMenuRequested(); };
        autoModeButton_.setTooltip("How moving this track's controls records automation");
        addAndMakeVisible(autoModeButton_);

        outputButton_.onClick = [this] { if (onOutputMenuRequested) onOutputMenuRequested(); };
        outputButton_.setTooltip("Where this track's output goes: the master or a bus");
        addAndMakeVisible(outputButton_);

        sendsButton_.onClick = [this] { if (onSendsMenuRequested) onSendsMenuRequested(); };
        sendsButton_.setTooltip("Send a copy of this track to a bus, at its own level");
        addAndMakeVisible(sendsButton_);

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
    void setArmed(bool armed)
    {
        armButton_.setToggleState(armed, juce::dontSendNotification);
        // Armed, the meter shows what's coming in, so a clip shows too.
        meter_.setShowsClipping(armed);
    }
    void setClipped(int channel) { meter_.setClipped(channel); }
    void setInputName(const juce::String& name)
    {
        armButton_.setTooltip("Arm this track to record - several can be armed at once. Records from: " + name
                              + ". Right-click to choose");
    }
    /** Shows the mode the track records automation in: @p name, and whether
        it's the track's own (lit) or the mix's it follows. */
    void setAutomationMode(const juce::String& name, bool ownMode)
    {
        autoModeButton_.setButtonText(name);
        autoModeButton_.setColour(juce::TextButton::buttonColourId,
                                  ownMode ? juce::Colours::steelblue.withAlpha(0.6f)
                                          : getLookAndFeel().findColour(juce::TextButton::buttonColourId));
        autoModeButton_.setColour(juce::TextButton::textColourOffId,
                                  juce::Colours::white.withAlpha(ownMode ? 1.0f : 0.55f));
    }
    /** Shows where the output goes, by name. */
    void setOutputName(const juce::String& name) { outputButton_.setButtonText("Out: " + name); }

    /** A bus can't be recorded onto. */
    void setArmable(bool armable)
    {
        armButton_.setEnabled(armable);
        armButton_.setAlpha(armable ? 1.0f : 0.35f);
    }

    /** The track's sends, a level slider each. Rebuilt only when how many
        there are changes, so a drag on one isn't cut short by a refresh. */
    void setSends(const std::vector<SendView>& sends)
    {
        if (sends.size() != sendSliders_.size())
        {
            sendSliders_.clear();
            for (int i = 0; i < (int) sends.size(); ++i)
            {
                auto slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
                slider->setRange(-60.0, 6.0, 0.1);
                slider->setDoubleClickReturnValue(true, 0.0);
                slider->setPopupDisplayEnabled(true, false, this);
                slider->setTextValueSuffix(" dB");
                slider->onValueChange = [this, i, raw = slider.get()]
                {
                    if (onSendLevelChange) onSendLevelChange(i, (float) raw->getValue());
                };
                slider->onDragStart = [this, i] { if (onSendDragStart) onSendDragStart(i); };
                slider->onDragEnd   = [this, i] { if (onSendDragEnd) onSendDragEnd(i); };
                addAndMakeVisible(*slider);
                sendSliders_.push_back(std::move(slider));
            }
            resized();
        }

        for (size_t i = 0; i < sends.size(); ++i)
        {
            sendSliders_[i]->setValue(sends[i].levelDb, juce::dontSendNotification);
            sendSliders_[i]->setTooltip("Send to " + sends[i].busName + (sends[i].preFader ? " (pre-fader)" : ""));
        }
        sendsButton_.setButtonText(sends.empty() ? juce::String("Sends") : "Sends (" + juce::String((int) sends.size()) + ")");
    }

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

    // The lit buttons take the theme's colours, which reach the strip only
    // once it is in the window: mute and arm are danger, solo is warn, with
    // dark text on solo's light amber.
    void lookAndFeelChanged() override { applyThemeColours(); }
    void parentHierarchyChanged() override { applyThemeColours(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);

        nameLabel_.setBounds(area.removeFromTop(20));
        area.removeFromTop(4);

        auto btnRow = area.removeFromTop(22);
        const int third = btnRow.getWidth() / 3;
        muteButton_.setBounds(btnRow.removeFromLeft(third).reduced(2));
        soloButton_.setBounds(btnRow.removeFromLeft(third).reduced(2));
        armButton_.setBounds(btnRow.reduced(2));
        area.removeFromTop(2);
        autoModeButton_.setBounds(area.removeFromTop(18).reduced(2, 0));
        area.removeFromTop(2);
        outputButton_.setBounds(area.removeFromTop(18).reduced(2, 0));
        area.removeFromTop(2);
        sendsButton_.setBounds(area.removeFromTop(18).reduced(2, 0));
        for (auto& slider : sendSliders_)
            slider->setBounds(area.removeFromTop(14).reduced(2, 0));
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
    void applyThemeColours()
    {
        muteButton_.setColour(juce::TextButton::buttonOnColourId, theme::colour(*this, theme::dangerId));
        armButton_.setColour(juce::TextButton::buttonOnColourId, theme::colour(*this, theme::dangerId));
        soloButton_.setColour(juce::TextButton::buttonOnColourId, theme::colour(*this, theme::warnId));
        soloButton_.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff141520));
    }

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
    juce::TextButton armButton_  { "R" };
    juce::TextButton autoModeButton_ { "Read" };
    juce::TextButton outputButton_ { "Out: Master" };
    juce::TextButton sendsButton_ { "Sends" };
    std::vector<std::unique_ptr<juce::Slider>> sendSliders_;
    juce::Label      panLabel_;
    juce::Slider     panSlider_;
    juce::Slider     gainSlider_;
    LevelMeter       meter_;
    bool             selected_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerStrip)
};

} // namespace soundsplice
