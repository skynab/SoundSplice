#pragma once

#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace soundsplice
{
/**
    The History pane: every undo step in a list - the ones before now, now,
    and the ones redo would bring back - to click and go to; the branches
    an edit after an undo set aside, to switch back to; and Compare, which
    says how the selected step differs from now.

    Owns no history: the owner feeds it steps and branches (setSteps) and
    acts on its callbacks.
*/
class HistoryPane final : public juce::Component
{
public:
    struct Step
    {
        juce::String label; // what made it
        bool         now = false, future = false;
    };

    struct Branch
    {
        juce::String label; // its first edit
        int          steps = 0;
        int          from  = -1; // the step it leaves from, or -1 if not on this line
    };

    std::function<void(int step)>                 onGoTo;
    std::function<void(int branch)>               onSwitchBranch;
    std::function<juce::StringArray(int step)>    onCompare;

    HistoryPane()
    {
        steps_.rows = [this] { return (int) stepList_.size(); };
        steps_.paint = [this](int row, juce::Graphics& g, int w, int h, bool selected)
        {
            const auto& step = stepList_[(size_t) row];
            if (step.now)
                g.fillAll(juce::Colours::steelblue.withAlpha(0.35f));
            if (selected)
                g.fillAll(theme::colour(*this, theme::textId).withAlpha(0.12f));
            g.setColour(step.future ? theme::colour(*this, theme::textFaintId) : theme::colour(*this, theme::textId));
            g.setFont(juce::FontOptions(14.0f, step.now ? juce::Font::bold : juce::Font::plain));
            g.drawText(juce::String(row) + ".  " + step.label + (step.now ? "   (now)" : ""), 8, 0, w - 16, h,
                       juce::Justification::centredLeft, true);
        };
        steps_.name = [this](int row)
        {
            const auto& step = stepList_[(size_t) row];
            return "Step " + juce::String(row) + ", " + step.label + (step.now ? ", now" : step.future ? ", undone" : "");
        };
        steps_.doubleClicked = [this](int row) { if (onGoTo) onGoTo(row); };
        steps_.selected = [this](int) { updateButtons(); };

        branches_.rows  = [this] { return (int) branchList_.size(); };
        branches_.paint = [this](int row, juce::Graphics& g, int w, int h, bool selected)
        {
            const auto& branch = branchList_[(size_t) row];
            if (selected)
                g.fillAll(theme::colour(*this, theme::textId).withAlpha(0.12f));
            g.setColour(branch.from >= 0 ? theme::colour(*this, theme::textId) : theme::colour(*this, theme::textFaintId));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(branch.label + "  (" + juce::String(branch.steps) + (branch.steps == 1 ? " step" : " steps")
                           + (branch.from >= 0 ? ", from step " + juce::String(branch.from) : juce::String(", off this line")) + ")",
                       8, 0, w - 16, h, juce::Justification::centredLeft, true);
        };
        branches_.name = [this](int row) { return "Branch " + branchList_[(size_t) row].label; };
        branches_.doubleClicked = [this](int row) { switchTo(row); };
        branches_.selected = [this](int) { updateButtons(); };

        for (auto* list : { &stepListBox_, &branchListBox_ })
        {
            list->setRowHeight(24);
            list->setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
            addAndMakeVisible(*list);
        }
        stepListBox_.setModel(&steps_);
        stepListBox_.setTitle("Undo steps");
        branchListBox_.setModel(&branches_);
        branchListBox_.setTitle("Branches");

        goButton_.onClick      = [this] { if (stepListBox_.getSelectedRow() >= 0 && onGoTo) onGoTo(stepListBox_.getSelectedRow()); };
        compareButton_.onClick = [this] { compare(); };
        switchButton_.onClick  = [this] { switchTo(branchListBox_.getSelectedRow()); };
        goButton_.setTooltip("Go back (or forward) to this step: the steps after it stay, to redo");
        compareButton_.setTooltip("How this step differs from now");
        switchButton_.setTooltip("Go to where this branch left off and make its steps the ones to redo; "
                                 "the steps it replaces become a branch in turn");
        for (auto* button : { &goButton_, &compareButton_, &switchButton_ })
            addAndMakeVisible(*button);

        branchHeading_.setText("Branches set aside by an edit after an undo", juce::dontSendNotification);
        branchHeading_.setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
        addAndMakeVisible(branchHeading_);

        comparison_.setMultiLine(true);
        comparison_.setReadOnly(true);
        comparison_.setCaretVisible(false);
        comparison_.setTitle("Comparison");
        addAndMakeVisible(comparison_);

        updateButtons();
    }

    /** The steps (oldest first) and branches, as the history has them now. */
    void setSteps(std::vector<Step> steps, std::vector<Branch> branches)
    {
        stepList_   = std::move(steps);
        branchList_ = std::move(branches);
        stepListBox_.updateContent();
        branchListBox_.updateContent();
        stepListBox_.repaint();
        branchListBox_.repaint();
        for (int i = 0; i < (int) stepList_.size(); ++i)
            if (stepList_[(size_t) i].now)
                stepListBox_.scrollToEnsureRowIsOnscreen(i);
        updateButtons();
    }

    void selectStepForTesting(int row) { stepListBox_.selectRow(row); }
    juce::String comparisonForTesting() const { return comparison_.getText(); }

    void paint(juce::Graphics& g) override { g.fillAll(theme::surface(*this, theme::paneId)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto buttons = area.removeFromTop(28);
        goButton_.setBounds(buttons.removeFromLeft(90).reduced(0, 2));
        buttons.removeFromLeft(6);
        compareButton_.setBounds(buttons.removeFromLeft(110).reduced(0, 2));
        area.removeFromTop(4);

        auto bottom = area.removeFromBottom(juce::jmax(60, area.getHeight() / 4));
        comparison_.setBounds(bottom);
        area.removeFromBottom(6);

        auto branchArea = area.removeFromBottom(juce::jmin(150, area.getHeight() / 3));
        auto heading = branchArea.removeFromTop(22);
        switchButton_.setBounds(heading.removeFromRight(110).reduced(0, 1));
        branchHeading_.setBounds(heading);
        branchListBox_.setBounds(branchArea);
        area.removeFromBottom(6);
        stepListBox_.setBounds(area);
    }

private:
    /** A list box model from lambdas. */
    struct Model final : juce::ListBoxModel
    {
        std::function<int()>                                         rows;
        std::function<void(int, juce::Graphics&, int, int, bool)>    paint;
        std::function<juce::String(int)>                             name;
        std::function<void(int)>                                     doubleClicked, selected;

        int  getNumRows() override { return rows ? rows() : 0; }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool s) override
        {
            if (row >= 0 && row < getNumRows())
                paint(row, g, w, h, s);
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { if (doubleClicked) doubleClicked(row); }
        void selectedRowsChanged(int row) override { if (selected) selected(row); }
        juce::String getNameForRow(int row) override { return row >= 0 && row < getNumRows() ? name(row) : juce::String(); }
    };

    void compare()
    {
        const int row = stepListBox_.getSelectedRow();
        if (row < 0 || ! onCompare)
            return;
        const auto lines = onCompare(row);
        comparison_.setText("Step " + juce::String(row) + " compared with now:\n" + lines.joinIntoString("\n"), false);
        juce::AccessibilityHandler::postAnnouncement(lines.joinIntoString(". "), juce::AccessibilityHandler::AnnouncementPriority::low);
    }

    void switchTo(int row)
    {
        if (row >= 0 && row < (int) branchList_.size() && branchList_[(size_t) row].from >= 0 && onSwitchBranch)
            onSwitchBranch(row);
    }

    void updateButtons()
    {
        const int step   = stepListBox_.getSelectedRow();
        const int branch = branchListBox_.getSelectedRow();
        goButton_.setEnabled(step >= 0 && step < (int) stepList_.size() && ! stepList_[(size_t) step].now);
        compareButton_.setEnabled(step >= 0 && step < (int) stepList_.size());
        switchButton_.setEnabled(branch >= 0 && branch < (int) branchList_.size() && branchList_[(size_t) branch].from >= 0);
    }

    std::vector<Step>   stepList_;
    std::vector<Branch> branchList_;
    Model               steps_, branches_;
    juce::ListBox       stepListBox_, branchListBox_;
    juce::TextButton    goButton_ { "Go Here" }, compareButton_ { "Compare to Now" }, switchButton_ { "Switch To" };
    juce::Label         branchHeading_;
    juce::TextEditor    comparison_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryPane)
};

} // namespace soundsplice
