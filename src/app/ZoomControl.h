#pragma once

#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    One zoom control for every timeline: zoom out, a slider across the whole
    range, zoom in, and Fit. The Audio pane and the Tracks pane each have one,
    so zooming reads and works the same in both rather than as a row of loose
    buttons in one and a bare slider in the other.

    Owns no zoom: the slider runs from 0 (all the way out) to 1 (all the way
    in), and the owner maps that onto its own scale (see logPosition), so the
    same control serves a waveform measured in seconds per pixel and a
    timeline measured as a multiplier.
*/
class ZoomControl final : public juce::Component
{
public:
    std::function<void()>              onZoomIn, onZoomOut, onFit;
    std::function<void(double)>        onZoomTo; // a slider position, 0 out to 1 in

    ZoomControl()
    {
        zoomOut_.setButtonText("-");
        zoomOut_.setTooltip("Zoom out");
        zoomOut_.onClick = [this] { if (onZoomOut) onZoomOut(); };

        zoomIn_.setButtonText("+");
        zoomIn_.setTooltip("Zoom in");
        zoomIn_.onClick = [this] { if (onZoomIn) onZoomIn(); };

        fit_.setButtonText("Fit");
        fit_.setTooltip("Show all of it");
        fit_.onClick = [this] { if (onFit) onFit(); };

        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setRange(0.0, 1.0, 0.0);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_.setTooltip("Zoom - drag right to zoom in");
        slider_.onValueChange = [this] { if (! updating_ && onZoomTo) onZoomTo(slider_.getValue()); };

        for (auto* child : { (juce::Component*) &zoomOut_, (juce::Component*) &slider_,
                             (juce::Component*) &zoomIn_, (juce::Component*) &fit_ })
            addAndMakeVisible(child);
    }

    /** Shows @p position (0 out, 1 in) without reporting it back. */
    void setPosition(double position)
    {
        updating_ = true;
        slider_.setValue(juce::jlimit(0.0, 1.0, position), juce::dontSendNotification);
        updating_ = false;
        zoomIn_.setEnabled(slider_.getValue() < 1.0 - 1.0e-6);
        zoomOut_.setEnabled(slider_.getValue() > 1.0e-6);
    }

    double position() const { return slider_.getValue(); }

    void setTooltips(const juce::String& zoomIn, const juce::String& zoomOut, const juce::String& fit)
    {
        zoomIn_.setTooltip(zoomIn);
        zoomOut_.setTooltip(zoomOut);
        fit_.setTooltip(fit);
    }

    /** Where @p value sits between @p outmost and @p inmost on a log scale,
        0 to 1: zoom is multiplicative, so each doubling should be the same
        distance along the slider. Works with either end the larger. */
    static double logPosition(double value, double outmost, double inmost)
    {
        if (value <= 0.0 || outmost <= 0.0 || inmost <= 0.0 || std::abs(std::log(inmost / outmost)) < 1.0e-12)
            return 0.0;
        return juce::jlimit(0.0, 1.0, std::log(value / outmost) / std::log(inmost / outmost));
    }

    /** The inverse of logPosition. */
    static double logValue(double position, double outmost, double inmost)
    {
        if (outmost <= 0.0 || inmost <= 0.0)
            return outmost;
        return outmost * std::pow(inmost / outmost, juce::jlimit(0.0, 1.0, position));
    }

    /** The width that fits everything comfortably. */
    static constexpr int kIdealWidth = 200;

    void resized() override
    {
        auto area = getLocalBounds();
        const int button = juce::jmin(26, area.getWidth() / 6);
        fit_.setBounds(area.removeFromRight(juce::jmin(36, area.getWidth() / 4)).reduced(1));
        zoomIn_.setBounds(area.removeFromRight(button).reduced(1));
        zoomOut_.setBounds(area.removeFromLeft(button).reduced(1));
        slider_.setBounds(area.reduced(2, 0));
    }

private:
    juce::TextButton zoomOut_, zoomIn_, fit_;
    juce::Slider     slider_;
    bool             updating_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZoomControl)
};

} // namespace soundsplice
