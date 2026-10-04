#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace soundsplice
{
/**
    A simple stereo peak meter. The owner feeds it linear peak values on a timer
    via setLevel(); the widget handles ballistics (fast attack, slow decay) and a
    held peak marker, and maps to a dB scale for display.

    With a clip light (setShowsClipping), a channel that reached full scale
    lights red at the loud end and stays lit until clicked - a clip in the
    middle of a take is exactly the thing that's over before anyone sees it.
*/
class LevelMeter final : public juce::Component,
                         public juce::SettableTooltipClient
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

    /** Bars left to right, for a row, rather than bottom to top. */
    void setHorizontal(bool horizontal) { horizontal_ = horizontal; repaint(); }

    /** Adds the clip light, and lets a click clear it. */
    void setShowsClipping(bool shows)
    {
        showsClipping_ = shows;
        setInterceptsMouseClicks(shows, false);
        repaint();
    }

    /** Lights @p channel's clip light; it stays lit until clicked. */
    void setClipped(int channel)
    {
        if (channel >= 0 && channel < numChannels && ! clipped_[channel])
        {
            clipped_[channel] = true;
            repaint();
        }
    }

    bool isClipped(int channel) const noexcept { return channel >= 0 && channel < numChannels && clipped_[channel]; }

    void mouseDown(const juce::MouseEvent&) override
    {
        clipped_[0] = clipped_[1] = false;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(area, 3.0f);

        const float gap    = horizontal_ ? 2.0f : 4.0f;
        const float across = ((horizontal_ ? area.getHeight() : area.getWidth()) - gap) / (float) numChannels;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto bar = horizontal_
                         ? juce::Rectangle<float>(area.getX(), area.getY() + (float) ch * (across + gap), area.getWidth(), across)
                         : juce::Rectangle<float>(area.getX() + (float) ch * (across + gap), area.getY(), across, area.getHeight());
            bar = bar.reduced(1.0f);

            // The clip light takes the loud end of the bar.
            if (showsClipping_)
            {
                const auto light = horizontal_ ? bar.removeFromRight(juce::jmin(8.0f, bar.getWidth() * 0.2f))
                                               : bar.removeFromTop(juce::jmin(6.0f, bar.getHeight() * 0.2f));
                g.setColour(clipped_[ch] ? theme::colour(*this, theme::dangerId) : juce::Colours::white.withAlpha(0.12f));
                g.fillRect(light.reduced(horizontal_ ? 1.0f : 0.0f, horizontal_ ? 0.0f : 1.0f));
            }

            const float levelNorm = normalise(level_[ch]);
            const float holdNorm  = normalise(hold_[ch]);
            g.setColour(colourFor(levelNorm));
            if (horizontal_)
                g.fillRect(bar.getX(), bar.getY(), bar.getWidth() * levelNorm, bar.getHeight());
            else
                g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * levelNorm, bar.getWidth(), bar.getHeight() * levelNorm);

            g.setColour(juce::Colours::white.withAlpha(0.85f));
            if (horizontal_)
                g.fillRect(bar.getX() + bar.getWidth() * holdNorm - 1.0f, bar.getY(), 2.0f, bar.getHeight());
            else
                g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * holdNorm - 1.0f, bar.getWidth(), 2.0f);
        }
    }

private:
    static float normalise(float linear)
    {
        const float db = juce::Decibels::gainToDecibels(linear, minDb);
        return juce::jlimit(0.0f, 1.0f, (db - minDb) / -minDb);
    }

    juce::Colour colourFor(float norm) const
    {
        if (norm > 0.9f) return theme::colour(*this, theme::dangerId);
        if (norm > 0.7f) return theme::colour(*this, theme::warnId);
        return theme::colour(*this, theme::signalId);
    }

    static constexpr int   numChannels = 2;
    static constexpr float minDb       = -60.0f;
    static constexpr float decay_      = 0.85f;
    static constexpr int   holdFrames_ = 30;

    bool  horizontal_    = false;
    bool  showsClipping_ = false;
    bool  clipped_[numChannels]       { false, false };
    float level_[numChannels]         { 0.0f, 0.0f };
    float hold_[numChannels]          { 0.0f, 0.0f };
    int   holdCountdown_[numChannels] { 0, 0 };
};

} // namespace soundsplice
