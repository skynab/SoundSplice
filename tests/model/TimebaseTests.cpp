#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/Timebase.h>

using namespace soundsplice::model;
using Catch::Matchers::WithinAbs;

namespace
{
    Song songWithAudioAndBus()
    {
        Song song;
        song.bpm = 120.0;

        Clip audio;
        audio.audioFile   = "take.wav";
        audio.startBeats  = 4.0; // 2 s at 120 bpm
        audio.lengthBeats = 8.0; // 4 s
        audio.sourceOffsetSeconds = 1.5;
        audio.fades.inSeconds     = 0.25;

        auto& audioTrack = addTrack(song, TrackType::Audio, "Vocal");
        audioTrack.clips.push_back(audio);
        audioTrack.laneFor(TrackParam::Gain).addPoint(2.0, -3.0f);
        audioTrack.laneFor(TrackParam::Gain).addPoint(6.0, 0.0f);

        auto& bus = addTrack(song, TrackType::Bus, "Reverb");
        bus.laneFor(TrackParam::Gain).addPoint(2.0, -6.0f);

        song.masterGainDb.addPoint(8.0, -1.0f);
        return song;
    }
}

TEST_CASE("A tempo change leaves audio at the same time in seconds", "[model][timebase]")
{
    auto song = songWithAudioAndBus();
    retimeAudioForTempoChange(song, 120.0, 60.0);

    const auto& audio = song.tracks[0].clips[0];
    REQUIRE_THAT(audio.startBeats, WithinAbs(2.0, 1e-9));  // still 2 s at 60 bpm
    REQUIRE_THAT(audio.lengthBeats, WithinAbs(4.0, 1e-9)); // still 4 s

    // Already in seconds, so untouched.
    REQUIRE(audio.sourceOffsetSeconds == 1.5);
    REQUIRE(audio.fades.inSeconds == 0.25);

    // Its automation moves with it.
    const auto& points = song.tracks[0].lane(TrackParam::Gain)->points();
    REQUIRE_THAT(points[0].beat, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(points[1].beat, WithinAbs(3.0, 1e-9));
    REQUIRE(points[0].value == -3.0f);
}

TEST_CASE("A tempo change leaves a bus and the master lane on their beats", "[model][timebase]")
{
    auto song = songWithAudioAndBus();
    retimeAudioForTempoChange(song, 120.0, 90.0);

    REQUIRE(song.tracks[1].lane(TrackParam::Gain)->points()[0].beat == 2.0);
    REQUIRE(song.masterGainDb.points()[0].beat == 8.0);
}

TEST_CASE("Changing the tempo and changing it back restores the song", "[model][timebase]")
{
    const auto original = songWithAudioAndBus();
    auto       song     = original;

    retimeAudioForTempoChange(song, 120.0, 97.3);
    retimeAudioForTempoChange(song, 97.3, 120.0);

    REQUIRE_THAT(song.tracks[0].clips[0].startBeats, WithinAbs(original.tracks[0].clips[0].startBeats, 1e-9));
    REQUIRE_THAT(song.tracks[0].clips[0].lengthBeats, WithinAbs(original.tracks[0].clips[0].lengthBeats, 1e-9));
}

TEST_CASE("A tempo that isn't a tempo changes nothing", "[model][timebase]")
{
    const auto original = songWithAudioAndBus();

    for (const auto& [from, to] : { std::pair { 120.0, 120.0 }, std::pair { 0.0, 90.0 }, std::pair { 120.0, -1.0 } })
    {
        auto song = original;
        retimeAudioForTempoChange(song, from, to);
        REQUIRE(song == original);
    }
}
