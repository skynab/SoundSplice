#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/ArrangementEdits.h>

#include <string>

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

TEST_CASE("Crossfading split pieces overlaps them from their hidden audio, fading linearly", "[model][arrange]")
{
    Fixture f;
    // Pieces of one 30 s take split at 10 s.
    f.add(f.audio, 0.0, 10.0, 0.0);
    f.add(f.audio, 10.0, 10.0, 10.0);
    const auto length = [](const std::string&) { return 30.0; };

    REQUIRE(crossfadeClips(f.song, { f.audio }, 9.5, 10.5, length) == 1);

    const auto clips = f.sorted(f.audio);
    requireClip(clips[0], 0.0, 10.5, 0.0);   // runs on half a second
    requireClip(clips[1], 9.5, 10.5, 9.5);   // starts half a second early, from where its audio was
    REQUIRE_THAT(clips[0].fades.outSeconds, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(clips[1].fades.inSeconds, WithinAbs(1.0, 1e-9));
    REQUIRE(clips[0].fades.outShape == soundsplice::engine::FadeShape::Linear);
    REQUIRE(clips[1].fades.inShape == soundsplice::engine::FadeShape::Linear);
}

TEST_CASE("Crossfading unrelated clips fades at equal power, as far as their audio reaches", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 10.0, 0.0, "a.wav");  // a.wav is exactly 10 s: nothing past its end
    f.add(f.audio, 10.0, 5.0, 3.0, "b.wav");  // 3 s of b.wav before its start
    const auto length = [](const std::string& file) { return file == "a.wav" ? 10.0 : 60.0; };

    REQUIRE(crossfadeClips(f.song, { f.audio }, 8.0, 12.0, length) == 1);

    const auto clips = f.sorted(f.audio);
    // The first can't go on past 10 s, so the overlap is [8, 10]: the second
    // moves back 2 s of its 3.
    requireClip(clips[0], 0.0, 10.0, 0.0);
    requireClip(clips[1], 8.0, 7.0, 1.0);
    REQUIRE_THAT(clips[0].fades.outSeconds, WithinAbs(2.0, 1e-9));
    REQUIRE(clips[1].fades.inShape == soundsplice::engine::FadeShape::EqualPower);
}

TEST_CASE("Crossfade leaves clips that don't meet in the selection alone", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 4.0);
    f.add(f.audio, 6.0, 4.0, 6.0);            // a gap
    f.add(f.other, 0.0, 10.0, 0.0);
    f.add(f.other, 10.0, 10.0, 0.0);          // meets at 10, outside the selection
    f.add(f.synth, 0.0, 4.0);
    const auto length = [](const std::string&) { return 60.0; };
    const auto before = f.song;

    REQUIRE(crossfadeClips(f.song, { f.audio, f.other, f.synth }, 3.0, 7.0, length) == 0);
    REQUIRE(f.song == before);

    // And a second clip with no audio before its start has nothing to overlap with.
    Fixture g;
    g.add(g.audio, 0.0, 10.0, 0.0, "a.wav");
    g.add(g.audio, 10.0, 5.0, 0.0, "b.wav");
    REQUIRE(crossfadeClips(g.song, { g.audio }, 9.0, 10.0, [](const std::string&) { return 60.0; }) == 0);
    const auto clips = g.sorted(g.audio);
    requireClip(clips[1], 10.0, 5.0, 0.0);   // couldn't move back
    requireClip(clips[0], 0.0, 10.0, 0.0);   // nor could the overlap start before it
}

TEST_CASE("Gaps are what no sound on any track covers", "[model][arrange]")
{
    using Spans = std::vector<std::pair<double, double>>;

    // Sound on one track at [1, 3) and [8, 9); on another at [2, 5).
    const Spans sounds { { 8.0, 9.0 }, { 1.0, 3.0 }, { 2.0, 5.0 } };

    REQUIRE(gapsBetween(sounds, 0.0, 12.0, 0.5) == Spans { { 0.0, 1.0 }, { 5.0, 8.0 }, { 9.0, 12.0 } });
    REQUIRE(gapsBetween(sounds, 0.0, 12.0, 2.0) == Spans { { 5.0, 8.0 }, { 9.0, 12.0 } });
    REQUIRE(gapsBetween(sounds, 4.0, 6.0, 0.1) == Spans { { 5.0, 6.0 } });
    REQUIRE(gapsBetween({}, 2.0, 4.0, 0.0) == Spans { { 2.0, 4.0 } });
}

TEST_CASE("Truncating silences takes out each pause's middle and closes up", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 10.0, 0.0);   // one clip, with silent stretches inside it
    f.add(f.other, 12.0, 2.0, 0.0);

    // Pauses at [2, 6) and [8, 13): keep 1 beat of each.
    const std::vector<std::pair<double, double>> silences { { 2.0, 6.0 }, { 8.0, 13.0 } };
    REQUIRE_THAT(truncateSilences(f.song, { f.audio, f.other }, silences, 1.0), WithinAbs(3.0 + 4.0, 1e-9));

    // The clip ran to 10, inside the second pause, so it now ends half a beat
    // into it; the first pause kept half a beat at each end.
    const auto audio = f.sorted(f.audio);
    REQUIRE(audio.size() == 2);
    requireClip(audio[0], 0.0, 2.5, 0.0);
    requireClip(audio[1], 2.5, 3.0, 5.5);

    // The other track's clip started inside the part of the second pause that
    // was taken out, so it lost that half beat, and closed up by both cuts.
    const auto other = f.sorted(f.other);
    REQUIRE(other.size() == 1);
    requireClip(other[0], 5.5, 1.5, 0.5);
}

TEST_CASE("Repeat puts copies after the selection, one after another", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 2.0, 0.0);
    f.add(f.audio, 4.0, 2.0, 5.0);           // after the selection: pushed along

    TimeSelection selection { 0.0, 2.0, { f.audio } };
    const auto    repeats = repeatRange(f.song, selection, 3);
    REQUIRE_THAT(repeats.startBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(repeats.endBeats, WithinAbs(8.0, 1e-9));

    const auto clips = f.sorted(f.audio);
    REQUIRE(clips.size() == 5);
    for (int i = 0; i < 4; ++i)
        requireClip(clips[(size_t) i], 2.0 * i, 2.0, 0.0);
    requireClip(clips[4], 10.0, 2.0, 5.0);

    REQUIRE(repeatRange(f.song, { 20.0, 20.0, { f.audio } }, 2).isEmpty());
}

TEST_CASE("Regions merge when they touch or nearly touch", "[model][arrange]")
{
    using Spans = std::vector<std::pair<double, double>>;

    const Spans regions { { 4.0, 5.0 }, { 0.0, 1.0 }, { 1.2, 2.0 }, { 2.0, 3.0 } };

    REQUIRE(mergeRegions(regions, 0.0) == Spans { { 0.0, 1.0 }, { 1.2, 3.0 }, { 4.0, 5.0 } });
    REQUIRE(mergeRegions(regions, 0.5) == Spans { { 0.0, 3.0 }, { 4.0, 5.0 } });
    REQUIRE(mergeRegions(regions, 2.0) == Spans { { 0.0, 5.0 } });
    REQUIRE(mergeRegions({ { 3.0, 3.0 } }, 1.0).empty());
}

TEST_CASE("Auto Duck writes a dip around each passage, in the clip's own time", "[model][arrange]")
{
    Fixture f;
    f.add(f.audio, 0.0, 20.0, 2.0);   // music, playing its file from 2 s in
    f.add(f.other, 0.0, 20.0, 0.0);   // the voice, not ducked itself

    // One passage of voice from beat 5 to beat 8 (seconds, at 60 bpm).
    REQUIRE(duckClips(f.song, { f.audio }, { { 5.0, 8.0 } }, 0.25f, 0.5) == 1);

    // By value: sorted() hands back a copy, and a reference into it would
    // dangle the moment the statement ended.
    const auto clip     = f.sorted(f.audio)[0];
    const auto envelope = clip.envelope;
    const auto points   = envelope.points();
    REQUIRE(points.size() == 4);

    // Beat 5 is 7 s into the file (2 s offset + 5), and the fade starts half a second before.
    REQUIRE_THAT(points[0].seconds, WithinAbs(6.5, 1e-9));
    REQUIRE(points[0].gain == 1.0f);
    REQUIRE_THAT(points[1].seconds, WithinAbs(7.0, 1e-9));
    REQUIRE(points[1].gain == 0.25f);
    REQUIRE_THAT(points[2].seconds, WithinAbs(10.0, 1e-9));
    REQUIRE(points[2].gain == 0.25f);
    REQUIRE_THAT(points[3].seconds, WithinAbs(10.5, 1e-9));
    REQUIRE(points[3].gain == 1.0f);

    // Between the passages the music is at full level, and under them it's ducked.
    REQUIRE(envelope.gainAt(6.0) == 1.0f);
    REQUIRE(envelope.gainAt(8.5) == 0.25f);
    REQUIRE(envelope.gainAt(11.0) == 1.0f);

    // The voice track keeps its own curve, and so does a clip no passage reaches.
    REQUIRE(f.sorted(f.other)[0].envelope.isEmpty());
    REQUIRE(duckClips(f.song, { f.audio }, { { 40.0, 41.0 } }, 0.25f, 0.5) == 0);
}
