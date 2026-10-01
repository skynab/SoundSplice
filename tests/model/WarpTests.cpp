#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/Serialization.h>
#include <model/Warp.h>

using Catch::Approx;
using namespace soundsplice::model;

namespace
{
    /** A 120 bpm song with a two-second loop at beat 4, played at 100 bpm. */
    Song loopSong(int& trackId, int& clipId)
    {
        Song song;
        song.bpm = 120.0;
        trackId  = addTrack(song, TrackType::Audio, "loop").id;
        Clip clip;
        clip.type                = ClipType::Audio;
        clip.startBeats          = 4.0;
        clip.lengthBeats         = 4.0; // two seconds at 120
        clip.sourceOffsetSeconds = 0.5;
        clip.envelope.addPoint(1.0, 0.5f);
        clipId = addClip(song, trackId, clip)->id;
        return song;
    }
}

TEST_CASE("Warping a clip stretches it to the song's tempo, its audio the same stretch of file", "[model][warp]")
{
    int  trackId = 0, clipId = 0;
    auto song    = loopSong(trackId, clipId);

    REQUIRE_FALSE(warpedit::setWarp(song, trackId, clipId, true)); // no tempo to warp from yet
    REQUIRE(warpedit::setSourceTempo(song, trackId, clipId, 100.0));
    REQUIRE(warpedit::setWarp(song, trackId, clipId, true));

    // At 120 a 100 bpm loop plays 100/120 as long: its offset, curve and
    // window all shrink by that.
    const auto& clip = song.tracks[0].clips[0];
    REQUIRE(warpedit::factorFor(song, clip) == Approx(100.0 / 120.0));
    REQUIRE(clip.sourceOffsetSeconds == Approx(0.5 * 100.0 / 120.0));
    REQUIRE(clip.envelope.points()[0].seconds == Approx(100.0 / 120.0));
    REQUIRE(clip.lengthBeats == Approx(4.0 * 100.0 / 120.0));

    // Unwarping puts it back.
    REQUIRE(warpedit::setWarp(song, trackId, clipId, false));
    REQUIRE(song.tracks[0].clips[0].sourceOffsetSeconds == Approx(0.5));
    REQUIRE(song.tracks[0].clips[0].lengthBeats == Approx(4.0));
}

TEST_CASE("A warped clip stays on its beats when the tempo changes, restretched", "[model][warp]")
{
    int  trackId = 0, clipId = 0;
    auto song    = loopSong(trackId, clipId);
    warpedit::setSourceTempo(song, trackId, clipId, 100.0);
    warpedit::setWarp(song, trackId, clipId, true);
    const double beats  = song.tracks[0].clips[0].lengthBeats;
    const double offset = song.tracks[0].clips[0].sourceOffsetSeconds;

    // Down to 60: the loop now plays 100/60 as long as its file.
    tempoedit::setTempo(song, 0.0, 60.0);
    const auto& clip = song.tracks[0].clips[0];
    REQUIRE(clip.startBeats == 4.0);
    REQUIRE(clip.lengthBeats == Approx(beats));
    REQUIRE(warpedit::factorFor(song, clip) == Approx(100.0 / 60.0));
    REQUIRE(clip.sourceOffsetSeconds == Approx(offset * 120.0 / 60.0));
}

TEST_CASE("Warp round-trips", "[model][warp][io]")
{
    int  trackId = 0, clipId = 0;
    auto song    = loopSong(trackId, clipId);
    warpedit::setSourceTempo(song, trackId, clipId, 97.5);
    warpedit::setWarp(song, trackId, clipId, true);

    Song restored;
    REQUIRE(deserialize(serialize(song), restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[0].clips[0].warp);
    REQUIRE(restored.tracks[0].clips[0].sourceBpm == 97.5);
}
