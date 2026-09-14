#include <catch2/catch_test_macros.hpp>

#include <app/EnvelopeGeometry.h>

#include <cmath>

using namespace soundsplice;

TEST_CASE("Height in a clip's box is gain, unity halfway up", "[app][envelopegeometry]")
{
    // A box from y=10, 100 tall: bottom at 110.
    REQUIRE(app::gainForY(110.0f, 10.0f, 100.0f) == 0.0f);
    REQUIRE(app::gainForY(60.0f, 10.0f, 100.0f) == 1.0f);
    REQUIRE(app::gainForY(10.0f, 10.0f, 100.0f) == app::kEnvelopeTopGain);

    // Outside the box: clamped to it.
    REQUIRE(app::gainForY(0.0f, 10.0f, 100.0f) == app::kEnvelopeTopGain);
    REQUIRE(app::gainForY(500.0f, 10.0f, 100.0f) == 0.0f);

    REQUIRE(app::yForGain(1.0f, 10.0f, 100.0f) == 60.0f);
    REQUIRE(app::yForGain(0.0f, 10.0f, 100.0f) == 110.0f);
    REQUIRE(app::yForGain(4.0f, 10.0f, 100.0f) == 10.0f); // above the top gain: pinned there

    for (const float gain : { 0.0f, 0.25f, 1.0f, 1.75f })
        REQUIRE(std::abs(app::gainForY(app::yForGain(gain, 10.0f, 100.0f), 10.0f, 100.0f) - gain) < 1.0e-5f);
}

TEST_CASE("Beats on the timeline map to seconds in the clip's file and back", "[app][envelopegeometry]")
{
    model::Clip clip;
    clip.startBeats          = 8.0;
    clip.lengthBeats         = 4.0;
    clip.sourceOffsetSeconds = 1.5;

    // 120 bpm: half a second a beat.
    REQUIRE(app::sourceSecondsAtBeat(clip, 8.0, 120.0) == 1.5);
    REQUIRE(app::sourceSecondsAtBeat(clip, 10.0, 120.0) == 2.5);
    REQUIRE(app::beatAtSourceSeconds(clip, 2.5, 120.0) == 10.0);

    // Kept to the part of the file the clip plays: 1.5 s to 3.5 s.
    REQUIRE(app::clampToClipSource(clip, 0.0, 120.0) == 1.5);
    REQUIRE(app::clampToClipSource(clip, 2.0, 120.0) == 2.0);
    REQUIRE(app::clampToClipSource(clip, 9.0, 120.0) == 3.5);
}
