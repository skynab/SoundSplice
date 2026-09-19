#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Track effect chains, plugins, effect presets, the master effects and the
// mastering rack.

namespace soundsplice
{
/** Shows the selected track's insert effects. Applies to *every* track
    type — an audio track wants a filter as much as an instrument one does. */
void MainComponent::refreshEffectChainForSelected()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        effectChain_.setNoTrackSelected();
        return;
    }

    effectChain_.setChain(history_.current().tracks[(size_t) selectedTrackIndex_].effectChain);
}

// makeEffectNode and toSlotParams used to live here. They moved to
// engine/EffectSlotFactory.h when a *third* copy of the same mapping turned
// up in the bounce tool: the tool's copy had silently fallen behind the
// engine's, so the tool was measuring a different signal path from the one
// the app plays - which is the one thing it exists not to do.

/** Adds a slot to the end of the selected track's chain. Structural, so it
    goes through history_ — and adding a plugin rebuilds the engine chain,
    which is what instantiates it. */
void MainComponent::addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Add effect", [index, kind, &plugin](model::Song& s)
    {
        auto slot   = model::makeEffectSlot(kind);
        slot.plugin = plugin;
        s.tracks[(size_t) index].effectChain.push_back(std::move(slot));
    });

    // Any open editor belongs to a node the rebuild is about to delete.
    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

void MainComponent::removeEffectSlot(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Remove effect", [index, slotIndex](model::Song& s)
    {
        auto& chain = s.tracks[(size_t) index].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) chain.size())
            chain.erase(chain.begin() + slotIndex);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Moves a slot one place up or down. Order is the whole point of a chain, so
    this is a real document edit rather than a view-only sort. */
void MainComponent::moveEffectSlot(int slotIndex, int delta)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Reorder effects", [index, slotIndex, delta](model::Song& s)
    {
        auto&     chain  = s.tracks[(size_t) index].effectChain;
        const int target = slotIndex + delta;
        if (slotIndex < 0 || slotIndex >= (int) chain.size() || target < 0 || target >= (int) chain.size())
            return;
        std::swap(chain[(size_t) slotIndex], chain[(size_t) target]);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Bypass. Not structural — the node stays in the chain — so this is a live
    tweak straight into the document and the engine, with no rebuild and no
    plugin reinstantiation. */
void MainComponent::setEffectSlotBypass(int slotIndex, bool enabled)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const int index = selectedTrackIndex_;
    history_.edit(enabled ? "Enable effect" : "Bypass effect", [index, slotIndex, enabled](model::Song& s)
    {
        s.tracks[(size_t) index].effectChain[(size_t) slotIndex].enabled = enabled;
    });

    const auto& updatedChain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, engine::toSlotParams(updatedChain[(size_t) slotIndex]));
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** A knob turn on a built-in slot: live, non-undoable per notch, same as the
    mixer faders. */
void MainComponent::setEffectSlotParams(const model::EffectSlot& slot, int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    auto& chain = history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    chain[(size_t) slotIndex] = slot;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, engine::toSlotParams(slot));
}

/** Probes for plugins and caches the result, so the next launch doesn't
    re-probe everything. In-process, so a plugin that crashes on probe takes
    the app with it — the dead man's pedal means it's skipped next time (see
    engine::PluginHost, and §20 for what's still owed here). */
void MainComponent::scanForPlugins()
{
    showBusy("Scanning for plugins...");

    const auto pedal = recordingsDirectory().getParentDirectory().getChildFile("plugin-scan.tmp");

    for (const auto& format : engine_.pluginHost().availableFormats())
        engine_.pluginHost().scanFormat(format, pedal);

    settings_.setValue("pluginScanCache", juce::String(engine_.pluginHost().saveScanCache()));
    settings_.saveIfNeeded();

    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    showStatus("Found " + juce::String((int) engine_.pluginHost().knownPlugins().size()) + " plugin(s)");
}

/** Opens a hosted plugin's own editor. */
void MainComponent::openPluginEditor(int slotIndex)
{
    auto* node = engine_.trackPluginNode(selectedTrackIndex_, slotIndex);
    if (node == nullptr || node->instance() == nullptr)
    {
        showError("That plugin isn't loaded on this machine");
        return;
    }

    // One window per plugin instance; re-opening focuses the existing one.
    for (auto* existing : pluginWindows_)
        if (existing->plugin() == node->instance())
        {
            existing->toFront(true);
            return;
        }

    auto* window = pluginWindows_.add(new PluginEditorWindow(node->instance()->getName(), *node->instance()));
    window->onCloseRequested = [this](PluginEditorWindow* w) { pluginWindows_.removeObject(w); };
}

/** Closes every plugin editor. Called before anything that rebuilds a chain,
    because the rebuild deletes the PluginNodes those editors are drawing —
    an editor outliving its processor is a crash, not a glitch. */
void MainComponent::closePluginEditors()
{
    pluginWindows_.clear();
}

/** Pushes the document's mastering rack into the pane and the engine. The
    one place both are refreshed from the model, so undo, load and a preset
    click all land the same way. */
void MainComponent::updateMasteringControls()
{
    const auto& mastering = history_.current().mastering;
    masteringPane_.setSettings(mastering);
    engine_.setMastering(mastering);
}

/** Live tweak from the mastering pane — document in place, then the engine,
    same path as the master EQ sliders. The undo step is bracketed by the
    drag pair below rather than taken per move. */
void MainComponent::setMasteringSettings(const model::MasteringSettings& settings)
{
    history_.mutableCurrent().mastering = settings;
    engine_.setMastering(settings);
}

void MainComponent::beginMasteringDrag()
{
    masteringDragging_ = true;
    masteringDragFrom_ = history_.current().mastering;
}

void MainComponent::endMasteringDrag()
{
    if (! masteringDragging_)
        return;

    masteringDragging_ = false;

    commitStructDrag(history_, "Set mastering", masteringDragFrom_,
                     history_.current().mastering,
                     [](model::Song& s, const model::MasteringSettings& value)
    {
        s.mastering = value;
    });
}

/** One-click mastering starting points — see model::presetForMastering,
    which is what knows the values. One undo step, same shape as every other
    preset application here. */
void MainComponent::applyMasteringPreset(engine::MasteringPreset preset)
{
    const auto settings = model::presetForMastering(preset);
    history_.edit(std::string(engine::masteringPresetName(preset)) + " mastering",
                  [settings](model::Song& s) { s.mastering = settings; });

    updateMasteringControls();
    showStatus("Mastering: " + juce::String(engine::masteringPresetName(preset)));
}

/** Asks for a name and saves @p slot's settings as a user preset. Saving
    under a name already used for that effect replaces it, which is what
    "save" means everywhere else. */
void MainComponent::promptToSaveEffectPreset(const model::EffectSlot& slot)
{
    const auto* descriptor = model::descriptorFor(slot.kind);
    if (descriptor == nullptr)
        return;

    const juce::String effectName(descriptor->name);
    auto* window = new juce::AlertWindow("Save Preset",
                                         "Save these " + effectName + " settings as a preset you can use on any track.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", "My " + effectName, "Name:");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, slot](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const auto name = window->getTextEditorContents("name").trim();
            if (name.isEmpty())
            {
                self->showError("A preset needs a name");
                return;
            }

            const auto* effect = model::descriptorFor(slot.kind);
            if (effect == nullptr)
                return;

            self->userEffectPresets_ = model::withUserPreset(self->userEffectPresets_, effect->id,
                                                             model::capturePreset(slot, *effect, name.toStdString()));
            self->storeUserEffectPresets();
            self->showStatus("Saved preset \"" + name + "\"");
        }));
}

void MainComponent::deleteUserEffectPreset(const std::string& effectId, const std::string& name)
{
    userEffectPresets_ = model::withoutUserPreset(userEffectPresets_, effectId, name);
    storeUserEffectPresets();
    showStatus("Deleted preset \"" + juce::String(name) + "\"");
}

/** Writes the preset library to the app settings and hands it to everything
    that shows it, so a preset saved from the Apply Effects dialog is in the
    Track FX panel's menu too, and the reverse. */
void MainComponent::storeUserEffectPresets()
{
    settings_.setValue("effectPresets", juce::String(model::serializeUserPresets(userEffectPresets_)));
    settings_.saveIfNeeded();

    effectChain_.setUserPresets(userEffectPresets_);
    if (applyEffectsDialog_ != nullptr)
        applyEffectsDialog_->setUserPresets(userEffectPresets_);
}

void MainComponent::updateDelayControls()
{
    const auto& d = history_.current().delay;
    delayButton.setToggleState(d.enabled, juce::dontSendNotification);
    delayTimeSlider.setValue(d.timeMs, juce::dontSendNotification);
    delayFbSlider.setValue(d.feedback * 100.0, juce::dontSendNotification);
    delayMixSlider.setValue(d.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterDelayEnabled(d.enabled);
    engine_.setMasterDelayTimeMs(d.timeMs);
    engine_.setMasterDelayFeedback(d.feedback);
    engine_.setMasterDelayMix(d.mix);
}

void MainComponent::updateFilterControls()
{
    const auto& f = history_.current().filter;
    filterButton.setToggleState(f.enabled, juce::dontSendNotification);
    filterModeBox_.setSelectedId(f.mode + 1, juce::dontSendNotification);
    filterCutoffSlider.setValue(f.cutoff, juce::dontSendNotification);
    filterResoSlider.setValue(f.resonance, juce::dontSendNotification);

    engine_.setMasterFilterEnabled(f.enabled);
    engine_.setMasterFilterMode(f.mode);
    engine_.setMasterFilterCutoff(f.cutoff);
    engine_.setMasterFilterResonance(f.resonance);
}

void MainComponent::updateReverbControls()
{
    const auto& rv = history_.current().reverb;
    reverbButton.setToggleState(rv.enabled, juce::dontSendNotification);
    reverbRoomSlider.setValue(rv.roomSize * 100.0, juce::dontSendNotification);
    reverbDampSlider.setValue(rv.damping * 100.0, juce::dontSendNotification);
    reverbMixSlider.setValue(rv.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterReverbEnabled(rv.enabled);
    engine_.setMasterReverbRoomSize(rv.roomSize);
    engine_.setMasterReverbDamping(rv.damping);
    engine_.setMasterReverbMix(rv.mix);
}

void MainComponent::updateEqControls()
{
    const auto& eq = history_.current().eq;
    eqButton.setToggleState(eq.enabled, juce::dontSendNotification);
    eqBassSlider.setValue(eq.bassDb, juce::dontSendNotification);
    eqMidSlider.setValue(eq.midDb, juce::dontSendNotification);
    eqTrebleSlider.setValue(eq.trebleDb, juce::dontSendNotification);

    engine_.setMasterEqEnabled(eq.enabled);
    engine_.setMasterEqBassDb(eq.bassDb);
    engine_.setMasterEqMidDb(eq.midDb);
    engine_.setMasterEqTrebleDb(eq.trebleDb);

    eqCurveView_.setSettings(eq);
}

/** Remembers an effect slot's parameters before a drag on one of its controls
    started — see EffectChainPanel::onSlotParamsDragStart. */
void MainComponent::beginEffectSlotParamsDrag(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    effectSlotDragging_  = true;
    effectSlotDragTrack_ = selectedTrackIndex_;
    effectSlotDragIndex_ = slotIndex;
    effectSlotDragFrom_  = chain[(size_t) slotIndex];
}

/** Commits a whole effect-slot-parameters drag as one undo step, the
    commitStructDrag equivalent of endFaderDrag above — a slot's parameters
    are a struct of several fields changed together, not one number, so
    there's no meaningful tolerance to check against: any real change
    commits, equality is the whole test. */
void MainComponent::endEffectSlotParamsDrag(int slotIndex)
{
    if (! effectSlotDragging_ || effectSlotDragTrack_ != selectedTrackIndex_ || effectSlotDragIndex_ != slotIndex)
        return;

    effectSlotDragging_ = false;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const auto landedOn   = chain[(size_t) slotIndex];
    const int  trackIndex = selectedTrackIndex_;

    commitStructDrag(history_, "Set effect parameters", effectSlotDragFrom_, landedOn,
                     [trackIndex, slotIndex](model::Song& s, const model::EffectSlot& value)
    {
        auto& c = s.tracks[(size_t) trackIndex].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) c.size())
            c[(size_t) slotIndex] = value;
    });
}

/** The convolution reverb's impulse response: a file chosen here, or back
    to the built-in hall. Kept as the file's path in the slot, like an audio
    clip's, and read by the effect when it changes. */
void MainComponent::chooseImpulseResponse(int slotIndex, bool browse)
{
    if (! browse)
    {
        setImpulseResponse(slotIndex, {});
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Load an impulse response", juce::File{},
                                                   audiofiles::wildcards());
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser_->launchAsync(flags, [this, slotIndex](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        std::vector<std::vector<float>> channels;
        double                          rate = 0.0;
        if (! engine::loadImpulseFile(file.getFullPathName().toStdString(), channels, rate))
        {
            showError("Could not read " + file.getFileName() + " as audio");
            return;
        }
        setImpulseResponse(slotIndex, file);
        showStatus("Reverb response: " + file.getFileName() + " ("
                   + juce::String((double) channels[0].size() / rate, 2) + "s, "
                   + (channels.size() > 1 ? "stereo" : "mono") + ")");
    });
}

void MainComponent::setImpulseResponse(int slotIndex, const juce::File& file)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int         index = selectedTrackIndex_;
    const std::string path  = file == juce::File{} ? std::string() : file.getFullPathName().toStdString();
    history_.edit(path.empty() ? "Use built-in reverb hall" : "Load impulse response", [index, slotIndex, path](model::Song& s)
    {
        auto& chain = s.tracks[(size_t) index].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) chain.size())
            chain[(size_t) slotIndex].convolution.irFile = path;
    });

    syncEngineTracks();
    refreshEffectChainForSelected();
}

} // namespace soundsplice
