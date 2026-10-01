#include "MainComponentInternal.h"

#include "model/Takes.h"

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
        // With Loop on and a time selection to loop, the take goes round it
        // and each pass becomes a take of one clip. Started from inside the
        // loop - from its start if the playhead is elsewhere - so there is a
        // loop to go round. Before arming, which reads where the playhead is.
        loopRecording_       = loopButton.getToggleState() && ! timeSelection_.isEmpty();
        loopRecordFromBeats_ = timeSelection_.startBeats;
        loopRecordToBeats_   = timeSelection_.endBeats;

        // Punching: the take replaces only the time selection. The transport
        // still rolls from the playhead, so there's a lead-up to play along
        // to; what's recorded before the selection is dropped when it's done.
        punchRecording_ = ! loopRecording_ && ! timeSelection_.isEmpty()
                       && settings_.getBoolValue("punchRecording", false);
        if (loopRecording_)
        {
            const double at = playheadBeat();
            if (at < loopRecordFromBeats_ || at >= loopRecordToBeats_)
                seekToBeat(loopRecordFromBeats_);
        }

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
        // when the take ends, so the user's setting survives. A loop
        // recording is the exception: going round is the point.
        post(Cmd::SetLooping, loopRecording_ ? 1.0 : 0.0);
        post(Cmd::SetPlaying, 1.0);
        if (loopRecording_)
            showStatus("Loop recording - every pass round the selection becomes a take");
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

    // The recording came back late by the device's round trip: the clip
    // plays from that far into its file, so it lines up with what was playing.
    const int latency = recordingLatencySamples();
    const int passes  = loopRecording_ ? makeLoopTakesFromRecording(file, startedAt, latency) : 0;
    if (passes == 0)
        compensateRecordingLatency(file, latency);
    const bool punched = punchRecording_ && punchRecordedClip(file);
    loopRecording_  = false;
    punchRecording_ = false;

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

    if (passes >= 2)
        showStatus("Recorded " + juce::String(passes) + " passes as takes - right-click the clip to choose one");
    else if (punched)
        showStatus("Punched in over the selection");
    else
        showStatus("Recorded: " + file.getFileName());
}

/** Turns the clip a loop recording was just imported as (the selected one)
    into a clip over the loop with a take for each pass. Folded into the
    import's undo step: the recording is one thing to undo, however many
    passes it had. Returns how many passes there were, or 0 if it never went
    round and so stays an ordinary clip. */
int MainComponent::makeLoopTakesFromRecording(const juce::File& file, int64_t startedAt, int latencySamples)
{
    const double rate = engine_.sampleRate();
    if (rate <= 0.0 || startedAt < 0)
        return 0;

    const double loopStart = (double) uiTempoMap_.samplesFromPpq(loopRecordFromBeats_) / rate;
    const double loopEnd   = (double) uiTempoMap_.samplesFromPpq(loopRecordToBeats_) / rate;
    // The file starts the round trip before capture began, in what was
    // playing: that's where its passes are counted from.
    const auto   offsets   = model::takeedit::loopPassOffsets((double) (startedAt - latencySamples) / rate, loopStart,
                                                              loopEnd, engine_.probeDurationSeconds(file), 1.0);
    if (offsets.empty())
        return 0;

    auto& song = history_.mutableCurrent();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return 0;
    auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size()
        || clips[(size_t) selectedClipIndex_].audioFile != file.getFullPathName().toStdString())
        return 0;

    model::takeedit::makeLoopTakes(clips[(size_t) selectedClipIndex_], file.getFullPathName().toStdString(), offsets,
                                   loopRecordFromBeats_, loopRecordToBeats_ - loopRecordFromBeats_);
    refreshAfterArrangementEdit();
    return (int) offsets.size();
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

/** Punches the clip a recording was just imported as (the selected one) in
    over the selection it was started with: it replaces what the track had
    there, and its lead-up is dropped. Folded into the recording's undo step.
    False, leaving the clip as recorded, if it never reached the selection. */
bool MainComponent::punchRecordedClip(const juce::File& file)
{
    auto& song = history_.mutableCurrent();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return false;

    auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size()
        || track.clips[(size_t) selectedClipIndex_].audioFile != file.getFullPathName().toStdString())
        return false;

    const auto recorded = track.clips[(size_t) selectedClipIndex_];
    const int  trackId  = track.id;
    track.clips.erase(track.clips.begin() + selectedClipIndex_);

    const int id = model::takeedit::punchIn(song, trackId, recorded, loopRecordFromBeats_, loopRecordToBeats_, 0.01);
    auto&     clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (id == 0)
    {
        clips.push_back(recorded); // put back as it was
        selectedClipIndex_ = (int) clips.size() - 1;
        return false;
    }

    for (int i = 0; i < (int) clips.size(); ++i)
        if (clips[(size_t) i].id == id)
            selectedClipIndex_ = i;
    refreshAfterArrangementEdit();
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    return true;
}

/** Retroactive recording: what came in since playback last started (or
    jumped), kept by the engine, saved as a recording and put on the selected
    audio track (a new one otherwise) where it was played - as if Record had
    been pressed when it started, latency compensation and all. */
void MainComponent::saveRecentInput()
{
    juce::AudioBuffer<float> kept;
    int64_t                  startedAt = -1;
    if (! engine_.copyRecentInput(kept, startedAt))
    {
        showError("Nothing kept yet - the input is kept while playing");
        return;
    }

    if (kept.getMagnitude(0, kept.getNumSamples()) <= 0.0f)
    {
        showError("What was kept is silence - check the input device");
        return;
    }

    const double rate = engine_.sampleRate();
    const auto   file = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Recovered", ".wav");
    if (rate <= 0.0 || ! engine::OfflineRenderer::writeWav(file, kept, rate))
    {
        showError("Could not write " + file.getFileName());
        return;
    }

    const auto& song         = history_.current();
    const bool  canHoldAudio = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size()
                            && song.tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio;
    importAudioFileAtBeat(file, juce::jmax(0.0, uiTempoMap_.ppqFromSamples(startedAt)),
                          canHoldAudio ? selectedTrackIndex_ : -1);
    compensateRecordingLatency(file, recordingLatencySamples());
    showStatus("Saved " + juce::String((double) kept.getNumSamples() / rate, 1) + " s of recent input");
}

/** The record format the settings hold: 24-bit stereo from the first input
    until it's been chosen. */
engine::AudioRecorder::Format MainComponent::savedRecordFormat()
{
    engine::AudioRecorder::Format format;
    format.bitsPerSample = settings_.getIntValue("recordBits", 24);
    format.channels      = juce::jlimit(1, 2, settings_.getIntValue("recordChannels", 2));
    format.firstInput    = juce::jmax(0, settings_.getIntValue("recordFirstInput", 0));
    return format;
}

/** Recording Format: bit depth, mono or stereo, and which input (or pair)
    takes are recorded from. Applies from the next take. */
void MainComponent::showRecordingFormatDialog()
{
    if (awaitingRecordedTake_)
    {
        showError("Stop recording first");
        return;
    }

    const auto format = savedRecordFormat();
    const auto inputs = engine_.inputChannelNames();

    auto* window = new juce::AlertWindow("Recording Format",
        "What takes are recorded as, from the next one on. 24-bit is plenty for most things; 32-bit float "
        "can't clip in the file, which helps when levels are unknown. Inputs come from the device chosen "
        "in Audio Settings.",
        juce::MessageBoxIconType::NoIcon, this);

    window->addComboBox("bits", { "16-bit", "24-bit", "32-bit float" }, "Bit depth:");
    window->getComboBoxComponent("bits")->setSelectedItemIndex(format.bitsPerSample <= 16 ? 0 : format.bitsPerSample >= 32 ? 2 : 1);

    window->addComboBox("channels", { "Mono", "Stereo" }, "Channels:");
    window->getComboBoxComponent("channels")->setSelectedItemIndex(format.channels == 1 ? 0 : 1);

    // Each input by name; for stereo, the take is it and the next one.
    juce::StringArray inputChoices;
    for (int i = 0; i < juce::jmax(1, inputs.size()); ++i)
        inputChoices.add(inputs.isEmpty() ? juce::String("Input 1") : inputs[i]);
    window->addComboBox("input", inputChoices, "From input (stereo: it and the next):");
    window->getComboBoxComponent("input")->setSelectedItemIndex(juce::jmin(format.firstInput, inputChoices.size() - 1));

    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            static constexpr int kBits[] { 16, 24, 32 };
            const int bits     = kBits[juce::jlimit(0, 2, window->getComboBoxComponent("bits")->getSelectedItemIndex())];
            const int channels = window->getComboBoxComponent("channels")->getSelectedItemIndex() == 0 ? 1 : 2;
            const int input    = juce::jmax(0, window->getComboBoxComponent("input")->getSelectedItemIndex());

            self->settings_.setValue("recordBits", bits);
            self->settings_.setValue("recordChannels", channels);
            self->settings_.setValue("recordFirstInput", input);
            self->settings_.saveIfNeeded();
            self->engine_.setRecordFormat(self->savedRecordFormat());
            self->showStatus("Recording " + juce::String(bits == 32 ? "32-bit float" : juce::String(bits) + "-bit")
                             + (channels == 1 ? " mono" : " stereo") + " from input " + juce::String(input + 1));
        }), false);
}

/** How late a recording is: the device's reported round trip, adjusted by
    the Recording Latency setting (a driver's figure is often a little off),
    or nothing with compensation turned off. */
int MainComponent::recordingLatencySamples()
{
    if (! settings_.getBoolValue("compensateRecordingLatency", true))
        return 0;

    const double adjustMs = settings_.getDoubleValue("recordingLatencyAdjustMs", 0.0);
    const int    adjust   = (int) std::lround(adjustMs * 0.001 * engine_.sampleRate());
    return juce::jmax(0, engine_.reportedRoundTripSamples() + adjust);
}

/** Moves the clip a recording was just imported as (the selected one) that
    far into its file, keeping it where it is on the timeline: what it holds
    at that point is what was played there. Folded into the import's undo
    step, like the loop takes. */
void MainComponent::compensateRecordingLatency(const juce::File& file, int latencySamples)
{
    const double rate = engine_.sampleRate();
    if (latencySamples <= 0 || rate <= 0.0)
        return;

    auto& song = history_.mutableCurrent();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    auto& clip = clips[(size_t) selectedClipIndex_];
    if (clip.audioFile != file.getFullPathName().toStdString())
        return;

    const double seconds     = (double) latencySamples / rate;
    clip.sourceOffsetSeconds += seconds;
    clip.lengthBeats          = juce::jmax(0.0, clip.lengthBeats - engine::beatsForSeconds(seconds, song.bpm));
    refreshAfterArrangementEdit();
}

/** Recording Latency: whether recordings are moved back by the device's
    round trip, and by how much more or less than it reports. */
void MainComponent::showRecordingLatencyDialog()
{
    const double rate     = engine_.sampleRate();
    const int    reported = engine_.reportedRoundTripSamples();
    const auto   reportedText = rate > 0.0
                                  ? juce::String((double) reported * 1000.0 / rate, 1) + " ms ("
                                        + juce::String(reported) + " samples)"
                                  : juce::String("no device open");

    auto* window = new juce::AlertWindow("Recording Latency",
        "A recording comes back late by the time sound takes to leave the device and return to it. "
        "The device reports " + reportedText + "; recordings are moved back by that, plus any "
        "adjustment below.\n\nTo measure it, record the metronome's click through a cable from an "
        "output to an input, and set the adjustment so the recorded click lands on the beat.",
        juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("adjust", juce::String(settings_.getDoubleValue("recordingLatencyAdjustMs", 0.0), 1),
                          "Adjustment (ms, + moves recordings earlier):");
    window->addComboBox("compensate", { "Compensate recordings", "Leave recordings where they land" });
    if (auto* box = window->getComboBoxComponent("compensate"))
        box->setSelectedItemIndex(settings_.getBoolValue("compensateRecordingLatency", true) ? 0 : 1);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double adjust = juce::jlimit(-500.0, 500.0, window->getTextEditorContents("adjust").getDoubleValue());
            const bool   on     = window->getComboBoxComponent("compensate")->getSelectedItemIndex() == 0;
            self->settings_.setValue("recordingLatencyAdjustMs", adjust);
            self->settings_.setValue("compensateRecordingLatency", on);
            self->settings_.saveIfNeeded();
            self->showStatus(on ? "Recordings are moved back by " + juce::String(self->recordingLatencySamples())
                                      + " samples"
                                : juce::String("Recordings are left where they land"));
        }), false);
}

juce::File MainComponent::recordingsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("SoundSplice Recordings");
    dir.createDirectory();
    return dir;
}

} // namespace soundsplice
