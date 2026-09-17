#pragma once

#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Loudness.h"

namespace soundsplice
{
/**
    The master bus's loudness, live: momentary, short-term and integrated
    LUFS, loudness range and true peak, as the loudness meters of Audition and
    REAPER show them. Integrated loudness and range count from the last reset,
    so Reset before playing the passage to measure.

    The true peak turns red over -1 dBTP, the ceiling streaming services and
    Normalize Loudness hold to.
*/
class LoudnessReadout final : public juce::Component
{
public:
    std::function<void()> onReset;

    LoudnessReadout()
    {
        resetButton_.setButtonText("Reset");
        resetButton_.setTooltip("Start measuring integrated loudness, range and true peak afresh");
        resetButton_.onClick = [this] { if (onReset) onReset(); };
        addAndMakeVisible(resetButton_);

        setReading({});
    }

    void setReading(const engine::LiveLoudness& reading)
    {
        if (hasReading_ && sameAs(reading))
            return;

        reading_    = reading;
        hasReading_ = true;
        repaint();
    }

    /** The values as drawn, for tests. */
    juce::String text() const
    {
        return "M " + lufs(reading_.momentaryLufs) + "  S " + lufs(reading_.shortTermLufs) + "  I "
             + lufs(reading_.integratedLufs) + " LUFS  LRA " + juce::String(reading_.loudnessRangeLu, 1)
             + "  TP " + lufs(reading_.truePeakDb);
    }

    bool truePeakOverCeiling() const noexcept { return reading_.truePeakDb > kCeilingDbtp; }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().withTrimmedRight(resetButton_.getWidth() + 6).toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(area, 3.0f);
        area.reduce(6.0f, 0.0f);

        struct Cell { const char* name; juce::String value; bool warn; };
        const Cell cells[] {
            { "M",   lufs(reading_.momentaryLufs), false },
            { "S",   lufs(reading_.shortTermLufs), false },
            { "I",   lufs(reading_.integratedLufs), false },
            { "LRA", juce::String(reading_.loudnessRangeLu, 1), false },
            { "TP",  lufs(reading_.truePeakDb), truePeakOverCeiling() },
        };

        const float width = area.getWidth() / (float) std::size(cells);
        for (const auto& cell : cells)
        {
            auto box = area.removeFromLeft(width);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawText(cell.name, box.removeFromLeft(juce::jmin(26.0f, box.getWidth() * 0.4f)),
                       juce::Justification::centredLeft, false);

            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.setColour(cell.warn ? juce::Colour(0xffff5a4a) : juce::Colours::white.withAlpha(0.9f));
            g.drawText(cell.value, box, juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        resetButton_.setBounds(getLocalBounds().removeFromRight(juce::jmin(56, getWidth() / 4)));
    }

private:
    static constexpr double kCeilingDbtp = -1.0;

    static juce::String lufs(double value)
    {
        return std::isfinite(value) ? juce::String(value, 1) : juce::String("-inf");
    }

    /** Equal to a tenth, as drawn, so a steady reading doesn't repaint 30 times a second. */
    bool sameAs(const engine::LiveLoudness& other) const noexcept
    {
        const auto close = [](double a, double b)
        {
            return (! std::isfinite(a) && ! std::isfinite(b)) || std::abs(a - b) < 0.05;
        };
        return close(reading_.momentaryLufs, other.momentaryLufs) && close(reading_.shortTermLufs, other.shortTermLufs)
            && close(reading_.integratedLufs, other.integratedLufs)
            && close(reading_.loudnessRangeLu, other.loudnessRangeLu) && close(reading_.truePeakDb, other.truePeakDb);
    }

    engine::LiveLoudness reading_;
    bool                 hasReading_ = false;
    juce::TextButton     resetButton_;
};

} // namespace soundsplice
