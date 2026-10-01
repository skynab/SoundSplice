#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "model/RazorEdits.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Track edit groups: tracks given the same group number (Track::editGroup)
    are edited as one, as a multitracked drum kit or a stereo pair of mics
    should be. A time selection or razor area made on one takes in the rest,
    mute and solo follow, a fader moves the others' by as much, and moving a
    clip moves the clips lined up with it on the others: the ones that start
    and end where it does, recorded in the same take.

    The rules are here, JUCE-free; MainComponent applies them where each of
    those edits happens.
*/
inline constexpr int kEditGroupCount = 8;

namespace groupedit
{
    /** The indices of every track in @p trackIndex's group, itself first,
        or just itself when it's in none. */
    inline std::vector<int> memberIndices(const Song& song, int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
            return {};

        std::vector<int> members { trackIndex };
        const int        group = song.tracks[(size_t) trackIndex].editGroup;
        if (group <= 0)
            return members;

        for (int i = 0; i < (int) song.tracks.size(); ++i)
            if (i != trackIndex && song.tracks[(size_t) i].editGroup == group)
                members.push_back(i);
        return members;
    }

    /** @p trackIds with every member of their groups added, in track order. */
    inline std::vector<int> withGroupMembers(const Song& song, const std::vector<int>& trackIds)
    {
        std::vector<int> groups;
        for (const auto& track : song.tracks)
            if (track.editGroup > 0 && std::find(trackIds.begin(), trackIds.end(), track.id) != trackIds.end())
                groups.push_back(track.editGroup);

        std::vector<int> ids;
        for (const auto& track : song.tracks)
            if (std::find(trackIds.begin(), trackIds.end(), track.id) != trackIds.end()
                || std::find(groups.begin(), groups.end(), track.editGroup) != groups.end())
                ids.push_back(track.id);
        return ids;
    }

    /** @p areas with each one repeated on the other tracks of its track's
        group, where those can take a razor area. */
    inline RazorAreas withGroupAreas(const Song& song, const RazorAreas& areas)
    {
        RazorAreas out = areas;
        for (const auto& area : areas)
        {
            const int index = razoredit::trackIndexOf(song, area.trackId);
            for (const int member : memberIndices(song, index))
                if (rangeedit::appliesTo(song.tracks[(size_t) member]))
                    out = razoredit::addArea(song, std::move(out),
                                             { song.tracks[(size_t) member].id, area.startBeats, area.endBeats });
        }
        return out;
    }

    /** The clips on the other tracks of @p trackIndex's group lined up with
        clip @p clipIndex - starting and ending where it does - as (track,
        clip) indices. */
    inline std::vector<std::pair<int, int>> alignedClips(const Song& song, int trackIndex, int clipIndex)
    {
        std::vector<std::pair<int, int>> aligned;
        if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
            return aligned;
        const auto& clips = song.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return aligned;

        constexpr double kEpsilon = 1.0e-6;
        const auto&      clip     = clips[(size_t) clipIndex];
        for (const int member : memberIndices(song, trackIndex))
        {
            if (member == trackIndex)
                continue;
            const auto& others = song.tracks[(size_t) member].clips;
            for (int c = 0; c < (int) others.size(); ++c)
                if (std::abs(others[(size_t) c].startBeats - clip.startBeats) < kEpsilon
                    && std::abs(others[(size_t) c].lengthBeats - clip.lengthBeats) < kEpsilon)
                    aligned.push_back({ member, c });
        }
        return aligned;
    }

    /** Sets a fader-like value on @p trackIndex to @p value and moves every
        other member of its group by as much as it moved from @p base, each
        kept to [@p lo, @p hi]. @p get and @p set read and write the value on
        a track.

        Measured from @p base - the song as a drag began, or @p song itself
        for a single change - so a member held at the end of its range on
        the way comes back to where it was, rather than drifting by what the
        clamp took. */
    template <typename Get, typename Set>
    void setRelative(Song& song, const Song& base, int trackIndex, float value, float lo, float hi, Get get, Set set)
    {
        if (trackIndex < 0 || trackIndex >= (int) song.tracks.size() || base.tracks.size() != song.tracks.size())
            return;

        const float delta = value - get(base.tracks[(size_t) trackIndex]);
        for (const int member : memberIndices(song, trackIndex))
            set(song.tracks[(size_t) member],
                member == trackIndex ? value : std::clamp(get(base.tracks[(size_t) member]) + delta, lo, hi));
    }
}

} // namespace soundsplice::model
