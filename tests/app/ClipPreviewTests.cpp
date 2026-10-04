#include <catch2/catch_test_macros.hpp>

#include <app/ClipPreview.h>

using namespace soundsplice;

TEST_CASE("A file shorter than its clip leaves the rest silent", "[app][preview]")
{
    // Audio clips don't loop: a two-second file in an eight-beat clip at
    // 120bpm (four seconds) is heard for the first half. Drawing a waveform
    // across the whole clip would be a picture of something it doesn't do.
    REQUIRE(audioClipDrawnFraction(2.0, 8.0, 0.5) == 0.5);
    REQUIRE(audioClipAudibleSeconds(2.0, 8.0, 0.5) == 2.0);
}

TEST_CASE("A file longer than its clip is cut at the clip's end", "[app][preview]")
{
    // Only what's heard is drawn; the rest of the file isn't shown.
    REQUIRE(audioClipDrawnFraction(30.0, 8.0, 0.5) == 1.0);
    REQUIRE(audioClipAudibleSeconds(30.0, 8.0, 0.5) == 4.0);
}

TEST_CASE("A file exactly filling its clip covers the whole width", "[app][preview]")
{
    REQUIRE(audioClipDrawnFraction(4.0, 8.0, 0.5) == 1.0);
    REQUIRE(audioClipAudibleSeconds(4.0, 8.0, 0.5) == 4.0);
}

TEST_CASE("Tempo changes what a clip can hold", "[app][preview]")
{
    // The same clip in beats is fewer seconds at a faster tempo, so more of
    // it is silence — or less of the file fits.
    const double atOneTwenty = audioClipDrawnFraction(4.0, 8.0, 0.5);  // 4s of room
    const double atSixty     = audioClipDrawnFraction(4.0, 8.0, 1.0);  // 8s of room

    REQUIRE(atOneTwenty == 1.0);
    REQUIRE(atSixty == 0.5);
}

TEST_CASE("Nothing to draw reports nothing to draw", "[app][preview]")
{
    // Callers skip the work on zero, so these must not return a usable-looking
    // fraction for a clip that has no audio or no length.
    REQUIRE(audioClipDrawnFraction(0.0, 8.0, 0.5) == 0.0);
    REQUIRE(audioClipDrawnFraction(4.0, 0.0, 0.5) == 0.0);
    REQUIRE(audioClipDrawnFraction(4.0, 8.0, 0.0) == 0.0);
    REQUIRE(audioClipDrawnFraction(-1.0, 8.0, 0.5) == 0.0);
    REQUIRE(audioClipAudibleSeconds(0.0, 8.0, 0.5) == 0.0);
}
