#include "MainComponentInternal.h"

#include <map>

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The document on disk: new, open, save, the discard prompt, and autosave
// with crash recovery.

namespace soundsplice
{
/** Discards the current document for an empty one, asking first. */
void MainComponent::newProject()
{
    confirmDiscardChanges([this] { createEmptyProject(); });
}

/** The document a fresh launch and File > New both start from: one synth
    track holding one empty one-bar clip, so the piano roll has something to
    open and recording has somewhere to land. */
model::Song MainComponent::makeEmptySong()
{
    model::Song song;
    const int id = model::addTrack(song, model::TrackType::Instrument, "Synth 1").id;
    model::Clip clip;
    clip.type                = model::ClipType::Instrument;
    clip.lengthBeats         = 4.0;
    clip.pattern.lengthBeats = 4.0;
    model::addClip(song, id, clip);
    return song;
}

void MainComponent::createEmptyProject()
{
    const model::Song song = makeEmptySong();

    history_.reset(song);
    selectedTrackIndex_ = 0;
    tempoSlider.setValue(song.bpm, juce::dontSendNotification);

    uiTempoMap_.setTempo(song.bpm);
    post(Cmd::SetTempo, song.bpm);
    refreshFromModel();
    clipLabel.setText("No clip loaded", juce::dontSendNotification);

    // A brand-new project has nothing worth saving yet, so it starts clean —
    // quitting straight after New shouldn't ask about it.
    projectFile_  = juce::File{};
    savedStateId_ = history_.stateId();
    updateWindowTitle();
}

void MainComponent::setProjectRootFolderDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Set project root folder", juce::File{});
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir == juce::File{} || ! dir.isDirectory())
            return;

        const auto path = dir.getFullPathName().toStdString();
        history_.edit("Set project root folder", [path](model::Song& s) { s.projectRootFolder = path; });

        fileBrowser_.setProjectRootFolder(dir);
        showStatus("Project root folder set to: " + dir.getFullPathName());
    });
}

/** True while the document differs from what's on disk. Asks the history for
    the identity of the state it's holding rather than tracking a modified
    flag, so undoing back to the saved state reads as saved again — see
    History::stateId. */
bool MainComponent::hasUnsavedChanges() const
{
    return history_.stateId() != savedStateId_;
}

/** Puts the project's name and an unsaved marker in the title bar, which is
    the only place either is visible. Called every timer tick, so it compares
    before setting: DocumentWindow::setName repaints the frame. */
void MainComponent::updateWindowTitle()
{
    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("Untitled")
                                  : projectFile_.getFileNameWithoutExtension();

    const juce::String title = name + (hasUnsavedChanges() ? " *" : "") + " - SoundSplice";
    if (title == windowTitle_)
        return;

    windowTitle_ = title;
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(title);
}

/** The actual write, shared by Save and Save As. Marks the document clean
    against the state that was written — not whatever it becomes later — so an
    edit made while the file chooser was up still counts as unsaved.

    The project's audio travels with it: see app/ProjectMedia.h. */
bool MainComponent::writeProjectTo(const juce::File& file)
{
    collectProjectAudio(file);

    // Taken after collecting, which repoints paths and so moves the state id.
    const auto        stateWritten = history_.stateId();
    const std::string text = model::serialize(app::media::withStoredPaths(history_.current(), file));

    if (! file.replaceWithText(juce::String::fromUTF8(text.c_str())))
    {
        showError("Could not save " + file.getFileName());
        return false;
    }

    projectFile_   = file;
    savedStateId_  = stateWritten;
    discardAutosave(); // on disk for real now; an edit made during the save gets autosaved afresh
    cleanUpProjectAudio();
    updateWindowTitle();
    return true;
}

juce::File MainComponent::audioDirectoryFor(const juce::File& scratchDirectory) const
{
    if (projectFile_ == juce::File{})
        return scratchDirectory;

    const auto folder = app::media::audioFolderFor(projectFile_);
    return folder.createDirectory().wasOk() ? folder : scratchDirectory;
}

/** Copies the audio the app made for this project (recordings and edits still
    in the scratch folders, or in the previous project's audio folder after
    Save As) into @p projectFile's audio folder, and repoints the document at
    the copies.

    Done in place rather than as an undoable edit, because where a file lives
    isn't an edit anyone made. The originals stay where they were, so an undo
    that brings back an older path still finds its audio. */
void MainComponent::collectProjectAudio(const juce::File& projectFile)
{
    const auto folder = app::media::audioFolderFor(projectFile);

    std::vector<juce::File> owned { recordingsDirectory(), editsDirectory() };
    if (projectFile_ != juce::File{} && projectFile_ != projectFile)
        owned.push_back(app::media::audioFolderFor(projectFile_));

    // Checked before touching the document, so a project whose audio is all
    // in place isn't marked as changed by saving it.
    bool anyToCollect = false;
    app::media::forEachAudioPath(history_.current(), [&](const std::string& path)
    {
        anyToCollect = anyToCollect || app::media::shouldCollect(app::media::fileFromPath(path), folder, owned);
    });

    if (! anyToCollect)
        return;

    std::map<juce::String, juce::File> copies; // original path -> its copy, so shared files copy once
    int failed = 0;

    app::media::forEachAudioPath(history_.mutableCurrent(), [&](std::string& path)
    {
        const auto audio = app::media::fileFromPath(path);
        if (! app::media::shouldCollect(audio, folder, owned))
            return;

        auto copy = copies.find(audio.getFullPathName());
        if (copy == copies.end())
        {
            const auto collected = app::media::collectInto(audio, folder);
            copy = copies.emplace(audio.getFullPathName(), collected).first;
            if (collected == audio)
                ++failed;
        }

        path = app::media::pathOf(copy->second);
    });

    if (failed > 0)
        showError(juce::String(failed) + (failed == 1 ? " audio file" : " audio files")
                  + " couldn't be copied into \"" + folder.getFileName()
                  + "\" - the project still plays them from where they are");

    // Same audio at new paths: the engine and the panes follow the document.
    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    refreshAudioEditorForSelected();
}

/** Moves audio files in the project's audio folder that nothing uses any more
    (superseded edits, deleted takes) to the trash.

    "Nothing" includes the undo history: a file an undo could bring back is
    kept for as long as that history exists. Only the project's own folder is
    touched, only audio files directly in it, and to the trash rather than
    deleted, so anything removed by mistake can be got back. */
void MainComponent::cleanUpProjectAudio()
{
    if (projectFile_ == juce::File{})
        return;

    juce::Array<juce::File> referenced;

    // A take being recorded right now isn't in the document until it stops,
    // but it is very much in use.
    if (awaitingRecordedTake_ && recordingFile_ != juce::File{})
        referenced.add(recordingFile_);

    history_.forEachState([&referenced](const model::Song& song)
    {
        app::media::forEachAudioPath(song, [&referenced](const std::string& path)
        {
            referenced.addIfNotAlreadyThere(app::media::fileFromPath(path));
        });
    });

    int moved = 0;
    for (const auto& file : app::media::unusedAudioFiles(app::media::audioFolderFor(projectFile_), referenced))
        if (file.moveToTrash())
            ++moved;

    if (moved > 0)
        showStatus("Moved " + juce::String(moved) + (moved == 1 ? " unused audio file" : " unused audio files")
                   + " to the trash");
}

/** Saves over the project's own file, falling back to Save As the first time.
    @p onDone reports whether the document actually reached disk — the
    discard prompt needs to know, since a cancelled save must cancel whatever
    it was clearing the way for. */
void MainComponent::saveProject(std::function<void(bool)> onDone)
{
    if (projectFile_ == juce::File{})
    {
        saveProjectAs(std::move(onDone));
        return;
    }

    const bool saved = writeProjectTo(projectFile_);
    if (onDone)
        onDone(saved);
}

void MainComponent::saveProjectAs(std::function<void(bool)> onDone)
{
    chooser_ = std::make_unique<juce::FileChooser>("Save project", projectFile_, "*.soundsplice");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, onDone](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
        {
            if (onDone)
                onDone(false); // dismissed the chooser: nothing was saved
            return;
        }

        const bool saved = writeProjectTo(file.withFileExtension("soundsplice"));
        if (onDone)
            onDone(saved);
    });
}

/** Runs @p onProceed once it's safe to throw the current document away,
    asking first if there's anything to lose. Everything that discards the
    document goes through here — New, Open, and quitting — so there is one
    place the question is asked and one place it can be got wrong.

    Cancel, and a Save the user backs out of, both simply drop @p onProceed:
    the destructive action doesn't happen. */
void MainComponent::confirmDiscardChanges(std::function<void()> onProceed)
{
    if (! hasUnsavedChanges())
    {
        if (onProceed)
            onProceed();
        return;
    }

    // Past this point the user is choosing what happens to the changes, so
    // no recovery offer is still outstanding for them.
    recoveryPending_ = false;

    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("this project")
                                  : projectFile_.getFileName();

    juce::NativeMessageBox::showYesNoCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        "Save changes to " + name + " before closing it?",
        this,
        juce::ModalCallbackFunction::create([self = juce::Component::SafePointer<MainComponent>(this),
                                             onProceed](int result)
        {
            if (self == nullptr)
                return; // the window went away while the box was up

            if (result == 1) // Yes: save first, and only then go ahead
            {
                self->saveProject([onProceed](bool saved) { if (saved && onProceed) onProceed(); });
            }
            else if (result == 2) // No: discard
            {
                // Deliberately thrown away, so never offered back at the next
                // launch. If this was Open and the chooser is then cancelled,
                // the next autosave tick simply writes it again.
                self->discardAutosave();
                if (onProceed)
                    onProceed();
            }
            // Cancel (0): stay exactly where we are.
        }));
}

void MainComponent::openProject()
{
    confirmDiscardChanges([this] { chooseProjectToOpen(); });
}

void MainComponent::chooseProjectToOpen()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open project", projectFile_, "*.soundsplice");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        model::Song song;
        std::string error;
        if (! model::deserialize(file.loadFileAsString().toStdString(), song, &error))
        {
            showError("Could not open " + file.getFileName() + ": " + error);
            return;
        }

        // Paths inside the project's folder are stored relative to it, so the
        // project finds its audio wherever the folder has been moved.
        loadSongIntoEditor(app::media::withResolvedPaths(song, file));

        projectFile_  = file;
        savedStateId_ = history_.stateId(); // what's on screen is what's on disk
        updateWindowTitle();
    });
}

/** Makes @p song the whole document: history, tempo and every view. Shared by
    Open and by recovering an autosave, which differ only in what they say
    about the file afterwards. */
void MainComponent::loadSongIntoEditor(const model::Song& song)
{
    history_.reset(song);
    selectedTrackIndex_ = 0;

    tempoSlider.setValue(song.bpm, juce::dontSendNotification);
    uiTempoMap_.setTempo(song.bpm);
    post(Cmd::SetTempo, song.bpm);
    refreshFromModel();
}

/** Where the unsaved document is kept: beside the app's settings rather than
    beside the project. An untitled project has no folder of its own, and a
    recovery file next to a real project would look like part of it.

    One file for the app, so two copies running at once would share it — a
    known limit, and a rare way to use this app. */
juce::File MainComponent::autosaveFile() const
{
    return settings_.getFile().getSiblingFile("Autosave").getChildFile("recovery.soundsplice-autosave");
}

/** Writes the document to the autosave file when there is something new to
    protect (see app::autosaveDue), and removes the file once there isn't.
    Called from the timer. */
void MainComponent::autosaveIfDue()
{
    // The file on disk is the one being offered back; leave it alone until
    // the user has answered.
    if (recoveryPending_)
        return;

    // Back to what's on disk, by saving or by undoing to the saved state:
    // nothing left to recover.
    if (! hasUnsavedChanges())
    {
        if (autosavedStateId_ != 0)
            discardAutosave();
        return;
    }

    const auto   stateId = history_.stateId();
    const double now     = juce::Time::getMillisecondCounterHiRes();

    if (! app::autosaveDue(stateId, savedStateId_, autosavedStateId_, (now - lastAutosaveMs_) / 1000.0,
                           kAutosaveIntervalSeconds))
        return;

    // Taken even if the write fails: retrying a failing disk thirty times a
    // second would help nobody.
    lastAutosaveMs_ = now;

    const auto file = autosaveFile();
    if (file.getParentDirectory().createDirectory().failed())
        return;

    const auto text = app::wrapAutosave(model::serialize(history_.current()),
                                        projectFile_.getFullPathName().toStdString());

    // Through a temporary file, so a crash in the middle of writing leaves the
    // previous autosave intact rather than half of this one.
    juce::TemporaryFile temp(file);
    if (temp.getFile().replaceWithText(juce::String::fromUTF8(text.c_str()))
        && temp.overwriteTargetFileWithTemporary())
        autosavedStateId_ = stateId;
}

void MainComponent::discardAutosave()
{
    autosaveFile().deleteFile();
    autosavedStateId_ = 0;
}

/** At launch: if the last session ended while holding changes it neither
    saved nor discarded (a crash, a power cut, the process being killed),
    offer them back. */
void MainComponent::offerAutosaveRecovery()
{
    const auto file = autosaveFile();
    if (! file.existsAsFile())
    {
        recoveryPending_ = false;
        return;
    }

    app::AutosaveContents contents;
    model::Song           song;
    std::string           error;

    if (! app::unwrapAutosave(file.loadFileAsString().toStdString(), contents)
        || ! model::deserialize(contents.projectText, song, &error))
    {
        // Not offered, but kept under another name rather than deleted, in
        // case it's worth digging out by hand.
        file.moveFileTo(file.withFileExtension("damaged").getNonexistentSibling());
        recoveryPending_ = false;
        return;
    }

    const juce::File original = contents.originalPath.empty()
                                    ? juce::File{}
                                    : juce::File(juce::String::fromUTF8(contents.originalPath.c_str()));
    const juce::String name = original == juce::File{} ? juce::String("an untitled project")
                                                      : original.getFileName();

    juce::NativeMessageBox::showYesNoBox(
        juce::MessageBoxIconType::QuestionIcon,
        "Recover unsaved changes?",
        "SoundSplice closed without saving " + name + ".\n\n"
            "Recover the changes it was holding? Choosing No deletes them.",
        this,
        juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainComponent>(this),
                                             song, original](int result)
        {
            if (safe == nullptr)
                return;

            safe->recoveryPending_ = false;

            if (result != 1)
            {
                safe->discardAutosave();
                safe->showStatus("Unsaved changes discarded");
                return;
            }

            safe->loadSongIntoEditor(song);

            // Pointed back at the project it came from, so Save goes where it
            // would have, but still unsaved: none of this is in that file yet.
            safe->projectFile_ = original != juce::File{} && original.getParentDirectory().isDirectory()
                                     ? original
                                     : juce::File{};
            safe->history_.mutableCurrent(); // moves the state id, so the document reads as unsaved
            safe->autosavedStateId_ = safe->history_.stateId(); // the file already holds exactly this
            safe->updateWindowTitle();
            safe->showStatus("Recovered unsaved changes - save to keep them");
        }));
}

} // namespace soundsplice
