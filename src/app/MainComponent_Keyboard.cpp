#include "MainComponentInternal.h"

// Keyboard operation of the arrangement, and what's said about it: moving
// between tracks and clips, nudging a clip, setting the time selection's
// edges at the playhead, and Where Am I. Each says what it did through the
// screen reader (announce), so the arrangement can be worked without seeing
// it - Audacity's strong suit.

namespace soundsplice
{
namespace
{
    juce::String secondsText(double seconds)
    {
        const int    minutes = (int) (seconds / 60.0);
        const double rest    = seconds - minutes * 60.0;
        return minutes > 0 ? juce::String(minutes) + " minutes " + juce::String(rest, 1) + " seconds"
                           : juce::String(rest, 1) + " seconds";
    }
}

/** Says @p text through the screen reader, if one is running. */
void MainComponent::announce(const juce::String& text, bool important)
{
    juce::AccessibilityHandler::postAnnouncement(text, important ? juce::AccessibilityHandler::AnnouncementPriority::high
                                                                 : juce::AccessibilityHandler::AnnouncementPriority::medium);
}

/** "Track 2 of 5, Voice, audio, 3 clips, muted, minus 6 dB". */
juce::String MainComponent::describeTrack(int index) const
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return "No track";
    const auto& track = song.tracks[(size_t) index];
    juce::String text = "Track " + juce::String(index + 1) + " of " + juce::String((int) song.tracks.size()) + ", "
                      + juce::String(track.name) + ", "
                      + (track.type == model::TrackType::Bus ? "bus" : "audio");
    if (track.type != model::TrackType::Bus)
        text << ", " << (int) track.clips.size() << (track.clips.size() == 1 ? " clip" : " clips");
    if (track.muted)
        text << ", muted";
    if (track.solo)
        text << ", soloed";
    if (track.gainDb != 0.0f)
        text << ", " << (track.gainDb < 0.0f ? "minus " : "plus ") << juce::String(std::abs(track.gainDb), 1) << " dB";
    return text;
}

/** "Clip 2 of 4, take one, at 12.5 seconds, 4.0 seconds long". */
juce::String MainComponent::describeClip(int trackIndex, int clipIndex) const
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return "No clip";
    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clipIndex < 0 || clipIndex >= (int) clips.size())
        return "No clip";

    const auto& clip  = clips[(size_t) clipIndex];
    const auto  clock = model::clockFor(song);
    // Its place among the track's clips in time, which isn't the list's order.
    int place = 1;
    for (const auto& other : clips)
        if (other.startBeats < clip.startBeats)
            ++place;

    const auto name = juce::File(clip.audioFile).getFileNameWithoutExtension();
    return "Clip " + juce::String(place) + " of " + juce::String((int) clips.size())
         + (name.isEmpty() ? juce::String() : ", " + name)
         + ", at " + secondsText(clock.secondsAt(clip.startBeats))
         + ", " + secondsText(clock.secondsBetween(clip.startBeats, clip.startBeats + clip.lengthBeats)) + " long";
}

void MainComponent::selectAdjacentTrack(int delta)
{
    if (trackCount() == 0)
        return;
    const int index = juce::jlimit(0, trackCount() - 1, selectedTrackIndex_ + delta);
    selectTrackAndRefreshAll(index);
    announce(describeTrack(index));
}

/** The previous or next clip in time on the selected track, with the
    playhead moved to its start so playing starts there. */
void MainComponent::selectAdjacentClip(int delta)
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (clips.empty())
    {
        announce("No clips on this track");
        return;
    }

    std::vector<int> byTime((size_t) clips.size());
    for (int i = 0; i < (int) clips.size(); ++i)
        byTime[(size_t) i] = i;
    std::stable_sort(byTime.begin(), byTime.end(), [&](int a, int b)
                     { return clips[(size_t) a].startBeats < clips[(size_t) b].startBeats; });

    const auto at   = std::find(byTime.begin(), byTime.end(), selectedClipIndex_);
    const int  from = at != byTime.end() ? (int) (at - byTime.begin()) : (delta > 0 ? -1 : (int) byTime.size());
    const int  to   = juce::jlimit(0, (int) byTime.size() - 1, from + delta);
    const int  clip = byTime[(size_t) to];

    selectTrackAndClip(selectedTrackIndex_, clip);
    seekToBeat(clips[(size_t) clip].startBeats);
    announce(describeClip(selectedTrackIndex_, clip));
}

/** The selected clip a beat earlier or later, as dragging it would. */
void MainComponent::nudgeSelectedClip(double beats)
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const int track = selectedTrackIndex_, clip = selectedClipIndex_;
    if (arrangementView_.onClipMoved)
        arrangementView_.onClipMoved(track, clip, juce::jmax(0.0, clips[(size_t) clip].startBeats + beats));
    announce(describeClip(track, clip));
}

/** "[" and "]": the time selection's start or end at the playhead, on the
    tracks it's already on, or the selected track. */
void MainComponent::setSelectionEdgeAtPlayhead(bool start)
{
    const auto& song = history_.current();
    if (song.tracks.empty())
        return;

    auto       selection = timeSelection_;
    const auto playhead  = playheadBeat();
    const auto bar       = (double) beatsPerBar();
    if (selection.trackIds.empty() && selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
        selection.trackIds.push_back(song.tracks[(size_t) selectedTrackIndex_].id);

    if (start)
    {
        selection.startBeats = playhead;
        if (selection.endBeats <= playhead)
            selection.endBeats = playhead + bar;
    }
    else
    {
        selection.endBeats = playhead;
        if (selection.startBeats >= playhead || timeSelection_.isEmpty())
            selection.startBeats = juce::jmax(0.0, playhead - bar);
    }
    if (selection.endBeats <= selection.startBeats)
        return; // the end at the very start: nothing to select
    setTimeSelection(selection);
    announce(describeSelection());
}

juce::String MainComponent::describeSelection() const
{
    if (timeSelection_.isEmpty())
        return "No time selection";
    const auto clock = model::clockFor(history_.current());
    return "Selection from " + secondsText(clock.secondsAt(timeSelection_.startBeats)) + " to "
         + secondsText(clock.secondsAt(timeSelection_.endBeats)) + ", on "
         + juce::String((int) timeSelection_.trackIds.size()) + (timeSelection_.trackIds.size() == 1 ? " track" : " tracks");
}

/** Where Am I: the playhead, the track, the clip and the selection, said. */
void MainComponent::announceWhereAmI()
{
    const auto clock = model::clockFor(history_.current());
    juce::String text = "Playhead at " + secondsText(clock.secondsAt(playheadBeat())) + ". " + describeTrack(selectedTrackIndex_) + ".";
    if (selectedAudioClip() != nullptr || (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                                           && ! history_.current().tracks[(size_t) selectedTrackIndex_].clips.empty()))
        text << " " << describeClip(selectedTrackIndex_, selectedClipIndex_) << ".";
    text << " " << describeSelection() << ".";
    showStatus(text);
}

} // namespace soundsplice
