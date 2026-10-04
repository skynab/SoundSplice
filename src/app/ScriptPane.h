#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include <juce_gui_extra/juce_gui_extra.h>

#include "Scripting.h"

namespace soundsplice
{
/**
    The Script pane: a Lua editor, Run, and what the script printed or why
    it failed. Open and Save keep scripts as .lua files; Help lists what a
    script can call (scripting::Engine::reference).

    Runs nothing itself - onRun hands the code to the owner, which has the
    engine and the document, and calls showResult with what came back.
*/
class ScriptPane final : public juce::Component,
                         private juce::KeyListener
{
public:
    std::function<void(const juce::String& code, const juce::String& name)> onRun;
    std::function<void(const juce::String& code)>                           onCodeChanged;

    ScriptPane()
    {
        editor_ = std::make_unique<juce::CodeEditorComponent>(document_, &tokeniser_);
        editor_->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
        editor_->setTabSize(4, true);
        editor_->setTitle("Script");
        addAndMakeVisible(*editor_);
        editor_->addKeyListener(this); // ahead of the editor, which would take Return
        document_.addListener(&changes_);
        changes_.owner = this;

        output_.setMultiLine(true);
        output_.setReadOnly(true);
        output_.setCaretVisible(false);
        output_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        output_.setTitle("Script output");
        addAndMakeVisible(output_);

        runButton_.onClick  = [this] { run(); };
        openButton_.onClick = [this] { open(); };
        saveButton_.onClick = [this] { save(); };
        helpButton_.onClick = [this] { showHelp(); };
        runButton_.setTooltip("Run the script (Ctrl+Return in the editor)");
        for (auto* button : { &runButton_, &openButton_, &saveButton_, &helpButton_ })
            addAndMakeVisible(*button);
        addAndMakeVisible(nameLabel_);
        nameLabel_.setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
        setName({});
    }

    ~ScriptPane() override
    {
        editor_->removeKeyListener(this);
        document_.removeListener(&changes_);
    }

    void setCode(const juce::String& code)
    {
        document_.replaceAllContent(code);
        document_.clearUndoHistory();
    }

    juce::String code() const { return document_.getAllContent(); }

    void showResult(const scripting::Result& result)
    {
        juce::String text = juce::String::fromUTF8(result.output.c_str());
        if (! result.ok)
            text << (text.isEmpty() ? "" : "\n") << "Error: " << juce::String::fromUTF8(result.error.c_str());
        else if (text.isEmpty())
            text = "Done.";
        output_.setText(text, false);
        output_.moveCaretToEnd();
    }

    void run()
    {
        if (onRun)
            onRun(code(), scriptName_.isEmpty() ? juce::String("script") : scriptName_);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto top  = area.removeFromTop(26);
        runButton_.setBounds(top.removeFromLeft(70));
        top.removeFromLeft(4);
        openButton_.setBounds(top.removeFromLeft(70));
        top.removeFromLeft(4);
        saveButton_.setBounds(top.removeFromLeft(70));
        top.removeFromLeft(4);
        helpButton_.setBounds(top.removeFromLeft(60));
        top.removeFromLeft(8);
        nameLabel_.setBounds(top);
        area.removeFromTop(4);
        output_.setBounds(area.removeFromBottom(juce::jmax(60, area.getHeight() / 4)));
        area.removeFromBottom(4);
        editor_->setBounds(area);
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key == juce::KeyPress(juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0))
        {
            run();
            return true;
        }
        return false;
    }

    juce::Button& runButtonForTesting() noexcept { return runButton_; }

private:
    struct Changes final : juce::CodeDocument::Listener
    {
        ScriptPane* owner = nullptr;
        void codeDocumentTextInserted(const juce::String&, int) override { changed(); }
        void codeDocumentTextDeleted(int, int) override { changed(); }
        void changed() const
        {
            if (owner != nullptr && owner->onCodeChanged)
                owner->onCodeChanged(owner->code());
        }
    };

    void setName(const juce::String& name)
    {
        scriptName_ = name;
        nameLabel_.setText(name.isEmpty() ? juce::String("Untitled script") : name, juce::dontSendNotification);
    }

    void open()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Open Script", lastFile_, "*.lua");
        juce::Component::SafePointer<ScriptPane> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [self](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (self == nullptr || file == juce::File())
                return;
            self->lastFile_ = file;
            self->setCode(file.loadFileAsString());
            self->setName(file.getFileName());
        });
    }

    void save()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Save Script",
                                                       lastFile_ != juce::File() ? lastFile_
                                                           : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                                 .getChildFile("Script.lua"),
                                                       "*.lua");
        juce::Component::SafePointer<ScriptPane> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [self](const juce::FileChooser& chooser)
        {
            if (self == nullptr || chooser.getResult() == juce::File())
                return;
            const auto file = chooser.getResult().withFileExtension("lua");
            if (file.replaceWithText(self->code()))
            {
                self->lastFile_ = file;
                self->setName(file.getFileName());
            }
            else
                self->output_.setText("Couldn't write " + file.getFullPathName(), false);
        });
    }

    void showHelp()
    {
        juce::String text = "Lua 5.4, with:\n";
        for (const auto& [call, what] : scripting::Engine::reference())
            text << "  " << call << (what.empty() ? juce::String() : "  -- " + juce::String(what)) << "\n";
        text << "Tracks are numbered from 1; times are in seconds. Each change is its own undo step.";
        output_.setText(text, false);
    }

    juce::CodeDocument                         document_;
    juce::LuaTokeniser                         tokeniser_;
    std::unique_ptr<juce::CodeEditorComponent> editor_;
    Changes                                    changes_;
    juce::TextEditor                           output_;
    juce::TextButton runButton_ { "Run" }, openButton_ { "Open..." }, saveButton_ { "Save..." }, helpButton_ { "Help" };
    juce::Label                                nameLabel_;
    juce::String                               scriptName_;
    juce::File                                 lastFile_;
    std::unique_ptr<juce::FileChooser>         chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScriptPane)
};

} // namespace soundsplice
