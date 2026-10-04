#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Glyphs.h"
#include "Theme.h"

#include <cmath>
#include <functional>
#include <vector>

namespace soundsplice
{
/**
    The bar of actions that floats over a selection in the waveform, as the
    Waveform mockup has it: the selection's length, then the edits most often
    made to a selection, each a glyph and a word. What an action does is the
    owner's - each one runs a command the menus already have.
*/
class SelectionActions final : public juce::Component,
                               public juce::SettableTooltipClient
{
public:
    struct Action
    {
        juce::String          label;
        glyphs::Glyph         glyph;
        juce::String          tooltip;
        std::function<void()> run;
    };

    /** The shadow round the bar is drawn inside the component, this far out. */
    static constexpr int kShadow = 10;

    void setActions(std::vector<Action> actions)
    {
        actions_ = std::move(actions);
        repaint();
    }

    bool hasActions() const noexcept { return ! actions_.empty(); }

    void setDuration(const juce::String& duration)
    {
        if (duration != duration_)
        {
            duration_ = duration;
            repaint();
        }
    }

    /** The bar's width with its shadow, for the actions it holds. */
    int idealWidth() const
    {
        int width = 2 * kShadow + 8 + durationWidth();
        for (size_t i = 0; i < actions_.size(); ++i)
            width += actionWidth(i) + 2;
        return width;
    }

    static constexpr int idealHeight() { return 2 * kShadow + 34; }

    void paint(juce::Graphics& g) override
    {
        const auto bar = barBounds().toFloat();
        juce::Path outline;
        outline.addRoundedRectangle(bar, 9.0f);
        juce::DropShadow(juce::Colours::black.withAlpha(0.55f), kShadow, { 0, 4 }).drawForPath(g, outline);

        g.setColour(theme::colour(*this, theme::paneId).withAlpha(0.96f));
        g.fillPath(outline);
        g.setColour(juce::Colour(0xff9397ab).withAlpha(0.55f));
        g.strokePath(outline, juce::PathStrokeType(1.0f));

        const auto text = theme::colour(*this, theme::textId);
        auto       row  = barBounds().reduced(4);

        g.setColour(text.withAlpha(0.55f));
        g.setFont(theme::monoFont(10.5f));
        g.drawText(duration_, row.removeFromLeft(durationWidth()).withTrimmedLeft(6), juce::Justification::centredLeft);

        for (size_t i = 0; i < actions_.size(); ++i)
        {
            auto cell = row.removeFromLeft(actionWidth(i));
            row.removeFromLeft(2);
            const bool hot = (int) i == hovered_;
            if (hot)
            {
                g.setColour(theme::colour(*this, theme::accentId).withAlpha(down_ ? 0.28f : 0.18f));
                g.fillRoundedRectangle(cell.toFloat(), 6.0f);
            }
            const auto ink = hot ? theme::accentStep(*this, 100) : text;
            auto content = cell.reduced(9, 0);
            glyphs::draw(g, actions_[i].glyph, content.removeFromLeft(13).toFloat(), ink, 13.0f);
            content.removeFromLeft(6);
            g.setColour(ink);
            g.setFont(theme::font(11.5f));
            g.drawText(actions_[i].label, content, juce::Justification::centredLeft, false);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHovered(indexAt(e.getPosition())); }
    void mouseExit(const juce::MouseEvent&) override   { setHovered(-1); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        down_ = indexAt(e.getPosition()) >= 0;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        down_ = false;
        repaint();
        const int index = indexAt(e.getPosition());
        if (index >= 0 && e.mouseWasClicked() && actions_[(size_t) index].run)
            actions_[(size_t) index].run();
    }

    bool hitTest(int x, int y) override { return barBounds().contains(x, y); }

private:
    juce::Rectangle<int> barBounds() const { return getLocalBounds().reduced(kShadow); }

    int durationWidth() const
    {
        return 14 + (int) std::ceil(juce::GlyphArrangement::getStringWidth(theme::monoFont(10.5f),
                                                                           duration_.isEmpty() ? "0:00.000" : duration_));
    }

    int actionWidth(size_t i) const
    {
        return 18 + 13 + 6
             + (int) std::ceil(juce::GlyphArrangement::getStringWidth(theme::font(11.5f), actions_[i].label));
    }

    int indexAt(juce::Point<int> p) const
    {
        auto row = barBounds().reduced(4);
        row.removeFromLeft(durationWidth());
        for (size_t i = 0; i < actions_.size(); ++i)
        {
            if (row.removeFromLeft(actionWidth(i)).contains(p))
                return (int) i;
            row.removeFromLeft(2);
        }
        return -1;
    }

    void setHovered(int index)
    {
        if (index == hovered_)
            return;
        hovered_ = index;
        setTooltip(index >= 0 ? actions_[(size_t) index].tooltip : juce::String());
        repaint();
    }

    std::vector<Action> actions_;
    juce::String        duration_;
    int                 hovered_ = -1;
    bool                down_    = false;
};

} // namespace soundsplice
