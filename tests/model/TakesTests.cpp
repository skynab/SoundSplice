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

TEST_CASE("Swiping a take across comped pieces comps each, and joins what carries on", "[model][takes]")
{
    int  trackId = 0;
    auto song    = twoPasses(trackId);
    const int id = takeedit::combineIntoTakes(song, trackId, { song.tracks[0].clips[0].id, song.tracks[0].clips[1].id });

    // b.wav | a.wav over beat 2-3 | b.wav
    REQUIRE(takeedit::compRange(song, trackId, id, 2.0, 3.0, 0));
    REQUIRE(song.tracks[0].clips.size() == 3);

    // A swipe of a.wav from 1.5 to 4 crosses all three pieces.
    REQUIRE(takeedit::compTrackRange(song, trackId, 1.5, 4.0, 0));
    const auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 3);
    REQUIRE(clips[0].audioFile == "b.wav");
    REQUIRE(clips[0].lengthBeats == Approx(1.5));
    REQUIRE(clips[1].audioFile == "a.wav");
    REQUIRE(clips[1].startBeats == Approx(1.5));
    REQUIRE(clips[1].lengthBeats == Approx(2.5)); // one piece, joined across the old cut
    REQUIRE(clips[1].sourceOffsetSeconds == Approx(0.75));
    REQUIRE(clips[2].audioFile == "b.wav");
    REQUIRE(clips[2].startBeats == Approx(4.0));

    // The same swipe again changes nothing; nor does a take there isn't.
    REQUIRE_FALSE(takeedit::compTrackRange(song, trackId, 1.5, 4.0, 0));
    REQUIRE_FALSE(takeedit::compTrackRange(song, trackId, 1.5, 4.0, 5));
    REQUIRE_FALSE(takeedit::compTrackRange(song, trackId, 3.0, 3.0, 1));
}

TEST_CASE("A loop recording's passes are found in its one file", "[model][takes]")
{
    // A 4-second loop from 10 s, capture starting at 9 s (a second of
    // pre-roll), 13.5 s recorded: the first pass, two more full passes - at
    // 5 and 9 s into the file - and a half-second scrap at 13 s.
    auto offsets = takeedit::loopPassOffsets(9.0, 10.0, 14.0, 13.5, 1.0);
    REQUIRE(offsets.size() == 3);
    REQUIRE(offsets[0] == Approx(1.0));  // the loop starts a second into the file
    REQUIRE(offsets[1] == Approx(5.0));
    REQUIRE(offsets[2] == Approx(9.0));

    // With the scrap long enough to keep, it's a take too.
    REQUIRE(takeedit::loopPassOffsets(9.0, 10.0, 14.0, 13.5, 0.25).size() == 4);

    // Started inside the loop: the first pass has nothing before that.
    offsets = takeedit::loopPassOffsets(12.0, 10.0, 14.0, 10.0, 1.0);
    REQUIRE(offsets.size() == 3);
    REQUIRE(offsets[0] == Approx(-2.0));
    REQUIRE(offsets[1] == Approx(2.0));

    // Never went round, or started after the loop: an ordinary recording.
    REQUIRE(takeedit::loopPassOffsets(10.0, 10.0, 14.0, 3.0, 1.0).empty());
    REQUIRE(takeedit::loopPassOffsets(15.0, 10.0, 14.0, 30.0, 1.0).empty());
}

TEST_CASE("A loop recording becomes one clip over the loop with a take per pass", "[model][takes]")
{
    Clip clip;
    clip.type = ClipType::Audio;
    takeedit::makeLoopTakes(clip, "rec.wav", { -2.0, 2.0, 6.0 }, 16.0, 8.0);

    REQUIRE(clip.startBeats == 16.0);
    REQUIRE(clip.lengthBeats == 8.0);
    REQUIRE(clip.takes.size() == 3);
    REQUIRE(clip.activeTake == 2);                   // the last pass plays
    REQUIRE(clip.sourceOffsetSeconds == Approx(6.0));

    REQUIRE(takeedit::setActiveTake(clip, 0));
    REQUIRE(clip.sourceOffsetSeconds == Approx(-2.0)); // each pass lines up with the loop
    REQUIRE(clip.audioFile == "rec.wav");
    REQUIRE(clip.takes[1].name == "Pass 2");
}

TEST_CASE("Punching in replaces the range and nothing either side of it", "[model][takes]")
{
    Song song;
    song.bpm          = 120.0;
    const int trackId = addTrack(song, TrackType::Audio, "Vox").id;

    Clip original;
    original.type        = ClipType::Audio;
    original.audioFile   = "verse.wav";
    original.lengthBeats = 16.0;
    addClip(song, trackId, original);

    // Recorded from beat 4 with a bar of pre-roll, punched over beats 8 to 12.
    Clip recorded;
    recorded.type        = ClipType::Audio;
    recorded.audioFile   = "fix.wav";
    recorded.startBeats  = 4.0;
    recorded.lengthBeats = 12.0;

    const int id = takeedit::punchIn(song, trackId, recorded, 8.0, 12.0, 0.01);
    REQUIRE(id != 0);

    const auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 3);

    const auto at = [&](double beat) -> const Clip*
    {
        for (const auto& clip : clips)
            if (beat >= clip.startBeats && beat < clip.startBeats + clip.lengthBeats)
                return &clip;
        return nullptr;
    };
    REQUIRE(at(2.0)->audioFile == "verse.wav");
    REQUIRE(at(10.0)->audioFile == "fix.wav");
    REQUIRE(at(10.0)->id == id);
    REQUIRE(at(10.0)->sourceOffsetSeconds == Approx(2.0)); // four beats past where it was recorded from
    REQUIRE(at(14.0)->audioFile == "verse.wav");
    REQUIRE(at(14.0)->sourceOffsetSeconds == Approx(6.0)); // the original, carrying on where it was

    // Every edge of the punch fades.
    REQUIRE(at(2.0)->fades.outSeconds == Approx(0.01));
    REQUIRE(at(10.0)->fades.inSeconds == Approx(0.01));
    REQUIRE(at(10.0)->fades.outSeconds == Approx(0.01));
    REQUIRE(at(14.0)->fades.inSeconds == Approx(0.01));

    // A recording that stops before the range punches nothing.
    Clip short_ = recorded;
    short_.lengthBeats = 2.0;
    REQUIRE(takeedit::punchIn(song, trackId, short_, 8.0, 12.0, 0.01) == 0);
}
