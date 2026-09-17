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
    const double measured        = engine::beatsForSeconds(durationSeconds, song.bpm);
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

/** Asks what kind of file to write, then where to put it, then writes it.

    Two dialogs in sequence rather than one: the format decides the file
    extension, so the save dialog can't filter correctly until the format is
    known. Asking for the location first would mean either an unfiltered
    chooser or one that lies about what it is about to write.

    This replaced a WAV-only "Bounce" that hardcoded both the extension and
    24-bit depth. */
void MainComponent::exportAudioDialog()
{
    const double deviceRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;

    app::ExportAudioDialog::show(this, deviceRate,
        [self = juce::Component::SafePointer<MainComponent>(this)](engine::ExportOptions options)
        {
            if (self != nullptr)
                self->exportProject(options);
        });
}

/** One file an export will write: what to render, and how to write it. */
struct MainComponent::ExportTask
{
    juce::File                                   file;
    engine::AudioEngine::OfflineRenderOptions    render;
    engine::ExportOptions                        write;
    juce::String                                 label; // shown in the progress window
};

void MainComponent::exportProject(const engine::ExportOptions& options)
{
    if (renderJob_ != nullptr)
    {
        showError("An export is already running");
        return;
    }

    const auto extension = engine::extensionFor(options.format);

    chooser_ = std::make_unique<juce::FileChooser>("Export " + extension.toUpperCase(),
                                                   juce::File{}, "*." + extension);
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, options, extension](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        file = file.withFileExtension(extension);

        bool       folderFailed = false;
        const auto tasks        = buildExportTasks(file, options, folderFailed);

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
                                bool& folderFailed)
{
    std::vector<ExportTask> tasks;

    // A tail past the last clip so reverb and delay decay into the file rather
    // than being cut off mid-ring at the final beat. Shared by the mix and
    // every stem, so they all come out the same length and line up when
    // dropped into another session.
    const double lengthBeats = songEndBeats() + kBounceTailBeats;

    if (engine::writesMasterMix(options.contents))
    {
        ExportTask task;
        task.file  = masterFile;
        task.write = options;
        task.label = "master mix";
        // Everything the mix contains, rendered by the mixer itself — see
        // AudioEngine::renderOffline, and the comment there for why an export
        // calling the mixer rather than copying it is the whole design.
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
        task.write = options;
        task.label = juce::String(song.tracks[(size_t) i].name);

        task.render.lengthBeats    = lengthBeats;
        task.render.sampleRate     = options.sampleRate;
        task.render.soloTrack      = i;
        task.render.applyMasterBus = false; // stems are pre-master — see OfflineRenderOptions

        tasks.push_back(std::move(task));
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
    };

    auto result = std::make_shared<Result>();

    // The engine belongs to the render thread for the duration — see the guard
    // in timerCallback, and note that pump() frees retired audio objects.
    offlineRenderInProgress_ = true;

    auto work = [this, tasks, result](app::OfflineRenderJob& job)
    {
        const int count = (int) tasks.size();

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

            const auto buffer = engine_.renderOffline(task.render);

            // Empty means cancelled, or nothing to render. Either way there is
            // no file worth writing — see OfflineRenderOptions::onProgress for
            // why a cancelled render deliberately returns nothing rather than
            // a partial buffer.
            if (buffer.getNumSamples() == 0)
                continue;

            if (engine::writeAudioFile(task.file, buffer, task.write))
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
        if (result->stems > 0)
            self->showStatus("Exported: " + masterFile.getFileName() + " + "
                             + juce::String(result->stems) + " stem(s)");
        else
            self->showStatus("Exported: " + masterFile.getFileName());
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

} // namespace soundsplice
