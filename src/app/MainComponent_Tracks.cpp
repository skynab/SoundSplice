#include "MainComponentInternal.h"

#include "model/ArrangementEdits.h"
#include "model/Takes.h"
#include "SpectralRender.h"
#include "engine/Resample.h"
#include "model/TrackResample.h"

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
    if (! model::routing::holdsClips(history_.current().tracks[(size_t) selectedTrackIndex_]))
    {
        showError("A bus has no clips - route or send tracks to it instead");
        return;
    }

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
    if (! model::rangeedit::fits(history_.current().tracks[(size_t) selectedTrackIndex_], clipClipboard_.front().type))
    {
        showError("That clip can't go on this track - audio goes on audio tracks, notes on instrument ones");
        return;
    }

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

    int copyIndex = trackIndex + 1;
    history_.edit("Duplicate track", [trackIndex, &copyIndex](model::Song& s)
    {
        // A folder's copy goes after the tracks in it, keeping them in it.
        copyIndex = model::folderedit::duplicate(s, trackIndex);
    });

    selectTrackAndRefreshAll(copyIndex); // the copy, so it can be worked on straight away
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

void MainComponent::showResampleTrackDialog()
{
    const auto& tracks = history_.current().tracks;
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) tracks.size()
        || tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Audio)
    {
        showError("Select an audio track first");
        return;
    }

    static const int kRates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };


    juce::StringArray names;
    int               selected = 6; // 48 kHz, unless the device runs at one of the others
    for (int i = 0; i < (int) std::size(kRates); ++i)
    {
        names.add(juce::String(kRates[i]) + " Hz");
        if (std::abs(engine_.sampleRate() - kRates[i]) < 0.5)
            selected = i;
    }

    dialog("Resample Track",
           "Converts the track's audio to a new sample rate. Clips keep their timing, fades and volume curves.")
        .choice("rate", "New sample rate:", names, selected)
        .unsaved()
        .show("Resample", [this](const FormDialog::Values& v) { resampleSelectedTrack((double) kRates[v.choice("rate")]); });
}

/** Resample, as in Audacity: the track's audio converted to another rate.

    Clips here play files of any rate (the engine converts as it plays), so
    this writes a converted copy of each file the track plays that isn't
    already at the rate, and points the clips at the copies. Clip positions,
    offsets, fades and volume curves are all in seconds, so nothing else about
    a clip changes, and undo points them back at the files they played before,
    which are untouched. Files are read and written a chunk at a time, so a
    long recording never has to fit in memory. The engine isn't involved, so
    it keeps its device. */
void MainComponent::resampleSelectedTrack(double sampleRate)
{
    const int   index  = selectedTrackIndex_;
    const auto& tracks = history_.current().tracks;
    if (index < 0 || index >= (int) tracks.size() || tracks[(size_t) index].type != model::TrackType::Audio)
    {
        showError("Select an audio track first");
        return;
    }

    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    const auto sources = model::trackresample::audioFilesOf(tracks[(size_t) index]);
    if (sources.empty())
    {
        showError("That track has no audio to resample");
        return;
    }

    struct Outcome
    {
        std::map<std::string, std::string> replacements;
        juce::Array<juce::File>            written;
        juce::String                       error;
    };

    const int  trackId = tracks[(size_t) index].id;
    const auto folder  = audioDirectoryFor(editsDirectory());
    auto       outcome = std::make_shared<Outcome>();

    auto work = [sources, sampleRate, folder, outcome](app::OfflineRenderJob& job)
    {
        juce::AudioFormatManager formats;
        engine::sequencefile::registerFormats(formats);

        constexpr int kChunk = 1 << 16;

        for (size_t f = 0; f < sources.size(); ++f)
        {
            const auto source = engine::sequencefile::fileFromPath(sources[f]);
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
            if (reader == nullptr || reader->sampleRate <= 0.0)
            {
                outcome->error = "Could not read " + source.getFileName();
                return;
            }

            if (std::abs(reader->sampleRate - sampleRate) < 0.5)
                continue; // already there

            const engine::Resampler resampler(reader->sampleRate, sampleRate);
            const auto              inputLength = (std::int64_t) reader->lengthInSamples;
            const auto              outLength   = resampler.outputLength(inputLength);
            const int               channels    = juce::jmax(1, (int) reader->numChannels);

            if (folder.createDirectory().failed())
            {
                outcome->error = "Could not write into " + folder.getFullPathName();
                return;
            }

            const auto destination = folder.getNonexistentChildFile(
                source.getFileNameWithoutExtension() + "-" + juce::String((int) std::lround(sampleRate)), ".wav");

            auto fileStream = std::make_unique<juce::FileOutputStream>(destination);
            if (fileStream->failedToOpen())
            {
                outcome->error = "Could not write " + destination.getFileName();
                return;
            }
            std::unique_ptr<juce::OutputStream> stream = std::move(fileStream);

            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate(sampleRate)
                                     .withNumChannels(channels)
                                     .withBitsPerSample(32)
                                     .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

            juce::WavAudioFormat wav;
            auto                 writer = wav.createWriterFor(stream, options);
            outcome->written.add(destination);
            if (writer == nullptr)
            {
                outcome->error = "Could not write " + destination.getFileName();
                return;
            }

            juce::AudioBuffer<float> input;
            juce::AudioBuffer<float> output(channels, kChunk);
            const auto               status = "Resampling " + source.getFileName();

            for (std::int64_t out = 0; out < outLength; out += kChunk)
            {
                if (job.shouldAbort())
                    return;

                const int    count = (int) juce::jmin<std::int64_t>(kChunk, outLength - out);
                std::int64_t first = 0, end = 0;
                resampler.inputRangeFor(out, count, first, end);

                // The reader gives silence for frames outside the file.
                input.setSize(channels, (int) (end - first), false, false, true);
                input.clear();
                reader->read(input.getArrayOfWritePointers(), channels, first, input.getNumSamples());

                for (int ch = 0; ch < channels; ++ch)
                    resampler.process(input.getReadPointer(ch), first, input.getNumSamples(), inputLength, out, count,
                                      output.getWritePointer(ch));

                if (! writer->writeFromAudioSampleBuffer(output, 0, count))
                {
                    outcome->error = "Could not write " + destination.getFileName();
                    return;
                }

                job.report(app::overallProgress((int) f, (int) sources.size(), (double) (out + count) / (double) outLength),
                           status);
            }

            writer.reset(); // finishes the header before anything plays the file
            outcome->replacements[sources[f]] = engine::sequencefile::pathOf(destination);
        }
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), outcome, trackId,
                       sampleRate](bool cancelled)
    {
        const auto discard = [outcome]
        {
            for (const auto& file : outcome->written)
                file.deleteFile();
        };

        if (self == nullptr)
            return;

        self->renderJob_.reset();

        if (cancelled || outcome->error.isNotEmpty())
        {
            discard();
            if (cancelled)
                self->showStatus("Resample cancelled");
            else
                self->showError(outcome->error);
            return;
        }

        const auto rateText = juce::String((int) std::lround(sampleRate)) + " Hz";
        if (outcome->replacements.empty())
        {
            self->showStatus("That track's audio is already at " + rateText);
            return;
        }

        int trackIndex = -1;
        self->history_.edit("Resample track", [&](model::Song& s)
        {
            for (int i = 0; i < (int) s.tracks.size(); ++i)
                if (s.tracks[(size_t) i].id == trackId)
                {
                    model::trackresample::replaceAudioFiles(s.tracks[(size_t) i], outcome->replacements);
                    trackIndex = i;
                }
        });

        if (trackIndex < 0)
        {
            discard();
            self->showError("The track was removed while resampling");
            return;
        }

        // The noise print and the editor's peaks describe the old files.
        self->noiseProfiles_.clear();
        self->noiseProfileFile_ = juce::File{};
        self->waveformPeaksKey_ = {};

        // Every clip is where it was, so the selection stays.
        self->syncEngineTracks();
        self->refreshAudioEditorForSelected();
        self->refreshSessionView();
        self->arrangementView_.setSong(self->history_.current());
        self->showStatus("Resampled the track to " + rateText);
    };

    renderJob_ = app::OfflineRenderJob::launch("Resample Track", std::move(work), std::move(onFinished));
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

    // Edit groups: see model/TrackGroups.h. Each says who's in it already.
    juce::PopupMenu groups;
    groups.addItem(kFirstEditGroupMenuId, "None", true, track.editGroup == 0);
    for (int g = 1; g <= model::kEditGroupCount; ++g)
    {
        juce::StringArray members;
        for (const auto& other : song.tracks)
            if (other.editGroup == g && other.id != track.id)
                members.add(other.name.empty() ? juce::String("unnamed") : juce::String(other.name));
        groups.addItem(kFirstEditGroupMenuId + g,
                       "Group " + juce::String(g) + (members.isEmpty() ? juce::String() : " (with " + members.joinIntoString(", ") + ")"),
                       true, track.editGroup == g);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader(track.name.empty() ? ("Track " + juce::String(trackIndex + 1))
                                             : juce::String(track.name));
    menu.addSubMenu("Colour", colours);
    menu.addSubMenu("Edit Group", groups);
    if (model::folderedit::parentIndexOf(song, trackIndex) >= 0)
        menu.addItem(kOutdentTrackMenuId, "Take Out of Folder");
    else
    {
        auto trial = song;
        menu.addItem(kIndentTrackMenuId, "Put in Folder Above", model::folderedit::indent(trial, trackIndex));
    }
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

        if (result == kIndentTrackMenuId)
        {
            self->indentTrack(trackIndex);
            return;
        }

        if (result == kOutdentTrackMenuId)
        {
            self->outdentTrack(trackIndex);
            return;
        }

        if (const int group = result - kFirstEditGroupMenuId; group >= 0 && group <= model::kEditGroupCount)
        {
            self->setTrackEditGroup(trackIndex, group);
            return;
        }

        if (const int index = result - kFirstColourMenuId; index >= 0 && index < kNumTrackColours)
            self->setTrackColour(trackIndex, kTrackColours[index].argb);
    });
}

/** The tracks mute and solo on @p trackIndex act on: its edit group and,
    for a folder, the tracks in each. */
std::vector<int> MainComponent::linkedTracks(int trackIndex) const
{
    const auto&      song = history_.current();
    std::vector<int> linked;
    for (const int member : model::groupedit::memberIndices(song, trackIndex))
        for (const int track : model::folderedit::withChildren(song, member))
            if (std::find(linked.begin(), linked.end(), track) == linked.end())
                linked.push_back(track);
    return linked;
}

/** Collapses a folder track, hiding the tracks in it, or opens it again. */
void MainComponent::toggleFolder(int trackIndex)
{
    const auto& song = history_.current();
    if (! model::folderedit::isFolder(song, trackIndex))
        return;

    const int  trackId   = song.tracks[(size_t) trackIndex].id;
    const bool collapsed = ! song.tracks[(size_t) trackIndex].folderCollapsed;
    history_.edit(collapsed ? "Collapse folder" : "Expand folder", [trackId, collapsed](model::Song& s)
    {
        if (auto* track = model::findTrack(s, trackId))
            track->folderCollapsed = collapsed;
    });
    arrangementView_.setSong(history_.current());
}

/** Puts a track in the folder above it (see model::folderedit::indent). */
void MainComponent::indentTrack(int trackIndex)
{
    auto trial = history_.current();
    if (! model::folderedit::indent(trial, trackIndex))
        return;

    history_.edit("Put track in folder", [trackIndex](model::Song& s) { model::folderedit::indent(s, trackIndex); });
    arrangementView_.setSong(history_.current());
    showStatus("In the folder above - click its triangle to collapse it");
}

/** Takes a track out of its folder, moving it past the folder's last track. */
void MainComponent::outdentTrack(int trackIndex)
{
    auto      trial = history_.current();
    const int moved = model::folderedit::outdent(trial, trackIndex);
    if (moved < 0)
        return;

    history_.edit("Take track out of folder", [trackIndex](model::Song& s) { model::folderedit::outdent(s, trackIndex); });

    // The track list's order changed: the selection follows its track, and
    // everything indexed by it is refreshed.
    int selected = selectedTrackIndex_;
    if (selected == trackIndex)
        selected = moved;
    else if (selected > trackIndex && selected <= moved)
        --selected;
    selectTrackAndRefreshAll(selected);
}

/** Track @p trackIndex's routing to the engine, by indices: where its
    output goes and its sends, each to a bus that's still there. */
void MainComponent::pushTrackRouting(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto&                            track = song.tracks[(size_t) trackIndex];
    std::vector<engine::AudioEngine::SendSpec> sends;
    for (const auto& send : track.sends)
        for (int b = 0; b < (int) song.tracks.size(); ++b)
            if (song.tracks[(size_t) b].id == send.busId && model::routing::isBus(song.tracks[(size_t) b]))
                sends.push_back({ b, send.levelDb, send.preFader });

    engine_.setTrackRouting(trackIndex, model::routing::isBus(track), model::routing::outputIndex(song, trackIndex), sends);

    // Sidechains: each keyed slot and the track it listens to, while that
    // track is still there.
    std::vector<std::pair<int, int>> keys;
    for (int s = 0; s < (int) track.effectChain.size(); ++s)
    {
        const auto& slot = track.effectChain[(size_t) s];
        if (! model::canBeKeyed(slot.kind) || slot.sidechainTrackId == 0)
            continue;
        for (int t = 0; t < (int) song.tracks.size(); ++t)
            if (t != trackIndex && song.tracks[(size_t) t].id == slot.sidechainTrackId)
                keys.push_back({ s, t });
    }
    engine_.setTrackSidechains(trackIndex, keys);
}

/** Which track the selected track's compressor or gate in slot
    @p slotIndex listens to: its own input, or any track it doesn't feed
    (a track it feeds would be listening to itself, a moment late). */
void MainComponent::chooseSidechain(int slotIndex)
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (slotIndex < 0 || slotIndex >= (int) track.effectChain.size())
        return;

    const int        current = track.effectChain[(size_t) slotIndex].sidechainTrackId;
    std::vector<int> sources;
    juce::PopupMenu  menu;
    menu.addSectionHeader("Listen to");
    menu.addItem(1, "Own Input", true, current == 0);
    for (const auto& other : song.tracks)
        if (other.id != track.id && ! model::routing::feeds(song, track.id, other.id))
        {
            sources.push_back(other.id);
            menu.addItem(100 + (int) sources.size() - 1, juce::String(other.name), true, other.id == current);
        }

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackId = track.id, slotIndex, sources](int result)
        {
            if (self == nullptr || result == 0)
                return;
            const int source = result == 1 ? 0 : sources[(size_t) (result - 100)];
            self->history_.edit(source == 0 ? "Remove sidechain" : "Set sidechain", [trackId, slotIndex, source](model::Song& s)
            {
                if (auto* t = model::findTrack(s, trackId); t != nullptr && slotIndex < (int) t->effectChain.size())
                    t->effectChain[(size_t) slotIndex].sidechainTrackId = source;
            });
            self->syncEngineTracks();
            self->refreshEffectChainForSelected();
        });
}

/** Adds a bus track (model/Routing.h) after the last track. */
void MainComponent::addBusTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    history_.edit("Add bus", [](model::Song& s)
    {
        int buses = 0;
        for (const auto& track : s.tracks)
            buses += model::routing::isBus(track) ? 1 : 0;
        model::addTrack(s, model::TrackType::Bus, "Bus " + std::to_string(buses + 1));
    });

    selectTrackAndRefreshAll(trackCount() - 1);
    showStatus("Added a bus - on a track's mixer strip, Out sends its output here and Sends adds a send");
}

/** Where a track's output goes: the master, or a bus that doesn't feed it. */
void MainComponent::chooseTrackOutput(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];
    const auto  buses = model::routing::busesFor(song, track.id);

    juce::PopupMenu menu;
    menu.addSectionHeader("Output");
    menu.addItem(1, "Master", true, model::routing::outputIndex(song, trackIndex) < 0);
    for (int b = 0; b < (int) buses.size(); ++b)
        menu.addItem(100 + b, juce::String(model::findTrack(song, buses[(size_t) b])->name), true,
                     track.outputBusId == buses[(size_t) b]);
    if (buses.empty())
        menu.addItem(2, "(add a bus to route to one)", false, false);

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackId = track.id, buses](int result)
        {
            if (self == nullptr || result == 0 || result == 2)
                return;
            const int busId = result == 1 ? 0 : buses[(size_t) (result - 100)];
            self->history_.edit("Route track", [trackId, busId](model::Song& s) { model::routing::setOutput(s, trackId, busId); });
            self->syncEngineTracks();
            self->updateMixerStrips();
        });
}

/** A track's sends: add one to a bus, or for each one, pre or post fader,
    or remove it. */
void MainComponent::showSendsMenu(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];
    std::vector<int> addable;
    for (const int busId : model::routing::busesFor(song, track.id))
        if (std::none_of(track.sends.begin(), track.sends.end(), [busId](const model::TrackSend& s) { return s.busId == busId; }))
            addable.push_back(busId);

    juce::PopupMenu add;
    for (int b = 0; b < (int) addable.size(); ++b)
        add.addItem(100 + b, juce::String(model::findTrack(song, addable[(size_t) b])->name));

    juce::PopupMenu menu;
    menu.addSubMenu("Add Send To", add, ! addable.empty());
    for (int k = 0; k < (int) track.sends.size(); ++k)
    {
        const auto* bus = model::routing::busById(song, track.sends[(size_t) k].busId);
        if (bus == nullptr)
            continue;
        juce::PopupMenu one;
        one.addItem(1000 + k * 10, "Pre-Fader", true, track.sends[(size_t) k].preFader);
        one.addItem(1000 + k * 10 + 1, "Remove");
        menu.addSubMenu("Send to " + juce::String(bus->name), one);
    }

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackId = track.id, addable](int result)
        {
            if (self == nullptr || result == 0)
                return;

            if (result >= 100 && result < 1000)
            {
                const int busId = addable[(size_t) (result - 100)];
                self->history_.edit("Add send", [trackId, busId](model::Song& s) { model::routing::addSend(s, trackId, busId); });
            }
            else
            {
                const int k = (result - 1000) / 10, action = (result - 1000) % 10;
                self->history_.edit(action == 0 ? "Change send" : "Remove send", [trackId, k, action](model::Song& s)
                {
                    auto* track = model::findTrack(s, trackId);
                    if (track == nullptr || k < 0 || k >= (int) track->sends.size())
                        return;
                    if (action == 0)
                        track->sends[(size_t) k].preFader = ! track->sends[(size_t) k].preFader;
                    else
                        track->sends.erase(track->sends.begin() + k);
                });
            }
            self->syncEngineTracks();
            self->updateMixerStrips();
        });
}

/** A send's level, live from its slider; a drag commits as one step. */
void MainComponent::setSendLevel(int trackIndex, int send, float levelDb)
{
    auto& song = history_.mutableCurrent();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    auto& sends = song.tracks[(size_t) trackIndex].sends;
    if (send < 0 || send >= (int) sends.size())
        return;

    sends[(size_t) send].levelDb = levelDb;
    pushTrackRouting(trackIndex);
}

void MainComponent::beginSendDrag(int trackIndex, int send)
{
    const auto& tracks = history_.current().tracks;
    if (trackIndex < 0 || trackIndex >= (int) tracks.size() || send < 0 || send >= (int) tracks[(size_t) trackIndex].sends.size())
        return;

    sendDrag_.begin({ trackIndex, send }, tracks[(size_t) trackIndex].sends[(size_t) send].levelDb);
}

void MainComponent::endSendDrag(int trackIndex, int send)
{
    const auto from = sendDrag_.end({ trackIndex, send });
    if (! from)
        return;

    const auto& tracks = history_.current().tracks;
    if (trackIndex >= (int) tracks.size() || send >= (int) tracks[(size_t) trackIndex].sends.size())
        return;

    const float landedOn = tracks[(size_t) trackIndex].sends[(size_t) send].levelDb;
    commitDrag(history_, std::string("Set send level"), *from, landedOn,
               [trackIndex, send](model::Song& s, float v)
               {
                   if (trackIndex < (int) s.tracks.size() && send < (int) s.tracks[(size_t) trackIndex].sends.size())
                       s.tracks[(size_t) trackIndex].sends[(size_t) send].levelDb = v;
               });
}

/** Warps clip @p clipId to the song's tempo, or stops warping it. */
void MainComponent::toggleClipWarp(int trackIndex, int clipId)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    const int  trackId = song.tracks[(size_t) trackIndex].id;
    const auto* clip   = model::warpedit::findClip(song, trackId, clipId);
    if (clip == nullptr)
        return;
    const bool warp = ! clip->warp;

    if (warp)
        showBusy("Stretching the clip to the song's tempo...");
    history_.edit(warp ? "Warp clip" : "Unwarp clip",
                  [trackId, clipId, warp](model::Song& s) { model::warpedit::setWarp(s, trackId, clipId, warp); });
    refreshAfterArrangementEdit();
    showStatus(warp ? "Warped - it follows the song's tempo now, tempo changes included"
                    : "Unwarped - it plays at its own tempo again");
}

/** Detects clip @p clipId's tempo from its file and keeps it on the clip. */
void MainComponent::detectClipTempo(int trackIndex, int clipId)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    const int   trackId = song.tracks[(size_t) trackIndex].id;
    const auto* clip    = model::warpedit::findClip(song, trackId, clipId);
    if (clip == nullptr || clip->type != model::ClipType::Audio)
        return;

    showBusy("Detecting the clip's tempo...");
    const auto estimate = engine_.detectFileTempo(juce::File(clip->audioFile));
    if (! estimate.isUsable())
    {
        showError("No steady tempo found in this clip - use Set Clip Tempo to give it one");
        return;
    }

    const double bpm = estimate.bpm;
    history_.edit("Detect clip tempo", [trackId, clipId, bpm](model::Song& s) { model::warpedit::setSourceTempo(s, trackId, clipId, bpm); });
    refreshAfterArrangementEdit();
    showStatus("Clip tempo: " + juce::String(bpm, 1) + " BPM"
               + (estimate.confidence < 0.5 ? juce::String(" (not sure - check it by ear)") : juce::String()));
}

/** Asks for clip @p clipId's tempo. */
void MainComponent::askClipTempo(int trackIndex, int clipId)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    const int   trackId = song.tracks[(size_t) trackIndex].id;
    const auto* clip    = model::warpedit::findClip(song, trackId, clipId);
    if (clip == nullptr)
        return;


    dialog("Set Clip Tempo", "The tempo this clip was played at")
        .text("bpm", "Tempo (BPM):", clip->sourceBpm > 0.0 ? juce::String(clip->sourceBpm, 2) : juce::String())
        .unsaved()
        .show("OK", [this, trackId, clipId](const FormDialog::Values& v)
        {
            const double bpm = v.text("bpm").getDoubleValue();
            if (bpm < 10.0 || bpm > 999.0)
            {
                showError("A tempo between 10 and 999 BPM, please");
                return;
            }
            history_.edit("Set clip tempo", [trackId, clipId, bpm](model::Song& s) { model::warpedit::setSourceTempo(s, trackId, clipId, bpm); });
            refreshAfterArrangementEdit();
        });
}

/** Sets the song's tempo at the clip to the clip's own, so it plays in time
    without warping. */
void MainComponent::songTempoFromClip(int trackIndex, int clipId)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    const auto* clip = model::warpedit::findClip(song, song.tracks[(size_t) trackIndex].id, clipId);
    if (clip == nullptr || clip->sourceBpm <= 0.0)
        return;

    const double bpm = clip->sourceBpm;
    const double at  = model::tempoedit::changeInForceAt(song, clip->startBeats);
    history_.edit("Tempo from clip", [at, bpm](model::Song& s) { model::tempoedit::setTempo(s, at, bpm); });
    afterTempoEdit();
    showStatus("Song tempo set to " + juce::String(bpm, 1) + " BPM from the clip");
}

/** Puts a track in edit group @p group, or none (0). */
void MainComponent::setTrackEditGroup(int trackIndex, int group)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size() || song.tracks[(size_t) trackIndex].editGroup == group)
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;
    history_.edit(group > 0 ? "Set edit group" : "Remove from edit group", [trackId, group](model::Song& s)
    {
        if (auto* track = model::findTrack(s, trackId))
            track->editGroup = group;
    });

    arrangementView_.setSong(history_.current());
    showStatus(group > 0 ? "In edit group " + juce::String(group)
                               + " - selections, mute, solo, faders and lined-up clips follow the group"
                         : juce::String("No longer in an edit group"));
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


    dialog("Rename Track")
        .text("name", "Name:", juce::String(track.name))
        .unsaved()
        .show("Rename", [this, trackId](const FormDialog::Values& v)
        {
            const auto name = v.text("name");
            if (name.isEmpty())
                return; // an unnamed track is worse than the generated name

            history_.edit("Rename track", [trackId, name](model::Song& s)
            {
                model::renameTrack(s, trackId, name.toStdString());
            });

            syncEngineTracks();
            updateMixerStrips();
            refreshSessionView();
            arrangementView_.setSong(history_.current());
            updateEditingLabel();
        });
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
    const bool crossfade = autoCrossfades_;
    history_.edit("Resize clip", [trackIndex, clipIndex, newLengthBeats, crossfade](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].lengthBeats = juce::jmax(1.0, newLengthBeats);
        if (crossfade)
            model::arrangeedit::applyAutoCrossfades(s, trackIndex);
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
    const bool crossfade = autoCrossfades_;
    history_.edit("Trim clip start", [trackIndex, clipIndex, newStartBeats, crossfade](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIndex];
        if (clip.type == model::ClipType::Audio)
            clip = trimClipStart(clip, newStartBeats, model::clockFor(s), kMinTrimmedClipBeats);
        if (crossfade)
            model::arrangeedit::applyAutoCrossfades(s, trackIndex);
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

        // A fade set by hand is the user's from now on, not an automatic one.
        if (fades.inSeconds != clip.fades.inSeconds || fades.inShape != clip.fades.inShape)
            clip.autoFadeIn = false;
        if (fades.outSeconds != clip.fades.outSeconds || fades.outShape != clip.fades.outShape)
            clip.autoFadeOut = false;

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
        { engine::FadeShape::SCurve,     "S-Curve" },
        { engine::FadeShape::Exponential, "Exponential" },
        { engine::FadeShape::Logarithmic, "Logarithmic" }
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

    // Takes: which one the clip plays, which one plays over the time
    // selection (comping), and making overlapping clips into takes.
    static constexpr int kTakeBase          = 1000;
    static constexpr int kSelectionTakeBase = 2000;
    static constexpr int kCombineTakes      = 3;

    const auto& clip  = clips[(size_t) clipIndex];
    const int   clipId = clip.id;
    const auto& track = song.tracks[(size_t) trackIndex];
    const bool  selectionOnClip = ! timeSelection_.isEmpty() && timeSelection_.includes(track.id)
                               && timeSelection_.startBeats < clip.startBeats + clip.lengthBeats
                               && timeSelection_.endBeats > clip.startBeats;
    const double selectionFrom = timeSelection_.startBeats, selectionTo = timeSelection_.endBeats;

    menu.addSeparator();
    if (! clip.takes.empty())
    {
        juce::PopupMenu takeMenu, selectionMenu;
        for (int t = 0; t < (int) clip.takes.size(); ++t)
        {
            const auto name = juce::String(clip.takes[(size_t) t].name);
            takeMenu.addItem(kTakeBase + t, name, true, t == clip.activeTake);
            selectionMenu.addItem(kSelectionTakeBase + t, name);
        }
        menu.addSubMenu("Takes", takeMenu);
        menu.addSubMenu("Use Take for Selection", selectionMenu, selectionOnClip);
    }

    int overlapping = 0;
    for (const auto& other : track.clips)
        if (other.type == model::ClipType::Audio && other.startBeats < clip.startBeats + clip.lengthBeats
            && other.startBeats + other.lengthBeats > clip.startBeats)
            ++overlapping;
    menu.addItem(kCombineTakes, "Combine Overlapping Clips into Takes", overlapping > 1);

    // Warp (model/Warp.h): follow the song's tempo, from the tempo the clip
    // was played at.
    static constexpr int kWarp            = 4;
    static constexpr int kDetectTempo     = 5;
    static constexpr int kSetClipTempo    = 6;
    static constexpr int kProjectFromClip = 7;
    if (clip.type == model::ClipType::Audio)
    {
        menu.addSeparator();
        const bool knows = clip.sourceBpm > 0.0;
        menu.addItem(kWarp, knows ? "Warp to Song Tempo   (clip is " + juce::String(clip.sourceBpm, 1) + " BPM)"
                                  : juce::String("Warp to Song Tempo   (detect or set its tempo first)"),
                     knows, clip.warp);
        menu.addItem(kDetectTempo, "Detect Clip Tempo");
        menu.addItem(kSetClipTempo, "Set Clip Tempo...");
        menu.addItem(kProjectFromClip, "Set Song Tempo from Clip", knows);
    }

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackIndex, clipIndex, fades, clipId,
         clipStart = clip.startBeats, clipEnd = clip.startBeats + clip.lengthBeats, selectionFrom, selectionTo](int result)
        {
            if (self == nullptr || result == 0)
                return;

            if (result == kCombineTakes)
            {
                self->combineOverlappingClipsIntoTakes(trackIndex, clipIndex);
                return;
            }
            if (result == kWarp)            { self->toggleClipWarp(trackIndex, clipId); return; }
            if (result == kDetectTempo)     { self->detectClipTempo(trackIndex, clipId); return; }
            if (result == kSetClipTempo)    { self->askClipTempo(trackIndex, clipId); return; }
            if (result == kProjectFromClip) { self->songTempoFromClip(trackIndex, clipId); return; }
            if (result >= kSelectionTakeBase)
            {
                self->useClipTake(trackIndex, clipId, selectionFrom, selectionTo, result - kSelectionTakeBase);
                return;
            }
            if (result >= kTakeBase)
            {
                self->useClipTake(trackIndex, clipId, clipStart, clipEnd, result - kTakeBase);
                return;
            }

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

/** Plays take @p take of clip @p clipId over [@p fromBeats, @p toBeats):
    the whole clip, or, comping, a stretch of it. One undo step. */
void MainComponent::useClipTake(int trackIndex, int clipId, double fromBeats, double toBeats, int take)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;
    auto      trial   = song;
    if (! model::takeedit::compRange(trial, trackId, clipId, fromBeats, toBeats, take))
        return;

    history_.edit("Choose take", [trackId, clipId, fromBeats, toBeats, take](model::Song& s)
    {
        model::takeedit::compRange(s, trackId, clipId, fromBeats, toBeats, take);
    });
    refreshAfterArrangementEdit();
}

/** Swipe comping in the take lanes: take @p take plays over [@p fromBeats,
    @p toBeats) in every clip on the track that has it. One undo step. */
void MainComponent::swipeCompTake(int trackIndex, int take, double fromBeats, double toBeats)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;
    auto      trial   = song;
    if (! model::takeedit::compTrackRange(trial, trackId, fromBeats, toBeats, take))
        return;

    history_.edit("Comp takes", [trackId, fromBeats, toBeats, take](model::Song& s)
    {
        model::takeedit::compTrackRange(s, trackId, fromBeats, toBeats, take);
    });
    refreshAfterArrangementEdit();
}

/** Makes every audio clip on the track that overlaps clip @p clipIndex,
    and that clip, into one clip with a take for each. */
void MainComponent::combineOverlappingClipsIntoTakes(int trackIndex, int clipIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];
    if (clipIndex < 0 || clipIndex >= (int) track.clips.size())
        return;

    const auto&      clip = track.clips[(size_t) clipIndex];
    std::vector<int> ids;
    for (const auto& other : track.clips)
        if (other.type == model::ClipType::Audio && other.startBeats < clip.startBeats + clip.lengthBeats
            && other.startBeats + other.lengthBeats > clip.startBeats)
            ids.push_back(other.id);

    const int trackId = track.id;
    int       made    = 0;
    history_.edit("Combine into takes", [trackId, &ids, &made](model::Song& s)
    {
        made = model::takeedit::combineIntoTakes(s, trackId, ids);
    });
    if (made == 0)
        return;

    refreshAfterArrangementEdit();
    showStatus("Combined " + juce::String((int) ids.size()) + " clips into takes - right-click to choose one, "
               "or select a range and use a take for it");
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
            // With spectral edits, the file with them applied: made once and
            // cached, the original left as it is.
            spec.file        = spectralrender::fileFor(juce::File(clip.audioFile), clip.spectralEdits);
            spec.startBeats  = clip.startBeats;
            spec.lengthBeats = clip.lengthBeats;
            spec.gainDb      = clip.gainDb;
            spec.sourceOffsetSeconds = clip.sourceOffsetSeconds;
            spec.fades               = clip.fades;
            spec.channels            = clip.channels;
            spec.envelope            = clip.envelope;
            spec.clipId              = clip.id;
            spec.stretch             = model::warpedit::factorFor(song, clip);
            for (const auto& slot : clip.effects)
            {
                spec.effects.push_back(model::effectParamValues(slot));
                spec.slots.push_back(effectSlotSpecFor(slot));
            }
            audioSpecs.push_back(spec);
        }
        // Submitted even when empty, which the guard here used to skip: the
        // engine holds the last list it was given, so deleting a track's only
        // audio clip left that clip still loaded and still playing, with
        // nothing on screen to explain it. "Unconditionally resubmitted" in
        // the comment above is only true if it's also submitted when there's
        // nothing to submit.
        engine_.setTrackAudioClips(i, audioSpecs);
        pushTrackRouting(i);

        // A clip's plugin went with its chain: so must any editor showing it.
        if (engine_.takeClipPluginChainsChanged())
            closePluginEditors();

        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackSolo(i, track.solo);
        engine_.setTrackGainDb(i, track.gainDb);
        engine_.setTrackPan(i, track.pan);
        engine_.setTrackAutomation(i, engineAutomationFor(i, track));

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
            chainSpecs.push_back(effectSlotSpecFor(slot));
        // A rebuild destroys this track's nodes, hosted plugins included, so
        // any editor drawing one has to go first. Only on an actual rebuild —
        // closing plugin windows on every unrelated edit would be maddening.
        if (engine_.setTrackEffectChain(i, chainSpecs))
            closePluginEditors();

        // Parameters, one call per slot, addressed by position — a chain may
        // hold two filters, and "the filter" stops meaning anything then.
        for (size_t s = 0; s < track.effectChain.size(); ++s)
            engine_.setTrackEffectSlotParams(i, (int) s, model::effectParamValues(track.effectChain[s], true));
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

    // The picker: the track's own parameters, then every parameter of every
    // effect in its chain, a section per effect. A plugin's come from the
    // loaded instance, in its own 0..1 range and its own words for the ends;
    // one that isn't loaded has none to offer.
    auto targets = AutomationPane::trackTargets();
    for (size_t s = 0; s < track.effectChain.size(); ++s)
    {
        const auto& slot = track.effectChain[s];
        if (slot.kind == model::EffectKind::Plugin)
        {
            const auto* node = engine_.trackPluginNode(selectedTrackIndex_, (int) s);
            if (node == nullptr)
                continue;

            const auto heading = juce::String((int) s + 1) + ". "
                               + (slot.plugin.name.empty() ? juce::String("Plugin") : juce::String(slot.plugin.name));
            for (const auto& info : node->parameters())
                targets.push_back({ AutomationTarget::effect((int) s, slot.kind, info.id), juce::String(info.name),
                                    heading, { 0.0f, 1.0f, info.highest, info.lowest, info.value } });
            continue;
        }

        const auto* effect = model::descriptorFor(slot.kind);
        if (effect == nullptr)
            continue;

        const auto heading = juce::String((int) s + 1) + ". " + effect->name;
        for (size_t p = 0; p < effect->params.size(); ++p)
        {
            const auto& param = effect->params[p];
            targets.push_back({ AutomationTarget::effect((int) s, slot.kind, param.id),
                                juce::String(automationParamName(*effect, p)), heading,
                                automationRangeFor(param, model::paramValue(slot, param)) });
        }
    }
    automationPane_.setTargets(std::move(targets));

    // A track with no lane for this parameter gets an empty one rather than
    // nothing: an empty lane is a real state (no automation, sitting at the
    // static value) and is the one you start drawing into.
    const auto& target = automationPane_.target();
    const auto* lane   = target.isEffect() ? track.effectChain[(size_t) target.slot].lane(target.paramId)
                                           : track.lane(target.trackParam);

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
void MainComponent::applyEditedAutomationLane(const AutomationTarget& target,
                                              const model::AutomationLane& lane)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;

    history_.edit("Edit automation", [index, &target, &lane](model::Song& s)
    {
        if (index < 0 || index >= (int) s.tracks.size())
            return;

        auto& track = s.tracks[(size_t) index];

        // An emptied lane is erased rather than stored empty, so a track with
        // no automation carries no lanes at all — the state every serialization
        // and playback path already treats as "use the static value".
        if (target.isEffect())
        {
            // The slot has to still be the effect the lane was drawn for.
            if (target.slot >= (int) track.effectChain.size()
                || track.effectChain[(size_t) target.slot].kind != target.kind)
                return;

            auto& lanes = track.effectChain[(size_t) target.slot].automation;
            if (lane.empty())
                lanes.erase(target.paramId);
            else
                lanes[target.paramId] = lane;
        }
        else if (lane.empty())
            track.automation.erase((int) target.trackParam);
        else
            track.laneFor(target.trackParam) = lane;
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
            strip->setArmed(armedTrackIds_.count(track.id) > 0);

            // Routing: where the output goes, the sends, and no arming a bus.
            const int out = model::routing::outputIndex(song, i);
            strip->setOutputName(out < 0 ? juce::String("Master") : juce::String(song.tracks[(size_t) out].name));
            std::vector<MixerStrip::SendView> sends;
            for (const auto& send : track.sends)
                if (const auto* bus = model::routing::busById(song, send.busId))
                    sends.push_back({ juce::String(bus->name), send.levelDb, send.preFader });
            strip->setSends(sends);
            strip->setArmable(! model::routing::isBus(track));

            static const char* const modeNames[] = { "Read", "Touch", "Latch", "Write" };
            strip->setAutomationMode(modeNames[(int) automationModeFor(i)], track.automationMode >= 0);

            const auto inputs = engine_.inputChannelNames();
            const int  input  = track.recordInput;
            strip->setInputName(input < 0 ? juce::String("as in Recording Format")
                                          : input < inputs.size() ? inputs[input] : "input " + juce::String(input + 1));
        }
        strip->setSelected(i == selectedTrackIndex_);
    }

    layoutMixerView();
}

void MainComponent::setTrackGain(int index, float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    // The rest of its edit group move by as much.
    auto& song = history_.mutableCurrent();
    writeGroupFader(song, index, MixerStrip::Fader::Gain, gainDb, faderDragBaseFor(index, MixerStrip::Fader::Gain));
    for (const int member : model::groupedit::memberIndices(song, index))
    {
        engine_.setTrackGainDb(member, song.tracks[(size_t) member].gainDb);
        if (member != index && member < trackStrips_.size())
            trackStrips_[member]->setGainDb(song.tracks[(size_t) member].gainDb);
    }

    // Written into the lane if the automation mode says so (see
    // MainComponent_AutomationWrite.cpp); the drag hooks say when it's held.
    automationControlMoved(AutomationWriteKey::trackParam(index, model::TrackParam::Gain), gainDb, false);
}

/** The song as the drag of @p fader on @p trackIndex began, which its edit
    group's faders move from; nullptr when that fader isn't being dragged. */
const model::Song* MainComponent::faderDragBaseFor(int trackIndex, MixerStrip::Fader fader) const
{
    const auto* start = faderDrag_.startOf({ trackIndex, fader });
    return start != nullptr ? &start->song : nullptr;
}

/** Remembers where a fader was when it was grabbed. */
void MainComponent::beginFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    const float value = readFader(history_.current(), trackIndex, fader);
    faderDrag_.begin({ trackIndex, fader }, { value, history_.current() }); // where its edit group's faders were too

    const auto param = fader == MixerStrip::Fader::Gain ? model::TrackParam::Gain : model::TrackParam::Pan;
    automationControlMoved(AutomationWriteKey::trackParam(trackIndex, param), value, true);
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
    auto start = faderDrag_.end({ trackIndex, fader });
    if (! start)
        return;

    automationControlReleased(AutomationWriteKey::trackParam(
        trackIndex, fader == MixerStrip::Fader::Gain ? model::TrackParam::Gain : model::TrackParam::Pan));

    const float landedOn = readFader(history_.current(), trackIndex, fader);
    commitDrag(history_, faderName(fader), start->value, landedOn,
               [trackIndex, fader, base = std::move(start->song)](model::Song& s, float v)
               { writeGroupFader(s, trackIndex, fader, v, &base); });
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

    // Its whole edit group with it, and on a folder the tracks in it.
    const auto members = linkedTracks(index);
    history_.edit(muted ? "Mute track" : "Unmute track", [members, muted](model::Song& s)
    {
        for (const int member : members)
            s.tracks[(size_t) member].muted = muted;
    });

    for (const int member : members)
        engine_.setTrackMuted(member, muted);

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

    const auto members = linkedTracks(index);
    history_.edit(solo ? "Solo track" : "Unsolo track", [members, solo](model::Song& s)
    {
        for (const int member : members)
            s.tracks[(size_t) member].solo = solo;
    });

    for (const int member : members)
        engine_.setTrackSolo(member, solo);
    updateMixerStrips();
}

void MainComponent::setTrackPan(int index, float pan)
{
    auto& song = history_.mutableCurrent();
    writeGroupFader(song, index, MixerStrip::Fader::Pan, pan, faderDragBaseFor(index, MixerStrip::Fader::Pan));
    for (const int member : model::groupedit::memberIndices(song, index))
    {
        engine_.setTrackPan(member, song.tracks[(size_t) member].pan);
        if (member != index && member < trackStrips_.size())
            trackStrips_[member]->setPan(song.tracks[(size_t) member].pan);
    }
    automationControlMoved(AutomationWriteKey::trackParam(index, model::TrackParam::Pan), pan, false);
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
