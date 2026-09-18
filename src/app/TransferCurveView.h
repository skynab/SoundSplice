#pragma once

#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/DynamicsProcessor.h"

namespace soundsplice
{
/**
    The dynamics processor's transfer curve, drawn and edited directly: input
    level across, output level up, the straight diagonal being "no change".
    Drag a point to move it; double-click empty space to add one (up to six)
    and double-click a point to remove it (down to two).
*/
class TransferCurveView final : public juce::Component
{
public:
    static constexpr float kFloorDb = engine::TransferCurve::kFloorDb;

    std::function<void(const engine::TransferCurve&)> onChanged;
    std::function<void()>                             onDragStart, onDragEnd;

    void setCurve(const engine::TransferCurve& curve)
    {
        if (curve == curve_)
            return;
        curve_ = curve;
        repaint();
    }

    const engine::TransferCurve& curve() const noexcept { return curve_; }

    // ---- geometry, public so a test can aim at a point

    float xForDb(float db) const
    {
        const auto area = plotArea();
        return area.getX() + (juce::jlimit(kFloorDb, 0.0f, db) - kFloorDb) / -kFloorDb * area.getWidth();
    }

    float yForDb(float db) const
    {
        const auto area = plotArea();
        return area.getBottom() - (juce::jlimit(kFloorDb, 0.0f, db) - kFloorDb) / -kFloorDb * area.getHeight();
    }

    float dbForX(float x) const
    {
        const auto area = plotArea();
        return juce::jlimit(kFloorDb, 0.0f, kFloorDb + (x - area.getX()) / juce::jmax(1.0f, area.getWidth()) * -kFloorDb);
    }

    float dbForY(float y) const
    {
        const auto area = plotArea();
        return juce::jlimit(kFloorDb, 0.0f, kFloorDb + (area.getBottom() - y) / juce::jmax(1.0f, area.getHeight()) * -kFloorDb);
    }

    juce::Point<float> pointFor(int index) const
    {
        const auto& point = curve_.points[(size_t) index];
        return { xForDb(point.inDb), yForDb(point.outDb) };
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = plotArea();
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);

        g.setFont(juce::FontOptions(9.0f));
        for (float db : { -80.0f, -60.0f, -40.0f, -20.0f })
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawVerticalLine((int) xForDb(db), area.getY(), area.getBottom());
            g.drawHorizontalLine((int) yForDb(db), area.getX(), area.getRight());
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawText(juce::String((int) db), juce::Rectangle<float>(xForDb(db) + 2.0f, area.getBottom() - 11.0f, 30.0f, 10.0f),
                       juce::Justification::centredLeft);
        }

        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawLine(area.getX(), area.getBottom(), area.getRight(), area.getY(), 1.0f); // no change

        juce::Path line;
        const int  steps = juce::jmax(2, (int) area.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const float in  = kFloorDb + -kFloorDb * (float) i / (float) steps;
            const float x   = xForDb(in);
            const float y   = yForDb(curve_.outputDb(in));
            if (i == 0)
                line.startNewSubPath(x, y);
            else
                line.lineTo(x, y);
        }
        g.setColour(juce::Colours::orange);
        g.strokePath(line, juce::PathStrokeType(1.6f));

        for (int i = 0; i < curve_.count; ++i)
        {
            const auto at = pointFor(i);
            g.setColour(i == dragging_ ? juce::Colours::white : juce::Colours::orange.brighter(0.4f));
            g.fillEllipse(at.x - kPointRadius, at.y - kPointRadius, kPointRadius * 2.0f, kPointRadius * 2.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging_ = pointAt(e.position);
        if (dragging_ >= 0 && onDragStart)
            onDragStart();
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging_ < 0)
            return;
        auto curve = curve_;
        curve.points[(size_t) dragging_] = { std::round(dbForX(e.position.x) * 2.0f) / 2.0f,
                                             std::round(dbForY(e.position.y) * 2.0f) / 2.0f };
        change(curve);
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
        auto curve = curve_;
        if (const int hit = pointAt(e.position); hit >= 0)
        {
            if (curve.count <= 2)
                return;
            for (int i = hit; i < curve.count - 1; ++i)
                curve.points[(size_t) i] = curve.points[(size_t) i + 1];
            --curve.count;
        }
        else
        {
            if (curve.count >= engine::TransferCurve::kMaxPoints)
                return;
            curve.points[(size_t) curve.count] = { std::round(dbForX(e.position.x) * 2.0f) / 2.0f,
                                                   std::round(dbForY(e.position.y) * 2.0f) / 2.0f };
            ++curve.count;
        }

        if (onDragStart)
            onDragStart();
        change(curve);
        if (onDragEnd)
            onDragEnd();
    }

private:
    static constexpr float kPointRadius = 5.0f;

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(6.0f); }

    int pointAt(juce::Point<float> position) const
    {
        int   best     = -1;
        float bestDist = kPointRadius + 4.0f;
        for (int i = 0; i < curve_.count; ++i)
            if (const float distance = pointFor(i).getDistanceFrom(position); distance <= bestDist)
            {
                best     = i;
                bestDist = distance;
            }
        return best;
    }

    void change(const engine::TransferCurve& curve)
    {
        if (curve == curve_)
            return;
        curve_ = curve;
        repaint();
        if (onChanged)
            onChanged(curve_);
    }

    engine::TransferCurve curve_;
    int                   dragging_ = -1;
};

} // namespace soundsplice
