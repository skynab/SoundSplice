#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/SequencerMath.h>

using Catch::Approx;
using looper::engine::edgeInBlock;
using looper::engine::wrapPositive;

TEST_CASE("wrapPositive keeps results within the pattern length", "[engine][seq]")
{
    REQUIRE(wrapPositive(5.0, 4.0)  == Approx(1.0));
    REQUIRE(wrapPositive(-1.0, 4.0) == Approx(3.0));
    REQUIRE(wrapPositive(0.0, 4.0)  == Approx(0.0));
    REQUIRE(wrapPositive(8.0, 4.0)  == Approx(0.0));
    REQUIRE(wrapPositive(3.5, 4.0)  == Approx(3.5));
}

TEST_CASE("edgeInBlock detects an edge inside the block", "[engine][seq]")
{
    int offset = -1;
    REQUIRE(edgeInBlock(50.0, 0.0, 1000.0, 100, offset));
    REQUIRE(offset == 50);

    REQUIRE(edgeInBlock(0.0, 0.0, 1000.0, 100, offset));
    REQUIRE(offset == 0);
}

TEST_CASE("edgeInBlock rejects an edge outside the block", "[engine][seq]")
{
    int offset = -1;
    REQUIRE_FALSE(edgeInBlock(150.0, 0.0, 1000.0, 100, offset));
    REQUIRE_FALSE(edgeInBlock(999.0, 0.0, 1000.0, 100, offset));
}

TEST_CASE("edgeInBlock handles pattern wrap-around", "[engine][seq]")
{
    int offset = -1;

    // Block that runs off the end of the pattern.
    REQUIRE(edgeInBlock(990.0, 950.0, 1000.0, 100, offset));
    REQUIRE(offset == 40);

    // Edge at time 30 with the block starting at 950 wraps: (30-950) mod 1000 = 80.
    REQUIRE(edgeInBlock(30.0, 950.0, 1000.0, 100, offset));
    REQUIRE(offset == 80);
}

TEST_CASE("A loop runs over what has been arranged", "[engine][loop]")
{
    // The loop used to be a hardcoded four bars whatever the song held, so
    // arranging anything longer silently looped only its opening.
    REQUIRE(looper::engine::loopEndForContent(16.0, 4.0) == 16.0); // exactly four bars
    REQUIRE(looper::engine::loopEndForContent(13.0, 4.0) == 16.0); // rounded up to the bar
    REQUIRE(looper::engine::loopEndForContent(0.5, 4.0)  == 4.0);
}

TEST_CASE("An empty song still gets a loop of one bar", "[engine][loop]")
{
    // A zero-length loop region would stall the transport or divide by zero
    // downstream, and an empty song is exactly when that would happen.
    REQUIRE(looper::engine::loopEndForContent(0.0, 4.0) == 4.0);
    REQUIRE(looper::engine::loopEndForContent(-5.0, 4.0) == 4.0);
    REQUIRE(looper::engine::loopEndForContent(0.0, 3.0) == 3.0); // and in 3/4
}

TEST_CASE("The loop end follows the time signature", "[engine][loop]")
{
    REQUIRE(looper::engine::loopEndForContent(10.0, 3.0) == 12.0); // 3/4: four bars
    REQUIRE(looper::engine::loopEndForContent(10.0, 5.0) == 10.0); // 5/4: two bars exactly
}

TEST_CASE("A nonsense bar length falls back rather than dividing by zero", "[engine][loop]")
{
    REQUIRE(looper::engine::loopEndForContent(10.0, 0.0) == 12.0); // treated as 4/4
    REQUIRE(std::isfinite(looper::engine::loopEndForContent(10.0, -1.0)));
}

TEST_CASE("Play at the end of the arrangement starts it again", "[engine][transport]")
{
    // Otherwise it resumes into silence and the end-of-song check stops it
    // again within a frame, which reads as a play button that does nothing.
    REQUIRE(looper::engine::shouldRestartFromStart(16.0, 16.0));
    REQUIRE(looper::engine::shouldRestartFromStart(20.0, 16.0)); // stopped a little past it
}

TEST_CASE("Play from the middle carries on from there", "[engine][transport]")
{
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(0.0, 16.0));
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(8.0, 16.0));
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(15.0, 16.0));
}

TEST_CASE("Landing a hair short of the end still counts as the end", "[engine][transport]")
{
    // The playhead is reconstructed from a sample count, so it can land just
    // either side. A hair short must not resume-and-immediately-stop.
    REQUIRE(looper::engine::shouldRestartFromStart(16.0 - 1.0e-9, 16.0));
    REQUIRE(looper::engine::shouldRestartFromStart(16.0 - 1.0e-4, 16.0));

    // But the margin stays far below anything audible — half a millisecond at
    // 120bpm — so a real position just before the end is still just before it.
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(15.99, 16.0));
}

TEST_CASE("With nothing arranged there is no end to restart from", "[engine][transport]")
{
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(0.0, 0.0));
    REQUIRE_FALSE(looper::engine::shouldRestartFromStart(50.0, 0.0));
}

TEST_CASE("Seconds convert to beats at the song's tempo", "[engine][tempo]")
{
    // Two seconds at 120bpm is four beats. A recorded take is sized with this,
    // and so is an imported file — they used to compute it separately and only
    // one of them was right.
    REQUIRE(looper::engine::beatsForSeconds(2.0, 120.0) == Approx(4.0));
    REQUIRE(looper::engine::beatsForSeconds(2.0, 60.0) == Approx(2.0));
    REQUIRE(looper::engine::beatsForSeconds(30.0, 120.0) == Approx(60.0));
}

TEST_CASE("A long recording is a long clip", "[engine][tempo]")
{
    // The specific fault: a thirty-second take used to be given a four-beat
    // clip, which made the whole song look four beats long.
    const double beats = looper::engine::beatsForSeconds(30.0, 120.0);
    REQUIRE(beats > 4.0);
    REQUIRE(beats == Approx(60.0));
}

TEST_CASE("A nonsense duration or tempo converts to nothing", "[engine][tempo]")
{
    // Callers fall back to a default on zero, so this must not return a
    // plausible-looking length for a take that never happened.
    REQUIRE(looper::engine::beatsForSeconds(0.0, 120.0) == 0.0);
    REQUIRE(looper::engine::beatsForSeconds(-5.0, 120.0) == 0.0);
    REQUIRE(looper::engine::beatsForSeconds(10.0, 0.0) == 0.0);
    REQUIRE(looper::engine::beatsForSeconds(10.0, -120.0) == 0.0);
}

TEST_CASE("A beat division converts to Hz at the song's tempo", "[engine][tempo]")
{
    // A quarter note at 120bpm is two per second, so 2Hz. Halving the
    // division doubles the rate; doubling the tempo also doubles it — this
    // is the arithmetic a tempo-synced wobble or LFO is built on.
    REQUIRE(looper::engine::hzForBeatDivision(120.0, 1.0) == Approx(2.0));
    REQUIRE(looper::engine::hzForBeatDivision(120.0, 0.5) == Approx(4.0));
    REQUIRE(looper::engine::hzForBeatDivision(240.0, 1.0) == Approx(4.0));
}

TEST_CASE("A nonsense tempo or division converts to no rate", "[engine][tempo]")
{
    // Zero here must mean "don't advance the phase", not a division by zero.
    REQUIRE(looper::engine::hzForBeatDivision(0.0, 1.0) == 0.0);
    REQUIRE(looper::engine::hzForBeatDivision(-120.0, 1.0) == 0.0);
    REQUIRE(looper::engine::hzForBeatDivision(120.0, 0.0) == 0.0);
    REQUIRE(looper::engine::hzForBeatDivision(120.0, -1.0) == 0.0);
}
