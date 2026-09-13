#pragma once

#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Effects.h"
#include "engine/TempoMap.h"
#include "model/Track.h"

namespace looper::model
{
/**
    The whole project document: tempo/metre plus a list of tracks. Pure value
    type — copyable and comparable — which is what makes snapshot undo/redo and
    (later) serialization straightforward. All mutation goes through the helper
    functions below so ids are allocated consistently.
*/
/** One row of the session grid, shared across every track. */
struct Scene
{
    std::string name;

    bool operator==(const Scene&) const = default;
};

struct Song
{
    /** The tempo at the start of the song.

        Kept alongside tempoChanges rather than replaced by it: it is read in
        fifteen places across the app, and every one of them is correct for a
        single-tempo project. tempoChanges[0] mirrors it, and the two are kept
        in step by model::tempoChangesFor / applyTempo below. */
    double bpm                = 120.0;
    int    timeSigNumerator   = 4;
    int    timeSigDenominator = 4;

    /** Tempo changes after the start, sorted by beat. Empty means one tempo
        for the whole song, which is what every project written before format
        version 32 means. See engine::TempoMap. */
    std::vector<engine::TempoChange> tempoChanges;

    std::vector<Track> tracks;
    std::vector<Scene> scenes; // session-grid rows; every track's sessionSlots matches this length
    int                nextId = 1; // monotonic id source for tracks and clips
    FilterSettings     filter;
    DelaySettings      delay;
    ReverbSettings     reverb;
    SendBusSettings    sendBus;
    EqSettings         eq;
    MasteringSettings  mastering;
    AutomationLane     masterGainDb; // master gain automation (dB over beats)

    // Project-specific data, not an app preference — round-trips with the
    // project so it's the same on every machine that opens it. Empty =
    // unset. See the file-manager's "Places" entry for it.
    std::string projectRootFolder;

    bool operator==(const Song&) const = default;
};

inline int allocateId(Song& song) { return song.nextId++; }

inline Track& addTrack(Song& song, TrackType type, std::string name)
{
    Track track;
    track.id   = allocateId(song);
    track.type = type;
    track.name = std::move(name);
    if (type == TrackType::Drum)
        track.drumKit = makeDefaultDrumKit();
    // A new track joins the existing scenes with every slot empty, so the grid
    // stays rectangular without anyone having to remember to resize it.
    track.sessionSlots.resize(song.scenes.size());
    song.tracks.push_back(std::move(track));
    return song.tracks.back();
}

/** Appends a scene (a session-grid row), giving every track an empty slot in
    it. Returns its index. */
inline int addScene(Song& song, std::string name)
{
    song.scenes.push_back(Scene { std::move(name) });
    for (auto& track : song.tracks)
        track.sessionSlots.resize(song.scenes.size());
    return (int) song.scenes.size() - 1;
}

/** The clip in a session cell, or nullptr when the cell is empty or the
    coordinates are out of range. */
inline const Clip* sessionClip(const Song& song, int trackIndex, int sceneIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return nullptr;
    const auto& slots = song.tracks[(size_t) trackIndex].sessionSlots;
    if (sceneIndex < 0 || sceneIndex >= (int) slots.size())
        return nullptr;
    const auto& slot = slots[(size_t) sceneIndex];
    return slot.hasClip ? &slot.clip : nullptr;
}

/** Puts @p clip into a session cell, growing the track's column if the grid
    was resized behind its back. Returns false if the coordinates are invalid. */
inline bool setSessionClip(Song& song, int trackIndex, int sceneIndex, Clip clip)
{
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return false;
    if (sceneIndex < 0 || sceneIndex >= (int) song.scenes.size())
        return false;

    auto& slots = song.tracks[(size_t) trackIndex].sessionSlots;
    if ((int) slots.size() <= sceneIndex)
        slots.resize(song.scenes.size());

    clip.id                 = allocateId(song);
    slots[(size_t) sceneIndex].hasClip = true;
    slots[(size_t) sceneIndex].clip    = std::move(clip);
    return true;
}

/** Empties a session cell. */
inline void clearSessionClip(Song& song, int trackIndex, int sceneIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;
    auto& slots = song.tracks[(size_t) trackIndex].sessionSlots;
    if (sceneIndex >= 0 && sceneIndex < (int) slots.size())
        slots[(size_t) sceneIndex] = SessionSlot {};
}

inline Track* findTrack(Song& song, int id)
{
    for (auto& t : song.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

inline const Track* findTrack(const Song& song, int id)
{
    for (const auto& t : song.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

inline bool removeTrack(Song& song, int id)
{
    for (auto it = song.tracks.begin(); it != song.tracks.end(); ++it)
    {
        if (it->id == id)
        {
            song.tracks.erase(it);
            return true;
        }
    }
    return false;
}

/** Renames a track. Returns false if there's no such track. */
inline bool renameTrack(Song& song, int id, std::string name)
{
    Track* track = findTrack(song, id);
    if (track == nullptr)
        return false;

    track->name = std::move(name);
    return true;
}

/** Adds @p clip to the given track, assigning it a fresh id. Returns nullptr if the track is missing. */
inline Clip* addClip(Song& song, int trackId, Clip clip)
{
    Track* track = findTrack(song, trackId);
    if (track == nullptr)
        return nullptr;

    clip.id = allocateId(song);
    track->clips.push_back(std::move(clip));
    return &track->clips.back();
}

/** Removes one arrangement clip by its position in the track. Returns false
    if the track or the index is out of range.

    By index rather than by id because that's how the UI addresses the clip it
    has selected; ids stay unique because allocateId only ever counts up, so a
    later clip can't reuse a removed one's id. */
inline bool removeClip(Song& song, int trackId, int clipIndex)
{
    Track* track = findTrack(song, trackId);
    if (track == nullptr || clipIndex < 0 || clipIndex >= (int) track->clips.size())
        return false;

    track->clips.erase(track->clips.begin() + clipIndex);
    return true;
}

/** Gives @p track and everything on it fresh ids from @p song's counter.

    Ids are how the rest of the app addresses things, so two tracks sharing
    them is a second track that edits resolve to at random. One function does
    this for every path that produces a copy — duplicating and pasting — so
    there is one place to get it right. */
inline void reissueTrackIds(Song& song, Track& track);

/** Copies a track and everything on it, inserting the copy directly after
    the original and returning it (nullptr if @p index is out of range).

    Every id is reissued: the track's, each arrangement clip's, and each
    session slot's. Ids are how the rest of the app addresses things, so a
    copy sharing them would be a second track that edits resolve to at random
    — the one bug this function exists to avoid.

    Inserted after the original rather than appended, because duplicating is
    something you do to work on a variant of that part, and having it turn up
    at the bottom of a long arrangement is a search. */
inline void reissueTrackIds(Song& song, Track& track)
{
    track.id = allocateId(song);

    for (auto& clip : track.clips)
        clip.id = allocateId(song);

    for (auto& slot : track.sessionSlots)
        if (slot.hasClip)
            slot.clip.id = allocateId(song);
}

inline Track* duplicateTrack(Song& song, int index)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return nullptr;

    Track copy = song.tracks[(size_t) index];
    reissueTrackIds(song, copy);
    copy.name = copy.name.empty() ? std::string("Track copy") : copy.name + " copy";

    const auto at = song.tracks.begin() + index + 1;
    return &*song.tracks.insert(at, std::move(copy));
}

/** Appends @p track — a copy held outside the song, as a paste buffer does —
    with fresh ids. Goes through the same reissuing as duplicateTrack, so
    pasting the same buffer twice can't produce two tracks sharing ids. */
inline Track& appendTrackCopy(Song& song, Track track)
{
    reissueTrackIds(song, track);
    song.tracks.push_back(std::move(track));
    return song.tracks.back();
}

/** Removes a session-grid row, taking that slot out of every track's column
    so the grid stays rectangular — the invariant every session lookup relies
    on. Returns false if there's no such scene. */
inline bool removeScene(Song& song, int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= (int) song.scenes.size())
        return false;

    song.scenes.erase(song.scenes.begin() + sceneIndex);

    for (auto& track : song.tracks)
        if (sceneIndex < (int) track.sessionSlots.size())
            track.sessionSlots.erase(track.sessionSlots.begin() + sceneIndex);

    return true;
}

/**
    The song's tempo map as one list: the starting tempo followed by every
    change after it.

    The document keeps those two apart — `bpm` because fifteen places in the
    app read it and are right to, `tempoChanges` because they came later — and
    this is the single place that knows they join. Anything handing a map to
    engine::TempoMap should go through here rather than assembling its own,
    which is how the two would drift.
*/
inline std::vector<engine::TempoChange> tempoMapFor(const Song& song)
{
    std::vector<engine::TempoChange> map;
    map.reserve(song.tempoChanges.size() + 1);
    map.push_back({ 0.0, song.bpm });

    for (const auto& change : song.tempoChanges)
        map.push_back(change);

    return map;
}

/** The tempo in force at @p beat — what the transport bar should show, and
    what a beats-from-seconds conversion at that point needs. */
inline double tempoAtBeat(const Song& song, double beat)
{
    double tempo = song.bpm;
    for (const auto& change : song.tempoChanges)
        if (beat >= change.beat)
            tempo = change.bpm;

    return tempo;
}

} // namespace looper::model
