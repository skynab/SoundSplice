#pragma once

#include <cmath>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    The master's stereo picture, live: a vectorscope (each recent sample pair
    as a dot, mid up and side across, so mono is a vertical line and a wide
    mix a cloud) with the phase correlation meter under it, -1 to +1. Click
    to switch the scope to an oscilloscope, the two channels' last few
    milliseconds as waveforms, and back.

    Correlation below zero is drawn red: that much of the mix would cancel
    summed to mono (a phone, a club's PA).
*/
class StereoScopeView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Mode
    {
        Vectorscope,
        Oscilloscope
    };

    StereoScopeView()
    {
        setTooltip("Stereo picture and phase correlation - click to switch between vectorscope and oscilloscope");
    }

    /** The recent (left, right) pairs, oldest first, and the correlation. */
    void setReading(std::vector<std::pair<float, float>> pairs, double correlation)
    {
        pairs_       = std::move(pairs);
        correlation_ = correlation;
        repaint();
    }

    Mode mode() const noexcept { return mode_; }
    double correlation() const noexcept { return correlation_; }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked())
        {
            mode_ = mode_ == Mode::Vectorscope ? Mode::Oscilloscope : Mode::Vectorscope;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff111115));
        g.fillRoundedRectangle(area, 3.0f);

        auto meter = area.removeFromBottom(16.0f).reduced(4.0f, 3.0f);
        auto scope = area.reduced(4.0f);

        if (mode_ == Mode::Vectorscope)
            paintVectorscope(g, scope);
        else
            paintOscilloscope(g, scope);
        paintCorrelation(g, meter);
    }

private:
    void paintVectorscope(juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float size   = juce::jmin(area.getWidth(), area.getHeight());
        const auto  square = area.withSizeKeepingCentre(size, size);
        const float half   = size * 0.5f;

        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawVerticalLine((int) square.getCentreX(), square.getY(), square.getBottom());   // mono
        g.drawHorizontalLine((int) square.getCentreY(), square.getX(), square.getRight());  // out of phase
        g.drawLine(square.getX(), square.getBottom(), square.getRight(), square.getY(), 1.0f); // right only
        g.drawLine(square.getX(), square.getY(), square.getRight(), square.getBottom(), 1.0f); // left only

        // Newer dots brighter, so the picture moves rather than smearing.
        const size_t count = pairs_.size();
        for (size_t i = 0; i < count; i += 2)
        {
            const auto [left, right] = pairs_[i];
            const float side = (left - right) * 0.7071f;
            const float mid  = (left + right) * 0.7071f;
            const float x    = square.getCentreX() + juce::jlimit(-1.0f, 1.0f, -side) * half;
            const float y    = square.getCentreY() - juce::jlimit(-1.0f, 1.0f, mid) * half;
            g.setColour(juce::Colour(0xff7fd6ff).withAlpha(0.15f + 0.6f * (float) i / (float) juce::jmax<size_t>(1, count)));
            g.fillRect(x, y, 1.5f, 1.5f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.4f));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("L", square.withTrimmedRight(size * 0.75f).withHeight(12.0f), juce::Justification::centredLeft);
        g.drawText("R", square.withTrimmedLeft(size * 0.75f).withHeight(12.0f), juce::Justification::centredRight);
    }

    void paintOscilloscope(juce::Graphics& g, juce::Rectangle<float> area)
    {
        // The last ~20 ms at 48 kHz: enough for a few cycles of a voice.
        const size_t shown = juce::jmin<size_t>(pairs_.size(), 1024);
        const size_t first = pairs_.size() - shown;
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto lane = channel == 0 ? area.withHeight(area.getHeight() * 0.5f)
                                           : area.withTrimmedTop(area.getHeight() * 0.5f);
            g.setColour(juce::Colours::white.withAlpha(0.1f));
            g.drawHorizontalLine((int) lane.getCentreY(), lane.getX(), lane.getRight());

            juce::Path wave;
            for (size_t i = 0; i < shown; ++i)
            {
                const float value = channel == 0 ? pairs_[first + i].first : pairs_[first + i].second;
                const float x     = lane.getX() + lane.getWidth() * (float) i / (float) juce::jmax<size_t>(1, shown - 1);
                const float y     = lane.getCentreY() - juce::jlimit(-1.0f, 1.0f, value) * lane.getHeight() * 0.45f;
                if (i == 0)
                    wave.startNewSubPath(x, y);
                else
                    wave.lineTo(x, y);
            }
            g.setColour(channel == 0 ? juce::Colour(0xff7fd6ff) : juce::Colour(0xffffb36b));
            g.strokePath(wave, juce::PathStrokeType(1.0f));
        }
    }

    void paintCorrelation(juce::Graphics& g, juce::Rectangle<float> bar)
    {
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.fillRect(bar);

        const float centre = bar.getCentreX();
        const float x      = centre + (float) juce::jlimit(-1.0, 1.0, correlation_) * bar.getWidth() * 0.5f;
        g.setColour(correlation_ < 0.0 ? juce::Colours::red.withAlpha(0.85f) : juce::Colour(0xff6fd08c));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(juce::jmin(centre, x), bar.getY(), juce::jmax(centre, x), bar.getBottom()));

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.drawVerticalLine((int) centre, bar.getY(), bar.getBottom());
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("-1", bar, juce::Justification::centredLeft);
        g.drawText("+1", bar, juce::Justification::centredRight);
    }

    std::vector<std::pair<float, float>> pairs_;
    double                               correlation_ = 0.0;
    Mode                                 mode_        = Mode::Vectorscope;
};

} // namespace soundsplice
