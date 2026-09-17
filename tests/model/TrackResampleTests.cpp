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
            clip.type                = ClipType::Audio;
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

    track.sessionSlots.resize(2);
    track.sessionSlots[1].hasClip        = true;
    track.sessionSlots[1].clip.type      = ClipType::Audio;
    track.sessionSlots[1].clip.audioFile = "c.wav";

    Clip instrument;
    track.clips.push_back(instrument);

    REQUIRE(audioFilesOf(track) == std::vector<std::string> { "a.wav", "b.wav", "c.wav" });
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
