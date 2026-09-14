#include <catch2/catch_test_macros.hpp>

#include <model/ArrangementEdits.h>
#include <model/Serialization.h>

using namespace soundsplice::model;

namespace
{
    Song songWithEnvelope()
    {
        Song song;
        song.bpm = 60.0;
        const int id = addTrack(song, TrackType::Audio, "Voice").id;

        Clip clip;
        clip.type                = ClipType::Audio;
        clip.audioFile           = "voice.wav";
        clip.lengthBeats         = 10.0;
        clip.sourceOffsetSeconds = 1.0;
        clip.envelope.addPoint(2.0, 1.0f);
        clip.envelope.addPoint(4.5, 0.125f);
        clip.envelope.addPoint(8.0, 2.0f);
        addClip(song, id, clip);
        return song;
    }
}

TEST_CASE("A clip's volume curve is saved with the project", "[model][envelope]")
{
    const auto original = songWithEnvelope();

    Song        loaded;
    std::string error;
    REQUIRE(deserialize(serialize(original), loaded, &error));
    REQUIRE(loaded == original);
    REQUIRE(loaded.tracks[0].clips[0].envelope.points().size() == 3);
}

TEST_CASE("A clip without a curve writes none, and a file without one loads as unity", "[model][envelope]")
{
    auto song = songWithEnvelope();
    song.tracks[0].clips[0].envelope.clear();

    const auto text = serialize(song);
    REQUIRE(text.find("CLIPENV") == std::string::npos);

    Song        loaded;
    std::string error;
    REQUIRE(deserialize(text, loaded, &error));
    REQUIRE(loaded.tracks[0].clips[0].envelope.isEmpty());
    REQUIRE(loaded.tracks[0].clips[0].envelope.gainAt(3.0) == 1.0f);
}

TEST_CASE("Splitting a clip keeps the curve on the audio it was drawn over", "[model][envelope]")
{
    auto song = songWithEnvelope();
    const int trackId = song.tracks[0].id;

    // At 60 bpm a beat is a second: split 3 s into the clip, which is 4 s
    // into its file.
    REQUIRE(arrangeedit::splitClipsAt(song, { trackId }, 3.0) == 1);

    const auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 2);

    // Both halves read the same curve at the same place in the file.
    for (const auto& half : clips)
        REQUIRE(half.envelope.gainAt(4.5) == 0.125f);
}
