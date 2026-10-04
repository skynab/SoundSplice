#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/TempoMap.h"
#include "model/BeatClock.h"
#include "model/WarpFactor.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Tempo changes: the song's tempo map, the starting tempo (Song::bpm)
    followed by the changes after it (Song::tempoChanges), each a step or a
    ramp into its tempo - see engine::TempoMap, which plays them.

    Every edit here keeps audio where it is in time, as changing the starting
    tempo always has (model/Timebase.h): a recording plays in real time,
    so the beat positions of audio clips,
    their tracks' automation and the markers are moved to wherever the same
    moment now falls.
*/
namespace tempoedit
{
    /** The song's whole tempo map, start first: what engine::TempoMap is
        given. The one place that joins Song::bpm and Song::tempoChanges. */
    inline std::vector<engine::TempoChange> mapFor(const Song& song)
    {
        std::vector<engine::TempoChange> map { { 0.0, song.bpm, false } };
        map.insert(map.end(), song.tempoChanges.begin(), song.tempoChanges.end());
        return map;
    }

    /** The song's clock, for turning beats into seconds and back along its
        tempo map (see model::BeatClock). */
    inline BeatClock clockFor(const Song& song)
    {
        if (song.tempoChanges.empty())
            return BeatClock(song.bpm);
        return BeatClock(mapFor(song));
    }

    /** The tempo in force at @p beat (on a ramp, where it has got to). */
    inline double tempoAt(const Song& song, double beat)
    {
        engine::TempoMap map;
        map.setTempoChanges(mapFor(song));
        return map.tempoAtBeat(beat);
    }

    /** Index of the change at @p beat, or -1. */
    inline int changeAt(const Song& song, double beat)
    {
        for (int i = 0; i < (int) song.tempoChanges.size(); ++i)
            if (std::abs(song.tempoChanges[(size_t) i].beat - beat) < 1.0e-9)
                return i;
        return -1;
    }

    /**
        Moves everything on audio tracks, and the markers, from where the
        tempo map @p before put them in time to the same times under @p after.
        Buses and the master lane stay on their beats.
    */
    inline void retimeAudio(Song& song, const std::vector<engine::TempoChange>& before,
                            const std::vector<engine::TempoChange>& after)
    {
        if (before == after)
            return;

        // At a microsecond resolution: positions round to it on the way
        // through, far below anything that can be heard or seen.
        engine::TempoMap from, to;
        from.setSampleRate(1.0e6);
        to.setSampleRate(1.0e6);
        from.setTempoChanges(before);
        to.setTempoChanges(after);
        const auto move = [&](double beat) { return to.ppqFromSamples(from.samplesFromPpq(beat)); };

        for (auto& track : song.tracks)
        {
            if (track.type != TrackType::Audio)
                continue;

            for (auto& clip : track.clips)
            {
                // A warped clip follows the beat instead, restretched to the
                // tempo it now starts at (model/Warp.h).
                if (clip.warp && clip.sourceBpm > 0.0)
                {
                    warpedit::rescale(clip, warpedit::factorUnder(after, clip) / warpedit::factorUnder(before, clip));
                    continue;
                }

                const double end = move(clip.startBeats + clip.lengthBeats);
                clip.startBeats  = move(clip.startBeats);
                clip.lengthBeats = std::max(0.0, end - clip.startBeats);
            }

            for (auto& entry : track.automation)
                entry.second.mapBeats(move);
            for (auto& slot : track.effectChain)
                for (auto& entry : slot.automation)
                    entry.second.mapBeats(move);
        }

        for (auto& slot : song.masterEffects)
            for (auto& entry : slot.automation)
                entry.second.mapBeats(move);

        for (auto& marker : song.markers)
        {
            const double end   = move(marker.startBeats + marker.lengthBeats);
            marker.startBeats  = move(marker.startBeats);
            marker.lengthBeats = marker.lengthBeats > 0.0 ? std::max(0.0, end - marker.startBeats) : 0.0;
        }
    }

    /** Applies @p change to the song's tempo map, then moves the audio to
        keep its time. */
    template <typename Change>
    void editMap(Song& song, Change change)
    {
        const auto before = mapFor(song);
        change(song);
        std::sort(song.tempoChanges.begin(), song.tempoChanges.end(),
                  [](const engine::TempoChange& a, const engine::TempoChange& b) { return a.beat < b.beat; });
        retimeAudio(song, before, mapFor(song));
    }

    /** Sets the tempo at @p beat to @p bpm: the starting tempo at beat 0 or
        before, else the change there, added if there isn't one. False for a
        tempo that isn't positive. */
    inline bool setTempo(Song& song, double beat, double bpm)
    {
        if (! (bpm > 0.0))
            return false;

        editMap(song, [beat, bpm](Song& s)
        {
            if (beat <= 0.0)
            {
                s.bpm = bpm;
                return;
            }
            if (const int i = changeAt(s, beat); i >= 0)
                s.tempoChanges[(size_t) i].bpm = bpm;
            else
                s.tempoChanges.push_back({ beat, bpm, false });
        });
        return true;
    }

    /** The beat of the change in force at @p beat: the latest at or before
        it, or 0 for the starting tempo. What the tempo control edits. */
    inline double changeInForceAt(const Song& song, double beat)
    {
        double at = 0.0;
        for (const auto& change : song.tempoChanges)
            if (change.beat <= beat + 1.0e-9)
                at = change.beat;
        return at;
    }

    inline bool removeChange(Song& song, double beat)
    {
        if (changeAt(song, beat) < 0)
            return false;
        editMap(song, [beat](Song& s) { s.tempoChanges.erase(s.tempoChanges.begin() + changeAt(s, beat)); });
        return true;
    }

    /** Moves the change at @p from to @p to (after beat 0, and not onto
        another change). */
    inline bool moveChange(Song& song, double from, double to)
    {
        if (changeAt(song, from) < 0 || ! (to > 0.0) || changeAt(song, to) >= 0)
            return false;
        editMap(song, [from, to](Song& s) { s.tempoChanges[(size_t) changeAt(s, from)].beat = to; });
        return true;
    }

    /** Switches the change at @p beat between jumping to its tempo and
        sliding to it from the one before. */
    inline bool toggleRamp(Song& song, double beat)
    {
        if (changeAt(song, beat) < 0)
            return false;
        editMap(song, [beat](Song& s)
        {
            auto& change = s.tempoChanges[(size_t) changeAt(s, beat)];
            change.ramp  = ! change.ramp;
        });
        return true;
    }
}

using tempoedit::clockFor;

} // namespace soundsplice::model
