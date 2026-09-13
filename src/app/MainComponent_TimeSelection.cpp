#include "MainComponentInternal.h"

#include "model/ArrangementEdits.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The arrangement's time selection across tracks: cut, copy, paste, delete
// and silence over every clip it covers. The edits themselves are in
// model/TimeSelection.h.

namespace soundsplice
{
void MainComponent::setTimeSelection(const model::TimeSelection& selection)
{
    timeSelection_ = selection;
    arrangementView_.setTimeSelection(selection);

    // Looping plays the selection when there is one (see updateLoopRegion).
    updateLoopRegion();

    if (selection.isEmpty())
        return;

    const double seconds = selection.lengthBeats() * 60.0 / juce::jmax(1.0, history_.current().bpm);
    const int    tracks  = (int) selection.trackIds.size();
    showStatus("Selected " + juce::String(seconds, 2) + "s on " + juce::String(tracks)
               + (tracks == 1 ? " track" : " tracks"));
}

/** Everything that shows clips follows an edit to the arrangement. The
    selected clip's index may now name a different clip or none, which the
    panes cope with: they look it up again. */
void MainComponent::refreshAfterArrangementEdit()
{
    arrangementView_.setSong(history_.current());
    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Cut (@p copy, @p remove, @p closeGap), Copy (@p copy only), Delete
    (@p remove, @p closeGap) or Silence (@p remove only) over the time
    selection, as one undo step.

    False when there's no time selection with length, so the command falls
    back to the audio editor. True, having said why, when there is one but it
    covers no track these edits work on. */
bool MainComponent::editTimeSelection(const juce::String& label, bool copy, bool remove, bool closeGap)
{
    if (timeSelection_.isEmpty())
        return false;

    const auto& song = history_.current();
    if (! model::rangeedit::anyTrackApplies(song, timeSelection_))
    {
        showError("Time selections edit audio tracks - include at least one");
        return true;
    }

    const double seconds = timeSelection_.lengthBeats() * 60.0 / juce::jmax(1.0, song.bpm);

    if (copy)
        rangeClipboard_ = model::rangeedit::copyRange(song, timeSelection_);

    if (remove)
    {
        const auto selection = timeSelection_;
        history_.edit(label.toStdString(), [selection, closeGap](model::Song& s)
        {
            model::rangeedit::removeRange(s, selection, closeGap);
        });

        // With the gap closed, what was after the selection is now at its
        // start, so the selection shrinks to a cursor there: a Paste puts the
        // audio straight back.
        if (closeGap)
        {
            auto collapsed     = timeSelection_;
            collapsed.endBeats = collapsed.startBeats;
            setTimeSelection(collapsed);
        }

        refreshAfterArrangementEdit();
    }

    showStatus(label + " " + juce::String(seconds, 2) + "s");
    return true;
}

/** Pastes what Cut or Copy took from a time selection at its start, on its
    tracks, replacing what it covers if it has length. False when there's no
    time selection or nothing to paste, so the command falls back to the audio
    editor. */
bool MainComponent::pasteAtTimeSelection()
{
    if (! timeSelection_.hasTracks() || rangeClipboard_.isEmpty())
        return false;

    const auto selection = timeSelection_;
    const auto clipboard = rangeClipboard_;

    if (! model::rangeedit::anyTrackApplies(history_.current(), selection))
    {
        showError("Time selections edit audio tracks - include at least one");
        return true;
    }

    history_.edit("Paste", [selection, clipboard](model::Song& s)
    {
        model::rangeedit::removeRange(s, selection, true);
        model::rangeedit::insertClipboard(s, selection.trackIds, clipboard, selection.startBeats);
    });

    // What was pasted ends up selected, so it can be moved on or undone as
    // one piece, as in Audacity.
    auto pasted     = selection;
    pasted.endBeats = pasted.startBeats + clipboard.lengthBeats;
    setTimeSelection(pasted);

    refreshAfterArrangementEdit();
    showStatus("Pasted");
    return true;
}

std::vector<int> MainComponent::arrangementEditTracks() const
{
    if (timeSelection_.hasTracks())
        return timeSelection_.trackIds;

    const auto& tracks = history_.current().tracks;
    if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) tracks.size())
        return { tracks[(size_t) selectedTrackIndex_].id };

    return {};
}

// Each of these tries its edit on a copy first, so an edit that would change
// nothing says so instead of leaving an undo step that does nothing.

void MainComponent::splitClipsAtPlayhead()
{
    const auto   tracks = arrangementEditTracks();
    const double beat   = playheadBeat();

    auto trial = history_.current();
    if (model::arrangeedit::splitClipsAt(trial, tracks, beat) == 0)
    {
        showStatus("No audio clip under the playhead on the selected tracks");
        return;
    }

    int split = 0;
    history_.edit("Split at playhead", [&tracks, beat, &split](model::Song& s)
    {
        split = model::arrangeedit::splitClipsAt(s, tracks, beat);
    });

    refreshAfterArrangementEdit();
    showStatus("Split " + juce::String(split) + (split == 1 ? " clip" : " clips"));
}

/** Joins within the time selection when it has length, or anywhere on the
    selected tracks when it doesn't. */
void MainComponent::joinArrangementClips()
{
    const auto   tracks = arrangementEditTracks();
    const double from   = timeSelection_.isEmpty() ? 0.0 : timeSelection_.startBeats;
    const double to     = timeSelection_.isEmpty() ? std::numeric_limits<double>::max() : timeSelection_.endBeats;

    auto trial = history_.current();
    if (model::arrangeedit::joinClips(trial, tracks, from, to) == 0)
    {
        showStatus("Nothing to join - only clips that carry straight on from each other can be joined");
        return;
    }

    int joined = 0;
    history_.edit("Join clips", [&tracks, from, to, &joined](model::Song& s)
    {
        joined = model::arrangeedit::joinClips(s, tracks, from, to);
    });

    refreshAfterArrangementEdit();
    showStatus("Made " + juce::String(joined) + (joined == 1 ? " join" : " joins"));
}

void MainComponent::duplicateTimeSelection()
{
    const auto selection = timeSelection_;

    auto trial = history_.current();
    if (model::arrangeedit::duplicateRange(trial, selection).isEmpty())
    {
        showError("Select time on at least one audio track to duplicate");
        return;
    }

    model::TimeSelection copy;
    history_.edit("Duplicate selection", [&selection, &copy](model::Song& s)
    {
        copy = model::arrangeedit::duplicateRange(s, selection);
    });

    setTimeSelection(copy);
    refreshAfterArrangementEdit();
}

} // namespace soundsplice
