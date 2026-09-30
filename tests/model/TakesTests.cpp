#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/Takes.h>

using Catch::Approx;
using namespace soundsplice::model;

namespace
{
    /** Two passes over bars 1-2 at 120 bpm (half a second a beat): the first
        from the start of its file, the second recorded a beat later and
        trimmed two seconds into its file. */
    Song twoPasses(int& trackId)
    {
        Song song;
        song.bpm = 120.0;
        trackId  = addTrack(song, TrackType::Audio, "Vox").id;

        Clip first;
        first.type        = ClipType::Audio;
        first.audioFile   = "a.wav";
        first.startBeats  = 0.0;
        first.lengthBeats = 4.0;
        addClip(song, trackId, first);

        Clip second                = first;
        second.audioFile           = "b.wav";
        second.startBeats          = 1.0;
        second.sourceOffsetSeconds = 2.0;
        addClip(song, trackId, second);
        return song;
    }
}

TEST_CASE("Combining clips into takes keeps every recording where it was", "[model][takes]")
{
    int  trackId = 0;
    auto song    = twoPasses(trackId);
    const int id = takeedit::combineIntoTakes(song, trackId, { song.tracks[0].clips[0].id, song.tracks[0].clips[1].id });
    REQUIRE(id != 0);

    const auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 1);
    const auto& clip = clips[0];
    REQUIRE(clip.id == id);
    REQUIRE(clip.startBeats == 0.0);
    REQUIRE(clip.lengthBeats == 5.0);         // both passes' whole span
    REQUIRE(clip.takes.size() == 2);
    REQUIRE(clip.audioFile == "b.wav");        // the latest plays
    REQUIRE(clip.sourceOffsetSeconds == Approx(1.5)); // where b.wav is at beat 0

    // Switching takes lines each one up with the timeline as it was recorded.
    auto other = clip;
    REQUIRE(takeedit::setActiveTake(other, 0));
    REQUIRE(other.audioFile == "a.wav");
    REQUIRE(other.sourceOffsetSeconds == Approx(0.0));
    REQUIRE(takeedit::setActiveTake(other, 1));
    REQUIRE(other.sourceOffsetSeconds == Approx(1.5));
    REQUIRE_FALSE(takeedit::setActiveTake(other, 2));

    // Nothing to combine with a single clip.
    REQUIRE(takeedit::combineIntoTakes(song, trackId, { id }) == 0);
}

TEST_CASE("Comping plays another take over a range, and comping back joins it again", "[model][takes]")
{
    int  trackId = 0;
    auto song    = twoPasses(trackId);
    const int id = takeedit::combineIntoTakes(song, trackId, { song.tracks[0].clips[0].id, song.tracks[0].clips[1].id });

    // The first pass's third beat.
    REQUIRE(takeedit::compRange(song, trackId, id, 2.0, 3.0, 0));
    auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 3);
    REQUIRE(clips[0].audioFile == "b.wav");
    REQUIRE(clips[0].id == id); // the first piece keeps the clip's id
    REQUIRE(clips[1].audioFile == "a.wav");
    REQUIRE(clips[1].startBeats == 2.0);
    REQUIRE(clips[1].lengthBeats == 1.0);
    REQUIRE(clips[1].sourceOffsetSeconds == Approx(1.0)); // a.wav one second in, at beat 2
    REQUIRE(clips[2].audioFile == "b.wav");
    REQUIRE(clips[2].sourceOffsetSeconds == Approx(3.0));
    REQUIRE(clips[1].takes.size() == 2); // every piece can still be comped

    // Already that take: nothing to do.
    REQUIRE_FALSE(takeedit::compRange(song, trackId, clips[1].id, 2.0, 3.0, 0));

    REQUIRE(takeedit::compRange(song, trackId, clips[1].id, 2.0, 3.0, 1));
    REQUIRE(song.tracks[0].clips.size() == 1);
    REQUIRE(song.tracks[0].clips[0].lengthBeats == 5.0);
    REQUIRE(song.tracks[0].clips[0].sourceOffsetSeconds == Approx(1.5));
}
