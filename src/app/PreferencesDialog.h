#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace soundsplice
{
/**
    Preferences: the app's settings in one window, a tab per area.

    The window knows nothing about what the settings mean. Each row is a
    label and a control, with a getter and a setter the owner supplies, so a
    setting here is the same setting the menus change: most rows just run
    the command that already toggles it (see MainComponent::preferencePages).
    Rows read their values again whenever a tab is shown, so a change made
    from a menu meanwhile shows up.
*/
namespace prefs
{
    struct Row
    {
        enum class Kind
        {
            Toggle,
            Choice,
            Number,
            Folder,
            Action,
            Custom,
            Heading,
        };

        Kind         kind = Kind::Toggle;
        juce::String label, tooltip;

        std::function<bool()>     getBool;
        std::function<void(bool)> setBool;

        juce::StringArray         choices;
        std::function<int()>      getChoice;
        std::function<void(int)>  setChoice;

        double                       minimum = 0.0, maximum = 1.0, step = 1.0;
        juce::String                 unit;
        std::function<double()>      getNumber;
        std::function<void(double)>  setNumber;

        std::function<juce::File()>            getFolder;
        std::function<void(const juce::File&)> setFolder; // empty: shown, not changed

        juce::String                  actionText;
        std::function<void()>         action;
        std::function<juce::String()> status; // beside an action: "12 MB", say

        std::function<std::unique_ptr<juce::Component>()> makeCustom;
        int                                               customHeight = 200;
    };

    struct Page
    {
        juce::String     name;
        std::vector<Row> rows;
    };

    inline Row heading(juce::String text)
    {
        Row row;
        row.kind  = Row::Kind::Heading;
        row.label = std::move(text);
        return row;
    }

    inline Row toggle(juce::String label, std::function<bool()> get, std::function<void(bool)> set, juce::String tooltip = {})
    {
        Row row;
        row.kind    = Row::Kind::Toggle;
        row.label   = std::move(label);
        row.getBool = std::move(get);
        row.setBool = std::move(set);
        row.tooltip = std::move(tooltip);
        return row;
    }

    inline Row choice(juce::String label, juce::StringArray choices, std::function<int()> get, std::function<void(int)> set,
                      juce::String tooltip = {})
    {
        Row row;
        row.kind      = Row::Kind::Choice;
        row.label     = std::move(label);
        row.choices   = std::move(choices);
        row.getChoice = std::move(get);
        row.setChoice = std::move(set);
        row.tooltip   = std::move(tooltip);
        return row;
    }

    inline Row number(juce::String label, double minimum, double maximum, double step, juce::String unit,
                      std::function<double()> get, std::function<void(double)> set, juce::String tooltip = {})
    {
        Row row;
        row.kind      = Row::Kind::Number;
        row.label     = std::move(label);
        row.minimum   = minimum;
        row.maximum   = maximum;
        row.step      = step;
        row.unit      = std::move(unit);
        row.getNumber = std::move(get);
        row.setNumber = std::move(set);
        row.tooltip   = std::move(tooltip);
        return row;
    }

    inline Row folder(juce::String label, std::function<juce::File()> get, std::function<void(const juce::File&)> set = {},
                      juce::String tooltip = {})
    {
        Row row;
        row.kind      = Row::Kind::Folder;
        row.label     = std::move(label);
        row.getFolder = std::move(get);
        row.setFolder = std::move(set);
        row.tooltip   = std::move(tooltip);
        return row;
    }

    inline Row action(juce::String label, juce::String button, std::function<void()> run,
                      std::function<juce::String()> status = {}, juce::String tooltip = {})
    {
        Row row;
        row.kind       = Row::Kind::Action;
        row.label      = std::move(label);
        row.actionText = std::move(button);
        row.action     = std::move(run);
        row.status     = std::move(status);
        row.tooltip    = std::move(tooltip);
        return row;
    }

    inline Row custom(std::function<std::unique_ptr<juce::Component>()> make, int height)
    {
        Row row;
        row.kind         = Row::Kind::Custom;
        row.makeCustom   = std::move(make);
        row.customHeight = height;
        return row;
    }

    /** One tab: its rows laid out in a column, label on the left. */
    class PageComponent final : public juce::Component
    {
    public:
        explicit PageComponent(std::vector<Row> rows) : rows_(std::move(rows))
        {
            for (auto& row : rows_)
                widgets_.push_back(build(row));
            refresh();
        }

        /** Reads every value again. */
        void refresh()
        {
            for (size_t i = 0; i < rows_.size(); ++i)
            {
                auto& row    = rows_[i];
                auto& widget = widgets_[i];
                switch (row.kind)
                {
                    case Row::Kind::Toggle:
                        if (row.getBool)
                            static_cast<juce::ToggleButton*>(widget.control.get())->setToggleState(row.getBool(), juce::dontSendNotification);
                        break;
                    case Row::Kind::Choice:
                        if (row.getChoice)
                            static_cast<juce::ComboBox*>(widget.control.get())->setSelectedItemIndex(row.getChoice(), juce::dontSendNotification);
                        break;
                    case Row::Kind::Number:
                        if (row.getNumber)
                            static_cast<juce::Slider*>(widget.control.get())->setValue(row.getNumber(), juce::dontSendNotification);
                        break;
                    case Row::Kind::Folder:
                        if (row.getFolder)
                            widget.value->setText(row.getFolder().getFullPathName(), juce::dontSendNotification);
                        break;
                    case Row::Kind::Action:
                        if (row.status)
                            widget.value->setText(row.status(), juce::dontSendNotification);
                        break;
                    case Row::Kind::Custom:
                    case Row::Kind::Heading:
                        break;
                }
            }
        }

        /** The height its rows need, for the viewport. */
        int idealHeight() const
        {
            int height = kPadding;
            for (const auto& row : rows_)
                height += heightOf(row) + kGap;
            return height + kPadding;
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(kPadding, kPadding);
            for (size_t i = 0; i < rows_.size(); ++i)
            {
                auto line = area.removeFromTop(heightOf(rows_[i]));
                area.removeFromTop(kGap);
                auto& widget = widgets_[i];

                if (rows_[i].kind == Row::Kind::Custom || rows_[i].kind == Row::Kind::Heading)
                {
                    (widget.control != nullptr ? widget.control.get() : widget.label.get())->setBounds(line);
                    continue;
                }

                widget.label->setBounds(line.removeFromLeft(juce::jmin(220, line.getWidth() / 2)));
                line.removeFromLeft(8);
                if (widget.second != nullptr)
                    widget.second->setBounds(line.removeFromRight(70).reduced(0, 2));
                if (widget.third != nullptr)
                {
                    line.removeFromRight(4);
                    widget.third->setBounds(line.removeFromRight(90).reduced(0, 2));
                }
                if (widget.value != nullptr && widget.control != nullptr)
                {
                    widget.control->setBounds(line.removeFromLeft(juce::jmin(160, line.getWidth())).reduced(0, 2));
                    line.removeFromLeft(8);
                    widget.value->setBounds(line);
                }
                else if (widget.value != nullptr)
                    widget.value->setBounds(line);
                else if (widget.control != nullptr)
                    widget.control->setBounds(rows_[i].kind == Row::Kind::Toggle ? line.removeFromLeft(40) : line.reduced(0, 2));
            }
        }

        juce::Component* controlForTesting(int row) { return widgets_[(size_t) row].control.get(); }

    private:
        static constexpr int kPadding = 12, kGap = 6, kRowHeight = 28;

        struct Widgets
        {
            std::unique_ptr<juce::Label>     label, value;
            std::unique_ptr<juce::Component> control, second, third;
        };

        static int heightOf(const Row& row)
        {
            return row.kind == Row::Kind::Custom ? row.customHeight : row.kind == Row::Kind::Heading ? 24 : kRowHeight;
        }

        Widgets build(Row& row)
        {
            Widgets widget;
            widget.label = std::make_unique<juce::Label>(juce::String(), row.label);
            widget.label->setTooltip(row.tooltip);
            if (row.kind == Row::Kind::Heading)
            {
                widget.label->setFont(juce::FontOptions(15.0f, juce::Font::bold));
                addAndMakeVisible(*widget.label);
                return widget;
            }
            if (row.kind != Row::Kind::Custom)
                addAndMakeVisible(*widget.label);

            switch (row.kind)
            {
                case Row::Kind::Toggle:
                {
                    auto button = std::make_unique<juce::ToggleButton>();
                    button->setTitle(row.label);
                    button->setTooltip(row.tooltip);
                    auto* raw = button.get();
                    button->onClick = [this, &row, raw]
                    {
                        if (row.setBool)
                            row.setBool(raw->getToggleState());
                        refresh();
                    };
                    widget.control = std::move(button);
                    break;
                }
                case Row::Kind::Choice:
                {
                    auto box = std::make_unique<juce::ComboBox>();
                    box->addItemList(row.choices, 1);
                    box->setTitle(row.label);
                    box->setTooltip(row.tooltip);
                    auto* raw = box.get();
                    box->onChange = [this, &row, raw]
                    {
                        if (row.setChoice && raw->getSelectedItemIndex() >= 0)
                            row.setChoice(raw->getSelectedItemIndex());
                        refresh();
                    };
                    widget.control = std::move(box);
                    break;
                }
                case Row::Kind::Number:
                {
                    auto slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                    slider->setRange(row.minimum, row.maximum, row.step);
                    slider->setTextValueSuffix(row.unit);
                    slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
                    slider->setTitle(row.label);
                    slider->setTooltip(row.tooltip);
                    auto* raw = slider.get();
                    slider->onValueChange = [&row, raw]
                    {
                        if (row.setNumber)
                            row.setNumber(raw->getValue());
                    };
                    widget.control = std::move(slider);
                    break;
                }
                case Row::Kind::Folder:
                {
                    widget.value = std::make_unique<juce::Label>();
                    widget.value->setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
                    widget.value->setMinimumHorizontalScale(0.6f);
                    addAndMakeVisible(*widget.value);

                    auto show = std::make_unique<juce::TextButton>("Show");
                    show->onClick = [&row] { if (row.getFolder) row.getFolder().revealToUser(); };
                    widget.second = std::move(show);
                    if (row.setFolder)
                    {
                        auto chooseButton = std::make_unique<juce::TextButton>("Choose...");
                        chooseButton->onClick = [this, &row]
                        {
                            chooser_ = std::make_unique<juce::FileChooser>(row.label, row.getFolder ? row.getFolder() : juce::File());
                            juce::Component::SafePointer<PageComponent> self(this);
                            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                                  [self, &row](const juce::FileChooser& chooser)
                                                  {
                                                      if (self == nullptr || chooser.getResult() == juce::File())
                                                          return;
                                                      row.setFolder(chooser.getResult());
                                                      self->refresh();
                                                  });
                        };
                        widget.third = std::move(chooseButton);
                    }
                    break;
                }
                case Row::Kind::Action:
                {
                    if (row.status)
                    {
                        widget.value = std::make_unique<juce::Label>();
                        widget.value->setColour(juce::Label::textColourId, theme::colour(*this, theme::textMutedId));
                        addAndMakeVisible(*widget.value);
                    }
                    auto button = std::make_unique<juce::TextButton>(row.actionText);
                    button->setTooltip(row.tooltip);
                    button->onClick = [this, &row]
                    {
                        if (row.action)
                            row.action();
                        refresh();
                    };
                    widget.control = std::move(button);
                    break;
                }
                case Row::Kind::Custom:
                    widget.control = row.makeCustom ? row.makeCustom() : nullptr;
                    break;
                case Row::Kind::Heading:
                    break;
            }

            for (auto* c : { widget.control.get(), widget.second.get(), widget.third.get() })
                if (c != nullptr)
                    addAndMakeVisible(*c);
            return widget;
        }

        std::vector<Row>                   rows_; // the widgets' callbacks hold references into this: never resized
        std::vector<Widgets>               widgets_;
        std::unique_ptr<juce::FileChooser> chooser_;
    };
}

class PreferencesDialog final : public juce::Component
{
public:
    explicit PreferencesDialog(std::vector<prefs::Page> pages, int initialTab = 0)
    {
        for (auto& page : pages)
        {
            auto  content  = std::make_unique<prefs::PageComponent>(std::move(page.rows));
            auto* viewport = new juce::Viewport();
            viewport->setScrollBarsShown(true, false);
            viewport->setViewedComponent(content.get(), false);
            pages_.push_back(std::move(content));
            tabs_.addTab(page.name, findColour(juce::ResizableWindow::backgroundColourId), viewport, true);
        }
        tabs_.setCurrentTabIndex(juce::jlimit(0, juce::jmax(0, tabs_.getNumTabs() - 1), initialTab));
        tabs_.getTabbedButtonBar().addChangeListener(&tabChanges_);
        tabChanges_.onChange = [this] { refreshCurrent(); };
        addAndMakeVisible(tabs_);
        setSize(640, 520);
    }

    ~PreferencesDialog() override { tabs_.getTabbedButtonBar().removeChangeListener(&tabChanges_); }

    void resized() override
    {
        tabs_.setBounds(getLocalBounds());
        for (int i = 0; i < tabs_.getNumTabs(); ++i)
            if (auto* viewport = dynamic_cast<juce::Viewport*>(tabs_.getTabContentComponent(i)))
            {
                auto* page = pages_[(size_t) i].get();
                page->setSize(juce::jmax(200, tabs_.getWidth() - 4 - viewport->getScrollBarThickness()), page->idealHeight());
            }
    }

    int                   pageCount() const noexcept { return (int) pages_.size(); }
    prefs::PageComponent& pageForTesting(int i) { return *pages_[(size_t) i]; }
    juce::TabbedComponent& tabsForTesting() noexcept { return tabs_; }

private:
    struct TabChanges final : juce::ChangeListener
    {
        std::function<void()> onChange;
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (onChange) onChange(); }
    };

    void refreshCurrent()
    {
        const int index = tabs_.getCurrentTabIndex();
        if (index >= 0 && index < (int) pages_.size())
            pages_[(size_t) index]->refresh();
    }

    // Before tabs_, so it's destroyed after: the tabs' viewports show these
    // without owning them, and let go of them as they're deleted.
    std::vector<std::unique_ptr<prefs::PageComponent>> pages_;
    TabChanges                                         tabChanges_;
    juce::TabbedComponent                              tabs_ { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PreferencesDialog)
};

} // namespace soundsplice
