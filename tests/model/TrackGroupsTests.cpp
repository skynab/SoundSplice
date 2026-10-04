#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/Serialization.h>
#include <model/TrackGroups.h>

using Catch::Approx;
using namespace soundsplice::model;

namespace
{
    /** Kick, snare and overheads in group 1, a bass in none, a vocal in
        group 2; each with one clip over beats 0-8. */
    Song drumSong()
    {
        Song song;
        song.bpm = 120.0;
        const std::pair<const char*, int> tracks[] = { { "Kick", 1 }, { "Bass", 0 }, { "Snare", 1 }, { "Vox", 2 }, { "OH", 1 } };
        for (const auto& [name, group] : tracks)
        {
            auto& track     = addTrack(song, TrackType::Audio, name);
            track.editGroup = group;
            Clip clip;
            clip.audioFile   = std::string(name) + ".wav";
            clip.lengthBeats = 8.0;
            addClip(song, track.id, clip);
        }
        return song;
    }

    int idOf(const Song& song, int index) { return song.tracks[(size_t) index].id; }
}

TEST_CASE("A track's group members are the tracks sharing its number", "[model][groups]")
{
    const auto song = drumSong();
    REQUIRE(groupedit::memberIndices(song, 2) == std::vector<int> { 2, 0, 4 });
    REQUIRE(groupedit::memberIndices(song, 1) == std::vector<int> { 1 }); // in none
    REQUIRE(groupedit::memberIndices(song, 3) == std::vector<int> { 3 }); // alone in its group
    REQUIRE(groupedit::memberIndices(song, 9).empty());
}

TEST_CASE("A selection on one grouped track takes in the whole group", "[model][groups]")
{
    const auto song = drumSong();
    REQUIRE(groupedit::withGroupMembers(song, { idOf(song, 4) })
            == std::vector<int> { idOf(song, 0), idOf(song, 2), idOf(song, 4) });
    REQUIRE(groupedit::withGroupMembers(song, { idOf(song, 1), idOf(song, 3) })
            == std::vector<int> { idOf(song, 1), idOf(song, 3) });

    const auto areas = groupedit::withGroupAreas(song, { { idOf(song, 0), 2.0, 4.0 } });
    REQUIRE(areas.size() == 3);
    REQUIRE(areas[1] == RazorArea { idOf(song, 2), 2.0, 4.0 });
}

TEST_CASE("Grouped clips lined up with a clip move with it; others don't", "[model][groups]")
{
    auto song = drumSong();
    song.tracks[4].clips[0].lengthBeats = 6.0; // the overheads' clip ends elsewhere

    const auto aligned = groupedit::alignedClips(song, 0, 0);
    REQUIRE(aligned == std::vector<std::pair<int, int>> { { 2, 0 } });
    REQUIRE(groupedit::alignedClips(song, 1, 0).empty());
}

TEST_CASE("A grouped fader moves the others by as much, within their range", "[model][groups]")
{
    auto song = drumSong();
    song.tracks[2].gainDb = 4.0f;
    song.tracks[4].gainDb = -10.0f;

    const auto get = [](const Track& t) { return t.gainDb; };
    const auto set = [](Track& t, float v) { t.gainDb = v; };
    const auto before = song;
    groupedit::setRelative(song, song, 0, 3.0f, -60.0f, 6.0f, get, set);

    REQUIRE(song.tracks[0].gainDb == 3.0f);
    REQUIRE(song.tracks[2].gainDb == 6.0f); // 7 kept to the top
    REQUIRE(song.tracks[4].gainDb == -7.0f);
    REQUIRE(song.tracks[1].gainDb == 0.0f); // not in the group

    // Within a drag, measured from where it began: back down, the clamped
    // one returns to where it was.
    groupedit::setRelative(song, before, 0, 0.0f, -60.0f, 6.0f, get, set);
    REQUIRE(song.tracks[2].gainDb == 4.0f);
    REQUIRE(song.tracks[4].gainDb == -10.0f);
}

TEST_CASE("A track's edit group round-trips, and none writes nothing", "[model][groups][io]")
{
    const auto song = drumSong();
    const auto text = serialize(song);
    REQUIRE(text.find("TRACKGROUP 1") != std::string::npos);
    REQUIRE(text.find("TRACKGROUP 0") == std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
}
