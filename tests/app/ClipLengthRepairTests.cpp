#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <app/ClipLengthRepair.h>
#include <model/Track.h>

using namespace looper;

namespace
{
    model::Clip audioClip(std::string file, double lengthBeats)
    {
        model::Clip clip;
        clip.type        = model::ClipType::Audio;
        clip.audioFile    = std::move(file);
        clip.lengthBeats  = lengthBeats;
        return clip;
    }

    model::Clip instrumentClip(double lengthBeats)
    {
        model::Clip clip;
        clip.type        = model::ClipType::Instrument;
        clip.lengthBeats = lengthBeats;
        return clip;
    }
}

TEST_CASE("A clip left over from the four-beat bug is corrected", "[app][repair]")
{
    model::Song song;
    song.bpm = 120.0;
    model::Track track;
    track.clips.push_back(audioClip("take.wav", 4.0)); // wrong: was a ten-second take
    song.tracks.push_back(track);

    auto probe = [](const std::string& file) { return file == "take.wav" ? 10.0 : 0.0; };
    auto fixes = repairAudioClipLengths(song, probe);

    REQUIRE(fixes.size() == 1);
    CHECK(fixes[0].trackIndex == 0);
    CHECK(fixes[0].clipIndex == 0);
    CHECK(fixes[0].oldLengthBeats == 4.0);
    CHECK(fixes[0].newLengthBeats == Catch::Approx(20.0)); // 10s at 120bpm = 20 beats
    CHECK(song.tracks[0].clips[0].lengthBeats == Catch::Approx(20.0));
}

TEST_CASE("A clip whose length already matches its file is left alone", "[app][repair]")
{
    model::Song song;
    song.bpm = 120.0;
    model::Track track;
    track.clips.push_back(audioClip("take.wav", 20.0)); // already correct
    song.tracks.push_back(track);

    auto probe = [](const std::string&) { return 10.0; };
    auto fixes = repairAudioClipLengths(song, probe);

    REQUIRE(fixes.empty());
    CHECK(song.tracks[0].clips[0].lengthBeats == 20.0);
}

TEST_CASE("A file that can't be probed is left untouched rather than guessed at", "[app][repair]")
{
    model::Song song;
    model::Track track;
    track.clips.push_back(audioClip("missing.wav", 4.0));
    song.tracks.push_back(track);

    auto probe = [](const std::string&) { return 0.0; }; // simulates a missing/unreadable file
    auto fixes = repairAudioClipLengths(song, probe);

    REQUIRE(fixes.empty());
    CHECK(song.tracks[0].clips[0].lengthBeats == 4.0);
}

TEST_CASE("Instrument clips are never touched", "[app][repair]")
{
    model::Song song;
    model::Track track;
    track.clips.push_back(instrumentClip(4.0));
    song.tracks.push_back(track);

    auto probe = [](const std::string&) { return 999.0; }; // would be a huge mismatch, if it applied
    auto fixes = repairAudioClipLengths(song, probe);

    REQUIRE(fixes.empty());
    CHECK(song.tracks[0].clips[0].lengthBeats == 4.0);
}

TEST_CASE("Repairing multiple clips across tracks reports each one", "[app][repair]")
{
    model::Song song;
    song.bpm = 60.0; // 1 beat per second, easy to check by eye
    model::Track trackA;
    trackA.clips.push_back(audioClip("a.wav", 4.0));
    model::Track trackB;
    trackB.clips.push_back(audioClip("ok.wav", 5.0));
    trackB.clips.push_back(audioClip("b.wav", 1.0));
    song.tracks.push_back(trackA);
    song.tracks.push_back(trackB);

    auto probe = [](const std::string& file) -> double
    {
        if (file == "a.wav") return 5.0;
        if (file == "ok.wav") return 5.0;
        if (file == "b.wav") return 8.0;
        return 0.0;
    };
    auto fixes = repairAudioClipLengths(song, probe);

    REQUIRE(fixes.size() == 2);
    CHECK(fixes[0].trackIndex == 0);
    CHECK(fixes[0].clipIndex == 0);
    CHECK(fixes[0].newLengthBeats == Catch::Approx(5.0));
    CHECK(fixes[1].trackIndex == 1);
    CHECK(fixes[1].clipIndex == 1);
    CHECK(fixes[1].newLengthBeats == Catch::Approx(8.0));
    CHECK(song.tracks[1].clips[0].lengthBeats == 5.0); // untouched
}
