#pragma once

#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    Folder tracks, for organization only: a track can hold the tracks below
    it (Track::folderParentId), which are drawn indented under it and hidden
    when it's collapsed, and muted and soloed with it. Nothing is routed: a
    folder is an ordinary track that happens to have others in it, as in
    REAPER, where any track can be a folder. Buses were cut from SoundSplice
    on purpose (see docs/ROADMAP.md), so a folder doesn't become one.

    One level deep. A track is in a folder only while it sits in the run of
    tracks straight after it that all name it: a delete, a duplicate or a
    paste that breaks the run simply leaves the tracks past the break at the
    top level, so no edit to the track list has to keep folders tidy.
*/
namespace folderedit
{
    /** The index of the folder @p index is in, or -1. */
    inline int parentIndexOf(const Song& song, int index)
    {
        if (index < 0 || index >= (int) song.tracks.size())
            return -1;

        const int parentId = song.tracks[(size_t) index].folderParentId;
        if (parentId == 0)
            return -1;

        int j = index - 1;
        while (j >= 0 && song.tracks[(size_t) j].id != parentId && song.tracks[(size_t) j].folderParentId == parentId)
            --j;

        if (j >= 0 && song.tracks[(size_t) j].id == parentId && song.tracks[(size_t) j].folderParentId == 0)
            return j;
        return -1;
    }

    /** The indices of the tracks in folder @p index, in order; empty for a
        track that isn't one. */
    inline std::vector<int> childIndices(const Song& song, int index)
    {
        std::vector<int> children;
        if (index < 0 || index >= (int) song.tracks.size() || song.tracks[(size_t) index].folderParentId != 0)
            return children;

        const int id = song.tracks[(size_t) index].id;
        for (int i = index + 1; i < (int) song.tracks.size() && song.tracks[(size_t) i].folderParentId == id; ++i)
            children.push_back(i);
        return children;
    }

    inline bool isFolder(const Song& song, int index) { return ! childIndices(song, index).empty(); }

    /** True for a track hidden in a collapsed folder. */
    inline bool isHidden(const Song& song, int index)
    {
        const int parent = parentIndexOf(song, index);
        return parent >= 0 && song.tracks[(size_t) parent].folderCollapsed;
    }

    /** The tracks shown in the arrangement, top to bottom, by index. */
    inline std::vector<int> visibleTrackIndices(const Song& song)
    {
        std::vector<int> visible;
        for (int i = 0; i < (int) song.tracks.size(); ++i)
            if (! isHidden(song, i))
                visible.push_back(i);
        return visible;
    }

    /** Puts track @p index into the folder above it: the one the track above
        is in, or the track above itself. Not for the first track, nor for a
        folder (folders don't nest). True if it moved in. */
    inline bool indent(Song& song, int index)
    {
        if (index <= 0 || index >= (int) song.tracks.size() || parentIndexOf(song, index) >= 0
            || isFolder(song, index))
            return false;

        const int above  = parentIndexOf(song, index - 1);
        const int parent = above >= 0 ? above : index - 1;
        song.tracks[(size_t) index].folderParentId = song.tracks[(size_t) parent].id;
        return true;
    }

    /** Takes track @p index out of its folder, moving it to just after the
        folder's last track so the tracks after it stay in. Returns its new
        index, or -1 if it wasn't in one. */
    inline int outdent(Song& song, int index)
    {
        const int parent = parentIndexOf(song, index);
        if (parent < 0)
            return -1;

        const int last  = childIndices(song, parent).back();
        Track     track = song.tracks[(size_t) index];
        track.folderParentId = 0;

        song.tracks.erase(song.tracks.begin() + index);
        song.tracks.insert(song.tracks.begin() + last, std::move(track)); // last moved up one with the erase
        return last;
    }

    /** @p index and, for a folder, every track in it: what mute and solo
        on a folder act on. */
    inline std::vector<int> withChildren(const Song& song, int index)
    {
        std::vector<int> tracks { index };
        for (const int child : childIndices(song, index))
            tracks.push_back(child);
        return tracks;
    }
}

} // namespace soundsplice::model
