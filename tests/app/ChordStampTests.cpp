#include <catch2/catch_test_macros.hpp>

#include <app/ChordStamp.h>

using namespace looper;

namespace
{
    /** A song with a guitar track holding one four-beat clip — what
        addGuitarTrack builds. */
    model::Song guitarSong(double clipLengthBeats = 4.0, double clipStartBeats = 0.0)
    {
        model::Song song;
        const int id = model::addTrack(song, model::TrackType::Guitar, "Riff").id;

        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = clipStartBeats;
        clip.lengthBeats         = clipLengthBeats;
        clip.pattern.lengthBeats = clipLengthBeats;
        model::addClip(song, id, clip);
        return song;
    }

    const engine::ChordShape& openE()
    {
        for (const auto& shape : engine::kChordShapes)
            if (std::string(shape.name) == "E")
                return shape;
        return engine::kChordShapes[0];
    }

    engine::StrumSettings plainStrum()
    {
        engine::StrumSettings strum;
        strum.spreadMs = 18.0;
        strum.humanise = 0.0;
        return strum;
    }
}

TEST_CASE("Stamping a chord onto a guitar clip produces notes", "[app][chord]")
{
    // The path the reported bug was about. Untestable until this decision was
    // lifted out of MainComponent, which no test can construct.
    const auto plan = planChordStamp(guitarSong(), 0, 0, openE(), 0, plainStrum(),
                                     0.0, 4.0, 1u);

    REQUIRE(plan.ok);
    REQUIRE(plan.problem.empty());
    REQUIRE(plan.notes.size() == 6); // an open E is six strings
    REQUIRE(plan.atBeats == 0.0);
}

TEST_CASE("Every refusal says why", "[app][chord]")
{
    // Silent refusals are what made the original report hard to place: a
    // button that declines without saying so is indistinguishable from one
    // that is broken.
    const auto strum = plainStrum();

    SECTION("no track selected")
    {
        model::Song empty;
        const auto plan = planChordStamp(empty, 0, 0, openE(), 0, strum, 0.0, 4.0, 1u);
        REQUIRE_FALSE(plan.ok);
        REQUIRE_FALSE(plan.problem.empty());
        REQUIRE(plan.notes.empty());
    }

    SECTION("the selected track isn't a guitar")
    {
        model::Song song;
        const int id = model::addTrack(song, model::TrackType::Instrument, "Keys").id;
        model::addClip(song, id, model::Clip {});

        const auto plan = planChordStamp(song, 0, 0, openE(), 0, strum, 0.0, 4.0, 1u);
        REQUIRE_FALSE(plan.ok);
        REQUIRE(plan.problem.find("guitar") != std::string::npos);
        REQUIRE(plan.problem.find("Keys") != std::string::npos); // names the track
    }

    SECTION("the guitar track has no clip selected")
    {
        model::Song song;
        model::addTrack(song, model::TrackType::Guitar, "Riff"); // no clips at all

        const auto plan = planChordStamp(song, 0, 0, openE(), 0, strum, 0.0, 4.0, 1u);
        REQUIRE_FALSE(plan.ok);
        REQUIRE(plan.problem.find("clip") != std::string::npos);
    }

    SECTION("a clip index past the end")
    {
        const auto plan = planChordStamp(guitarSong(), 0, 5, openE(), 0, strum, 0.0, 4.0, 1u);
        REQUIRE_FALSE(plan.ok);
        REQUIRE(plan.problem.find("clip") != std::string::npos);
    }
}

TEST_CASE("A chord lands on the bar the playhead is in", "[app][chord]")
{
    // Stamping while stopped puts the chord where the transport is, rather
    // than always at the start.
    const auto strum = plainStrum();
    const auto song  = guitarSong(16.0);

    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 0.0,  4.0, 1u).atBeats == 0.0);
    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 5.5,  4.0, 1u).atBeats == 4.0);
    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 11.9, 4.0, 1u).atBeats == 8.0);
}

TEST_CASE("The playhead is measured from the clip, not the song", "[app][chord]")
{
    // A clip starting at bar 3 and a playhead at bar 3 is the clip's *first*
    // bar. Measuring from the song's origin would put the chord three bars
    // into a clip that has only just started.
    const auto strum = plainStrum();
    const auto song  = guitarSong(16.0, 8.0); // clip starts at beat 8

    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 8.0,  4.0, 1u).atBeats == 0.0);
    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 13.0, 4.0, 1u).atBeats == 4.0);
}

TEST_CASE("The playhead wraps into the pattern, because a clip loops", "[app][chord]")
{
    // The engine wraps playback within the pattern length, so a playhead past
    // the end is somewhere inside the repeat — not off the end of the clip.
    const auto strum = plainStrum();
    const auto song  = guitarSong(8.0);

    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 9.0,  4.0, 1u).atBeats == 0.0);
    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 13.0, 4.0, 1u).atBeats == 4.0);
}

TEST_CASE("The time signature decides where a bar starts", "[app][chord]")
{
    const auto strum = plainStrum();
    const auto song  = guitarSong(12.0);

    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 4.0, 3.0, 1u).atBeats == 3.0);
    REQUIRE(planChordStamp(song, 0, 0, openE(), 0, strum, 4.0, 4.0, 1u).atBeats == 4.0);
}

TEST_CASE("Notes are placed where they land, not at the bar's origin", "[app][chord]")
{
    // The caller adds them straight to the pattern, so the offset has to be
    // applied here rather than being left for it to remember.
    const auto plan = planChordStamp(guitarSong(16.0), 0, 0, openE(), 0, plainStrum(),
                                     5.0, 4.0, 1u);

    REQUIRE(plan.ok);
    REQUIRE(plan.atBeats == 4.0);
    for (const auto& note : plan.notes)
        REQUIRE(note.startBeats >= 4.0);
}

TEST_CASE("The first note of a chord always fits", "[app][chord]")
{
    // Worth pinning because it is why there is no "the chord fell off the end"
    // case: atBeats comes from wrapping the playhead into the pattern, so it
    // is always less than the pattern's length. Even a pattern far shorter
    // than the strum keeps its opening note.
    model::Song song = guitarSong(4.0);
    song.tracks[0].clips[0].pattern.lengthBeats = 0.01;

    const auto plan = planChordStamp(song, 0, 0, openE(), 0, plainStrum(), 0.0, 4.0, 1u);

    REQUIRE(plan.ok);
    REQUIRE_FALSE(plan.notes.empty());
    for (const auto& note : plan.notes)
        REQUIRE(note.startBeats < 0.01); // the rest of the strum was dropped
}

TEST_CASE("A shape with no strings to play is refused, not silently empty", "[app][chord]")
{
    // The only way the note list comes back empty. The built-in shapes never
    // do this; a hand-made one could, and it should say so rather than
    // appearing to work.
    const engine::ChordShape silent { "Muted", { -1, -1, -1, -1, -1, -1 } };

    const auto plan = planChordStamp(guitarSong(), 0, 0, silent, 0, plainStrum(),
                                     0.0, 4.0, 1u);

    REQUIRE_FALSE(plan.ok);
    REQUIRE(plan.notes.empty());
    REQUIRE(plan.problem.find("no strings") != std::string::npos);
}

TEST_CASE("A chord uses the track's own tuning", "[app][chord]")
{
    // Retuning the guitar has to change the chord, or the pane's tuning
    // controls would be decoration.
    auto song = guitarSong();
    const auto standard = planChordStamp(song, 0, 0, openE(), 0, plainStrum(), 0.0, 4.0, 1u);

    song.tracks[0].guitarSettings.tuning = { 38, 45, 50, 55, 59, 64 }; // drop D
    const auto dropped = planChordStamp(song, 0, 0, openE(), 0, plainStrum(), 0.0, 4.0, 1u);

    REQUIRE(standard.ok);
    REQUIRE(dropped.ok);
    REQUIRE(dropped.notes.front().noteNumber == standard.notes.front().noteNumber - 2);
}
