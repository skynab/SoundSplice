#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Spectrum.h"

namespace looper
{
/**
    The frequency content of a passage of audio.

    Its own pane rather than another row in the Audio editor: a spectrum is
    only readable at a decent size, and the audio pane is already carrying a
    waveform plus twenty controls. The workspace layouts exist exactly so a
    pane like this can be present when editing audio and absent when
    composing.

    Log frequency across, dB down — the same conventions EqCurveView uses, so
    a resonance found here lines up visually with the mastering EQ band you'd
    reach for to remove it.

    Owns no state beyond the spectrum it was handed; MainComponent computes it
    from the audio editor's selection.
*/
class AnalyserPane final : public juce::Component
{
public:
    /** Analyse whatever is currently selected in the audio editor. */
    std::function<void()> onAnalyseRequested;

    AnalyserPane()
    {
        analyseButton_.setButtonText("Analyse Selection");
        analyseButton_.setTooltip("Measure the frequency content of the selected audio");
        analyseButton_.onClick = [this] { if (onAnalyseRequested) onAnalyseRequested(); };
        addAndMakeVisible(analyseButton_);

        readoutLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        readoutLabel_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(readoutLabel_);

        updateReadout();
    }

    void setSpectrum(engine::Spectrum spectrum)
    {
        spectrum_ = std::move(spectrum);
        updateReadout();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));

        const auto area = graphBounds().toFloat();
        if (area.isEmpty())
            return;

        g.setColour(juce::Colour(0xff121216));
        g.fillRoundedRectangle(area, 3.0f);

        paintGrid(g, area);

        if (spectrum_.isEmpty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.drawText("Select audio and press Analyse", area, juce::Justification::centred);
            return;
        }

        juce::Path path;
        const int  steps = juce::jmax(2, (int) area.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const float proportion = (float) i / (float) steps;
            const float hz         = hzForProportion(proportion);
            const float db         = spectrum_.magnitudeDbAtHz(hz);

            const float x = area.getX() + proportion * area.getWidth();
            const float y = yForDb(db, area);

            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }

        g.setColour(juce::Colours::aquamarine.withAlpha(0.9f));
        g.strokePath(path, juce::PathStrokeType(1.5f));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);

        auto row = area.removeFromTop(kRowHeight);
        analyseButton_.setBounds(row.removeFromLeft(juce::jmax(1, row.getWidth() / 3)).reduced(1));
        readoutLabel_.setBounds(row.reduced(4, 0));
    }

private:
    static constexpr int   kRowHeight = 24;
    static constexpr float kMinDb     = -90.0f;
    static constexpr float kMaxDb     = 6.0f;
    static constexpr float kMinHz     = 20.0f;
    static constexpr float kMaxHz     = 20000.0f;

    juce::Rectangle<int> graphBounds() const
    {
        auto area = getLocalBounds().reduced(6);
        area.removeFromTop(kRowHeight);
        return area;
    }

    static float hzForProportion(float proportion)
    {
        // Log-spaced, the way a frequency axis is always read — linear would
        // give three quarters of the width to everything above 5kHz.
        return kMinHz * std::pow(kMaxHz / kMinHz, proportion);
    }

    static float proportionForHz(float hz)
    {
        return std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz);
    }

    static float yForDb(float db, juce::Rectangle<float> area)
    {
        const float clamped = juce::jlimit(kMinDb, kMaxDb, db);
        return area.getBottom() - (clamped - kMinDb) / (kMaxDb - kMinDb) * area.getHeight();
    }

    void paintGrid(juce::Graphics& g, juce::Rectangle<float> area)
    {
        // Decade lines, labelled — a spectrum with no axis tells you a bump
        // exists but not where, which is the one thing you came to find out.
        g.setFont(juce::FontOptions(9.0f));
        for (float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = area.getX() + proportionForHz(hz) * area.getWidth();
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.drawVerticalLine((int) x, area.getY(), area.getBottom());

            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText(hz >= 1000.0f ? juce::String((int) (hz / 1000.0f)) + "k"
                                     : juce::String((int) hz),
                       juce::Rectangle<float>(x + 2.0f, area.getBottom() - 12.0f, 30.0f, 11.0f),
                       juce::Justification::centredLeft);
        }

        for (float db : { 0.0f, -24.0f, -48.0f, -72.0f })
        {
            const float y = yForDb(db, area);
            g.setColour(juce::Colours::white.withAlpha(db == 0.0f ? 0.22f : 0.08f));
            g.drawHorizontalLine((int) y, area.getX(), area.getRight());

            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.drawText(juce::String((int) db),
                       juce::Rectangle<float>(area.getX() + 2.0f, y - 11.0f, 30.0f, 11.0f),
                       juce::Justification::centredLeft);
        }
    }

    void updateReadout()
    {
        if (spectrum_.isEmpty())
        {
            readoutLabel_.setText("No analysis yet", juce::dontSendNotification);
            return;
        }

        const double hz = spectrum_.dominantFrequency();
        readoutLabel_.setText("Loudest: " + juce::String(hz, 1) + " Hz  ("
                                  + juce::String(spectrum_.magnitudeDbAtHz(hz), 1) + " dBFS)",
                              juce::dontSendNotification);
    }

    engine::Spectrum spectrum_;

    juce::TextButton analyseButton_;
    juce::Label      readoutLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyserPane)
};

} // namespace looper
