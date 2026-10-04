#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include "engine/DeliverySpec.h"

namespace soundsplice
{
/**
    The Delivery pane: pick where the mix is going (ACX, Spotify, Apple
    Podcasts, YouTube, broadcast), Check renders it and measures it against
    that platform's spec, a line per measure with pass or fail and what to do
    about a fail, and Make It Pass opens Export Audio set to the spec's
    loudness, ceiling, format and rate.

    Runs nothing itself: the owner renders and measures (onCheck) and hands
    back the results (showResults).
*/
class DeliveryPane final : public juce::Component,
                           private juce::TableListBoxModel
{
public:
    std::function<void(int spec)> onCheck;
    std::function<void(int spec)> onMakeItPass;

    DeliveryPane()
    {
        for (const auto& spec : engine::delivery::all())
            specBox_.addItem(spec.name, specBox_.getNumItems() + 1);
        specBox_.setSelectedItemIndex(0, juce::dontSendNotification);
        specBox_.setTitle("Delivery spec");
        specBox_.onChange = [this]
        {
            results_.clear();
            table_.updateContent();
            refreshText();
        };
        addAndMakeVisible(specBox_);

        checkButton_.onClick = [this] { if (onCheck) onCheck(specIndex()); };
        fixButton_.onClick   = [this] { if (onMakeItPass) onMakeItPass(specIndex()); };
        fixButton_.setTooltip("Open Export Audio set to this spec's loudness target, ceiling, format and rate");
        addAndMakeVisible(checkButton_);
        addAndMakeVisible(fixButton_);

        summary_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        addAndMakeVisible(summary_);
        description_.setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
        addAndMakeVisible(description_);

        auto& header = table_.getHeader();
        header.addColumn("", 1, 28, 28, 28, juce::TableHeaderComponent::notResizable);
        header.addColumn("Measure", 2, 150);
        header.addColumn("Measured", 3, 100);
        header.addColumn("Wanted", 4, 140);
        header.addColumn("To fix", 5, 300);
        table_.setModel(this);
        table_.setRowHeight(24);
        table_.setTitle("Delivery check results");
        addAndMakeVisible(table_);

        refreshText();
    }

    int specIndex() const { return juce::jmax(0, specBox_.getSelectedItemIndex()); }

    /** Results of a check of spec @p spec, or busy while one runs. */
    void showResults(int spec, std::vector<engine::delivery::Result> results)
    {
        specBox_.setSelectedItemIndex(spec, juce::dontSendNotification);
        results_ = std::move(results);
        busy_    = false;
        table_.updateContent();
        table_.repaint();
        refreshText();
        juce::AccessibilityHandler::postAnnouncement(summary_.getText(), juce::AccessibilityHandler::AnnouncementPriority::medium);
    }

    void setBusy()
    {
        busy_ = true;
        refreshText();
    }

    const std::vector<engine::delivery::Result>& resultsForTesting() const { return results_; }
    juce::String summaryForTesting() const { return summary_.getText(); }

    void paint(juce::Graphics& g) override { g.fillAll(theme::surface(*this, theme::paneId)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto top  = area.removeFromTop(28);
        specBox_.setBounds(top.removeFromLeft(220).reduced(0, 2));
        top.removeFromLeft(8);
        checkButton_.setBounds(top.removeFromLeft(90).reduced(0, 2));
        top.removeFromLeft(6);
        fixButton_.setBounds(top.removeFromLeft(110).reduced(0, 2));
        area.removeFromTop(6);
        description_.setBounds(area.removeFromTop(20));
        summary_.setBounds(area.removeFromTop(24));
        area.removeFromTop(4);
        table_.setBounds(area);
    }

private:
    void refreshText()
    {
        const auto& spec = engine::delivery::all()[(size_t) specIndex()];
        description_.setText(spec.description, juce::dontSendNotification);
        if (busy_)
            summary_.setText("Rendering and measuring the mix...", juce::dontSendNotification);
        else if (results_.empty())
            summary_.setText("Check renders the mix and measures it against " + juce::String(spec.name) + ".",
                             juce::dontSendNotification);
        else
        {
            int failed = 0;
            for (const auto& r : results_)
                failed += r.pass ? 0 : 1;
            summary_.setText(failed == 0 ? "Passes " + juce::String(spec.name)
                                         : juce::String(failed) + (failed == 1 ? " thing fails " : " things fail ") + spec.name,
                             juce::dontSendNotification);
            summary_.setColour(juce::Label::textColourId, failed == 0 ? theme::colour(*this, theme::okId) : theme::colour(*this, theme::dangerTextId));
        }
        checkButton_.setEnabled(! busy_);
    }

    int getNumRows() override { return (int) results_.size(); }

    void paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) override
    {
        if (selected)
            g.fillAll(juce::Colours::steelblue.withAlpha(0.35f));
        else if (row % 2 == 1)
            g.fillAll(theme::colour(*this, theme::textId).withAlpha(0.03f));
    }

    void paintCell(juce::Graphics& g, int row, int column, int width, int height, bool) override
    {
        if (row < 0 || row >= (int) results_.size())
            return;
        const auto& r = results_[(size_t) row];
        g.setFont(juce::FontOptions(13.0f));
        juce::String text;
        switch (column)
        {
            case 1:
                g.setColour(r.pass ? theme::colour(*this, theme::okId) : theme::colour(*this, theme::dangerTextId));
                g.drawText(r.pass ? juce::String::fromUTF8("\xe2\x9c\x93") : juce::String::fromUTF8("\xe2\x9c\x97"), 0, 0, width,
                           height, juce::Justification::centred);
                return;
            case 2: text = r.measure; break;
            case 3: text = r.measured; break;
            case 4: text = r.wanted; break;
            case 5: text = r.pass ? juce::String() : juce::String(r.advice); break;
            default: break;
        }
        g.setColour(column == 5 ? theme::colour(*this, theme::textMutedId) : theme::colour(*this, theme::textId));
        g.drawText(text, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    juce::String getCellTooltip(int row, int) override
    {
        return row >= 0 && row < (int) results_.size() ? juce::String(results_[(size_t) row].advice) : juce::String();
    }

    juce::ComboBox                          specBox_;
    juce::TextButton                        checkButton_ { "Check" }, fixButton_ { "Make It Pass..." };
    juce::Label                             summary_, description_;
    juce::TableListBox                      table_;
    std::vector<engine::delivery::Result>   results_;
    bool                                    busy_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeliveryPane)
};

} // namespace soundsplice
