#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::model
{
/**
    Edits on the arrangement itself, rather than inside one clip in the audio
    editor: split at the playhead, join clips, and duplicate a time selection.

    Like the time selection's edits (model/TimeSelection.h), every one is
    non-destructive, working through clip windows and source offsets, and
    applies to audio tracks only for now.
*/
namespace arrangeedit
{
    /** Splits every audio clip on the tracks in @p trackIds that @p beat falls
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
} // namespace arrangeedit

} // namespace soundsplice::model
