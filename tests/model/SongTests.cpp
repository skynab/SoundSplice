#include <catch2/catch_test_macros.hpp>

#include <model/History.h>
#include <model/Song.h>

using namespace looper::model;

TEST_CASE("addTrack assigns increasing ids and appends", "[model][song]")
{
    Song s;
    // Capture ids by value — addTrack returns a reference into the vector, which
    // a subsequent addTrack may invalidate by reallocating.
    const int aId = addTrack(s, TrackType::Instrument, "Synth").id;
    const int bId = addTrack(s, TrackType::Audio, "Vocals").id;

    REQUIRE(s.tracks.size() == 2);
    REQUIRE(aId == 1);
    REQUIRE(bId == 2);
    REQUIRE(s.tracks[0].name == "Synth");
    REQUIRE(s.tracks[1].type == TrackType::Audio);
}

TEST_CASE("findTrack and removeTrack behave", "[model][song]")
{
    Song s;
    const int id = addTrack(s, TrackType::Instrument, "A").id;

    REQUIRE(findTrack(s, id) != nullptr);
    REQUIRE(findTrack(s, 999) == nullptr);
    REQUIRE(removeTrack(s, id));
    REQUIRE(s.tracks.empty());
    REQUIRE_FALSE(removeTrack(s, id));
}

TEST_CASE("addClip appends to a track with a fresh id", "[model][song]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "A").id;

    Clip clip;
    clip.lengthBeats = 8.0;
    Clip* added = addClip(s, trackId, clip);

    REQUIRE(added != nullptr);
    REQUIRE(added->id != 0);
    REQUIRE(findTrack(s, trackId)->clips.size() == 1);
    REQUIRE(addClip(s, 999, clip) == nullptr); // missing track
}

TEST_CASE("Song edits are undoable through History", "[model][song][history]")
{
    Song initial;
    addTrack(initial, TrackType::Instrument, "Synth");

    History<Song> h(initial);
    h.edit("Add track", [](Song& s) { addTrack(s, TrackType::Audio, "Drums"); });
    REQUIRE(h.current().tracks.size() == 2);

    h.undo();
    REQUIRE(h.current().tracks.size() == 1);
    REQUIRE(h.current() == initial); // exact restore

    h.redo();
    REQUIRE(h.current().tracks.size() == 2);
}

TEST_CASE("A clip can be removed from a track", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    addClip(song, id, Clip {});
    addClip(song, id, Clip {});
    const int secondClipId = findTrack(song, id)->clips[1].id;

    REQUIRE(removeClip(song, id, 0));
    REQUIRE(findTrack(song, id)->clips.size() == 1);
    REQUIRE(findTrack(song, id)->clips[0].id == secondClipId); // the right one survived
}

TEST_CASE("Removing a clip refuses coordinates it doesn't have", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    addClip(song, id, Clip {});

    REQUIRE_FALSE(removeClip(song, id, 1));    // past the end
    REQUIRE_FALSE(removeClip(song, id, -1));
    REQUIRE_FALSE(removeClip(song, 9999, 0));  // no such track
    REQUIRE(findTrack(song, id)->clips.size() == 1);
}

TEST_CASE("A removed clip's id is never handed out again", "[model][song]")
{
    // Ids come from a counter that only counts up. If removal freed ids for
    // reuse, a clip could inherit a stale reference to a deleted one.
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    const int firstId = addClip(song, id, Clip {})->id;

    REQUIRE(removeClip(song, id, 0));
    REQUIRE(addClip(song, id, Clip {})->id != firstId);
}

TEST_CASE("Removing a track takes its session column with it", "[model][song]")
{
    Song song;
    const int keep = addTrack(song, TrackType::Instrument, "Keep").id;
    const int drop = addTrack(song, TrackType::Instrument, "Drop").id;
    addScene(song, "A");
    addScene(song, "B");
    setSessionClip(song, 1, 0, Clip {});

    REQUIRE(removeTrack(song, drop));
    REQUIRE(song.tracks.size() == 1);
    REQUIRE(song.tracks[0].id == keep);
    REQUIRE(song.tracks[0].sessionSlots.size() == 2); // the grid is still two rows deep
}

TEST_CASE("Removing a scene keeps the session grid rectangular", "[model][song]")
{
    // Every session lookup indexes a track's slots by scene index, so a grid
    // that loses a row from the scene list but not from the tracks would read
    // the wrong cell from then on.
    Song song;
    addTrack(song, TrackType::Instrument, "One");
    addTrack(song, TrackType::Instrument, "Two");
    addScene(song, "A");
    addScene(song, "B");
    addScene(song, "C");

    Clip marker;
    marker.lengthBeats = 7.0;
    setSessionClip(song, 0, 2, marker); // a clip in scene C

    REQUIRE(removeScene(song, 0)); // drop scene A

    REQUIRE(song.scenes.size() == 2);
    for (const auto& track : song.tracks)
        REQUIRE(track.sessionSlots.size() == 2);

    // C was the third row and is now the second; the clip must have moved
    // with it rather than staying at an index that no longer means C.
    const Clip* moved = sessionClip(song, 0, 1);
    REQUIRE(moved != nullptr);
    REQUIRE(moved->lengthBeats == 7.0);
}

TEST_CASE("Removing a scene refuses an index it doesn't have", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "One");
    addScene(song, "A");

    REQUIRE_FALSE(removeScene(song, 1));
    REQUIRE_FALSE(removeScene(song, -1));
    REQUIRE(song.scenes.size() == 1);
}

TEST_CASE("A track can be renamed", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth 1").id;

    REQUIRE(renameTrack(song, id, "Lead"));
    REQUIRE(findTrack(song, id)->name == "Lead");
    REQUIRE_FALSE(renameTrack(song, 9999, "Nope"));
}

TEST_CASE("A duplicated track is a separate track", "[model][song]")
{
    Song song;
    const int original = addTrack(song, TrackType::Guitar, "Riff").id;
    addClip(song, original, Clip {});

    Track* copy = duplicateTrack(song, 0);
    REQUIRE(copy != nullptr);
    REQUIRE(song.tracks.size() == 2);
    REQUIRE(copy->id != original);
    REQUIRE(copy->type == TrackType::Guitar); // it's still a guitar track
}

TEST_CASE("A duplicated track's clips get their own ids", "[model][song]")
{
    // Ids are how the rest of the app addresses clips. Sharing them would
    // make an edit to one resolve to the other at random.
    Song song;
    const int trackId = addTrack(song, TrackType::Instrument, "Keys").id;
    addClip(song, trackId, Clip {});
    addClip(song, trackId, Clip {});

    const auto originalIds = std::vector<int> { song.tracks[0].clips[0].id,
                                                song.tracks[0].clips[1].id };

    Track* copy = duplicateTrack(song, 0);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->clips.size() == 2);

    for (const auto& clip : copy->clips)
        for (int id : originalIds)
            REQUIRE(clip.id != id);

    // ...and not the same as each other either.
    REQUIRE(copy->clips[0].id != copy->clips[1].id);
}

TEST_CASE("A duplicated track's session clips get their own ids", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "Keys");
    addScene(song, "A");
    setSessionClip(song, 0, 0, Clip {});

    const int originalSlotId = song.tracks[0].sessionSlots[0].clip.id;

    Track* copy = duplicateTrack(song, 0);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->sessionSlots.size() == 1);
    REQUIRE(copy->sessionSlots[0].hasClip);
    REQUIRE(copy->sessionSlots[0].clip.id != originalSlotId);
}

TEST_CASE("A duplicate keeps the music and lands next to the original", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "One");
    const int second = addTrack(song, TrackType::Instrument, "Two").id;
    addTrack(song, TrackType::Instrument, "Three");

    Clip clip;
    clip.startBeats  = 8.0;
    clip.lengthBeats = 4.0;
    clip.pattern.notes.push_back({ 1.0, 0.5, 64, 0.9f });
    addClip(song, second, clip);

    song.tracks[1].gainDb = -6.0f;
    song.tracks[1].colour = 0xff36618e;

    Track* copy = duplicateTrack(song, 1);
    REQUIRE(copy != nullptr);

    // Directly after the original, not at the end.
    REQUIRE(song.tracks.size() == 4);
    REQUIRE(song.tracks[1].id == second);
    REQUIRE(song.tracks[2].id == copy->id);

    REQUIRE(song.tracks[2].gainDb == -6.0f);
    REQUIRE(song.tracks[2].colour == 0xff36618e);
    REQUIRE(song.tracks[2].clips.size() == 1);
    REQUIRE(song.tracks[2].clips[0].startBeats == 8.0);
    REQUIRE(song.tracks[2].clips[0].pattern.notes.size() == 1);
    REQUIRE(song.tracks[2].clips[0].pattern.notes[0].noteNumber == 64);
}

TEST_CASE("A duplicate is named so it can be told apart", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "Bass");
    REQUIRE(duplicateTrack(song, 0)->name == "Bass copy");
}

TEST_CASE("Duplicating a track that isn't there does nothing", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "One");

    REQUIRE(duplicateTrack(song, -1) == nullptr);
    REQUIRE(duplicateTrack(song, 5) == nullptr);
    REQUIRE(song.tracks.size() == 1);
}

TEST_CASE("Pasting the same track twice gives two separate tracks", "[model][song]")
{
    // A paste buffer is used more than once. If the ids came from the buffer
    // rather than being reissued, the second paste would produce a track the
    // app couldn't tell from the first.
    Song song;
    const int sourceId = addTrack(song, TrackType::Instrument, "Pad").id;
    addClip(song, sourceId, Clip {});

    const Track buffer = song.tracks[0]; // as a clipboard would hold it

    Track& first  = appendTrackCopy(song, buffer);
    const int firstId = first.id;
    const int firstClipId = first.clips[0].id;

    Track& second = appendTrackCopy(song, buffer);

    REQUIRE(song.tracks.size() == 3);
    REQUIRE(second.id != firstId);
    REQUIRE(second.id != sourceId);
    REQUIRE(second.clips[0].id != firstClipId);
    REQUIRE(second.clips[0].id != song.tracks[0].clips[0].id);
}

TEST_CASE("An appended copy keeps its music and its name", "[model][song]")
{
    // Unlike duplicating, pasting doesn't rename: the buffer already carries
    // whatever the user called it.
    Song song;
    const int id = addTrack(song, TrackType::Guitar, "Riff").id;
    Clip clip;
    clip.pattern.notes.push_back({ 0.0, 1.0, 55, 0.8f });
    addClip(song, id, clip);

    const Track buffer = song.tracks[0];
    Track& pasted = appendTrackCopy(song, buffer);

    REQUIRE(pasted.name == "Riff");
    REQUIRE(pasted.type == TrackType::Guitar);
    REQUIRE(pasted.clips.size() == 1);
    REQUIRE(pasted.clips[0].pattern.notes[0].noteNumber == 55);
}
