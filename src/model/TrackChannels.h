#pragma once

#include <string>

#include "engine/ClipChannels.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Track channel operations: splitting a stereo track into two mono tracks,
    joining two such halves again, and swapping a track's left and right.
    All three only change which channels each clip plays
    (engine/ClipChannels.h), so none writes audio and all undo exactly.
    Joining two unrelated mono tracks does need a render; foldToMono is the
    arithmetic for that. Audio tracks only; session-grid clips change along with the
    arrangement's.
*/
namespace channelops
{
    namespace detail
    {
        template <typename Fn>
        void forEachAudioClip(Track& track, Fn&& fn)
        {
            for (auto& clip : track.clips)
                if (clip.type == ClipType::Audio)
                    fn(clip);

            for (auto& slot : track.sessionSlots)
                if (slot.hasClip && slot.clip.type == ClipType::Audio)
                    fn(slot.clip);
        }

        /** What a clip plays once only one side of what it played is kept. */
        inline engine::ClipChannels keepSide(engine::ClipChannels mode, bool left)
        {
            using engine::ClipChannels;
            switch (mode)
            {
                case ClipChannels::Both:    return left ? ClipChannels::LeftOnly : ClipChannels::RightOnly;
                case ClipChannels::Swapped: return left ? ClipChannels::RightOnly : ClipChannels::LeftOnly;
                default:                    return mode; // already one channel, which both halves keep
            }
        }
    }

    /** Splits audio track @p index into two mono tracks: it plays what was
        on its left, and a copy straight after it plays what was on its right.
        False for anything but an audio track. */
    inline bool splitStereoToMono(Song& song, int index)
    {
        if (index < 0 || index >= (int) song.tracks.size() || song.tracks[(size_t) index].type != TrackType::Audio)
            return false;

        if (duplicateTrack(song, index) == nullptr)
            return false;

        // By index: inserting the copy may have moved the tracks.
        auto& left  = song.tracks[(size_t) index];
        auto& right = song.tracks[(size_t) index + 1];

        const auto base = left.name.empty() ? std::string("Track") : left.name;
        left.name  = base + " (L)";
        right.name = base + " (R)";

        detail::forEachAudioClip(left, [](Clip& clip) { clip.channels = detail::keepSide(clip.channels, true); });
        detail::forEachAudioClip(right, [](Clip& clip) { clip.channels = detail::keepSide(clip.channels, false); });
        return true;
    }

    /** Exchanges left and right on every audio clip of track @p index. False
        for anything but an audio track. */
    inline bool swapChannels(Song& song, int index)
    {
        if (index < 0 || index >= (int) song.tracks.size() || song.tracks[(size_t) index].type != TrackType::Audio)
            return false;

        detail::forEachAudioClip(song.tracks[(size_t) index],
                                 [](Clip& clip) { clip.channels = engine::swappedChannels(clip.channels); });
        return true;
    }
    namespace detail
    {
        /** What one clip played before a split, given what its left and right
            halves play now; false if the two aren't the halves of one clip. */
        inline bool joinedSides(engine::ClipChannels left, engine::ClipChannels right, engine::ClipChannels& joined)
        {
            using engine::ClipChannels;
            if (left == ClipChannels::LeftOnly && right == ClipChannels::RightOnly)
                joined = ClipChannels::Both;
            else if (left == ClipChannels::RightOnly && right == ClipChannels::LeftOnly)
                joined = ClipChannels::Swapped;
            else
                return false;
            return true;
        }

        /** @p name without a trailing " (L)", as a split named it. */
        inline std::string withoutSideSuffix(const std::string& name)
        {
            const std::string suffix = " (L)";
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                return name.substr(0, name.size() - suffix.size());
            return name;
        }

        /** @p right with everything that may differ between two halves of a
            split (ids, name, the channel each clip plays) set to @p left's,
            so the two compare equal exactly when nothing else was changed.
            False if a clip pair isn't a left and right half. */
        inline bool asLeftHalf(const Track& left, Track right, Track& out)
        {
            if (right.clips.size() != left.clips.size() || right.sessionSlots.size() != left.sessionSlots.size())
                return false;

            const auto match = [](const Clip& l, Clip& r)
            {
                if (l.type != r.type)
                    return false;
                r.id = l.id;
                if (l.type != ClipType::Audio)
                    return true;
                engine::ClipChannels joined;
                if (! joinedSides(l.channels, r.channels, joined))
                    return false;
                r.channels = l.channels;
                return true;
            };

            for (size_t i = 0; i < left.clips.size(); ++i)
                if (! match(left.clips[i], right.clips[i]))
                    return false;

            for (size_t i = 0; i < left.sessionSlots.size(); ++i)
                if (left.sessionSlots[i].hasClip && right.sessionSlots[i].hasClip
                    && ! match(left.sessionSlots[i].clip, right.sessionSlots[i].clip))
                    return false;

            right.id   = left.id;
            right.name = left.name;
            out        = std::move(right);
            return true;
        }
    }

    /** True if audio track @p index and the track after it are the two halves
        Split Stereo to Mono made: the same clips in the same places, one
        playing each side, and nothing else about them changed since. Only
        then can they be joined back without rendering anything. */
    inline bool areSplitHalves(const Song& song, int index)
    {
        if (index < 0 || index + 1 >= (int) song.tracks.size())
            return false;

        const auto& left  = song.tracks[(size_t) index];
        const auto& right = song.tracks[(size_t) index + 1];
        if (left.type != TrackType::Audio || right.type != TrackType::Audio)
            return false;

        Track normalised;
        return detail::asLeftHalf(left, right, normalised) && normalised == left;
    }

    /** Joins the split halves at @p index and @p index + 1 (see areSplitHalves)
        back into one stereo track, in the left half's place. False, changing
        nothing, if they aren't split halves. */
    inline bool joinSplitHalves(Song& song, int index)
    {
        if (! areSplitHalves(song, index))
            return false;

        auto&       left  = song.tracks[(size_t) index];
        const auto& right = song.tracks[(size_t) index + 1];

        const auto join = [](Clip& l, const Clip& r)
        {
            if (l.type == ClipType::Audio)
                detail::joinedSides(l.channels, r.channels, l.channels);
        };

        for (size_t i = 0; i < left.clips.size(); ++i)
            join(left.clips[i], right.clips[i]);

        for (size_t i = 0; i < left.sessionSlots.size(); ++i)
            if (left.sessionSlots[i].hasClip)
                join(left.sessionSlots[i].clip, right.sessionSlots[i].clip);

        left.name = detail::withoutSideSuffix(left.name);
        song.tracks.erase(song.tracks.begin() + index + 1);
        return true;
    }

    /** One sample of a track's stereo output, folded back to the mono signal
        that was panned to make it, for making a stereo track out of two
        rendered monos. @p leftGain and @p rightGain are the pan law's gains
        for the track's pan: the least-squares estimate, so a centred track
        gives the average of its sides and a hard-panned one the side it's on,
        both at the level it had before panning. */
    inline float foldToMono(float left, float right, float leftGain, float rightGain)
    {
        const float power = leftGain * leftGain + rightGain * rightGain;
        return power > 0.0f ? (left * leftGain + right * rightGain) / power : 0.0f;
    }
} // namespace channelops

} // namespace soundsplice::model
