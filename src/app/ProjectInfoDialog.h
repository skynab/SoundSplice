#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

namespace soundsplice
{
/**
    File > Project Info: the title, artist, album and the rest that exports
    write as tags, and a cover image. Edits a copy; Save hands it back.
*/
class ProjectInfoDialog final : public juce::Component
{
public:
    std::function<void(const model::ProjectInfo&)> onSave;
    std::function<void()>                          onCancel;

    explicit ProjectInfoDialog(model::ProjectInfo info) : info_(std::move(info))
    {
        info_.forEachField([this](const char* key, std::string& value)
        {
            if (juce::String(key) == "cover")
                return;
            Field field;
            field.key    = key;
            field.label  = std::make_unique<juce::Label>(juce::String(), labelFor(key));
            field.editor = std::make_unique<juce::TextEditor>();
            field.editor->setText(juce::String::fromUTF8(value.c_str()), false);
            field.editor->setTitle(labelFor(key));
            field.editor->setMultiLine(juce::String(key) == "comment");
            addAndMakeVisible(*field.label);
            addAndMakeVisible(*field.editor);
            fields_.push_back(std::move(field));
        });

        coverLabel_.setText("Cover", juce::dontSendNotification);
        addAndMakeVisible(coverLabel_);
        coverPath_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
        coverPath_.setMinimumHorizontalScale(0.6f);
        addAndMakeVisible(coverPath_);
        chooseCover_.onClick = [this] { chooseCover(); };
        clearCover_.onClick  = [this]
        {
            info_.coverArt.clear();
            refreshCover();
        };
        addAndMakeVisible(chooseCover_);
        addAndMakeVisible(clearCover_);

        hint_.setText("Written into exported files as tags: ID3 in MP3, Vorbis comments in FLAC and Ogg, INFO and bext in WAV. "
                      "Markers become chapters.",
                      juce::dontSendNotification);
        hint_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        addAndMakeVisible(hint_);

        saveButton_.onClick   = [this] { if (onSave) onSave(read()); };
        cancelButton_.onClick = [this] { if (onCancel) onCancel(); };
        addAndMakeVisible(saveButton_);
        addAndMakeVisible(cancelButton_);

        refreshCover();
        setSize(520, 470);
    }

    /** What's in the boxes now. */
    model::ProjectInfo read() const
    {
        auto info = info_;
        info.forEachField([this](const char* key, std::string& value)
        {
            for (const auto& field : fields_)
                if (field.key == key)
                    value = field.editor->getText().trim().toStdString();
        });
        return info;
    }

    juce::TextEditor* editorForTesting(const juce::String& key)
    {
        for (auto& field : fields_)
            if (field.key == key)
                return field.editor.get();
        return nullptr;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
        if (cover_.isValid())
            g.drawImageWithin(cover_, coverArea_.getX(), coverArea_.getY(), coverArea_.getWidth(), coverArea_.getHeight(),
                              juce::RectanglePlacement::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);
        auto buttons = area.removeFromBottom(28);
        cancelButton_.setBounds(buttons.removeFromRight(90));
        buttons.removeFromRight(6);
        saveButton_.setBounds(buttons.removeFromRight(90));
        area.removeFromBottom(8);
        hint_.setBounds(area.removeFromBottom(36));
        area.removeFromBottom(4);

        for (auto& field : fields_)
        {
            auto line = area.removeFromTop(field.key == "comment" ? 56 : 26);
            area.removeFromTop(6);
            field.label->setBounds(line.removeFromLeft(80));
            field.editor->setBounds(line);
        }

        auto cover = area.removeFromTop(64);
        coverLabel_.setBounds(cover.removeFromLeft(80).removeFromTop(26));
        coverArea_ = cover.removeFromLeft(64);
        cover.removeFromLeft(8);
        auto coverButtons = cover.removeFromTop(26);
        chooseCover_.setBounds(coverButtons.removeFromLeft(100));
        coverButtons.removeFromLeft(6);
        clearCover_.setBounds(coverButtons.removeFromLeft(70));
        coverPath_.setBounds(cover.removeFromTop(26));
    }

private:
    struct Field
    {
        juce::String                      key;
        std::unique_ptr<juce::Label>      label;
        std::unique_ptr<juce::TextEditor> editor;
    };

    static juce::String labelFor(const juce::String& key)
    {
        return key == "track" ? juce::String("Track no.") : key.substring(0, 1).toUpperCase() + key.substring(1);
    }

    void refreshCover()
    {
        const juce::File file(juce::String::fromUTF8(info_.coverArt.c_str()));
        cover_ = info_.coverArt.empty() ? juce::Image() : juce::ImageFileFormat::loadFrom(file);
        coverPath_.setText(info_.coverArt.empty() ? juce::String("None") : file.getFileName(), juce::dontSendNotification);
        clearCover_.setEnabled(! info_.coverArt.empty());
        repaint();
    }

    void chooseCover()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Cover Image", juce::File(), "*.jpg;*.jpeg;*.png");
        juce::Component::SafePointer<ProjectInfoDialog> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [self](const juce::FileChooser& chooser)
                              {
                                  if (self == nullptr || chooser.getResult() == juce::File())
                                      return;
                                  self->info_.coverArt = chooser.getResult().getFullPathName().toStdString();
                                  self->refreshCover();
                              });
    }

    model::ProjectInfo                 info_;
    std::vector<Field>                 fields_;
    juce::Label                        coverLabel_, coverPath_, hint_;
    juce::TextButton                   chooseCover_ { "Choose..." }, clearCover_ { "Clear" };
    juce::TextButton                   saveButton_ { "Save" }, cancelButton_ { "Cancel" };
    juce::Image                        cover_;
    juce::Rectangle<int>               coverArea_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectInfoDialog)
};

} // namespace soundsplice
