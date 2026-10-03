#include "MainComponentInternal.h"

#include "engine/Diagnostics.h"
#include "model/EssentialSound.h"
#include "model/Templates.h"
#include "engine/Transcriber.h"
#include "SpectralRender.h"
#include "app/ApplyEffectsDialog.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Audition's finishing workflows: the Diagnostics pane - scan a clip for what's
// wrong with it, then select or fix each problem.

namespace soundsplice
{
namespace
{
    /** A click's stretch widened a little, so the repair has its
        surroundings to work from. */
    constexpr double kClickMarginSeconds = 0.005;
}

/** Scans the selected audio clip (engine/Diagnostics.h) and fills the
    Diagnostics pane with what it found. */
void MainComponent::runDiagnostics()
{
    const auto* clip = selectedAudioClip();
    ClipAudio   audio;
    if (clip == nullptr || clip->warp || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError(clip != nullptr && clip->warp ? "This clip is warped - turn Warp off to diagnose its audio"
                                                : "Select an audio clip first");
        return;
    }

    struct DiagnosticScan final : ClipScan
    {
        engine::diagnostics::Scanner scanner;
        std::vector<engine::diagnostics::Issue> issues;

        void prepare(double sampleRate) override { scanner.prepare(sampleRate, {}); }
        void process(const float* const* outputs, int frames) override { scanner.process(outputs, 2, frames); }
    };

    auto       scan   = std::make_shared<DiagnosticScan>();
    const int  clipId = clip->id;
    const auto name   = juce::File(clip->audioFile).getFileNameWithoutExtension();

    workspace_.revealPanel("Diagnostics");
    scanClipAudio("Diagnostics", "Looking for problems", audio, 0, audio.window.length(), scan,
                  [this, scan, clipId, name](double rate)
    {
        std::vector<DiagnosticsPane::Row> rows;
        for (const auto& issue : scan->scanner.finish())
            rows.push_back({ issue, (double) issue.from / rate, (double) issue.to / rate });

        diagnosedClipId_ = clipId;
        diagnosticsPane_.setRows(std::move(rows), name);
        showStatus("Diagnostics: " + juce::String((int) diagnosticsPane_.rows().size()) + " found in " + name);
    });
}

/** The row's stretch, selected in the audio editor - of the clip it was
    found in, which is selected again if need be. */
void MainComponent::selectDiagnostic(const DiagnosticsPane::Row& row)
{
    const auto where = app::OpenFiles::locate(history_.current(), diagnosedClipId_);
    if (! where.isValid())
    {
        showError("That clip has gone - Scan again");
        return;
    }
    if (where.track != selectedTrackIndex_ || where.clip != selectedClipIndex_)
        selectTrackAndClip(where.track, where.clip);

    workspace_.revealPanel("Audio");
    const double margin = row.issue.kind == engine::diagnostics::Kind::Click ? kClickMarginSeconds : 0.0;
    audioEditor_.selectRange({ juce::jmax(0.0, row.fromSeconds - margin), row.toSeconds + margin });
}

/** Repairs [range] of the selected clip as @p kind needs: clicks filled,
    clipped peaks redrawn, silence taken out, DC offset removed. */
bool MainComponent::fixDiagnosticRange(engine::diagnostics::Kind kind, AudioRange range)
{
    audioEditor_.selectRange(range);
    switch (kind)
    {
        case engine::diagnostics::Kind::Click:    removeClicksInSelection(8.0, 2.0); return true;
        case engine::diagnostics::Kind::Clipping: fixClippingInSelection(95.0, 0.0); return true;
        case engine::diagnostics::Kind::Silence:  deleteAudioSelection(); return true;
        case engine::diagnostics::Kind::DcOffset: removeDcOffsetInSelection(); return true;
    }
    return false;
}

/** Fixes one problem, then scans again: a fix moves what comes after it. */
void MainComponent::fixDiagnostic(const DiagnosticsPane::Row& row)
{
    selectDiagnostic(row);
    if (selectedAudioClip() == nullptr || selectedAudioClip()->id != diagnosedClipId_)
        return;

    const double margin = row.issue.kind == engine::diagnostics::Kind::Click ? kClickMarginSeconds : 0.0;
    const double clipEnd = row.issue.kind == engine::diagnostics::Kind::DcOffset ? row.toSeconds : row.toSeconds + margin;
    fixDiagnosticRange(row.issue.kind, { juce::jmax(0.0, row.fromSeconds - margin), clipEnd });
    runDiagnostics();
}

/** Fixes every problem of @p kind. Clicks, clipping and DC offset in one
    pass over the whole clip; silences one at a time from the last, so
    taking one out doesn't move the ones still to go. */
void MainComponent::fixAllDiagnostics(engine::diagnostics::Kind kind)
{
    std::vector<DiagnosticsPane::Row> rows;
    for (const auto& row : diagnosticsPane_.rows())
        if (row.issue.kind == kind)
            rows.push_back(row);
    if (rows.empty())
        return;

    selectDiagnostic(rows.front());
    if (selectedAudioClip() == nullptr || selectedAudioClip()->id != diagnosedClipId_)
        return;

    if (kind == engine::diagnostics::Kind::Silence)
    {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            fixDiagnosticRange(kind, { it->fromSeconds, it->toSeconds });
    }
    else
    {
        // From the first to the last: the repairs only touch what they find.
        const double from = rows.front().fromSeconds;
        double       to   = rows.front().toSeconds;
        for (const auto& row : rows)
            to = juce::jmax(to, row.toSeconds);
        fixDiagnosticRange(kind, { juce::jmax(0.0, from - kClickMarginSeconds), to + kClickMarginSeconds });
    }
    runDiagnostics();
}

/** Batch Process (app/BatchProcess.h): asks for the folder of files, then
    the chain, then the options and where to write, then runs. */
void MainComponent::startBatchProcess(std::optional<std::vector<model::EffectSlot>> chain)
{
    chooser_ = std::make_unique<juce::FileChooser>("Batch Process: choose a folder of audio files",
                                                   juce::File(settings_.getValue("batch.inputFolder")));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this, chain](const juce::FileChooser& fc)
        {
            const auto folder = fc.getResult();
            if (folder == juce::File {})
                return;

            auto inputs = batch::audioFilesIn(folder);
            if (inputs.empty())
            {
                showError("No audio files in " + folder.getFileName());
                return;
            }
            settings_.setValue("batch.inputFolder", folder.getFullPathName());
            if (chain)
                chooseBatchOptions(std::move(inputs), *chain); // a macro's
            else
                chooseBatchChain(std::move(inputs));
        });
}

/** The chain, in the Apply Effects dialog: the same effect panel, presets
    and plugins as everywhere else. */
void MainComponent::chooseBatchChain(std::vector<juce::File> inputs)
{
    auto dialog = std::make_unique<ApplyEffectsDialog>();
    dialog->setSize(520, 460);
    dialog->setUserPresets(userEffectPresets_);
    dialog->setAvailablePlugins(engine_.pluginHost().offeredPlugins());
    dialog->onPluginEditorRequested = [this](int slotIndex, const model::EffectSlot& slot) { openScratchPluginEditor(slotIndex, slot); };
    dialog->onChainAboutToChange    = [this] { closeScratchPluginEditors(); };
    dialog->onPresetSaveRequested   = [this](const model::EffectSlot& slot) { promptToSaveEffectPreset(slot); };
    dialog->onUserPresetDeleted     = [this](const std::string& effectId, const std::string& name) { deleteUserEffectPreset(effectId, name); };
    dialog->onDismissed = [safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (safe != nullptr)
            safe->closeScratchPluginEditors();
    };
    dialog->setForBatch();
    applyEffectsDialog_ = dialog.get();

    auto* raw = dialog.get();
    raw->onApply = [this, raw, inputs](const std::vector<model::EffectSlot>& chain)
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
        chooseBatchOptions(inputs, withScratchPluginStates(chain));
    };
    raw->onCancel = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Batch Process " + juce::String((int) inputs.size()) + " Files: Effects";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** A loudness target and an output format, then the folder to write to. */
void MainComponent::chooseBatchOptions(std::vector<juce::File> inputs, std::vector<model::EffectSlot> chain)
{
    struct Loudness
    {
        double      lufs;
        const char* name;
    };
    static constexpr Loudness kLoudness[] { { 0.0, "Leave it" }, { -14.0, "-14 LUFS" }, { -16.0, "-16 LUFS" },
                                            { -18.0, "-18 LUFS" }, { -23.0, "-23 LUFS" }, { -24.0, "-24 LUFS" } };
    struct Format
    {
        engine::ExportFormat format;
        int                  bits;
        const char*          name;
    };
    static constexpr Format kFormats[] { { engine::ExportFormat::Wav, 24, "WAV, 24-bit" },
                                         { engine::ExportFormat::Wav, 16, "WAV, 16-bit" },
                                         { engine::ExportFormat::Wav, 32, "WAV, 32-bit float" },
                                         { engine::ExportFormat::Flac, 24, "FLAC, 24-bit" } };


    juce::StringArray loudnessNames, formatNames;
    for (const auto& l : kLoudness) loudnessNames.add(l.name);
    for (const auto& f : kFormats)  formatNames.add(f.name);

    dialog("Batch Process: Output",
           juce::String((int) inputs.size()) + " files, " + juce::String((int) chain.size())
               + (chain.size() == 1 ? " effect" : " effects") + ". New files are written; the originals aren't touched.")
        .choice("batch.loudness", "Loudness:", loudnessNames, 0)
        .choice("batch.format", "Format:", formatNames, 0)
        .show("Choose Output Folder...", [this, inputs, chain](const FormDialog::Values& v)
        {
            const auto& format = kFormats[v.choice("batch.format")];

            batch::Settings settings;
            settings.chain                = chain;
            settings.loudnessLufs         = kLoudness[v.choice("batch.loudness")].lufs;
            settings.output.format        = format.format;
            settings.output.bitsPerSample = format.bits;
            settings.bpm                  = history_.current().bpm;

            chooser_ = std::make_unique<juce::FileChooser>("Batch Process: where to write the new files",
                                                           juce::File(settings_.getValue("batch.outputFolder")));
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [self = juce::Component::SafePointer<MainComponent>(this), inputs, settings](const juce::FileChooser& fc) mutable
                {
                    if (self == nullptr || fc.getResult() == juce::File {})
                        return;
                    settings.folder = fc.getResult();
                    self->settings_.setValue("batch.outputFolder", settings.folder.getFullPathName());
                    self->runBatch(inputs, settings);
                });
        });
}

/** Runs the batch: on a background job with built-in effects, on this
    thread (one file at a time, busy) when the chain has a hosted plugin,
    which can only be made here. Says how it went. */
void MainComponent::runBatch(std::vector<juce::File> inputs, batch::Settings settings)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    struct Outcome
    {
        int          done = 0;
        int          failed = 0;
        juce::String firstFailure;
    };
    auto outcome = std::make_shared<Outcome>();
    const auto record = [outcome](const juce::File& input, const batch::Result& result)
    {
        if (result.ok)
            ++outcome->done;
        else if (++outcome->failed == 1)
            outcome->firstFailure = input.getFileName() + " " + result.error;
    };
    const auto report = [this, outcome, total = (int) inputs.size(), folder = settings.folder]
    {
        auto message = "Batch Process: wrote " + juce::String(outcome->done) + " of " + juce::String(total)
                     + " files to " + folder.getFileName();
        if (outcome->failed > 0)
            showError(message + " - " + outcome->firstFailure
                      + (outcome->failed > 1 ? " (and " + juce::String(outcome->failed - 1) + " more)" : juce::String()));
        else
            showStatus(message);
    };

    const bool hasPlugin = std::any_of(settings.chain.begin(), settings.chain.end(),
                                       [](const model::EffectSlot& s) { return s.enabled && s.kind == model::EffectKind::Plugin; });
    if (hasPlugin)
    {
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            showBusy("Batch Process: " + juce::String((int) i + 1) + " of " + juce::String((int) inputs.size()));
            record(inputs[i], batch::processFile(inputs[i], settings, engine_.pluginHost()));
        }
        report();
        return;
    }

    auto work = [inputs, settings, record](app::OfflineRenderJob& job)
    {
        engine::PluginHost noPlugins; // built-ins only on this thread
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            if (job.shouldAbort())
                return;
            job.report((double) i / (double) inputs.size(), "Processing " + inputs[i].getFileName());
            record(inputs[i], batch::processFile(inputs[i], settings, noPlugins));
        }
    };
    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), report](bool cancelled)
    {
        if (self == nullptr)
            return;
        self->renderJob_.reset();
        if (cancelled)
            self->showStatus("Batch Process stopped - the files already written are kept");
        report();
    };
    renderJob_ = app::OfflineRenderJob::launch("Batch Process", std::move(work), std::move(onFinished));
}

/** Applies favorite @p index's chain as Apply Effects would: to the time
    selection across audio tracks if there is one, else the audio editor's
    selection. */
void MainComponent::applyFavorite(int index)
{
    if (index < 0 || index >= (int) favorites_.size())
        return;

    const auto favorite = favorites_[(size_t) index];
    if (! applyChainToSelection(favorite.chain, "\"" + juce::String(favorite.name) + "\""))
        return;
    noteMacroEffects(favorite.chain);
    showStatus("Applied favorite \"" + juce::String(favorite.name) + "\"");
}

/** Applies @p chain to the time selection across audio tracks, or else the
    audio editor's selection - whichever there is. True if it changed
    anything; if there's no selection, says so, naming @p what. */
bool MainComponent::applyChainToSelection(const std::vector<model::EffectSlot>& chain, const juce::String& what)
{
    const auto& tracks = history_.current().tracks;
    const bool  onTime = ! timeSelection_.isEmpty()
                      && std::any_of(tracks.begin(), tracks.end(), [this](const model::Track& t)
                                     { return t.type == model::TrackType::Audio && timeSelection_.includes(t.id); });
    const auto before = history_.stateId();
    if (onTime)
        applyEffectsToTimeSelection(chain);
    else if (selectedAudioClip() != nullptr && ! audioEditor_.selection().isEmpty())
        applyEffectsToSelection(chain);
    else
    {
        showError("Select part of a clip in the audio editor, or time across audio tracks, to apply " + what + " to");
        return false;
    }
    return history_.stateId() != before;
}

/** Asks for a name and keeps @p chain as a favorite under it. */
void MainComponent::promptSaveFavorite(std::vector<model::EffectSlot> chain)
{
    if (chain.empty())
    {
        showError("Add an effect first - a favorite is a chain of them");
        return;
    }


    dialog("Save as Favorite", "The Favorites menu applies it to the selection in one click.")
        .text("name", "Name:", {})
        .unsaved()
        .show("Save", [this, chain](const FormDialog::Values& v)
        {
            const auto name = v.text("name");
            if (name.isEmpty())
            {
                showError("A favorite needs a name");
                return;
            }
            favorites_ = model::withFavorite(favorites_, { name.toStdString(), chain });
            settings_.setValue("favorites", juce::String(model::serializeFavorites(favorites_)));
            settings_.saveIfNeeded();
            showStatus("Saved \"" + name + "\" in the Favorites menu");
        });
}

void MainComponent::removeFavorite(int index)
{
    if (index < 0 || index >= (int) favorites_.size())
        return;
    const auto name = juce::String(favorites_[(size_t) index].name);
    favorites_.erase(favorites_.begin() + index);
    settings_.setValue("favorites", juce::String(model::serializeFavorites(favorites_)));
    settings_.saveIfNeeded();
    showStatus("Removed \"" + name + "\" from Favorites");
}

/** The Essential Sound pane shows the selected audio clip's tag. */
void MainComponent::refreshEssentialSoundForSelected()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
    {
        essentialSoundPane_.setClip(false, model::SoundRole::None, {}, {});
        return;
    }
    essentialSoundPane_.setClip(true, clip->essential.role, clip->essential.amounts,
                                juce::File(clip->audioFile).getFileNameWithoutExtension());
}

/** Tags the selected audio clip (or clears its tag), as one undo step. */
void MainComponent::setEssentialRole(model::SoundRole role)
{
    if (selectedAudioClip() == nullptr)
        return;
    const int trackIndex = selectedTrackIndex_, clipIndex = selectedClipIndex_;
    history_.edit(role == model::SoundRole::None ? "Clear Essential Sound tag" : "Tag clip",
                  [trackIndex, clipIndex, role](model::Song& s)
    {
        model::essential::setRole(s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex], role);
    });
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshEssentialSoundForSelected();
}

/** A task's amount, live as its slider moves; the drag is committed as one
    undo step on release (endEssentialDrag). */
void MainComponent::setEssentialAmount(const std::string& task, float amount)
{
    if (selectedAudioClip() == nullptr)
        return;
    auto& clip = history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].clips[(size_t) selectedClipIndex_];
    if (! essentialDragging_)
    {
        // A click or a wheel turn: its own undo step.
        const auto before = clip.essential;
        model::essential::setAmount(clip, task, amount);
        const auto after = clip.essential;
        clip.essential = before;
        model::essential::apply(clip);
        const int trackIndex = selectedTrackIndex_, clipIndex = selectedClipIndex_;
        history_.edit("Essential Sound", [trackIndex, clipIndex, after](model::Song& s)
        {
            auto& c     = s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
            c.essential = after;
            model::essential::apply(c);
        });
    }
    else
        model::essential::setAmount(clip, task, amount);

    syncEngineTracks();
    refreshEffectChainForSelected();
}

void MainComponent::endEssentialDrag()
{
    if (! essentialDragging_ || selectedAudioClip() == nullptr)
        return;
    essentialDragging_ = false;

    const int  trackIndex = selectedTrackIndex_, clipIndex = selectedClipIndex_;
    const auto landed     = selectedAudioClip()->essential;
    commitStructDrag(history_, std::string("Essential Sound"), essentialDragFrom_, landed,
                     [trackIndex, clipIndex](model::Song& s, const model::EssentialSettings& settings)
                     {
                         auto& c     = s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
                         c.essential = settings;
                         model::essential::apply(c);
                     });
}

/** Match Loudness over every clip tagged @p role, wherever it is. */
void MainComponent::matchLoudnessForRole(model::SoundRole role, double lufs)
{
    std::vector<app::MatchedClip> clips;
    for (const auto& track : history_.current().tracks)
        for (const auto& clip : track.clips)
            if (clip.type == model::ClipType::Audio && clip.essential.role == role)
                clips.push_back({ track.id, clip.id });
    if (clips.empty())
        return;
    matchLoudness(clips, lufs, true);
}

/** Ducks every clip tagged @p role under the Dialogue clips: a volume curve
    dipping @p depthDb wherever Dialogue plays (Auto Duck's curves, so each
    stays editable). Tracks holding Dialogue themselves are left alone. */
void MainComponent::duckUnderDialogue(model::SoundRole role, float depthDb)
{
    const auto& song = history_.current();
    std::vector<std::pair<double, double>> dialogue;
    std::vector<int> dialogueTracks, ducked;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
        {
            if (clip.essential.role == model::SoundRole::Dialogue)
            {
                dialogue.emplace_back(clip.startBeats, clip.startBeats + clip.lengthBeats);
                dialogueTracks.push_back(track.id);
            }
            else if (clip.essential.role == role)
                ducked.push_back(track.id);
        }

    std::sort(ducked.begin(), ducked.end());
    ducked.erase(std::unique(ducked.begin(), ducked.end()), ducked.end());
    ducked.erase(std::remove_if(ducked.begin(), ducked.end(), [&](int id)
                 { return std::find(dialogueTracks.begin(), dialogueTracks.end(), id) != dialogueTracks.end(); }),
                 ducked.end());

    if (dialogue.empty() || ducked.empty())
    {
        showError(dialogue.empty() ? "Tag some clips as Dialogue first"
                                   : "Those clips share a track with Dialogue - put them on a track of their own to duck them");
        return;
    }

    // Gaps under a second between lines don't let the bed back up.
    const auto merged = model::arrangeedit::mergeRegions(dialogue, model::clockFor(song).beatsAfter(0.0, 1.0));
    const auto gain   = juce::Decibels::decibelsToGain(depthDb);
    int        count  = 0;
    history_.edit("Duck under dialogue", [&](model::Song& s) { count = model::arrangeedit::duckClips(s, ducked, merged, gain, 0.4); });
    refreshAfterArrangementEdit();
    showStatus("Ducked " + juce::String(count) + " clips " + juce::String(-depthDb, 1) + " dB under the dialogue");
}

/** Takes each channel's mean out of the audio editor's selection. */
void MainComponent::removeDcOffsetInSelection()
{
    if (editSelection("Remove DC offset", false, [](std::vector<std::vector<float>>& channels, double)
        {
            for (auto& channel : channels)
            {
                if (channel.empty())
                    continue;
                double sum = 0.0;
                for (const float x : channel)
                    sum += x;
                const auto mean = (float) (sum / (double) channel.size());
                for (auto& x : channel)
                    x -= mean;
            }
        }))
        showStatus("Removed the DC offset");
}

// ---- Macros -----------------------------------------------------------------

void MainComponent::saveMacros()
{
    settings_.setValue("macros", macros::serialize(macros_));
    settings_.saveIfNeeded();
    if (macrosDialog_ != nullptr)
        macrosDialog_->setMacros(macros_);
    commandManager_.commandStatusChanged();
}

/** Record Macro: starts gathering, or stops and asks for a name. */
void MainComponent::toggleMacroRecording()
{
    if (! recordingMacro_)
    {
        recordingMacro_ = macros::Macro {};
        showStatus("Recording a macro: the commands and effects you use now are kept - Tools > Stop Recording Macro when done");
        commandManager_.commandStatusChanged();
        return;
    }

    auto recorded = std::move(*recordingMacro_);
    recordingMacro_.reset();
    commandManager_.commandStatusChanged();
    if (recorded.steps.empty())
    {
        showStatus("Stopped recording: nothing was recorded");
        return;
    }


    dialog("Save Macro", juce::String((int) recorded.steps.size()) + " steps recorded.")
        .text("name", "Name:", "Macro " + juce::String((int) macros_.size() + 1))
        .unsaved()
        .cancelButton("Discard")
        .show("Save", [this, recorded](const FormDialog::Values& v) mutable
        {
            const auto name = v.text("name");
            if (name.isEmpty())
                return;
            recorded.name = name.toStdString();
            macros_       = macros::with(macros_, std::move(recorded));
            saveMacros();
            showStatus("Saved macro \"" + name + "\" in the Tools menu");
        });
}

/** A command was run: kept, if a macro is being recorded and it can be a step. */
void MainComponent::noteMacroCommand(juce::CommandID id)
{
    if (! recordingMacro_ || runningMacro_)
        return;
    const auto* definition = commands::find(id);
    if (definition == nullptr || definition->id == commands::recordMacro)
        return;
    if (! macros::recordable(*definition))
    {
        // Apply Effects is recorded as its effects, once they're applied.
        if (definition->id != commands::applyEffects)
            showStatus(juce::String(definition->name) + " isn't recorded: a macro step can't stop to ask anything");
        return;
    }
    recordingMacro_->steps.push_back({ definition->name, {} });
}

void MainComponent::noteMacroEffects(const std::vector<model::EffectSlot>& chain)
{
    if (recordingMacro_ && ! runningMacro_ && ! chain.empty())
        recordingMacro_->steps.push_back({ {}, chain });
}

/** Runs a macro's steps in order on the selection, stopping at the first
    one that can't run. */
bool MainComponent::runMacro(int index)
{
    if (runningMacro_ || index < 0 || index >= (int) macros_.size())
        return false;
    const auto macro = macros_[(size_t) index]; // a step may change the list
    if (macro.steps.empty())
        return false;

    runningMacro_ = true;
    int number    = 0;
    for (const auto& step : macro.steps)
    {
        ++number;
        juce::String problem;
        if (step.isEffects())
        {
            if (! applyChainToSelection(step.effects, "the macro"))
                problem = "it needs a selection to apply its effects to";
        }
        else if (const auto* definition = macros::commandFor(step); definition == nullptr)
            problem = "there's no such command any more";
        else
        {
            juce::ApplicationCommandInfo info(definition->id);
            getCommandInfo(definition->id, info);
            if ((info.flags & juce::ApplicationCommandInfo::isDisabled) != 0
                || ! commandManager_.invokeDirectly(definition->id, false))
                problem = "it can't be used right now";
        }

        if (problem.isNotEmpty())
        {
            runningMacro_ = false;
            showError("Macro \"" + juce::String(macro.name) + "\" stopped at step " + juce::String(number) + " ("
                      + macros::describe(step) + "): " + problem);
            commandManager_.commandStatusChanged();
            return false;
        }
    }
    runningMacro_ = false;

    // Running a macro while recording one records its steps.
    if (recordingMacro_)
        recordingMacro_->steps.insert(recordingMacro_->steps.end(), macro.steps.begin(), macro.steps.end());

    commandManager_.commandStatusChanged();
    showStatus("Ran macro \"" + juce::String(macro.name) + "\": " + juce::String(number) + (number == 1 ? " step" : " steps"));
    return true;
}

/** Apply Macro to Files: which macro, from those that are all effects. */
void MainComponent::chooseMacroForFiles()
{
    juce::PopupMenu menu;
    for (int i = 0; i < (int) macros_.size(); ++i)
    {
        const bool ok = ! macros_[(size_t) i].steps.empty() && macros::effectsOnly(macros_[(size_t) i]).has_value();
        menu.addItem(i + 1, macros_[(size_t) i].name + (ok ? "" : "   (has command steps)"), ok);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                       [self = juce::Component::SafePointer<MainComponent>(this)](int chosen)
                       {
                           if (self != nullptr && chosen > 0)
                               self->runMacroOnFiles(chosen - 1);
                       });
}

/** A macro of effects over a folder: Batch Process with its chain. */
void MainComponent::runMacroOnFiles(int index)
{
    if (index < 0 || index >= (int) macros_.size())
        return;
    const auto chain = macros::effectsOnly(macros_[(size_t) index]);
    if (! chain || chain->empty())
    {
        showError("Only a macro made of effects can run over files: commands act on the project");
        return;
    }
    startBatchProcess(*chain);
}

void MainComponent::showMacros()
{
    if (macrosDialog_ != nullptr)
    {
        if (auto* window = macrosDialog_->findParentComponentOfClass<juce::DialogWindow>())
            window->toFront(true);
        return;
    }

    auto dialog = std::make_unique<MacrosDialog>();
    dialog->setMacros(macros_);
    dialog->onChanged = [this](const std::vector<macros::Macro>& edited)
    {
        macros_ = edited;
        settings_.setValue("macros", macros::serialize(macros_));
        settings_.saveIfNeeded();
        commandManager_.commandStatusChanged();
    };
    dialog->onRun         = [this](int index) { runMacro(index); };
    dialog->onRunOnFiles  = [this](int index) { runMacroOnFiles(index); };
    dialog->onEditEffects = [this](int macro, int step, const std::vector<model::EffectSlot>& current)
    {
        editMacroEffects(macro, step, current);
    };
    macrosDialog_ = dialog.get();

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Macros";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** The effects of a macro step, in the Apply Effects dialog. */
void MainComponent::editMacroEffects(int macro, int step, std::vector<model::EffectSlot> current)
{
    auto dialog = std::make_unique<ApplyEffectsDialog>();
    dialog->setSize(520, 460);
    dialog->setUserPresets(userEffectPresets_);
    dialog->setAvailablePlugins(engine_.pluginHost().offeredPlugins());
    dialog->onPluginEditorRequested = [this](int slotIndex, const model::EffectSlot& slot) { openScratchPluginEditor(slotIndex, slot); };
    dialog->onChainAboutToChange    = [this] { closeScratchPluginEditors(); };
    dialog->onPresetSaveRequested   = [this](const model::EffectSlot& slot) { promptToSaveEffectPreset(slot); };
    dialog->onUserPresetDeleted     = [this](const std::string& effectId, const std::string& name) { deleteUserEffectPreset(effectId, name); };
    dialog->onDismissed = [safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (safe != nullptr)
            safe->closeScratchPluginEditors();
    };
    dialog->setForMacro(std::move(current));
    applyEffectsDialog_ = dialog.get();

    auto* raw = dialog.get();
    raw->onApply = [this, raw, macro, step](const std::vector<model::EffectSlot>& chain)
    {
        const auto kept = withScratchPluginStates(chain);
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
        if (macrosDialog_ != nullptr && ! kept.empty())
            macrosDialog_->setEffects(macro, step, kept);
    };
    raw->onCancel = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = step < 0 ? "Macro: Add Effects" : "Macro: Edit Effects";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

// ---- Scripting --------------------------------------------------------------

/** What a script can see and do (scripting::Host): the commands and macros,
    the tracks, the playhead and the time selection, in seconds. Each change
    is an undo step of its own, as it would be made by hand. */
scripting::Host MainComponent::makeScriptHost()
{
    scripting::Host host;

    host.commandNames = []
    {
        std::vector<std::string> names;
        for (const auto& definition : commands::all())
            names.push_back(juce::String(definition.name).upToFirstOccurrenceOf("   ", false, false).toStdString());
        return names;
    };
    host.runCommand = [this](const std::string& name)
    {
        // "Fade In", or "Normalize" for "Normalize...".
        for (const auto& definition : commands::all())
        {
            const auto full = juce::String(definition.name).upToFirstOccurrenceOf("   ", false, false);
            if (full != juce::String(name) && full.trimCharactersAtEnd(".") != juce::String(name))
                continue;
            juce::ApplicationCommandInfo info(definition.id);
            getCommandInfo(definition.id, info);
            return (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0
                && commandManager_.invokeDirectly(definition.id, false);
        }
        return false;
    };
    host.macroNames = [this]
    {
        std::vector<std::string> names;
        for (const auto& macro : macros_)
            names.push_back(macro.name);
        return names;
    };
    host.runMacro = [this](const std::string& name)
    {
        for (int i = 0; i < (int) macros_.size(); ++i)
            if (macros_[(size_t) i].name == name)
                return runMacro(i);
        return false;
    };
    host.status = [this](const std::string& text) { showStatus(juce::String::fromUTF8(text.c_str())); };

    host.tracks = [this]
    {
        std::vector<scripting::TrackInfo> list;
        const auto& song = history_.current();
        for (int i = 0; i < (int) song.tracks.size(); ++i)
        {
            const auto& track = song.tracks[(size_t) i];
            scripting::TrackInfo info;
            info.name     = track.name;
            info.type     = track.type == model::TrackType::Audio ? "audio"
                          : track.type == model::TrackType::Bus   ? "bus" : "midi";
            info.clips    = (int) track.clips.size();
            info.volumeDb = track.gainDb;
            info.pan      = track.pan;
            info.muted    = track.muted;
            info.soloed   = track.solo;
            info.selected = i == selectedTrackIndex_;
            list.push_back(std::move(info));
        }
        return list;
    };
    host.setTrackVolume = [this](int index, double db)
    {
        const auto gain = (float) juce::jlimit(-96.0, 12.0, db);
        history_.edit("Set track volume", [index, gain](model::Song& s) { s.tracks[(size_t) index].gainDb = gain; });
        engine_.setTrackGainDb(index, gain);
        updateMixerStrips();
        return true;
    };
    host.setTrackPan = [this](int index, double pan)
    {
        const auto value = (float) juce::jlimit(-1.0, 1.0, pan);
        history_.edit("Set track pan", [index, value](model::Song& s) { s.tracks[(size_t) index].pan = value; });
        engine_.setTrackPan(index, value);
        updateMixerStrips();
        return true;
    };
    host.setTrackMuted  = [this](int index, bool on) { setTrackMuted(index, on); return true; };
    host.setTrackSoloed = [this](int index, bool on) { setTrackSolo(index, on); return true; };
    host.renameTrack    = [this](int index, const std::string& name)
    {
        if (juce::String(name).trim().isEmpty())
            return false;
        const int id = history_.current().tracks[(size_t) index].id;
        history_.edit("Rename track", [id, name](model::Song& s) { model::renameTrack(s, id, name); });
        syncEngineTracks();
        updateMixerStrips();
        refreshSessionView();
        arrangementView_.setSong(history_.current());
        updateEditingLabel();
        return true;
    };
    host.selectTrack = [this](int index) { selectTrackAndRefreshAll(index); return true; };

    host.playhead    = [this] { return model::clockFor(history_.current()).secondsAt(playheadBeat()); };
    host.setPlayhead = [this](double seconds) { seekToBeat(model::clockFor(history_.current()).beatAt(seconds)); };
    host.selection   = [this]() -> std::optional<std::pair<double, double>>
    {
        if (timeSelection_.isEmpty())
            return std::nullopt;
        const auto clock = model::clockFor(history_.current());
        return std::pair(clock.secondsAt(timeSelection_.startBeats), clock.secondsAt(timeSelection_.endBeats));
    };
    host.setSelection = [this](double start, double end)
    {
        const auto& song  = history_.current();
        const auto  clock = model::clockFor(song);
        model::TimeSelection selection;
        selection.startBeats = clock.beatAt(start);
        selection.endBeats   = clock.beatAt(end);
        for (const auto& track : song.tracks)
            if (track.type == model::TrackType::Audio)
                selection.trackIds.push_back(track.id);
        if (selection.trackIds.empty())
            return false;
        setTimeSelection(selection);
        return true;
    };
    host.bpm = [this] { return history_.current().bpm; };
    return host;
}

void MainComponent::runScript(const juce::String& code, const juce::String& name)
{
    scripting::Engine engine(makeScriptHost());
    const auto result = engine.run(code.toStdString(), name.toStdString());
    scriptPane_.showResult(result);
    if (! result.ok)
        showError("The script stopped: " + juce::String::fromUTF8(result.error.c_str()).upToFirstOccurrenceOf("\n", false, false));
}

/** Run Script: a .lua file, run as it is; what it prints goes to the Script pane. */
void MainComponent::chooseScriptToRun()
{
    chooser_ = std::make_unique<juce::FileChooser>("Run Script", juce::File(settings_.getValue("script.folder")), "*.lua");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File {})
                                  return;
                              settings_.setValue("script.folder", file.getParentDirectory().getFullPathName());
                              if (workspace_.isPanelOpen("Script"))
                                  workspace_.revealPanel("Script");
                              else
                                  togglePanel(panelMenuIndex("Script"));
                              runScript(file.loadFileAsString(), file.getFileName());
                          });
}

// ---- Project templates ------------------------------------------------------

/** A new project from a template, after asking to save this one. */
void MainComponent::newFromTemplate(model::Song song)
{
    confirmDiscardChanges([this, song = std::move(song)]
    {
        startProject(song);
        showStatus("New project from a template: its tracks are ready for audio");
    });
}

juce::File MainComponent::templatesFolder() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("SoundSplice").getChildFile("Templates");
}

/** The user's templates, by name. */
std::vector<juce::File> MainComponent::userTemplates() const
{
    std::vector<juce::File> found;
    if (! templatesFolder().isDirectory())
        return found;
    for (const auto& entry : juce::RangedDirectoryIterator(templatesFolder(), false, "*.soundsplice", juce::File::findFiles))
        found.push_back(entry.getFile());
    std::sort(found.begin(), found.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName()) < 0; });
    return found;
}

/** Save as Template: this project without its audio, under a name, in the
    templates folder File > New from Template lists. */
void MainComponent::saveAsTemplate()
{

    dialog("Save as Template",
           "Keeps the tracks, their routing and effects, and the tempo - not the audio, notes or markers.")
        .text("name", "Name:",
              projectFile_ != juce::File() ? projectFile_.getFileNameWithoutExtension() : juce::String("My Template"))
        .unsaved()
        .show("Save", [this](const FormDialog::Values& v)
        {
            const auto name = juce::File::createLegalFileName(v.text("name"));
            if (name.isEmpty())
                return;

            const auto folder = templatesFolder();
            folder.createDirectory();
            const auto file = folder.getChildFile(name + ".soundsplice");
            const auto text = model::serialize(model::templates::asTemplate(history_.current()));
            if (! file.replaceWithText(juce::String::fromUTF8(text.c_str())))
            {
                showError("Could not write " + file.getFullPathName());
                return;
            }
            showStatus("Saved template \"" + name + "\": File > New from Template");
        });
}

// ---- Preferences ------------------------------------------------------------

void MainComponent::showPreferences(int tab)
{
    if (preferencesDialog_ != nullptr)
    {
        preferencesDialog_->tabsForTesting().setCurrentTabIndex(tab);
        if (auto* window = preferencesDialog_->findParentComponentOfClass<juce::DialogWindow>())
            window->toFront(true);
        return;
    }

    auto dialog        = std::make_unique<PreferencesDialog>(preferencePages(), tab);
    preferencesDialog_ = dialog.get();

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Preferences";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** What Preferences shows. Most rows are a command the menus already have,
    read through getCommandInfo and changed by running it, so there's one
    copy of each setting and the menu and the window can't disagree. */
std::vector<prefs::Page> MainComponent::preferencePages()
{
    const auto ticked = [this](commands::Id id)
    {
        juce::ApplicationCommandInfo info(id);
        getCommandInfo(id, info);
        return (info.flags & juce::ApplicationCommandInfo::isTicked) != 0;
    };
    const auto commandToggle = [this, ticked](commands::Id id)
    {
        const auto* definition = commands::find(id);
        return prefs::toggle(juce::String(definition->name).upToFirstOccurrenceOf("   ", false, false),
                             [ticked, id] { return ticked(id); },
                             [this, ticked, id](bool on)
                             {
                                 if (ticked(id) != on)
                                     commandManager_.invokeDirectly(id, false);
                             },
                             definition->description);
    };
    const auto commandChoice = [this, ticked](juce::String label, std::vector<commands::Id> ids, juce::StringArray names)
    {
        return prefs::choice(std::move(label), std::move(names),
                             [ticked, ids]
                             {
                                 for (int i = 0; i < (int) ids.size(); ++i)
                                     if (ticked(ids[(size_t) i]))
                                         return i;
                                 return -1;
                             },
                             [this, ids](int index) { commandManager_.invokeDirectly(ids[(size_t) index], false); });
    };
    const auto savedBool = [this](const char* key, bool fallback)
    {
        return [this, key, fallback] { return settings_.getBoolValue(key, fallback); };
    };
    const auto storeBool = [this](const char* key)
    {
        return [this, key](bool on)
        {
            settings_.setValue(key, on);
            settings_.saveIfNeeded();
        };
    };

    std::vector<prefs::Page> pages;

    pages.push_back({ "Devices", {
        prefs::custom([this]
        {
            auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(engine_.deviceManager(), 0, 2, 1, 2,
                                                                                  true, true, true, false);
            return std::unique_ptr<juce::Component>(std::move(selector));
        }, 380),
        commandToggle(commands::followSystemOutput),
    } });

    // The recording format: what Recording Format... sets, without its dialog.
    const auto setRecordFormat = [this](const char* key, int value)
    {
        if (awaitingRecordedTake_)
        {
            showError("Stop recording first");
            return;
        }
        settings_.setValue(key, value);
        settings_.saveIfNeeded();
        engine_.setRecordFormat(savedRecordFormat());
    };
    pages.push_back({ "Recording", {
        prefs::choice("Bit depth", { "16-bit", "24-bit", "32-bit float" },
                      [this] { const int bits = settings_.getIntValue("recordBits", 24); return bits == 16 ? 0 : bits == 32 ? 2 : 1; },
                      [setRecordFormat](int i) { static constexpr int kBits[] { 16, 24, 32 }; setRecordFormat("recordBits", kBits[i]); },
                      "What takes are written as. 32-bit float can't clip in the file."),
        prefs::choice("Channels", { "Mono", "Stereo" },
                      [this] { return settings_.getIntValue("recordChannels", 2) == 1 ? 0 : 1; },
                      [setRecordFormat](int i) { setRecordFormat("recordChannels", i == 0 ? 1 : 2); }),
        prefs::toggle("Line takes up with what played", savedBool("compensateRecordingLatency", true),
                      storeBool("compensateRecordingLatency"),
                      "Move each take earlier by the device's delay, measured or reported, so it lines up."),
        prefs::number("Latency adjustment", -200.0, 200.0, 0.1, " ms",
                      [this] { return settings_.getDoubleValue("recordingLatencyAdjustMs", 0.0); },
                      [this](double ms) { settings_.setValue("recordingLatencyAdjustMs", ms); },
                      "Added to the device's delay: more moves takes earlier."),
        prefs::action("Latency", "Measure...", [this] { measureRecordingLatency(); }, {},
                      "Time a click through a cable from an output to an input."),
        commandToggle(commands::punchRecording),
        commandToggle(commands::keepRecentInput),
        commandToggle(commands::soundActivatedRecording),
    } });

    pages.push_back({ "Editing", {
        commandToggle(commands::snapToGrid),
        commandToggle(commands::snapToMarkers),
        commandToggle(commands::snapToClipEdges),
        commandToggle(commands::autoCrossfades),
        commandChoice("Time shown in",
                      { commands::timeFormatBarsBeats, commands::timeFormatMinutesSeconds, commands::timeFormatSamples,
                        commands::timeFormatTimecode },
                      { "Bars and beats", "Minutes and seconds", "Samples", "Timecode" }),
        commandChoice("Timecode frames", { commands::timecode24, commands::timecode25, commands::timecode30 },
                      { "24 fps", "25 fps", "30 fps" }),
    } });

    pages.push_back({ "Display", {
        commandToggle(commands::showClipEnvelopes),
        commandToggle(commands::showTakeLanes),
        commandToggle(commands::waveformDbScale),
        commandToggle(commands::trackSpectrograms),
        prefs::heading("Theme"),
        prefs::choice("Theme",
                      [] { juce::StringArray names; for (const auto& t : theme::all()) names.add(t.name); return names; }(),
                      [this]
                      {
                          const auto& current = theme::named(settings_.getValue("theme", "Dark"));
                          for (int i = 0; i < (int) theme::all().size(); ++i)
                              if (&theme::all()[(size_t) i] == &current)
                                  return i;
                          return 0;
                      },
                      [this](int i)
                      {
                          settings_.setValue("theme", theme::all()[(size_t) i].name);
                          applyTheme();
                      },
                      "High Contrast is black, white and yellow, with a ring round whatever has keyboard focus."),
        prefs::choice("Accent",
                      [] { juce::StringArray names; for (const auto& a : theme::accents()) names.add(a.name); names.add("Custom"); return names; }(),
                      [this]
                      {
                          const auto saved = settings_.getValue("theme.accent");
                          if (saved.isEmpty())
                              return 0;
                          for (int i = 1; i < (int) theme::accents().size(); ++i)
                              if (theme::accents()[(size_t) i].colour.toString() == saved)
                                  return i;
                          return (int) theme::accents().size(); // Custom
                      },
                      [this](int i)
                      {
                          if (i >= (int) theme::accents().size())
                          {
                              if (preferencesDialog_ != nullptr)
                                  chooseCustomAccent(*preferencesDialog_);
                              return;
                          }
                          settings_.setValue("theme.accent", i == 0 ? juce::String() : theme::accents()[(size_t) i].colour.toString());
                          applyTheme();
                      },
                      "The colour of what's on or chosen: buttons, sliders, ticks."),
        prefs::toggle("Show keyboard focus",
                      [this] { return settings_.getBoolValue("theme.focusRings", false); },
                      [this](bool on)
                      {
                          settings_.setValue("theme.focusRings", on);
                          applyTheme();
                      },
                      "A ring round the control the keyboard is on, in every theme (High Contrast always has it)."),
    } });

    const auto folderSize = [](const juce::File& folder)
    {
        juce::int64 bytes = 0;
        if (folder.isDirectory())
            for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
                bytes += entry.getFileSize();
        return juce::File::descriptionOfSizeInBytes(bytes);
    };
    pages.push_back({ "Folders", {
        prefs::folder("Recordings", [this] { return recordingsDirectory(); },
                      [this](const juce::File& folder)
                      {
                          settings_.setValue("paths.recordings", folder.getFullPathName());
                          settings_.saveIfNeeded();
                          fileBrowser_.setRecordingsDirectory(recordingsDirectory());
                      },
                      "Where takes go until a project is saved; then they're kept beside it."),
        prefs::folder("Edits", [this] { return editsDirectory(); },
                      [this](const juce::File& folder)
                      {
                          settings_.setValue("paths.edits", folder.getFullPathName());
                          settings_.saveIfNeeded();
                      },
                      "Where edited audio goes until a project is saved."),
        prefs::folder("Templates", [this] { return templatesFolder(); }),
        prefs::folder("Settings", [this] { return settings_.getFile().getParentDirectory(); }),
        prefs::heading("Cache"),
        prefs::action("Spectral edits", "Clear",
                      [this]
                      {
                          for (const auto& entry : juce::RangedDirectoryIterator(spectralrender::cacheFolder(), false, "*",
                                                                                 juce::File::findFiles))
                              entry.getFile().deleteFile(); // one in use stays
                          syncEngineTracks(); // remakes what this project needs
                      },
                      [folderSize] { return folderSize(spectralrender::cacheFolder()); },
                      "Audio made with a clip's kept spectral edits applied. Remade when needed."),
        prefs::action("Plugins", "Plugin Manager...", [this] { showPluginManager(); }),
        prefs::heading("Transcription"),
        prefs::action("Model", "Choose...", [this] { chooseTranscriptionModel(); },
                      [this]
                      {
                          const juce::File model(settings_.getValue("transcribe.model"));
                          return model.existsAsFile() ? model.getFileName() : juce::String("None chosen");
                      },
                      "A whisper.cpp model file (ggml-base.en.bin, say): larger is slower and more accurate"),
        prefs::action("Models", "Get Models...", [] { juce::URL(engine::kModelsUrl).launchInDefaultBrowser(); }, {},
                      "Open the page whisper.cpp's models come from, in your browser"),
        prefs::choice("Language", { "Detect it", "English", "Spanish", "French", "German", "Italian", "Portuguese", "Dutch",
                                    "Japanese", "Chinese" },
                      [this]
                      {
                          static const juce::StringArray codes { "auto", "en", "es", "fr", "de", "it", "pt", "nl", "ja", "zh" };
                          return juce::jmax(0, codes.indexOf(settings_.getValue("transcribe.language", "auto")));
                      },
                      [this](int i)
                      {
                          static const juce::StringArray codes { "auto", "en", "es", "fr", "de", "it", "pt", "nl", "ja", "zh" };
                          settings_.setValue("transcribe.language", codes[i]);
                          settings_.saveIfNeeded();
                      },
                      "What's spoken. A \".en\" model is English only."),
    } });

    pages.push_back({ "Keyboard", {
        prefs::action("Shortcuts", "Edit Shortcuts...", [this] { showKeyboardShortcuts(); }, {},
                      "Change the key for any command, and import or export a set."),
        prefs::action("Find a command", "Command Palette", [this] { showCommandPalette(); }),
    } });

    return pages;
}

// ---- Themes -----------------------------------------------------------------

/** Puts the saved theme and accent on the app's look and feel, and has every
    window take them up. Nothing to do where the look and feel isn't the
    app's (a headless render, a test). */
void MainComponent::applyTheme()
{
    auto* appLook = dynamic_cast<AppLookAndFeel*>(&juce::LookAndFeel::getDefaultLookAndFeel());
    if (appLook == nullptr)
        return;

    const auto accentText = settings_.getValue("theme.accent");
    appLook->apply(theme::named(settings_.getValue("theme", "Dark")),
                       accentText.isEmpty() ? juce::Colours::transparentBlack : juce::Colour::fromString(accentText),
                       settings_.getBoolValue("theme.focusRings", false));
    settings_.saveIfNeeded();

    const auto background = appLook->findColour(juce::ResizableWindow::backgroundColourId);
    auto&      desktop    = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        auto* window = desktop.getComponent(i);
        if (auto* resizable = dynamic_cast<juce::ResizableWindow*>(window))
            resizable->setBackgroundColour(background);
        window->sendLookAndFeelChange();
        window->repaint();
    }
}

/** Any accent at all, from a colour picker beside the Preferences window. */
void MainComponent::chooseCustomAccent(juce::Component& near)
{
    struct Picker final : juce::Component, juce::ChangeListener
    {
        juce::ColourSelector                  selector { juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders };
        std::function<void(juce::Colour)>     onChange;

        Picker()
        {
            selector.addChangeListener(this);
            addAndMakeVisible(selector);
            setSize(300, 280);
        }
        ~Picker() override { selector.removeChangeListener(this); }
        void resized() override { selector.setBounds(getLocalBounds()); }
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (onChange) onChange(selector.getCurrentColour()); }
    };

    auto picker = std::make_unique<Picker>();
    const auto saved = settings_.getValue("theme.accent");
    picker->selector.setCurrentColour(saved.isEmpty() ? theme::named(settings_.getValue("theme", "Dark")).accent
                                                      : juce::Colour::fromString(saved),
                                      juce::dontSendNotification);
    picker->onChange = [safe = juce::Component::SafePointer<MainComponent>(this)](juce::Colour colour)
    {
        if (safe == nullptr)
            return;
        safe->settings_.setValue("theme.accent", colour.withAlpha(1.0f).toString());
        safe->applyTheme();
    };
    juce::CallOutBox::launchAsynchronously(std::move(picker), near.getScreenBounds().removeFromRight(40), nullptr);
}

// ---- Screensets --------------------------------------------------------------

/** Puts a saved arrangement of the panes back, in place of this one. */
void MainComponent::applyScreenset(int index)
{
    if (index < 0 || index >= (int) screensets_.size())
        return;
    const auto& set = screensets_[(size_t) index];
    if (! workspace_.restoreLayout(set.layout))
    {
        showError("\"" + set.name + "\" names panes this version doesn't have");
        return;
    }
    saveDockLayout(); // it's what's on screen now: what the next launch opens with
    showStatus("Layout \"" + set.name + "\"");
}

void MainComponent::promptSaveScreenset()
{

    dialog("Save Current Layout", "View > Layout puts it back.")
        .text("name", "Name:", "My Layout " + juce::String((int) screensets_.size() + 1))
        .unsaved()
        .show("Save", [this](const FormDialog::Values& v)
        {
            const auto name = v.text("name");
            if (name.isEmpty())
                return;
            screensets_ = screensets::with(screensets_, { name, workspace_.saveLayout() });
            settings_.setValue("screensets", screensets::serialize(screensets_));
            settings_.saveIfNeeded();
            showStatus("Saved layout \"" + name + "\" in View > Layout");
        });
}

void MainComponent::removeScreenset(int index)
{
    if (index < 0 || index >= (int) screensets_.size())
        return;
    const auto name = screensets_[(size_t) index].name;
    screensets_.erase(screensets_.begin() + index);
    settings_.setValue("screensets", screensets::serialize(screensets_));
    settings_.saveIfNeeded();
    showStatus("Removed layout \"" + name + "\"");
}

} // namespace soundsplice
