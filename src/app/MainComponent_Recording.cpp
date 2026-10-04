#include "MainComponentInternal.h"

#include "model/Takes.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Recording audio takes.

namespace soundsplice
{
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

/** Toggles between arming/starting a take and stopping it.

    The take streams straight to its file as it is played, so the destination
    and the track it will land on are both decided *here*, at arm time. They
    used to be decided when the take ended — which meant every recording became
    a new track at bar 1, however the project was set up when you hit record. */
void MainComponent::toggleRecording()
{
    if (! audioTake_.running)
    {
        // Permission is settled before a missing input is reported: a denied
        // microphone looks exactly like a device with no input channels from
        // here, and a one-click fix shouldn't be reported as missing hardware.
        if (! ensureMicrophoneAccess())
            return; // prompting, or already explained

        if (! engine_.hasAudioInput())
        {
            showError(engine_.inputOpenError().isNotEmpty()
                          ? "No audio input - check microphone permission "
                            "(System Settings > Privacy & Security > Microphone), then restart"
                          : "No audio input device to record from");
            return;
        }
    }

    if (! audioTake_.running)
    {
        // With Loop on and a time selection to loop, the take goes round it
        // and each pass becomes a take of one clip; with Punch Recording on,
        // it replaces only the selection. The transport still rolls from the
        // playhead, so there's a lead-up to play along to - except round a
        // loop, which starts inside it so there is a loop to go round. Before
        // arming, which reads where the playhead is.
        audioTake_       = {};
        audioTake_.range = app::recording::takeRange(timeSelection_, loopButton.getToggleState(),
                                                     settings_.getBoolValue("punchRecording", false));
        if (const auto seek = app::recording::loopTakeSeek(audioTake_.range, playheadBeat()))
            seekToBeat(*seek);

        // At this device's rate, which the silence to stop on is counted in.
        applySoundTrigger();

        // Which tracks the take goes onto: every armed audio track, each from
        // its own input, or - with none armed - the selected track if it can
        // hold audio, otherwise a new one.
        const auto targets = app::recording::takeTargets(history_.current(), armedTrackIds_, selectedTrackIndex_);

        // The extra tracks first: they're armed alongside the main take, which
        // is what starts the transport.
        for (size_t i = 1; i < targets.size() && (int) i <= engine::AudioEngine::kExtraTakes; ++i)
        {
            const int  slot  = (int) i - 1;
            const auto extra = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Recording", ".wav");
            if (engine_.beginExtraRecording(slot, extra, recordFormatFor(targets[i])))
                audioTake_.extras.push_back({ targets[i], slot });
        }

        // Into the saved project's audio folder, or the scratch folder until
        // there is one (see app/ProjectMedia.h).
        const auto file = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Recording", ".wav");
        engine_.setRecordFormat(recordFormatFor(targets.front()));

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
            engine_.stopRecording(); // the extras armed above
            audioTake_ = {};
            return;
        }

        recordingFile_        = file;
        audioTake_.mainTrack = targets.front();

        audioTake_.running = true;
        recordButton.setToggleState(true, juce::dontSendNotification); // swaps to the stop square
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));

        // The transport runs free for the length of a take. Looping would wrap
        // it at the end of what is already arranged, which is precisely where
        // a recording needs to keep going — you are recording the part that
        // isn't there yet. The button's own state is left alone and restored
        // when the take ends, so the user's setting survives. A loop
        // recording is the exception: going round is the point.
        post(Cmd::SetLooping, audioTake_.range.loop ? 1.0 : 0.0);
        post(Cmd::SetPlaying, 1.0);
        if (audioTake_.range.loop)
            showStatus("Loop recording - every pass round the selection becomes a take");
        else if (settings_.getBoolValue("soundActivated", false))
            showStatus("Waiting for sound - the take starts when the input passes the threshold");
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
    if (! audioTake_.running || ! engine_.isRecordingFinished())
        return;
    audioTake_.running = false;

    // However it ended. A take that stopped itself on silence also stops the
    // transport, as the Stop button would have.
    recordButton.setToggleState(false, juce::dontSendNotification);
    recordButton.setTooltip(withShortcut("Record", keys::record));
    if (engine_.recordingStoppedOnSilence())
        post(Cmd::SetPlaying, 0.0);

    // Every ending passes through here — the stop button, play/pause during a
    // take, or the engine finishing on its own — so this is where looping is
    // put back. Restoring it only in the stop button's handler would leave
    // loop silently off after any other route out, including an empty take
    // that returns just below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

    int64_t       dropped   = engine_.recordedDroppedSamples();
    const int64_t startedAt = engine_.recordedTakeStartSample();

    // Closes the file and hands it over; empty means nothing was captured.
    const auto file = engine_.finishRecordedTake();

    const int  latency = recordingLatencySamples();
    int        passes  = 0;
    bool       punched = false;
    int        placed  = 0;

    // One take onto its track: where the transport was when its capture
    // began (after any count-in), through the import path - which measures
    // the file and appends to the track - then late by the device's round
    // trip, as loop takes, or punched in, as the take was started.
    const auto place = [&](const juce::File& take, int64_t takeStart, int trackIndex)
    {
        const double startBeats = takeStart >= 0 ? juce::jmax(0.0, uiTempoMap_.ppqFromSamples(takeStart)) : 0.0;
        importAudioFileAtBeat(take, startBeats, trackIndex);

        const int takePasses = audioTake_.range.loop ? makeLoopTakesFromRecording(take, takeStart, latency) : 0;
        if (takePasses == 0)
            compensateRecordingLatency(take, latency);
        const bool takePunched = audioTake_.range.punch && punchRecordedClip(take);

        passes = juce::jmax(passes, takePasses);
        punched |= takePunched;
        ++placed;
    };

    if (file != juce::File{})
        place(file, startedAt, audioTake_.mainTrack);

    // Every other armed track's take, the same way onto its own track.
    for (const auto& extra : audioTake_.extras)
    {
        dropped += engine_.extraTakeDroppedSamples(extra.slot);
        const auto extraStart = engine_.extraTakeStartSample(extra.slot);
        const auto extraFile  = engine_.finishExtraTake(extra.slot);
        if (extraFile != juce::File{})
            place(extraFile, extraStart, extra.trackIndex);
    }

    audioTake_     = {};
    recordingFile_ = juce::File{};

    if (placed == 0)
    {
        showError("Recording was empty (no input captured)");
        return;
    }

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
    if (file != juce::File{} && isSilentAudioFile(file))
    {
        showError("Recorded silence - check microphone permission "
                  "(System Settings > Privacy & Security > Microphone) and the input device");
        return;
    }

    if (passes >= 2)
        showStatus("Recorded " + juce::String(passes) + " passes as takes - right-click the clip to choose one");
    else if (punched)
        showStatus("Punched in over the selection");
    else if (placed > 1)
        showStatus("Recorded " + juce::String(placed) + " tracks");
    else
        showStatus("Recorded: " + file.getFileName());
}

/** The record format for a take onto @p trackIndex: the Recording Format
    setting, from the track's own input when it has one. */
engine::AudioRecorder::Format MainComponent::recordFormatFor(int trackIndex)
{
    auto        format = savedRecordFormat();
    const auto& song   = history_.current();
    if (trackIndex >= 0 && trackIndex < (int) song.tracks.size())
    {
        const auto& track = song.tracks[(size_t) trackIndex];
        if (track.recordInput >= 0)
            format.firstInput = track.recordInput;
        if (track.recordChannels > 0)
            format.channels = track.recordChannels;
    }
    return format;
}

/** Arms or disarms a track for recording. Several can be armed: Record then
    takes each from its own input onto its own track. Not an edit - it says
    what the next take will do, not what the song is. */
void MainComponent::setTrackArmed(int trackIndex, bool armed)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];
    if (armed && track.type != model::TrackType::Audio)
    {
        showError("Only audio tracks record audio - arm an audio track");
        updateMixerStrips();
        return;
    }

    if (armed)
        armedTrackIds_.insert(track.id);
    else
        armedTrackIds_.erase(track.id);
    updateMixerStrips();

    if (audioTake_.running)
        joinOrLeaveTake(trackIndex, armed);
}

/** Arming or disarming a track while a take is running: armed, it joins the
    take from now, with a recorder of its own; disarmed, its recorder stops
    and what it recorded is placed with the rest when the take ends. */
void MainComponent::joinOrLeaveTake(int trackIndex, bool armed)
{
    if (! armed)
    {
        bool left = false;
        if (trackIndex == audioTake_.mainTrack && engine_.isMainTakeArmed())
        {
            engine_.stopMainTake();
            left = true;
        }
        for (const auto& extra : audioTake_.extras)
            if (extra.trackIndex == trackIndex && engine_.isExtraTakeArmed(extra.slot))
            {
                engine_.stopExtraTake(extra.slot);
                left = true;
            }
        if (left)
            showStatus("That track has stopped recording - the rest carry on");
        return;
    }

    // A recorder not already in this take.
    if (const auto slot = app::recording::freeRecorderSlot(audioTake_.extras, engine::AudioEngine::kExtraTakes))
    {
        const auto file = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Recording", ".wav");
        if (engine_.beginExtraRecording(*slot, file, recordFormatFor(trackIndex), true))
        {
            audioTake_.extras.push_back({ trackIndex, *slot });
            showStatus("That track has joined the take");
        }
        else
            showError("Could not start recording that track (the file could not be created)");
        return;
    }
    showError("Every recorder is in use - " + juce::String(engine::AudioEngine::kExtraTakes + 1) + " takes at once at most");
}

/** Which input a track records from: a menu of the device's inputs, and the
    app's default. Saved with the project. */
void MainComponent::chooseTrackInput(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track    = song.tracks[(size_t) trackIndex];
    const int   current  = track.recordInput;
    const int   channels = track.recordChannels;
    const auto  inputs   = engine_.inputChannelNames();
    const bool  stereo   = recordFormatFor(trackIndex).channels == 2;

    // Which input, and mono or stereo: each the app's choice unless the
    // track has its own.
    juce::PopupMenu menu;
    menu.addSectionHeader(stereo ? "Record from (and the input after it)" : "Record from");
    menu.addItem(1, "As in Recording Format", true, current < 0);
    for (int i = 0; i < inputs.size(); ++i)
        menu.addItem(100 + i, inputs[i], true, current == i);
    menu.addSeparator();
    menu.addItem(10, "Mono or stereo as in Recording Format", true, channels == 0);
    menu.addItem(11, "Mono", true, channels == 1);
    menu.addItem(12, "Stereo", true, channels == 2);

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackIndex](int result)
        {
            if (self == nullptr || result == 0)
                return;

            const bool isChannels = result >= 10 && result <= 12;
            const int  value      = isChannels ? result - 10 : (result == 1 ? -1 : result - 100);
            self->history_.edit(isChannels ? "Set track channels" : "Set track input",
                                [trackIndex, isChannels, value](model::Song& s)
            {
                if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
                    return;
                if (isChannels)
                    s.tracks[(size_t) trackIndex].recordChannels = value;
                else
                    s.tracks[(size_t) trackIndex].recordInput = value;
            });
            self->updateMixerStrips();
        });
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

    const double loopStart = (double) uiTempoMap_.samplesFromPpq(audioTake_.range.fromBeats) / rate;
    const double loopEnd   = (double) uiTempoMap_.samplesFromPpq(audioTake_.range.toBeats) / rate;
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
                                   audioTake_.range.fromBeats, audioTake_.range.toBeats - audioTake_.range.fromBeats);
    refreshAfterArrangementEdit();
    return (int) offsets.size();
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

    const int id = model::takeedit::punchIn(song, trackId, recorded, audioTake_.range.fromBeats, audioTake_.range.toBeats, 0.01);
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

/** Hands the Sound-Activated Recording setting to the engine, for the next
    take: a threshold to wait for, and a silence to stop on. */
void MainComponent::applySoundTrigger()
{
    const auto trigger = app::recording::soundTrigger(settings_.getBoolValue("soundActivated", false),
                                                      settings_.getDoubleValue("soundThresholdDb", -40.0),
                                                      settings_.getDoubleValue("soundStopSeconds", 0.0),
                                                      engine_.sampleRate());
    engine_.setSoundTrigger(trigger.thresholdGain, trigger.stopAfterSamples);
}

/** Sound-Activated Recording: whether a take waits for sound, how loud it
    has to be, and how long a silence ends it. */
void MainComponent::showSoundActivatedDialog()
{
    dialog("Sound-Activated Recording",
           "With this on, Record waits for the input to pass the threshold before the take starts, and can "
           "stop it by itself after a silence. Set the threshold a little above the room's noise on the "
           "input meter.")
        .choice("on", "Sound-activated:", { "On", "Off" }, settings_.getBoolValue("soundActivated", false) ? 0 : 1)
        .unsaved()
        .number("soundThresholdDb", "Threshold (dB):", -40.0, -90.0, 0.0)
        .number("soundStopSeconds", "Stop after this many seconds of silence (0: never):", 0.0, 0.0, 3600.0)
        .show("OK", [this](const FormDialog::Values& v)
        {
            const bool on = v.choice("on") == 0;
            settings_.setValue("soundActivated", on);
            settings_.saveIfNeeded();
            applySoundTrigger();
            showStatus(on ? "Takes start when the input passes " + juce::String(v.number("soundThresholdDb"), 1) + " dB"
                          : juce::String("Takes start when Record is pressed"));
        });
}

/** Timer Record: a take that starts at a set time and, if given a length,
    stops by itself. Asked again while one is waiting, it offers to cancel. */
void MainComponent::showTimerRecordDialog()
{
    if (timerRecord_.isPending())
    {
        timerRecord_.cancel();
        showStatus("Timer record cancelled");
        return;
    }

    dialog("Timer Record",
           "Start a take a while from now, and stop it after a set length. The app has to stay open, with "
           "the track to record onto selected.")
        .number("in", "Start in (minutes):", 5.0, 0.0, 1.0e6)
        .unsaved()
        .number("for", "Record for (minutes, 0: until stopped):", 0.0, 0.0, 1.0e6)
        .unsaved()
        .show("Start Timer", [this](const FormDialog::Values& v)
        {
            timerRecord_.schedule(juce::Time::currentTimeMillis(), v.number("in"), v.number("for"));
            const auto stop = timerRecord_.stopMs();
            showStatus("Recording starts at " + juce::Time(timerRecord_.startMs()).toString(false, true, false)
                       + (stop ? " and stops at " + juce::Time(*stop).toString(false, true, false) : juce::String()));
        });
}

/** From the UI timer: starts a timed take when its time comes, and stops it
    when its length is up. */
void MainComponent::tickTimerRecord()
{
    using Action = app::recording::TimerRecord::Action;
    if (timerRecord_.tick(juce::Time::currentTimeMillis(), audioTake_.running) != Action::None)
        toggleRecording(); // starts or stops, whichever is due
}

/** Append recording (Audacity's Shift+R): the take starts where the
    selected track's last clip ends, so it carries the track on - or where
    the song ends, for a track with nothing on it yet. */
void MainComponent::recordAtEndOfTrack()
{
    if (audioTake_.running)
        return;

    const auto& song = history_.current();
    double      end  = songEndBeats();
    if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
    {
        const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
        if (! clips.empty())
        {
            end = 0.0;
            for (const auto& clip : clips)
                end = juce::jmax(end, clip.startBeats + clip.lengthBeats);
        }
    }

    seekToBeat(end);
    recordButton.triggerClick();
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
    if (audioTake_.running)
    {
        showError("Stop recording first");
        return;
    }

    const auto format = savedRecordFormat();
    const auto inputs = engine_.inputChannelNames();

    // Each input by name; for stereo, the take is it and the next one.
    juce::StringArray inputChoices;
    for (int i = 0; i < juce::jmax(1, inputs.size()); ++i)
        inputChoices.add(inputs.isEmpty() ? juce::String("Input 1") : inputs[i]);

    // Shown as savedRecordFormat reads them; saved below as what they stand for.
    dialog("Recording Format",
           "What takes are recorded as, from the next one on. 24-bit is plenty for most things; 32-bit float "
           "can't clip in the file, which helps when levels are unknown. Inputs come from the device chosen "
           "in Audio Settings.")
        .choice("bits", "Bit depth:", { "16-bit", "24-bit", "32-bit float" },
                format.bitsPerSample <= 16 ? 0 : format.bitsPerSample >= 32 ? 2 : 1)
        .unsaved()
        .choice("channels", "Channels:", { "Mono", "Stereo" }, format.channels == 1 ? 0 : 1)
        .unsaved()
        .choice("input", "From input (stereo: it and the next):", inputChoices,
                juce::jmin(format.firstInput, inputChoices.size() - 1))
        .unsaved()
        .show("OK", [this](const FormDialog::Values& v)
        {
            static constexpr int kBits[] { 16, 24, 32 };
            const int bits     = kBits[v.choice("bits")];
            const int channels = v.choice("channels") == 0 ? 1 : 2;
            const int input    = v.choice("input");

            settings_.setValue("recordBits", bits);
            settings_.setValue("recordChannels", channels);
            settings_.setValue("recordFirstInput", input);
            settings_.saveIfNeeded();
            engine_.setRecordFormat(savedRecordFormat());
            showStatus("Recording " + juce::String(bits == 32 ? "32-bit float" : juce::String(bits) + "-bit")
                       + (channels == 1 ? " mono" : " stereo") + " from input " + juce::String(input + 1));
        });
}

/** How late a recording is: the device's reported round trip, adjusted by
    the Recording Latency setting (a driver's figure is often a little off),
    or nothing with compensation turned off. */
int MainComponent::recordingLatencySamples()
{
    return app::recording::latencySamples(latencySetting(), engine_.sampleRate(), engine_.reportedRoundTripSamples());
}

app::recording::Latency MainComponent::latencySetting() const
{
    app::recording::Latency latency;
    latency.compensate      = settings_.getBoolValue("compensateRecordingLatency", true);
    latency.measuredSamples = settings_.getIntValue("measuredLatencySamples", -1);
    latency.measuredRate    = settings_.getDoubleValue("measuredLatencyRate", 0.0);
    latency.adjustMs        = settings_.getDoubleValue("recordingLatencyAdjustMs", 0.0);
    return latency;
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
    clip.lengthBeats          = juce::jmax(0.0, clip.lengthBeats - model::clockFor(song).beatsAfter(clip.startBeats, seconds));
    refreshAfterArrangementEdit();
}

/** Measure Recording Latency: plays a click and times it coming back
    through a loopback cable (engine::LatencyProbe). The answer is kept for
    this rate and used in place of what the device reports. */
void MainComponent::measureRecordingLatency()
{
    if (engine_.isPlaying())
    {
        showError("Stop playback first");
        return;
    }

    auto options = juce::MessageBoxOptions()
                       .withIconType(juce::MessageBoxIconType::InfoIcon)
                       .withTitle("Measure Recording Latency")
                       .withMessage("Connect an output to an input with a cable, or turn on your interface's "
                                    "loopback, and turn your speakers down: a click plays once, and the time it "
                                    "takes to come back in is what recordings will be moved back by.")
                       .withButton("Measure")
                       .withButton("Cancel")
                       .withAssociatedComponent(this);

    juce::AlertWindow::showAsync(options, [self = juce::Component::SafePointer<MainComponent>(this)](int result)
    {
        if (self == nullptr || result != 1 || self->engine_.isMeasuringLatency())
            return;
        self->engine_.startLatencyMeasurement();
        self->measuringLatency_ = true;
        self->showStatus("Measuring...");
    });
}

/** From the UI timer: reports and keeps a measurement once it's done. */
void MainComponent::finishLatencyMeasurementIfReady()
{
    if (! measuringLatency_ || engine_.isMeasuringLatency())
        return;
    measuringLatency_ = false;

    const int    samples = engine_.measuredLatencySamples();
    const double rate    = engine_.sampleRate();
    if (samples < 0 || rate <= 0.0)
    {
        showError("The click didn't come back - check the cable runs from an output to an input that's switched "
                  "on in Audio Settings, and that the input level isn't all the way down");
        return;
    }

    settings_.setValue("measuredLatencySamples", samples);
    settings_.setValue("measuredLatencyRate", rate);
    settings_.saveIfNeeded();
    showStatus("Measured round trip: " + juce::String((double) samples * 1000.0 / rate, 2) + " ms (" + juce::String(samples)
               + " samples, the device reports " + juce::String(engine_.reportedRoundTripSamples())
               + ") - recordings use it now");
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
    const auto   latency      = latencySetting();
    const auto   measuredText = rate > 0.0 && app::recording::usesMeasured(latency, rate)
                                  ? juce::String((double) latency.measuredSamples * 1000.0 / rate, 1) + " ms"
                                  : juce::String();

    dialog("Recording Latency",
           "A recording comes back late by the time sound takes to leave the device and return to it. "
           "The device reports " + reportedText + (measuredText.isEmpty() ? juce::String() : "; measured, it's " + measuredText)
           + ". Recordings are moved back by " + (measuredText.isEmpty() ? "the reported figure" : "the measured one")
           + ", plus any adjustment below.\n\nFile > Measure Recording Latency times it through a cable "
             "from an output to an input.")
        .number("recordingLatencyAdjustMs", "Adjustment (ms, + moves recordings earlier):", 0.0, -500.0, 500.0)
        .choice("compensate", {}, { "Compensate recordings", "Leave recordings where they land" },
                settings_.getBoolValue("compensateRecordingLatency", true) ? 0 : 1)
        .unsaved()
        .show("OK", [this](const FormDialog::Values& v)
        {
            const bool on = v.choice("compensate") == 0;
            settings_.setValue("compensateRecordingLatency", on);
            settings_.saveIfNeeded();
            showStatus(on ? "Recordings are moved back by " + juce::String(recordingLatencySamples()) + " samples"
                          : juce::String("Recordings are left where they land"));
        });
}

juce::File MainComponent::recordingsDirectory() const
{
    // Where Preferences says, if it says anywhere that can be used.
    if (const juce::File chosen(settings_.getValue("paths.recordings")); chosen.isDirectory())
        return chosen;
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("SoundSplice Recordings");
    dir.createDirectory();
    return dir;
}

} // namespace soundsplice
