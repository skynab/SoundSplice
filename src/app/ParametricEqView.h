#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/ParametricEq.h"

namespace soundsplice
{
/**
    The parametric EQ's curve, drawn and edited directly: each band that's
    on is a numbered point on the curve. Drag a point to move its frequency
    and (for a bell or shelf) its gain; the mouse wheel over a point widens
    or narrows it; double-click empty space to turn on the next band that's
    off as a bell there, and double-click a point to turn it off.

    The curve is engine::parametricMagnitudeDb, the filters the effect runs,
    so what's drawn is what's heard.
*/
class ParametricEqView final : public juce::Component
{
public:
    using Bands = engine::ParametricEq::Bands;

    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;
    static constexpr float kRangeDb = 24.0f;

    /** Fired with the new bands on every change. */
    std::function<void(const Bands&)> onChanged;
    /** Bracket a drag, so it's one undo step. */
    std::function<void()> onDragStart, onDragEnd;

    void setBands(const Bands& bands)
    {
        if (bands == bands_)
            return;
        bands_ = bands;
        repaint();
    }

    const Bands& bands() const noexcept { return bands_; }

    // ---- geometry, public so a test can aim at a band

    float xForHz(float hz) const
    {
        const auto area = plotArea();
        const float p   = std::log(juce::jlimit(kMinHz, kMaxHz, hz) / kMinHz) / std::log(kMaxHz / kMinHz);
        return area.getX() + p * area.getWidth();
    }

    float hzForX(float x) const
    {
        const auto area = plotArea();
        const float p   = juce::jlimit(0.0f, 1.0f, (x - area.getX()) / juce::jmax(1.0f, area.getWidth()));
        return kMinHz * std::pow(kMaxHz / kMinHz, p);
    }

    float yForDb(float db) const
    {
        const auto area = plotArea();
        return area.getCentreY() - juce::jlimit(-kRangeDb, kRangeDb, db) / kRangeDb * area.getHeight() * 0.5f;
    }

    float dbForY(float y) const
    {
        const auto area = plotArea();
        return juce::jlimit(-kRangeDb, kRangeDb, (area.getCentreY() - y) / (area.getHeight() * 0.5f) * kRangeDb);
    }

    /** Where band @p index's point is drawn. */
    juce::Point<float> pointFor(int index) const
    {
        const auto& band = bands_[(size_t) index];
        const float db   = engine::ParametricBand::hasGain(band.type) ? band.gainDb : 0.0f;
        return { xForHz(band.hz), yForDb(db) };
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = plotArea();
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);

        g.setFont(juce::FontOptions(9.0f));
        for (float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = xForHz(hz);
            g.setColour(juce::Colours::white.withAlpha(0.1f));
            g.drawVerticalLine((int) x, area.getY(), area.getBottom());
            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.drawText(hz >= 1000.0f ? juce::String((int) (hz / 1000.0f)) + "k" : juce::String((int) hz),
                       juce::Rectangle<float>(x + 2.0f, area.getBottom() - 11.0f, 30.0f, 10.0f),
                       juce::Justification::centredLeft);
        }
        for (float db : { -12.0f, 0.0f, 12.0f })
        {
            g.setColour(juce::Colours::white.withAlpha(db == 0.0f ? 0.25f : 0.1f));
            g.drawHorizontalLine((int) yForDb(db), area.getX(), area.getRight());
        }

        juce::Path curve;
        const int  steps = juce::jmax(2, (int) area.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const float x  = area.getX() + area.getWidth() * (float) i / (float) steps;
            const float db = engine::parametricMagnitudeDb(bands_, hzForX(x));
            const float y  = juce::jlimit(area.getY(), area.getBottom(), yForDb(db));
            if (i == 0)
                curve.startNewSubPath(x, y);
            else
                curve.lineTo(x, y);
        }
        g.setColour(juce::Colours::orange);
        g.strokePath(curve, juce::PathStrokeType(1.6f));

        for (int b = 0; b < engine::ParametricEq::kBands; ++b)
        {
            if (bands_[(size_t) b].type == engine::ParametricBand::Type::Off)
                continue;
            const auto at = pointFor(b);
            g.setColour(b == dragging_ ? juce::Colours::white : juce::Colours::orange.brighter(0.4f));
            g.fillEllipse(at.x - kPointRadius, at.y - kPointRadius, kPointRadius * 2.0f, kPointRadius * 2.0f);
            g.setColour(juce::Colours::black);
            g.drawText(juce::String(b + 1), juce::Rectangle<float>(at.x - kPointRadius, at.y - kPointRadius,
                                                                   kPointRadius * 2.0f, kPointRadius * 2.0f),
                       juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging_ = bandAt(e.position);
        if (dragging_ >= 0 && onDragStart)
            onDragStart();
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging_ < 0)
            return;
        auto bands = bands_;
        auto& band = bands[(size_t) dragging_];
        band.hz = std::round(hzForX(e.position.x));
        if (engine::ParametricBand::hasGain(band.type))
            band.gainDb = std::round(dbForY(e.position.y) * 10.0f) / 10.0f;
        change(bands);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_ >= 0 && onDragEnd)
            onDragEnd();
        dragging_ = -1;
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        auto bands = bands_;
        if (const int hit = bandAt(e.position); hit >= 0)
        {
            bands[(size_t) hit].type = engine::ParametricBand::Type::Off;
        }
        else
        {
            const auto off = std::find_if(bands.begin(), bands.end(), [](const engine::ParametricBand& band)
                                          { return band.type == engine::ParametricBand::Type::Off; });
            if (off == bands.end())
                return;
            *off = { engine::ParametricBand::Type::Bell, std::round(hzForX(e.position.x)),
                     std::round(dbForY(e.position.y) * 10.0f) / 10.0f, 1.0f };
        }
        instantChange(bands);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int hit = bandAt(e.position);
        if (hit < 0 || wheel.deltaY == 0.0f)
        {
            Component::mouseWheelMove(e, wheel); // off a point, it scrolls whatever holds the curve
            return;
        }
        auto bands = bands_;
        auto& q    = bands[(size_t) hit].q;
        q          = juce::jlimit(0.1f, 30.0f, q * (wheel.deltaY > 0.0f ? 1.15f : 1.0f / 1.15f));
        instantChange(bands);
    }

private:
    static constexpr float kPointRadius = 7.0f;

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(4.0f); }

    int bandAt(juce::Point<float> position) const
    {
        int   best     = -1;
        float bestDist = kPointRadius + 3.0f;
        for (int b = 0; b < engine::ParametricEq::kBands; ++b)
        {
            if (bands_[(size_t) b].type == engine::ParametricBand::Type::Off)
                continue;
            const float distance = pointFor(b).getDistanceFrom(position);
            if (distance <= bestDist)
            {
                best     = b;
                bestDist = distance;
            }
        }
        return best;
    }

    void change(const Bands& bands)
    {
        if (bands == bands_)
            return;
        bands_ = bands;
        repaint();
        if (onChanged)
            onChanged(bands_);
    }

    /** A change with no drag around it: bracketed as its own undo step. */
    void instantChange(const Bands& bands)
    {
        if (onDragStart)
            onDragStart();
        change(bands);
        if (onDragEnd)
            onDragEnd();
    }

    Bands bands_ {};
    int   dragging_ = -1;
};

} // namespace soundsplice
