#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Pattern.h"
#include "model/DrumKit.h"

#include "PianoRollGeometry.h"

namespace looper
{
/**
    The rhythm half of the Drums pane: one row per drum pad, one column per
    step, click a cell to toggle that pad's hit on that step — the classic
    step sequencer / FL-channel-rack grid.

    Deliberately built on the same PianoRollGeometry the piano roll uses: a
    16th-note step grid with a left-hand name gutter is exactly the shape it
    already models, the only difference being that rows here are bound to the
    pad list rather than a contiguous pitch range. It keeps its own gutter
    (rather than relying on the kit editor beside it for row names) so the
    two halves of the pane stay independently sized and scrollable without
    their rows having to line up pixel-for-pixel.

    Unlike the piano roll, the grid follows the pattern's own length — a
    longer clip simply gets more columns — and it draws a playhead column
    during playback (see setPlayheadBeats), since watching the position walk
    the grid is most of the point of a step sequencer.
*/
class DrumStepGrid final : public juce::Component
{
public:
    // Explicit because JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (below)
    // declares a deleted copy constructor, and any user-declared constructor
    // — deleted or not — suppresses the implicit default one. Classes here
    // that define their own constructor (PianoRoll, MixerStrip, ...) never
    // hit this; ones that don't need this line.
    DrumStepGrid() = default;

    std::function<void(const engine::Pattern&)> onChange;
    std::function<void(int noteNumber)>         onNotePreview; // fired when a hit is *added* by clicking

    /** Replaces the displayed pattern without firing onChange (used on
        selection changes and undo/redo). The column count follows the
        pattern's length, capped so a very long clip can't produce
        hairline-thin columns. */
    void setPattern(const engine::Pattern& pattern)
    {
        pattern_ = pattern;
        const int steps = (int) std::llround(pattern_.lengthBeats / geometry_.stepBeats);
        geometry_.numSteps = juce::jlimit(1, kMaxSteps, steps);
        repaint();
    }

    /** Sets the rows: one per pad, labelled and pitched by @p pads. */
    void setPads(const std::vector<model::DrumPad>& pads)
    {
        pads_             = pads;
        geometry_.numRows = juce::jmax(1, (int) pads.size());
        hoverRow_         = -1;
        repaint();
    }

    /** @p patternLocalBeats is the playhead's position within the pattern's
        own loop (the owner subtracts the clip's start — see
        MainComponent::timerCallback); it's wrapped to the pattern length
        here. Pass visible = false when stopped to hide the column. */
    void setPlayheadBeats(double patternLocalBeats, bool visible)
    {
        const int step = visible && pattern_.lengthBeats > 0.0
                             ? (int) std::floor(wrapped(patternLocalBeats) / geometry_.stepBeats)
                             : -1;
        if (step != playheadStep_)
        {
            playheadStep_ = step;
            repaint();
        }
    }

    /** Which pad row is highlighted as selected — kept in sync with the kit
        editor beside it so the two halves agree on "the current pad". */
    void setSelectedPad(int padIndex)
    {
        if (selectedPad_ != padIndex)
        {
            selectedPad_ = padIndex;
            repaint();
        }
    }

    std::function<void(int padIndex)> onPadSelected;

    void mouseDown(const juce::MouseEvent& e) override
    {
        int row = 0, step = 0;
        if (! geometry_.cellAt(e.position.x, e.position.y, (float) getWidth(), (float) getHeight(), row, step))
        {
            // A click in the gutter selects that pad rather than editing a step.
            const int gutterRow = rowAtY(e.position.y);
            if (e.position.x < geometry_.gutterWidth && gutterRow >= 0 && onPadSelected)
                onPadSelected(gutterRow);
            return;
        }

        if (row < 0 || row >= (int) pads_.size())
            return;

        if (onPadSelected)
            onPadSelected(row); // editing a pad's row also makes it the current pad

        const int    noteNumber = pads_[(size_t) row].noteNumber;
        const double start      = step * geometry_.stepBeats;

        auto it = std::find_if(pattern_.notes.begin(), pattern_.notes.end(),
                               [&](const engine::Note& n)
                               {
                                   return n.noteNumber == noteNumber
                                       && std::abs(n.startBeats - start) < 1.0e-6;
                               });

        if (it != pattern_.notes.end())
        {
            pattern_.notes.erase(it);
        }
        else
        {
            pattern_.notes.push_back({ start, geometry_.stepBeats, noteNumber, 0.8f });
            if (onNotePreview)
                onNotePreview(noteNumber); // only on add, same as the piano roll
        }

        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int row = 0, step = 0;
        const int newHoverRow = geometry_.cellAt(e.position.x, e.position.y,
                                                 (float) getWidth(), (float) getHeight(), row, step)
                                    ? row : -1;
        if (newHoverRow != hoverRow_)
        {
            hoverRow_ = newHoverRow;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverRow_ != -1)
        {
            hoverRow_ = -1;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        const float w  = (float) getWidth();
        const float h  = (float) getHeight();
        const float cw = geometry_.colWidth(w);
        const float ch = geometry_.rowHeight(h);
        const float gx = geometry_.gutterWidth;

        g.fillAll(juce::Colour(0xff1a1a1e));

        // The playhead's whole column, drawn under the hits so they stay legible.
        if (playheadStep_ >= 0 && playheadStep_ < geometry_.numSteps)
        {
            g.setColour(juce::Colours::white.withAlpha(0.13f));
            g.fillRect(geometry_.xForStep(playheadStep_, w), 0.0f, cw, h);
        }

        g.setFont(juce::FontOptions(11.0f));
        for (int r = 0; r < geometry_.numRows; ++r)
        {
            const float y        = geometry_.yForRow(r, h);
            const bool  selected = (r == selectedPad_);
            const bool  hovered  = (r == hoverRow_);

            g.setColour(selected ? juce::Colour(0xff45454e) : juce::Colour(0xff35353a));
            g.fillRect(juce::Rectangle<float>(0.0f, y, gx, ch));
            g.setColour(juce::Colours::white.withAlpha(selected ? 0.95f : 0.8f));
            g.drawText(labelForRow(r), 4, (int) y, (int) gx - 6, (int) ch, juce::Justification::centredLeft);

            g.setColour(hovered ? juce::Colours::white.withAlpha(0.10f)
                                : juce::Colours::white.withAlpha(selected ? 0.07f : 0.04f));
            g.fillRect(juce::Rectangle<float>(gx, y, w - gx, ch));
        }

        // Column lines, heavier on each beat so the metre is readable at a glance.
        const int stepsPerBeat = juce::jmax(1, (int) std::llround(1.0 / geometry_.stepBeats));
        for (int s = 0; s <= geometry_.numSteps; ++s)
        {
            const bool beat = (s % stepsPerBeat) == 0;
            g.setColour(juce::Colours::white.withAlpha(beat ? 0.25f : 0.08f));
            g.fillRect(geometry_.xForStep(s, w), 0.0f, beat ? 2.0f : 1.0f, h);
        }

        for (int r = 0; r <= geometry_.numRows; ++r)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRect(0.0f, geometry_.yForRow(r, h), w, 1.0f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(gx - 1.0f, 0.0f, 1.0f, h);

        for (const auto& n : pattern_.notes)
        {
            const int step = (int) std::llround(n.startBeats / geometry_.stepBeats);
            const int row  = rowForNote(n.noteNumber);
            if (row < 0 || row >= geometry_.numRows || step < 0 || step >= geometry_.numSteps)
                continue; // a hit on a pad this kit no longer has, or past the pattern's end

            // Muted pads still show their hits, dimmed — the step pattern is
            // worth seeing even when you can't currently hear it.
            const bool dim = pads_[(size_t) row].muted;
            g.setColour(dim ? juce::Colours::limegreen.withAlpha(0.35f) : juce::Colours::limegreen);
            g.fillRect(juce::Rectangle<float>(geometry_.xForStep(step, w) + 1.0f,
                                              geometry_.yForRow(row, h) + 1.0f,
                                              cw - 2.0f, ch - 2.0f));
        }
    }

private:
    static constexpr int kMaxSteps = 64;

    double wrapped(double beats) const
    {
        const double length = pattern_.lengthBeats;
        if (length <= 0.0)
            return 0.0;
        const double m = std::fmod(beats, length);
        return m < 0.0 ? m + length : m;
    }

    int rowAtY(float y) const
    {
        const float ch = geometry_.rowHeight((float) getHeight());
        if (ch <= 0.0f)
            return -1;
        const int row = (int) (y / ch);
        return (row >= 0 && row < (int) pads_.size()) ? row : -1;
    }

    int rowForNote(int noteNumber) const
    {
        for (size_t i = 0; i < pads_.size(); ++i)
            if (pads_[i].noteNumber == noteNumber)
                return (int) i;
        return -1;
    }

    juce::String labelForRow(int row) const
    {
        return (row >= 0 && row < (int) pads_.size()) ? juce::String(pads_[(size_t) row].label)
                                                      : juce::String();
    }

    PianoRollGeometry           geometry_;
    engine::Pattern             pattern_;
    std::vector<model::DrumPad> pads_;
    int                         hoverRow_     = -1;
    int                         selectedPad_  = -1;
    int                         playheadStep_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumStepGrid)
};

} // namespace looper
