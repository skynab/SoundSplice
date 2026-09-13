#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    A span of time across one or more tracks in the arrangement, including the
    empty space between clips: what Audacity means by a selection, and what the
    arrangement's Cut, Copy, Paste, Delete and Silence act on.

    Not part of the document. Selecting isn't an edit, so it lives with the
    view, and tracks are named by id so a selection still means the same lanes
    after one is added or removed above them.

    A selection with tracks but no length is a cursor: where Paste lands.
*/
struct TimeSelection
{
    double           startBeats = 0.0;
    double           endBeats   = 0.0;
    std::vector<int> trackIds;

    bool   isEmpty() const { return endBeats <= startBeats || trackIds.empty(); }
    bool   hasTracks() const { return ! trackIds.empty(); }
    double lengthBeats() const { return isEmpty() ? 0.0 : endBeats - startBeats; }

    bool includes(int trackId) const
    {
        return std::find(trackIds.begin(), trackIds.end(), trackId) != trackIds.end();
    }

    bool operator==(const TimeSelection&) const = default;
};

/** The selection a drag from @p anchorBeat on lane @p anchorTrack to
    @p currentBeat on lane @p currentTrack describes: ordered in time, and
    every lane from one to the other, whichever way the drag went. */
inline TimeSelection selectionFromDrag(const Song& song, double anchorBeat, double currentBeat, int anchorTrack,
                                       int currentTrack)
{
    TimeSelection selection;
    selection.startBeats = std::max(0.0, std::min(anchorBeat, currentBeat));
    selection.endBeats   = std::max(0.0, std::max(anchorBeat, currentBeat));

    const int last = (int) song.tracks.size() - 1;
    if (last < 0)
        return selection;

    const int from = std::clamp(std::min(anchorTrack, currentTrack), 0, last);
    const int to   = std::clamp(std::max(anchorTrack, currentTrack), 0, last);
    for (int i = from; i <= to; ++i)
        selection.trackIds.push_back(song.tracks[(size_t) i].id);

    return selection;
}

/** What Cut and Copy take from a time selection: the clips, or the parts of
    clips, it covered on each selected track, placed relative to its start. */
struct RangeClipboard
{
    double                         lengthBeats = 0.0;
    std::vector<std::vector<Clip>> tracks; // one entry per selected track, top to bottom

    bool isEmpty() const { return tracks.empty() || lengthBeats <= 0.0; }
};

/**
    The edits a time selection makes. Every one is non-destructive: a clip cut
    by the selection is trimmed or split (moving its source offset, as
    app/ClipWindow.h does for one clip), never rewritten, so all of them undo
    to exactly what was there.

    Audio tracks only for now. An instrument clip plays its pattern from the
    start and has no offset to trim with, so a range can't take the middle
    out of one yet.
*/
namespace rangeedit
{
    inline constexpr double kEpsilonBeats = 1.0e-9;

    inline bool appliesTo(const Track& track)
    {
        return track.type == TrackType::Audio;
    }

    /** Whether @p selection covers any track these edits apply to. */
    inline bool anyTrackApplies(const Song& song, const TimeSelection& selection)
    {
        for (const auto& track : song.tracks)
            if (selection.includes(track.id) && appliesTo(track))
                return true;
        return false;
    }

    /** The part of @p clip inside [fromBeats, toBeats), or nothing if they
        don't overlap. A piece keeps a fade only on an edge it still has. */
    inline std::optional<Clip> pieceOf(const Clip& clip, double fromBeats, double toBeats, double bpm)
    {
        const double start = clip.startBeats;
        const double end   = clip.startBeats + clip.lengthBeats;
        const double from  = std::max(fromBeats, start);
        const double to    = std::min(toBeats, end);

        if (to - from <= kEpsilonBeats || bpm <= 0.0)
            return std::nullopt;

        Clip piece                = clip;
        piece.startBeats          = from;
        piece.lengthBeats         = to - from;
        piece.sourceOffsetSeconds = clip.sourceOffsetSeconds + (from - start) * 60.0 / bpm;

        if (from > start + kEpsilonBeats)
            piece.fades.inSeconds = 0.0;
        if (to < end - kEpsilonBeats)
            piece.fades.outSeconds = 0.0;

        return piece;
    }

    /** What @p selection covers, for Cut and Copy. */
    inline RangeClipboard copyRange(const Song& song, const TimeSelection& selection)
    {
        RangeClipboard clipboard;
        clipboard.lengthBeats = selection.lengthBeats();
        if (selection.isEmpty())
            return clipboard;

        for (const auto& track : song.tracks)
        {
            if (! selection.includes(track.id) || ! appliesTo(track))
                continue;

            std::vector<Clip> clips;
            for (const auto& clip : track.clips)
            {
                if (auto piece = pieceOf(clip, selection.startBeats, selection.endBeats, song.bpm))
                {
                    piece->startBeats -= selection.startBeats;
                    clips.push_back(*piece);
                }
            }
            clipboard.tracks.push_back(std::move(clips));
        }

        return clipboard;
    }

    /** Takes [start, end) of @p selection out of every selected audio track.
        With @p closeGap (Delete, Cut) everything after it moves back to
        fill the space; without (Silence) the space is left empty. */
    inline void removeRange(Song& song, const TimeSelection& selection, bool closeGap)
    {
        if (selection.isEmpty())
            return;

        const double from   = selection.startBeats;
        const double to     = selection.endBeats;
        const double length = to - from;

        for (auto& track : song.tracks)
        {
            if (! selection.includes(track.id) || ! appliesTo(track))
                continue;

            std::vector<Clip> kept;
            for (const auto& clip : track.clips)
            {
                const double start = clip.startBeats;
                const double end   = clip.startBeats + clip.lengthBeats;

                if (end <= from + kEpsilonBeats)
                {
                    kept.push_back(clip);
                    continue;
                }

                if (start >= to - kEpsilonBeats)
                {
                    auto moved = clip;
                    if (closeGap)
                        moved.startBeats -= length;
                    kept.push_back(moved);
                    continue;
                }

                const auto before = pieceOf(clip, start, from, song.bpm);
                auto       after  = pieceOf(clip, to, end, song.bpm);

                if (before)
                    kept.push_back(*before);

                if (after)
                {
                    if (closeGap)
                        after->startBeats -= length;
                    if (before)
                        after->id = allocateId(song); // the clip became two
                    kept.push_back(*after);
                }
            }

            track.clips = std::move(kept);
        }
    }

    /** Puts @p clipboard at @p atBeats on the audio tracks among @p trackIds,
        its first track onto the topmost of them, pushing what's there from
        @p atBeats on later by its length (splitting a clip that spans it).
        False if there was nowhere to put it. */
    inline bool insertClipboard(Song& song, const std::vector<int>& trackIds, const RangeClipboard& clipboard,
                                double atBeats)
    {
        if (clipboard.isEmpty())
            return false;

        const double at     = std::max(0.0, atBeats);
        const double length = clipboard.lengthBeats;

        size_t next = 0;
        for (auto& track : song.tracks)
        {
            if (next >= clipboard.tracks.size())
                break;

            if (std::find(trackIds.begin(), trackIds.end(), track.id) == trackIds.end() || ! appliesTo(track))
                continue;

            std::vector<Clip> placed;
            for (const auto& clip : track.clips)
            {
                const double start = clip.startBeats;
                const double end   = clip.startBeats + clip.lengthBeats;

                if (end <= at + kEpsilonBeats)
                {
                    placed.push_back(clip);
                }
                else if (start >= at - kEpsilonBeats)
                {
                    auto moved = clip;
                    moved.startBeats += length;
                    placed.push_back(moved);
                }
                else
                {
                    const auto before = pieceOf(clip, start, at, song.bpm);
                    auto       after  = pieceOf(clip, at, end, song.bpm);
                    if (before)
                        placed.push_back(*before);
                    if (after)
                    {
                        after->startBeats += length;
                        after->id = allocateId(song);
                        placed.push_back(*after);
                    }
                }
            }

            for (auto clip : clipboard.tracks[next])
            {
                clip.startBeats += at;
                clip.id = allocateId(song);
                placed.push_back(clip);
            }

            track.clips = std::move(placed);
            ++next;
        }

        return next > 0;
    }
} // namespace rangeedit

} // namespace soundsplice::model
