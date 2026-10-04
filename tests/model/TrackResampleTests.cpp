#include <catch2/catch_test_macros.hpp>

#include <model/TrackResample.h>

using namespace soundsplice::model;
using namespace soundsplice::model::trackresample;

namespace
{
    Track trackPlaying(std::initializer_list<const char*> files)
    {
        Track track;
        track.type = TrackType::Audio;
        for (const auto* file : files)
        {
            Clip clip;
            clip.audioFile           = file;
            clip.sourceOffsetSeconds = 1.5;
            track.clips.push_back(clip);
        }
        return track;
    }
}

TEST_CASE("A track's audio files are listed once each", "[model][resample]")
{
    auto track = trackPlaying({ "a.wav", "b.wav", "a.wav" });

    Clip empty;
    track.clips.push_back(empty);

    REQUIRE(audioFilesOf(track) == std::vector<std::string> { "a.wav", "b.wav" });
}

TEST_CASE("Replacing files repoints the clips and changes nothing else", "[model][resample]")
{
    auto       track  = trackPlaying({ "a.wav", "b.wav", "a.wav" });
    const auto before = track;

    REQUIRE(replaceAudioFiles(track, { { "a.wav", "a-48000.wav" } }) == 2);
    REQUIRE(track.clips[0].audioFile == "a-48000.wav");
    REQUIRE(track.clips[1].audioFile == "b.wav");
    REQUIRE(track.clips[2].audioFile == "a-48000.wav");

    track.clips[0].audioFile = "a.wav";
    track.clips[2].audioFile = "a.wav";
    REQUIRE(track == before);
}
