#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

#include "model/EssentialSound.h"

namespace soundsplice
{
/**
    The Essential Sound pane (Audition's): tag the selected audio clip as
    Dialogue, Music, SFX or Ambience, then move a few sliders - its role's
    tasks (model/EssentialSound.h) - and match the loudness of every clip with
    the same tag, or duck Music and Ambience under the Dialogue.

    Owns no document state: the owner feeds it the selected clip's tag and
    amounts and acts on its callbacks.
*/
class EssentialSoundPane final : public juce::Component
{
public:
    std::function<void(model::SoundRole)>                 onRoleChosen;
    std::function<void(const std::string& task, float)>   onAmountChanged; // live, as a slider moves
    std::function<void(const std::string& task)>          onAmountDragStart;
    std::function<void(const std::string& task)>          onAmountDragEnd;
    std::function<void(model::SoundRole, double lufs)>    onMatchLoudness;
    std::function<void(model::SoundRole, float depthDb)>  onDuck;

    EssentialSoundPane()
    {
        static constexpr model::SoundRole kRoles[] { model::SoundRole::Dialogue, model::SoundRole::Music,
                                                     model::SoundRole::Sfx, model::SoundRole::Ambience };
        for (const auto role : kRoles)
        {
            auto button = std::make_unique<juce::TextButton>(model::essential::roleName(role));
            button->setClickingTogglesState(false);
            button->setColour(juce::TextButton::buttonOnColourId, juce::Colours::steelblue);
            button->onClick = [this, role] { if (onRoleChosen) onRoleChosen(role_ == role ? model::SoundRole::None : role); };
            button->setTooltip(juce::String("Tag the clip as ") + model::essential::roleName(role) + " - click again to clear it");
            addAndMakeVisible(*button);
            roleButtons_.push_back({ role, std::move(button) });
        }

        for (const auto* name : { "-14 LUFS", "-16 LUFS", "-18 LUFS", "-23 LUFS", "-24 LUFS" })
            loudnessTarget_.addItem(name, loudnessTarget_.getNumItems() + 1);
        loudnessTarget_.setSelectedItemIndex(1, juce::dontSendNotification);
        addAndMakeVisible(loudnessTarget_);
        matchButton_.onClick = [this]
        {
            static constexpr double kTargets[] { -14.0, -16.0, -18.0, -23.0, -24.0 };
            if (onMatchLoudness)
                onMatchLoudness(role_, kTargets[juce::jlimit(0, 4, loudnessTarget_.getSelectedItemIndex())]);
        };
        addAndMakeVisible(matchButton_);

        duckDepth_.setRange(-30.0, -3.0, 0.5);
        duckDepth_.setValue(-15.0, juce::dontSendNotification);
        duckDepth_.setTextValueSuffix(" dB");
        duckDepth_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        addAndMakeVisible(duckDepth_);
        duckButton_.onClick = [this] { if (onDuck) onDuck(role_, (float) duckDepth_.getValue()); };
        duckButton_.setTooltip("Turn this tag's clips down wherever there's Dialogue, as volume curves you can edit");
        addAndMakeVisible(duckButton_);

        for (auto* label : { &heading_, &tasksHeading_, &loudnessHeading_, &duckHeading_ })
        {
            label->setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
            addAndMakeVisible(*label);
        }
        tasksHeading_.setText("Tasks", juce::dontSendNotification);
        loudnessHeading_.setText("Loudness", juce::dontSendNotification);
        duckHeading_.setText("Ducking", juce::dontSendNotification);

        setClip(false, model::SoundRole::None, {}, {});
    }

    /** The selected clip's tag and amounts; @p hasClip false when no audio
        clip is selected. */
    void setClip(bool hasClip, model::SoundRole role, const std::map<std::string, float>& amounts, const juce::String& name)
    {
        hasClip_ = hasClip;
        heading_.setText(hasClip ? (name + (role == model::SoundRole::None ? juce::String(": choose what it is")
                                                                            : juce::String(" is ") + model::essential::roleName(role)))
                                 : juce::String("Select an audio clip to tag it"),
                         juce::dontSendNotification);

        for (auto& [r, button] : roleButtons_)
        {
            button->setEnabled(hasClip);
            button->setToggleState(r == role, juce::dontSendNotification);
        }

        if (role != role_ || ! hasClip)
            rebuildTasks(hasClip ? role : model::SoundRole::None);
        role_ = hasClip ? role : model::SoundRole::None;

        for (auto& row : rows_)
        {
            const auto it = amounts.find(row.task);
            row.slider->setValue(it != amounts.end() ? it->second : 0.0f, juce::dontSendNotification);
        }

        const bool tagged = role_ != model::SoundRole::None;
        matchButton_.setButtonText(tagged ? juce::String("Match All ") + model::essential::roleName(role_) + " Clips"
                                          : juce::String("Match Loudness"));
        for (auto* c : { (juce::Component*) &loudnessHeading_, (juce::Component*) &loudnessTarget_, (juce::Component*) &matchButton_ })
            c->setVisible(tagged);
        const bool ducks = role_ == model::SoundRole::Music || role_ == model::SoundRole::Ambience;
        for (auto* c : { (juce::Component*) &duckHeading_, (juce::Component*) &duckDepth_, (juce::Component*) &duckButton_ })
            c->setVisible(ducks);
        tasksHeading_.setVisible(tagged);
        resized();
    }

    int  taskCountForTesting() const noexcept { return (int) rows_.size(); }
    juce::Slider& taskSliderForTesting(int i) { return *rows_[(size_t) i].slider; }
    juce::Button& roleButtonForTesting(int i) { return *roleButtons_[(size_t) i].second; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        heading_.setBounds(area.removeFromTop(20));
        area.removeFromTop(4);

        auto roles = area.removeFromTop(26);
        const int width = roles.getWidth() / juce::jmax(1, (int) roleButtons_.size());
        for (auto& [role, button] : roleButtons_)
            button->setBounds(roles.removeFromLeft(width).reduced(2, 0));
        area.removeFromTop(8);

        if (tasksHeading_.isVisible())
            tasksHeading_.setBounds(area.removeFromTop(18));
        for (auto& row : rows_)
        {
            auto line = area.removeFromTop(24);
            row.label->setBounds(line.removeFromLeft(juce::jmin(130, line.getWidth() / 2)));
            row.slider->setBounds(line);
        }

        if (loudnessHeading_.isVisible())
        {
            area.removeFromTop(8);
            loudnessHeading_.setBounds(area.removeFromTop(18));
            auto line = area.removeFromTop(26);
            loudnessTarget_.setBounds(line.removeFromLeft(juce::jmin(110, line.getWidth() / 3)).reduced(0, 1));
            line.removeFromLeft(6);
            matchButton_.setBounds(line.reduced(0, 1));
        }

        if (duckHeading_.isVisible())
        {
            area.removeFromTop(8);
            duckHeading_.setBounds(area.removeFromTop(18));
            auto line = area.removeFromTop(26);
            duckButton_.setBounds(line.removeFromRight(juce::jmin(150, line.getWidth() / 2)).reduced(0, 1));
            duckDepth_.setBounds(line);
        }
    }

private:
    struct Row
    {
        std::string                   task;
        std::unique_ptr<juce::Label>  label;
        std::unique_ptr<juce::Slider> slider;
    };

    void rebuildTasks(model::SoundRole role)
    {
        rows_.clear();
        for (const auto& task : model::essential::tasksFor(role))
        {
            Row row;
            row.task   = task.id;
            row.label  = std::make_unique<juce::Label>(juce::String(), task.name);
            row.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            row.slider->setRange(0.0, 10.0, 0.1);
            row.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
            row.slider->setTooltip(task.tooltip);
            row.label->setTooltip(task.tooltip);
            const std::string id = task.id;
            auto* slider = row.slider.get();
            slider->onValueChange = [this, id, slider] { if (onAmountChanged) onAmountChanged(id, (float) slider->getValue()); };
            slider->onDragStart   = [this, id] { if (onAmountDragStart) onAmountDragStart(id); };
            slider->onDragEnd     = [this, id] { if (onAmountDragEnd) onAmountDragEnd(id); };
            addAndMakeVisible(*row.label);
            addAndMakeVisible(*row.slider);
            rows_.push_back(std::move(row));
        }
    }

    bool                  hasClip_ = false;
    model::SoundRole      role_    = model::SoundRole::None;
    std::vector<std::pair<model::SoundRole, std::unique_ptr<juce::TextButton>>> roleButtons_;
    std::vector<Row>      rows_;
    juce::Label           heading_, tasksHeading_, loudnessHeading_, duckHeading_;
    juce::ComboBox        loudnessTarget_;
    juce::TextButton      matchButton_ { "Match Loudness" }, duckButton_ { "Duck Under Dialogue" };
    juce::Slider          duckDepth_ { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EssentialSoundPane)
};

} // namespace soundsplice
