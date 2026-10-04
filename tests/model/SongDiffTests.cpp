#include <catch2/catch_test_macros.hpp>

#include "model/SongDiff.h"

#include <algorithm>

using namespace soundsplice::model;

namespace
{
    bool says(const std::vector<std::string>& lines, const std::string& text)
    {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& l) { return l.find(text) != std::string::npos; });
    }
}

TEST_CASE("Two states of a song are compared in plain words", "[model][diff]")
{
    Song before;
    auto& voice = addTrack(before, TrackType::Audio, "Voice");
    Clip clip;
    clip.lengthBeats = 4.0;
    addClip(before, voice.id, clip);
    addClip(before, voice.id, clip);

    REQUIRE(differences(before, before) == std::vector<std::string> { "No difference" });

    Song after = before;
    after.bpm  = 96.0;
    after.tracks[0].name   = "Host";
    after.tracks[0].gainDb = -3.0f;
    after.tracks[0].clips[0].gainDb = 2.0f;
    after.tracks[0].clips.pop_back();
    addTrack(after, TrackType::Bus, "Reverb");

    const auto lines = differences(before, after);
    REQUIRE(says(lines, "Tempo 120 -> 96 BPM"));
    REQUIRE(says(lines, "Added track \"Reverb\""));
    REQUIRE(says(lines, "Renamed \"Voice\" to \"Host\""));
    REQUIRE(says(lines, "\"Host\": volume or pan changed"));
    REQUIRE(says(lines, "\"Host\": 1 clip removed"));
    REQUIRE(says(lines, "\"Host\": 1 clip changed"));

    const auto back = differences(after, before);
    REQUIRE(says(back, "Removed track \"Reverb\""));
}
