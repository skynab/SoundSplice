#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Recording audio and MIDI takes.

namespace soundsplice
{
/** Toggles between arming/starting a take and stopping it.

    The take streams straight to its file as it is played, so the destination
    and the track it will land on are both decided *here*, at arm time. They
    used to be decided when the take ended — which meant every recording became
    a new track at bar 1, however the project was set up when you hit record. */
app::RecordSource MainComponent::chooseRecordSource(int trackIndex, juce::String& explanation) const
{
    const auto& song = history_.current();

    // An Instrument track is driven by MIDI clips, so it can hold a recorded
    // pattern; an Audio track cannot.
    const bool trackHoldsMidi = trackIndex >= 0 && trackIndex < (int) song.tracks.size()
                             && song.tracks[(size_t) trackIndex].type != model::TrackType::Audio;

    // The decision itself lives in app/RecordSourceChoice.h, where the whole
    // table is enumerated and tested — this function only gathers the inputs
    // and turns the reason into something worth reading.
    const auto decision = app::chooseRecordSource(trackHoldsMidi,
                                                  engine_.hasMidiInput(),
                                                  engine_.hasAudioInput(),
                                                  engine_.inputOpenError().isNotEmpty());

    switch (decision.reason)
    {
        case app::RecordSourceReason::Ok:
            break;

        case app::RecordSourceReason::FallbackToAudioNoMidi:
            explanation = "No MIDI input connected - recording audio to a new track instead";
            break;

        case app::RecordSourceReason::NoAudioInput:
            explanation = "No audio input device to record from";
            break;

        case app::RecordSourceReason::NoAudioInputPermission:
            explanation = "No audio input - check microphone permission "
                          "(System Settings > Privacy & Security > Microphone), then restart";
            break;

        case app::RecordSourceReason::NothingConnected:
            explanation = "Nothing to record from - connect a MIDI controller, or an audio "
                          "input (and check microphone permission)";
            break;
    }

    return decision.source;
}

bool MainComponent::ensureMicrophoneAccess()
{
    const auto status = app::microphonePermission();

    if (status == app::MicPermission::NotRequired)
        return true; // no permission model here; a missing device is a different report

    if (status == app::MicPermission::Granted)
    {
        // Granted, but possibly *after* the input was opened at startup — in
        // which case the engine is still running output-only and would report
        // "no audio input" with the microphone switched on. Re-ask once here
        // rather than telling anyone to restart the app.
        if (engine_.hasAudioInput())
            return true;

        if (engine_.reopenAudioInput())
            return true;

        showError("Microphone access is on, but no audio input device could be opened - "
                  "check the input device in Audio Settings");
        return false;
    }

    if (status == app::MicPermission::NotDetermined)
    {
        // The OS has never asked — usually because its one prompt appeared at
        // launch, before anyone had a reason to care, and was dismissed. This
        // is the moment it actually means something, so ask now.
        showStatus("Waiting for microphone permission...");

        app::requestMicrophonePermission(
            [self = juce::Component::SafePointer<MainComponent>(this)](bool granted)
        {
            if (self == nullptr)
                return; // the window went away while the prompt was up

            if (! granted)
            {
                self->showError("Microphone access denied - recording audio is not possible "
                                "until it is enabled in System Settings");
                return;
            }

            // Granted: the device was opened without input at startup, so it
            // has to be re-opened before anything can be captured.
            self->engine_.reopenAudioInput();

            // Pick up exactly where the user left off — they pressed Record,
            // and answering a permission prompt should not mean pressing it
            // again. Guarded so a grant that still yields no input reports
            // that rather than looping back here.
            if (self->retryingAfterMicPermission_)
                return;

            self->retryingAfterMicPermission_ = true;
            self->toggleRecording();
            self->retryingAfterMicPermission_ = false;
        });

        return false; // the prompt owns this press now
    }

    // Denied, or restricted by policy. The OS will not prompt again no matter
    // what this app does, so the only useful thing left is to take the user
    // straight to the setting instead of describing where it lives.
    juce::NativeMessageBox::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Microphone access is off",
        "SoundSplice needs microphone access to record audio.\n\n"
        "macOS will not ask again, so it has to be switched on in System Settings > "
        "Privacy & Security > Microphone. Recording will work as soon as it is on - "
        "no need to restart.",
        this,
        juce::ModalCallbackFunction::create([](int result)
        {
            if (result == 1) // Open Settings
                app::openMicrophonePrivacySettings();
        }));

    return false;
}

void MainComponent::toggleRecording()
{
    // Stopping always goes back to whichever take is actually running — the
    // sources are re-examined only when starting one.
    if (awaitingMidiTake_)
    {
        toggleMidiRecording();
        return;
    }

    if (! awaitingRecordedTake_)
    {
        // Devices are re-scanned here rather than trusted from startup: a
        // controller plugged in after launch is extremely common, and before
        // this it was invisible to the app for the whole session.
        engine_.refreshMidiInputs();

        juce::String explanation;
        auto         source = chooseRecordSource(selectedTrackIndex_, explanation);

        // A MIDI take needs no microphone, so it is decided before any
        // permission question — prompting a controller user for microphone
        // access would be a non-sequitur.
        if (source == app::RecordSource::Midi)
        {
            toggleMidiRecording();
            return;
        }

        // Everything else wants audio — *including* the "nothing connected"
        // answer, which is exactly what a blocked microphone looks like from
        // here, since a denied permission shows up as a device with no input
        // channels. So permission is settled before that answer is treated as
        // final; otherwise a one-click fix gets reported as missing hardware.
        if (! ensureMicrophoneAccess())
            return; // prompting, or already explained

        // Asked again, because granting access can have just opened an input
        // and turned None into Audio (and because a controller may have been
        // plugged in while a prompt was up).
        explanation.clear();
        source = chooseRecordSource(selectedTrackIndex_, explanation);

        if (source == app::RecordSource::None)
        {
            showError(explanation);
            return;
        }

        if (source == app::RecordSource::Midi)
        {
            toggleMidiRecording();
            return;
        }

        if (explanation.isNotEmpty())
            showStatus(explanation); // audio, but not from the armed track
    }

    if (! awaitingRecordedTake_)
    {
        // Into the saved project's audio folder, or the scratch folder until
        // there is one (see app/ProjectMedia.h).
        const auto file = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Recording", ".wav");

        if (! engine_.beginRecording(file))
        {
            // Names microphone permission first when that is what actually
            // happened, rather than making the user guess between three
            // possible causes. No longer says "then restart": the input is
            // re-opened on the next Record press (see ensureMicrophoneAccess),
            // so a restart has stopped being part of the fix.
            if (engine_.inputOpenError().isNotEmpty())
                showError("No audio input - check microphone permission "
                          "(System Settings > Privacy & Security > Microphone)");
            else
                showError("Could not start recording (no audio input device, "
                          "or the file could not be created)");
            return;
        }

        recordingFile_ = file;

        // Onto the selected track if it can hold audio, otherwise a new one.
        // A Synth track can't take an audio clip, so recording while
        // one is selected has to mean "somewhere else" rather than fail.
        const auto& song = history_.current();
        const bool  canHoldAudio = selectedTrackIndex_ >= 0
                                && selectedTrackIndex_ < (int) song.tracks.size()
                                && song.tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio;
        recordingTargetTrack_ = canHoldAudio ? selectedTrackIndex_ : -1;

        awaitingRecordedTake_ = true;
        recordButton.setToggleState(true, juce::dontSendNotification); // swaps to the stop square
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));

        // The transport runs free for the length of a take. Looping would wrap
        // it at the end of what is already arranged, which is precisely where
        // a recording needs to keep going — you are recording the part that
        // isn't there yet. The button's own state is left alone and restored
        // when the take ends, so the user's setting survives.
        post(Cmd::SetLooping, 0.0);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopRecording();
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0); // whatever it was before
        recordButton.setToggleState(false, juce::dontSendNotification); // back to the record disc
        recordButton.setTooltip(withShortcut("Record", keys::record));
    }
}

void MainComponent::finishRecordingIfReady()
{
    if (! awaitingRecordedTake_ || ! engine_.isRecordingFinished())
        return;
    awaitingRecordedTake_ = false;

    // Every ending passes through here — the stop button, play/pause during a
    // take, or the engine finishing on its own — so this is where looping is
    // put back. Restoring it only in the stop button's handler would leave
    // loop silently off after any other route out, including an empty take
    // that returns just below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

    const int64_t dropped   = engine_.recordedDroppedSamples();
    const int64_t startedAt = engine_.recordedTakeStartSample();

    // Closes the file and hands it over; empty means nothing was captured.
    const auto file = engine_.finishRecordedTake();
    if (file == juce::File{})
    {
        showError("Recording was empty (no input captured)");
        return;
    }

    // Where the take goes on the timeline: where the transport actually was
    // when capture began, which is after any count-in. Falls back to the start
    // only if the engine never reported a position.
    const double startBeats = startedAt >= 0
                                ? juce::jmax(0.0, uiTempoMap_.ppqFromSamples(startedAt))
                                : 0.0;

    // The clip's length, undo, and selection all come from the existing import
    // path — which measures the file's real duration rather than guessing, and
    // appends to the target track rather than always making a new one. This
    // used to be a second, hand-written copy of that logic here.
    importAudioFileAtBeat(file, startBeats, recordingTargetTrack_);

    recordingFile_        = juce::File{};
    recordingTargetTrack_ = -1;

    // Reported after the import, so the take is on the timeline either way —
    // a recording with a gap is still worth keeping, it just must not be
    // presented as a clean one.
    if (dropped > 0)
    {
        showError("Recorded with gaps - the disk could not keep up ("
                  + juce::String((int) dropped) + " samples lost)");
        return;
    }

    // A take of pure digital silence means the input device handed us zeros
    // for its whole length, which is a different failure from "no input
    // device" and used to be reported as a success: the track appeared, the
    // status bar said "Recorded:", and only playing it back revealed nothing
    // was there. On macOS the usual cause is microphone permission — the OS
    // grants none and CoreAudio delivers zeros rather than an error — so the
    // message names that first.
    if (isSilentAudioFile(file))
    {
        showError("Recorded silence - check microphone permission "
                  "(System Settings > Privacy & Security > Microphone) and the input device");
        return;
    }

    showStatus("Recorded: " + file.getFileName());
}

/** Starts or stops a MIDI take. The mirror of toggleRecording's audio path,
    and deliberately the same shape — the transport handling, the button state
    and the looping-off rule are identical, because they are the same
    behaviours for the same reasons. What differs is only which recorder is
    armed and that there is no file to open, so this cannot fail: a controller
    that is absent simply sends nothing, which is an empty take rather than an
    error. */
void MainComponent::toggleMidiRecording()
{
    if (! awaitingMidiTake_)
    {
        midiTakeEvents_.clear();
        midiRecordingTargetTrack_ = selectedTrackIndex_;

        engine_.beginMidiRecording();
        awaitingMidiTake_ = true;

        recordButton.setToggleState(true, juce::dontSendNotification);
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));

        // Looping off for the length of a take, restored when it ends — the
        // same reasoning as the audio path: looping would wrap the transport
        // at the end of what is already arranged, which is precisely where a
        // recording needs to keep going.
        post(Cmd::SetLooping, 0.0);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopMidiRecording();
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        recordButton.setToggleState(false, juce::dontSendNotification);
        recordButton.setTooltip(withShortcut("Record", keys::record));
    }
}

void MainComponent::finishMidiRecordingIfReady()
{
    if (! awaitingMidiTake_)
        return;

    // Drained every tick, take finished or not: this is what keeps the
    // engine's ring from having to hold a whole take (see engine::MidiRecorder
    // for why that matters — a fixed ring sized for a take is a silent cap).
    engine_.drainMidiTake(midiTakeEvents_);

    if (! engine_.isMidiRecordingFinished())
        return;
    awaitingMidiTake_ = false;

    // Every ending passes through here, so this is where looping is put back
    // — restoring it only in the stop handler would leave it silently off
    // after any other route out, including the empty take that returns below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

    const int64_t dropped   = engine_.midiRecordedDroppedEvents();
    const int64_t startedAt = engine_.midiTakeStartSample();
    const int64_t endedAt   = engine_.midiTakeEndSample();
    const int     target    = midiRecordingTargetTrack_;

    midiRecordingTargetTrack_ = -1;

    if (midiTakeEvents_.empty() || startedAt < 0)
    {
        midiTakeEvents_.clear();
        showError("Recording was empty (no MIDI input captured)");
        return;
    }

    commitMidiTake(target, startedAt, endedAt);
    midiTakeEvents_.clear();

    // Reported after the commit, so the take is on the timeline either way —
    // a take missing a note is still worth keeping, it just must not be
    // presented as a clean one.
    if (dropped > 0)
        showError("Recorded with gaps - " + juce::String((int) dropped)
                  + " MIDI event(s) were lost");
}

void MainComponent::commitMidiTake(int targetTrack, int64_t startSample, int64_t endSample)
{
    if (targetTrack < 0 || targetTrack >= trackCount())
        return;

    // Samples to beats happens here, on the message thread, through the same
    // tempo map the UI already reads — which is why engine::MidiCapture takes
    // beats and knows nothing about tempo: with a tempo map, a take spanning a
    // tempo change cannot be converted by one scalar, and this is the only
    // place that has the map.
    const double takeStartBeats = juce::jmax(0.0, uiTempoMap_.ppqFromSamples(startSample));
    const double takeEndBeats   = endSample > startSample
                                    ? juce::jmax(takeStartBeats, uiTempoMap_.ppqFromSamples(endSample))
                                    : takeStartBeats;

    std::vector<engine::TimedMidiEvent> timed;
    timed.reserve(midiTakeEvents_.size());
    for (const auto& event : midiTakeEvents_)
    {
        engine::TimedMidiEvent converted;
        // Relative to the take's own start: a clip's notes are positioned from
        // the clip start, and the clip is placed at takeStartBeats below.
        converted.beats      = uiTempoMap_.ppqFromSamples(event.timeSamples) - takeStartBeats;
        converted.noteNumber = event.noteNumber;
        converted.velocity   = event.velocity;
        converted.noteOn     = event.noteOn;
        timed.push_back(converted);
    }

    auto notes = engine::MidiCapture::notesFromEvents(std::move(timed),
                                                      takeEndBeats - takeStartBeats);
    if (notes.empty())
    {
        // Every captured event was an unmatched note-off — keys that were
        // already down when capture began. Nothing was actually played.
        showError("Recording was empty (no MIDI input captured)");
        return;
    }

    // The clip is as long as the take, rounded up to a whole bar: a take is a
    // musical phrase, and ending the clip on the last note's release would
    // make a loop of it jarringly short.
    double contentEnd = takeEndBeats - takeStartBeats;
    for (const auto& note : notes)
        contentEnd = juce::jmax(contentEnd, note.startBeats + note.lengthBeats);

    const double lengthBeats = engine::MidiCapture::clipLengthForTake(
        contentEnd, juce::jmax(1.0, uiTempoMap_.quartersPerBar()));

    const int noteCount = (int) notes.size();
    int       newClipIndex = -1;

    history_.edit("Record MIDI", [&](model::Song& s)
    {
        if (targetTrack < 0 || targetTrack >= (int) s.tracks.size())
            return;
        auto& track = s.tracks[(size_t) targetTrack];

        model::Clip clip;
        clip.id                  = model::allocateId(s);
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = takeStartBeats;
        clip.lengthBeats         = lengthBeats;
        clip.pattern.lengthBeats = lengthBeats;
        clip.pattern.notes       = std::move(notes);

        track.clips.push_back(clip);
        newClipIndex = (int) track.clips.size() - 1;
    });

    if (newClipIndex < 0)
        return;

    // Open the take in the piano roll, the way Add Clip opens the clip it
    // made: the first thing anyone does with a recorded part is look at it.
    selectedTrackIndex_ = targetTrack;
    selectedClipIndex_  = newClipIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();

    showStatus("Recorded " + juce::String(noteCount) + " note(s)");
}

juce::File MainComponent::recordingsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("SoundSplice Recordings");
    dir.createDirectory();
    return dir;
}

} // namespace soundsplice
