#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Diagnostics.h"

namespace soundsplice
{
/**
    The Diagnostics pane (Audition's): what a scan of the selected clip found
    - clicks, clipped stretches, silences, a DC offset - one row each, with
    where it is. Select picks the row's stretch in the audio editor, Fix
    repairs it, and Fix All repairs every row of its kind.

    Owns nothing but the rows: the owner scans, fixes and fills it again.
*/
class DiagnosticsPane final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Row
    {
        engine::diagnostics::Issue issue;
        double                     fromSeconds = 0.0; // from the clip's start
        double                     toSeconds   = 0.0;
    };

    std::function<void()>                             onScan;
    std::function<void(const Row&)>                   onSelect;
    std::function<void(const Row&)>                   onFix;
    std::function<void(engine::diagnostics::Kind)>    onFixAll;

    DiagnosticsPane()
    {
        list_.setModel(this);
        list_.setRowHeight(20);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.15f));
        addAndMakeVisible(list_);

        scanButton_.onClick = [this] { if (onScan) onScan(); };
        scanButton_.setTooltip("Scan the selected audio clip for clicks, clipping, silence and DC offset");
        selectButton_.onClick = [this] { if (auto* row = selectedRow(); row != nullptr && onSelect) onSelect(*row); };
        fixButton_.onClick    = [this] { if (auto* row = selectedRow(); row != nullptr && onFix) onFix(*row); };
        fixAllButton_.onClick = [this] { if (auto* row = selectedRow(); row != nullptr && onFixAll) onFixAll(row->issue.kind); };
        fixAllButton_.setTooltip("Fix every problem of the selected one's kind");
        for (auto* button : { &scanButton_, &selectButton_, &fixButton_, &fixAllButton_ })
            addAndMakeVisible(button);

        summary_.setText("Select an audio clip and Scan", juce::dontSendNotification);
        summary_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        addAndMakeVisible(summary_);
        updateButtons();
    }

    /** What the last scan found, for clip @p clipName; @p scanned false
        shows the pane as not scanned yet. */
    void setRows(std::vector<Row> rows, const juce::String& clipName, bool scanned = true)
    {
        rows_ = std::move(rows);
        list_.updateContent();
        list_.deselectAllRows();
        list_.repaint();
        if (! scanned)
            summary_.setText("Select an audio clip and Scan", juce::dontSendNotification);
        else if (rows_.empty())
            summary_.setText(clipName + ": nothing found", juce::dontSendNotification);
        else
            summary_.setText(clipName + ": " + juce::String((int) rows_.size()) + (rows_.size() == 1 ? " problem" : " problems"),
                             juce::dontSendNotification);
        updateButtons();
    }

    const std::vector<Row>& rows() const noexcept { return rows_; }
    void selectRowForTesting(int row) { list_.selectRow(row); }
    juce::Button& fixButtonForTesting() { return fixButton_; }

    /** A row's description, as the list shows it. */
    static juce::String describe(const Row& row)
    {
        const auto at = [](double seconds)
        {
            const int minutes = (int) (seconds / 60.0);
            return juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 3).paddedLeft('0', 6);
        };
        switch (row.issue.kind)
        {
            case engine::diagnostics::Kind::Click:    return "Click at " + at(row.fromSeconds);
            case engine::diagnostics::Kind::Clipping:
                return "Clipping at " + at(row.fromSeconds) + "  ("
                       + juce::String((row.toSeconds - row.fromSeconds) * 1000.0, 1) + " ms)";
            case engine::diagnostics::Kind::Silence:
                return "Silence " + at(row.fromSeconds) + " - " + at(row.toSeconds);
            case engine::diagnostics::Kind::DcOffset:
                return "DC offset " + juce::String(row.issue.value * 100.0, 2) + " % ("
                       + juce::String(juce::Decibels::gainToDecibels(row.issue.value), 1) + " dB)";
        }
        return {};
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto top  = area.removeFromTop(26);
        scanButton_.setBounds(top.removeFromLeft(70).reduced(2));
        selectButton_.setBounds(top.removeFromLeft(64).reduced(2));
        fixButton_.setBounds(top.removeFromLeft(50).reduced(2));
        fixAllButton_.setBounds(top.removeFromLeft(64).reduced(2));
        area.removeFromTop(4);
        summary_.setBounds(area.removeFromTop(18));
        area.removeFromTop(4);
        list_.setBounds(area);
    }

private:
    const Row* selectedRow() const
    {
        const int row = list_.getSelectedRow();
        return row >= 0 && row < (int) rows_.size() ? &rows_[(size_t) row] : nullptr;
    }

    void updateButtons()
    {
        const bool any = selectedRow() != nullptr;
        selectButton_.setEnabled(any && selectedRow()->issue.kind != engine::diagnostics::Kind::DcOffset);
        fixButton_.setEnabled(any);
        fixAllButton_.setEnabled(any);
    }

    int  getNumRows() override { return (int) rows_.size(); }
    void selectedRowsChanged(int) override { updateButtons(); }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < (int) rows_.size() && onSelect)
            onSelect(rows_[(size_t) row]);
    }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) rows_.size())
            return;
        if (selected)
            g.fillAll(juce::Colours::orange.withAlpha(0.3f));

        static const juce::Colour kColours[] { juce::Colours::orange, juce::Colours::red,
                                               juce::Colours::skyblue, juce::Colours::violet };
        g.setColour(kColours[(int) rows_[(size_t) row].issue.kind]);
        g.fillEllipse(6.0f, (float) height * 0.5f - 4.0f, 8.0f, 8.0f);
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(describe(rows_[(size_t) row]), 22, 0, width - 26, height, juce::Justification::centredLeft, true);
    }

    juce::ListBox    list_ { "Problems" };
    juce::TextButton scanButton_ { "Scan" }, selectButton_ { "Select" }, fixButton_ { "Fix" }, fixAllButton_ { "Fix All" };
    juce::Label      summary_;
    std::vector<Row> rows_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsPane)
};

} // namespace soundsplice
