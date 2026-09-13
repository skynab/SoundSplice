#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/EqCurve.h"
#include "model/Effects.h"

namespace looper
{
/**
    Draws the master EQ's frequency-response curve — the standard visual
    every EQ plugin has, so a boost or cut reads as a shape rather than three
    numbers you have to imagine. The owner feeds it a fresh model::EqSettings
    via setSettings() whenever a slider moves; the curve itself is sampled
    from engine::eqMagnitudeDb(), the same math the master bus actually
    applies (see EqCurve.h), so what's drawn can't drift from what's heard.
*/
class EqCurveView final : public juce::Component
{
public:
    EqCurveView() { setInterceptsMouseClicks(false, false); }

    void setSettings(const model::EqSettings& eq)
    {
        settings_ = eq;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        if (area.isEmpty())
            return;

        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(area, 3.0f);

        // The 0dB reference line — every curve pivots around it.
        const float zeroY = yForDb(0.0f, area);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawLine(area.getX(), zeroY, area.getRight(), zeroY);

        // Faint markers at the two fixed crossovers, so the three bands each
        // control read as "this side of this line" rather than needing the
        // numbers memorised.
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawLine(xForHz(model::EqSettings::bassHz, area), area.getY(),
                   xForHz(model::EqSettings::bassHz, area), area.getBottom());
        g.drawLine(xForHz(model::EqSettings::trebleHz, area), area.getY(),
                   xForHz(model::EqSettings::trebleHz, area), area.getBottom());

        juce::Path curve;
        constexpr int numPoints = 96;
        for (int i = 0; i < numPoints; ++i)
        {
            const float t  = (float) i / (float) (numPoints - 1);
            const float hz = minHz * std::pow(maxHz / minHz, t); // log-spaced
            const float db = settings_.enabled ? engine::eqMagnitudeDb(settings_, hz) : 0.0f;

            const float x = xForHz(hz, area);
            const float y = yForDb(db, area);
            if (i == 0) curve.startNewSubPath(x, y);
            else        curve.lineTo(x, y);
        }

        g.setColour(settings_.enabled ? juce::Colours::orange : juce::Colours::grey);
        g.strokePath(curve, juce::PathStrokeType(1.5f));
    }

private:
    static constexpr float minHz  = 20.0f;
    static constexpr float maxHz  = 20000.0f;
    static constexpr float rangeDb = 18.0f; // matches the sliders' -18..+18 range

    static float xForHz(float hz, juce::Rectangle<float> area)
    {
        const float t = std::log(hz / minHz) / std::log(maxHz / minHz);
        return area.getX() + t * area.getWidth();
    }

    static float yForDb(float db, juce::Rectangle<float> area)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (db + rangeDb) / (2.0f * rangeDb));
        return area.getBottom() - t * area.getHeight();
    }

    model::EqSettings settings_;
};

} // namespace looper
