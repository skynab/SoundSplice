#pragma once

#include <algorithm>
#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    Bus tracks and sends. A bus (TrackType::Bus) holds no clips: it mixes
    what other tracks give it - their whole output (Track::outputBusId) or a
    send (Track::sends) - through its own effects and fader, and on to the
    master or another bus. Folders (model/Folders.h) stay organization only;
    routing is these.

    The rule is no loops: a track may only feed a bus that doesn't already
    feed it, however indirectly. A routing to a track that has gone, or has
    stopped being a bus, means the master (for an output) or nothing (for a
    send), so deleting a bus needs no clean-up elsewhere.

    JUCE-free; the engine's side of the same graph is engine/MixRouting.h.
*/
namespace routing
{
    inline bool isBus(const Track& track) { return track.type == TrackType::Bus; }

    /** A track can hold clips: anything but a bus. */
    inline bool holdsClips(const Track& track) { return ! isBus(track); }

    /** The bus @p busId names, if it is one. */
    inline const Track* busById(const Song& song, int busId)
    {
        const auto* track = busId != 0 ? findTrack(song, busId) : nullptr;
        return track != nullptr && isBus(*track) ? track : nullptr;
    }

    /** True if @p fromId's audio reaches @p toId, through outputs and sends. */
    inline bool feeds(const Song& song, int fromId, int toId)
    {
        std::vector<int> visited, pending { fromId };
        while (! pending.empty())
        {
            const int at = pending.back();
            pending.pop_back();
            if (std::find(visited.begin(), visited.end(), at) != visited.end())
                continue;
            visited.push_back(at);

            const auto* track = findTrack(song, at);
            if (track == nullptr)
                continue;

            std::vector<int> next;
            if (busById(song, track->outputBusId) != nullptr)
                next.push_back(track->outputBusId);
            for (const auto& send : track->sends)
                if (busById(song, send.busId) != nullptr)
                    next.push_back(send.busId);

            for (const int target : next)
            {
                if (target == toId)
                    return true;
                pending.push_back(target);
            }
        }
        return false;
    }

    /** Whether track @p fromId may feed bus @p busId: it's a bus, not the
        track itself, and doesn't already feed it back. */
    inline bool canFeed(const Song& song, int fromId, int busId)
    {
        return busById(song, busId) != nullptr && busId != fromId && ! feeds(song, busId, fromId);
    }

    /** The buses track @p trackId may feed, in track order. */
    inline std::vector<int> busesFor(const Song& song, int trackId)
    {
        std::vector<int> buses;
        for (const auto& track : song.tracks)
            if (canFeed(song, trackId, track.id))
                buses.push_back(track.id);
        return buses;
    }

    /** Routes track @p trackId's output to bus @p busId, or to the master
        (0). False, changing nothing, if that would make a loop. */
    inline bool setOutput(Song& song, int trackId, int busId)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr || (busId != 0 && ! canFeed(song, trackId, busId)))
            return false;
        track->outputBusId = busId;
        return true;
    }

    /** Adds a send from @p trackId to @p busId at @p levelDb. False if it
        would make a loop or there is already one to that bus. */
    inline bool addSend(Song& song, int trackId, int busId, float levelDb = 0.0f, bool preFader = false)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr || ! canFeed(song, trackId, busId)
            || std::any_of(track->sends.begin(), track->sends.end(), [busId](const TrackSend& s) { return s.busId == busId; }))
            return false;
        track->sends.push_back({ busId, levelDb, preFader });
        return true;
    }

    /** The bus track @p index's output goes to, as an index, or -1 for the
        master (also for a routing to something that isn't a bus). */
    inline int outputIndex(const Song& song, int index)
    {
        if (index < 0 || index >= (int) song.tracks.size())
            return -1;
        const int busId = song.tracks[(size_t) index].outputBusId;
        for (int i = 0; i < (int) song.tracks.size(); ++i)
            if (song.tracks[(size_t) i].id == busId && isBus(song.tracks[(size_t) i]) && i != index)
                return i;
        return -1;
    }
}

} // namespace soundsplice::model
