#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/TimeSelection.h>

using namespace soundsplice::model;
using namespace soundsplice::model::rangeedit;
using Catch::Matchers::WithinAbs;

namespace
{
    /** At 60 bpm a beat is a second, so beats and source offsets read alike. */
    struct Fixture
    {
        Song song;
        int  audioA = 0, audioB = 0, synth = 0;

        Fixture()
        {
            song.bpm = 60.0;
            audioA   = addTrack(song, TrackType::Audio, "A").id;
            audioB   = addTrack(song, TrackType::Audio, "B").id;
            synth    = addTrack(song, TrackType::Instrument, "Synth").id;
        }

        Clip& add(int trackId, double start, double length, double offset = 0.0)
        {
            Clip clip;
            clip.type                = ClipType::Audio;
            clip.audioFile           = "take.wav";
            clip.startBeats          = start;
            clip.lengthBeats         = length;
            clip.sourceOffsetSeconds = offset;
            return *addClip(song, trackId, clip);
        }

        const std::vector<Clip>& clips(int trackId) const { return findTrack(song, trackId)->clips; }

        TimeSelection over(double from, double to, std::vector<int> tracks) const
        {
            return { from, to, std::move(tracks) };
        }
    };

    void requireClip(const Clip& clip, double start, double length, double offset)
    {
        REQUIRE_THAT(clip.startBeats, WithinAbs(start, 1.0e-9));
        REQUIRE_THAT(clip.lengthBeats, WithinAbs(length, 1.0e-9));
        REQUIRE_THAT(clip.sourceOffsetSeconds, WithinAbs(offset, 1.0e-9));
    }
}

TEST_CASE("A drag selects every lane between its ends, in time order", "[model][timeselection]")
{
    Fixture f;

    const auto down = selectionFromDrag(f.song, 8.0, 2.0, 2, 0);
    REQUIRE(down.startBeats == 2.0);
    REQUIRE(down.endBeats == 8.0);
    REQUIRE(down.trackIds == std::vector<int> { f.audioA, f.audioB, f.synth });

    const auto cursor = selectionFromDrag(f.song, 3.0, 3.0, 1, 1);
    REQUIRE(cursor.isEmpty());
    REQUIRE(cursor.hasTracks());

    REQUIRE(selectionFromDrag(f.song, -4.0, 1.0, 9, 9).startBeats == 0.0);
    REQUIRE(selectionFromDrag(f.song, -4.0, 1.0, 9, 9).trackIds == std::vector<int> { f.synth });
}

TEST_CASE("Delete closes the gap, keeping the audio either side where it was in the file", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioA, 0.0, 10.0, 5.0);  // spans the selection
    f.add(f.audioA, 12.0, 2.0);       // after it
    f.add(f.audioB, 3.0, 2.0);        // running into it, on the other track

    removeRange(f.song, f.over(4.0, 8.0, { f.audioA, f.audioB }), true);

    const auto& a = f.clips(f.audioA);
    REQUIRE(a.size() == 3);
    requireClip(a[0], 0.0, 4.0, 5.0);
    requireClip(a[1], 4.0, 2.0, 5.0 + 8.0); // what played from beat 8 now plays from 4
    requireClip(a[2], 8.0, 2.0, 0.0);
    REQUIRE(a[0].id != a[1].id);

    // The part before the selection is only the part before it.
    const auto& b = f.clips(f.audioB);
    REQUIRE(b.size() == 1);
    requireClip(b[0], 3.0, 1.0, 0.0);
}

TEST_CASE("Silence leaves the gap and moves nothing", "[model][timeselection]")
{
    Fixture f;
    auto& clip = f.add(f.audioA, 0.0, 10.0);
    clip.fades.inSeconds  = 1.0;
    clip.fades.outSeconds = 2.0;
    f.add(f.audioA, 12.0, 2.0);

    removeRange(f.song, f.over(4.0, 8.0, { f.audioA }), false);

    const auto& a = f.clips(f.audioA);
    REQUIRE(a.size() == 3);
    requireClip(a[0], 0.0, 4.0, 0.0);
    requireClip(a[1], 8.0, 2.0, 8.0);
    requireClip(a[2], 12.0, 2.0, 0.0);

    // Each half keeps the fade on the edge it still has.
    REQUIRE(a[0].fades.inSeconds == 1.0);
    REQUIRE(a[0].fades.outSeconds == 0.0);
    REQUIRE(a[1].fades.inSeconds == 0.0);
    REQUIRE(a[1].fades.outSeconds == 2.0);
}

TEST_CASE("Tracks outside the selection, and instrument tracks, are left alone", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioB, 0.0, 10.0);

    Clip pattern;
    pattern.type        = ClipType::Instrument;
    pattern.lengthBeats = 10.0;
    addClip(f.song, f.synth, pattern);

    const auto before = f.song;
    removeRange(f.song, f.over(2.0, 6.0, { f.audioA, f.synth }), true);

    REQUIRE(f.clips(f.audioB) == before.tracks[1].clips);
    REQUIRE(f.clips(f.synth) == before.tracks[2].clips);
    REQUIRE(anyTrackApplies(f.song, f.over(2.0, 6.0, { f.audioA })));
    REQUIRE_FALSE(anyTrackApplies(f.song, f.over(2.0, 6.0, { f.synth })));
}

TEST_CASE("Copy then paste puts the same audio back, pushing later clips along", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioA, 0.0, 10.0, 1.0);
    f.add(f.audioB, 5.0, 4.0);

    const auto selection = f.over(4.0, 6.0, { f.audioA, f.audioB });
    const auto clipboard = copyRange(f.song, selection);

    REQUIRE(clipboard.lengthBeats == 2.0);
    REQUIRE(clipboard.tracks.size() == 2);
    requireClip(clipboard.tracks[0].at(0), 0.0, 2.0, 5.0);
    requireClip(clipboard.tracks[1].at(0), 1.0, 1.0, 0.0); // B only starts at 5

    // Pasted at beat 7 on B alone: the first clipboard track goes to B.
    REQUIRE(insertClipboard(f.song, { f.audioB }, clipboard, 7.0));

    const auto& b = f.clips(f.audioB);
    REQUIRE(b.size() == 3);
    requireClip(b[0], 5.0, 2.0, 0.0);  // the part before 7
    requireClip(b[1], 9.0, 2.0, 2.0);  // the rest, pushed on by 2
    requireClip(b[2], 7.0, 2.0, 5.0);  // A's copied audio

    // A wasn't pasted to, so it didn't move.
    requireClip(f.clips(f.audioA)[0], 0.0, 10.0, 1.0);
}

TEST_CASE("Cut and paste at the same place is a round trip", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioA, 0.0, 10.0, 3.0);
    f.add(f.audioA, 11.0, 1.0);

    const auto selection = f.over(2.0, 5.0, { f.audioA });
    const auto clipboard = copyRange(f.song, selection);
    removeRange(f.song, selection, true);
    REQUIRE(insertClipboard(f.song, selection.trackIds, clipboard, selection.startBeats));

    // Three pieces of the original clip, back to back, and the second clip back
    // where it started.
    auto clips = f.clips(f.audioA);
    std::sort(clips.begin(), clips.end(), [](const Clip& x, const Clip& y) { return x.startBeats < y.startBeats; });

    REQUIRE(clips.size() == 4);
    requireClip(clips[0], 0.0, 2.0, 3.0);
    requireClip(clips[1], 2.0, 3.0, 5.0);
    requireClip(clips[2], 5.0, 5.0, 8.0);
    requireClip(clips[3], 11.0, 1.0, 0.0);
}

TEST_CASE("Nothing to paste, or nowhere to paste it, changes nothing", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioA, 0.0, 4.0);
    const auto before = f.song;

    REQUIRE_FALSE(insertClipboard(f.song, { f.audioA }, RangeClipboard {}, 1.0));
    REQUIRE_FALSE(insertClipboard(f.song, { f.synth }, copyRange(f.song, f.over(0.0, 1.0, { f.audioA })), 1.0));
    REQUIRE(f.song.tracks == before.tracks);
}
