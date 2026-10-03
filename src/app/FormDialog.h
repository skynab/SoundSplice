#pragma once

#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    The small modal dialogs the menus open: a few fields, an action button and
    Cancel. Click Removal, Auto Duck, Rename Marker and the rest were once each
    forty lines of the same AlertWindow code - fields loaded from the settings,
    read back and clamped, saved, then the modal callback that owns the window
    and checks its owner is still alive. This is that code, once.

    Each field is named by the settings key it is remembered under, so the
    dialog opens with what was used last and saves what is used now. A field
    followed by unsaved() starts at its fallback every time instead.

        FormDialog(*this, settings_, "Repeat", "Puts copies of the time selection straight after it.")
            .integer("repeat.times", "Number of repeats:", 1, 1, 1000)
            .show("Repeat", [this](const FormDialog::Values& v) { repeatTimeSelection(v.integer("repeat.times")); });

    The action runs only if the owner still exists, so it may capture it.
    Fields read back clamped to their ranges, and are saved as they read,
    before the action runs.

    withPreview() gives it a Preview: the action, run without committing (see
    MainComponent::addPreviewStrip), and a hook for when the dialog closes.
*/
class FormDialog
{
public:
    /** What the fields hold as the action runs, each by its key. */
    class Values
    {
    public:
        double       number(const juce::String& key) const  { return get(key); }
        int          integer(const juce::String& key) const { return (int) get(key); }
        int          choice(const juce::String& key) const  { return (int) get(key); }
        bool         toggle(const juce::String& key) const  { return (bool) get(key); }
        juce::String text(const juce::String& key) const    { return get(key).toString(); }

        /** The window itself, for fields added to it directly. */
        juce::AlertWindow& window() const { return *window_; }

    private:
        friend class FormDialog;
        juce::var get(const juce::String& key) const
        {
            const auto found = values_.find(key);
            jassert(found != values_.end()); // not one of this dialog's fields
            return found != values_.end() ? found->second : juce::var();
        }

        juce::AlertWindow*                  window_ = nullptr;
        std::map<juce::String, juce::var>   values_;
    };

    using Action = std::function<void(const Values&)>;

    FormDialog(juce::Component& owner, juce::PropertiesFile& settings, const juce::String& title,
               const juce::String& message = {})
        : state_(std::make_shared<State>(owner, settings))
    {
        state_->window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon, &owner);
    }

    FormDialog(FormDialog&& other) noexcept
        : state_(std::move(other.state_)), shown_(other.shown_)
    {
    }
    FormDialog& operator=(FormDialog&&) = delete;

    ~FormDialog()
    {
        if (state_ != nullptr && ! shown_)
            delete state_->window;
    }

    /** A number typed in, clamped to [@p min, @p max]. */
    FormDialog& number(const juce::String& key, const juce::String& label, double fallback, double min, double max)
    {
        const double initial = state_->settings.getDoubleValue(key, fallback);
        state_->window->addTextEditor(key, juce::String(initial), label);
        return add({ Kind::Number, key, min, max, fallback });
    }

    /** A whole number typed in, clamped to [@p min, @p max]. */
    FormDialog& integer(const juce::String& key, const juce::String& label, int fallback, int min, int max)
    {
        const int initial = state_->settings.getIntValue(key, fallback);
        state_->window->addTextEditor(key, juce::String(initial), label);
        return add({ Kind::Integer, key, (double) min, (double) max, fallback });
    }

    /** One of @p options, read back as its index. */
    FormDialog& choice(const juce::String& key, const juce::String& label, const juce::StringArray& options,
                       int fallback)
    {
        state_->window->addComboBox(key, options, label);
        state_->window->getComboBoxComponent(key)->setSelectedItemIndex(state_->settings.getIntValue(key, fallback));
        return add({ Kind::Choice, key, 0.0, (double) juce::jmax(0, options.size() - 1), fallback });
    }

    /** A tick box, labelled @p label. */
    FormDialog& toggle(const juce::String& key, const juce::String& label, bool fallback)
    {
        auto button = std::make_unique<juce::ToggleButton>(label);
        button->setToggleState(state_->settings.getBoolValue(key, fallback), juce::dontSendNotification);
        button->setSize(320, 24);
        state_->window->addCustomComponent(button.get());
        state_->toggles.push_back(std::move(button));
        return add({ Kind::Toggle, key, 0.0, 1.0, fallback });
    }

    /** Text typed in, trimmed. */
    FormDialog& text(const juce::String& key, const juce::String& label, const juce::String& fallback)
    {
        state_->window->addTextEditor(key, state_->settings.getValue(key, fallback), label);
        return add({ Kind::Text, key, 0.0, 0.0, fallback });
    }

    /** The field just added starts at its fallback every time, and isn't saved. */
    FormDialog& unsaved()
    {
        jassert(! state_->fields.empty());
        auto& field = state_->fields.back();
        field.saved = false;
        switch (field.kind)
        {
            case Kind::Number:  state_->window->getTextEditor(field.key)->setText(juce::String((double) field.fallback)); break;
            case Kind::Integer: state_->window->getTextEditor(field.key)->setText(juce::String((int) field.fallback)); break;
            case Kind::Choice:  state_->window->getComboBoxComponent(field.key)->setSelectedItemIndex((int) field.fallback); break;
            case Kind::Toggle:  state_->toggles.back()->setToggleState((bool) field.fallback, juce::dontSendNotification); break;
            case Kind::Text:    state_->window->getTextEditor(field.key)->setText(field.fallback.toString()); break;
        }
        return *this;
    }

    /** The window, for anything the fields above don't cover. Read it back
        with Values::window(). */
    juce::AlertWindow& window() { return *state_->window; }

    /** Adds a Preview: @p add is handed the window and a function that runs
        the action, before the buttons go in; @p end runs as the dialog
        closes, before the action if it was applied. */
    FormDialog& withPreview(std::function<void(juce::AlertWindow&, std::function<void()> run)> add,
                            std::function<void()> end)
    {
        state_->addPreview = std::move(add);
        state_->endPreview = std::move(end);
        return *this;
    }

    /** Names the button that dismisses the dialog ("Cancel" unless set),
        and gives it something to do as well. */
    FormDialog& cancelButton(const juce::String& text, std::function<void()> onCancel = {})
    {
        state_->cancelText = text;
        state_->onCancel   = std::move(onCancel);
        return *this;
    }

    /** Opens the dialog, with @p applyText (Return) and Cancel (Escape);
        @p action runs when @p applyText is pressed. */
    void show(const juce::String& applyText, Action action)
    {
        jassert(! shown_);
        shown_ = true;

        auto state    = state_;
        state->action = std::move(action);
        if (state->addPreview)
            state->addPreview(*state->window, [state] { if (state->window != nullptr) state->run(*state->window); });

        state->window->addButton(applyText, 1, juce::KeyPress(juce::KeyPress::returnKey));
        state->window->addButton(state->cancelText, 0, juce::KeyPress(juce::KeyPress::escapeKey));

        state->window->enterModalState(true, juce::ModalCallbackFunction::create([state](int result)
        {
            const std::unique_ptr<juce::AlertWindow> owned(std::exchange(state->window, nullptr));
            if (state->owner == nullptr)
                return;
            if (state->endPreview)
                state->endPreview();
            if (result == 1)
                state->run(*owned);
            else if (state->onCancel)
                state->onCancel();
        }));
    }

    /** Runs the action as Apply would, without showing anything: for tests. */
    void applyNow(Action action)
    {
        state_->action = std::move(action);
        state_->run(*state_->window);
    }

private:
    enum class Kind { Number, Integer, Choice, Toggle, Text };

    struct Field
    {
        Kind         kind;
        juce::String key;
        double       min = 0.0, max = 0.0;
        juce::var    fallback;
        bool         saved = true;
    };

    struct State
    {
        State(juce::Component& o, juce::PropertiesFile& s) : owner(&o), settings(s) {}

        /** Reads every field, clamped, saves the ones that are, and runs the
            action with them. */
        void run(juce::AlertWindow& shown)
        {
            if (owner == nullptr || ! action)
                return;

            Values values;
            values.window_ = &shown;
            size_t toggle  = 0;
            for (const auto& field : fields)
            {
                juce::var value;
                switch (field.kind)
                {
                    case Kind::Number:
                        value = juce::jlimit(field.min, field.max, shown.getTextEditorContents(field.key).getDoubleValue());
                        break;
                    case Kind::Integer:
                        value = juce::jlimit((int) field.min, (int) field.max, shown.getTextEditorContents(field.key).getIntValue());
                        break;
                    case Kind::Choice:
                        value = juce::jlimit(0, (int) field.max, shown.getComboBoxComponent(field.key)->getSelectedItemIndex());
                        break;
                    case Kind::Toggle:
                        value = toggles[toggle++]->getToggleState();
                        break;
                    case Kind::Text:
                        value = shown.getTextEditorContents(field.key).trim();
                        break;
                }
                values.values_[field.key] = value;
                if (field.saved)
                    settings.setValue(field.key, value);
            }
            action(values);
        }

        juce::Component::SafePointer<juce::Component>    owner;
        juce::PropertiesFile&                            settings;
        juce::AlertWindow*                               window = nullptr; // owned until it's shown, then by the modal callback
        std::vector<Field>                               fields;
        std::vector<std::unique_ptr<juce::ToggleButton>> toggles; // the window's tick boxes, which it doesn't own
        Action                                           action;
        std::function<void(juce::AlertWindow&, std::function<void()>)> addPreview;
        std::function<void()>                            endPreview;
        juce::String                                     cancelText = "Cancel";
        std::function<void()>                            onCancel;
    };

    FormDialog& add(Field field)
    {
        state_->fields.push_back(std::move(field));
        return *this;
    }

    std::shared_ptr<State> state_;
    bool                   shown_ = false;
};

} // namespace soundsplice
