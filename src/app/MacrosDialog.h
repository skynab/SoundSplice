#pragma once

#include <functional>
#include <map>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Macros.h"

namespace soundsplice
{
/**
    The Macros window: the macros on the left, the selected one's steps on
    the right, to build, reorder and run.

    Edits a copy and hands each change to the owner (onChanged), which keeps
    and saves the list; adding or editing an effects step and running are
    the owner's too, since they need the effects dialog and the document.
*/
class MacrosDialog final : public juce::Component
{
public:
    std::function<void(const std::vector<macros::Macro>&)> onChanged;
    std::function<void(int macro)>                         onRun;
    std::function<void(int macro)>                         onRunOnFiles;
    // The effects for a new step (@p step = -1) or an existing one; the owner
    // calls setEffects with the chain chosen.
    std::function<void(int macro, int step, const std::vector<model::EffectSlot>& current)> onEditEffects;

    MacrosDialog()
    {
        macroList_.setModel(&macroModel_);
        stepList_.setModel(&stepModel_);
        macroList_.setTitle("Macros");
        stepList_.setTitle("Steps");
        macroModel_.rows     = [this] { return (int) macros_.size(); };
        macroModel_.text     = [this](int row) { return juce::String(macros_[(size_t) row].name); };
        macroModel_.selected = [this](int) { refreshSteps(); };
        macroModel_.doubleClicked = [this](int row) { if (onRun) onRun(row); };
        stepModel_.rows      = [this] { const auto* m = current(); return m != nullptr ? (int) m->steps.size() : 0; };
        stepModel_.text      = [this](int row)
        {
            const auto& step = current()->steps[(size_t) row];
            auto text = juce::String(row + 1) + ". " + macros::describe(step);
            if (! step.isEffects() && macros::commandFor(step) == nullptr)
                text << "  (no longer a command)";
            return text;
        };
        stepModel_.selected      = [this](int) { updateButtons(); };
        stepModel_.doubleClicked = [this](int row) { editStep(row); };

        for (auto* list : { &macroList_, &stepList_ })
        {
            list->setRowHeight(24);
            addAndMakeVisible(*list);
        }

        newButton_.onClick      = [this] { askName("New Macro", {}, [this](const juce::String& name)
                                  {
                                      macros_.push_back({ name.toStdString(), {} });
                                      changed();
                                      macroList_.selectRow((int) macros_.size() - 1);
                                  }); };
        renameButton_.onClick   = [this]
        {
            const int row = macroList_.getSelectedRow();
            if (current() == nullptr)
                return;
            askName("Rename Macro", juce::String(current()->name), [this, row](const juce::String& name)
            {
                macros_[(size_t) row].name = name.toStdString();
                changed();
            });
        };
        deleteButton_.onClick   = [this]
        {
            const int row = macroList_.getSelectedRow();
            if (current() == nullptr)
                return;
            macros_.erase(macros_.begin() + row);
            changed();
            macroList_.selectRow(juce::jmin(row, (int) macros_.size() - 1));
        };
        addCommandButton_.onClick = [this] { chooseCommand(); };
        addEffectsButton_.onClick = [this]
        {
            if (current() != nullptr && onEditEffects)
                onEditEffects(macroList_.getSelectedRow(), -1, {});
        };
        editButton_.onClick     = [this] { editStep(stepList_.getSelectedRow()); };
        removeButton_.onClick   = [this]
        {
            const int step = stepList_.getSelectedRow();
            if (auto* macro = current(); macro != nullptr && step >= 0 && step < (int) macro->steps.size())
            {
                macro->steps.erase(macro->steps.begin() + step);
                changed();
                stepList_.selectRow(juce::jmin(step, (int) macro->steps.size() - 1));
            }
        };
        upButton_.onClick       = [this] { moveStep(-1); };
        downButton_.onClick     = [this] { moveStep(1); };
        runButton_.onClick      = [this] { if (current() != nullptr && onRun) onRun(macroList_.getSelectedRow()); };
        runFilesButton_.onClick = [this] { if (current() != nullptr && onRunOnFiles) onRunOnFiles(macroList_.getSelectedRow()); };

        addCommandButton_.setTooltip("A command that acts at once: one that asks something first can't be a step");
        addEffectsButton_.setTooltip("A chain of effects with their settings, applied to the selection");
        runButton_.setTooltip("Run the steps in order on the selection; each is its own undo step");
        runFilesButton_.setTooltip("Run a macro of effects over every audio file in a folder, writing new files");

        for (auto* button : { &newButton_, &renameButton_, &deleteButton_, &addCommandButton_, &addEffectsButton_,
                              &editButton_, &removeButton_, &upButton_, &downButton_, &runButton_, &runFilesButton_ })
            addAndMakeVisible(*button);

        setSize(720, 440);
        updateButtons();
    }

    /** The macros to show, keeping the selection where it can. */
    void setMacros(std::vector<macros::Macro> macros)
    {
        const int row = macroList_.getSelectedRow();
        macros_ = std::move(macros);
        macroList_.updateContent();
        macroList_.selectRow(juce::jlimit(-1, (int) macros_.size() - 1, row < 0 ? 0 : row));
        refreshSteps();
    }

    /** The effects chosen for @p step of @p macro (-1: a new step, added
        after the selected one). */
    void setEffects(int macro, int step, std::vector<model::EffectSlot> effects)
    {
        if (macro < 0 || macro >= (int) macros_.size())
            return;
        auto& steps = macros_[(size_t) macro].steps;
        if (step >= 0 && step < (int) steps.size())
            steps[(size_t) step].effects = std::move(effects);
        else
        {
            const int at = stepList_.getSelectedRow() >= 0 ? stepList_.getSelectedRow() + 1 : (int) steps.size();
            steps.insert(steps.begin() + juce::jmin(at, (int) steps.size()), macros::Step { {}, std::move(effects) });
            step = juce::jmin(at, (int) steps.size() - 1);
        }
        changed();
        stepList_.selectRow(step);
    }

    void selectMacro(int index) { macroList_.selectRow(index); }
    const std::vector<macros::Macro>& macrosForTesting() const noexcept { return macros_; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto bottom = area.removeFromBottom(28);
        runFilesButton_.setBounds(bottom.removeFromRight(130));
        bottom.removeFromRight(6);
        runButton_.setBounds(bottom.removeFromRight(90));
        area.removeFromBottom(8);

        auto left = area.removeFromLeft(220);
        auto leftButtons = left.removeFromBottom(26);
        const int third = leftButtons.getWidth() / 3;
        newButton_.setBounds(leftButtons.removeFromLeft(third).reduced(1, 0));
        renameButton_.setBounds(leftButtons.removeFromLeft(third).reduced(1, 0));
        deleteButton_.setBounds(leftButtons.reduced(1, 0));
        left.removeFromBottom(4);
        macroList_.setBounds(left);
        area.removeFromLeft(8);

        auto rightButtons = area.removeFromBottom(26);
        const int sixth = rightButtons.getWidth() / 6;
        for (auto* button : { &addCommandButton_, &addEffectsButton_, &editButton_, &removeButton_, &upButton_ })
            button->setBounds(rightButtons.removeFromLeft(sixth).reduced(1, 0));
        downButton_.setBounds(rightButtons.reduced(1, 0));
        area.removeFromBottom(4);
        stepList_.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    }

private:
    /** A list box model from lambdas. */
    struct Model final : juce::ListBoxModel
    {
        std::function<int()>             rows;
        std::function<juce::String(int)> text;
        std::function<void(int)>         selected, doubleClicked;

        int getNumRows() override { return rows ? rows() : 0; }

        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected) override
        {
            if (row < 0 || row >= getNumRows())
                return;
            if (isSelected)
                g.fillAll(juce::Colours::steelblue.withAlpha(0.45f));
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(text(row), 6, 0, width - 12, height, juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged(int row) override { if (selected) selected(row); }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { if (doubleClicked) doubleClicked(row); }
        juce::String getNameForRow(int row) override { return row >= 0 && row < getNumRows() ? text(row) : juce::String(); }
    };

    macros::Macro* current()
    {
        const int row = macroList_.getSelectedRow();
        return row >= 0 && row < (int) macros_.size() ? &macros_[(size_t) row] : nullptr;
    }

    void changed()
    {
        macroList_.updateContent();
        macroList_.repaint();
        refreshSteps();
        if (onChanged)
            onChanged(macros_);
    }

    void refreshSteps()
    {
        stepList_.updateContent();
        stepList_.repaint();
        updateButtons();
    }

    void updateButtons()
    {
        const auto* macro   = current();
        const int   step    = stepList_.getSelectedRow();
        const bool  hasStep = macro != nullptr && step >= 0 && step < (int) macro->steps.size();
        for (auto* button : { &renameButton_, &deleteButton_, &addCommandButton_, &addEffectsButton_ })
            button->setEnabled(macro != nullptr);
        runButton_.setEnabled(macro != nullptr && ! macro->steps.empty());
        runFilesButton_.setEnabled(macro != nullptr && ! macro->steps.empty() && macros::effectsOnly(*macro).has_value());
        editButton_.setEnabled(hasStep && macro->steps[(size_t) step].isEffects());
        removeButton_.setEnabled(hasStep);
        upButton_.setEnabled(hasStep && step > 0);
        downButton_.setEnabled(hasStep && step < (int) macro->steps.size() - 1);
    }

    void moveStep(int delta)
    {
        auto*     macro = current();
        const int step  = stepList_.getSelectedRow();
        if (macro == nullptr || step < 0 || step + delta < 0 || step + delta >= (int) macro->steps.size())
            return;
        std::swap(macro->steps[(size_t) step], macro->steps[(size_t) (step + delta)]);
        changed();
        stepList_.selectRow(step + delta);
    }

    void editStep(int step)
    {
        auto* macro = current();
        if (macro == nullptr || step < 0 || step >= (int) macro->steps.size() || ! macro->steps[(size_t) step].isEffects())
            return;
        if (onEditEffects)
            onEditEffects(macroList_.getSelectedRow(), step, macro->steps[(size_t) step].effects);
    }

    void chooseCommand()
    {
        if (current() == nullptr)
            return;
        std::map<juce::String, juce::PopupMenu> byCategory;
        std::vector<const commands::Definition*> offered;
        for (const auto& definition : commands::all())
        {
            if (! macros::recordable(definition))
                continue;
            offered.push_back(&definition);
            byCategory[definition.category].addItem((int) offered.size(),
                                                    juce::String(definition.name).upToFirstOccurrenceOf("   ", false, false));
        }
        juce::PopupMenu menu;
        for (auto& [category, sub] : byCategory)
            menu.addSubMenu(category, sub);

        juce::Component::SafePointer<MacrosDialog> self(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addCommandButton_), [self, offered](int chosen)
        {
            if (self == nullptr || chosen <= 0 || chosen > (int) offered.size() || self->current() == nullptr)
                return;
            auto&     steps = self->current()->steps;
            const int at    = self->stepList_.getSelectedRow() >= 0 ? self->stepList_.getSelectedRow() + 1 : (int) steps.size();
            const int index = juce::jmin(at, (int) steps.size());
            steps.insert(steps.begin() + index, macros::Step { offered[(size_t) chosen - 1]->name, {} });
            self->changed();
            self->stepList_.selectRow(index);
        });
    }

    void askName(const juce::String& title, const juce::String& initial, std::function<void(const juce::String&)> then)
    {
        auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", initial, "Name:");
        window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MacrosDialog> self(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([self, window, then](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            const auto name = window->getTextEditorContents("name").trim();
            if (self == nullptr || result != 1 || name.isEmpty())
                return;
            then(name);
        }));
    }

    std::vector<macros::Macro> macros_;
    Model                      macroModel_, stepModel_;
    juce::ListBox              macroList_, stepList_;
    juce::TextButton newButton_ { "New" }, renameButton_ { "Rename" }, deleteButton_ { "Delete" },
                     addCommandButton_ { "+ Command" }, addEffectsButton_ { "+ Effects" }, editButton_ { "Edit" },
                     removeButton_ { "Remove" }, upButton_ { "Up" }, downButton_ { "Down" },
                     runButton_ { "Run" }, runFilesButton_ { "Apply to Files..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MacrosDialog)
};

} // namespace soundsplice
