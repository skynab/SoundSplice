#include <catch2/catch_test_macros.hpp>

#include <app/LoudnessMatch.h>

using namespace soundsplice;
using namespace soundsplice::model;

namespace
{
    Song threeTracks()
    {
        Song song;
        for (const auto type : { TrackType::Audio, TrackType::Audio, TrackType::Instrument })
        {
            const int id = addTrack(song, type, "t").id;
            for (const double start : { 8.0, 0.0 })
            {
                Clip clip;
                clip.type        = type == TrackType::Audio ? ClipType::Audio : ClipType::Instrument;
                clip.audioFile   = type == TrackType::Audio ? "a.wav" : "";
                clip.startBeats  = start;
                clip.lengthBeats = 4.0;
                addClip(song, id, clip);
            }
        }
        return song;
    }
}

TEST_CASE("Match Loudness takes the clips the time selection touches, in time order", "[app][loudnessmatch]")
{
    const auto song = threeTracks();
    const TimeSelection selection { 2.0, 9.0, { song.tracks[0].id, song.tracks[2].id } };

    const auto clips = app::clipsToMatch(song, selection, -1);
    REQUIRE(clips.size() == 2); // track 0's two clips; the instrument track has no audio
    REQUIRE(clips[0] == app::MatchedClip { song.tracks[0].id, song.tracks[0].clips[1].id }); // the one at beat 0 first
    REQUIRE(clips[1] == app::MatchedClip { song.tracks[0].id, song.tracks[0].clips[0].id });

    // A selection that misses the clip at 8 leaves it out.
    const TimeSelection early { 0.0, 3.0, { song.tracks[0].id } };
    REQUIRE(app::clipsToMatch(song, early, -1).size() == 1);
}

TEST_CASE("Without a time selection, Match Loudness takes the selected track's clips", "[app][loudnessmatch]")
{
    const auto song = threeTracks();
    REQUIRE(app::clipsToMatch(song, {}, 1).size() == 2);
    REQUIRE(app::clipsToMatch(song, {}, 2).empty());
    REQUIRE(app::clipsToMatch(song, {}, -1).empty());
}
