#pragma once

#include <algorithm>
#include <cmath>
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
    std::vector<std::vector<Clip>> tracks;     // one entry per selected track, top to bottom
    std::vector<TrackType>         trackTypes; // the kind of track each entry came from

    bool isEmpty() const { return tracks.empty() || lengthBeats <= 0.0; }
};

/**
    The edits a time selection makes. Every one is non-destructive: a clip cut
    by the selection is trimmed or split (moving its source offset, as
    app/ClipWindow.h does for one clip), never rewritten, so all of them undo
    to exactly what was there.

    Audio and instrument tracks alike. An instrument clip has no source
    offset, so a piece of one gets a pattern of its own instead: the notes that
    start inside the piece, from the looping pattern as it plays there (see
    patternWindow).
*/
namespace rangeedit
{
    inline constexpr double kEpsilonBeats = 1.0e-9;

    inline bool appliesTo(const Track& track)
    {
        return track.type == TrackType::Audio || track.type == TrackType::Instrument;
    }

    /** Whether a clip of @p type can be placed on @p track by these edits. */
    inline bool fits(const Track& track, ClipType type)
    {
        return (track.type == TrackType::Audio) == (type == ClipType::Audio);
    }

    /** What @p pattern, looping from beat 0, plays over [@p fromBeats,
        @p toBeats), as a pattern of that length starting at @p fromBeats.

        A note belongs to the piece it starts in, and is cut short at the
        piece's end; one already sounding when the piece begins is left out,
        since a note can't start before its pattern does. The pedal keeps the
        state it had: a pedal held down across the start is put down at 0. */
    inline engine::Pattern patternWindow(const engine::Pattern& pattern, double fromBeats, double toBeats)
    {
        engine::Pattern out;
        out.lengthBeats = std::max(0.0, toBeats - fromBeats);

        const double loop = pattern.lengthBeats;
        if (loop <= 0.0 || out.lengthBeats <= kEpsilonBeats)
            return out;

        // Every repeat of the loop that overlaps the window.
        const auto first = (long long) std::floor(fromBeats / loop);
        const auto last  = (long long) std::ceil(toBeats / loop);

        for (long long repeat = first; repeat < last; ++repeat)
        {
            const double base = (double) repeat * loop;

            for (const auto& note : pattern.notes)
            {
                if (note.startBeats < 0.0 || note.startBeats >= loop)
                    continue; // never sounds (see PatternPlayback)

                const double on = base + note.startBeats;
                if (on < fromBeats - kEpsilonBeats || on >= toBeats - kEpsilonBeats)
                    continue;

                const double off = base + std::min(note.startBeats + note.lengthBeats, loop);

                auto piece        = note;
                piece.startBeats  = std::max(0.0, on - fromBeats);
                piece.lengthBeats = std::max(0.0, std::min(off, toBeats) - std::max(on, fromBeats));
                out.notes.push_back(piece);
            }
        }

        // The pedal's state where the window starts: the last movement at or
        // before it, in the loop as played.
        if (! pattern.pedals.empty())
        {
            const double into      = fromBeats - std::floor(fromBeats / loop) * loop;
            bool         down      = false;
            double       latest    = -1.0;
            for (const auto& pedal : pattern.pedals)
                if (pedal.beat >= 0.0 && pedal.beat < loop && pedal.beat <= into + kEpsilonBeats && pedal.beat >= latest)
                {
                    latest = pedal.beat;
                    down   = pedal.down;
                }
            if (latest < 0.0 && fromBeats >= loop) // nothing yet this time round: how the last one ended
                for (const auto& pedal : pattern.pedals)
                    if (pedal.beat >= 0.0 && pedal.beat < loop && pedal.beat >= latest)
                    {
                        latest = pedal.beat;
                        down   = pedal.down;
                    }
            if (down)
                out.pedals.push_back({ 0.0, true });

            for (long long repeat = first; repeat < last; ++repeat)
                for (const auto& pedal : pattern.pedals)
                {
                    if (pedal.beat < 0.0 || pedal.beat >= loop)
                        continue;
                    const double at = (double) repeat * loop + pedal.beat;
                    if (at > fromBeats + kEpsilonBeats && at < toBeats - kEpsilonBeats)
                        out.pedals.push_back({ at - fromBeats, pedal.down });
                }
        }

        std::stable_sort(out.notes.begin(), out.notes.end(),
                         [](const engine::Note& a, const engine::Note& b) { return a.startBeats < b.startBeats; });
        std::stable_sort(out.pedals.begin(), out.pedals.end(),
                         [](const engine::PedalEvent& a, const engine::PedalEvent& b) { return a.beat < b.beat; });
        return out;
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

        if (clip.type == ClipType::Instrument)
        {
            // Already the whole clip: keep its pattern exactly, loop and all.
            if (from <= start + kEpsilonBeats && to >= end - kEpsilonBeats)
                return clip;

            piece.pattern = patternWindow(clip.pattern, from - start, to - start);
            return piece;
        }

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
            clipboard.trackTypes.push_back(track.type);
        }

        return clipboard;
    }

    /** Takes [start, end) of @p selection out of every selected track.
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

    /** Puts @p clipboard at @p atBeats on the tracks among @p trackIds,
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

            // Each entry goes to the next track of the kind it came from:
            // audio has no place on an instrument track, nor notes on an audio one.
            if (next < clipboard.trackTypes.size() && clipboard.trackTypes[next] != track.type)
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
                if (! fits(track, clip.type))
                    continue;

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
