#include <catch2/catch_test_macros.hpp>

#include <engine/Transport.h>

using looper::engine::Transport;

TEST_CASE("Transport advances only while playing", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.seek(0);

    t.advance(512); // stopped — no movement
    REQUIRE(t.playhead() == 0);

    t.setPlaying(true);
    t.advance(512);
    REQUIRE(t.playhead() == 512);
    t.advance(488);
    REQUIRE(t.playhead() == 1000);

    t.setPlaying(false);
    t.advance(1000);
    REQUIRE(t.playhead() == 1000);
}

TEST_CASE("Transport wraps inside a loop starting at zero", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.setLoopRegion(0, 1000);
    t.setLooping(true);
    t.setPlaying(true);
    t.seek(0);

    t.advance(1500); // 1500 -> wrap to 500
    REQUIRE(t.playhead() == 500);

    t.advance(600); // 1100 -> wrap to 100
    REQUIRE(t.playhead() == 100);
}

TEST_CASE("Transport loop wrap respects a non-zero loop start", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.setLoopRegion(1000, 2000); // length 1000
    t.setLooping(true);
    t.setPlaying(true);
    t.seek(1900);

    t.advance(200); // 2100 -> 1000 + (2100-1000)%1000 = 1100
    REQUIRE(t.playhead() == 1100);
}

TEST_CASE("Transport publishes state to the UI snapshot", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.setPlaying(true);
    t.seek(0);

    t.advance(256);
    REQUIRE(t.playheadForUI() == 256);
    REQUIRE(t.playingForUI() == true);
}
