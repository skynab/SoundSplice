#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    The Open Files pane: the audio open in the editor (app/OpenFiles.h), one
    row each, with the one showing highlighted. Click a row to edit it,
    right-click one to close it, or use the buttons along the top.

    Holds only what it was handed to draw; MainComponent owns the list and
    decides what choosing or closing an entry does.
*/
class OpenFilesPane final : public juce::Component,
                            private juce::ListBoxModel
{
public:
    struct Entry
    {
        int          clipId = 0;
        juce::String name;   // the file
        juce::String detail; // its track and length
        juce::Colour colour; // its track's
    };

    std::function<void(int clipId)> onChosen;
    std::function<void(int clipId)> onClosed;
    std::function<void()>           onCloseAll;

    OpenFilesPane()
    {
        closeButton_.setButtonText("Close");
        closeButton_.setTooltip("Close the file showing in the Audio editor");
        closeButton_.onClick = [this]
        {
            if (current_ != 0 && onClosed)
                onClosed(current_);
        };
        addAndMakeVisible(closeButton_);

        closeAllButton_.setButtonText("Close All");
        closeAllButton_.setTooltip("Empty this list. Nothing is removed from the project.");
        closeAllButton_.onClick = [this] { if (onCloseAll) onCloseAll(); };
        addAndMakeVisible(closeAllButton_);

        list_.setModel(this);
        list_.setRowHeight(kRowHeight);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1a1a1e));
        list_.setOutlineThickness(0);
        addAndMakeVisible(list_);

        updateButtons();
    }

    ~OpenFilesPane() override { list_.setModel(nullptr); }

    /** The list to show, and which entry is in the editor (0 for none). */
    void setEntries(std::vector<Entry> entries, int currentClipId)
    {
        entries_ = std::move(entries);
        current_ = currentClipId;
        list_.updateContent();

        int row = -1;
        for (int i = 0; i < (int) entries_.size(); ++i)
            if (entries_[(size_t) i].clipId == current_)
                row = i;

        if (row >= 0)
        {
            list_.selectRow(row, true, true);
            list_.scrollToEnsureRowIsOnscreen(row);
        }
        else
        {
            list_.deselectAllRows();
        }

        updateButtons();
        repaint();
    }

    const std::vector<Entry>& entries() const noexcept { return entries_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));

        if (entries_.empty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawFittedText("Audio clips opened in the Audio editor are listed here.",
                             list_.getBounds().reduced(12), juce::Justification::centred, 3);
        }
    }

    void resized() override
    {
        auto area    = getLocalBounds().reduced(6);
        auto buttons = area.removeFromTop(24);
        const int width = juce::jmin(90, (buttons.getWidth() - 6) / 2);
        closeButton_.setBounds(buttons.removeFromLeft(width));
        buttons.removeFromLeft(6);
        closeAllButton_.setBounds(buttons.removeFromLeft(width));
        area.removeFromTop(6);
        list_.setBounds(area);
    }

private:
    static constexpr int kRowHeight = 36;

    int getNumRows() override { return (int) entries_.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) entries_.size())
            return;

        const auto& entry = entries_[(size_t) row];
        auto        area  = juce::Rectangle<int>(0, 0, width, height);

        if (selected)
            g.fillAll(juce::Colour(0xff2c3e5a));

        g.setColour(entry.colour);
        g.fillRect(area.removeFromLeft(4).reduced(0, 4));
        area.removeFromLeft(6);
        area.reduce(0, 3);

        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(entry.name, area.removeFromTop(area.getHeight() / 2), juce::Justification::centredLeft, true);

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(entry.detail, area, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& event) override
    {
        if (row < 0 || row >= (int) entries_.size())
            return;

        const int clipId = entries_[(size_t) row].clipId;

        if (event.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Close");
            menu.addItem(2, "Close All");
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&list_),
                               [safe = juce::Component::SafePointer<OpenFilesPane>(this), clipId](int result)
                               {
                                   if (safe == nullptr)
                                       return;
                                   if (result == 1 && safe->onClosed)
                                       safe->onClosed(clipId);
                                   else if (result == 2 && safe->onCloseAll)
                                       safe->onCloseAll();
                               });
            return;
        }

        if (onChosen)
            onChosen(clipId);
    }

    void updateButtons()
    {
        closeButton_.setEnabled(current_ != 0 && ! entries_.empty());
        closeAllButton_.setEnabled(! entries_.empty());
    }

    juce::TextButton   closeButton_;
    juce::TextButton   closeAllButton_;
    juce::ListBox      list_ { "Open Files" };
    std::vector<Entry> entries_;
    int                current_ = 0;
};

} // namespace soundsplice
