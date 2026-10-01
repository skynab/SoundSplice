#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/RazorEdits.h>

using Catch::Approx;
using namespace soundsplice::model;

namespace
{
    /** Three audio tracks at 120 bpm, each one clip over beats 0-16 of its
        own file, and an instrument track. */
    Song threeTracks()
    {
        Song song;
        song.bpm = 120.0;
        for (const char* name : { "a", "b", "c" })
        {
            const int id = addTrack(song, TrackType::Audio, name).id;
            Clip      clip;
            clip.type        = ClipType::Audio;
            clip.audioFile   = std::string(name) + ".wav";
            clip.lengthBeats = 16.0;
            addClip(song, id, clip);
        }
        addTrack(song, TrackType::Instrument, "keys");
        return song;
    }

    int trackId(const Song& song, int index) { return song.tracks[(size_t) index].id; }
}

TEST_CASE("Razor areas on a track merge where they overlap or touch", "[model][razor]")
{
    const auto song = threeTracks();
    const int  a = trackId(song, 0), b = trackId(song, 1);

    RazorAreas areas;
    areas = razoredit::addArea(song, areas, { b, 4.0, 6.0 });
    areas = razoredit::addArea(song, areas, { a, 2.0, 3.0 });
    areas = razoredit::addArea(song, areas, { a, 8.0, 9.0 });
    REQUIRE(areas.size() == 3);
    REQUIRE(areas[0] == RazorArea { a, 2.0, 3.0 }); // track, then time order
    REQUIRE(areas[1] == RazorArea { a, 8.0, 9.0 });
    REQUIRE(areas[2] == RazorArea { b, 4.0, 6.0 });

    areas = razoredit::addArea(song, areas, { a, 3.0, 8.5 }); // touches one, overlaps the other
    REQUIRE(areas.size() == 2);
    REQUIRE(areas[0] == RazorArea { a, 2.0, 9.0 });

    REQUIRE(razoredit::addArea(song, areas, { a, 5.0, 5.0 }) == areas); // no length, no area
    REQUIRE(razoredit::areaAt(areas, b, 5.0) != nullptr);
    REQUIRE(razoredit::areaAt(areas, b, 6.0) == nullptr);
}

TEST_CASE("A razor drag makes one area per lane it crosses", "[model][razor]")
{
    const auto song  = threeTracks();
    const auto areas = razoredit::areasFromDrag(song, 6.0, 2.0, 2, 0);
    REQUIRE(areas.size() == 3);
    for (int i = 0; i < 3; ++i)
        REQUIRE(areas[(size_t) i] == RazorArea { trackId(song, i), 2.0, 6.0 });
}

TEST_CASE("Deleting razor areas cuts each its own stretch, closing no gap", "[model][razor]")
{
    auto      song = threeTracks();
    const int a = trackId(song, 0), b = trackId(song, 1);

    razoredit::removeAreas(song, { { a, 2.0, 4.0 }, { b, 8.0, 12.0 } });

    const auto& clipsA = song.tracks[0].clips;
    REQUIRE(clipsA.size() == 2);
    REQUIRE(clipsA[0].lengthBeats == Approx(2.0));
    REQUIRE(clipsA[1].startBeats == Approx(4.0));        // not pulled back
    REQUIRE(clipsA[1].sourceOffsetSeconds == Approx(2.0)); // beat 4 at half a second a beat

    const auto& clipsB = song.tracks[1].clips;
    REQUIRE(clipsB.size() == 2);
    REQUIRE(clipsB[1].startBeats == Approx(12.0));

    REQUIRE(song.tracks[2].clips.size() == 1); // untouched
}

TEST_CASE("Copied razor areas paste in the same shape elsewhere", "[model][razor]")
{
    auto      song = threeTracks();
    const int a = trackId(song, 0), b = trackId(song, 1);

    const auto clipboard = razoredit::copyAreas(song, { { a, 2.0, 4.0 }, { b, 3.0, 4.0 } });
    REQUIRE(clipboard.entries.size() == 2);
    REQUIRE(clipboard.entries[1].trackOffset == 1);
    REQUIRE(clipboard.entries[1].startOffset == Approx(1.0));

    // Onto tracks b and c, from beat 20.
    const auto landed = razoredit::pasteAreas(song, clipboard, 1, 20.0);
    REQUIRE(landed.size() == 2);
    REQUIRE(landed[0] == RazorArea { b, 20.0, 22.0 });
    REQUIRE(landed[1] == RazorArea { trackId(song, 2), 21.0, 22.0 });

    const auto& pasted = song.tracks[1].clips.back();
    REQUIRE(pasted.audioFile == "a.wav");
    REQUIRE(pasted.startBeats == Approx(20.0));
    REQUIRE(pasted.sourceOffsetSeconds == Approx(1.0));
    REQUIRE(song.tracks[2].clips.back().audioFile == "b.wav");

    // Onto the last audio track, the second area would land on the
    // instrument track, which can't take audio: it's left out.
    REQUIRE(razoredit::pasteAreas(song, clipboard, 2, 30.0).size() == 1);
}

TEST_CASE("Moving razor areas takes their audio with them, and keeps to lanes that fit", "[model][razor]")
{
    auto      song = threeTracks();
    const int a = trackId(song, 0), b = trackId(song, 1), c = trackId(song, 2);

    auto moved = razoredit::moveAreas(song, { { a, 0.0, 2.0 } }, 18.0, 1);
    REQUIRE(moved == RazorAreas { { b, 18.0, 20.0 } });
    REQUIRE(song.tracks[0].clips.size() == 1);
    REQUIRE(song.tracks[0].clips[0].startBeats == Approx(2.0)); // what's left of a
    REQUIRE(song.tracks[1].clips.back().audioFile == "a.wav");

    // Two lanes down from c is past the audio tracks: it stays on c.
    moved = razoredit::moveAreas(song, { { c, 4.0, 6.0 } }, 1.0, 2);
    REQUIRE(moved == RazorAreas { { c, 5.0, 7.0 } });
}
