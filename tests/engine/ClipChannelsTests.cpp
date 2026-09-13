#include <catch2/catch_test_macros.hpp>

#include <engine/ClipChannels.h>

using namespace soundsplice::engine;

TEST_CASE("Each channel mode sends the right file channel to each output", "[engine][clipchannels]")
{
    // A stereo file.
    REQUIRE(sourceChannelFor(ClipChannels::Both, 0, 2) == 0);
    REQUIRE(sourceChannelFor(ClipChannels::Both, 1, 2) == 1);

    REQUIRE(sourceChannelFor(ClipChannels::LeftOnly, 0, 2) == 0);
    REQUIRE(sourceChannelFor(ClipChannels::LeftOnly, 1, 2) == 0);

    REQUIRE(sourceChannelFor(ClipChannels::RightOnly, 0, 2) == 1);
    REQUIRE(sourceChannelFor(ClipChannels::RightOnly, 1, 2) == 1);

    REQUIRE(sourceChannelFor(ClipChannels::Swapped, 0, 2) == 1);
    REQUIRE(sourceChannelFor(ClipChannels::Swapped, 1, 2) == 0);

    // A mono file has one channel whatever the mode.
    for (const auto mode : { ClipChannels::Both, ClipChannels::LeftOnly, ClipChannels::RightOnly, ClipChannels::Swapped })
    {
        REQUIRE(sourceChannelFor(mode, 0, 1) == 0);
        REQUIRE(sourceChannelFor(mode, 1, 1) == 0);
    }

    // More outputs than the file has channels reuse its last one.
    REQUIRE(sourceChannelFor(ClipChannels::Both, 3, 2) == 1);
}

TEST_CASE("Swapping twice is no change, and stored values read back", "[engine][clipchannels]")
{
    for (const auto mode : { ClipChannels::Both, ClipChannels::LeftOnly, ClipChannels::RightOnly, ClipChannels::Swapped })
    {
        REQUIRE(swappedChannels(swappedChannels(mode)) == mode);
        REQUIRE(clipChannelsFrom((int) mode) == mode);
    }

    REQUIRE(swappedChannels(ClipChannels::LeftOnly) == ClipChannels::RightOnly);
    REQUIRE(clipChannelsFrom(-1) == ClipChannels::Both);
    REQUIRE(clipChannelsFrom(9) == ClipChannels::Both);
}
