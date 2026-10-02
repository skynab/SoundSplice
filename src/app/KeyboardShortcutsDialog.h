#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "ShortcutSets.h"

namespace soundsplice
{
/**
    Keyboard Shortcuts: every command by category with its keys, to change,
    add or remove (JUCE's key mapping editor), plus Import and Export of
    whole sets as files, and Reset to put the defaults back.

    Edits go straight into the command manager's mappings; @p onChanged is
    told after each so the owner can save them.
*/
class KeyboardShortcutsDialog final : public juce::Component,
                                      private juce::ChangeListener
{
public:
    std::function<void()> onChanged;

    explicit KeyboardShortcutsDialog(juce::KeyPressMappingSet& mappings)
        : mappings_(mappings), editor_(mappings, true)
    {
        addAndMakeVisible(editor_);
        mappings_.addChangeListener(this);

        hint_.setText("Click + beside a command to give it a key, or a key to change or remove it.",
                      juce::dontSendNotification);
        hint_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        addAndMakeVisible(hint_);

        importButton_.onClick = [this] { importSet(); };
        exportButton_.onClick = [this] { exportSet(); };
        importButton_.setTooltip("Load a shortcut set saved from SoundSplice, replacing the keys you have now");
        exportButton_.setTooltip("Save your shortcuts to a file, to keep or to use on another computer");
        addAndMakeVisible(importButton_);
        addAndMakeVisible(exportButton_);

        setSize(620, 560);
    }

    ~KeyboardShortcutsDialog() override { mappings_.removeChangeListener(this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto bottom = area.removeFromBottom(28);
        exportButton_.setBounds(bottom.removeFromRight(110));
        bottom.removeFromRight(6);
        importButton_.setBounds(bottom.removeFromRight(110));
        hint_.setBounds(bottom);
        area.removeFromBottom(6);
        editor_.setBounds(area);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (onChanged)
            onChanged();
    }

    void importSet()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Import Shortcuts", juce::File(), "*.xml");
        juce::Component::SafePointer<KeyboardShortcutsDialog> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [self](const juce::FileChooser& chooser)
        {
            if (self == nullptr || chooser.getResult() == juce::File())
                return;
            const auto bindings = shortcutsets::fromXml(chooser.getResult().loadFileAsString());
            if (! bindings)
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import Shortcuts",
                                                       "That file isn't a SoundSplice shortcut set.");
                return;
            }
            shortcutsets::apply(self->mappings_, *bindings); // the change message saves them
        });
    }

    void exportSet()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Export Shortcuts",
                                                       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                           .getChildFile("SoundSplice Shortcuts.xml"),
                                                       "*.xml");
        juce::Component::SafePointer<KeyboardShortcutsDialog> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [self](const juce::FileChooser& chooser)
        {
            if (self == nullptr || chooser.getResult() == juce::File())
                return;
            const auto file = chooser.getResult().withFileExtension("xml");
            if (! file.replaceWithText(shortcutsets::toXml(shortcutsets::customised(self->mappings_))))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export Shortcuts",
                                                       "Couldn't write " + file.getFullPathName());
        });
    }

    juce::KeyPressMappingSet&          mappings_;
    juce::KeyMappingEditorComponent    editor_;
    juce::Label                        hint_;
    juce::TextButton                   importButton_ { "Import..." }, exportButton_ { "Export..." };
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeyboardShortcutsDialog)
};

} // namespace soundsplice
