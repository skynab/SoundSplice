#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Track effect chains, plugins, effect presets, the master effects and the
// mastering rack.

namespace soundsplice
{
/** The chain the effects panel is editing: the selected track's, or, with
    the panel switched to Clip, the selected clip's own. Clip -1 when that's
    the track's; track -1 when there's nothing to edit. */
MainComponent::EffectChainRef MainComponent::editedChainRef() const
{
    if (effectChain_.masterScope())
        return EffectChainRef::masterChain();

    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return {};
    if (! effectChain_.clipScope())
        return { selectedTrackIndex_, -1 };

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return {};
    return { selectedTrackIndex_, selectedClipIndex_ };
}

std::vector<model::EffectSlot>* MainComponent::chainAt(model::Song& song, const EffectChainRef& ref)
{
    if (ref.master)
        return &song.masterEffects;
    if (ref.track < 0 || ref.track >= (int) song.tracks.size())
        return nullptr;
    auto& track = song.tracks[(size_t) ref.track];
    if (! ref.isClip())
        return &track.effectChain;
    return ref.clip < (int) track.clips.size() ? &track.clips[(size_t) ref.clip].effects : nullptr;
}

const std::vector<model::EffectSlot>* MainComponent::editedChain() const
{
    return chainAt(const_cast<model::Song&>(history_.current()), editedChainRef());
}

/** A slot's settings, to the engine, without rebuilding anything: the
    track's live chain, or the chain the engine keeps for the clip. A track's
    automated parameters are left to their lanes. */
void MainComponent::pushEffectSlotToEngine(const EffectChainRef& ref, int slotIndex, const model::EffectSlot& slot)
{
    if (ref.master)
    {
        engine_.setMasterEffectSlotParams(slotIndex, model::effectParamValues(slot));
        return;
    }
    if (ref.isClip())
    {
        const auto& clips = history_.current().tracks[(size_t) ref.track].clips;
        if (ref.clip < (int) clips.size())
            engine_.setClipEffectParams(clips[(size_t) ref.clip].id, slotIndex, model::effectParamValues(slot));
        return;
    }
    engine_.setTrackEffectSlotParams(ref.track, slotIndex, model::effectParamValues(slot, true));
}

/** Shows the selected track's insert effects, its selected clip's own, or
    the master's. Applies to buses as much as to audio tracks. */
void MainComponent::refreshEffectChainForSelected()
{
    if (effectChain_.masterScope())
    {
        effectChain_.setChain(history_.current().masterEffects);
        return;
    }
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        effectChain_.setNoTrackSelected();
        return;
    }

    if (const auto* chain = editedChain())
        effectChain_.setChain(*chain);
    else
        effectChain_.setNoClipSelected();
}

// Turning a slot into its configured node used to live here. It moved to
// engine/EffectSlotFactory.h when a *third* copy of the same mapping turned
// up in the bounce tool: the tool's copy had silently fallen behind the
// engine's, so the tool was measuring a different signal path from the one
// the app plays - which is the one thing it exists not to do.

/** Adds a slot to the end of the selected track's chain. Structural, so it
    goes through history_ — and adding a plugin rebuilds the engine chain,
    which is what instantiates it. */
void MainComponent::addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin)
{
    const auto ref = editedChainRef();
    if (! ref.isValid())
        return;

    history_.edit(ref.master ? "Add master effect" : ref.isClip() ? "Add clip effect" : "Add effect", [ref, kind, &plugin](model::Song& s)
    {
        auto slot   = model::makeEffectSlot(kind);
        slot.plugin = plugin;
        if (auto* chain = chainAt(s, ref))
            chain->push_back(std::move(slot));
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
    const auto ref = editedChainRef();
    if (! ref.isValid())
        return;

    history_.edit("Remove effect", [ref, slotIndex](model::Song& s)
    {
        auto* chain = chainAt(s, ref);
        if (chain != nullptr && slotIndex >= 0 && slotIndex < (int) chain->size())
            chain->erase(chain->begin() + slotIndex);
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
    const auto ref = editedChainRef();
    if (! ref.isValid())
        return;

    history_.edit("Reorder effects", [ref, slotIndex, delta](model::Song& s)
    {
        auto*     chain  = chainAt(s, ref);
        const int target = slotIndex + delta;
        if (chain == nullptr || slotIndex < 0 || slotIndex >= (int) chain->size() || target < 0
            || target >= (int) chain->size())
            return;
        std::swap((*chain)[(size_t) slotIndex], (*chain)[(size_t) target]);
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
    const auto  ref   = editedChainRef();
    const auto* chain = editedChain();
    if (chain == nullptr || slotIndex < 0 || slotIndex >= (int) chain->size())
        return;

    history_.edit(enabled ? "Enable effect" : "Bypass effect", [ref, slotIndex, enabled](model::Song& s)
    {
        if (auto* c = chainAt(s, ref); c != nullptr && slotIndex < (int) c->size())
            (*c)[(size_t) slotIndex].enabled = enabled;
    });

    pushEffectSlotToEngine(ref, slotIndex, (*editedChain())[(size_t) slotIndex]);
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** A knob turn on a built-in slot: live, non-undoable per notch, same as the
    mixer faders. */
void MainComponent::setEffectSlotParams(const model::EffectSlot& slot, int slotIndex)
{
    const auto ref = editedChainRef();
    auto*      live = chainAt(history_.mutableCurrent(), ref);
    if (live == nullptr || slotIndex < 0 || slotIndex >= (int) live->size())
        return;

    auto& chain = *live;
    if (ref.isClip() || ref.master)
    {
        // A clip's or the master's effects aren't automated: the settings
        // are all there is.
        chain[(size_t) slotIndex] = slot;
        pushEffectSlotToEngine(ref, slotIndex, slot);
        return;
    }

    // Which parameters this move changed, for recording automation: the panel
    // hands over the whole slot, not the one control that moved.
    std::vector<const model::EffectParam*> moved;
    if (const auto* effect = model::descriptorFor(slot.kind); effect != nullptr && chain[(size_t) slotIndex].kind == slot.kind)
        for (const auto& param : effect->params)
            if (model::paramValue(chain[(size_t) slotIndex], param) != model::paramValue(slot, param))
                moved.push_back(&param);

    // The lanes stay as the document has them - the panel's copy of the slot
    // may be older than a lane being written into it.
    auto updated       = slot;
    updated.automation = chain[(size_t) slotIndex].automation;
    chain[(size_t) slotIndex] = updated;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, model::effectParamValues(updated, true));

    for (const auto* param : moved)
    {
        const auto key   = LaneKey::effect(selectedTrackIndex_, slotIndex, slot.kind, param->id);
        const auto value = (float) model::paramValue(slot, *param);
        automationControlMoved(key, value, effectSlotDrag_.isActive());

        // A parameter with a lane isn't in the static values above, so one
        // being written is set on its own - otherwise the knob would do
        // nothing audible until the pass ended.
        if (isWritingAutomation(key))
            engine_.setTrackEffectParam(selectedTrackIndex_, slotIndex, slot.kind, param->id, value);
    }
}

/** Probes for plugins and caches the result, so the next launch doesn't
    re-probe everything. Each plugin is probed in a copy of the app (see
    app/PluginProbe.h), so one that crashes or hangs is blocklisted rather
    than taking the app with it. */
void MainComponent::scanForPlugins()
{
    showBusy("Scanning for plugins...");

    const auto pedal = recordingsDirectory().getParentDirectory().getChildFile("plugin-scan.tmp");

    for (const auto& format : engine_.pluginHost().availableFormats())
        engine_.pluginHost().scanFormat(format, pedal);

    pluginListsChanged();

    const int blocked = (int) engine_.pluginHost().blockedPlugins().size();
    showStatus("Found " + juce::String((int) engine_.pluginHost().knownPlugins().size()) + " plugin(s)"
               + (blocked > 0 ? ", " + juce::String(blocked) + " blocked - see Plugin Manager" : juce::String()));
}

/** Saves the scan with the plugin manager's lists, and shows the result
    everywhere plugins are offered or listed. */
void MainComponent::pluginListsChanged()
{
    auto& host = engine_.pluginHost();
    settings_.setValue("pluginScanCache", juce::String(host.saveScanCache()));
    settings_.saveIfNeeded();
    effectChain_.setAvailablePlugins(host.offeredPlugins());

    if (pluginManager_ == nullptr)
        return;

    std::vector<PluginManagerDialog::Row> rows;
    for (const auto& entry : host.knownPlugins())
        rows.push_back({ entry.format, entry.identifier, entry.name,
                         entry.disabled ? PluginManagerDialog::Row::State::Off : PluginManagerDialog::Row::State::On });
    for (const auto& [format, identifier] : host.blockedPlugins())
        rows.push_back({ format, identifier, {}, PluginManagerDialog::Row::State::Blocked });
    pluginManager_->setRows(std::move(rows));
}

/** The plugin manager (File > Plugin Manager): see PluginManagerDialog. */
void MainComponent::showPluginManager()
{
    if (pluginManager_ != nullptr)
    {
        if (auto* window = pluginManager_->findParentComponentOfClass<juce::DialogWindow>())
            window->toFront(true);
        return;
    }

    auto  dialog = std::make_unique<PluginManagerDialog>();
    auto& host   = engine_.pluginHost();

    dialog->onSetEnabled = [this, &host](const PluginManagerDialog::Row& row, bool on)
    {
        host.setDisabled(row.format, row.identifier, ! on);
        pluginListsChanged();
    };
    dialog->onUnblock = [this, &host](const PluginManagerDialog::Row& row)
    {
        host.unblock(row.format, row.identifier);
        pluginListsChanged();
        showStatus("Unblocked - the next scan will try it again");
    };
    dialog->onForget = [this, &host](const PluginManagerDialog::Row& row)
    {
        host.forget(row.format, row.identifier);
        pluginListsChanged();
    };
    dialog->onScan = [this] { scanForPlugins(); };

    pluginManager_ = dialog.get();

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Plugin Manager";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();

    pluginListsChanged();
}

/** Opens a hosted plugin's own editor. */
void MainComponent::openPluginEditor(int slotIndex)
{
    // The plugin in the chain the effects panel is showing: the track's, or
    // the selected clip's own.
    const auto       ref  = editedChainRef();
    engine::PluginNode* node = nullptr;
    if (ref.master)
        node = engine_.masterPluginNode(slotIndex);
    else if (ref.isClip())
        node = engine_.clipPluginNode(history_.current().tracks[(size_t) ref.track].clips[(size_t) ref.clip].id,
                                      slotIndex);
    else if (ref.track >= 0)
        node = engine_.trackPluginNode(ref.track, slotIndex);
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
    const auto& song = history_.current();
    window->address  = ref.master ? PluginSlotAddress { 0, 0, slotIndex }
                                  : PluginSlotAddress { song.tracks[(size_t) ref.track].id,
                                                        ref.isClip() ? song.tracks[(size_t) ref.track].clips[(size_t) ref.clip].id : 0,
                                                        slotIndex };
    window->stateAtOpen = node->saveState();
    window->onCloseRequested = [this](PluginEditorWindow* w) { closePluginEditor(w); };
}

/** The document slot at @p at, or nullptr if it's gone. */
model::EffectSlot* MainComponent::pluginSlotFor(model::Song& song, const PluginSlotAddress& at)
{
    auto* track = at.trackId != 0 ? model::findTrack(song, at.trackId) : nullptr;
    if (track == nullptr && at.trackId != 0)
        return nullptr;

    auto* chain = track != nullptr ? &track->effectChain : &song.masterEffects;
    if (track != nullptr && at.clipId != 0)
    {
        const auto clip = std::find_if(track->clips.begin(), track->clips.end(),
                                       [&](const model::Clip& c) { return c.id == at.clipId; });
        if (clip == track->clips.end())
            return nullptr;
        chain = &clip->effects;
    }

    if (at.slot < 0 || at.slot >= (int) chain->size() || (*chain)[(size_t) at.slot].kind != model::EffectKind::Plugin)
        return nullptr;
    return &(*chain)[(size_t) at.slot];
}

/** The running plugin @p window shows, or nullptr if its node has gone. */
engine::PluginNode* MainComponent::pluginNodeFor(const PluginEditorWindow& window)
{
    if (window.address.trackId == 0)
    {
        auto* node = engine_.masterPluginNode(window.address.slot);
        return node != nullptr && node->instance() == window.plugin() ? node : nullptr;
    }

    const auto& tracks = history_.current().tracks;
    const auto  track  = std::find_if(tracks.begin(), tracks.end(),
                                      [&](const model::Track& t) { return t.id == window.address.trackId; });
    if (track == tracks.end())
        return nullptr;

    const auto& at   = window.address;
    auto*       node = at.clipId != 0 ? engine_.clipPluginNode(at.clipId, at.slot)
                                      : engine_.trackPluginNode((int) std::distance(tracks.begin(), track), at.slot);
    return node != nullptr && node->instance() == window.plugin() ? node : nullptr;
}

/** Copies the state of every plugin with an open editor into the document,
    without an undo step: a live tweak, like a fader mid-drag, so the
    document never falls behind what's being heard - a save, an autosave or
    a rebuild of the chain all see it. The engine is told, so it doesn't
    restore the state it was just given (see AudioEngine::notePluginState).
    Only plugins being edited: some never give the same state twice. */
void MainComponent::syncOpenPluginStates()
{
    for (auto* window : pluginWindows_)
    {
        auto* node = pluginNodeFor(*window);
        auto* slot = pluginSlotFor(history_.mutableCurrent(), window->address);
        if (node == nullptr || slot == nullptr)
            continue;

        auto state = node->saveState();
        if (state == slot->plugin.state)
            continue;

        notePluginStateToEngine(window->address, state);
        slot->plugin.state = std::move(state);
    }
}

void MainComponent::notePluginStateToEngine(const PluginSlotAddress& at, const std::string& state)
{
    if (at.trackId == 0)
    {
        engine_.noteMasterPluginState(at.slot, state);
        return;
    }
    if (at.clipId != 0)
    {
        engine_.noteClipPluginState(at.clipId, at.slot, state);
        return;
    }

    const auto& tracks = history_.current().tracks;
    for (int t = 0; t < (int) tracks.size(); ++t)
        if (tracks[(size_t) t].id == at.trackId)
            engine_.notePluginState(t, at.slot, state);
}

/** A plugin's editor is closing: everything done in it becomes one undo
    step, from its state when it opened to its state now. */
void MainComponent::closePluginEditor(PluginEditorWindow* window)
{
    std::string finalState;
    if (auto* node = pluginNodeFor(*window))
        finalState = node->saveState();

    const auto        where   = window->address;
    const std::string from    = window->stateAtOpen;
    const bool        changed = ! finalState.empty() && finalState != from;
    if (changed)
        notePluginStateToEngine(where, finalState);

    pluginWindows_.removeObject(window);
    if (! changed)
        return;

    if (auto* slot = pluginSlotFor(history_.mutableCurrent(), where))
        slot->plugin.state = from; // rewound, so the step undoes to where it began
    history_.edit("Change plugin settings", [where, finalState](model::Song& s)
    {
        if (auto* slot = pluginSlotFor(s, where))
            slot->plugin.state = finalState;
    });
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
    masteringDrag_.begin({}, history_.current().mastering);
}

void MainComponent::endMasteringDrag()
{
    const auto from = masteringDrag_.end({});
    if (! from)
        return;

    commitStructDrag(history_, "Set mastering", *from,
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

    dialog("Save Preset", "Save these " + effectName + " settings as a preset you can use on any track.")
        .text("name", "Name:", "My " + effectName)
        .unsaved()
        .show("Save", [this, slot](const FormDialog::Values& v)
        {
            const auto name = v.text("name");
            if (name.isEmpty())
            {
                showError("A preset needs a name");
                return;
            }

            const auto* effect = model::descriptorFor(slot.kind);
            if (effect == nullptr)
                return;

            userEffectPresets_ = model::withUserPreset(userEffectPresets_, effect->id,
                                                       model::capturePreset(slot, *effect, name.toStdString()));
            storeUserEffectPresets();
            showStatus("Saved preset \"" + name + "\"");
        });
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

/** Remembers an effect slot's parameters before a drag on one of its controls
    started — see EffectChainPanel::onSlotParamsDragStart. */
void MainComponent::beginEffectSlotParamsDrag(int slotIndex)
{
    const auto* chain = editedChain();
    if (chain == nullptr || slotIndex < 0 || slotIndex >= (int) chain->size())
        return;

    effectSlotDrag_.begin({ editedChainRef(), slotIndex }, (*chain)[(size_t) slotIndex]);
}

/** Commits a whole effect-slot-parameters drag as one undo step, the
    commitStructDrag equivalent of endFaderDrag above — a slot's parameters
    are a struct of several fields changed together, not one number, so
    there's no meaningful tolerance to check against: any real change
    commits, equality is the whole test. */
void MainComponent::endEffectSlotParamsDrag(int slotIndex)
{
    const auto ref = editedChainRef();
    const auto from = effectSlotDrag_.end({ ref, slotIndex });
    if (! from)
        return;

    const auto* edited = editedChain();
    if (edited == nullptr || slotIndex < 0 || slotIndex >= (int) edited->size())
        return;
    const auto& chain = *edited;

    // Every parameter of this slot being written was held by this drag.
    for (const auto& key : automation_.heldOnSlot(selectedTrackIndex_, slotIndex))
        automationControlReleased(key);

    const auto landedOn = chain[(size_t) slotIndex];

    commitStructDrag(history_, "Set effect parameters", *from, landedOn,
                     [ref, slotIndex](model::Song& s, const model::EffectSlot& value)
    {
        auto* c = chainAt(s, ref);
        if (c != nullptr && slotIndex >= 0 && slotIndex < (int) c->size())
            (*c)[(size_t) slotIndex] = value;
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
    const auto ref = editedChainRef();
    if (ref.track < 0)
        return;

    const std::string path = file == juce::File{} ? std::string() : file.getFullPathName().toStdString();
    history_.edit(path.empty() ? "Use built-in reverb hall" : "Load impulse response", [ref, slotIndex, path](model::Song& s)
    {
        auto* chain = chainAt(s, ref);
        if (chain != nullptr && slotIndex >= 0 && slotIndex < (int) chain->size())
            (*chain)[(size_t) slotIndex].convolution.irFile = path;
    });

    syncEngineTracks();
    refreshEffectChainForSelected();
}

} // namespace soundsplice
