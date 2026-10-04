#pragma once

#include <map>
#include <string>
#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    What differs between two states of a song, said in a few lines - for
    the History pane's comparison of an earlier step with now. Tracks are
    matched by id (so a renamed or moved one is the same track), clips the
    same way within a track.
*/
inline std::vector<std::string> differences(const Song& from, const Song& to, size_t limit = 24)
{
    std::vector<std::string> out;
    const auto say = [&out](std::string line) { out.push_back(std::move(line)); };
    const auto number = [](double v)
    {
        auto text = std::to_string(v);
        text.erase(text.find_last_not_of('0') + 1);
        if (! text.empty() && text.back() == '.')
            text.pop_back();
        return text;
    };

    if (from.bpm != to.bpm)
        say("Tempo " + number(from.bpm) + " -> " + number(to.bpm) + " BPM");
    if (from.tempoChanges != to.tempoChanges)
        say("Tempo changes edited");
    if (from.timeSigNumerator != to.timeSigNumerator || from.timeSigDenominator != to.timeSigDenominator)
        say("Time signature " + std::to_string(from.timeSigNumerator) + "/" + std::to_string(from.timeSigDenominator) + " -> "
            + std::to_string(to.timeSigNumerator) + "/" + std::to_string(to.timeSigDenominator));

    std::map<int, const Track*> before, after;
    for (const auto& t : from.tracks)
        before[t.id] = &t;
    for (const auto& t : to.tracks)
        after[t.id] = &t;

    for (const auto& [id, track] : after)
        if (before.count(id) == 0)
            say("Added track \"" + track->name + "\"");
    for (const auto& [id, track] : before)
        if (after.count(id) == 0)
            say("Removed track \"" + track->name + "\"");

    for (const auto& [id, a] : before)
    {
        const auto it = after.find(id);
        if (it == after.end() || *a == *it->second)
            continue;
        const auto& b    = *it->second;
        const auto  name = "\"" + b.name + "\"";
        if (a->name != b.name)
            say("Renamed \"" + a->name + "\" to " + name);
        if (a->gainDb != b.gainDb || a->pan != b.pan)
            say(name + ": volume or pan changed");
        if (a->muted != b.muted || a->solo != b.solo)
            say(name + ": mute or solo changed");
        if (a->effectChain != b.effectChain)
            say(name + ": effects changed");
        if (a->automation != b.automation)
            say(name + ": automation changed");

        std::map<int, const Clip*> clipsBefore, clipsAfter;
        for (const auto& c : a->clips)
            clipsBefore[c.id] = &c;
        for (const auto& c : b.clips)
            clipsAfter[c.id] = &c;
        int added = 0, removed = 0, changed = 0;
        for (const auto& [cid, clip] : clipsAfter)
            if (clipsBefore.count(cid) == 0)
                ++added;
        for (const auto& [cid, clip] : clipsBefore)
        {
            const auto other = clipsAfter.find(cid);
            if (other == clipsAfter.end())
                ++removed;
            else if (! (*clip == *other->second))
                ++changed;
        }
        const auto clips = [](int n) { return std::to_string(n) + (n == 1 ? " clip" : " clips"); };
        if (added > 0)
            say(name + ": " + clips(added) + " added");
        if (removed > 0)
            say(name + ": " + clips(removed) + " removed");
        if (changed > 0)
            say(name + ": " + clips(changed) + " changed");
    }

    if (from.markers != to.markers)
    {
        if (from.markers.size() != to.markers.size())
            say("Markers: " + std::to_string(from.markers.size()) + " -> " + std::to_string(to.markers.size()));
        else
            say("Markers moved or renamed");
    }
    if (! (from.mastering == to.mastering) || from.masterEffects != to.masterEffects || from.masterGainDb != to.masterGainDb)
        say("Master bus changed");
    if (! (from.info == to.info))
        say("Project info changed");

    if (out.empty())
        say("No difference");
    if (out.size() > limit)
    {
        const auto more = out.size() - limit;
        out.resize(limit);
        out.push_back("... and " + std::to_string(more) + " more");
    }
    return out;
}

} // namespace soundsplice::model
