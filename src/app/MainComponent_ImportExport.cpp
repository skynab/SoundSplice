#include "engine/ExportLoudness.h"
#include "engine/CdImage.h"
#include "engine/DeliverySpec.h"
#include "engine/Transcriber.h"
#include "model/TextEdit.h"
#include "ExportNaming.h"
#include "RenderReport.h"
#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Getting audio and MIDI in and out: preview, import, and export of mixes and stems.

namespace soundsplice
{
void MainComponent::chooseFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load an audio file", juce::File{},
                                                   audiofiles::wildcards());

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            previewAudioFile(file);
    });
}

/** Loads a file into the global preview player (not tied to any track) — used
    by the "Preview Audio File..." menu item and by double-clicking a file in
    the file-browser pane.

    Named "Preview" rather than "Import" because that is what it does: the
    file plays once and never becomes a clip. While both menu items said
    "Import Audio", picking this one looked like importing and produced a
    project with nothing in it. */
void MainComponent::previewAudioFile(const juce::File& file)
{
    if (engine_.loadAudioFile(file))
        clipLabel.setText(engine_.loadedClipName()
                              + juce::String::formatted("   (%.2f s)", engine_.loadedClipSeconds()),
                          juce::dontSendNotification);
    else
        showError("Could not load: " + file.getFileName());
}

void MainComponent::importMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import MIDI file", juce::File{}, "*.mid;*.midi");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        engine::MidiImportResult result;
        history_.edit("Import MIDI", [&file, &result](model::Song& s)
        {
            result = engine::importMidiFile(file, s);
        });

        if (! result.ok)
        {
            showError("Could not import: " + file.getFileName());
            return;
        }

        syncEngineTracks();
        arrangementView_.setSong(history_.current());
        updateMixerStrips();
        updateEditingLabel();

        auto msg = "Imported " + juce::String(result.tracksImported) + " track(s) at "
                 + juce::String(history_.current().bpm, 1) + " BPM";
        if (result.extraTempoEventsIgnored > 0)
            msg += " (" + juce::String(result.extraTempoEventsIgnored) + " further tempo change(s) not imported)";
        showStatus(msg);
    });
}

void MainComponent::importRawDataDialog()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Import raw data", juce::File{}, "*");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [self = juce::Component::SafePointer<MainComponent>(this)](const juce::FileChooser& fc)
    {
        const auto source = fc.getResult();
        if (self == nullptr || source == juce::File{})
            return;

        app::ImportRawDialog::show(self, source, [self, source](engine::RawPcmFormat format)
        {
            if (self != nullptr)
                self->importRawData(source, format);
        });
    });
}

/** Converts @p source, read as @p format, to a 32-bit float WAV beside the
    project's other audio and imports that. A block at a time on the render
    thread, so a file of any length converts without being held in memory or
    freezing the app. Nothing about the engine is touched until the import. */
void MainComponent::importRawData(const juce::File& source, const engine::RawPcmFormat& format)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    const auto frames = format.framesIn((std::uint64_t) juce::jmax((juce::int64) 0, source.getSize()));
    if (frames == 0)
    {
        showError("No whole samples in " + source.getFileName() + " after the bytes skipped");
        return;
    }

    const auto destination = audioDirectoryFor(recordingsDirectory())
                                 .getNonexistentChildFile(source.getFileNameWithoutExtension(), ".wav");
    auto       written     = std::make_shared<bool>(false);

    auto work = [source, format, frames, destination, written](app::OfflineRenderJob& job)
    {
        juce::FileInputStream input(source);
        if (! input.openedOk() || ! input.setPosition((juce::int64) format.headerBytes))
            return;

        destination.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> output(destination.createOutputStream());
        if (output == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(output, // consumed on success
            juce::AudioFormatWriterOptions{}
                .withSampleRate(format.sampleRate)
                .withNumChannels(format.channels)
                .withBitsPerSample(32)
                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
            return;

        constexpr std::uint64_t kBlockFrames = 1 << 16;
        std::vector<std::uint8_t>       bytes;
        std::vector<std::vector<float>> channels;
        std::vector<const float*>       pointers;

        for (std::uint64_t done = 0; done < frames;)
        {
            if (job.shouldAbort())
                return;

            const auto block = (std::size_t) std::min(kBlockFrames, frames - done);
            bytes.resize(block * (std::size_t) format.frameBytes());
            if (input.read(bytes.data(), (int) bytes.size()) != (int) bytes.size())
                return;

            engine::rawpcm::decodeFrames(bytes.data(), block, format, channels);
            pointers.clear();
            for (const auto& channel : channels)
                pointers.push_back(channel.data());

            if (! writer->writeFromFloatArrays(pointers.data(), (int) pointers.size(), (int) block))
                return;

            done += block;
            job.report((double) done / (double) frames, "Converting " + source.getFileName());
        }

        writer.reset(); // finishes the header
        *written = true;
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), written, destination,
                       source](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->renderJob_.reset();

        if (cancelled || ! *written)
        {
            destination.deleteFile();
            if (cancelled)
                self->showStatus("Import cancelled");
            else
                self->showError("Could not convert " + source.getFileName());
            return;
        }

        self->importAudioFileAtBeat(destination, 0.0);
    };

    renderJob_ = app::OfflineRenderJob::launch("Import Raw Data", std::move(work), std::move(onFinished));
}

void MainComponent::exportMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI file", juce::File{}, "*.mid");
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;
        file = file.withFileExtension("mid");

        const bool ok = engine::exportMidiFile(file, history_.current());
        if (ok)
            showStatus("Exported: " + file.getFileName());
        else
            showError("MIDI export failed (no instrument track has any notes)");
    });
}

/** Imports an audio file onto a brand-new Audio track (as its one clip, at
    beat 0), so it actually plays back as part of the mix — unlike "Import
    Audio..." above, which only feeds the disconnected global preview player. */
void MainComponent::importAudioToNewTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Import audio to a new track", juce::File{},
                                                   audiofiles::wildcards());
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            importAudioFileAtBeat(file, 0.0);
    });
}

/** Imports a file as an audio clip starting at @p startBeats — the shared
    machinery behind "Import Audio to Track..." (always beat 0, always a new
    track), and dragging a file from the file-browser pane onto the
    arrangement (beat = wherever it was dropped; @p targetTrackIndex = the
    track lane it landed on, or -1 for empty space below the tracks).

    Dropping onto an existing Audio-type track adds a clip there instead of
    creating a new track — the track keeps its single-clip unbounded window
    if it still only has one clip, or gets real per-clip length gating (see
    AudioFilePlayerNode) the moment it has more than one, exactly like
    instrument clips. Any other drop target (empty space, or a non-Audio
    track) creates a brand-new Audio track instead, as it always has. */
void MainComponent::importAudioFileAtBeat(const juce::File& file, double startBeats,
                                          int targetTrackIndex)
{
    const auto& song = history_.current();
    const bool  addToExistingTrack = targetTrackIndex >= 0 && targetTrackIndex < (int) song.tracks.size()
                                   && song.tracks[(size_t) targetTrackIndex].type == model::TrackType::Audio;

    // Size the clip to the file's real duration rather than a fixed guess —
    // display-only for a track's sole clip (unbounded window regardless), but
    // functionally gates playback the moment a track has more than one clip,
    // so guessing wrong there would audibly truncate the clip.
    const double durationSeconds = engine_.probeDurationSeconds(file);
    if (durationSeconds <= 0.0)
    {
        // The file couldn't be decoded at all — corrupt, truncated, or an
        // unsupported format. Falling through to a fabricated 4-beat clip
        // pointing at a file the engine can't play would create a track (or
        // clip) that just sits there silent with nothing to say why.
        showError("Could not import: " + file.getFileName());
        return;
    }
    const double measured        = model::clockFor(song).beatsAfter(juce::jmax(0.0, startBeats), durationSeconds);
    const double lengthBeats     = measured > 0.0 ? measured : 4.0;
    const auto   path            = file.getFullPathName().toStdString();

    if (addToExistingTrack)
    {
        int newClipIndex = -1;
        history_.edit("Add audio clip", [targetTrackIndex, &path, startBeats, lengthBeats,
                                         &newClipIndex](model::Song& s)
        {
            auto& track = s.tracks[(size_t) targetTrackIndex];

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = juce::jmax(0.0, startBeats);
            clip.lengthBeats = lengthBeats;
            clip.audioFile   = path;
            track.clips.push_back(clip);

            newClipIndex = (int) track.clips.size() - 1;
        });

        syncEngineTracks();
        selectTrackAndClip(targetTrackIndex, newClipIndex);
        arrangementView_.setSong(history_.current());
        showStatus("Imported: " + file.getFileName() + "  (added clip)");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    int newTrackIndex = -1;
    history_.edit("Import audio track", [&path, &newTrackIndex, startBeats, lengthBeats](model::Song& s)
    {
        const auto name = "Audio " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = juce::jmax(0.0, startBeats);
        clip.lengthBeats = lengthBeats;
        clip.audioFile   = path;
        s.tracks.back().clips.push_back(clip);

        newTrackIndex = (int) s.tracks.size() - 1;
    });

    selectTrackAndRefreshAll(newTrackIndex);
    showStatus("Imported: " + file.getFileName() + "  (new track)");
}

/** True if every sample in @p file is exactly zero.

    Reads the written file rather than a buffer held in memory, because with
    recording streamed to disk there is no such buffer any more — and reading
    back what was actually written is the stronger check anyway. */
bool MainComponent::isSilentAudioFile(const juce::File& file)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(manager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false; // unreadable is a different problem, and not this one to report

    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    std::vector<juce::Range<float>> levels((size_t) numChannels);
    reader->readMaxLevels(0, reader->lengthInSamples, levels.data(), numChannels);

    for (const auto& range : levels)
        if (range.getStart() != 0.0f || range.getEnd() != 0.0f)
            return false;

    return true;
}

namespace
{
    /** Loudness-normalize on export, for one rendered file. A mix is brought
        to the target, limited as it must be; its stems after it get the
        same gain without the limiter, so they still sum to it. A stem with
        no mix before it (stems only) is brought to the target itself. */
    engine::ExportLoudnessResult applyExportLoudness(bool isStem, const engine::ExportOptions& options,
                                                     juce::AudioBuffer<float>& buffer, std::optional<double>& mixGainDb)
    {
        if (options.loudnessLufs >= 0.0)
            return {};
        if (isStem && mixGainDb)
        {
            buffer.applyGain(juce::Decibels::decibelsToGain((float) *mixGainDb));
            return {};
        }
        const auto result = engine::normalizeForExport(buffer, options.sampleRate, options.loudnessLufs,
                                                       options.truePeakCeilingDb);
        if (! isStem && result.measured)
            mixGainDb = result.gainDb;
        return result;
    }
}

/** Asks what kind of file to write, then where to put it, then writes it.

    Two dialogs in sequence rather than one: the format decides the file
    extension, so the save dialog can't filter correctly until the format is
    known. Asking for the location first would mean either an unfiltered
    chooser or one that lies about what it is about to write.

    This replaced a WAV-only "Bounce" that hardcoded both the extension and
    24-bit depth. */
void MainComponent::exportAudioDialog(std::optional<app::ExportChoice> initial)
{
    const double deviceRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;

    int markerRanges = 0;
    for (const auto& marker : history_.current().markers)
        if (marker.lengthBeats > 0.0)
            ++markerRanges;

    std::vector<app::NamedExportChoice> presets;
    for (const auto& preset : renderPresets_)
        presets.push_back({ preset.name, preset.choice });

    app::ExportAudioDialog::show(this, deviceRate, ! timeSelection_.isEmpty(), markerRanges,
                                 (int) history_.current().markers.size(), presets,
                                 initial ? &*initial : nullptr,
        [self = juce::Component::SafePointer<MainComponent>(this)](app::ExportChoice choice, app::ExportAction action)
        {
            if (self == nullptr)
                return;
            switch (action)
            {
                case app::ExportAction::Export:     self->exportProject(choice); break;
                case app::ExportAction::Queue:      self->queueExport(choice); break;
                case app::ExportAction::SavePreset: self->promptSaveRenderPreset(choice); break;
            }
        });
}

/** One file an export will write: what to render, and how to write it. */
struct MainComponent::ExportTask
{
    juce::File                                   file;
    engine::AudioEngine::OfflineRenderOptions    render;
    engine::ExportOptions                        write;
    juce::String                                 label; // shown in the progress window

    // A render report beside the file (app/RenderReport.h), and what it lists.
    bool                                         writeReport = false;
    std::vector<app::renderreport::ClipLine>     clips;
    juce::String                                 project;
};

/** What rendering and writing one task came to. */
struct MainComponent::TaskOutcome
{
    bool                         rendered = false; // false: cancelled, or nothing to render
    bool                         written  = false;
    engine::ExportLoudnessResult loudness;
};

/** Every file @p choice asks for, with @p chosenFile as the one named. */
std::vector<MainComponent::ExportTask> MainComponent::tasksForChoice(const juce::File& chosenFile, const app::ExportChoice& choice,
                                                                     bool& folderFailed)
{
    // The tags for the whole project; buildExportTasks gives each file its
    // own chapters, for the stretch of time it covers.
    auto options = choice.options;
    options.tags = exportTagsFor(0.0, -1.0, choice.tagging);

    auto tasks = buildRangeExportTasks(chosenFile, options, choice.range, choice.namePattern, folderFailed,
                                       choice.selectionStartBeats, choice.selectionLengthBeats);
    if (choice.report)
        addReportDetails(tasks);
    return tasks;
}

/** What each file's report lists: the clips in its stretch of time, on its
    track for a stem. Gathered on the message thread, where the document is. */
void MainComponent::addReportDetails(std::vector<ExportTask>& tasks) const
{
    const auto& song  = history_.current();
    const auto  clock = model::clockFor(song);
    const auto  name  = projectFile_ != juce::File() ? projectFile_.getFileNameWithoutExtension() : juce::String("Untitled");
    for (auto& task : tasks)
    {
        task.writeReport = true;
        task.project     = name;
        const double from = task.render.startBeats, to = from + task.render.lengthBeats;
        for (int t = 0; t < (int) song.tracks.size(); ++t)
        {
            if (task.render.soloTrack >= 0 && t != task.render.soloTrack)
                continue;
            for (const auto& clip : song.tracks[(size_t) t].clips)
            {
                if (clip.startBeats + clip.lengthBeats <= from || clip.startBeats >= to)
                    continue;
                app::renderreport::ClipLine line;
                line.track         = juce::String(song.tracks[(size_t) t].name);
                line.file          = clip.type == model::ClipType::Audio ? juce::File(clip.audioFile).getFileName()
                                                                         : juce::String("(notes)");
                line.startSeconds  = clock.secondsBetween(from, clip.startBeats);
                line.lengthSeconds = clock.secondsBetween(clip.startBeats, clip.startBeats + clip.lengthBeats);
                task.clips.push_back(line);
            }
        }
        std::stable_sort(task.clips.begin(), task.clips.end(),
                         [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    }
}

void MainComponent::exportProject(const app::ExportChoice& choice)
{
    if (renderJob_ != nullptr)
    {
        showError("An export is already running");
        return;
    }

    const auto extension = engine::extensionFor(choice.options.format);

    // One file per marker range: what's chosen is where they go, and its
    // name stands in for $project; each file is named from the pattern.
    const bool many = choice.range == app::ExportRange::MarkerRanges || choice.range == app::ExportRange::BetweenMarkers;
    chooser_ = std::make_unique<juce::FileChooser>(many ? "Export " + extension.toUpperCase() + " - a file per range, in this folder"
                                                       : "Export " + extension.toUpperCase(),
                                                   juce::File{}, "*." + extension);
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, choice, extension](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        file = file.withFileExtension(extension);

        bool       folderFailed = false;
        const auto tasks        = tasksForChoice(file, choice, folderFailed);

        if (folderFailed)
        {
            showError("Export failed (could not create the stems folder)");
            return;
        }

        if (tasks.empty())
        {
            showError("Nothing to export - every track is muted or silenced by a solo");
            return;
        }

        startExport(tasks, file);
    });
}

/** Renders @p task and writes it, with its loudness and report. */
MainComponent::TaskOutcome MainComponent::renderAndWrite(const ExportTask& task, std::optional<double>& mixGainDb)
{
    TaskOutcome outcome;
    auto        buffer = engine_.renderOffline(task.render);

    // Empty means cancelled, or nothing to render. Either way there is no
    // file worth writing - see OfflineRenderOptions::onProgress for why a
    // cancelled render deliberately returns nothing rather than a partial
    // buffer.
    if (buffer.getNumSamples() == 0)
        return outcome;
    outcome.rendered = true;

    if (task.render.soloTrack < 0)
        mixGainDb.reset(); // a new range: its own mix sets its stems' gain
    outcome.loudness = applyExportLoudness(task.render.soloTrack >= 0, task.write, buffer, mixGainDb);

    if (! engine::writeAudioFile(task.file, buffer, task.write))
        return outcome;
    outcome.written = true;

    if (task.writeReport)
    {
        auto report     = app::renderreport::analyse(buffer, task.write.sampleRate);
        report.fileName = task.file.getFileName();
        report.format   = engine::displayNameFor(task.write.format);
        report.project  = task.project;
        report.clips    = task.clips;
        app::renderreport::fileFor(task.file).replaceWithText(app::renderreport::html(report));
    }
    return outcome;
}

// ---- Render presets and the render queue --------------------------------------

void MainComponent::promptSaveRenderPreset(const app::ExportChoice& choice)
{
    auto* window = new juce::AlertWindow("Save Render Preset", "Export Audio's Preset box puts these choices back.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", engine::displayNameFor(choice.options.format).upToFirstOccurrenceOf(" ", false, false)
                                      + (choice.options.loudnessLufs < 0.0 ? " " + juce::String((int) choice.options.loudnessLufs) + " LUFS"
                                                                           : juce::String()),
                          "Name:");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, choice](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr)
                return;
            const auto name = window->getTextEditorContents("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                self->renderPresets_ = app::exportchoices::withPreset(self->renderPresets_, { name, choice });
                self->settings_.setValue("renderPresets", app::exportchoices::serializePresets(self->renderPresets_));
                self->settings_.saveIfNeeded();
                self->showStatus("Saved render preset \"" + name + "\"");
            }
            self->exportAudioDialog(choice); // back to the export, as it was
        }));
}

/** Add to Render Queue: a snapshot of the project as it is now, rendered
    later from File > Render Queue. */
void MainComponent::queueExport(app::ExportChoice choice)
{
    const auto extension = engine::extensionFor(choice.options.format);
    chooser_ = std::make_unique<juce::FileChooser>("Queue " + extension.toUpperCase() + " export: where it goes",
                                                   juce::File{}, "*." + extension);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, choice, extension](const juce::FileChooser& fc) mutable
    {
        if (fc.getResult() == juce::File{})
            return;
        const auto output = fc.getResult().withFileExtension(extension);

        // The time selection as it is now: the queue renders later.
        if (choice.range == app::ExportRange::TimeSelection && ! timeSelection_.isEmpty())
        {
            choice.selectionStartBeats  = timeSelection_.startBeats;
            choice.selectionLengthBeats = timeSelection_.lengthBeats();
        }

        const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                .getChildFile("SoundSplice").getChildFile("Render Queue");
        folder.createDirectory();
        const auto snapshot = folder.getNonexistentChildFile(projectFile_ != juce::File() ? projectFile_.getFileNameWithoutExtension()
                                                                                         : juce::String("Untitled"),
                                                             ".soundsplice");
        if (! snapshot.replaceWithText(juce::String::fromUTF8(model::serialize(history_.current()).c_str())))
        {
            showError("Could not save the project for the queue");
            return;
        }

        const auto project = projectFile_ != juce::File() ? projectFile_.getFileNameWithoutExtension() : juce::String("Untitled");
        renderQueue_.push_back({ snapshot, output, choice, output.getFileName() + "  (" + project + ")" });
        saveRenderQueue();
        showStatus("Queued " + output.getFileName() + " - File > Render Queue renders it");
    });
}

void MainComponent::saveRenderQueue()
{
    settings_.setValue("renderQueue", app::exportchoices::serializeQueue(renderQueue_));
    settings_.saveIfNeeded();
    if (renderQueueDialog_ != nullptr)
        renderQueueDialog_->updateJobs(renderQueue_);
}

/** File > Render Queue. Each job runs in a child process: this app, headless
    (`--render-job`), so nothing here waits on it. */
void MainComponent::showRenderQueue()
{
    if (renderQueueDialog_ != nullptr)
    {
        if (auto* window = renderQueueDialog_->findParentComponentOfClass<juce::DialogWindow>())
            window->toFront(true);
        return;
    }

    auto runner = [](const app::exportchoices::Job& job, juce::String& report, std::function<bool()> shouldStop)
    {
        const juce::TemporaryFile jobFile(".xml"), reportFile(".txt");
        if (! jobFile.getFile().replaceWithText(app::exportchoices::serializeJob(job)))
        {
            report = "couldn't write the job";
            return false;
        }
        juce::ChildProcess child;
        if (! child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                                              "--render-job", jobFile.getFile().getFullPathName(),
                                              reportFile.getFile().getFullPathName() },
                          0))
        {
            report = "couldn't start the render";
            return false;
        }
        while (child.isRunning())
        {
            if (shouldStop())
            {
                child.kill();
                report = "stopped";
                return false;
            }
            child.waitForProcessToFinish(200);
        }
        report = reportFile.getFile().loadFileAsString();
        return child.getExitCode() == 0;
    };

    auto dialog = std::make_unique<RenderQueueDialog>(runner);
    dialog->setJobs(renderQueue_);
    dialog->onRemove = [this](int index)
    {
        if (index < 0 || index >= (int) renderQueue_.size())
            return;
        renderQueue_[(size_t) index].project.deleteFile();
        renderQueue_.erase(renderQueue_.begin() + index);
        saveRenderQueue();
    };
    dialog->onRunFinished = [this](const std::vector<int>& rendered)
    {
        // Rendered ones are done with, snapshot and all.
        for (auto it = rendered.rbegin(); it != rendered.rend(); ++it)
            if (*it >= 0 && *it < (int) renderQueue_.size())
            {
                renderQueue_[(size_t) *it].project.deleteFile();
                renderQueue_.erase(renderQueue_.begin() + *it);
            }
        settings_.setValue("renderQueue", app::exportchoices::serializeQueue(renderQueue_));
        settings_.saveIfNeeded();
        showStatus("Render queue: " + juce::String((int) rendered.size()) + " rendered");
    };
    renderQueueDialog_ = dialog.get();

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Render Queue";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/**
    Every file the export will write, decided up front on the message thread.

    Built as a list rather than worked out as it goes because the rendering
    happens on a background thread, and that thread must not reach into the
    document or the UI. Everything it needs — files, render options, track
    names — is captured by value here.
*/
std::vector<MainComponent::ExportTask>
MainComponent::buildExportTasks(const juce::File& masterFile,
                                const engine::ExportOptions& options,
                                bool& folderFailed,
                                double startBeats,
                                double lengthBeats)
{
    std::vector<ExportTask> tasks;

    // The whole project: a tail past the last clip so reverb and delay decay
    // into the file rather than being cut off mid-ring at the final beat.
    // Shared by the mix and every stem, so they all come out the same length
    // and line up when dropped into another session.
    if (lengthBeats < 0.0)
        lengthBeats = songEndBeats() + kBounceTailBeats;

    // This file's chapters: the markers inside the stretch it covers, timed
    // from its start. Only when the export asked for chapters at all.
    auto ranged = options;
    if (! options.tags.chapters.empty())
        ranged.tags.chapters = exportTagsFor(startBeats, lengthBeats, app::ExportTagging::InfoAndChapters).chapters;

    if (engine::writesMasterMix(options.contents))
    {
        ExportTask task;
        task.file  = masterFile;
        task.write = ranged;
        task.label = "master mix";
        // Everything the mix contains, rendered by the mixer itself — see
        // AudioEngine::renderOffline, and the comment there for why an export
        // calling the mixer rather than copying it is the whole design.
        task.render.startBeats  = startBeats;
        task.render.lengthBeats = lengthBeats;
        task.render.sampleRate  = options.sampleRate;
        tasks.push_back(std::move(task));
    }

    if (! engine::writesStems(options.contents))
        return tasks;

    const auto folder = app::stemFolderFor(masterFile);
    if (! folder.createDirectory())
    {
        folderFailed = true;
        return {};
    }

    const auto& song      = history_.current();
    const auto  extension = engine::extensionFor(options.format);
    const int   trackCount = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < trackCount; ++i)
    {
        // Which tracks get a stem is the engine's rule, not a second copy of
        // it here — see AudioEngine::trackContributesToMix.
        if (! engine_.trackContributesToMix(i))
            continue;

        ExportTask task;
        // Numbered by the track's position in the song, not by how many stems
        // have been written — so the file names line up with the tracks on
        // screen even when a muted track in the middle has been skipped.
        task.file  = folder.getChildFile(
            app::stemFileName(i + 1, song.tracks[(size_t) i].name, extension));
        task.write = ranged;
        task.write.tags.title = task.write.tags.title.isEmpty() ? juce::String(song.tracks[(size_t) i].name)
                                                                : task.write.tags.title + " - " + juce::String(song.tracks[(size_t) i].name);
        task.label = juce::String(song.tracks[(size_t) i].name);

        task.render.startBeats     = startBeats;
        task.render.lengthBeats    = lengthBeats;
        task.render.sampleRate     = options.sampleRate;
        task.render.soloTrack      = i;
        task.render.applyMasterBus = false; // stems are pre-master — see OfflineRenderOptions

        tasks.push_back(std::move(task));
    }

    return tasks;
}

/** The tasks for @p range: the project or the time selection into
    @p chosenFile, or one file per marker range beside it, named by
    @p namePattern (app/ExportNaming.h). */
std::vector<MainComponent::ExportTask>
MainComponent::buildRangeExportTasks(const juce::File& chosenFile, const engine::ExportOptions& options,
                                     app::ExportRange range, const juce::String& namePattern, bool& folderFailed,
                                     double selectionStartBeats, double selectionLengthBeats)
{
    // A queued export carries its own selection; otherwise, the one there is.
    if (range == app::ExportRange::TimeSelection && selectionLengthBeats > 0.0)
        return buildExportTasks(chosenFile, options, folderFailed, selectionStartBeats, selectionLengthBeats);
    if (range == app::ExportRange::TimeSelection && ! timeSelection_.isEmpty())
        return buildExportTasks(chosenFile, options, folderFailed, timeSelection_.startBeats, timeSelection_.lengthBeats());
    if (range != app::ExportRange::MarkerRanges && range != app::ExportRange::BetweenMarkers)
        return buildExportTasks(chosenFile, options, folderFailed);

    std::vector<model::Marker> regions;
    if (range == app::ExportRange::MarkerRanges)
    {
        for (const auto& marker : history_.current().markers)
            if (marker.lengthBeats > 0.0)
                regions.push_back(marker);
        std::stable_sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) { return a.startBeats < b.startBeats; });
    }
    else
    {
        // Split at every marker (Audacity's Export Multiple by labels): a
        // stretch from each to the next, the first from the start, the last
        // with the usual tail. Each named for the marker it starts at.
        auto markers = history_.current().markers;
        std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.startBeats < b.startBeats; });
        const double end = songEndBeats() + kBounceTailBeats;
        double from = 0.0;
        juce::String name = "Start";
        for (const auto& marker : markers)
        {
            if (marker.startBeats > from + 1.0e-6)
                regions.push_back({ 0, from, marker.startBeats - from, name.toStdString() });
            from = marker.startBeats;
            name = juce::String(marker.name);
        }
        if (end > from + 1.0e-6)
            regions.push_back({ 0, from, end - from, name.toStdString() });
    }

    const auto project = projectFile_ != juce::File() ? projectFile_.getFileNameWithoutExtension()
                                                      : chosenFile.getFileNameWithoutExtension();
    juce::StringArray names;
    for (int i = 0; i < (int) regions.size(); ++i)
        names.add(app::exportnaming::expand(namePattern,
                                            app::exportnaming::regionFields(project, juce::String(regions[(size_t) i].name),
                                                                            i, (int) regions.size())));
    names = app::exportnaming::distinct(names);

    std::vector<ExportTask> tasks;
    for (int i = 0; i < (int) regions.size(); ++i)
    {
        const auto file = chosenFile.getSiblingFile(names[i]).withFileExtension(chosenFile.getFileExtension());
        auto more = buildExportTasks(file, options, folderFailed, regions[(size_t) i].startBeats, regions[(size_t) i].lengthBeats);
        if (folderFailed)
            return {};
        for (auto& task : more)
        {
            if (task.render.soloTrack < 0)
                task.label = names[i];
            tasks.push_back(std::move(task));
        }
    }
    return tasks;
}


/** Runs @p tasks on a background thread behind a progress window. */
void MainComponent::startExport(const std::vector<ExportTask>& tasks, const juce::File& masterFile)
{
    struct Result
    {
        int  written  = 0;
        int  stems    = 0;
        bool anyFailure = false;
        engine::ExportLoudnessResult loudness; // the last mix's
    };

    auto result = std::make_shared<Result>();

    // The engine belongs to the render thread for the duration — see the guard
    // in timerCallback, and note that pump() frees retired audio objects.
    offlineRenderInProgress_ = true;

    auto work = [this, tasks, result](app::OfflineRenderJob& job)
    {
        const int count = (int) tasks.size();
        std::optional<double> mixGainDb;

        for (int i = 0; i < count; ++i)
        {
            if (job.shouldAbort())
                return;

            auto task = tasks[(size_t) i];
            const auto label = task.label;

            task.render.onProgress = [&job, i, count, label](double fraction)
            {
                job.report(app::overallProgress(i, count, fraction),
                           "Rendering " + label + "  (" + juce::String(i + 1)
                               + " of " + juce::String(count) + ")");
                return ! job.shouldAbort();
            };

            const auto outcome = renderAndWrite(task, mixGainDb);
            if (! outcome.rendered)
                continue;
            if (outcome.loudness.measured && task.render.soloTrack < 0)
                result->loudness = outcome.loudness;

            if (outcome.written)
            {
                ++result->written;
                if (task.render.soloTrack >= 0)
                    ++result->stems;
            }
            else
            {
                result->anyFailure = true;
            }
        }
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this),
                       result, masterFile](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->offlineRenderInProgress_ = false;
        self->renderJob_.reset();

        // Device changes were ignored while the render owned the engine, so
        // this catches up on any that happened — headphones plugged in
        // mid-export are still plugged in now.
        self->followSystemOutputIfEnabled();

        // Cancelling keeps whatever was already written rather than deleting
        // it: those files are complete, and throwing away finished work
        // because the *next* one was interrupted would be its own surprise.
        if (cancelled)
        {
            self->showError("Export cancelled - " + juce::String(result->written)
                            + " file(s) written");
            return;
        }

        if (result->anyFailure)
        {
            self->showError("Exported with errors - " + juce::String(result->written)
                            + " file(s) written, some failed");
            return;
        }

        if (result->written == 0)
        {
            self->showError("Nothing was exported");
            return;
        }

        // The stem count is said plainly because it is the only place the
        // mute/solo rule becomes visible: stems are the tracks that sound in
        // the mix, so exporting with solo left on legitimately writes one.
        // The loudness it reached, which a limited mix may fall a little short of.
        const auto loudness = result->loudness.measured
                                ? ", " + juce::String(result->loudness.integratedLufs, 1) + " LUFS, "
                                      + juce::String(result->loudness.truePeakDb, 1) + " dBTP"
                                : juce::String();
        if (result->stems > 0)
            self->showStatus("Exported: " + masterFile.getFileName() + " + "
                             + juce::String(result->stems) + " stem(s)" + loudness);
        else if (result->written > 1)
            self->showStatus("Exported " + juce::String(result->written) + " files to " + masterFile.getParentDirectory().getFileName() + loudness);
        else
            self->showStatus("Exported: " + masterFile.getFileName() + loudness);
    };

    renderJob_ = app::OfflineRenderJob::launch("Exporting audio", std::move(work),
                                               std::move(onFinished));
}

/** Mix and Render to New Track: the time selection's tracks (or the selected
    track) rendered over the time selection (or everything arranged) into one
    audio file, on a new track where the render started. The tracks rendered
    are left as they are.

    Rendered as stems are — each track on its own, through its own effects,
    gain and pan but before the master bus — then summed, so the result is
    what those tracks contribute to the mix. Tracks that don't sound in the
    mix (muted, or silenced by a solo) are left out, by the same rule. On the
    render thread behind a progress window, like an export. */
void MainComponent::mixAndRenderToNewTrack()
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    const auto& song     = history_.current();
    const auto  trackIds = arrangementEditTracks();

    std::vector<int> indices;
    const int count = juce::jmin((int) song.tracks.size(), engine_.maxTracks());
    for (int i = 0; i < count; ++i)
        if (std::find(trackIds.begin(), trackIds.end(), song.tracks[(size_t) i].id) != trackIds.end()
            && engine_.trackContributesToMix(i))
            indices.push_back(i);

    if (indices.empty())
    {
        showError("Nothing to render - the tracks are muted, or silenced by a solo");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const double rate = engine_.sampleRate();
    if (rate <= 0.0)
    {
        showError("Rendering needs an audio device - choose one in Audio Settings");
        return;
    }

    const double startBeats  = timeSelection_.isEmpty() ? 0.0 : timeSelection_.startBeats;
    const double lengthBeats = timeSelection_.isEmpty() ? songEndBeats() : timeSelection_.lengthBeats();
    if (lengthBeats <= 0.0)
    {
        showError("Nothing arranged to render");
        return;
    }

    const auto file    = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Mix", ".wav");
    auto       written = std::make_shared<bool>(false);

    // The engine belongs to the render thread for the duration, as for an export.
    offlineRenderInProgress_ = true;

    auto work = [this, indices, startBeats, lengthBeats, rate, file, written](app::OfflineRenderJob& job)
    {
        juce::AudioBuffer<float> mix;
        const int                total = (int) indices.size();

        for (int n = 0; n < total; ++n)
        {
            if (job.shouldAbort())
                return;

            engine::AudioEngine::OfflineRenderOptions options;
            options.startBeats     = startBeats;
            options.lengthBeats    = lengthBeats;
            options.soloTrack      = indices[(size_t) n];
            options.applyMasterBus = false;
            options.onProgress     = [&job, n, total](double fraction)
            {
                job.report(app::overallProgress(n, total, fraction),
                           "Rendering track " + juce::String(n + 1) + " of " + juce::String(total));
                return ! job.shouldAbort();
            };

            const auto buffer = engine_.renderOffline(options);
            if (buffer.getNumSamples() == 0)
                return; // cancelled

            if (mix.getNumSamples() == 0)
            {
                mix.makeCopyOf(buffer);
                continue;
            }

            const int samples = juce::jmin(mix.getNumSamples(), buffer.getNumSamples());
            for (int ch = 0; ch < juce::jmin(mix.getNumChannels(), buffer.getNumChannels()); ++ch)
                mix.addFrom(ch, 0, buffer, ch, 0, samples);
        }

        *written = mix.getNumSamples() > 0 && engine::OfflineRenderer::writeWav(file, mix, rate);
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), written, file, startBeats,
                       lengthBeats, rendered = (int) indices.size()](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->offlineRenderInProgress_ = false;
        self->renderJob_.reset();
        self->followSystemOutputIfEnabled();

        if (cancelled || ! *written)
        {
            file.deleteFile();
            if (cancelled)
                self->showStatus("Mix and render cancelled");
            else
                self->showError("Could not write " + file.getFileName());
            return;
        }

        const auto path          = file.getFullPathName().toStdString();
        int        newTrackIndex = -1;

        self->history_.edit("Mix and render to new track", [&path, startBeats, lengthBeats, &newTrackIndex](model::Song& s)
        {
            model::addTrack(s, model::TrackType::Audio, "Mix");

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = startBeats;
            clip.lengthBeats = lengthBeats;
            clip.audioFile   = path;
            s.tracks.back().clips.push_back(clip);

            newTrackIndex = (int) s.tracks.size() - 1;
        });

        self->selectTrackAndRefreshAll(newTrackIndex);
        self->showStatus("Rendered " + juce::String(rendered) + (rendered == 1 ? " track" : " tracks")
                         + " to a new track");
    };

    renderJob_ = app::OfflineRenderJob::launch("Mix and Render", std::move(work), std::move(onFinished));
}

bool MainComponent::renderHeadless(const juce::File& project, const juce::File& out,
                                   const engine::ExportOptions& options, juce::String& report)
{
    app::ExportChoice choice;
    choice.options = options;
    return renderHeadless(project, out, choice, report);
}

/** A render queue job, or soundsplice-cli's render: opens @p project and
    writes what @p choice asks for, as Export Audio would, on this thread. */
bool MainComponent::renderHeadless(const juce::File& project, const juce::File& out, const app::ExportChoice& choice,
                                   juce::String& report)
{
    model::Song song;
    std::string error;
    if (! project.existsAsFile() || ! model::deserialize(project.loadFileAsString().toStdString(), song, &error))
    {
        report = "Could not open " + project.getFullPathName() + (error.empty() ? "" : ": " + juce::String(error));
        return false;
    }
    loadSongIntoEditor(app::media::withResolvedPaths(song, project));
    projectFile_  = project;
    savedStateId_ = history_.stateId();

    bool folderFailed = false;
    const auto tasks  = tasksForChoice(out, choice, folderFailed);
    if (folderFailed)
    {
        report = "Could not create the stems folder beside " + out.getFullPathName();
        return false;
    }
    if (tasks.empty())
    {
        report = "Nothing to render: every track is muted or silenced by a solo";
        return false;
    }

    bool ok = true;
    std::optional<double> mixGainDb;
    for (const auto& task : tasks)
    {
        const auto outcome = renderAndWrite(task, mixGainDb);
        if (outcome.loudness.measured)
            report << task.label << ": " << juce::String(outcome.loudness.integratedLufs, 1) << " LUFS, "
                   << juce::String(outcome.loudness.truePeakDb, 1) << " dBTP\n";
        if (! outcome.rendered)
        {
            report << "Nothing rendered for " << task.label << "\n";
            ok = false;
        }
        else if (outcome.written)
            report << "Wrote " << task.file.getFullPathName() << "\n";
        else
        {
            report << "Could not write " << task.file.getFullPathName() << "\n";
            ok = false;
        }
    }
    return ok;
}

/** The project's info as tags, and - for @p tagging with chapters - its
    markers inside [@p startBeats, + @p lengthBeats) as chapters timed from
    @p startBeats, each running to the next (the last to the end). A
    negative length is the whole project. */
engine::ExportTags MainComponent::exportTagsFor(double startBeats, double lengthBeats, app::ExportTagging tagging) const
{
    engine::ExportTags tags;
    if (tagging == app::ExportTagging::None)
        return tags;

    const auto& song = history_.current();
    const auto  text = [](const std::string& s) { return juce::String::fromUTF8(s.c_str()); };
    tags.title   = text(song.info.title);
    tags.artist  = text(song.info.artist);
    tags.album   = text(song.info.album);
    tags.year    = text(song.info.year);
    tags.genre   = text(song.info.genre);
    tags.comment = text(song.info.comment);
    tags.track   = text(song.info.track);
    if (! song.info.coverArt.empty())
        tags.coverArt = juce::File(text(song.info.coverArt));

    if (tagging != app::ExportTagging::InfoAndChapters)
        return tags;

    if (lengthBeats < 0.0)
        lengthBeats = songEndBeats() + kBounceTailBeats;
    const double endBeats = startBeats + lengthBeats;
    const auto   clock    = model::clockFor(song);

    auto markers = song.markers;
    std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.startBeats < b.startBeats; });
    for (const auto& marker : markers)
    {
        if (marker.startBeats < startBeats - 1.0e-9 || marker.startBeats >= endBeats)
            continue;
        engine::ExportChapter chapter;
        chapter.startSeconds = clock.secondsBetween(startBeats, marker.startBeats);
        chapter.title        = text(marker.name);
        if (! tags.chapters.empty())
            tags.chapters.back().endSeconds = chapter.startSeconds;
        tags.chapters.push_back(chapter);
    }
    if (! tags.chapters.empty())
        tags.chapters.back().endSeconds = clock.secondsBetween(startBeats, endBeats);
    return tags;
}

/** File > Project Info: the tags every export carries. */
void MainComponent::showProjectInfo()
{
    auto dialog = std::make_unique<ProjectInfoDialog>(history_.current().info);
    auto* raw   = dialog.get();
    raw->onSave = [this, raw](const model::ProjectInfo& info)
    {
        if (info != history_.current().info)
            history_.edit("Edit project info", [info](model::Song& s) { s.info = info; });
        updateWindowTitle();
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    raw->onCancel = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Project Info";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** Export CD Image: the mix as a BIN of CD audio (16-bit, 44.1 kHz, dithered)
    and a CUE sheet with a track at each marker (and CD-TEXT from Project
    Info), for any burner. Rendered behind a progress window like an export. */
void MainComponent::exportCdImage()
{
    if (renderJob_ != nullptr)
    {
        showError("An export is already running");
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Export CD Image (a .cue, with its .bin beside it)", juce::File{}, "*.cue");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc)
    {
        if (fc.getResult() == juce::File{})
            return;
        const auto cueFile = fc.getResult().withFileExtension("cue");
        const auto binFile = cueFile.withFileExtension("bin");

        const auto& song  = history_.current();
        const auto  clock = model::clockFor(song);
        std::vector<std::pair<double, std::string>> marks;
        for (const auto& marker : song.markers)
            marks.emplace_back(clock.secondsAt(marker.startBeats), marker.name);

        engine::AudioEngine::OfflineRenderOptions render;
        render.lengthBeats = songEndBeats() + kBounceTailBeats;
        render.sampleRate  = engine::cdimage::kSampleRate;

        auto cue = std::make_shared<std::string>();
        auto ok  = std::make_shared<bool>(false);
        auto trackCount = std::make_shared<int>(0);
        const auto title     = song.info.album.empty() ? song.info.title : song.info.album;
        const auto performer = song.info.artist;

        offlineRenderInProgress_ = true;
        auto work = [this, render, marks, cueFile, binFile, cue, ok, trackCount, title, performer](app::OfflineRenderJob& job) mutable
        {
            render.onProgress = [&job](double fraction)
            {
                job.report(fraction, "Rendering the CD image");
                return ! job.shouldAbort();
            };
            auto mix = engine_.renderOffline(render);
            if (mix.getNumSamples() == 0)
                return;

            // CD audio is 16-bit: dithered, as a 16-bit export would be.
            engine::TpdfDither left(16, 0x9E3779B9u), right(16, 0x2545F491u);
            for (int i = 0; i < mix.getNumSamples(); ++i)
            {
                mix.setSample(0, i, left.processSample(mix.getSample(0, i)));
                mix.setSample(1, i, right.processSample(mix.getSample(1, i)));
            }

            const auto tracks = engine::cdimage::tracksFor(marks, mix.getNumSamples() / (double) engine::cdimage::kSampleRate);
            const auto bin    = engine::cdimage::binFor(mix.getReadPointer(0), mix.getReadPointer(1), mix.getNumSamples());
            *cue        = engine::cdimage::cueSheet(binFile.getFileName().toStdString(), tracks, title, performer);
            *trackCount = (int) tracks.size();
            *ok = binFile.replaceWithData(bin.data(), bin.size())
               && cueFile.replaceWithText(juce::String::fromUTF8(cue->c_str()), false, false, nullptr);
        };

        auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), ok, trackCount, cueFile](bool cancelled)
        {
            if (self == nullptr)
                return;
            self->offlineRenderInProgress_ = false;
            self->renderJob_.reset();
            self->followSystemOutputIfEnabled();
            if (cancelled)
                self->showStatus("CD image cancelled");
            else if (! *ok)
                self->showError("Could not write " + cueFile.getFileName());
            else
                self->showStatus("Exported a CD image: " + cueFile.getFileName() + ", " + juce::String(*trackCount)
                                 + (*trackCount == 1 ? " track" : " tracks"));
        };

        renderJob_ = app::OfflineRenderJob::launch("Exporting CD image", std::move(work), std::move(onFinished));
    });
}

/** The Delivery pane's Check: the mix rendered (as an export would render
    it) and measured against the spec, behind a progress window. */
void MainComponent::runDeliveryCheck(int specIndex)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }
    const auto& specs = engine::delivery::all();
    if (specIndex < 0 || specIndex >= (int) specs.size())
        return;

    engine::AudioEngine::OfflineRenderOptions render;
    render.sampleRate  = 48000.0;
    // The tail added for reverbs to ring out isn't the mix's own silence: the
    // tail check is of the audio as it would be delivered, so the render
    // stops where the arrangement does.
    render.lengthBeats = juce::jmax(1.0, songEndBeats());

    auto results = std::make_shared<std::vector<engine::delivery::Result>>();
    deliveryPane_.setBusy();
    offlineRenderInProgress_ = true;

    auto work = [this, render, specIndex, results](app::OfflineRenderJob& job) mutable
    {
        render.onProgress = [&job](double fraction)
        {
            job.report(fraction, "Rendering the mix to measure");
            return ! job.shouldAbort();
        };
        const auto mix = engine_.renderOffline(render);
        if (mix.getNumSamples() == 0)
            return;
        const auto m = engine::delivery::measure(mix.getReadPointer(0), mix.getReadPointer(juce::jmin(1, mix.getNumChannels() - 1)),
                                                 mix.getNumSamples(), render.sampleRate);
        *results = engine::delivery::check(engine::delivery::all()[(size_t) specIndex], m);
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), results, specIndex](bool)
    {
        if (self == nullptr)
            return;
        self->offlineRenderInProgress_ = false;
        self->renderJob_.reset();
        self->followSystemOutputIfEnabled();
        self->deliveryPane_.showResults(specIndex, *results);
    };

    renderJob_ = app::OfflineRenderJob::launch("Checking delivery", std::move(work), std::move(onFinished));
}

/** Make It Pass: Export Audio, set to the spec's loudness target and
    ceiling, its file format and its rate. */
void MainComponent::exportToDeliverySpec(int specIndex)
{
    const auto& specs = engine::delivery::all();
    if (specIndex < 0 || specIndex >= (int) specs.size())
        return;
    const auto& spec = specs[(size_t) specIndex];

    app::ExportChoice choice;
    for (const auto format : engine::allExportFormats())
        if (engine::extensionFor(format) == spec.exportFormat)
            choice.options.format = format;
    choice.options.sampleRate        = spec.exportRate;
    choice.options.bitsPerSample     = choice.options.format == engine::ExportFormat::Wav ? 24 : 16;
    choice.options.qualityIndex      = 3;
    choice.options.loudnessLufs      = spec.exportLufs;
    choice.options.truePeakCeilingDb = spec.exportCeilingDb;
    exportAudioDialog(choice);
}

// ---- Reference A/B ----------------------------------------------------------

/** Load Reference Track: decoded whole and measured, then the mix measured
    too, so the two can be heard at one loudness. */
void MainComponent::loadReferenceTrack()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load Reference Track", juce::File(settings_.getValue("reference.folder")),
                                                   audiofiles::wildcards());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;
        settings_.setValue("reference.folder", file.getParentDirectory().getFullPathName());

        juce::AudioFormatManager formats;
        engine::audioformats::registerAll(formats);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        {
            showError("Could not read " + file.getFileName());
            return;
        }
        const int channels = (int) juce::jlimit(1u, 2u, reader->numChannels);
        juce::AudioBuffer<float> audio(channels, (int) reader->lengthInSamples);
        reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);

        const auto loudness = engine::exportloudness::measure(audio, reader->sampleRate);
        auto* reference       = new engine::ReferenceAudio();
        reference->sampleRate = reader->sampleRate;
        for (int ch = 0; ch < channels; ++ch)
            reference->channels.emplace_back(audio.getReadPointer(ch), audio.getReadPointer(ch) + audio.getNumSamples());
        engine_.reference().setAudio(reference);

        referenceName_ = file.getFileNameWithoutExtension();
        referenceLufs_ = loudness.integratedLufs;
        mixMeasured_   = false;
        measureMixForReference([this] { setReferenceComparing(true); });
    });
}

/** Renders the mix to measure its loudness, then @p then. */
void MainComponent::measureMixForReference(std::function<void()> then)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }
    engine::AudioEngine::OfflineRenderOptions render;
    render.lengthBeats = juce::jmax(1.0, songEndBeats());
    render.sampleRate  = 48000.0;
    auto lufs = std::make_shared<double>(engine::LoudnessMeter::kSilence);

    offlineRenderInProgress_ = true;
    auto work = [this, render, lufs](app::OfflineRenderJob& job) mutable
    {
        render.onProgress = [&job](double fraction)
        {
            job.report(fraction, "Measuring the mix's loudness");
            return ! job.shouldAbort();
        };
        const auto mix = engine_.renderOffline(render);
        if (mix.getNumSamples() > 0)
            *lufs = engine::exportloudness::measure(mix, render.sampleRate).integratedLufs;
    };
    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), lufs, then](bool cancelled)
    {
        if (self == nullptr)
            return;
        self->offlineRenderInProgress_ = false;
        self->renderJob_.reset();
        self->followSystemOutputIfEnabled();
        if (cancelled)
            return;
        self->mixLufs_            = *lufs;
        self->mixMeasured_        = true;
        self->mixMeasuredAtState_ = self->history_.stateId();
        if (then)
            then();
    };
    renderJob_ = app::OfflineRenderJob::launch("Measuring the mix", std::move(work), std::move(onFinished));
}

/** Compare with Reference: on, the mix (A) at the matched level, ready to
    switch; off, the mix as it is. A mix edited since it was measured is
    measured again first. */
void MainComponent::setReferenceComparing(bool on)
{
    auto& reference = engine_.reference();
    if (! on)
    {
        reference.setMode(engine::ReferenceAB::Off);
        showStatus("Comparison off: hearing the mix as it is");
        commandManager_.commandStatusChanged();
        return;
    }
    if (referenceName_.isEmpty())
    {
        showError("Load a reference track first (Transport > Load Reference Track)");
        return;
    }
    if (! mixMeasured_ || mixMeasuredAtState_ != history_.stateId())
    {
        measureMixForReference([this] { setReferenceComparing(true); });
        return;
    }

    float mixGain = 1.0f, referenceGain = 1.0f;
    engine::matchedGains(mixLufs_, referenceLufs_, mixGain, referenceGain);
    reference.setGains(mixGain, referenceGain);
    reference.setMode(engine::ReferenceAB::A);
    commandManager_.commandStatusChanged();

    const auto db = [](float gain) { return juce::String(juce::Decibels::gainToDecibels(gain), 1); };
    showStatus("Comparing with \"" + referenceName_ + "\" (" + juce::String(referenceLufs_, 1) + " LUFS) - the mix is "
               + juce::String(mixLufs_, 1) + " LUFS. "
               + (mixGain < 1.0f ? "The mix is turned down " + db(mixGain) + " dB" : "The reference is turned down " + db(referenceGain) + " dB")
               + " to match. Hearing A, the mix: Alt+B switches");
}

void MainComponent::switchReferenceAB()
{
    auto& reference = engine_.reference();
    if (reference.mode() == engine::ReferenceAB::Off)
        return;
    const bool toB = reference.mode() == engine::ReferenceAB::A;
    reference.setMode(toB ? engine::ReferenceAB::B : engine::ReferenceAB::A);
    commandManager_.commandStatusChanged();
    showStatus(toB ? "B: the reference, \"" + referenceName_ + "\"" : juce::String("A: the mix"));
}

// ---- Transcription and editing by text ------------------------------------------

/** The Transcript pane, when the document or the selected track has changed
    since it last looked (it keeps its selection otherwise). */
void MainComponent::refreshTranscriptPane(bool force)
{
    const auto signature = history_.stateId() * 131ull + (unsigned long long) (selectedTrackIndex_ + 1);
    if (! force && signature == transcriptShown_)
        return;
    transcriptShown_ = signature;

    const auto& song  = history_.current();
    const auto  words = model::textedit::wordsOn(song, selectedTrackIndex_);
    juce::String empty;
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        empty = "Select a track to see what's said on it.";
    else
    {
        bool hasAudio = false;
        for (const auto& clip : song.tracks[(size_t) selectedTrackIndex_].clips)
            hasAudio = hasAudio || clip.type == model::ClipType::Audio;
        empty = hasAudio ? "Transcribe this track to edit its audio as text: delete words to cut them, "
                           "find the ums and long pauses to take out. It runs on this computer."
                         : "This track has no audio to transcribe.";
    }
    transcriptPane_.setWords(words, empty);
}

/** Transcribe Track: every file the selected track's audio clips play,
    transcribed whole (so a trimmed clip's words are there if it's extended
    again), behind a progress window. Every clip of those files, on any
    track, gets the words. */
void MainComponent::transcribeSelectedTrack()
{
    if (renderJob_ != nullptr)
    {
        showError("Something is already rendering");
        return;
    }
    const juce::File model(settings_.getValue("transcribe.model"));
    if (! model.existsAsFile())
    {
        showError("Choose a transcription model first: Preferences > Folders > Transcription");
        showPreferences(4);
        return;
    }
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    std::vector<std::string> files;
    for (const auto& clip : song.tracks[(size_t) selectedTrackIndex_].clips)
        if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty() && ! clip.warp
            && std::find(files.begin(), files.end(), clip.audioFile) == files.end())
            files.push_back(clip.audioFile);
    if (files.empty())
    {
        showError("The selected track has no audio to transcribe (warped clips can't be)");
        return;
    }

    const auto language = settings_.getValue("transcribe.language", "auto").toStdString();
    auto results = std::make_shared<std::vector<std::pair<std::string, std::vector<model::TranscriptWord>>>>();
    auto error   = std::make_shared<juce::String>();

    auto work = [files, model, language, results, error](app::OfflineRenderJob& job)
    {
        juce::AudioFormatManager formats;
        engine::audioformats::registerAll(formats);
        const int count = (int) files.size();
        for (int f = 0; f < count && ! job.shouldAbort(); ++f)
        {
            const juce::File file(juce::String::fromUTF8(files[(size_t) f].c_str()));
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
            {
                *error = "Could not read " + file.getFileName();
                continue;
            }
            const int channels = (int) juce::jlimit(1u, 8u, reader->numChannels);
            juce::AudioBuffer<float> audio(channels, (int) reader->lengthInSamples);
            reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
            std::vector<float> mono((size_t) audio.getNumSamples(), 0.0f);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    mono[(size_t) i] += audio.getSample(ch, i) / (float) channels;

            const auto result = engine::transcribe(model, mono, reader->sampleRate, language, [&job, f, count, file](double p)
            {
                job.report(app::overallProgress(f, count, p), "Transcribing " + file.getFileName());
                return ! job.shouldAbort();
            });
            if (! result.ok)
            {
                *error = juce::String::fromUTF8(result.error.c_str());
                continue;
            }
            std::vector<model::TranscriptWord> words;
            for (const auto& w : result.words)
                words.push_back({ w.text, w.start, w.end, w.confidence });
            results->emplace_back(files[(size_t) f], std::move(words));
        }
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), results, error](bool cancelled)
    {
        if (self == nullptr)
            return;
        self->renderJob_.reset();
        if (cancelled)
        {
            self->showStatus("Transcription stopped");
            return;
        }
        if (results->empty())
        {
            self->showError(error->isNotEmpty() ? *error : juce::String("Nothing was transcribed"));
            return;
        }
        int words = 0;
        for (const auto& [file, list] : *results)
            words += (int) list.size();
        self->history_.edit("Transcribe", [results](model::Song& s)
        {
            for (auto& track : s.tracks)
                for (auto& clip : track.clips)
                    for (const auto& entry : *results)
                        if (clip.audioFile == entry.first)
                            clip.transcript = entry.second;
        });
        self->refreshTranscriptPane(true);
        self->showStatus("Transcribed " + juce::String(words) + " words" + (error->isNotEmpty() ? " (" + *error + ")" : juce::String())
                         + " - edit the audio as text in the Transcript pane");
    };

    renderJob_ = app::OfflineRenderJob::launch("Transcribing", std::move(work), std::move(onFinished));
}

/** The Transcript pane's Delete: the stretches cut from the selected track,
    gaps closed and joins crossfaded, as one undo step. */
void MainComponent::deleteTranscriptRanges(const std::vector<std::pair<double, double>>& ranges)
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size() || ranges.empty())
        return;
    const int  trackId     = song.tracks[(size_t) selectedTrackIndex_].id;
    const auto fileSeconds = [this](const std::string& file)
    {
        return engine_.probeDurationSeconds(juce::File(juce::String::fromUTF8(file.c_str())));
    };

    // On a copy, so a delete that cuts nothing leaves no empty undo step.
    auto       edited = song;
    const int  cuts   = model::textedit::cutRanges(edited, trackId, ranges, 0.01, fileSeconds);
    if (cuts == 0)
        return;
    double removed = 0.0; // overlapping ranges counted once: the track's own loss
    {
        auto sorted = ranges;
        std::sort(sorted.begin(), sorted.end());
        double reached = -1.0e300;
        for (const auto& r : sorted)
        {
            const double from = juce::jmax(r.first, reached);
            if (r.second > from)
                removed += r.second - from;
            reached = juce::jmax(reached, r.second);
        }
    }
    history_.apply(std::move(edited), ranges.size() == 1 ? "Delete words" : "Delete words and pauses");
    refreshAfterArrangementEdit();
    refreshTranscriptPane(true);
    showStatus("Cut " + juce::String(cuts) + (cuts == 1 ? " stretch, " : " stretches, ") + juce::String(removed, 1)
               + " s - Undo puts them back");
}

/** Preferences' Transcription model: a whisper.cpp ggml file. */
void MainComponent::chooseTranscriptionModel()
{
    chooser_ = std::make_unique<juce::FileChooser>("Transcription Model (a whisper.cpp ggml .bin file)",
                                                   juce::File(settings_.getValue("transcribe.model")), "*.bin");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc)
                          {
                              if (fc.getResult() == juce::File{})
                                  return;
                              settings_.setValue("transcribe.model", fc.getResult().getFullPathName());
                              settings_.saveIfNeeded();
                              showStatus("Transcription model: " + fc.getResult().getFileName());
                          });
}

/** Load Video: a reference video, kept with the project and shown in the
    Video pane in step with the playhead. Starts with the song unless moved. */
void MainComponent::loadVideo()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load Video", juce::File(settings_.getValue("video.folder")),
                                                   "*.mp4;*.mov;*.m4v;*.avi;*.wmv;*.mkv;*.webm");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File{})
                                  return;
                              settings_.setValue("video.folder", file.getParentDirectory().getFullPathName());
                              history_.edit("Load video", [path = file.getFullPathName().toStdString()](model::Song& s)
                              {
                                  s.videoFile = path;
                              });
                              if (workspace_.isPanelOpen("Video"))
                                  workspace_.revealPanel("Video");
                              else
                                  togglePanel(panelMenuIndex("Video"));
                              showStatus("Video: " + file.getFileName() + " - it follows the playhead; set where it starts in the Video pane");
                          });
}

} // namespace soundsplice
