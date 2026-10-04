#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace soundsplice
{
/**
    The command palette (VS Code's, REAPER's Actions list): type part of a
    command's name and run it, without knowing which menu it's in.

    The entries are whatever the owner supplies - every command in the
    command table, plus the Favorites and macros - each with what to do when
    it's chosen. Matching is fuzzy (palette::score) so "sav as" finds Save
    Project As and "nrm" finds Normalize.
*/
namespace palette
{
    /** One thing the palette can run. */
    struct Entry
    {
        juce::String          name;        // as listed: "Normalize..."
        juce::String          category;    // shown alongside: "Edit"
        juce::String          description;
        juce::String          shortcut;    // "Ctrl+Shift+P", or empty
        bool                  enabled = true;
        bool                  ticked  = false;
        juce::String          key;         // stable id for the recent list: "cmd:8200", "fav:Voice"
        std::function<void()> run;
    };

    /** How well @p query matches @p text, higher is better, or -1 if it
        doesn't: every character of the query (spaces aside) has to appear
        in the text in order, ignoring case. A run of consecutive characters,
        or one starting a word, counts for more, and so does a shorter text,
        so "zoom in" puts Zoom In ahead of Zoom to Selection. */
    inline int score(const juce::String& query, const juce::String& text)
    {
        juce::String q;
        for (auto c : query.toLowerCase())
            if (! juce::CharacterFunctions::isWhitespace(c))
                q += c;
        if (q.isEmpty())
            return 0;

        const auto t = text.toLowerCase();
        const int  n = q.length(), m = t.length();
        if (n > m)
            return -1;

        auto wordStart = [&](int j)
        {
            if (j == 0)
                return true;
            const auto before = t[j - 1];
            return ! juce::CharacterFunctions::isLetterOrDigit(before);
        };

        // best[i][j]: the best score with query[i] matched at text[j].
        constexpr int kNone = std::numeric_limits<int>::min() / 2;
        std::vector<std::vector<int>> best((size_t) n, std::vector<int>((size_t) m, kNone));
        for (int i = 0; i < n; ++i)
        {
            int bestBefore = kNone; // max over k < j-1 of best[i-1][k]
            for (int j = i; j < m; ++j)
            {
                if (i > 0 && j >= 2)
                    bestBefore = std::max(bestBefore, best[(size_t) i - 1][(size_t) j - 2]);
                if (t[j] != q[i])
                    continue;

                int here = 1 + (wordStart(j) ? 8 : 0) + (j == 0 ? 4 : 0);
                if (i == 0)
                {
                    best[0][(size_t) j] = here;
                    continue;
                }
                int from = bestBefore;
                if (const auto adjacent = best[(size_t) i - 1][(size_t) j - 1]; adjacent > kNone)
                    from = std::max(from, adjacent + 5);
                if (from > kNone)
                    best[(size_t) i][(size_t) j] = from + here;
            }
        }

        const auto& last  = best[(size_t) n - 1];
        const int   found = *std::max_element(last.begin(), last.end());
        if (found <= kNone)
            return -1;
        return found * 100 - m;
    }

    /** The indices of the entries matching @p query, best first; ties keep
        their order. With no query, @p recent (most recent first, by key)
        comes first and the rest follow in order. */
    inline std::vector<int> filter(const std::vector<Entry>& entries, const juce::String& query,
                                   const juce::StringArray& recent = {})
    {
        std::vector<std::pair<int, int>> scored; // score, index
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto& entry = entries[(size_t) i];
            int s = score(query, entry.name);
            // The category too ("edit norm"), for a little less.
            if (const int withCategory = score(query, entry.category + " " + entry.name); withCategory - 50 > s)
                s = withCategory - 50;
            if (s < 0)
                continue;
            if (const int r = recent.indexOf(entry.key); r >= 0)
                s += query.trim().isEmpty() ? 1000000 - r : 150;
            scored.emplace_back(s, i);
        }
        std::stable_sort(scored.begin(), scored.end(), [](auto a, auto b) { return a.first > b.first; });

        std::vector<int> result;
        for (const auto& [s, i] : scored)
            result.push_back(i);
        return result;
    }

    /** @p recent with @p key moved to the front, kept to @p limit. */
    inline juce::StringArray noteRecent(juce::StringArray recent, const juce::String& key, int limit = 12)
    {
        recent.removeString(key);
        recent.insert(0, key);
        while (recent.size() > limit)
            recent.remove(recent.size() - 1);
        return recent;
    }
}

/** The palette itself: a search box over a list, shown over the window.
    Up and Down move, Return runs, Escape (or clicking elsewhere) closes. */
class CommandPalette final : public juce::Component,
                             private juce::ListBoxModel,
                             private juce::KeyListener
{
public:
    /** Called with the entry chosen, after the palette has closed. */
    std::function<void(const palette::Entry&)> onChosen;
    std::function<void()>                      onClosed;

    CommandPalette()
    {
        search_.setTextToShowWhenEmpty("Type a command...", theme::colour(*this, theme::textFaintId));
        search_.setFont(juce::FontOptions(16.0f));
        search_.addKeyListener(this);
        search_.onTextChange = [this] { refilter(); };
        search_.setTitle("Search commands");
        addAndMakeVisible(search_);

        list_.setModel(this);
        list_.setRowHeight(34);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list_.setTitle("Commands");
        list_.setWantsKeyboardFocus(false);
        addAndMakeVisible(list_);

        setWantsKeyboardFocus(false);
        setTitle("Command palette");
    }

    ~CommandPalette() override { search_.removeKeyListener(this); }

    /** Shows @p entries, empty-searched, and takes the keyboard. */
    void open(std::vector<palette::Entry> entries, juce::StringArray recent)
    {
        entries_ = std::move(entries);
        recent_  = std::move(recent);
        search_.clear();
        refilter();
        setVisible(true);
        toFront(false);
        search_.grabKeyboardFocus();
    }

    void close()
    {
        if (! isVisible())
            return;
        setVisible(false);
        if (onClosed)
            onClosed();
    }

    int                 visibleCount() const noexcept { return (int) shown_.size(); }
    const palette::Entry& visibleEntry(int row) const { return entries_[(size_t) shown_[(size_t) row]]; }
    juce::TextEditor&   searchBoxForTesting() noexcept { return search_; }
    int                 selectedRow() const { return list_.getSelectedRow(); }

    /** Runs the highlighted entry, if it can be run. */
    void runSelected()
    {
        const int row = list_.getSelectedRow();
        if (row < 0 || row >= (int) shown_.size())
            return;
        const auto entry = entries_[(size_t) shown_[(size_t) row]];
        if (! entry.enabled)
            return;
        close();
        if (onChosen)
            onChosen(entry);
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour(theme::surface(*this, theme::popupId));
        g.fillRoundedRectangle(area, 6.0f);
        g.setColour(theme::colour(*this, theme::dividerId));
        g.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        search_.setBounds(area.removeFromTop(30));
        area.removeFromTop(6);
        list_.setBounds(area);
    }

    void focusOfChildComponentChanged(FocusChangeType) override
    {
        // Clicking anywhere else closes it, as a menu would. Checked a
        // moment later: focus moves through nothing on its way between two
        // of our own children.
        juce::Component::SafePointer<CommandPalette> self(this);
        juce::MessageManager::callAsync([self]
        {
            if (self != nullptr && self->isVisible() && ! self->hasKeyboardFocus(true))
                self->close();
        });
    }

private:
    void refilter()
    {
        shown_ = palette::filter(entries_, search_.getText(), recent_);
        list_.updateContent();
        list_.repaint();
        int first = -1;
        for (int row = 0; row < (int) shown_.size() && first < 0; ++row)
            if (visibleEntry(row).enabled)
                first = row;
        list_.selectRow(first >= 0 ? first : 0);
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::rowSelectionChanged);
        announceSelection();
    }

    void announceSelection()
    {
        const int row = list_.getSelectedRow();
        if (row < 0 || row >= (int) shown_.size())
            return;
        const auto& entry = visibleEntry(row);
        juce::AccessibilityHandler::postAnnouncement(entry.name + (entry.enabled ? "" : ", unavailable")
                                                         + (entry.shortcut.isEmpty() ? "" : ", " + entry.shortcut),
                                                     juce::AccessibilityHandler::AnnouncementPriority::low);
    }

    void move(int delta)
    {
        if (shown_.empty())
            return;
        const int row = juce::jlimit(0, (int) shown_.size() - 1, list_.getSelectedRow() + delta);
        list_.selectRow(row);
        announceSelection();
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key == juce::KeyPress::upKey)       { move(-1); return true; }
        if (key == juce::KeyPress::downKey)     { move(1); return true; }
        if (key == juce::KeyPress::pageUpKey)   { move(-8); return true; }
        if (key == juce::KeyPress::pageDownKey) { move(8); return true; }
        if (key == juce::KeyPress::returnKey)   { runSelected(); return true; }
        if (key == juce::KeyPress::escapeKey)   { close(); return true; }
        return false;
    }

    int getNumRows() override { return (int) shown_.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) shown_.size())
            return;
        const auto& entry = visibleEntry(row);
        if (selected)
        {
            g.setColour(juce::Colours::steelblue.withAlpha(0.45f));
            g.fillRoundedRectangle(juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(1.0f), 4.0f);
        }

        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(8, 2);
        const float alpha = entry.enabled ? 1.0f : 0.4f;

        g.setFont(juce::FontOptions(12.0f));
        g.setColour(theme::colour(*this, theme::textId).withAlpha(0.55f * alpha));
        if (entry.shortcut.isNotEmpty())
            g.drawText(entry.shortcut, area.removeFromRight(130), juce::Justification::centredRight);
        g.drawText(entry.category, area.removeFromRight(80), juce::Justification::centredRight);

        g.setFont(juce::FontOptions(14.0f));
        g.setColour(theme::colour(*this, theme::textId).withAlpha(alpha));
        g.drawText((entry.ticked ? juce::String::fromUTF8("\xe2\x9c\x93 ") : juce::String()) + entry.name,
                   area, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override
    {
        list_.selectRow(row);
        search_.grabKeyboardFocus();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        list_.selectRow(row);
        runSelected();
    }

    juce::String getTooltipForRow(int row) override
    {
        return row >= 0 && row < (int) shown_.size() ? visibleEntry(row).description : juce::String();
    }

    juce::String getNameForRow(int row) override
    {
        return row >= 0 && row < (int) shown_.size() ? visibleEntry(row).name : juce::String();
    }

    std::vector<palette::Entry> entries_;
    std::vector<int>            shown_;
    juce::StringArray           recent_;
    juce::TextEditor            search_;
    juce::ListBox               list_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandPalette)
};

} // namespace soundsplice
