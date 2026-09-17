#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::model
{
/**
    Edits on the arrangement itself, rather than inside one clip in the audio
    editor: split at the playhead, join clips, and duplicate a time selection.

    Like the time selection's edits (model/TimeSelection.h), every one is
    non-destructive, working through clip windows and source offsets (or, on
    an instrument track, pieces of pattern). Joining and detaching at silences
    are audio only.
*/
namespace arrangeedit
{
    /** Splits every clip on the tracks in @p trackIds that @p beat falls
        strictly inside into two that play back to back. Returns how many
        clips were split. */
    inline int splitClipsAt(Song& song, const std::vector<int>& trackIds, double beat)
    {
        int split = 0;

        for (auto& track : song.tracks)
        {
            if (std::find(trackIds.begin(), trackIds.end(), track.id) == trackIds.end()
                || ! rangeedit::appliesTo(track))
                continue;

            std::vector<Clip> clips;
            for (const auto& clip : track.clips)
            {
                const double start = clip.startBeats;
                const double end   = clip.startBeats + clip.lengthBeats;

                if (beat <= start + rangeedit::kEpsilonBeats || beat >= end - rangeedit::kEpsilonBeats)
                {
                    clips.push_back(clip);
                    continue;
                }

                auto first  = rangeedit::pieceOf(clip, start, beat, song.bpm);
                auto second = rangeedit::pieceOf(clip, beat, end, song.bpm);
                if (! first || ! second)
                {
                    clips.push_back(clip);
                    continue;
                }

                second->id = allocateId(song);
                clips.push_back(*first);
                clips.push_back(*second);
                ++split;
            }

            track.clips = std::move(clips);
        }

        return split;
    }

    /** Whether @p next carries straight on from @p clip: it starts where
        @p clip ends, plays the same file from where @p clip leaves off, and at
        the same gain, so the two sound exactly like one clip. */
    inline bool continues(const Clip& clip, const Clip& next, double bpm)
    {
        if (clip.type != ClipType::Audio || next.type != ClipType::Audio || bpm <= 0.0)
            return false;

        const double end            = clip.startBeats + clip.lengthBeats;
        const double offsetAtEnd    = clip.sourceOffsetSeconds + clip.lengthBeats * 60.0 / bpm;
        constexpr double kSeconds   = 1.0e-6;

        return clip.audioFile == next.audioFile
            && std::abs(next.startBeats - end) <= 1.0e-6
            && std::abs(next.sourceOffsetSeconds - offsetAtEnd) <= kSeconds
            && clip.gainDb == next.gainDb;
    }

    /** Joins neighbouring audio clips that carry straight on from each other
        (see continues), on the tracks in @p trackIds, where the join falls in
        [@p fromBeats, @p toBeats]: the way to undo a split without undoing
        everything since. The joined clip keeps the first's fade-in and the
        last's fade-out. Clips that don't continue each other are left alone,
        since joining them would mean writing new audio. Returns how many
        joins were made. */
    inline int joinClips(Song& song, const std::vector<int>& trackIds, double fromBeats, double toBeats)
    {
        int joined = 0;

        for (auto& track : song.tracks)
        {
            if (std::find(trackIds.begin(), trackIds.end(), track.id) == trackIds.end()
                || ! rangeedit::appliesTo(track))
                continue;

            auto clips = track.clips;
            std::stable_sort(clips.begin(), clips.end(),
                             [](const Clip& a, const Clip& b) { return a.startBeats < b.startBeats; });

            std::vector<Clip> out;
            for (const auto& clip : clips)
            {
                if (! out.empty())
                {
                    auto&        previous = out.back();
                    const double joinAt   = previous.startBeats + previous.lengthBeats;

                    if (joinAt >= fromBeats - rangeedit::kEpsilonBeats && joinAt <= toBeats + rangeedit::kEpsilonBeats
                        && continues(previous, clip, song.bpm))
                    {
                        previous.lengthBeats     += clip.lengthBeats;
                        previous.fades.outSeconds = clip.fades.outSeconds;
                        previous.fades.outShape   = clip.fades.outShape;
                        ++joined;
                        continue;
                    }
                }

                out.push_back(clip);
            }

            if (joined > 0)
                track.clips = std::move(out);
        }

        return joined;
    }

    /** Crossfades neighbouring audio clips on the tracks in @p trackIds
        wherever one ends and the next begins (or they already overlap) inside
        [@p fromBeats, @p toBeats]: the first is lengthened and the second
        started earlier, from the audio each has beyond its edge, so they
        overlap across the range, and each fades over the overlap. As much of
        the range as both clips have audio for is used; @p fileSeconds gives a
        file's length. Pieces of one recording that carry straight on from each
        other fade linearly, which keeps their sum level; anything else fades
        at equal power. Returns how many crossfades were made. */
    inline int crossfadeClips(Song& song, const std::vector<int>& trackIds, double fromBeats, double toBeats,
                              const std::function<double(const std::string&)>& fileSeconds)
    {
        if (song.bpm <= 0.0 || toBeats <= fromBeats)
            return 0;

        const double secondsPerBeat = 60.0 / song.bpm;
        constexpr double kEpsilon   = rangeedit::kEpsilonBeats;
        int              made       = 0;

        for (auto& track : song.tracks)
        {
            if (std::find(trackIds.begin(), trackIds.end(), track.id) == trackIds.end()
                || track.type != TrackType::Audio)
                continue;

            // In timeline order, by index: the clips stay where they are in
            // the track's list, which is what the selection refers to them by.
            std::vector<size_t> order(track.clips.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&track](size_t a, size_t b)
            {
                return track.clips[a].startBeats < track.clips[b].startBeats;
            });

            for (size_t i = 0; i + 1 < order.size(); ++i)
            {
                auto& first  = track.clips[order[i]];
                auto& second = track.clips[order[i + 1]];
                if (first.type != ClipType::Audio || second.type != ClipType::Audio)
                    continue;

                const double firstEnd = first.startBeats + first.lengthBeats;
                const double seam     = std::min(firstEnd, second.startBeats);

                // Touching or overlapping, with where they meet in the range.
                if (second.startBeats > firstEnd + kEpsilon || seam < fromBeats - kEpsilon
                    || std::max(firstEnd, second.startBeats) > toBeats + kEpsilon)
                    continue;

                const bool carriesOn = continues(first, second, song.bpm);

                // How far each can reach: the first to the end of its audio,
                // the second back to the start of its file.
                const double firstAudioEnd = first.startBeats
                                           + (fileSeconds(first.audioFile) - first.sourceOffsetSeconds) / secondsPerBeat;
                const double secondEarliest = second.startBeats - second.sourceOffsetSeconds / secondsPerBeat;
                const double secondEnd      = second.startBeats + second.lengthBeats;

                const double from = std::max({ fromBeats, secondEarliest, first.startBeats });
                const double to   = std::min({ toBeats, firstAudioEnd, secondEnd });
                if (to - from <= kEpsilon)
                    continue;

                const double moveBack = second.startBeats - from;
                second.sourceOffsetSeconds = std::max(0.0, second.sourceOffsetSeconds - moveBack * secondsPerBeat);
                second.lengthBeats        += moveBack;
                second.startBeats          = from;
                first.lengthBeats          = std::max(first.lengthBeats, to - first.startBeats);

                const double seconds = (to - from) * secondsPerBeat;
                const auto   shape   = carriesOn ? engine::FadeShape::Linear : engine::FadeShape::EqualPower;
                first.fades.outSeconds  = seconds;
                first.fades.outShape    = shape;
                second.fades.inSeconds  = seconds;
                second.fades.inShape    = shape;
                ++made;
            }
        }

        return made;
    }

    /** Puts a copy of what @p selection covers straight after it on the same
        tracks, pushing later clips along. Returns the selection over the copy,
        or an empty one if there was nothing to duplicate. */
    inline TimeSelection duplicateRange(Song& song, const TimeSelection& selection)
    {
        if (selection.isEmpty() || ! rangeedit::anyTrackApplies(song, selection))
            return {};

        const auto clipboard = rangeedit::copyRange(song, selection);
        if (! rangeedit::insertClipboard(song, selection.trackIds, clipboard, selection.endBeats))
            return {};

        return { selection.endBeats, selection.endBeats + selection.lengthBeats(), selection.trackIds };
    }
    /** Splits clip @p clipId on track @p trackId around @p silences (seconds
        from the clip's start, in order and not overlapping), leaving the
        silent parts out: the clip becomes the sounding pieces between them,
        each still playing what it played, where it played it. A clip that is
        silent throughout is removed. The first piece keeps the clip's id.
        Returns how many pieces are left, or -1 if there's no such audio clip. */
    inline int detachAtSilences(Song& song, int trackId, int clipId,
                                const std::vector<std::pair<double, double>>& silences)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr || ! rangeedit::appliesTo(*track) || song.bpm <= 0.0)
            return -1;

        const auto found = std::find_if(track->clips.begin(), track->clips.end(),
                                        [clipId](const Clip& c) { return c.id == clipId; });
        if (found == track->clips.end() || found->type != ClipType::Audio)
            return -1;

        const Clip   clip          = *found;
        const double start         = clip.startBeats;
        const double end           = clip.startBeats + clip.lengthBeats;
        const double beatsPerSecond = song.bpm / 60.0;

        std::vector<Clip> pieces;
        double            cursor = start;

        for (const auto& [fromSeconds, toSeconds] : silences)
        {
            const double from = std::clamp(start + fromSeconds * beatsPerSecond, start, end);
            const double to   = std::clamp(start + toSeconds * beatsPerSecond, start, end);
            if (to <= from)
                continue;

            if (auto piece = rangeedit::pieceOf(clip, cursor, from, song.bpm))
                pieces.push_back(*piece);
            cursor = std::max(cursor, to);
        }

        if (auto piece = rangeedit::pieceOf(clip, cursor, end, song.bpm))
            pieces.push_back(*piece);

        if (pieces.size() == 1 && pieces[0] == clip)
            return 1; // nothing silent enough to take out

        for (size_t i = 1; i < pieces.size(); ++i)
            pieces[i].id = allocateId(song);

        const auto at = track->clips.erase(found);
        track->clips.insert(at, pieces.begin(), pieces.end());
        return (int) pieces.size();
    }
} // namespace arrangeedit

} // namespace soundsplice::model
