#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/Serialization.h>
#include <model/TrackChannels.h>

using namespace soundsplice::model;
using namespace soundsplice::model::channelops;
using soundsplice::engine::ClipChannels;

namespace
{
    Song songWithAudioTrack()
    {
        Song song;
        const int id = addTrack(song, TrackType::Audio, "Interview").id;

        Clip clip;
        clip.type        = ClipType::Audio;
        clip.audioFile   = "interview.wav";
        clip.lengthBeats = 8.0;
        addClip(song, id, clip);

        clip.startBeats = 10.0;
        clip.channels   = ClipChannels::Swapped;
        addClip(song, id, clip);

        addTrack(song, TrackType::Instrument, "Synth");
        return song;
    }
}

TEST_CASE("Splitting a stereo track makes a left track and a right track", "[model][channels]")
{
    auto song = songWithAudioTrack();

    REQUIRE(splitStereoToMono(song, 0));
    REQUIRE(song.tracks.size() == 3);

    const auto& left  = song.tracks[0];
    const auto& right = song.tracks[1];
    REQUIRE(left.name == "Interview (L)");
    REQUIRE(right.name == "Interview (R)");
    REQUIRE(left.id != right.id);

    REQUIRE(left.clips[0].channels == ClipChannels::LeftOnly);
    REQUIRE(right.clips[0].channels == ClipChannels::RightOnly);

    // A swapped clip's left was the file's right, and stays so.
    REQUIRE(left.clips[1].channels == ClipChannels::RightOnly);
    REQUIRE(right.clips[1].channels == ClipChannels::LeftOnly);

    REQUIRE(song.tracks[2].name == "Synth");
    REQUIRE_FALSE(splitStereoToMono(song, 2)); // not an audio track
    REQUIRE_FALSE(splitStereoToMono(song, 9));
}

TEST_CASE("Swapping a track's channels twice puts them back", "[model][channels]")
{
    auto       song   = songWithAudioTrack();
    const auto before = song;

    REQUIRE(swapChannels(song, 0));
    REQUIRE(song.tracks[0].clips[0].channels == ClipChannels::Swapped);
    REQUIRE(song.tracks[0].clips[1].channels == ClipChannels::Both);

    REQUIRE(swapChannels(song, 0));
    REQUIRE(song == before);

    REQUIRE_FALSE(swapChannels(song, 1));
}

TEST_CASE("A clip's channels are saved with the project", "[model][channels]")
{
    auto song = songWithAudioTrack();
    splitStereoToMono(song, 0);

    Song        loaded;
    std::string error;
    REQUIRE(deserialize(serialize(song), loaded, &error));
    REQUIRE(loaded.tracks[0].clips[0].channels == ClipChannels::LeftOnly);
    REQUIRE(loaded.tracks[1].clips[0].channels == ClipChannels::RightOnly);
    REQUIRE(loaded.tracks[0].clips[1].channels == ClipChannels::RightOnly);
}

TEST_CASE("Split halves join back into the track they came from", "[model][channels]")
{
    auto       song   = songWithAudioTrack();
    const auto before = song;

    REQUIRE_FALSE(areSplitHalves(song, 0)); // the next track is the synth
    REQUIRE(splitStereoToMono(song, 0));
    REQUIRE(areSplitHalves(song, 0));
    REQUIRE_FALSE(areSplitHalves(song, 1));

    REQUIRE(joinSplitHalves(song, 0));
    REQUIRE(song.tracks == before.tracks); // the split used up ids, so the song's counter moved on
}

TEST_CASE("Tracks that aren't untouched split halves don't join", "[model][channels]")
{
    auto song = songWithAudioTrack();
    REQUIRE(splitStereoToMono(song, 0));

    SECTION("a half's gain was changed")
    {
        song.tracks[1].gainDb = -6.0f;
    }
    SECTION("a clip on one half was moved")
    {
        song.tracks[1].clips[1].startBeats += 1.0;
    }
    SECTION("both halves play the same side")
    {
        song.tracks[1].clips[0].channels = ClipChannels::LeftOnly;
    }
    SECTION("a half has an extra clip")
    {
        song.tracks[0].clips.push_back(song.tracks[0].clips[0]);
    }

    const auto before = song;
    REQUIRE_FALSE(areSplitHalves(song, 0));
    REQUIRE_FALSE(joinSplitHalves(song, 0));
    REQUIRE(song == before);
}

TEST_CASE("Folding a panned track to mono recovers the signal that was panned", "[model][channels]")
{
    // Unity-centre linear pan law: centre is 1 and 1, hard left 1 and 0.
    REQUIRE_THAT(foldToMono(0.5f, 0.5f, 1.0f, 1.0f), Catch::Matchers::WithinAbs(0.5f, 1e-6));
    REQUIRE_THAT(foldToMono(0.8f, 0.0f, 1.0f, 0.0f), Catch::Matchers::WithinAbs(0.8f, 1e-6));
    REQUIRE_THAT(foldToMono(0.0f, 0.8f, 0.0f, 1.0f), Catch::Matchers::WithinAbs(0.8f, 1e-6));
    REQUIRE_THAT(foldToMono(0.4f, 0.2f, 1.0f, 0.5f), Catch::Matchers::WithinAbs(0.4f, 1e-6)); // pan -0.5 of a 0.4 signal
    REQUIRE_THAT(foldToMono(0.3f, 0.1f, 1.0f, 1.0f), Catch::Matchers::WithinAbs(0.2f, 1e-6)); // a stereo source averages
    REQUIRE_THAT(foldToMono(0.3f, 0.1f, 0.0f, 0.0f), Catch::Matchers::WithinAbs(0.0f, 1e-6));
}
