#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <app/ExportPlan.h>

using namespace soundsplice;
using namespace soundsplice::app::exportplan;
using Catch::Approx;

namespace
{
    model::Marker marker(double start, double length, std::string name)
    {
        return { 0, start, length, std::move(name) };
    }
}

TEST_CASE("A per-range export writes each marker range, in time order", "[app][export]")
{
    // Out of order, and a point marker among them, which isn't a range.
    const std::vector<model::Marker> markers { marker(16.0, 8.0, "Chorus"), marker(4.0, 0.0, "Cue"),
                                               marker(0.0, 8.0, "Verse") };
    const auto out = regions(markers, false, 40.0);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].name == "Verse");
    REQUIRE(out[1].name == "Chorus");
    REQUIRE(out[1].startBeats == 16.0);
    REQUIRE(out[1].lengthBeats == 8.0);
}

TEST_CASE("Splitting at markers runs from each to the next, from the start to the end", "[app][export]")
{
    const std::vector<model::Marker> markers { marker(20.0, 0.0, "Outro"), marker(8.0, 4.0, "Verse") };
    const auto out = regions(markers, true, 30.0);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].name == "Start");
    REQUIRE(out[0].startBeats == 0.0);
    REQUIRE(out[0].lengthBeats == 8.0);
    REQUIRE(out[1].name == "Verse"); // to the next marker, not its own length
    REQUIRE(out[1].lengthBeats == 12.0);
    REQUIRE(out[2].name == "Outro");
    REQUIRE(out[2].lengthBeats == 10.0);

    // A marker at the very start leaves no empty "Start" before it, and one
    // at the end no empty stretch after it.
    const auto edges = regions({ marker(0.0, 0.0, "Top"), marker(30.0, 0.0, "End") }, true, 30.0);
    REQUIRE(edges.size() == 1);
    REQUIRE(edges[0].name == "Top");
    REQUIRE(edges[0].lengthBeats == 30.0);

    // No markers: the whole song.
    const auto none = regions({}, true, 30.0);
    REQUIRE(none.size() == 1);
    REQUIRE(none[0].name == "Start");
}

TEST_CASE("A file's chapters are the markers inside it, timed from its start", "[app][export]")
{
    model::Song song; // 120 bpm: half a second a beat
    song.markers = { marker(12.0, 0.0, "Outro"), marker(4.0, 0.0, "Verse"), marker(0.0, 0.0, "Intro"),
                     marker(40.0, 0.0, "Past the end") };

    const auto whole = chapters(song, 0.0, 16.0);
    REQUIRE(whole.size() == 3);
    REQUIRE(whole[0].title == "Intro");
    REQUIRE(whole[0].startSeconds == Approx(0.0));
    REQUIRE(whole[0].endSeconds == Approx(2.0)); // to the next
    REQUIRE(whole[1].endSeconds == Approx(6.0));
    REQUIRE(whole[2].title == "Outro");
    REQUIRE(whole[2].endSeconds == Approx(8.0)); // the last, to the end

    // A file from beat 4: what's before it is left out, and times start at 0.
    const auto part = chapters(song, 4.0, 16.0);
    REQUIRE(part.size() == 2);
    REQUIRE(part[0].title == "Verse");
    REQUIRE(part[0].startSeconds == Approx(0.0));
    REQUIRE(part[1].startSeconds == Approx(4.0));

    REQUIRE(chapters(song, 13.0, 16.0).empty());
}

TEST_CASE("A render report lists the clips sounding in its stretch, on its track for a stem", "[app][export]")
{
    model::Song song;
    auto&       vox = model::addTrack(song, model::TrackType::Audio, "Vox");
    model::Clip take;
    take.audioFile   = "/takes/vox.wav";
    take.startBeats  = 8.0;
    take.lengthBeats = 4.0;
    vox.clips.push_back(take);
    take.startBeats = 20.0; // after the stretch
    vox.clips.push_back(take);

    auto&       synth = model::addTrack(song, model::TrackType::Audio, "Synth");
    model::Clip part;
    part.audioFile   = "/takes/synth.wav";
    part.startBeats  = 2.0;
    part.lengthBeats = 4.0;
    synth.clips.push_back(part);

    const auto mix = reportClips(song, -1, 4.0, 16.0);
    REQUIRE(mix.size() == 2);
    REQUIRE(mix[0].track == "Synth"); // in the order they start
    REQUIRE(mix[0].audioFile == "/takes/synth.wav");
    REQUIRE(mix[0].startSeconds == Approx(-1.0)); // began before the stretch
    REQUIRE(mix[1].track == "Vox");
    REQUIRE(mix[1].audioFile == "/takes/vox.wav");
    REQUIRE(mix[1].startSeconds == Approx(2.0));
    REQUIRE(mix[1].lengthSeconds == Approx(2.0));

    const auto stem = reportClips(song, 0, 4.0, 16.0);
    REQUIRE(stem.size() == 1);
    REQUIRE(stem[0].track == "Vox");
}
