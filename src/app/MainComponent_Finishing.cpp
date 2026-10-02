#include "MainComponentInternal.h"

#include "engine/Diagnostics.h"
#include "model/EssentialSound.h"
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
void MainComponent::startBatchProcess()
{
    chooser_ = std::make_unique<juce::FileChooser>("Batch Process: choose a folder of audio files",
                                                   juce::File(settings_.getValue("batch.inputFolder")));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
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

    auto* window = new juce::AlertWindow("Batch Process: Output",
                                         juce::String((int) inputs.size()) + " files, "
                                             + juce::String((int) chain.size()) + (chain.size() == 1 ? " effect" : " effects")
                                             + ". New files are written; the originals aren't touched.",
                                         juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray loudnessNames, formatNames;
    for (const auto& l : kLoudness) loudnessNames.add(l.name);
    for (const auto& f : kFormats)  formatNames.add(f.name);
    window->addComboBox("loudness", loudnessNames, "Loudness:");
    window->getComboBoxComponent("loudness")->setSelectedItemIndex(settings_.getIntValue("batch.loudness", 0));
    window->addComboBox("format", formatNames, "Format:");
    window->getComboBoxComponent("format")->setSelectedItemIndex(settings_.getIntValue("batch.format", 0));
    window->addButton("Choose Output Folder...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, inputs, chain](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int loudness = juce::jlimit(0, (int) std::size(kLoudness) - 1, window->getComboBoxComponent("loudness")->getSelectedItemIndex());
            const int format   = juce::jlimit(0, (int) std::size(kFormats) - 1, window->getComboBoxComponent("format")->getSelectedItemIndex());
            self->settings_.setValue("batch.loudness", loudness);
            self->settings_.setValue("batch.format", format);

            batch::Settings settings;
            settings.chain                = chain;
            settings.loudnessLufs         = kLoudness[loudness].lufs;
            settings.output.format        = kFormats[format].format;
            settings.output.bitsPerSample = kFormats[format].bits;
            settings.bpm                  = self->history_.current().bpm;

            self->chooser_ = std::make_unique<juce::FileChooser>("Batch Process: where to write the new files",
                                                                 juce::File(self->settings_.getValue("batch.outputFolder")));
            self->chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [self, inputs, settings](const juce::FileChooser& fc) mutable
                {
                    if (self == nullptr || fc.getResult() == juce::File {})
                        return;
                    settings.folder = fc.getResult();
                    self->settings_.setValue("batch.outputFolder", settings.folder.getFullPathName());
                    self->runBatch(inputs, settings);
                });
        }));
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

    const auto& favorite = favorites_[(size_t) index];
    const auto& tracks   = history_.current().tracks;
    const bool  onTime   = ! timeSelection_.isEmpty()
                        && std::any_of(tracks.begin(), tracks.end(), [this](const model::Track& t)
                                       { return t.type == model::TrackType::Audio && timeSelection_.includes(t.id); });
    if (onTime)
        applyEffectsToTimeSelection(favorite.chain);
    else if (selectedAudioClip() != nullptr && ! audioEditor_.selection().isEmpty())
        applyEffectsToSelection(favorite.chain);
    else
    {
        showError("Select part of a clip in the audio editor, or time across audio tracks, to apply \""
                  + juce::String(favorite.name) + "\" to");
        return;
    }
    showStatus("Applied favorite \"" + juce::String(favorite.name) + "\"");
}

/** Asks for a name and keeps @p chain as a favorite under it. */
void MainComponent::promptSaveFavorite(std::vector<model::EffectSlot> chain)
{
    if (chain.empty())
    {
        showError("Add an effect first - a favorite is a chain of them");
        return;
    }

    auto* window = new juce::AlertWindow("Save as Favorite", "The Favorites menu applies it to the selection in one click.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", {}, "Name:");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, chain](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;
            const auto name = window->getTextEditorContents("name").trim();
            if (name.isEmpty())
            {
                self->showError("A favorite needs a name");
                return;
            }
            self->favorites_ = model::withFavorite(self->favorites_, { name.toStdString(), chain });
            self->settings_.setValue("favorites", juce::String(model::serializeFavorites(self->favorites_)));
            self->settings_.saveIfNeeded();
            self->showStatus("Saved \"" + name + "\" in the Favorites menu");
        }));
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

} // namespace soundsplice
