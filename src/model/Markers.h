#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/SequencerMath.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Markers: named points and ranges on the timeline, as Audacity's labels and
    Audition's markers are — "Verse 2", "cough here", "ad break".

    Kept in Song::markers, in timeline order. Positions are in beats like
    everything else on the timeline, and a tempo change keeps them at the same
    time (see retimeAudioForTempoChange), because a marker usually marks a
    place in recorded audio.

    Audacity's label files can be read and written, so markers move between
    the two apps: one "start<TAB>end<TAB>name" line per label, in seconds.

    JUCE-free and tested headless.
*/

/** How close two positions have to be to count as the same place. */
inline constexpr double kMarkerTolerance = 1.0e-6;

/** Adds a marker, keeping the list in timeline order (a marker at the same
    place as another goes after it). Returns its id. */
inline int addMarker(Song& song, double startBeats, double lengthBeats, std::string name)
{
    Marker marker;
    marker.id          = allocateId(song);
    marker.startBeats  = std::max(0.0, startBeats);
    marker.lengthBeats = std::max(0.0, lengthBeats);
    marker.name        = std::move(name);

    const auto position = std::upper_bound(song.markers.begin(), song.markers.end(), marker.startBeats,
                                           [](double beat, const Marker& m) { return beat < m.startBeats; });
    const int id = marker.id;
    song.markers.insert(position, std::move(marker));
    return id;
}

inline Marker* findMarker(Song& song, int id)
{
    for (auto& marker : song.markers)
        if (marker.id == id)
            return &marker;
    return nullptr;
}

inline const Marker* findMarker(const Song& song, int id)
{
    for (const auto& marker : song.markers)
        if (marker.id == id)
            return &marker;
    return nullptr;
}

inline bool removeMarker(Song& song, int id)
{
    const auto it = std::find_if(song.markers.begin(), song.markers.end(),
                                 [id](const Marker& marker) { return marker.id == id; });
    if (it == song.markers.end())
        return false;

    song.markers.erase(it);
    return true;
}

inline bool renameMarker(Song& song, int id, std::string name)
{
    auto* marker = findMarker(song, id);
    if (marker == nullptr)
        return false;

    marker->name = std::move(name);
    return true;
}

/** Moves marker @p id to start at @p startBeats (never before zero), keeping
    its length, and keeping the list in timeline order. */
inline bool moveMarker(Song& song, int id, double startBeats)
{
    const auto* marker = findMarker(song, id);
    if (marker == nullptr)
        return false;

    Marker moved     = *marker;
    moved.startBeats = std::max(0.0, startBeats);
    removeMarker(song, id);

    const auto position = std::upper_bound(song.markers.begin(), song.markers.end(), moved.startBeats,
                                           [](double beat, const Marker& m) { return beat < m.startBeats; });
    song.markers.insert(position, std::move(moved));
    return true;
}

/** The stretch between the marker edges either side of @p beat: from the last
    marker start or range end at or before it (the start of the song if there
    is none) to the first one after it (@p endBeats if there is none). Inside a
    range, that's the range. What double-clicking between markers selects. */
inline std::pair<double, double> spanBetweenMarkers(const Song& song, double beat, double endBeats)
{
    double from = 0.0;
    double to   = std::max(endBeats, beat);

    for (const auto& marker : song.markers)
    {
        for (const double edge : { marker.startBeats, marker.startBeats + marker.lengthBeats })
        {
            if (edge <= beat + kMarkerTolerance)
                from = std::max(from, edge);
            else
                to = std::min(to, edge);
        }
    }

    return { from, to };
}

/** "Marker 3": the lowest "Marker N" not already in use, so new names stay
    distinct and a deleted marker's name gets reused rather than skipped. */
inline std::string nextMarkerName(const Song& song)
{
    for (int n = 1;; ++n)
    {
        const auto candidate = "Marker " + std::to_string(n);
        const bool taken = std::any_of(song.markers.begin(), song.markers.end(),
                                       [&candidate](const Marker& marker) { return marker.name == candidate; });
        if (! taken)
            return candidate;
    }
}

/** Where the first marker after @p beat starts, or nothing. A marker at
    @p beat itself doesn't count, so jumping from one marker moves on to the
    next rather than staying put. */
inline std::optional<double> nextMarkerStart(const Song& song, double beat)
{
    for (const auto& marker : song.markers)
        if (marker.startBeats > beat + kMarkerTolerance)
            return marker.startBeats;
    return std::nullopt;
}

/** Where the last marker before @p beat starts, or nothing. */
inline std::optional<double> previousMarkerStart(const Song& song, double beat)
{
    for (auto it = song.markers.rbegin(); it != song.markers.rend(); ++it)
        if (it->startBeats < beat - kMarkerTolerance)
            return it->startBeats;
    return std::nullopt;
}

/** The song's markers as an Audacity label file: "start<TAB>end<TAB>name"
    per line, in seconds, with a point marker's end equal to its start. */
inline std::string exportMarkersAsLabels(const Song& song)
{
    const double secondsPerBeat = 60.0 / (song.bpm > 0.0 ? song.bpm : 120.0);

    std::ostringstream out;
    for (const auto& marker : song.markers)
    {
        // A name is the rest of its line, so tabs and line breaks become spaces.
        auto name = marker.name;
        std::replace(name.begin(), name.end(), '\t', ' ');
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');

        char times[96];
        std::snprintf(times, sizeof(times), "%.6f\t%.6f\t", marker.startBeats * secondsPerBeat,
                      (marker.startBeats + marker.lengthBeats) * secondsPerBeat);
        out << times << name << "\n";
    }
    return out.str();
}

/** Markers read from an Audacity label file, converted to beats at @p bpm,
    in file order and without ids (addMarker gives them those). Lines that
    aren't labels are skipped: blank ones, Audacity's frequency-range lines
    (which start with a backslash), and anything whose times don't parse. */
inline std::vector<Marker> markersFromLabels(const std::string& text, double bpm)
{
    const double tempo = bpm > 0.0 ? bpm : 120.0;

    std::vector<Marker> markers;
    std::istringstream  in(text);
    std::string         line;

    while (std::getline(in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '\\')
            continue;

        const char* cursor   = line.c_str();
        char*       afterOne = nullptr;
        const double start   = std::strtod(cursor, &afterOne);
        if (afterOne == cursor)
            continue;

        char*        afterTwo = nullptr;
        const double end      = std::strtod(afterOne, &afterTwo);
        if (afterTwo == afterOne)
            continue;

        std::string name(afterTwo);
        const auto  first = name.find_first_not_of(" \t");
        name = first == std::string::npos ? std::string() : name.substr(first);

        Marker marker;
        marker.startBeats  = engine::beatsForSeconds(std::max(0.0, start), tempo);
        marker.lengthBeats = engine::beatsForSeconds(std::max(0.0, end - start), tempo);
        marker.name        = std::move(name);
        markers.push_back(std::move(marker));
    }

    return markers;
}

} // namespace soundsplice::model
