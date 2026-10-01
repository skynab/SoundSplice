#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/Serialization.h>
#include <model/TempoChanges.h>

using Catch::Approx;
using namespace soundsplice::model;

namespace
{
    /** An audio clip at beat 16 and a synth clip at beat 16, at 120 bpm. */
    Song twoClips()
    {
        Song song;
        song.bpm = 120.0;
        for (const auto type : { TrackType::Audio, TrackType::Instrument })
        {
            const int id = addTrack(song, type, "t").id;
            Clip clip;
            clip.type        = type == TrackType::Audio ? ClipType::Audio : ClipType::Instrument;
            clip.startBeats  = 16.0;
            clip.lengthBeats = 4.0;
            addClip(song, id, clip);
        }
        song.markers.push_back({ 1, 16.0, 0.0, "here" });
        return song;
    }
}

TEST_CASE("A tempo change keeps audio at its time and instrument parts on their beats", "[model][tempo]")
{
    auto song = twoClips();

    // From beat 4 at 60 bpm: beat 16 was 8 s in. Beats 0..4 still take 2 s,
    // and after that a beat is a second, so 8 s in is now beat 4 + 6 = 10.
    REQUIRE(tempoedit::setTempo(song, 4.0, 60.0));
    REQUIRE(song.tempoChanges.size() == 1);

    const auto& audio = song.tracks[0].clips[0];
    REQUIRE(audio.startBeats == Approx(10.0).margin(1.0e-4));
    REQUIRE(audio.lengthBeats == Approx(2.0).margin(1.0e-4)); // 2 s, now a beat a second
    REQUIRE(song.tracks[1].clips[0].startBeats == 16.0);         // the synth part stays
    REQUIRE(song.markers[0].startBeats == Approx(10.0).margin(1.0e-4));

    // Removing it puts everything back.
    REQUIRE(tempoedit::removeChange(song, 4.0));
    REQUIRE(song.tempoChanges.empty());
    REQUIRE(song.tracks[0].clips[0].startBeats == Approx(16.0).margin(1.0e-4));
}

TEST_CASE("The tempo control edits whichever tempo is in force", "[model][tempo]")
{
    auto song = twoClips();
    tempoedit::setTempo(song, 8.0, 90.0);

    REQUIRE(tempoedit::changeInForceAt(song, 2.0) == 0.0);
    REQUIRE(tempoedit::changeInForceAt(song, 8.0) == 8.0);
    REQUIRE(tempoedit::changeInForceAt(song, 30.0) == 8.0);

    tempoedit::setTempo(song, 0.0, 100.0); // the start
    REQUIRE(song.bpm == 100.0);
    REQUIRE(song.tempoChanges[0].bpm == 90.0);
    REQUIRE(tempoedit::tempoAt(song, 9.0) == Approx(90.0));
}

TEST_CASE("Tempo changes move, ramp, and are refused where they can't go", "[model][tempo]")
{
    auto song = twoClips();
    tempoedit::setTempo(song, 8.0, 60.0);
    tempoedit::setTempo(song, 12.0, 90.0);

    REQUIRE_FALSE(tempoedit::moveChange(song, 8.0, 12.0)); // onto another
    REQUIRE_FALSE(tempoedit::moveChange(song, 8.0, 0.0));  // onto the start
    REQUIRE(tempoedit::moveChange(song, 12.0, 4.0));
    REQUIRE(song.tempoChanges[0].beat == 4.0); // kept sorted

    REQUIRE(tempoedit::toggleRamp(song, 8.0));
    REQUIRE(song.tempoChanges[1].ramp);
    REQUIRE(tempoedit::tempoAt(song, 6.0) < 90.0); // sliding from 90 at beat 4 to 60 at beat 8
    REQUIRE(tempoedit::tempoAt(song, 6.0) > 60.0);

    REQUIRE_FALSE(tempoedit::setTempo(song, 4.0, 0.0));
    REQUIRE_FALSE(tempoedit::removeChange(song, 5.0));
}

TEST_CASE("Tempo changes round-trip", "[model][tempo][io]")
{
    auto song = twoClips();
    tempoedit::setTempo(song, 8.0, 60.0);
    tempoedit::setTempo(song, 12.0, 140.5);
    tempoedit::toggleRamp(song, 12.0);

    Song restored;
    REQUIRE(deserialize(serialize(song), restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tempoChanges[1].ramp);
}
