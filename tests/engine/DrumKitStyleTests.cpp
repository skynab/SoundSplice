#include <catch2/catch_test_macros.hpp>

#include <engine/DrumKitStyle.h>

#include <set>
#include <string>

using namespace looper::engine;

namespace
{
    constexpr DrumKitStyle kAllStyles[] = { DrumKitStyle::Classic,
                                            DrumKitStyle::Industrial,
                                            DrumKitStyle::Midtempo };
}

TEST_CASE("kNumDrumKitStyles covers every style the table handles", "[engine][drumkitstyle]")
{
    // DrumsPane builds its button row by counting to kNumDrumKitStyles and
    // casting, so a style added without bumping the count is missing from
    // the UI silently.
    REQUIRE((size_t) kNumDrumKitStyles == std::size(kAllStyles));
}

TEST_CASE("Drum kit style names are distinct and non-empty", "[engine][drumkitstyle]")
{
    std::set<std::string> names;
    for (auto style : kAllStyles)
    {
        const std::string name = drumKitStyleName(style);
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == std::size(kAllStyles));
}

TEST_CASE("Every kit style uses the same pad note numbers", "[engine][drumkitstyle]")
{
    // Load-bearing, and the reason this test exists rather than a comment:
    // clips store raw MIDI note numbers, so a style that renumbered its pads
    // would leave every note already written against the track pointing at
    // nothing. The notes would still be in the clip and simply not play -
    // a silent failure nobody would trace back to a kit button.
    for (auto style : kAllStyles)
    {
        const auto pads = padsForDrumKitStyle(style);
        INFO("style " << drumKitStyleName(style));
        REQUIRE(pads.size() == (size_t) kDrumKitStylePads);
        REQUIRE(pads[0].noteNumber == 36);
        REQUIRE(pads[1].noteNumber == 38);
        REQUIRE(pads[2].noteNumber == 42);
        REQUIRE(pads[3].noteNumber == 45);
    }
}

TEST_CASE("Every kit style uses the same pad labels", "[engine][drumkitstyle]")
{
    // The other half of the same invariant: generateDrumLoop finds its
    // kick/snare/hat by label (see MainComponent's noteForLabel), so a style
    // that renamed them would quietly send generated patterns to the wrong
    // pad - or to the positional fallback, which is worse because it
    // sometimes looks right.
    for (auto style : kAllStyles)
    {
        const auto pads = padsForDrumKitStyle(style);
        INFO("style " << drumKitStyleName(style));
        REQUIRE(std::string(pads[0].label) == "Kick");
        REQUIRE(std::string(pads[1].label) == "Snare");
        REQUIRE(std::string(pads[2].label) == "Hat");
        REQUIRE(std::string(pads[3].label) == "Other");
    }
}

TEST_CASE("Every pad names a sample, and none names a path", "[engine][drumkitstyle]")
{
    // A pad with no stem resolves to a file that can't exist, which is a
    // silent pad. And a stem carrying a path separator or extension would
    // mean this table had started to know about the filesystem, which is
    // exactly what keeping it in the engine layer is meant to prevent.
    for (auto style : kAllStyles)
    {
        for (const auto& pad : padsForDrumKitStyle(style))
        {
            const std::string stem = pad.sampleStem;
            INFO("style " << drumKitStyleName(style) << " pad " << pad.label);
            REQUIRE_FALSE(stem.empty());
            REQUIRE(stem.find('/') == std::string::npos);
            REQUIRE(stem.find('\\') == std::string::npos);
            REQUIRE(stem.find(".wav") == std::string::npos);
        }
    }
}

TEST_CASE("Pad trims stay in a sane range", "[engine][drumkitstyle]")
{
    // Pitch is resampling transposition, so a large value doesn't just
    // sound wrong - it stretches a one-shot into something unrecognisable.
    for (auto style : kAllStyles)
    {
        for (const auto& pad : padsForDrumKitStyle(style))
        {
            INFO("style " << drumKitStyleName(style) << " pad " << pad.label);
            REQUIRE(pad.gainDb >= -24.0f);
            REQUIRE(pad.gainDb <= 12.0f);
            REQUIRE(pad.pitchSemitones >= -12.0f);
            REQUIRE(pad.pitchSemitones <= 12.0f);
        }
    }
}

TEST_CASE("The styles actually differ from each other", "[engine][drumkitstyle]")
{
    // Three buttons that all load the same kit would pass every test above.
    for (size_t a = 0; a < std::size(kAllStyles); ++a)
    {
        for (size_t b = a + 1; b < std::size(kAllStyles); ++b)
        {
            const auto first  = padsForDrumKitStyle(kAllStyles[a]);
            const auto second = padsForDrumKitStyle(kAllStyles[b]);

            bool anyDifference = false;
            for (size_t i = 0; i < first.size(); ++i)
                anyDifference = anyDifference
                             || std::string(first[i].sampleStem) != std::string(second[i].sampleStem)
                             || first[i].gainDb != second[i].gainDb
                             || first[i].pitchSemitones != second[i].pitchSemitones;

            INFO(drumKitStyleName(kAllStyles[a]) << " vs " << drumKitStyleName(kAllStyles[b]));
            REQUIRE(anyDifference);
        }
    }
}
