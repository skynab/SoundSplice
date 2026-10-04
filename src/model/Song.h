#pragma once

#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Effects.h"
#include "engine/TempoMap.h"
#include "model/Track.h"

namespace soundsplice::model
{
/**
    The whole project document: tempo/metre plus a list of tracks. Pure value
    type — copyable and comparable — which is what makes snapshot undo/redo and
    (later) serialization straightforward. All mutation goes through the helper
    functions below so ids are allocated consistently.
*/
/** A named point or range on the timeline: what Audacity calls a label and
    Audition a marker. A point marker has no length. See model/Markers.h for
    what can be done with them. */
struct Marker
{
    int         id          = 0;
    double      startBeats  = 0.0;
    double      lengthBeats = 0.0; // 0 for a point
    std::string name;

    bool operator==(const Marker&) const = default;
};

/** What an exported file says about itself (its tags): ID3 in an MP3,
    Vorbis comments in FLAC and Ogg, INFO and bext in a WAV. Kept with the
    project, so every export of it carries the same. */
struct ProjectInfo
{
    std::string title, artist, album, year, genre, comment, track;
    std::string coverArt; // an image file, for the formats that carry one

    bool operator==(const ProjectInfo&) const = default;

    /** Each field by the key the project file and the tags use. */
    template <typename Fn>
    void forEachField(Fn&& fn)
    {
        fn("title", title);
        fn("artist", artist);
        fn("album", album);
        fn("year", year);
        fn("genre", genre);
        fn("comment", comment);
        fn("track", track);
        fn("cover", coverArt);
    }
};

struct Song
{
    /** The tempo at the start of the song; tempoChanges holds any after it
        (see model/TempoChanges.h, the one place the two are joined). */
    double bpm                = 120.0;

    /** Tempo changes after the start, sorted by beat, each a step or a ramp
        into its tempo. Empty: one tempo for the whole song. */
    std::vector<engine::TempoChange> tempoChanges;
    int    timeSigNumerator   = 4;
    int    timeSigDenominator = 4;

    std::vector<Track> tracks;
    int                nextId = 1; // monotonic id source for tracks and clips
    // The master bus's insert effects, in order: after every track, before
    // the mastering rack. A chain like a track's (EffectSlot), without
    // automation or sidechains.
    std::vector<EffectSlot> masterEffects;
    MasteringSettings  mastering;
    AutomationLane     masterGainDb; // master gain automation (dB over beats)
    std::vector<Marker> markers;     // in timeline order; see model/Markers.h

    // Project-specific data, not an app preference — round-trips with the
    // project so it's the same on every machine that opens it. Empty =
    // unset. See the file-manager's "Places" entry for it.
    std::string projectRootFolder;

    ProjectInfo info; // File > Project Info; written into exports

    // A reference video to work to (the Video pane), and where in the song
    // its first frame is. Empty: none.
    std::string videoFile;
    double      videoOffsetSeconds = 0.0;


    bool operator==(const Song&) const = default;
};

inline int allocateId(Song& song) { return song.nextId++; }

inline Track& addTrack(Song& song, TrackType type, std::string name)
{
    Track track;
    track.id   = allocateId(song);
    track.type = type;
    track.name = std::move(name);
    song.tracks.push_back(std::move(track));
    return song.tracks.back();
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

    Every id is reissued: the track's and each clip's. Ids are how the rest of the app addresses things, so a
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

} // namespace soundsplice::model
