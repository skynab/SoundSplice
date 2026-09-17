#pragma once

#include <algorithm>
#include <vector>

#include "model/Song.h"

namespace soundsplice::app
{
/**
    The audio open in the editor, as Audition's Files panel lists it: every
    audio clip shown in the Audio pane joins the list, in the order it was
    first opened, and stays until closed, so moving between the takes being
    worked on is one click rather than a hunt through the arrangement.

    Entries are clip ids, which stay the same through edits, moves between
    tracks and undo, where track and clip indices don't. A clip that no longer
    exists drops out when the list is next pruned. JUCE-free, so the list's
    rules are tested headless; the pane is app/OpenFilesPane.h.
*/
class OpenFiles
{
public:
    const std::vector<int>& clipIds() const noexcept { return ids_; }
    bool                    isEmpty() const noexcept { return ids_.empty(); }

    bool contains(int clipId) const
    {
        return std::find(ids_.begin(), ids_.end(), clipId) != ids_.end();
    }

    /** Adds @p clipId at the end, unless it's already open. */
    void open(int clipId)
    {
        if (! contains(clipId))
            ids_.push_back(clipId);
    }

    /** Removes @p clipId. Returns the clip to show in its place if it was the
        one showing: the entry after it, or before it if it was last; 0 when
        nothing else is open or it wasn't open. */
    int close(int clipId)
    {
        const auto it = std::find(ids_.begin(), ids_.end(), clipId);
        if (it == ids_.end())
            return 0;

        const auto index = (size_t) (it - ids_.begin());
        ids_.erase(it);
        if (ids_.empty())
            return 0;
        return ids_[std::min(index, ids_.size() - 1)];
    }

    void closeAll() { ids_.clear(); }

    /** The entry @p direction steps (+1 next, -1 previous) from @p clipId,
        wrapping at either end. The first entry when @p clipId isn't open; 0
        when nothing is. */
    int neighbour(int clipId, int direction) const
    {
        if (ids_.empty())
            return 0;

        const auto it = std::find(ids_.begin(), ids_.end(), clipId);
        if (it == ids_.end())
            return ids_.front();

        const auto count = (int) ids_.size();
        const auto index = (int) (it - ids_.begin());
        return ids_[(size_t) (((index + direction) % count + count) % count)];
    }

    /** Drops every entry that isn't an audio clip on @p song's arrangement.
        True if anything was dropped. */
    bool prune(const model::Song& song)
    {
        const auto before = ids_.size();
        ids_.erase(std::remove_if(ids_.begin(), ids_.end(),
                                  [&song](int id) { return ! locate(song, id).isValid(); }),
                   ids_.end());
        return ids_.size() != before;
    }

    struct Location
    {
        int track = -1;
        int clip  = -1;

        bool isValid() const noexcept { return track >= 0 && clip >= 0; }
    };

    /** Where audio clip @p clipId is on @p song's arrangement, if it is. */
    static Location locate(const model::Song& song, int clipId)
    {
        for (size_t t = 0; t < song.tracks.size(); ++t)
        {
            const auto& clips = song.tracks[t].clips;
            for (size_t c = 0; c < clips.size(); ++c)
                if (clips[c].id == clipId && clips[c].type == model::ClipType::Audio)
                    return { (int) t, (int) c };
        }
        return {};
    }

private:
    std::vector<int> ids_;
};

} // namespace soundsplice::app
