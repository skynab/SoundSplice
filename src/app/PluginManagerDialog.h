#pragma once

#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace soundsplice
{
/**
    The plugin manager: every plugin a scan has found, and every one it
    blocklisted, in one table. A found plugin can be turned off (it stops
    being offered; projects already using it still load it) or forgotten (the
    next scan probes it again); a blocklisted one - it crashed, hung or
    wouldn't load while being probed - can be unblocked to be tried again.

    Owns no plugin state: the owner fills it from engine::PluginHost and
    acts on its callbacks, then fills it again.
*/
class PluginManagerDialog final : public juce::Component, private juce::TableListBoxModel
{
public:
    struct Row
    {
        enum class State { On, Off, Blocked };

        std::string format;
        std::string identifier;
        std::string name;
        State       state = State::On;
    };

    std::function<void(const Row&, bool turnOn)> onSetEnabled;
    std::function<void(const Row&)>              onUnblock;
    std::function<void(const Row&)>              onForget;
    std::function<void()>                        onScan;

    PluginManagerDialog()
    {
        table_.setModel(this);
        auto& header = table_.getHeader();
        header.addColumn("Plugin", kName, 260);
        header.addColumn("Format", kFormat, 80);
        header.addColumn("State", kState, 110);
        table_.setMultipleSelectionEnabled(false);
        addAndMakeVisible(table_);

        toggleButton_.onClick = [this]
        {
            if (const auto* row = selectedRow(); row != nullptr && row->state != Row::State::Blocked && onSetEnabled)
                onSetEnabled(*row, row->state == Row::State::Off);
        };
        unblockButton_.onClick = [this]
        {
            if (const auto* row = selectedRow(); row != nullptr && row->state == Row::State::Blocked && onUnblock)
                onUnblock(*row);
        };
        forgetButton_.onClick = [this]
        {
            if (const auto* row = selectedRow(); row != nullptr && row->state != Row::State::Blocked && onForget)
                onForget(*row);
        };
        scanButton_.onClick = [this] { if (onScan) onScan(); };

        toggleButton_.setTooltip("Stop offering this plugin, or offer it again. Projects using it still load it");
        unblockButton_.setTooltip("Try this plugin again at the next scan");
        forgetButton_.setTooltip("Forget what the scan found, so the next scan probes it again");
        scanButton_.setTooltip("Look for plugins not yet scanned. Each is loaded in a separate process, "
                               "so one that crashes is blocked rather than taking SoundSplice down");

        for (auto* button : { &toggleButton_, &unblockButton_, &forgetButton_, &scanButton_ })
            addAndMakeVisible(button);

        summary_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(summary_);

        setSize(520, 420);
        updateButtons();
    }

    void setRows(std::vector<Row> rows)
    {
        // Kept on the same plugin across a refresh, where it's still there.
        const auto* selected = selectedRow();
        const auto  keep     = selected != nullptr ? selected->format + "|" + selected->identifier : std::string();

        rows_ = std::move(rows);
        table_.updateContent();
        table_.deselectAllRows();
        for (int i = 0; i < (int) rows_.size(); ++i)
            if (rows_[(size_t) i].format + "|" + rows_[(size_t) i].identifier == keep)
                table_.selectRow(i);

        int off = 0, blocked = 0;
        for (const auto& row : rows_)
        {
            off     += row.state == Row::State::Off ? 1 : 0;
            blocked += row.state == Row::State::Blocked ? 1 : 0;
        }
        summary_.setText(juce::String((int) rows_.size() - blocked) + " found, " + juce::String(off) + " turned off, "
                             + juce::String(blocked) + " blocked",
                         juce::dontSendNotification);
        table_.repaint();
        updateButtons();
    }

    const std::vector<Row>& rows() const noexcept { return rows_; }
    void selectRowForTesting(int row) { table_.selectRow(row); }
    juce::Button& toggleButtonForTesting() { return toggleButton_; }
    juce::Button& unblockButtonForTesting() { return unblockButton_; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto bottom = area.removeFromBottom(28);
        scanButton_.setBounds(bottom.removeFromRight(130));
        bottom.removeFromRight(6);
        forgetButton_.setBounds(bottom.removeFromRight(80));
        bottom.removeFromRight(6);
        unblockButton_.setBounds(bottom.removeFromRight(80));
        bottom.removeFromRight(6);
        toggleButton_.setBounds(bottom.removeFromRight(80));
        summary_.setBounds(bottom);
        area.removeFromBottom(6);
        table_.setBounds(area);
    }

private:
    enum Column { kName = 1, kFormat, kState };

    const Row* selectedRow() const
    {
        const int row = table_.getSelectedRow();
        return row >= 0 && row < (int) rows_.size() ? &rows_[(size_t) row] : nullptr;
    }

    void updateButtons()
    {
        const auto* row = selectedRow();
        toggleButton_.setEnabled(row != nullptr && row->state != Row::State::Blocked);
        toggleButton_.setButtonText(row != nullptr && row->state == Row::State::Off ? "Turn On" : "Turn Off");
        forgetButton_.setEnabled(row != nullptr && row->state != Row::State::Blocked);
        unblockButton_.setEnabled(row != nullptr && row->state == Row::State::Blocked);
    }

    int getNumRows() override { return (int) rows_.size(); }

    void paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) override
    {
        if (selected)
            g.fillAll(theme::colour(*this, theme::accentId).withAlpha(0.3f));
        else if (row % 2 == 0)
            g.fillAll(theme::colour(*this, theme::textId).withAlpha(0.03f));
    }

    void paintCell(juce::Graphics& g, int row, int column, int width, int height, bool) override
    {
        if (row < 0 || row >= (int) rows_.size())
            return;

        const auto& entry = rows_[(size_t) row];
        juce::String text;
        switch (column)
        {
            case kName:   text = entry.name.empty() ? juce::File(entry.identifier).getFileName() : juce::String(entry.name); break;
            case kFormat: text = entry.format; break;
            case kState:  text = entry.state == Row::State::On ? "On" : entry.state == Row::State::Off ? "Turned off" : "Blocked"; break;
            default:      break;
        }

        g.setColour(entry.state == Row::State::On ? theme::colour(*this, theme::textId) : theme::colour(*this, theme::textMutedId));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(text, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged(int) override { updateButtons(); }

    juce::TableListBox table_;
    juce::TextButton   toggleButton_ { "Turn Off" };
    juce::TextButton   unblockButton_ { "Unblock" };
    juce::TextButton   forgetButton_ { "Forget" };
    juce::TextButton   scanButton_ { "Scan for Plugins" };
    juce::Label        summary_;
    std::vector<Row>   rows_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginManagerDialog)
};

} // namespace soundsplice
