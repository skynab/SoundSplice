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
    The window's chrome, as the SoundSplice mockups lay it out: a title strip
    with the menus and the document's name, a toolbar, the workspace's panes,
    a transport bar and a status line. Everything here is drawn on the
    window's own ground (color-bg); the panes float on the darker workspace
    between the toolbar and the transport.

    These are only the widgets. What they show and what their buttons do is
    MainComponent's (see MainComponent_Shell.cpp), which already owns the
    commands they stand for.
*/

namespace shell
{
    inline constexpr int kTitleHeight     = 34;
    inline constexpr int kToolbarHeight   = 44;
    inline constexpr int kTransportHeight = 52;
    inline constexpr int kStatusHeight    = 24;

    inline void ruleBelow(juce::Graphics& g, const juce::Component& c)
    {
        g.setColour(theme::colour(c, theme::dividerId));
        g.fillRect(0, c.getHeight() - 1, c.getWidth(), 1);
    }

    inline void ruleAbove(juce::Graphics& g, const juce::Component& c)
    {
        g.setColour(theme::colour(c, theme::dividerId));
        g.fillRect(0, 0, c.getWidth(), 1);
    }
}

/** The top strip: the app's name, the menus, the document's place in its
    project in the middle, and whether it's saved on the right. */
class TitleStrip final : public juce::Component
{
public:
    explicit TitleStrip(juce::MenuBarComponent& menus) : menus_(menus) { addAndMakeVisible(menus_); }

    void setDocument(const juce::String& project, const juce::String& document, bool dirty)
    {
        if (project == project_ && document == document_ && dirty == dirty_)
            return;
        project_  = project;
        document_ = document;
        dirty_    = dirty;
        repaint();
    }

    void setStatus(const juce::String& status)
    {
        if (status == status_)
            return;
        status_ = status;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto ground = findColour(juce::ResizableWindow::backgroundColourId);
        g.setGradientFill(juce::ColourGradient::vertical(juce::Colour(0xff292b31), 0.0f, ground, (float) getHeight()));
        g.fillAll();
        shell::ruleBelow(g, *this);

        const auto text = theme::colour(*this, theme::textId);
        g.setColour(text);
        g.setFont(theme::font(12.5f, true));
        g.drawText("SoundSplice", brandBounds(), juce::Justification::centredLeft);

        // The document, centred on the window: its project muted, a slash,
        // then its own name, and a dot in the accent while it's unsaved.
        const auto font  = theme::font(12.0f);
        const auto slash = juce::String("  /  ");
        juce::AttributedString crumb;
        crumb.setJustification(juce::Justification::centred);
        if (project_.isNotEmpty())
        {
            crumb.append(project_, font, text.withAlpha(0.55f));
            crumb.append(slash, font, text.withAlpha(0.30f));
        }
        crumb.append(document_, font, text);
        if (dirty_)
            crumb.append(juce::String::fromUTF8(" \xe2\x80\xa2"), font, theme::accentStep(*this, 300));
        crumb.draw(g, getLocalBounds().withSizeKeepingCentre(juce::jmin(560, getWidth() / 2), getHeight()).toFloat());

        g.setColour(text.withAlpha(0.45f));
        g.setFont(theme::font(11.0f));
        g.drawText(status_, getLocalBounds().withTrimmedRight(14).removeFromRight(220), juce::Justification::centredRight);
    }

    void resized() override
    {
        int width = 0;
        auto* model = menus_.getModel();
        if (model != nullptr)
        {
            auto& laf = getLookAndFeel();
            const auto names = model->getMenuBarNames();
            for (int i = 0; i < names.size(); ++i)
                width += laf.getMenuBarItemWidth(menus_, i, names[i]);
        }
        menus_.setBounds(brandBounds().getRight() + 4, 0, width, getHeight());
    }

private:
    juce::Rectangle<int> brandBounds() const { return { 14, 0, 86, getHeight() }; }

    juce::MenuBarComponent& menus_;
    juce::String            project_, document_ { "Untitled" }, status_;
    bool                    dirty_ = false;
};

/**
    An icon button: a glyph with no box until the mouse is over it. A toggle
    lights in the accent; the play button's variant has an accent outline,
    and a tint (record's red) can take the glyph's colour.
*/
class GlyphButton final : public juce::Button
{
public:
    GlyphButton(const juce::String& name, glyphs::Glyph glyph) : juce::Button(name), glyph_(glyph)
    {
        setTooltip(name);
    }

    void setGlyph(glyphs::Glyph glyph) { glyph_ = glyph; repaint(); }
    void setOutlined(bool outlined)    { outlined_ = outlined; repaint(); }
    void setTint(juce::Colour tint)    { tint_ = tint; repaint(); }
    void setGlyphSize(float size)      { glyphSize_ = size; repaint(); }

    /** On without being a click-to-toggle button: the state follows whatever
        it stands for (the engine looping, say), set from outside. */
    void setLit(bool lit)
    {
        if (lit != lit_)
        {
            lit_ = lit;
            repaint();
        }
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        const auto accent = theme::colour(*this, theme::accentId);
        const auto text   = theme::colour(*this, theme::textId);
        const bool on     = lit_ || getToggleState();

        juce::Colour fill = juce::Colours::transparentBlack;
        if (down)
            fill = accent.withAlpha(0.22f);
        else if (on && ! outlined_)
            fill = accent.withAlpha(0.18f);
        else if (highlighted)
            fill = text.withAlpha(0.08f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 6.0f);

        if (outlined_)
        {
            g.setColour(accent);
            g.drawRoundedRectangle(bounds, 6.0f, 1.0f);
        }

        juce::Colour ink = ! tint_.isTransparent() ? tint_
                         : outlined_ || on         ? theme::accentStep(accent, 200)
                         : highlighted             ? text
                                                   : text.withAlpha(0.62f);
        if (! isEnabled())
            ink = ink.withMultipliedAlpha(0.4f);
        glyphs::draw(g, glyph_, bounds, ink, glyphSize_);
    }

private:
    glyphs::Glyph glyph_;
    juce::Colour  tint_;
    bool          outlined_  = false;
    bool          lit_       = false;
    float         glyphSize_ = 15.0f;
};

/**
    A row of options in a recessed track, the chosen one raised onto the
    surface: the Waveform / Multitrack switch, and the Waveform / Spectral
    view. Each option may carry a glyph.
*/
class SegmentedControl final : public juce::Component
{
public:
    struct Option
    {
        juce::String  label;
        bool          hasGlyph = false;
        glyphs::Glyph glyph    = glyphs::Glyph::layout;
    };

    std::function<void(int)> onChange;

    void setOptions(std::vector<Option> options, float fontSize = 12.0f)
    {
        options_  = std::move(options);
        fontSize_ = fontSize;
        repaint();
    }

    void setSelected(int index)
    {
        if (index != selected_)
        {
            selected_ = index;
            repaint();
        }
    }

    int selected() const noexcept { return selected_; }

    /** The width the options want at their font size. */
    int idealWidth() const
    {
        int width = 4;
        for (int i = 0; i < (int) options_.size(); ++i)
            width += optionWidth(i);
        return width;
    }

    void paint(juce::Graphics& g) override
    {
        const auto text   = theme::colour(*this, theme::textId);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(text.withAlpha(0.05f));
        g.fillRoundedRectangle(bounds, 8.0f);

        for (int i = 0; i < (int) options_.size(); ++i)
        {
            const auto cell = optionBounds(i).toFloat();
            const bool on   = i == selected_;
            if (on)
            {
                g.setColour(theme::colour(*this, theme::paneId));
                g.fillRoundedRectangle(cell, 6.0f);
                g.setColour(juce::Colour(0xff3f424d));
                g.drawRoundedRectangle(cell.reduced(0.5f), 6.0f, 1.0f);
            }
            else if (i == hovered_)
            {
                g.setColour(text.withAlpha(0.05f));
                g.fillRoundedRectangle(cell, 6.0f);
            }

            const auto ink = on ? (options_[(size_t) i].hasGlyph ? theme::accentStep(*this, 200) : text)
                                : text.withAlpha(i == hovered_ ? 0.85f : 0.60f);
            auto content = cell.reduced(10.0f, 0.0f);
            if (options_[(size_t) i].hasGlyph)
            {
                glyphs::draw(g, options_[(size_t) i].glyph, content.removeFromLeft(14.0f), ink, 14.0f);
                content.removeFromLeft(6.0f);
            }
            g.setColour(ink);
            g.setFont(theme::font(fontSize_));
            g.drawText(options_[(size_t) i].label, content, juce::Justification::centredLeft, false);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { setHovered(indexAt(e.getPosition())); }
    void mouseExit(const juce::MouseEvent&) override   { setHovered(-1); }

    void mouseUp(const juce::MouseEvent& e) override
    {
        const int index = indexAt(e.getPosition());
        if (index < 0 || index == selected_ || ! e.mouseWasClicked())
            return;
        setSelected(index);
        if (onChange)
            onChange(index);
    }

private:
    int optionWidth(int i) const
    {
        const auto& option = options_[(size_t) i];
        const float label  = juce::GlyphArrangement::getStringWidth(theme::font(fontSize_), option.label);
        return (int) std::ceil(label) + 20 + (option.hasGlyph ? 20 : 0);
    }

    juce::Rectangle<int> optionBounds(int i) const
    {
        int x = 2;
        for (int j = 0; j < i; ++j)
            x += optionWidth(j);
        return { x, 2, optionWidth(i), getHeight() - 4 };
    }

    int indexAt(juce::Point<int> p) const
    {
        for (int i = 0; i < (int) options_.size(); ++i)
            if (optionBounds(i).contains(p))
                return i;
        return -1;
    }

    void setHovered(int index)
    {
        if (index != hovered_)
        {
            hovered_ = index;
            repaint();
        }
    }

    std::vector<Option> options_;
    float               fontSize_ = 12.0f;
    int                 selected_ = 0;
    int                 hovered_  = -1;
};

/** A one-pixel upright rule between groups on a bar. */
class BarSeparator final : public juce::Component
{
public:
    BarSeparator() { setInterceptsMouseClicks(false, false); }

    void paint(juce::Graphics& g) override
    {
        g.setColour(theme::colour(*this, theme::dividerId));
        g.fillRect(getWidth() / 2, (getHeight() - 20) / 2, 1, 20);
    }
};

/** The toolbar: what it holds is placed by MainComponent; this only paints
    the strip it sits on. */
class ToolbarStrip final : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
        shell::ruleBelow(g, *this);
    }
};

/**
    The transport bar along the bottom: the playhead's time large in the
    accent, the transport's buttons, the master level, and the selection's
    start, end and length beside the view's.
*/
class TransportBar final : public juce::Component
{
public:
    GlyphButton toStart  { "Go to start", glyphs::Glyph::skipBack };
    GlyphButton back     { "Back", glyphs::Glyph::rewind };
    GlyphButton stop     { "Stop", glyphs::Glyph::stop };
    GlyphButton play     { "Play", glyphs::Glyph::play };
    GlyphButton forward  { "Forward", glyphs::Glyph::fastForward };
    GlyphButton toEnd    { "Go to end", glyphs::Glyph::skipForward };
    GlyphButton record   { "Record", glyphs::Glyph::record };
    GlyphButton loop     { "Loop", glyphs::Glyph::repeat };

    TransportBar()
    {
        for (auto* b : buttons())
            addAndMakeVisible(*b);
        play.setOutlined(true);
        play.setGlyphSize(16.0f);
        stop.setGlyphSize(14.0f);
        record.setGlyphSize(15.0f);
    }

    std::vector<GlyphButton*> buttons() { return { &toStart, &back, &stop, &play, &forward, &toEnd, &record, &loop }; }

    void setTime(const juce::String& time)
    {
        if (time != time_)
        {
            time_ = time;
            repaint(timeBounds());
        }
    }

    void setLevels(float left, float right)
    {
        if (std::abs(left - levels_[0]) < 1.0e-4f && std::abs(right - levels_[1]) < 1.0e-4f)
            return;
        levels_[0] = left;
        levels_[1] = right;
        repaint(meterBounds());
    }

    /** The two rows of the readout on the right: a label, then start, end
        and length, already formatted. */
    void setReadout(const juce::StringArray& selection, const juce::StringArray& view)
    {
        if (selection == selection_ && view == view_)
            return;
        selection_ = selection;
        view_      = view;
        repaint(readoutBounds());
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
        shell::ruleAbove(g, *this);

        g.setColour(theme::accentStep(*this, 300));
        g.setFont(theme::monoFont(24.0f));
        g.drawText(time_, timeBounds(), juce::Justification::centredLeft, false);

        paintMeters(g);
        paintReadout(g);
    }

    void resized() override
    {
        auto row = getLocalBounds().withTrimmedLeft(timeBounds().getRight() + 8);
        const auto place = [&row](juce::Component& c, int w, int h)
        {
            c.setBounds(row.removeFromLeft(w).withSizeKeepingCentre(w, h));
            row.removeFromLeft(2);
        };
        place(toStart, 32, 30);
        place(back, 32, 30);
        place(stop, 32, 30);
        place(play, 42, 32);
        place(forward, 32, 30);
        place(toEnd, 32, 30);
        place(record, 32, 30);
        place(loop, 32, 30);
    }

private:
    juce::Rectangle<int> timeBounds() const { return { 16, 0, 168, getHeight() }; }

    juce::Rectangle<int> meterBounds() const
    {
        const int left  = loop.getRight() + 20;
        const int right = readoutBounds().getX() - 24;
        return juce::Rectangle<int>(left, 0, juce::jlimit(0, 260, right - left), getHeight())
            .withSizeKeepingCentre(juce::jlimit(0, 260, right - left), 12);
    }

    juce::Rectangle<int> readoutBounds() const
    {
        return getLocalBounds().withTrimmedRight(14).removeFromRight(juce::jmin(400, getWidth() / 3)).reduced(0, 6);
    }

    void paintMeters(juce::Graphics& g)
    {
        const auto area = meterBounds();
        if (area.getWidth() < 40)
            return;

        // Green into yellow into red, over the meter's whole width, so how
        // far a level reaches is also what colour it's reached.
        const auto ok    = theme::colour(*this, theme::okId);
        const auto mid   = theme::colour(*this, theme::meterMidId);
        const auto hot   = theme::colour(*this, theme::dangerId);
        auto       ramp  = juce::ColourGradient::horizontal(ok, (float) area.getX(), hot, (float) area.getRight());
        ramp.addColour(0.82, mid);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto bar = juce::Rectangle<float>((float) area.getX(), (float) area.getY() + (float) ch * 7.0f,
                                                    (float) area.getWidth(), 5.0f);
            g.setColour(theme::colour(*this, theme::textId).withAlpha(0.07f));
            g.fillRoundedRectangle(bar, 2.0f);

            const float db       = juce::Decibels::gainToDecibels(levels_[ch], -60.0f);
            const float fraction = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            if (fraction <= 0.0f)
                continue;
            g.setGradientFill(ramp);
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * fraction), 2.0f);
        }
    }

    void paintReadout(juce::Graphics& g)
    {
        const auto area = readoutBounds();
        if (area.getWidth() < 200)
            return;

        const auto text    = theme::colour(*this, theme::textId);
        const int  labelW  = 72;
        const int  columnW = (area.getWidth() - labelW) / 3;
        const int  rowH    = area.getHeight() / 3;

        g.setFont(theme::font(9.5f).withExtraKerningFactor(0.08f));
        g.setColour(text.withAlpha(0.40f));
        const char* heads[] = { "START", "END", "DURATION" };
        for (int c = 0; c < 3; ++c)
            g.drawText(heads[c], area.getX() + labelW + c * columnW, area.getY(), columnW, rowH,
                       juce::Justification::centredLeft);

        const auto row = [&](const juce::StringArray& cells, int r)
        {
            if (cells.isEmpty())
                return;
            const int y = area.getY() + rowH * r;
            g.setFont(theme::font(11.0f));
            g.setColour(text.withAlpha(0.50f));
            g.drawText(cells[0], area.getX(), y, labelW - 12, rowH, juce::Justification::centredRight);
            g.setFont(theme::monoFont(11.0f));
            g.setColour(text);
            for (int c = 1; c < cells.size() && c <= 3; ++c)
                g.drawText(cells[c], area.getX() + labelW + (c - 1) * columnW, y, columnW, rowH,
                           juce::Justification::centredLeft);
        };
        row(selection_, 1);
        row(view_, 2);
    }

    juce::String      time_ { "0:00.000" };
    float             levels_[2] {};
    juce::StringArray selection_, view_;
};

/** The status line under everything: a hint for what the mouse can do on
    the left, the format of what's open on the right. */
class StatusLine final : public juce::Component
{
public:
    void setText(const juce::String& hint, const juce::String& info)
    {
        if (hint == hint_ && info == info_)
            return;
        hint_ = hint;
        info_ = info;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
        shell::ruleAbove(g, *this);
        g.setColour(theme::colour(*this, theme::textId).withAlpha(0.45f));
        g.setFont(theme::font(10.5f));
        auto area = getLocalBounds().reduced(12, 0);
        g.drawText(info_, area.removeFromRight(360), juce::Justification::centredRight, true);
        g.drawText(hint_, area, juce::Justification::centredLeft, true);
    }

private:
    juce::String hint_, info_;
};

} // namespace soundsplice
