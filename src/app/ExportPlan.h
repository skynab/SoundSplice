#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "model/Song.h"
#include "model/TempoChanges.h"

namespace soundsplice::app::exportplan
{
/**
    What an export writes, worked out from the song: the stretches a
    per-marker export splits into, the chapters a file carries, and the clips
    its render report lists. JUCE-free and tested; MainComponent_ImportExport
    turns them into files, tags and reports.
*/

/** @p markers in time order. */
inline std::vector<model::Marker> inTimeOrder(std::vector<model::Marker> markers)
{
    std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.startBeats < b.startBeats; });
    return markers;
}

/** The stretches a file-per-range export writes, each named. Every marker
    range, in time order; or, with @p betweenMarkers, the song split at every
    marker (Audacity's Export Multiple by labels): from each to the next, the
    first from the start (named "Start"), the last running to @p endBeats.
    Each is named for the marker it starts at. */
inline std::vector<model::Marker> regions(const std::vector<model::Marker>& markers, bool betweenMarkers, double endBeats)
{
    std::vector<model::Marker> out;
    if (! betweenMarkers)
    {
        for (const auto& marker : inTimeOrder(markers))
            if (marker.lengthBeats > 0.0)
                out.push_back(marker);
        return out;
    }

    double      from = 0.0;
    std::string name = "Start";
    for (const auto& marker : inTimeOrder(markers))
    {
        if (marker.startBeats > from + 1.0e-6)
            out.push_back({ 0, from, marker.startBeats - from, name });
        from = marker.startBeats;
        name = marker.name;
    }
    if (endBeats > from + 1.0e-6)
        out.push_back({ 0, from, endBeats - from, name });
    return out;
}

struct Chapter
{
    double      startSeconds = 0.0, endSeconds = 0.0;
    std::string title;
};

/** The markers in [@p startBeats, @p endBeats) as chapters timed from
    @p startBeats, each running to the next and the last to @p endBeats. */
inline std::vector<Chapter> chapters(const model::Song& song, double startBeats, double endBeats)
{
    std::vector<Chapter> out;
    const auto           clock = model::clockFor(song);
    for (const auto& marker : inTimeOrder(song.markers))
    {
        if (marker.startBeats < startBeats - 1.0e-9 || marker.startBeats >= endBeats)
            continue;
        Chapter chapter;
        chapter.startSeconds = clock.secondsBetween(startBeats, marker.startBeats);
        chapter.title        = marker.name;
        if (! out.empty())
            out.back().endSeconds = chapter.startSeconds;
        out.push_back(chapter);
    }
    if (! out.empty())
        out.back().endSeconds = clock.secondsBetween(startBeats, endBeats);
    return out;
}

/** A clip a render report lists. */
struct ReportClip
{
    std::string track;
    std::string audioFile;
    double      startSeconds  = 0.0; // from the start of what's rendered
    double      lengthSeconds = 0.0;
};

/** The clips sounding in [@p fromBeats, @p toBeats), on @p soloTrack only
    for a stem (-1 for the mix), in the order they start. */
inline std::vector<ReportClip> reportClips(const model::Song& song, int soloTrack, double fromBeats, double toBeats)
{
    std::vector<ReportClip> out;
    const auto              clock = model::clockFor(song);
    for (int t = 0; t < (int) song.tracks.size(); ++t)
    {
        if (soloTrack >= 0 && t != soloTrack)
            continue;
        for (const auto& clip : song.tracks[(size_t) t].clips)
        {
            if (clip.startBeats + clip.lengthBeats <= fromBeats || clip.startBeats >= toBeats)
                continue;
            ReportClip line;
            line.track         = song.tracks[(size_t) t].name;
            line.audioFile     = clip.audioFile;
            line.startSeconds  = clock.secondsBetween(fromBeats, clip.startBeats);
            line.lengthSeconds = clock.secondsBetween(clip.startBeats, clip.startBeats + clip.lengthBeats);
            out.push_back(line);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    return out;
}

} // namespace soundsplice::app::exportplan
