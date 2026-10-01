#include <catch2/catch_test_macros.hpp>

#include <model/Folders.h>
#include <model/Serialization.h>

using namespace soundsplice::model;

namespace
{
    Song fiveTracks()
    {
        Song song;
        for (const char* name : { "Drums", "Kick", "Snare", "Bass", "Vox" })
            addTrack(song, TrackType::Audio, name);
        return song;
    }
}

TEST_CASE("Indenting puts a track in the folder above, and folders don't nest", "[model][folders]")
{
    auto song = fiveTracks();
    REQUIRE_FALSE(folderedit::indent(song, 0)); // nothing above

    REQUIRE(folderedit::indent(song, 1)); // Kick into Drums
    REQUIRE(folderedit::indent(song, 2)); // Snare into the folder Kick is in
    REQUIRE(folderedit::childIndices(song, 0) == std::vector<int> { 1, 2 });
    REQUIRE(folderedit::parentIndexOf(song, 2) == 0);
    REQUIRE(folderedit::isFolder(song, 0));
    REQUIRE_FALSE(folderedit::isFolder(song, 1));

    REQUIRE_FALSE(folderedit::indent(song, 2)); // already in one
    REQUIRE_FALSE(folderedit::indent(song, 0)); // a folder can't go in one
}

TEST_CASE("A collapsed folder hides the tracks in it", "[model][folders]")
{
    auto song = fiveTracks();
    folderedit::indent(song, 1);
    folderedit::indent(song, 2);

    REQUIRE(folderedit::visibleTrackIndices(song).size() == 5);
    song.tracks[0].folderCollapsed = true;
    REQUIRE(folderedit::visibleTrackIndices(song) == std::vector<int> { 0, 3, 4 });
    REQUIRE(folderedit::isHidden(song, 2));
    REQUIRE_FALSE(folderedit::isHidden(song, 3));
    REQUIRE(folderedit::withChildren(song, 0) == std::vector<int> { 0, 1, 2 });
}

TEST_CASE("Outdenting moves a track past the rest of its folder", "[model][folders]")
{
    auto song = fiveTracks();
    folderedit::indent(song, 1);
    folderedit::indent(song, 2);

    REQUIRE(folderedit::outdent(song, 1) == 2);
    REQUIRE(song.tracks[1].name == "Snare");
    REQUIRE(song.tracks[2].name == "Kick");
    REQUIRE(folderedit::childIndices(song, 0) == std::vector<int> { 1 });
    REQUIRE(folderedit::parentIndexOf(song, 2) == -1);
    REQUIRE(folderedit::outdent(song, 2) == -1);
}

TEST_CASE("A track cut off from its folder is simply at the top level", "[model][folders]")
{
    auto song = fiveTracks();
    folderedit::indent(song, 1);
    folderedit::indent(song, 2);

    // A track that isn't in it lands between the folder and its tracks.
    Track stray;
    stray.id   = allocateId(song);
    stray.name = "Stray";
    song.tracks.insert(song.tracks.begin() + 1, stray);

    REQUIRE(folderedit::childIndices(song, 0).empty());
    REQUIRE(folderedit::parentIndexOf(song, 2) == -1);
    REQUIRE(folderedit::parentIndexOf(song, 3) == -1);
}

TEST_CASE("Folders round-trip, and a track in none writes nothing", "[model][folders][io]")
{
    auto song = fiveTracks();
    folderedit::indent(song, 1);
    song.tracks[0].folderCollapsed = true;

    const auto text = serialize(song);
    REQUIRE(text.find("TRACKFOLDER") != std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(folderedit::isHidden(restored, 1));
}
