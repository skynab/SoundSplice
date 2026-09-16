#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/ArrangementEdits.h>

using namespace soundsplice::model;
using namespace soundsplice::model::arrangeedit;
using Catch::Matchers::WithinAbs;

namespace
{
    /** At 60 bpm a beat is a second, so beats and source offsets read alike. */
    struct Fixture
    {
        Song song;
        int  audio = 0, other = 0, synth = 0;

        Fixture()
        {
            song.bpm = 60.0;
            audio    = addTrack(song, TrackType::Audio, "Audio").id;
            other    = addTrack(song, TrackType::Audio, "Other").id;
            synth    = addTrack(song, TrackType::Instrument, "Synth").id;
        }

        Clip& add(int trackId, double start, double length, double offset = 0.0, const char* file = "take.wav")
        {
            Clip clip;
            clip.type                = ClipType::Audio;
            clip.audioFile           = file;
            clip.startBeats          = start;
            clip.lengthBeats         = length;
            clip.sourceOffsetSeconds = offset;
            return *addClip(song, trackId, clip);
        }

        std::vector<Clip> sorted(int trackId) const
        {
            auto clips = findTrack(song, trackId)->clips;
            std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.startBeats < b.startBeats; });
            return clips;
        }
    };

    void requireClip(const Clip& clip, double start, double length, double offset)
    {
        REQUIRE_THAT(clip.startBeats, WithinAbs(start, 1.0e-9));
        REQUIRE_THAT(clip.lengthBeats, WithinAbs(length, 1.0e-9));
        REQUIRE_THAT(clip.sourceOffsetSeconds, WithinAbs(offset, 1.0e-9));
    }
}

TEST_CASE("Splitting at a beat splits only the clips it falls inside, on the chosen tracks", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 10.0, 2.0);
    f.add(f.audio, 12.0, 4.0);          // not under the beat
    f.add(f.other, 0.0, 10.0);          // under it, but not chosen
    f.add(f.audio, 20.0, 5.0);

    REQUIRE(splitClipsAt(f.song, { f.audio, f.synth }, 4.0) == 1);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 4);
    requireClip(clips[0], 0.0, 4.0, 2.0);
    requireClip(clips[1], 4.0, 6.0, 6.0);
    REQUIRE(clips[0].id != clips[1].id);

    REQUIRE(f.sorted(f.other).size() == 1);

    // Exactly on a clip's edge there is nothing to split.
    REQUIRE(splitClipsAt(f.song, { f.audio }, 12.0) == 0);
}

TEST_CASE("Joining undoes a split, keeping the outer fades", "[model][arrange]")
{
    Fixture f;
    auto& original = f.add(f.audio, 2.0, 10.0, 1.0);
    original.fades.inSeconds  = 0.5;
    original.fades.outSeconds = 1.5;
    const auto before = original;

    REQUIRE(splitClipsAt(f.song, { f.audio }, 5.0) == 1);
    REQUIRE(splitClipsAt(f.song, { f.audio }, 8.0) == 1);
    REQUIRE(f.sorted(f.audio).size() == 3);

    REQUIRE(joinClips(f.song, { f.audio }, 0.0, 100.0) == 2);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 1);
    requireClip(clips[0], 2.0, 10.0, 1.0);
    REQUIRE(clips[0].fades == before.fades);
    REQUIRE(clips[0].id == before.id);
}

TEST_CASE("Only clips that carry straight on from each other are joined", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 4.0, 0.0);
    f.add(f.audio, 4.0, 4.0, 10.0);                 // abuts, but plays from elsewhere in the file
    f.add(f.audio, 8.0, 4.0, 14.0, "other.wav");    // abuts, carries on in time, but another file
    f.add(f.audio, 13.0, 4.0, 18.0);                // a gap before it

    auto& quieter = f.add(f.other, 0.0, 4.0, 0.0);
    auto& louder  = f.add(f.other, 4.0, 4.0, 4.0);
    louder.gainDb = -6.0f;
    (void) quieter;

    REQUIRE(joinClips(f.song, { f.audio, f.other }, 0.0, 100.0) == 0);
    REQUIRE(f.sorted(f.audio).size() == 4);
    REQUIRE(f.sorted(f.other).size() == 2);
}

TEST_CASE("Joins happen only where the join falls inside the range", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 12.0);
    splitClipsAt(f.song, { f.audio }, 4.0);
    splitClipsAt(f.song, { f.audio }, 8.0);

    REQUIRE(joinClips(f.song, { f.audio }, 6.0, 9.0) == 1);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 2);
    requireClip(clips[0], 0.0, 4.0, 0.0);
    requireClip(clips[1], 4.0, 8.0, 4.0);
}

TEST_CASE("Duplicating a selection puts a copy straight after it and selects the copy", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 10.0, 3.0);
    f.add(f.audio, 12.0, 2.0);

    const TimeSelection selection { 2.0, 6.0, { f.audio } };
    const auto copy = duplicateRange(f.song, selection);

    REQUIRE(copy.startBeats == 6.0);
    REQUIRE(copy.endBeats == 10.0);
    REQUIRE(copy.trackIds == selection.trackIds);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 4);
    requireClip(clips[0], 0.0, 6.0, 3.0);   // up to where the copy goes in
    requireClip(clips[1], 6.0, 4.0, 5.0);   // the copy of 2..6
    requireClip(clips[2], 10.0, 4.0, 9.0);  // the rest, pushed on by 4
    requireClip(clips[3], 16.0, 2.0, 0.0);

    // Nothing to duplicate.
    REQUIRE(duplicateRange(f.song, TimeSelection { 2.0, 2.0, { f.audio } }).isEmpty());
    REQUIRE(duplicateRange(f.song, TimeSelection { 2.0, 6.0, {} }).isEmpty());
}

TEST_CASE("Detaching at silences leaves the sounding pieces where they played", "[model][arrange]")
{
    Fixture f;
    auto& clip = f.add(f.audio, 0.0, 10.0, 2.0);
    clip.fades.inSeconds  = 0.5;
    clip.fades.outSeconds = 0.5;
    const int id = clip.id;
    f.add(f.audio, 20.0, 2.0);

    // Silent at the start, in the middle and at the end.
    REQUIRE(detachAtSilences(f.song, f.audio, id, { { 0.0, 1.0 }, { 4.0, 5.0 }, { 9.0, 10.0 } }) == 2);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 3);
    requireClip(clips[0], 1.0, 3.0, 3.0);
    requireClip(clips[1], 5.0, 4.0, 7.0);
    requireClip(clips[2], 20.0, 2.0, 0.0);
    REQUIRE(clips[0].id == id);
    REQUIRE(clips[1].id != id);

    // Neither piece keeps an edge of the original, so neither keeps a fade.
    REQUIRE(clips[0].fades.inSeconds == 0.0);
    REQUIRE(clips[1].fades.outSeconds == 0.0);
}

TEST_CASE("A middle silence keeps the clip's own fades on its outer pieces", "[model][arrange]")
{
    Fixture f;
    auto& clip = f.add(f.audio, 0.0, 10.0);
    clip.fades.inSeconds  = 0.5;
    clip.fades.outSeconds = 1.0;

    REQUIRE(detachAtSilences(f.song, f.audio, clip.id, { { 4.0, 6.0 } }) == 2);

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips[0].fades.inSeconds == 0.5);
    REQUIRE(clips[0].fades.outSeconds == 0.0);
    REQUIRE(clips[1].fades.inSeconds == 0.0);
    REQUIRE(clips[1].fades.outSeconds == 1.0);
}

TEST_CASE("A silent clip is removed, and no silence or no clip changes nothing", "[model][arrange]")
{
    Fixture f;
    const int silent = f.add(f.audio, 0.0, 4.0).id;
    const int loud   = f.add(f.audio, 8.0, 4.0).id;

    REQUIRE(detachAtSilences(f.song, f.audio, silent, { { 0.0, 4.0 } }) == 0);
    REQUIRE(f.sorted(f.audio).size() == 1);

    const auto before = f.song;
    REQUIRE(detachAtSilences(f.song, f.audio, loud, {}) == 1);
    REQUIRE(detachAtSilences(f.song, f.audio, loud, { { 9.0, 12.0 } }) == 1); // past the clip's end
    REQUIRE(f.song == before);

    REQUIRE(detachAtSilences(f.song, f.audio, 999, { { 0.0, 1.0 } }) == -1);
    REQUIRE(detachAtSilences(f.song, f.synth, loud, { { 0.0, 1.0 } }) == -1);
}
