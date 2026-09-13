#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace looper
{
/**
    A simple stereo peak meter. The owner feeds it linear peak values on a timer
    via setLevel(); the widget handles ballistics (fast attack, slow decay) and a
    held peak marker, and maps to a dB scale for display.
*/
class LevelMeter final : public juce::Component
{
public:
    LevelMeter() { setInterceptsMouseClicks(false, false); }

    /** Feed a fresh linear peak for a channel (0 or 1). */
    void setLevel(int channel, float linearPeak)
    {
        if (channel < 0 || channel >= numChannels)
            return;

        level_[channel] = juce::jmax(linearPeak, level_[channel] * decay_);

        if (level_[channel] >= hold_[channel])
        {
            hold_[channel]        = level_[channel];
            holdCountdown_[channel] = holdFrames_;
        }
        else if (holdCountdown_[channel] > 0)
        {
            --holdCountdown_[channel];
        }
        else
        {
            hold_[channel] = juce::jmax(0.0f, hold_[channel] - 0.02f);
        }

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(area, 3.0f);

        const float gap   = 4.0f;
        const float width = (area.getWidth() - gap) / (float) numChannels;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto bar = juce::Rectangle<float>(area.getX() + (float) ch * (width + gap),
                                              area.getY(), width, area.getHeight()).reduced(1.0f);

            const float levelNorm = normalise(level_[ch]);
            const float h         = bar.getHeight() * levelNorm;

            g.setColour(colourFor(levelNorm));
            g.fillRect(bar.getX(), bar.getBottom() - h, bar.getWidth(), h);

            const float peakY = bar.getBottom() - bar.getHeight() * normalise(hold_[ch]);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.fillRect(bar.getX(), peakY - 1.0f, bar.getWidth(), 2.0f);
        }
    }

private:
    static float normalise(float linear)
    {
        const float db = juce::Decibels::gainToDecibels(linear, minDb);
        return juce::jlimit(0.0f, 1.0f, (db - minDb) / -minDb);
    }

    static juce::Colour colourFor(float norm)
    {
        if (norm > 0.9f) return juce::Colours::red;
        if (norm > 0.7f) return juce::Colours::yellow;
        return juce::Colours::limegreen;
    }

    static constexpr int   numChannels = 2;
    static constexpr float minDb       = -60.0f;
    static constexpr float decay_      = 0.85f;
    static constexpr int   holdFrames_ = 30;

    float level_[numChannels]         { 0.0f, 0.0f };
    float hold_[numChannels]          { 0.0f, 0.0f };
    int   holdCountdown_[numChannels] { 0, 0 };
};

} // namespace looper
