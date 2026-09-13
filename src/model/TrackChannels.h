#pragma once

#include <string>

#include "engine/ClipChannels.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Track channel operations: splitting a stereo track into two mono tracks
    and swapping a track's left and right. Both only change which channels
    each clip plays (engine/ClipChannels.h), so neither writes audio and both
    undo exactly. Audio tracks only; session-grid clips change along with the
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
} // namespace channelops

} // namespace soundsplice::model
