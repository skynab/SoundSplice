#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace looper
{
/**
    A transient message overlaid on the window: "Recorded: take3.wav",
    "Could not open project.looper: bad version".

    Everything the app had to say used to go into the transport pane's clip
    label, which is a poor place for it twice over. It already had a job —
    naming the loaded clip — so a message and the clip name overwrote each
    other; and that label can be scrolled away by collapsing the transport
    pane, or removed from the window entirely by closing it. A failed save
    reporting itself into a hidden label is indistinguishable from a save that
    worked.

    So this is a child of the top-level component rather than of any pane: it
    can't be closed, collapsed or docked away. It never takes mouse clicks, and
    it fades out on its own — a message that has to be dismissed is a dialog,
    and none of these are worth interrupting anyone for.
*/
class StatusBanner final : public juce::Component,
                           private juce::Timer
{
public:
    StatusBanner()
    {
        setInterceptsMouseClicks(false, false);
        setVisible(false);
    }

    /** Shows @p message, replacing whatever is on screen. @p isError holds it
        longer and colours it: the messages that report a failure are the ones
        worth being sure the user caught. */
    void show(const juce::String& message, bool isError)
    {
        message_    = message;
        isError_    = isError;
        opacity_    = 1.0f;
        msRemaining_ = isError ? kErrorHoldMs : kInfoHoldMs;

        updateBounds();
        setVisible(true);
        toFront(false); // never taking focus: it's a notice, not a control
        repaint();
        startTimerHz(kFrameRate);
    }

    /** Sizes and places the banner against its parent — bottom centre, clear
        of the edge. Called on show and whenever the window resizes. */
    void updateBounds()
    {
        auto* parent = getParentComponent();
        if (parent == nullptr)
            return;

        const int textWidth = juce::GlyphArrangement::getStringWidthInt(font(), message_);
        const int width     = juce::jmin(parent->getWidth() - 2 * kMargin, textWidth + 2 * kPadding);
        const int height    = kHeight;

        setBounds(parent->getWidth() / 2 - width / 2,
                  parent->getHeight() - height - kMargin,
                  juce::jmax(width, 1),
                  height);
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        g.setColour(juce::Colour(0xff1c1c20).withAlpha(0.94f * opacity_));
        g.fillRoundedRectangle(area, 5.0f);

        g.setColour((isError_ ? juce::Colour(0xffe05c4a) : juce::Colours::white).withAlpha(0.45f * opacity_));
        g.drawRoundedRectangle(area.reduced(0.5f), 5.0f, 1.0f);

        g.setColour(juce::Colours::white.withAlpha((isError_ ? 0.98f : 0.85f) * opacity_));
        g.setFont(font());
        g.drawText(message_, getLocalBounds().reduced(kPadding, 0), juce::Justification::centred, false);
    }

private:
    static constexpr int   kInfoHoldMs  = 3200;
    static constexpr int   kErrorHoldMs = 6000;
    static constexpr int   kFadeMs      = 500;
    static constexpr int   kFrameRate   = 30;
    static constexpr int   kHeight      = 30;
    static constexpr int   kPadding     = 16;
    static constexpr int   kMargin      = 14;

    static juce::Font font() { return juce::FontOptions(14.0f); }

    /** Holds at full strength, then fades. The hold is what makes it readable;
        the fade is what stops it becoming permanent furniture. */
    void timerCallback() override
    {
        msRemaining_ -= 1000 / kFrameRate;

        if (msRemaining_ > 0)
            return; // still holding

        opacity_ += -1.0f / (float) (kFadeMs / (1000 / kFrameRate));

        if (opacity_ <= 0.0f)
        {
            opacity_ = 0.0f;
            setVisible(false);
            stopTimer();
        }

        repaint();
    }

    juce::String message_;
    bool         isError_     = false;
    float        opacity_     = 0.0f;
    int          msRemaining_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusBanner)
};

} // namespace looper
