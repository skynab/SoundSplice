#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/TimeSelection.h>

using namespace soundsplice;
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

TEST_CASE("Tracks outside the selection are left alone", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioB, 0.0, 10.0);

    const auto before = f.song;
    removeRange(f.song, f.over(2.0, 6.0, { f.audioA }), true);

    REQUIRE(f.clips(f.audioB) == before.tracks[1].clips);
    REQUIRE(anyTrackApplies(f.song, f.over(2.0, 6.0, { f.audioA })));
    REQUIRE(anyTrackApplies(f.song, f.over(2.0, 6.0, { f.synth })));
}

namespace
{
    engine::Note note(double start, double length, int number)
    {
        engine::Note n;
        n.startBeats  = start;
        n.lengthBeats = length;
        n.noteNumber  = number;
        return n;
    }

    /** A four-beat loop: notes 60, 62, 64 and 65, one on each beat, each
        half a beat long. */
    Clip loopClip(double start, double length)
    {
        Clip clip;
        clip.type                = ClipType::Instrument;
        clip.startBeats          = start;
        clip.lengthBeats         = length;
        clip.pattern.lengthBeats = 4.0;
        clip.pattern.notes       = { note(0.0, 0.5, 60), note(1.0, 0.5, 62), note(2.0, 0.5, 64), note(3.0, 0.5, 65) };
        return clip;
    }
}

TEST_CASE("A piece of a pattern holds the notes that start in it, as the loop plays them", "[model][timeselection]")
{
    const auto clip = loopClip(0.0, 8.0);

    // Beats 3 to 6 of a looping bar: 65, then round again to 60 and 62.
    const auto piece = patternWindow(clip.pattern, 3.0, 6.0);
    REQUIRE_THAT(piece.lengthBeats, WithinAbs(3.0, 1e-9));
    REQUIRE(piece.notes.size() == 3);
    REQUIRE(piece.notes[0].noteNumber == 65);
    REQUIRE_THAT(piece.notes[0].startBeats, WithinAbs(0.0, 1e-9));
    REQUIRE(piece.notes[1].noteNumber == 60);
    REQUIRE_THAT(piece.notes[1].startBeats, WithinAbs(1.0, 1e-9));
    REQUIRE(piece.notes[2].noteNumber == 62);
    REQUIRE_THAT(piece.notes[2].startBeats, WithinAbs(2.0, 1e-9));

    // A note running past the piece's end is cut short there; one already
    // sounding at its start is left out.
    const auto tight = patternWindow(clip.pattern, 1.25, 2.25);
    REQUIRE(tight.notes.size() == 1);
    REQUIRE(tight.notes[0].noteNumber == 64);
    REQUIRE_THAT(tight.notes[0].startBeats, WithinAbs(0.75, 1e-9));
    REQUIRE_THAT(tight.notes[0].lengthBeats, WithinAbs(0.25, 1e-9));
}

TEST_CASE("A held pedal stays held in a piece that starts while it's down", "[model][timeselection]")
{
    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.pedals      = { { 1.0, true }, { 3.0, false } };

    const auto during = patternWindow(pattern, 2.0, 5.0);
    REQUIRE(during.pedals.size() == 2);
    REQUIRE(during.pedals[0] == engine::PedalEvent { 0.0, true });
    REQUIRE(during.pedals[1] == engine::PedalEvent { 1.0, false });

    REQUIRE(patternWindow(pattern, 0.0, 0.5).pedals.empty()); // before it goes down
}

TEST_CASE("Delete on an instrument track takes the notes out and closes the gap", "[model][timeselection]")
{
    Fixture f;
    addClip(f.song, f.synth, loopClip(0.0, 8.0));
    f.add(f.audioA, 0.0, 8.0);

    removeRange(f.song, f.over(2.0, 4.0, { f.audioA, f.synth }), true);

    const auto& clips = f.clips(f.synth);
    REQUIRE(clips.size() == 2);
    REQUIRE(clips[0].id != clips[1].id);

    // The first two beats, then the second bar moved back to beat 2.
    REQUIRE_THAT(clips[0].lengthBeats, WithinAbs(2.0, 1e-9));
    REQUIRE(clips[0].pattern.notes.size() == 2);
    REQUIRE_THAT(clips[1].startBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(clips[1].lengthBeats, WithinAbs(4.0, 1e-9));
    REQUIRE(clips[1].pattern.notes.size() == 4);
    REQUIRE(clips[1].pattern.notes[0].noteNumber == 60);

    // The audio track in the same selection was edited alongside.
    REQUIRE(f.clips(f.audioA).size() == 2);
}

TEST_CASE("A whole instrument clip inside the selection is copied as it is", "[model][timeselection]")
{
    Fixture f;
    const auto clip = loopClip(2.0, 8.0);
    addClip(f.song, f.synth, clip);

    const auto clipboard = copyRange(f.song, f.over(0.0, 12.0, { f.synth }));
    REQUIRE(clipboard.tracks.size() == 1);
    REQUIRE(clipboard.tracks[0].size() == 1);
    REQUIRE(clipboard.tracks[0][0].pattern == clip.pattern); // still the four-beat loop
}

TEST_CASE("Paste puts clips only on tracks of their own kind", "[model][timeselection]")
{
    Fixture f;
    f.add(f.audioA, 0.0, 4.0);
    addClip(f.song, f.synth, loopClip(0.0, 4.0));

    const auto audio = copyRange(f.song, f.over(0.0, 2.0, { f.audioA }));
    const auto midi  = copyRange(f.song, f.over(0.0, 2.0, { f.synth }));

    // Audio onto the instrument track alone: nowhere for it to go.
    const auto before = f.song;
    REQUIRE_FALSE(insertClipboard(f.song, { f.synth }, audio, 0.0));
    REQUIRE(f.song.tracks == before.tracks);

    REQUIRE(insertClipboard(f.song, { f.synth }, midi, 0.0));
    const auto& clips   = f.clips(f.synth);
    const auto  atStart = std::count_if(clips.begin(), clips.end(), [](const Clip& c)
    {
        return c.type == ClipType::Instrument && std::abs(c.startBeats) < 1e-9;
    });
    REQUIRE(atStart == 1);
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
