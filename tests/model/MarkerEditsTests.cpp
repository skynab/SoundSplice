#include <catch2/catch_test_macros.hpp>

#include <model/Markers.h>

using namespace soundsplice::model;

TEST_CASE("Moving a marker keeps its length and the list in order", "[model][markers]")
{
    Song song;
    const int a     = addMarker(song, 2.0, 0.0, "a");
    const int range = addMarker(song, 4.0, 3.0, "range");
    const int c     = addMarker(song, 10.0, 0.0, "c");

    REQUIRE(moveMarker(song, range, 12.0));
    REQUIRE(song.markers.size() == 3);
    REQUIRE(song.markers[0].id == a);
    REQUIRE(song.markers[1].id == c);
    REQUIRE(song.markers[2].id == range);
    REQUIRE(song.markers[2].startBeats == 12.0);
    REQUIRE(song.markers[2].lengthBeats == 3.0);
    REQUIRE(song.markers[2].name == "range");

    REQUIRE(moveMarker(song, a, -5.0));
    REQUIRE(song.markers[0].startBeats == 0.0);

    REQUIRE_FALSE(moveMarker(song, 999, 1.0));
}

TEST_CASE("Between markers is from the edge before to the edge after", "[model][markers]")
{
    Song song;
    addMarker(song, 4.0, 0.0, "point");
    addMarker(song, 10.0, 4.0, "range"); // edges at 10 and 14

    using Span = std::pair<double, double>;

    REQUIRE(spanBetweenMarkers(song, 2.0, 30.0) == Span { 0.0, 4.0 });   // before the first
    REQUIRE(spanBetweenMarkers(song, 6.0, 30.0) == Span { 4.0, 10.0 });  // between two
    REQUIRE(spanBetweenMarkers(song, 12.0, 30.0) == Span { 10.0, 14.0 }); // inside the range
    REQUIRE(spanBetweenMarkers(song, 20.0, 30.0) == Span { 14.0, 30.0 }); // after the last
    REQUIRE(spanBetweenMarkers(song, 4.0, 30.0) == Span { 4.0, 10.0 });  // on a marker: the span it starts

    // Past the end of the content, the span still reaches the click.
    REQUIRE(spanBetweenMarkers(song, 40.0, 30.0) == Span { 14.0, 40.0 });

    // No markers at all: everything up to the end.
    REQUIRE(spanBetweenMarkers(Song {}, 5.0, 30.0) == Span { 0.0, 30.0 });
}
