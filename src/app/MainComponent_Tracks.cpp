#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Tracks, clips, notes, the session grid and the mixer, and keeping the engine's
// tracks in step with the document.

namespace soundsplice
{
const engine::Pattern& MainComponent::currentPattern() const
{
    static const engine::Pattern empty;
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return empty;
    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return empty;
    return track.clips[(size_t) selectedClipIndex_].pattern;
}

void MainComponent::editPattern(const engine::Pattern& pattern)
{
    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    history_.edit("Edit notes", [&pattern, trackIdx, clipIdx](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx >= 0 && clipIdx < (int) clips.size())
            clips[(size_t) clipIdx].pattern = pattern;
    });
    syncEngineTracks(); // rebuilds every track's clip list, including this edit
}

void MainComponent::addTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    history_.edit("Add track", [](model::Song& s)
    {
        const auto name = "Synth " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Instrument, name.toStdString()).id;
        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(s, id, clip);
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Bar length in beats, from the current time signature. */
double MainComponent::beatsPerBar() const
{
    return juce::jmax(1.0, uiTempoMap_.quartersPerBar());
}

/** Adds a new clip to the currently selected track, positioned 2 beats after
    its last existing clip (or at beat 0 if it has none), and selects it for
    editing. */
void MainComponent::addClipToSelectedTrack()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    int       newClipIndex = -1;

    history_.edit("Add clip", [trackIdx, &newClipIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track = s.tracks[(size_t) trackIdx];

        double nextStart = 0.0;
        for (const auto& c : track.clips)
            nextStart = juce::jmax(nextStart, c.startBeats + c.lengthBeats);
        if (! track.clips.empty())
            nextStart += 2.0; // a small gap after the last clip

        model::Clip clip;
        clip.id                  = model::allocateId(s);
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = nextStart;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        track.clips.push_back(clip);
        newClipIndex = (int) track.clips.size() - 1;
    });

    if (newClipIndex < 0)
        return;

    selectedClipIndex_ = newClipIndex;
    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Redraws the session grid from the document. Which cells are *playing* is
    pushed separately from the engine each timer tick — see timerCallback —
    because a launch stays pending until the next bar line and the grid would
    otherwise light the wrong cell. */
void MainComponent::refreshSessionView()
{
    sessionView_.setSong(history_.current());
}

/** Adds a scene (a grid row), giving every track an empty slot in it. */
/** Removes a session row and every clip in it. Stops playback first: the
    slots the engine is holding are addressed by index, and the row below
    would inherit the index of the one that just went away. */
void MainComponent::deleteSessionScene(int sceneIndex)
{
    const auto& song = history_.current();
    if (sceneIndex < 0 || sceneIndex >= (int) song.scenes.size())
        return;

    const auto name = song.scenes[(size_t) sceneIndex].name;

    engine_.stopAllSessionSlots();

    history_.edit("Delete scene", [sceneIndex](model::Song& s) { model::removeScene(s, sceneIndex); });

    syncEngineTracks();
    refreshSessionView();

    // Bigger blast radius than a track deletion — every clip on every track
    // in the row — so it earns the same reassurance, not less.
    showStatus("Deleted \"" + juce::String(name) + "\" - undo to bring it back");
}

void MainComponent::addSessionScene()
{
    std::string name;
    history_.edit("Add scene", [&name](model::Song& s)
    {
        name = "Scene " + std::to_string(s.scenes.size() + 1);
        model::addScene(s, name);
    });

    syncEngineTracks();
    refreshSessionView();
    showStatus("Added \"" + juce::String(name) + "\"");
}

/** Clicking an empty cell fills it with a copy of the track's currently open
    clip — the quickest way to get material into the grid without a separate
    "new session clip" flow. Declines if there's nothing to copy. */
void MainComponent::captureClipIntoSession(int trackIndex, int sceneIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clips.empty())
    {
        showError("Nothing to capture - this track has no clips");
        return;
    }

    const int  sourceIndex = juce::jlimit(0, (int) clips.size() - 1,
                                          trackIndex == selectedTrackIndex_ ? selectedClipIndex_ : 0);
    const auto source      = clips[(size_t) sourceIndex];

    history_.edit("Add session clip", [trackIndex, sceneIndex, &source](model::Song& s)
    {
        model::setSessionClip(s, trackIndex, sceneIndex, source);
    });

    syncEngineTracks();
    refreshSessionView();
}
/** Copies the piano roll's selected notes, or the whole pattern if nothing
    is selected — the same "no selection means everything" rule quantize
    uses, so both commands are useful before the selection gesture is
    discovered. */
void MainComponent::copyNotes()
{
    const auto& pattern   = currentPattern();
    const auto& selection = pianoRoll_.selectedNoteIndices();

    noteClipboard_.clear();
    if (selection.empty())
    {
        noteClipboard_ = pattern.notes;
    }
    else
    {
        for (int index : selection)
            if (index >= 0 && index < (int) pattern.notes.size())
                noteClipboard_.push_back(pattern.notes[(size_t) index]);
    }

    showStatus("Copied " + juce::String((int) noteClipboard_.size()) + " note(s)");
}

/** Pastes notes into the open clip at the positions they were copied from,
    which is what makes "copy this part into that clip" work. Anything past
    the destination pattern's end is dropped rather than pasted somewhere it
    can't be seen or heard. */
void MainComponent::pasteNotes()
{
    if (noteClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    const auto notes   = noteClipboard_;

    history_.edit("Paste notes", [trackIdx, clipIdx, &notes](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (const auto& note : notes)
            if (note.startBeats < pattern.lengthBeats)
                pattern.notes.push_back(note);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
}

/** Copies the selected clip whole — pattern, length and all. */
void MainComponent::copyClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    clipClipboard_.assign(1, clips[(size_t) selectedClipIndex_]);
    showStatus("Copied clip");
}

/** Pastes onto the selected track at the playhead, snapped to a beat — the
    playhead is the one position the user can see, which makes where it lands
    predictable. */
void MainComponent::pasteClip()
{
    if (clipClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double dropBeat = std::round(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
    const int    trackIdx = selectedTrackIndex_;
    auto         pasted   = clipClipboard_.front();
    int          newIndex = -1;

    history_.edit("Paste clip", [trackIdx, dropBeat, &pasted, &newIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track  = s.tracks[(size_t) trackIdx];

        auto clip       = pasted;
        clip.id         = model::allocateId(s); // a paste is a new clip, not the same one twice
        clip.startBeats = juce::jmax(0.0, dropBeat);
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Copy + paste in one step, landing the copy immediately after the original
    — the usual way to extend a part by a bar. */
/** Whether selectedClipIndex_ currently names a real clip on the selected
    track — the same bounds check deleteSelectedClip() itself needs, pulled
    out so the bare-delete-key dispatcher (see keyPressed) can ask "is there
    a clip to act on" without duplicating it. */
bool MainComponent::hasSelectedClip() const
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return false;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    return selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) track.clips.size();
}

/** Deletes the selected arrangement clip. No confirmation: undo is the safety
    net for editing actions, and a prompt on every delete is friction the user
    pays for on the many times they meant it. */
void MainComponent::deleteSelectedClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return;

    const int trackId   = track.id;
    const int clipIndex = selectedClipIndex_;

    history_.edit("Delete clip", [trackId, clipIndex](model::Song& s)
    {
        model::removeClip(s, trackId, clipIndex);
    });

    // The clip after the deleted one shuffles down into its index; selecting
    // it keeps the selection somewhere real, and clamps at the end.
    const auto& clips = history_.current().tracks[(size_t) selectedTrackIndex_].clips;
    selectedClipIndex_ = clips.empty() ? 0 : juce::jmin(clipIndex, (int) clips.size() - 1);

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();

    // Reachable by a bare key and the most-used delete in the app, so it's
    // the one most likely to be hit by accident — same reasoning as track
    // deletion below.
    showStatus("Deleted clip - undo to bring it back");
}

/** Deletes the selected track and everything on it. Undo covers it, as with
    clips — but the last track isn't deletable, because a song with no tracks
    has no pane that can do anything and no obvious way back. */
void MainComponent::deleteSelectedTrack()
{
    deleteTrackAt(selectedTrackIndex_);
}

/** Deletes one track by index, which is not necessarily the selected one —
    the gear menu acts on the track whose gear was clicked.

    That is why the selection is fixed up through selectionAfterTrackRemoved
    rather than merely clamped: removing a track above the selected one shifts
    it down, and getting that wrong doesn't crash, it quietly leaves a
    different track selected than the one that was highlighted, so the next
    edit lands somewhere the user didn't mean. */
void MainComponent::deleteTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to delete");
        return;
    }

    if (song.tracks.size() <= 1)
    {
        showError("The last track can't be deleted");
        return;
    }

    const auto& track   = song.tracks[(size_t) trackIndex];
    const int   trackId = track.id;

    // Copied before the edit: committing one move-assigns the document, which
    // leaves any reference into the old one dangling.
    const auto name = track.name.empty() ? ("track " + juce::String(trackIndex + 1))
                                         : ("\"" + juce::String(track.name) + "\"");

    history_.edit("Delete track", [trackId](model::Song& s) { model::removeTrack(s, trackId); });

    selectTrackAndRefreshAll(selectionAfterTrackRemoved(selectedTrackIndex_, trackIndex,
                                                        trackCount()));

    // Deleting is reachable by an unmodified key and by one menu click, so it
    // can be hit by accident. Saying what went and that undo will bring it
    // back is the difference between a recoverable slip and a mystery.
    showStatus("Deleted " + name + " - undo to bring it back");
}

/** Renames the selected track. Track names are the only label distinguishing
    one strip, row or tab from the next, and until now they were whatever the
    Add button happened to generate. */
void MainComponent::renameSelectedTrack()
{
    renameTrackAt(selectedTrackIndex_);
}

/** Copies a track and everything on it, putting the copy directly after it.

    The point of duplicating a track here is a second copy of a part to loop
    against the first, so the copy has to be complete — clips, instrument
    settings, effect chain and all — and it has to be genuinely separate,
    which is what model::duplicateTrack's reissuing of every id gives. */
void MainComponent::duplicateTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to duplicate");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto sourceName = juce::String(song.tracks[(size_t) trackIndex].name);

    history_.edit("Duplicate track", [trackIndex](model::Song& s)
    {
        model::duplicateTrack(s, trackIndex);
    });

    selectTrackAndRefreshAll(trackIndex + 1); // the copy, so it can be worked on straight away
    showStatus("Duplicated " + (sourceName.isEmpty() ? juce::String("track") : "\"" + sourceName + "\""));
}

/** Makes the selected audio track a left track and a right track, without
    writing any audio: each only changes which channel its clips play. */
void MainComponent::splitSelectedTrackToMono()
{
    const int   index  = selectedTrackIndex_;
    const auto& tracks = history_.current().tracks;
    if (index < 0 || index >= (int) tracks.size() || tracks[(size_t) index].type != model::TrackType::Audio)
    {
        showError("Select an audio track first");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    history_.edit("Split stereo to mono", [index](model::Song& s)
    {
        model::channelops::splitStereoToMono(s, index);
    });

    selectTrackAndRefreshAll(index);
    showStatus("Split into a left track and a right track");
}

void MainComponent::swapSelectedTrackChannels()
{
    const int   index  = selectedTrackIndex_;
    const auto& tracks = history_.current().tracks;
    if (index < 0 || index >= (int) tracks.size() || tracks[(size_t) index].type != model::TrackType::Audio)
    {
        showError("Select an audio track first");
        return;
    }

    history_.edit("Swap channels", [index](model::Song& s)
    {
        model::channelops::swapChannels(s, index);
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    showStatus("Swapped left and right");
}

/** Make Stereo Track, as in Audacity: the selected audio track becomes the
    left channel and the track below it the right, as one track in their place.

    Two halves of a split go back together as the track they came from, with
    nothing rendered. Any other pair has no single file to play from, so each
    track is rendered as a stem is (its clips, effects and gain, before the
    master bus), folded back to mono through its pan, and the two written as
    one stereo file. That bakes in their effects, so the new track starts with
    none, at unity and centred. Pan is folded at its fixed value; a pan curve
    isn't followed. */
void MainComponent::makeStereoTrack()
{
    const int   index  = selectedTrackIndex_;
    const auto& song   = history_.current();
    const auto& tracks = song.tracks;
    if (index < 0 || index + 1 >= (int) tracks.size() || tracks[(size_t) index].type != model::TrackType::Audio
        || tracks[(size_t) index + 1].type != model::TrackType::Audio)
    {
        showError("Select an audio track with an audio track below it");
        return;
    }

    if (model::channelops::areSplitHalves(song, index))
    {
        history_.edit("Make stereo track", [index](model::Song& s)
        {
            model::channelops::joinSplitHalves(s, index);
        });

        selectTrackAndRefreshAll(index);
        showStatus("Joined the left and right tracks back into one");
        return;
    }

    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    if (index + 1 >= engine_.maxTracks() || ! engine_.trackContributesToMix(index)
        || ! engine_.trackContributesToMix(index + 1))
    {
        showError("Both tracks need to be heard - unmute them, or clear the solo");
        return;
    }

    const double rate = engine_.sampleRate();
    if (rate <= 0.0)
    {
        showError("Rendering needs an audio device - choose one in Audio Settings");
        return;
    }

    double startBeats = 0.0;
    double endBeats   = 0.0;
    bool   any        = false;
    for (int i = index; i <= index + 1; ++i)
        for (const auto& clip : tracks[(size_t) i].clips)
        {
            const double end = clip.startBeats + clip.lengthBeats;
            startBeats       = any ? juce::jmin(startBeats, clip.startBeats) : clip.startBeats;
            endBeats         = any ? juce::jmax(endBeats, end) : end;
            any              = true;
        }

    if (! any || endBeats <= startBeats)
    {
        showError("Nothing on those tracks to join");
        return;
    }

    const auto&  left    = tracks[(size_t) index];
    const auto&  right   = tracks[(size_t) index + 1];
    const float  pans[2] = { left.pan, right.pan };
    const auto   name    = left.name.empty() ? std::string("Stereo")
                                             : model::channelops::detail::withoutSideSuffix(left.name);
    const int    leftId  = left.id;
    const int    rightId = right.id;
    const double length  = endBeats - startBeats;
    const auto   file    = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile("Stereo", ".wav");
    auto         written = std::make_shared<bool>(false);

    // The engine belongs to the render thread for the duration, as for an export.
    offlineRenderInProgress_ = true;

    auto work = [this, index, startBeats, length, rate, file, written, pans](app::OfflineRenderJob& job)
    {
        juce::AudioBuffer<float> stereo;

        for (int side = 0; side < 2; ++side)
        {
            engine::AudioEngine::OfflineRenderOptions options;
            options.startBeats     = startBeats;
            options.lengthBeats    = length;
            options.soloTrack      = index + side;
            options.applyMasterBus = false;
            options.onProgress     = [&job, side](double fraction)
            {
                job.report(app::overallProgress(side, 2, fraction),
                           side == 0 ? "Rendering the left track" : "Rendering the right track");
                return ! job.shouldAbort();
            };

            const auto buffer = engine_.renderOffline(options);
            if (buffer.getNumSamples() == 0 || job.shouldAbort())
                return; // cancelled

            if (stereo.getNumSamples() == 0)
            {
                stereo.setSize(2, buffer.getNumSamples());
                stereo.clear();
            }

            const float leftGain  = engine::InstrumentTrack::panGainFor(0, pans[side]);
            const float rightGain = engine::InstrumentTrack::panGainFor(1, pans[side]);
            const int   samples   = juce::jmin(stereo.getNumSamples(), buffer.getNumSamples());
            const auto* inLeft    = buffer.getReadPointer(0);
            const auto* inRight   = buffer.getReadPointer(juce::jmin(1, buffer.getNumChannels() - 1));
            auto*       out       = stereo.getWritePointer(side);

            for (int n = 0; n < samples; ++n)
                out[n] = model::channelops::foldToMono(inLeft[n], inRight[n], leftGain, rightGain);
        }

        *written = stereo.getNumSamples() > 0 && engine::OfflineRenderer::writeWav(file, stereo, rate);
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), written, file, startBeats, length,
                       leftId, rightId, name](bool cancelled)
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
                self->showStatus("Make stereo track cancelled");
            else
                self->showError("Could not write " + file.getFileName());
            return;
        }

        const auto path     = file.getFullPathName().toStdString();
        int        newIndex = -1;

        // By id: the render ran behind a modal progress window, but ids are
        // what edits address tracks by everywhere else too.
        self->history_.edit("Make stereo track", [&](model::Song& s)
        {
            int at = -1;
            for (int i = 0; i < (int) s.tracks.size(); ++i)
                if (s.tracks[(size_t) i].id == leftId)
                    at = i;
            if (at < 0)
                return;

            model::Track track;
            track.id   = model::allocateId(s);
            track.name = name;
            track.type = model::TrackType::Audio;
            track.sessionSlots.resize(s.scenes.size());

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = startBeats;
            clip.lengthBeats = length;
            clip.audioFile   = path;
            track.clips.push_back(clip);

            s.tracks[(size_t) at] = std::move(track);
            s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(),
                                          [rightId](const model::Track& t) { return t.id == rightId; }),
                           s.tracks.end());
            newIndex = (int) std::min((size_t) at, s.tracks.size() - 1);
        });

        if (newIndex < 0)
        {
            file.deleteFile();
            self->showError("The tracks were removed while rendering");
            return;
        }

        self->selectTrackAndRefreshAll(newIndex);
        self->showStatus("Made a stereo track");
    };

    renderJob_ = app::OfflineRenderJob::launch("Make Stereo Track", std::move(work), std::move(onFinished));
}

/** Copies the selected track for later pasting. Deliberately its own
    clipboard rather than sharing the clip one: pasting a track when a clip
    was copied, or the reverse, is the kind of guess that loses work. */
void MainComponent::copyTrack()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        showError("No track selected");
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    trackClipboard_   = track;

    showStatus("Copied " + (track.name.empty() ? juce::String("track")
                                               : "\"" + juce::String(track.name) + "\""));
}

void MainComponent::pasteTrack()
{
    if (! trackClipboard_.has_value())
    {
        showError("No track copied");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto copied = *trackClipboard_;

    // appendTrackCopy reissues every id, so pasting the same buffer twice
    // gives two genuinely separate tracks rather than two the app can't tell
    // apart.
    history_.edit("Paste track", [&copied](model::Song& s) { model::appendTrackCopy(s, copied); });

    selectTrackAndRefreshAll(trackCount() - 1);
    showStatus("Pasted " + (copied.name.empty() ? juce::String("track")
                                                : "\"" + juce::String(copied.name) + "\""));
}

/** The per-track settings menu, opened from the gear in the tracks pane. */
void MainComponent::showTrackSettingsMenu(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];

    juce::PopupMenu colours;
    for (int i = 0; i < kNumTrackColours; ++i)
    {
        const auto& option = kTrackColours[i];
        const bool  chosen = (track.colour == option.argb);

        // Ticked rather than swatched: PopupMenu has no colour-chip item, and
        // a tick at least says which one is in force.
        colours.addItem(kFirstColourMenuId + i, option.name, true, chosen);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader(track.name.empty() ? ("Track " + juce::String(trackIndex + 1))
                                             : juce::String(track.name));
    menu.addSubMenu("Colour", colours);
    menu.addItem(1, "Rename...");
    menu.addSeparator();

    // Greyed rather than absent when it's the last track: an item that isn't
    // there reads as a missing feature, where a disabled one says the rule.
    menu.addItem(2, "Delete Track", song.tracks.size() > 1);

    juce::Component::SafePointer<MainComponent> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [self, trackIndex](int result)
    {
        if (self == nullptr || result == 0)
            return;

        if (result == 1)
        {
            self->renameTrackAt(trackIndex);
            return;
        }

        if (result == 2)
        {
            self->deleteTrackAt(trackIndex);
            return;
        }

        if (const int index = result - kFirstColourMenuId; index >= 0 && index < kNumTrackColours)
            self->setTrackColour(trackIndex, kTrackColours[index].argb);
    });
}

/** Colour is document state, so it's an undoable edit rather than a live
    tweak — unlike mute, which is a performance control you flip while
    listening and would not want filling the undo stack. */
void MainComponent::setTrackColour(int trackIndex, unsigned int argb)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;

    history_.edit("Recolour track", [trackId, argb](model::Song& s)
    {
        if (auto* track = model::findTrack(s, trackId))
            track->colour = argb;
    });

    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

/** Moves an arrangement clip from one track to another, in place of a
    same-track reposition (see ArrangementView::onClipMovedToTrack — this
    only ever fires when the drop landed on a track the view already
    confirmed is compatible, but the check is repeated here rather than
    trusted, since the model layer shouldn't rely on a UI-side gate alone).

    Composed from the same two model primitives every other clip edit uses —
    erase from the source track's clips, model::addClip into the
    destination (which reissues the id) — as one history_.edit, so a
    cross-track drag is one undo step just like a same-track one. */
void MainComponent::moveClipToTrack(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats)
{
    const auto& song = history_.current();
    if (srcTrackIndex < 0 || srcTrackIndex >= (int) song.tracks.size())
        return;
    if (destTrackIndex < 0 || destTrackIndex >= (int) song.tracks.size())
        return;

    const auto& srcTrack = song.tracks[(size_t) srcTrackIndex];
    if (clipIndex < 0 || clipIndex >= (int) srcTrack.clips.size())
        return;

    const auto& destTrack = song.tracks[(size_t) destTrackIndex];
    if (srcTrack.type != destTrack.type || srcTrack.type == model::TrackType::Audio)
        return;

    const int srcTrackId  = srcTrack.id;
    const int destTrackId = destTrack.id;
    int       newClipIndex = -1;

    history_.edit("Move clip to track", [srcTrackId, destTrackId, clipIndex, newStartBeats, &newClipIndex](model::Song& s)
    {
        auto* source = model::findTrack(s, srcTrackId);
        if (source == nullptr || clipIndex < 0 || clipIndex >= (int) source->clips.size())
            return;

        model::Clip clip = source->clips[(size_t) clipIndex]; // copied before erase invalidates the reference
        source->clips.erase(source->clips.begin() + clipIndex);
        clip.startBeats = juce::jmax(0.0, newStartBeats);

        if (model::addClip(s, destTrackId, clip) != nullptr)
        {
            if (auto* dest = model::findTrack(s, destTrackId))
                newClipIndex = (int) dest->clips.size() - 1;
        }
    });

    if (newClipIndex < 0)
        return; // the edit above bailed out (a stale index) - nothing to select or refresh

    selectedTrackIndex_ = destTrackIndex;
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
}

void MainComponent::renameTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track   = song.tracks[(size_t) trackIndex];
    const int   trackId = track.id;

    auto* window = new juce::AlertWindow("Rename Track", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", juce::String(track.name), "Name:");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window, trackId](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                const auto name = window->getTextEditorContents("name").trim();
                if (name.isEmpty())
                    return; // an unnamed track is worse than the generated name

                self->history_.edit("Rename track", [trackId, name](model::Song& s)
                {
                    model::renameTrack(s, trackId, name.toStdString());
                });

                self->syncEngineTracks();
                self->updateMixerStrips();
                self->refreshSessionView();
                self->arrangementView_.setSong(self->history_.current());
                self->updateEditingLabel();
            }),
        true);
}

void MainComponent::duplicateClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const auto source   = clips[(size_t) selectedClipIndex_];
    const int  trackIdx = selectedTrackIndex_;
    int        newIndex = -1;

    history_.edit("Duplicate clip", [trackIdx, &source, &newIndex](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIdx];

        auto clip       = source;
        clip.id         = model::allocateId(s);
        clip.startBeats = source.startBeats + source.lengthBeats;
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Snaps the open clip's notes onto the grid, optionally swung. Acts on the
    piano roll's selection, or the whole pattern when nothing is selected. */
void MainComponent::quantizeNotes(double swingAmount)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& song = history_.current();
    if (selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const int  trackIdx  = selectedTrackIndex_;
    const int  clipIdx   = selectedClipIndex_;
    const auto selection = pianoRoll_.selectedNoteIndices();
    const int  affected  = selection.empty()
                              ? (int) clips[(size_t) clipIdx].pattern.notes.size()
                              : (int) selection.size();

    if (affected == 0)
    {
        showStatus("Nothing to " + juce::String(swingAmount > 0.0 ? "swing" : "quantize")
                   + " - this clip has no notes");
        return;
    }

    history_.edit(swingAmount > 0.0 ? "Swing" : "Quantize",
                  [trackIdx, clipIdx, swingAmount, &selection](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& trackClips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) trackClips.size())
            return;

        // The grid the editor draws is 16ths, so that's what notes snap to.
        engine::NoteOps::quantizeNotes(trackClips[(size_t) clipIdx].pattern.notes, 0.25, swingAmount, selection);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();

    // Reloading the pattern clears the selection, which would silently widen
    // a follow-up Swing to the whole part. Quantizing never adds, removes or
    // reorders notes, so the same indices still mean the same notes.
    pianoRoll_.setSelectedNoteIndices(selection);

    showStatus((swingAmount > 0.0 ? "Swung " : "Quantized ") + juce::String(affected)
               + (affected == 1 ? " note" : " notes"));
}

/** Sets a clip's window on the timeline (from the arrangement's resize
    handle). Note this is the window, not the pattern's loop length — see
    setPatternBars. A track holding a *single* clip is still given an
    unbounded window by syncEngineTracks (the long-standing "one clip plays
    until Stop" rule), so resizing a lone clip changes what you see and what
    gets exported, but not when it stops sounding; that only bites once the
    track has more than one clip. */
void MainComponent::setClipLength(int trackIndex, int clipIndex, double newLengthBeats)
{
    history_.edit("Resize clip", [trackIndex, clipIndex, newLengthBeats](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].lengthBeats = juce::jmax(1.0, newLengthBeats);
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Moves an audio clip's left edge (from the arrangement's left resize
    handle) without moving its audio — see trimClipStart. The file is never
    touched, so dragging the edge back out brings the audio back. */
void MainComponent::trimClipStartTo(int trackIndex, int clipIndex, double newStartBeats)
{
    const double bpm = history_.current().bpm;

    history_.edit("Trim clip start", [trackIndex, clipIndex, newStartBeats, bpm](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIndex];
        if (clip.type == model::ClipType::Audio)
            clip = trimClipStart(clip, newStartBeats, bpm, kMinTrimmedClipBeats);
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Slides an audio clip's audio inside its edges (a Ctrl-drag in the
    arrangement) by changing where in its file it starts playing. The clip
    stays put and the file is untouched; its volume curve, kept in file time,
    moves with the audio, while its fades stay on its edges. */
void MainComponent::slipClipTo(int trackIndex, int clipIndex, double newOffsetSeconds)
{
    history_.edit("Slip clip", [trackIndex, clipIndex, newOffsetSeconds](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIndex];
        if (clip.type == model::ClipType::Audio)
            clip.sourceOffsetSeconds = std::max(0.0, newOffsetSeconds);
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Replaces an audio clip's fades as one undo step, from dragging a fade
    handle or picking a shape. The file is never touched: fades are applied
    as the clip plays. */
void MainComponent::setClipFades(int trackIndex, int clipIndex, const engine::ClipFades& fades,
                                 const juce::String& label)
{
    history_.edit(label.toStdString(), [trackIndex, clipIndex, fades](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIndex];
        if (clip.type != model::ClipType::Audio)
            return;

        clip.fades            = fades;
        clip.fades.inSeconds  = juce::jmax(0.0, fades.inSeconds);
        clip.fades.outSeconds = juce::jmax(0.0, fades.outSeconds);
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
}

/** The right-click menu on an audio clip: a shape for each fade, and a way
    to remove both. The two ends get separate shapes because they are used
    differently: an equal-power fade suits a crossfade, while an S-curve
    suits a clean start or finish. */
void MainComponent::showClipMenu(int trackIndex, int clipIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clipIndex < 0 || clipIndex >= (int) clips.size()
        || clips[(size_t) clipIndex].type != model::ClipType::Audio)
        return;

    const auto fades = clips[(size_t) clipIndex].fades;

    static constexpr std::pair<engine::FadeShape, const char*> kShapes[] {
        { engine::FadeShape::Linear,     "Linear" },
        { engine::FadeShape::EqualPower, "Equal Power" },
        { engine::FadeShape::SCurve,     "S-Curve" }
    };
    static constexpr int kFadeInBase  = 100;
    static constexpr int kFadeOutBase = 200;
    static constexpr int kRemoveFades = 1;

    juce::PopupMenu fadeInMenu, fadeOutMenu;
    for (int i = 0; i < (int) std::size(kShapes); ++i)
    {
        fadeInMenu.addItem(kFadeInBase + i, kShapes[i].second, true, fades.inShape == kShapes[i].first);
        fadeOutMenu.addItem(kFadeOutBase + i, kShapes[i].second, true, fades.outShape == kShapes[i].first);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader("Drag a clip's top corners to fade it");
    menu.addSubMenu("Fade In Shape", fadeInMenu);
    menu.addSubMenu("Fade Out Shape", fadeOutMenu);
    menu.addItem(kRemoveFades, "Remove Fades", ! fades.isNone());

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackIndex, clipIndex, fades](int result)
        {
            if (self == nullptr || result == 0)
                return;

            auto         updated = fades;
            juce::String label;

            if (result == kRemoveFades)
            {
                updated.inSeconds  = 0.0;
                updated.outSeconds = 0.0;
                label = "Remove clip fades";
            }
            else if (result >= kFadeOutBase)
            {
                updated.outShape = kShapes[(size_t) juce::jlimit(0, (int) std::size(kShapes) - 1,
                                                                 result - kFadeOutBase)].first;
                label = "Set fade-out shape";
            }
            else
            {
                updated.inShape = kShapes[(size_t) juce::jlimit(0, (int) std::size(kShapes) - 1,
                                                                result - kFadeInBase)].first;
                label = "Set fade-in shape";
            }

            self->setClipFades(trackIndex, clipIndex, updated, label);
        });
}

/** Sets how many bars the open clip's pattern loops over. Growing the pattern
    also grows the clip's window if the window would otherwise be too short to
    contain it — keeping a clip able to hold its own content isn't the same as
    silently re-looping it, which is why the window is only ever grown here,
    never shrunk. */
void MainComponent::setPatternBars(int bars)
{
    if (bars <= 0 || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const double lengthBeats = beatsPerBar * bars;
    const int    trackIdx    = selectedTrackIndex_;
    const int    clipIdx     = selectedClipIndex_;

    history_.edit("Set pattern length", [trackIdx, clipIdx, lengthBeats](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIdx];
        clip.pattern.lengthBeats = lengthBeats;
        clip.lengthBeats         = juce::jmax(clip.lengthBeats, lengthBeats);

        // Notes now past the end would be unreachable in the editor and
        // silent in the sequencer, so drop them rather than leave them
        // invisibly attached to the clip.
        auto& notes = clip.pattern.notes;
        notes.erase(std::remove_if(notes.begin(), notes.end(),
                                   [lengthBeats](const engine::Note& n) { return n.startBeats >= lengthBeats; }),
                    notes.end());
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Mirrors the open clip's pattern length into the Bars box. */
void MainComponent::updateBarsControl()
{
    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const auto&  pattern     = currentPattern();
    const int    bars        = juce::jmax(1, (int) std::llround(pattern.lengthBeats / beatsPerBar));

    // Only reflects lengths the box actually offers; an odd length set
    // elsewhere leaves it blank rather than silently rounding the clip.
    barsBox_.setSelectedId(bars == 1 || bars == 2 || bars == 4 ? bars : 0, juce::dontSendNotification);
}

void MainComponent::syncEngineTracks()
{
    const auto& song = history_.current();
    const int   n    = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < n; ++i)
    {
        const auto& track = song.tracks[(size_t) i];

        std::vector<engine::ClipSlot> slots;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Instrument)
                continue; // audio clips aren't sequenced

            engine::ClipSlot slot;
            slot.pattern     = clip.pattern;
            slot.startBeats  = clip.startBeats;
            slot.lengthBeats = clip.lengthBeats;
            slots.push_back(slot);
        }
        engine_.setTrackClips(i, slots);

        // Audio clips -> the track's own audio-clip player. Each Audio-type
        // clip becomes one AudioClipSlot, gated to its own
        // [startBeats, startBeats+lengthBeats) window exactly like the
        // instrument clips above. Unconditionally resubmitted every sync,
        // same as instrument clips — cheap, since AudioEngine caches decoded
        // audio by file path (see AudioEngine::setTrackAudioClips), so this
        // never re-decodes a file it's already loaded, even across tracks
        // that share one.
        std::vector<engine::AudioClipSpec> audioSpecs;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            engine::AudioClipSpec spec;
            spec.file        = juce::File(clip.audioFile);
            spec.startBeats  = clip.startBeats;
            spec.lengthBeats = clip.lengthBeats;
            spec.gainDb      = clip.gainDb;
            spec.sourceOffsetSeconds = clip.sourceOffsetSeconds;
            spec.fades               = clip.fades;
            spec.channels            = clip.channels;
            spec.envelope            = clip.envelope;
            audioSpecs.push_back(spec);
        }
        // Submitted even when empty, which the guard here used to skip: the
        // engine holds the last list it was given, so deleting a track's only
        // audio clip left that clip still loaded and still playing, with
        // nothing on screen to explain it. "Unconditionally resubmitted" in
        // the comment above is only true if it's also submitted when there's
        // nothing to submit.
        engine_.setTrackAudioClips(i, audioSpecs);

        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackSolo(i, track.solo);
        engine_.setTrackGainDb(i, track.gainDb);
        engine_.setTrackPan(i, track.pan);
        engine_.setTrackAutomation(i, toTrackAutomation(track));

        // The session grid's column for this track. Empty slots are submitted
        // too — the index is the scene, so the list has to stay aligned with
        // Song::scenes even where there's nothing to play.
        std::vector<engine::SessionSlotData> sessionSlots;
        sessionSlots.reserve(track.sessionSlots.size());
        for (const auto& slot : track.sessionSlots)
        {
            engine::SessionSlotData data;
            data.hasClip = slot.hasClip && slot.clip.type == model::ClipType::Instrument;
            if (data.hasClip)
                data.pattern = slot.clip.pattern;
            sessionSlots.push_back(std::move(data));
        }
        engine_.setTrackSessionSlots(i, sessionSlots);

        const auto& synth = track.synthSettings;
        engine_.setTrackSynthWaveform(i, synth.waveform);
        engine_.setTrackSynthAttackMs(i, synth.attackMs);
        engine_.setTrackSynthDecayMs(i, synth.decayMs);
        engine_.setTrackSynthSustain(i, synth.sustain);
        engine_.setTrackSynthReleaseMs(i, synth.releaseMs);
        engine_.setTrackSynthFilterEnabled(i, synth.filterEnabled);
        engine_.setTrackSynthFilterMode(i, synth.filterMode);
        engine_.setTrackSynthFilterCutoff(i, synth.filterCutoff);
        engine_.setTrackSynthFilterResonance(i, synth.filterResonance);
        engine_.setTrackSynthGainDb(i, synth.gainDb);
        engine_.setTrackSynthFilterEnvAmount(i, synth.filterEnvAmount);
        engine_.setTrackSynthFilterEnvAttackMs(i, synth.filterEnvAttackMs);
        engine_.setTrackSynthFilterEnvDecayMs(i, synth.filterEnvDecayMs);
        engine_.setTrackSynthFilterEnvSustain(i, synth.filterEnvSustain);
        engine_.setTrackSynthFilterEnvReleaseMs(i, synth.filterEnvReleaseMs);
        engine_.setTrackSynthSubOscEnabled(i, synth.subOscEnabled);
        engine_.setTrackSynthSubOscLevel(i, synth.subOscLevel);
        engine_.setTrackSynthUnisonVoices(i, synth.unisonVoices);
        engine_.setTrackSynthUnisonDetuneCents(i, synth.unisonDetuneCents);

        // The chain's shape, in order. Only pushed when it actually changed —
        // rebuilding resets every tail in the chain, so an unrelated edit must
        // not glitch a delay (see AudioEngine::setTrackEffectChain).
        std::vector<engine::EffectSlotSpec> chainSpecs;
        chainSpecs.reserve(track.effectChain.size());
        for (const auto& slot : track.effectChain)
        {
            engine::EffectSlotSpec spec;
            switch (slot.kind)
            {
                case model::EffectKind::Filter: spec.kind = engine::EffectNodeKind::Filter; break;
                case model::EffectKind::Delay:  spec.kind = engine::EffectNodeKind::Delay;  break;
                case model::EffectKind::Reverb: spec.kind = engine::EffectNodeKind::Reverb; break;
                case model::EffectKind::Drive:  spec.kind = engine::EffectNodeKind::Drive;  break;
                case model::EffectKind::Compressor: spec.kind = engine::EffectNodeKind::Compressor; break;
                case model::EffectKind::Tremolo:    spec.kind = engine::EffectNodeKind::Tremolo;    break;
                case model::EffectKind::Chorus:     spec.kind = engine::EffectNodeKind::Chorus;     break;
                case model::EffectKind::Wobble:     spec.kind = engine::EffectNodeKind::Wobble;     break;
                case model::EffectKind::Gate:       spec.kind = engine::EffectNodeKind::Gate;       break;
                case model::EffectKind::Eq:         spec.kind = engine::EffectNodeKind::Eq;         break;
                case model::EffectKind::Plugin:
                    spec.kind             = engine::EffectNodeKind::Plugin;
                    spec.pluginFormat     = pluginFormatName(slot.plugin.format);
                    spec.pluginIdentifier = slot.plugin.identifier;
                    spec.pluginState      = slot.plugin.state;
                    break;
            }
            chainSpecs.push_back(std::move(spec));
        }
        // A rebuild destroys this track's nodes, hosted plugins included, so
        // any editor drawing one has to go first. Only on an actual rebuild —
        // closing plugin windows on every unrelated edit would be maddening.
        if (engine_.setTrackEffectChain(i, chainSpecs))
            closePluginEditors();

        // Parameters, one call per slot, addressed by position — a chain may
        // hold two filters, and "the filter" stops meaning anything then.
        for (size_t s = 0; s < track.effectChain.size(); ++s)
            engine_.setTrackEffectSlotParams(i, (int) s, engine::toSlotParams(track.effectChain[s]));
    }
    engine_.setActiveTrackCount(n);

    // The arrangement just changed, so the loop it runs over has too. This is
    // the one place every clip edit passes through, which is why it lives
    // here rather than in each of them.
    updateLoopRegion();
}

void MainComponent::refreshPianoRollForSelected()
{
    pianoRoll_.setPattern(currentPattern());
    updateBarsControl();

    // The same condition currentPattern() falls back to its shared empty
    // Pattern for — the roll can't otherwise tell "nothing is open" apart
    // from "a real clip that's genuinely empty."
    const auto& song = history_.current();
    const bool  noClipOpen = selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size()
                          || selectedClipIndex_ < 0
                          || selectedClipIndex_ >= (int) song.tracks[(size_t) selectedTrackIndex_].clips.size();
    pianoRoll_.setNoClipSelected(noClipOpen);

    if (! noClipOpen)
    {
        const auto& track = song.tracks[(size_t) selectedTrackIndex_];
        pianoRoll_.setTrackInfo(track.name, track.colour);
    }
}

/** Hands the automation pane the selected track's lane for whichever
    parameter it is showing. */
void MainComponent::refreshAutomationPaneForSelected()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        automationPane_.setNoTrackSelected();
        return;
    }

    const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
    const auto  param = automationPane_.param();

    // A track with no lane for this parameter gets an empty one rather than
    // nothing: an empty lane is a real state (no automation, sitting at the
    // static value) and is the one you start drawing into.
    const auto* lane = track.lane(param);

    automationPane_.setLane(track.name.empty()
                                ? ("Track " + juce::String(selectedTrackIndex_ + 1))
                                : juce::String(track.name),
                            track.type,
                            lane != nullptr ? *lane : model::AutomationLane {},
                            juce::jmax(16.0, songEndBeats()));
}

/** Commits a lane edited in the automation pane.

    One undo step per gesture, not per breakpoint: the pane reports the whole
    lane when a drag ends, which is the same "a drag is one edit" rule the
    mixer faders follow. */
void MainComponent::applyEditedAutomationLane(model::TrackParam param,
                                              const model::AutomationLane& lane)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;

    history_.edit("Edit automation", [index, param, &lane](model::Song& s)
    {
        if (index < 0 || index >= (int) s.tracks.size())
            return;

        auto& track = s.tracks[(size_t) index];

        // An emptied lane is erased rather than stored empty, so a track with
        // no automation carries no lanes at all — the state every serialization
        // and playback path already treats as "use the static value".
        if (lane.empty())
            track.automation.erase((int) param);
        else
            track.laneFor(param) = lane;
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
}

/** Briefly sounds @p noteNumber through whichever track is currently armed —
    the same live-MIDI path the on-screen keyboard already uses (see
    InstrumentTrack::render's receivesLiveMidi routing), so it plays through
    that track's synth. Fired when clicking to add a note in the piano roll,
    so pitches can be found by ear. */
void MainComponent::previewNote(int noteNumber)
{
    engine_.keyboardState().noteOn(1, noteNumber, 0.8f);

    // Guarded by a SafePointer rather than capturing `this` directly: the
    // note-off fires 150ms later, and quitting the app within that window
    // would otherwise run this lambda against a destroyed MainComponent (and
    // a destroyed engine). A dangling preview is easy to trigger — click a
    // note, close the window — and would crash on the way out.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Timer::callAfterDelay(150, [safeThis, noteNumber]
    {
        if (auto* self = safeThis.getComponent())
            self->engine_.keyboardState().noteOff(1, noteNumber, 0.8f);
    });
}

void MainComponent::updateEditingLabel()
{
    const auto& song = history_.current();

    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        editingLabel_.setText("No track selected", juce::dontSendNotification);
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    const auto  name  = track.name.empty() ? ("Track " + juce::String(selectedTrackIndex_ + 1))
                                           : juce::String(track.name);

    juce::String text = "Editing: " + name;
    if (! track.clips.empty())
    {
        text << "   |   Clip " << (selectedClipIndex_ + 1) << " of " << (int) track.clips.size();

        if (selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) track.clips.size()
            && track.clips[(size_t) selectedClipIndex_].type == model::ClipType::Audio)
            text << "  (audio clip - not MIDI-editable)";
    }
    editingLabel_.setText(text, juce::dontSendNotification);
}

void MainComponent::updateMixerStrips()
{
    const auto& song = history_.current();

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip  = trackStrips_[i];
        const bool active = i < (int) song.tracks.size();
        strip->setVisible(active);

        if (active)
        {
            const auto& track = song.tracks[(size_t) i];
            strip->setTrackName(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name));
            strip->setGainDb(track.gainDb);
            strip->setMuted(track.muted);
            strip->setSoloed(track.solo);
            strip->setPan(track.pan);
        }
        strip->setSelected(i == selectedTrackIndex_);
    }

    layoutMixerView();
}

void MainComponent::setTrackGain(int index, float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.gainDb = gainDb;

        // The same global "Rec Auto" toggle arms every automatable per-track
        // parameter — touch whichever control you want to automate while it's
        // on (see also setTrackPan).
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Gain)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), gainDb);
    }
    engine_.setTrackGainDb(index, gainDb);
}

/** Remembers where a fader was when it was grabbed. */
void MainComponent::beginFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    faderDragTrack_ = trackIndex;
    faderDragWhich_ = fader;
    faderDragFrom_  = readFader(history_.current(), trackIndex, fader);
    faderDragging_  = true;
}

/** Turns a whole fader drag into one undo step.

    The live changes during the drag go through mutableCurrent, so the audio
    follows the fader without hundreds of snapshots. On release the document is
    rewound to where the drag started and the final value committed as a single
    edit — which is what leaves exactly one step on the stack for the whole
    gesture.

    Mute and solo don't need this: a click is already one edit. A fader is
    hundreds of values, and one step each would bury the last real edit under a
    drag. */
void MainComponent::endFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    if (! faderDragging_ || faderDragTrack_ != trackIndex || faderDragWhich_ != fader)
        return;

    faderDragging_ = false;

    const float landedOn = readFader(history_.current(), trackIndex, fader);
    commitDrag(history_, faderName(fader), faderDragFrom_, landedOn,
               [trackIndex, fader](model::Song& s, float v) { writeFader(s, trackIndex, fader, v); });
}

/** Mutes or unmutes a track, as an undoable edit.

    Mute and solo go through the history where gain, pan and send level do
    not, and the difference is that these two are discrete. A click is one
    edit, so it makes one undo step. A fader is a drag of hundreds of values,
    and putting each on the stack would bury the last real edit under a
    hundred nudges — those stay live tweaks until there is somewhere to
    coalesce a whole drag into a single step.

    Undo reaches the audio as well as the document: refreshFromModel runs
    syncEngineTracks, which pushes every track's mute and solo back to the
    engine. Without that an undone mute would restore the checkbox and leave
    the track silent. */
void MainComponent::setTrackMuted(int index, bool muted)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].muted == muted)
        return; // nothing changed, so nothing worth an undo step

    history_.edit(muted ? "Mute track" : "Unmute track", [index, muted](model::Song& s)
    {
        s.tracks[(size_t) index].muted = muted;
    });

    engine_.setTrackMuted(index, muted);

    // Both views show mute, and either can set it, so both are refreshed from
    // the document here rather than by whichever one happened to be clicked.
    // Without this the tracks pane muted the audio and left its own icon
    // unchanged — indistinguishable from a button that does nothing.
    // MixerStrip::setMuted uses dontSendNotification, so this can't echo back.
    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

/** Solos or unsolos a track. Undoable for the same reason as mute — see
    setTrackMuted, which explains why the continuous controls are not. */
void MainComponent::setTrackSolo(int index, bool solo)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].solo == solo)
        return;

    history_.edit(solo ? "Solo track" : "Unsolo track", [index, solo](model::Song& s)
    {
        s.tracks[(size_t) index].solo = solo;
    });

    engine_.setTrackSolo(index, solo);
    updateMixerStrips();
}

void MainComponent::setTrackPan(int index, float pan)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.pan = pan;
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Pan)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), pan);
    }
    engine_.setTrackPan(index, pan);
}

void MainComponent::selectTrack(int index)
{
    // A mixer-strip click doesn't know about specific clips, so it defaults to
    // the track's first one.
    selectTrackAndClip(index, 0);
}

void MainComponent::selectTrackAndClip(int trackIndex, int clipIndex)
{
    if (trackIndex < 0 || trackIndex >= trackCount())
        return;

    selectedTrackIndex_ = trackIndex;
    selectedClipIndex_  = clipIndex;
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    updateMixerStrips(); // refreshes the selection highlight
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Points the whole UI at a track: every pane that shows per-track state is
    refreshed from it. Used after adding a track and after deleting one, which
    is why it isn't named for either. */
void MainComponent::selectTrackAndRefreshAll(int newTrackIndex)
{
    if (newTrackIndex < 0)
        return;

    selectedTrackIndex_ = newTrackIndex;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

} // namespace soundsplice
