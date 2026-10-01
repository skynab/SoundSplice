#pragma once

#include <algorithm>
#include <vector>

#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::model
{
/**
    Razor edits (REAPER's): stretches of time on single tracks, each with its
    own start and end, made one or several at a time and then cut, copied,
    deleted or moved together. Where a time selection is one range across a
    run of tracks, razor areas can be a verse on the vocal, a bar on the drums
    and nothing on the bass.

    Like a time selection, not part of the document: an area names its track
    by id, so it means the same lane after tracks are added or removed. The
    edits keep every clip non-destructive, through rangeedit::pieceOf, and
    never close the gap a deletion leaves: with different stretches on
    different tracks, rippling would pull the tracks out of time with each
    other.
*/
struct RazorArea
{
    int    trackId    = 0;
    double startBeats = 0.0;
    double endBeats   = 0.0;

    double lengthBeats() const { return endBeats - startBeats; }

    bool operator==(const RazorArea&) const = default;
};

using RazorAreas = std::vector<RazorArea>;

/** What Copy took from razor areas: each area's clips, placed relative to
    its own start, with where that area was relative to the earliest one and
    to the topmost track. */
struct RazorClipboard
{
    struct Entry
    {
        int               trackOffset = 0;   // lanes below the topmost area's
        double            startOffset = 0.0; // beats after the earliest area's start
        double            lengthBeats = 0.0;
        TrackType         trackType   = TrackType::Audio;
        std::vector<Clip> clips;             // startBeats relative to the area's start
    };

    std::vector<Entry> entries;

    bool isEmpty() const { return entries.empty(); }
};

namespace razoredit
{
    inline int trackIndexOf(const Song& song, int trackId)
    {
        for (int i = 0; i < (int) song.tracks.size(); ++i)
            if (song.tracks[(size_t) i].id == trackId)
                return i;
        return -1;
    }

    /** @p areas with @p area added: overlapping or touching areas on the
        same track become one. An area with no length adds nothing. Kept in
        track then time order. */
    inline RazorAreas addArea(const Song& song, RazorAreas areas, RazorArea area)
    {
        if (area.endBeats <= area.startBeats + rangeedit::kEpsilonBeats)
            return areas;

        RazorAreas kept;
        for (const auto& other : areas)
        {
            if (other.trackId == area.trackId && other.startBeats <= area.endBeats + rangeedit::kEpsilonBeats
                && other.endBeats >= area.startBeats - rangeedit::kEpsilonBeats)
            {
                area.startBeats = std::min(area.startBeats, other.startBeats);
                area.endBeats   = std::max(area.endBeats, other.endBeats);
            }
            else
                kept.push_back(other);
        }
        kept.push_back(area);

        std::sort(kept.begin(), kept.end(), [&song](const RazorArea& a, const RazorArea& b)
        {
            const int ta = trackIndexOf(song, a.trackId), tb = trackIndexOf(song, b.trackId);
            return ta != tb ? ta < tb : a.startBeats < b.startBeats;
        });
        return kept;
    }

    /** The areas a drag from @p anchorBeat on lane @p anchorTrack to
        @p currentBeat on lane @p currentTrack makes: the same stretch on
        every lane from one to the other that razor edits apply to. */
    inline RazorAreas areasFromDrag(const Song& song, double anchorBeat, double currentBeat, int anchorTrack,
                                    int currentTrack)
    {
        const auto selection = selectionFromDrag(song, anchorBeat, currentBeat, anchorTrack, currentTrack);

        RazorAreas areas;
        if (selection.isEmpty())
            return areas;

        for (const auto& track : song.tracks)
            if (selection.includes(track.id) && rangeedit::appliesTo(track))
                areas.push_back({ track.id, selection.startBeats, selection.endBeats });
        return areas;
    }

    /** The area on @p trackId that contains @p beat, or nullptr. */
    inline const RazorArea* areaAt(const RazorAreas& areas, int trackId, double beat)
    {
        for (const auto& area : areas)
            if (area.trackId == trackId && beat >= area.startBeats && beat < area.endBeats)
                return &area;
        return nullptr;
    }

    /** What @p areas cover, for Copy and Cut and for moving them. */
    inline RazorClipboard copyAreas(const Song& song, const RazorAreas& areas)
    {
        RazorClipboard clipboard;

        // Where the topmost and earliest areas are, which the rest are placed against.
        int    topTrack = -1;
        double earliest = 0.0;
        for (const auto& area : areas)
        {
            const int index = trackIndexOf(song, area.trackId);
            if (index < 0 || area.lengthBeats() <= 0.0)
                continue;
            earliest = topTrack < 0 ? area.startBeats : std::min(earliest, area.startBeats);
            topTrack = topTrack < 0 ? index : std::min(topTrack, index);
        }

        for (const auto& area : areas)
        {
            const int index = trackIndexOf(song, area.trackId);
            if (index < 0 || area.lengthBeats() <= 0.0)
                continue;

            const auto&          track = song.tracks[(size_t) index];
            RazorClipboard::Entry entry;
            entry.trackOffset = index - topTrack;
            entry.startOffset = area.startBeats - earliest;
            entry.lengthBeats = area.lengthBeats();
            entry.trackType   = track.type;
            for (const auto& clip : track.clips)
                if (auto piece = rangeedit::pieceOf(clip, area.startBeats, area.endBeats, clockFor(song)))
                {
                    piece->startBeats -= area.startBeats;
                    entry.clips.push_back(*piece);
                }
            clipboard.entries.push_back(std::move(entry));
        }
        return clipboard;
    }

    /** Takes what @p areas cover out of their tracks, leaving the space
        empty (see the note at the top). */
    inline void removeAreas(Song& song, const RazorAreas& areas)
    {
        for (const auto& area : areas)
            rangeedit::removeRange(song, { area.startBeats, area.endBeats, { area.trackId } }, false);
    }

    /** Whether @p clipboard can land with its topmost area on lane
        @p topTrack: every entry has a lane there, of the kind it came from. */
    inline bool fitsAt(const Song& song, const RazorClipboard& clipboard, int topTrack)
    {
        for (const auto& entry : clipboard.entries)
        {
            const int index = topTrack + entry.trackOffset;
            if (index < 0 || index >= (int) song.tracks.size() || song.tracks[(size_t) index].type != entry.trackType)
                return false;
        }
        return ! clipboard.isEmpty();
    }

    /** Puts @p clipboard down with its earliest area at @p atBeats and its
        topmost on lane @p topTrack, each area replacing what its stretch
        covered there. An entry with no lane of its kind is left out. Returns
        where the areas landed, to select them. */
    inline RazorAreas pasteAreas(Song& song, const RazorClipboard& clipboard, int topTrack, double atBeats)
    {
        RazorAreas landed;
        for (const auto& entry : clipboard.entries)
        {
            const int index = topTrack + entry.trackOffset;
            if (index < 0 || index >= (int) song.tracks.size() || song.tracks[(size_t) index].type != entry.trackType)
                continue;

            const int    trackId = song.tracks[(size_t) index].id;
            const double start   = std::max(0.0, atBeats + entry.startOffset);
            const double end     = start + entry.lengthBeats;
            rangeedit::removeRange(song, { start, end, { trackId } }, false);

            for (auto clip : entry.clips)
            {
                clip.startBeats += start;
                addClip(song, trackId, std::move(clip));
            }
            landed = addArea(song, std::move(landed), { trackId, start, end });
        }
        return landed;
    }

    /** Moves what @p areas cover by @p deltaBeats and @p deltaTracks lanes,
        each area replacing what's where it lands. A move onto lanes that
        can't take it (past the last track, or the wrong kind) keeps to its
        own lanes. Returns where the areas are now. */
    inline RazorAreas moveAreas(Song& song, const RazorAreas& areas, double deltaBeats, int deltaTracks)
    {
        const auto clipboard = copyAreas(song, areas);
        if (clipboard.isEmpty())
            return areas;

        int    topTrack = -1;
        double earliest = 0.0;
        bool   first    = true;
        for (const auto& area : areas)
        {
            const int index = trackIndexOf(song, area.trackId);
            if (index < 0)
                continue;
            topTrack = topTrack < 0 ? index : std::min(topTrack, index);
            earliest = first ? area.startBeats : std::min(earliest, area.startBeats);
            first    = false;
        }

        if (! fitsAt(song, clipboard, topTrack + deltaTracks))
            deltaTracks = 0;

        removeAreas(song, areas);
        return pasteAreas(song, clipboard, topTrack + deltaTracks, std::max(0.0, earliest + deltaBeats));
    }
}

} // namespace soundsplice::model
