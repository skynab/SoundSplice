#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "model/ArrangementEdits.h"
#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::model
{
/**
    Takes and comping: an audio clip that holds several recordings of the same
    stretch (model::ClipTake), plays one of them, and can be cut into pieces
    that each play a different one - the best phrase of each pass, joined.

    Built on the clip as it already was: the playing take's file and offset
    are the clip's own, and the others are kept relative to it, so every edit
    that trims, splits, slips or copies a clip carries its takes along
    without knowing they're there. Comping a range is a split at its edges and
    a change of take in the middle.
*/
namespace takeedit
{
    /** Where in @p take's file @p clip is at its start, in seconds. */
    inline double takeOffsetSeconds(const Clip& clip, const ClipTake& take)
    {
        const double activeShift = clip.takes.empty() ? 0.0 : clip.takes[(size_t) clip.activeTake].shiftSeconds;
        return clip.sourceOffsetSeconds + take.shiftSeconds - activeShift;
    }

    /** Plays take @p index of @p clip from now on. False if there is no such
        take; true, changing nothing, if it's already the one playing. */
    inline bool setActiveTake(Clip& clip, int index)
    {
        if (index < 0 || index >= (int) clip.takes.size())
            return false;

        const auto& take           = clip.takes[(size_t) index];
        clip.sourceOffsetSeconds    = takeOffsetSeconds(clip, take);
        clip.audioFile              = take.audioFile;
        clip.activeTake             = index;
        return true;
    }

    /**
        Makes the audio clips @p clipIds on track @p trackId into one clip
        with a take for each - each of theirs, for a clip that already had
        takes - spanning all of them, every take kept where it was on the
        timeline. The clip that plays is the one with the highest id, the
        latest recorded or added. Returns the new clip's id, or 0 if fewer than
        two audio clips were found.
    */
    inline int combineIntoTakes(Song& song, int trackId, const std::vector<int>& clipIds)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr || song.bpm <= 0.0)
            return 0;

        std::vector<const Clip*> sources;
        for (const auto& clip : track->clips)
            if (clip.type == ClipType::Audio && std::find(clipIds.begin(), clipIds.end(), clip.id) != clipIds.end())
                sources.push_back(&clip);
        if (sources.size() < 2)
            return 0;

        double start = sources.front()->startBeats, end = start;
        for (const auto* clip : sources)
        {
            start = std::min(start, clip->startBeats);
            end   = std::max(end, clip->startBeats + clip->lengthBeats);
        }
        const auto* playing = *std::max_element(sources.begin(), sources.end(),
                                                [](const Clip* a, const Clip* b) { return a->id < b->id; });

        // Where each take's file is at the combined clip's start.
        struct Placed
        {
            ClipTake take;
            double   offsetAtStart = 0.0;
            bool     playing       = false;
        };
        std::vector<Placed> placed;
        for (const auto* clip : sources)
        {
            const double lead = (start - clip->startBeats) * 60.0 / song.bpm;
            if (clip->takes.empty())
            {
                placed.push_back({ { clip->audioFile, 0.0, {} }, clip->sourceOffsetSeconds + lead, clip == playing });
                continue;
            }
            for (int t = 0; t < (int) clip->takes.size(); ++t)
                placed.push_back({ clip->takes[(size_t) t], takeOffsetSeconds(*clip, clip->takes[(size_t) t]) + lead,
                                   clip == playing && t == clip->activeTake });
        }

        const auto active = std::find_if(placed.begin(), placed.end(), [](const Placed& p) { return p.playing; });

        Clip combined              = *playing;
        combined.id                = allocateId(song);
        combined.startBeats        = start;
        combined.lengthBeats       = end - start;
        combined.sourceOffsetSeconds = active->offsetAtStart;
        combined.fades             = {};
        combined.autoFadeIn        = false;
        combined.autoFadeOut       = false;
        combined.takes.clear();
        for (size_t i = 0; i < placed.size(); ++i)
        {
            auto take         = placed[i].take;
            take.shiftSeconds = placed[i].offsetAtStart - active->offsetAtStart;
            if (take.name.empty())
                take.name = "Take " + std::to_string(i + 1);
            combined.takes.push_back(take);
        }
        combined.activeTake = (int) std::distance(placed.begin(), active);

        const int firstIndex = (int) (sources.front() - track->clips.data());
        std::vector<Clip> clips;
        for (int i = 0; i < (int) track->clips.size(); ++i)
        {
            if (i == firstIndex)
                clips.push_back(combined);
            const auto& clip = track->clips[(size_t) i];
            if (std::find(sources.begin(), sources.end(), &clip) == sources.end())
                clips.push_back(clip);
        }
        track->clips = std::move(clips);
        return combined.id;
    }

    /**
        Comps: take @p takeIndex plays over [@p fromBeats, @p toBeats) of clip
        @p clipId, which is split at those points (where they fall inside it)
        so the rest still plays what it did. Pieces that end up carrying
        straight on from each other are joined again, so comping a stretch
        back to the take either side leaves one clip. True if anything changed.
    */
    inline bool compRange(Song& song, int trackId, int clipId, double fromBeats, double toBeats, int takeIndex)
    {
        auto* track = findTrack(song, trackId);
        if (track == nullptr)
            return false;

        const auto it = std::find_if(track->clips.begin(), track->clips.end(),
                                     [clipId](const Clip& c) { return c.id == clipId; });
        if (it == track->clips.end() || takeIndex < 0 || takeIndex >= (int) it->takes.size())
            return false;

        const Clip   clip  = *it;
        const double start = clip.startBeats;
        const double end   = clip.startBeats + clip.lengthBeats;
        const double from  = std::max(fromBeats, start);
        const double to    = std::min(toBeats, end);

        auto middle = rangeedit::pieceOf(clip, from, to, song.bpm);
        if (! middle || middle->activeTake == takeIndex)
            return false;

        auto before = rangeedit::pieceOf(clip, start, from, song.bpm);
        auto after  = rangeedit::pieceOf(clip, to, end, song.bpm);
        setActiveTake(*middle, takeIndex);

        // The first piece keeps the clip's id, so a selection on it holds.
        std::vector<Clip> pieces;
        if (before)
            pieces.push_back(*before);
        pieces.push_back(*middle);
        if (after)
            pieces.push_back(*after);
        for (size_t i = 1; i < pieces.size(); ++i)
            pieces[i].id = allocateId(song);

        const auto at = std::distance(track->clips.begin(), it);
        track->clips.erase(track->clips.begin() + at);
        track->clips.insert(track->clips.begin() + at, pieces.begin(), pieces.end());

        arrangeedit::joinClips(song, { trackId }, from, to);
        return true;
    }
}

} // namespace soundsplice::model
