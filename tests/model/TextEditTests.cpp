#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "model/TextEdit.h"

using namespace soundsplice::model;
using Catch::Matchers::WithinAbs;

namespace
{
    /** One audio clip at the start of a voice track, 10 s of a 20 s file,
        at 120 BPM (2 beats a second): "so um we went to the shop". */
    Song talk()
    {
        Song song;
        song.bpm = 120.0;
        auto& voice = addTrack(song, TrackType::Audio, "Voice");
        Clip clip;
        clip.type        = ClipType::Audio;
        clip.audioFile   = "talk.wav";
        clip.lengthBeats = 20.0; // 10 s
        clip.transcript  = { { "So", 0.5, 0.8 }, { "um,", 1.0, 1.4 }, { "we", 1.5, 1.7 }, { "went", 1.7, 2.0 },
                             { "to", 4.0, 4.1 }, { "the", 4.1, 4.3 }, { "shop.", 4.3, 4.8 }, { "Later", 12.0, 12.5 } };
        addClip(song, voice.id, clip);
        return song;
    }

    double fileSeconds(const std::string&) { return 20.0; }
}

TEST_CASE("A track's words are placed on the timeline, only what its clips play", "[model][textedit]")
{
    const auto song  = talk();
    auto       words = textedit::wordsOn(song, 0);
    REQUIRE(words.size() == 7); // "Later" is past the clip's end
    REQUIRE(words[1].text == "um,");
    REQUIRE_THAT(words[1].start, WithinAbs(1.0, 1e-9));

    // Moved and trimmed: the words follow.
    auto moved = song;
    moved.tracks[0].clips[0].startBeats          = 4.0; // 2 s later
    moved.tracks[0].clips[0].sourceOffsetSeconds = 1.45; // just before "we"
    moved.tracks[0].clips[0].lengthBeats         = 6.0;  // 3 s: up to 4.45 in the file
    words = textedit::wordsOn(moved, 0);
    REQUIRE(words.size() == 4); // we, went, to, the ("shop." centred at 4.55 is out)
    REQUIRE(words.front().text == "we");
    REQUIRE_THAT(words.front().start, WithinAbs(2.05, 1e-9)); // 0.05 s into the clip, which starts at 2 s
    REQUIRE(words.back().text == "the");

    // A word the clip starts partway into is shown from where the clip starts.
    moved.tracks[0].clips[0].sourceOffsetSeconds = 1.55; // inside "we" (1.5-1.7, middle 1.6)
    words = textedit::wordsOn(moved, 0);
    REQUIRE(words.front().text == "we");
    REQUIRE_THAT(words.front().start, WithinAbs(2.0, 1e-9));
}

TEST_CASE("Fillers and long pauses are found", "[model][textedit]")
{
    REQUIRE(textedit::isFiller("um,"));
    REQUIRE(textedit::isFiller("Uhh..."));
    REQUIRE_FALSE(textedit::isFiller("umbrella"));
    REQUIRE_FALSE(textedit::isFiller("I"));

    const auto words  = textedit::wordsOn(talk(), 0);
    const auto pauses = textedit::pausesBetween(words, 1.0);
    REQUIRE(pauses.size() == 1);
    REQUIRE_THAT(pauses[0].first, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(pauses[0].second, WithinAbs(4.0, 1e-9));

    const auto cut = textedit::shortened(2.0, 4.0, 0.5);
    REQUIRE_THAT(cut.first, WithinAbs(2.25, 1e-9));
    REQUIRE_THAT(cut.second, WithinAbs(3.75, 1e-9));
    REQUIRE(textedit::shortened(2.0, 2.3, 0.5).first == textedit::shortened(2.0, 2.3, 0.5).second);
}

TEST_CASE("Deleting words cuts their audio and closes the gap", "[model][textedit]")
{
    auto song  = talk();
    auto words = textedit::wordsOn(song, 0);

    // Take out "um," (1.0-1.4) and shorten the pause (2.0-4.0) to half a second.
    const auto pause = textedit::shortened(2.0, 4.0, 0.5);
    const int  cuts  = textedit::cutRanges(song, song.tracks[0].id, { { 1.0, 1.4 }, pause }, 0.0, fileSeconds);
    REQUIRE(cuts == 2);

    words = textedit::wordsOn(song, 0);
    std::vector<std::string> text;
    for (const auto& w : words)
        text.push_back(w.text);
    REQUIRE(text == std::vector<std::string> { "So", "we", "went", "to", "the", "shop." });

    // 0.4 s and 1.5 s out: "to" now starts at 4.0 - 1.9.
    REQUIRE_THAT(words[3].start, WithinAbs(2.1, 1e-6));
    // The track is 1.9 s shorter.
    double end = 0.0;
    for (const auto& clip : song.tracks[0].clips)
        end = std::max(end, clip.startBeats + clip.lengthBeats);
    REQUIRE_THAT(end, WithinAbs(20.0 - 3.8, 1e-6));

    // Overlapping and touching ranges count once.
    auto again = talk();
    REQUIRE(textedit::cutRanges(again, again.tracks[0].id, { { 1.0, 1.4 }, { 1.3, 1.6 }, { 1.6, 1.7 } }, 0.0, fileSeconds) == 1);
}

TEST_CASE("A cut's join is crossfaded from the audio beyond its edges", "[model][textedit]")
{
    auto song = talk();
    REQUIRE(textedit::cutRanges(song, song.tracks[0].id, { { 1.0, 1.4 } }, 0.02, fileSeconds) == 1);
    const auto& clips = song.tracks[0].clips;
    REQUIRE(clips.size() == 2);
    // They overlap at the join, each fading.
    const auto& first  = clips[0].startBeats < clips[1].startBeats ? clips[0] : clips[1];
    const auto& second = &first == &clips[0] ? clips[1] : clips[0];
    REQUIRE(first.startBeats + first.lengthBeats > second.startBeats);
    REQUIRE(first.fades.outSeconds > 0.0);
    REQUIRE(second.fades.inSeconds > 0.0);
}
