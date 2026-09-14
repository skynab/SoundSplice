#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LayoutHelpers.h"
#include "engine/PluginHost.h"
#include "model/EffectParams.h"
#include "model/EffectPresets.h"
#include "model/Effects.h"

namespace soundsplice
{
/**
    The selected track's effect chain: an ordered list of slots, each a
    built-in or a hosted plugin, with add, remove, reorder and bypass — and
    the selected slot's parameters below.

    The parameter controls are generated from model::builtInEffects(), one
    labelled row per parameter, rather than written out per effect. Before,
    every parameter here was a hand-declared member with its own range, show
    and hide case, read and write, so a new effect meant repeating all of that
    and a parameter missed in any one place was a control that did nothing
    (which is how the drive pedal once shipped invisible). An effect added to
    the descriptor table now gets its controls here, and in the Apply Effects
    dialog, with nothing else to wire.

    Owns no document state: it draws from a snapshot and reports intent through
    the callbacks, like ArrangementView and SessionView.
*/
class EffectChainPanel final : public juce::Component
{
public:
    std::function<void(int slotIndex)>                              onSlotSelected;
    std::function<void(int slotIndex, bool enabled)>                onSlotBypassToggled;
    std::function<void(int slotIndex)>                              onSlotRemoved;
    std::function<void(int slotIndex, int delta)>                   onSlotMoved;   // -1 up, +1 down
    std::function<void(model::EffectKind kind)>                     onBuiltInAdded;
    std::function<void(const engine::PluginEntry&)>                 onPluginAdded;
    std::function<void(int slotIndex)>                              onPluginEditorRequested;
    std::function<void()>                                           onScanRequested;
    std::function<void(const model::EffectSlot& slot, int slotIndex)> onSlotParamsChanged;

    /** Brackets a change to the selected slot's parameters, so the owner can
        commit the whole thing as one undo step (see MainComponent's
        beginEffectSlotParamsDrag/endEffectSlotParamsDrag) rather than one
        step per notch — the same problem the mixer faders solve, but for a
        whole struct of fields rather than one number. A slider spans a real
        drag; a toggle or dropdown fires both back to back, since a click has
        no "during" to span. */
    std::function<void(int slotIndex)> onSlotParamsDragStart;
    std::function<void(int slotIndex)> onSlotParamsDragEnd;

    /** Fired by "Save Current Settings as Preset...", with the selected
        slot. The owner asks for a name and stores the preset: it owns the
        preset library, and this panel only shows it (see setUserPresets). */
    std::function<void(const model::EffectSlot& slot, int slotIndex)> onPresetSaveRequested;

    /** Fired when one of the user's presets is picked from Delete Preset. */
    std::function<void(const std::string& effectId, const std::string& name)> onUserPresetDeleted;

    EffectChainPanel()
    {
        placeholder_.setText("Select a track to edit its effects", juce::dontSendNotification);
        placeholder_.setJustificationType(juce::Justification::centred);
        placeholder_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholder_);

        addButton_.setButtonText("+ Add");
        addButton_.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(addButton_);

        removeButton_.setButtonText("Remove");
        removeButton_.onClick = [this]
        {
            if (onSlotRemoved && isValidSlot(selected_))
                onSlotRemoved(selected_);
        };
        addAndMakeVisible(removeButton_);

        upButton_.setButtonText("Up");
        upButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, -1); };
        addAndMakeVisible(upButton_);

        downButton_.setButtonText("Down");
        downButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, +1); };
        addAndMakeVisible(downButton_);

        editorButton_.setButtonText("Open Plugin Editor");
        editorButton_.onClick = [this]
        {
            if (onPluginEditorRequested && isValidSlot(selected_))
                onPluginEditorRequested(selected_);
        };
        addChildComponent(editorButton_);

        presetsButton_.setButtonText("Presets");
        presetsButton_.setTooltip("Starting points for this effect, and settings you've saved");
        presetsButton_.onClick = [this] { showPresetsMenu(); };
        addChildComponent(presetsButton_);

        setContentVisible(false);
    }

    /** The scanned plugins offered by the Add menu. */
    void setAvailablePlugins(std::vector<engine::PluginEntry> plugins) { plugins_ = std::move(plugins); }

    /** The user's saved presets, for every effect; the Presets menu shows the
        ones for the selected slot's effect. */
    void setUserPresets(std::vector<model::UserEffectPreset> presets) { userPresets_ = std::move(presets); }

    void setChain(const std::vector<model::EffectSlot>& chain)
    {
        chain_ = chain;
        selected_ = chain_.empty() ? -1 : juce::jlimit(0, (int) chain_.size() - 1, juce::jmax(0, selected_));
        refreshParamControls();
        setContentVisible(true);
    }

    void setNoTrackSelected() { setContentVisible(false); }

    /** Selects a slot, so a test can walk every effect kind's controls. The
        app selects by clicking the list, which a headless test can't do. */
    void selectSlotForTesting(int index)
    {
        selected_ = index;
        refreshParamControls();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int slot = slotAtY(e.position.y);
        if (slot < 0)
            return;

        // The left-hand strip of a row is its bypass button; the rest selects.
        if (e.position.x < (float) kBypassWidth)
        {
            if (onSlotBypassToggled)
                onSlotBypassToggled(slot, ! chain_[(size_t) slot].enabled);
            return;
        }

        selected_ = slot;
        refreshParamControls();
        repaint();
        resized();
        if (onSlotSelected)
            onSlotSelected(slot);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
        if (! contentVisible_)
            return;

        g.setFont(juce::FontOptions(12.0f));
        for (int i = 0; i < (int) chain_.size(); ++i)
        {
            const auto  row  = rowBounds(i);
            const auto& slot = chain_[(size_t) i];

            g.setColour(i == selected_ ? juce::Colours::white.withAlpha(0.10f)
                                       : juce::Colours::white.withAlpha(0.04f));
            g.fillRect(row);

            if (i == selected_)
            {
                g.setColour(juce::Colours::orange.withAlpha(0.7f));
                g.drawRect(row, 1);
            }

            // Bypass indicator: filled when active, hollow when bypassed —
            // the same meaning for a plugin as for a built-in.
            const auto dot = juce::Rectangle<int>(row.getX() + 8, row.getCentreY() - 5, 10, 10).toFloat();
            g.setColour(slot.enabled ? juce::Colours::limegreen : juce::Colours::white.withAlpha(0.25f));
            slot.enabled ? g.fillEllipse(dot) : g.drawEllipse(dot, 1.2f);

            g.setColour(juce::Colours::white.withAlpha(slot.enabled ? 0.9f : 0.45f));
            g.drawText(slotLabel(slot), row.getX() + kBypassWidth, row.getY(),
                       row.getWidth() - kBypassWidth - 6, row.getHeight(),
                       juce::Justification::centredLeft);
        }

        if (chain_.empty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText("No effects - use + Add", listArea(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        placeholder_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds().reduced(6);

        // Four fixed-width buttons in a row: in a narrow pane the later ones
        // run past the right edge and used to end up zero wide. Dropping the
        // ones that don't fit keeps the rest usable, and they return when the
        // pane is widened. Add is first because it's the one that has to work
        // for the panel to be worth anything.
        auto toolbar = area.removeFromTop(kToolbarHeight);
        setBoundsOrHide(addButton_, toolbar.removeFromLeft(64).reduced(2));
        setBoundsOrHide(removeButton_, toolbar.removeFromLeft(70).reduced(2));
        setBoundsOrHide(upButton_, toolbar.removeFromLeft(44).reduced(2));
        setBoundsOrHide(downButton_, toolbar.removeFromLeft(56).reduced(2));

        area.removeFromTop((int) chain_.size() * kRowHeight + 6);

        // Whatever the selected slot needs: a plugin gets an editor button, a
        // built-in gets its own parameters.
        if (! isValidSlot(selected_))
            return;

        if (chain_[(size_t) selected_].kind == model::EffectKind::Plugin)
        {
            setBoundsOrHide(editorButton_, area.removeFromTop(kRowHeight).reduced(2));
            return;
        }

        auto presetRow = area.removeFromTop(kRowHeight).reduced(2);
        setBoundsOrHide(presetsButton_, presetRow.removeFromLeft(juce::jmin(kPresetsButtonWidth, presetRow.getWidth())));

        // A kind with many rows in a short pane runs the last of them off the
        // bottom. setBoundsOrHide hides those rather than leaving them
        // zero-high and clickable against nothing.
        for (auto& row : rows_)
        {
            auto line = area.removeFromTop(kRowHeight).reduced(2);

            if (row.label != nullptr)
                setBoundsOrHide(*row.label, line.removeFromLeft(juce::jmin(kLabelWidth, line.getWidth() / 3)));

            setBoundsOrHide(*row.control(), line);
        }
    }

private:
    static constexpr int kRowHeight     = 26;
    static constexpr int kToolbarHeight = 26;
    static constexpr int kBypassWidth   = 26;
    static constexpr int kLabelWidth    = 90;
    static constexpr int kPresetsButtonWidth = 110;

    /** One parameter's row: a label and the control its descriptor asks for.
        Exactly one of slider, toggle and choice is set. A toggle carries its
        name on the button itself, so it has no label. */
    struct ParamRow
    {
        const model::EffectParam*           param = nullptr;
        std::unique_ptr<juce::Label>        label;
        std::unique_ptr<juce::Slider>       slider;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::ComboBox>     choice;

        juce::Component* control() const
        {
            if (slider != nullptr) return slider.get();
            if (toggle != nullptr) return toggle.get();
            return choice.get();
        }
    };

    juce::Rectangle<int> listArea() const
    {
        auto area = getLocalBounds().reduced(6);
        area.removeFromTop(kToolbarHeight);
        return area;
    }

    juce::Rectangle<int> rowBounds(int index) const
    {
        auto area = listArea();
        return { area.getX(), area.getY() + index * kRowHeight, area.getWidth(), kRowHeight };
    }

    int slotAtY(float y) const
    {
        const auto area = listArea();
        const int  idx  = (int) ((y - (float) area.getY()) / (float) kRowHeight);
        return (y >= (float) area.getY() && idx >= 0 && idx < (int) chain_.size()) ? idx : -1;
    }

    bool isValidSlot(int index) const { return index >= 0 && index < (int) chain_.size(); }

    static juce::String slotLabel(const model::EffectSlot& slot)
    {
        if (const auto* descriptor = model::descriptorFor(slot.kind))
            return descriptor->name;

        // A plugin the machine no longer has still names itself, which is
        // the whole reason the document stores the name.
        if (slot.kind == model::EffectKind::Plugin)
            return slot.plugin.name.empty() ? juce::String("(missing plugin)")
                                            : juce::String(slot.plugin.name);
        return {};
    }

    void showAddMenu()
    {
        const auto& effects = model::builtInEffects();

        // Built-ins first, grouped as their descriptors say. The groups are
        // presentation only: every effect runs in the same chain, in whatever
        // order it is put, on whatever track type.
        juce::PopupMenu menu;
        std::vector<std::pair<juce::String, juce::PopupMenu>> groups;

        for (int i = 0; i < (int) effects.size(); ++i)
        {
            const auto& effect = effects[(size_t) i];
            const juce::String group(effect.group);

            if (group.isEmpty())
            {
                menu.addItem(kFirstBuiltInId + i, effect.name);
                continue;
            }

            auto existing = std::find_if(groups.begin(), groups.end(),
                                         [&group](const auto& g) { return g.first == group; });
            if (existing == groups.end())
            {
                groups.emplace_back(group, juce::PopupMenu());
                existing = groups.end() - 1;
            }
            existing->second.addItem(kFirstBuiltInId + i, effect.name);
        }

        for (auto& [name, submenu] : groups)
            menu.addSubMenu(name, submenu);
        menu.addSeparator();

        if (plugins_.empty())
        {
            menu.addItem(kScanId, "Scan for plugins...");
        }
        else
        {
            juce::PopupMenu pluginMenu;
            for (int i = 0; i < (int) plugins_.size(); ++i)
                pluginMenu.addItem(kFirstPluginId + i,
                                   plugins_[(size_t) i].name + "  (" + plugins_[(size_t) i].format + ")");
            pluginMenu.addSeparator();
            pluginMenu.addItem(kScanId, "Rescan...");
            menu.addSubMenu("Plugins", pluginMenu);
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_),
                           [this](int result)
        {
            const auto& builtIns = model::builtInEffects();

            if (result == kScanId)
            {
                if (onScanRequested)
                    onScanRequested();
            }
            else if (result >= kFirstPluginId)
            {
                const size_t index = (size_t) (result - kFirstPluginId);
                if (index < plugins_.size() && onPluginAdded)
                    onPluginAdded(plugins_[index]);
            }
            else if (result >= kFirstBuiltInId)
            {
                const size_t index = (size_t) (result - kFirstBuiltInId);
                if (index < builtIns.size() && onBuiltInAdded)
                    onBuiltInAdded(builtIns[index].kind);
            }
        });
    }

    static constexpr int kScanId         = 1;
    static constexpr int kFirstBuiltInId = 100;
    static constexpr int kFirstPluginId  = 1000;

    static constexpr int kSavePresetId         = 1;
    static constexpr int kFirstFactoryPresetId = 100;
    static constexpr int kFirstUserPresetId    = 1000;
    static constexpr int kFirstDeletePresetId  = 5000;

    /** Factory presets for the selected slot's effect, the user's own, and
        saving and deleting them. */
    void showPresetsMenu()
    {
        if (! isValidSlot(selected_))
            return;

        const auto* descriptor = model::descriptorFor(chain_[(size_t) selected_].kind);
        if (descriptor == nullptr)
            return;

        const auto& factory = model::factoryPresets(descriptor->kind);

        // Copied rather than pointed into: the library can change (a save
        // from elsewhere) while the menu is open, and indices into a list
        // that has since moved would pick the wrong preset.
        std::vector<model::UserEffectPreset> mine;
        for (const auto& user : userPresets_)
            if (user.effectId == descriptor->id)
                mine.push_back(user);
        std::sort(mine.begin(), mine.end(),
                  [](const auto& a, const auto& b) { return a.preset.name < b.preset.name; });

        juce::PopupMenu menu;
        menu.addSectionHeader("Factory");
        for (int i = 0; i < (int) factory.size(); ++i)
            menu.addItem(kFirstFactoryPresetId + i, factory[(size_t) i].name);

        if (! mine.empty())
        {
            menu.addSectionHeader("Yours");
            for (int i = 0; i < (int) mine.size(); ++i)
                menu.addItem(kFirstUserPresetId + i, mine[(size_t) i].preset.name);
        }

        menu.addSeparator();
        menu.addItem(kSavePresetId, "Save Current Settings as Preset...");

        if (! mine.empty())
        {
            juce::PopupMenu deleteMenu;
            for (int i = 0; i < (int) mine.size(); ++i)
                deleteMenu.addItem(kFirstDeletePresetId + i, mine[(size_t) i].preset.name);
            menu.addSubMenu("Delete Preset", deleteMenu);
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetsButton_),
            [safe = juce::Component::SafePointer<EffectChainPanel>(this), kind = descriptor->kind,
             mine = std::move(mine)](int result)
            {
                // The panel may be gone (its dialog closed), or showing a
                // different effect, by the time the menu is dismissed.
                if (safe == nullptr || result == 0 || ! safe->isValidSlot(safe->selected_)
                    || safe->chain_[(size_t) safe->selected_].kind != kind)
                    return;

                const auto& presets = model::factoryPresets(kind);

                if (result == kSavePresetId)
                {
                    if (safe->onPresetSaveRequested)
                        safe->onPresetSaveRequested(safe->chain_[(size_t) safe->selected_], safe->selected_);
                }
                else if (result >= kFirstDeletePresetId)
                {
                    const size_t index = (size_t) (result - kFirstDeletePresetId);
                    if (index < mine.size() && safe->onUserPresetDeleted)
                        safe->onUserPresetDeleted(mine[index].effectId, mine[index].preset.name);
                }
                else if (result >= kFirstUserPresetId)
                {
                    const size_t index = (size_t) (result - kFirstUserPresetId);
                    if (index < mine.size())
                        safe->applyPresetToSelected(mine[index].preset);
                }
                else if (result >= kFirstFactoryPresetId)
                {
                    const size_t index = (size_t) (result - kFirstFactoryPresetId);
                    if (index < presets.size())
                        safe->applyPresetToSelected(presets[index]);
                }
            });
    }

    /** Applies @p preset to the selected slot as one undo step, through the
        same start/change/end bracket a slider drag reports. */
    void applyPresetToSelected(const model::EffectPreset& preset)
    {
        if (! isValidSlot(selected_))
            return;

        auto slot = chain_[(size_t) selected_];
        if (! model::applyPreset(slot, preset))
            return;

        if (onSlotParamsDragStart) onSlotParamsDragStart(selected_);
        chain_[(size_t) selected_] = slot;
        if (onSlotParamsChanged) onSlotParamsChanged(slot, selected_);
        if (onSlotParamsDragEnd) onSlotParamsDragEnd(selected_);

        refreshParamControls();
    }

    /** Every parameter control currently built, plus the plugin editor
        button: what setContentVisible hides. */
    std::vector<juce::Component*> paramControls()
    {
        std::vector<juce::Component*> controls { &editorButton_, &presetsButton_ };
        for (auto& row : rows_)
        {
            if (row.label != nullptr)
                controls.push_back(row.label.get());
            controls.push_back(row.control());
        }
        return controls;
    }

    /** For a discrete control (toggle, dropdown) rather than a slider: a
        click has no "during" to bracket, so both ends of the drag report
        fire back to back around the one edit it makes. */
    void reportInstantEdit()
    {
        if (onSlotParamsDragStart) onSlotParamsDragStart(selected_);
        pushParams();
        if (onSlotParamsDragEnd) onSlotParamsDragEnd(selected_);
    }

    /** Builds one row per parameter of @p descriptor, replacing the rows of
        whatever was shown before.

        Each control is configured (range, items) before its callback is set,
        so building a row can never report an edit nobody made. */
    void buildRows(const model::EffectDescriptor& descriptor)
    {
        rows_.clear();
        rowsFor_ = &descriptor;

        for (const auto& param : descriptor.params)
        {
            ParamRow row;
            row.param = &param;
            const juce::String tooltip(param.tooltip);

            switch (param.control)
            {
                case model::ParamControl::Toggle:
                {
                    row.toggle = std::make_unique<juce::ToggleButton>(param.name);
                    row.toggle->setTooltip(tooltip);
                    row.toggle->onClick = [this] { reportInstantEdit(); };
                    addAndMakeVisible(*row.toggle);
                    break;
                }

                case model::ParamControl::Choice:
                {
                    row.choice = std::make_unique<juce::ComboBox>(param.name);
                    for (int i = 0; i < (int) param.choices.size(); ++i)
                        row.choice->addItem(param.choices[(size_t) i], i + 1);
                    row.choice->setTooltip(tooltip);
                    row.choice->onChange = [this] { reportInstantEdit(); };
                    addAndMakeVisible(*row.choice);
                    break;
                }

                case model::ParamControl::Slider:
                {
                    row.slider = std::make_unique<juce::Slider>(param.name);
                    auto&        slider = *row.slider;
                    const double scale  = param.displayScale;
                    const double lo     = param.min * scale;
                    const double hi     = param.max * scale;

                    slider.setSliderStyle(juce::Slider::LinearHorizontal);
                    slider.setRange(lo, hi, param.step * scale);
                    if (param.skewMidpoint > lo && param.skewMidpoint < hi)
                        slider.setSkewFactorFromMidPoint(param.skewMidpoint);
                    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 18);
                    slider.setTextValueSuffix(param.unit);
                    slider.setTooltip(tooltip);

                    slider.onValueChange = [this] { pushParams(); };
                    slider.onDragStart   = [this] { if (onSlotParamsDragStart) onSlotParamsDragStart(selected_); };
                    slider.onDragEnd     = [this] { if (onSlotParamsDragEnd)   onSlotParamsDragEnd(selected_); };
                    addAndMakeVisible(slider);
                    break;
                }
            }

            if (param.control != model::ParamControl::Toggle)
            {
                row.label = std::make_unique<juce::Label>(juce::String(), param.name);
                row.label->setFont(juce::FontOptions(12.0f));
                row.label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
                row.label->setTooltip(tooltip);
                row.label->setInterceptsMouseClicks(false, false);
                addAndMakeVisible(*row.label);
            }

            rows_.push_back(std::move(row));
        }
    }

    /** Mirrors the selected slot into the parameter controls, building them
        first if the slot is a different effect from the one shown. Rows are
        rebuilt only on that change, never on a plain refresh, so a refresh
        arriving mid-drag can't delete the slider being dragged. */
    void refreshParamControls()
    {
        editorButton_.setVisible(false);
        presetsButton_.setVisible(false);

        const auto* descriptor = isValidSlot(selected_) ? model::descriptorFor(chain_[(size_t) selected_].kind)
                                                        : nullptr;
        if (descriptor == nullptr)
        {
            rows_.clear();
            rowsFor_ = nullptr;

            if (isValidSlot(selected_) && chain_[(size_t) selected_].kind == model::EffectKind::Plugin)
                editorButton_.setVisible(true);

            resized();
            return;
        }

        if (descriptor != rowsFor_)
            buildRows(*descriptor);

        presetsButton_.setVisible(true);

        const auto& slot = chain_[(size_t) selected_];
        updating_ = true;

        for (auto& row : rows_)
        {
            const double value = model::paramValue(slot, *row.param);

            if (row.slider != nullptr)
                row.slider->setValue(value * row.param->displayScale, juce::dontSendNotification);
            else if (row.toggle != nullptr)
                row.toggle->setToggleState(value >= 0.5, juce::dontSendNotification);
            else if (row.choice != nullptr)
                row.choice->setSelectedId((int) std::llround(value - row.param->min) + 1,
                                          juce::dontSendNotification);

            if (row.label != nullptr)
                row.label->setVisible(true);
            row.control()->setVisible(true);
        }

        updating_ = false;
        resized();
    }

    /** Reads the controls back into the selected slot and reports it. Guarded
        against the setValue calls in refreshParamControls, which would
        otherwise echo straight back as a user edit. */
    void pushParams()
    {
        if (updating_ || ! isValidSlot(selected_) || ! onSlotParamsChanged)
            return;

        auto slot = chain_[(size_t) selected_];

        // A plugin's parameters live in its own editor; and rows built for a
        // different effect must never be written into this one.
        if (model::descriptorFor(slot.kind) != rowsFor_ || rowsFor_ == nullptr)
            return;

        for (const auto& row : rows_)
        {
            double value = 0.0;
            if (row.slider != nullptr)
                value = row.slider->getValue() / row.param->displayScale;
            else if (row.toggle != nullptr)
                value = row.toggle->getToggleState() ? 1.0 : 0.0;
            else if (row.choice != nullptr)
                value = row.param->min + juce::jmax(0, row.choice->getSelectedId() - 1);

            model::setParamValue(slot, *row.param, value);
        }

        chain_[(size_t) selected_] = slot;
        onSlotParamsChanged(slot, selected_);
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholder_.setVisible(! visible);

        juce::Component* toolbar[] = { &addButton_, &removeButton_, &upButton_, &downButton_ };
        for (auto* c : toolbar)
            c->setVisible(visible);

        if (! visible)
        {
            for (auto* c : paramControls())
                c->setVisible(false);
        }
        resized();
        repaint();
    }

    std::vector<model::EffectSlot>   chain_;
    std::vector<engine::PluginEntry> plugins_;
    int                              selected_       = 0;
    bool                             contentVisible_ = false;
    bool                             updating_       = false;

    juce::Label      placeholder_;
    juce::TextButton addButton_, removeButton_, upButton_, downButton_, editorButton_, presetsButton_;

    std::vector<model::UserEffectPreset> userPresets_;

    // Declared after the fixed controls, so the rows are destroyed first,
    // while this component is still a whole parent to remove them from.
    std::vector<ParamRow>           rows_;
    const model::EffectDescriptor* rowsFor_ = nullptr; // the effect rows_ were built for

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectChainPanel)
};

} // namespace soundsplice
