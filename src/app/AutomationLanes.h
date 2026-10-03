#pragma once

#include <algorithm>
#include <string>

#include "app/AutomationGeometry.h"
#include "model/Song.h"

namespace soundsplice
{
/**
    Finding and storing a song's automation lanes. JUCE-free and tested: the
    automation pane's edits and automation recording both go through here,
    and the lane is the whole of what the user drew or played.
*/

/** A lane anywhere in the song: the master's volume (track -1), or
    @p target on a track. */
struct LaneKey
{
    int              track = -1;
    AutomationTarget target;

    bool isMaster() const noexcept { return track < 0; }
    bool operator==(const LaneKey&) const = default;

    static LaneKey master() { return {}; }
    static LaneKey trackParam(int track, model::TrackParam param) { return { track, AutomationTarget::track(param) }; }
    static LaneKey effect(int track, int slot, model::EffectKind kind, std::string paramId)
    {
        return { track, AutomationTarget::effect(slot, kind, std::move(paramId)) };
    }
};

namespace automationlanes
{
    /** The lane @p key names, created empty if it has none yet; nullptr if
        its track is gone, or its slot no longer holds the effect it was for. */
    inline model::AutomationLane* laneFor(model::Song& song, const LaneKey& key)
    {
        if (key.isMaster())
            return &song.masterGainDb;
        if (key.track >= (int) song.tracks.size())
            return nullptr;

        auto&       track  = song.tracks[(size_t) key.track];
        const auto& target = key.target;
        if (! target.isEffect())
            return &track.laneFor(target.trackParam);
        if (target.slot >= (int) track.effectChain.size() || track.effectChain[(size_t) target.slot].kind != target.kind)
            return nullptr;
        return &track.effectChain[(size_t) target.slot].automation[target.paramId];
    }

    /** Puts @p lane on @p track as @p target's. An emptied lane is erased
        rather than kept empty, so a track with no automation carries no lanes
        - what every save and playback path treats as "the static value".
        False, changing nothing, if the slot no longer holds the effect. */
    inline bool storeLane(model::Track& track, const AutomationTarget& target, const model::AutomationLane& lane)
    {
        if (! target.isEffect())
        {
            if (lane.empty())
                track.automation.erase((int) target.trackParam);
            else
                track.laneFor(target.trackParam) = lane;
            return true;
        }

        if (target.slot >= (int) track.effectChain.size() || track.effectChain[(size_t) target.slot].kind != target.kind)
            return false;
        auto& lanes = track.effectChain[(size_t) target.slot].automation;
        if (lane.empty())
            lanes.erase(target.paramId);
        else
            lanes[target.paramId] = lane;
        return true;
    }

    /** Every lane in @p from - the master's, the tracks', their effects' -
        put into @p to, wherever the same track and the same effect still are.
        Nothing else in @p to changes. */
    inline void copyLanes(const model::Song& from, model::Song& to)
    {
        to.masterGainDb = from.masterGainDb;
        for (size_t t = 0; t < std::min(from.tracks.size(), to.tracks.size()); ++t)
        {
            const auto& source = from.tracks[t];
            auto&       target = to.tracks[t];
            if (source.id != target.id)
                continue;

            target.automation = source.automation;
            for (size_t s = 0; s < std::min(source.effectChain.size(), target.effectChain.size()); ++s)
                if (source.effectChain[s].kind == target.effectChain[s].kind)
                    target.effectChain[s].automation = source.effectChain[s].automation;
        }
    }

    /** True if @p a's lanes, put into @p b, would change it. */
    inline bool lanesDiffer(const model::Song& a, const model::Song& b)
    {
        auto probe = b;
        copyLanes(a, probe);
        return ! (probe == b);
    }
}

} // namespace soundsplice
