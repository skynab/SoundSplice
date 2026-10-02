#pragma once

#include <algorithm>
#include <vector>

#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::app
{
/**
    Match Loudness (Audition's): the audio clips to bring to one loudness
    target together - every one the time selection touches, on its tracks,
    or with no selection, every one on the selected track. Each keeps its own
    audio; only its gain changes, so the match can be undone or redone at
    another target freely.

    JUCE-free: which clips, by id, in track then time order.
*/
struct MatchedClip
{
    int trackId = 0;
    int clipId  = 0;

    bool operator==(const MatchedClip&) const = default;
};

inline std::vector<MatchedClip> clipsToMatch(const model::Song& song, const model::TimeSelection& selection,
                                             int selectedTrackIndex)
{
    std::vector<MatchedClip> clips;
    const bool byTime = selection.hasTracks() && ! selection.isEmpty();

    for (int t = 0; t < (int) song.tracks.size(); ++t)
    {
        const auto& track = song.tracks[(size_t) t];
        if (byTime ? ! selection.includes(track.id) : t != selectedTrackIndex)
            continue;

        std::vector<const model::Clip*> found;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;
            if (byTime && (clip.startBeats >= selection.endBeats || clip.startBeats + clip.lengthBeats <= selection.startBeats))
                continue;
            found.push_back(&clip);
        }

        std::sort(found.begin(), found.end(), [](const model::Clip* a, const model::Clip* b) { return a->startBeats < b->startBeats; });
        for (const auto* clip : found)
            clips.push_back({ track.id, clip->id });
    }
    return clips;
}

} // namespace soundsplice::app
