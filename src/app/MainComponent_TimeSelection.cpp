#include "MainComponentInternal.h"

#include "engine/SilenceDetection.h"
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

/** Asks how quiet, and for how long, counts as silence. */
void MainComponent::showDetachAtSilencesDialog()
{
    if (arrangementEditTracks().empty())
        return;

    auto* window = new juce::AlertWindow("Detach at Silences",
                                         "Splits the audio clips on the selected tracks where they fall silent, "
                                         "leaving the silent parts out. Only within the time selection, if there is one.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("threshold", "-40", "Silent below (dB):");
    window->addTextEditor("minimum", "0.5", "For at least (seconds):");
    window->addButton("Detach", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const float  threshold = juce::jlimit(-120.0f, 0.0f, window->getTextEditorContents("threshold").getFloatValue());
            const double minimum   = juce::jlimit(0.01, 60.0, window->getTextEditorContents("minimum").getDoubleValue());
            self->detachAtSilences(threshold, minimum);
        }));
}

/** Scans each audio clip on the edit's tracks (within the time selection, if
    there is one) for silence, then splits them all around what it found in
    one undo step.

    Reads a chunk at a time and keeps only a peak per 10 ms, so a long
    recording costs a small fraction of its size to scan. */
void MainComponent::detachAtSilences(float thresholdDb, double minSilenceSeconds)
{
    const auto  tracks  = arrangementEditTracks();
    const auto& song    = history_.current();
    const bool  limited = ! timeSelection_.isEmpty();
    const double selectionFrom = timeSelection_.startBeats;
    const double selectionTo   = timeSelection_.endBeats;

    struct Found
    {
        int                                    trackId = 0;
        int                                    clipId  = 0;
        std::vector<std::pair<double, double>> silences;
    };
    std::vector<Found> found;
    int                silenceCount = 0;

    showBusy("Finding silences...");

    for (const auto& track : song.tracks)
    {
        if (std::find(tracks.begin(), tracks.end(), track.id) == tracks.end() || ! model::rangeedit::appliesTo(track))
            continue;

        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            const double clipEnd = clip.startBeats + clip.lengthBeats;
            if (limited && (clipEnd <= selectionFrom || clip.startBeats >= selectionTo))
                continue;

            const juce::File file(clip.audioFile);
            const auto       sequence = engine::sequencefile::sequenceOf(file);
            if (! sequence || sequence->sampleRate <= 0.0)
                continue;

            const double rate   = sequence->sampleRate;
            const auto   window = clipSampleWindow(clip,
                                                   (int) juce::jmin<std::int64_t>(sequence->length(),
                                                                                  std::numeric_limits<int>::max()),
                                                   rate, song.bpm);
            if (window.isEmpty())
                continue;

            engine::silence::PeakEnvelope envelope(juce::jmax(1, (int) std::llround(rate * 0.01)));
            constexpr int kChunk = 1 << 20;
            bool          read   = true;

            for (int from = 0; from < window.length() && read; from += kChunk)
            {
                juce::AudioBuffer<float> buffer;
                const int count = juce::jmin(kChunk, window.length() - from);
                read = engine::sequencefile::readRange(file, (juce::int64) window.start + from, count, buffer);
                if (read)
                    envelope.append(buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
            }

            if (! read)
                continue;

            const auto runs = engine::silence::silentRuns(envelope.finish(), envelope.windowFrames(), window.length(),
                                                          engine::silence::gainForDecibels(thresholdDb),
                                                          std::llround(minSilenceSeconds * rate));

            // Seconds from the clip's start, cut down to the time selection.
            const double secondsPerBeat = 60.0 / song.bpm;
            const double limitFrom      = limited ? (selectionFrom - clip.startBeats) * secondsPerBeat : 0.0;
            const double limitTo        = limited ? (selectionTo - clip.startBeats) * secondsPerBeat
                                                  : std::numeric_limits<double>::max();

            Found clipFound { track.id, clip.id, {} };
            for (const auto& run : runs)
            {
                const double from = juce::jmax((double) run.from / rate, limitFrom);
                const double to   = juce::jmin((double) run.to / rate, limitTo);
                if (to > from)
                    clipFound.silences.emplace_back(from, to);
            }

            if (! clipFound.silences.empty())
            {
                silenceCount += (int) clipFound.silences.size();
                found.push_back(std::move(clipFound));
            }
        }
    }

    if (found.empty())
    {
        showStatus("No silences below " + juce::String(thresholdDb, 1) + " dB lasting "
                   + juce::String(minSilenceSeconds, 2) + "s or more");
        return;
    }

    history_.edit("Detach at silences", [found](model::Song& s)
    {
        for (const auto& clip : found)
            model::arrangeedit::detachAtSilences(s, clip.trackId, clip.clipId, clip.silences);
    });

    refreshAfterArrangementEdit();
    showStatus("Detached " + juce::String(silenceCount) + (silenceCount == 1 ? " silence" : " silences"));
}

} // namespace soundsplice
