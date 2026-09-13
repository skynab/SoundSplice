#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <model/Markers.h>
#include <model/Serialization.h>
#include <model/Timebase.h>

using namespace soundsplice::model;
using Catch::Matchers::WithinAbs;

TEST_CASE("Markers are kept in timeline order, each with its own id", "[model][markers]")
{
    Song song;
    const int late   = addMarker(song, 8.0, 0.0, "Outro");
    const int early  = addMarker(song, 2.0, 0.0, "Intro");
    const int middle = addMarker(song, 5.0, 4.0, "Verse");

    REQUIRE(song.markers.size() == 3);
    REQUIRE(song.markers[0].id == early);
    REQUIRE(song.markers[1].id == middle);
    REQUIRE(song.markers[2].id == late);
    REQUIRE(late != early);
    REQUIRE(middle != early);

    // Clamped rather than placed before the start of the song.
    const int clamped = addMarker(song, -3.0, -1.0, "Before");
    REQUIRE(findMarker(song, clamped)->startBeats == 0.0);
    REQUIRE(findMarker(song, clamped)->lengthBeats == 0.0);
    REQUIRE(song.markers.front().id == clamped);
}

TEST_CASE("Markers can be renamed and removed", "[model][markers]")
{
    Song song;
    const int id = addMarker(song, 4.0, 0.0, "Cough");

    REQUIRE(renameMarker(song, id, "Cough - cut this"));
    REQUIRE(findMarker(song, id)->name == "Cough - cut this");

    REQUIRE(removeMarker(song, id));
    REQUIRE(song.markers.empty());
    REQUIRE_FALSE(removeMarker(song, id));
    REQUIRE_FALSE(renameMarker(song, id, "gone"));
}

TEST_CASE("New markers get the lowest free default name", "[model][markers]")
{
    Song song;
    REQUIRE(nextMarkerName(song) == "Marker 1");

    const int first = addMarker(song, 0.0, 0.0, nextMarkerName(song));
    addMarker(song, 1.0, 0.0, nextMarkerName(song));
    REQUIRE(nextMarkerName(song) == "Marker 3");

    removeMarker(song, first);
    REQUIRE(nextMarkerName(song) == "Marker 1");
}

TEST_CASE("Jumping between markers moves on from the one you're at", "[model][markers]")
{
    Song song;
    addMarker(song, 4.0, 0.0, "A");
    addMarker(song, 8.0, 2.0, "B");

    REQUIRE(nextMarkerStart(song, 0.0) == 4.0);
    REQUIRE(nextMarkerStart(song, 4.0) == 8.0);
    REQUIRE_FALSE(nextMarkerStart(song, 8.0).has_value());

    REQUIRE(previousMarkerStart(song, 8.0) == 4.0);
    REQUIRE(previousMarkerStart(song, 6.0) == 4.0);
    REQUIRE_FALSE(previousMarkerStart(song, 4.0).has_value());
}

TEST_CASE("Markers export as an Audacity label file", "[model][markers]")
{
    Song song;
    song.bpm = 120.0; // half a second per beat
    addMarker(song, 4.0, 0.0, "Verse");
    addMarker(song, 8.0, 4.0, "Chorus\tone");

    REQUIRE(exportMarkersAsLabels(song) == "2.000000\t2.000000\tVerse\n"
                                           "4.000000\t6.000000\tChorus one\n");
}

TEST_CASE("Audacity labels import, and anything else in the file is skipped", "[model][markers]")
{
    const std::string text = "0.5\t0.5\tIntro\r\n"
                             "\\\t100.000000\t2000.000000\n" // a frequency-range line
                             "\n"
                             "not a label\n"
                             "1.5\t3\tPart two, with spaces\n"
                             "4\t5\n";                        // a label with no name

    const auto markers = markersFromLabels(text, 60.0); // a beat per second

    REQUIRE(markers.size() == 3);
    REQUIRE_THAT(markers[0].startBeats, WithinAbs(0.5, 1e-9));
    REQUIRE_THAT(markers[0].lengthBeats, WithinAbs(0.0, 1e-9));
    REQUIRE(markers[0].name == "Intro");

    REQUIRE_THAT(markers[1].startBeats, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(markers[1].lengthBeats, WithinAbs(1.5, 1e-9));
    REQUIRE(markers[1].name == "Part two, with spaces");

    REQUIRE(markers[2].name.empty());
}

TEST_CASE("Markers exported as labels import back the same", "[model][markers]")
{
    Song song;
    song.bpm = 97.0;
    addMarker(song, 3.25, 0.0, "Point");
    addMarker(song, 10.0, 6.5, "A range");

    const auto imported = markersFromLabels(exportMarkersAsLabels(song), song.bpm);
    REQUIRE(imported.size() == song.markers.size());

    for (size_t i = 0; i < imported.size(); ++i)
    {
        REQUIRE_THAT(imported[i].startBeats, WithinAbs(song.markers[i].startBeats, 1e-4));
        REQUIRE_THAT(imported[i].lengthBeats, WithinAbs(song.markers[i].lengthBeats, 1e-4));
        REQUIRE(imported[i].name == song.markers[i].name);
    }
}

TEST_CASE("Markers are saved with the project", "[model][markers]")
{
    Song song;
    addMarker(song, 2.0, 0.0, "Take 3 starts");
    addMarker(song, 16.5, 8.25, "Keep this bit");

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(song), restored, &error));
    REQUIRE(restored.markers == song.markers);
    REQUIRE(restored.nextId == song.nextId);

    // A name can't break the file by holding a line break.
    Song awkward;
    addMarker(awkward, 1.0, 0.0, "two\nlines");
    REQUIRE(deserialize(serialize(awkward), restored, &error));
    REQUIRE(restored.markers.size() == 1);
    REQUIRE(restored.markers[0].name == "two lines");
}

TEST_CASE("Markers keep their time when the tempo changes", "[model][markers]")
{
    Song song;
    song.bpm = 120.0;
    addMarker(song, 4.0, 2.0, "At two seconds, for one");

    retimeAudioForTempoChange(song, 120.0, 60.0);
    REQUIRE_THAT(song.markers[0].startBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(song.markers[0].lengthBeats, WithinAbs(1.0, 1e-9));
}
