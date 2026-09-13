#pragma once

#include <algorithm>

namespace soundsplice::engine
{
/**
    Which of its file's channels an audio clip plays, and where.

    Non-destructive, like a clip's gain and fades: the file is untouched, so
    splitting a stereo track into two mono ones, or swapping a recording's
    left and right, is a setting that can be changed back, never a render.
*/
enum class ClipChannels
{
    Both      = 0, // left to left, right to right (a mono file on both)
    LeftOnly  = 1, // the file's left channel, on every output
    RightOnly = 2, // the file's right channel, on every output
    Swapped   = 3  // left to right and right to left
};

/** Reads a stored value, falling back to Both for anything unknown. */
inline ClipChannels clipChannelsFrom(int value)
{
    return value >= 0 && value <= 3 ? (ClipChannels) value : ClipChannels::Both;
}

/** The channel of a @p fileChannels-channel file that output channel
    @p outChannel plays under @p mode. A mono file has only one channel to
    give, whatever the mode. */
inline int sourceChannelFor(ClipChannels mode, int outChannel, int fileChannels)
{
    const int last = std::max(0, fileChannels - 1);

    switch (mode)
    {
        case ClipChannels::LeftOnly:  return 0;
        case ClipChannels::RightOnly: return std::min(1, last);
        case ClipChannels::Swapped:
            if (last < 1)
                return 0;
            return outChannel == 0 ? 1 : outChannel == 1 ? 0 : std::min(outChannel, last);
        case ClipChannels::Both:
        default:                      return std::min(std::max(0, outChannel), last);
    }
}

/** @p mode with left and right exchanged. */
inline ClipChannels swappedChannels(ClipChannels mode)
{
    switch (mode)
    {
        case ClipChannels::Both:      return ClipChannels::Swapped;
        case ClipChannels::Swapped:   return ClipChannels::Both;
        case ClipChannels::LeftOnly:  return ClipChannels::RightOnly;
        case ClipChannels::RightOnly: return ClipChannels::LeftOnly;
    }
    return mode;
}

} // namespace soundsplice::engine
