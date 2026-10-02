#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "model/ArrangementEdits.h"
#include "model/BeatClock.h"
#include "model/TempoChanges.h"
#include "model/TimeSelection.h"

namespace soundsplice::model::textedit
{
/**
    Editing audio by editing its transcript (Descript's idea): a track's
    clips' words as one running text in timeline time, and cutting the audio
    under the words taken out.

    Words live on their clips in file time (Clip::transcript); here they're
    placed on the timeline through each clip's start and offset, and only the
    ones inside a clip's window - what it actually plays - are shown. So a
    clip split by an earlier cut still reads as one text: each piece shows
    its own words.

    A cut takes the time out of that track alone and closes the gap - a
    music bed on another track carries on under it - then crossfades the
    join over a few milliseconds from the audio beyond each edge, so it's
    heard as speech rather than a click.
*/
struct TrackWord
{
    int         clipId    = 0;
    int         wordIndex = 0;
    std::string text;
    double      start = 0.0, end = 0.0; // seconds on the timeline
    float       confidence = 1.0f;
};

/** The words track @p trackIndex plays, in time order. Warped clips are
    left out: their file time and the timeline's don't map one to one. */
inline std::vector<TrackWord> wordsOn(const Song& song, int trackIndex)
{
    std::vector<TrackWord> words;
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return words;
    const auto clock = clockFor(song);

    for (const auto& clip : song.tracks[(size_t) trackIndex].clips)
    {
        if (clip.type != ClipType::Audio || clip.warp || clip.transcript.empty())
            continue;
        const double clipStart = clock.secondsAt(clip.startBeats);
        const double length    = clock.secondsBetween(clip.startBeats, clip.startBeats + clip.lengthBeats);
        const double from = clip.sourceOffsetSeconds, to = from + length;
        for (int i = 0; i < (int) clip.transcript.size(); ++i)
        {
            const auto& word = clip.transcript[(size_t) i];
            const double middle = 0.5 * (word.start + word.end);
            if (middle < from || middle >= to)
                continue; // trimmed away or cut out
            words.push_back({ clip.id, i, word.text, clipStart + std::max(word.start, from) - from,
                              clipStart + std::min(word.end, to) - from, word.confidence });
        }
    }
    std::stable_sort(words.begin(), words.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return words;
}

/** "Um", "uh," and the rest: the hesitations a filler pass takes out. */
inline bool isFiller(const std::string& word)
{
    std::string bare;
    for (const unsigned char c : word)
        if (std::isalpha(c))
            bare += (char) std::tolower(c);
    static const std::set<std::string> fillers { "um", "umm", "uh", "uhh", "uhm", "erm", "er", "err", "ah", "ahh", "hmm", "mm", "mmm" };
    return fillers.count(bare) > 0;
}

/** The silences between consecutive @p words longer than @p minimumSeconds,
    as [end of one, start of the next]. */
inline std::vector<std::pair<double, double>> pausesBetween(const std::vector<TrackWord>& words, double minimumSeconds)
{
    std::vector<std::pair<double, double>> pauses;
    for (size_t i = 1; i < words.size(); ++i)
        if (words[i].start - words[i - 1].end > minimumSeconds)
            pauses.emplace_back(words[i - 1].end, words[i].start);
    return pauses;
}

/** The part of a pause from @p start to @p end to cut, leaving @p keepSeconds
    of it (half each side, so neither word is clipped). Empty if it's
    already that short. */
inline std::pair<double, double> shortened(double start, double end, double keepSeconds)
{
    if (end - start <= keepSeconds)
        return { start, start };
    return { start + 0.5 * keepSeconds, end - 0.5 * keepSeconds };
}

/** Cuts each of @p ranges (timeline seconds) out of track @p trackId and
    closes the gaps, crossfading each join over @p crossfadeSeconds; ranges
    that touch or overlap are merged. @p fileSeconds gives a file's length,
    for the crossfades' audio beyond the edges. Returns how many cuts. */
inline int cutRanges(Song& song, int trackId, std::vector<std::pair<double, double>> ranges, double crossfadeSeconds,
                     const std::function<double(const std::string&)>& fileSeconds)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& r : ranges)
    {
        if (r.second <= r.first)
            continue;
        if (! merged.empty() && r.first <= merged.back().second + 1e-6)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }

    // Last first, so the earlier ones are where they were.
    int cuts = 0;
    for (auto it = merged.rbegin(); it != merged.rend(); ++it)
    {
        const auto    clock = clockFor(song);
        TimeSelection selection;
        selection.startBeats = clock.beatAt(it->first);
        selection.endBeats   = clock.beatAt(it->second);
        selection.trackIds   = { trackId };
        if (selection.isEmpty())
            continue;
        rangeedit::removeRange(song, selection, true);
        ++cuts;

        if (crossfadeSeconds > 0.0)
        {
            const double half = clockFor(song).beatsAfter(selection.startBeats, 0.5 * crossfadeSeconds);
            arrangeedit::crossfadeClips(song, { trackId }, std::max(0.0, selection.startBeats - half), selection.startBeats + half,
                                        fileSeconds);
        }
    }
    return cuts;
}

} // namespace soundsplice::model::textedit
