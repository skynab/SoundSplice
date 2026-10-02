#include <catch2/catch_test_macros.hpp>

#include <engine/CdImage.h>

using namespace soundsplice::engine;

TEST_CASE("CD tracks start on frames, and keep the Red Book's limits", "[engine][cdimage]")
{
    // 60 seconds; marks at 10 s, 10.5 s (too close), 30.004 s (between
    // frames) and 58 s (too close to the end).
    const auto tracks = cdimage::tracksFor({ { 30.004, "Third" }, { 10.0, "Second" }, { 10.5, "Too close" },
                                             { 58.0, "Too late" } },
                                           60.0);
    REQUIRE(tracks.size() == 3);
    REQUIRE(tracks[0].startFrame == 0);
    REQUIRE(tracks[1].startFrame == 750);
    REQUIRE(tracks[1].title == "Second");
    REQUIRE(tracks[2].startFrame == 2250); // 30.004 s, to the nearest frame

    // A mark at the very start names track 1.
    REQUIRE(cdimage::tracksFor({ { 0.0, "Intro" } }, 60.0).front().title == "Intro");

    // No more than 99.
    std::vector<std::pair<double, std::string>> many;
    for (int i = 1; i < 200; ++i)
        many.emplace_back(i * 5.0, "x");
    REQUIRE(cdimage::tracksFor(many, 2000.0).size() == 99);
}

TEST_CASE("A cue sheet says where each track is", "[engine][cdimage]")
{
    REQUIRE(cdimage::timeText(0) == "00:00:00");
    REQUIRE(cdimage::timeText(75 * 61 + 12) == "01:01:12");

    const auto cue = cdimage::cueSheet("Show.bin", { { 0, "Intro" }, { 750, "Say \"hi\"" } }, "The Show", "The Host");
    REQUIRE(cue.find("FILE \"Show.bin\" BINARY") != std::string::npos);
    REQUIRE(cue.find("TITLE \"The Show\"") != std::string::npos);
    REQUIRE(cue.find("  TRACK 02 AUDIO\r\n    TITLE \"Say 'hi'\"") != std::string::npos);
    REQUIRE(cue.find("    INDEX 01 00:10:00") != std::string::npos);
}

TEST_CASE("The BIN is 16-bit little-endian stereo, padded to whole sectors", "[engine][cdimage]")
{
    const float left[3] { 0.5f, -1.0f, 1.0f }, right[3] { -0.5f, 0.0f, 0.25f };
    const auto bin = cdimage::binFor(left, right, 3);
    REQUIRE(bin.size() == 588 * 4); // one sector
    REQUIRE(bin[0] == 0x00);
    REQUIRE(bin[1] == 0x40);        // 16384, low byte first
    REQUIRE(bin[2] == 0x00);
    REQUIRE(bin[3] == 0xc0);        // -16384
    REQUIRE((bin[4] | (bin[5] << 8)) == 0x8000); // -32768
    REQUIRE((bin[8] | (bin[9] << 8)) == 0x7fff); // clipped to 32767
    REQUIRE(bin[12] == 0);          // the padding is silence
}
