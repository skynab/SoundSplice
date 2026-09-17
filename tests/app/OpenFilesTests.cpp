#include <catch2/catch_test_macros.hpp>

#include <app/OpenFiles.h>

using namespace soundsplice;
using app::OpenFiles;

TEST_CASE("Files join the list once, in the order they were opened", "[app][openfiles]")
{
    OpenFiles files;
    REQUIRE(files.isEmpty());

    files.open(7);
    files.open(3);
    files.open(7);
    files.open(9);

    REQUIRE(files.clipIds() == std::vector<int> { 7, 3, 9 });
    REQUIRE(files.contains(3));
    REQUIRE_FALSE(files.contains(4));
}

TEST_CASE("Closing a file says what to show next", "[app][openfiles]")
{
    OpenFiles files;
    for (int id : { 1, 2, 3 })
        files.open(id);

    REQUIRE(files.close(2) == 3);  // the one after it
    REQUIRE(files.close(3) == 1);  // the last one: the one before it
    REQUIRE(files.close(42) == 0); // wasn't open
    REQUIRE(files.clipIds() == std::vector<int> { 1 });
    REQUIRE(files.close(1) == 0);  // nothing left
    REQUIRE(files.isEmpty());
}

TEST_CASE("Stepping through open files wraps at both ends", "[app][openfiles]")
{
    OpenFiles files;
    REQUIRE(files.neighbour(1, 1) == 0);

    for (int id : { 10, 20, 30 })
        files.open(id);

    REQUIRE(files.neighbour(10, 1) == 20);
    REQUIRE(files.neighbour(30, 1) == 10);
    REQUIRE(files.neighbour(10, -1) == 30);
    REQUIRE(files.neighbour(99, 1) == 10); // none showing: start at the top
}

TEST_CASE("Clips that are gone, or aren't audio, drop out of the list", "[app][openfiles]")
{
    model::Song song;
    const int   trackId = model::addTrack(song, model::TrackType::Audio, "Vox").id;

    model::Clip audio;
    audio.type      = model::ClipType::Audio;
    audio.audioFile = "take.wav";
    const int kept  = model::addClip(song, trackId, audio)->id;
    const int gone  = model::addClip(song, trackId, audio)->id;

    model::Clip notes;
    const int   instrument = model::addClip(song, trackId, notes)->id;

    OpenFiles files;
    for (int id : { kept, gone, instrument })
        files.open(id);

    song.tracks[0].clips.erase(song.tracks[0].clips.begin() + 1);

    REQUIRE(files.prune(song));
    REQUIRE(files.clipIds() == std::vector<int> { kept });
    REQUIRE_FALSE(files.prune(song));

    const auto where = OpenFiles::locate(song, kept);
    REQUIRE(where.track == 0);
    REQUIRE(where.clip == 0);
    REQUIRE_FALSE(OpenFiles::locate(song, gone).isValid());
}
