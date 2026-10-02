#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    Preview before apply, in every offline effect's dialog: Preview runs the
    effect as set on (up to ten seconds of) the selection without changing
    anything and plays it; Processed, Original and Difference switch between
    the result, the untouched audio (a bypass), and what the effect took out
    or added - the difference solo, which is how you hear that a noise
    reduction is eating the voice.

    Sits inside the dialog (AlertWindow::addCustomComponent); the owner runs
    the effect and plays (onPreview) and stops (onStop).
*/
namespace preview
{
    enum Mode
    {
        Processed  = 0,
        Original   = 1,
        Difference = 2,
    };

    /** @p original minus @p processed, channel by channel; empty when they
        aren't the same length (an effect that changes the length has no
        sample-for-sample difference). */
    inline std::vector<std::vector<float>> difference(const std::vector<std::vector<float>>& original,
                                                      const std::vector<std::vector<float>>& processed)
    {
        if (original.size() != processed.size())
            return {};
        std::vector<std::vector<float>> out(original.size());
        for (size_t ch = 0; ch < original.size(); ++ch)
        {
            if (original[ch].size() != processed[ch].size())
                return {};
            out[ch].resize(original[ch].size());
            for (size_t i = 0; i < original[ch].size(); ++i)
                out[ch][i] = original[ch][i] - processed[ch][i];
        }
        return out;
    }
}

class PreviewStrip final : public juce::Component
{
public:
    std::function<void(preview::Mode)> onPreview; // run (again) and play this
    std::function<void(preview::Mode)> onSwitch;  // play another of what was run
    std::function<void()>              onStop;

    PreviewStrip()
    {
        previewButton_.onClick = [this]
        {
            ran_ = true;
            if (onPreview)
                onPreview(mode());
        };
        stopButton_.onClick = [this] { if (onStop) onStop(); };
        previewButton_.setTooltip("Run it as set on the selection (up to ten seconds) and play it - nothing changes");

        const char* names[] { "Processed", "Original", "Difference" };
        const char* tips[] { "The result", "The audio as it is: the effect bypassed", "Only what the effect takes out or adds" };
        for (int i = 0; i < 3; ++i)
        {
            auto& b = modeButtons_[i];
            b.setButtonText(names[i]);
            b.setTooltip(tips[i]);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(0x5e1f);
            b.setConnectedEdges((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
            b.onClick = [this, i]
            {
                if (ran_ && modeButtons_[i].getToggleState() && onSwitch)
                    onSwitch((preview::Mode) i);
            };
            addAndMakeVisible(b);
        }
        modeButtons_[0].setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(previewButton_);
        addAndMakeVisible(stopButton_);
        setSize(420, 30);
    }

    ~PreviewStrip() override
    {
        if (onStop)
            onStop(); // the dialog going doesn't leave it playing
    }

    preview::Mode mode() const
    {
        for (int i = 0; i < 3; ++i)
            if (modeButtons_[i].getToggleState())
                return (preview::Mode) i;
        return preview::Processed;
    }

    /** No difference to play: the effect changes the length. */
    void setDifferenceAvailable(bool available)
    {
        modeButtons_[2].setEnabled(available);
        if (! available && modeButtons_[2].getToggleState())
            modeButtons_[0].setToggleState(true, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        previewButton_.setBounds(area.removeFromLeft(80).reduced(0, 2));
        area.removeFromLeft(8);
        stopButton_.setBounds(area.removeFromRight(56).reduced(0, 2));
        area.removeFromRight(8);
        const int w = area.getWidth() / 3;
        for (auto& b : modeButtons_)
            b.setBounds(area.removeFromLeft(w).reduced(0, 2));
    }

private:
    juce::TextButton previewButton_ { "Preview" }, stopButton_ { "Stop" };
    juce::TextButton modeButtons_[3];
    bool             ran_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PreviewStrip)
};

} // namespace soundsplice
